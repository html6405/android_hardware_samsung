/*
 * Copyright (C) 2019 The Android Open Source Project
 * Copyright (C) 2019 The LineageOS Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

.globl eglGetConfigs
eglGetConfigs:
    b shim_eglGetConfigs

.globl eglChooseConfig
eglChooseConfig:
    b shim_eglChooseConfig

/*
.globl eglGetConfigAttrib
eglGetConfigAttrib:
    b shim_eglGetConfigAttrib
*/

.globl eglGetError
eglGetError:
    b shim_eglGetError

.globl eglQueryString
eglQueryString:
    b shim_eglQueryString

.globl eglWaitClient
eglWaitClient:
    b shim_eglWaitClient

.globl eglWaitGL
eglWaitGL:
    b shim_eglWaitGL

.globl eglWaitNative
eglWaitNative:
    b shim_eglWaitNative

.globl eglSwapBuffers
eglSwapBuffers:
    b shim_eglSwapBuffers

.globl eglCopyBuffers
eglCopyBuffers:
    b shim_eglCopyBuffers

.globl eglSwapInterval
eglSwapInterval:
    b shim_eglSwapInterval

.globl eglBindTexImage
eglBindTexImage:
    b shim_eglBindTexImage

.globl eglReleaseTexImage
eglReleaseTexImage:
    b shim_eglReleaseTexImage

.globl eglBindAPI
eglBindAPI:
    b shim_eglBindAPI

.globl eglQueryAPI
eglQueryAPI:
    b shim_eglQueryAPI

.globl eglCreateContext
eglCreateContext:
    b shim_eglCreateContext

.globl eglDestroyContext
eglDestroyContext:
    b shim_eglDestroyContext

.globl eglQueryContext
eglQueryContext:
    b shim_eglQueryContext

.globl eglMakeCurrent
eglMakeCurrent:
    b shim_eglMakeCurrent

.globl eglGetCurrentContext
eglGetCurrentContext:
    b shim_eglGetCurrentContext

.globl eglGetDisplay
eglGetDisplay:
    b shim_eglGetDisplay

.globl eglGetCurrentDisplay
eglGetCurrentDisplay:
    b shim_eglGetCurrentDisplay

.globl eglInitialize
eglInitialize:
    b shim_eglInitialize

.globl eglTerminate
eglTerminate:
    b shim_eglTerminate

/*.globl eglCreateWindowSurface
eglCreateWindowSurface:
    b shim_eglCreateWindowSurface
*/

.globl eglCreatePbufferSurface
eglCreatePbufferSurface:
    b shim_eglCreatePbufferSurface

.globl eglCreatePbufferFromClientBuffer
eglCreatePbufferFromClientBuffer:
    b shim_eglCreatePbufferFromClientBuffer

.globl eglCreatePixmapSurface
eglCreatePixmapSurface:
    b shim_eglCreatePixmapSurface

.globl eglDestroySurface
eglDestroySurface:
    b shim_eglDestroySurface

.globl eglQuerySurface
eglQuerySurface:
    b shim_eglQuerySurface

.globl eglSurfaceAttrib
eglSurfaceAttrib:
    b shim_eglSurfaceAttrib

.globl eglGetCurrentSurface
eglGetCurrentSurface:
    b shim_eglGetCurrentSurface

.globl eglReleaseThread
eglReleaseThread:
    b shim_eglReleaseThread

.globl eglGetProcAddress
eglGetProcAddress:
    b shim_eglGetProcAddress

.globl eglCreateSyncKHR
eglCreateSyncKHR:
    b shim_eglCreateSyncKHR

.globl eglDestroySyncKHR
eglDestroySyncKHR:
    b shim_eglDestroySyncKHR

.globl eglClientWaitSyncKHR
eglClientWaitSyncKHR:
    b shim_eglClientWaitSyncKHR

.globl eglSignalSyncKHR
eglSignalSyncKHR:
    b shim_eglSignalSyncKHR

.globl eglGetSyncAttribKHR
eglGetSyncAttribKHR:
    b shim_eglGetSyncAttribKHR

.globl eglCreateImageKHR
eglCreateImageKHR:
    b shim_eglCreateImageKHR

.globl eglDestroyImageKHR
eglDestroyImageKHR:
    b shim_eglDestroyImageKHR

.globl eglSetBlobCacheFuncsANDROID
eglSetBlobCacheFuncsANDROID:
    b shim_eglSetBlobCacheFuncsANDROID

.globl mali_egl_image_init
mali_egl_image_init:
    b shim_mali_egl_image_init

.globl mali_egl_image_get_error
mali_egl_image_get_error:
    b shim_mali_egl_image_get_error

.globl mali_egl_image_lock_ptr
mali_egl_image_lock_ptr:
    b shim_mali_egl_image_lock_ptr

.globl mali_egl_image_unlock_ptr
mali_egl_image_unlock_ptr:
    b shim_mali_egl_image_unlock_ptr

.globl mali_egl_image_set_data
mali_egl_image_set_data:
    b shim_mali_egl_image_set_data

.globl mali_egl_image_get_width
mali_egl_image_get_width:
    b shim_mali_egl_image_get_width

.globl mali_egl_image_get_height
mali_egl_image_get_height:
    b shim_mali_egl_image_get_height

.globl mali_egl_image_get_format
mali_egl_image_get_format:
    b shim_mali_egl_image_get_format

.globl mali_egl_image_get_miplevels
mali_egl_image_get_miplevels:
    b shim_mali_egl_image_get_miplevels

.globl mali_egl_image_map_buffer
mali_egl_image_map_buffer:
    b shim_mali_egl_image_map_buffer

.globl mali_egl_image_unmap_buffer
mali_egl_image_unmap_buffer:
    b shim_mali_egl_image_unmap_buffer

.globl mali_egl_image_get_buffer_width
mali_egl_image_get_buffer_width:
    b shim_mali_egl_image_get_buffer_width

.globl mali_egl_image_get_buffer_height
mali_egl_image_get_buffer_height:
    b shim_mali_egl_image_get_buffer_height

.globl mali_egl_image_get_buffer_secure_id
mali_egl_image_get_buffer_secure_id:
    b shim_mali_egl_image_get_buffer_secure_id

.globl mali_egl_image_get_buffer_layout
mali_egl_image_get_buffer_layout:
    b shim_mali_egl_image_get_buffer_layout

.globl mali_egl_image_create_sync
mali_egl_image_create_sync:
    b shim_mali_egl_image_create_sync

.globl mali_egl_image_set_sync
mali_egl_image_set_sync:
    b shim_mali_egl_image_set_sync

.globl mali_egl_image_unset_sync
mali_egl_image_unset_sync:
    b shim_mali_egl_image_unset_sync

.globl mali_egl_image_wait_sync
mali_egl_image_wait_sync:
    b shim_mali_egl_image_wait_sync

.globl mali_egl_image_create
mali_egl_image_create:
    b shim_mali_egl_image_create

.globl mali_egl_image_destroy
mali_egl_image_destroy:
    b shim_mali_egl_image_destroy
