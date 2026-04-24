/*
 * Changes from Qualcomm Technologies, Inc. are provided under the following license:
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#pragma once
#include <PalApi.h>
#include <aidl/android/hardware/audio/core/IModule.h>
#include <aidl/android/hardware/audio/core/VendorParameter.h>
#include <aidl/android/media/audio/common/AudioDevice.h>
#include <aidl/android/media/audio/common/AudioFormatDescription.h>
#include <aidl/android/media/audio/common/AudioPlaybackRate.h>
#include <aidl/android/media/audio/common/AudioPort.h>
#include <aidl/android/media/audio/common/AudioPortConfig.h>
#include <aidl/android/hardware/audio/core/ITelephony.h>
#include <aidl/android/media/audio/common/MicrophoneDynamicInfo.h>
#include <aidl/android/media/audio/common/MicrophoneInfo.h>
#include <qti-audio-core/AudioUsecase.h>
#include <system/audio.h>

#include <unordered_map>

namespace qti::audio::core {

struct HdmiParameters {
    int controller;
    int stream;
    pal_device_id_t deviceId;
};

enum class PlaybackRateStatus { SUCCESS, UNSUPPORTED, ILLEGAL_ARGUMENT };

using GetLatency = int32_t (*)();
using GetFrameCount =
        size_t (*)(const ::aidl::android::media::audio::common::AudioPortConfig& portConfig);
using GetBufferConfig = struct BufferConfig (*)(
        const ::aidl::android::media::audio::common::AudioPortConfig& portConfig);

/*
 * Helper to map getLatency, getFrameCount, getBufferConfig APIs
 * from platform to audiousecase. While Introducing new usecase
 * always provide the APIS.
 */
struct UsecaseOps {
    GetLatency getLatency;
    GetFrameCount getFrameCount;
    GetBufferConfig getBufferConfig;
};

template <typename UsecaseClass>
inline UsecaseOps makeUsecaseOps() {
    UsecaseOps ops;
    ops.getLatency = UsecaseClass::getLatency;
    ops.getFrameCount = UsecaseClass::getFrameCount;
    ops.getBufferConfig = UsecaseClass::getBufferConfig;
    return ops;
}

class Platform {
  private:
    explicit Platform();

    Platform(const Platform&) = delete;
    Platform& operator=(const Platform& x) = delete;

    Platform(Platform&& other) = delete;
    Platform& operator=(Platform&& other) = delete;
    static int palGlobalCallback(uint32_t event_id, uint32_t* event_data, uint64_t cookie);

  public:
    // BT related params used across
    bool bt_lc3_speech_enabled;
    static btsco_lc3_cfg_t btsco_lc3_cfg;

    mutable bool mUSBCapEnable{false};
    int mCallState;
    int mCallMode;
    static Platform& getInstance();
    size_t getFrameCount(
            const ::aidl::android::media::audio::common::AudioPortConfig& mixPortConfig,
            Usecase const& inTag = Usecase::INVALID);

    struct BufferConfig getBufferConfig(
            const ::aidl::android::media::audio::common::AudioPortConfig& mixPortConfig,
            Usecase const& inTag = Usecase::INVALID);

    int32_t getLatencyMs(
            const ::aidl::android::media::audio::common::AudioPortConfig& mixPortConfig,
            Usecase const& inTag = Usecase::INVALID);

    std::vector<::aidl::android::media::audio::common::MicrophoneInfo> getMicrophoneInfo() {
        return mMicrophoneInfo;
    }
    std::vector<::aidl::android::media::audio::common::MicrophoneDynamicInfo>
            getMicrophoneDynamicInfo(
                    const std::vector<::aidl::android::media::audio::common::AudioDevice>& devices);

    bool setParameter(const std::string& key, const std::string& value);
    bool setBluetoothParameters(const char* kvpairs);
    bool setVendorParameters(
            const std::vector<::aidl::android::hardware::audio::core::VendorParameter>&
                    in_parameters,
            bool in_async);

    std::string getParameter(const std::string& key) const;
    std::string toString() const;
    bool isSoundCardUp() const noexcept;
    bool isSoundCardDown() const noexcept;

    size_t getMinimumStreamSizeFrames(
            const std::vector<::aidl::android::media::audio::common::AudioPortConfig*>& sources,
            const std::vector<::aidl::android::media::audio::common::AudioPortConfig*>& sinks);
    std::unique_ptr<pal_stream_attributes> getPalStreamAttributes(
            const ::aidl::android::media::audio::common::AudioPortConfig& portConfig,
            const bool isInput) const;
    std::vector<pal_device> convertToPalDevices(
            const std::vector<::aidl::android::media::audio::common::AudioDevice>& devices)
            const noexcept;

    /*
     * @breif provides pal devices for given mixport and audiodevices.
     *
     * @param mixPortConfig mixportconfig for which devices are requested
     * @param tag usecase tag
     * @param setDevices vector of devices for which pal devices are requested
     * @param dummyDevice setDevices can be empty, in that case if client needs
     * dummy device in form of PAL_DEVICE_[IN/OUT]_DUMMY
     */

    std::vector<pal_device> configureAndFetchPalDevices(
            const ::aidl::android::media::audio::common::AudioPortConfig& mixPortConfig,
            const Usecase& tag,
            const std::vector<::aidl::android::media::audio::common::AudioDevice>& setDevices,
            const bool dummyDevice = false) const;

    std::vector<pal_device> getDummyPalDevices(
            const ::aidl::android::media::audio::common::AudioPortConfig& mixPortConfig) const;
    /*
    * @breif In order to get stream position in the DSP pipeline
    * 
    * @param, 
    * Input Parameters: 
    * palHandle, a valid stream pal handle
    * sampleRate, a valid stream sample rate
    * 
    * Output Parameters:
    * dspFrames, num of frames delivered by DSP
    */
    void getPositionInFrames(pal_stream_handle_t* palHandle, int32_t const& sampleRate,
                                   int64_t* const dspFrames) const;

    /*
    * @brief requiresBufferReformat is used to check if format converter is needed for
    * a PCM format or not, it is not applicable for compressed formats.
    * It is possible that framework can use a format which might not
    * be supported at below layers, so HAL needs to convert the buffer in desired format
    * before writing.
    *
    * @param portConfig : mixport config of the stream.
    * return return a pair of input and output audio_format_t in case a format converter
    * is needed, otherwise nullopt.
    * For example, mix port using audio format FLOAT is not supported, closest to FLOAT,
    * INT_32 can be used as target format. so, return a pair of
    * <AUDIO_FORMAT_PCM_FLOAT, AUDIO_FORMAT_PCM_32_BIT>
    * Caller can utilize this to create a converter based of provided input, output formats.
    */
    static std::optional<std::pair<audio_format_t, audio_format_t>> requiresBufferReformat(
            const ::aidl::android::media::audio::common::AudioPortConfig& portConfig);

    /*
    * @brief creates a pal payload for a pal volume and sets to PAL
    * @param handle : valid pal stream handle
    * @param volumes vector of volumes in floats
    * return 0 in success, error code otherwise
    */
    int setVolume(pal_stream_handle_t* handle, const std::vector<float>& volumes) const;

    std::unique_ptr<pal_buffer_config_t> getPalBufferConfig(const size_t bufferSize,
                                                            const size_t bufferCount) const;
    std::vector<::aidl::android::media::audio::common::AudioProfile> getDynamicProfiles(
            const ::aidl::android::media::audio::common::AudioPort& dynamicDeviceAudioPort) const;
    int handleDeviceConnectionChange(
            const ::aidl::android::media::audio::common::AudioPort& deviceAudioPort,
            const bool isConnect) const;
    uint32_t getBluetoothLatencyMs(
            const std::vector<::aidl::android::media::audio::common::AudioDevice>&
                    bluetoothDevices);
    std::unique_ptr<pal_stream_attributes> getDefaultTelephonyAttributes() const;
    std::unique_ptr<pal_stream_attributes> getDefaultCRSTelephonyAttributes() const;
    void configurePalDevicesCustomKey(std::vector<pal_device>& palDevices,
                                      const std::string& customKey) const;

    bool setStreamMicMute(pal_stream_handle_t* streamHandlePtr, const bool muted);
    bool getMicMuteStatus();
    void setMicMuteStatus(bool mute);
    bool updateScreenState(const bool isTurnedOn) noexcept;
    bool isScreenTurnedOn() const noexcept;

    bool isHDREnabled() const { return mHDREnabled; }
    void setHDREnabled(bool const& enable) { mHDREnabled = enable; }

    int32_t getHDRSampleRate() const { return mHDRSampleRate; }

    void setHDRSampleRate(int32_t const& sampleRate) { mHDRSampleRate = sampleRate; }

    int32_t getHDRChannelCount() const { return mHDRChannelCount; }

    void setHDRChannelCount(int32_t const& channelCount) { mHDRChannelCount = channelCount; }

    bool isWNREnabled() const { return mWNREnabled; }
    void setWNREnabled(bool const& enable) { mWNREnabled = enable; }

    bool isANREnabled() const { return mANREnabled; }
    void setANREnabled(bool const& enable) { mANREnabled = enable; }

    bool isInverted() const { return mInverted; }
    void setInverted(bool const& enable) { mInverted = enable; }

    std::string getOrientation() const { return mOrientation; }

    void setOrientation(std::string const& value) { mOrientation = value; }

    std::string getFacing() const { return mFacing; }

    void setFacing(std::string const& value) { mFacing = value; }

    void setTelephony(const std::weak_ptr<::aidl::android::hardware::audio::core::ITelephony> telephony) noexcept {
        mTelephony = telephony;
    }

    std::weak_ptr<::aidl::android::hardware::audio::core::ITelephony> getTelephony() const noexcept {
        return mTelephony;
    }

    /*
    * @brief creates a pal payload for a speed factor and sets to PAL
    * @param handle : pal stream handle
    * @param tag usecase tag
    * @param playbackRate  playback rate to be set
    * return PlaybackRateStatus::SUCCESS on success, or if stream handle is not set.
    * return PlaybackRateStatus::UNSUPPORTED operation, usecase does not support speed operations
    * or speed parameters are not in the range
    * return PlaybackRateStatus::ILLEGAL_ARGUMENT in case of any other failure
    */
    PlaybackRateStatus setPlaybackRate(
            pal_stream_handle_t* handle, const Usecase& tag,
            const ::aidl::android::media::audio::common::AudioPlaybackRate& playbackRate);

    void setInCallMusicState(const bool state) noexcept { mInCallMusicEnabled = state; }
    bool getInCallMusicState() noexcept { return mInCallMusicEnabled; }

    // Set and Get Value Functions for Translate Record.
    void setTranslationRecordState(const bool state) noexcept { mIsTranslationRecordEnabled = state; }
    bool getTranslationRecordState() noexcept { return mIsTranslationRecordEnabled; }

    // Set and Get Value Functions for Voice Call Volume mute during Translation Record Usecase.
    void setTranslationRxMuteState(const bool state) noexcept { mIsTranslationRxMuteEnabled = state; }
    bool getTranslationRxMuteState() noexcept { return mIsTranslationRxMuteEnabled; }

    void setHACEnabled(const bool& enable) noexcept { mIsHACEnabled = enable; }

    bool isHACEnabled() const noexcept { return mIsHACEnabled; }

    void updateCallState(int callState) { mCallState = callState; }
    void updateCallMode(int callMode) { mCallMode = callMode; }

    int getCallState() { return mCallState; }
    int getCallMode() { return mCallMode; }
    bool isA2dpSuspended();

    void setWFDProxyChannels(const uint32_t numProxyChannels) noexcept;
    void setProxyRecordFMQSize(const size_t& FMQSize) noexcept;
    size_t getProxyRecordFMQSize() const noexcept;
    uint32_t getWFDProxyChannels() const noexcept;
    /* Check if proxy record session is active in  PAL_DEVICE_IN_RECORD_PROXY */
    std::string IsProxyRecordActive() const noexcept;
    bool isIPAsProxyDeviceConnected() const noexcept { return mIsIPAsProxyConnected; };
    void setIPAsProxyDeviceConnected(bool isIPAsProxy) noexcept { mIsIPAsProxyConnected = isIPAsProxy; };

    void setHapticsVolume(const float hapticsVolume) const noexcept;
    void setHapticsIntensity(const int hapticsIntensity) const noexcept;

    void updateUHQA(const bool enable) noexcept;
    bool isUHQAEnabled() const noexcept;
    void setFTMSpeakerProtectionMode(uint32_t const heatUpTime, uint32_t const runTime,
                                     bool const isFactoryTest, bool const isValidationMode,
                                     bool const isDynamicCalibration) const noexcept;
    std::optional<std::string> getFTMResult() const noexcept;
    std::optional<std::string> getSpeakerCalibrationResult() const noexcept;
#if defined(AUDIO_FEATURE_ENABLED_MIC_OCCLUSION)
    std::optional<std::string> getMicocclusionparameter() const noexcept;
#endif
    void updateScreenRotation(const ::aidl::android::hardware::audio::core::IModule::ScreenRotation
                                      in_rotation) noexcept;
    ::aidl::android::hardware::audio::core::IModule::ScreenRotation getCurrentScreenRotation() const
            noexcept;

    bool platformSupportsOffloadSpeed() { return mOffloadSpeedSupported; }
    bool usecaseSupportsOffloadSpeed(const Usecase& tag) {
        return platformSupportsOffloadSpeed() && isOffload(tag);
    }

    bool isOffload(const Usecase& tag) { return tag == Usecase::COMPRESS_OFFLOAD_PLAYBACK; }
    int setLatencyMode(uint32_t mode, pal_device_id_t dev_id);
    int getRecommendedLatencyModes(
          std::vector<::aidl::android::media::audio::common::AudioLatencyMode>* _aidl_return,
          pal_device_id_t dev_id);

    void configurePalDevices(
            const ::aidl::android::media::audio::common::AudioPortConfig& mixPortConfig,
            std::vector<pal_device>& palDevices);
    void setHdrOnPalDevice(pal_device* palDeviceIn);
    bool isHDRARMenabled();
    bool isHDRSPFEnabled();
    bool getUSBCapEnable() { return mUSBCapEnable; }
    void updateHotwordPortConfig(
        ::aidl::android::media::audio::common::AudioPortConfig& portConfig);
  private:
    void customizePalDevices(
            const ::aidl::android::media::audio::common::AudioPortConfig& mixPortConfig,
            const Usecase& tag, std::vector<pal_device>& palDevices) const noexcept;
    void configurePalDevicesForHIFIPCMFilter(std::vector<pal_device>&) const noexcept;
    std::vector<::aidl::android::media::audio::common::AudioProfile> getUsbProfiles(
            const ::aidl::android::media::audio::common::AudioPort& port) const;

    std::optional<struct HdmiParameters> getHdmiParameters(
            const ::aidl::android::media::audio::common::AudioDevice&) const;

    void initUsecaseOpMap();

  public:
    constexpr static uint32_t kDefaultOutputSampleRate = 48000;
    constexpr static uint32_t kDefaultPCMBidWidth = 16;
    constexpr static pal_audio_fmt_t kDefaultPalPCMFormat = PAL_AUDIO_FMT_PCM_S16_LE;
    constexpr static int32_t kDefaultLatencyMs = 51;

  private:
    std::vector<::aidl::android::media::audio::common::AudioDevice> mPrimaryPlaybackDevices{};

    std::map<std::string, std::string> mParameters;
    card_status_t mSndCardStatus{CARD_STATUS_OFFLINE};
    bool mInCallMusicEnabled{false};
    bool mIsTranslationRecordEnabled{false};
    bool mIsTranslationRxMuteEnabled{false};
    bool mIsScreenTurnedOn{false};
    uint32_t mWFDProxyChannels{0};
    bool mIsUHQAEnabled{false};
    bool mIsIPAsProxyConnected{false};
    ::aidl::android::hardware::audio::core::IModule::ScreenRotation mCurrentScreenRotation{
            ::aidl::android::hardware::audio::core::IModule::ScreenRotation::DEG_0};
    bool mOffloadSpeedSupported = false;
    bool mMicMuted = false;

    /* HDR */
    bool mHDREnabled{false};
    int32_t mHDRSampleRate{0};
    int32_t mHDRChannelCount{0};
    bool mWNREnabled{false};
    bool mANREnabled{false};
    bool mInverted{false};
    std::string mOrientation{""};
    std::string mFacing{""};

    /* HAC enablement*/
    bool mIsHACEnabled{false};

    std::unordered_map<Usecase, UsecaseOps> mUsecaseOpMap;
    std::vector<::aidl::android::media::audio::common::MicrophoneInfo> mMicrophoneInfo;
    using PalDevToMicDynamicInfoMap = std::unordered_map<
            pal_device_id_t,
            std::vector<::aidl::android::media::audio::common::MicrophoneDynamicInfo>>;
    PalDevToMicDynamicInfoMap mMicrophoneDynamicInfoMap;
    // proxy related info
    size_t mProxyRecordFMQSize{0};
    std::weak_ptr<::aidl::android::hardware::audio::core::ITelephony> mTelephony;
};
} // namespace qti::audio::core
