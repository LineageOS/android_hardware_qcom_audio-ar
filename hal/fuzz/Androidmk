LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := fuzz-audio-hal
LOCAL_VENDOR_MODULE := true

LOCAL_SRC_FILES := \
    main.cpp

LOCAL_SHARED_LIBRARIES := \
    $(AHAL_DEFAULT_AIDL_INTERFACE_DEPENDENCIES) \
    libbase \
    libbinder_ndk \
    libbinder \
    libcutils \
    libhardware \
    libutils \
    libclang_rt.ubsan_standalone

include $(BUILD_FUZZ_TEST)
