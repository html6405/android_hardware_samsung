#pragma once

#include <cutils/native_handle.h>

#include <cstddef>
#include <cstdint>

namespace exynos4::gralloc5 {

// Samsung's planar I420 and linear NV12 formats shared by OMX, Gralloc5 and FIMC.
constexpr int32_t kExynosI420 = 0x101;
constexpr int32_t kExynosNv12 = 0x105;

// The Mali-400 userspace blob consumes the historical one-fd/21-int handle.
// These indices are the serialized prefix of private_handle_t in gralloc_priv.h.
enum HandleIndex : int {
    kFd = 0,
    kMagic = 1,
    kFlags = 2,
    kSize = 3,
    kBase = 4,
    kLockState = 5,
    kWriteOwner = 6,
    kPid = 7,
    kUmpId = 8,
    kUmpHandle = 9,
    kOffset = 10,
    kPaddr = 11,
    kFormat = 12,
    kUsage = 13,
    kWidth = 14,
    kHeight = 15,
    kBpp = 16,
    kStride = 17,
    kYaddr = 18,
    kUoffset = 19,
    kVoffset = 20,
    kIonClient = 21,
};

struct BufferLayout {
    uint32_t stride = 0;
    uint32_t bytesPerPixel = 0;
    size_t size = 0;
    uint32_t chromaOffset = 0;
    uint32_t vOffset = 0;  // Second chroma plane, relative to chromaOffset.
};

// Stored in the final page of new ION allocations. No extra FD or handle
// integer is added, so the Mali private-handle ABI remains unchanged.
struct SharedMetadata {
    uint32_t magic;
    uint32_t version;
    uint64_t id;
    int32_t dataspace;
    int32_t blendMode;
    int32_t cropLeft;
    int32_t cropTop;
    int32_t cropRight;
    int32_t cropBottom;
    char name[128];
};

constexpr uint32_t kMetadataMagic = 0x344d4558;
constexpr size_t kMetadataPageSize = 4096;
size_t metadataOffset(const BufferLayout& layout);

// All arithmetic is checked before narrowing into the legacy 32-bit ABI.
bool computeLayout(uint32_t width, uint32_t height, int32_t format, BufferLayout* out);
bool validateHandle(const native_handle_t* handle);
native_handle_t* allocateBuffer(uint32_t width, uint32_t height, int32_t format, uint64_t usage,
                                const BufferLayout& layout, const char* name);

}  // namespace exynos4::gralloc5
