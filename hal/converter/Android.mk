LOCAL_PATH := $(call my-dir)
CURRENT_PATH := $(call my-dir)
ifneq ($(TARGET_PROVIDES_AUDIO_HAL),true)
include $(CLEAR_VARS)
LOCAL_MODULE  := libaudioplatformconverter.qti
LOCAL_MODULE_OWNER  := qti
LOCAL_MODULE_TAGS   := optional
LOCAL_VENDOR_MODULE := true

LOCAL_C_INCLUDES    += \
     $(LOCAL_PATH)/include

LOCAL_EXPORT_C_INCLUDE_DIRS   := $(LOCAL_PATH)/include

LOCAL_HEADER_LIBRARIES := libarpal_headers

LOCAL_SRC_FILES := \
    PlatformConverter.cpp

LOCAL_SHARED_LIBRARIES := \
    libbase \
    libstagefright_foundation \
    $(LATEST_ANDROID_HARDWARE_AUDIO_CORE) \
    $(LATEST_ANDROID_MEDIA_ADUIO_COMMON_TYPES) \
    libar-pal

include $(BUILD_SHARED_LIBRARY)
endif
