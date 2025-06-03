/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#pragma once

namespace qti::audio::core {

/*
 * This interface is designed to facilitate the use of StreamInPrimary and StreamOutPrimary
 * via the AudioUseCase in scenarios where getParameters is invoked. By implementing this
 * interface, the AudioUseCase can hold an instance of the implementor (StreamIn/Out)
 * and make calls to the implementation as needed, help to avoid duplication of
 * mmap APIs in both in/out classes.
 * Note that configureMMapStream is coming from both StreamCommonInterface and MmapBufferImpl
 * Make sure to keep these aligned.
 */
struct MmapBufferImpl {
    virtual ~MmapBufferImpl() = default;
    virtual ndk::ScopedAStatus configureMMapStream(
            ::aidl::android::hardware::audio::core::MmapBufferDescriptor* desc,
            int32_t* bufferSizeFrames) = 0;
};

}  // namespace qti::audio::core