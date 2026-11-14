#define LOG_TAG "exynos4-composer3"
#include "ComposerClient.h"
#include <aidl/android/hardware/graphics/common/DisplayHotplugEvent.h>
#include <aidl/android/hardware/graphics/common/BufferUsage.h>
#include <aidl/android/hardware/graphics/common/PixelFormat.h>
#include <aidlcommonsupport/NativeHandle.h>
#include <android/binder_ibinder_platform.h>
#include <cutils/properties.h>
#include <fcntl.h>
#include <log/log.h>
#include <poll.h>
#include <sync/sync.h>
#include <unistd.h>
#include <algorithm>
#include <limits>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include "backend/BufferManager.h"

namespace exynos4::composer3 {
namespace {
// SurfaceFlinger reserves BufferQueue::NUM_BUFFER_SLOTS (64) plus one slot
// for every Composer3 layer, including batched layer creation.
constexpr int32_t kMaxLayerBufferSlots = 65;

ndk::ScopedAStatus unsupported() {
    return ndk::ScopedAStatus::fromServiceSpecificError(IComposerClient::EX_UNSUPPORTED);
}
ndk::ScopedAStatus badDisplay() {
    return ndk::ScopedAStatus::fromServiceSpecificError(IComposerClient::EX_BAD_DISPLAY);
}
ndk::ScopedAStatus badParameter() {
    return ndk::ScopedAStatus::fromServiceSpecificError(IComposerClient::EX_BAD_PARAMETER);
}
}  // namespace

ComposerClient::ComposerClient() = default;
ComposerClient::~ComposerClient() {
    mStopVsync = true;
    if (mVsyncThread.joinable()) mVsyncThread.join();
    for (auto* h : mTargets) {
        if (h) {
            native_handle_close(h);
            native_handle_delete(h);
        }
    }
    for (auto& [_, layer] : mLayers) clearLayer(layer);
    for (auto* h : mPendingRetire) {
        native_handle_close(h);
        native_handle_delete(h);
    }
    for (auto& item : mRetiredHandles) {
        native_handle_close(item.handle);
        native_handle_delete(item.handle);
    }
    for (auto& item : mFimcScratch) {
        native_handle_close(item.handle);
        native_handle_delete(item.handle);
    }
}
bool ComposerClient::initialize() {
    if (!mDisplay.openDisplay()) return false;
    switch (property_get_int32("ro.vendor.exynos4.display_orientation", 0)) {
        case 0:
            mOrientation = common::Transform::NONE;
            break;
        case 90:
            mOrientation = common::Transform::ROT_90;
            break;
        case 180:
            mOrientation = common::Transform::ROT_180;
            break;
        case 270:
            mOrientation = common::Transform::ROT_270;
            break;
        default:
            ALOGE("Invalid display orientation property");
            return false;
    }
    char path[PROPERTY_VALUE_MAX];
    property_get("ro.vendor.exynos4.vsync_time_path", path,
                 "/sys/devices/platform/samsung-pd.2/s3cfb.0/vsync_time");
    mVsyncFd.reset(open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK));
    if (mVsyncFd.get() < 0) {
        ALOGE("Cannot open kernel vsync source %s: %s", path, strerror(errno));
        return false;
    }
    char initialTimestamp[64];
    (void)read(mVsyncFd.get(), initialTimestamp, sizeof(initialTimestamp));
    mVsyncThread = std::thread(&ComposerClient::vsyncLoop, this);
    return true;
}

ndk::ScopedAStatus ComposerClient::createLayer(int64_t displayId, int32_t bufferSlotCount,
                                               int64_t* layer) {
    if (displayId != 0) return badDisplay();
    if (!layer || bufferSlotCount <= 0 || bufferSlotCount > kMaxLayerBufferSlots)
        return badParameter();
    std::lock_guard lock(mMutex);
    *layer = mNextLayer++;
    mLayers[*layer].slots.resize(bufferSlotCount);
    mPendingChanges.clear();
    mValidated = false;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::createVirtualDisplay(int32_t width, int32_t height,
                                                        common::PixelFormat formatHint,
                                                        int32_t outputBufferSlotCount,
                                                        VirtualDisplay* display) {
    return unsupported();
}

ndk::ScopedAStatus ComposerClient::destroyLayer(int64_t displayId, int64_t layer) {
    if (displayId != 0) return badDisplay();
    std::lock_guard lock(mMutex);
    auto it = mLayers.find(layer);
    if (it == mLayers.end())
        return ndk::ScopedAStatus::fromServiceSpecificError(IComposerClient::EX_BAD_LAYER);
    clearLayer(it->second);
    mLayers.erase(it);
    mPendingChanges.clear();
    mValidated = false;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::destroyVirtualDisplay(int64_t displayId) {
    return unsupported();
}

void ComposerClient::clearLayer(Layer& layer) {
    for (auto& slot : layer.slots) {
        if (slot.handle) {
            mPendingRetire.push_back(slot.handle);
            slot.handle = nullptr;
        }
    }
    layer.acquireFence.reset();
    layer.slot = -1;
}

void ComposerClient::collectRetiredHandles() {
    auto it = mRetiredHandles.begin();
    while (it != mRetiredHandles.end()) {
        if (it->fence.get() < 0 || sync_wait(it->fence.get(), 0) != 0) {
            ++it;
            continue;
        }
        native_handle_close(it->handle);
        native_handle_delete(it->handle);
        it = mRetiredHandles.erase(it);
    }
}

bool ComposerClient::isVideoLayer(const Layer& layer) const {
    if (layer.composition != Composition::DEVICE || layer.slot < 0 ||
        static_cast<size_t>(layer.slot) >= layer.slots.size()) return false;
    const native_handle_t* h = layer.slots[layer.slot].handle;
    if (!exynos4::gralloc5::validateHandle(h)) return false;
    const int format = h->data[exynos4::gralloc5::kFormat];
    return format == exynos4::gralloc5::kExynosI420 ||
           format == static_cast<int>(common::PixelFormat::YCBCR_420_888) ||
           format == static_cast<int>(common::PixelFormat::YCRCB_420_SP) ||
           format == exynos4::gralloc5::kExynosNv12;
}

native_handle_t* ComposerClient::acquireFimcScratch(uint32_t width, uint32_t height) {
    for (auto& scratch : mFimcScratch) {
        if (scratch.displayed) continue;
        if (scratch.retireFence.get() >= 0) {
            if (sync_wait(scratch.retireFence.get(), 0) != 0) continue;
            scratch.retireFence.reset();
        }
        if (scratch.handle->data[exynos4::gralloc5::kWidth] == static_cast<int>(width) &&
            scratch.handle->data[exynos4::gralloc5::kHeight] == static_cast<int>(height))
            return scratch.handle;
    }
    if (mFimcScratch.size() >= 3) {
        for (auto it = mFimcScratch.begin(); it != mFimcScratch.end(); ++it) {
            if (it->displayed ||
                (it->retireFence.get() >= 0 && sync_wait(it->retireFence.get(), 0) != 0))
                continue;
            native_handle_close(it->handle);
            native_handle_delete(it->handle);
            mFimcScratch.erase(it);
            break;
        }
        if (mFimcScratch.size() >= 3) return nullptr;
    }
    exynos4::gralloc5::BufferLayout layout;
    constexpr int32_t format = static_cast<int32_t>(common::PixelFormat::BGRA_8888);
    constexpr uint64_t usage = static_cast<uint64_t>(common::BufferUsage::COMPOSER_OVERLAY);
    if (!exynos4::gralloc5::computeLayout(width, height, format, &layout)) return nullptr;
    native_handle_t* h = exynos4::gralloc5::allocateBuffer(width, height, format, usage,
                                                           layout, "Exynos4 FIMC overlay");
    if (!h) return nullptr;
    mFimcScratch.emplace_back();
    mFimcScratch.back().handle = h;
    return h;
}

bool ComposerClient::canScanout(const Layer& layer, bool bottom) const {
    if (mDisableDeviceComposition || mHasDisplayColorTransform || mHasDisplayPictureProfile ||
        layer.hasColorTransform || layer.hasLuts ||
        layer.hasHdrMetadata || layer.hasPictureProfile ||
        !layer.hasDisplayFrame ||
        (layer.composition != Composition::SOLID_COLOR && !layer.hasSourceCrop) ||
        layer.transform != common::Transform::NONE ||
        !std::isfinite(layer.planeAlpha) || layer.planeAlpha <= 0.0f ||
        layer.planeAlpha > 1.0f)
        return false;
    const auto& frame = layer.displayFrame;
    if (frame.left < 0 || frame.top < 0 || frame.right <= frame.left ||
        frame.bottom <= frame.top ||
        static_cast<uint32_t>(frame.right) > mDisplay.mode().width ||
        static_cast<uint32_t>(frame.bottom) > mDisplay.mode().height)
        return false;
    // Without an opaque full-screen bottom plane, pixels outside a device
    // layer would contain stale data from the prior frame.
    if (bottom && (frame.left != 0 || frame.top != 0 ||
                   static_cast<uint32_t>(frame.right) != mDisplay.mode().width ||
                   static_cast<uint32_t>(frame.bottom) != mDisplay.mode().height ||
                   layer.blendMode != common::BlendMode::NONE || layer.planeAlpha != 1.0f))
        return false;
    // This FIMD driver accepts COLOR windows but never writes their WINMAP
    // register. Compositing them as DEVICE would scan address zero.
    if (layer.composition == Composition::SOLID_COLOR) return false;
    if (layer.hasBrightness || layer.composition != Composition::DEVICE || layer.slot < 0 ||
        static_cast<size_t>(layer.slot) >= layer.slots.size())
        return false;
    const native_handle_t* h = layer.slots[layer.slot].handle;
    if (!exynos4::gralloc5::validateHandle(h) || !(h->data[exynos4::gralloc5::kFlags] & 0x100) ||
        h->data[exynos4::gralloc5::kPaddr] == 0)
        return false;
    using common::PixelFormat;
    const auto& crop = layer.sourceCrop;
    if (!std::isfinite(crop.left) || !std::isfinite(crop.top) ||
        !std::isfinite(crop.right) || !std::isfinite(crop.bottom) ||
        crop.left < 0 || crop.top < 0 || crop.left != std::floor(crop.left) ||
        crop.top != std::floor(crop.top) || crop.right != std::floor(crop.right) ||
        crop.bottom != std::floor(crop.bottom) ||
        crop.right <= crop.left || crop.bottom <= crop.top ||
        crop.right > h->data[exynos4::gralloc5::kWidth] ||
        crop.bottom > h->data[exynos4::gralloc5::kHeight])
        return false;
    if (isVideoLayer(layer)) {
        if (layer.dataspace != common::Dataspace::UNKNOWN &&
            layer.dataspace != common::Dataspace::BT601_625 &&
            layer.dataspace != common::Dataspace::BT601_525 &&
            layer.dataspace != common::Dataspace::BT709)
            return false;
        const uint32_t width = frame.right - frame.left;
        const uint32_t height = frame.bottom - frame.top;
        // FIMC selects its CSC matrix from the DMA canvas width, not the
        // scaled output window.  BT.709 can therefore use a 1280-pixel canvas
        // even when the visible window is narrower.  A wide BT.601 window
        // cannot use the required 601 matrix on this kernel.
        if (width >= 1280 && layer.dataspace != common::Dataspace::UNKNOWN &&
            layer.dataspace != common::Dataspace::BT709)
            return false;
        const uint32_t sourceWidth = static_cast<uint32_t>(crop.right - crop.left);
        const uint32_t sourceHeight = static_cast<uint32_t>(crop.bottom - crop.top);
        // FIMC without input rotation accepts an even visible height such as
        // 360. The planar allocation's padded height is supplied separately
        // by DisplayBackend when configuring the DMA input format. RGB32 DMA
        // output has no 16-pixel width restriction; Shorts commonly scales
        // to a 404-pixel window, and rejecting that forces GPU composition.
        return mDisplay.hasFimc() && layer.blendMode == common::BlendMode::NONE &&
               layer.planeAlpha == 1.0f && width >= 32 && height >= 8 &&
               sourceWidth >= 16 && sourceHeight >= 8 &&
               height % 2 == 0 &&
               static_cast<int>(crop.left) % 2 == 0 && static_cast<int>(crop.top) % 2 == 0 &&
               sourceWidth % 2 == 0 && sourceHeight % 2 == 0 &&
               width <= static_cast<uint64_t>(sourceWidth) * 8 &&
               height <= static_cast<uint64_t>(sourceHeight) * 8 &&
               sourceWidth <= static_cast<uint64_t>(width) * 8 &&
               sourceHeight <= static_cast<uint64_t>(height) * 8;
    }
    switch (static_cast<PixelFormat>(h->data[exynos4::gralloc5::kFormat])) {
        case PixelFormat::RGBA_8888:
        case PixelFormat::RGBX_8888:
        case PixelFormat::RGB_888:
        case PixelFormat::RGB_565:
        case PixelFormat::BGRA_8888:
            break;
        default:
            return false;
    }
    if (layer.dataspace != common::Dataspace::UNKNOWN &&
        layer.dataspace != common::Dataspace::SRGB) return false;
    if (crop.right - crop.left != frame.right - frame.left ||
        crop.bottom - crop.top != frame.bottom - frame.top ||
        crop.right > h->data[exynos4::gralloc5::kWidth])
        return false;
    const int bpp = h->data[exynos4::gralloc5::kBpp];
    const int alignment = 32 / (bpp * 8);
    if (frame.left % alignment || frame.right % alignment ||
        (frame.right - frame.left) * bpp < 128)
        return false;
    if (layer.blendMode != common::BlendMode::NONE &&
        layer.blendMode != common::BlendMode::PREMULTIPLIED &&
        layer.blendMode != common::BlendMode::COVERAGE)
        return false;
    if (layer.blendMode == common::BlendMode::NONE && layer.planeAlpha != 1.0f)
        return false;
    const uint64_t stride = static_cast<uint64_t>(h->data[exynos4::gralloc5::kStride]) * bpp;
    const uint64_t offset = static_cast<uint64_t>(crop.top) * stride +
                            static_cast<uint64_t>(crop.left) * bpp;
    if (stride > UINT32_MAX || offset > UINT32_MAX ||
        offset + (static_cast<uint64_t>(frame.bottom - frame.top) - 1) * stride +
                static_cast<uint64_t>(frame.right - frame.left) * bpp >
                static_cast<uint32_t>(h->data[exynos4::gralloc5::kSize]))
        return false;
    return true;
}

void ComposerClient::validate(std::vector<CommandResultPayload>* results) {
    std::vector<std::pair<int64_t, Layer*>> ordered;
    for (auto& [id, layer] : mLayers) ordered.emplace_back(id, &layer);
    std::stable_sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
        return a.second->z < b.second->z;
    });
    mDeviceLayers.clear();
    mPendingChanges.clear();
    mValidatedGeneration = mStateGeneration;
    const size_t count = ordered.size();
    std::vector<char> scanout(count, 0);
    std::vector<char> video(count, 0);
    for (size_t i = 0; i < count; ++i) {
        scanout[i] = canScanout(*ordered[i].second, false);
        video[i] = isVideoLayer(*ordered[i].second);
    }
    const bool scanoutAsBottom = count > 0 && canScanout(*ordered[0].second, true);
    size_t bestBegin = 0, bestEnd = count;
    size_t bestDeviceCount = 0;
    unsigned int bestFimcLayers = 0;
    bool found = false;
    // One client target is a single FIMD window. Its layers must therefore
    // occupy one contiguous Z-order interval; eligible DEVICE windows may be
    // below and above that interval, subject to the five-window limit.
    for (size_t begin = 0; begin <= std::min(count, size_t{5}); ++begin) {
        for (size_t end = std::max(begin, count > 5 ? count - 5 : size_t{0});
             end <= count; ++end) {
            const bool needsClient = end != begin;
            const size_t deviceCount = count - (end - begin);
            const bool deviceAtBottom = count > 0 && (begin > 0 || !needsClient);
            if (deviceAtBottom && !scanoutAsBottom)
                continue;
            if (deviceCount + (needsClient ? 1 : 0) > 5)
                continue;
            bool eligible = true;
            unsigned int fimcLayers = 0;
            for (size_t i = 0; i < count; ++i) {
                if (i >= begin && i < end) continue;
                if (!(i == 0 && deviceAtBottom ? scanoutAsBottom : scanout[i])) {
                    eligible = false;
                    break;
                }
                if (needsClient && i < begin &&
                    (ordered[i].second->blendMode != common::BlendMode::NONE ||
                     ordered[i].second->planeAlpha != 1.0f)) {
                    eligible = false;
                    break;
                }
                if (video[i] && ++fimcLayers > 1) {
                    eligible = false;
                    break;
                }
            }
            if (eligible && (!found || deviceCount > bestDeviceCount ||
                             (deviceCount == bestDeviceCount && fimcLayers > bestFimcLayers))) {
                found = true;
                bestBegin = begin;
                bestEnd = end;
                bestDeviceCount = deviceCount;
                bestFimcLayers = fimcLayers;
            }
        }
    }
    mNeedsClientTarget = bestEnd != bestBegin;
    mClientTargetInsertIndex = bestBegin;
    for (size_t i = 0; i < count; ++i) {
        if (i < bestBegin || i >= bestEnd) {
            mDeviceLayers.push_back(ordered[i].first);
        } else if (ordered[i].second->composition != Composition::CLIENT) {
            mPendingChanges.emplace(ordered[i].first, Composition::CLIENT);
        }
    }
    ALOGV("validated %zu layers: %zu DEVICE, %zu CLIENT, target at %zu",
          count, mDeviceLayers.size(), bestEnd - bestBegin, mClientTargetInsertIndex);
    mValidated = mPendingChanges.empty();
    if (!mPendingChanges.empty()) {
        ChangedCompositionTypes changes;
        changes.display = 0;
        for (const auto& [id, type] : mPendingChanges) {
            ChangedCompositionLayer change;
            change.layer = id;
            change.composition = type;
            changes.layers.push_back(change);
        }
        results->emplace_back(std::move(changes));
    }
    if (mNeedsClientTarget && mClientTargetInsertIndex > 0) {
        DisplayRequest requests;
        requests.display = 0;
        requests.mask = 0;
        for (size_t i = 0; i < mClientTargetInsertIndex; ++i) {
            DisplayRequest::LayerRequest clear;
            clear.layer = mDeviceLayers[i];
            clear.mask = DisplayRequest::LayerRequest::CLEAR_CLIENT_TARGET;
            requests.layerRequests.push_back(clear);
        }
        results->emplace_back(std::move(requests));
    }
}

int ComposerClient::present(std::vector<CommandResultPayload>* results) {
    if (!mValidated) return IComposerClient::EX_NOT_VALIDATED;
    std::vector<DisplayBackend::Window> windows;
    windows.reserve(mDeviceLayers.size() + 1);
    native_handle_t* currentFimcScratch = nullptr;
    for (size_t layerIndex = 0; layerIndex < mDeviceLayers.size(); ++layerIndex) {
        int64_t id = mDeviceLayers[layerIndex];
        if (mNeedsClientTarget && layerIndex == mClientTargetInsertIndex) {
            DisplayBackend::Window client;
            if (mTargetSlot < 0 || static_cast<size_t>(mTargetSlot) >= mTargets.size() ||
                !mTargets[mTargetSlot]) return IComposerClient::EX_BAD_PARAMETER;
            client.buffer = mTargets[mTargetSlot];
            client.acquireFence = mTargetAcquireFence.get();
            client.width = mDisplay.mode().width;
            client.height = mDisplay.mode().height;
            client.blending = windows.empty() ? 0 : 1;
            windows.push_back(client);
        }
        auto& layer = mLayers.at(id);
        DisplayBackend::Window win;
        win.x = layer.displayFrame.left;
        win.y = layer.displayFrame.top;
        win.width = layer.displayFrame.right - layer.displayFrame.left;
        win.height = layer.displayFrame.bottom - layer.displayFrame.top;
        win.planeAlpha = static_cast<uint8_t>(std::lround(layer.planeAlpha * 255));
        switch (layer.blendMode) {
            case common::BlendMode::PREMULTIPLIED: win.blending = 1; break;
            case common::BlendMode::COVERAGE: win.blending = 2; break;
            default: win.blending = 0; break;
        }
        if (isVideoLayer(layer)) {
            const uint32_t canvasWidth =
                    layer.dataspace == common::Dataspace::BT709
                            ? std::max(win.width, uint32_t{1280}) : win.width;
            currentFimcScratch = acquireFimcScratch(canvasWidth, win.height);
            if (!currentFimcScratch) {
                ALOGE("No free FIMC scanout buffer; disabling FIMC for CLIENT fallback");
                mDisplay.disableFimc();
                mValidated = false;
                return IComposerClient::EX_NO_RESOURCES;
            }
            const int conversion = mDisplay.convertVideo(
                    layer.slots[layer.slot].handle, layer.acquireFence.get(),
                    static_cast<uint32_t>(layer.sourceCrop.left),
                    static_cast<uint32_t>(layer.sourceCrop.top),
                    static_cast<uint32_t>(layer.sourceCrop.right - layer.sourceCrop.left),
                    static_cast<uint32_t>(layer.sourceCrop.bottom - layer.sourceCrop.top),
                    currentFimcScratch, win.width, win.height);
            if (conversion != 0) {
                ALOGE("FIMC failed for layer %lld: %d; falling back on next validation",
                      static_cast<long long>(id), conversion);
                mDisplay.disableFimc();
                mValidated = false;
                return IComposerClient::EX_NO_RESOURCES;
            }
            win.buffer = currentFimcScratch;
        } else {
            win.buffer = layer.slots[layer.slot].handle;
            win.acquireFence = layer.acquireFence.get();
            win.sourceX = static_cast<uint32_t>(layer.sourceCrop.left);
            win.sourceY = static_cast<uint32_t>(layer.sourceCrop.top);
        }
        windows.push_back(win);
    }
    if (mNeedsClientTarget && mClientTargetInsertIndex == mDeviceLayers.size()) {
        if (mTargetSlot < 0 || static_cast<size_t>(mTargetSlot) >= mTargets.size() ||
            !mTargets[mTargetSlot])
            return IComposerClient::EX_BAD_PARAMETER;
        DisplayBackend::Window win;
        win.buffer = mTargets[mTargetSlot];
        win.acquireFence = mTargetAcquireFence.get();
        win.width = mDisplay.mode().width;
        win.height = mDisplay.mode().height;
        win.blending = windows.empty() ? 0 : 1;
        windows.push_back(win);
    }
    // A client target can be submitted before the first HWC layer exists.
    // Do not replace that valid GPU frame with an empty FIMD configuration.
    if (windows.empty() && mLayers.empty() && mClientTargetUpdated) {
        if (mTargetSlot < 0 || static_cast<size_t>(mTargetSlot) >= mTargets.size() ||
            !mTargets[mTargetSlot]) return IComposerClient::EX_BAD_PARAMETER;
        DisplayBackend::Window client;
        client.buffer = mTargets[mTargetSlot];
        client.acquireFence = mTargetAcquireFence.get();
        client.width = mDisplay.mode().width;
        client.height = mDisplay.mode().height;
        windows.push_back(client);
    }
    if (windows.empty() && !mLayers.empty()) return IComposerClient::EX_BAD_PARAMETER;
    android::base::unique_fd fence;
    const int rc = mDisplay.presentWindows(windows, &fence);
    if (rc != 0) {
        if (rc == -EINVAL && !mDeviceLayers.empty()) {
            ALOGE("FIMD rejected DEVICE layout; switching to CLIENT composition");
            mDisableDeviceComposition = true;
            mValidated = false;
        }
        return rc == -EINVAL ? IComposerClient::EX_BAD_PARAMETER
                             : IComposerClient::EX_NO_RESOURCES;
    }

    if (mDisplayedFimcScratch != currentFimcScratch) {
        for (auto& scratch : mFimcScratch) {
            if (scratch.handle == mDisplayedFimcScratch) {
                scratch.displayed = false;
                scratch.retireFence.reset(dup(fence.get()));
                if (scratch.retireFence.get() < 0 && sync_wait(fence.get(), 3000) != 0)
                    scratch.displayed = true;  // Retain an untracked scanout allocation.
            }
            if (scratch.handle == currentFimcScratch) scratch.displayed = true;
        }
        mDisplayedFimcScratch = currentFimcScratch;
    }

    for (auto* h : mPendingRetire) {
        RetiredHandle retired;
        retired.handle = h;
        retired.fence.reset(dup(fence.get()));
        mRetiredHandles.push_back(std::move(retired));
    }
    mPendingRetire.clear();
    collectRetiredHandles();

    ReleaseFences releases;
    releases.display = 0;
    // The newly committed FIMD fence retires buffers from the prior frame;
    // the current buffers must not be released while they are being scanned.
    // SurfaceFlinger needs a fence for every previously scanned-out layer,
    // even when that layer reuses the same allocation in the new frame.
    std::set<int64_t> nextDisplayed;
    for (int64_t id : mDeviceLayers) {
        auto& layer = mLayers.at(id);
        // FIMC has already finished reading the YUV source at VIDIOC_DQBUF
        // in convertVideo(). Only the RGB scratch buffer is scanned by FIMD.
        // Do not attach the next FIMD commit fence to the previous YUV buffer:
        // it can unnecessarily withhold a decoder output slot if scanout
        // stops or the next commit is delayed.
        if (layer.composition == Composition::DEVICE && !isVideoLayer(layer))
            nextDisplayed.insert(id);
        layer.acquireFence.reset();
    }
    for (int64_t id : mDisplayedDeviceLayers) {
        if (mLayers.contains(id)) {
            ReleaseFences::Layer release;
            release.layer = id;
            release.fence = ndk::ScopedFileDescriptor(dup(fence.get()));
            releases.layers.push_back(std::move(release));
        }
    }
    mDisplayedDeviceLayers = std::move(nextDisplayed);
    mTargetAcquireFence.reset();
    mClientTargetUpdated = false;
    PresentFence presentFence;
    presentFence.display = 0;
    presentFence.fence = ndk::ScopedFileDescriptor(fence.release());
    results->emplace_back(std::move(presentFence));
    if (!releases.layers.empty()) results->emplace_back(std::move(releases));
    return 0;
}

ndk::ScopedAStatus ComposerClient::executeCommands(const std::vector<DisplayCommand>& commands,
                                                   std::vector<CommandResultPayload>* results) {
    if (!results) return badParameter();
    results->clear();
    auto error = [&](size_t index, int code, const char* reason = "command",
                     int64_t layerId = -1, int32_t bufferSlot = -1) {
        const auto now = std::chrono::steady_clock::now();
        if (code == IComposerClient::EX_BAD_PARAMETER &&
            now - mLastBadParameterLog >= std::chrono::seconds(1)) {
            mLastBadParameterLog = now;
            const auto& command = commands[index];
            ALOGE("Rejected Composer3 command %zu (%s, layer=%lld bufferSlot=%d): "
                  "layers=%zu target=%d validate=%d present=%d "
                  "presentOrValidate=%d slot=%d/%zu",
                  index, reason, static_cast<long long>(layerId), bufferSlot,
                  command.layers.size(), command.clientTarget.has_value(),
                  command.validateDisplay, command.presentDisplay,
                  command.presentOrValidateDisplay, mTargetSlot, mTargets.size());
        }
        CommandError result;
        result.commandIndex = static_cast<int32_t>(index);
        result.errorCode = code;
        results->emplace_back(std::move(result));
    };
    std::lock_guard lock(mMutex);
    collectRetiredHandles();

    for (size_t index = 0; index < commands.size(); ++index) {
        const auto& command = commands[index];
        if (command.display != 0) {
            error(index, IComposerClient::EX_BAD_DISPLAY);
            continue;
        }
        if (command.virtualDisplayOutputBuffer) {
            error(index, IComposerClient::EX_UNSUPPORTED);
            continue;
        }
        if (!command.layers.empty() || command.colorTransformMatrix ||
            mHasDisplayPictureProfile != (command.pictureProfileId != 0))
            ++mStateGeneration;
        if (mHasDisplayPictureProfile != (command.pictureProfileId != 0)) {
            mHasDisplayPictureProfile = command.pictureProfileId != 0;
            mValidated = false;
        }
        if (command.brightness) {
            const float brightness = command.brightness->brightness;
            if (!std::isfinite(brightness) || brightness < 0.0f || brightness > 1.0f) {
                error(index, IComposerClient::EX_BAD_PARAMETER, "display brightness");
                continue;
            }
            const int rc = mDisplay.setBrightness(brightness);
            if (rc != 0) {
                ALOGE("Cannot set panel brightness: %d", rc);
                error(index, IComposerClient::EX_NO_RESOURCES);
                continue;
            }
        }
        bool failed = false;
        if (command.colorTransformMatrix) {
            const auto& matrix = *command.colorTransformMatrix;
            if (matrix.size() != 16) {
                error(index, IComposerClient::EX_BAD_PARAMETER, "display color matrix size");
                continue;
            }
            bool identity = true;
            bool finite = true;
            for (size_t i = 0; i < 16; ++i) {
                finite &= std::isfinite(matrix[i]);
                identity &= matrix[i] == ((i % 5 == 0) ? 1.0f : 0.0f);
            }
            if (!finite) {
                error(index, IComposerClient::EX_BAD_PARAMETER, "display color matrix value");
                continue;
            }
            mHasDisplayColorTransform = !identity;
            mValidated = false;
        }
        for (const auto& layerCommand : command.layers) {
            if (layerCommand.layerLifecycleBatchCommandType ==
                LayerLifecycleBatchCommandType::CREATE) {
                if (layerCommand.layer == std::numeric_limits<int64_t>::max() ||
                    mLayers.contains(layerCommand.layer) ||
                    layerCommand.newBufferSlotCount <= 0 ||
                    layerCommand.newBufferSlotCount > kMaxLayerBufferSlots) {
                    error(index, IComposerClient::EX_BAD_PARAMETER, "layer create",
                          layerCommand.layer);
                    failed = true;
                    break;
                }
                mLayers[layerCommand.layer].slots.resize(layerCommand.newBufferSlotCount);
                mNextLayer = std::max(mNextLayer, layerCommand.layer + 1);
                mValidated = false;
            }
            auto it = mLayers.find(layerCommand.layer);
            if (it == mLayers.end()) {
                error(index, IComposerClient::EX_BAD_LAYER);
                failed = true;
                break;
            }
            if (layerCommand.layerLifecycleBatchCommandType ==
                LayerLifecycleBatchCommandType::DESTROY) {
                clearLayer(it->second);
                mLayers.erase(it);
                mPendingChanges.clear();
                mValidated = false;
                continue;
            }
            Layer& layer = it->second;
            const Composition requestedComposition = layerCommand.composition
                    ? layerCommand.composition->composition : layer.composition;
            if (layerCommand.sidebandStream && requestedComposition == Composition::SIDEBAND) {
                error(index, IComposerClient::EX_UNSUPPORTED);
                failed = true;
                break;
            }
            if (layerCommand.composition) {
                layer.composition = layerCommand.composition->composition;
                mValidated = false;
            }
            if (layerCommand.z) { layer.z = layerCommand.z->z; mValidated = false; }
            if (layerCommand.displayFrame) {
                layer.displayFrame = *layerCommand.displayFrame;
                layer.hasDisplayFrame = true;
                mValidated = false;
            }
            if (layerCommand.sourceCrop) {
                layer.sourceCrop = *layerCommand.sourceCrop;
                layer.hasSourceCrop = true;
                mValidated = false;
            }
            if (layerCommand.blendMode) {
                layer.blendMode = layerCommand.blendMode->blendMode;
                mValidated = false;
            }
            if (layerCommand.transform) {
                layer.transform = layerCommand.transform->transform;
                mValidated = false;
            }
            if (layerCommand.dataspace) {
                layer.dataspace = layerCommand.dataspace->dataspace;
                mValidated = false;
            }
            if (layerCommand.planeAlpha) {
                if (!std::isfinite(layerCommand.planeAlpha->alpha) ||
                    layerCommand.planeAlpha->alpha < 0.0f ||
                    layerCommand.planeAlpha->alpha > 1.0f) {
                    error(index, IComposerClient::EX_BAD_PARAMETER, "plane alpha",
                          layerCommand.layer);
                    failed = true;
                    break;
                }
                layer.planeAlpha = layerCommand.planeAlpha->alpha;
                mValidated = false;
            }
            // setLayerColor must have no effect for non-SOLID_COLOR layers.
            if (layerCommand.color && requestedComposition == Composition::SOLID_COLOR) {
                const auto& c = *layerCommand.color;
                if (!std::isfinite(c.r) || !std::isfinite(c.g) || !std::isfinite(c.b) ||
                    !std::isfinite(c.a) || c.r < 0 || c.r > 1 || c.g < 0 || c.g > 1 ||
                    c.b < 0 || c.b > 1 || c.a < 0 || c.a > 1) {
                    error(index, IComposerClient::EX_BAD_PARAMETER, "solid color",
                          layerCommand.layer);
                    failed = true;
                    break;
                }
                layer.color = (static_cast<uint32_t>(std::lround(c.r * 255)) << 16) |
                              (static_cast<uint32_t>(std::lround(c.g * 255)) << 8) |
                              static_cast<uint32_t>(std::lround(c.b * 255));
                layer.colorAlpha = c.a;
                mValidated = false;
            }
            if (layerCommand.colorTransform) {
                const auto& matrix = *layerCommand.colorTransform;
                if (matrix.size() != 16) {
                    error(index, IComposerClient::EX_BAD_PARAMETER, "layer color matrix size",
                          layerCommand.layer);
                    failed = true;
                    break;
                }
                bool identity = true;
                bool finite = true;
                for (size_t i = 0; i < 16; ++i) {
                    finite &= std::isfinite(matrix[i]);
                    identity &= matrix[i] == ((i % 5 == 0) ? 1.0f : 0.0f);
                }
                if (!finite) {
                    error(index, IComposerClient::EX_BAD_PARAMETER, "layer color matrix value",
                          layerCommand.layer);
                    failed = true;
                    break;
                }
                layer.hasColorTransform = !identity;
                mValidated = false;
            }
            if (layerCommand.luts) {
                layer.hasLuts = !layerCommand.luts->lutProperties.empty();
                mValidated = false;
            }
            if (layerCommand.perFrameMetadata || layerCommand.perFrameMetadataBlob) {
                layer.hasHdrMetadata =
                        (layerCommand.perFrameMetadata &&
                         !layerCommand.perFrameMetadata->empty()) ||
                        (layerCommand.perFrameMetadataBlob &&
                         !layerCommand.perFrameMetadataBlob->empty());
                mValidated = false;
            }
            if (layerCommand.pictureProfileId != 0) {
                layer.hasPictureProfile = true;
                mValidated = false;
            }
            if (layerCommand.brightness) {
                const float brightness = layerCommand.brightness->brightness;
                if (!std::isfinite(brightness) || brightness < 0.0f || brightness > 1.0f) {
                    error(index, IComposerClient::EX_BAD_PARAMETER, "layer brightness",
                          layerCommand.layer);
                    failed = true;
                    break;
                }
                layer.hasBrightness = brightness != 1.0f;
                mValidated = false;
            }
            if (layerCommand.bufferSlotsToClear) {
                for (int32_t slot : *layerCommand.bufferSlotsToClear) {
                    // SurfaceFlinger uses UINT32_MAX when the active buffer was
                    // never cached. There is no cache entry to release then.
                    if (slot == -1) continue;
                    if (slot < 0 || static_cast<size_t>(slot) >= layer.slots.size()) {
                        error(index, IComposerClient::EX_BAD_PARAMETER, "clear layer slot",
                              layerCommand.layer, slot);
                        failed = true;
                        break;
                    }
                }
                if (failed) break;
                for (int32_t slot : *layerCommand.bufferSlotsToClear) {
                    if (slot == -1) continue;
                    auto& cached = layer.slots[slot];
                    cached.cached = false;
                    if (layer.slot != slot && cached.handle) {
                        mPendingRetire.push_back(cached.handle);
                        cached.handle = nullptr;
                    }
                    // Clearing the cache must not discard the active buffer.
                    // It remains available for the current presentation only.
                }
                mValidated = false;
            }
            // The cache belongs to the layer, not to its current composition
            // type. Retain even a non-scanout handle received while CLIENT so
            // a later DEVICE transition cannot accidentally reuse an older
            // buffer from the same slot. CLIENT buffer updates must not fail
            // the command or affect the current presentation.
            if (layerCommand.buffer &&
                (layer.composition == Composition::DEVICE ||
                 layer.composition == Composition::CLIENT)) {
                const auto& buffer = *layerCommand.buffer;
                auto retireUncachedActive = [&](bool keepBufferSlot) {
                    if (layer.slot < 0 || static_cast<size_t>(layer.slot) >= layer.slots.size() ||
                        (keepBufferSlot && layer.slot == buffer.slot)) return;
                    auto& previous = layer.slots[layer.slot];
                    if (!previous.cached && previous.handle) {
                        mPendingRetire.push_back(previous.handle);
                        previous.handle = nullptr;
                    }
                };
                if (buffer.slot < 0 || static_cast<size_t>(buffer.slot) >= layer.slots.size()) {
                    if (layer.composition != Composition::DEVICE) continue;
                    error(index, IComposerClient::EX_BAD_PARAMETER, "layer buffer slot",
                          layerCommand.layer, buffer.slot);
                    failed = true;
                    break;
                }
                if (buffer.handle) {
                    native_handle_t* imported = android::dupFromAidl(*buffer.handle);
                    if (!imported) {
                        if (layer.composition != Composition::DEVICE) {
                            auto& cached = layer.slots[buffer.slot];
                            if (cached.handle) {
                                mPendingRetire.push_back(cached.handle);
                                cached.handle = nullptr;
                            }
                            cached.cached = false;
                            retireUncachedActive(false);
                            layer.slot = -1;
                            layer.acquireFence.reset();
                            continue;
                        }
                        error(index, IComposerClient::EX_BAD_PARAMETER, "layer handle import",
                              layerCommand.layer, buffer.slot);
                        failed = true;
                        break;
                    }
                    const bool scanoutHandle = exynos4::gralloc5::validateHandle(imported);
                    const native_handle_t* previous =
                            layer.slot >= 0 ? layer.slots[layer.slot].handle : nullptr;
                    if (!scanoutHandle ||
                        (previous && !exynos4::gralloc5::validateHandle(previous))) {
                        // Retain the buffer reference, but do not attempt FIMD
                        // scanout of an incompatible Exynos allocation.
                        if (layer.composition == Composition::DEVICE) mValidated = false;
                    } else if (previous &&
                        (previous->data[exynos4::gralloc5::kWidth] !=
                                 imported->data[exynos4::gralloc5::kWidth] ||
                         previous->data[exynos4::gralloc5::kHeight] !=
                                 imported->data[exynos4::gralloc5::kHeight] ||
                         previous->data[exynos4::gralloc5::kFormat] !=
                                 imported->data[exynos4::gralloc5::kFormat] ||
                         previous->data[exynos4::gralloc5::kStride] !=
                                 imported->data[exynos4::gralloc5::kStride] ||
                         (previous->data[exynos4::gralloc5::kPaddr] != 0) !=
                                 (imported->data[exynos4::gralloc5::kPaddr] != 0)))
                        if (layer.composition == Composition::DEVICE) mValidated = false;
                    auto& cached = layer.slots[buffer.slot].handle;
                    if (cached) mPendingRetire.push_back(cached);
                    cached = imported;
                    layer.slots[buffer.slot].cached = true;
                } else if (!layer.slots[buffer.slot].cached ||
                           !layer.slots[buffer.slot].handle) {
                    if (layer.composition == Composition::CLIENT) {
                        retireUncachedActive(false);
                        layer.slot = -1;
                        layer.acquireFence.reset();
                        continue;
                    }
                    error(index, IComposerClient::EX_BAD_PARAMETER, "uncached device buffer",
                          layerCommand.layer, buffer.slot);
                    failed = true;
                    break;
                }
                retireUncachedActive(true);
                layer.slot = buffer.slot;
                layer.acquireFence.reset(buffer.fence.get() < 0 ? -1 : dup(buffer.fence.get()));
                if (buffer.fence.get() >= 0 && layer.acquireFence.get() < 0) {
                    if (layer.composition == Composition::CLIENT) {
                        layer.slot = -1;
                        continue;
                    }
                    error(index, IComposerClient::EX_NO_RESOURCES);
                    failed = true;
                    break;
                }
            }
        }
        if (failed) continue;
        if (command.clientTarget) {
            const auto& buffer = command.clientTarget->buffer;
            android::base::unique_fd newAcquire;
            if (buffer.fence.get() >= 0) {
                newAcquire.reset(dup(buffer.fence.get()));
                if (newAcquire.get() < 0) {
                    error(index, IComposerClient::EX_NO_RESOURCES);
                    continue;
                }
            }
            if (buffer.slot < 0 || static_cast<size_t>(buffer.slot) >= mTargets.size()) {
                error(index, IComposerClient::EX_BAD_PARAMETER);
                continue;
            }
            if (buffer.handle) {
                native_handle_t* imported = android::dupFromAidl(*buffer.handle);
                if (!imported || !exynos4::gralloc5::validateHandle(imported)) {
                    if (imported) {
                        native_handle_close(imported);
                        native_handle_delete(imported);
                    }
                    error(index, IComposerClient::EX_BAD_PARAMETER);
                    continue;
                }
                // Keep the former ION allocation alive until the next FIMD
                // commit fence retires the frame that was scanning it.
                if (mTargets[buffer.slot]) {
                    mPendingRetire.push_back(mTargets[buffer.slot]);
                }
                mTargets[buffer.slot] = imported;
            } else if (!mTargets[buffer.slot]) {
                error(index, IComposerClient::EX_BAD_PARAMETER);
                continue;
            }
            mTargetSlot = buffer.slot;
            mTargetAcquireFence = std::move(newAcquire);
            mClientTargetUpdated = true;
        }
        if (command.validateDisplay) validate(results);
        if (command.acceptDisplayChanges) {
            if (mValidatedGeneration != mStateGeneration ||
                (!mValidated && mPendingChanges.empty())) {
                mPendingChanges.clear();
                error(index, IComposerClient::EX_NOT_VALIDATED);
                continue;
            }
            bool validPendingLayers = true;
            for (const auto& pending : mPendingChanges)
                validPendingLayers &= mLayers.contains(pending.first);
            if (!validPendingLayers) {
                mPendingChanges.clear();
                error(index, IComposerClient::EX_NOT_VALIDATED);
                continue;
            }
            for (const auto& [id, type] : mPendingChanges) mLayers.at(id).composition = type;
            mPendingChanges.clear();
            mValidated = true;
        }
        if (command.presentOrValidateDisplay) {
            PresentOrValidate result;
            result.display = command.display;
            if (mValidated) {
                const int rc = present(results);
                if (rc == 0) {
                    result.result = PresentOrValidate::Result::Presented;
                } else if (!mValidated) {
                    // A failed DEVICE commit invalidates its plan.  Return a
                    // fresh CLIENT plan through the normal Composer3 validate
                    // lifecycle, rather than leaving SurfaceFlinger with an
                    // error and no valid composition for this frame.
                    validate(results);
                    result.result = PresentOrValidate::Result::Validated;
                } else {
                    error(index, rc);
                    continue;
                }
            } else {
                validate(results);
                result.result = PresentOrValidate::Result::Validated;
            }
            results->emplace_back(result);
        }
        if (command.presentDisplay) {
            const int rc = present(results);
            if (rc != 0) error(index, rc);
        }
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getActiveConfig(int64_t displayId, int32_t* config) {
    if (displayId != 0) return badDisplay();
    if (!config) return badParameter();
    *config = 0;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getColorModes(int64_t displayId,
                                                 std::vector<ColorMode>* colorModes) {
    if (displayId != 0) return badDisplay();
    if (!colorModes) return badParameter();
    *colorModes = {ColorMode::NATIVE};
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getDataspaceSaturationMatrix(common::Dataspace dataspace,
                                                                std::vector<float>* matrix) {
    if (dataspace != common::Dataspace::SRGB_LINEAR || !matrix) return badParameter();
    *matrix = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getDisplayAttribute(int64_t displayId, int32_t config,
                                                       DisplayAttribute attribute, int32_t* value) {
    if (displayId != 0) return badDisplay();
    if (config != 0)
        return ndk::ScopedAStatus::fromServiceSpecificError(IComposerClient::EX_BAD_CONFIG);
    if (!value) return badParameter();
    const auto& mode = mDisplay.mode();
    switch (attribute) {
        case DisplayAttribute::WIDTH:
            *value = mode.width;
            break;
        case DisplayAttribute::HEIGHT:
            *value = mode.height;
            break;
        case DisplayAttribute::VSYNC_PERIOD:
            *value = 1000000000000LL / mode.refreshRateMilliHz;
            break;
        case DisplayAttribute::DPI_X:
            if (!mode.xdpiMilli) return unsupported();
            *value = mode.xdpiMilli;
            break;
        case DisplayAttribute::DPI_Y:
            if (!mode.ydpiMilli) return unsupported();
            *value = mode.ydpiMilli;
            break;
        case DisplayAttribute::CONFIG_GROUP:
            *value = 0;
            break;
        default:
            return badParameter();
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getDisplayCapabilities(int64_t displayId,
                                                          std::vector<DisplayCapability>* caps) {
    if (displayId != 0) return badDisplay();
    if (!caps) return badParameter();
    caps->clear();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getDisplayConfigs(int64_t displayId,
                                                     std::vector<int32_t>* configs) {
    if (displayId != 0) return badDisplay();
    if (!configs) return badParameter();
    *configs = {0};
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getDisplayConnectionType(int64_t displayId,
                                                            DisplayConnectionType* type) {
    if (displayId != 0) return badDisplay();
    if (!type) return badParameter();
    *type = DisplayConnectionType::INTERNAL;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getDisplayIdentificationData(int64_t displayId,
                                                                DisplayIdentification* id) {
    return unsupported();
}

ndk::ScopedAStatus ComposerClient::getDisplayName(int64_t displayId, std::string* name) {
    if (displayId != 0) return badDisplay();
    if (!name) return badParameter();
    *name = "Exynos4 FIMD";
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getDisplayVsyncPeriod(int64_t displayId, int32_t* vsyncPeriod) {
    if (displayId != 0) return badDisplay();
    if (!vsyncPeriod) return badParameter();
    *vsyncPeriod = 1000000000000LL / mDisplay.mode().refreshRateMilliHz;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getDisplayedContentSample(int64_t displayId, int64_t maxFrames,
                                                             int64_t timestamp,
                                                             DisplayContentSample* samples) {
    return unsupported();
}

ndk::ScopedAStatus ComposerClient::getDisplayedContentSamplingAttributes(
        int64_t displayId, DisplayContentSamplingAttributes* attrs) {
    return unsupported();
}

ndk::ScopedAStatus ComposerClient::getDisplayPhysicalOrientation(int64_t displayId,
                                                                 common::Transform* orientation) {
    if (displayId != 0) return badDisplay();
    if (!orientation) return badParameter();
    *orientation = mOrientation;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getHdrCapabilities(int64_t displayId, HdrCapabilities* caps) {
    if (displayId != 0) return badDisplay();
    if (!caps) return badParameter();
    *caps = HdrCapabilities{};
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getOverlaySupport(OverlayProperties* properties) {
    if (!properties) return badParameter();
    std::lock_guard lock(mMutex);
    *properties = OverlayProperties{};
    if (mDisableDeviceComposition) return ndk::ScopedAStatus::ok();
    OverlayProperties::SupportedBufferCombinations rgb;
    rgb.pixelFormats = {common::PixelFormat::RGBA_8888, common::PixelFormat::RGBX_8888,
                        common::PixelFormat::RGB_888, common::PixelFormat::RGB_565,
                        common::PixelFormat::BGRA_8888};
    rgb.standards = {common::Dataspace::STANDARD_BT709};
    rgb.transfers = {common::Dataspace::TRANSFER_SRGB};
    rgb.ranges = {common::Dataspace::RANGE_FULL};
    properties->combinations.push_back(std::move(rgb));
    if (mDisplay.hasFimc()) {
        OverlayProperties::SupportedBufferCombinations video;
        video.pixelFormats = {common::PixelFormat::YCBCR_420_888,
                              common::PixelFormat::YCRCB_420_SP};
        video.standards = {common::Dataspace::STANDARD_BT601_625,
                           common::Dataspace::STANDARD_BT601_525,
                           common::Dataspace::STANDARD_BT709};
        video.transfers = {common::Dataspace::TRANSFER_SMPTE_170M};
        video.ranges = {common::Dataspace::RANGE_LIMITED};
        properties->combinations.push_back(std::move(video));
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getMaxVirtualDisplayCount(int32_t* count) {
    if (!count) return badParameter();
    *count = 0;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getPerFrameMetadataKeys(int64_t displayId,
                                                           std::vector<PerFrameMetadataKey>* keys) {
    if (displayId != 0) return badDisplay();
    if (!keys) return badParameter();
    keys->clear();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getReadbackBufferAttributes(int64_t displayId,
                                                               ReadbackBufferAttributes* attrs) {
    return unsupported();
}

ndk::ScopedAStatus ComposerClient::getReadbackBufferFence(int64_t displayId,
                                                          ndk::ScopedFileDescriptor* acquireFence) {
    return unsupported();
}

ndk::ScopedAStatus ComposerClient::getRenderIntents(int64_t displayId, ColorMode mode,
                                                    std::vector<RenderIntent>* intents) {
    if (displayId != 0) return badDisplay();
    if (mode != ColorMode::NATIVE || !intents) return badParameter();
    *intents = {RenderIntent::COLORIMETRIC};
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getSupportedContentTypes(int64_t displayId,
                                                            std::vector<ContentType>* types) {
    if (displayId != 0) return badDisplay();
    if (!types) return badParameter();
    types->clear();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getDisplayDecorationSupport(
        int64_t displayId, std::optional<common::DisplayDecorationSupport>* support) {
    if (displayId != 0) return badDisplay();
    if (!support) return badParameter();
    support->reset();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::registerCallback(
        const std::shared_ptr<IComposerCallback>& callback) {
    if (!callback) return badParameter();
    {
        std::lock_guard lock(mVsyncStateMutex);
        mCallback = callback;
    }
    std::lock_guard callbackLock(mCallbackMutex);
    return callback->onHotplugEvent(0, common::DisplayHotplugEvent::CONNECTED);
}

ndk::ScopedAStatus ComposerClient::setActiveConfig(int64_t displayId, int32_t config) {
    if (displayId != 0) return badDisplay();
    return config == 0
                   ? ndk::ScopedAStatus::ok()
                   : ndk::ScopedAStatus::fromServiceSpecificError(IComposerClient::EX_BAD_CONFIG);
}

ndk::ScopedAStatus ComposerClient::setActiveConfigWithConstraints(
        int64_t displayId, int32_t config, const VsyncPeriodChangeConstraints& constraints,
        VsyncPeriodChangeTimeline* timeline) {
    if (displayId != 0) return badDisplay();
    if (config != 0)
        return ndk::ScopedAStatus::fromServiceSpecificError(IComposerClient::EX_BAD_CONFIG);
    if (!timeline) return badParameter();
    const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count();
    timeline->newVsyncAppliedTimeNanos = std::max(now, constraints.desiredTimeNanos);
    timeline->refreshRequired = false;
    timeline->refreshTimeNanos = 0;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::setBootDisplayConfig(int64_t displayId, int32_t config) {
    return unsupported();
}

ndk::ScopedAStatus ComposerClient::clearBootDisplayConfig(int64_t displayId) {
    return unsupported();
}

ndk::ScopedAStatus ComposerClient::getPreferredBootDisplayConfig(int64_t displayId,
                                                                 int32_t* config) {
    return unsupported();
}

ndk::ScopedAStatus ComposerClient::getHdrConversionCapabilities(
        std::vector<aidl::android::hardware::graphics::common::HdrConversionCapability>* caps) {
    if (!caps) return badParameter();
    caps->clear();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::setHdrConversionStrategy(
        const aidl::android::hardware::graphics::common::HdrConversionStrategy& conversionStrategy,
        aidl::android::hardware::graphics::common::Hdr* preferredHdrOutputType) {
    return unsupported();
}

ndk::ScopedAStatus ComposerClient::setAutoLowLatencyMode(int64_t displayId, bool on) {
    return unsupported();
}

ndk::ScopedAStatus ComposerClient::setClientTargetSlotCount(int64_t displayId, int32_t count) {
    if (displayId != 0) return badDisplay();
    if (count <= 0 || count > 64) return badParameter();
    std::lock_guard lock(mMutex);
    if (static_cast<size_t>(count) == mTargets.size()) return ndk::ScopedAStatus::ok();
    if (static_cast<size_t>(count) < mTargets.size()) {
        for (size_t i = count; i < mTargets.size(); ++i) {
            if (mTargets[i]) mPendingRetire.push_back(mTargets[i]);
        }
    }
    mTargets.resize(count, nullptr);
    if (mTargetSlot >= count) {
        mTargetSlot = -1;
        mTargetAcquireFence.reset();
        mClientTargetUpdated = false;
        mPendingChanges.clear();
        mValidated = false;
        ++mStateGeneration;
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::setColorMode(int64_t displayId, ColorMode mode,
                                                RenderIntent intent) {
    if (displayId != 0) return badDisplay();
    if (mode != ColorMode::NATIVE || intent != RenderIntent::COLORIMETRIC) return badParameter();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::setContentType(int64_t displayId, ContentType type) {
    if (displayId != 0) return badDisplay();
    return type == ContentType::NONE ? ndk::ScopedAStatus::ok() : unsupported();
}

ndk::ScopedAStatus ComposerClient::setDisplayedContentSamplingEnabled(
        int64_t displayId, bool enable, FormatColorComponent componentMask, int64_t maxFrames) {
    return unsupported();
}

ndk::ScopedAStatus ComposerClient::setPowerMode(int64_t displayId, PowerMode mode) {
    if (displayId != 0) return badDisplay();
    if (mode != PowerMode::OFF && mode != PowerMode::ON) return unsupported();
    std::lock_guard displayLock(mMutex);
    if (mDisplay.setPower(mode == PowerMode::ON) != 0)
        return ndk::ScopedAStatus::fromServiceSpecificError(IComposerClient::EX_NO_RESOURCES);
    std::lock_guard lock(mVsyncStateMutex);
    mPowerOn = mode == PowerMode::ON;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::setReadbackBuffer(
        int64_t displayId, const aidl::android::hardware::common::NativeHandle& buffer,
        const ndk::ScopedFileDescriptor& releaseFence) {
    return unsupported();
}

ndk::ScopedAStatus ComposerClient::setVsyncEnabled(int64_t displayId, bool enabled) {
    if (displayId != 0) return badDisplay();
    std::lock_guard displayLock(mMutex);
    if (mDisplay.setVsyncEnabled(enabled) != 0)
        return ndk::ScopedAStatus::fromServiceSpecificError(IComposerClient::EX_NO_RESOURCES);
    std::lock_guard lock(mVsyncStateMutex);
    mVsyncEnabled = enabled;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::setIdleTimerEnabled(int64_t displayId, int32_t timeoutMs) {
    return unsupported();
}

ndk::ScopedAStatus ComposerClient::setRefreshRateChangedCallbackDebugEnabled(int64_t displayId,
                                                                             bool enabled) {
    return unsupported();
}

ndk::ScopedAStatus ComposerClient::getDisplayConfigurations(
        int64_t displayId, int32_t maxFrameIntervalNs,
        std::vector<DisplayConfiguration>* outConfigs) {
    if (displayId != 0) return badDisplay();
    if (!outConfigs) return badParameter();
    const auto& mode = mDisplay.mode();
    DisplayConfiguration c;
    c.configId = 0;
    c.width = mode.width;
    c.height = mode.height;
    c.configGroup = 0;
    c.vsyncPeriod = 1000000000000LL / mode.refreshRateMilliHz;
    c.hdrOutputType = OutputType::SDR;
    if (mode.xdpiMilli && mode.ydpiMilli) {
        DisplayConfiguration::Dpi dpi;
        dpi.x = mode.xdpiMilli / 1000.0f;
        dpi.y = mode.ydpiMilli / 1000.0f;
        c.dpi = dpi;
    }
    *outConfigs = {c};
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::notifyExpectedPresent(
        int64_t displayId, const ClockMonotonicTimestamp& expectedPresentTime,
        int32_t maxFrameIntervalNs) {
    return unsupported();
}

ndk::ScopedAStatus ComposerClient::getMaxLayerPictureProfiles(int64_t displayId,
                                                              int32_t* outMaxProfiles) {
    if (displayId != 0) return badDisplay();
    if (!outMaxProfiles) return badParameter();
    *outMaxProfiles = 0;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::startHdcpNegotiation(
        int64_t displayId, const aidl::android::hardware::drm::HdcpLevels& levels) {
    return unsupported();
}

ndk::ScopedAStatus ComposerClient::getLuts(int64_t displayId, const std::vector<Buffer>&,
                                           std::vector<Luts>*) {
    return unsupported();
}

void ComposerClient::vsyncLoop() {
    char timestamp[64];
    while (!mStopVsync) {
        pollfd pfd{mVsyncFd.get(), POLLPRI | POLLERR, 0};
        const int rc = poll(&pfd, 1, 250);
        if (rc < 0) {
            if (errno == EINTR) continue;
            ALOGE("Vsync poll failed: %s", strerror(errno));
            break;
        }
        if (rc == 0) continue;
        if (lseek(mVsyncFd.get(), 0, SEEK_SET) < 0) continue;
        const ssize_t len = read(mVsyncFd.get(), timestamp, sizeof(timestamp) - 1);
        if (len <= 0) continue;
        timestamp[len] = '\0';
        const int64_t timeNs = strtoll(timestamp, nullptr, 0);
        if (timeNs <= 0) continue;
        std::shared_ptr<IComposerCallback> callback;
        {
            std::lock_guard lock(mVsyncStateMutex);
            if (mVsyncEnabled && mPowerOn) callback = mCallback;
        }
        if (callback) {
            std::lock_guard callbackLock(mCallbackMutex);
            callback->onVsync(0, timeNs, 1000000000000LL / mDisplay.mode().refreshRateMilliHz);
        }
    }
}
}  // namespace exynos4::composer3
