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
 * Changes from Qualcomm Innovation Center, Inc. are provided under the following license:
 * Copyright (c) 2023-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#pragma once
#include <string>

#include <aidl/android/hardware/audio/effect/BnEffect.h>
#include <aidl/android/hardware/audio/effect/Range.h>
#include <android-base/logging.h>
#include <system/audio_effects/aidl_effects_utils.h>

typedef binder_exception_t (*EffectCreateFunctor)(
        const ::aidl::android::media::audio::common::AudioUuid*,
        std::shared_ptr<::aidl::android::hardware::audio::effect::IEffect>*);
typedef binder_exception_t (*EffectDestroyFunctor)(
        const std::shared_ptr<::aidl::android::hardware::audio::effect::IEffect>&);
typedef binder_exception_t (*EffectQueryFunctor)(
        const ::aidl::android::media::audio::common::AudioUuid*,
        ::aidl::android::hardware::audio::effect::Descriptor*);

struct effect_dl_interface_s {
    EffectCreateFunctor createEffectFunc;
    EffectDestroyFunctor destroyEffectFunc;
    EffectQueryFunctor queryEffectFunc;
};

namespace aidl::qti::effects {

enum class RetCode {
    SUCCESS,
    ERROR_ILLEGAL_PARAMETER, /* Illegal parameter */
    ERROR_THREAD,            /* Effect thread error */
    ERROR_NULL_POINTER,      /* NULL pointer */
    ERROR_ALIGNMENT_ERROR,   /* Memory alignment error */
    ERROR_BLOCK_SIZE_EXCEED, /* Maximum block size exceeded */
    ERROR_EFFECT_LIB_ERROR,  /* Effect implementation library error */
    ERROR_EVENT_FLAG_ERROR   /* Error with effect event flags */
};

static const int INVALID_AUDIO_SESSION_ID = -1;

inline std::ostream& operator<<(std::ostream& out, const RetCode& code) {
    switch (code) {
        case RetCode::SUCCESS:
            return out << "SUCCESS";
        case RetCode::ERROR_ILLEGAL_PARAMETER:
            return out << "ERROR_ILLEGAL_PARAMETER";
        case RetCode::ERROR_THREAD:
            return out << "ERROR_THREAD";
        case RetCode::ERROR_NULL_POINTER:
            return out << "ERROR_NULL_POINTER";
        case RetCode::ERROR_ALIGNMENT_ERROR:
            return out << "ERROR_ALIGNMENT_ERROR";
        case RetCode::ERROR_BLOCK_SIZE_EXCEED:
            return out << "ERROR_BLOCK_SIZE_EXCEED";
        case RetCode::ERROR_EFFECT_LIB_ERROR:
            return out << "ERROR_EFFECT_LIB_ERROR";
        case RetCode::ERROR_EVENT_FLAG_ERROR:
            return out << "ERROR_EVENT_FLAG_ERROR";
    }

    return out << "EnumError: " << code;
}

#define RETURN_IF_ASTATUS_NOT_OK(status, message)                                              \
    do {                                                                                       \
        const ::ndk::ScopedAStatus curr_status = (status);                                     \
        if (!curr_status.isOk()) {                                                             \
            LOG(ERROR) << __func__ << ":" << __LINE__                                          \
                       << "return with status: " << curr_status.getDescription() << (message); \
            return ndk::ScopedAStatus::fromExceptionCodeWithMessage(                           \
                    curr_status.getExceptionCode(), (message));                                \
        }                                                                                      \
    } while (0)

#define RETURN_IF(expr, exception, message)                                                  \
    do {                                                                                     \
        if (expr) {                                                                          \
            LOG(VERBOSE) << __func__ << ":" << __LINE__ << " return with expr " << #expr;    \
            return ndk::ScopedAStatus::fromExceptionCodeWithMessage((exception), (message)); \
        }                                                                                    \
    } while (0)

#define RETURN_OK_IF(expr)                                                                \
    do {                                                                                  \
        if (expr) {                                                                       \
            LOG(VERBOSE) << __func__ << ":" << __LINE__ << " return with expr " << #expr; \
            return ndk::ScopedAStatus::ok();                                              \
        }                                                                                 \
    } while (0)

#define RETURN_VALUE_IF(expr, ret, log)                                                  \
    do {                                                                                 \
        if (expr) {                                                                      \
            LOG(ERROR) << __func__ << ":" << __LINE__ << " return with expr \"" << #expr \
                       << "\":" << (log);                                                \
            return ret;                                                                  \
        }                                                                                \
    } while (0)

#define RETURN_IF_BINDER_EXCEPTION(functor)                                 \
    {                                                                       \
        binder_exception_t exception = functor;                             \
        if (EX_NONE != exception) {                                         \
            LOG(ERROR) << #functor << ":  failed with error " << exception; \
            return ndk::ScopedAStatus::fromExceptionCode(exception);        \
        }                                                                   \
    }

/**
 * Make a Range::$EffectType$Range.
 * T: The $EffectType$, Visualizer for example.
 * Tag: The union tag name in $EffectType$ definition, latencyMs for example.
 * l: The value of Range::$EffectType$Range.min.
 * r: The value of Range::$EffectType$Range.max.
 */
#define MAKE_RANGE(T, Tag, l, r) \
    { .min = T::make<T::Tag>(l), .max = T::make<T::Tag>(r) }

} // namespace aidl::qti::effects

constexpr size_t getPcmSampleSizeInBytes(::aidl::android::media::audio::common::PcmType pcm) {
    using ::aidl::android::media::audio::common::PcmType;
    switch (pcm) {
        case PcmType::UINT_8_BIT:
            return 1;
        case PcmType::INT_16_BIT:
            return 2;
        case PcmType::INT_32_BIT:
            return 4;
        case PcmType::FIXED_Q_8_24:
            return 4;
        case PcmType::FLOAT_32_BIT:
            return 4;
        case PcmType::INT_24_BIT:
            return 3;
    }
    return 0;
}

constexpr size_t getChannelCount(
        const ::aidl::android::media::audio::common::AudioChannelLayout& layout,
        int32_t mask = std::numeric_limits<int32_t>::max()) {
    using Tag = ::aidl::android::media::audio::common::AudioChannelLayout::Tag;
    switch (layout.getTag()) {
        case Tag::none:
            return 0;
        case Tag::invalid:
            return 0;
        case Tag::indexMask:
            return __builtin_popcount(layout.get<Tag::indexMask>() & mask);
        case Tag::layoutMask:
            return __builtin_popcount(layout.get<Tag::layoutMask>() & mask);
        case Tag::voiceMask:
            return __builtin_popcount(layout.get<Tag::voiceMask>() & mask);
        case Tag::acnMask:
            // acnMask specifies channel count.
            return layout.get<Tag::acnMask>() &
                   ::aidl::android::media::audio::common::AudioChannelLayout::
                           ACN_CHANNEL_COUNT_BIT_MASK;
    }
    return 0;
}

constexpr size_t getFrameSizeInBytes(
        const ::aidl::android::media::audio::common::AudioFormatDescription& format,
        const ::aidl::android::media::audio::common::AudioChannelLayout& layout) {
    if (format == ::aidl::android::media::audio::common::AudioFormatDescription{}) {
        // Unspecified format.
        return 0;
    }
    using ::aidl::android::media::audio::common::AudioFormatType;
    if (format.type == AudioFormatType::PCM) {
        return getPcmSampleSizeInBytes(format.pcm) * getChannelCount(layout);
    } else if (format.type == AudioFormatType::NON_PCM) {
        // For non-PCM formats always use the underlying PCM size. The default value for
        // PCM is "UINT_8_BIT", thus non-encapsulated streams have the frame size of 1.
        return getPcmSampleSizeInBytes(format.pcm);
    }
    // Something unexpected.
    return 0;
}