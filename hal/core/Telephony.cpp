/*
 * Copyright (C) 2022 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Changes from Qualcomm Technologies, Inc. are provided under the following license:
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#define LOG_TAG "AHAL_Telephony_QTI"
#include <android-base/logging.h>
#include <android/binder_to_string.h>
#include <hardware/audio.h>
#include <qti-audio-core/StreamInPrimary.h>
#include <qti-audio-core/StreamOutPrimary.h>
#include <qti-audio-core/Telephony.h>
#include <qti-audio-core/Utils.h>
#include <system/audio.h>

using aidl::android::media::audio::common::AudioDevice;
using aidl::android::media::audio::common::AudioDeviceAddress;
using aidl::android::media::audio::common::AudioDeviceDescription;
using aidl::android::media::audio::common::AudioDeviceType;
using aidl::android::media::audio::common::AudioMode;
using aidl::android::media::audio::common::Boolean;
using aidl::android::media::audio::common::Float;

namespace qti::audio::core {

const AudioDevice Telephony::kDefaultRxDevice =
        AudioDevice{.type.type = AudioDeviceType::OUT_SPEAKER_EARPIECE};
const AudioDevice Telephony::kDefaultCRSRxDevice =
        AudioDevice{.type.type = AudioDeviceType::OUT_SPEAKER};

Telephony::Telephony() {
    mVoiceSession.session[VSID1_VOICE_SESSION].CallUpdate.mVSID = VSID::VSID_1;
    mVoiceSession.session[VSID1_VOICE_SESSION].state.current_ = CallState::IN_ACTIVE;
    mVoiceSession.session[VSID1_VOICE_SESSION].state.new_ = CallState::IN_ACTIVE;
    mVoiceSession.session[VSID2_VOICE_SESSION].CallUpdate.mVSID = VSID::VSID_2;
    mVoiceSession.session[VSID2_VOICE_SESSION].state.current_ = CallState::IN_ACTIVE;
    mVoiceSession.session[VSID2_VOICE_SESSION].state.new_ = CallState::IN_ACTIVE;
    mTelecomConfig.voiceVolume = Float{TelecomConfig::VOICE_VOLUME_MAX};
    mTelecomConfig.ttyMode = TelecomConfig::TtyMode::OFF;
    mTelecomConfig.isHacEnabled = Boolean{false};
    // Todo check on default RX device
    mRxDevice = kDefaultRxDevice;
    mTxDevice = getMatchingTxDevice(mRxDevice);
}

Telephony::~Telephony() {
    stopCall();
}

ndk::ScopedAStatus Telephony::getSupportedAudioModes(std::vector<AudioMode>* _aidl_return) {
    std::scoped_lock lock{mLock};

    *_aidl_return = mSupportedAudioModes;
    LOG(DEBUG) << __func__ << ": returning " << ::android::internal::ToString(*_aidl_return);
    return ndk::ScopedAStatus::ok();
}

void Telephony::VoiceStop() {
    for (int i = 0; i < MAX_VOICE_SESSIONS; i++) {
         mVoiceSession.session[i].CallUpdate.mCallState = CallState::IN_ACTIVE;
         mVoiceSession.session[i].state.new_ = CallState::IN_ACTIVE;
    }
    mIsVoiceStarted = false;
    updateCalls();

    LOG(DEBUG) << __func__ << ": Exit";
}

ndk::ScopedAStatus Telephony::switchAudioMode(AudioMode newAudioMode) {
    std::scoped_lock lock{mLock};
    auto status = ndk::ScopedAStatus::ok();

    if (std::find(mSupportedAudioModes.begin(), mSupportedAudioModes.end(), newAudioMode) ==
        mSupportedAudioModes.end()) {
        LOG(ERROR) << __func__ << ": illegal mode " << toString(newAudioMode);
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }

    mPlatform.updateCallMode((int)newAudioMode);

    if (mAudioMode == newAudioMode) {
        LOG(VERBOSE) << __func__ << ": no change" << toString(newAudioMode);
        return ndk::ScopedAStatus::ok();
    }
    if (newAudioMode == AudioMode::NORMAL) {
        // safe to stop now
        VoiceStop();
    } else if (newAudioMode == AudioMode::RINGTONE && mSetUpdates.mIsCrsCall) {
        if (!mIsCRSStarted && !isAnyCallActive()) {
            getPlaybackStreamDevices();
            updateCrsDevice();
            auto palDevices = mPlatform.convertToPalDevices({mRxDevice, mTxDevice});
            if ((palDevices[0].id == PAL_DEVICE_OUT_BLUETOOTH_BLE) &&
                (palDevices[1].id == PAL_DEVICE_IN_BLUETOOTH_BLE)) {
                updateVoiceMetadataForBT(true);
            }
            status = startCall();
            if (!status.isOk()) {
                LOG(ERROR) << __func__ << ": start crs call failed";
            } else {
                if (mRxDevice.type.type != AudioDeviceType::OUT_SPEAKER) {
                   startCrsLoopback();
                }
                mIsCRSStarted = true;
                mCRSVSID = mSetUpdates.mVSID;
                LOG(DEBUG) << __func__ << " start CRS call";
            }
        }
    }

    mAudioMode = newAudioMode;
    LOG(DEBUG) << __func__ << ": switching to AudioMode:" << toString(mAudioMode);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Telephony::setTelecomConfig(const TelecomConfig& in_config,
                                               TelecomConfig* _aidl_return) {
    std::unique_lock lock{mLock};

    if (in_config.voiceVolume.has_value() &&
        (in_config.voiceVolume.value().value <
                 static_cast<float>(TelecomConfig::VOICE_VOLUME_MIN) ||
         in_config.voiceVolume.value().value >
                 static_cast<float>(TelecomConfig::VOICE_VOLUME_MAX))) {
        LOG(ERROR) << __func__
                   << ": voice volume value is invalid: " << in_config.voiceVolume.value().value;
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    if (in_config.voiceVolume.has_value()) {
        mTelecomConfig.voiceVolume = in_config.voiceVolume;
        // safe to update when there is volume provided
        updateVoiceVolume();
    }
    if (in_config.ttyMode != TelecomConfig::TtyMode::UNSPECIFIED) {
        mTelecomConfig.ttyMode = in_config.ttyMode;
        // safe to update when there is ttymode is provided
        updateTtyMode();
        if (mTelecomConfig.ttyMode == TelecomConfig::TtyMode::HCO ||
            mTelecomConfig.ttyMode == TelecomConfig::TtyMode::VCO) {
            updateDevices();
        }
    }
    if (in_config.isHacEnabled.has_value()) {
        mTelecomConfig.isHacEnabled = in_config.isHacEnabled;
        mPlatform.setHACEnabled(mTelecomConfig.isHacEnabled.value().value);
        /**
         * TODO remove this unusual way with streams
         * remove the telephony lock before handling the streams.
         * unlocking the telephony is necessary because the stream already have telephony instance.
         */

        lock.unlock();
        triggerHACinVoipPlayback();
        lock.lock();
    }
    *_aidl_return = mTelecomConfig;
    LOG(DEBUG) << __func__ << ": received " << in_config.toString() << ", returning "
               << _aidl_return->toString();
    return ndk::ScopedAStatus::ok();
}

void Telephony::setMicMute(const bool muted) {
    std::scoped_lock lock{mLock};
    if (mPalHandle == nullptr) {
        return;
    }
    if (!mPlatform.setStreamMicMute(mPalHandle, muted)) {
        LOG(ERROR) << __func__ << ": failed";
    }
}

bool Telephony::isCrsCallSupported() {
    std::scoped_lock lock{mLock};
    if (property_get_bool("vendor.audio.crs_call_disabled", false)) {
        LOG(INFO) << __func__ << " CRS calls disabled";
        return false;
    }
    return true;
}

bool Telephony::isAnyCallActive() {
    for (int i = 0; i < MAX_VOICE_SESSIONS; i++) {
         if (mVoiceSession.session[i].state.current_ == CallState::ACTIVE) {
             return true;
         }
    }
    return false;
}

bool Telephony::isValidDevice(const AudioDevice& rxDevice) {
    if (getMatchingTxDevice(rxDevice).type.type == AudioDeviceType::NONE) {
        return false;
    }
    return true;
}

bool Telephony::isUsbDeviceConnected(const AudioDevice& usbDevice) {
    const auto& deviceAddress = usbDevice.address;
    if (deviceAddress.getTag() != AudioDeviceAddress::Tag::alsa) {
        LOG(ERROR) << __func__ << " no alsa address provided for the AudioPort "
                   << usbDevice.toString();
        return false;
    }

    const auto& deviceAddressAlsa = deviceAddress.get<AudioDeviceAddress::Tag::alsa>();
    if (!isValidAlsaAddr(deviceAddressAlsa)) {
        LOG(ERROR) << __func__ << " failed to find alsa address for given usb device ";
        return false;
    }
    const auto cardId = deviceAddressAlsa[0];
    const auto deviceId = deviceAddressAlsa[1];
    auto deviceCapability = std::make_unique<pal_param_device_capability_t>();
    if (!deviceCapability) {
        LOG(ERROR) << __func__ << ": allocation failed ";
        return false;
     }
    auto dynamicMediaConfig = std::make_unique<dynamic_media_config_t>();
    if (!dynamicMediaConfig) {
         LOG(ERROR) << __func__ << ": allocation failed ";
         return false;
    }
    size_t payloadSize = 0;
    deviceCapability->addr.card_id = cardId;
    deviceCapability->addr.device_num = deviceId;
    deviceCapability->config = dynamicMediaConfig.get();
    if (isOutputDevice(usbDevice)) {
        deviceCapability->id = PAL_DEVICE_OUT_USB_HEADSET;
        deviceCapability->is_playback = true;
    } else {
        deviceCapability->id = PAL_DEVICE_IN_USB_HEADSET;
        deviceCapability->is_playback = false;
    }

    void* deviceCapabilityPtr = deviceCapability.get();
    if (int32_t ret = pal_get_param(PAL_PARAM_ID_DEVICE_CAPABILITY, &deviceCapabilityPtr,
                                    &payloadSize, nullptr);
        ret != 0) {
        LOG(ERROR) << __func__ << " PAL get param failed for PAL_PARAM_ID_DEVICE_CAPABILITY" << ret;
        return false;
    }
    return true;
}

void Telephony::resetDevices(const bool resetRx) {
    std::scoped_lock lock{mLock};
    LOG(VERBOSE) <<__func__<<": ignore reset device ";
}

void Telephony::setDevices(const std::vector<AudioDevice>& devices, const bool updateRx) {
    std::scoped_lock lock{mLock};

    if (devices.size() != 1) {
        LOG(ERROR) << __func__ << " invalid size / combo devices unsupported: " << devices;
        return;
    }

    LOG(DEBUG) << __func__ << (updateRx ? " Rx " : " Tx") << " devices : " << devices;
    if (updateRx) {
        mRxDevice = devices[0]; // expected to have 1 device.
        mTxDevice = getMatchingTxDevice(mRxDevice);
        updateDevices();
    } else {
        /* USB TX capability is ready may later then USB RX devices. Here is to update
         * TX device if voice call already start on USB RX devices.
         */
        if (isAnyCallActive() &&
            (mTxDevice.type.type != devices[0].type.type)) {
            if (isUsbDevice(devices[0]) && isUsbDevice(mRxDevice)) {
                if (!isUsbDeviceConnected(devices[0])) {
                    LOG(DEBUG) << __func__ << ": usb_tx is not connected ";
                    return;
                }
            }
            mTxDevice = devices[0];
            updateDevices();
       }
    }
}

void Telephony::updateTTYPalDevices(std::vector<pal_device>& palDevices) {
    if (!palDevices.size()) {
        LOG(ERROR) << __func__ << " no connected devices";
         return;
    }

    if(mTelecomConfig.ttyMode == TelecomConfig::TtyMode::HCO) {
      /**  device pairs for HCO usecase
        *  <handset, headset-mic>
        *  <speaker, headset-mic>
        *  override devices accordingly.
        */
       if (palDevices[0].id == PAL_DEVICE_OUT_WIRED_HEADSET) {
             palDevices[0].id = PAL_DEVICE_OUT_HANDSET;
       } else if ( palDevices[0].id== PAL_DEVICE_OUT_SPEAKER) {
            palDevices[1].id = PAL_DEVICE_IN_WIRED_HEADSET;
       } else {
           LOG(DEBUG) << __func__ << ": invalid device pair for TTY HCO usecase";
       }
    } else if (mTelecomConfig.ttyMode == TelecomConfig::TtyMode::VCO) {
        /**  device pairs for VCO usecase
          *  <headphones, handset-mic>
          *  <headphones, speaker-mic>
          *  override devices accordingly.
          */
       if (palDevices[0].id == PAL_DEVICE_OUT_WIRED_HEADSET ||
           palDevices[0].id == PAL_DEVICE_OUT_WIRED_HEADPHONE) {
           palDevices[1].id = PAL_DEVICE_IN_HANDSET_MIC;
       } else if (palDevices[0].id == PAL_DEVICE_OUT_SPEAKER) {
           palDevices[0].id = PAL_DEVICE_OUT_WIRED_HEADSET;
       } else {
           LOG(DEBUG) << __func__ << ": invalid device pair for TTY VCO usecase";
       }
    }
}

void Telephony::updateVoiceMetadataForBT(bool call_active) {
    ssize_t track_count = 1;
    std::vector<playback_track_metadata_t> sourceTracks;
    std::vector<record_track_metadata_t> sinkTracks;
    sourceTracks.resize(track_count);
    sinkTracks.resize(track_count);
    int32_t ret = 0;

    source_metadata_t btSourceMetadata;
    sink_metadata_t btSinkMetadata;

    if (call_active) {
        btSourceMetadata.track_count = track_count;
        btSourceMetadata.tracks = sourceTracks.data();

        btSourceMetadata.tracks->usage = AUDIO_USAGE_VOICE_COMMUNICATION;
        btSourceMetadata.tracks->content_type = AUDIO_CONTENT_TYPE_SPEECH;

        LOG(DEBUG) << __func__
                   << "Source metadata for voice call usage: " << btSourceMetadata.tracks->usage
                   << "content_type: " << btSourceMetadata.tracks->content_type;
        // Pass the source metadata to PAL
        pal_set_param(PAL_PARAM_ID_SET_SOURCE_METADATA, (void*)&btSourceMetadata, 0);

        btSinkMetadata.track_count = track_count;
        btSinkMetadata.tracks = sinkTracks.data();

        btSinkMetadata.tracks->source = AUDIO_SOURCE_VOICE_CALL;

        LOG(DEBUG) << __func__
                   << "Sink metadata for voice call source: " << btSinkMetadata.tracks->source;
        // Pass the sink metadata to PAL
        pal_set_param(PAL_PARAM_ID_SET_SINK_METADATA, (void*)&btSinkMetadata, 0);
    } else {
        /* When voice call ends, we need to restore metadata configuration for
         * source and sink sessions same as prior to the call. Send source
         * and sink metadata separately to BT.
         */
        if (mStreamOutPrimary.lock()) {
            StreamOutPrimary::sourceMetadata_mutex_.lock();
            ret = mStreamOutPrimary.lock()->setAggregateSourceMetadata(false);
            if (ret != 0) {
                LOG(ERROR) << __func__ << " Set PAL_PARAM_ID_SET_SOURCE_METADATA for" << ret
                           << " failed";
            }
            StreamOutPrimary::sourceMetadata_mutex_.unlock();
        }

        if (mStreamInPrimary.lock()) {
            StreamInPrimary::sinkMetadata_mutex_.lock();
            ret = mStreamInPrimary.lock()->setAggregateSinkMetadata(false);
            if (ret != 0) {
                LOG(ERROR) << __func__ << " Set PAL_PARAM_ID_SET_SINK_METADATA for" << ret
                           << " failed";
            }
            StreamInPrimary::sinkMetadata_mutex_.unlock();
        }
    }
}

void Telephony::onExternalDeviceConnectionChanged(const AudioDevice& extDevice,
                                                  const bool& connect) {
    std::scoped_lock lock{mLock};
    // Placeholder for telephony to act upon external device connection
    if(connect){
        mExternalDevices.push_back(extDevice);
    } else {
        std::erase(mExternalDevices, extDevice);
    }
    if (isBluetoothSCODevice(extDevice) || isBluetoothA2dpDevice(extDevice)) {
        LOG(VERBOSE) << __func__ << ": sco/a2dp no change";
        return;
    }

    if (connect) {
        if (isOutputDevice(extDevice)) {
            CRSPluginDevices.push_back(extDevice);
            if (mIsCRSStarted) {
               updateDevices();
            }
        } else {
            if (mTxDevice.type.type != extDevice.type.type) {
                mTxDevice = getMatchingTxDevice(mRxDevice);
                if (mIsCRSStarted) {
                    updateDevices();
                }
            }
        }
    } else {
        //remove disconnect devices
        for (auto iter = CRSPluginDevices.begin(); iter != CRSPluginDevices.end();) {
             if ((*iter).type.type == extDevice.type.type ) {
                 iter = CRSPluginDevices.erase(iter);
                 continue;
             }
             iter++;
        }
        if (CRSPluginDevices.empty()) {
            mRxDevice = kDefaultRxDevice;
            mTxDevice = getMatchingTxDevice(mRxDevice);
            if (mIsCRSStarted)
                updateDevices();
        } else {
            if (mIsCRSStarted)
                updateDevices();
        }
    }
}

void Telephony::onPlaybackStreamDevices(const std::vector<AudioDevice>& playbackStreamDevices) {
    std::scoped_lock lock{mLock};

    /**
     * CRS ringtone routing piggybacks on output primary stream devices
     **/
    if (isAnyCallActive() || mAudioMode == AudioMode::IN_CALL) {
        LOG(VERBOSE) << __func__ << ": voice call exist";
        return;
    }
    if (hasValidPlaybackStream &&
        playbackStreamDevices.size() == 1 &&
        isValidDevice(playbackStreamDevices[0])) {// combo devices unsupported.
        mPlaybackStreamDevices = playbackStreamDevices;
        mRxDevice = playbackStreamDevices[0]; // expected to have 1 device.
        mTxDevice = getMatchingTxDevice(mRxDevice);
        updateDevices();
     }
}

void Telephony::onBluetoothScoEvent(const bool& enable) {
    std::scoped_lock lock{mLock};

    if (isAnyCallActive() || mAudioMode == AudioMode::IN_CALL) {
        LOG(VERBOSE) << __func__ << ": voice call exist";
        return;
    }

   if (enable) {
       mRxDevice = AudioDevice{.type.type = AudioDeviceType::OUT_DEVICE,
                               .type.connection = AudioDeviceDescription::CONNECTION_BT_SCO};
       mTxDevice = getMatchingTxDevice(mRxDevice);
       CRSPluginDevices.push_back(mRxDevice);
       if (mIsCRSStarted) {
           updateDevices();
       }
   } else {
       if (isBluetoothSCODevice(mRxDevice) || isBluetoothA2dpDevice(mRxDevice)) {
          for (auto iter = CRSPluginDevices.begin(); iter != CRSPluginDevices.end();) {
               if ((*iter).type.connection == AudioDeviceDescription::CONNECTION_BT_SCO) {
                   iter = CRSPluginDevices.erase(iter);
                   continue;
               }
               iter++;
          }
          if (CRSPluginDevices.empty()) {
              mRxDevice = kDefaultRxDevice;
              mTxDevice = getMatchingTxDevice(mRxDevice);
              if (mIsCRSStarted)
                  updateDevices();
          } else {
              if (mIsCRSStarted)
                  updateDevices();
          }
     }
  }
}

void Telephony::updateCrsDevice() {
    LOG(VERBOSE) << __func__ << ": Enter";
    if (hasValidPlaybackStream) {
        return;
    }

    if (CRSPluginDevices.empty()) {
        if (mRxDevice.type.type == AudioDeviceType::OUT_SPEAKER_EARPIECE) {
            mRxDevice = kDefaultCRSRxDevice;
            mTxDevice = getMatchingTxDevice(mRxDevice);
        }
    } else {
            mRxDevice = CRSPluginDevices.back();
            mTxDevice = getMatchingTxDevice(mRxDevice);
    }
}



AudioDevice Telephony::getMatchingTxDevice(const AudioDevice& rxDevice) {
    auto getComplementDeviceIfAny =
            [&](const AudioDeviceDescription& desc) -> std::optional<AudioDevice> {
        auto itr =
                std::find_if(mExternalDevices.begin(), mExternalDevices.end(), [&](const auto& d) {
                    if (d.type.connection == desc.connection && d.type.type == desc.type) {
                        return true;
                    }
                    return false;
                });
        if (itr != mExternalDevices.end()) {
            return *itr;
        }
        return std::nullopt;
    };

    if (rxDevice.type.type == AudioDeviceType::OUT_SPEAKER_EARPIECE) {
        return AudioDevice{.type.type = AudioDeviceType::IN_MICROPHONE};
    } else if (rxDevice.type.type == AudioDeviceType::OUT_SPEAKER) {
        return AudioDevice{.type.type = AudioDeviceType::IN_MICROPHONE_BACK};
    } else if (rxDevice.type.type == AudioDeviceType::OUT_HEADSET &&
               rxDevice.type.connection == AudioDeviceDescription::CONNECTION_ANALOG) {
        return AudioDevice{.type.type = AudioDeviceType::IN_HEADSET,
                           .type.connection = AudioDeviceDescription::CONNECTION_ANALOG,
                           .address = rxDevice.address};
    } else if (rxDevice.type.type == AudioDeviceType::OUT_HEADPHONE &&
               rxDevice.type.connection == AudioDeviceDescription::CONNECTION_ANALOG) {
        return AudioDevice{.type.type = AudioDeviceType::IN_MICROPHONE};
    } else if (rxDevice.type.type == AudioDeviceType::OUT_DEVICE &&
               rxDevice.type.connection == AudioDeviceDescription::CONNECTION_ANALOG) {
        return AudioDevice{.type.type = AudioDeviceType::IN_MICROPHONE};
    } else if (rxDevice.type.type == AudioDeviceType::OUT_DEVICE &&
               rxDevice.type.connection == AudioDeviceDescription::CONNECTION_BT_SCO) {
        auto found = getComplementDeviceIfAny(
                AudioDeviceDescription{.type = AudioDeviceType::IN_DEVICE,
                                       .connection = AudioDeviceDescription::CONNECTION_BT_SCO});
        if (found) {
            return found.value();
        }
        return AudioDevice{.type.type = AudioDeviceType::IN_MICROPHONE};
    } else if (rxDevice.type.type == AudioDeviceType::OUT_HEADSET &&
               rxDevice.type.connection == AudioDeviceDescription::CONNECTION_BT_SCO) {
        auto found = getComplementDeviceIfAny(
                AudioDeviceDescription{.type = AudioDeviceType::IN_HEADSET,
                                       .connection = AudioDeviceDescription::CONNECTION_BT_SCO});
        if (found) {
            return found.value();
        }
        return AudioDevice{.type.type = AudioDeviceType::IN_MICROPHONE};
    } else if (rxDevice.type.type == AudioDeviceType::OUT_HEADSET &&
               rxDevice.type.connection == AudioDeviceDescription::CONNECTION_BT_LE) {
        auto found = getComplementDeviceIfAny(
                AudioDeviceDescription{.type = AudioDeviceType::IN_HEADSET,
                                       .connection = AudioDeviceDescription::CONNECTION_BT_LE});
        if (found) {
            return found.value();
        }
        return AudioDevice{.type.type = AudioDeviceType::IN_MICROPHONE};
    } else if (rxDevice.type.type == AudioDeviceType::OUT_CARKIT &&
               rxDevice.type.connection == AudioDeviceDescription::CONNECTION_BT_SCO) {
        return AudioDevice{.type.type = AudioDeviceType::IN_HEADSET,
                           .type.connection = AudioDeviceDescription::CONNECTION_BT_SCO};
    } else if ((rxDevice.type.type == AudioDeviceType::OUT_DEVICE ||
                rxDevice.type.type == AudioDeviceType::OUT_HEADSET) &&
               rxDevice.type.connection == AudioDeviceDescription::CONNECTION_USB) {
        if (isUsbDeviceConnected(rxDevice)) {
            if (mPlatform.getUSBCapEnable()) {
                return AudioDevice{.type.type = AudioDeviceType::IN_HEADSET,
                                   .type.connection = AudioDeviceDescription::CONNECTION_USB,
                                   .address = rxDevice.address};
            } else {
                return AudioDevice{.type.type = AudioDeviceType::IN_MICROPHONE};
            }
        } else {
            LOG(DEBUG) << __func__ << ": usb device is not connected ";
            mRxDevice = kDefaultRxDevice;
            return AudioDevice{.type.type = AudioDeviceType::IN_MICROPHONE};
        }
    } else if (rxDevice.type.type == AudioDeviceType::OUT_HEARING_AID) {
        return AudioDevice{.type.type = AudioDeviceType::IN_MICROPHONE};
    } else {
        LOG(ERROR) << __func__ << ": unable to find matching TX device for " << rxDevice.toString();
    }
    return {};
}

void Telephony::reconfigure(const SetUpdates& newUpdates) {
    std::scoped_lock lock{mLock};
    auto status = ndk::ScopedAStatus::ok();
    auto palDevices = mPlatform.convertToPalDevices({mRxDevice, mTxDevice});
    LOG(DEBUG) << __func__ << " : Enter : current setUpdates" << mSetUpdates.toString() << " new setUpdates"
                 << newUpdates.toString();
    // Todo Implement
    mPlatform.updateCallState((int)mSetUpdates.mCallState);

    if (newUpdates.mIsCrsCall) {
        mSetUpdates.mIsCrsCall = newUpdates.mIsCrsCall;
        mSetUpdates.mVSID = newUpdates.mVSID;
        if (!mIsCRSStarted && !isAnyCallActive() &&
            mAudioMode == AudioMode::RINGTONE) {
             getPlaybackStreamDevices();
             updateCrsDevice();
             if ((palDevices[0].id == PAL_DEVICE_OUT_BLUETOOTH_BLE) &&
                 (palDevices[1].id == PAL_DEVICE_IN_BLUETOOTH_BLE)) {
                 updateVoiceMetadataForBT(true);
             }
             status = startCall();
             if (!status.isOk()) {
                 LOG(ERROR) << __func__ << ": start crs call failed";
             } else {
                 if (mRxDevice.type.type != AudioDeviceType::OUT_SPEAKER) {
                     startCrsLoopback();
                 }
                 mIsCRSStarted  = true;
                 mCRSVSID = newUpdates.mVSID;
                 LOG(DEBUG) << __func__ << ": start CRS call";
             }
             return;
         }
    } else {
         if (mIsCRSStarted && mCRSVSID == newUpdates.mVSID) {
             status = stopCall();
             if (!status.isOk()) {
                 LOG(ERROR) << __func__ << ": stop crs call failed";
             } else {
                 if (mPalCrsHandle != nullptr) {
                     stopCrsLoopback();
                 }
                 mSetUpdates.mIsCrsCall = newUpdates.mIsCrsCall;
                 mIsCRSStarted  = false;
                 LOG(DEBUG) << __func__ << ": stop CRS call";
             }
         }
    }

    for (int i = 0; i < MAX_VOICE_SESSIONS; i++) {
         if (newUpdates.mVSID == mVoiceSession.session[i].CallUpdate.mVSID) {
             mVoiceSession.session[i].state.new_  = newUpdates.mCallState;
             mVoiceSession.session[i].CallUpdate = newUpdates;
             break;
         }
    }
    if (isAnyCallActive() ||
       ((mAudioMode == AudioMode::IN_CALL) && mIsVoiceStarted)) {
       updateCalls();
    }

    LOG(DEBUG) << __func__ << ": Exit";
}

void Telephony::updateCalls() {
     auto status = ndk::ScopedAStatus::ok();
     auto palDevices = mPlatform.convertToPalDevices({mRxDevice, mTxDevice});
     for (int i = 0; i < MAX_VOICE_SESSIONS; i++) {
            switch (mVoiceSession.session[i].state.new_) {
                  case CallState::ACTIVE:
                      switch (mVoiceSession.session[i].state.current_) {
                            case CallState::IN_ACTIVE:
                                LOG(DEBUG) << __func__ << " CallState: INACTIVE -> ACTIVE vsid:" << mVoiceSession.session[i].CallUpdate.mVSID;
                                if ((palDevices[0].id == PAL_DEVICE_OUT_BLUETOOTH_BLE) &&
                                    (palDevices[1].id == PAL_DEVICE_IN_BLUETOOTH_BLE)) {
                                    updateVoiceMetadataForBT(true);
                                }
                                if (!isAnyCallActive() && !mIsCRSStarted) {
                                    mSetUpdates =  mVoiceSession.session[i].CallUpdate;
                                    status = startCall();
                                    if (!status.isOk()) {
                                        LOG(ERROR) << __func__ << ": start call failed";
                                    } else {
                                        mIsVoiceStarted = true;
                                        mVoiceSession.session[i].state.current_ = mVoiceSession.session[i].state.new_;
                                    }
                                } else {
                                    LOG(DEBUG) << __func__ << ": voice already started";
                                }
                                break;

                            default:
                                LOG(INFO) << __func__ << " CallState: ACTIVE cannot be handled in "
                                          << "state " << mVoiceSession.session[i].state.current_
                                          << " vsid " << mVoiceSession.session[i].CallUpdate.mVSID;
                                break;
                      }
                      break;

                  case CallState::IN_ACTIVE:
                      switch (mVoiceSession.session[i].state.current_) {
                            case CallState::ACTIVE:
                                LOG(DEBUG) << __func__ << " CallState: ACTIVE -> INACTIVE vsid:" << mVoiceSession.session[i].CallUpdate.mVSID;
                                mSetUpdates =  mVoiceSession.session[i].CallUpdate;
                                status = stopCall();
                                if (!status.isOk()) {
                                    LOG(ERROR) << __func__ << ": stop call failed";
                                } else {
                                    mVoiceSession.session[i].state.current_ = mVoiceSession.session[i].state.new_;
                                }
                                break;

                             default:
                                 LOG(INFO) << __func__ << " CallState: Default cannot be handled in "
                                           << "state " << mVoiceSession.session[i].state.current_
                                           << " vsid " << mVoiceSession.session[i].CallUpdate.mVSID;
                                break;
                      }
                      break;
                  default:
                      break;
            }
     }
}

void Telephony::updateVolumeBoost(const bool enable) {
    std::scoped_lock lock{mLock};
    mIsVolumeBoostEnabled = enable;
    LOG(INFO) << __func__ << ": is enabled: " << mIsVolumeBoostEnabled;
    configureVolumeBoost();
}

void Telephony::updateSlowTalk(const bool enable) {
    std::scoped_lock lock{mLock};
    mIsSlowTalkEnabled = enable;
    LOG(INFO) << __func__ << ": is enabled: " << mIsSlowTalkEnabled;
    configureSlowTalk();
}

void Telephony::updateHDVoice(const bool enable) {
    std::scoped_lock lock{mLock};
    mIsHDVoiceEnabled = enable;
    LOG(INFO) << __func__ << ": is enabled: " << mIsHDVoiceEnabled;
    configureHDVoice();
}

void Telephony::updateDeviceMute(const bool isMute, const std::string& muteDirection) {
    std::scoped_lock lock{mLock};
    mIsDeviceMuted = isMute;
    mMuteDirection = muteDirection;
    LOG(INFO) << __func__ << ": is muted: " << mIsDeviceMuted
              << ", mute direction: " << mMuteDirection;
    configureDeviceMute();
}

void Telephony::configureVolumeBoost() {
    if (mPalHandle == nullptr) {
        LOG(ERROR) << __func__ << ": invalid pal handle";
        return;
    }
    auto byteSize = sizeof(pal_param_payload) + sizeof(bool);
    auto bytes = std::make_unique<uint8_t[]>(byteSize);
    auto palParamPayload = reinterpret_cast<pal_param_payload*>(bytes.get());
    palParamPayload->payload_size = sizeof(bool);
    palParamPayload->payload[0] = mIsVolumeBoostEnabled;
    if (int32_t ret =
                ::pal_stream_set_param(mPalHandle, PAL_PARAM_ID_VOLUME_BOOST, palParamPayload);
        ret) {
        LOG(ERROR) << __func__ << ": failed to set PAL_PARAM_ID_VOLUME_BOOST";
        return;
    }
}

void Telephony::configureSlowTalk() {
    if (mPalHandle == nullptr) {
        LOG(ERROR) << __func__ << ": invalid pal handle";
        return;
    }
    auto byteSize = sizeof(pal_param_payload) + sizeof(bool);
    auto bytes = std::make_unique<uint8_t[]>(byteSize);
    auto palParamPayload = reinterpret_cast<pal_param_payload*>(bytes.get());
    palParamPayload->payload_size = sizeof(bool);
    palParamPayload->payload[0] = mIsSlowTalkEnabled;
    if (int32_t ret = ::pal_stream_set_param(mPalHandle, PAL_PARAM_ID_SLOW_TALK, palParamPayload);
        ret) {
        LOG(ERROR) << __func__ << ": failed to set PAL_PARAM_ID_SLOW_TALK";
        return;
    }
}

void Telephony::configureHDVoice() {
    if (mPalHandle == nullptr) {
        LOG(ERROR) << __func__ << ": invalid pal handle";
        return;
    }
    auto byteSize = sizeof(pal_param_payload) + sizeof(bool);
    auto bytes = std::make_unique<uint8_t[]>(byteSize);
    auto palParamPayload = reinterpret_cast<pal_param_payload*>(bytes.get());
    palParamPayload->payload_size = sizeof(bool);
    palParamPayload->payload[0] = mIsHDVoiceEnabled;
    if (int32_t ret = ::pal_stream_set_param(mPalHandle, PAL_PARAM_ID_HD_VOICE, palParamPayload);
        ret) {
        LOG(ERROR) << __func__ << ": failed to set PAL_PARAM_ID_HD_VOICE";
        return;
    }
}

void Telephony::configureDeviceMute() {
    if (mPalHandle == nullptr) {
        LOG(ERROR) << __func__ << ": invalid pal handle";
        return;
    }
    auto byteSize = sizeof(pal_param_payload) + sizeof(pal_device_mute_t);
    auto bytes = std::make_unique<uint8_t[]>(byteSize);
    auto palParamPayload = reinterpret_cast<pal_param_payload*>(bytes.get());
    palParamPayload->payload_size = sizeof(pal_device_mute_t);
    auto palDeviceMute = reinterpret_cast<pal_device_mute_t*>(palParamPayload->payload);
    palDeviceMute->mute = mIsDeviceMuted;
    if (mMuteDirection == "rx") {
        palDeviceMute->dir = PAL_AUDIO_OUTPUT;
    } else {
        palDeviceMute->dir = PAL_AUDIO_INPUT;
    }
    if (int32_t ret = ::pal_stream_set_param(mPalHandle, PAL_PARAM_ID_DEVICE_MUTE, palParamPayload);
        ret) {
        LOG(ERROR) << __func__ << ": failed to set PAL_PARAM_ID_DEVICE_MUTE";
        return;
    }
}

void Telephony::setVoipPlaybackStream(std::weak_ptr<StreamCommonInterface> voipStream) {
    std::scoped_lock lock{mLock};
    mVoipStreamWptr = voipStream;
}

void Telephony::triggerHACinVoipPlayback() {
    auto voipStream = mVoipStreamWptr.lock();
    if (!voipStream) {
        return;
    }
    const auto& voipConnectedDevices = voipStream->getConnectedDevices();
    if (hasOutputSpeakerEarpiece(voipConnectedDevices)) {
        LOG(INFO) << __func__ << ": HAC status changed for VOIP playback";
        voipStream->reconfigureConnectedDevices();
    }
}

void Telephony::getPlaybackStreamDevices() {
    if (hasValidPlaybackStream) {
       mRxDevice = mPlaybackStreamDevices[0];
       mTxDevice = getMatchingTxDevice(mRxDevice);
   }
}

void Telephony::onPlaybackStart(const std::vector<AudioDevice>& playbackStreamDevices) {
    std::scoped_lock lock{mLock};

    if (playbackStreamDevices.size() == 1 &&
        isValidDevice(playbackStreamDevices[0])) {
        mPlaybackStreamDevices = playbackStreamDevices;
        hasValidPlaybackStream = true;
        if (mIsCRSStarted) {
            mRxDevice = playbackStreamDevices[0];
            mTxDevice = getMatchingTxDevice(mRxDevice);
            LOG(INFO) << __func__ << ": playback conc status change for CRS call";
            updateDevices();
        }
    }
}

void Telephony::onPlaybackClose() {
    std::scoped_lock lock{mLock};
    if (hasValidPlaybackStream) {
        hasValidPlaybackStream = false;
        if (mIsCRSStarted) {
            LOG(INFO) << __func__ << ": playback conc status stop for CRS call";
            updateDevices();
        }
    }
}

void Telephony::setCRSVolumeFromIndex(const int index) {
    std::scoped_lock lock{mLock};
    if (index <= MAX_CRS_VOL_INDEX && index >= MIN_CRS_VOL_INDEX)
        mCRSVolume = index / 10.0;
    else {
        mCRSVolume = 0.4;
        LOG(INFO) << __func__ << ": use defalut CRS volume: " << mCRSVolume;
    }
    updateVoiceVolume();
}

void Telephony::updateVoiceVolume() {
    if (mPalHandle == nullptr) {
        return;
    }
    float volumeFloat = 0.0f;
    if (mSetUpdates.mIsCrsCall) {
        volumeFloat = mCRSVolume;
    } else if (mPlatform.getTranslationRxMuteState()) {
        volumeFloat = 0.0f;
        LOG(INFO) << __func__ << ": set voice volume to mute.";
    } else {
        volumeFloat = mTelecomConfig.voiceVolume ? mTelecomConfig.voiceVolume.value().value : 1.0;
    }

    if (int32_t ret = mPlatform.setVolume(mPalHandle, {volumeFloat}); ret) {
        LOG(ERROR) << __func__ << ": pal stream set volume failed !!" << ret;
        return;
    }
    LOG(DEBUG) << __func__ << ": updated voice volume value as " << volumeFloat;
}

void Telephony::updateTtyMode() {
    if (mPalHandle == nullptr) {
        return;
    }
    // Todo fix this bad size conversion
    const size_t ttyModeSizeInBytes = 4;
    auto bytes = std::make_unique<uint8_t[]>(sizeof(pal_param_payload) + ttyModeSizeInBytes);
    auto paramPtr = reinterpret_cast<pal_param_payload*>(bytes.get());
    if (int32_t ret = ::pal_stream_set_param(mPalHandle, PAL_PARAM_ID_TTY_MODE, paramPtr); ret) {
        LOG(ERROR) << __func__ << ": failed to set PAL_PARAM_ID_TTY_MODE";
        return;
    }
    LOG(VERBOSE) << __func__ << ": success";
    return;
}

ndk::ScopedAStatus Telephony::startCall() {
    LOG(DEBUG) << __func__ << ": Enter: "
               << " Rx: " << mRxDevice.toString() << " Tx: " << mTxDevice.toString();
    auto attributes = mPlatform.getDefaultTelephonyAttributes();

    auto palDevices = mPlatform.convertToPalDevices({mRxDevice, mTxDevice});

    attributes->info.voice_call_info.VSID = static_cast<uint32_t>(mSetUpdates.mVSID);
    {
        const auto ttyMode = mTtyMap.find(mTelecomConfig.ttyMode);
        attributes->info.voice_call_info.tty_mode =
                ttyMode != mTtyMap.cend() ? ttyMode->second : PAL_TTY_OFF;
    }
    //override device pairs of HCO and VCO usecase
    updateTTYPalDevices(palDevices);

    const size_t numDevices = 2;
    //set custom key for hac mode
    if (mTelecomConfig.isHacEnabled.has_value() && mTelecomConfig.isHacEnabled.value().value &&
        palDevices[0].id == PAL_DEVICE_OUT_HANDSET) {
        strlcpy(palDevices[0].custom_config.custom_key, "HAC",
                sizeof(palDevices[0].custom_config.custom_key));
        LOG(VERBOSE) << __func__ << "setting custom key as " << palDevices[0].custom_config.custom_key;
    }
    if (mSetUpdates.mIsCrsCall) {
        strlcpy(palDevices[0].custom_config.custom_key, "crsCall",
                sizeof(palDevices[0].custom_config.custom_key));
        LOG(VERBOSE) << __func__ << "setting custom key as " << palDevices[0].custom_config.custom_key;
    }

    if (int32_t ret = ::pal_stream_open(
                attributes.get(), numDevices, reinterpret_cast<pal_device*>(palDevices.data()), 0,
                nullptr, nullptr, reinterpret_cast<uint64_t>(this), &mPalHandle);
        ret) {
        LOG(ERROR) << __func__ << ": pal stream open failed !!" << ret;
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }
    if (int32_t ret = ::pal_stream_start(mPalHandle); ret) {
        LOG(ERROR) << __func__ << ": pal stream start failed !!" << ret;
        pal_stream_close(mPalHandle);
        mPalHandle = nullptr;
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }
    if (mPlatform.getMicMuteStatus()) {
        mPlatform.setStreamMicMute(mPalHandle, true);
    }
    updateVoiceVolume();
    if (mIsDeviceMuted) {
        configureDeviceMute();
    }
    if (mSetUpdates.mIsCrsCall) {
        mPlatform.setStreamMicMute(mPalHandle, true);
        LOG(DEBUG) << __func__ << ": CRS usecase mute TX";
    }
    LOG(DEBUG) << __func__ << ": Exit : Voice Stream";
    return ndk::ScopedAStatus::ok();
}

void Telephony::startCrsLoopback() {
    LOG(DEBUG) << __func__ << ": Enter";
    if (hasValidPlaybackStream) {
        LOG(VERBOSE) << __func__ << ": block loopback start";
        return;
    }
    auto attributes = mPlatform.getDefaultCRSTelephonyAttributes();
    std::vector<::aidl::android::media::audio::common::AudioDevice> RxDevices;
    RxDevices = {kDefaultCRSRxDevice};

    auto palDevices = mPlatform.convertToPalDevices({RxDevices});
    palDevices[0].id = PAL_DEVICE_OUT_SPEAKER;
    palDevices[0].config.sample_rate = Platform::kDefaultOutputSampleRate;
    palDevices[0].config.bit_width = Platform::kDefaultPCMBidWidth;
    palDevices[0].config.aud_fmt_id = PAL_AUDIO_FMT_PCM_S16_LE;

    attributes->info.voice_call_info.VSID = static_cast<uint32_t>(mSetUpdates.mVSID);
    if (mSetUpdates.mIsCrsCall) {
        strlcpy(palDevices[0].custom_config.custom_key, "crsCall",
                sizeof(palDevices[0].custom_config.custom_key));
        LOG(VERBOSE) << __func__ << " setting custom key as ", palDevices[0].custom_config.custom_key;
    } else {
        strlcpy(palDevices[0].custom_config.custom_key, "",
                sizeof(palDevices[0].custom_config.custom_key));
    }

    const size_t numDevices = 1;
    if (int32_t ret = ::pal_stream_open(
                attributes.get(), numDevices, reinterpret_cast<pal_device*>(palDevices.data()), 0,
                nullptr, nullptr, reinterpret_cast<uint64_t>(this), &mPalCrsHandle);
        ret) {
        LOG(ERROR) << __func__ << ": pal stream open failed !!" << ret;
        return;
    }
    if (int32_t ret = ::pal_stream_start(mPalCrsHandle); ret) {
        LOG(ERROR) << __func__ << ": pal stream open failed !!" << ret;
        return;
    }
    updateVoiceVolume();
    LOG(DEBUG) << __func__ << ": Exit";
}

ndk::ScopedAStatus Telephony::stopCall() {
    LOG(DEBUG) << __func__ << ": Enter";
    if (mPalHandle == nullptr) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
    int32_t ret = 0;
    auto palDevices = mPlatform.convertToPalDevices({mRxDevice, mTxDevice});
    if (mSetUpdates.mIsCrsCall) {
        strlcpy(palDevices[0].custom_config.custom_key, "",
                sizeof(palDevices[0].custom_config.custom_key));
        LOG(VERBOSE) << __func__ << "setting custom key as ", palDevices[0].custom_config.custom_key;
    }
    if (int32_t ret = pal_stream_stop(mPalHandle); ret) {
        LOG(ERROR) << __func__ << ": pal stream stop failed !!" << ret;
    }
    if (int32_t ret = pal_stream_close(mPalHandle); ret) {
        LOG(ERROR) << __func__ << ": pal stream stop failed !!" << ret;
    }
    if ((palDevices[0].id == PAL_DEVICE_OUT_BLUETOOTH_BLE) &&
        (palDevices[1].id == PAL_DEVICE_IN_BLUETOOTH_BLE)) {
        updateVoiceMetadataForBT(false);
    }
    mPalHandle = nullptr;
    if (mSetUpdates.mIsCrsCall &&
        mRxDevice.type.type == AudioDeviceType::OUT_SPEAKER) {
        mRxDevice = kDefaultRxDevice;
        mTxDevice = getMatchingTxDevice(mRxDevice);
    }
    if (ret) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }
    LOG(DEBUG) << __func__ << ": EXIT";
    return ndk::ScopedAStatus::ok();
}

void Telephony::stopCrsLoopback() {
    LOG(DEBUG) << __func__ << ": Enter";
    if (mPalCrsHandle == nullptr) {
        return;
    }
    std::vector<::aidl::android::media::audio::common::AudioDevice> RxDevices;
    RxDevices = {kDefaultCRSRxDevice};
    auto palDevices = mPlatform.convertToPalDevices({RxDevices});
    if (mSetUpdates.mIsCrsCall) {
        strlcpy(palDevices[0].custom_config.custom_key, "",
                sizeof(palDevices[0].custom_config.custom_key));
        LOG(VERBOSE) << __func__ << "setting custom key as ", palDevices[0].custom_config.custom_key;
    }
    ::pal_stream_stop(mPalCrsHandle);
    ::pal_stream_close(mPalCrsHandle);
    mPalCrsHandle = nullptr;
    LOG(DEBUG) << __func__ << ": EXIT";
}

void Telephony::updateDevices() {

    auto palDevices = mPlatform.convertToPalDevices({mRxDevice, mTxDevice});

    pal_param_bta2dp_t* param_bt_a2dp_ptr = nullptr;
    bool a2dp_capture_suspended = false;
    size_t bt_param_size = 0;
    bool a2dp_suspended = false;
    int ret = 0;
    int retry_cnt = 20;
    const int retry_period_ms = 100;
    bool is_suspend_setparam = false;
    LOG(INFO) << __func__ << ": Enter";

    if (!isAnyCallActive()) {
        if (mAudioMode == AudioMode::IN_CALL && (mPalHandle == nullptr || mSetUpdates.mIsCrsCall)) {
            mIsVoiceStarted = true;
            updateCalls();
            return;
        }
    }

    // TODO configure pal devices with custom key if any
    if (mSetUpdates.mCallState == CallState::ACTIVE) {
        /*In case of active LEA profile, if voice call accepted by an inactive legacy headset
         * over SCO profile. APM is not aware about SCO active profile until BT_SCO=ON
         * event triggers from BT. In meantime before BT_SCO=ON, when LEA is suspended via
         * setparam call, APM tries to route voice call to BLE device.
         * In RouteStream call, if suspended state for LEA is true it keep checks over a
         * sleep of 2 secs. This causes timecheck issue in audioservice. Thus check for
         * is_suspend_setparam flag to know whether BLE suspended due to the actual setparam
         * or reconfig_cb(suspend->resume).
       */
        if ((palDevices[0].id == PAL_DEVICE_OUT_BLUETOOTH_BLE) &&
            (palDevices[1].id == PAL_DEVICE_IN_BLUETOOTH_BLE)) {
            updateVoiceMetadataForBT(true);
            pal_param_bta2dp_t param_bt_a2dp;
            do {
                std::unique_lock<std::mutex> guard(AudioExtension::reconfig_wait_mutex_);
                param_bt_a2dp_ptr = &param_bt_a2dp;
                param_bt_a2dp_ptr->dev_id = PAL_DEVICE_OUT_BLUETOOTH_BLE;

                ret = pal_get_param(PAL_PARAM_ID_BT_A2DP_SUSPENDED, (void**)&param_bt_a2dp_ptr,
                                    &bt_param_size, nullptr);
                if (!ret && bt_param_size && param_bt_a2dp_ptr) {
                    a2dp_suspended = param_bt_a2dp_ptr->a2dp_suspended;
                    is_suspend_setparam = param_bt_a2dp_ptr->is_suspend_setparam;
                } else {
                    LOG(ERROR) << __func__ << "getparam for PAL_PARAM_ID_BT_A2DP_SUSPENDED failed";
                }
                param_bt_a2dp_ptr = &param_bt_a2dp;
                param_bt_a2dp_ptr->dev_id = PAL_DEVICE_IN_BLUETOOTH_BLE;
                bt_param_size = 0;
                ret = pal_get_param(PAL_PARAM_ID_BT_A2DP_CAPTURE_SUSPENDED,
                                    (void**)&param_bt_a2dp_ptr, &bt_param_size, nullptr);
                if (!ret && bt_param_size && param_bt_a2dp_ptr)
                    a2dp_capture_suspended = param_bt_a2dp_ptr->a2dp_capture_suspended;
                else
                    LOG(ERROR) << __func__ << "getparam for BT_A2DP_CAPTURE_SUSPENDED failed";
                param_bt_a2dp_ptr = nullptr;
                bt_param_size = 0;
            } while (!is_suspend_setparam && (a2dp_suspended || a2dp_capture_suspended) &&
                     retry_cnt-- && !usleep(retry_period_ms * 1000));
            LOG(INFO) << __func__ << "a2dp_suspended status: " << a2dp_suspended
                      << "and a2dp_capture_suspended status: " << a2dp_capture_suspended;
        }
    }

    //override device pairs of HCO and VCO usecase
    updateTTYPalDevices(palDevices);

    //set or remove custom key for hac mode
    if (mTelecomConfig.isHacEnabled.has_value() && mTelecomConfig.isHacEnabled.value().value &&
        palDevices[0].id == PAL_DEVICE_OUT_HANDSET) {
        strlcpy(palDevices[0].custom_config.custom_key, "HAC",
                sizeof(palDevices[0].custom_config.custom_key));
        LOG(VERBOSE) << __func__ << "setting custom key as " << palDevices[0].custom_config.custom_key;
    } else {
        strlcpy(palDevices[0].custom_config.custom_key, "",
                sizeof(palDevices[0].custom_config.custom_key));
    }

    if (mSetUpdates.mIsCrsCall) {
        if (mPalCrsHandle != nullptr)
            stopCrsLoopback();
        updateCrsDevice();
        palDevices = mPlatform.convertToPalDevices({mRxDevice, mTxDevice});
        strlcpy(palDevices[0].custom_config.custom_key, "crsCall",
                  sizeof(palDevices[0].custom_config.custom_key));
    }

    if (mPalHandle == nullptr) return;

    if (int32_t ret = ::pal_stream_set_device(mPalHandle, 2,
                                              reinterpret_cast<pal_device*>(palDevices.data()));
        ret) {
        LOG(ERROR) << __func__ << ": failed to set devices";
        return;
    }
    if (mSetUpdates.mIsCrsCall) {
        if (mRxDevice.type.type != AudioDeviceType::OUT_SPEAKER &&
            mRxDevice.type.type != AudioDeviceType::OUT_SPEAKER_EARPIECE) {
            startCrsLoopback();
        }
    }
    updateVoiceVolume();
    if (mIsDeviceMuted) {
        configureDeviceMute();
    }
    LOG(DEBUG) << __func__ << ": Exit : Rx: " << mRxDevice.toString() << " Tx: " << mTxDevice.toString();
}

std::ostream& operator<<(std::ostream& os, const Telephony::CallState& state) {
    switch (state) {
        case Telephony::CallState::IN_ACTIVE:
            os << "IN_ACTIVE";
            break;
        case Telephony::CallState::ACTIVE:
            os << "ACTIVE";
            break;
        default:
            os << "UNKNOWN";
            break;
    }
    return os;
}

std::ostream& operator<<(std::ostream& os, const Telephony::VSID& vsid) {
    switch (vsid) {
        case Telephony::VSID::VSID_1:
            os << "VSID_1";
            break;
        case Telephony::VSID::VSID_2:
            os << "VSID_2";
            break;
        default:
            os << "UNKNOWN";
            break;
    }
    return os;
}

} // namespace qti::audio::core
