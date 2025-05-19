/*
 * Copyright © 2025 Igalia S.L.
 * SPDX-License-Identifier: MIT
 */

#ifndef _U_LINUX_TRACE_EVENTS_H
#define _U_LINUX_TRACE_EVENTS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef HAVE_LINUX_USER_EVENTS_H

struct util_linux_trace_events_tracepoint_data {
   uint32_t enable;
   uint32_t write_index;
};

struct util_linux_trace_events_tracepoint {
   const char *name;
   const char *command;
   void (*write)(const struct util_linux_trace_events_tracepoint *tp,
                 const void *payload,
                 const void *indirect);
   struct util_linux_trace_events_tracepoint_data *data;
};

typedef void (*util_linux_trace_events_for_each_tracepoint)(void (*)(struct util_linux_trace_events_tracepoint *));

void util_linux_trace_events_init(void);

void util_linux_trace_events_write(const struct util_linux_trace_events_tracepoint *tracepoint,
                                   const void *payload,
                                   size_t payload_size);

#endif /* HAVE_LINUX_USER_EVENTS_H */

#ifdef __cplusplus
}
#endif

#endif /* _U_LINUX_TRACE_EVENTS_H */
