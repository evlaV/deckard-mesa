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

#include "freedreno_drmif.h"
#include "freedreno_priv.h"

/**
 * priority of zero is highest priority, and higher numeric values are
 * lower priorities
 */
struct fd_pipe *
fd_pipe_new2(struct fd_device *dev, enum fd_pipe_id id, uint32_t prio)
{
   struct fd_pipe *pipe;
   uint64_t val;

   if (id > FD_PIPE_MAX) {
      ERROR_MSG("invalid pipe id: %d", id);
      return NULL;
   }

   pipe = malloc(sizeof(*pipe));

   pipe->dev = fd_device_ref(dev);
   pipe->id = id;
   p_atomic_set(&pipe->refcnt, 1);

   fd_pipe_get_param(pipe, FD_GPU_ID, &val);
   pipe->dev_id.gpu_id = val;

   fd_pipe_get_param(pipe, FD_CHIP_ID, &val);
   pipe->dev_id.chip_id = val;

   pipe->suballoc_bo = NULL;
   pipe->suballoc_offset = 0;

   simple_mtx_init(&pipe->submit_lock, mtx_plain);
   pipe->nr_submits = 0;
   pipe->max_submits = 0;
   pipe->submits = NULL;

   return pipe;
}

struct fd_pipe *
fd_pipe_new(struct fd_device *dev, enum fd_pipe_id id)
{
   return fd_pipe_new2(dev, id, 1);
}

struct fd_pipe *
fd_pipe_ref(struct fd_pipe *pipe)
{
   p_atomic_inc(&pipe->refcnt);
   return pipe;
}

struct fd_pipe *
fd_pipe_ref_locked(struct fd_pipe *pipe)
{
   pipe->refcnt++;
   return pipe;
}

void
fd_pipe_del(struct fd_pipe *pipe)
{
   fd_pipe_del_locked(pipe);
}

void
fd_pipe_del_locked(struct fd_pipe *pipe)
{
   if (!p_atomic_dec_zero(&pipe->refcnt))
      return;

   if (pipe->suballoc_bo)
      fd_bo_del(pipe->suballoc_bo);

   fd_device_del_locked(pipe->dev);

   assert(!pipe->nr_submits);

   free(pipe->submits);
   free(pipe);
}

/**
 * Discard any unflushed deferred submits.  This is called at context-
 * destroy to make sure we don't leak unflushed submits.
 */
void
fd_pipe_purge(struct fd_pipe *pipe)
{
}

int
fd_pipe_get_param(struct fd_pipe *pipe, enum fd_param_id param, uint64_t *value)
{
   switch (param) {
   case FD_DEVICE_ID: // XXX probably get rid of this..
   case FD_GPU_ID:
      *value = 650;
      return 0;
   case FD_GMEM_SIZE:
      *value = (1024+128) * 1024;
      return 0;
   case FD_GMEM_BASE:
      *value = 0;
      return 0;
   case FD_CHIP_ID:
      *value = 0x06050002;
      return 0;
   case FD_MAX_FREQ:
      *value = 1000000;
      return 0;
   case FD_TIMESTAMP:
      *value = 0;
      return 0;
   case FD_NR_PRIORITIES:
      *value = 1;
      return 0;
   case FD_CTX_FAULTS:
      *value = 0;
      return 0;
   case FD_GLOBAL_FAULTS:
      *value = 0;
      return 0;
   case FD_SUSPEND_COUNT:
      *value = 0;
      return 0;
   case FD_SYSPROF:
      *value = 0;
      return 0;
   default:
      ERROR_MSG("invalid param id: %d", param);
      return -1;
   }
}

int fd_pipe_set_param(struct fd_pipe *pipe, enum fd_param_id param, uint64_t value)
{
   assert(param == FD_SYSPROF);
   return 0;
}

const struct fd_dev_id *
fd_pipe_dev_id(struct fd_pipe *pipe)
{
   return &pipe->dev_id;
}

int
fd_pipe_wait(struct fd_pipe *pipe, const struct fd_fence *fence)
{
   return fd_pipe_wait_timeout(pipe, fence, ~0);
}

#include <sys/poll.h>

static int
sync_wait(struct pollfd *fds, uint32_t count, uint64_t timeout)
{
   int ret;
   struct timespec tmo = {};
   /* limit timeout to 1second */
   if (timeout >= 1000000000ull)
      tmo.tv_sec = 1;
   else
      tmo.tv_nsec = timeout;

   do {
      ret = ppoll(fds, count, &tmo, NULL);
   } while (ret == -1 && (errno == EINTR || errno == EAGAIN));
   return ret;
}

void
fd_pipe_reap_submits(struct fd_pipe *pipe)
{
   uint64_t rptr = read_rptr(pipe->dev);

   /* free bos from submits that completed: */
   for (int i = 0; i < pipe->nr_submits; i++) {
      struct fd_submit *submit = pipe->submits[i];

      if (rptr < submit->rptr)
         continue;

      fd_submit_del(submit);

      pipe->nr_submits--;
      pipe->submits[i] = pipe->submits[pipe->nr_submits];
      i--;
   }
}

int
fd_pipe_wait_rptr(struct fd_pipe *pipe, uint64_t fence_rptr, uint64_t timeout)
{
   /* fast path fence status check from userspace */
   if (read_rptr(pipe->dev) >= fence_rptr)
      return 0;

   if (!timeout) // non-blocking case
      return -1;

   /* wait with kernel */
   int fd = create_fence(pipe->dev, fence_rptr);
   assert(fd >= 0);

   struct pollfd pfd = { .fd = fd, .events = POLLIN };
   int ret = sync_wait(&pfd, 1, timeout);
   if (ret <= 0)
      printf("sync_wait returned %d\n", ret); // all uses of blocking wait expect it to finish, so print a warning

   close(fd);

   return ret == 1 ? 0 : -1;
}

int
fd_pipe_wait_timeout(struct fd_pipe *pipe, const struct fd_fence *fence,
                     uint64_t timeout)
{
   simple_mtx_lock(&pipe->submit_lock);
   fd_pipe_reap_submits(pipe);
   simple_mtx_unlock(&pipe->submit_lock);

   uint64_t rptr = __atomic_load_n((uint64_t*)(fence),  __ATOMIC_RELAXED);
   assert((rptr & 1) == 0);

   return fd_pipe_wait_rptr(pipe, rptr, timeout);
}
