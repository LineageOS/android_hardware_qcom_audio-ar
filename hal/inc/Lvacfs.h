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

#pragma once

class StreamInPrimary;

extern "C" bool _ZN8OplusPal12isLvacEnableEv();

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

    void deinit();
    void startInputStream(StreamInPrimary* in);
    void processInputStream(StreamInPrimary* in, void* buffer, size_t bytes);
    void stopInputStream(StreamInPrimary* in);

    const struct lvacfs_wrapper_ops* getWrapperOps() const { return wrapper_ops_.get(); }

  private:
    Lvacfs() : wrapper_ops_(nullptr), params_file_path_(nullptr) {
        if (_ZN8OplusPal12isLvacEnableEv()) {
            init();
        }
    }
    ~Lvacfs() { deinit(); }

    Lvacfs(const Lvacfs&) = delete;
    Lvacfs& operator=(const Lvacfs&) = delete;

    bool init();
    std::unique_ptr<struct lvacfs_wrapper_ops> wrapper_ops_;
    const char* params_file_path_;
};
