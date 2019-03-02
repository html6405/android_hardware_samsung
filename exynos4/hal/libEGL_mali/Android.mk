#
# Copyright (C) 2019 The Android Open Source Project
# Copyright (C) 2019 The LineageOS Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

ifeq ($(TARGET_BOARD_PLATFORM),exynos4)

LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

LOCAL_PRELINK_MODULE := false
LOCAL_C_INCLUDES := \
        frameworks/native/libs/nativewindow/include \
        frameworks/native/libs/nativebase/include \
        frameworks/native/libs/arect/include \
        system/core/include

LOCAL_SHARED_LIBRARIES := liblog libcutils

LOCAL_CFLAGS := -DLOG_TAG=\"EGL_mali\"

ifeq ($(TARGET_NEEDS_NATIVE_WINDOW_FORMAT_FIX),true)
LOCAL_CFLAGS += -DNEEDS_NATIVE_WINDOW_FORMAT_FIX
endif

LOCAL_SRC_FILES := shim.S eglApi.cpp
LOCAL_SHARED_LIBRARIES := libMali

LOCAL_MODULE := libEGL_mali
LOCAL_MODULE_TAGS := optional
LOCAL_MODULE_RELATIVE_PATH := egl

include $(BUILD_SHARED_LIBRARY)

endif
