LOCAL_PATH := $(call my-dir)
CURRENT_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE  := libaudioplatformconverter.qti
LOCAL_MODULE_OWNER  := qti
LOCAL_MODULE_TAGS   := optional
LOCAL_VENDOR_MODULE := true

LOCAL_C_INCLUDES    += \
     $(LOCAL_PATH)/include

LOCAL_EXPORT_C_INCLUDE_DIRS   := $(LOCAL_PATH)/include

LOCAL_SRC_FILES := \
    PlatformConverter.cpp

LOCAL_SHARED_LIBRARIES := \
    $(AHAL_DEFAULT_AIDL_INTERFACE_DEPENDENCIES) \
    libbase \
    libstagefright_foundation \
    libar-pal

include $(BUILD_SHARED_LIBRARY)
