/*
 * Copyright (c) 2023-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#define LOG_TAG "AHAL_AudioExtension_QTI"

#include <qti-audio-core/Utils.h>
#include <android-base/logging.h>
#include <dlfcn.h>
#include <extensions/AudioExtension.h>
#include <log/log.h>
#include "PalApi.h"

#define DEFAULT_OUTPUT_SAMPLING_RATE 48000
#define CODEC_BACKEND_DEFAULT_BIT_WIDTH 16
#define AFS_PARAMETER_QVA_VERSION "qva.version"

#define AUDIO_PARAMETER_KEY_CAN_OPEN_PROXY "can_open_proxy"

#define AFS_QVA_FILE_NAME "/data/vendor/audio/adc_qva_version.txt"

using ::aidl::android::media::audio::common::AudioDevice;
using ::aidl::android::media::audio::common::AudioDeviceType;
using ::aidl::android::media::audio::common::AudioDeviceDescription;

namespace qti::audio::core {

std::mutex AudioExtension::reconfig_wait_mutex_;
bool BatteryListenerExtension::isCharging;

AudioExtensionBase::AudioExtensionBase(std::string library, bool enabled)
    : mEnabled(enabled), mLibraryName(library) {
    LOG(INFO) << __func__ << " opening " << mLibraryName.c_str() << " enabled " << enabled;
    if (mEnabled) {
        mHandle = dlopen(mLibraryName.c_str(), RTLD_LAZY);
        if (mHandle == nullptr) {
            const char *error = dlerror();
            LOG(INFO) << __func__ << " Failed to dlopen  " << mLibraryName.c_str() << " error  "
                      << error;
        }
    }
}

AudioExtensionBase::~AudioExtensionBase() {
    cleanUp();
}

void AudioExtension::audio_extn_get_parameters(struct str_parms *query, struct str_parms *reply) {
    char *kv_pairs = NULL;
    char value[32] = {0};
    int ret, val = 0;
}
void AudioExtension::audio_extn_set_parameters(struct str_parms *params) {
    mHfpExtension->audio_extn_hfp_set_parameters(params);
    mFmExtension->audio_extn_fm_set_parameters(params);
    audio_feature_stats_set_parameters(params);
}

void AudioExtension::audio_feature_stats_set_parameters(struct str_parms *params) {
    FILE *fp;
    int status = 0;
    char value[50] = {0};

    status = str_parms_get_str(params, AFS_PARAMETER_QVA_VERSION, value, sizeof(value));
    if (status >= 0) {
        fp = fopen(AFS_QVA_FILE_NAME, "w");
        if (!fp) {
            LOG(ERROR) << __func__ << " File open failed for write";
        } else {
            char qva_version[50] = "qva_version=";
            strlcat(qva_version, value, sizeof(qva_version));
            LOG(DEBUG) << __func__ << " QVA Version : " << qva_version;
            fprintf(fp, "%s", qva_version);
            fclose(fp);
        }
    }
}

void AudioExtensionBase::cleanUp() {
    if (mHandle != nullptr) {
        dlclose(mHandle);
    }
}

void BatteryListenerExtension::setChargingMode(bool is_charging) {
    int32_t result = 0;
    pal_param_charging_state_t charge_state;

    LOG(DEBUG) << __func__ << " enter, is_charging " << is_charging;
    isCharging = is_charging;
    charge_state.charging_state = is_charging;

    result = pal_set_param(PAL_PARAM_ID_CHARGING_STATE, (void *)&charge_state,
                           sizeof(pal_param_charging_state_t));
    if (result) LOG(DEBUG) << __func__ << " error while handling charging event result " << result;

    LOG(DEBUG) << __func__ << " exit";
}

void on_battery_status_changed(bool charging) {
    LOG(DEBUG) << __func__ << " battery status changed to " << charging;
    BatteryListenerExtension::setChargingMode(charging);
}

BatteryListenerExtension::~BatteryListenerExtension() {
    battery_properties_listener_deinit();
}

void BatteryListenerExtension::battery_properties_listener_deinit() {
    if (batt_listener_deinit) batt_listener_deinit();
}

bool BatteryListenerExtension::battery_properties_is_charging() {
    return (batt_prop_is_charging) ? batt_prop_is_charging() : false;
}

void BatteryListenerExtension::battery_properties_listener_init() {
    if (batt_listener_init) batt_listener_init(on_battery_status_changed);
}

BatteryListenerExtension::BatteryListenerExtension()
    : AudioExtensionBase(kBatteryListenerLibrary, isExtensionEnabled(kBatteryListenerProperty)) {
    LOG(INFO) << __func__ << " Enter";
    if (mHandle != nullptr) {
        if (!(batt_listener_init =
                      (batt_listener_init_t)dlsym(mHandle, "battery_properties_listener_init")) ||
            !(batt_listener_deinit = (batt_listener_deinit_t)dlsym(
                      mHandle, "battery_properties_listener_deinit")) ||
            !(batt_prop_is_charging =
                      (batt_prop_is_charging_t)dlsym(mHandle, "battery_properties_is_charging"))) {
            LOG(ERROR) << __func__ << "dlsym failed";
            goto feature_disabled;
        }
        LOG(INFO) << __func__ << "----- Feature BATTERY_LISTENER is enabled ----";
        battery_properties_listener_init();
        setChargingMode(battery_properties_is_charging());
        return;
    }

feature_disabled:
    if (mHandle) {
        dlclose(mHandle);
        mHandle = NULL;
    }

    batt_listener_init = NULL;
    batt_listener_deinit = NULL;
    batt_prop_is_charging = NULL;
    LOG(INFO) << __func__ << "----- Feature BATTERY_LISTENER is disabled ----";
}

static int reconfig_cb(tSESSION_TYPE session_type, int state) {
    int ret = 0;
    pal_param_bta2dp_t param_bt_a2dp;
    LOG(DEBUG) << __func__ << " reconfig_cb enter with state "
               << reconfigStateName.at(state).c_str() << " for "
               << deviceNameLUT.at(SessionTypePalDevMap.at(session_type)).c_str();

    /* If reconfiguration is in progress state (state = 0), perform a2dp suspend.
     * If reconfiguration is in complete state (state = 1), perform a2dp resume.
     * Set LC3 channel mode as mono (state = 2).
     * Set LC3 channel mode as stereo (state = 3).
     */
    if (session_type == LE_AUDIO_HARDWARE_OFFLOAD_ENCODING_DATAPATH) {
        if ((tRECONFIG_STATE)state == SESSION_SUSPEND) {
            std::unique_lock<std::mutex> guard(AudioExtension::reconfig_wait_mutex_);
            param_bt_a2dp.a2dp_suspended = true;
            param_bt_a2dp.is_suspend_setparam = false;
            param_bt_a2dp.dev_id = PAL_DEVICE_OUT_BLUETOOTH_BLE;

            ret = pal_set_param(PAL_PARAM_ID_BT_A2DP_SUSPENDED, (void *)&param_bt_a2dp,
                                sizeof(pal_param_bta2dp_t));
        } else if ((tRECONFIG_STATE)state == SESSION_RESUME) {
            std::unique_lock<std::mutex> guard(AudioExtension::reconfig_wait_mutex_);
            param_bt_a2dp.a2dp_suspended = false;
            param_bt_a2dp.is_suspend_setparam = false;
            param_bt_a2dp.dev_id = PAL_DEVICE_OUT_BLUETOOTH_BLE;

            ret = pal_set_param(PAL_PARAM_ID_BT_A2DP_SUSPENDED, (void *)&param_bt_a2dp,
                                sizeof(pal_param_bta2dp_t));
        } else if ((tRECONFIG_STATE)state == CHANNEL_MONO) {
            param_bt_a2dp.is_lc3_mono_mode_on = true;

            ret = pal_set_param(PAL_PARAM_ID_BT_A2DP_LC3_CONFIG, (void *)&param_bt_a2dp,
                                sizeof(pal_param_bta2dp_t));
        } else if ((tRECONFIG_STATE)state == CHANNEL_STEREO) {
            param_bt_a2dp.is_lc3_mono_mode_on = false;

            ret = pal_set_param(PAL_PARAM_ID_BT_A2DP_LC3_CONFIG, (void *)&param_bt_a2dp,
                                sizeof(pal_param_bta2dp_t));
        }
    } else if (session_type == LE_AUDIO_HARDWARE_OFFLOAD_DECODING_DATAPATH) {
        if ((tRECONFIG_STATE)state == SESSION_SUSPEND) {
            std::unique_lock<std::mutex> guard(AudioExtension::reconfig_wait_mutex_);
            param_bt_a2dp.a2dp_capture_suspended = true;
            param_bt_a2dp.is_suspend_setparam = false;
            param_bt_a2dp.dev_id = PAL_DEVICE_IN_BLUETOOTH_BLE;

            ret = pal_set_param(PAL_PARAM_ID_BT_A2DP_CAPTURE_SUSPENDED, (void *)&param_bt_a2dp,
                                sizeof(pal_param_bta2dp_t));
        } else if ((tRECONFIG_STATE)state == SESSION_RESUME) {
            std::unique_lock<std::mutex> guard(AudioExtension::reconfig_wait_mutex_);
            param_bt_a2dp.a2dp_capture_suspended = false;
            param_bt_a2dp.is_suspend_setparam = false;
            param_bt_a2dp.dev_id = PAL_DEVICE_IN_BLUETOOTH_BLE;

            ret = pal_set_param(PAL_PARAM_ID_BT_A2DP_CAPTURE_SUSPENDED, (void *)&param_bt_a2dp,
                                sizeof(pal_param_bta2dp_t));
        }
    } else if (session_type == A2DP_HARDWARE_OFFLOAD_DATAPATH) {
        if ((tRECONFIG_STATE)state == SESSION_SUSPEND) {
            std::unique_lock<std::mutex> guard(AudioExtension::reconfig_wait_mutex_);
            param_bt_a2dp.a2dp_suspended = true;
            param_bt_a2dp.is_suspend_setparam = false;
            param_bt_a2dp.dev_id = PAL_DEVICE_OUT_BLUETOOTH_A2DP;

            ret = pal_set_param(PAL_PARAM_ID_BT_A2DP_SUSPENDED, (void *)&param_bt_a2dp,
                                sizeof(pal_param_bta2dp_t));
        } else if ((tRECONFIG_STATE)state == SESSION_RESUME) {
            std::unique_lock<std::mutex> guard(AudioExtension::reconfig_wait_mutex_);
            param_bt_a2dp.a2dp_suspended = false;
            param_bt_a2dp.is_suspend_setparam = false;
            param_bt_a2dp.dev_id = PAL_DEVICE_OUT_BLUETOOTH_A2DP;

            ret = pal_set_param(PAL_PARAM_ID_BT_A2DP_SUSPENDED, (void *)&param_bt_a2dp,
                                sizeof(pal_param_bta2dp_t));
        }
    }
    LOG(DEBUG) << __func__ << " reconfig_cb exit with state " << reconfigStateName.at(state).c_str()
               << " for " << deviceNameLUT.at(SessionTypePalDevMap.at(session_type)).c_str();
    return ret;
}

A2dpExtension::~A2dpExtension() {}
A2dpExtension::A2dpExtension()
    : AudioExtensionBase(kBluetoothIpcLibrary, isExtensionEnabled(kBluetoothProperty)) {
    LOG(INFO) << __func__ << " Enter";
    if (mHandle != nullptr) {
        if (!(a2dp_bt_audio_pre_init =
                      (a2dp_bt_audio_pre_init_t)dlsym(mHandle, "bt_audio_pre_init"))) {
            LOG(ERROR) << __func__ << " dlsym failed";
            goto feature_disabled;
        }

        if (mHandle && a2dp_bt_audio_pre_init) {
            LOG(VERBOSE) << __func__ << " calling BT module preinit";
            // fwk related check's will be done in the BT layer
            a2dp_bt_audio_pre_init();
        }

        if (!(register_reconfig_cb =
                      (register_reconfig_cb_t)dlsym(mHandle, "register_reconfig_cb"))) {
            LOG(ERROR) << __func__ << " dlsym failed for reconfig";
            goto feature_disabled;
        }

        if (mHandle && register_reconfig_cb) {
            LOG(VERBOSE) << __func__ << " calling BT module register reconfig";
            int (*reconfig_cb_ptr)(tSESSION_TYPE, int) = &reconfig_cb;
            register_reconfig_cb(reconfig_cb_ptr);
        }

        LOG(VERBOSE) << __func__ << "---- Feature A2DP offload is Enabled ---";
        return;
    }

feature_disabled:
    if (mHandle) {
        dlclose(mHandle);
        mHandle = NULL;
    }

    a2dp_bt_audio_pre_init = nullptr;
    LOG(VERBOSE) << __func__ << "---- Feature A2DP offload is disabled ---";
}

AudioDevice HfpExtension::audio_extn_hfp_get_matching_tx_device(const AudioDevice& rxDevice) {
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
    } else if ((rxDevice.type.type == AudioDeviceType::OUT_DEVICE ||
                rxDevice.type.type == AudioDeviceType::OUT_HEADSET) &&
               rxDevice.type.connection == AudioDeviceDescription::CONNECTION_BT_SCO) {
        return AudioDevice{.type.type = AudioDeviceType::IN_HEADSET,
                           .type.connection = AudioDeviceDescription::CONNECTION_BT_SCO};
    } else if (rxDevice.type.type == AudioDeviceType::OUT_HEADSET &&
               rxDevice.type.connection == AudioDeviceDescription::CONNECTION_BT_LE) {
        return AudioDevice{.type.type = AudioDeviceType::IN_HEADSET,
                           .type.connection = AudioDeviceDescription::CONNECTION_BT_LE};
    } else if ((rxDevice.type.type == AudioDeviceType::OUT_DEVICE ||
                rxDevice.type.type == AudioDeviceType::OUT_HEADSET) &&
               rxDevice.type.connection == AudioDeviceDescription::CONNECTION_USB) {
        if (mPlatform.getUSBCapEnable()) {
            return AudioDevice{.type.type = AudioDeviceType::IN_HEADSET,
                               .type.connection = AudioDeviceDescription::CONNECTION_USB,
                               .address = rxDevice.address};
        } else {
            return AudioDevice{.type.type = AudioDeviceType::IN_MICROPHONE};
        }
    } else if (rxDevice.type.type == AudioDeviceType::OUT_HEARING_AID) {
        return AudioDevice{.type.type = AudioDeviceType::IN_MICROPHONE};
    } else {
        LOG(ERROR) << __func__ << ": unable to find matching TX device for " << rxDevice.toString();
    }
    return {};
}

void HfpExtension::audio_extn_hfp_set_device(const std::vector<AudioDevice>& devices,
                                              const bool updateRx) {
    AudioDevice rxDevice;
    AudioDevice txDevice;
    if (devices.size() != 1) {
        LOG(ERROR) << __func__ << " invalid size / combo devices unsupported: " << devices;
        return;
    }

    LOG(DEBUG) << __func__ << (updateRx ? " Rx " : " Tx") << " devices : " << devices;
    if (updateRx) {
        rxDevice = devices[0];
        txDevice = audio_extn_hfp_get_matching_tx_device(rxDevice);
        if (hfp_set_device) {
            auto palDevices = mPlatform.convertToPalDevices({rxDevice, txDevice});
            hfp_set_device(reinterpret_cast<pal_device*>(palDevices.data()));
        }
    }
}

void HfpExtension::audio_extn_hfp_set_parameters(struct str_parms *params) {
    if (hfp_set_parameters) hfp_set_parameters(micMute, params);
}

int HfpExtension::audio_extn_hfp_set_mic_mute(bool state) {
    if (audio_extn_hfp_is_active()) {
        micMute = state;
        return ((hfp_set_mic_mute) ? hfp_set_mic_mute(state) : -1);
    }
    return -1;
}

bool HfpExtension::audio_extn_hfp_is_active() {
    return ((hfp_is_active) ? hfp_is_active() : false);
}

HfpExtension::~HfpExtension() {}
HfpExtension::HfpExtension() : AudioExtensionBase(kHfpLibrary, isExtensionEnabled(kHfpProperty)) {
    LOG(INFO) << __func__ << " Enter";
    if (mHandle != nullptr) {
        if (!(hfp_init = (hfp_init_t)dlsym(mHandle, "hfp_init")) ||
            !(hfp_is_active = (hfp_is_active_t)dlsym(mHandle, "hfp_is_active")) ||
            !(hfp_set_mic_mute = (hfp_set_mic_mute_t)dlsym(mHandle, "hfp_set_mic_mute")) ||
            !(hfp_set_mic_mute2 = (hfp_set_mic_mute2_t)dlsym(mHandle, "hfp_set_mic_mute2")) ||
            !(hfp_set_parameters = (hfp_set_parameters_t)dlsym(mHandle, "hfp_set_parameters")) ||
            !(hfp_set_device = (hfp_set_device_t)dlsym(mHandle, "hfp_set_device"))) {
            LOG(ERROR) << __func__ << " dlsym failed";
            goto feature_disabled;
        }
        LOG(DEBUG) << __func__ << "---- Feature HFP is Enabled ----";
        return;
    }

feature_disabled:
    if (mHandle) {
        dlclose(mHandle);
        mHandle = NULL;
    }

    hfp_init = NULL;
    hfp_is_active = NULL;
    hfp_get_usecase = NULL;
    hfp_set_mic_mute = NULL;
    hfp_set_mic_mute2 = NULL;
    hfp_set_parameters = NULL;
}

FmExtension::~FmExtension() {}

bool FmExtension::audio_extn_fm_get_status() {
    if (fm_running_status) return fm_running_status;

    return false;
}

void FmExtension::audio_extn_fm_set_parameters(struct str_parms *params) {
    if (fm_set_params) fm_set_params(params);
}
FmExtension::FmExtension() : AudioExtensionBase(kFmLibrary) {
    LOG(INFO) << __func__ << " Enter";
    if (mHandle != nullptr) {
        fm_set_params = (set_parameters_t)dlsym(mHandle, "fm_set_parameters");
        fm_running_status = (fm_running_status_t)dlsym(mHandle, "fm_get_running_status");
        if (!fm_set_params || !fm_running_status) {
            LOG(ERROR) << "error " << dlerror();
            dlclose(mHandle);
            fm_set_params = NULL;
            fm_running_status = NULL;
        }
    } else {
        fm_set_params = NULL;
        fm_running_status = NULL;
    }
}

int KarokeExtension::karaoke_open(pal_device_id_t device_out, pal_stream_callback pal_callback,
                                  pal_channel_info ch_info) {
    // std::shared_ptr<AudioDevice> adevice = AudioDevice::GetInstance();
    const int num_pal_devs = 2;
    struct pal_device pal_devs[num_pal_devs];
    karaoke_stream_handle = NULL;
    pal_device_id_t device_in;
    dynamic_media_config_t dynamic_media_config;
    size_t payload_size = 0;

    // Configuring Hostless Loopback
    if (device_out == PAL_DEVICE_OUT_WIRED_HEADSET)
        device_in = PAL_DEVICE_IN_WIRED_HEADSET;
    else if (device_out == PAL_DEVICE_OUT_USB_HEADSET) {
        device_in = PAL_DEVICE_IN_USB_HEADSET;
        // get capability from device of USB
    } else
        return 0;

    sattr.type = PAL_STREAM_LOOPBACK;
    sattr.info.opt_stream_info.loopback_type = PAL_STREAM_LOOPBACK_KARAOKE;
    sattr.direction = PAL_AUDIO_INPUT_OUTPUT;
    sattr.in_media_config.sample_rate = DEFAULT_OUTPUT_SAMPLING_RATE;
    sattr.in_media_config.bit_width = CODEC_BACKEND_DEFAULT_BIT_WIDTH;
    sattr.in_media_config.ch_info = ch_info;
    sattr.in_media_config.aud_fmt_id = PAL_AUDIO_FMT_DEFAULT_PCM;
    sattr.out_media_config.sample_rate = DEFAULT_OUTPUT_SAMPLING_RATE;
    sattr.out_media_config.bit_width = CODEC_BACKEND_DEFAULT_BIT_WIDTH;
    sattr.out_media_config.ch_info = ch_info;
    sattr.out_media_config.aud_fmt_id = PAL_AUDIO_FMT_DEFAULT_PCM;
    for (int i = 0; i < num_pal_devs; ++i) {
        pal_devs[i].id = i ? device_in : device_out;
        if (device_out == PAL_DEVICE_OUT_USB_HEADSET || device_in == PAL_DEVICE_IN_USB_HEADSET) {
            // Configure USB Digital Headset parameters
            pal_param_device_capability_t *device_cap_query =
                    (pal_param_device_capability_t *)malloc(sizeof(pal_param_device_capability_t));
            if (!device_cap_query) {
                LOG(ERROR) << __func__ << " Failed to allocate mem for device_cap_query";
                return 0;
            }

            if (pal_devs[i].id == PAL_DEVICE_OUT_USB_HEADSET) {
                device_cap_query->id = PAL_DEVICE_OUT_USB_DEVICE;
                device_cap_query->is_playback = true;
            } else {
                device_cap_query->id = PAL_DEVICE_IN_USB_DEVICE;
                device_cap_query->is_playback = false;
            }
            // TODO: //get usb details
            device_cap_query->addr.card_id = 0;    // adevice->usb_card_id_;
            device_cap_query->addr.device_num = 0; // adevice->usb_dev_num_;
            device_cap_query->config = &dynamic_media_config;
            pal_get_param(PAL_PARAM_ID_DEVICE_CAPABILITY, (void **)&device_cap_query, &payload_size,
                          nullptr);
            pal_devs[i].address.card_id = 0;    // adevice->usb_card_id_;
            pal_devs[i].address.device_num = 0; // adevice->usb_dev_num_;
            pal_devs[i].config.sample_rate = dynamic_media_config.sample_rate[0];
            pal_devs[i].config.ch_info = ch_info;
            pal_devs[i].config.aud_fmt_id = (pal_audio_fmt_t)dynamic_media_config.format[0];
            free(device_cap_query);
        } else {
            pal_devs[i].config.sample_rate = DEFAULT_OUTPUT_SAMPLING_RATE;
            pal_devs[i].config.bit_width = CODEC_BACKEND_DEFAULT_BIT_WIDTH;
            pal_devs[i].config.ch_info = ch_info;
            pal_devs[i].config.aud_fmt_id = PAL_AUDIO_FMT_DEFAULT_PCM;
        }
    }
    return pal_stream_open(&sattr, num_pal_devs, pal_devs, 0, NULL, pal_callback, (uint64_t)this,
                           &karaoke_stream_handle);
}

int KarokeExtension::karaoke_start() {
    return pal_stream_start(karaoke_stream_handle);
}

int KarokeExtension::karaoke_stop() {
    return pal_stream_stop(karaoke_stream_handle);
}

int KarokeExtension::karaoke_close() {
    return pal_stream_close(karaoke_stream_handle);
}
KarokeExtension::~KarokeExtension() {}
KarokeExtension::KarokeExtension() : AudioExtensionBase(kKarokeLibrary) {
    LOG(INFO) << __func__ << " Enter";
    if (mHandle != nullptr) {
    }
}

void GefExtension::gef_interface_init() {
    if (gef_init) gef_init();
}

void GefExtension::gef_interface_deinit() {
    if (gef_deinit) gef_deinit();
}

GefExtension::~GefExtension() {
    gef_interface_deinit();
}

GefExtension::GefExtension() : AudioExtensionBase(kGefLibrary, true) {
    LOG(INFO) << __func__ << " Enter";
    if (mHandle != nullptr) {
        if (!(gef_init = (gef_init_t)dlsym(mHandle, "gef_interface_init")) ||
            !(gef_deinit = (gef_deinit_t)dlsym(mHandle, "gef_interface_deinit"))) {
            LOG(ERROR) << __func__ << "dlsym failed";
            goto feature_disabled;
        }
        LOG(INFO) << __func__ << "----- GEF interface is initialized ----";
        gef_interface_init();
        return;
    }

feature_disabled:
    if (mHandle) {
        dlclose(mHandle);
        mHandle = NULL;
    }

    gef_init = NULL;
    gef_deinit = NULL;
}
} // namespace qti::audio::core
