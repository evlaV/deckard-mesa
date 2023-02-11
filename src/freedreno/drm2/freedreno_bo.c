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

#include "util/os_mman.h"

#include "freedreno_drmif.h"
#include "freedreno_priv.h"

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

   //printf("bo new %.8x %d\n", bo->handle, size);

   bo->dev_fd = dev->fd; // XXX dup?
   bo->fd = -1;
   bo->iova = BO_IOVA_BASE + bo->handle;
   bo->size = size;
#if __SIZEOF_POINTER__ == 8
   bo->map = dev->map + bo->handle;
#else
   bo->map = NULL;
   fd_bo_map(bo); // don't always do this..
#endif

   p_atomic_set(&bo->refcnt, 1);

   //p_atomic_inc(&bo_cnt);

   return bo;
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
#if __SIZEOF_POINTER__ == 8
   bo->map = dev->map + bo->handle;
#else
   bo->map = NULL;
   fd_bo_map(bo); // don't always do this..
#endif

   p_atomic_set(&bo->refcnt, 1);

   //p_atomic_inc(&bo_cnt);

   return bo;
}

struct fd_bo *
fd_bo_from_handle(struct fd_device *dev, uint32_t handle, uint32_t size)
{
   return fd_bo_from_dmabuf(dev, handle);
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
#if __SIZEOF_POINTER__ == 8
      ioctl(bo->dev_fd, GPU_IOCTL_FREE, (uint64_t) bo->size << 32 | bo->handle);
#else
      ioctl(bo->dev_fd, GPU_IOCTL_FREE | (_IOC_WRITE << _IOC_DIRSHIFT), (uint32_t[]) {bo->handle, bo->size});
#endif

#if __SIZEOF_POINTER__ != 8
   if (bo->map)
      munmap(bo->map, bo->size);
#endif

   free(bo);
}

int
fd_bo_get_name(struct fd_bo *bo, uint32_t *name)
{
   assert(0);
   return 1;
}

static int
_fd_bo_dmabuf(struct fd_bo *bo)
{
   if (bo->fd >= 0)
      return bo->fd;

#if __SIZEOF_POINTER__ == 8
   bo->fd = ioctl(bo->dev_fd, GPU_IOCTL_EXPORT_MEM, (uint64_t) bo->size << 32 | bo->handle);
#else
   bo->fd = ioctl(bo->dev_fd, GPU_IOCTL_EXPORT_MEM | (_IOC_WRITE << _IOC_DIRSHIFT), (uint32_t[]) {bo->handle, bo->size});
#endif
   return bo->fd;
}

int
fd_bo_dmabuf(struct fd_bo *bo)
{
   return dup(_fd_bo_dmabuf(bo));
}

uint32_t
fd_bo_handle(struct fd_bo *bo)
{
   return _fd_bo_dmabuf(bo);
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
#if __SIZEOF_POINTER__ != 8
   if (!bo->map) {
      bo->map = mmap(0, bo->size, PROT_READ | PROT_WRITE, MAP_SHARED, bo->dev_fd, bo->handle);
      assert(bo->map != MAP_FAILED);
   }
#endif
   return bo->map;
}

void
fd_bo_upload(struct fd_bo *bo, void *src, unsigned off, unsigned len)
{
   memcpy(fd_bo_map(bo) + off, src, len);
}

int
fd_bo_cpu_prep(struct fd_bo *bo, struct fd_pipe *pipe, uint32_t op)
{
   if ((op & (FD_BO_PREP_READ | FD_BO_PREP_WRITE)) == 0)
      return 0;

   uint64_t rptr = 0;

   // find the highest rptr for this BO in the pending submits
   // note this is not really an efficient way to do things
   simple_mtx_lock(&pipe->submit_lock);

   for (int i = 0; i < pipe->nr_submits; i++) {
      struct fd_submit *submit = pipe->submits[i];

      bool bo_in_submit = false;
      for (int i = 0; i < submit->nr_bos; i++) {
         if (submit->bos[i] == bo) {
            bo_in_submit = true;
            break;
         }
      }

      if (bo_in_submit && rptr < submit->rptr)
         rptr = submit->rptr;
   }

   simple_mtx_unlock(&pipe->submit_lock);

   return fd_pipe_wait_rptr(pipe, rptr, (op & FD_BO_PREP_NOSYNC) ? 0 : ~0ull);
}

void fd_bo_cpu_fini(struct fd_bo *bo)
{
}

