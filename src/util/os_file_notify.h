/*
 * Copyright © 2025 Igalia S.L.
 * SPDX-License-Identifier: MIT
 *
 * File modification and deletion notification mechanism.
 */

#ifndef _OS_FILE_NOTIFY_H_
#define _OS_FILE_NOTIFY_H_

#include <stdbool.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

struct os_file_notifier;
typedef struct os_file_notifier *os_file_notifier_t;

/*
 * Callback function for file notification.
 * The `data` parameter is the same as the one passed to os_file_notifier_create().
 * The `path` parameter is the path of the file that was modified.
 * The `deleted` parameter is true if the file was deleted, no further events will be delivered.
 */
typedef void (*os_file_notify_cb)(void *data, const char *path, bool deleted);

/*
 * Create a new file notifier which watches the file at the specified path.
 * If a file notifier cannot be created, NULL is returned.
 * Note: The specified file must already exist, if the file doesn't exist this will fail.
 */
os_file_notifier_t
os_file_notifier_create(const char *path, os_file_notify_cb cb, void *data);

/*
 * Destroy a file notifier.
 */
void
os_file_notifier_destroy(os_file_notifier_t notifier);

#ifdef __cplusplus
}
#endif

#endif /* _OS_FILE_NOTIFY_H_ */
