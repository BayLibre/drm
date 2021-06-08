/*
 * Copyright (C) 2021 BayLibre SAS
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef __APU_DRM_H__
#define __APU_DRM_H__

#include "drm.h"

#if defined(__cplusplus)
extern "C" {
#endif

/* Please note that modifications to all structs defined here are
 * subject to backwards-compatibility constraints.
 */

/* cache modes */
#define APU_BO_CACHED		0x00000000	/* default */
#define APU_BO_WC		0x00000002	/* write-combine */
#define APU_BO_UNCACHED		0x00000004	/* strongly-ordered (uncached) */

struct drm_apu_gem_new {
	uint32_t size;			/* in */
	uint32_t flags;			/* in */
	uint32_t handle;		/* out */
	uint64_t offset;		/* out */
};

struct drm_apu_gem_queue {
	uint32_t device;
	uint32_t cmd;
	uint32_t out_sync;
	uint64_t bo_handles;
	uint32_t bo_handle_count;
	uint16_t size_in;
	uint16_t size_out;
	uint64_t data;
};

struct drm_apu_gem_dequeue {
	uint32_t out_sync;
	uint16_t result;
	uint16_t size;
	uint64_t data;
};

struct drm_apu_gem_iommu_map {
	uint64_t bo_handles;
	uint32_t bo_handle_count;
	uint64_t bo_device_addresses;
};


struct apu_job_event {
        struct drm_event base;
        uint32_t out_sync;
};

#define APU_ONLINE		1
#define APU_CRASHED		2
#define APU_TIMEDOUT		4

struct drm_apu_state {
	uint32_t device;
	uint32_t flags;
};

#define DRM_APU_GEM_NEW			0x00
#define DRM_APU_GEM_QUEUE		0x01
#define DRM_APU_GEM_DEQUEUE		0x02
#define DRM_APU_GEM_IOMMU_MAP		0x03
#define DRM_APU_GEM_IOMMU_UNMAP		0x04
#define DRM_APU_STATE			0x05
#define DRM_APU_NUM_IOCTLS		0x06

#define DRM_IOCTL_APU_GEM_NEW		DRM_IOWR(DRM_COMMAND_BASE + DRM_APU_GEM_NEW, struct drm_apu_gem_new)
#define DRM_IOCTL_APU_GEM_QUEUE		DRM_IOWR(DRM_COMMAND_BASE + DRM_APU_GEM_QUEUE, struct drm_apu_gem_queue)
#define DRM_IOCTL_APU_GEM_DEQUEUE	DRM_IOWR(DRM_COMMAND_BASE + DRM_APU_GEM_DEQUEUE, struct drm_apu_gem_dequeue)
#define DRM_IOCTL_APU_GEM_IOMMU_MAP	DRM_IOWR(DRM_COMMAND_BASE + DRM_APU_GEM_IOMMU_MAP, struct drm_apu_gem_iommu_map)
#define DRM_IOCTL_APU_GEM_IOMMU_UNMAP	DRM_IOWR(DRM_COMMAND_BASE + DRM_APU_GEM_IOMMU_UNMAP, struct drm_apu_gem_iommu_map)
#define DRM_IOCTL_APU_STATE		DRM_IOWR(DRM_COMMAND_BASE + DRM_APU_STATE, struct drm_apu_state)

#if defined(__cplusplus)
}
#endif

#endif /* __APU_DRM_H__ */
