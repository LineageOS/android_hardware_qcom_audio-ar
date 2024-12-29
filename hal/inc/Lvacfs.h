/*
 * SPDX-FileCopyrightText: 2025 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "PalApi.h"
#include "PalDefs.h"

class StreamInPrimary;

struct lvacfs_wrapper_ops {
    void* lib_handle;
    int (*create_instance)(void**, uint32_t, uint64_t, uint32_t);
    int (*destroy_instance)(void*);
    int (*process)(void*, void*, void*, int32_t, void*);
    void (*update_zoom_info)(void*, float);
    void (*update_angle_info)(void*, float);
    void (*set_params_file_path)(const char*);
    void (*set_profile)(void*, uint32_t);
    void (*set_audio_direction)(void*, uint32_t);
    void (*set_device_orientation)(void*, uint32_t);
    void (*get_versions)(char*, size_t);
};

class Lvacfs {
  public:
    static Lvacfs& getInstance() {
        static Lvacfs instance;
        return instance;
    }

    static bool isLvacfsEnabled();
    void init();
    void deinit();
    void startInputStream(StreamInPrimary* in);
    void processInputStream(StreamInPrimary* in, void* buffer, size_t bytes);
    void stopInputStream(StreamInPrimary* in);

    const struct lvacfs_wrapper_ops* getWrapperOps() const { return wrapper_ops_.get(); }

  private:
    Lvacfs() : wrapper_ops_(nullptr), params_file_path_(nullptr) {}

    Lvacfs(const Lvacfs&) = delete;
    Lvacfs& operator=(const Lvacfs&) = delete;

    std::unique_ptr<struct lvacfs_wrapper_ops> wrapper_ops_;
    const char* params_file_path_;
};
