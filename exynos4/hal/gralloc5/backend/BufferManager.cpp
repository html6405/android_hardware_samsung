#define LOG_TAG "exynos4-gralloc5"

#include "BufferManager.h"

#include <aidl/android/hardware/graphics/common/BufferUsage.h>
#include <aidl/android/hardware/graphics/common/PixelFormat.h>
#include <cutils/properties.h>
#include <log/log.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <cstring>
#include <cerrno>
#include <limits>
#include <mutex>

#include "gralloc_usage.h"
#include "secion.h"
#include "ump.h"
#include "ump_ref_drv.h"

namespace exynos4::gralloc5 {
namespace {

using aidl::android::hardware::graphics::common::BufferUsage;
using aidl::android::hardware::graphics::common::PixelFormat;

constexpr int kLegacyMagic = 0x3141592;
constexpr int kUsesUmp = 0x02;
constexpr int kUsesIon = 0x20;
constexpr int kGraphicBuffer = 0x80;
constexpr int kContiguousIon = 0x100;
constexpr uint64_t kNeedsPhysical = static_cast<uint64_t>(BufferUsage::COMPOSER_CLIENT_TARGET) |
                                    static_cast<uint64_t>(BufferUsage::VIDEO_DECODER) |
                                    static_cast<uint64_t>(BufferUsage::CAMERA_OUTPUT) |
                                    static_cast<uint64_t>(BufferUsage::CAMERA_INPUT) |
                                    GRALLOC_USAGE_EXYNOS4_SCANOUT |
                                    GRALLOC_USAGE_EXYNOS4_CONTIGUOUS;

std::once_flag umpOnce;
bool umpReady = false;

bool ensureUmp() {
    std::call_once(umpOnce, [] { umpReady = ump_open() == UMP_OK; });
    return umpReady;
}

bool align16(uint64_t value, uint64_t* result) {
    if (value > std::numeric_limits<uint64_t>::max() - 15) return false;
    *result = (value + 15) & ~uint64_t(15);
    return true;
}

bool fitsLegacySize(uint64_t bytes) {
    return bytes > 0 && bytes <= static_cast<uint64_t>(INT32_MAX);
}

int createSharedIonClient() {
    const int fd = ion_client_create();
    if (fd >= 0) fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}

}  // namespace

IonClientRef::IonClientRef() {
    static const int shared = createSharedIonClient();
    if (shared >= 0) {
        mFd = shared;
        return;
    }
    mFd = ion_client_create();
    mOwned = mFd >= 0;
}

IonClientRef::~IonClientRef() {
    if (mOwned) ion_client_destroy(mFd);
}

bool computeLayout(uint32_t width, uint32_t height, int32_t format, BufferLayout* out) {
    if (!out || !width || !height || width > INT32_MAX || height > INT32_MAX) return false;
    BufferLayout result;
    uint64_t stride = 0;
    if (!align16(width, &stride)) return false;

    switch (format) {
        case static_cast<int32_t>(PixelFormat::RGBA_8888):
        case static_cast<int32_t>(PixelFormat::RGBX_8888):
        case static_cast<int32_t>(PixelFormat::BGRA_8888):
            result.bytesPerPixel = 4;
            break;
        case static_cast<int32_t>(PixelFormat::RGB_565):
            result.bytesPerPixel = 2;
            break;
        case static_cast<int32_t>(PixelFormat::RGB_888):
            result.bytesPerPixel = 3;
            break;
        case static_cast<int32_t>(PixelFormat::BLOB):
            if (height != 1) return false;
            result.bytesPerPixel = 1;
            break;
        case static_cast<int32_t>(PixelFormat::YCRCB_420_SP):
        case static_cast<int32_t>(PixelFormat::YCBCR_420_888):
        case static_cast<int32_t>(PixelFormat::YV12):
        case kExynosI420:
        case kExynosNv12:
            if ((width | height) & 1) return false;
            result.bytesPerPixel = 1;
            break;
        case static_cast<int32_t>(PixelFormat::YCBCR_422_SP):
            if (width & 1) return false;
            result.bytesPerPixel = 1;
            break;
        default:
            return false;
    }

    uint64_t allocationHeight = height;
    if (format == kExynosI420) {
        // FIMC's single-buffer YUV420 output derives Cb/Cr addresses from
        // full_width * full_height (and one quarter of that for Cr).  Both
        // dimensions must therefore be multiples of 32: the chroma stride
        // is exactly half the luma stride and the chroma height exactly half
        // the padded luma height, including for 360p and other odd macroblock
        // counts.  These are buffer dimensions, not the visible crop.
        if (width > UINT32_MAX - 31 || height > UINT32_MAX - 31) return false;
        stride = (static_cast<uint64_t>(width) + 31) & ~uint64_t{31};
        allocationHeight = (static_cast<uint64_t>(height) + 31) & ~uint64_t{31};
    }
    if (format == static_cast<int32_t>(PixelFormat::YV12))
        allocationHeight = (static_cast<uint64_t>(height) + 15) & ~uint64_t{15};
    if (stride > INT32_MAX || stride > UINT64_MAX / allocationHeight / result.bytesPerPixel)
        return false;
    const uint64_t lumaBytes = stride * allocationHeight * result.bytesPerPixel;
    uint64_t totalBytes = lumaBytes;
    const auto pixelFormat = static_cast<PixelFormat>(format);
    if (pixelFormat == PixelFormat::YCRCB_420_SP || pixelFormat == PixelFormat::YCBCR_420_888 ||
        format == kExynosNv12 ||
        pixelFormat == PixelFormat::YCBCR_422_SP) {
        if (lumaBytes > UINT32_MAX) return false;
        result.chromaOffset = static_cast<uint32_t>(lumaBytes);
        const uint64_t chromaBytes =
                stride * (pixelFormat == PixelFormat::YCBCR_422_SP ? height : height / 2);
        if (totalBytes > UINT64_MAX - chromaBytes) return false;
        totalBytes += chromaBytes;
    } else if (pixelFormat == PixelFormat::YV12 || format == kExynosI420) {
        uint64_t chromaStride;
        if (!align16(stride / 2, &chromaStride) || lumaBytes > UINT32_MAX) return false;
        uint64_t chromaHeight = height / 2;
        if (!align16(chromaHeight, &chromaHeight)) return false;
        if (chromaStride > UINT64_MAX / chromaHeight) return false;
        const uint64_t chromaBytes = chromaStride * chromaHeight;
        if (chromaBytes > UINT32_MAX || lumaBytes > UINT32_MAX - chromaBytes ||
            totalBytes > UINT64_MAX - 2 * chromaBytes)
            return false;
        result.chromaOffset = static_cast<uint32_t>(lumaBytes);
        // The Mali private-handle ABI stores the second chroma-plane offset
        // relative to the first for both I420 and YV12. Mapper metadata must
        // translate it back to an absolute offset for framework clients.
        result.vOffset = static_cast<uint32_t>(chromaBytes);
        totalBytes += 2 * chromaBytes;
    }
    if (!fitsLegacySize(totalBytes) || stride > INT32_MAX) return false;
    result.stride = static_cast<uint32_t>(stride);
    result.size = static_cast<size_t>(totalBytes);
    *out = result;
    return true;
}

size_t metadataOffset(const BufferLayout& layout) {
    return (layout.size + kMetadataPageSize - 1) & ~(kMetadataPageSize - 1);
}

bool validateHandle(const native_handle_t* h) {
    if (!h || h->version != sizeof(native_handle_t) || h->numFds != 1 || h->numInts != 21)
        return false;
    if (h->data[kFd] < 0 || h->data[kMagic] != kLegacyMagic || h->data[kSize] <= 0 ||
        h->data[kWidth] <= 0 || h->data[kHeight] <= 0 || h->data[kStride] <= 0 ||
        h->data[kUmpId] == static_cast<int>(UMP_INVALID_SECURE_ID))
        return false;
    BufferLayout layout;
    if (!computeLayout(h->data[kWidth], h->data[kHeight], h->data[kFormat], &layout)) return false;
    return static_cast<size_t>(h->data[kSize]) >= layout.size &&
           h->data[kStride] == static_cast<int>(layout.stride) &&
           h->data[kBpp] == static_cast<int>(layout.bytesPerPixel) &&
           h->data[kUoffset] == static_cast<int>(layout.chromaOffset) &&
           h->data[kVoffset] == static_cast<int>(layout.vOffset) &&
           (h->data[kFlags] & (kUsesUmp | kUsesIon)) == (kUsesUmp | kUsesIon);
}

native_handle_t* allocateBuffer(uint32_t width, uint32_t height, int32_t format, uint64_t usage,
                                const BufferLayout& layout, const char* name) {
    const size_t offset = metadataOffset(layout);
    if (offset > INT32_MAX - kMetadataPageSize || usage > UINT32_MAX)
        return nullptr;
    if (!ensureUmp()) {
        ALOGE("UMP unavailable for %ux%u format=%d usage=0x%llx", width, height, format,
              static_cast<unsigned long long>(usage));
        return nullptr;
    }
    const size_t allocationSize = offset + kMetadataPageSize;

    // COMPOSER_OVERLAY is a possible consumer, not a promise that every
    // GPU-rendered UI surface will be scanned out. FIMD requires a physical
    // address, but ordinary RGB layers with GPU usage can be composed into
    // the contiguous client target.
    const uint64_t overlay = static_cast<uint64_t>(BufferUsage::COMPOSER_OVERLAY);
    const uint64_t gpu = static_cast<uint64_t>(BufferUsage::GPU_TEXTURE) |
                         static_cast<uint64_t>(BufferUsage::GPU_RENDER_TARGET);
    const bool optionalGpuOverlay = (usage & overlay) && (usage & gpu) &&
                                    (format == static_cast<int32_t>(PixelFormat::RGBA_8888) ||
                                     format == static_cast<int32_t>(PixelFormat::RGBX_8888) ||
                                     format == static_cast<int32_t>(PixelFormat::BGRA_8888) ||
                                     format == static_cast<int32_t>(PixelFormat::RGB_888) ||
                                     format == static_cast<int32_t>(PixelFormat::RGB_565));
    const bool contiguous = (usage & kNeedsPhysical) ||
                            ((usage & overlay) && !optionalGpuOverlay);
    IonClientRef client;
    const int ionClient = client.get();
    if (ionClient < 0) {
        ALOGE("Cannot open ION client for %ux%u: errno=%d", width, height, errno);
        return nullptr;
    }
    const unsigned int heap = static_cast<unsigned int>(
            property_get_int32(contiguous ? "ro.vendor.exynos4.ion_contig_heap_mask"
                                          : "ro.vendor.exynos4.ion_heap_mask",
                               contiguous ? ION_HEAP_EXYNOS_CONTIG_MASK : ION_HEAP_EXYNOS_MASK));
    if (!heap) {
        ALOGE("ION heap mask is zero for %ux%u contiguous=%d", width, height, contiguous);
        return nullptr;
    }
    const int fd = ion_alloc(ionClient, allocationSize, contiguous ? 0x10000 : 0x1000, heap);
    if (fd < 0) {
        ALOGE("Exynos4 ION alloc failed: %ux%u format=%d usage=0x%llx "
              "bytes=%zu heap=0x%x contiguous=%d, errno=%d",
              width, height, format, static_cast<unsigned long long>(usage),
              allocationSize, heap, contiguous, errno);
        return nullptr;
    }
    const ion_phys_addr_t phys = contiguous ? ion_getphys(ionClient, fd) : 0;
    if (contiguous && (phys == 0 || phys > UINT32_MAX)) {
        ALOGE("Contiguous ION allocation has no 32-bit physical address");
        ion_free(fd);
        return nullptr;
    }
    ump_handle ump = ump_ref_drv_ion_import(fd, UMP_REF_DRV_CONSTRAINT_USE_CACHE);
    if (ump == UMP_INVALID_MEMORY_HANDLE) {
        ALOGE("UMP import failed: %ux%u format=%d usage=0x%llx bytes=%zu heap=0x%x",
              width, height, format, static_cast<unsigned long long>(usage), allocationSize,
              heap);
        ion_free(fd);
        return nullptr;
    }
    const ump_secure_id secureId = ump_secure_id_get(ump);
    if (secureId == UMP_INVALID_SECURE_ID) {
        ALOGE("UMP import returned no secure ID for %ux%u", width, height);
        ump_reference_release(ump);
        ion_free(fd);
        return nullptr;
    }

    // The kernel eagerly inserts every mapped ION page into the allocator's
    // 32-bit address space. Only the final metadata page needs CPU access;
    // mapping the entire image wastes page tables and work on each allocation.
    void* mapping = ion_map(fd, kMetadataPageSize, offset);
    if (mapping == MAP_FAILED) {
        ALOGE("ION metadata mmap failed: %ux%u offset=%zu errno=%d", width, height, offset,
              errno);
        ump_reference_release(ump);
        ion_free(fd);
        return nullptr;
    }
    auto* metadata = reinterpret_cast<SharedMetadata*>(mapping);
    memset(metadata, 0, sizeof(*metadata));
    metadata->version = 1;
    metadata->blendMode = 1;  // graphics.common.BlendMode::NONE
    metadata->id = 0;
    if (getrandom(&metadata->id, sizeof(metadata->id), 0) !=
        static_cast<ssize_t>(sizeof(metadata->id)))
        metadata->id = (static_cast<uint64_t>(secureId) << 32) | getpid();
    metadata->cropRight = width;
    metadata->cropBottom = height;
    if (name) {
        const size_t nameLength = strnlen(name, sizeof(metadata->name) - 1);
        memcpy(metadata->name, name, nameLength);
    }
    metadata->magic = kMetadataMagic;
    const int metadataSync = ion_msync(ionClient, fd, IMSYNC_DEV_TO_RW | IMSYNC_SYNC_FOR_DEV,
                                       kMetadataPageSize, offset);
    const int metadataSyncErrno = errno;
    ion_unmap(mapping, kMetadataPageSize);
    if (metadataSync < 0) {
        ALOGE("ION metadata sync failed: %ux%u offset=%zu errno=%d", width, height, offset,
              metadataSyncErrno);
        ump_reference_release(ump);
        ion_free(fd);
        return nullptr;
    }

    native_handle_t* h = native_handle_create(1, 21);
    if (!h) {
        ALOGE("Native handle allocation failed for %ux%u: errno=%d", width, height, errno);
        ump_reference_release(ump);
        ion_free(fd);
        return nullptr;
    }
    for (int i = 0; i <= kIonClient; ++i) h->data[i] = 0;
    h->data[kFd] = fd;
    h->data[kMagic] = kLegacyMagic;
    h->data[kFlags] = kUsesUmp | kUsesIon | kGraphicBuffer |
                      (contiguous ? kContiguousIon : 0);
    h->data[kSize] = static_cast<int>(allocationSize);
    h->data[kUmpId] = static_cast<int>(secureId);
    h->data[kPid] = getpid();
    h->data[kPaddr] = static_cast<int>(phys);
    h->data[kFormat] = format;
    h->data[kUsage] = static_cast<int>(usage);
    h->data[kWidth] = static_cast<int>(width);
    h->data[kHeight] = static_cast<int>(height);
    h->data[kBpp] = layout.bytesPerPixel;
    h->data[kStride] = layout.stride;
    h->data[kUoffset] = layout.chromaOffset;
    h->data[kVoffset] = layout.vOffset;

    ump_reference_release(ump);
    return h;
}

}  // namespace exynos4::gralloc5
