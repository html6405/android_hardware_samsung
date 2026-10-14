/*
 * Copyright 2011 Samsung Electronics S.LSI Co. LTD
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

/*
 * @file        SEC_OSAL_Android.cpp
 * @brief
 * @author      Seungbeom Kim (sbcrux.kim@samsung.com)
 * @author      Hyeyeon Chung (hyeon.chung@samsung.com)
 * @author      Yunji Kim (yunji.kim@samsung.com)
 * @author      Jinsung Yang (jsgood.yang@samsung.com)
 * @version     1.1.0
 * @history
 *   2011.7.15 : Create
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>


#include <ui/GraphicBuffer.h>
#include <ui/GraphicBufferMapper.h>
#include <ui/Rect.h>
#include <media/hardware/HardwareAPI.h>
#include <hardware/hardware.h>
#include <media/hardware/MetadataBufferType.h>

#include "gralloc_priv.h"
#include "sec_format.h"

#include "SEC_OSAL_Semaphore.h"
#include "SEC_OMX_Baseport.h"
#include "SEC_OMX_Basecomponent.h"
#include "SEC_OMX_Macros.h"
#include "SEC_OMX_Vdec.h"
#include "SEC_OSAL_Android.h"

#undef  SEC_LOG_TAG
#define SEC_LOG_TAG    "SEC_OSAL_Android"
#define SEC_LOG_OFF
#include "SEC_OSAL_Log.h"

using namespace android;

#ifdef __cplusplus
extern "C" {
#endif

OMX_ERRORTYPE useAndroidNativeBuffer(
    SEC_OMX_BASEPORT      *pSECPort,
    OMX_BUFFERHEADERTYPE **ppBufferHdr,
    OMX_U32                nPortIndex,
    OMX_PTR                pAppPrivate,
    OMX_U32                nSizeBytes,
    OMX_U8                *pBuffer)
{
    OMX_ERRORTYPE         ret = OMX_ErrorNone;
    OMX_BUFFERHEADERTYPE *temp_bufferHeader = NULL;
    unsigned int          i = 0;

    FunctionIn();

    if (pSECPort == NULL) {
        ret = OMX_ErrorBadParameter;
        goto EXIT;
    }
    if (pSECPort->portState != OMX_StateIdle) {
        ret = OMX_ErrorIncorrectStateOperation;
        goto EXIT;
    }
    if (CHECK_PORT_TUNNELED(pSECPort) && CHECK_PORT_BUFFER_SUPPLIER(pSECPort)) {
        ret = OMX_ErrorBadPortIndex;
        goto EXIT;
    }

    temp_bufferHeader = (OMX_BUFFERHEADERTYPE *)SEC_OSAL_Malloc(sizeof(OMX_BUFFERHEADERTYPE));
    if (temp_bufferHeader == NULL) {
        ret = OMX_ErrorInsufficientResources;
        goto EXIT;
    }
    SEC_OSAL_Memset(temp_bufferHeader, 0, sizeof(OMX_BUFFERHEADERTYPE));

    for (i = 0; i < pSECPort->portDefinition.nBufferCountActual; i++) {
        if (pSECPort->bufferStateAllocate[i] == BUFFER_STATE_FREE) {
            pSECPort->bufferHeader[i] = temp_bufferHeader;
            pSECPort->bufferStateAllocate[i] = (BUFFER_STATE_ASSIGNED | HEADER_STATE_ALLOCATED);
            INIT_SET_SIZE_VERSION(temp_bufferHeader, OMX_BUFFERHEADERTYPE);
            temp_bufferHeader->pBuffer        = pBuffer;
            temp_bufferHeader->nAllocLen      = nSizeBytes;
            temp_bufferHeader->pAppPrivate    = pAppPrivate;
            if (nPortIndex == INPUT_PORT_INDEX)
                temp_bufferHeader->nInputPortIndex = INPUT_PORT_INDEX;
            else
                temp_bufferHeader->nOutputPortIndex = OUTPUT_PORT_INDEX;

            pSECPort->assignedBufferNum++;
            if (pSECPort->assignedBufferNum == pSECPort->portDefinition.nBufferCountActual) {
                pSECPort->portDefinition.bPopulated = OMX_TRUE;
                /* SEC_OSAL_MutexLock(pSECComponent->compMutex); */
                SEC_OSAL_SemaphorePost(pSECPort->loadedResource);
                /* SEC_OSAL_MutexUnlock(pSECComponent->compMutex); */
            }
            *ppBufferHdr = temp_bufferHeader;
            goto EXIT;
        }
    }

    SEC_OSAL_Free(temp_bufferHeader);
    ret = OMX_ErrorInsufficientResources;

EXIT:
    FunctionOut();

    return ret;
}

OMX_ERRORTYPE SEC_OSAL_LockANBHandle(
    OMX_IN OMX_U32 handle,
    OMX_IN OMX_U32 width,
    OMX_IN OMX_U32 height,
    OMX_IN OMX_COLOR_FORMATTYPE format,
    OMX_OUT OMX_PTR *vaddr)
{
    FunctionIn();

    OMX_ERRORTYPE ret = OMX_ErrorNone;
    GraphicBufferMapper &mapper = GraphicBufferMapper::get();
    buffer_handle_t bufferHandle = (buffer_handle_t) handle;
    Rect bounds((uint32_t)width, (uint32_t)height);

    SEC_OSAL_Log(SEC_LOG_TRACE, "%s: handle: 0x%x", __func__, handle);

    int usage = 0;

    switch (format) {
    case OMX_COLOR_FormatYUV420Planar:
    case OMX_COLOR_FormatYCbCr420Planar:
    case OMX_COLOR_FormatYUV420SemiPlanar:
#ifdef S3D_SUPPORT
    case OMX_SEC_COLOR_FormatNV12Tiled_SBS_LR:
    case OMX_SEC_COLOR_FormatNV12Tiled_SBS_RL:
    case OMX_SEC_COLOR_FormatNV12Tiled_TB_LR:
    case OMX_SEC_COLOR_FormatNV12Tiled_TB_RL:
    case OMX_SEC_COLOR_FormatYUV420SemiPlanar_SBS_LR:
    case OMX_SEC_COLOR_FormatYUV420SemiPlanar_SBS_RL:
    case OMX_SEC_COLOR_FormatYUV420SemiPlanar_TB_LR:
    case OMX_SEC_COLOR_FormatYUV420SemiPlanar_TB_RL:
    case OMX_SEC_COLOR_FormatYUV420Planar_SBS_LR:
    case OMX_SEC_COLOR_FormatYUV420Planar_SBS_RL:
    case OMX_SEC_COLOR_FormatYUV420Planar_TB_LR:
    case OMX_SEC_COLOR_FormatYUV420Planar_TB_RL:
#endif
    case OMX_SEC_COLOR_FormatANBYUV420SemiPlanar:
        /*
         * The decoder is the producer of these buffers.  It never reads the
         * old contents, so asking gralloc for SW_READ_OFTEN needlessly
         * invalidates the complete buffer before every frame.  At 720p/1080p
         * that cache operation is large enough to starve the native window.
         */
        usage = GRALLOC_USAGE_SW_WRITE_OFTEN;
        break;
    case OMX_COLOR_FormatAndroidOpaque:
        // The video encoder copies this GPU-produced input into its MFC
        // staging buffer.  It is a CPU reader, never the buffer producer.
        usage = GRALLOC_USAGE_SW_READ_OFTEN;
        break;
    default:
        usage = GRALLOC_USAGE_SW_WRITE_OFTEN;
        break;
    }

    if (mapper.lock(bufferHandle, usage, bounds, vaddr) != 0) {
        SEC_OSAL_Log(SEC_LOG_ERROR, "%s: mapper.lock() fail", __func__);
        ret = OMX_ErrorUndefined;
        goto EXIT;
    }

    SEC_OSAL_Log(SEC_LOG_TRACE, "%s: buffer locked: 0x%x", __func__, *vaddr);

EXIT:
    FunctionOut();

    return ret;
}

OMX_ERRORTYPE SEC_OSAL_UnlockANBHandle(OMX_IN OMX_U32 handle)
{
    FunctionIn();

    OMX_ERRORTYPE ret = OMX_ErrorNone;
    GraphicBufferMapper &mapper = GraphicBufferMapper::get();
    buffer_handle_t bufferHandle = (buffer_handle_t) handle;

    SEC_OSAL_Log(SEC_LOG_TRACE, "%s: handle: 0x%x", __func__, handle);

    if (mapper.unlock(bufferHandle) != 0) {
        SEC_OSAL_Log(SEC_LOG_ERROR, "%s: mapper.unlock() fail", __func__);
        ret = OMX_ErrorUndefined;
        goto EXIT;
    }

    SEC_OSAL_Log(SEC_LOG_TRACE, "%s: buffer unlocked: 0x%x", __func__, handle);

EXIT:
    FunctionOut();

    return ret;
}

OMX_ERRORTYPE SEC_OSAL_GetPhysANBHandle(
    OMX_IN OMX_U32 handle,
    OMX_OUT OMX_PTR *paddr)
{
    FunctionIn();

    OMX_ERRORTYPE ret = OMX_ErrorNone;
    buffer_handle_t bufferHandle = (buffer_handle_t) handle;
    private_handle_t *hnd = private_handle_t::dynamicCast(bufferHandle);
    const bool yv12 = hnd != NULL && hnd->format == HAL_PIXEL_FORMAT_YV12;

    SEC_OSAL_Log(SEC_LOG_TRACE, "%s: handle: 0x%x", __func__, handle);

    if (paddr == NULL || hnd == NULL || hnd->paddr == 0) {
        SEC_OSAL_Log(SEC_LOG_ERROR, "%s: invalid or non-contiguous buffer", __func__);
        ret = OMX_ErrorBadParameter;
        goto EXIT;
    }

    /*
     * FIMC consumes physical plane addresses. All video ANBs are allocated
     * from contiguous ION and gralloc records the chroma offsets in the
     * legacy 21-word handle, so no process-local pointer is used here.
     */
    paddr[0] = reinterpret_cast<void *>(
            static_cast<uintptr_t>(hnd->paddr + hnd->offset));
    paddr[1] = reinterpret_cast<void *>(static_cast<uintptr_t>(
            hnd->paddr + hnd->offset + (yv12 ? hnd->voffset : hnd->uoffset)));
    paddr[2] = reinterpret_cast<void *>(static_cast<uintptr_t>(
            hnd->paddr + hnd->offset +
            (yv12 ? hnd->uoffset : hnd->uoffset + hnd->voffset)));
EXIT:
    FunctionOut();

    return ret;
}

OMX_ERRORTYPE SEC_OSAL_GetANBInfoHandle(
    OMX_IN OMX_U32 handle,
    OMX_OUT SEC_OSAL_ANB_FORMATTYPE *format,
    OMX_OUT OMX_U32 *stride,
    OMX_OUT OMX_U32 *chromaOffset)
{
    buffer_handle_t bufferHandle = (buffer_handle_t)handle;
    private_handle_t *hnd = private_handle_t::dynamicCast(bufferHandle);

    if (format == NULL || stride == NULL || chromaOffset == NULL)
        return OMX_ErrorBadParameter;

    *format = SEC_OSAL_ANB_FORMAT_UNKNOWN;
    *stride = 0;
    *chromaOffset = 0;

    if (hnd == NULL || hnd->stride <= 0)
        return OMX_ErrorBadParameter;

    switch (hnd->format) {
    case HAL_PIXEL_FORMAT_YCbCr_420_SP:
        *format = SEC_OSAL_ANB_FORMAT_NV12;
        break;
    case HAL_PIXEL_FORMAT_YCrCb_420_SP:
        *format = SEC_OSAL_ANB_FORMAT_NV21;
        break;
    case HAL_PIXEL_FORMAT_RGBA_8888:
    case HAL_PIXEL_FORMAT_RGBX_8888:
    case HAL_PIXEL_FORMAT_BGRA_8888:
        *format = SEC_OSAL_ANB_FORMAT_ABGR8888;
        break;
    default:
        SEC_OSAL_Log(SEC_LOG_ERROR, "%s: unsupported gralloc format 0x%x",
                     __func__, hnd->format);
        return OMX_ErrorUnsupportedSetting;
    }

    *stride = hnd->stride;
    *chromaOffset = hnd->uoffset;
    return OMX_ErrorNone;
}

OMX_ERRORTYPE SEC_OSAL_LockANB(
    OMX_IN OMX_PTR pBuffer,
    OMX_IN OMX_U32 width,
    OMX_IN OMX_U32 height,
    OMX_IN OMX_COLOR_FORMATTYPE format,
    OMX_OUT OMX_U32 *pStride,
    OMX_OUT OMX_PTR *vaddr)
{
    FunctionIn();

    OMX_ERRORTYPE ret = OMX_ErrorNone;
    android_native_buffer_t *pANB = (android_native_buffer_t *) pBuffer;
    private_handle_t *hnd = private_handle_t::dynamicCast(pANB->handle);
    uint64_t allocatedHeight = 0;
    if (hnd == NULL || hnd->uoffset == 0 || hnd->size <= 0 ||
        hnd->uoffset >= static_cast<unsigned>(hnd->size)) {
        ret = OMX_ErrorBadParameter;
        goto EXIT;
    }
    if ((format == OMX_COLOR_FormatYUV420Planar ||
         format == OMX_COLOR_FormatYCbCr420Planar) &&
        hnd->format != HAL_PIXEL_FORMAT_YV12 &&
        hnd->format != HAL_PIXEL_FORMAT_YCbCr_420_P) {
        SEC_OSAL_Log(SEC_LOG_ERROR, "%s: planar OMX output requires planar ANB", __func__);
        ret = OMX_ErrorUnsupportedSetting;
        goto EXIT;
    }
    if (format == OMX_COLOR_FormatYUV420SemiPlanar &&
        (hnd->format == HAL_PIXEL_FORMAT_YV12 ||
         hnd->format == HAL_PIXEL_FORMAT_YCbCr_420_P)) {
        SEC_OSAL_Log(SEC_LOG_ERROR, "%s: semiplanar OMX output requires semiplanar ANB", __func__);
        ret = OMX_ErrorUnsupportedSetting;
        goto EXIT;
    }

    // MFC reports macroblock-aligned output dimensions.  The native window
    // buffer describes the visible frame, while its stride (and, for the
    // Exynos planar format, padded height) provides room for that output.
    // A mapper lock rectangle must describe visible pixels, not the padded
    // MFC write extent.  Check the latter separately before permitting the
    // decoder to write into the mapped allocation.
    allocatedHeight = hnd->format == HAL_PIXEL_FORMAT_YCbCr_420_P
            ? (static_cast<uint64_t>(hnd->height) + 31) & ~uint64_t{31}
            : static_cast<uint64_t>(hnd->height);
    if (hnd->width <= 0 || hnd->height <= 0 || hnd->stride <= 0 ||
        pANB->stride != static_cast<OMX_U32>(hnd->stride) ||
        width == 0 || height == 0 || width > static_cast<OMX_U32>(hnd->stride) ||
        height > allocatedHeight) {
        SEC_OSAL_Log(SEC_LOG_ERROR,
                     "%s: MFC output %ux%u exceeds ANB %dx%d stride %d padded height %llu",
                     __func__, width, height, hnd->width, hnd->height, hnd->stride,
                     static_cast<unsigned long long>(allocatedHeight));
        ret = OMX_ErrorBadParameter;
        goto EXIT;
    }

    ret = SEC_OSAL_LockANBHandle((OMX_U32)pANB->handle, hnd->width, hnd->height,
                                 format, vaddr);
    if (ret != OMX_ErrorNone)
        goto EXIT;

    // Mapper5 returns the mapped buffer, not the legacy YUV_ADDR pointer array.
    // Use the serialized plane offsets that accompany the 21-word handle.
    if (vaddr[0] == NULL) {
        SEC_OSAL_UnlockANBHandle((OMX_U32)pANB->handle);
        ret = OMX_ErrorUndefined;
        goto EXIT;
    }
    if (hnd->format == HAL_PIXEL_FORMAT_YV12 ||
        hnd->format == HAL_PIXEL_FORMAT_YCbCr_420_P) {
        if (hnd->voffset == 0 || hnd->voffset >= static_cast<unsigned>(hnd->size)) {
            SEC_OSAL_UnlockANBHandle((OMX_U32)pANB->handle);
            ret = OMX_ErrorBadParameter;
            goto EXIT;
        }
        if (hnd->format == HAL_PIXEL_FORMAT_YV12) {
            vaddr[1] = static_cast<unsigned char *>(vaddr[0]) + hnd->voffset; // Cb
            vaddr[2] = static_cast<unsigned char *>(vaddr[0]) + hnd->uoffset; // Cr
        } else {
            vaddr[1] = static_cast<unsigned char *>(vaddr[0]) + hnd->uoffset; // Cb
            vaddr[2] = static_cast<unsigned char *>(vaddr[0]) +
                       hnd->uoffset + hnd->voffset; // Cr
        }
    } else {
        vaddr[1] = static_cast<unsigned char *>(vaddr[0]) + hnd->uoffset;
        vaddr[2] = NULL;
    }

    *pStride = pANB->stride;

EXIT:
    FunctionOut();

    return ret;
}

OMX_ERRORTYPE SEC_OSAL_UnlockANB(OMX_IN OMX_PTR pBuffer)
{
    FunctionIn();

    OMX_ERRORTYPE ret = OMX_ErrorNone;
    android_native_buffer_t *pANB = (android_native_buffer_t *) pBuffer;

    ret = SEC_OSAL_UnlockANBHandle((OMX_U32)pANB->handle);

EXIT:
    FunctionOut();

    return ret;
}

OMX_ERRORTYPE SEC_OSAL_GetPhysANB(
    OMX_IN OMX_PTR pBuffer,
    OMX_OUT OMX_PTR *paddr)
{
    FunctionIn();

    OMX_ERRORTYPE ret = OMX_ErrorNone;
    android_native_buffer_t *pANB = (android_native_buffer_t *) pBuffer;

    ret = SEC_OSAL_GetPhysANBHandle((OMX_U32)pANB->handle, paddr);

EXIT:
    FunctionOut();

    return ret;
}

OMX_ERRORTYPE SEC_OSAL_GetANBParameter(
    OMX_IN OMX_HANDLETYPE hComponent,
    OMX_IN OMX_INDEXTYPE  nIndex,
    OMX_INOUT OMX_PTR     ComponentParameterStructure)
{
    OMX_ERRORTYPE          ret = OMX_ErrorNone;
    OMX_COMPONENTTYPE     *pOMXComponent = NULL;
    SEC_OMX_BASECOMPONENT *pSECComponent = NULL;

    FunctionIn();

    if (hComponent == NULL) {
        ret = OMX_ErrorBadParameter;
        goto EXIT;
    }

    pOMXComponent = (OMX_COMPONENTTYPE *)hComponent;
    ret = SEC_OMX_Check_SizeVersion(pOMXComponent, sizeof(OMX_COMPONENTTYPE));
    if (ret != OMX_ErrorNone) {
        goto EXIT;
    }

    if (pOMXComponent->pComponentPrivate == NULL) {
        ret = OMX_ErrorBadParameter;
        goto EXIT;
    }

    pSECComponent = (SEC_OMX_BASECOMPONENT *)pOMXComponent->pComponentPrivate;
    if (pSECComponent->currentState == OMX_StateInvalid ) {
        ret = OMX_ErrorInvalidState;
        goto EXIT;
    }

    if (ComponentParameterStructure == NULL) {
        ret = OMX_ErrorBadParameter;
        goto EXIT;
    }

    switch (nIndex) {
    case OMX_IndexParamGetAndroidNativeBuffer:
    {
        GetAndroidNativeBufferUsageParams *pANBParams = (GetAndroidNativeBufferUsageParams *) ComponentParameterStructure;
        OMX_U32 portIndex = pANBParams->nPortIndex;

        SEC_OSAL_Log(SEC_LOG_TRACE, "%s: OMX_IndexParamGetAndroidNativeBuffer", __func__);

        ret = SEC_OMX_Check_SizeVersion(pANBParams, sizeof(GetAndroidNativeBufferUsageParams));
        if (ret != OMX_ErrorNone) {
            SEC_OSAL_Log(SEC_LOG_ERROR, "%s: SEC_OMX_Check_SizeVersion(GetAndroidNativeBufferUsageParams) is failed", __func__);
            goto EXIT;
        }

        if (portIndex >= pSECComponent->portParam.nPorts) {
            ret = OMX_ErrorBadPortIndex;
            goto EXIT;
        }

        /*
         * Frames are converted from MFC tiled output into the ANB by FIMC,
         * with a CPU fallback if the converter is unavailable. Advertise the
         * fallback producer access and the Exynos HWC consumer so gralloc
         * supplies contiguous memory and video stays off the Mali path.
         */
        pANBParams->nUsage |= GRALLOC_USAGE_SW_WRITE_OFTEN |
                              GRALLOC_USAGE_HW_COMPOSER;
    }
        break;

    default:
    {
        SEC_OSAL_Log(SEC_LOG_ERROR, "%s: Unsupported index (%d)", __func__, nIndex);
        ret = OMX_ErrorUnsupportedIndex;
        goto EXIT;
    }
        break;
    }

EXIT:
    FunctionOut();

    return ret;
}

OMX_ERRORTYPE SEC_OSAL_SetANBParameter(
    OMX_IN OMX_HANDLETYPE hComponent,
    OMX_IN OMX_INDEXTYPE  nIndex,
    OMX_IN OMX_PTR        ComponentParameterStructure)
{
    OMX_ERRORTYPE          ret = OMX_ErrorNone;
    OMX_COMPONENTTYPE     *pOMXComponent = NULL;
    SEC_OMX_BASECOMPONENT *pSECComponent = NULL;
    SEC_OMX_VIDEODEC_COMPONENT *pVideoDec = NULL;

    FunctionIn();

    if (hComponent == NULL) {
        ret = OMX_ErrorBadParameter;
        goto EXIT;
    }

    pOMXComponent = (OMX_COMPONENTTYPE *)hComponent;
    ret = SEC_OMX_Check_SizeVersion(pOMXComponent, sizeof(OMX_COMPONENTTYPE));
    if (ret != OMX_ErrorNone) {
        goto EXIT;
    }

    if (pOMXComponent->pComponentPrivate == NULL) {
        ret = OMX_ErrorBadParameter;
        goto EXIT;
    }

    pSECComponent = (SEC_OMX_BASECOMPONENT *)pOMXComponent->pComponentPrivate;
    if (pSECComponent->currentState == OMX_StateInvalid ) {
        ret = OMX_ErrorInvalidState;
        goto EXIT;
    }

    if (ComponentParameterStructure == NULL) {
        ret = OMX_ErrorBadParameter;
        goto EXIT;
    }

    pVideoDec = (SEC_OMX_VIDEODEC_COMPONENT *)pSECComponent->hComponentHandle;

    switch (nIndex) {
    case OMX_IndexParamEnableAndroidBuffers:
    {
        EnableAndroidNativeBuffersParams *pANBParams = (EnableAndroidNativeBuffersParams *) ComponentParameterStructure;
        OMX_U32 portIndex = pANBParams->nPortIndex;
        SEC_OMX_BASEPORT *pSECPort = NULL;

        SEC_OSAL_Log(SEC_LOG_TRACE, "%s: OMX_IndexParamEnableAndroidNativeBuffers", __func__);

        ret = SEC_OMX_Check_SizeVersion(pANBParams, sizeof(EnableAndroidNativeBuffersParams));
        if (ret != OMX_ErrorNone) {
            SEC_OSAL_Log(SEC_LOG_ERROR, "%s: SEC_OMX_Check_SizeVersion(EnableAndroidNativeBuffersParams) is failed", __func__);
            goto EXIT;
        }

        if (portIndex >= pSECComponent->portParam.nPorts) {
            ret = OMX_ErrorBadPortIndex;
            goto EXIT;
        }

        pSECPort = &pSECComponent->pSECPort[portIndex];
        if (CHECK_PORT_TUNNELED(pSECPort) && CHECK_PORT_BUFFER_SUPPLIER(pSECPort)) {
            ret = OMX_ErrorBadPortIndex;
            goto EXIT;
        }

        pSECPort->bIsANBEnabled = pANBParams->enable;
    }
        break;

    case OMX_IndexParamUseAndroidNativeBuffer:
    {
        UseAndroidNativeBufferParams *pANBParams = (UseAndroidNativeBufferParams *) ComponentParameterStructure;
        OMX_U32 portIndex = pANBParams->nPortIndex;
        SEC_OMX_BASEPORT *pSECPort = NULL;
        android_native_buffer_t *pANB;
        OMX_U32 nSizeBytes;

        SEC_OSAL_Log(SEC_LOG_TRACE, "%s: OMX_IndexParamUseAndroidNativeBuffer, portIndex: %d", __func__, portIndex);

        ret = SEC_OMX_Check_SizeVersion(pANBParams, sizeof(UseAndroidNativeBufferParams));
        if (ret != OMX_ErrorNone) {
            SEC_OSAL_Log(SEC_LOG_ERROR, "%s: SEC_OMX_Check_SizeVersion(UseAndroidNativeBufferParams) is failed", __func__);
            goto EXIT;
        }

        if (portIndex >= pSECComponent->portParam.nPorts) {
            ret = OMX_ErrorBadPortIndex;
            goto EXIT;
        }

        pSECPort = &pSECComponent->pSECPort[portIndex];
        if (CHECK_PORT_TUNNELED(pSECPort) && CHECK_PORT_BUFFER_SUPPLIER(pSECPort)) {
            ret = OMX_ErrorBadPortIndex;
            goto EXIT;
        }

        if (pSECPort->portState != OMX_StateIdle) {
            SEC_OSAL_Log(SEC_LOG_ERROR, "%s: Port state should be IDLE", __func__);
            ret = OMX_ErrorIncorrectStateOperation;
            goto EXIT;
        }

        pANB = pANBParams->nativeBuffer.get();

        /* MALI alignment restriction */
        nSizeBytes = ALIGN(pANB->width, 16) * ALIGN(pANB->height, 16);
        nSizeBytes += ALIGN(pANB->width / 2, 16) * ALIGN(pANB->height / 2, 16) * 2;

        ret = useAndroidNativeBuffer(pSECPort,
                                     pANBParams->bufferHeader,
                                     pANBParams->nPortIndex,
                                     pANBParams->pAppPrivate,
                                     nSizeBytes,
                                     (OMX_U8 *) pANB);
        if (ret != OMX_ErrorNone) {
            SEC_OSAL_Log(SEC_LOG_ERROR, "%s: useAndroidNativeBuffer is failed", __func__);
            goto EXIT;
        }
    }
        break;

    case OMX_IndexParamStoreMetaDataBuffer:
    {
        StoreMetaDataInBuffersParams *pANBParams = (StoreMetaDataInBuffersParams *) ComponentParameterStructure;
        OMX_U32 portIndex = pANBParams->nPortIndex;
        SEC_OMX_BASEPORT *pSECPort = NULL;

        SEC_OSAL_Log(SEC_LOG_TRACE, "%s: OMX_IndexParamStoreMetaDataBuffer", __func__);

        ret = SEC_OMX_Check_SizeVersion(pANBParams, sizeof(StoreMetaDataInBuffersParams));
        if (ret != OMX_ErrorNone) {
            SEC_OSAL_Log(SEC_LOG_ERROR, "%s: SEC_OMX_Check_SizeVersion(StoreMetaDataInBuffersParams) is failed", __func__);
            goto EXIT;
        }

        if (portIndex >= pSECComponent->portParam.nPorts) {
            ret = OMX_ErrorBadPortIndex;
            goto EXIT;
        }

        pSECPort = &pSECComponent->pSECPort[portIndex];
        if (CHECK_PORT_TUNNELED(pSECPort) && CHECK_PORT_BUFFER_SUPPLIER(pSECPort)) {
            ret = OMX_ErrorBadPortIndex;
            goto EXIT;
        }

        pSECPort->bStoreMetaData = pANBParams->bStoreMetaData;
    }
        break;

    default:
    {
        SEC_OSAL_Log(SEC_LOG_ERROR, "%s: Unsupported index (%d)", __func__, nIndex);
        ret = OMX_ErrorUnsupportedIndex;
        goto EXIT;
    }
        break;
    }

EXIT:
    FunctionOut();

    return ret;
}

OMX_ERRORTYPE SEC_OSAL_GetInfoFromMetaData(OMX_IN SEC_OMX_DATA *pBuffer,
                                           OMX_OUT OMX_PTR *ppBuf)
{
    OMX_ERRORTYPE      ret = OMX_ErrorNone;
    MetadataBufferType type;
    buffer_handle_t    pBufHandle;

    FunctionIn();

/*
 * meta data contains the following data format.
 * payload depends on the MetadataBufferType
 * --------------------------------------------------------------
 * | MetadataBufferType                         |          payload                           |
 * --------------------------------------------------------------
 *
 * If MetadataBufferType is kMetadataBufferTypeCameraSource, then
 * --------------------------------------------------------------
 * | kMetadataBufferTypeCameraSource  | physical addr. of Y |physical addr. of CbCr |
 * --------------------------------------------------------------
 *
 * If MetadataBufferType is kMetadataBufferTypeGrallocSource, then
 * --------------------------------------------------------------
 * | kMetadataBufferTypeGrallocSource    | buffer_handle_t |
 * --------------------------------------------------------------
 */

    /* MetadataBufferType */
    memcpy(&type, (MetadataBufferType *)(pBuffer->dataBuffer), sizeof(type));

    if (type == kMetadataBufferTypeCameraSource) {
        /* physical addr. of Y */
        ppBuf[0] = (OMX_PTR)(pBuffer->dataBuffer + sizeof(type));
        /* physical addr. of CbCr */
        ppBuf[1] = (OMX_PTR)(pBuffer->dataBuffer + sizeof(type) + sizeof(pBuffer->dataBuffer));
    } else if (type == kMetadataBufferTypeGrallocSource) {
        /* buffer_handle_t */
        memcpy(&pBufHandle, pBuffer->dataBuffer + sizeof(type), sizeof(buffer_handle_t));
        ppBuf[0] = (OMX_PTR)pBufHandle;
    }

EXIT:
    FunctionOut();

    return ret;
}

#ifdef __cplusplus
}
#endif
