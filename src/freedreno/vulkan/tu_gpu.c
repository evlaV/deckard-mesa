/*
 * Copyright 2021 Valve Corporation
 * SPDX-License-Identifier: MIT
 */

#include "tu_device.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include "tu_cmd_buffer.h"

#define BO_IOVA_BASE 0x80000000ul
#define BO_IOVA_TO_PTR(dev, x) (dev->physical_device->map + ((x)-BO_IOVA_BASE))
#define RPTR_FULL(dev, i) (*(volatile uint64_t*) ((dev)->physical_device->map + 0x20000 - 128 + (i)*8) | 3)

#define GPU_IOCTL_SUBMIT _IOC(_IOC_WRITE, 'S', 0, 0) /* arg to raw submit cmds, size/flags encoded in ioctl cmd */
#define GPU_IOCTL_SUBMIT_PRIORITY BIT(0) /* exclusive high priority submit */

#define GPU_IOCTL_ALLOC	_IO('S', 128) /* argument is size, returns offset on success */
#define GPU_IOCTL_FREE _IO('S', 129) /* argument is size<<32|offset */
#define GPU_IOCTL_EXPORT_MEM _IO('S', 130) /* argument is size<<32|offset, returns fd on success. memory becomes owned by the dmabuf (free by closing dmabuf and not GPU_IOCTL_FREE) */
#define GPU_IOCTL_IMPORT_MEM _IO('S', 131) /* argument is fd. returns offset. will only succeed on dmabufs from this driver. does not take a reference to dmabuf (user must keep the fd open) */
#define GPU_IOCTL_CREATE_FENCE _IO('S', 132) /* create fence from rptr value */
#define GPU_IOCTL_RESET _IO('S', 133) /* reset GPU to recover from error */

struct timeline_point {
   uint64_t value; /* value for this point (vk API value) */
   uint64_t rptr; /* rptr value associated with timeline point */
};

struct timeline {
   pthread_mutex_t lock;
   pthread_cond_t cond; /* signals changes to max_value */
   uint32_t num_points;
   uint64_t value; /* current value */
   uint64_t max_value; /* max pending value */
   struct timeline_point points[248]; /* pending values (signaled by GPU). sorted by value */
};
_Static_assert(sizeof(struct timeline) < 4096, "too large");

static void timeline_update(struct timeline *tl, uint64_t rptr)
{
   uint32_t j = 0;
   for (uint32_t i = 0; i < tl->num_points; i++) {
      if (rptr >= tl->points[i].rptr) {
         tl->value = tl->points[i].value;
         continue;
      }
      tl->points[j++] = tl->points[i];
   }
   tl->num_points = j;
}

static uint64_t timeline_rptr_for_value(struct timeline *tl, uint64_t value)
{
   for (uint32_t i = 0; i < tl->num_points; i++) {
      if (tl->points[i].value >= value)
         return tl->points[i].rptr;
   }
   unreachable("no rptr for timeline value");
}

struct VkFence_T {
   struct vk_object_base base;
   bool is_timeline;
   int mem_fd;
   union {
      struct timeline *timeline;
      uint64_t *rptr;
   };
};

struct VkSemaphore_T {
   struct vk_object_base base;
   bool is_timeline;
   int mem_fd;
   union {
      struct timeline *timeline;
      uint64_t *rptr;
   };
};

VK_DEFINE_NONDISP_HANDLE_CASTS(VkFence_T, base, VkFence, VK_OBJECT_TYPE_FENCE)
VK_DEFINE_NONDISP_HANDLE_CASTS(VkSemaphore_T, base, VkSemaphore, VK_OBJECT_TYPE_SEMAPHORE)

int
tu_drm_submitqueue_new(const struct tu_device *dev,
                       int priority,
                       uint32_t *queue_id)
{
   *queue_id = 0;
   return 0;
}

void
tu_bo_allow_dump(struct tu_device *dev, struct tu_bo *bo)
{
   //flto look here
   //mtx_lock(&dev->bo_mutex);
   //dev->bo_list[bo->bo_list_idx].flags |= MSM_SUBMIT_BO_DUMP;
   //mtx_unlock(&dev->bo_mutex);
}

void
tu_drm_submitqueue_close(const struct tu_device *dev, uint32_t queue_id)
{
}

VkResult
tu_bo_init_new_explicit_iova(struct tu_device *dev,
                             struct tu_bo **_bo,
                             uint64_t size,
                             uint64_t client_iova,
                             enum tu_bo_alloc_flags flags, const char *name)
{
   struct tu_bo *bo = malloc(sizeof(*bo));
   assert(bo);

   size = (size+0xfff)&~0xfff;

   unsigned offset = ioctl(dev->fd, GPU_IOCTL_ALLOC, size);
   assert(offset != 0);

   bo->gem_handle = -1;
   bo->size = size;
   bo->iova = BO_IOVA_BASE + offset;
#if __SIZEOF_POINTER__ == 8
   bo->map = BO_IOVA_TO_PTR(dev, bo->iova);
#else
   bo->map = mmap(0, bo->size, PROT_READ | PROT_WRITE, MAP_SHARED, dev->fd, offset);
   assert(bo->map != MAP_FAILED);
#endif
   bo->refcnt = 1;

   *_bo = bo;
   return VK_SUCCESS;
}

VkResult
tu_bo_init_dmabuf(struct tu_device *dev,
                  struct tu_bo **_bo,
                  uint64_t size,
                  int fd)
{
   struct tu_bo *bo = malloc(sizeof(*bo));
   assert(bo);

   uint32_t size2 = lseek(fd, 0, SEEK_END);
   lseek(fd, 0, SEEK_CUR);

   assert(size);
   size = (size+0xfff)&~0xfff;
   assert(size2 >= size);

   unsigned offset = ioctl(dev->fd, GPU_IOCTL_IMPORT_MEM, fd);
   assert(offset != 0);

   bo->gem_handle = fd;
   bo->size = size;
   bo->iova = BO_IOVA_BASE + offset;
#if __SIZEOF_POINTER__ == 8
   bo->map = BO_IOVA_TO_PTR(dev, bo->iova);
#else
   bo->map = mmap(0, bo->size, PROT_READ | PROT_WRITE, MAP_SHARED, dev->fd, offset);
   assert(bo->map != MAP_FAILED);
#endif
   bo->refcnt = 1;

   *_bo = bo;
   return VK_SUCCESS;
}

int
tu_bo_export_dmabuf(struct tu_device *dev, struct tu_bo *bo)
{
   if ((int) bo->gem_handle < 0) {
#if __SIZEOF_POINTER__ == 8
      bo->gem_handle = ioctl(dev->fd, GPU_IOCTL_EXPORT_MEM, (uint64_t) bo->size << 32 | (bo->iova-BO_IOVA_BASE));
#else
      bo->gem_handle = ioctl(dev->fd, GPU_IOCTL_EXPORT_MEM | (_IOC_WRITE << _IOC_DIRSHIFT), (uint32_t[]) {(bo->iova-BO_IOVA_BASE), bo->size });
#endif
   }
   return dup(bo->gem_handle);
}

VkResult
tu_bo_map(struct tu_device *dev, struct tu_bo *bo)
{
   assert(bo->map);
   return VK_SUCCESS;
}

void
tu_bo_finish(struct tu_device *dev, struct tu_bo *bo)
{
   if (!p_atomic_dec_zero(&bo->refcnt))
      return;

   assert(bo->size);

   int dma_fd = bo->gem_handle;
   if (dma_fd >= 0) {
      close(dma_fd);
      return;
   }

#if __SIZEOF_POINTER__ == 8
   int ret = ioctl(dev->fd, GPU_IOCTL_FREE, (uint64_t) bo->size << 32 | (bo->iova-BO_IOVA_BASE));
#else
   int ret = ioctl(dev->fd, GPU_IOCTL_FREE | (_IOC_WRITE << _IOC_DIRSHIFT), (uint32_t[]) {(bo->iova-BO_IOVA_BASE), bo->size });
#endif
   assert(!ret);
}

static inline int create_fence(int fd, uint64_t rptr)
{
#if __SIZEOF_POINTER__ == 8
   return ioctl(fd, GPU_IOCTL_CREATE_FENCE, rptr);
#else
   return ioctl(fd, GPU_IOCTL_CREATE_FENCE | (_IOC_WRITE << _IOC_DIRSHIFT), &rptr);
#endif
}

VkResult
tu_enumerate_devices(struct vk_instance *vk_instance)
{
   struct tu_instance *instance =
      container_of(vk_instance, struct tu_instance, vk);

   struct tu_physical_device *device =
      vk_zalloc(&instance->vk.alloc, sizeof(*device), 8,
                VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);

   int fd = open("/dev/gpu", O_RDWR | O_CLOEXEC);
   if (fd < 0)
      return VK_ERROR_INCOMPATIBLE_DRIVER;

   device->instance = instance;
   if (instance->vk.enabled_extensions.KHR_display)
      device->master_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
   else
      device->master_fd = -1;
   device->local_fd = fd;

   device->dev_id.gpu_id = 650;
   device->dev_id.chip_id = 0x06050002;
   device->gmem_size = (1024 + 128) * 1024;
   device->gmem_base = 0;

   device->heap.size = tu_get_system_heap_size();
   device->heap.used = 0u;
   device->heap.flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;

#if __SIZEOF_POINTER__ == 8
   device->map = mmap(0, GPU_MEM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
#else
   device->map = mmap(0, 0x20000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
#endif
   assert(device->map != MAP_FAILED);

   if (tu_physical_device_init(device, instance) != VK_SUCCESS) {
      close(fd);
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   list_addtail(&device->vk.link, &instance->vk.physical_devices.list);

   return VK_SUCCESS;
}

void
tu_GetPhysicalDeviceExternalSemaphoreProperties(
   VkPhysicalDevice physicalDevice,
   const VkPhysicalDeviceExternalSemaphoreInfo *info,
   VkExternalSemaphoreProperties *props)
{
   props->exportFromImportedHandleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
   props->compatibleHandleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
   props->externalSemaphoreFeatures = VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT;
}

#include "freedreno_pm4.h"

struct simple_cs {
   uint32_t *cur;
#ifndef NDEBUG
   uint32_t *reserved_end;
#endif
};

static inline void
simple_cs_emit(struct simple_cs *cs, uint32_t value)
{
   assert(cs->cur < cs->reserved_end);
   *cs->cur = value;
   ++cs->cur;
}

static inline void
simple_cs_emit_pkt4(struct simple_cs *cs, uint16_t regindx, uint16_t cnt)
{
   simple_cs_emit(cs, pm4_pkt4_hdr(regindx, cnt));
}

static inline void
simple_cs_emit_pkt7(struct simple_cs *cs, uint8_t opcode, uint16_t cnt)
{
   simple_cs_emit(cs, pm4_pkt7_hdr(opcode, cnt));
}

static inline void
simple_cs_emit_qw(struct simple_cs *cs, uint64_t value)
{
   simple_cs_emit(cs, (uint32_t) value);
   simple_cs_emit(cs, (uint32_t) (value >> 32));
}

VkResult
tu_QueueSubmit(VkQueue _queue, uint32_t count, const VkSubmitInfo *submits, VkFence _fence)
{
   TU_FROM_HANDLE(tu_queue, queue, _queue);
   TU_FROM_HANDLE(VkFence_T, fence, _fence);

   struct tu_device *device = queue->device;
   uint32_t cmds[32*1024];
   struct simple_cs cs;
   long ret = queue->rptr;
#ifndef NDEBUG
   cs.reserved_end = (void*) cmds + sizeof(cmds);
#endif

   for (uint32_t i = 0; i < count; i++) {
      const VkSubmitInfo *submit = &submits[i];
      const VkTimelineSemaphoreSubmitInfo *sinfo =
         vk_find_struct_const(submit->pNext, TIMELINE_SEMAPHORE_SUBMIT_INFO_KHR);

      cs.cur = cmds;

      for (uint32_t i = 0; i < submit->waitSemaphoreCount; i++) {
         TU_FROM_HANDLE(VkSemaphore_T, semaphore, submit->pWaitSemaphores[i]);
         /* for timeline semaphores, block until its safe to submit
          * TODO: deferred submits instead of blocking
          */
         if (semaphore->is_timeline) {
            struct timeline *tl = semaphore->timeline;
            pthread_mutex_lock(&tl->lock);
            while (tl->max_value < sinfo->pWaitSemaphoreValues[i])
               pthread_cond_wait(&tl->cond, &tl->lock);
            pthread_mutex_unlock(&tl->lock);
         } else {
            if (semaphore->rptr[0] == ~0ul)
               printf("attempted to wait for unsignaled fence\n");
         }
      }

      struct tu_cmd_buffer *cmd_buffers[submit->commandBufferCount]; // XXX for autotune

      if (submit->commandBufferCount == 0) {
         /* if submit is empty, just use the rptr value of the last submit to this queue */
         goto skip_empty_submit;
      }

      if ( TU_DEBUG(PREEMPT) ) {
         simple_cs_emit_pkt7(&cs, CP_SET_MARKER, 1);
         simple_cs_emit(&cs, 0xe); // disable preemption
      }

      for (uint32_t i = 0; i < submit->commandBufferCount; i++) {
         TU_FROM_HANDLE(tu_cmd_buffer, cmdbuf, submit->pCommandBuffers[i]);
         struct tu_cs *tcs = &cmdbuf->cs;

         cmd_buffers[i] = cmdbuf;

         for (uint32_t i = 0; i < tcs->entry_count; i++) {
            struct tu_cs_entry *entry = &tcs->entries[i];
            simple_cs_emit_pkt7(&cs, CP_INDIRECT_BUFFER_PFE, 3);
            simple_cs_emit_qw(&cs, entry->bo->iova + entry->offset);
            simple_cs_emit(&cs, entry->size / sizeof(uint32_t));
         }
      }

      if (tu_autotune_submit_requires_fence(cmd_buffers, submit->commandBufferCount)) {
         struct tu_cs *autotune_cs =
            tu_autotune_on_submit(queue->device,
                                  &queue->device->autotune,
                                  cmd_buffers,
                                  submit->commandBufferCount);
         struct tu_cs_entry *entry = &autotune_cs->entries[0];

         simple_cs_emit_pkt7(&cs, CP_INDIRECT_BUFFER_PFE, 3);
         simple_cs_emit_qw(&cs, entry->bo->iova + entry->offset);
         simple_cs_emit(&cs, entry->size / sizeof(uint32_t));
      }

      simple_cs_emit_pkt7(&cs, CP_EVENT_WRITE, 4);
      simple_cs_emit(&cs, CP_EVENT_WRITE_0_EVENT(CACHE_FLUSH_TS) | CP_EVENT_WRITE_0_IRQ);
      simple_cs_emit_qw(&cs, device->global_bo->iova + gb_offset(seqno_dummy));
      simple_cs_emit(&cs, 0);

      if ( TU_DEBUG(PREEMPT) ) {
         simple_cs_emit_pkt7(&cs, CP_SET_MARKER, 1);
         simple_cs_emit(&cs, 0xd); // re-enable preemption
      }

      unsigned cmd = GPU_IOCTL_SUBMIT;
      cmd |= (cs.cur - cmds) * 4 << _IOC_SIZESHIFT;
      if (TU_DEBUG(PREEMPT))
         cmd |= GPU_IOCTL_SUBMIT_PRIORITY;

      ret = syscall(SYS_ioctl, device->fd, cmd, cmds);
      assert(ret >= 0);
      queue->rptr = ret;
   skip_empty_submit:;

      for (uint32_t i = 0; i < submit->signalSemaphoreCount; i++) {
         TU_FROM_HANDLE(VkSemaphore_T, semaphore, submit->pSignalSemaphores[i]);
         if (semaphore->is_timeline) {
            struct timeline *tl = semaphore->timeline;
            uint64_t value = sinfo->pSignalSemaphoreValues[i];
            pthread_mutex_lock(&tl->lock);
            timeline_update(tl, RPTR_FULL(device, tl->points[0].rptr & 1));
            assert(!tl->num_points || (tl->points[0].rptr & 1) == (ret & 1));
            assert(tl->num_points < ARRAY_SIZE(tl->points));
            tl->points[tl->num_points++] = (struct timeline_point) {
               .value = value,
               .rptr = ret,
            };
            assert(tl->max_value < value);
            tl->max_value = value;
            pthread_mutex_unlock(&tl->lock);
            pthread_cond_broadcast(&tl->cond);
         } else {
            semaphore->rptr[0] = ret;
         }
      }
   }

   if (fence)
      fence->rptr[0] = ret;

   return VK_SUCCESS;
}

VkResult
tu_CreateFence(VkDevice _device,
              const VkFenceCreateInfo *info,
              const VkAllocationCallbacks *alloc,
              VkFence *p_fence)
{
   TU_FROM_HANDLE(tu_device, device, _device);

   const VkSemaphoreTypeCreateInfoKHR *tinfo =
      vk_find_struct_const(info->pNext, SEMAPHORE_TYPE_CREATE_INFO_KHR);

   struct VkFence_T *fence = vk_object_alloc(&device->vk, alloc, sizeof(*fence),
            (info->sType == VK_STRUCTURE_TYPE_FENCE_CREATE_INFO) ? VK_OBJECT_TYPE_FENCE : VK_OBJECT_TYPE_SEMAPHORE);
   if (!fence)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   /* TODO: only use shared memory if necessary */

   fence->mem_fd = memfd_create("fence", MFD_CLOEXEC);
   assert(fence->mem_fd >= 0);

   int ret = ftruncate(fence->mem_fd, 0x1000);
   assert(ret == 0);

   fence->is_timeline = false;
   fence->rptr = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fence->mem_fd, 0);
   assert(fence->rptr != MAP_FAILED);

   if (tinfo && tinfo->semaphoreType == VK_SEMAPHORE_TYPE_TIMELINE) {
      struct timeline *tl = fence->timeline;
      fence->is_timeline = true;

      pthread_mutexattr_t mutexattr;
      pthread_condattr_t condattr;

      pthread_mutexattr_init(&mutexattr);
      pthread_mutexattr_setpshared(&mutexattr, PTHREAD_PROCESS_SHARED);
      pthread_condattr_init(&condattr);
      pthread_condattr_setpshared(&condattr, PTHREAD_PROCESS_SHARED);

      int ret = pthread_mutex_init(&tl->lock, &mutexattr);
      assert(ret == 0);
      ret = pthread_cond_init(&tl->cond, &condattr);
      assert(ret == 0);
      tl->num_points = 0;
      tl->value = tl->max_value = tinfo->initialValue;
   } else {
      if (info->flags & VK_FENCE_CREATE_SIGNALED_BIT)
         fence->rptr[0] = 0;
      else
         fence->rptr[0] = ~0ul;
   }

   *p_fence = (VkFence) (uintptr_t) (fence);
   return VK_SUCCESS;
}

void
tu_DestroyFence(VkDevice _device, VkFence _fence, const VkAllocationCallbacks *alloc)
{
   TU_FROM_HANDLE(tu_device, device, _device);
   TU_FROM_HANDLE(VkFence_T, fence, _fence);

   if (!fence)
      return;

   close(fence->mem_fd);
   munmap(fence->rptr, 0x1000);

   vk_object_free(&device->vk, alloc, fence);
}

VkResult
tu_ImportFenceFdKHR(VkDevice _device, const VkImportFenceFdInfoKHR *info)
{
   TU_FROM_HANDLE(tu_device, device, _device);
   TU_FROM_HANDLE(VkFence_T, fence, info->fence);

   assert((info->flags & VK_FENCE_IMPORT_TEMPORARY_BIT) == 0);
   assert(info->handleType == VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT);

   close(fence->mem_fd);
   munmap(fence->rptr, 0x1000);

   fence->mem_fd = info->fd;
   fence->rptr = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fence->mem_fd, 0);
   assert(fence->rptr != MAP_FAILED);

   return VK_SUCCESS;
}

VkResult
tu_GetFenceFdKHR(VkDevice _device, const VkFenceGetFdInfoKHR *info, int *p_fd)
{
   TU_FROM_HANDLE(tu_device, device, _device);
   TU_FROM_HANDLE(VkFence_T, fence, info->fence);

   if (info->handleType == VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT) {
      int fd = create_fence(device->fd, fence->rptr[0]);
      assert(fd >= 0);
      *p_fd = fd;
      return VK_SUCCESS;
   }

   assert(info->handleType == VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT);
   *p_fd = dup(fence->mem_fd);
   return VK_SUCCESS;
}

/* note: not exactly right because info->flags doesn't have VK_FENCE_CREATE_SIGNALED_BIT */
__attribute__ ((alias ("tu_CreateFence")))
VkResult tu_CreateSemaphore(VkDevice device, const VkSemaphoreCreateInfo *info,
                           const VkAllocationCallbacks *alloc, VkSemaphore *p_semaphore);
__attribute__ ((alias ("tu_DestroyFence")))
void tu_DestroySemaphore(VkDevice device, VkSemaphore sem, const VkAllocationCallbacks *alloc);
__attribute__ ((alias ("tu_ImportFenceFdKHR")))
VkResult tu_ImportSemaphoreFdKHR(VkDevice device, const VkImportSemaphoreFdInfoKHR *info);
__attribute__ ((alias ("tu_GetFenceFdKHR")))
VkResult tu_GetSemaphoreFdKHR(VkDevice device, const VkSemaphoreGetFdInfoKHR *info, int *p_fd);

static int
rptr_wait(struct tu_device *device, uint64_t *rptr, uint64_t timeout)
{
   struct pollfd pfd[2];
   int num_fds = 0;
   uint64_t rptr_now[2] = {
      RPTR_FULL(device, 0),
      RPTR_FULL(device, 1),
   };

   for (uint32_t i = 0; i < 2; i++) {
      if (rptr_now[i] >= rptr[i])
         continue;

      int fd = create_fence(device->fd, rptr[i]);
      assert(fd >= 0);
      pfd[num_fds++] = (struct pollfd) { .fd = fd, .events = POLLIN };
   }

   if (num_fds == 0)
      return 1;

   struct timespec tmo = {};
   /* limit timeout to 1second */
   if (timeout >= 1000000000ull)
      tmo.tv_sec = 1;
   else
      tmo.tv_nsec = timeout;

   int ret;
   do {
      /* use syscall() so that timeout is updated on error */
      ret = syscall(SYS_ppoll, pfd, num_fds, &tmo, NULL);
   } while (ret == -EINTR || ret == -EAGAIN);

   for (uint32_t i = 0; i < num_fds; i++)
      close(pfd[i].fd);
   return ret;
}

VkResult
tu_WaitForFences(VkDevice _device, uint32_t count, const VkFence *p_fence, VkBool32 wait_all, uint64_t timeout)
{
   TU_FROM_HANDLE(tu_device, device, _device);

   uint64_t rptr[2] = {};

   for (uint32_t i = 0; i < count; i++) {
      TU_FROM_HANDLE(VkFence_T, fence, p_fence[i]);

      uint64_t ts = fence->rptr[0];
      if (ts == ~0ull) {
         printf("vkWaitForFences tried to wait for unsignaled fence\n");
         continue;
      }
      bool j = ts & 1;

      if (rptr[j] == 0) {
         rptr[j] = ts;
         continue;
      }

      if (wait_all)
         rptr[j] = MAX2(rptr[j], ts);
      else
         rptr[j] = MIN2(rptr[j], ts);
   }

   if (rptr[0] && rptr[1])
      assert(!wait_all); /* TODO */

   int ret = rptr_wait(device, rptr, timeout);

   if (ret <= 0)
      printf("WaitForFences timed out (%d / %" PRIu64 ")\n", ret, timeout);

   return ret > 0 ? VK_SUCCESS : VK_TIMEOUT;
}

VkResult
tu_ResetFences(VkDevice _device, uint32_t count, const VkFence *p_fence)
{
   for (uint32_t i = 0; i < count; i++) {
      TU_FROM_HANDLE(VkFence_T, fence, p_fence[i]);
      fence->rptr[0] = ~0ul;
   }
   return VK_SUCCESS;
}

VkResult
tu_GetFenceStatus(VkDevice _device, VkFence _fence)
{
   TU_FROM_HANDLE(tu_device, device, _device);
   TU_FROM_HANDLE(VkFence_T, fence, _fence);
   return (RPTR_FULL(device, fence->rptr[0] & 1) >= fence->rptr[0]) ? VK_SUCCESS : VK_NOT_READY;
}

/* TODO: timeline semaphores */

VkResult
tu_GetSemaphoreCounterValue(VkDevice _device, VkSemaphore _semaphore, uint64_t *value)
{
   TU_FROM_HANDLE(tu_device, device, _device);
   TU_FROM_HANDLE(VkSemaphore_T, semaphore, _semaphore);

   struct timeline *tl = semaphore->timeline;
   pthread_mutex_lock(&tl->lock);
   timeline_update(tl, RPTR_FULL(device, tl->points[0].rptr & 1));
   *value = tl->value;
   pthread_mutex_unlock(&tl->lock);
   return VK_SUCCESS;
}

VkResult
tu_WaitSemaphores(VkDevice _device, const VkSemaphoreWaitInfo* info, uint64_t timeout)
{
   TU_FROM_HANDLE(tu_device, device, _device);
   /* TODO: support wait any */
   assert(info->semaphoreCount == 1 || !(info->flags & VK_SEMAPHORE_WAIT_ANY_BIT));

   uint64_t rptr_max[2] = {};

   for (uint32_t i = 0; i < info->semaphoreCount; i++) {
      TU_FROM_HANDLE(VkSemaphore_T, semaphore, info->pSemaphores[i]);
      struct timeline *tl  = semaphore->timeline;
      uint64_t value = info->pValues[i];

      pthread_mutex_lock(&tl->lock);

      while (tl->max_value < value) {
         pthread_cond_wait(&tl->cond, &tl->lock); // TODO timedwait, update timeout
      }

      if (tl->value < value) {
         uint64_t rptr = timeline_rptr_for_value(tl, value);
         if (rptr_max[rptr & 1] < rptr)
            rptr_max[rptr & 1] = rptr;
      }

      pthread_mutex_unlock(&tl->lock);
   }

   int ret = rptr_wait(device, rptr_max, timeout);
   if (ret <= 0)
      printf("WaitSemaphores timed out (%d / %" PRIu64 ")\n", ret, timeout);

   return ret > 0 ? VK_SUCCESS : VK_TIMEOUT;
}

VkResult
tu_SignalSemaphore(VkDevice _device, const VkSemaphoreSignalInfo *info)
{
   TU_FROM_HANDLE(tu_device, device, _device);
   TU_FROM_HANDLE(VkSemaphore_T, semaphore, info->semaphore);
   struct timeline *tl = semaphore->timeline;

   pthread_mutex_lock(&tl->lock);
   assert(info->value >= tl->max_value);

#ifndef NDEBUG
   timeline_update(tl, RPTR_FULL(device, tl->points[0].rptr & 1));
   assert(tl->num_points == 0);
#endif
   tl->num_points = 0;
   tl->value = tl->max_value = info->value;
   pthread_mutex_unlock(&tl->lock);
   pthread_cond_broadcast(&tl->cond);
   return VK_SUCCESS;
}

#if 0
int
tu_signal_fences(struct tu_device *device, VkFence _fence, VkSemaphore _semaphore)
{
   TU_FROM_HANDLE(VkFence_T, fence, _fence);
   TU_FROM_HANDLE(VkSemaphore_T, semaphore, _semaphore);
   /* this is only so that WaitForFences will behavior correctly on this fence
    * (WaitForFences is the only function that cares about waiting for non-timeline semaphores)
    */
   if (fence) {
      assert(!fence->is_timeline);
      fence->rptr[0] = 0;
   }
   if (semaphore) {
      assert(!semaphore->is_timeline);
      semaphore->rptr[0] = 0;
   }
   return 0;
}
#endif

void tu_gpu_signal_fence(VkFence fence);

void tu_gpu_signal_fence(VkFence _fence)
{
   struct VkFence_T *fence = (void*) (uintptr_t) _fence;
   assert(!fence->is_timeline);
   fence->rptr[0] = 0;
}

int
tu_syncobj_to_fd(struct tu_device *device, struct vk_sync *sync);

int
tu_syncobj_to_fd(struct tu_device *device, struct vk_sync *sync)
{
   tu_finishme("tu_syncobj_to_fd");
   return -1;
}

VkResult
tu_device_wait_u_trace(struct tu_device *dev, struct tu_u_trace_syncobj *syncobj)
{
   tu_finishme("tu_device_wait_u_trace");
   return VK_SUCCESS;
}

VkResult
tu_device_check_status(struct vk_device *vk_device)
{
   return VK_SUCCESS;
}

#if 1
VKAPI_ATTR VkResult VKAPI_CALL
tu_QueueWaitIdle(VkQueue _queue)
{
   TU_FROM_HANDLE(tu_queue, queue, _queue);
   uint64_t rptr[2] = {};
   rptr[queue->rptr & 1] = queue->rptr;
   int ret = rptr_wait(queue->device, rptr, ~0ul);
   if (ret <= 0)
      printf("QueueWaitIdle timed out (%d)\n", ret);

   return VK_SUCCESS;
}
#endif
