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

#ifndef APU_DRMIF_H_
#define APU_DRMIF_H_

#include <xf86drm.h>
#include <stdint.h>
#include <apu_drm.h>

struct apu_drm_device;

/*
 * device related functions:
 */
struct apu_drm_device *apu_device_new(int fd, int device_id);
struct apu_drm_device *apu_device_ref(struct apu_drm_device *dev);
int apu_device_del(struct apu_drm_device *dev);

/*
 * buffer-object related functions:
 */
struct apu_bo *apu_bo_new(struct apu_drm_device *dev,
		uint32_t size, uint32_t flags);
struct apu_bo *apu_cached_bo_new(struct apu_drm_device *dev,
		uint32_t size, uint32_t flags);
struct apu_bo *apu_bo_ref(struct apu_bo *bo);
void apu_bo_del(struct apu_bo *bo);
uint32_t apu_bo_handle(struct apu_bo *bo);
int apu_bo_dmabuf(struct apu_bo *bo);
void *apu_bo_map(struct apu_bo *bo);

/*
 * job related functions:
 */
struct apu_drm_job *apu_new_job(struct apu_drm_device *dev, size_t size);
int apu_job_init(struct apu_drm_job *job, uint32_t cmd,
		 struct apu_bo **bos, uint32_t count,
		 void *data_in, size_t size_in, size_t size_out);
void *apu_job_get_data(struct apu_drm_job *job);
int apu_job_wait(struct apu_drm_job *job);
struct apu_drm_job *apu_job_wait_any(struct apu_drm_device *dev);
void apu_free_job(struct apu_drm_job *job);

int apu_queue(struct apu_drm_job *job);
int apu_dequeue_result(struct apu_drm_job *job, uint16_t *result,
		       void *data_out, size_t *size);
int apu_bo_iommu_map(struct apu_drm_device *dev,
		struct apu_bo **bos, uint64_t *da, uint32_t count);
int apu_bo_iommu_unmap(
		struct apu_drm_device *dev,
		struct apu_bo **bos, uint32_t count);
int apu_device_online(struct apu_drm_device *dev);

#endif /* APU_DRMIF_H_ */
