/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef EXYNOS4_GRALLOC_USAGE_H_
#define EXYNOS4_GRALLOC_USAGE_H_

/* Samsung Exynos4 private gralloc usage extensions. */
#define GRALLOC_USAGE_PRIVATE_NONECACHE  0x00800000U
#define GRALLOC_USAGE_HW_FIMC1           0x01000000U
#define GRALLOC_USAGE_HW_ION             0x02000000U
#define GRALLOC_USAGE_YUV_ADDR           0x04000000U
#define GRALLOC_USAGE_CAMERA             0x08000000U
#define GRALLOC_USAGE_HWC_HWOVERLAY      0x20000000U
#define GRALLOC_USAGE_CAMERA3_CONTIGUOUS 0x40000000U

/*
 * These two extensions occupy Android's vendor-reserved usage bits and may
 * be passed to the native Gralloc5 allocator without colliding with standard
 * BufferUsage definitions. Keep the historical values for Exynos4 consumers.
 *
 * DO NOT forward the old 0x00800000-0x08000000 private flags directly as
 * Gralloc5 requests: those bits have standard meanings on recent Android.
 * Translate their intended behavior at legacy call sites instead.
 */
#define GRALLOC_USAGE_EXYNOS4_SCANOUT    GRALLOC_USAGE_HWC_HWOVERLAY
#define GRALLOC_USAGE_EXYNOS4_CONTIGUOUS GRALLOC_USAGE_CAMERA3_CONTIGUOUS

#endif  // EXYNOS4_GRALLOC_USAGE_H_
