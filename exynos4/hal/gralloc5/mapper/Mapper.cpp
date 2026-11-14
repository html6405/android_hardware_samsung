#define LOG_TAG "exynos4-mapper5"

#include <aidl/android/hardware/graphics/common/PlaneLayoutComponentType.h>
#include <android-base/unique_fd.h>
#include <android/hardware/graphics/mapper/utils/IMapperMetadataTypes.h>
#include <android/hardware/graphics/mapper/utils/IMapperProvider.h>
#include <drm_fourcc.h>
#include <log/log.h>
#include <sync/sync.h>

#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "backend/BufferManager.h"
#include "secion.h"
#include "ump.h"
#include "ump_ref_drv.h"

namespace exynos4::gralloc5 {

static_assert(sizeof(void*) == 4, "Mali-400 private handles require a 32-bit mapper");

namespace gm = android::hardware::graphics::mapper;
using gm::StandardMetadata;
using gm::StandardMetadataType;

constexpr char kStandardTypeName[] = "android.hardware.graphics.common.StandardMetadataType";

gm::PlaneLayoutComponent component(
        aidl::android::hardware::graphics::common::PlaneLayoutComponentType type, int64_t offset,
        int64_t bits) {
    gm::PlaneLayoutComponent result;
    result.type.name = "android.hardware.graphics.common.PlaneLayoutComponentType";
    result.type.value = static_cast<int64_t>(type);
    result.offsetInBits = offset;
    result.sizeInBits = bits;
    return result;
}

uint32_t fourcc(int format) {
    using gm::PixelFormat;
    if (format == kExynosI420) return DRM_FORMAT_YUV420;
    if (format == kExynosNv12) return DRM_FORMAT_NV12;
    switch (static_cast<PixelFormat>(format)) {
        case PixelFormat::RGBA_8888:
            return DRM_FORMAT_ABGR8888;
        case PixelFormat::RGBX_8888:
            return DRM_FORMAT_XBGR8888;
        case PixelFormat::BGRA_8888:
            return DRM_FORMAT_ARGB8888;
        case PixelFormat::RGB_888:
            return DRM_FORMAT_BGR888;
        case PixelFormat::RGB_565:
            return DRM_FORMAT_RGB565;
        case PixelFormat::YCRCB_420_SP:
            return DRM_FORMAT_NV21;
        case PixelFormat::YCBCR_420_888:
            return DRM_FORMAT_NV21;
        case PixelFormat::YCBCR_422_SP:
            return DRM_FORMAT_NV16;
        case PixelFormat::YV12:
            return DRM_FORMAT_YVU420;
        case PixelFormat::BLOB:
            return DRM_FORMAT_R8;
        default:
            return 0;
    }
}

std::vector<gm::PlaneLayout> planeLayouts(const native_handle_t* h) {
    namespace gc = aidl::android::hardware::graphics::common;
    const int format = h->data[kFormat];
    const int width = h->data[kWidth];
    const int height = h->data[kHeight];
    const int stride = h->data[kStride];
    const int bpp = h->data[kBpp];
    gm::PlaneLayout plane;
    plane.offsetInBytes = 0;
    plane.sampleIncrementInBits = bpp * 8;
    plane.strideInBytes = stride * bpp;
    plane.widthInSamples = width;
    plane.heightInSamples = height;
    plane.totalSizeInBytes = static_cast<int64_t>(stride) * height * bpp;
    plane.horizontalSubsampling = 1;
    plane.verticalSubsampling = 1;
    using T = gc::PlaneLayoutComponentType;
    if (format == kExynosI420) {
        plane.components = {component(T::Y, 0, 8)};
        plane.totalSizeInBytes = static_cast<int64_t>(stride) *
                                 ((static_cast<int64_t>(height) + 31) & ~int64_t{31});
        const int64_t cstride = (stride / 2 + 15) & ~15;
        const int64_t chromaSize = cstride *
                                   ((static_cast<int64_t>(height / 2) + 15) & ~int64_t{15});
        gm::PlaneLayout cb;
        cb.components = {component(T::CB, 0, 8)};
        cb.offsetInBytes = h->data[kUoffset];
        cb.sampleIncrementInBits = 8;
        cb.strideInBytes = cstride;
        cb.widthInSamples = width / 2;
        cb.heightInSamples = height / 2;
        cb.totalSizeInBytes = chromaSize;
        cb.horizontalSubsampling = cb.verticalSubsampling = 2;
        auto cr = cb;
        cr.components = {component(T::CR, 0, 8)};
        cr.offsetInBytes = static_cast<int64_t>(h->data[kUoffset]) + h->data[kVoffset];
        return {plane, cb, cr};
    }
    if (format == kExynosNv12) {
        plane.components = {component(T::Y, 0, 8)};
        gm::PlaneLayout chroma;
        chroma.components = {component(T::CB, 0, 8), component(T::CR, 8, 8)};
        chroma.offsetInBytes = h->data[kUoffset];
        chroma.sampleIncrementInBits = 16;
        chroma.strideInBytes = stride;
        chroma.widthInSamples = width / 2;
        chroma.heightInSamples = height / 2;
        chroma.totalSizeInBytes = static_cast<int64_t>(stride) * height / 2;
        chroma.horizontalSubsampling = 2;
        chroma.verticalSubsampling = 2;
        return {plane, chroma};
    }
    switch (static_cast<gc::PixelFormat>(format)) {
        case gc::PixelFormat::RGBA_8888:
        case gc::PixelFormat::RGBX_8888:
            plane.components = {component(T::R, 0, 8), component(T::G, 8, 8),
                                component(T::B, 16, 8)};
            if (static_cast<gc::PixelFormat>(format) == gc::PixelFormat::RGBA_8888)
                plane.components.push_back(component(T::A, 24, 8));
            break;
        case gc::PixelFormat::BGRA_8888:
            plane.components = {component(T::B, 0, 8), component(T::G, 8, 8),
                                component(T::R, 16, 8), component(T::A, 24, 8)};
            break;
        case gc::PixelFormat::RGB_888:
            plane.components = {component(T::R, 0, 8), component(T::G, 8, 8),
                                component(T::B, 16, 8)};
            break;
        case gc::PixelFormat::RGB_565:
            plane.components = {component(T::B, 0, 5), component(T::G, 5, 6),
                                component(T::R, 11, 5)};
            break;
        case gc::PixelFormat::BLOB:
            plane.components = {component(T::RAW, 0, 8)};
            break;
        case gc::PixelFormat::YCRCB_420_SP:
        case gc::PixelFormat::YCBCR_420_888:
        case gc::PixelFormat::YCBCR_422_SP: {
            plane.components = {component(T::Y, 0, 8)};
            gm::PlaneLayout chroma;
            if (static_cast<gc::PixelFormat>(format) == gc::PixelFormat::YCBCR_422_SP)
                chroma.components = {component(T::CB, 0, 8), component(T::CR, 8, 8)};
            else
                chroma.components = {component(T::CR, 0, 8), component(T::CB, 8, 8)};
            chroma.offsetInBytes = h->data[kUoffset];
            chroma.sampleIncrementInBits = 16;
            chroma.strideInBytes = stride;
            chroma.widthInSamples = width / 2;
            const bool is422 =
                    static_cast<gc::PixelFormat>(format) == gc::PixelFormat::YCBCR_422_SP;
            chroma.heightInSamples = is422 ? height : height / 2;
            chroma.totalSizeInBytes = static_cast<int64_t>(stride) * chroma.heightInSamples;
            chroma.horizontalSubsampling = 2;
            chroma.verticalSubsampling = is422 ? 1 : 2;
            return {plane, chroma};
        }
        case gc::PixelFormat::YV12: {
            plane.components = {component(T::Y, 0, 8)};
            const int64_t cstride = (stride / 2 + 15) & ~15;
            gm::PlaneLayout cr;
            cr.components = {component(T::CR, 0, 8)};
            cr.offsetInBytes = h->data[kUoffset];
            cr.sampleIncrementInBits = 8;
            cr.strideInBytes = cstride;
            cr.widthInSamples = width / 2;
            cr.heightInSamples = height / 2;
            cr.totalSizeInBytes = cstride * height / 2;
            cr.horizontalSubsampling = cr.verticalSubsampling = 2;
            auto cb = cr;
            cb.components = {component(T::CB, 0, 8)};
            cb.offsetInBytes = static_cast<int64_t>(h->data[kUoffset]) + h->data[kVoffset];
            return {plane, cr, cb};
        }
        default:
            return {};
    }
    return {plane};
}

struct ImportState {
    void* address = nullptr;  // UMP-owned mapping, shared by Mali and CPU locks.
    size_t size = 0;
    ump_handle ump = UMP_INVALID_MEMORY_HANDLE;
    bool locked = false;
    bool write = false;
};

class Mapper final : public vendor::mapper::IMapperV5Impl {
  public:
    Mapper() { mUmpReady = ump_open() == UMP_OK; }
    ~Mapper() override {
        for (auto& [buffer, state] : mImports) {
            if (state.ump != UMP_INVALID_MEMORY_HANDLE) {
                ump_mapped_pointer_release(state.ump);
                ump_reference_release(state.ump);
            }
            native_handle_close(const_cast<native_handle_t*>(buffer));
            native_handle_delete(const_cast<native_handle_t*>(buffer));
        }
        mImports.clear();
        if (mUmpReady) ump_close();
    }

    AIMapper_Error importBuffer(const native_handle_t* raw, buffer_handle_t* out) override {
        if (!out) return AIMAPPER_ERROR_BAD_VALUE;
        *out = nullptr;
        if (!mUmpReady || !validateHandle(raw)) {
            ALOGE("Cannot import buffer: UMP unavailable or invalid legacy handle");
            return AIMAPPER_ERROR_BAD_BUFFER;
        }
        native_handle_t* h = native_handle_clone(raw);
        if (!h) return AIMAPPER_ERROR_NO_RESOURCES;
        // All of these legacy fields are process-local. Never trust a Binder copy.
        h->data[kBase] = 0;
        h->data[kLockState] = 0;
        h->data[kWriteOwner] = 0;
        h->data[kPid] = getpid();
        h->data[kUmpHandle] = 0;
        h->data[kIonClient] = 0;
        // The allocator cannot retain a UMP kernel reference after handing
        // out an ION fd: AIDL has no per-buffer free notification. Import the
        // ION fd afresh here and keep this process's UMP reference until free.
        ump_handle ump = ump_ref_drv_ion_import(h->data[kFd], UMP_REF_DRV_CONSTRAINT_USE_CACHE);
        if (ump == UMP_INVALID_MEMORY_HANDLE) {
            ALOGE("UMP rejected Exynos ION buffer import");
            native_handle_close(h);
            native_handle_delete(h);
            return AIMAPPER_ERROR_BAD_BUFFER;
        }
        // Exynos ION share FDs are anon inodes: fstat().st_size is zero.
        // The kernel UMP importer returns the actual ION allocation size.
        if (ump_size_get(ump) < static_cast<unsigned long>(h->data[kSize])) {
            ALOGE("ION allocation smaller than gralloc handle size (%lu < %d)",
                  ump_size_get(ump), h->data[kSize]);
            ump_reference_release(ump);
            native_handle_close(h);
            native_handle_delete(h);
            return AIMAPPER_ERROR_BAD_BUFFER;
        }
        void* maliBase = ump_mapped_pointer_get(ump);
        if (!maliBase) {
            ump_reference_release(ump);
            native_handle_close(h);
            native_handle_delete(h);
            return AIMAPPER_ERROR_NO_RESOURCES;
        }
        h->data[kUmpId] = static_cast<int>(ump_secure_id_get(ump));
        h->data[kUmpHandle] = static_cast<int>(reinterpret_cast<intptr_t>(ump));
        h->data[kBase] = static_cast<int>(reinterpret_cast<intptr_t>(maliBase));
        {
            std::lock_guard guard(mMutex);
            // Use the UMP mapping for CPU access too. A second ION mmap would
            // create a cache alias that UMP's synchronization does not own.
            mImports.emplace(h, ImportState{maliBase, static_cast<size_t>(h->data[kSize]), ump,
                                            false, false});
        }
        *out = h;
        return AIMAPPER_ERROR_NONE;
    }

    AIMapper_Error freeBuffer(buffer_handle_t buffer) override {
        std::lock_guard guard(mMutex);
        auto it = mImports.find(buffer);
        if (it == mImports.end() || it->second.locked) return AIMAPPER_ERROR_BAD_BUFFER;
        ump_mapped_pointer_release(it->second.ump);
        ump_reference_release(it->second.ump);
        native_handle_close(const_cast<native_handle_t*>(buffer));
        native_handle_delete(const_cast<native_handle_t*>(buffer));
        mImports.erase(it);
        return AIMAPPER_ERROR_NONE;
    }

    AIMapper_Error getTransportSize(buffer_handle_t buffer, uint32_t* fds,
                                    uint32_t* ints) override {
        if (!known(buffer) || !fds || !ints) return AIMAPPER_ERROR_BAD_BUFFER;
        *fds = 1;
        *ints = 21;
        return AIMAPPER_ERROR_NONE;
    }

    AIMapper_Error lock(buffer_handle_t buffer, uint64_t cpuUsage, ARect rect, int acquireFence,
                        void** out) override {
        android::base::unique_fd fence(acquireFence);
        if (!out) return AIMAPPER_ERROR_BAD_VALUE;
        *out = nullptr;
        if (fence.get() >= 0 && sync_wait(fence.get(), 3000) < 0)
            return AIMAPPER_ERROR_NO_RESOURCES;
        std::lock_guard guard(mMutex);
        auto it = mImports.find(buffer);
        if (it == mImports.end()) return AIMAPPER_ERROR_BAD_BUFFER;
        ImportState& state = it->second;
        const uint64_t allowedCpuUsage = 0xff;
        const uint64_t allocatedCpuUsage =
                static_cast<uint32_t>(buffer->data[kUsage]) & allowedCpuUsage;
        if (state.locked || cpuUsage == 0 || (cpuUsage & ~allowedCpuUsage) ||
            (cpuUsage & allocatedCpuUsage) != cpuUsage || rect.left < 0 || rect.top < 0 ||
            rect.right < rect.left || rect.bottom < rect.top || rect.right > buffer->data[kWidth] ||
            rect.bottom > buffer->data[kHeight]) {
            ALOGE("Invalid CPU lock: locked=%d usage=0x%llx allocated=0x%llx "
                  "rect=[%d,%d,%d,%d] buffer=%dx%d format=0x%x",
                  state.locked, static_cast<unsigned long long>(cpuUsage),
                  static_cast<unsigned long long>(allocatedCpuUsage), rect.left, rect.top,
                  rect.right, rect.bottom, buffer->data[kWidth], buffer->data[kHeight],
                  buffer->data[kFormat]);
            return AIMAPPER_ERROR_BAD_VALUE;
        }
        // One UMP sync covers both cache levels. The ION msync ioctl flushes
        // all CPU caches for buffers over 1 MiB, and switching UMP ownership
        // would repeat this synchronization on every camera frame.
        ump_cpu_msync_now(state.ump, UMP_MSYNC_INVALIDATE, nullptr, state.size);
        state.write = (cpuUsage & 0xf0) != 0;
        state.locked = true;
        *out = state.address;
        return AIMAPPER_ERROR_NONE;
    }

    AIMapper_Error unlock(buffer_handle_t buffer, int* releaseFence) override {
        if (!releaseFence) return AIMAPPER_ERROR_BAD_VALUE;
        *releaseFence = -1;
        std::lock_guard guard(mMutex);
        auto it = mImports.find(buffer);
        if (it == mImports.end() || !it->second.locked) return AIMAPPER_ERROR_BAD_BUFFER;
        ImportState& state = it->second;
        if (state.write) {
            ump_cpu_msync_now(state.ump, UMP_MSYNC_CLEAN_AND_INVALIDATE, nullptr, state.size);
        }
        state.locked = false;
        *releaseFence = -1;  // Synchronous cache maintenance has completed.
        return AIMAPPER_ERROR_NONE;
    }

    AIMapper_Error flushLockedBuffer(buffer_handle_t buffer) override {
        std::lock_guard guard(mMutex);
        auto it = mImports.find(buffer);
        if (it == mImports.end() || !it->second.locked) return AIMAPPER_ERROR_BAD_BUFFER;
        ump_cpu_msync_now(it->second.ump, UMP_MSYNC_CLEAN, nullptr, it->second.size);
        return AIMAPPER_ERROR_NONE;
    }

    AIMapper_Error rereadLockedBuffer(buffer_handle_t buffer) override {
        std::lock_guard guard(mMutex);
        auto it = mImports.find(buffer);
        if (it == mImports.end() || !it->second.locked) return AIMAPPER_ERROR_BAD_BUFFER;
        ump_cpu_msync_now(it->second.ump, UMP_MSYNC_INVALIDATE, nullptr, it->second.size);
        return AIMAPPER_ERROR_NONE;
    }

    int32_t getMetadata(buffer_handle_t buffer, AIMapper_MetadataType type, void* dest,
                        size_t size) override {
        if (!type.name || strcmp(type.name, kStandardTypeName)) return -AIMAPPER_ERROR_UNSUPPORTED;
        return getStandardMetadata(buffer, type.value, dest, size);
    }

    int32_t getStandardMetadata(buffer_handle_t buffer, int64_t rawType, void* dest,
                                size_t size) override {
        std::lock_guard guard(mMutex);
        auto it = mImports.find(buffer);
        if (it == mImports.end()) return -AIMAPPER_ERROR_BAD_BUFFER;
        const SharedMetadata* metadata = sharedMetadata(buffer, it->second);
        const auto type = static_cast<StandardMetadataType>(rawType);
        switch (type) {
            case StandardMetadataType::BUFFER_ID:
                if (!metadata) return -AIMAPPER_ERROR_UNSUPPORTED;
                return StandardMetadata<StandardMetadataType::BUFFER_ID>::value::encode(
                        metadata->id, dest, size);
            case StandardMetadataType::NAME:
                if (!metadata) return -AIMAPPER_ERROR_UNSUPPORTED;
                return StandardMetadata<StandardMetadataType::NAME>::value::encode(
                        std::string_view(metadata->name,
                                         strnlen(metadata->name, sizeof(metadata->name))),
                        dest, size);
            case StandardMetadataType::WIDTH:
                return StandardMetadata<StandardMetadataType::WIDTH>::value::encode(
                        buffer->data[kWidth], dest, size);
            case StandardMetadataType::HEIGHT:
                return StandardMetadata<StandardMetadataType::HEIGHT>::value::encode(
                        buffer->data[kHeight], dest, size);
            case StandardMetadataType::LAYER_COUNT:
                return StandardMetadata<StandardMetadataType::LAYER_COUNT>::value::encode(
                        uint64_t{1}, dest, size);
            case StandardMetadataType::PIXEL_FORMAT_REQUESTED:
                return StandardMetadata<StandardMetadataType::PIXEL_FORMAT_REQUESTED>::value::
                        encode(static_cast<gm::PixelFormat>(buffer->data[kFormat]), dest, size);
            case StandardMetadataType::PIXEL_FORMAT_FOURCC:
                return StandardMetadata<StandardMetadataType::PIXEL_FORMAT_FOURCC>::value::encode(
                        fourcc(buffer->data[kFormat]), dest, size);
            case StandardMetadataType::USAGE:
                return StandardMetadata<StandardMetadataType::USAGE>::value::encode(
                        static_cast<gm::BufferUsage>(static_cast<uint32_t>(buffer->data[kUsage])),
                        dest, size);
            case StandardMetadataType::ALLOCATION_SIZE: {
                // The final ION page holds private mapper metadata, not image data.
                // Do not expose it as writable buffer capacity to camera/codec clients.
                BufferLayout layout;
                if (!computeLayout(buffer->data[kWidth], buffer->data[kHeight],
                                   buffer->data[kFormat], &layout))
                    return -AIMAPPER_ERROR_BAD_BUFFER;
                return StandardMetadata<StandardMetadataType::ALLOCATION_SIZE>::value::encode(
                        static_cast<uint64_t>(layout.size), dest, size);
            }
            case StandardMetadataType::STRIDE:
                return StandardMetadata<StandardMetadataType::STRIDE>::value::encode(
                        buffer->data[kStride], dest, size);
            case StandardMetadataType::PROTECTED_CONTENT:
                return StandardMetadata<StandardMetadataType::PROTECTED_CONTENT>::value::encode(
                        uint64_t{0}, dest, size);
            case StandardMetadataType::PIXEL_FORMAT_MODIFIER:
                return StandardMetadata<StandardMetadataType::PIXEL_FORMAT_MODIFIER>::value::encode(
                        uint64_t{0}, dest, size);
            case StandardMetadataType::COMPRESSION:
                return StandardMetadata<StandardMetadataType::COMPRESSION>::value::encode(
                        gm::ExtendableType{"android.hardware.graphics.common.Compression", 0}, dest,
                        size);
            case StandardMetadataType::INTERLACED:
                return StandardMetadata<StandardMetadataType::INTERLACED>::value::encode(
                        gm::ExtendableType{"android.hardware.graphics.common.Interlaced", 0}, dest,
                        size);
            case StandardMetadataType::CHROMA_SITING: {
                const auto format = static_cast<gm::PixelFormat>(buffer->data[kFormat]);
                const int64_t siting = format == static_cast<gm::PixelFormat>(kExynosI420) ||
                                                       format == static_cast<gm::PixelFormat>(kExynosNv12) ||
                                                       format == gm::PixelFormat::YCRCB_420_SP ||
                                                       format == gm::PixelFormat::YCBCR_420_888 ||
                                                       format == gm::PixelFormat::YV12
                                               ? 2
                                       : format == gm::PixelFormat::YCBCR_422_SP ? 1
                                                                                 : 0;
                return StandardMetadata<StandardMetadataType::CHROMA_SITING>::value::encode(
                        gm::ExtendableType{"android.hardware.graphics.common.ChromaSiting", siting},
                        dest, size);
            }
            case StandardMetadataType::PLANE_LAYOUTS:
                return StandardMetadata<StandardMetadataType::PLANE_LAYOUTS>::value::encode(
                        planeLayouts(buffer), dest, size);
            case StandardMetadataType::DATASPACE:
                if (!metadata) return -AIMAPPER_ERROR_UNSUPPORTED;
                return StandardMetadata<StandardMetadataType::DATASPACE>::value::encode(
                        static_cast<gm::Dataspace>(metadata->dataspace), dest, size);
            case StandardMetadataType::BLEND_MODE:
                if (!metadata) return -AIMAPPER_ERROR_UNSUPPORTED;
                return StandardMetadata<StandardMetadataType::BLEND_MODE>::value::encode(
                        static_cast<gm::BlendMode>(metadata->blendMode), dest, size);
            case StandardMetadataType::CROP:
                if (!metadata) return -AIMAPPER_ERROR_UNSUPPORTED;
                return StandardMetadata<StandardMetadataType::CROP>::value::encode(
                        std::vector<gm::Rect>{{metadata->cropLeft, metadata->cropTop,
                                               metadata->cropRight, metadata->cropBottom}},
                        dest, size);
            case StandardMetadataType::SMPTE2086:
                return StandardMetadata<StandardMetadataType::SMPTE2086>::value::encode(
                        std::optional<gm::Smpte2086>{}, dest, size);
            case StandardMetadataType::CTA861_3:
                return StandardMetadata<StandardMetadataType::CTA861_3>::value::encode(
                        std::optional<gm::Cta861_3>{}, dest, size);
            case StandardMetadataType::SMPTE2094_10:
                return StandardMetadata<StandardMetadataType::SMPTE2094_10>::value::encode(
                        std::optional<std::vector<uint8_t>>{}, dest, size);
            case StandardMetadataType::SMPTE2094_40:
                return StandardMetadata<StandardMetadataType::SMPTE2094_40>::value::encode(
                        std::optional<std::vector<uint8_t>>{}, dest, size);
            default:
                return -AIMAPPER_ERROR_UNSUPPORTED;
        }
    }

    AIMapper_Error setMetadata(buffer_handle_t buffer, AIMapper_MetadataType type, const void* data,
                               size_t size) override {
        if (!type.name || strcmp(type.name, kStandardTypeName)) return AIMAPPER_ERROR_UNSUPPORTED;
        return setStandardMetadata(buffer, type.value, data, size);
    }

    AIMapper_Error setStandardMetadata(buffer_handle_t buffer, int64_t rawType, const void* data,
                                       size_t size) override {
        if (!data) return AIMAPPER_ERROR_BAD_VALUE;
        std::lock_guard guard(mMutex);
        auto it = mImports.find(buffer);
        if (it == mImports.end()) return AIMAPPER_ERROR_BAD_BUFFER;
        SharedMetadata* metadata = sharedMetadata(buffer, it->second);
        if (!metadata) return AIMAPPER_ERROR_UNSUPPORTED;
        switch (static_cast<StandardMetadataType>(rawType)) {
            case StandardMetadataType::DATASPACE: {
                auto value = StandardMetadata<StandardMetadataType::DATASPACE>::value::decode(data,
                                                                                              size);
                if (!value) return AIMAPPER_ERROR_BAD_VALUE;
                metadata->dataspace = static_cast<int32_t>(*value);
                break;
            }
            case StandardMetadataType::BLEND_MODE: {
                auto value = StandardMetadata<StandardMetadataType::BLEND_MODE>::value::decode(
                        data, size);
                if (!value || *value == gm::BlendMode::INVALID) return AIMAPPER_ERROR_BAD_VALUE;
                metadata->blendMode = static_cast<int32_t>(*value);
                break;
            }
            case StandardMetadataType::CROP: {
                auto value =
                        StandardMetadata<StandardMetadataType::CROP>::value::decode(data, size);
                if (!value || value->size() != 1) return AIMAPPER_ERROR_BAD_VALUE;
                const auto& rect = value->front();
                if (rect.left < 0 || rect.top < 0 || rect.right < rect.left ||
                    rect.bottom < rect.top || rect.right > buffer->data[kWidth] ||
                    rect.bottom > buffer->data[kHeight])
                    return AIMAPPER_ERROR_BAD_VALUE;
                metadata->cropLeft = rect.left;
                metadata->cropTop = rect.top;
                metadata->cropRight = rect.right;
                metadata->cropBottom = rect.bottom;
                break;
            }
            default:
                return AIMAPPER_ERROR_UNSUPPORTED;
        }
        IonClientRef ion;
        const int client = ion.get();
        if (client < 0) return AIMAPPER_ERROR_NO_RESOURCES;
        BufferLayout layout;
        computeLayout(buffer->data[kWidth], buffer->data[kHeight], buffer->data[kFormat], &layout);
        ump_cpu_msync_now(it->second.ump, UMP_MSYNC_CLEAN, metadata, kMetadataPageSize);
        const int rc = ion_msync(client, buffer->data[kFd], IMSYNC_DEV_TO_RW | IMSYNC_SYNC_FOR_DEV,
                                 kMetadataPageSize, metadataOffset(layout));
        return rc < 0 ? AIMAPPER_ERROR_NO_RESOURCES : AIMAPPER_ERROR_NONE;
    }

    AIMapper_Error listSupportedMetadataTypes(const AIMapper_MetadataTypeDescription** list,
                                              size_t* count) override {
        if (!list || !count) return AIMAPPER_ERROR_BAD_VALUE;
        *list = kSupportedTypes;
        *count = sizeof(kSupportedTypes) / sizeof(kSupportedTypes[0]);
        return AIMAPPER_ERROR_NONE;
    }

    AIMapper_Error dumpBuffer(buffer_handle_t buffer, AIMapper_DumpBufferCallback callback,
                              void* context) override {
        if (!known(buffer)) return AIMAPPER_ERROR_BAD_BUFFER;
        if (!callback) return AIMAPPER_ERROR_BAD_VALUE;
        for (const auto& entry : kSupportedTypes) {
            const int32_t length =
                    getStandardMetadata(buffer, entry.metadataType.value, nullptr, 0);
            if (length <= 0) continue;
            std::vector<uint8_t> bytes(length);
            const int32_t written = getStandardMetadata(buffer, entry.metadataType.value,
                                                        bytes.data(), bytes.size());
            if (written == length)
                callback(context, entry.metadataType, bytes.data(), bytes.size());
        }
        return AIMAPPER_ERROR_NONE;
    }

    AIMapper_Error dumpAllBuffers(AIMapper_BeginDumpBufferCallback begin,
                                  AIMapper_DumpBufferCallback callback, void* context) override {
        if (!begin || !callback) return AIMAPPER_ERROR_BAD_VALUE;
        std::vector<buffer_handle_t> handles;
        {
            std::lock_guard guard(mMutex);
            for (const auto& [h, _] : mImports) handles.push_back(h);
        }
        for (auto h : handles) {
            begin(context);
            dumpBuffer(h, callback, context);
        }
        return AIMAPPER_ERROR_NONE;
    }

    AIMapper_Error getReservedRegion(buffer_handle_t buffer, void** region,
                                     uint64_t* size) override {
        if (!known(buffer)) return AIMAPPER_ERROR_BAD_BUFFER;
        if (!region || !size) return AIMAPPER_ERROR_BAD_VALUE;
        *region = nullptr;
        *size = 0;
        return AIMAPPER_ERROR_NONE;
    }

  private:
    SharedMetadata* sharedMetadata(buffer_handle_t buffer, ImportState& state) {
        BufferLayout layout;
        if (!computeLayout(buffer->data[kWidth], buffer->data[kHeight], buffer->data[kFormat],
                           &layout))
            return nullptr;
        const size_t offset = metadataOffset(layout);
        if (offset > state.size || state.size - offset < sizeof(SharedMetadata))
            return nullptr;  // Legacy buffer with no metadata page.
        IonClientRef ion;
        const int client = ion.get();
        if (client < 0) return nullptr;
        const int rc = ion_msync(client, buffer->data[kFd], IMSYNC_DEV_TO_RW | IMSYNC_SYNC_FOR_CPU,
                                 kMetadataPageSize, offset);
        if (rc < 0) return nullptr;
        ump_cpu_msync_now(state.ump, UMP_MSYNC_INVALIDATE,
                          static_cast<uint8_t*>(state.address) + offset, kMetadataPageSize);
        auto* metadata =
                reinterpret_cast<SharedMetadata*>(static_cast<uint8_t*>(state.address) + offset);
        return metadata->magic == kMetadataMagic && metadata->version == 1 ? metadata : nullptr;
    }

    bool known(buffer_handle_t buffer) {
        std::lock_guard guard(mMutex);
        return mImports.find(buffer) != mImports.end();
    }

    static constexpr AIMapper_MetadataTypeDescription kSupportedTypes[] = {
            {{kStandardTypeName, static_cast<int64_t>(StandardMetadataType::BUFFER_ID)},
             nullptr,
             true,
             false,
             {}},
            {{kStandardTypeName, static_cast<int64_t>(StandardMetadataType::NAME)},
             nullptr,
             true,
             false,
             {}},
            {{kStandardTypeName, static_cast<int64_t>(StandardMetadataType::WIDTH)},
             nullptr,
             true,
             false,
             {}},
            {{kStandardTypeName, static_cast<int64_t>(StandardMetadataType::HEIGHT)},
             nullptr,
             true,
             false,
             {}},
            {{kStandardTypeName, static_cast<int64_t>(StandardMetadataType::LAYER_COUNT)},
             nullptr,
             true,
             false,
             {}},
            {{kStandardTypeName,
              static_cast<int64_t>(StandardMetadataType::PIXEL_FORMAT_REQUESTED)},
             nullptr,
             true,
             false,
             {}},
            {{kStandardTypeName, static_cast<int64_t>(StandardMetadataType::PIXEL_FORMAT_FOURCC)},
             nullptr,
             true,
             false,
             {}},
            {{kStandardTypeName, static_cast<int64_t>(StandardMetadataType::USAGE)},
             nullptr,
             true,
             false,
             {}},
            {{kStandardTypeName, static_cast<int64_t>(StandardMetadataType::ALLOCATION_SIZE)},
             nullptr,
             true,
             false,
             {}},
            {{kStandardTypeName, static_cast<int64_t>(StandardMetadataType::STRIDE)},
             nullptr,
             true,
             false,
             {}},
            {{kStandardTypeName, static_cast<int64_t>(StandardMetadataType::PROTECTED_CONTENT)},
             nullptr,
             true,
             false,
             {}},
            {{kStandardTypeName, static_cast<int64_t>(StandardMetadataType::PIXEL_FORMAT_MODIFIER)},
             nullptr,
             true,
             false,
             {}},
            {{kStandardTypeName, static_cast<int64_t>(StandardMetadataType::COMPRESSION)},
             nullptr,
             true,
             false,
             {}},
            {{kStandardTypeName, static_cast<int64_t>(StandardMetadataType::INTERLACED)},
             nullptr,
             true,
             false,
             {}},
            {{kStandardTypeName, static_cast<int64_t>(StandardMetadataType::CHROMA_SITING)},
             nullptr,
             true,
             false,
             {}},
            {{kStandardTypeName, static_cast<int64_t>(StandardMetadataType::PLANE_LAYOUTS)},
             nullptr,
             true,
             false,
             {}},
            {{kStandardTypeName, static_cast<int64_t>(StandardMetadataType::DATASPACE)},
             nullptr,
             true,
             true,
             {}},
            {{kStandardTypeName, static_cast<int64_t>(StandardMetadataType::BLEND_MODE)},
             nullptr,
             true,
             true,
             {}},
            {{kStandardTypeName, static_cast<int64_t>(StandardMetadataType::CROP)},
             nullptr,
             true,
             true,
             {}},
            {{kStandardTypeName, static_cast<int64_t>(StandardMetadataType::SMPTE2086)},
             nullptr,
             true,
             false,
             {}},
            {{kStandardTypeName, static_cast<int64_t>(StandardMetadataType::CTA861_3)},
             nullptr,
             true,
             false,
             {}},
            {{kStandardTypeName, static_cast<int64_t>(StandardMetadataType::SMPTE2094_10)},
             nullptr,
             true,
             false,
             {}},
            {{kStandardTypeName, static_cast<int64_t>(StandardMetadataType::SMPTE2094_40)},
             nullptr,
             true,
             false,
             {}},
    };

    std::mutex mMutex;
    std::unordered_map<buffer_handle_t, ImportState> mImports;
    bool mUmpReady = false;
};

}  // namespace exynos4::gralloc5

extern "C" uint32_t ANDROID_HAL_MAPPER_VERSION = AIMAPPER_VERSION_5;
extern "C" uint32_t ANDROID_HAL_STABLEC_VERSION = AIMAPPER_VERSION_5;

extern "C" AIMapper_Error AIMapper_loadIMapper(AIMapper** out) {
    static vendor::mapper::IMapperProvider<exynos4::gralloc5::Mapper> provider;
    return provider.load(out);
}
