/*
 * Copyright © 2025 Igalia S.L.
 * SPDX-License-Identifier: MIT
 */

#include "os_file_notify.h"
#include "detect_os.h"
#include "log.h"
#include "u_thread.h"

#if DETECT_OS_LINUX
#include <sys/inotify.h>

#include <errno.h>
#include <stdlib.h>

struct os_file_notifier {
   int fd;
   int wd;
   os_file_notify_cb cb;
   void *data;
   thrd_t thread;
   volatile bool quit;
   char path[PATH_MAX];
};

#define INOTIFY_BUF_LEN ((10 * (sizeof(struct inotify_event) + NAME_MAX + 1)))

static int
os_file_notifier_thread(void *data)
{
   os_file_notifier_t notifier = data;
   char buf[INOTIFY_BUF_LEN] __attribute__((aligned(__alignof__(struct inotify_event))));
   ssize_t len;

   u_thread_setname("File Notifier");

   /* To ensure the callback sets the file up to the initial state. */
   if (access(notifier->path, F_OK) != -1) {
      notifier->cb(notifier->data, notifier->path, false);
   } else {
      /* The file doesn't exist, we cannot watch it. */
      notifier->cb(notifier->data, notifier->path, true);
      return 0;
   }

   while (!notifier->quit) {
      struct timespec interval = {.tv_nsec = 100000000}; /* 1OOms */

      len = read(notifier->fd, buf, sizeof(buf));
      if (len == -1) {
         if (errno == EINTR || errno == EAGAIN)
            continue;

         mesa_logw("Failed to read inotify events: %s", strerror(errno));
         return -1;
      } else {
         for (char *ptr = buf; ptr < buf + len; ptr += ((struct inotify_event *)ptr)->len + sizeof(struct inotify_event)) {
            struct inotify_event *event = (struct inotify_event *)ptr;
            bool deleted = false;

            if (event->mask & IN_DELETE_SELF) {
               deleted = true;

               thrd_sleep(&interval, NULL);

               if (access(notifier->path, F_OK) == -1) {
                  /* Some editors delete and recreate the file when saving. The file was deleted and
                   * the IN_DELETE_SELF event was triggered. We wait for an interval need to check
                   * if the file was recreated. If so, we consider it as a modification rather than
                   * a deletion.
                   */
                  deleted = false;

                  /* The inotify watcher is stale after IN_DELETE_SELF, we need to recreate it. */
                  inotify_rm_watch(notifier->fd, notifier->wd);
                  notifier->wd = inotify_add_watch(notifier->fd, notifier->path, IN_DELETE_SELF | IN_CLOSE_WRITE);
                  if (notifier->wd == -1)
                     /* If the watch cannot be recreated, we consider the file to be deleted. */
                     deleted = true;
               }
            }

            notifier->cb(notifier->data, notifier->path, deleted);
            if (deleted)
               break;
         }
      }

      thrd_sleep(&interval, NULL);
   }

   return 0;
}

os_file_notifier_t
os_file_notifier_create(const char *path, os_file_notify_cb cb, void *data)
{
   os_file_notifier_t notifier = calloc(1, sizeof(struct os_file_notifier));
   if (!notifier)
      return NULL;

   notifier->fd = inotify_init1(IN_NONBLOCK);
   if (notifier->fd == -1) {
      free(notifier);
      return NULL;
   }

   notifier->wd = inotify_add_watch(notifier->fd, path, IN_DELETE_SELF | IN_CLOSE_WRITE);
   if (notifier->wd == -1) {
      close(notifier->fd);
      free(notifier);
      return NULL;
   }

   notifier->cb = cb;
   notifier->data = data;
   strncpy(notifier->path, path, sizeof(notifier->path));

   if (u_thread_create(&notifier->thread, os_file_notifier_thread, notifier) != 0) {
      inotify_rm_watch(notifier->fd, notifier->wd);
      close(notifier->fd);
      free(notifier);
      return NULL;
   }

   return notifier;
}

void
os_file_notifier_destroy(os_file_notifier_t notifier)
{
   if (!notifier)
      return;

   notifier->quit = true;
   thrd_join(notifier->thread, NULL);

   close(notifier->fd);
   free(notifier);
}

#else /* !DETECT_OS_LINUX */

struct os_file_notifier {};

os_file_notifier_t
os_file_notifier_create(const char *path, os_file_notify_cb cb, void *data)
{
   (void)path;
   (void)cb;
   (void)data;

   return NULL;
}

void
os_file_notifier_destroy(os_file_notifier_t notifier)
{
   (void)notifier;
}

#endif