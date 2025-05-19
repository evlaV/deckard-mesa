/*
 * Copyright © 2025 Igalia S.L.
 * SPDX-License-Identifier: MIT
 */

#ifdef HAVE_LINUX_USER_EVENTS_H

#include "u_linux_trace_events.h"

#include "util/log.h"
#include "util/ralloc.h"
#include "util/u_call_once.h"

#include <fcntl.h>
#include <linux/user_events.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/uio.h>

static struct util_linux_trace_events_state {
   util_once_flag once;
   int user_events_data_fd;
} util_linux_trace_events_state = {
   .once = UTIL_ONCE_FLAG_INIT,
   .user_events_data_fd = -1,
};

static void
util_linux_trace_events_register_tracepoint(struct util_linux_trace_events_tracepoint *tracepoint)
{
   tracepoint->data = rzalloc(NULL, struct util_linux_trace_events_tracepoint_data);

   struct user_reg reg = {
      .size = sizeof(struct user_reg),
      .enable_bit = 0,
      .enable_size = sizeof(tracepoint->data->enable),
      .flags = 0,
      .enable_addr = (__u64) &tracepoint->data->enable,
      .name_args = (__u64) tracepoint->command,
      .write_index = 0,
   };

   int ret = ioctl(util_linux_trace_events_state.user_events_data_fd, DIAG_IOCSREG, &reg);
   if (ret < 0) {
      mesa_logw("util_linux_trace_events: failed to register tracepoint '%s': %d %s",
                tracepoint->name, ret, strerror(-ret));
      return;
   }

   tracepoint->data->write_index = reg.write_index;
}

static void
util_linux_trace_events_unregister_tracepoint(struct util_linux_trace_events_tracepoint *tracepoint)
{
   struct user_unreg unreg = {
      .size = sizeof(struct user_unreg),
      .disable_bit = 0,
      .disable_addr = (__u64) &tracepoint->data->enable,
   };

   int ret = ioctl(util_linux_trace_events_state.user_events_data_fd, DIAG_IOCSUNREG, &unreg);
   if (ret < 0) {
      mesa_logw("util_linux_trace_events: failed to unregister tracepoint '%s': %d %s",
                tracepoint->name, ret, strerror(-ret));
   }

   ralloc_free(tracepoint->data);
}

__attribute__((weak)) util_linux_trace_events_for_each_tracepoint tu_linux_trace_events_register_tracepoints;
__attribute__((weak)) util_linux_trace_events_for_each_tracepoint tu_linux_trace_events_unregister_tracepoints;

static void util_linux_trace_events_deinit(void);

static void
util_linux_trace_events_init_once(void)
{
   util_linux_trace_events_state.user_events_data_fd = open("/sys/kernel/tracing/user_events_data", O_RDWR);
   if (util_linux_trace_events_state.user_events_data_fd == -1) {
      mesa_logw("util_linux_trace_events: failed to open user_events_data: %d %s",
                errno, strerror(errno));
      return;
   }

   if (tu_linux_trace_events_register_tracepoints)
      tu_linux_trace_events_register_tracepoints(util_linux_trace_events_register_tracepoint);

   atexit(util_linux_trace_events_deinit);
}

void
util_linux_trace_events_init(void)
{
   util_call_once(&util_linux_trace_events_state.once, util_linux_trace_events_init_once);
}

static void
util_linux_trace_events_deinit(void)
{
   if (util_linux_trace_events_state.user_events_data_fd == -1)
      return;

   if (tu_linux_trace_events_unregister_tracepoints)
      tu_linux_trace_events_unregister_tracepoints(util_linux_trace_events_unregister_tracepoint);
}

void
util_linux_trace_events_write(const struct util_linux_trace_events_tracepoint *tracepoint,
                              const void *payload,
                              size_t payload_size)
{
   if (util_linux_trace_events_state.user_events_data_fd == -1 || !tracepoint->data->enable)
      return;

   struct iovec io[2] = {
      [0] = {
         .iov_base = (void *) &tracepoint->data->write_index,
         .iov_len = sizeof(tracepoint->data->write_index),
      },
      [1] = {
         .iov_base = (void *) payload,
         .iov_len = payload_size,
      },
   };

   ssize_t ret = writev(util_linux_trace_events_state.user_events_data_fd, io, 2);
   if (ret < 0) {
      mesa_logw("util_linux_trace_events: failed to write tracepoint event: %s",
                strerror(ret));
   }
}

#endif /* HAVE_LINUX_USER_EVENTS_H */
