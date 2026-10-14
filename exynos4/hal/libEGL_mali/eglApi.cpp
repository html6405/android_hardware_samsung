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

#include <log/log.h>
#include <system/graphics.h>
#include <system/window.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#if defined(NEEDS_NATIVE_WINDOW_FORMAT_FIX) || \
    defined(SKIP_EGL_CONTEXT_DETACH)
static int debug_level = 0;
#endif

extern "C" EGLBoolean shim_eglChooseConfig(EGLDisplay dpy,
        const EGLint *attrib_list, EGLConfig *configs, EGLint config_size,
        EGLint *num_config);

extern "C" EGLBoolean shim_eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config,
        EGLint attribute, EGLint *value);

extern "C" EGLSurface shim_eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config,
                                    NativeWindowType window,
                                    const EGLint *attrib_list);

extern "C" EGLBoolean shim_eglMakeCurrent(  EGLDisplay dpy, EGLSurface draw,
                            EGLSurface read, EGLContext ctx);

extern "C" EGLBoolean shim_eglDestroySurface(EGLDisplay dpy, EGLSurface surface);

extern "C" EGLSurface shim_eglGetCurrentSurface(EGLint readdraw);

extern "C" EGLContext shim_eglGetCurrentContext(void);

extern "C" const char *shim_eglQueryString(EGLDisplay dpy, EGLint name);

extern "C" EGLContext shim_eglCreateContext(EGLDisplay dpy, EGLConfig config,
        EGLContext share_context, const EGLint *attrib_list);

namespace {

constexpr EGLint kOpenGlEs3Bit = 0x0040;
constexpr EGLint kContextOpenGLRobustAccessExt = 0x30BF;
constexpr EGLint kContextOpenGLResetNotificationStrategyExt = 0x3138;
constexpr EGLint kContextOpenGLResetNotificationStrategyKhr = 0x31BD;
constexpr size_t kMaxContextAttribPairs = 64;

constexpr char kCreateContextRobustnessExt[] =
        "EGL_EXT_create_context_robustness";
constexpr char kCreateContextKhrExt[] = "EGL_KHR_create_context";

pthread_mutex_t gCompatLock = PTHREAD_MUTEX_INITIALIZER;
char *gSanitizedExtensions = nullptr;
bool gLoggedExtensionFix = false;
bool gLoggedContextFix = false;
bool gLoggedEs3ConfigProbe = false;

bool hasExtensionToken(const char *extensions, const char *wanted) {
    if (extensions == nullptr || wanted == nullptr || wanted[0] == '\0')
        return false;

    const size_t wantedLen = strlen(wanted);
    const char *cursor = extensions;

    while (*cursor != '\0') {
        while (*cursor == ' ')
            ++cursor;

        const char *start = cursor;
        while (*cursor != '\0' && *cursor != ' ')
            ++cursor;

        const size_t tokenLen = static_cast<size_t>(cursor - start);
        if (tokenLen == wantedLen && !strncmp(start, wanted, wantedLen))
            return true;
    }

    return false;
}

bool isLegacyEgl14WithoutKhrCreateContext(EGLDisplay dpy,
        const char *extensions) {
    const char *version = shim_eglQueryString(dpy, EGL_VERSION);

    return version != nullptr && !strncmp(version, "1.4", 3) &&
            extensions != nullptr &&
            !hasExtensionToken(extensions, kCreateContextKhrExt);
}

bool isLegacyEgl14RobustnessMismatch(EGLDisplay dpy, const char *extensions) {
    /*
     * Only activate on the problematic capability combination.  This keeps
     * the shim safe if a future Exynos4 userspace stack is replaced by a
     * driver that genuinely supports newer context creation.
     */
    return isLegacyEgl14WithoutKhrCreateContext(dpy, extensions) &&
            hasExtensionToken(extensions, kCreateContextRobustnessExt);
}

bool requestsUnsupportedEs3Config(const EGLint *attrib_list) {
    if (attrib_list == nullptr)
        return false;

    bool terminated = false;
    bool requestsEs3 = false;

    for (size_t pair = 0; pair < kMaxContextAttribPairs; ++pair) {
        const EGLint attribute = attrib_list[pair * 2];
        if (attribute == EGL_NONE) {
            terminated = true;
            break;
        }

        const EGLint value = attrib_list[pair * 2 + 1];
        if (attribute == EGL_RENDERABLE_TYPE && value != EGL_DONT_CARE &&
                (value & kOpenGlEs3Bit) != 0) {
            requestsEs3 = true;
        }
    }

    /* Leave malformed/abnormally large lists to the vendor for validation. */
    return terminated && requestsEs3;
}

char *buildSanitizedExtensionString(const char *extensions) {
    const size_t inputLen = strlen(extensions);
    char *filtered = static_cast<char *>(malloc(inputLen + 1));
    if (filtered == nullptr)
        return nullptr;

    const char *cursor = extensions;
    char *out = filtered;
    bool first = true;

    while (*cursor != '\0') {
        while (*cursor == ' ')
            ++cursor;
        if (*cursor == '\0')
            break;

        const char *start = cursor;
        while (*cursor != '\0' && *cursor != ' ')
            ++cursor;

        const size_t tokenLen = static_cast<size_t>(cursor - start);
        const bool blocked =
                tokenLen == strlen(kCreateContextRobustnessExt) &&
                !strncmp(start, kCreateContextRobustnessExt, tokenLen);

        if (blocked)
            continue;

        if (!first)
            *out++ = ' ';
        memcpy(out, start, tokenLen);
        out += tokenLen;
        first = false;
    }

    *out = '\0';
    return filtered;
}

bool isRobustnessContextAttribute(EGLint attribute) {
    return attribute == kContextOpenGLRobustAccessExt ||
            attribute == kContextOpenGLResetNotificationStrategyExt ||
            attribute == kContextOpenGLResetNotificationStrategyKhr;
}

}  // namespace

const char *eglQueryString(EGLDisplay dpy, EGLint name) {
    const char *extensions = shim_eglQueryString(dpy, name);

    if (name != EGL_EXTENSIONS || extensions == nullptr ||
            !isLegacyEgl14RobustnessMismatch(dpy, extensions)) {
        return extensions;
    }

    pthread_mutex_lock(&gCompatLock);

    if (gSanitizedExtensions == nullptr)
        gSanitizedExtensions = buildSanitizedExtensionString(extensions);

    if (!gLoggedExtensionFix) {
        ALOGI("Legacy EGL 1.4 compatibility: hiding %s",
                kCreateContextRobustnessExt);
        gLoggedExtensionFix = true;
    }

    const char *result =
            gSanitizedExtensions != nullptr ? gSanitizedExtensions : extensions;

    pthread_mutex_unlock(&gCompatLock);
    return result;
}

EGLContext eglCreateContext(EGLDisplay dpy, EGLConfig config,
        EGLContext share_context, const EGLint *attrib_list) {
    if (attrib_list == nullptr)
        return shim_eglCreateContext(dpy, config, share_context, attrib_list);

    const char *extensions = shim_eglQueryString(dpy, EGL_EXTENSIONS);
    if (!isLegacyEgl14RobustnessMismatch(dpy, extensions))
        return shim_eglCreateContext(dpy, config, share_context, attrib_list);

    /*
     * Normally hiding the extension prevents ANGLE from requesting robust
     * contexts at all.  Keep this second line of defence for callers that
     * build an attribute list without consulting eglQueryString(), and for
     * future META-EGL behaviour changes.  Preserve every unrelated attribute.
     */
    EGLint sanitized[kMaxContextAttribPairs * 2 + 1];
    size_t out = 0;
    bool changed = false;
    bool terminated = false;

    for (size_t pair = 0; pair < kMaxContextAttribPairs; ++pair) {
        const EGLint attribute = attrib_list[pair * 2];
        if (attribute == EGL_NONE) {
            terminated = true;
            break;
        }

        const EGLint value = attrib_list[pair * 2 + 1];
        if (isRobustnessContextAttribute(attribute)) {
            changed = true;
            continue;
        }

        sanitized[out++] = attribute;
        sanitized[out++] = value;
    }

    /* Malformed/abnormally large lists are left to the vendor unchanged. */
    if (!terminated || !changed)
        return shim_eglCreateContext(dpy, config, share_context, attrib_list);

    sanitized[out] = EGL_NONE;

    pthread_mutex_lock(&gCompatLock);
    if (!gLoggedContextFix) {
        ALOGI("Legacy EGL 1.4 compatibility: stripped unsupported robustness "
              "context attributes");
        gLoggedContextFix = true;
    }
    pthread_mutex_unlock(&gCompatLock);

    return shim_eglCreateContext(dpy, config, share_context, sanitized);
}


EGLBoolean eglChooseConfig(EGLDisplay dpy, const EGLint *attrib_list,
        EGLConfig *configs, EGLint config_size, EGLint *num_config) {
    /*
     * Mali-400 r3p2 is an EGL 1.4 / GLES2 stack and does not expose
     * EGL_KHR_create_context.  Chromium and other modern EGL clients probe
     * EGL_OPENGL_ES3_BIT_KHR before falling back to ES2.  The legacy driver
     * treats that otherwise-valid probe value as EGL_BAD_ATTRIBUTE instead of
     * simply reporting no matching configs.
     *
     * Correct the capability negotiation at the vendor boundary: an ES3
     * request has zero matching configs on this hardware.  Do not rewrite the
     * request to ES2, do not fabricate an ES3-capable config, and do not alter
     * any real ES2 query.
     */
    if (num_config != nullptr && requestsUnsupportedEs3Config(attrib_list)) {
        const char *extensions = shim_eglQueryString(dpy, EGL_EXTENSIONS);

        if (isLegacyEgl14WithoutKhrCreateContext(dpy, extensions)) {
            *num_config = 0;

            pthread_mutex_lock(&gCompatLock);
            if (!gLoggedEs3ConfigProbe) {
                ALOGI("Legacy EGL 1.4 compatibility: ES3 config probe -> 0 "
                      "configs (GLES2-only Mali-400)");
                gLoggedEs3ConfigProbe = true;
            }
            pthread_mutex_unlock(&gCompatLock);

            return EGL_TRUE;
        }
    }

    return shim_eglChooseConfig(dpy, attrib_list, configs, config_size,
            num_config);
}

EGLBoolean eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config,
        EGLint attribute, EGLint *value) {
     return shim_eglGetConfigAttrib(dpy, config, attribute, value);
}

EGLSurface eglCreateWindowSurface(  EGLDisplay dpy, EGLConfig config,
                                    NativeWindowType window,
                                    const EGLint *attrib_list) {
#ifdef NEEDS_NATIVE_WINDOW_FORMAT_FIX
     int curFormat, newFormat, err;
     window->query(window, NATIVE_WINDOW_FORMAT, &curFormat);

     shim_eglGetConfigAttrib(dpy, config, EGL_NATIVE_VISUAL_ID, &newFormat);

     if (curFormat != newFormat) {
         err = window->perform(window, NATIVE_WINDOW_SET_BUFFERS_FORMAT, newFormat);
         ALOGD_IF(debug_level > 0, "%s: curFormat=%d, newFormat=%d", __func__, curFormat, newFormat);
     }
#endif
     return shim_eglCreateWindowSurface(dpy, config, window, attrib_list);
}

EGLBoolean eglMakeCurrent(  EGLDisplay dpy, EGLSurface draw,
                            EGLSurface read, EGLContext ctx) {
#ifdef SKIP_EGL_CONTEXT_DETACH
    EGLSurface curRead, curDraw;
    EGLContext curCtx;
    if (debug_level > 1) {
        curRead = shim_eglGetCurrentSurface(EGL_READ);
        curDraw = shim_eglGetCurrentSurface(EGL_DRAW);
        curCtx  = shim_eglGetCurrentContext();
        ALOGD("%s: before makeCurrent: curRead=%s, curDraw=%s, curCtx=%s",
                    __func__, (curRead == EGL_NO_SURFACE ? "NULL" : "ok"),
                              (curDraw == EGL_NO_SURFACE ? "NULL" : "ok"),
                              (curCtx  == EGL_NO_CONTEXT ? "NULL" : "ok"));
    }

    if (draw == EGL_NO_SURFACE &&
          read == EGL_NO_SURFACE &&
             ctx == EGL_NO_CONTEXT) {
        ALOGD_IF(debug_level > 0, "%s: skipping context detach!", __func__);

        return true;
    } else
        ALOGD_IF(debug_level > 1, "%s: init", __func__);
#endif

    EGLBoolean res = shim_eglMakeCurrent(dpy, draw, read, ctx);

#ifdef SKIP_EGL_CONTEXT_DETACH
    if (debug_level > 1) {
        curRead = shim_eglGetCurrentSurface(EGL_READ);
        curDraw = shim_eglGetCurrentSurface(EGL_DRAW);
        curCtx  = shim_eglGetCurrentContext();
        ALOGD("%s: after makeCurrent: curRead=%s, curDraw=%s, curCtx=%s",
                    __func__, (curRead == EGL_NO_SURFACE ? "NULL" : "ok"),
                              (curDraw == EGL_NO_SURFACE ? "NULL" : "ok"),
                              (curCtx  == EGL_NO_CONTEXT ? "NULL" : "ok"));
    }
#endif

    return res;
}

EGLBoolean eglDestroySurface(EGLDisplay dpy, EGLSurface surface) {
#ifdef SKIP_EGL_CONTEXT_DETACH
    EGLSurface curRead, curDraw;
    EGLContext curCtx;
    if (debug_level > 2) {
        curRead = shim_eglGetCurrentSurface(EGL_READ);
        curDraw = shim_eglGetCurrentSurface(EGL_DRAW);
        curCtx  = shim_eglGetCurrentContext();
        ALOGD("%s: before destroySurface: curRead=%s, curDraw=%s, curCtx=%s",
                       __func__, (curRead == EGL_NO_SURFACE ? "NULL" : "ok"),
                                 (curDraw == EGL_NO_SURFACE ? "NULL" : "ok"),
                                (curCtx  == EGL_NO_CONTEXT ? "NULL" : "ok"));
    }
#endif

    EGLBoolean res = shim_eglDestroySurface(dpy, surface);

#ifdef SKIP_EGL_CONTEXT_DETACH
    if (debug_level > 2) {
        curRead = shim_eglGetCurrentSurface(EGL_READ);
        curDraw = shim_eglGetCurrentSurface(EGL_DRAW);
        curCtx  = shim_eglGetCurrentContext();
        ALOGD("%s: after destroySurface: curRead=%s, curDraw=%s, curCtx=%s",
                      __func__, (curRead == EGL_NO_SURFACE ? "NULL" : "ok"),
                                (curDraw == EGL_NO_SURFACE ? "NULL" : "ok"),
                               (curCtx  == EGL_NO_CONTEXT ? "NULL" : "ok"));
    }
#endif
   return res;
}
