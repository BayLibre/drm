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

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>


#include "xf86drm.h"
#include "apu_drmif.h"

static const char default_device[] = "/dev/dri/card0";

int memtest(void *ptr, int value, size_t count);
int test_apu_memory(struct apu_drm_device *apu);
int test_apu_job(struct apu_drm_device *apu);

int memtest(void *ptr, int value, size_t count)
{
	unsigned int i;
	uint8_t *to_test = ptr;

	for (i = 0; i < count; i++) {
		if (to_test[i] != value)
			return 1;
	}

	return 0;
}

int test_apu_memory(struct apu_drm_device *apu)
{
	struct apu_bo *bo;
	void *ptr;
	int fd;

	printf ("Testing memory management\n");

	bo = apu_bo_new(apu, 4096, 0);
	if (!bo) {
		printf ("Failed to allocate bo\n");
		return 1;
	}

	ptr = apu_bo_map(bo);
	if (!ptr) {
		printf ("Failed to map bo\n");
		apu_bo_del(bo);
		return 1;
	}

	memset(ptr, 0x45, 4096);

	fd = apu_bo_dmabuf(bo);
	if (fd < 0) {
		printf ("Failed to dma_buf handle\n");
		apu_bo_del(bo);
	}

	apu_bo_del(bo);

	/* test dma_buf */
	ptr = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (ptr == MAP_FAILED) {
			printf ("Failed to map dma_buf fd\n");
			close(fd);
			return 1;
	}

	if (memtest(ptr, 0x45, 4096)) {
		printf ("Invalid buffer\n");
		close(fd);
		return 1;
	}

	close(fd);

	printf ("Testing memory management: OK\n");

	return 0;
}

int test_apu_job(struct apu_drm_device *apu)
{
	int ret;
	struct apu_bo *bo;
	struct apu_bo *bos[1];
	struct apu_drm_job *job;
	uint16_t result;

	printf ("Testing job queue\n");

	bo = apu_bo_new(apu, 4096, 0);
	if (!bo) {
		printf ("Failed to allocate bo\n");
		return 1;
	}

	job = apu_new_job(apu, 0);
	if (!job) {
		apu_bo_del(bo);
		return 1;
	}

	bos[0] = bo;
	ret = apu_job_init(job, 1, bos, 1, NULL, 0, 0);
	if (ret) {
		apu_free_job(job);
		apu_bo_del(bo);
		return 1;
	}

	printf ("Submitting a job\n");

	ret = apu_queue(job);
	if (ret) {
		printf ("Failed to queue a job\n");
		apu_free_job(job);
		apu_bo_del(bo);
		return 1;
	}
	/* TODO: dequeue and free the job */

	ret = apu_queue(job);
	if (ret) {
		printf ("Failed to queue a job\n");
		apu_free_job(job);
		apu_bo_del(bo);
		return 1;
	}

        ret = apu_job_wait(job);
        if (ret) {
		printf ("Failed to wait for job completion\n");
		apu_free_job(job);
		apu_bo_del(bo);
		return 1;
        }

	ret = apu_dequeue_result(job, &result, NULL, NULL);
	if (ret) {
		printf ("Failed to queue a job\n");
		apu_free_job(job);
		apu_bo_del(bo);
		return 1;
	}

        /* TODO: check for result */

	apu_free_job(job);
	apu_bo_del(bo);

	printf ("Testing job queue: OK\n");

	return 0;
}

int main(int argc, char *argv[])
{
	struct apu_drm_device *apu;
	drmVersionPtr version;
	const char *device;
	int err, fd;

	if (argc < 2)
		device = default_device;
	else
		device = argv[1];

	fd = open(device, O_RDWR);
	if (fd < 0)
		return 1;

	version = drmGetVersion(fd);
	if (version) {
		printf("Version: %d.%d.%d\n", version->version_major,
		       version->version_minor, version->version_patchlevel);
		printf("  Name: %s\n", version->name);
		printf("  Date: %s\n", version->date);
		printf("  Description: %s\n", version->desc);

		drmFreeVersion(version);
	}

	apu = apu_device_new(fd, 0);
	if (!apu)
		return 1;

	err = test_apu_memory(apu);
	if (!err)
		err = test_apu_job(apu);

	apu_device_del(apu);
	close(fd);

	return err;
}
