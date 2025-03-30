LOCAL_PATH := $(call my-dir)
ifneq ($(TARGET_PROVIDES_AUDIO_HAL),true)
include $(CLEAR_VARS)

LOCAL_MODULE            := libaudiocorehal.default
LOCAL_VENDOR_MODULE     := true
LOCAL_MODULE_RELATIVE_PATH := hw

LOCAL_CFLAGS := \
    -DBACKEND_NDK \
    -Wall \
    -Wextra \
    -Werror \
    -Wthread-safety

LOCAL_VINTF_FRAGMENTS += manifest_audiocorehal_default.xml

LOCAL_SRC_FILES := \
    DefaultServices.cpp

LOCAL_HEADER_LIBRARIES :=  \
    libxsdc-utils \
    liberror_headers

LOCAL_SHARED_LIBRARIES := \
    libaudioaidlcommon \
    libaudioserviceexampleimpl \
    $(LATEST_ANDROID_HARDWARE_AUDIO_CORE) \
    libbase \
    libbinder_ndk \
    libcutils \
    liblog \
    libdl \
    libxml2 \
    libaudioutils \
    libutils \
    $(LATEST_ANDROID_HARDWARE_COMMON) \
    $(LATEST_ANDROID_MEDIA_ADUIO_COMMON_TYPES) \
    libmedia_helper \
    libstagefright_foundation \
    libhidlbase \
    libhardware \
    libfmq

include $(BUILD_SHARED_LIBRARY)

endif
