/*
 * Copyright (c) 2019-2021, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of The Linux Foundation nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Changes from Qualcomm Innovation Center are provided under the following license:
 *
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#define LOG_TAG "AHAL: AudioVoice"
#define ATRACE_TAG (ATRACE_TAG_AUDIO|ATRACE_TAG_HAL)
#define LOG_NDEBUG 0

#include <stdio.h>
#include <cutils/str_parms.h>
#include "audio_extn.h"
#include "AudioVoice.h"
#include "PalApi.h"
#include "AudioCommon.h"
#include <AudioExtn.h>
#include <utils/Trace.h>

#ifndef AUDIO_MODE_CALL_SCREEN
#define AUDIO_MODE_CALL_SCREEN 4
#endif


int AudioVoice::SetMode(const audio_mode_t mode) {
    std::shared_ptr<AudioDevice> adevice = AudioDevice::GetInstance();
    int ret = 0;
    ATRACE_BEGIN("hal: SetMode");
    AHAL_DBG("Enter: mode: %d", mode);
    if (mode_ != mode) {
        /*start a new session for full voice call*/
        if ((mode ==  AUDIO_MODE_CALL_SCREEN && mode_ == AUDIO_MODE_IN_CALL)||
           (mode == AUDIO_MODE_IN_CALL && mode_ == AUDIO_MODE_CALL_SCREEN)){
            mode_ = mode;
            AHAL_DBG("call screen device switch called: %d", mode);
            VoiceSetDevice(voice_.session);
        } else {
            mode_ = mode;
            if ((voice_.in_call && mode == AUDIO_MODE_NORMAL) ||
                (mode_ == AUDIO_MODE_IN_CALL && voice_.crsCall)) {
                ret = StopCall();
                voice_.crsVsid = 0;
            } else if (mode ==  AUDIO_MODE_CALL_SCREEN)
                UpdateCalls(voice_.session);
            else if (mode == AUDIO_MODE_RINGTONE) {
                if (voice_.crsVsid != 0) {
                    voice_.crsCall = true;
                    //check CRS concurrent case happen
                    if (adevice->getCrsConcurrentState())
                        voice_.crsLoopback = false;
                    UpdateCalls(voice_.session);
                }
            }
        }
    }
    AHAL_DBG("Exit ret: %d", ret);
    ATRACE_END();
    return ret;
}

void AudioVoice::MapCrsVolume(int CrsVolumeIndex) {
     if (CrsVolumeIndex > 0 && CrsVolumeIndex < 4)
        voice_.crsVol = 0.2;
     else if (CrsVolumeIndex == 4)
       voice_.crsVol = 0.3;
     else if (CrsVolumeIndex == 5)
        voice_.crsVol = 0.4;
     else if (CrsVolumeIndex == 6)
       voice_.crsVol = 0.5;
     else if (CrsVolumeIndex >= 7)
       voice_.crsVol = 0.6;
     else
       voice_.crsVol = 0.0;
     AHAL_DBG("Crs volume is: %f", voice_.crsVol);
}

int AudioVoice::VoiceSetParameters(const char *kvpairs) {
    int value, i;
    char c_value[32];
    int ret = 0, err;
    struct str_parms *parms;
    pal_param_payload *params = nullptr;
    uint32_t tty_mode;
    bool volume_boost;
    bool slow_talk;
    bool hd_voice;
    bool hac;
    ATRACE_BEGIN("hal: VoiceSetParameters");
    parms = str_parms_create_str(kvpairs);
    if (!parms)
       return  -EINVAL;

    AHAL_DBG("Enter params: %s", kvpairs);
    err = str_parms_get_int(parms, AUDIO_PARAMETER_KEY_VSID, &value);
    if (err >= 0) {
        uint32_t vsid = value;

        err = str_parms_get_str(parms, AUDIO_PARAMETER_KEY_CRS_ACTIVE, c_value, sizeof(c_value));
        if (err >= 0) {
            if (strcmp(c_value, "true") == 0) {
                AHAL_DBG("CRS param and VSID verified");
                voice_.crsVsid = vsid;
            }
            else {
                if (vsid == voice_.crsVsid) {
                    AHAL_DBG("CRS call = false");
                    if (voice_.crsCall)
                        ret = StopCall();
                    voice_.crsVsid = 0;
                }
            }
        }

        int call_state = -1;
        err = str_parms_get_int(parms, AUDIO_PARAMETER_KEY_CALL_STATE, &value);
        if (err >= 0) {
            call_state = value;
        } else {
            AHAL_ERR("error call_state key not found");
            ret = -EINVAL;
            goto done;
        }

        if (is_valid_vsid(vsid) && is_valid_call_state(call_state)) {
            if (!voice_.crsCall) {
                if (mode_ == AUDIO_MODE_RINGTONE && vsid == voice_.crsVsid) {
                    voice_.in_call = true;
                    voice_.crsCall = true;
                }
                ret = UpdateCallState(vsid, call_state);
            }
        } else {
            AHAL_ERR("invalid vsid:%x or call_state:%d",
                     vsid, call_state);
            ret = -EINVAL;
            goto done;
        }
    }

    err = str_parms_get_int(parms, AUDIO_PARAMETER_KEY_CRS_VOLUME, &value);
    if (err >= 0) {
        float crsVol = voice_.crsVol;
        if (value >= 0) {
            MapCrsVolume(value);
            if (voice_.crsVsid && crsVol != voice_.crsVol)
                SetVoiceVolume(voice_.crsVol);
        } else
            AHAL_ERR("Invalid CRS Volume");
    }

    err = str_parms_get_str(parms, AUDIO_PARAMETER_KEY_TTY_MODE, c_value, sizeof(c_value));
    if (err >= 0) {
        if (strcmp(c_value, AUDIO_PARAMETER_VALUE_TTY_OFF) == 0)
            tty_mode = PAL_TTY_OFF;
        else if (strcmp(c_value, AUDIO_PARAMETER_VALUE_TTY_VCO) == 0)
            tty_mode = PAL_TTY_VCO;
        else if (strcmp(c_value, AUDIO_PARAMETER_VALUE_TTY_HCO) == 0)
            tty_mode = PAL_TTY_HCO;
        else if (strcmp(c_value, AUDIO_PARAMETER_VALUE_TTY_FULL) == 0)
            tty_mode = PAL_TTY_FULL;
        else {
            ret = -EINVAL;
            goto done;
        }

        for ( i = 0; i < MAX_VOICE_SESSIONS; i++) {
            voice_.session[i].tty_mode = tty_mode;
            if (IsCallActive(&voice_.session[i])) {
                params = (pal_param_payload *)calloc(1,
                                   sizeof(pal_param_payload) + sizeof(tty_mode));
                if (!params) {
                    AHAL_ERR("calloc failed for size %zu",
                            sizeof(pal_param_payload) + sizeof(tty_mode));
                    continue;
                }
                params->payload_size = sizeof(tty_mode);
                memcpy(params->payload, &tty_mode, params->payload_size);
                pal_stream_set_param(voice_.session[i].pal_voice_handle,
                                     PAL_PARAM_ID_TTY_MODE, params);
                free(params);
                params = nullptr;

                /*need to device switch for hco and vco*/
                if (tty_mode == PAL_TTY_VCO || tty_mode == PAL_TTY_HCO) {
                    VoiceSetDevice(&voice_.session[i]);
                }
            }
        }
    }
    err = str_parms_get_str(parms, AUDIO_PARAMETER_KEY_VOLUME_BOOST, c_value, sizeof(c_value));
    if (err >= 0) {
        if (strcmp(c_value, "on") == 0)
            volume_boost = true;
        else if (strcmp(c_value, "off") == 0) {
            volume_boost = false;
        }
        else {
            ret = -EINVAL;
            goto done;
        }
        params = (pal_param_payload *)calloc(1, sizeof(pal_param_payload) +
                                                sizeof(volume_boost));
        if (!params) {
            AHAL_ERR("calloc failed for size %zu",
                   sizeof(pal_param_payload) + sizeof(volume_boost));
        } else {
            params->payload_size = sizeof(volume_boost);
            params->payload[0] = volume_boost;

            for ( i = 0; i < MAX_VOICE_SESSIONS; i++) {
                voice_.session[i].volume_boost = volume_boost;
                if (IsCallActive(&voice_.session[i])) {
                    pal_stream_set_param(voice_.session[i].pal_voice_handle,
                                        PAL_PARAM_ID_VOLUME_BOOST, params);
                }
            }
            free(params);
            params = nullptr;
        }
    }

    err = str_parms_get_str(parms, AUDIO_PARAMETER_KEY_SLOWTALK, c_value, sizeof(c_value));
    if (err >= 0) {
        if (strcmp(c_value, "true") == 0)
            slow_talk = true;
        else if (strcmp(c_value, "false") == 0) {
            slow_talk = false;
        }
        else {
            ret = -EINVAL;
            goto done;
        }
        params = (pal_param_payload *)calloc(1, sizeof(pal_param_payload) +
                                                sizeof(slow_talk));
        if (!params) {
            AHAL_ERR("calloc failed for size %zu",
                   sizeof(pal_param_payload) + sizeof(slow_talk));
        } else {
            params->payload_size = sizeof(slow_talk);
            params->payload[0] = slow_talk;

            for ( i = 0; i < MAX_VOICE_SESSIONS; i++) {
                voice_.session[i].slow_talk = slow_talk;
                if (IsCallActive(&voice_.session[i])) {
                    pal_stream_set_param(voice_.session[i].pal_voice_handle,
                                         PAL_PARAM_ID_SLOW_TALK, params);
                }
            }
            free(params);
            params = nullptr;
        }
    }
    err = str_parms_get_str(parms, AUDIO_PARAMETER_KEY_HD_VOICE, c_value, sizeof(c_value));
    if (err >= 0) {
        if (strcmp(c_value, "true") == 0)
            hd_voice = true;
        else if (strcmp(c_value, "false") == 0) {
            hd_voice = false;
        }
        else {
            ret = -EINVAL;
            goto done;
        }
        params = (pal_param_payload *)calloc(1, sizeof(pal_param_payload) +
                                                sizeof(hd_voice));
        if (!params) {
            AHAL_ERR("calloc failed for size %zu",
                     sizeof(pal_param_payload) + sizeof(hd_voice));
        } else {
            params->payload_size = sizeof(hd_voice);
            params->payload[0] = hd_voice;

            for ( i = 0; i < MAX_VOICE_SESSIONS; i++) {
                voice_.session[i].hd_voice = hd_voice;
                if (IsCallActive(&voice_.session[i])) {
                    pal_stream_set_param(voice_.session[i].pal_voice_handle,
                                         PAL_PARAM_ID_HD_VOICE, params);
                }
            }
            free(params);
            params = nullptr;
        }
    }
    err = str_parms_get_str(parms, AUDIO_PARAMETER_KEY_DEVICE_MUTE, c_value,
                            sizeof(c_value));
    if (err >= 0) {
        bool mute = false;
        pal_stream_direction_t dir = PAL_AUDIO_INPUT;
        str_parms_del(parms, AUDIO_PARAMETER_KEY_DEVICE_MUTE);

        if (strcmp(c_value, "true") == 0) {
            mute = true;
        }

        err = str_parms_get_str(parms, AUDIO_PARAMETER_KEY_DIRECTION, c_value,
                                sizeof(c_value));
        if (err >= 0) {
            str_parms_del(parms, AUDIO_PARAMETER_KEY_DIRECTION);

            if (strcmp(c_value, "rx") == 0){
                dir = PAL_AUDIO_OUTPUT;
            }
        } else {
            AHAL_ERR("error direction key not found");
            ret = -EINVAL;
            goto done;
        }
        params = (pal_param_payload *)calloc(1, sizeof(pal_param_payload) +
                                                sizeof(pal_device_mute_t));
        if (!params) {
            AHAL_ERR("calloc failed for size %zu",
                     sizeof(pal_param_payload) + sizeof(pal_device_mute_t));
        } else {
            params->payload_size = sizeof(pal_device_mute_t);

            for ( i = 0; i < MAX_VOICE_SESSIONS; i++) {
                voice_.session[i].device_mute.mute = mute;
                voice_.session[i].device_mute.dir = dir;
                memcpy(params->payload, &(voice_.session[i].device_mute), params->payload_size);
                if (IsCallActive(&voice_.session[i])) {
                    ret= pal_stream_set_param(voice_.session[i].pal_voice_handle,
                                         PAL_PARAM_ID_DEVICE_MUTE, params);
                }
                if (ret != 0) {
                    AHAL_ERR("Failed to set mute err:%d", ret);
                    free(params);
                    params = nullptr;
                    ret = -EINVAL;
                    goto done;
                }
            }
            free(params);
            params = nullptr;
        }
    }
    err = str_parms_get_str(parms, AUDIO_PARAMETER_KEY_HAC, c_value, sizeof(c_value));
    if (err >= 0) {
        hac = false;
        if (strcmp(c_value, AUDIO_PARAMETER_VALUE_HAC_ON) == 0)
            hac = true;
        for ( i = 0; i < MAX_VOICE_SESSIONS; i++) {
            if (voice_.session[i].hac != hac) {
                voice_.session[i].hac = hac;
                if (IsCallActive(&voice_.session[i])) {
                    ret = VoiceSetDevice(&voice_.session[i]);
                }
            }
        }
    }

done:
    str_parms_destroy(parms);
    AHAL_DBG("Exit ret: %d", ret);
    ATRACE_END();
    return ret;
}

void AudioVoice::VoiceGetParameters(struct str_parms *query, struct str_parms *reply)
{
    uint32_t tty_mode = 0;
    int ret = 0;
    char value[256]={0};

    ret = str_parms_get_str(query, AUDIO_PARAMETER_KEY_TTY_MODE,
                            value, sizeof(value));
    if (ret >= 0) {
        for (int voiceSession_ind = 0; voiceSession_ind < MAX_VOICE_SESSIONS; voiceSession_ind++) {
            tty_mode = voice_.session[voiceSession_ind].tty_mode;
        }
        if (tty_mode >= PAL_TTY_OFF || tty_mode <= PAL_TTY_FULL) {
            switch(tty_mode) {
                case PAL_TTY_OFF:
                    str_parms_add_str(reply, AUDIO_PARAMETER_KEY_TTY_MODE, AUDIO_PARAMETER_VALUE_TTY_OFF);
                break;
               case PAL_TTY_VCO:
                    str_parms_add_str(reply, AUDIO_PARAMETER_KEY_TTY_MODE, AUDIO_PARAMETER_VALUE_TTY_VCO);
                break;
                case PAL_TTY_HCO:
                    str_parms_add_str(reply, AUDIO_PARAMETER_KEY_TTY_MODE, AUDIO_PARAMETER_VALUE_TTY_HCO);
                break;
                case PAL_TTY_FULL:
                    str_parms_add_str(reply, AUDIO_PARAMETER_KEY_TTY_MODE, AUDIO_PARAMETER_VALUE_TTY_FULL);
                break;
            }
        } else {
            AHAL_ERR("Error happened for getting TTY mode");
        }
    }
    ret = str_parms_get_str(query, AUDIO_PARAMETER_KEY_CRS_CALL_SUPPORTED, value, sizeof(value));
    if (ret >= 0) {
        str_parms_add_int(reply, AUDIO_PARAMETER_KEY_CRS_CALL_SUPPORTED, 1);
    }
    return;
}

bool AudioVoice::is_valid_vsid(uint32_t vsid)
{
    if (vsid == VOICEMMODE1_VSID ||
        vsid == VOICEMMODE2_VSID)
        return true;
    else
        return false;
}

bool AudioVoice::is_valid_call_state(int call_state)
{
    if (call_state < CALL_INACTIVE || call_state > CALL_ACTIVE)
        return false;
    else
        return true;
}

int AudioVoice::GetMatchingTxDevices(const std::set<audio_devices_t>& rx_devices,
                                     std::set<audio_devices_t>& tx_devices){
    std::shared_ptr<AudioDevice> adevice = AudioDevice::GetInstance();
    for(auto rx_dev : rx_devices)
        switch(rx_dev) {
            case AUDIO_DEVICE_OUT_EARPIECE:
                tx_devices.insert(AUDIO_DEVICE_IN_BUILTIN_MIC);
                break;
            case AUDIO_DEVICE_OUT_SPEAKER:
                tx_devices.insert(AUDIO_DEVICE_IN_BACK_MIC);
                break;
            case AUDIO_DEVICE_OUT_WIRED_HEADSET:
                tx_devices.insert(AUDIO_DEVICE_IN_WIRED_HEADSET);
                break;
            case AUDIO_DEVICE_OUT_LINE:
            case AUDIO_DEVICE_OUT_WIRED_HEADPHONE:
                tx_devices.insert(AUDIO_DEVICE_IN_BUILTIN_MIC);
                break;
            case AUDIO_DEVICE_OUT_USB_HEADSET:
            case AUDIO_DEVICE_OUT_USB_DEVICE:
                if (adevice->usb_input_dev_enabled)
                    tx_devices.insert(AUDIO_DEVICE_IN_USB_HEADSET);
                else
                    tx_devices.insert(AUDIO_DEVICE_IN_BUILTIN_MIC);
                break;
            case AUDIO_DEVICE_OUT_BLUETOOTH_SCO:
            case AUDIO_DEVICE_OUT_BLUETOOTH_SCO_HEADSET:
            case AUDIO_DEVICE_OUT_BLUETOOTH_SCO_CARKIT:
                tx_devices.insert(AUDIO_DEVICE_IN_BLUETOOTH_SCO_HEADSET);
                break;
            case AUDIO_DEVICE_OUT_BLE_HEADSET:
                tx_devices.insert(AUDIO_DEVICE_IN_BLE_HEADSET);
                break;
            case AUDIO_DEVICE_OUT_HEARING_AID:
                tx_devices.insert(AUDIO_DEVICE_IN_BUILTIN_MIC);
                break;
            default:
                tx_devices.insert(AUDIO_DEVICE_NONE);
                AHAL_ERR("unsupported Device Id of %d", rx_dev);
                break;
        }

    return tx_devices.size();
}

int AudioVoice::RouteStream(const std::set<audio_devices_t>& rx_devices) {
    int ret = 0;
    std::set<audio_devices_t> tx_devices;
    pal_device_id_t pal_rx_device = (pal_device_id_t) NULL;
    pal_device_id_t pal_tx_device = (pal_device_id_t) NULL;
    pal_device_id_t* pal_device_ids = NULL;
    uint16_t device_count = 0;

    pal_param_bta2dp_t *param_bt_a2dp_ptr = nullptr;
    size_t bt_param_size = 0;
    bool a2dp_suspended = false;
    bool a2dp_capture_suspended = false;
    const int retry_period_ms = 100;
    bool is_suspend_setparam = false;

    AHAL_DBG("Enter");

    if (AudioExtn::audio_devices_empty(rx_devices) || rx_devices.size() > 1){
        AHAL_ERR("invalid routing device %d", AudioExtn::get_device_types(rx_devices));
        goto exit;
    }

    GetMatchingTxDevices(rx_devices, tx_devices);

    /**
     * if device_none is in Tx/Rx devices,
     * which is invalid, teardown the usecase.
     */
    if (tx_devices.find(AUDIO_DEVICE_NONE) != tx_devices.end() ||
        rx_devices.find(AUDIO_DEVICE_NONE) != rx_devices.end()) {
        AHAL_ERR("Invalid Tx/Rx device");
        ret = 0;
        goto exit;
    }

    device_count = tx_devices.size() > rx_devices.size() ? tx_devices.size() : rx_devices.size();

    pal_device_ids = (pal_device_id_t *)calloc(1, device_count * sizeof(pal_device_id_t));
    if (!pal_device_ids) {
        AHAL_ERR("fail to allocate memory for pal device array");
        ret = -ENOMEM;
        goto exit;
    }

    AHAL_DBG("Routing is %d", AudioExtn::get_device_types(rx_devices));

    if (stream_out_primary_) {
        stream_out_primary_->getPalDeviceIds(rx_devices, pal_device_ids);
        pal_rx_device = pal_device_ids[0];
        memset(pal_device_ids, 0, device_count * sizeof(pal_device_id_t));
        stream_out_primary_->getPalDeviceIds(tx_devices, pal_device_ids);
        pal_tx_device = pal_device_ids[0];
    }

    if (voice_.crsCall &&
        (pal_voice_rx_device_id_ == pal_rx_device)) {
        AHAL_DBG("same Rx device in crs call");
        goto exit;
    }

    pal_voice_rx_device_id_ = pal_rx_device;
    pal_voice_tx_device_id_ = pal_tx_device;

    voice_mutex_.lock();
    if (!IsAnyCallActive()) {
        if (mode_ == AUDIO_MODE_IN_CALL || mode_ == AUDIO_MODE_CALL_SCREEN || voice_.crsCall) {
            voice_.in_call = true;
            ret = UpdateCalls(voice_.session);
        }
    } else {
        // do device switch here
        for (int i = 0; i < MAX_VOICE_SESSIONS; i++) {
            /*In case of active LEA profile, if voice call accepted by an inactive legacy headset
            * over SCO profile. APM is not aware about SCO active profile until BT_SCO=ON
            * event triggers from BT. In meantime before BT_SCO=ON, when LEA is suspended via
            * setparam call, APM tries to route voice call to BLE device.
            * In RouteStream call, if suspended state for LEA is true it keep checks over a
            * sleep of 2 secs. This causes timecheck issue in audioservice. Thus check for
            * is_suspend_setparam flag to know whether BLE suspended due to the actual setparam
            * or reconfig_cb(suspend->resume).
            */
            int retry_cnt = 20;
            if ((pal_voice_rx_device_id_ == PAL_DEVICE_OUT_BLUETOOTH_BLE) &&
                (pal_voice_tx_device_id_ == PAL_DEVICE_IN_BLUETOOTH_BLE)) {
                updateVoiceMetadataForBT(true);
                pal_param_bta2dp_t param_bt_a2dp;
                do {
                    std::unique_lock<std::mutex> guard(reconfig_wait_mutex_);
                    param_bt_a2dp_ptr = &param_bt_a2dp;
                    param_bt_a2dp_ptr->dev_id = PAL_DEVICE_OUT_BLUETOOTH_BLE;

                    ret = pal_get_param(PAL_PARAM_ID_BT_A2DP_SUSPENDED,
                                        (void **)&param_bt_a2dp_ptr, &bt_param_size, nullptr);
                    if (!ret && bt_param_size && param_bt_a2dp_ptr) {
                        a2dp_suspended = param_bt_a2dp_ptr->a2dp_suspended;
                        is_suspend_setparam = param_bt_a2dp_ptr->is_suspend_setparam;
                    } else {
                        AHAL_ERR("getparam for PAL_PARAM_ID_BT_A2DP_SUSPENDED failed");
                    }
                    param_bt_a2dp_ptr = &param_bt_a2dp;
                    param_bt_a2dp_ptr->dev_id = PAL_DEVICE_IN_BLUETOOTH_BLE;
                    bt_param_size = 0;
                    ret = pal_get_param(PAL_PARAM_ID_BT_A2DP_CAPTURE_SUSPENDED,
                                        (void **)&param_bt_a2dp_ptr, &bt_param_size, nullptr);
                    if (!ret && bt_param_size && param_bt_a2dp_ptr)
                        a2dp_capture_suspended = param_bt_a2dp_ptr->a2dp_capture_suspended;
                    else
                        AHAL_ERR("getparam for BT_A2DP_CAPTURE_SUSPENDED failed");
                    param_bt_a2dp_ptr = nullptr;
                    bt_param_size = 0;
                } while (!is_suspend_setparam && (a2dp_suspended || a2dp_capture_suspended) &&
                    --retry_cnt && !usleep(retry_period_ms * 1000));
                AHAL_INFO("a2dp_suspended status %d and a2dp_capture_suspended status %d",
                       a2dp_suspended, a2dp_capture_suspended);
            }

            // dont start the call, if suspend is in progress for BLE
            std::unique_lock<std::mutex> guard(reconfig_wait_mutex_);
            ret = VoiceSetDevice(&voice_.session[i]);
            if (ret)
                AHAL_ERR("Device switch failed for session[%d]", i);
        }
    }
    voice_mutex_.unlock();

    free(pal_device_ids);
exit:
    AHAL_DBG("Exit ret: %d", ret);
    return ret;
}

bool AudioVoice::get_voice_call_state(audio_mode_t *mode) {
    int i, ret = 0;
    *mode = mode_;
    for (i = 0; i < MAX_VOICE_SESSIONS; i++) {
        if (voice_.session[i].state.current_ == CALL_ACTIVE) {
            return true;
        }
    }
    return false;
}

int AudioVoice::UpdateCallState(uint32_t vsid, int call_state) {
    voice_session_t *session = NULL;
    int i, ret = 0;
    bool is_call_active;


    for (i = 0; i < MAX_VOICE_SESSIONS; i++) {
        if (vsid == voice_.session[i].vsid) {
            session = &voice_.session[i];
            break;
        }
    }

    voice_mutex_.lock();
    if (session) {
        session->state.new_ = call_state;
        is_call_active = IsCallActive(session);
        AHAL_DBG("is_call_active:%d in_call:%d, mode:%d",
                 is_call_active, voice_.in_call, mode_);
        if (is_call_active ||
            (voice_.in_call && (mode_ == AUDIO_MODE_IN_CALL || mode_ == AUDIO_MODE_CALL_SCREEN)) ||
            (voice_.crsCall && mode_ == AUDIO_MODE_RINGTONE && session->vsid == voice_.crsVsid)) {
            ret = UpdateCalls(voice_.session);
        }
    } else {
        ret = -EINVAL;
    }
    voice_mutex_.unlock();

    return ret;
}

int AudioVoice::UpdateCalls(voice_session_t *pSession) {
    int i, ret = 0;
    voice_session_t *session = NULL;
    ATRACE_BEGIN("hal: UpdateCalls");
    pal_param_bta2dp_t *param_bt_a2dp_ptr = nullptr;
    size_t bt_param_size = 0;
    bool a2dp_suspended = false;
    bool a2dp_capture_suspended = false;
    const int retry_period_ms = 100;
    bool is_suspend_setparam = false;

    for (i = 0; i < MAX_VOICE_SESSIONS; i++) {
        int retry_cnt = 20;
        session = &pSession[i];
        AHAL_DBG("cur_state=%d new_state=%d vsid=%x",
                 session->state.current_, session->state.new_, session->vsid);

        if (voice_.crsCall && session->vsid == voice_.crsVsid) {
            AHAL_DBG("CRS Update call state");
            if (session->state.current_ == CALL_ACTIVE)
                session->state.new_ = CALL_INACTIVE;
            else
                session->state.new_ = CALL_ACTIVE;
        }

        switch(session->state.new_)
        {
        case CALL_ACTIVE:
            switch(session->state.current_)
            {
            case CALL_INACTIVE:
                AHAL_DBG("INACTIVE -> ACTIVE vsid:%x", session->vsid);
                {
                    if ((pal_voice_rx_device_id_ == PAL_DEVICE_OUT_BLUETOOTH_BLE) &&
                        (pal_voice_tx_device_id_ == PAL_DEVICE_IN_BLUETOOTH_BLE)) {
                        updateVoiceMetadataForBT(true);
                        pal_param_bta2dp_t param_bt_a2dp;
                        do {
                            std::unique_lock<std::mutex> guard(reconfig_wait_mutex_);
                            param_bt_a2dp_ptr = &param_bt_a2dp;
                            param_bt_a2dp_ptr->dev_id = PAL_DEVICE_OUT_BLUETOOTH_BLE;

                            ret = pal_get_param(PAL_PARAM_ID_BT_A2DP_SUSPENDED,
                                                (void **)&param_bt_a2dp_ptr, &bt_param_size, nullptr);
                            if (!ret && bt_param_size && param_bt_a2dp_ptr) {
                                a2dp_suspended = param_bt_a2dp_ptr->a2dp_suspended;
                                is_suspend_setparam = param_bt_a2dp_ptr->is_suspend_setparam;
                            } else {
                                AHAL_ERR("getparam for PAL_PARAM_ID_BT_A2DP_SUSPENDED failed");
                            }
                            param_bt_a2dp_ptr = &param_bt_a2dp;
                            param_bt_a2dp_ptr->dev_id = PAL_DEVICE_IN_BLUETOOTH_BLE;
                            bt_param_size = 0;
                            ret = pal_get_param(PAL_PARAM_ID_BT_A2DP_CAPTURE_SUSPENDED,
                                                (void **)&param_bt_a2dp_ptr, &bt_param_size, nullptr);
                            if (!ret && bt_param_size && param_bt_a2dp_ptr) {
                                a2dp_capture_suspended = param_bt_a2dp_ptr->a2dp_capture_suspended;
                            } else {
                                AHAL_ERR("getparam for BT_A2DP_CAPTURE_SUSPENDED failed");
                            }
                            param_bt_a2dp_ptr = nullptr;
                            bt_param_size = 0;
                        } while (!is_suspend_setparam && (a2dp_suspended || a2dp_capture_suspended)
                            && --retry_cnt && !usleep(retry_period_ms * 1000));
                        AHAL_INFO("a2dp_suspended status %d and a2dp_capture_suspended status %d",
                                  a2dp_suspended, a2dp_capture_suspended);
                    }

                    // dont start the call, if suspend is in progress for BLE
                    std::unique_lock<std::mutex> guard(reconfig_wait_mutex_);

                    if (!IsAnyCallActive()) {
                        ret = VoiceStart(session);
                        if (ret < 0) {
                            AHAL_ERR("VoiceStart() failed");
                        } else {
                            voice_.in_call = true;
                            session->state.current_ = session->state.new_;
                        }
                    } else {
                         AHAL_INFO("Voice already started");
                    }
                }
                break;
            default:
                AHAL_ERR("CALL_ACTIVE cannot be handled in state=%d vsid:%x",
                          session->state.current_, session->vsid);
                break;
            }
            break;

        case CALL_INACTIVE:
            switch(session->state.current_)
            {
            case CALL_ACTIVE:
                AHAL_DBG("ACTIVE -> INACTIVE vsid:%x", session->vsid);
                ret = VoiceStop(session);
                if (ret < 0) {
                    AHAL_ERR("VoiceStop() failed");
                } else {
                    session->state.current_ = session->state.new_;
                }
                if ((pal_voice_rx_device_id_ == PAL_DEVICE_OUT_BLUETOOTH_BLE) &&
                    (pal_voice_tx_device_id_ == PAL_DEVICE_IN_BLUETOOTH_BLE)) {
                        AHAL_DBG("ACTIVE -> INACTIVE update cached meta data");
                        updateVoiceMetadataForBT(false);
                    }
                break;

            default:
                AHAL_ERR("CALL_INACTIVE cannot be handled in state=%d vsid:%x",
                         session->state.current_, session->vsid);
                break;
            }
            break;
        default:
            break;
        } //end out switch loop
    } //end for loop
    ATRACE_END();
    return ret;
}

int AudioVoice::StopCall() {
    int i, ret = 0;

    AHAL_DBG("Enter");
    voice_.in_call = false;
    for (i = 0; i < MAX_VOICE_SESSIONS; i++)
        voice_.session[i].state.new_ = CALL_INACTIVE;
    ret = UpdateCalls(voice_.session);
    AHAL_DBG("Exit ret: %d", ret);
    return ret;
}

bool AudioVoice::IsCallActive(AudioVoice::voice_session_t *pSession) {

    return (pSession->state.current_ != CALL_INACTIVE) ? true : false;
}

bool AudioVoice::IsAnyCallActive()
{
    int i;

    for (i = 0; i < MAX_VOICE_SESSIONS; i++) {
        if (IsCallActive(&voice_.session[i]))
            return true;
    }

    return false;
}

int AudioVoice::VoiceStart(voice_session_t *session) {
    int ret;
    struct pal_stream_attributes streamAttributes;
    std::shared_ptr<AudioDevice> adevice = AudioDevice::GetInstance();
    struct pal_device palDevices[2];
    struct pal_channel_info out_ch_info = {0, {0}}, in_ch_info = {0, {0}};
    pal_param_payload *param_payload = nullptr;
    ATRACE_BEGIN("hal: VoiceStart");
    if (!session) {
        AHAL_ERR("Invalid session");
        return -EINVAL;
    }

    AHAL_DBG("Enter");

    in_ch_info.channels = 1;
    in_ch_info.ch_map[0] = PAL_CHMAP_CHANNEL_FL;

    out_ch_info.channels = 2;
    out_ch_info.ch_map[0] = PAL_CHMAP_CHANNEL_FL;
    out_ch_info.ch_map[1] = PAL_CHMAP_CHANNEL_FR;

    if (voice_.crsCall) {
        if (voice_.crsLoopback &&
            (adevice->crs_device.size() == 0 || pal_voice_rx_device_id_ == PAL_DEVICE_OUT_HANDSET)) {
             AHAL_DBG("No plugin device, use speaker by default in CRS call");
             pal_voice_rx_device_id_ = PAL_DEVICE_OUT_SPEAKER;
             pal_voice_tx_device_id_ = PAL_DEVICE_IN_SPEAKER_MIC;
        }
    }


    if (voice_.crsCall)
       SetVoiceVolume(voice_.crsVol);

    palDevices[0].id = pal_voice_tx_device_id_;
    palDevices[0].config.ch_info = in_ch_info;
    palDevices[0].config.sample_rate = 48000;
    palDevices[0].config.bit_width = CODEC_BACKEND_DEFAULT_BIT_WIDTH;
    palDevices[0].config.aud_fmt_id = PAL_AUDIO_FMT_PCM_S16_LE; // TODO: need to convert this from output format
    palDevices[0].address.card_id = adevice->usb_card_id_;
    palDevices[0].address.device_num =adevice->usb_dev_num_;

    palDevices[1].id = pal_voice_rx_device_id_;
    palDevices[1].config.ch_info = out_ch_info;
    palDevices[1].config.sample_rate = 48000;
    palDevices[1].config.bit_width = CODEC_BACKEND_DEFAULT_BIT_WIDTH;
    palDevices[1].config.aud_fmt_id = PAL_AUDIO_FMT_PCM_S16_LE; // TODO: need to convert this from output format
    palDevices[1].address.card_id = adevice->usb_card_id_;
    palDevices[1].address.device_num = adevice->usb_dev_num_;

    memset(&streamAttributes, 0, sizeof(streamAttributes));
    streamAttributes.type = PAL_STREAM_VOICE_CALL;
    streamAttributes.info.voice_call_info.VSID = session->vsid;
    streamAttributes.info.voice_call_info.tty_mode = session->tty_mode;
    /*device overrides for specific use cases*/
    if (mode_ == AUDIO_MODE_CALL_SCREEN) {
        AHAL_DBG("in call screen mode");
        palDevices[0].id = PAL_DEVICE_IN_PROXY;  //overwrite the device with proxy dev
        palDevices[1].id = PAL_DEVICE_OUT_PROXY;  //overwrite the device with proxy dev
    }
    if (streamAttributes.info.voice_call_info.tty_mode == PAL_TTY_HCO) {
        /**  device pairs for HCO usecase
          *  <handset, headset-mic>
          *  <handset, usb-headset-mic>
          *  <speaker, headset-mic>
          *  <speaker, usb-headset-mic>
          *  override devices accordingly.
          */
        if (pal_voice_rx_device_id_ == PAL_DEVICE_OUT_WIRED_HEADSET ||
            (pal_voice_rx_device_id_ == PAL_DEVICE_OUT_USB_HEADSET &&
                        adevice->usb_input_dev_enabled))
            palDevices[1].id = PAL_DEVICE_OUT_HANDSET;
        else if (pal_voice_rx_device_id_ == PAL_DEVICE_OUT_SPEAKER) {
            if (adevice->usb_out_headset) {
               if (adevice->usb_input_dev_enabled) {
                  palDevices[0].id = PAL_DEVICE_IN_USB_HEADSET;
               } else {
                 AHAL_DBG("3-pole USB Headset connected.No MIC");
                 palDevices[0].id = PAL_DEVICE_IN_SPEAKER_MIC;
                 AHAL_DBG("tty_hco_mode:Device pairs:Speaker-Speaker_mic");
               }
            } else {
                /*Need to add support for 3-pole Wired Headset */
                 palDevices[0].id = PAL_DEVICE_IN_WIRED_HEADSET;
            }
        }
        else {
            AHAL_ERR("Invalid device pair for the usecase");
        }
    }
    if (streamAttributes.info.voice_call_info.tty_mode == PAL_TTY_VCO) {
        /**  device pairs for VCO usecase
          *  <headphones, handset-mic>
          *  <usb-headset, handset-mic>
          *  <headphones, speaker-mic>
          *  <usb-headset, speaker-mic>
          *  override devices accordingly.
          */
        if (pal_voice_rx_device_id_ == PAL_DEVICE_OUT_WIRED_HEADSET ||
            pal_voice_rx_device_id_ == PAL_DEVICE_OUT_WIRED_HEADPHONE ||
            pal_voice_rx_device_id_ == PAL_DEVICE_OUT_USB_HEADSET)
            palDevices[0].id = PAL_DEVICE_IN_HANDSET_MIC;
        else if (pal_voice_rx_device_id_ == PAL_DEVICE_OUT_SPEAKER) {
            if (adevice->usb_out_headset) {
               if (adevice->usb_input_dev_enabled) {
                   palDevices[0].id = PAL_DEVICE_IN_USB_HEADSET;
               }
               else {
                   AHAL_DBG("3-pole USB Headset connected.No MIC");
                   palDevices[0].id = PAL_DEVICE_IN_SPEAKER_MIC;
                   AHAL_DBG("tty_vco_mode:Device pairs:Speaker-Speaker_mic");
               }
            } else {
                /*Need to add support for 3-pole Wired Headset */
                palDevices[1].id = PAL_DEVICE_OUT_WIRED_HEADSET;
            }
        }
        else {
            AHAL_ERR("Invalid device pair for the usecase");
        }
    }
    streamAttributes.direction = PAL_AUDIO_INPUT_OUTPUT;
    streamAttributes.in_media_config.sample_rate = 48000;
    streamAttributes.in_media_config.ch_info = in_ch_info;
    streamAttributes.in_media_config.bit_width = CODEC_BACKEND_DEFAULT_BIT_WIDTH;
    streamAttributes.in_media_config.aud_fmt_id = PAL_AUDIO_FMT_PCM_S16_LE; // TODO: need to convert this from output format
    streamAttributes.out_media_config.sample_rate = 48000;
    streamAttributes.out_media_config.ch_info = out_ch_info;
    streamAttributes.out_media_config.bit_width = CODEC_BACKEND_DEFAULT_BIT_WIDTH;
    streamAttributes.out_media_config.aud_fmt_id = PAL_AUDIO_FMT_PCM_S16_LE; // TODO: need to convert this from output format

    /*set custom key for hac mode*/
    if (session && session->hac && palDevices[1].id ==
        PAL_DEVICE_OUT_HANDSET) {
        strlcpy(palDevices[0].custom_config.custom_key, "HAC",
                    sizeof(palDevices[0].custom_config.custom_key));
        strlcpy(palDevices[1].custom_config.custom_key, "HAC",
                    sizeof(palDevices[1].custom_config.custom_key));
        AHAL_INFO("Setting custom key as %s", palDevices[0].custom_config.custom_key);
    }

    /*set custom key for crsCall mode to force ringtone to play in stereo speaker and allow SVA concurrency*/
    if (voice_.crsCall) {
        strlcpy(palDevices[1].custom_config.custom_key, "crsCall",
                  sizeof(palDevices[1].custom_config.custom_key));
        AHAL_INFO("Setting custom key as %s", palDevices[1].custom_config.custom_key);
    }

    //streamAttributes.in_media_config.ch_info = ch_info;
    ret = pal_stream_open(&streamAttributes,
                          2,
                          palDevices,
                          0,
                          NULL,
                          NULL,//callback
                          (uint64_t)this,
                          &session->pal_voice_handle);// Need to add this to the audio stream structure.

    AHAL_DBG("pal_stream_open() ret:%d", ret);
    if (ret) {
        AHAL_ERR("Pal Stream Open Error (%x)", ret);
        ret = -EINVAL;
        goto error_open;
    }

    if (voice_.crsCall && voice_.crsLoopback &&
        palDevices[1].id != PAL_DEVICE_OUT_SPEAKER) {
        AHAL_DBG("CRS Setup looback device");
        palDevices[0].id = PAL_DEVICE_OUT_SPEAKER;
        palDevices[0].config.ch_info = out_ch_info;
        palDevices[0].config.sample_rate = 48000;
        palDevices[0].config.bit_width = CODEC_BACKEND_DEFAULT_BIT_WIDTH;
        palDevices[0].config.aud_fmt_id = PAL_AUDIO_FMT_PCM_S16_LE;

        streamAttributes.type = PAL_STREAM_LOOPBACK;
        streamAttributes.direction = PAL_AUDIO_OUTPUT;
        streamAttributes.info.opt_stream_info.loopback_type = PAL_STREAM_LOOPBACK_PLAYBACK_ONLY;

        ret = pal_stream_open(&streamAttributes,
                          1,
                          palDevices,
                          0,
                          NULL,
                          NULL,
                          (uint64_t)this,
                          &session->pal_voice_loopback_handle);

        AHAL_DBG("pal_stream_open() for loopback device ret:%d", ret);
        if (ret) {
            AHAL_ERR("Pal Stream Open Error for loopback device (%x)", ret);
            ret = -EINVAL;
            goto error_open;
        }
    }

    /*apply cached voice effects features*/
    if (session->slow_talk) {
        param_payload = (pal_param_payload *)calloc(1, sizeof(pal_param_payload) +
                                             sizeof(session->slow_talk));
        if (!param_payload) {
            AHAL_ERR("calloc for size %zu failed",
                   sizeof(pal_param_payload) + sizeof(session->slow_talk));
        } else {
            param_payload->payload_size = sizeof(session->slow_talk);
            param_payload->payload[0] = session->slow_talk;
            ret = pal_stream_set_param(session->pal_voice_handle,
                                       PAL_PARAM_ID_SLOW_TALK,
                                       param_payload);
            if (ret)
                AHAL_ERR("Slow Talk enable failed %x", ret);
            free(param_payload);
            param_payload = nullptr;
        }
    }

    if (session->volume_boost) {
        param_payload = (pal_param_payload *)calloc(1, sizeof(pal_param_payload) +
                                             sizeof(session->volume_boost));
        if (!param_payload) {
            AHAL_ERR("calloc for size %zu failed",
                  sizeof(pal_param_payload) + sizeof(session->volume_boost));
        } else {
            param_payload->payload_size = sizeof(session->volume_boost);
            param_payload->payload[0] = session->volume_boost;
            ret = pal_stream_set_param(session->pal_voice_handle, PAL_PARAM_ID_VOLUME_BOOST,
                                   param_payload);
            if (ret)
                AHAL_ERR("Volume Boost enable failed %x", ret);
            free(param_payload);
            param_payload = nullptr;
        }
    }

    if (session->hd_voice) {
        param_payload = (pal_param_payload *)calloc(1, sizeof(pal_param_payload) +
                                             sizeof(session->hd_voice));
        if (!param_payload) {
            AHAL_ERR("calloc for size %zu failed",
                     sizeof(pal_param_payload) + sizeof(session->hd_voice));
        } else {
            param_payload->payload_size = sizeof(session->hd_voice);
            param_payload->payload[0] = session->hd_voice;
            ret = pal_stream_set_param(session->pal_voice_handle, PAL_PARAM_ID_HD_VOICE,
                                   param_payload);
            if (ret)
                AHAL_ERR("HD Voice enable failed %x",ret);
            free(param_payload);
            param_payload = nullptr;
        }
    }

    /* apply cached volume set by APM */
    if (voice_.crsCall) {
        if (session->pal_voice_handle && session->pal_vol_crs_data &&
            session->pal_vol_crs_data->volume_pair[0].vol != -1.0) {
            ret = pal_stream_set_volume(session->pal_voice_handle, session->pal_vol_crs_data);
            if (ret)
               AHAL_ERR("Failed to apply volume on crs session %x", ret);
        } else {
            if (!session->pal_voice_handle || !session->pal_vol_crs_data)
               AHAL_ERR("Invalid voice handle or crs volume data");
            if (session->pal_vol_crs_data && session->pal_vol_crs_data->volume_pair[0].vol == -1.0)
               AHAL_DBG("crs session volume is not set");
        }
    } else if (session->pal_voice_handle && session->pal_vol_data &&
        session->pal_vol_data->volume_pair[0].vol != -1.0) {
        ret = pal_stream_set_volume(session->pal_voice_handle, session->pal_vol_data);
        if (ret)
            AHAL_ERR("Failed to apply volume on voice session %x", ret);
    } else {
        if (!session->pal_voice_handle || !session->pal_vol_data)
            AHAL_ERR("Invalid voice handle or volume data");
        if (session->pal_vol_data && session->pal_vol_data->volume_pair[0].vol == -1.0)
            AHAL_DBG("session volume is not set");
    }

   ret = pal_stream_start(session->pal_voice_handle);
   if (ret) {
       AHAL_ERR("Pal Stream Start Error (%x)", ret);
       ret = pal_stream_close(session->pal_voice_handle);
       if (ret)
           AHAL_ERR("Pal Stream close failed %x", ret);
           session->pal_voice_handle = NULL;
           ret = -EINVAL;
   } else {
      AHAL_DBG("Pal Stream Start Success");
   }

   if (voice_.crsCall && session->pal_voice_loopback_handle) {
        ret = pal_stream_start(session->pal_voice_loopback_handle);
        if (ret) {
            AHAL_ERR("Pal Stream Start Error for loopback device (%x)", ret);
            ret = pal_stream_close(session->pal_voice_loopback_handle);
            if (ret)
                AHAL_ERR("Pal Stream close failed for loopback device %x", ret);
            session->pal_voice_loopback_handle = NULL;
            ret = -EINVAL;
        } else {
            AHAL_DBG("Pal Stream Start Success for loopback device");
        }
    }

   /*Apply device mute if needed*/
   if (session->device_mute.mute) {
        ret = SetDeviceMute(session);
   }
   if (voice_.crsCall) {
        AHAL_DBG("CRS usecase mute TX");
       pal_stream_set_mute(session->pal_voice_handle, true);
   }

    /*apply cached mic mute*/
   if (adevice->mute_) {
       pal_stream_set_mute(session->pal_voice_handle, adevice->mute_);
   }

error_open:
    AHAL_DBG("Exit ret: %d", ret);
    ATRACE_END();
    return ret;
}

int AudioVoice::SetDeviceMute(voice_session_t *session) {
    int ret = 0;
    pal_param_payload *param_payload = nullptr;

    if (!session) {
        AHAL_ERR("Invalid Session");
        return -EINVAL;
    }

    param_payload = (pal_param_payload *)calloc(1, sizeof(pal_param_payload) +
                        sizeof(session->device_mute));
    if (!param_payload) {
        AHAL_ERR("calloc failed for size %zu",
                sizeof(pal_param_payload) + sizeof(session->device_mute));
        ret = -EINVAL;
    } else {
        param_payload->payload_size = sizeof(session->device_mute);
        memcpy(param_payload->payload, &(session->device_mute), param_payload->payload_size);
        ret = pal_stream_set_param(session->pal_voice_handle, PAL_PARAM_ID_DEVICE_MUTE,
                                param_payload);
        if (ret)
            AHAL_ERR("Voice Device mute failed %x", ret);
        free(param_payload);
        param_payload = nullptr;
    }

   AHAL_DBG("Exit ret: %d", ret);
   return ret;
}

int AudioVoice::VoiceStop(voice_session_t *session) {
    int ret = 0;

    AHAL_DBG("Enter");
    if (session && session->pal_voice_handle) {
        if (voice_.crsCall && session->pal_voice_loopback_handle) {
            ret = pal_stream_stop(session->pal_voice_loopback_handle);
            if (ret)
                AHAL_ERR("Pal Stream stop failed for loopback %x", ret);
        }
        ret = pal_stream_stop(session->pal_voice_handle);
        if (ret)
            AHAL_ERR("Pal Stream stop failed %x", ret);

        if (voice_.crsCall && session->pal_voice_loopback_handle) {
            ret = pal_stream_close(session->pal_voice_loopback_handle);
            if (ret)
                AHAL_ERR("Pal Stream close failed for loopback %x", ret);
        }
        ret = pal_stream_close(session->pal_voice_handle);
        if (ret)
            AHAL_ERR("Pal Stream close failed %x", ret);
        session->pal_voice_handle = NULL;

        if (voice_.crsCall) {
            session->pal_voice_loopback_handle = NULL;
            voice_.crsCall = false;
            voice_.in_call = false;
            voice_.crsLoopback = true;
        }
    }

    if (ret)
        ret = -EINVAL;
    AHAL_DBG("Exit ret: %d", ret);
    return ret;
}

int AudioVoice::VoiceSetDevice(voice_session_t *session) {
    int ret = 0;
    struct pal_device palDevices[2];
    struct pal_channel_info out_ch_info = {0, {0}}, in_ch_info = {0, {0}};
    std::shared_ptr<AudioDevice> adevice = AudioDevice::GetInstance();
    pal_param_payload *param_payload = nullptr;
    struct pal_stream_attributes streamAttributes;

    if (!session) {
        AHAL_ERR("Invalid session");
        return -EINVAL;
    }

    AHAL_DBG("Enter");
    in_ch_info.channels = 1;
    in_ch_info.ch_map[0] = PAL_CHMAP_CHANNEL_FL;

    out_ch_info.channels = 2;
    out_ch_info.ch_map[0] = PAL_CHMAP_CHANNEL_FL;
    out_ch_info.ch_map[1] = PAL_CHMAP_CHANNEL_FR;

    if (voice_.crsCall) {
        if (voice_.crsLoopback && pal_voice_rx_device_id_ == PAL_DEVICE_OUT_HANDSET) {
            AHAL_DBG("CRS force handset to speaker");
            pal_voice_rx_device_id_ = PAL_DEVICE_OUT_SPEAKER;
            pal_voice_tx_device_id_ = PAL_DEVICE_IN_SPEAKER_MIC;
        }
    }

    if (session->pal_voice_loopback_handle) {
        AHAL_DBG("CRS teardown for device switch");
        ret = pal_stream_stop(session->pal_voice_loopback_handle);
        if (ret)
            AHAL_ERR("Pal Stream stop failed for loopback %x", ret);
        ret = pal_stream_close(session->pal_voice_loopback_handle);
        if (ret)
            AHAL_ERR("Pal Stream close failed for loopback %x", ret);
        session->pal_voice_loopback_handle = NULL;
    }
    if (voice_.crsCall)
       SetVoiceVolume(voice_.crsVol);

    palDevices[0].id = pal_voice_tx_device_id_;
    palDevices[0].config.ch_info = in_ch_info;
    palDevices[0].config.sample_rate = 48000;
    palDevices[0].config.bit_width = CODEC_BACKEND_DEFAULT_BIT_WIDTH;
    palDevices[0].config.aud_fmt_id = PAL_AUDIO_FMT_PCM_S16_LE; // TODO: need to convert this from output format
    palDevices[0].address.card_id = adevice->usb_card_id_;
    palDevices[0].address.device_num =adevice->usb_dev_num_;

    palDevices[1].id = pal_voice_rx_device_id_;
    palDevices[1].config.ch_info = out_ch_info;
    palDevices[1].config.sample_rate = 48000;
    palDevices[1].config.bit_width = CODEC_BACKEND_DEFAULT_BIT_WIDTH;
    palDevices[1].config.aud_fmt_id = PAL_AUDIO_FMT_PCM_S16_LE; // TODO: need to convert this from output format
    palDevices[1].address.card_id = adevice->usb_card_id_;
    palDevices[1].address.device_num =adevice->usb_dev_num_;
    /*device overwrites for usecases*/
    if (mode_ == AUDIO_MODE_CALL_SCREEN) {
        AHAL_DBG("in call screen mode");
        palDevices[0].id = PAL_DEVICE_IN_PROXY;  //overwrite the device with proxy dev
        palDevices[1].id = PAL_DEVICE_OUT_PROXY;  //overwrite the device with proxy dev
    }

    if (session && session->tty_mode == PAL_TTY_HCO) {
        /**  device pairs for HCO usecase
          *  <handset, headset-mic>
          *  <handset, usb-headset-mic>
          *  <speaker, headset-mic>
          *  <speaker, usb-headset-mic>
          *  override devices accordingly.
          */
        if (pal_voice_rx_device_id_ == PAL_DEVICE_OUT_WIRED_HEADSET ||
            (pal_voice_rx_device_id_ == PAL_DEVICE_OUT_USB_HEADSET &&
                                     adevice->usb_input_dev_enabled))
            palDevices[1].id = PAL_DEVICE_OUT_HANDSET;
        else if (pal_voice_rx_device_id_ == PAL_DEVICE_OUT_SPEAKER) {
            if (adevice->usb_out_headset) {
               if (adevice->usb_input_dev_enabled) {
                   palDevices[0].id = PAL_DEVICE_IN_USB_HEADSET;
               }
               else {
                 AHAL_DBG("3-pole USB with No Mic Connected!!");
                 palDevices[0].id = PAL_DEVICE_IN_SPEAKER_MIC;
                 AHAL_DBG("tty_hco_mode:Device pairs:Speaker-Speaker_mic");
               }
            }
            else {
                 /* does not handle 3-pole wired headset */
                 palDevices[0].id = PAL_DEVICE_IN_WIRED_HEADSET;
            }
        }
        else {
            AHAL_ERR("Invalid device pair for the usecase");
        }
    }
    if (session && session->tty_mode == PAL_TTY_VCO) {
        /**  device pairs for VCO usecase
          *  <headphones, handset-mic>
          *  <usb-headset, handset-mic>
          *  <headphones, speaker-mic>
          *  <usb-headset, speaker-mic>
          *  override devices accordingly.
          */
        if (pal_voice_rx_device_id_ == PAL_DEVICE_OUT_WIRED_HEADSET ||
            pal_voice_rx_device_id_ == PAL_DEVICE_OUT_WIRED_HEADPHONE ||
            pal_voice_rx_device_id_ == PAL_DEVICE_OUT_USB_HEADSET)
            palDevices[0].id = PAL_DEVICE_IN_HANDSET_MIC;
        else if (pal_voice_rx_device_id_ == PAL_DEVICE_OUT_SPEAKER) {
            if (adevice->usb_out_headset) {
               if (adevice->usb_input_dev_enabled) {
                   palDevices[0].id = PAL_DEVICE_IN_USB_HEADSET;
               }
               else {
                   AHAL_DBG("3-pole USB Headset connected.No MIC");
                   palDevices[0].id = PAL_DEVICE_IN_SPEAKER_MIC;
                   AHAL_DBG("tty_vco_mode:Device pairs:Speaker-Speaker_mic");
               }
            } else {
                /* does not handle 3-pole wired headset */
                palDevices[1].id = PAL_DEVICE_OUT_WIRED_HEADSET;
            }
        }
        else {
            AHAL_ERR("Invalid device pair for the usecase");
        }
    }

    if (session && session->volume_boost) {
            /* volume boost if device is not supported */
            param_payload = (pal_param_payload *)calloc(1, sizeof(pal_param_payload) +
                                               sizeof(session->volume_boost));
            if (!param_payload) {
                AHAL_ERR("calloc for size %zu failed",
                     sizeof(pal_param_payload) + sizeof(session->volume_boost));
            } else {
                param_payload->payload_size = sizeof(session->volume_boost);
                if (palDevices[1].id != PAL_DEVICE_OUT_HANDSET &&
                    palDevices[1].id != PAL_DEVICE_OUT_SPEAKER)
                    param_payload->payload[0] = false;
                else
                    param_payload->payload[0] = true;
                ret = pal_stream_set_param(session->pal_voice_handle, PAL_PARAM_ID_VOLUME_BOOST,
                                           param_payload);
                if (ret)
                    AHAL_ERR("Volume Boost enable/disable failed %x", ret);
                free(param_payload);
                param_payload = nullptr;
            }
    }
    /*set or remove custom key for hac mode*/
    if (session && session->hac && palDevices[1].id ==
        PAL_DEVICE_OUT_HANDSET) {
        strlcpy(palDevices[0].custom_config.custom_key, "HAC",
                    sizeof(palDevices[0].custom_config.custom_key));
        strlcpy(palDevices[1].custom_config.custom_key, "HAC",
                    sizeof(palDevices[1].custom_config.custom_key));
            AHAL_INFO("Setting custom key as %s", palDevices[0].custom_config.custom_key);
    } else {
        strlcpy(palDevices[0].custom_config.custom_key, "",
                sizeof(palDevices[0].custom_config.custom_key));
    }

    /*set custom key for crsCall mode to force ringtone to play in stereo speaker and allow SVA concurrency*/
    if (voice_.crsCall) {
        strlcpy(palDevices[1].custom_config.custom_key, "crsCall",
                  sizeof(palDevices[1].custom_config.custom_key));
        AHAL_INFO("Setting custom key as %s", palDevices[1].custom_config.custom_key);
    }

    if (session && session->pal_voice_handle) {
        ret = pal_stream_set_device(session->pal_voice_handle, 2, palDevices);
        if (ret) {
            AHAL_ERR("Pal Stream Set Device failed %x", ret);
            ret = -EINVAL;
            goto exit;
        }
    } else {
        AHAL_ERR("Voice handle not found");
    }

    if (session && session->pal_voice_handle && voice_.crsLoopback &&
        voice_.crsCall && palDevices[1].id != PAL_DEVICE_OUT_SPEAKER && !session->pal_voice_loopback_handle) {
        AHAL_DBG("CRS Device switch: setup new device");
        palDevices[0].id = PAL_DEVICE_OUT_SPEAKER;
        palDevices[0].config.ch_info = out_ch_info;
        palDevices[0].config.sample_rate = 48000;
        palDevices[0].config.bit_width = CODEC_BACKEND_DEFAULT_BIT_WIDTH;
        palDevices[0].config.aud_fmt_id = PAL_AUDIO_FMT_PCM_S16_LE;

        memset(&streamAttributes, 0, sizeof(streamAttributes));
        streamAttributes.info.voice_call_info.VSID = session->vsid;
        streamAttributes.info.voice_call_info.tty_mode = session->tty_mode;
        streamAttributes.type = PAL_STREAM_LOOPBACK;
        streamAttributes.direction = PAL_AUDIO_OUTPUT;
        streamAttributes.info.opt_stream_info.loopback_type = PAL_STREAM_LOOPBACK_PLAYBACK_ONLY;
        streamAttributes.in_media_config.sample_rate = 48000;
        streamAttributes.in_media_config.ch_info = in_ch_info;
        streamAttributes.in_media_config.bit_width = CODEC_BACKEND_DEFAULT_BIT_WIDTH;
        streamAttributes.in_media_config.aud_fmt_id = PAL_AUDIO_FMT_PCM_S16_LE;
        streamAttributes.out_media_config.sample_rate = 48000;
        streamAttributes.out_media_config.ch_info = out_ch_info;
        streamAttributes.out_media_config.bit_width = CODEC_BACKEND_DEFAULT_BIT_WIDTH;
        streamAttributes.out_media_config.aud_fmt_id = PAL_AUDIO_FMT_PCM_S16_LE;

        ret = pal_stream_open(&streamAttributes,
                          1,
                          palDevices,
                          0,
                          NULL,
                          NULL,
                          (uint64_t)this,
                          &session->pal_voice_loopback_handle);

        AHAL_DBG("pal_stream_open() for loopback device ret:%d", ret);
        if (ret) {
            AHAL_ERR("Pal Stream Open Error for loopback device (%x)", ret);
            ret = -EINVAL;
            goto exit;
        }

        if (voice_.crsCall && session->pal_voice_loopback_handle) {
            ret = pal_stream_start(session->pal_voice_loopback_handle);
            if (ret) {
                AHAL_ERR("Pal Stream Start Error for loopback device (%x)", ret);
                ret = pal_stream_close(session->pal_voice_loopback_handle);
                if (ret)
                    AHAL_ERR("Pal Stream close failed for loopback device %x", ret);
                session->pal_voice_loopback_handle = NULL;
                ret = -EINVAL;
            }
        }
        else
            AHAL_DBG("Pal Stream Start Success for loopback device");
    }

    /* apply device mute if needed*/
    if (session->device_mute.mute) {
        ret = SetDeviceMute(session);
   }

exit:
    AHAL_DBG("Exit ret: %d", ret);
    return ret;
}

int AudioVoice::SetMicMute(bool mute) {
    int ret = 0;
    voice_session_t *session = voice_.session;

    AHAL_DBG("Enter mute: %d", mute);
    if (session) {
        for (int i = 0; i < MAX_VOICE_SESSIONS; i++) {
            if (session[i].pal_voice_handle) {
                ret = pal_stream_set_mute(session[i].pal_voice_handle, mute);
                if (ret)
                    AHAL_ERR("Error applying mute %d for voice session %d", mute, i);
            }
        }
    }
    AHAL_DBG("Exit ret: %d", ret);
    return ret;

}

int AudioVoice::SetVoiceVolume(float volume) {
    int ret = 0;
    voice_session_t *session = voice_.session;

    AHAL_DBG("Enter vol: %f", volume);
    if (session) {
        for (int i = 0; i < MAX_VOICE_SESSIONS; i++) {
            /* APM volume is cached when voice call is not active
             * cached volume is applied in voicestart before pal_stream_start
             */
            if (voice_.crsVsid || voice_.crsCall) {
                if (session[i].pal_vol_crs_data) {
                    session[i].pal_vol_crs_data->volume_pair[0].vol = voice_.crsVol;
                    if (session[i].pal_voice_handle) {
                        ret = pal_stream_set_volume(session[i].pal_voice_handle,
                                 session[i].pal_vol_crs_data);
                        AHAL_DBG("volume applied on crs session %d status %x", i, ret);
                    } else {
                        AHAL_DBG("volume is cached on crs session %d", i);
                    }
                }
            } else if (session[i].pal_vol_data) {
                session[i].pal_vol_data->volume_pair[0].vol = volume;
                if (session[i].pal_voice_handle) {
                    ret = pal_stream_set_volume(session[i].pal_voice_handle,
                            session[i].pal_vol_data);
                    AHAL_DBG("volume applied on voice session %d status %x", i, ret);
                } else {
                    AHAL_DBG("volume is cached on voice session %d", i);
                }
            } else {
                AHAL_ERR("unable to apply/cache volume on voice session %d", i);
            }
        }
    }
    AHAL_DBG("Exit ret: %d", ret);
    return ret;
}

void AudioVoice::updateVoiceMetadataForBT(bool call_active)
{
    ssize_t track_count = 1;
    std::vector<playback_track_metadata_t> Sourcetracks;
    std::vector<record_track_metadata_t> Sinktracks;
    Sourcetracks.resize(track_count);
    Sinktracks.resize(track_count);
    int32_t ret = 0;

    source_metadata_t btSourceMetadata;
    sink_metadata_t btSinkMetadata;

    if (call_active) {
        btSourceMetadata.track_count = track_count;
        btSourceMetadata.tracks = Sourcetracks.data();

        btSourceMetadata.tracks->usage = AUDIO_USAGE_VOICE_COMMUNICATION;
        btSourceMetadata.tracks->content_type = AUDIO_CONTENT_TYPE_SPEECH;

        AHAL_DBG("Source metadata for voice call usage:%d content_type:%d",
            btSourceMetadata.tracks->usage, btSourceMetadata.tracks->content_type);
        //Pass the source metadata to PAL
        pal_set_param(PAL_PARAM_ID_SET_SOURCE_METADATA, (void*)&btSourceMetadata, 0);

        btSinkMetadata.track_count = track_count;
        btSinkMetadata.tracks = Sinktracks.data();

        btSinkMetadata.tracks->source = AUDIO_SOURCE_VOICE_CALL;

        AHAL_DBG("Sink metadata for voice call source:%d", btSinkMetadata.tracks->source);
        //Pass the sink metadata to PAL
        pal_set_param(PAL_PARAM_ID_SET_SINK_METADATA, (void*)&btSinkMetadata, 0);
    } else {

        /* When voice call ends, we need to restore metadata configuration for
         * source and sink sessions same as prior to the call. Send source
         * and sink metadata separately to BT.
         */
        if (stream_out_primary_) {
            stream_out_primary_->sourceMetadata_mutex_.lock();
            ret = stream_out_primary_->SetAggregateSourceMetadata(false);
            if (ret != 0) {
                AHAL_ERR("Set PAL_PARAM_ID_SET_SOURCE_METADATA for %d failed", ret);
            }
            stream_out_primary_->sourceMetadata_mutex_.unlock();
        }

        if (stream_in_primary_) {
            stream_in_primary_->sinkMetadata_mutex_.lock();
            ret = stream_in_primary_->SetAggregateSinkMetadata(false);
            if (ret != 0) {
                AHAL_ERR("Set PAL_PARAM_ID_SET_SINK_METADATA for %d failed", ret);
            }
            stream_in_primary_->sinkMetadata_mutex_.unlock();
        }
    }
}

AudioVoice::AudioVoice() {

    voice_.in_call = false;
    pal_vol_ = NULL;
    pal_vol_ = (struct pal_volume_data*)malloc(sizeof(uint32_t)
        + sizeof(struct pal_channel_vol_kv));
    if (pal_vol_) {
        pal_vol_->no_of_volpair = 1;
        pal_vol_->volume_pair[0].channel_mask = 0x01;
        pal_vol_->volume_pair[0].vol = -1.0;
    } else {
        AHAL_ERR("volume malloc failed %s", strerror(errno));
        return;
    }

     pal_crs_vol_ = NULL;
     pal_crs_vol_ = (struct pal_volume_data*)malloc(sizeof(uint32_t)
           + sizeof(struct pal_channel_vol_kv));
     if (pal_crs_vol_) {
         pal_crs_vol_->no_of_volpair = 1;
         pal_crs_vol_->volume_pair[0].channel_mask = 0x01;
         pal_crs_vol_->volume_pair[0].vol = -1.0;
     } else {
         AHAL_ERR("crs volume malloc failed %s", strerror(errno));
         return;
     }

    for (int i = 0; i < MAX_VOICE_SESSIONS; i++) {
        voice_.session[i].state.current_ = CALL_INACTIVE;
        voice_.session[i].state.new_ = CALL_INACTIVE;
        voice_.session[i].vsid = VOICEMMODE1_VSID;
        voice_.session[i].pal_voice_handle = NULL;
        voice_.session[i].tty_mode = PAL_TTY_OFF;
        voice_.session[i].volume_boost = false;
        voice_.session[i].slow_talk = false;
        voice_.session[i].pal_voice_loopback_handle = NULL;
        voice_.session[i].hd_voice = false;
        voice_.session[i].pal_vol_data = pal_vol_;
        voice_.session[i].pal_vol_crs_data = pal_crs_vol_;
        voice_.session[i].device_mute.dir = PAL_AUDIO_OUTPUT;
        voice_.session[i].device_mute.mute = false;
        voice_.session[i].hac = false;
    }

    voice_.session[MMODE1_SESS_IDX].vsid = VOICEMMODE1_VSID;
    voice_.session[MMODE2_SESS_IDX].vsid = VOICEMMODE2_VSID;
    voice_.crsVsid = 0;
    voice_.crsVol = 0.4;
    voice_.crsCall = false;
    voice_.crsLoopback = true;
    stream_out_primary_ = NULL;
}

AudioVoice::~AudioVoice() {

    voice_.in_call = false;
    if (pal_vol_)
        free(pal_vol_);
    if (pal_crs_vol_)
        free(pal_crs_vol_);

    for (int i = 0; i < MAX_VOICE_SESSIONS; i++) {
        voice_.session[i].state.current_ = CALL_INACTIVE;
        voice_.session[i].state.new_ = CALL_INACTIVE;
        voice_.session[i].vsid = VOICEMMODE1_VSID;
        voice_.session[i].tty_mode = PAL_TTY_OFF;
        voice_.session[i].volume_boost = false;
        voice_.session[i].slow_talk = false;
        voice_.session[i].pal_voice_handle = NULL;
        voice_.session[i].pal_voice_loopback_handle = NULL;
        voice_.session[i].hd_voice = false;
        voice_.session[i].pal_vol_data = NULL;
        voice_.session[i].pal_vol_crs_data = NULL;
        voice_.session[i].hac = false;
    }

    voice_.session[MMODE1_SESS_IDX].vsid = VOICEMMODE1_VSID;
    voice_.session[MMODE2_SESS_IDX].vsid = VOICEMMODE2_VSID;

    stream_out_primary_ = NULL;
}

