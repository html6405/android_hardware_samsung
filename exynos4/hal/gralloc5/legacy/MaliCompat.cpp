#define LOG_TAG "exynos4-mali-gralloc"

#include <hardware/gralloc.h>
#include <log/log.h>
#include <system/graphics.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "backend/BufferManager.h"
#include "ump.h"
#include "ump_ref_drv.h"

namespace {

using namespace exynos4::gralloc5;

struct Registration {
    ump_handle ump = UMP_INVALID_MEMORY_HANDLE;
    void* address = nullptr;
    uint32_t references = 0;
    uint32_t readLocks = 0;
    bool writeLocked = false;
};

std::mutex gMutex;
std::unordered_map<buffer_handle_t, Registration> gRegistrations;
std::once_flag gUmpOnce;
bool gUmpReady = false;

bool ensureUmp() {
    std::call_once(gUmpOnce, [] { gUmpReady = ump_open() == UMP_OK; });
    return gUmpReady;
}

void releaseRegistration(std::unordered_map<buffer_handle_t, Registration>::iterator it) {
    ump_mapped_pointer_release(it->second.ump);
    ump_reference_release(it->second.ump);
    gRegistrations.erase(it);
}

int registerLocked(buffer_handle_t buffer, bool explicitRegistration) {
    if (!validateHandle(buffer) || !ensureUmp()) return -EINVAL;

    auto found = gRegistrations.find(buffer);
    if (found != gRegistrations.end()) {
        if (explicitRegistration) ++found->second.references;
        return 0;
    }

    ump_handle ump = ump_ref_drv_ion_import(buffer->data[kFd],
                                             UMP_REF_DRV_CONSTRAINT_USE_CACHE);
    if (ump == UMP_INVALID_MEMORY_HANDLE) return -EINVAL;

    void* address = ump_mapped_pointer_get(ump);
    if (!address) {
        ump_reference_release(ump);
        return -ENOMEM;
    }

    // A direct lock is not a registerBuffer() reference. Keep its import only
    // until the matching unlock, unless a caller explicitly registers it.
    gRegistrations.emplace(buffer, Registration{ump, address,
                                                explicitRegistration ? 1U : 0U, 0, false});
    return 0;
}

int registerBuffer(const gralloc_module_t*, buffer_handle_t buffer) {
    std::lock_guard lock(gMutex);
    return registerLocked(buffer, true);
}

int unregisterBuffer(const gralloc_module_t*, buffer_handle_t buffer) {
    std::lock_guard lock(gMutex);
    auto found = gRegistrations.find(buffer);
    if (found == gRegistrations.end()) return -EINVAL;

    Registration& registration = found->second;
    if (!registration.references) return -EINVAL;
    if (registration.readLocks || registration.writeLocked) return -EBUSY;
    if (--registration.references) return 0;
    releaseRegistration(found);
    return 0;
}

int lockBuffer(const gralloc_module_t*, buffer_handle_t buffer, int usage,
               int left, int top, int width, int height, void** address) {
    if (!address || left < 0 || top < 0 || width < 0 || height < 0 ||
        !validateHandle(buffer) || left + width > buffer->data[kWidth] ||
        top + height > buffer->data[kHeight]) {
        return -EINVAL;
    }

    std::lock_guard lock(gMutex);
    auto found = gRegistrations.find(buffer);
    if (found == gRegistrations.end()) {
        const int error = registerLocked(buffer, false);
        if (error) return error;
        found = gRegistrations.find(buffer);
    }

    Registration& registration = found->second;
    const bool write = (usage & GRALLOC_USAGE_SW_WRITE_MASK) != 0;
    if (registration.writeLocked || (write && registration.readLocks)) return -EBUSY;

    if (usage & GRALLOC_USAGE_SW_READ_MASK) {
        ump_cpu_msync_now(registration.ump, UMP_MSYNC_CLEAN_AND_INVALIDATE, nullptr, 0);
    }

    if (write)
        registration.writeLocked = true;
    else
        ++registration.readLocks;
    *address = registration.address;
    return 0;
}

int unlockBuffer(const gralloc_module_t*, buffer_handle_t buffer) {
    std::lock_guard lock(gMutex);
    auto found = gRegistrations.find(buffer);
    if (found == gRegistrations.end()) return -EINVAL;

    Registration& registration = found->second;
    if (registration.writeLocked) {
        ump_cpu_msync_now(registration.ump, UMP_MSYNC_CLEAN_AND_INVALIDATE, nullptr, 0);
        registration.writeLocked = false;
    } else if (registration.readLocks) {
        --registration.readLocks;
    } else {
        return -EINVAL;
    }

    if (!registration.readLocks && !registration.writeLocked) {
        if (!registration.references) releaseRegistration(found);
    }
    return 0;
}

int lockYcbcr(const gralloc_module_t* module, buffer_handle_t buffer, int usage,
              int left, int top, int width, int height, android_ycbcr* ycbcr) {
    if (!ycbcr) return -EINVAL;
    void* address = nullptr;
    const int error = lockBuffer(module, buffer, usage, left, top, width, height, &address);
    if (error) return error;

    memset(ycbcr, 0, sizeof(*ycbcr));
    auto* base = static_cast<uint8_t*>(address);
    ycbcr->y = base;
    ycbcr->ystride = buffer->data[kStride];

    switch (buffer->data[kFormat]) {
        case kExynosI420:
            ycbcr->cb = base + buffer->data[kUoffset];
            ycbcr->cr = base + buffer->data[kUoffset] + buffer->data[kVoffset];
            ycbcr->cstride = (buffer->data[kStride] / 2 + 15) & ~15;
            ycbcr->chroma_step = 1;
            return 0;
        case HAL_PIXEL_FORMAT_YCrCb_420_SP:
        case HAL_PIXEL_FORMAT_YCbCr_420_888:
            ycbcr->cr = base + buffer->data[kUoffset];
            ycbcr->cb = static_cast<uint8_t*>(ycbcr->cr) + 1;
            ycbcr->cstride = buffer->data[kStride];
            ycbcr->chroma_step = 2;
            return 0;
        case HAL_PIXEL_FORMAT_YCbCr_422_SP:
        case kExynosNv12:
            ycbcr->cb = base + buffer->data[kUoffset];
            ycbcr->cr = static_cast<uint8_t*>(ycbcr->cb) + 1;
            ycbcr->cstride = buffer->data[kStride];
            ycbcr->chroma_step = 2;
            return 0;
        case HAL_PIXEL_FORMAT_YV12:
            ycbcr->cr = base + buffer->data[kUoffset];
            ycbcr->cb = base + buffer->data[kUoffset] + buffer->data[kVoffset];
            ycbcr->cstride = (buffer->data[kStride] / 2 + 15) & ~15;
            ycbcr->chroma_step = 1;
            return 0;
        default:
            unlockBuffer(module, buffer);
            return -EINVAL;
    }
}

int perform(const gralloc_module_t*, int, ...) {
    return -EINVAL;
}

int openDevice(const hw_module_t*, const char*, hw_device_t**) {
    // Allocation is exclusively provided by the AIDL allocator service.
    return -EINVAL;
}

hw_module_methods_t kMethods = {.open = openDevice};

}  // namespace

extern "C" gralloc_module_t HAL_MODULE_INFO_SYM = {
        .common =
                {
                        .tag = HARDWARE_MODULE_TAG,
                        .module_api_version = GRALLOC_MODULE_API_VERSION_0_2,
                        .hal_api_version = HARDWARE_HAL_API_VERSION,
                        .id = GRALLOC_HARDWARE_MODULE_ID,
                        .name = "Exynos4 Mali Gralloc5 compatibility frontend",
                        .author = "The LineageOS Project",
                        .methods = &kMethods,
                },
        .registerBuffer = registerBuffer,
        .unregisterBuffer = unregisterBuffer,
        .lock = lockBuffer,
        .unlock = unlockBuffer,
        .perform = perform,
        .lock_ycbcr = lockYcbcr,
};
