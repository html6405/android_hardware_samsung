LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES := \
	SEC_OMX_Venc.c

LOCAL_MODULE := libSEC_OMX_Venc
LOCAL_VENDOR_MODULE := true
LOCAL_ARM_MODE := arm
LOCAL_MODULE_TAGS := optional

LOCAL_C_INCLUDES := $(SEC_OMX_INC)/khronos \
	frameworks/native/include \
	$(SEC_OMX_INC)/sec \
	$(SEC_OMX_TOP)/osal \
	$(SEC_OMX_TOP)/core \
	$(SEC_OMX_COMPONENT)/common \
	$(SEC_OMX_COMPONENT)/video/dec

LOCAL_HEADER_LIBRARIES := libseccscapi_headers libhardware_headers

ifeq ($(BOARD_USE_METADATABUFFERTYPE), true)
LOCAL_CFLAGS += -DUSE_METADATABUFFERTYPE
endif

ifeq ($(BOARD_USE_STOREMETADATA), true)
LOCAL_CFLAGS += -DUSE_STOREMETADATA
endif

LOCAL_CFLAGS += -Wno-error

include $(BUILD_STATIC_LIBRARY)
