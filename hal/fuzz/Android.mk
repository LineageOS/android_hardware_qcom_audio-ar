LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := fuzz-audio-hal
LOCAL_VENDOR_MODULE := true

LOCAL_SRC_FILES := \
    main.cpp

LOCAL_SHARED_LIBRARIES := \
    libbase \
    libbinder_ndk \
    libbinder \
    libcutils \
    libhardware \
    libutils \
    $(LATEST_ANDROID_MEDIA_ADUIO_COMMON_TYPES) \
    $(LATEST_ANDROID_HARDWARE_AUDIO_CORE) \
    libclang_rt.ubsan_standalone

include $(BUILD_FUZZ_TEST)
