/*
 * Copyright (C) 2012-2018 Rob Clark <robclark@freedesktop.org>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors:
 *    Rob Clark <robclark@freedesktop.org>
 */

#include "os/os_mman.h"

#include "freedreno_drmif.h"
#include "freedreno_priv.h"

#define BO_IOVA_BASE 0x80000000ul

//static int32_t bo_cnt;

struct fd_bo *
_fd_bo_new(struct fd_device *dev, uint32_t size, uint32_t flags)
{
   struct fd_bo *bo = malloc(sizeof(*bo));
   if (!bo)
      return NULL;

   size = (size+0xfff)&~0xfff;

   bo->handle = ioctl(dev->fd, GPU_IOCTL_ALLOC, size);
   if (!bo->handle) {
      assert(0);
      free(bo);
      return NULL;
   }

   //printf("bo new %.8x %d %d\n", bo->handle, size, bo_cnt);

   bo->dev_fd = dev->fd; // XXX dup?
   bo->fd = -1;
   bo->iova = BO_IOVA_BASE + bo->handle;
   bo->size = size;
   bo->map = dev->map + bo->handle;

   p_atomic_set(&bo->refcnt, 1);

   //p_atomic_inc(&bo_cnt);

   return bo;
}

struct fd_bo *
fd_bo_from_handle(struct fd_device *dev, uint32_t handle, uint32_t size)
{
   assert(0);
}

struct fd_bo *
fd_bo_from_dmabuf(struct fd_device *dev, int fd)
{
   struct fd_bo *bo = malloc(sizeof(*bo));
   if (!bo)
      return NULL;

   uint32_t size = lseek(fd, 0, SEEK_END);
   lseek(fd, 0, SEEK_CUR);

   assert(size && !(size & 0xfff));

   bo->handle = ioctl(dev->fd, GPU_IOCTL_IMPORT_MEM, fd);
   if (!bo->handle) {
      free(bo);
      return NULL;
   }

   bo->dev_fd = dev->fd; // XXX dup?
   bo->fd = dup(fd);
   bo->iova = BO_IOVA_BASE + bo->handle;
   bo->size = size;
   bo->map = dev->map + bo->handle;

   p_atomic_set(&bo->refcnt, 1);

   //p_atomic_inc(&bo_cnt);

   return bo;
}

struct fd_bo *
fd_bo_from_name(struct fd_device *dev, uint32_t name)
{
   assert(0);
   return NULL;
}

void
fd_bo_mark_for_dump(struct fd_bo *bo)
{
   // bo->flags |= FD_RELOC_DUMP;
}

uint64_t
fd_bo_get_iova(struct fd_bo *bo)
{
   /* ancient kernels did not support this */
   assert(bo->iova != 0);
   return bo->iova;
}

struct fd_bo *
fd_bo_ref(struct fd_bo *bo)
{
   p_atomic_inc(&bo->refcnt);
   return bo;
}

void
fd_bo_del(struct fd_bo *bo)
{
   if (!p_atomic_dec_zero(&bo->refcnt))
      return;

   //p_atomic_dec(&bo_cnt);
   //printf("fd_bo_del %.8x %d\n", bo->handle, bo_cnt);

   if (bo->fd >= 0)
      close(bo->fd);
   else
      ioctl(bo->dev_fd, GPU_IOCTL_FREE, (uint64_t) bo->size << 32 | bo->handle);

   free(bo);
}

int
fd_bo_get_name(struct fd_bo *bo, uint32_t *name)
{
   assert(0);
   return 1;
}

uint32_t
fd_bo_handle(struct fd_bo *bo)
{
   assert(0);
   return bo->handle;
}

int
fd_bo_dmabuf(struct fd_bo *bo)
{
   if (bo->fd >= 0)
      return bo->fd;

   bo->fd = ioctl(bo->dev_fd, GPU_IOCTL_EXPORT_MEM, (uint64_t) bo->size << 32 | bo->handle);
   return bo->fd;
}

uint32_t
fd_bo_size(struct fd_bo *bo)
{
   return bo->size;
}

bool
fd_bo_is_cached(struct fd_bo *bo)
{
   return true;
}

void *
fd_bo_map(struct fd_bo *bo)
{
   return bo->map;
}

int
fd_bo_cpu_prep(struct fd_bo *bo, struct fd_pipe *pipe, uint32_t op)
{
   return 0;
}

void fd_bo_cpu_fini(struct fd_bo *bo)
{
}

