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

#ifndef FREEDRENO_PRIV_H_
#define FREEDRENO_PRIV_H_

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <xf86drm.h>

#include "util/hash_table.h"
#include "util/list.h"
#include "util/log.h"
#include "util/simple_mtx.h"
#include "util/u_atomic.h"
#include "util/u_debug.h"
#include "util/u_math.h"

#include "freedreno_dev_info.h"
#include "freedreno_drmif.h"
#include "freedreno_ringbuffer.h"

#define GPU_IOCTL_SUBMIT _IOC(_IOC_WRITE, 'S', 0, 0) /* arg to raw submit cmds, size/flags encoded in ioctl cmd */
#define GPU_IOCTL_SUBMIT_PRIORITY BIT(0) /* exclusive high priority submit (cannot have IN_FENCE) */
#define GPU_IOCTL_SUBMIT_IN_FENCE_SHIFT 3 /* 4-bit in-fence count */
#define GPU_IOCTL_SUBMIT_IN_FENCE_MASK 0xf

#define GPU_IOCTL_ALLOC	_IO('S', 128) /* argument is size, returns offset on success */
#define GPU_IOCTL_FREE _IO('S', 129) /* argument is size<<32|offset */
#define GPU_IOCTL_EXPORT_MEM _IO('S', 130) /* argument is size<<32|offset, returns fd on success. memory becomes owned by the dmabuf (free by closing dmabuf and not GPU_IOCTL_FREE) */
#define GPU_IOCTL_IMPORT_MEM _IO('S', 131) /* argument is fd. returns offset. will only succeed on dmabufs from this driver. does not take a reference to dmabuf (user must keep the fd open) */
#define GPU_IOCTL_CREATE_FENCE _IO('S', 132) /* create fence from rptr value */
#define GPU_IOCTL_RESET _IO('S', 133) /* reset GPU to recover from error */

extern simple_mtx_t table_lock;

/*
 * Stupid/simple growable array implementation:
 */

#define MAX_ARRAY_SIZE ((unsigned short)~0)

static inline void
grow(void **ptr, uint16_t nr, uint16_t *max, uint16_t sz)
{
   assert((nr + 1) < MAX_ARRAY_SIZE);
   if ((nr + 1) > *max) {
      if (*max > MAX_ARRAY_SIZE/2)
         *max = MAX_ARRAY_SIZE;
      else if ((*max * 2) < (nr + 1))
         *max = nr + 5;
      else
         *max = *max * 2;
      *ptr = realloc(*ptr, *max * sz);
   }
}

#define DECLARE_ARRAY(type, name)                                              \
   unsigned short nr_##name, max_##name;                                       \
   type *name;

#define APPEND(x, name, ...)                                                   \
   ({                                                                          \
      grow((void **)&(x)->name, (x)->nr_##name, &(x)->max_##name,              \
           sizeof((x)->name[0]));                                              \
      (x)->name[(x)->nr_##name] = __VA_ARGS__;                                 \
      (x)->nr_##name++;                                                        \
   })

#define READ_ONCE(x) (*(volatile __typeof__(x) *)&(x))

struct fd_device {
   int fd;
   int32_t refcnt;
   void *map;
   bool closefd;
};

/* for where @table_lock is already held: */
void fd_device_del_locked(struct fd_device *dev);
void fd_pipe_del_locked(struct fd_pipe *pipe);

struct fd_pipe_control {
   uint32_t fence;
};
#define control_ptr(pipe, member) \
   (pipe)->control_mem, offsetof(struct fd_pipe_control, member), 0, 0

struct fd_pipe {
   struct fd_device *dev;
   enum fd_pipe_id id;
   struct fd_dev_id dev_id;

   /**
    * Note refcnt is *not* atomic, but protected by table_lock, since the
    * table_lock is held in fd_bo_add_fence(), which is the hotpath.
    */
   int32_t refcnt;

   struct fd_bo *suballoc_bo;
   uint32_t suballoc_offset;

   DECLARE_ARRAY(struct fd_submit*, submits);
};

uint64_t
fd_pipe_reap_submits(struct fd_pipe *pipe);

struct fd_submit {
   int32_t refcnt;
   struct fd_pipe *pipe;

   struct fd_ringbuffer *primary;
   uint64_t rptr;

   struct fd_ringbuffer stream;

   DECLARE_ARRAY(struct fd_bo *, bos);
};

struct fd_bo {
   int dev_fd;
   uint32_t size;
   uint32_t handle;
   int32_t refcnt;
   int fd;
   //uint32_t flags; /* flags like FD_RELOC_DUMP to use for relocs to this BO */
   uint64_t iova;
   void *map;
};

#define enable_debug 0 /* TODO make dynamic */

bool fd_dbg(void);

#define INFO_MSG(fmt, ...)                                                     \
   do {                                                                        \
      if (fd_dbg())                                                            \
         mesa_logi("%s:%d: " fmt, __FUNCTION__, __LINE__, ##__VA_ARGS__);      \
   } while (0)
#define DEBUG_MSG(fmt, ...)                                                    \
   do                                                                          \
      if (enable_debug) {                                                      \
         mesa_logd("%s:%d: " fmt, __FUNCTION__, __LINE__, ##__VA_ARGS__);      \
      }                                                                        \
   while (0)
#define WARN_MSG(fmt, ...)                                                     \
   do {                                                                        \
      mesa_logw("%s:%d: " fmt, __FUNCTION__, __LINE__, ##__VA_ARGS__);         \
   } while (0)
#define ERROR_MSG(fmt, ...)                                                    \
   do {                                                                        \
      mesa_loge("%s:%d: " fmt, __FUNCTION__, __LINE__, ##__VA_ARGS__);         \
   } while (0)

#define U642VOID(x) ((void *)(unsigned long)(x))
#define VOID2U64(x) ((uint64_t)(unsigned long)(x))

#endif /* FREEDRENO_PRIV_H_ */
