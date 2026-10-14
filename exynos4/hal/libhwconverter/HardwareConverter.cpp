/*
 * Copyright (C) 2009 The Android Open Source Project
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

#include <utils/Log.h>
#include <cstdint>
#include <limits>
#include "SEC_OMX_Def.h"
#include "SecFimc.h"
#include "HardwareConverter.h"

HardwareConverter::HardwareConverter()
{
    SecFimc* handle_fimc = new SecFimc();
    mSecFimc = (void *)handle_fimc;

    // Each conversion has one caller-owned destination ANB.  The multi-buffer
    // mode requires V4L2_CID_DST_INFO for every plane, which this API does
    // not provide; single-buffer mode uses the external framebuffer address.
    // FIMC0 is the camera capture engine while recording.  Use the fourth
    // independently exposed FIMC for OMX CSC so decoder/encoder setup cannot
    // fail merely because the camera holds /dev/video0.  Probe capabilities
    // through SecFimc::create and retain FIMC0 for boards without FIMC3.
    if (!handle_fimc->create(SecFimc::FIMC_DEV3, FIMC_OVLY_NONE_SINGLE_BUF, 1) &&
        !handle_fimc->create(SecFimc::FIMC_DEV0, FIMC_OVLY_NONE_SINGLE_BUF, 1))
        bHWconvert_flag = 0;
    else
        bHWconvert_flag = 1;
}

HardwareConverter::~HardwareConverter()
{
    SecFimc* handle_fimc = (SecFimc*)mSecFimc;
    if (handle_fimc->flagCreate())
        handle_fimc->destroy();
    delete handle_fimc;
}

bool HardwareConverter::convert(
    void * src_addr,
    void *dst_addr,
    OMX_COLOR_FORMATTYPE src_format,
    int32_t width,
    int32_t height,
    OMX_COLOR_FORMATTYPE dst_format)
{
    SecFimc* handle_fimc = (SecFimc*)mSecFimc;

    int rotate_value = 0;
    unsigned int src_crop_x = 0;
    unsigned int src_crop_y = 0;
    unsigned int src_crop_width = width;
    unsigned int src_crop_height = height;

    unsigned int dst_crop_x = 0;
    unsigned int dst_crop_y = 0;
    unsigned int dst_crop_width = width;
    unsigned int dst_crop_height = height;

    void **src_addr_array = (void **)src_addr;
    void **dst_addr_array = (void **)dst_addr;

    if (width <= 0 || height <= 0 || !src_addr_array || !dst_addr_array ||
        !src_addr_array[0] || !src_addr_array[1] || !dst_addr_array[0] ||
        !dst_addr_array[1]) {
        ALOGE("%s: invalid source or destination plane", __func__);
        return false;
    }

    const bool planar = dst_format == OMX_COLOR_FormatYUV420Planar ||
                        dst_format == OMX_COLOR_FormatYCbCr420Planar;
    const bool semiplanar = dst_format == OMX_COLOR_FormatYUV420SemiPlanar;
    if (!planar && !semiplanar) {
        ALOGE("%s: unsupported destination format 0x%x", __func__, dst_format);
        return false;
    }
    const uint64_t align = planar ? 32 : 16;
    const uint64_t fullWidth = (static_cast<uint64_t>(width) + align - 1) & ~(align - 1);
    const uint64_t fullHeight = planar
            ? (static_cast<uint64_t>(height) + 31) & ~uint64_t{31}
            : static_cast<uint64_t>(height);
    const uint64_t ySize = fullWidth * fullHeight;
    const uintptr_t y = reinterpret_cast<uintptr_t>(dst_addr_array[0]);
    const uintptr_t cb = reinterpret_cast<uintptr_t>(dst_addr_array[1]);
    const uintptr_t cr = reinterpret_cast<uintptr_t>(dst_addr_array[2]);
    // In single-buffer mode the Exynos FIMC kernel computes chroma addresses
    // from the full framebuffer geometry.  Reject layouts that disagree with
    // those DMA addresses so the caller can use its CPU converter safely.
    if (fullWidth > std::numeric_limits<unsigned int>::max() ||
        fullHeight > std::numeric_limits<unsigned int>::max() ||
        ySize > std::numeric_limits<uint32_t>::max() || cb <= y ||
        static_cast<uint64_t>(cb - y) != ySize ||
        (planar && (cr <= cb || static_cast<uint64_t>(cr - cb) != ySize / 4))) {
        ALOGE("%s: destination planes do not match FIMC framebuffer geometry", __func__);
        return false;
    }

    unsigned int src_har_format = OMXtoHarPixelFomrat(src_format);
    unsigned int dst_har_format = OMXtoHarPixelFomrat(dst_format);

    // set post processor configuration
    if (!handle_fimc->setSrcParams(width, height, src_crop_x, src_crop_y,
                                   &src_crop_width, &src_crop_height,
                                   src_har_format)) {
        ALOGE("%s:: setSrcParms() failed", __func__);
        return false;
    }

    if (!handle_fimc->setSrcPhyAddr((unsigned int)src_addr_array[0],
                                 (unsigned int)src_addr_array[1],
                                 (unsigned int)src_addr_array[1],
                                 src_har_format)) {
        ALOGE("%s:: setSrcPhyAddr() failed", __func__);
        return false;
    }

    if (!handle_fimc->setRotVal(rotate_value)) {
        ALOGE("%s:: setRotVal() failed", __func__);
        return false;
    }

    if (!handle_fimc->setDstParams(static_cast<unsigned int>(fullWidth),
                                  static_cast<unsigned int>(fullHeight),
                                  dst_crop_x, dst_crop_y,
                                  &dst_crop_width, &dst_crop_height,
                                  dst_har_format)) {
        ALOGE("%s:: setDstParams() failed", __func__);
        return false;
    }
    if (src_crop_width != static_cast<unsigned int>(width) ||
        src_crop_height != static_cast<unsigned int>(height) ||
        dst_crop_width != static_cast<unsigned int>(width) ||
        dst_crop_height != static_cast<unsigned int>(height)) {
        ALOGE("%s: FIMC changed the visible crop", __func__);
        return false;
    }

    switch (dst_format) {
    case OMX_COLOR_FormatYUV420SemiPlanar:
        if (!handle_fimc->setDstPhyAddr((unsigned int)(dst_addr_array[0]),
                                     (unsigned int)(dst_addr_array[1]),
                                     (unsigned int)(dst_addr_array[1]))) {
            ALOGE("%s:: setDstPhyAddr() failed", __func__);
            return false;
        }
        break;
    case OMX_COLOR_FormatYUV420Planar:
    case OMX_COLOR_FormatYCbCr420Planar:
    default:
        if (!handle_fimc->setDstPhyAddr((unsigned int)(dst_addr_array[0]),
                                     (unsigned int)(dst_addr_array[1]),
                                     (unsigned int)(dst_addr_array[2]))) {
            ALOGE("%s:: setDstPhyAddr() failed", __func__);
            return false;
        }
        break;
    }

    if (!handle_fimc->handleOneShot()) {
        ALOGE("%s:: handleOneShot() failed", __func__);
        return false;
    }

    return true;
}

unsigned int HardwareConverter::OMXtoHarPixelFomrat(OMX_COLOR_FORMATTYPE omx_format)
{
    if (omx_format == static_cast<OMX_COLOR_FORMATTYPE>(OMX_SEC_COLOR_FormatNV12TPhysicalAddress))
        return HAL_PIXEL_FORMAT_CUSTOM_YCbCr_420_SP_TILED;

    unsigned int hal_format = 0;

    switch (omx_format) {
    case OMX_COLOR_FormatYUV420Planar:
        hal_format = HAL_PIXEL_FORMAT_YCbCr_420_P;
        break;
    case OMX_COLOR_FormatYUV420SemiPlanar:
        hal_format = HAL_PIXEL_FORMAT_YCbCr_420_SP;
        break;
    default:
        hal_format = HAL_PIXEL_FORMAT_YCbCr_420_P;
        break;
    }
    return hal_format;
}
