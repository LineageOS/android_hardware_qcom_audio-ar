/*
 * Copyright (c) 2023-2025 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <cmath>

#define LOG_TAG "AHAL_StreamOut_QTI"

#include <android-base/logging.h>
#include <audio_utils/clock.h>
#include <hardware/audio.h>
#include <qti-audio-core/Module.h>
#include <qti-audio-core/ModulePrimary.h>
#include <qti-audio-core/StreamOutPrimary.h>
#include <qti-audio/PlatformConverter.h>
#include <qti-audio-core/Parameters.h>

using aidl::android::hardware::audio::common::AudioOffloadMetadata;
using aidl::android::hardware::audio::common::SinkMetadata;
using aidl::android::hardware::audio::common::SourceMetadata;
using aidl::android::media::audio::common::AudioDevice;
using aidl::android::media::audio::common::AudioDualMonoMode;
using aidl::android::media::audio::common::AudioLatencyMode;
using aidl::android::media::audio::common::AudioOffloadInfo;
using aidl::android::media::audio::common::AudioPlaybackRate;
using aidl::android::media::audio::common::MicrophoneDynamicInfo;
using aidl::android::media::audio::common::MicrophoneInfo;
using aidl::android::media::audio::common::AudioLatencyMode;
using aidl::android::media::audio::common::AudioChannelLayout;
using aidl::android::media::audio::common::AudioMode;

using ::aidl::android::hardware::audio::core::IStreamCallback;
using ::aidl::android::hardware::audio::core::IStreamCommon;
using ::aidl::android::hardware::audio::core::StreamDescriptor;
using ::aidl::android::hardware::audio::core::VendorParameter;

using aidl::android::media::audio::common::AudioPortExt;

// uncomment this to enable logging of very verbose logs like burst commands.
// #define VERY_VERBOSE_LOGGING 1

static bool karaoke = false;

namespace qti::audio::core {

std::mutex StreamOutPrimary::sourceMetadata_mutex_;

StreamOutPrimary::StreamOutPrimary(StreamContext&& context, const SourceMetadata& sourceMetadata,
                                   const std::optional<AudioOffloadInfo>& offloadInfo)
    : StreamOut(std::move(context), offloadInfo),
      StreamCommonImpl(&(StreamOut::mContext), sourceMetadata),
      mTag(getUsecaseTag(getContext().getMixPortConfig())),
      mTagName(getName(mTag)),
      mFrameSizeBytes(getContext().getFrameSize()),
      mPlaybackRate(sDefaultPlaybackRate),
      mMixPortConfig(getContext().getMixPortConfig()) {
    if (mTag == Usecase::PRIMARY_PLAYBACK) {
        mExt.emplace<PrimaryPlayback>();
    } else if (mTag == Usecase::DEEP_BUFFER_PLAYBACK) {
        mExt.emplace<DeepBufferPlayback>();
    } else if (mTag == Usecase::COMPRESS_OFFLOAD_PLAYBACK) {
        mExt.emplace<CompressPlayback>(offloadInfo.value(), this,
                                       mMixPortConfig);
    } else if (mTag == Usecase::PCM_OFFLOAD_PLAYBACK) {
        mExt.emplace<PcmOffloadPlayback>(mMixPortConfig);
    } else if (mTag == Usecase::VOIP_PLAYBACK) {
        mExt.emplace<VoipPlayback>();
    } else if (mTag == Usecase::SPATIAL_PLAYBACK) {
        mExt.emplace<SpatialPlayback>();
    } else if (mTag == Usecase::MMAP_PLAYBACK) {
        mExt.emplace<MMapPlayback>();
    } else if (mTag == Usecase::ULL_PLAYBACK) {
        mExt.emplace<UllPlayback>();
    } else if (mTag == Usecase::IN_CALL_MUSIC) {
        mExt.emplace<InCallMusic>();
    } else if (mTag == Usecase::HAPTICS_PLAYBACK) {
        mExt.emplace<HapticsPlayback>();
    }

    mHwVolumeSupported = isHwVolumeSupported();
    mHwFlushSupported = isHwFlushSupported();
    mHwPauseSupported = isHwPauseSupported();
    if (mHwVolumeSupported) {
        mVolumes.resize(getChannelCount(mMixPortConfig.channelMask.value()));
    }
    std::ostringstream os;
    os << " : usecase: " << mTagName;
    os << " IoHandle: " << mMixPortConfig.ext.get<AudioPortExt::Tag::mix>().handle << " ";
    mLogPrefix = os.str();
    LOG(DEBUG) << __func__ << mLogPrefix;
}

bool StreamOutPrimary::isHwVolumeSupported() {
    switch (mTag) {
        case Usecase::COMPRESS_OFFLOAD_PLAYBACK:
        case Usecase::PCM_OFFLOAD_PLAYBACK:
        case Usecase::MMAP_PLAYBACK:
        case Usecase::VOIP_PLAYBACK:
            return true;
        default:
            break;
    }
    return false;
}

bool StreamOutPrimary::isHwFlushSupported() {
    switch (mTag) {
        case Usecase::COMPRESS_OFFLOAD_PLAYBACK:
        case Usecase::PCM_OFFLOAD_PLAYBACK:
            return true;
        default:
            break;
    }
    return false;
}

bool StreamOutPrimary::isHwPauseSupported() {
    switch (mTag) {
        case Usecase::COMPRESS_OFFLOAD_PLAYBACK:
        case Usecase::PCM_OFFLOAD_PLAYBACK:
            return true;
        default:
            break;
    }
    return false;
}

struct BufferConfig StreamOutPrimary::getBufferConfig() {
    return mPlatform.getBufferConfig(mMixPortConfig, mTag);
}

StreamOutPrimary::~StreamOutPrimary() {
    cleanupWorker();
    LOG(DEBUG) << __func__ << mLogPrefix;
}

// start of methods called from IModule
ndk::ScopedAStatus StreamOutPrimary::setConnectedDevices(
        const std::vector<::aidl::android::media::audio::common::AudioDevice>& devices) {
    mWorker->setIsConnected(!devices.empty());
    mConnectedDevices = devices;

    return configureConnectedDevices_I();
}

ndk::ScopedAStatus StreamOutPrimary::reconfigureConnectedDevices() {
    return configureConnectedDevices_I();
}

ndk::ScopedAStatus StreamOutPrimary::configureConnectedDevices_I() {
    //skip a2dp routing if voice call is active
    if (hasBluetoothA2dpDevice(mConnectedDevices) &&
              (mPlatform.getCallMode() != (int)AudioMode::NORMAL)) {
        LOG(DEBUG) << __func__ << mLogPrefix << ": removing a2dp from the connected devices";
        mConnectedDevices.erase(std::remove_if(mConnectedDevices.begin(),
                                mConnectedDevices.end(), isBluetoothA2dpDevice),
                                mConnectedDevices.end());
    }

    if (mConnectedDevices.empty()) {
        LOG(DEBUG) << __func__ << mLogPrefix << ": stream is not connected";
        return ndk::ScopedAStatus::ok();
    }

    if (mTag == Usecase::LOW_LATENCY_PLAYBACK ||
        mTag == Usecase::DEEP_BUFFER_PLAYBACK ||
        mTag == Usecase::VOIP_PLAYBACK) {
        if (auto telephony = mContext.getTelephony().lock()) {
            telephony->onPlaybackStreamDevices(mConnectedDevices);
        }
    }

    if (!mPalHandle) {
        LOG(WARNING) << __func__ << mLogPrefix << ": stream is not configured";
        return ndk::ScopedAStatus::ok();
    }

    auto connectedPalDevices =
            mPlatform.configureAndFetchPalDevices(mMixPortConfig, mTag, mConnectedDevices);

    if (int32_t ret = ::pal_stream_set_device(mPalHandle, connectedPalDevices.size(),
                                              connectedPalDevices.data());
        ret) {
        LOG(ERROR) << __func__ << mLogPrefix << " failed to set devices on stream, ret:" << ret;
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }

    LOG(DEBUG) << __func__ << mLogPrefix << " connected to " << mConnectedDevices;

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus StreamOutPrimary::configureMMapStream(int32_t* fd, int64_t* burstSizeFrames,
                                                         int32_t* flags,
                                                         int32_t* bufferSizeFrames) {
    if (mTag != Usecase::MMAP_PLAYBACK) {
        LOG(ERROR) << __func__ << mLogPrefix << " cannot call on non-MMAP stream types";
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }

    auto attr = mPlatform.getPalStreamAttributes(mMixPortConfig, false);
    if (!attr) {
        LOG(ERROR) << __func__ << mLogPrefix << " can't get pal attributes";
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }
    attr->type = PAL_STREAM_ULTRA_LOW_LATENCY;
    auto palDevices = mPlatform.configureAndFetchPalDevices(mMixPortConfig, mTag, mConnectedDevices,
                                                            true /*dummyDevice*/);

    /* For MMAP playback usecase, audio framework updates track metadata to AHAL after
     * CreateMmapBuffer(). In case of MMAP playback on BT, device starts well before
     * track metadata updated to BT stack. Due to this, it requires unnecessary
     * reconfig to change existing BT config to gaming config params. Thus to avoid
     * extra reconfig event and ISO parameters error, HAL updates metadata to BT stack
     * before MMAP stream opens.
     */
    if (hasBluetoothLEDevice(mConnectedDevices)) {
        ssize_t track_count = 1;
        std::vector<playback_track_metadata_t> sourceTracks;
        source_metadata_t btSourceMetadata;
        sourceTracks.resize(track_count);

        btSourceMetadata.track_count = track_count;
        btSourceMetadata.tracks = sourceTracks.data();
        btSourceMetadata.tracks->usage = AUDIO_USAGE_GAME;
        btSourceMetadata.tracks->content_type = AUDIO_CONTENT_TYPE_MUSIC;
        LOG(DEBUG) << __func__<< "Source metadata for mmap usage: "
                   << btSourceMetadata.tracks->usage << " content_type: "
                   << btSourceMetadata.tracks->content_type;
         // Pass the source metadata to PAL
         pal_set_param(PAL_PARAM_ID_SET_SOURCE_METADATA, (void*)&btSourceMetadata, 0);
    }
    uint64_t cookie = reinterpret_cast<uint64_t>(this);
    pal_stream_callback palFn = nullptr;
    attr->flags = static_cast<pal_stream_flags_t>(PAL_STREAM_FLAG_MMAP_NO_IRQ);
    if (int32_t ret = ::pal_stream_open(attr.get(), palDevices.size(), palDevices.data(), 0,
                                        nullptr, palFn, cookie, &(this->mPalHandle));
        ret) {
        LOG(ERROR) << __func__ << mLogPrefix
                   << " pal stream open failed, ret:" << std::to_string(ret);
        mPalHandle = nullptr;
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }

    auto bufConfig = getBufferConfig();
    const size_t ringBufSizeInBytes = bufConfig.bufferSize;
    const size_t ringBufCount = bufConfig.bufferCount;
    auto palBufferConfig = mPlatform.getPalBufferConfig(ringBufSizeInBytes, ringBufCount);
    LOG(DEBUG) << __func__ << mLogPrefix << " set pal_stream_set_buffer_size to "
               << std::to_string(ringBufSizeInBytes) << " with count "
               << std::to_string(ringBufCount);
    if (int32_t ret =
                ::pal_stream_set_buffer_size(this->mPalHandle, nullptr, palBufferConfig.get());
        ret) {
        LOG(ERROR) << __func__ << mLogPrefix
                   << " pal_stream_set_buffer_size failed!!! ret:" << std::to_string(ret);
        ::pal_stream_close(mPalHandle);
        mPalHandle = nullptr;
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }

    std::get<MMapPlayback>(mExt).setPalHandle(mPalHandle);

    const auto frameSize = getFrameSizeInBytes(
            mMixPortConfig.format.value(), mMixPortConfig.channelMask.value());
    int32_t ret = std::get<MMapPlayback>(mExt).createMMapBuffer(frameSize, fd, burstSizeFrames,
                                                                flags, bufferSizeFrames);
    if (ret != 0) {
        LOG(ERROR) << __func__ << mLogPrefix << " create MMap buffer failed!";
        ::pal_stream_close(mPalHandle);
        mPalHandle = nullptr;
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }

    LOG(INFO) << __func__ << mLogPrefix << ": stream is configured";

    setHwVolume(mVolumes);

    return ndk::ScopedAStatus::ok();
}

// end of methods called from IModule

// start of DriverInterface Methods

::android::status_t StreamOutPrimary::init() {
    return ::android::OK;
}

::android::status_t StreamOutPrimary::drain(StreamDescriptor::DrainMode mode) {
    if (!mPalHandle) {
        LOG(WARNING) << __func__ << mLogPrefix << ": stream is not configured";
        return ::android::OK;
    }

    if (mTag == Usecase::MMAP_PLAYBACK) {
        // drain is stop for mmap
        return stopMMAP();
    }

    auto palDrainMode =
            mode == StreamDescriptor::DrainMode::DRAIN_ALL ? PAL_DRAIN : PAL_DRAIN_PARTIAL;
    if (int32_t ret = ::pal_stream_drain(mPalHandle, palDrainMode); ret) {
        LOG(ERROR) << __func__ << mLogPrefix << " failed to drain the stream, ret:" << ret;
        return ret;
    }
    LOG(DEBUG) << __func__ << mLogPrefix << " drained ";
    return ::android::OK;
}

::android::status_t StreamOutPrimary::flush() {
    if (!mPalHandle) {
        LOG(WARNING) << __func__ << mLogPrefix << ": stream is not configured ";
        return ::android::OK;
    }

    if (mTag == Usecase::MMAP_PLAYBACK) {
        // flush is stop for mmap
        return stopMMAP();
    }

    if (!mHwFlushSupported) {
        LOG(VERBOSE) << __func__ << mLogPrefix << " unsupported operation!!, Hence ignored";
        return ::android::OK;
    }

    // to be precise or accurate w.r.t to frames consumed
    // before flush operation, we would fetch latest frames
    // and cache it.
    if (mTag == Usecase::COMPRESS_OFFLOAD_PLAYBACK) {
        std::get<CompressPlayback>(mExt).getPositionInFrames(mPalHandle);
    } else if (mTag == Usecase::PCM_OFFLOAD_PLAYBACK) {
        std::get<PcmOffloadPlayback>(mExt).getPositionInFrames(mPalHandle);
    }

    if (int32_t ret = ::pal_stream_flush(mPalHandle); ret) {
        LOG(ERROR) << __func__ << mLogPrefix << " failed to flush the stream, ret:" << ret;
        return ret;
    }

    // after flush operation
    if (mTag == Usecase::COMPRESS_OFFLOAD_PLAYBACK) {
        std::get<CompressPlayback>(mExt).onFlush();
    } else if (mTag == Usecase::PCM_OFFLOAD_PLAYBACK) {
        std::get<PcmOffloadPlayback>(mExt).onFlush();
    }

    LOG(DEBUG) << __func__ << mLogPrefix;
    return ::android::OK;
}

::android::status_t StreamOutPrimary::pause() {
    if (!mPalHandle) {
        LOG(WARNING) << __func__ << mLogPrefix << ": stream is not configured ";
        return ::android::OK;
    }

    if (mTag == Usecase::MMAP_PLAYBACK) {
        // pause is stop for mmap
        return stopMMAP();
    }

    if (!mHwPauseSupported) {
        LOG(VERBOSE) << __func__ << mLogPrefix << " unsupported operation!!, Hence ignored";
        return ::android::OK;
    }

    if (int32_t ret = pal_stream_pause(mPalHandle); ret) {
        LOG(ERROR) << __func__ << mLogPrefix
                   << " failed to pause the stream, ret:" << std::to_string(ret);
        return ret;
    }
    mIsPaused = true;
    LOG(DEBUG) << __func__ << mLogPrefix;
    return ::android::OK;
}

void StreamOutPrimary::resume() {
    if (mTag == Usecase::MMAP_PLAYBACK) {
        if (int32_t ret = pal_stream_start(mPalHandle); ret) {
            LOG(ERROR) << __func__ << mLogPrefix << " failed to start the stream, ret:" << ret;
        }
    } else {
        if (int32_t ret = ::pal_stream_resume(mPalHandle); ret) {
            LOG(ERROR) << __func__ << mLogPrefix << " failed to resume the stream, ret:" << ret;
        }
    }
    LOG(DEBUG) << __func__ << mLogPrefix;
    mIsPaused = false;
}

::android::status_t StreamOutPrimary::standby() {
    if (!mPalHandle) {
        LOG(WARNING) << __func__ << mLogPrefix << ": stream is not configured ";
        return ::android::OK;
    }

    shutdown_I();
    LOG(DEBUG) << __func__ << mLogPrefix;
    return ::android::OK;
}

::android::status_t StreamOutPrimary::start() {
    // hardware is expected to up on start
    // but we are doing on first write
    LOG(DEBUG) << __func__ << mLogPrefix;

    if (mTag == Usecase::MMAP_PLAYBACK) {
        return startMMAP();
    }

    if (mPalHandle && mIsPaused) {
        resume();
    }

    if (mConnectedDevices.empty()) {
        LOG(DEBUG) << __func__ << mLogPrefix << ": stream is not connected";
        return ::android::OK;
    }
    if (hasOutputVoipRxFlag(mMixPortConfig.flags.value()) ||
        hasOutputDeepBufferFlag(mMixPortConfig.flags.value())) {
        if (auto telephony = mContext.getTelephony().lock()) {
            telephony->onPlaybackStart(mConnectedDevices);
        }
    }
    return ::android::OK;
}

::android::status_t StreamOutPrimary::transfer(void* buffer, size_t frameCount,
                                               size_t* actualFrameCount, int32_t* latencyMs) {
    if (!mPalHandle) {
        // configure on first transfer or after stand by
        configure();
        if (!mPalHandle) {
            LOG(ERROR) << __func__ << mLogPrefix << ": failed to configure";
            return onWriteError(frameCount, actualFrameCount);
        }
    }

    if (mIsPaused) {
        resume();
    }

    if (mTag == Usecase::COMPRESS_OFFLOAD_PLAYBACK) {
        /**
         * upon partial drain, gapless metadata resets in tinycompress.
         * if there is a write after that, we would need configure the gapless
         * gain.
         */
        auto& compressPlayback = std::get<CompressPlayback>(mExt);
        if (!(compressPlayback.isGaplessConfigured())) {
            compressPlayback.configureGapless(mPalHandle);
        }
    }

    if (frameCount == 0) {
        *actualFrameCount = 0;
        return burstZero();
    }

    pal_buffer palBuffer{};
    palBuffer.buffer = static_cast<uint8_t*>(buffer);
    palBuffer.size = frameCount * mFrameSizeBytes;

    ssize_t bytesWritten;
    if (mBufferFormatConverter.has_value()) {
        bytesWritten = convertBufferAndWrite(buffer, frameCount);
    } else if (mTag == Usecase::HAPTICS_PLAYBACK && mHapticsPalHandle) {
        bytesWritten = hapticsWrite(buffer, frameCount);
    } else {
        bytesWritten = ::pal_stream_write(mPalHandle, &palBuffer);
    }
    if (bytesWritten < 0) {
        LOG(ERROR) << __func__ << mLogPrefix << " write failed, ret: " << bytesWritten;
        return onWriteError(frameCount, actualFrameCount);
    }

    *actualFrameCount = static_cast<size_t>(bytesWritten / mFrameSizeBytes);

#ifdef VERY_VERBOSE_LOGGING
    LOG(VERBOSE) << __func__ << mLogPrefix << ": byteswritten: " << bytesWritten;
#endif

    // Todo findout write latency
    *latencyMs = mContext.getNominalLatencyMs();
    return ::android::OK;
}

::android::status_t StreamOutPrimary::convertBufferAndWrite(const void* buffer, size_t frameCount) {
    auto& converter = *mBufferFormatConverter.value();
    if (auto result = converter.convert(buffer, frameCount * mFrameSizeBytes)) {
        pal_buffer palBuffer{};
        palBuffer.buffer = result->first;
        palBuffer.size = result->second;
        ssize_t bytesWritten = ::pal_stream_write(mPalHandle, &palBuffer);
        if (bytesWritten >= 0) {
            bytesWritten = (bytesWritten * converter.getInputBytesPerSample()) /
                           converter.getOutputBytesPerSample();
            return bytesWritten;
        }
    }

    return -EINVAL;
}
::android::status_t StreamOutPrimary::hapticsWrite(const void* buffer, size_t frameCount) {
    int ret_audio = 0;
    int ret_haptics = 0;
    bool allocHapticsBuffer = false;
    struct pal_buffer audioBuf;
    struct pal_buffer hapticsBuf;
    size_t srcIndex = 0, audIndex = 0, hapIndex = 0;
    uint8_t channelCount =  getChannelCount(mMixPortConfig.channelMask.value());
    uint8_t bytesPerSample = getPcmSampleSizeInBytes(mMixPortConfig.format.value().pcm);
    uint32_t frameSize = channelCount * bytesPerSample;

    // Calculate Haptics Buffer size

    uint32_t hapticsFrameSize = mHapticsChannelCount * bytesPerSample;
    uint32_t audioFrameSize = frameSize - hapticsFrameSize;
    uint32_t totalHapticsBufferSize = frameCount * hapticsFrameSize;


    if (!mHapticsBuffer) {
        allocHapticsBuffer = true;
    } else if (mHapticsBufSize < totalHapticsBufferSize) {
        allocHapticsBuffer = true;
        mHapticsBufSize = 0;
    }

    if (allocHapticsBuffer) {
        mHapticsBuffer = std::make_unique<uint8_t[]>(totalHapticsBufferSize);
        if(!mHapticsBuffer) {
            LOG(ERROR) << __func__ << ": Failed to allocate haptic buffer";
            return -ENOMEM;
        }
        mHapticsBufSize = totalHapticsBufferSize;
    }

    audioBuf.buffer = (uint8_t *)buffer;
    audioBuf.size = frameCount * audioFrameSize;
    audioBuf.offset = 0;
    hapticsBuf.buffer  = reinterpret_cast<uint8_t*>(mHapticsBuffer.get());
    hapticsBuf.size = frameCount * hapticsFrameSize;
    hapticsBuf.offset = 0;

    for (size_t i = 0; i < frameCount; i++) {
        memcpy((uint8_t *)(audioBuf.buffer) + audIndex, (uint8_t *)(audioBuf.buffer) + srcIndex,
            audioFrameSize);
        audIndex += audioFrameSize;
        srcIndex += audioFrameSize;

        memcpy((uint8_t *)(hapticsBuf.buffer) + hapIndex, (uint8_t *)(audioBuf.buffer) + srcIndex,
            hapticsFrameSize);
        hapIndex += hapticsFrameSize;
        srcIndex += hapticsFrameSize;
    }
    // write audio data
    ret_audio = ::pal_stream_write(mPalHandle, &audioBuf);
    // write haptics data
    ret_haptics = ::pal_stream_write(mHapticsPalHandle, &hapticsBuf);

    if (ret_audio < 0 || ret_haptics < 0) {
        return (ret_audio < 0) ? ret_audio : ret_haptics;
    }

    return (frameSize * frameCount);
}

::android::status_t StreamOutPrimary::refinePosition(StreamDescriptor::Reply* reply) {

    if (mTag == Usecase::COMPRESS_OFFLOAD_PLAYBACK) {
        reply->observable.frames = std::get<CompressPlayback>(mExt).getPositionInFrames(mPalHandle);
    } else if (mTag == Usecase::PCM_OFFLOAD_PLAYBACK) {
        reply->observable.frames =
                std::get<PcmOffloadPlayback>(mExt).getPositionInFrames(mPalHandle);
    } else if (mTag == Usecase::MMAP_PLAYBACK) {
        int32_t ret = std::get<MMapPlayback>(mExt).getMMapPosition(&(reply->hardware.frames),
                                                                       &(reply->hardware.timeNs));
        if (ret != 0) {
            LOG(ERROR) << __func__ << mLogPrefix << ": mmap position failed";
            return android::INVALID_OPERATION;
        }

        int64_t totalDelayFrames = 0;
        totalDelayFrames =
                mContext.getNominalLatencyMs() * mMixPortConfig.sampleRate.value().value / 1000;
        reply->observable.frames = (reply->hardware.frames > totalDelayFrames)
                                           ? (reply->hardware.frames - totalDelayFrames)
                                           : 0;
        reply->observable.timeNs = reply->hardware.timeNs;
        LOG(VERBOSE) << __func__ << mLogPrefix << ": hw frames " << reply->hardware.frames
                     << " observable frames " << reply->observable.frames
                     << " time : " << reply->observable.timeNs;
        return ::android::OK;
    } else {
        int64_t totalDelayFrames = 0;
        totalDelayFrames = mContext.getNominalLatencyMs() *
                           mMixPortConfig.sampleRate.value().value / 1000;
        reply->observable.frames = (reply->observable.frames > totalDelayFrames) ?
                                   (reply->observable.frames - totalDelayFrames) : 0;
    }

    // if the stream is connected to any bluetooth device, consider bluetooth encoder latency
    if (hasBluetoothDevice(mConnectedDevices)) {
        const auto& latencyMs = mPlatform.getBluetoothLatencyMs(mConnectedDevices);
        const auto& sampleRate = mMixPortConfig.sampleRate.value().value;
        const auto btExtraFrames = latencyMs * sampleRate / 1000;
        // Todo, Check do we want to consider this for MMAP usecase
        reply->observable.frames = (reply->observable.frames > btExtraFrames) ?
                                   (reply->observable.frames - btExtraFrames) : 0;
        reply->latencyMs += latencyMs;
    }
    reply->observable.timeNs = ::android::uptimeNanos();
    LOG(VERBOSE) <<__func__ << mLogPrefix << " observable.frames: " << reply->observable.frames;

    return ::android::OK;
}

void StreamOutPrimary::shutdown() {
    shutdown_I();
}

// end of DriverInterface Methods

// start of PlatformStreamCallback methods

void StreamOutPrimary::onTransferReady() {
    publishTransferReady();
}

void StreamOutPrimary::onDrainReady() {
    publishDrainReady();
}

void StreamOutPrimary::onError() {
    publishError();
}

// end of PlatformStreamCallback methods

// start of IStreamOut Methods
ndk::ScopedAStatus StreamOutPrimary::updateOffloadMetadata(
        const AudioOffloadMetadata& in_offloadMetadata) {
    if (isClosed()) {
        LOG(ERROR) << __func__ << mLogPrefix << ": stream was closed";
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }
    if (!mOffloadInfo.has_value()) {
        LOG(ERROR) << __func__ << mLogPrefix << ": not a compressed offload stream";
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
    if (in_offloadMetadata.sampleRate < 0) {
        LOG(ERROR) << __func__ << mLogPrefix
                   << ": invalid sample rate value: " << in_offloadMetadata.sampleRate;
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    if (in_offloadMetadata.averageBitRatePerSecond < 0) {
        LOG(ERROR) << __func__ << mLogPrefix
                   << ": invalid average BPS value: " << in_offloadMetadata.averageBitRatePerSecond;
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    if (in_offloadMetadata.delayFrames < 0) {
        LOG(ERROR) << __func__ << mLogPrefix
                   << ": invalid delay frames value: " << in_offloadMetadata.delayFrames;
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    if (in_offloadMetadata.paddingFrames < 0) {
        LOG(ERROR) << __func__ << mLogPrefix
                   << ": invalid padding frames value: " << in_offloadMetadata.paddingFrames;
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    if (mTag != Usecase::COMPRESS_OFFLOAD_PLAYBACK) {
        LOG(WARNING) << __func__ << mLogPrefix << ": expected CompressOffloadPlayback instead of "
                     << mTagName;
        return ndk::ScopedAStatus::ok();
    }

    mOffloadMetadata = in_offloadMetadata;

    if (mTag == Usecase::COMPRESS_OFFLOAD_PLAYBACK) {
        std::get<CompressPlayback>(mExt).updateOffloadMetadata(mOffloadMetadata.value());
    }

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus StreamOutPrimary::getHwVolume(std::vector<float>* _aidl_return) {
    if (!mHwVolumeSupported) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
    *_aidl_return = mVolumes;
    LOG(VERBOSE) << __func__ << mLogPrefix << ::android::internal::ToString(mVolumes);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus StreamOutPrimary::setHwVolume(const std::vector<float>& in_channelVolumes) {
    if (!mHwVolumeSupported) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }

    if (mVolumes.size() != in_channelVolumes.size()) {
        LOG(ERROR) << __func__ << mLogPrefix << " channel count mismatch with port, expected "
                   << mVolumes.size() << " got " << in_channelVolumes.size();
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }

    auto isVolumeInRange = [](const std::vector<float>& volumes) {
        return std::all_of(volumes.begin(), volumes.end(),
                           [](float vol) { return (vol >= 0.0f && vol <= 1.0f); });
    };

    if (!isVolumeInRange(in_channelVolumes)) {
        LOG(ERROR) << __func__ << mLogPrefix << " out of range volume "
                   << ::android::internal::ToString(in_channelVolumes);
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }

    if (!mPalHandle) {
        mVolumes = in_channelVolumes;
        LOG(DEBUG) << __func__ << mLogPrefix << " cache volume "
                   << ::android::internal::ToString(in_channelVolumes);
        return ndk::ScopedAStatus::ok();
    }

    if (int32_t ret = mPlatform.setVolume(mPalHandle, in_channelVolumes); ret) {
        LOG(ERROR) << __func__ << mLogPrefix << " failed to set volume";
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }

    mVolumes = in_channelVolumes;

    LOG(DEBUG) << __func__ << mLogPrefix << ::android::internal::ToString(mVolumes);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus StreamOutPrimary::getPlaybackRateParameters(AudioPlaybackRate* _aidl_return) {
    if (!mPlatform.usecaseSupportsOffloadSpeed(mTag)) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }

    LOG(DEBUG) << __func__ << mLogPrefix << mPlaybackRate.toString();
    *_aidl_return = mPlaybackRate;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus StreamOutPrimary::setPlaybackRateParameters(
        const AudioPlaybackRate& in_playbackRate) {
    auto ret = mPlatform.setPlaybackRate(mPalHandle, mTag, in_playbackRate);
    if (PlaybackRateStatus::SUCCESS == ret) {
        mPlaybackRate = in_playbackRate;
        LOG(DEBUG) << __func__ << mLogPrefix << mPlaybackRate.toString();
        return ndk::ScopedAStatus::ok();
    } else if (PlaybackRateStatus::UNSUPPORTED == ret) {
        LOG(VERBOSE) << __func__ << mLogPrefix << "raise EX_UNSUPPORTED_OPERATION exception for "
                     << mPlaybackRate.toString();
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }

    LOG(ERROR) << __func__ << mLogPrefix << "raise EX_ILLEGAL_ARGUMENT exception for "
                 << mPlaybackRate.toString();
    return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
}

// end of IStreamOut Methods

// start of StreamCommonInterface Methods

ndk::ScopedAStatus StreamOutPrimary::updateMetadataCommon(const Metadata& metadata) {
    if (isClosed()) {
        LOG(ERROR) << __func__ << mLogPrefix << ": stream was closed";
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }

    if (metadata.index() != mMetadata.index()) {
        LOG(FATAL) << __func__ << mLogPrefix << ": changing metadata variant is not allowed";
    }
    StreamOutPrimary::sourceMetadata_mutex_.lock();
    mMetadata = metadata;

    if (mTag == Usecase::COMPRESS_OFFLOAD_PLAYBACK) {
        auto& compressPlayback = std::get<CompressPlayback>(mExt);
        compressPlayback.updateSourceMetadata(std::get<SourceMetadata>(mMetadata));
    }
    int callState = mPlatform.getCallState();
    int callMode = mPlatform.getCallMode();
    bool voiceActive = ((callState == 2) || (callMode == 2));

    setAggregateSourceMetadata(voiceActive);
    StreamOutPrimary::sourceMetadata_mutex_.unlock();

    return ndk::ScopedAStatus::ok();
}

int32_t StreamOutPrimary::setAggregateSourceMetadata(bool voiceActive) {
    ssize_t track_count_total = 0;

    std::vector<playback_track_metadata_t> total_tracks;
    source_metadata_t btSourceMetadata;

    ModulePrimary::outListMutex.lock();
    std::vector<std::weak_ptr<StreamOut>>& outStreams = ModulePrimary::getOutStreams();
    if (voiceActive || outStreams.empty()) {
        ModulePrimary::outListMutex.unlock();
        return 0;
    }
    auto removeStreams = [&](std::weak_ptr<StreamOut> streamOut) -> bool {
        if (!streamOut.lock()) return true;
        return streamOut.lock()->isClosed();
    };
    outStreams.erase(std::remove_if(outStreams.begin(), outStreams.end(), removeStreams),
                     outStreams.end());

    LOG(VERBOSE) << __func__ << mLogPrefix << " out streams not empty size is " << outStreams.size();

    for (auto it = outStreams.begin(); it < outStreams.end(); it++) {
        if (it->lock() && !it->lock()->isClosed()) {
            ::aidl::android::hardware::audio::common::SourceMetadata srcMetadata;
            it->lock()->getMetadata(srcMetadata);
            track_count_total += srcMetadata.tracks.size();
        } else {
        }
    }
    LOG(VERBOSE) << __func__ << mLogPrefix << " out streams size after deleting : " << outStreams.size()
                 << " total track count " << track_count_total;

    if (track_count_total <= 0) {
        ModulePrimary::outListMutex.unlock();
        return 0;
    }

    total_tracks.resize(track_count_total);
    btSourceMetadata.track_count = track_count_total;
    btSourceMetadata.tracks = total_tracks.data();

    int32_t totalTracks = 0;
    for (auto it = outStreams.begin(); it != outStreams.end(); it++) {
        ::aidl::android::hardware::audio::common::SourceMetadata srcMetadata;
        if (it->lock()) {
            it->lock()->getMetadata(srcMetadata);
            for (auto& item : srcMetadata.tracks) {
                // check tracks size in this stream metadata not to exceed total count
                if (totalTracks >= track_count_total) {
                    LOG(WARNING) << __func__ << mLogPrefix << ": mismatch in total tracks for metadata allocation";
                    break;
                }

                /* currently after cs call ends, we are getting metadata as
                * usage voice and content speech, this is causing BT to again
                * open call session, so added below check to send metadata of
                * voice only if call is active, else discard it
                */

                if (!voiceActive && (mPlatform.getCallMode() != 3) &&
                    (AUDIO_USAGE_VOICE_COMMUNICATION == static_cast<audio_usage_t>(item.usage)) &&
                    (AUDIO_CONTENT_TYPE_SPEECH ==
                     static_cast<audio_content_type_t>(item.contentType))) {
                    btSourceMetadata.track_count--;
                } else {
                    btSourceMetadata.tracks->usage = static_cast<audio_usage_t>(item.usage);
                    btSourceMetadata.tracks->content_type =
                            static_cast<audio_content_type_t>(item.contentType);
                    LOG(VERBOSE) << __func__ << mLogPrefix << " source metadata usage is "
                                 << btSourceMetadata.tracks->usage << " content is "
                                 << btSourceMetadata.tracks->content_type;
                   ++btSourceMetadata.tracks;
                }
                ++totalTracks;
            }
        }
    }
    btSourceMetadata.tracks = total_tracks.data();
    pal_set_param(PAL_PARAM_ID_SET_SOURCE_METADATA, (void*)&btSourceMetadata, 0);
    ModulePrimary::outListMutex.unlock();
    return 0;
}

ndk::ScopedAStatus StreamOutPrimary::getVendorParameters(
        const std::vector<std::string>& in_ids, std::vector<VendorParameter>* _aidl_return) {
    if (mTag == Usecase::COMPRESS_OFFLOAD_PLAYBACK) {
        auto& compressPlayback = std::get<CompressPlayback>(mExt);
        return compressPlayback.getVendorParameters(in_ids, _aidl_return);
    }

    for (const auto& id : in_ids) {
        if (id == Parameters::kSupportsHwSuspend) {
            if (mTag == Usecase::LOW_LATENCY_PLAYBACK) {
                _aidl_return->push_back(makeVendorParameter(id, "1"));
            }
        } else if (id == Parameters::kIsDirectPCMTrack) {
            if (mTag == Usecase::PCM_OFFLOAD_PLAYBACK) {
                _aidl_return->push_back(makeVendorParameter(id, "true"));
            }
        }
    }

    if (in_ids.size() != _aidl_return->size())
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus StreamOutPrimary::setVendorParameters(
        const std::vector<VendorParameter>& in_parameters, bool in_async) {
    if (mTag == Usecase::COMPRESS_OFFLOAD_PLAYBACK) {
        auto& compressPlayback = std::get<CompressPlayback>(mExt);
        return compressPlayback.setVendorParameters(in_parameters, in_async);
    }
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus StreamOutPrimary::addEffect(
        const std::shared_ptr<::aidl::android::hardware::audio::effect::IEffect>& in_effect) {
    if (in_effect == nullptr) {
        LOG(VERBOSE) << __func__ << mLogPrefix << ": null effect";
    } else {
        LOG(VERBOSE) << __func__ << mLogPrefix << ": effect Binder" << in_effect->asBinder().get();
    }
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus StreamOutPrimary::removeEffect(
        const std::shared_ptr<::aidl::android::hardware::audio::effect::IEffect>& in_effect) {
    if (in_effect == nullptr) {
        LOG(VERBOSE) << __func__ << mLogPrefix << ": null effect";
    } else {
        LOG(VERBOSE) << __func__ << mLogPrefix << ": effect Binder" << in_effect->asBinder().get();
    }
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

// end of StreamCommonInterface Methods

size_t StreamOutPrimary::getPlatformDelay() const noexcept {
    return 0;
}

::android::status_t StreamOutPrimary::onWriteError(const size_t sleepFrameCount, size_t* const consumedFrameCount) {
    shutdown_I();
    if (mTag == Usecase::COMPRESS_OFFLOAD_PLAYBACK) {
        // return error for offload, so that FW sends data again
        LOG(ERROR) << __func__ << mLogPrefix << ": cannot afford write failure";
        *consumedFrameCount = 0;
        return ::android::DEAD_OBJECT;
    }
    auto& sampleRate = mMixPortConfig.sampleRate.value().value;
    if (sampleRate == 0) {
        LOG(ERROR) << __func__ << mLogPrefix << ": cannot afford write failure, sampleRate is zero";
        *consumedFrameCount = 0;
        return ::android::UNEXPECTED_NULL;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds((sleepFrameCount * 1000) / sampleRate));
    *consumedFrameCount = sleepFrameCount;
    LOG(WARNING) << __func__ << mLogPrefix << ": ignoring this write";
    return ::android::OK;
}

void StreamOutPrimary::configure() {

    if(hasOutputMMapFlag(mMixPortConfig.flags.value())){
        // this API doesn't handle for MMAP
        return;
    }

    const auto startTime = std::chrono::steady_clock::now();
    std::unique_ptr<pal_channel_info> palNonHapticChannelInfo;
    std::unique_ptr<pal_channel_info> palHapticChannelInfo;
    AudioChannelLayout channelLayout;
    AudioChannelLayout hapticChannelLayout;

    auto attr = mPlatform.getPalStreamAttributes(mMixPortConfig, false);

    if (!attr) {
        LOG(ERROR) << __func__ << mLogPrefix << " no pal attributes found";
        return;
    }

    if (mTag == Usecase::DEEP_BUFFER_PLAYBACK || mTag == Usecase::PRIMARY_PLAYBACK) {
        attr->type = PAL_STREAM_DEEP_BUFFER;
    } else if (mTag == Usecase::LOW_LATENCY_PLAYBACK) {
        attr->type = PAL_STREAM_LOW_LATENCY;

        auto countProxyDevices = std::count_if(mConnectedDevices.cbegin(), mConnectedDevices.cend(),
                                                isIPDevice);
        if (countProxyDevices > 0) {
            attr->type = PAL_STREAM_PROXY;
            attr->info.opt_stream_info.rx_proxy_type = PAL_STREAM_PROXY_RX_WFD;
            LOG(INFO) << __func__ << mLogPrefix << ": proxy playback on IPV4";
        }
    } else if (mTag == Usecase::COMPRESS_OFFLOAD_PLAYBACK) {
        attr->type = PAL_STREAM_COMPRESSED;
    } else if (mTag == Usecase::PCM_OFFLOAD_PLAYBACK) {
        attr->type = PAL_STREAM_PCM_OFFLOAD;
    } else if (mTag == Usecase::VOIP_PLAYBACK) {
        attr->type = PAL_STREAM_VOIP_RX;
    } else if (mTag == Usecase::SPATIAL_PLAYBACK) {
        attr->type = PAL_STREAM_SPATIAL_AUDIO;
    } else if (mTag == Usecase::ULL_PLAYBACK) {
        attr->type = PAL_STREAM_ULTRA_LOW_LATENCY;
    } else if (mTag == Usecase::IN_CALL_MUSIC) {
        attr->type = PAL_STREAM_VOICE_CALL_MUSIC;
        attr->info.incall_music_info.local_playback = mPlatform.getInCallMusicState();
    } else if (mTag == Usecase::HAPTICS_PLAYBACK) {
        attr->type = PAL_STREAM_LOW_LATENCY;
        channelLayout = AudioChannelLayout::make<AudioChannelLayout::Tag::layoutMask>
           (mMixPortConfig.channelMask.value().get<AudioChannelLayout::Tag::layoutMask>() & ~ (AudioChannelLayout::LAYOUT_HAPTIC_AB));
        LOG(DEBUG) << __func__ << " pal channel info for haptics data stream "
               << channelLayout.toString();
        palNonHapticChannelInfo = PlatformConverter::getPalChannelInfoForChannelCount(
          getChannelCount(channelLayout));

        if (palNonHapticChannelInfo == nullptr) {
            LOG(ERROR) << __func__ << " failed to find corresponding pal channel info for "
               << channelLayout.toString();
            return;
        } else {
            attr->out_media_config.ch_info = *palNonHapticChannelInfo;
        }
    } else {
        LOG(ERROR) << __func__ << mLogPrefix << " invalid usecase to configure";
        return;
    }

    LOG(VERBOSE) << __func__ << mLogPrefix << " assigned pal stream type:" << attr->type;

    auto palDevices =
            mPlatform.configureAndFetchPalDevices(mMixPortConfig, mTag, mConnectedDevices);
    if (!palDevices.size()) {
        LOG(ERROR) << __func__ << mLogPrefix << " no connected devices on stream!!";
        return;
    }

    uint64_t cookie = reinterpret_cast<uint64_t>(this);
    pal_stream_callback palFn = nullptr;
    if (mTag == Usecase::COMPRESS_OFFLOAD_PLAYBACK) {
        auto& compressPlayback = std::get<CompressPlayback>(mExt);
        cookie = reinterpret_cast<uint64_t>(&compressPlayback);
        palFn = CompressPlayback::palCallback;
        /* TODO check any dynamic update as per offload metadata or source
         * metadata */
        if (mOffloadInfo.value().bitWidth != 0) {
            attr->out_media_config.bit_width = mOffloadInfo.value().bitWidth;
        }
        attr->flags = static_cast<pal_stream_flags_t>(PAL_STREAM_FLAG_NON_BLOCKING);
    }
    if (mTag == Usecase::ULL_PLAYBACK) {
        attr->flags = static_cast<pal_stream_flags_t>(PAL_STREAM_FLAG_MMAP);
    }

    const auto palOpenApiStartTime = std::chrono::steady_clock::now();
    if (int32_t ret = ::pal_stream_open(attr.get(), palDevices.size(), palDevices.data(), 0,
                                        nullptr, palFn, cookie, &(this->mPalHandle));
        ret) {
        LOG(ERROR) << __func__ << mLogPrefix << " pal stream open failed!!! ret:" << ret;
        mPalHandle = nullptr;
        return;
    }

    setHwVolume(mVolumes);

    if (mTag == Usecase::HAPTICS_PLAYBACK) {

        hapticChannelLayout = AudioChannelLayout::make<AudioChannelLayout::Tag::layoutMask>
           (mMixPortConfig.channelMask.value().get<AudioChannelLayout::Tag::layoutMask>() & (AudioChannelLayout::LAYOUT_HAPTIC_AB));

        LOG(DEBUG) << __func__ << mLogPrefix << " pal channel info for haptics stream "
               << hapticChannelLayout.toString();

        palHapticChannelInfo = PlatformConverter::getPalChannelInfoForChannelCount(
          getChannelCount(hapticChannelLayout));

        if (palHapticChannelInfo == nullptr) {
            LOG(ERROR) << __func__ << mLogPrefix << " failed to find pal channel info for haptics"
               << hapticChannelLayout.toString();
            return ;
        }

        // set haptics stream attributes
        struct pal_stream_attributes hapticsStreamAttributes {};
        hapticsStreamAttributes.type = PAL_STREAM_HAPTICS;
        hapticsStreamAttributes.flags = static_cast<pal_stream_flags_t>(0);
        hapticsStreamAttributes.direction = PAL_AUDIO_OUTPUT;
        hapticsStreamAttributes.out_media_config.sample_rate = Platform::kDefaultOutputSampleRate;
        hapticsStreamAttributes.out_media_config.bit_width = Platform::kDefaultPCMBidWidth;
        hapticsStreamAttributes.out_media_config.aud_fmt_id = Platform::kDefaultPalPCMFormat;
        hapticsStreamAttributes.out_media_config.ch_info = *(palHapticChannelInfo);
        hapticsStreamAttributes.info.opt_stream_info.haptics_type = PAL_STREAM_HAPTICS_RINGTONE;
        mHapticsChannelCount = hapticsStreamAttributes.out_media_config.ch_info.channels;

        // set haptics device
        struct pal_device hapticsDevice {};
        hapticsDevice.id = PAL_DEVICE_OUT_HAPTICS_DEVICE;
        hapticsDevice.config.sample_rate = Platform::kDefaultOutputSampleRate;
        hapticsDevice.config.bit_width = Platform::kDefaultPCMBidWidth;
        hapticsDevice.config.ch_info = hapticsStreamAttributes.out_media_config.ch_info;
        hapticsDevice.config.aud_fmt_id = Platform::kDefaultPalPCMFormat;

        if (int32_t ret = ::pal_stream_open(&hapticsStreamAttributes, 1, &hapticsDevice, 0,
                                        nullptr, palFn, cookie, &(this->mHapticsPalHandle))) {
            LOG(ERROR) << __func__ << mLogPrefix << " pal_stream_open failed for haptics " << ret;
        }
    }

    const auto palOpenApiEndTime = std::chrono::steady_clock::now();

    if (karaoke) {
        int size = palDevices.size();
        mAudExt.mKarokeExtension->karaoke_open(palDevices[size - 1].id, palFn,
                                               attr.get()->out_media_config.ch_info);
    }
    auto bufConfig = getBufferConfig();
    if (mTag == Usecase::ULL_PLAYBACK) {
        //The buffer size for ULL_PLAYBACK set to PAL should not be more than 2ms
        const size_t durationMs = 1; // set to 1ms
        size_t frameSizeInBytes = getFrameSizeInBytes(
                mMixPortConfig.format.value(), mMixPortConfig.channelMask.value());
        bufConfig.bufferSize = durationMs *
                    (mMixPortConfig.sampleRate.value().value /1000) * frameSizeInBytes;
    }
    size_t ringBufSizeInBytes = bufConfig.bufferSize;
    const size_t ringBufCount = bufConfig.bufferCount;

    if (mTag == Usecase::HAPTICS_PLAYBACK && mHapticsPalHandle) {
        size_t hapticsFrameSize = getFrameSizeInBytes(mMixPortConfig.format.value(), channelLayout);
        ringBufSizeInBytes = HapticsPlayback::getFrameCount(mMixPortConfig) * hapticsFrameSize;
    }

    if (auto converter = Platform::requiresBufferReformat(mMixPortConfig);
        converter && !mBufferFormatConverter.has_value()) {
        mBufferFormatConverter = std::make_unique<BufferFormatConverter>(
                converter->first, converter->second, ringBufSizeInBytes);
        LOG(DEBUG) << __func__ << mLogPrefix << "created format converter from format "
                   << converter->first << " to format " << converter->second;
    }

    auto palBufferConfig = mPlatform.getPalBufferConfig(ringBufSizeInBytes, ringBufCount);
    LOG(DEBUG) << __func__ << mLogPrefix << "set pal_stream_set_buffer_size to "
               << std::to_string(ringBufSizeInBytes) << " with count "
               << std::to_string(ringBufCount);
    if (int32_t ret =
                ::pal_stream_set_buffer_size(this->mPalHandle, nullptr, palBufferConfig.get());
        ret) {
        LOG(ERROR) << __func__ << mLogPrefix << " pal_stream_set_buffer_size failed!!! ret:" << ret;
        ::pal_stream_close(mPalHandle);
        mPalHandle = nullptr;
        return;
    }
    if (mTag == Usecase::HAPTICS_PLAYBACK && mHapticsPalHandle) {
        const size_t hapticsRingBufCount = ringBufCount;
        size_t hapticsFrameSize = getFrameSizeInBytes(mMixPortConfig.format.value(), hapticChannelLayout);
        const size_t hapticsRingBufSizeInBytes = HapticsPlayback::getFrameCount(mMixPortConfig) * hapticsFrameSize;

        auto hapticsPalBufferConfig = mPlatform.getPalBufferConfig(hapticsRingBufSizeInBytes, hapticsRingBufCount);
        if (int32_t ret =
                 ::pal_stream_set_buffer_size(this->mHapticsPalHandle, nullptr, hapticsPalBufferConfig.get());
            ret) {
            LOG(ERROR) << __func__ << mLogPrefix << " pal_stream_set_buffer_size failed!!! ret:" << ret;
            ::pal_stream_close(mHapticsPalHandle);
            mHapticsPalHandle = nullptr;
            return;
        }
    }

    if (mTag == Usecase::COMPRESS_OFFLOAD_PLAYBACK) {
        // Must be before pal stream start
        std::get<CompressPlayback>(mExt).setAndConfigureCodecInfo(mPalHandle);
    }

    const auto palStartApiStartTime = std::chrono::steady_clock::now();
    if (int32_t ret = ::pal_stream_start(this->mPalHandle); ret) {
        LOG(ERROR) << __func__ << mLogPrefix << " pal_stream_start failed, ret:" << ret;
        ::pal_stream_close(mPalHandle);
        mPalHandle = nullptr;
        return;
        }

    if (mTag == Usecase::HAPTICS_PLAYBACK && mHapticsPalHandle) {
        if (int32_t ret = ::pal_stream_start(this->mHapticsPalHandle); ret) {
            LOG(ERROR) << __func__ << mLogPrefix << " failed to start haptics stream. ret:" << ret;
            ::pal_stream_close(mHapticsPalHandle);
            mHapticsPalHandle = nullptr;
            return;
        }
    }

    const auto palStartApiEndTime = std::chrono::steady_clock::now();

    if (karaoke) mAudExt.mKarokeExtension->karaoke_start();

    LOG(VERBOSE) << __func__ << mLogPrefix << " pal_stream_start successful";

    if (mPlaybackRate != sDefaultPlaybackRate) {
        LOG(DEBUG) << __func__ << mLogPrefix << ": using playspeed " << mPlaybackRate.speed;
        mPlatform.setPlaybackRate(mPalHandle, mTag, mPlaybackRate);
    }

    LOG(INFO) << __func__ << mLogPrefix << ": stream is configured";
    enableOffloadEffects(true);
    const auto endTime = std::chrono::steady_clock::now();
    using FloatMillis = std::chrono::duration<float, std::milli>;
    const float palStreamOpenTimeTaken =
            std::chrono::duration_cast<FloatMillis>(palOpenApiEndTime - palOpenApiStartTime)
                    .count();
    const float palStreamStartTimeTaken =
            std::chrono::duration_cast<FloatMillis>(palStartApiEndTime - palStartApiStartTime)
                    .count();
    const float timeTaken = std::chrono::duration_cast<FloatMillis>(endTime - startTime).count();
    LOG(INFO) << __func__ << mLogPrefix << ": completed in " << timeTaken
              << " ms [pal_stream_open: " << palStreamOpenTimeTaken
              << ", ms pal_stream_start: " << palStreamStartTimeTaken << " ms]";
}

void StreamOutPrimary::enableOffloadEffects(const bool enable) {
    if (mTag == Usecase::COMPRESS_OFFLOAD_PLAYBACK || mTag == Usecase::PCM_OFFLOAD_PLAYBACK) {
        auto& ioHandle = mMixPortConfig.ext.get<AudioPortExt::Tag::mix>().handle;
        if (enable) {
            mHalEffects.startEffect(ioHandle, mPalHandle);
            LOG(VERBOSE) << __func__ << mLogPrefix;
        } else {
            mHalEffects.stopEffect(ioHandle);
        }
    }
}

ndk::ScopedAStatus StreamOutPrimary::getRecommendedLatencyModes(
        std::vector<::aidl::android::media::audio::common::AudioLatencyMode>* _aidl_return) {

     int ret = 0;

     if (!hasBluetoothA2dpDevice(mConnectedDevices)) {
         return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
     }

     ret = mPlatform.getRecommendedLatencyModes(_aidl_return);
     if (ret) ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
     return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus StreamOutPrimary::setLatencyMode(
            ::aidl::android::media::audio::common::AudioLatencyMode in_mode) {

    LOG(DEBUG) << __func__ << mLogPrefix << ": latency mode " << toString(in_mode);
    int ret = 0;

    if (!hasBluetoothA2dpDevice(mConnectedDevices)) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }

    ret = mPlatform.setLatencyMode((uint32_t)in_mode);
    if (ret) ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);

    return ndk::ScopedAStatus::ok();
}

void StreamOutPrimary::shutdown_I() {

    if (mTag == Usecase::COMPRESS_OFFLOAD_PLAYBACK) {
        std::get<CompressPlayback>(mExt).setAndConfigureCodecInfo(nullptr);
    } else if (mTag == Usecase::MMAP_PLAYBACK) {
        std::get<MMapPlayback>(mExt).setPalHandle(nullptr);
    }

    if (karaoke) mAudExt.mKarokeExtension->karaoke_stop();

    if (mPalHandle != nullptr) {
        enableOffloadEffects(false);
        ::pal_stream_stop(mPalHandle);
        ::pal_stream_close(mPalHandle);
    }
    if (mHapticsPalHandle != nullptr) {
        ::pal_stream_stop(mHapticsPalHandle);
        ::pal_stream_close(mHapticsPalHandle);
        if (mHapticsBuffer) {
            mHapticsBuffer = nullptr;
        }
        mHapticsBufSize = 0;
    }

    if (hasOutputVoipRxFlag(mMixPortConfig.flags.value()) ||
        hasOutputDeepBufferFlag(mMixPortConfig.flags.value())) {
        if (auto telephony = mContext.getTelephony().lock()) {
            telephony->onPlaybackClose();
        }
    }

    mIsPaused = false;
    mPalHandle = nullptr;
    mHapticsPalHandle = nullptr;
    LOG(VERBOSE) << __func__ << mLogPrefix;
}

::android::status_t StreamOutPrimary::burstZero() {
    LOG(VERBOSE) << __func__ << mLogPrefix;
    if (mTag == Usecase::MMAP_PLAYBACK) {
        return startMMAP();
    }
    return ::android::OK;
}

::android::status_t StreamOutPrimary::startMMAP() {
    auto& mmap = std::get<MMapPlayback>(mExt);
    if (auto ret = mmap.start(); ret) {
        LOG(ERROR) << __func__ << mLogPrefix << ": failed";
        return ret;
    }
    return ::android::OK;
}

::android::status_t StreamOutPrimary::stopMMAP() {
    auto& mmap = std::get<MMapPlayback>(mExt);
    if (auto ret = mmap.stop(); ret) {
        LOG(ERROR) << __func__ << mLogPrefix << ": failed";
        return ret;
    }
    return ::android::OK;
}

} // namespace qti::audio::core
