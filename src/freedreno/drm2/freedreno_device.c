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

#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "util/os_file.h"

#include "freedreno_drmif.h"
#include "freedreno_priv.h"

struct fd_device *msm_device_new(int fd, drmVersionPtr version);

struct fd_device *
fd_device_new(int fd)
{
   struct fd_device *dev;

   dev = malloc(sizeof(*dev));
   if (!dev)
      return NULL;

   dev->fd = fd;
   p_atomic_set(&dev->refcnt, 1);
   dev->map = mmap(0, 4ul<<30, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
   assert(dev->map != MAP_FAILED);
   dev->closefd = 0;

   return dev;
}

/* like fd_device_new() but creates it's own private dup() of the fd
 * which is close()d when the device is finalized.
 */
struct fd_device *
fd_device_new_dup(int fd)
{
   int dup_fd = os_dupfd_cloexec(fd);
   struct fd_device *dev = fd_device_new(dup_fd);
   if (dev)
      dev->closefd = 1;
   else
      close(dup_fd);
   return dev;
}

struct fd_device *
fd_device_ref(struct fd_device *dev)
{
   p_atomic_inc(&dev->refcnt);
   return dev;
}

void
fd_device_purge(struct fd_device *dev)
{
}

static void
fd_device_del_impl(struct fd_device *dev)
{
   munmap(dev->map, 4ul<<30);

   if (dev->closefd)
      close(dev->fd);
   free(dev);
}

void
fd_device_del_locked(struct fd_device *dev)
{
   if (!p_atomic_dec_zero(&dev->refcnt))
      return;
   fd_device_del_impl(dev);
}

void
fd_device_del(struct fd_device *dev)
{
   if (!p_atomic_dec_zero(&dev->refcnt))
      return;
   fd_device_del_impl(dev);
}

int
fd_device_fd(struct fd_device *dev)
{
   return dev->fd;
}

enum fd_version
fd_device_version(struct fd_device *dev)
{
   return FD_VERSION_ROBUSTNESS;
}

bool
fd_dbg(void)
{
   static int dbg;

   if (!dbg)
      dbg = getenv("LIBGL_DEBUG") ? 1 : -1;

   return dbg == 1;
}

bool
fd_has_syncobj(struct fd_device *dev)
{
   return true;
}
