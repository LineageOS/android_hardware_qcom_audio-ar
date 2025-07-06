LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE            := aidl_fuzzer_audio_core_hal
LOCAL_VENDOR_MODULE     := true

LOCAL_SRC_FILES := \
    fuzzer.cpp

LOCAL_SHARED_LIBRARIES := \
    $(AHAL_DEFAULT_AIDL_INTERFACE_DEPENDENCIES) \
    libbase \
    libbinder_ndk \
    libbinder \
    libcutils \
    libhardware \
    libutils \
    libclang_rt.ubsan_standalone

LOCAL_STATIC_LIBRARIES += libbinder_random_parcel

include $(BUILD_FUZZ_TEST)