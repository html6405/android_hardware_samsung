LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE_TAGS := optional

LOCAL_SRC_FILES := \
	color_space_convertor.c \
	csc_linear_to_tiled_crop_neon.s \
	csc_linear_to_tiled_interleave_crop_neon.s \
	csc_tiled_to_linear_crop_neon.s \
	csc_tiled_to_linear_deinterleave_crop_neon.s \
	csc_ARGB8888_to_YUV420SP_NEON.s \
	csc_ABGR8888_to_YUV420SP_NEON.s \
	csc_interleave_memcpy_neon.s \
	csc_fimc.cpp

LOCAL_C_INCLUDES := \
	$(LOCAL_PATH)/include \
	$(TOP)/$(TARGET_OMX_PATH)/include/khronos \
	$(TOP)/$(TARGET_OMX_PATH)/include/sec \
	$(TOP)/$(TARGET_HAL_PATH)/include \
	$(TOP)/$(TARGET_HAL_PATH)/libhwconverter

ifeq ($(BOARD_USE_SAMSUNG_COLORFORMAT), true)
LOCAL_CFLAGS += -DUSE_SAMSUNG_COLORFORMAT
endif
LOCAL_CFLAGS += -Wno-error

LOCAL_MODULE := libseccscapi
LOCAL_VENDOR_MODULE := true

LOCAL_PRELINK_MODULE := false

LOCAL_CFLAGS :=

LOCAL_ARM_MODE := arm

LOCAL_STATIC_LIBRARIES :=
LOCAL_SHARED_LIBRARIES := liblog libfimc libhwconverter libutils

include $(BUILD_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := libseccscapi_headers
LOCAL_VENDOR_MODULE := true
LOCAL_EXPORT_C_INCLUDE_DIRS := $(LOCAL_PATH)/include $(LOCAL_PATH)
include $(BUILD_HEADER_LIBRARY)
