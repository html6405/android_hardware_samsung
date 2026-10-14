#
# Copyright (C) 2026 The LineageOS Project
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
ifeq ($(TARGET_PROVIDES_LIBGLESV2_MALI),true)

LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

LOCAL_PRELINK_MODULE := false
LOCAL_SRC_FILES := shim.S glesApi.c
LOCAL_HEADER_LIBRARIES := gl_headers
LOCAL_SHARED_LIBRARIES := libMali liblog
LOCAL_CFLAGS := -Wall -Wextra -Werror -DLOG_TAG=\"GLESv2_mali\"

LOCAL_MODULE := libGLESv2_mali
LOCAL_MODULE_TAGS := optional
LOCAL_MODULE_RELATIVE_PATH := egl
LOCAL_VENDOR_MODULE := true

include $(BUILD_SHARED_LIBRARY)
endif
endif
