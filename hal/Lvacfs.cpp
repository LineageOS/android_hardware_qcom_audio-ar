/*
 * Copyright (C) 2025 The LineageOS Project
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

#define LOG_TAG "AHAL: Lvacfs"

#include <AudioStream.h>
#include <Lvacfs.h>
#include <dlfcn.h>
#include <log/log.h>

#ifdef __LP64__
#define VENDOR_LIBS "/vendor/lib64/"
#define ODM_LIBS "/odm/lib64/"
#else
#define VENDOR_LIBS "/vendor/lib/"
#define ODM_LIBS "/odm/lib/"
#endif
#define LVACFS_WRAPPER_LIB_NAME "liblvacfs_wrapper.so"
#define VENDOR_LIB_PATH VENDOR_LIBS LVACFS_WRAPPER_LIB_NAME
#define ODM_LIB_PATH ODM_LIBS LVACFS_WRAPPER_LIB_NAME

#define ODM_PARAMS_DIR_PATH "/odm/etc/lvacfs_params"
#define VENDOR_PARAMS_DIR_PATH "/vendor/etc/lvacfs_params"

void Lvacfs::init() {
    if (access(ODM_PARAMS_DIR_PATH, F_OK) == 0) {
        params_file_path_ = ODM_PARAMS_DIR_PATH;
    } else if (access(VENDOR_PARAMS_DIR_PATH, F_OK) == 0) {
        params_file_path_ = VENDOR_PARAMS_DIR_PATH;
    } else {
        ALOGE("No params directory found");
        return;
    }

    wrapper_ops_ = std::make_unique<struct lvacfs_wrapper_ops>();
    if (!wrapper_ops_) {
        ALOGE("Failed to allocate lvacfs_wrapper_ops");
        return;
    }

    wrapper_ops_->lib_handle = dlopen(ODM_LIB_PATH, RTLD_NOW);
    if (!wrapper_ops_->lib_handle) {
        wrapper_ops_->lib_handle = dlopen(VENDOR_LIB_PATH, RTLD_NOW);
        if (!wrapper_ops_->lib_handle) {
            ALOGE("dlopen failed for %s and %s: %s", ODM_LIB_PATH, VENDOR_LIB_PATH, dlerror());
            return;
        }
    }

    if (!(wrapper_ops_->create_instance = (decltype(wrapper_ops_->create_instance))dlsym(
                  wrapper_ops_->lib_handle, "lvacfs_wrapper_CreateLibraryInstance")) ||
        !(wrapper_ops_->destroy_instance = (decltype(wrapper_ops_->destroy_instance))dlsym(
                  wrapper_ops_->lib_handle, "lvacfs_wrapper_DestroyLibraryInstance")) ||
        !(wrapper_ops_->process = (decltype(wrapper_ops_->process))dlsym(
                  wrapper_ops_->lib_handle, "lvacfs_wrapper_Process")) ||
        !(wrapper_ops_->update_zoom_info = (decltype(wrapper_ops_->update_zoom_info))dlsym(
                  wrapper_ops_->lib_handle, "lvacfs_wrapper_UpdateZoomInfo")) ||
        !(wrapper_ops_->update_angle_info = (decltype(wrapper_ops_->update_angle_info))dlsym(
                  wrapper_ops_->lib_handle, "lvacfs_wrapper_UpdateAngleInfo")) ||
        !(wrapper_ops_->set_params_file_path = (decltype(wrapper_ops_->set_params_file_path))dlsym(
                  wrapper_ops_->lib_handle, "lvacfs_SetParamsFilePath")) ||
        !(wrapper_ops_->set_profile = (decltype(wrapper_ops_->set_profile))dlsym(
                  wrapper_ops_->lib_handle, "lvacfs_wrapper_SetProfile")) ||
        !(wrapper_ops_->set_audio_direction = (decltype(wrapper_ops_->set_audio_direction))dlsym(
                  wrapper_ops_->lib_handle, "lvacfs_wrapper_SetAudioDirection")) ||
        !(wrapper_ops_->set_device_orientation =
                  (decltype(wrapper_ops_->set_device_orientation))dlsym(
                          wrapper_ops_->lib_handle, "lvacfs_wrapper_SetDeviceOrientation")) ||
        !(wrapper_ops_->get_versions = (decltype(wrapper_ops_->get_versions))dlsym(
                  wrapper_ops_->lib_handle, "lvacfs_wrapper_GetVersions"))) {
        ALOGE("Failed to load one or more symbols: %s", dlerror());
        deinit();
        return;
    }

    ALOGI("LVACFS initialized successfully");
}

void Lvacfs::deinit() {
    if (wrapper_ops_ && wrapper_ops_->lib_handle) {
        dlclose(wrapper_ops_->lib_handle);
    }
    wrapper_ops_.reset();
    ALOGI("LVACFS deinitialized");
}

void Lvacfs::startInputStream(StreamInPrimary* in) {
    in->lvacfs_handle = (void**)calloc(1, sizeof(void*));
    if (!in->lvacfs_handle) {
        ALOGE("Failed to allocate lvacfs_handle");
        return;
    }
    wrapper_ops_->set_params_file_path(params_file_path_);
    int channel_count = audio_channel_count_from_in_mask(in->config_.channel_mask);
    uint32_t channels = ((channel_count & 0xFFFF) << 16) | (channel_count & 0xFFFF);
    // Set format to false (0) since lvacfs does not support pal audio format
    uint64_t sample_rate_and_format = ((uint64_t)0 << 32) | in->config_.sample_rate;
    int ret = wrapper_ops_->create_instance(in->lvacfs_handle, in->source_,
                                            sample_rate_and_format, channels);
    if (ret < 0) {
        ALOGE("create instance failed: %d", ret);
        stopInputStream(in);
    }
}

void Lvacfs::processInputStream(StreamInPrimary* in, void* buffer, size_t bytes) {
    std::lock_guard<std::mutex> lock(in->lvacfs_lock);
    int channel_count = audio_channel_count_from_in_mask(in->config_.channel_mask);
    uint32_t num_frames = bytes / (channel_count * audio_bytes_per_sample(in->config_.format));
    uint8_t status_buffer[0x160] = {0};
    int ret = wrapper_ops_->process(in->lvacfs_handle, buffer, buffer, num_frames, status_buffer);
    if (ret < 0) {
        ALOGE("process failed: %d", ret);
        stopInputStream(in);
    }
}

void Lvacfs::stopInputStream(StreamInPrimary* in) {
    int ret = wrapper_ops_->destroy_instance(in->lvacfs_handle);
    if (ret < 0) {
        ALOGE("destroy instance failed: %d", ret);
    }
    free(in->lvacfs_handle);
    in->lvacfs_handle = nullptr;
}
