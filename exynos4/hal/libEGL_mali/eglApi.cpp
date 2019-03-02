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

#include <system/graphics.h>
#include <system/window.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

extern "C" EGLBoolean shim_eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config,
        EGLint attribute, EGLint *value);

extern "C" EGLSurface shim_eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config,
                                    NativeWindowType window,
                                    const EGLint *attrib_list);

EGLBoolean eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config,
        EGLint attribute, EGLint *value) {
     return shim_eglGetConfigAttrib(dpy, config, attribute, value);
}

EGLSurface eglCreateWindowSurface(  EGLDisplay dpy, EGLConfig config,
                                    NativeWindowType window,
                                    const EGLint *attrib_list) {
#ifdef NEEDS_NATIVE_WINDOW_FORMAT_FIX
     int format, err;
     window->query(window, NATIVE_WINDOW_FORMAT, &format);

     if (format == (int)HAL_PIXEL_FORMAT_RGBA_8888) {
         format = (int)HAL_PIXEL_FORMAT_BGRA_8888;
         err = window->perform(window, NATIVE_WINDOW_SET_BUFFERS_FORMAT, format);
     }

#endif
     return shim_eglCreateWindowSurface(dpy, config, window, attrib_list);
}
