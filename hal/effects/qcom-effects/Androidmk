ifneq ($(AUDIO_USE_STUB_HAL), true)
CURRENT_PATH := $(call my-dir)
include $(CURRENT_PATH)/offloadbundle/Android.mk
include $(CURRENT_PATH)/offloadvisualizer/Android.mk
include $(CURRENT_PATH)/voiceprocessing/Android.mk
include $(CURRENT_PATH)/volumelistener/Android.mk
#include $(call all-subdir-makefiles)
endif