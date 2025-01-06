/*
 * Copyright © 2025 Igalia S.L.
 * SPDX-License-Identifier: MIT
 */

#include "os_file_notify.h"
#include "detect_os.h"
#include "log.h"
#include "u_thread.h"

#if DETECT_OS_LINUX
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/poll.h>

#include <errno.h>
#include <stdlib.h>

struct os_file_notifier {
   int ifd;
   int file_wd, dir_wd;
   int efd;
   os_file_notify_cb cb;
   void *data;
   thrd_t thread;
   volatile bool quit;
   char file_path[PATH_MAX], dir_path[PATH_MAX];
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
   if (access(notifier->file_path, F_OK) != -1) {
      notifier->cb(notifier->data, notifier->file_path, false, false, false);
   } else {
      /* The file doesn't exist, we cannot watch it. */
      notifier->cb(notifier->data, notifier->file_path, false, true, false);
   }

   while (!notifier->quit) {
      /* Poll on the inotify file descriptor and the eventfd file descriptor. */
      struct pollfd fds[] = {
         {notifier->ifd, POLLIN, 0},
         {notifier->efd, POLLIN, 0},
      };
      if (poll(fds, ARRAY_SIZE(fds), -1) == -1) {
         if (errno == EINTR || errno == EAGAIN)
            continue;

         mesa_logw("Failed to poll on file notifier FDs: %s", strerror(errno));
         return -1;
      }

      if (fds[1].revents & POLLIN) {
         /* The eventfd is used to wake up the thread when the notifier is destroyed. */
         eventfd_t val;
         eventfd_read(notifier->efd, &val);
         if (val == 1)
            return 0; /* Quit the thread. */
      }

      len = read(notifier->ifd, buf, sizeof(buf));
      if (len == -1) {
         if (errno == EINTR || errno == EAGAIN)
            continue;

         mesa_logw("Failed to read inotify events: %s", strerror(errno));
         return -1;
      } else {
         for (char *ptr = buf; ptr < buf + len; ptr += ((struct inotify_event *)ptr)->len + sizeof(struct inotify_event)) {
            struct inotify_event *event = (struct inotify_event *)ptr;

            bool created = false, deleted = false, dir_deleted = false;
            if (event->wd == notifier->dir_wd) {
               /* Check if the event is about the directory itself or the file. */
               if (event->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) {
                  /* The directory was deleted/moved, we cannot watch it or the file anymore. */
                  dir_deleted = true;
               } else if (strcmp(event->name, basename(notifier->file_path)) == 0) {
                  /* If it's a file event, ensure that the event is about the file we are watching. */
                  if (event->mask & IN_CREATE) {
                     created = true;

                     /* The file was just created, add a watch to it. */
                     notifier->file_wd = inotify_add_watch(notifier->ifd, notifier->file_path, IN_CLOSE_WRITE);
                     if (notifier->file_wd == -1) {
                        mesa_logw("Failed to add inotify watch for file");
                        return -1;
                     }
                  } else if (event->mask & IN_DELETE) {
                     deleted = true;

                     /* The file was deleted, we cannot watch it anymore. */
                     inotify_rm_watch(notifier->ifd, notifier->file_wd);
                     notifier->file_wd = -1;
                  }
               }
            }

            notifier->cb(notifier->data, notifier->file_path, created, deleted, dir_deleted);
            if (dir_deleted)
               return 0;
         }
      }
   }

   return 0;
}

os_file_notifier_t
os_file_notifier_create(const char *file_path, os_file_notify_cb cb, void *data, const char **error_str)
{
#define RET_ERR(s)       \
   do {                  \
      if (error_str) {   \
         *error_str = s; \
      }                  \
      return NULL;       \
   } while (0)

   os_file_notifier_t notifier = calloc(1, sizeof(struct os_file_notifier));
   if (!notifier)
      RET_ERR("Failed to allocate memory for file notifier");

   notifier->ifd = inotify_init1(IN_NONBLOCK);
   if (notifier->ifd == -1) {
      free(notifier);
      RET_ERR("Failed to initialize inotify");
   }

   strncpy(notifier->file_path, file_path, sizeof(notifier->file_path) - 1);
   notifier->file_path[sizeof(notifier->file_path) - 1] = 0;

   notifier->file_wd = inotify_add_watch(notifier->ifd, notifier->file_path, IN_CLOSE_WRITE);
   if (notifier->file_wd == -1 && errno != ENOENT) {
      close(notifier->ifd);
      free(notifier);
      RET_ERR("Failed to add inotify watch for file");
   }

   /* Determine the parent directory path of the file. */
   char *last_slash = strrchr(notifier->file_path, '/');
   if (last_slash) {
      size_t len = last_slash - notifier->file_path;
      strncpy(notifier->dir_path, notifier->file_path, len);
      notifier->dir_path[len] = 0;
   } else {
      notifier->dir_path[0] = '.';
      notifier->dir_path[1] = 0;
   }

   notifier->dir_wd = inotify_add_watch(notifier->ifd, notifier->dir_path, IN_CREATE | IN_MOVE | IN_DELETE | IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR);
   if (notifier->dir_wd == -1) {
      close(notifier->ifd);
      free(notifier);
      if (errno == ENOENT)
         RET_ERR("The folder containing the watched file doesn't exist");
      RET_ERR("Failed to add inotify watch for directory");
   }

   notifier->efd = eventfd(0, EFD_NONBLOCK);
   if (notifier->efd == -1) {
      close(notifier->ifd);
      free(notifier);
      RET_ERR("Failed to create eventfd");
   }

   notifier->cb = cb;
   notifier->data = data;

   if (u_thread_create(&notifier->thread, os_file_notifier_thread, notifier) != 0) {
      close(notifier->ifd);
      close(notifier->efd);
      free(notifier);
      RET_ERR("Failed to create file notifier thread");
   }

#undef RET_ERR

   return notifier;
}

void
os_file_notifier_destroy(os_file_notifier_t notifier)
{
   if (!notifier)
      return;

   notifier->quit = true;
   eventfd_write(notifier->efd, 1);
   thrd_join(notifier->thread, NULL);

   close(notifier->ifd);
   close(notifier->efd);
   free(notifier);
}

#else /* !DETECT_OS_LINUX */

struct os_file_notifier {};

os_file_notifier_t
os_file_notifier_create(const char *file_path, os_file_notify_cb cb, void *data)
{
   (void)file_path;
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