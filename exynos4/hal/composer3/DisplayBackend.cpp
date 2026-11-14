#define LOG_TAG "exynos4-composer3-display"

#include "DisplayBackend.h"

#include <aidl/android/hardware/graphics/common/PixelFormat.h>
#include <cutils/properties.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <log/log.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sync/sync.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <utility>

#include "backend/BufferManager.h"
#include "s3c_lcd.h"
#include "s5p_fimc_v4l2.h"

namespace exynos4::composer3 {
namespace {

using namespace exynos4::gralloc5;

bool displayFormat(int pixelFormat, s3c_fb_pixel_format* out) {
    using aidl::android::hardware::graphics::common::PixelFormat;
    switch (static_cast<PixelFormat>(pixelFormat)) {
        case PixelFormat::RGBA_8888:
            *out = S3C_FB_PIXEL_FORMAT_RGBA_8888;
            return true;
        case PixelFormat::RGBX_8888:
            *out = S3C_FB_PIXEL_FORMAT_RGBX_8888;
            return true;
        case PixelFormat::RGB_888:
            *out = S3C_FB_PIXEL_FORMAT_RGB_888;
            return true;
        case PixelFormat::RGB_565:
            *out = S3C_FB_PIXEL_FORMAT_RGB_565;
            return true;
        case PixelFormat::BGRA_8888:
            *out = S3C_FB_PIXEL_FORMAT_BGRA_8888;
            return true;
        default:
            return false;
    }
}

}  // namespace

bool DisplayBackend::openDisplay() {
    char path[PROPERTY_VALUE_MAX];
    property_get("persist.vendor.exynos4.framebuffer", path, "/dev/graphics/fb0");
    mFramebuffer.reset(open(path, O_RDWR | O_CLOEXEC));
    if (mFramebuffer.get() < 0 && !strcmp(path, "/dev/graphics/fb0"))
        mFramebuffer.reset(open("/dev/fb0", O_RDWR | O_CLOEXEC));
    if (mFramebuffer.get() < 0) return false;

    fb_var_screeninfo var{};
    if (ioctl(mFramebuffer.get(), FBIOGET_VSCREENINFO, &var) < 0 || var.xres == 0 || var.yres == 0)
        return false;
    mMode.width = var.xres;
    mMode.height = var.yres;
    // A board-supplied mode is the nominal rate exposed to DisplayManager.
    // fb_var_screeninfo.pixclock is a rounded picosecond value; on n8000 it
    // computes 60.003 Hz, which falls outside Android's 60.000 Hz policy once
    // frame-rate override is disabled. Use framebuffer timing only when the
    // device has not specified its mode explicitly.
    const int32_t configuredRate =
            property_get_int32("ro.vendor.exynos4.refresh_rate_millihz", 0);
    if (configuredRate > 0) {
        mMode.refreshRateMilliHz = static_cast<uint32_t>(configuredRate);
    } else if (var.pixclock) {
        const uint64_t denominator =
                static_cast<uint64_t>(var.pixclock) *
                (var.xres + var.left_margin + var.right_margin + var.hsync_len) *
                (var.yres + var.upper_margin + var.lower_margin + var.vsync_len);
        if (denominator)
            mMode.refreshRateMilliHz = static_cast<uint64_t>(1000000000000000ULL) / denominator;
    }
    if (!mMode.refreshRateMilliHz) {
        ALOGE("Framebuffer supplies no timing; set device refresh rate property");
        return false;
    }
    if (var.width > 0 && var.width != UINT32_MAX)
        mMode.xdpiMilli = static_cast<uint64_t>(var.xres) * 25400 / var.width;
    if (var.height > 0 && var.height != UINT32_MAX)
        mMode.ydpiMilli = static_cast<uint64_t>(var.yres) * 25400 / var.height;
    mFimc.reset(open("/dev/video2", O_RDWR | O_CLOEXEC));
    if (mFimc.get() >= 0) {
        v4l2_capability capability{};
        if (ioctl(mFimc.get(), VIDIOC_QUERYCAP, &capability) < 0 ||
            !(capability.capabilities & V4L2_CAP_STREAMING) ||
            !(capability.capabilities & V4L2_CAP_VIDEO_OUTPUT)) {
            ALOGW("/dev/video2 is not a usable FIMC output node");
            mFimc.reset();
        } else {
            v4l2_format initial{};
            initial.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
            v4l2_control mode{};
            mode.id = V4L2_CID_OVLY_MODE;
            mode.value = FIMC_OVLY_NONE_MULTI_BUF;
            if (ioctl(mFimc.get(), VIDIOC_G_FMT, &initial) < 0 ||
                ioctl(mFimc.get(), VIDIOC_S_CTRL, &mode) < 0) {
                ALOGW("/dev/video2 does not support FIMC DMA overlay mode: %s", strerror(errno));
                mFimc.reset();
            }
        }
    }
    return true;
}

int DisplayBackend::convertVideo(const native_handle_t* source, int acquireFence,
                                 uint32_t cropX, uint32_t cropY, uint32_t cropWidth,
                                 uint32_t cropHeight, const native_handle_t* destination,
                                 uint32_t width, uint32_t height) {
    if (mFimc.get() < 0 || !validateHandle(source) || !validateHandle(destination) ||
        !source->data[kPaddr] || !destination->data[kPaddr] ||
        source->data[kOffset] < 0 ||
        static_cast<uint64_t>(cropX) + cropWidth > static_cast<uint32_t>(source->data[kWidth]) ||
        static_cast<uint64_t>(cropY) + cropHeight > static_cast<uint32_t>(source->data[kHeight]) ||
        destination->data[kWidth] < static_cast<int>(width) ||
        destination->data[kHeight] != static_cast<int>(height) ||
        destination->data[kStride] != destination->data[kWidth] ||
        destination->data[kFormat] !=
                static_cast<int32_t>(aidl::android::hardware::graphics::common::PixelFormat::BGRA_8888))
        return -EINVAL;
    const uint64_t sourceEnd = static_cast<uint32_t>(source->data[kPaddr]) +
                               static_cast<uint32_t>(source->data[kOffset]) +
                               static_cast<uint32_t>(source->data[kSize]);
    if (sourceEnd > UINT32_MAX) return -EINVAL;
    uint32_t inputFormat;
    uint32_t inputHeight = source->data[kHeight];
    using aidl::android::hardware::graphics::common::PixelFormat;
    switch (source->data[kFormat]) {
        case kExynosI420:
            inputFormat = V4L2_PIX_FMT_YUV420;
            // This planar buffer has a padded luma plane.  FIMC's original
            // input size must describe that allocation, while S_CROP below
            // retains the visible picture (for example 360 of 384 lines).
            inputHeight = static_cast<uint32_t>(source->data[kUoffset]) /
                          static_cast<uint32_t>(source->data[kStride]);
            break;
        case static_cast<int32_t>(PixelFormat::YCBCR_420_888):
        case kExynosNv12:
            inputFormat = V4L2_PIX_FMT_NV12M;
            break;
        case static_cast<int32_t>(PixelFormat::YCRCB_420_SP):
            inputFormat = V4L2_PIX_FMT_NV21;
            break;
        default:
            return -EINVAL;
    }
    if (acquireFence >= 0 && sync_wait(acquireFence, 3000) < 0) return -errno;
    const int fd = mFimc.get();
    auto issue = [fd](unsigned long request, void* value) -> int {
        return ioctl(fd, request, value) < 0 ? -errno : 0;
    };
    v4l2_requestbuffers buffers{};
    buffers.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    buffers.memory = V4L2_MEMORY_USERPTR;
    if (int rc = issue(VIDIOC_REQBUFS, &buffers); rc != 0) return rc;

    v4l2_format input{};
    input.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    input.fmt.pix.width = source->data[kStride];
    input.fmt.pix.height = inputHeight;
    input.fmt.pix.pixelformat = inputFormat;
    input.fmt.pix.field = V4L2_FIELD_NONE;
    if (int rc = issue(VIDIOC_S_FMT, &input); rc != 0) return rc;
    v4l2_crop crop{};
    crop.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    crop.c.left = cropX;
    crop.c.top = cropY;
    crop.c.width = cropWidth;
    crop.c.height = cropHeight;
    if (int rc = issue(VIDIOC_S_CROP, &crop); rc != 0) return rc;
    buffers.count = 1;
    if (int rc = issue(VIDIOC_REQBUFS, &buffers); rc != 0) return rc;
    if (buffers.count != 1) return -ENOBUFS;
    for (const auto [id, value] : {
                 std::pair<uint32_t, int32_t>{V4L2_CID_ROTATION, 0},
                 std::pair<uint32_t, int32_t>{V4L2_CID_HFLIP, 0},
                 std::pair<uint32_t, int32_t>{V4L2_CID_VFLIP, 0},
                 std::pair<uint32_t, int32_t>{V4L2_CID_OVLY_MODE, FIMC_OVLY_NONE_MULTI_BUF}}) {
        v4l2_control control{};
        control.id = id;
        control.value = value;
        if (int rc = issue(VIDIOC_S_CTRL, &control); rc != 0) return rc;
    }
    v4l2_framebuffer framebuffer{};
    if (int rc = issue(VIDIOC_G_FBUF, &framebuffer); rc != 0) return rc;
    framebuffer = {};
    framebuffer.base = reinterpret_cast<void*>(
            static_cast<uintptr_t>(static_cast<uint32_t>(destination->data[kPaddr])));
    // The kernel chooses BT.601/BT.709 from the original DMA canvas width.
    // Keep the actual scaled output window below, so FIMD scans only the
    // visible pixels while the row stride selects the correct CSC matrix.
    framebuffer.fmt.width = destination->data[kStride];
    framebuffer.fmt.height = height;
    framebuffer.fmt.pixelformat = V4L2_PIX_FMT_RGB32;
    if (int rc = issue(VIDIOC_S_FBUF, &framebuffer); rc != 0) return rc;
    fimc_buf destinationInfo{};
    destinationInfo.base[0] = static_cast<uint32_t>(destination->data[kPaddr]);
    v4l2_control destinationControl{};
    destinationControl.id = V4L2_CID_DST_INFO;
    destinationControl.value = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&destinationInfo));
    if (int rc = issue(VIDIOC_S_CTRL, &destinationControl); rc != 0) return rc;
    v4l2_format output{};
    output.type = V4L2_BUF_TYPE_VIDEO_OVERLAY;
    output.fmt.win.w.width = width;
    output.fmt.win.w.height = height;
    if (int rc = issue(VIDIOC_S_FMT, &output); rc != 0) return rc;
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    if (int rc = issue(VIDIOC_STREAMON, &type); rc != 0) return rc;
    fimc_buf sourceInfo{};
    const uint32_t base = static_cast<uint32_t>(source->data[kPaddr]) +
                          static_cast<uint32_t>(source->data[kOffset]);
    sourceInfo.base[0] = base;
    sourceInfo.base[1] = base + source->data[kUoffset];
    sourceInfo.base[2] = sourceInfo.base[1] + source->data[kVoffset];
    v4l2_buffer buffer{};
    buffer.type = type;
    buffer.memory = V4L2_MEMORY_USERPTR;
    buffer.index = 0;
    buffer.m.userptr = reinterpret_cast<unsigned long>(&sourceInfo);
    int rc = issue(VIDIOC_QBUF, &buffer);
    if (rc == 0) rc = issue(VIDIOC_DQBUF, &buffer);
    const int stopRc = issue(VIDIOC_STREAMOFF, &type);
    if (rc == 0) rc = stopRc;
    if (rc != 0) ALOGE("FIMC conversion failed: %d", rc);
    return rc;
}

int DisplayBackend::presentClientTarget(const native_handle_t* h, int acquireFence,
                                        android::base::unique_fd* presentFence) {
    if (mFramebuffer.get() < 0 || !presentFence || !validateHandle(h)) {
        ALOGE("Cannot present: framebuffer unavailable or invalid Gralloc5 handle");
        return -EINVAL;
    }
    if (!(h->data[kFlags] & 0x100) || h->data[kPaddr] == 0 ||
        h->data[kWidth] < static_cast<int>(mMode.width) ||
        h->data[kHeight] < static_cast<int>(mMode.height)) {
        ALOGE("Non-scanout client target: flags=0x%x paddr=0x%x "
              "buffer=%dx%d display=%ux%u usage=0x%x",
              h->data[kFlags], static_cast<unsigned>(h->data[kPaddr]),
              h->data[kWidth], h->data[kHeight], mMode.width, mMode.height, h->data[kUsage]);
        return -EINVAL;
    }
    s3c_fb_pixel_format format;
    if (!displayFormat(h->data[kFormat], &format)) return -EINVAL;
    if (acquireFence >= 0 && sync_wait(acquireFence, 3000) < 0) {
        const int savedErrno = errno;
        ALOGE("GPU client-target acquire fence did not signal: fd=%d errno=%d (%s)",
              acquireFence, savedErrno, strerror(savedErrno));
        return -savedErrno;
    }

    s3c_fb_win_config_data request{};
    request.fence = -1;
    for (auto& win : request.config) {
        win.state = s3c_fb_win_config::S3C_FB_WIN_STATE_DISABLED;
        win.fence_fd = -1;
    }
    auto& win = request.config[0];
    win.state = s3c_fb_win_config::S3C_FB_WIN_STATE_BUFFER;
    win.fd = h->data[kFd];
    win.phys_addr = static_cast<uint32_t>(h->data[kPaddr]);
    win.offset = 0;
    win.stride = h->data[kStride] * h->data[kBpp];
    win.format = format;
    win.blending = S3C_FB_BLENDING_NONE;
    win.plane_alpha = 255;
    win.x = 0;
    win.y = 0;
    win.w = mMode.width;
    win.h = mMode.height;
    if (ioctl(mFramebuffer.get(), S3CFB_WIN_CONFIG, &request) < 0) {
        const int savedErrno = errno;
        ALOGE("S3CFB_WIN_CONFIG failed: errno=%d (%s) "
              "phys=0x%x strideBytes=%u format=%d",
              savedErrno, strerror(savedErrno), win.phys_addr, win.stride, win.format);
        return -savedErrno;
    }
    if (request.fence < 0) {
        ALOGE("FIMD accepted frame without a completion fence");
        return -EIO;
    }
    presentFence->reset(request.fence);
    return 0;
}

int DisplayBackend::presentWindows(const std::vector<Window>& windows,
                                   android::base::unique_fd* presentFence) {
    if (mFramebuffer.get() < 0 || !presentFence || windows.size() > S3C_FB_MAX_WIN)
        return -EINVAL;

    s3c_fb_win_config_data request{};
    request.fence = -1;
    for (auto& win : request.config) {
        win.state = s3c_fb_win_config::S3C_FB_WIN_STATE_DISABLED;
        win.fence_fd = -1;
    }
    for (size_t i = 0; i < windows.size(); ++i) {
        const Window& source = windows[i];
        auto& win = request.config[i];
        if (!source.width || !source.height || source.x < 0 || source.y < 0 ||
            static_cast<uint64_t>(source.x) + source.width > mMode.width ||
            static_cast<uint64_t>(source.y) + source.height > mMode.height ||
            (i == 0 && (source.blending != S3C_FB_BLENDING_NONE || source.planeAlpha != 255)))
            return -EINVAL;
        win.x = source.x;
        win.y = source.y;
        win.w = source.width;
        win.h = source.height;
        const native_handle_t* h = source.buffer;
        if (!validateHandle(h) || !(h->data[kFlags] & 0x100) || h->data[kPaddr] == 0)
            return -EINVAL;
        s3c_fb_pixel_format format;
        if (!displayFormat(h->data[kFormat], &format) ||
            static_cast<uint64_t>(source.sourceX) + source.width >
                    static_cast<uint32_t>(h->data[kWidth]) ||
            static_cast<uint64_t>(source.sourceY) + source.height >
                    static_cast<uint32_t>(h->data[kHeight]))
            return -EINVAL;
        const uint64_t stride = static_cast<uint64_t>(h->data[kStride]) * h->data[kBpp];
        const uint64_t offset = static_cast<uint64_t>(source.sourceY) * stride +
                                static_cast<uint64_t>(source.sourceX) * h->data[kBpp];
        if (stride > UINT32_MAX || offset > UINT32_MAX ||
            offset + (static_cast<uint64_t>(source.height) - 1) * stride +
                    static_cast<uint64_t>(source.width) * h->data[kBpp] >
                    static_cast<uint32_t>(h->data[kSize]))
            return -EINVAL;
        if (source.acquireFence >= 0 && sync_wait(source.acquireFence, 3000) < 0)
            return -errno;
        win.state = s3c_fb_win_config::S3C_FB_WIN_STATE_BUFFER;
        win.fd = h->data[kFd];
        win.phys_addr = static_cast<uint32_t>(h->data[kPaddr]);
        win.offset = static_cast<uint32_t>(offset);
        win.stride = static_cast<uint32_t>(stride);
        win.format = format;
        win.blending = static_cast<s3c_fb_blending>(source.blending);
        win.plane_alpha = source.planeAlpha;
    }
    if (ioctl(mFramebuffer.get(), S3CFB_WIN_CONFIG, &request) < 0) {
        const int error = errno;
        ALOGE("S3CFB_WIN_CONFIG (%zu windows) failed: %s", windows.size(), strerror(error));
        return -error;
    }
    if (request.fence < 0) return -EIO;
    presentFence->reset(request.fence);
    return 0;
}

int DisplayBackend::setPower(bool on) {
    if (mFramebuffer.get() < 0) return -ENODEV;
    return ioctl(mFramebuffer.get(), FBIOBLANK, on ? FB_BLANK_UNBLANK : FB_BLANK_POWERDOWN) < 0
                   ? -errno
                   : 0;
}

int DisplayBackend::setBrightness(float brightness) {
    if (!isfinite(brightness) || brightness < 0.0f || brightness > 1.0f) return -EINVAL;

    if (mBacklightMax == 0) {
        int maxFd = open("/sys/class/backlight/panel/max_brightness", O_RDONLY | O_CLOEXEC);
        if (maxFd < 0) return -errno;
        char maxText[16]{};
        const ssize_t count = read(maxFd, maxText, sizeof(maxText) - 1);
        const int readError = errno;
        close(maxFd);
        if (count <= 0) return count < 0 ? -readError : -EIO;
        unsigned int maximum = 0;
        if (sscanf(maxText, "%u", &maximum) != 1 || maximum == 0) return -EINVAL;
        mBacklightMax = maximum;
    }
    if (mBacklight.get() < 0) {
        mBacklight.reset(open("/sys/class/backlight/panel/brightness", O_WRONLY | O_CLOEXEC));
        if (mBacklight.get() < 0) return -errno;
    }

    const unsigned int level = lroundf(brightness * mBacklightMax);
    char levelText[16];
    const int length = snprintf(levelText, sizeof(levelText), "%u\n", level);
    if (length <= 0 || length >= static_cast<int>(sizeof(levelText))) return -EINVAL;
    // sysfs attributes parse one write per call from offset zero.
    const ssize_t written = pwrite(mBacklight.get(), levelText, length, 0);
    const int writeError = errno;
    if (written != length) {
        // Drop the cached descriptor so the next update starts clean.
        mBacklight.reset();
        return written < 0 ? -writeError : -EIO;
    }
    return 0;
}

int DisplayBackend::setVsyncEnabled(bool enabled) {
    if (mFramebuffer.get() < 0) return -ENODEV;
    unsigned int state = enabled ? 1 : 0;
    return ioctl(mFramebuffer.get(), S3CFB_SET_VSYNC_INT, &state) < 0 ? -errno : 0;
}

}  // namespace exynos4::composer3
