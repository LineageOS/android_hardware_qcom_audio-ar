/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#define LOG_TAG "AHAL_Effect_VisualizerContextQti"
#include "VisualizerOffloadContext.h"

#include <android/binder_status.h>
#include <audio_utils/primitives.h>
#include <math.h>
#include <system/audio.h>
#include <time.h>

#include <algorithm>

namespace aidl::qti::effects {

StreamProxy::StreamProxy() {
    init();
}
StreamProxy::~StreamProxy() {
    cleanUp();
}
void StreamProxy::init() {
    memset(&data, 0, sizeof(data));

    chInfo.channels = AUDIO_CAPTURE_CHANNEL_COUNT;
    chInfo.ch_map[0] = PAL_CHMAP_CHANNEL_FL;
    chInfo.ch_map[1] = PAL_CHMAP_CHANNEL_FR;
    streamAttr.type = PAL_STREAM_PROXY;
    streamAttr.direction = PAL_AUDIO_INPUT;
    streamAttr.in_media_config.sample_rate = AUDIO_CAPTURE_SMP_RATE;
    streamAttr.in_media_config.bit_width = AUDIO_CAPTURE_BIT_WIDTH;
    streamAttr.in_media_config.ch_info = chInfo;
    streamAttr.in_media_config.aud_fmt_id = PAL_AUDIO_FMT_PCM_S16_LE;
    devices.id = PAL_DEVICE_IN_PROXY;
    devices.config.sample_rate = AUDIO_CAPTURE_SMP_RATE;
    devices.config.bit_width = AUDIO_CAPTURE_BIT_WIDTH;
    devices.config.ch_info = chInfo;
    devices.config.aud_fmt_id = PAL_AUDIO_FMT_PCM_S16_LE;

    if (openStream()) {
        if (!startStream()) cleanUp();
    }
}

bool StreamProxy::openStream() {
    if (auto ret = pal_stream_open(&streamAttr, noOfDevices, &devices, 0, NULL, NULL, 0,
                                   &inStreamHandle);
        ret) {
        LOG(ERROR) << __func__ << "pal_stream_open failed" << ret;
        return false;
    }
    mStreamOpened = true;
    return true;
}

bool StreamProxy::startStream() {
    inBufferCfg.buf_size = inBufferSize;
    inBufferCfg.buf_count = inBuffCount;

    if (auto ret = pal_stream_set_buffer_size(inStreamHandle, &inBufferCfg, NULL); ret) {
        LOG(ERROR) << __func__ << "pal_stream_set_buffer_size failed with err" << ret;
        return false;
    }

    inBufferSize = inBufferCfg.buf_size;
    if (auto ret = pal_stream_start(inStreamHandle); ret) {
        LOG(ERROR) << __func__ << "pal_stream_start failed with err" << ret;
        return false;
    }
    mStreamStarted = true;
    return true;
}

void StreamProxy::cleanUp() {
    if (mStreamStarted) {
        if (auto ret = pal_stream_stop(inStreamHandle); ret)
            LOG(ERROR) << __func__ << "pal_stream_stop failed with err" << ret;
    }
    mStreamStarted = false;
    if (mStreamOpened) {
        if (auto ret = pal_stream_close(inStreamHandle); ret)
            LOG(ERROR) << __func__ << "pal_stream_close failed with err" << ret;
    }
    mStreamOpened = false;
}

int16_t* StreamProxy::read() {
    int readStatus = 0;
    memset(&inBuffer, 0, sizeof(struct pal_buffer));
    inBuffer.buffer = (uint8_t*)&data[0];
    inBuffer.size = inBufferSize;
    readStatus = pal_stream_read(inStreamHandle, &inBuffer);
    if (readStatus > 0) {
        LOG(VERBOSE) << __func__ << " pal_stream_read success no_of_bytes_read =" << readStatus;
        return data;
    }
    LOG(ERROR) << __func__ << "pal_stream_read failed with read status " << readStatus;
    return nullptr;
}

bool StreamProxy::isStreamStarted() {
    return mStreamStarted;
}

std::string VisualizerOffloadContext::details() {
    std::ostringstream os;
    os << " ";
    os << this;
    os << " offload ";
    os << mOffload;
    os << " session ";
    os << getSessionId();
    os << " ioHandle ";
    os << getIoHandle();
    return os.str();
}

// Note: The thread loop is started upon receiving the setOffload(true) command.
// At this time, ioHandle might not point to the offload ioHandle.
// After setOffload, the FWK should update the parameters via setParameter,
// which would set ioHandle to the offload ioHandle.
// Therefore, it is possible to start the thread loop with a non-offload handle,
// which will be updated later.
// However, the actual ioHandle is not a concern here; we only need the offload indication
// to determine when the effect needs to be started.
int VisualizerOffloadContext::startThreadLoop() {
    std::lock_guard lg(mMutex);
    // we shouldn't run into this, should not get twice
    if (mCaptureThreadRunning.load()) {
        LOG(DEBUG) << __func__ << " thread is already running" << details();
        return -1;
    }
    LOG(DEBUG) << __func__ << details();
    mCaptureThreadHandler = std::thread(&VisualizerOffloadContext::captureThreadLoop, this);
    if (!mCaptureThreadHandler.joinable()) {
        LOG(ERROR) << __func__ << "failed to create captureThreadLoop" << details();
        return -EINVAL;
    }
    mCaptureThreadRunning.store(true);
    mExitThread = false;
    mCaptureThreadCondition.notify_one();
    return 0;
}

int VisualizerOffloadContext::stopThreadLoop() {
    // not an error if thread is not running
    if (!mCaptureThreadRunning.load()) {
        LOG(VERBOSE) << __func__ << " thread is not running " << details();
        return -1;
    }

    LOG(DEBUG) << __func__ << details();
    {
        std::lock_guard lg(mMutex);
        mExitThread = true;
    }

    mCaptureThreadCondition.notify_one();

    if (mCaptureThreadHandler.joinable()) {
        mCaptureThreadHandler.join();
        LOG(DEBUG) << __func__ << " capture thread joined " << details();
    }
    mCaptureThreadRunning.store(false);
    return 0;
}

void VisualizerOffloadContext::captureThreadLoop() {
    LOG(INFO) << __func__ << " entering threadloop " << details();
    int status = 0;
    bool captureEnabled = false;
    StreamProxy streamProxy;

    if (!streamProxy.isStreamStarted()) {
        LOG(ERROR) << __func__ << " failed to start proxy stream, exit " << details();
        return;
    }

    while (true) {
        {
            std::unique_lock<std::mutex> lck(mMutex);
            //LOG(VERBOSE) << __func__ << " waiting for active state" << details();
            mCaptureThreadCondition.wait(lck,
                                         [this] { return mState == State::ACTIVE || mExitThread; });
            //LOG(VERBOSE) << __func__ << " done waiting for active state" << details();

            if (mExitThread) {
                LOG(INFO) << __func__ << " Exiting threadloop" << details();
                break;
            }
        }
        process(streamProxy.read());
    }

    LOG(DEBUG) << __func__ << " completed threadloop" << details();
}

VisualizerOffloadContext::VisualizerOffloadContext(
        const aidl::android::hardware::audio::effect::Parameter::Common& common, bool processData)
    : EffectContext(common, processData) {
    std::lock_guard lg(mMutex);
    LOG(DEBUG) << __func__ << details();
    if (common.input != common.output) {
        LOG(ERROR) << __func__ << " mismatch input: " << common.input.toString()
                   << " and output: " << common.output.toString() << details();
    }
    mState = State::INITIALIZED;
    auto channelCount = getChannelCount(common.input.base.channelMask);
    mChannelCount = channelCount;
}

VisualizerOffloadContext::~VisualizerOffloadContext() {
    LOG(DEBUG) << __func__ << details();
    {
        std::lock_guard lg(mMutex);
        mState = State::UNINITIALIZED;
    }
    stopThreadLoop();
}

// When setOffload is set true ->FWK is starting a offload thread
// start the capture thread to read data.
// When setOffload is set false ->FWK is stopping a offload thread
// stop the capture thread to read data.
RetCode VisualizerOffloadContext::setOffload(bool offload) {
    if (offload != mOffload) {
        mOffload = offload;
        LOG(DEBUG) << __func__ << " changed to " << offload << details();
        if (offload) {
            startThreadLoop();
        } else {
            stopThreadLoop();
        }
    }

    return RetCode::SUCCESS;
}

RetCode VisualizerOffloadContext::enable() {
    LOG(DEBUG) << __func__ << details();
    std::lock_guard lg(mMutex);
    if (mState != State::INITIALIZED) {
        return RetCode::ERROR_EFFECT_LIB_ERROR;
    }
    mState = State::ACTIVE;
    mCaptureThreadCondition.notify_one();
    return RetCode::SUCCESS;
}

RetCode VisualizerOffloadContext::disable() {
    LOG(DEBUG) << __func__ << details();
    std::lock_guard lg(mMutex);
    if (mState != State::ACTIVE) {
        return RetCode::ERROR_EFFECT_LIB_ERROR;
    }
    mState = State::INITIALIZED;
    mCaptureThreadCondition.notify_one();
    return RetCode::SUCCESS;
}

void VisualizerOffloadContext::reset() {
    std::lock_guard lg(mMutex);
    std::fill(mCaptureBuf.begin(), mCaptureBuf.end(), 0x80);
}

RetCode VisualizerOffloadContext::setCaptureSamples(int samples) {
    std::lock_guard lg(mMutex);
    mCaptureSamples = samples;
    return RetCode::SUCCESS;
}

int VisualizerOffloadContext::getCaptureSamples() {
    std::lock_guard lg(mMutex);
    return mCaptureSamples;
}

RetCode VisualizerOffloadContext::setMeasurementMode(
        aidl::android::hardware::audio::effect::Visualizer::MeasurementMode mode) {
    std::lock_guard lg(mMutex);
    mMeasurementMode = mode;
    return RetCode::SUCCESS;
}

Visualizer::MeasurementMode VisualizerOffloadContext::getMeasurementMode() {
    std::lock_guard lg(mMutex);
    return mMeasurementMode;
}

RetCode VisualizerOffloadContext::setScalingMode(Visualizer::ScalingMode mode) {
    std::lock_guard lg(mMutex);
    mScalingMode = mode;
    return RetCode::SUCCESS;
}

Visualizer::ScalingMode VisualizerOffloadContext::getScalingMode() {
    std::lock_guard lg(mMutex);
    return mScalingMode;
}

RetCode VisualizerOffloadContext::setDownstreamLatency(int latency) {
    std::lock_guard lg(mMutex);
    mDownstreamLatency = latency;
    return RetCode::SUCCESS;
}

int VisualizerOffloadContext::getDownstreamLatency() {
    std::lock_guard lg(mMutex);
    return mDownstreamLatency;
}

uint32_t VisualizerOffloadContext::getDeltaTimeMsFromUpdatedTime_l() {
    uint32_t deltaMs = 0;
    if (mBufferUpdateTime.tv_sec != 0) {
        struct timespec ts;
        if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
            time_t secs = ts.tv_sec - mBufferUpdateTime.tv_sec;
            long nsec = ts.tv_nsec - mBufferUpdateTime.tv_nsec;
            if (nsec < 0) {
                --secs;
                nsec += 1000000000;
            }
            deltaMs = secs * 1000 + nsec / 1000000;
        }
    }
    return deltaMs;
}

Visualizer::Measurement VisualizerOffloadContext::getMeasure() {
    uint16_t peakU16 = 0;
    float sumRmsSquared = 0.0f;
    uint8_t nbValidMeasurements = 0;
    {
        std::lock_guard lg(mMutex);
        // reset measurements if last measurement was too long ago (which implies stored
        // measurements aren't relevant anymore and shouldn't bias the new one)
        const uint32_t delayMs = getDeltaTimeMsFromUpdatedTime_l();
        if (delayMs > kDiscardMeasurementsTimeMs) {
            LOG(INFO) << __func__ << " Discarding " << delayMs << " ms old measurements "
                      << details();
            for (uint32_t i = 0; i < mMeasurementWindowSizeInBuffers; i++) {
                mPastMeasurements[i].mIsValid = false;
                mPastMeasurements[i].mPeakU16 = 0;
                mPastMeasurements[i].mRmsSquared = 0;
            }
            mMeasurementBufferIdx = 0;
        } else {
            // only use actual measurements, otherwise the first RMS measure happening before
            // MEASUREMENT_WINDOW_MAX_SIZE_IN_BUFFERS have been played will always be artificially
            // low
            for (uint32_t i = 0; i < mMeasurementWindowSizeInBuffers; i++) {
                if (mPastMeasurements[i].mIsValid) {
                    if (mPastMeasurements[i].mPeakU16 > peakU16) {
                        peakU16 = mPastMeasurements[i].mPeakU16;
                    }
                    sumRmsSquared += mPastMeasurements[i].mRmsSquared;
                    nbValidMeasurements++;
                }
            }
        }
    }
    float rms = nbValidMeasurements == 0 ? 0.0f : sqrtf(sumRmsSquared / nbValidMeasurements);
    Visualizer::Measurement measure;
    // convert from I16 sample values to mB and write results
    measure.rms = (rms < 0.000016f) ? -9600 : (int32_t)(2000 * log10(rms / 32767.0f));
    measure.peak = (peakU16 == 0) ? -9600 : (int32_t)(2000 * log10(peakU16 / 32767.0f));
    LOG(DEBUG) << __func__ << " peak " << peakU16 << " (" << measure.peak << "mB), rms " << rms
               << " (" << measure.rms << "mB)" << details();
    return measure;
}

std::vector<uint8_t> VisualizerOffloadContext::capture() {
    uint32_t captureSamples = mCaptureSamples;
    std::vector<uint8_t> result(captureSamples, 0x80);
    std::lock_guard lg(mMutex);
    if (mState != State::ACTIVE) {
        return result;
    }

    const uint32_t deltaMs = getDeltaTimeMsFromUpdatedTime_l();
    // if audio framework has stopped playing audio although the effect is still active we must
    // clear the capture buffer to return silence
    if ((mLastCaptureIdx == mCaptureIdx) && (mBufferUpdateTime.tv_sec != 0) &&
        (deltaMs > kMaxStallTimeMs)) {
        LOG(DEBUG) << __func__ << " capture going to idle" << details();
        mBufferUpdateTime.tv_sec = 0;
        return result;
    }
    int32_t latencyMs = mDownstreamLatency;
    latencyMs -= deltaMs;
    if (latencyMs < 0) {
        latencyMs = 0;
    }
    uint32_t deltaSamples = captureSamples + mCommon.input.base.sampleRate * latencyMs / 1000;

    // large sample rate, latency, or capture size, could cause overflow.
    // do not offset more than the size of buffer.
    if (deltaSamples > kMaxCaptureBufSize) {
        // android_errorWriteLog(0x534e4554, "31781965");
        deltaSamples = kMaxCaptureBufSize;
    }

    int32_t capturePoint;
    __builtin_sub_overflow((int32_t)mCaptureIdx, deltaSamples, &capturePoint);
    // a negative capturePoint means we wrap the buffer.
    if (capturePoint < 0) {
        uint32_t size = -capturePoint;
        if (size > captureSamples) {
            size = captureSamples;
        }
        std::copy(std::begin(mCaptureBuf) + kMaxCaptureBufSize - size,
                  std::begin(mCaptureBuf) + kMaxCaptureBufSize, result.begin());
        captureSamples -= size;
        capturePoint = 0;
    }
    std::copy(std::begin(mCaptureBuf) + capturePoint,
              std::begin(mCaptureBuf) + capturePoint + captureSamples,
              result.begin() + mCaptureSamples - captureSamples);
    mLastCaptureIdx = mCaptureIdx;
    return result;
}

int VisualizerOffloadContext::process(int16_t* inBuffer) {
    if (!inBuffer || mState != State::ACTIVE) return -EINVAL;

    int samples = AUDIO_CAPTURE_PERIOD_SIZE * mChannelCount;
    // perform measurements if needed
    if (mMeasurementMode == Visualizer::MeasurementMode::PEAK_RMS) {
        // find the peak and RMS squared for the new buffer
        float rmsSqAcc = 0;
        float maxSample = 0.f;
        float sample = 0.f;
        for (size_t inIdx = 0; inIdx < (unsigned)samples; ++inIdx) {
            sample = float_from_i16(inBuffer[inIdx]);
            maxSample = fmax(maxSample, fabs(sample));
            rmsSqAcc += sample * sample;
        }
        maxSample *= 1 << 15;  // scale to int16_t, with exactly 1 << 15 representing positive num.
        rmsSqAcc *= 1 << 30;   // scale to int16_t * 2

        mPastMeasurements[mMeasurementBufferIdx] = {.mIsValid = true,
                                                    .mPeakU16 = (uint16_t)maxSample,
                                                    .mRmsSquared = rmsSqAcc / samples};
        if (++mMeasurementBufferIdx >= mMeasurementWindowSizeInBuffers) {
            mMeasurementBufferIdx = 0;
        }
    }

    float fscale;  // multiplicative scale
    if (mScalingMode == Visualizer::ScalingMode::NORMALIZED) {
        // derive capture scaling factor from peak value in current buffer
        // this gives more interesting captures for display.
        float maxSample = 0.f;
        for (size_t inIdx = 0; inIdx < (unsigned)samples;) {
            // we reconstruct the actual summed value to ensure proper normalization
            // for multichannel outputs (channels > 2 may often be 0).
            float smp = 0.f;
            for (int i = 0; i < mChannelCount; ++i) {
                smp += float_from_i16(inBuffer[inIdx++]);
            }
            maxSample = fmax(maxSample, fabs(smp));
        }
        if (maxSample > 0.f) {
            fscale = 0.99f / maxSample;
            int exp;  // unused
            const float significand = frexp(fscale, &exp);
            if (significand == 0.5f) {
                fscale *= 255.f / 256.f;  // avoid returning unaltered PCM signal
            }
        } else {
            // scale doesn't matter, the values are all 0.
            fscale = 1.f;
        }
    } else {
        assert(mScalingMode == Visualizer::ScalingMode::AS_PLAYED);
        // Note: if channels are uncorrelated, 1/sqrt(N) could be used at the risk of clipping.
        fscale = 1.f / mChannelCount;  // account for summing all the channels together.
    }

    uint32_t captIdx;
    uint32_t inIdx;
    for (inIdx = 0, captIdx = mCaptureIdx; inIdx < (unsigned)samples; captIdx++) {
        // wrap
        if (captIdx >= kMaxCaptureBufSize) {
            captIdx = 0;
        }

        float smp = 0.f;
        for (uint32_t i = 0; i < mChannelCount; ++i) {
            smp += float_from_i16(inBuffer[inIdx++]);
        }
        mCaptureBuf[captIdx] = clamp8_from_float(smp * fscale);
    }

    // the following two should really be atomic, though it probably doesn't
    // matter much for visualization purposes
    mCaptureIdx = captIdx;
    // update last buffer update time stamp
    if (clock_gettime(CLOCK_MONOTONIC, &mBufferUpdateTime) < 0) {
        mBufferUpdateTime.tv_sec = 0;
    }

    return 0;
}

}  // namespace aidl::qti::effects
