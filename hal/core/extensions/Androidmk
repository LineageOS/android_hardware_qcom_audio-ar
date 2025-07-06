LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE            := libaudiocore.extension
LOCAL_VENDOR_MODULE     := true

LOCAL_C_INCLUDES            := $(LOCAL_PATH)/include \
                               $(LOCAL_PATH)/../platform/include \
                               $(LOCAL_PATH)/../utils/include

LOCAL_EXPORT_C_INCLUDE_DIRS   := $(LOCAL_PATH)/include

LOCAL_CFLAGS := -Wall -Wextra -Werror -Wthread-safety

LOCAL_SRC_FILES := \
    AudioExtension.cpp

LOCAL_HEADER_LIBRARIES :=  \
    libaudioclient_headers \
    libmedia_helper_headers \
    libexpectedutils_headers

LOCAL_SHARED_LIBRARIES := \
    libbase \
    libbinder_ndk \
    libcutils \
    libfmq \
    liblog \
    libmedia_helper \
    libstagefright_foundation \
    libutils \
    libxml2 \
    android.hardware.common-V2-ndk \
    android.hardware.common.fmq-V1-ndk \
    android.media.audio.common.types-V3-ndk \
    android.hardware.audio.core-V2-ndk \
    qti-audio-types-aidl-V1-ndk \
    libar-pal

include $(BUILD_STATIC_LIBRARY)

#-------------------------------------------
#              Build HFP LIB
#-------------------------------------------
include $(CLEAR_VARS)

LOCAL_MODULE := libhfp_pal
LOCAL_VENDOR_MODULE := true

ifeq ($(TARGET_BOARD_AUTO),true)
  LOCAL_CFLAGS += -DPLATFORM_AUTO
endif

ifeq ($(strip $(AUDIO_FEATURE_ENABLED_HFP_SERVER_CLIENT)), true)
LOCAL_CFLAGS           += -DHFP_SERVER_CLIENT_ENABLED
endif

LOCAL_SRC_FILES:= Hfp.cpp

LOCAL_CFLAGS += \
    -Wall \
    -Werror \
    -Wno-unused-function \
    -Wno-unused-variable

LOCAL_CPPFLAGS += -fexceptions

LOCAL_SHARED_LIBRARIES := \
    libaudioroute \
    libbase \
    liblog \
    libaudioutils \
    libcutils \
    libdl \
    libexpat \
    liblog \
    libar-pal

LOCAL_C_INCLUDES := \
    $(TOP)/vendor/qcom/opensource/pal \
    $(TOP)/vendor/qcom/opensource/audio-hal/primary-hal/hal \
    $(TOP)/external/expat/lib \
    $(TOP)/system/media/audio_utils/include \
    $(call include-path-for, audio-route) \

LOCAL_HEADER_LIBRARIES += libhardware_headers
LOCAL_HEADER_LIBRARIES += libsystem_headers
include $(BUILD_SHARED_LIBRARY)

#-------------------------------------------
#            Build FM LIB
#-------------------------------------------
include $(CLEAR_VARS)

LOCAL_MODULE := libfmpal
LOCAL_VENDOR_MODULE := true

LOCAL_SRC_FILES:= FM.cpp

LOCAL_CFLAGS += \
    -Wall \
    -Werror \
    -Wno-unused-function \
    -Wno-unused-variable

LOCAL_SHARED_LIBRARIES := \
    libaudioroute \
    libbase \
    liblog \
    libaudioutils \
    libcutils \
    libdl \
    libexpat \
    liblog \
    libar-pal

LOCAL_C_INCLUDES := \
    $(TOP)/vendor/qcom/opensource/pal \
    $(TOP)/vendor/qcom/opensource/audio-hal/primary-hal/hal \
    $(TOP)/vendor/qcom/opensource/audio-hal/primary-hal/hal/core/extensions/include \
    $(TOP)/external/expat/lib \
    $(TOP)/system/media/audio_utils/include \
    $(call include-path-for, audio-route) \

LOCAL_HEADER_LIBRARIES += libhardware_headers
LOCAL_HEADER_LIBRARIES += libsystem_headers
include $(BUILD_SHARED_LIBRARY)

#-------------------------------------------
#            Build BATTERY_LISTENER
#-------------------------------------------
include $(CLEAR_VARS)

LOCAL_MODULE := libbatterylistener
LOCAL_VENDOR_MODULE := true

LOCAL_SRC_FILES:= battery_listener.cpp
LOCAL_C_INCLUDES := $(LOCAL_PATH)/include

LOCAL_CFLAGS := \
    -Wall \
    -Werror \
    -Wno-unused-function \
    -Wno-unused-variable

LOCAL_SHARED_LIBRARIES := \
    android.hardware.health@1.0 \
    android.hardware.health@2.0 \
    android.hardware.health@2.1 \
    android.hardware.power@1.2 \
    android.hardware.health-V1-ndk \
    libbinder_ndk \
    libaudioutils \
    libbase \
    libcutils \
    libdl \
    libhidlbase \
    liblog \
    libutils \

LOCAL_STATIC_LIBRARIES := libhealthhalutils

include $(BUILD_SHARED_LIBRARY)

