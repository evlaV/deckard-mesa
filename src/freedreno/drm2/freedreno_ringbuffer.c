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

#include <assert.h>

#include "freedreno_drmif.h"
#include "freedreno_priv.h"
#include "freedreno_ringbuffer.h"

struct fd_submit *
fd_submit_new(struct fd_pipe *pipe)
{
   struct fd_submit *submit = malloc(sizeof(*submit));
   if (!submit)
      return NULL;

   submit->refcnt = 1;
   submit->pipe = fd_pipe_ref(pipe);
   submit->primary = NULL;
   submit->rptr = 0;

   memset(&submit->stream, 0, sizeof(submit->stream));
   submit->stream.submit = submit;

   submit->nr_bos = 0;
   submit->max_bos = 0;
   submit->bos = NULL;

   return submit;
}

void
fd_submit_del(struct fd_submit *submit)
{
   if (!p_atomic_dec_zero(&submit->refcnt))
      return;

   if (submit->primary)
      fd_ringbuffer_del(submit->primary);

   fd_pipe_del(submit->pipe);

   for (int i = 0; i < submit->nr_bos; i++)
      fd_bo_del(submit->bos[i]);

   free(submit->bos);

   free(submit);
}

struct fd_submit *
fd_submit_ref(struct fd_submit *submit)
{
   p_atomic_inc(&submit->refcnt);
   return submit;
}

#include "util/libsync.h"

int
fd_submit_flush(struct fd_submit *submit, int in_fence_fd,
                struct fd_submit_fence *out_fence)
{
   uint32_t cmds[32*1024];
   struct fd_ringbuffer ring = {
      .cur = cmds,
      .end = &cmds[ARRAY_SIZE(cmds)],
      .start = cmds,
   };

   // block until its possible to submit (TODO: submit defer mechanism instead)
   if (in_fence_fd >= 0) {
      int ret = sync_wait(in_fence_fd, -1);
      assert(ret == 0);
   }

   assert(submit->rptr == 0);

   uint64_t iova = submit->primary->iova;

   OUT_PKT7(&ring, CP_INDIRECT_BUFFER_PFE, 3);
   OUT_RING(&ring, (uint32_t) iova);
   OUT_RING(&ring, iova >> 32);
   OUT_RING(&ring, submit->primary->cur - submit->primary->start);

   OUT_PKT7(&ring, CP_EVENT_WRITE, 4);
   OUT_RING(&ring, CP_EVENT_WRITE_0_EVENT(CACHE_FLUSH_TS) | CP_EVENT_WRITE_0_IRQ);
   uint64_t iova2 = DUMMY_TIMESTAMP_IOVA;
   OUT_RING(&ring, (uint32_t) iova2);
   OUT_RING(&ring, iova2 >> 32);
   OUT_RING(&ring, 0);


   unsigned cmd = _IOC(_IOC_WRITE, 'S', 0, (ring.cur - cmds) * 4);
   // for arm64, wptr is returned directly as the syscall return value
   // for 32-bit, the return value is only 32-bits so wptr is returned in memory instead
#if __SIZEOF_POINTER__ == 8
   uint64_t wptr = syscall(SYS_ioctl, submit->pipe->dev->fd, cmd, cmds);
   assert((int64_t) wptr >= 0);
#else
   int ret = syscall(SYS_ioctl, submit->pipe->dev->fd, cmd | (_IOC_READ << _IOC_DIRSHIFT), cmds);
   assert(ret == 0);
   uint64_t wptr = (uint64_t) cmds[1] << 32 | cmds[0];
#endif

   submit->rptr = wptr;

   if (out_fence) {
      __atomic_store_n(out_fence->rptr, wptr, __ATOMIC_RELAXED); // atomic store: it may be shared memory
      if (out_fence->use_fence_fd) {
         out_fence->fence_fd = create_fence(submit->pipe->dev, wptr);
         assert(out_fence->fence_fd >= 0);
      }
   }

   simple_mtx_lock(&submit->pipe->submit_lock);

   APPEND(submit->pipe, submits, fd_submit_ref(submit));

   fd_pipe_reap_submits(submit->pipe);

   simple_mtx_unlock(&submit->pipe->submit_lock);

   return 0;
}

static bool
fd_ring_new_bo(struct fd_ringbuffer *ring, uint32_t size)
{
   struct fd_bo *bo = _fd_bo_new(ring->submit->pipe->dev, size, FD_BO_GPUREADONLY);
   if (!bo)
      return false;

   ring->start = ring->cur = bo->map;
   ring->end = bo->map + size;
   ring->iova = bo->iova;

   APPEND(ring->submit, bos, bo);
   return true;
}

struct fd_ringbuffer *
fd_submit_new_ringbuffer(struct fd_submit *submit, uint32_t size,
                         enum fd_ringbuffer_flags flags)
{
   assert(!(flags & _FD_RINGBUFFER_OBJECT));
   if (flags & FD_RINGBUFFER_STREAMING) {
      assert(!(flags & FD_RINGBUFFER_GROWABLE));
      assert(!(flags & FD_RINGBUFFER_PRIMARY));
   }

   if (!(flags & FD_RINGBUFFER_GROWABLE))
      assert(flags == FD_RINGBUFFER_STREAMING);

   struct fd_ringbuffer *ring = malloc(sizeof(*ring));
   if (!ring)
      return NULL;

   ring->submit = submit;

   if (flags & FD_RINGBUFFER_GROWABLE) {
      size = 1024*1024; // XXX
      if (!fd_ring_new_bo(ring, size)) {
         free(ring);
         return NULL;
      }
   } else {
      struct fd_ringbuffer *stream = &submit->stream;

      assert((size & 3) == 0);
      assert(size <= 32*1024);

      if ((void*) stream->cur + size > (void*) stream->end) {
         if (!fd_ring_new_bo(stream, 32*1024)) {
            free(ring);
            return NULL;
         }
      }

      ring->iova = stream->iova + ((void*) stream->cur - (void*) stream->start);
      ring->start = ring->cur = stream->cur;
      stream->cur = (void*) stream->cur + size;
      ring->end = stream->cur;
   }

   ring->refcnt = 1;
   ring->flags = flags;

   if (flags & FD_RINGBUFFER_PRIMARY) {
      assert(!submit->primary);
      submit->primary = fd_ringbuffer_ref(ring);
   }

   return ring;
}

void
fd_ringbuffer_del(struct fd_ringbuffer *ring)
{
   if (!p_atomic_dec_zero(&ring->refcnt))
      return;

   if (ring->flags & _FD_RINGBUFFER_OBJECT) {
      for (int i = 0; i < ring->nr_bos; i++)
         fd_bo_del(ring->bos[i]);
      free(ring->bos);
   }

   free(ring);
}

struct fd_ringbuffer *
fd_ringbuffer_new_object(struct fd_pipe *pipe, uint32_t size)
{
   struct fd_ringbuffer *ring = malloc(sizeof(*ring));
   if (!ring)
      return NULL;

   struct fd_bo *bo = pipe->suballoc_bo;

   if (!bo || pipe->suballoc_offset + size > bo->size) {
      assert(size <= 32*1024);
      bo = _fd_bo_new(pipe->dev, 32*1024, FD_BO_GPUREADONLY);
      if (!bo) {
         free(ring);
         return NULL;
      }

      if (pipe->suballoc_bo)
         fd_bo_del(pipe->suballoc_bo);
      pipe->suballoc_bo = bo;
      pipe->suballoc_offset = 0;
   }

   ring->start = ring->cur = bo->map + pipe->suballoc_offset;
   ring->end = (void*) ring->start + size;
   ring->iova = bo->iova + pipe->suballoc_offset;
   ring->refcnt = 1;
   ring->flags = _FD_RINGBUFFER_OBJECT;

   ring->nr_bos = 0;
   ring->max_bos = 0;
   ring->bos = NULL;

   APPEND(ring, bos, fd_bo_ref(bo));

   pipe->suballoc_offset += size;
   pipe->suballoc_offset = align(pipe->suballoc_offset, 64);

   return ring;
}

void fd_ringbuffer_ref_bo(struct fd_ringbuffer *ring, struct fd_bo *bo)
{
   if (ring->flags & _FD_RINGBUFFER_OBJECT)
      APPEND(ring, bos, fd_bo_ref(bo));
   else
      APPEND(ring->submit, bos, fd_bo_ref(bo));
}

void fd_ringbuffer_ref_bos(struct fd_ringbuffer *ring, struct fd_ringbuffer *target)
{
   if (ring->flags & _FD_RINGBUFFER_OBJECT) {
      assert(target->flags & _FD_RINGBUFFER_OBJECT);

      for (int i = 0; i < target->nr_bos; i++)
         APPEND(ring, bos, fd_bo_ref(target->bos[i]));
      return;
   }

   if (!(target->flags & _FD_RINGBUFFER_OBJECT))
      return;

   for (int i = 0; i < target->nr_bos; i++)
      APPEND(ring->submit, bos, fd_bo_ref(target->bos[i]));
}
