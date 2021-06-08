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

#include <stdlib.h>
#include <linux/stddef.h>
#include <linux/types.h>
#include <errno.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>

#include <libdrm_macros.h>
#include <libsync.h>
#include <util_double_list.h>
#include <xf86drm.h>
#include <xf86atomic.h>

#include "apu_drm.h"
#include "apu_drmif.h"

static pthread_mutex_t table_lock = PTHREAD_MUTEX_INITIALIZER;
static void * dev_table;

struct apu_cached_bo {
	struct list_head free;
	struct list_head allocated;
};

struct apu_drm_device {
	int fd;
	int device_id;
	atomic_t refcnt;

	/* The handle_table is used to track GEM bo handles associated w/
	 * this fd.  This is needed, in particular, when importing
	 * dmabuf's because we don't want multiple 'struct apu_bo's
	 * floating around with the same handle.  Otherwise, when the
	 * first one is apu_bo_del()'d the handle becomes no longer
	 * valid, and the remaining 'struct apu_bo's are left pointing
	 * to an invalid handle (and possible a GEM bo that is already
	 * free'd).
	 */
	void *handle_table;

	pthread_mutex_t queue_lock;
	struct list_head queue;

	void *cached_alloc_table;
};

struct apu_drm_job {
	struct apu_drm_device *dev;
	struct drm_apu_gem_queue *req;
	uint32_t syncobj;
	struct list_head node;

	char data[0];
};

/* a GEM buffer object allocated from the DRM device */
struct apu_bo {
	struct apu_drm_device	*dev;
	int		fd;		/* dmabuf handle */
	uint32_t	handle;
	uint32_t	size;
	void		*map;		/* userspace mmap'ing (if there is one) */
	uint64_t	offset;		/* offset to mmap() */
	atomic_t	refcnt;

	struct list_head cache;
	int cached;
};

static void _apu_bo_del(struct apu_bo *bo);

static struct apu_drm_device * apu_device_new_impl(int fd)
{
	struct apu_drm_device *dev = calloc(sizeof(*dev), 1);
	if (!dev)
		return NULL;
	dev->fd = fd;
	atomic_set(&dev->refcnt, 1);
	dev->handle_table = drmHashCreate();
	dev->cached_alloc_table = drmHashCreate();
	pthread_mutex_init(&dev->queue_lock, NULL);
	list_inithead(&dev->queue);
	return dev;
}

drm_public struct apu_drm_device * apu_device_new(int fd, int device_id)
{
	struct apu_drm_device *dev = NULL;

	pthread_mutex_lock(&table_lock);

	if (!dev_table)
		dev_table = drmHashCreate();

	if (drmHashLookup(dev_table, fd, (void **)&dev)) {
		/* not found, create new device */
		dev = apu_device_new_impl(fd);
		dev->device_id = device_id;
		drmHashInsert(dev_table, fd, dev);
	} else {
		/* found, just incr refcnt */
		dev = apu_device_ref(dev);
	}

	pthread_mutex_unlock(&table_lock);

	return dev;
}

drm_public struct apu_drm_device * apu_device_ref(struct apu_drm_device *dev)
{
	atomic_inc(&dev->refcnt);
	return dev;
}

static void free_apu_cached_bo(struct apu_drm_device *dev)
{
	struct apu_cached_bo *cached_bo;
	struct apu_bo *bo, *tmp;
	unsigned long key;
	int ret;

	ret = drmHashFirst(dev->cached_alloc_table, &key, (void **)&cached_bo);
	if (!ret)
		return;

	do {
		LIST_FOR_EACH_ENTRY_SAFE(bo, tmp, &cached_bo->free, cache) {
			_apu_bo_del(bo);
		}
		free(cached_bo);
		drmHashDelete(dev->cached_alloc_table, key);
	} while (drmHashNext(dev->cached_alloc_table, &key, (void **)&cached_bo));
}

drm_public int apu_device_del(struct apu_drm_device *dev)
{
	if (!atomic_dec_and_test(&dev->refcnt))
		return 0;

	pthread_mutex_lock(&table_lock);
	drmHashDestroy(dev->handle_table);
	free_apu_cached_bo(dev);
	drmHashDestroy(dev->cached_alloc_table);
	drmHashDelete(dev_table, dev->fd);
	pthread_mutex_unlock(&table_lock);
	free(dev);

	return 1;
}

/* allocate a new buffer object, call w/ table_lock held */
static struct apu_bo * bo_from_handle(struct apu_drm_device *dev,
		uint32_t handle)
{
	struct apu_bo *bo = calloc(sizeof(*bo), 1);
	if (!bo) {
		struct drm_gem_close req = {
				.handle = handle,
		};
		drmIoctl(dev->fd, DRM_IOCTL_GEM_CLOSE, &req);
		return NULL;
	}
	bo->dev = apu_device_ref(dev);
	bo->handle = handle;
	bo->fd = -1;
	atomic_set(&bo->refcnt, 1);
	/* add ourselves to the handle table: */
	drmHashInsert(dev->handle_table, handle, bo);
	return bo;
}

/* allocate a new buffer object */
static struct apu_bo * apu_bo_new_impl(struct apu_drm_device *dev,
		uint32_t size, uint32_t flags)
{
	struct apu_bo *bo = NULL;
	struct drm_apu_gem_new req = {
			.size = size,
			.flags = flags,
	};

	if (size == 0) {
		goto fail;
	}

	if (drmCommandWriteRead(dev->fd, DRM_APU_GEM_NEW, &req, sizeof(req)))
		goto fail;

	pthread_mutex_lock(&table_lock);
	bo = bo_from_handle(dev, req.handle);
	pthread_mutex_unlock(&table_lock);
	bo->size = size;
	bo->offset = req.offset;
	list_inithead(&bo->cache);
	bo->cached = 0;

	return bo;

fail:
	free(bo);
	return NULL;
}

/* allocate a new buffer object */
drm_public struct apu_bo *
apu_bo_new(struct apu_drm_device *dev, uint32_t size, uint32_t flags)
{
	return apu_bo_new_impl(dev, size, flags);
}

/* allocate a new buffer object */
drm_public struct apu_bo *
apu_cached_bo_new(struct apu_drm_device *dev, uint32_t size, uint32_t flags)
{
	struct apu_cached_bo *cached_bo = NULL;
	struct apu_bo *bo = NULL;

	if (!drmHashLookup(dev->cached_alloc_table, size, (void **)&cached_bo))
	{
		if (!LIST_IS_EMPTY(&cached_bo->free)) {
			bo = LIST_FIRST_ENTRY(&cached_bo->free, struct apu_bo,
					      cache);
			apu_bo_ref(bo);
			list_del(&bo->cache);
		}
	}

	if (!bo) {
		bo = apu_bo_new_impl(dev, size, flags);
		if (!bo)
			return NULL;
		bo->cached = 1;
	}

	if (!cached_bo) {
		cached_bo = malloc(sizeof(*cached_bo));
		if (!cached_bo) {
			apu_bo_del(bo);
			return NULL;
		}
		list_inithead(&cached_bo->free);
		list_inithead(&cached_bo->allocated);
		drmHashInsert(dev->cached_alloc_table, size, cached_bo);
	}

	list_add(&bo->cache, &cached_bo->allocated);

	return bo;
}

/* allocate a new buffer object */
static struct apu_bo * apu_bo_user_new_impl(struct apu_drm_device *dev,
		void *hostptr, uint32_t size, uint32_t flags)
{
	struct apu_bo *bo = NULL;
	struct drm_apu_gem_user_new req = {
			.hostptr = (uint64_t)hostptr,
			.size = size,
			.flags = flags,
	};

	if (size == 0) {
		goto fail;
	}

	if (drmCommandWriteRead(dev->fd, DRM_APU_GEM_USER_NEW, &req, sizeof(req)))
		goto fail;

	pthread_mutex_lock(&table_lock);
	bo = bo_from_handle(dev, req.handle);
	pthread_mutex_unlock(&table_lock);
	bo->size = size;
	bo->offset = req.offset;

	return bo;

fail:
	free(bo);
	return NULL;
}

/* allocate a new (un-tiled) buffer object */
drm_public struct apu_bo *
apu_bo_user_new(struct apu_drm_device *dev, void *hostptr,
		uint32_t size, uint32_t flags)
{
	return apu_bo_user_new_impl(dev, hostptr, size, flags);
}

drm_public struct apu_bo *apu_bo_ref(struct apu_bo *bo)
{
	atomic_inc(&bo->refcnt);
	return bo;
}

/* destroy a buffer object */
drm_public void apu_cached_bo_del(struct apu_bo *bo)
{
	struct apu_cached_bo *cached_bo = NULL;
	if (drmHashLookup(bo->dev->cached_alloc_table, bo->size,
			(void **)&cached_bo))
		return;

	list_del(&bo->cache);
	list_add(&bo->cache, &cached_bo->free);
}

static void _apu_bo_del(struct apu_bo *bo)
{
	if (bo->handle) {
		struct drm_gem_close req = {
				.handle = bo->handle,
		};
		pthread_mutex_lock(&table_lock);
		drmHashDelete(bo->dev->handle_table, bo->handle);
		drmIoctl(bo->dev->fd, DRM_IOCTL_GEM_CLOSE, &req);
		pthread_mutex_unlock(&table_lock);
	}

	apu_device_del(bo->dev);

	free(bo);
}

drm_public void apu_bo_del(struct apu_bo *bo)
{
	if (!bo) {
		return;
	}

	if (!atomic_dec_and_test(&bo->refcnt))
		return;

	if (bo->map) {
		munmap(bo->map, bo->size);
		bo->map = NULL;
	}

	/* return the bo to the cache allocator */
	if (!LIST_IS_EMPTY(&bo->cache) || bo->cached) {
		apu_cached_bo_del(bo);
		return;
	}

	if (bo->fd >= 0) {
		close(bo->fd);
		bo->fd = -1;
	}

	_apu_bo_del(bo);
}

drm_public void *apu_bo_map(struct apu_bo *bo)
{
	if (!bo->map) {
		bo->map = mmap(0, bo->size, PROT_READ | PROT_WRITE,
				MAP_SHARED, bo->dev->fd, bo->offset);
		if (bo->map == MAP_FAILED) {
			bo->map = NULL;
		}
	}
	return bo->map;
}

drm_public uint32_t apu_bo_handle(struct apu_bo *bo)
{
	return bo->handle;
}

/* caller owns the dmabuf fd that is returned and is responsible
 * to close() it when done
 */
drm_public int apu_bo_dmabuf(struct apu_bo *bo)
{
	if (bo->fd < 0) {
		struct drm_prime_handle req = {
				.handle = bo->handle,
				.flags = DRM_CLOEXEC | DRM_RDWR,
		};
		int ret;

		ret = drmIoctl(bo->dev->fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &req);
		if (ret)
			return ret;

		bo->fd = req.fd;
	}
	return dup(bo->fd);
}

drm_public int apu_bo_iommu_map(struct apu_drm_device *dev,
			struct apu_bo **bos, uint64_t *das, uint32_t count)
{
	unsigned int i;
	int ret;
	uint32_t bo_handles[count];
	uint32_t bo_das[count];

	struct drm_apu_gem_iommu_map req = {
		.bo_handles = (uint64_t)bo_handles,
		.bo_handle_count = count,
		.bo_device_addresses = (uint64_t)bo_das,
	};

	for (i = 0; i < count; i++)
		bo_handles[i] = bos[i]->handle;

	ret = drmCommandWrite(dev->fd, DRM_APU_GEM_IOMMU_MAP, &req, sizeof(req));
	if (ret)
		return ret;

	for (i = 0; i < count; i++)
		das[i] = bo_das[i];

	return 0;
}

drm_public int apu_bo_iommu_unmap(struct apu_drm_device *dev,
			struct apu_bo **bos, uint32_t count)
{
	unsigned int i;
	int ret;
	uint32_t bo_handles[count];

	struct drm_apu_gem_iommu_map req = {
		.bo_handles = (uint64_t)bo_handles,
		.bo_handle_count = count,
	};

	for (i = 0; i < count; i++)
		bo_handles[i] = bos[i]->handle;

	ret = drmCommandWrite(dev->fd, DRM_APU_GEM_IOMMU_UNMAP, &req, sizeof(req));
	if (ret)
		return ret;

	return 0;
}

drm_public
struct apu_drm_job *apu_new_job(struct apu_drm_device *dev, size_t size)
{
	struct apu_drm_job *job;
	int ret;

	job = malloc(sizeof(*job) + size);
	if (!job) {
		errno = ENOMEM;
		return NULL;
	}
	job->dev = dev;

	ret = drmSyncobjCreate(dev->fd, 0, &job->syncobj);
	if (ret) {
		free(job);
		return NULL;
	}

	return job;
}

drm_public
int apu_job_init(struct apu_drm_job *job, uint32_t cmd,
		 struct apu_bo **bos, uint32_t count,
		 void *data_in, size_t size_in, size_t size_out)
{
	uint32_t *bo_handles;
	uint32_t i;

	job->req = malloc(sizeof(*job->req));
	if (!job->req)
		return -ENOMEM;

	bo_handles = malloc(sizeof(*bo_handles) * count);
	if (!bo_handles) {
		free(job->req);
		return -ENOMEM;
	}

	job->req->device = job->dev->device_id;
	job->req->cmd = cmd;
	job->req->bo_handles = (uint64_t)bo_handles;
	job->req->bo_handle_count = count;
	job->req->out_sync = job->syncobj;
	job->req->size_in = size_in;
	job->req->size_out = size_out;
	job->req->data = (uint64_t)data_in;

	for (i = 0; i < count; i++)
		bo_handles[i] = bos[i]->handle;

	list_inithead(&job->node);

	return 0;
}

drm_public
void *apu_job_get_data(struct apu_drm_job *job)
{
	return job->data;
}

drm_public
int apu_job_wait(struct apu_drm_job *job)
{
	return drmSyncobjWait(job->dev->fd, &job->syncobj,
			      1, INT64_MAX, 0, NULL);
}

drm_public
struct apu_drm_job *apu_job_wait_any(struct apu_drm_device *dev)
{
	int ret;
	uint8_t buffer[4096];
	struct apu_drm_job *job;
	struct apu_job_event *event;

	ret = sync_wait(dev->fd, 1000);
	if (ret) {
		return NULL;
	}

	ret = read(dev->fd, buffer, sizeof(*event));
	if (ret < 0) {
		// TODO: log error and eventually, exit
	}

	event = (struct apu_job_event *)buffer;
	if (event->base.type != 0x80000000) {
		// TODO: log error and eventually, exit
	}

	pthread_mutex_lock(&dev->queue_lock);
	LIST_FOR_EACH_ENTRY(job, &dev->queue, node) {
		if (job->syncobj == event->out_sync) {
			pthread_mutex_unlock(&dev->queue_lock);
			return job;
		}
	}
	pthread_mutex_unlock(&dev->queue_lock);

	return NULL;
}

drm_public
void apu_free_job(struct apu_drm_job *job)
{
	free((void *)job->req->bo_handles);
	free(job->req);
	free(job);
}


drm_public
int apu_queue(struct apu_drm_job *job)
{
	struct apu_drm_device *dev = job->dev;
	int ret;

	pthread_mutex_lock(&dev->queue_lock);
	ret = drmCommandWrite(dev->fd, DRM_APU_GEM_QUEUE,
			      job->req, sizeof(*job->req));
	if (ret) {
		pthread_mutex_unlock(&dev->queue_lock);
		return ret;
	}

	list_add(&job->node, &dev->queue);
	pthread_mutex_unlock(&dev->queue_lock);

	return 0;
}

drm_public
int apu_dequeue_result(struct apu_drm_job *job, uint16_t *result,
		       void *data_out, size_t *size)
{
	struct apu_drm_device *dev = job->dev;
	int ret;

	struct drm_apu_gem_dequeue req = {
		.out_sync = job->syncobj,
		.data = (uint64_t)data_out,
	};

	ret = drmCommandWriteRead(dev->fd, DRM_APU_GEM_DEQUEUE, &req,
				  sizeof(req));
	if (ret)
		return ret;

	*result = req.result;
	if (size)
		*size = req.size;

	pthread_mutex_lock(&dev->queue_lock);
	list_del(&job->node);
	pthread_mutex_unlock(&dev->queue_lock);

	return 0;	
}

drm_public int apu_device_online(struct apu_drm_device *dev)
{
	int ret;

	struct drm_apu_state req = {
		.device = dev->device_id,
	};

	ret = drmCommandWriteRead(dev->fd, DRM_APU_STATE, &req, sizeof(req));
	if (ret)
		return ret;

	return req.flags & APU_ONLINE;
}
