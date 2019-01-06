//
// Created by cain on 2018/12/30.
//

#include "MediaSync.h"

MediaSync::MediaSync(PlayerState *playerState) {
    this->playerState = playerState;
    audioDecoder = NULL;
    videoDecoder = NULL;
    audioClock = new MediaClock();
    videoClock = new MediaClock();
    extClock = new MediaClock();

    syncThread = NULL;

    forceRefresh = 0;
    maxFrameDuration = 10.0;
    frameTimer = 0;
    abortRequest = 0;

    swsContext = NULL;
    mBuffer = NULL;
    pFrameRGBA = av_frame_alloc();
}

MediaSync::~MediaSync() {
    playerState = NULL;
    videoDecoder = NULL;
    audioDecoder = NULL;
    if (pFrameRGBA) {
        av_frame_free(&pFrameRGBA);
        av_freep(&pFrameRGBA);
        pFrameRGBA = NULL;
    }
}

void MediaSync::start(VideoDecoder *videoDecoder, AudioDecoder *audioDecoder) {
    mMutex.lock();
    this->videoDecoder = videoDecoder;
    this->audioDecoder = audioDecoder;
    abortRequest = 0;
    mCondition.signal();
    mMutex.unlock();
    if (videoDecoder && !syncThread) {
        syncThread = new Thread(this);
        syncThread->start();
    }
}

void MediaSync::stop() {
    mMutex.lock();
    abortRequest = 1;
    mCondition.signal();
    mMutex.unlock();
    if (syncThread) {
        syncThread->join();
        delete syncThread;
        syncThread = NULL;
    }
}

void MediaSync::setSurface(ANativeWindow *window) {
    mMutex.lock();
    if (this->window != NULL) {
        ANativeWindow_release(this->window);
    }
    this->window = window;
    mCondition.signal();
    mMutex.unlock();
}

void MediaSync::setMaxDuration(double maxDuration) {
    this->maxFrameDuration = maxDuration;
}

void MediaSync::refreshVideoTimer() {
    mMutex.lock();
    this->frameTimerRefresh = 1;
    mCondition.signal();
    mMutex.unlock();
}

void MediaSync::updateAudioClock(double pts, double time) {
    audioClock->setClock(pts, time);
    extClock->syncToSlave(audioClock);
}

double MediaSync::getAudioDiffClock() {
    return audioClock->getClock() - getMasterClock();
}

void MediaSync::updateExternalClock(double pts) {
    extClock->setClock(pts);
}

double MediaSync::getMasterClock() {
    double val = 0;
    switch (playerState->syncType) {
        case AV_SYNC_VIDEO: {
            val = videoClock->getClock();
            break;
        }
        case AV_SYNC_AUDIO: {
            val = audioClock->getClock();
            break;
        }
        case AV_SYNC_EXTERNAL: {
            val = extClock->getClock();
            break;
        }
    }
    return val;
}

void MediaSync::run() {
    ALOGD("media sync thread start!");
    double remaining_time = 0.0;
    while (true) {

        if (abortRequest || playerState->abortRequest) {
            break;
        }

        if (remaining_time > 0.0) {
            av_usleep((int64_t) (remaining_time * 1000000.0));
        }
        remaining_time = REFRESH_RATE;
        if (!abortRequest && (!playerState->pauseRequest || forceRefresh)) {
            refreshVideo(&remaining_time);
        }
    }

    ALOGD("render video thread exit!");
}

void MediaSync::refreshVideo(double *remaining_time) {
    double time;

    // 检查外部时钟
    if (!abortRequest & !playerState->pauseRequest && playerState->realTime &&
        playerState->syncType == AV_SYNC_EXTERNAL) {
        checkExternalClockSpeed();
    }

    Frame *af = (Frame *) av_mallocz(sizeof(Frame));
    af->frame = av_frame_alloc();

    for (;;) {

        if (abortRequest || playerState->abortRequest || !videoDecoder) {
            break;
        }

        // 判断是否存在帧队列是否存在数据
        if (videoDecoder->getFrameSize() > 0) {
            double lastDuration, duration, delay;
            Frame *currentFrame, *lastFrame;
            // 上一帧
            lastFrame = videoDecoder->getFrameQueue()->lastFrame();
            // 当前帧
            currentFrame = videoDecoder->getFrameQueue()->currentFrame();
            // 判断是否需要强制更新帧的时间
            if (frameTimerRefresh) {
                frameTimer = av_gettime_relative() / 1000000.0;
                frameTimerRefresh = 0;
            }

            // 如果处于暂停状态，则直接显示
            if (playerState->abortRequest || playerState->pauseRequest) {
                break;
            }

            // 计算上一次显示时长
            lastDuration = calculateDuration(lastFrame, currentFrame);
            // 根据上一次显示的时长，计算延时
            delay = calculateDelay(lastDuration);
            // 获取当前时间
            time = av_gettime_relative() / 1000000.0;
            // 如果当前时间小于帧计时器的时间 + 延时时间，则表示还没到当前帧
            if (time < frameTimer + delay) {
                *remaining_time = FFMIN(frameTimer + delay - time, *remaining_time);
                break;
            }

            // 更新帧计时器
            frameTimer += delay;
            // 帧计时器落后当前时间超过了阈值，则用当前的时间作为帧计时器时间
            if (delay > 0 && time - frameTimer > AV_SYNC_THRESHOLD_MAX) {
                frameTimer = time;
            }

            // 更新视频时钟的pts
            mMutex.lock();
            if (!isnan(currentFrame->pts)) {
                videoClock->setClock(currentFrame->pts);
                extClock->syncToSlave(videoClock);
            }
            mMutex.unlock();

            // 如果队列中还剩余超过一帧的数据时，需要拿到下一帧，然后计算间隔，并判断是否需要进行舍帧操作
            if (videoDecoder->getFrameSize() > 1) {
                Frame *nextFrame = videoDecoder->getFrameQueue()->nextFrame();
                duration = calculateDuration(currentFrame, nextFrame);
                // 如果不处于同步到视频状态，并且处于跳帧状态，则跳过当前帧
                if ((time > frameTimer + duration)
                    && (playerState->frameDrop && playerState->syncType != AV_SYNC_VIDEO)) {
                    videoDecoder->getFrameQueue()->popFrame();
                    continue;
                }
            }

            // 下一帧
            videoDecoder->getFrameQueue()->popFrame();
            forceRefresh = 1;
        }

        break;
    }

    // 显示画面
    if (!playerState->displayDisable && forceRefresh && videoDecoder
        && videoDecoder->getFrameQueue()->getShowIndex()) {
        renderVideo();
    }
    forceRefresh = 0;
}

void MediaSync::checkExternalClockSpeed() {
    if (videoDecoder && videoDecoder->getPacketSize() <= EXTERNAL_CLOCK_MIN_FRAMES
        || audioDecoder && audioDecoder->getPacketSize() <= EXTERNAL_CLOCK_MIN_FRAMES) {
        extClock->setSpeed(FFMAX(EXTERNAL_CLOCK_SPEED_MIN,
                                 extClock->getSpeed() - EXTERNAL_CLOCK_SPEED_STEP));
    } else if ((!videoDecoder || videoDecoder->getPacketSize() > EXTERNAL_CLOCK_MAX_FRAMES)
               && (!audioDecoder || audioDecoder->getPacketSize() > EXTERNAL_CLOCK_MAX_FRAMES)) {
        extClock->setSpeed(FFMIN(EXTERNAL_CLOCK_SPEED_MAX,
                                 extClock->getSpeed() + EXTERNAL_CLOCK_SPEED_STEP));
    } else {
        double speed = extClock->getSpeed();
        if (speed != 1.0) {
            extClock->setSpeed(speed + EXTERNAL_CLOCK_SPEED_STEP * (1.0 - speed) / fabs(1.0 - speed));
        }
    }
}

double MediaSync::calculateDelay(double delay) {
    double sync_threshold, diff = 0;
    // 如果不是同步到视频流，则需要计算延时时间
    if (playerState->syncType != AV_SYNC_VIDEO) {
        // 计算差值
        diff = videoClock->getClock() - getMasterClock();
        // 用差值与同步阈值计算延时
        sync_threshold = FFMAX(AV_SYNC_THRESHOLD_MIN, FFMIN(AV_SYNC_THRESHOLD_MAX, delay));
        if (!isnan(diff) && fabs(diff) < maxFrameDuration) {
            if (diff <= -sync_threshold) {
                delay = FFMAX(0, delay + diff);
            } else if (diff >= sync_threshold && delay > AV_SYNC_FRAMEDUP_THRESHOLD) {
                delay = delay + diff;
            } else if (diff >= sync_threshold) {
                delay = 2 * delay;
            }
        }
    }

    av_log(NULL, AV_LOG_TRACE, "video: delay=%0.3f A-V=%f\n", delay, -diff);

    return delay;
}

double MediaSync::calculateDuration(Frame *vp, Frame *nextvp) {
    double duration = nextvp->pts - vp->pts;
    if (isnan(duration) || duration <= 0 || duration > maxFrameDuration) {
        return vp->duration;
    } else {
        return duration;
    }
}

void MediaSync::renderVideo() {
    if (!videoDecoder) {
        return;
    }
    Frame *vp = videoDecoder->getFrameQueue()->lastFrame();

    // 转码
    AVCodecContext *pCodecCtx = videoDecoder->getCodecContext();
    if (!vp->uploaded) {
        if (!swsContext) {
            // buffer中数据用于渲染,且格式为RGBA
            int numBytes = av_image_get_buffer_size(AV_PIX_FMT_RGBA, pCodecCtx->width, pCodecCtx->height, 1);

            mBuffer = (uint8_t *)av_malloc(numBytes * sizeof(uint8_t));
            av_image_fill_arrays(pFrameRGBA->data, pFrameRGBA->linesize, mBuffer, AV_PIX_FMT_RGBA,
                                 pCodecCtx->width, pCodecCtx->height, 1);

            // 由于解码出来的帧格式不是RGBA的,在渲染之前需要进行格式转换
            swsContext = sws_getContext(pCodecCtx->width,
                                        pCodecCtx->height,
                                        pCodecCtx->pix_fmt,
                                        pCodecCtx->width,
                                        pCodecCtx->height,
                                        AV_PIX_FMT_RGBA,
                                        SWS_BICUBIC,
                                        NULL, NULL, NULL);
        }

        // 转码
        if (swsContext) {
            sws_scale(swsContext, (uint8_t const * const *)vp->frame->data,
                      vp->frame->linesize, 0, pCodecCtx->height,
                      pFrameRGBA->data, pFrameRGBA->linesize);
        }
        vp->uploaded = 1;
    }

    mMutex.lock();
    if (window != NULL) {
        ANativeWindow_setBuffersGeometry(window, pCodecCtx->width, pCodecCtx->height,
                                         WINDOW_FORMAT_RGBA_8888);
        ANativeWindow_Buffer windowBuffer;
        ANativeWindow_lock(window, &windowBuffer, 0);

        // 获取stride
        uint8_t * dst = (uint8_t *)windowBuffer.bits;
        int dstStride = windowBuffer.stride * 4;
        uint8_t * src = pFrameRGBA->data[0];
        int srcStride = pFrameRGBA->linesize[0];
        // 由于window的stride和帧的stride不同,因此需要逐行复制
        int h;
        for (h = 0; h < pCodecCtx->height; h++) {
            memcpy(dst + h * dstStride, src + h * srcStride, (size_t) srcStride);
        }
        ANativeWindow_unlockAndPost(window);
    }
    mMutex.unlock();
}
