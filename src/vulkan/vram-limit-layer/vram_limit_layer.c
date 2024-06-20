/*
 * Copyright © 2017 Google
 * Copyright © 2019 Red Hat
 * Copyright © 2024 Igalia S.L.
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vk_layer.h>
#include <vulkan/vulkan_core.h>

#include "util/bitscan.h"
#include "util/hash_table.h"
#include "util/macros.h"
#include "util/os_misc.h"
#include "util/ralloc.h"
#include "util/simple_mtx.h"
#include "util/strtod.h"
#include "vk_dispatch_table.h"
#include "vk_enum_to_str.h"
#include "vk_util.h"

#define DEBUG 1

#define KiB(v) (UINT64_C(1024) * v)
#define MiB(v) (UINT64_C(1024) * KiB(v))

#define VRAM_LIMIT_DEBUG_LOG_TAG "VRAM-LIMIT DEBUG: "
#define VRAM_LIMIT_WARN_LOG_TAG  "VRAM-LIMIT WARNING: "
#define VRAM_LIMIT_ERROR_LOG_TAG "VRAM-LIMIT ERROR: "

struct vram_limit_instance_data {
   struct vk_instance_dispatch_table vtable;
   struct vk_physical_device_dispatch_table pd_vtable;
   VkInstance instance;

   /* Used to indicate that the heap size is unaffected. I.e. the layer will use
    * the size reported by the underlying driver.
    */
#define VRAM_LIMIT_STATIC_HEAP_SIZE_DEFAULT (0)
   uint64_t static_heap_size;

   uint32_t active_pdevices_count;
   struct vram_limit_pdevice_data {
      /* 100% is 1.0 */
      double per_heap_budget_percentage[VK_MAX_MEMORY_HEAPS];
      VkPhysicalDevice pdevice;
   } active_pdevices_array[];
};

#define HKEY(obj)       ((uint64_t)(obj))
#define FIND(type, obj) ((type *)find_object_data(HKEY(obj)))

static struct hash_table_u64 *vk_object_to_data = NULL;
static simple_mtx_t vk_object_to_data_mutex = SIMPLE_MTX_INITIALIZER;

static inline void
ensure_vk_object_map(void)
{
   if (!vk_object_to_data)
      vk_object_to_data = _mesa_hash_table_u64_create(NULL);
}

static void *
find_object_data(uint64_t obj)
{
   simple_mtx_lock(&vk_object_to_data_mutex);
   ensure_vk_object_map();
   void *data = _mesa_hash_table_u64_search(vk_object_to_data, obj);
   simple_mtx_unlock(&vk_object_to_data_mutex);
   return data;
}

static void
map_object(uint64_t obj, void *data)
{
   simple_mtx_lock(&vk_object_to_data_mutex);
   ensure_vk_object_map();
   _mesa_hash_table_u64_insert(vk_object_to_data, obj, data);
   simple_mtx_unlock(&vk_object_to_data_mutex);
}

static void
unmap_object(uint64_t obj)
{
   simple_mtx_lock(&vk_object_to_data_mutex);
   _mesa_hash_table_u64_remove(vk_object_to_data, obj);
   simple_mtx_unlock(&vk_object_to_data_mutex);
}

static uint64_t
vram_limit_env_get_static_heap_size_or_default()
{
   const char *const env_var_value_str =
      os_get_option("VK_VRAM_LIMIT_HEAP_SIZE");
   if (!env_var_value_str)
      return VRAM_LIMIT_STATIC_HEAP_SIZE_DEFAULT;

   errno = 0;
   const long long env_var_value = strtoll(env_var_value_str, NULL, 0);
   if (env_var_value < 0 || errno == EINVAL || errno == ERANGE) {
      fprintf(
         stderr, VRAM_LIMIT_ERROR_LOG_TAG
         "VK_VRAM_LIMIT_HEAP_SIZE has an invalid value. It will not be used.\n");
      return VRAM_LIMIT_STATIC_HEAP_SIZE_DEFAULT;
   }

   const uint64_t size_in_bytes = MiB(env_var_value);

#if defined(DEBUG)
   printf(VRAM_LIMIT_DEBUG_LOG_TAG "Static heap size override value: %lu MiB\n",
          size_in_bytes / MiB(1));
#endif

   return size_in_bytes;
}

static bool
vram_limit_env_get_device_id(VkVendorId *vendor_id_out, uint32_t *device_id_out)
{
   const char *const env_var_value_str =
      os_get_option("VK_VRAM_LIMIT_DEVICE_ID");
   if (!env_var_value_str)
      return false;

   char *end_ptr;

   errno = 0;
   long long val_0 = strtoul(env_var_value_str, &end_ptr, 0);
   if (errno == EINVAL || errno == ERANGE || end_ptr == env_var_value_str)
      return false;

   char *start_ptr = end_ptr;

   if (*start_ptr != ':')
      return false;

   start_ptr++;

   errno = 0;
   long long val_1 = strtoul(start_ptr, &end_ptr, 0);
   if (errno == EINVAL || errno == ERANGE || end_ptr == start_ptr)
      return false;

   *vendor_id_out = val_0;
   *device_id_out = val_1;

   return true;
}

static void
vram_limit_get_memory_heaps_with_device_propertiy(
   VkPhysicalDeviceMemoryProperties *memory_properties,
   VkMemoryHeap *heaps_out[static const VK_MAX_MEMORY_HEAPS],
   uint32_t *const heaps_bitmask_out)
{
   uint32_t packed_heap_index = 0;
   uint32_t heaps_bitmask = 0;

   STATIC_ASSERT(sizeof(heaps_bitmask) * CHAR_BIT >= VK_MAX_MEMORY_HEAPS);

   for (uint32_t i = 0; i < VK_MAX_MEMORY_HEAPS; i++)
      heaps_out[i] = NULL;

   for (uint32_t i = 0; i < memory_properties->memoryTypeCount; i++) {
      const VkMemoryType *const memory_type =
         &memory_properties->memoryTypes[i];

#if !defined(NDEBUG)
      const VkMemoryPropertyFlags handled_mem_flags =
         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
         VK_MEMORY_PROPERTY_HOST_CACHED_BIT |
         VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT |
         VK_MEMORY_PROPERTY_PROTECTED_BIT;

      u_foreach_bit (mem_flag,
                     memory_type->propertyFlags & ~handled_mem_flags) {
         fprintf(stderr,
                 VRAM_LIMIT_WARN_LOG_TAG
                 "unhandled VkMemoryPropertyFlagBits: %s\n",
                 vk_MemoryPropertyFlagBits_to_str(mem_flag));
      }
#endif

      const VkMemoryPropertyFlags device_mem_flags =
         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
         VK_MEMORY_PROPERTY_PROTECTED_BIT |
         VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT;

      if (!(memory_type->propertyFlags & device_mem_flags))
         continue;

      const uint32_t heap_index = memory_type->heapIndex;

      /* From the Vulkan spec:
       *
       *   "More than one memory type may share each heap"
       *
       * So we don't accidentally want to get the same heap again.
       */
      if (heaps_bitmask & BITFIELD_BIT(heap_index))
         continue;

      heaps_bitmask |= BITFIELD_BIT(heap_index);

      heaps_out[packed_heap_index] =
         &memory_properties->memoryHeaps[heap_index];
   }

   *heaps_bitmask_out = heaps_bitmask;
}

static double
vram_limit_budget_percentages_calculate()
{
   double percentage = 100;

#if defined(DEBUG)
   printf(VRAM_LIMIT_DEBUG_LOG_TAG "budget percentage value: %0.2f %%\n",
          percentage);
#endif

   return percentage;
}

static void
destroy_instance_data(struct vram_limit_instance_data *data)
{
   unmap_object(HKEY(data->instance));
   ralloc_free(data);
}

static void
instance_data_unmap_physical_devices(
   struct vram_limit_instance_data *instance_data)
{
   uint32_t physicalDeviceCount = 0;

   instance_data->vtable.EnumeratePhysicalDevices(instance_data->instance,
                                                  &physicalDeviceCount, NULL);
   VkPhysicalDevice *physicalDevices = (VkPhysicalDevice *)malloc(
      sizeof(VkPhysicalDevice) * physicalDeviceCount);
   instance_data->vtable.EnumeratePhysicalDevices(
      instance_data->instance, &physicalDeviceCount, physicalDevices);

   for (uint32_t i = 0; i < physicalDeviceCount; i++)
      unmap_object(HKEY(physicalDevices[i]));

   free(physicalDevices);
}

static VkLayerInstanceCreateInfo *
get_instance_chain_info(const VkInstanceCreateInfo *pCreateInfo,
                        VkLayerFunction func)
{
   vk_foreach_struct_const (item, pCreateInfo->pNext) {
      if (item->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
          ((VkLayerInstanceCreateInfo *)item)->function == func)
         return (VkLayerInstanceCreateInfo *)item;
   }
   unreachable("instance chain info not found");
   return NULL;
}

static VkResult
vram_limit_CreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                          const VkAllocationCallbacks *pAllocator,
                          VkInstance *pInstance)
{
   VkResult result;

   VkLayerInstanceCreateInfo *chain_info =
      get_instance_chain_info(pCreateInfo, VK_LAYER_LINK_INFO);

   assert(chain_info->u.pLayerInfo);
   PFN_vkGetInstanceProcAddr fpGetInstanceProcAddr =
      chain_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;

#define DEFINE_VK_VOID_FUNC_PTR(proc_addr_func, instance, func_name)           \
   CONCAT2(PFN_vk, func_name)                                                  \
   CONCAT2(fp, func_name) =                                                    \
      (CONCAT2(PFN_vk, func_name))proc_addr_func(instance, "vk" #func_name)

   DEFINE_VK_VOID_FUNC_PTR(fpGetInstanceProcAddr, NULL, CreateInstance);
   if (fpCreateInstance == NULL) {
      result = VK_ERROR_INITIALIZATION_FAILED;
      goto err_return;
   }

   PFN_GetPhysicalDeviceProcAddr fpGetPhysicalDeviceProcAddr =
      chain_info->u.pLayerInfo->pfnNextGetPhysicalDeviceProcAddr;
   if (fpGetPhysicalDeviceProcAddr == NULL) {
      result = VK_ERROR_INITIALIZATION_FAILED;
      goto err_return;
   }

   /* Advance the link info for the next element on the chain */
   chain_info->u.pLayerInfo = chain_info->u.pLayerInfo->pNext;

   result = fpCreateInstance(pCreateInfo, pAllocator, pInstance);
   if (result != VK_SUCCESS) {
      goto err_return;
   }

   DEFINE_VK_VOID_FUNC_PTR(fpGetInstanceProcAddr, *pInstance, DestroyInstance);
   if (fpDestroyInstance == NULL) {
      result = VK_ERROR_INITIALIZATION_FAILED;
      goto err_return;
   }

   DEFINE_VK_VOID_FUNC_PTR(fpGetInstanceProcAddr, *pInstance,
                           EnumeratePhysicalDevices);
   if (fpEnumeratePhysicalDevices == NULL) {
      result = VK_ERROR_INITIALIZATION_FAILED;
      goto err_destroy_instance;
   }

   DEFINE_VK_VOID_FUNC_PTR(fpGetPhysicalDeviceProcAddr, *pInstance,
                           GetPhysicalDeviceProperties);
   if (fpGetPhysicalDeviceProperties == NULL) {
      result = VK_ERROR_INITIALIZATION_FAILED;
      goto err_destroy_instance;
   }

#undef DEFINE_VK_VOID_FUNC_PTR

   uint32_t pdevice_count = 0;
   fpEnumeratePhysicalDevices(*pInstance, &pdevice_count, NULL);

   VkPhysicalDevice *pdevices_array =
      (VkPhysicalDevice *)malloc(sizeof(*pdevices_array) * pdevice_count);
   if (pdevices_array == NULL) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto err_destroy_instance;
   }

   fpEnumeratePhysicalDevices(*pInstance, &pdevice_count, pdevices_array);

   bool *is_pdevice_active_array =
      (bool *)calloc(pdevice_count, sizeof(*is_pdevice_active_array));
   if (is_pdevice_active_array == NULL) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto err_free_pdevices_array;
   }

   const uint64_t static_heap_size =
      vram_limit_env_get_static_heap_size_or_default();

   VkVendorId vendor_id;
   uint32_t device_id;
   const bool device_id_is_valid =
      vram_limit_env_get_device_id(&vendor_id, &device_id);

   uint32_t active_pdevices_count = 0;
   if (device_id_is_valid &&
       static_heap_size != VRAM_LIMIT_STATIC_HEAP_SIZE_DEFAULT) {
      for (uint32_t i = 0; i < pdevice_count; i++) {
         VkPhysicalDevice pdevice = pdevices_array[i];
         VkPhysicalDeviceProperties properties;

         is_pdevice_active_array[i] = false;

         fpGetPhysicalDeviceProperties(pdevice, &properties);

         if (properties.vendorID != vendor_id)
            continue;

         if (properties.deviceID != device_id)
            continue;

#if defined(DEBUG)
         printf(VRAM_LIMIT_DEBUG_LOG_TAG
                "Active device: %s\nStatic Heap size: %lu MiB\n",
                properties.deviceName, static_heap_size / MiB(1));
#endif

         is_pdevice_active_array[i] = true;
         active_pdevices_count++;
      }
   }

   struct vram_limit_instance_data *instance_data =
      rzalloc_size(NULL, sizeof(*instance_data) +
                            sizeof(instance_data->active_pdevices_array[0]) *
                               active_pdevices_count);
   if (instance_data == NULL) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto err_free_is_pdevice_active_array;
   }

   vk_instance_dispatch_table_load(&instance_data->vtable,
                                   fpGetInstanceProcAddr, *pInstance);
   vk_physical_device_dispatch_table_load(&instance_data->pd_vtable,
                                          fpGetInstanceProcAddr, *pInstance);

   instance_data->instance = *pInstance;
   instance_data->static_heap_size = static_heap_size;

   instance_data->active_pdevices_count = 0;
   for (uint32_t i = 0; i < pdevice_count; i++) {
      VkPhysicalDevice pdevice = pdevices_array[i];
      struct vram_limit_pdevice_data *pdevice_data;

      /* Even though multiple physical devices have the same vendor id and
       * device id, they might not have the same heap arrangements due to
       * potentially differing drivers. So we have to maintain per pdevice
       * budged percentages and not just calculate it once to be used with
       * all.
       */
      if (!is_pdevice_active_array[i]) {
         continue;
      }

      /* No device should be active if the default size is set. */
      assert(static_heap_size != VRAM_LIMIT_STATIC_HEAP_SIZE_DEFAULT);

      pdevice_data =
         &instance_data
             ->active_pdevices_array[instance_data->active_pdevices_count];
      instance_data->active_pdevices_count++;

      pdevice_data->pdevice = pdevice;

      if (instance_data->pd_vtable.GetPhysicalDeviceMemoryProperties2 == NULL) {
#if defined(DEBUG)
         for (uint32_t i = 0; i < VK_MAX_MEMORY_HEAPS; i++)
            pdevice_data->per_heap_budget_percentage[i] = NAN;
#endif

         continue;
      }

      /* For each active device we need to setup a budget percentage to scale
       * down the reported budget to keep it under the new heap size.
       */
      VkPhysicalDeviceMemoryProperties2 memory_properties = {
         .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2,
      };

      instance_data->pd_vtable.GetPhysicalDeviceMemoryProperties2(
         pdevice, &memory_properties);

      VkMemoryHeap *heaps_array[VK_MAX_MEMORY_HEAPS];
      uint32_t heaps_array_bitmask;

      vram_limit_get_memory_heaps_with_device_propertiy(
         &memory_properties.memoryProperties, heaps_array,
         &heaps_array_bitmask);

      STATIC_ASSERT(ARRAY_SIZE(instance_data->active_pdevices_array[0]
                                  .per_heap_budget_percentage) ==
                    VK_MAX_MEMORY_HEAPS);
      for (uint32_t i = 0; i < VK_MAX_MEMORY_HEAPS; i++) {
         const VkMemoryHeap *const heap = heaps_array[i];

         if (!(BITFIELD_BIT(i) & heaps_array_bitmask)) {
            pdevice_data->per_heap_budget_percentage[i] = 1.0;
            continue;
         }

         assert(static_heap_size != VRAM_LIMIT_STATIC_HEAP_SIZE_DEFAULT);
         const double ratio =
            (double)instance_data->static_heap_size / heap->size;

         pdevice_data->per_heap_budget_percentage[i] = ratio;
      }
   }

   map_object(HKEY(instance_data->instance), instance_data);

   for (uint32_t i = 0; i < pdevice_count; i++) {
      map_object(HKEY(pdevices_array[i]), instance_data);
   }

#if defined(DEBUG)

#endif

   free(is_pdevice_active_array);
   free(pdevices_array);

   return VK_SUCCESS;

err_free_is_pdevice_active_array:
   free(is_pdevice_active_array);

err_free_pdevices_array:
   free(pdevices_array);

err_destroy_instance:
   // fpDestroyInstance(*pInstance, NULL);

err_return:
   return result;
}

static void
vram_limit_DestroyInstance(VkInstance instance,
                           const VkAllocationCallbacks *pAllocator)
{
   struct vram_limit_instance_data *instance_data =
      FIND(struct vram_limit_instance_data, instance);

   instance_data_unmap_physical_devices(instance_data);
   instance_data->vtable.DestroyInstance(instance, pAllocator);

   destroy_instance_data(instance_data);
}

static inline void
vram_limit_apply_budget_percentage(double percentage,
                                   VkDeviceSize *const size_in_out)
{
   const VkDeviceSize old_size = *size_in_out;
   const VkDeviceSize new_size = old_size * percentage;

#if defined(DEBUG)
   if (percentage != 1.0) {
      printf(VRAM_LIMIT_DEBUG_LOG_TAG
             "tweaking budget size to %0.2f %%, %lu MiB -> %lu MiB\n",
             percentage * 100, old_size / MiB(1), new_size / MiB(1));
   }
#endif

   *size_in_out = new_size;
}

static void
vram_limit_tweak_memory_properties(
   const struct vram_limit_instance_data *instance_data,
   VkPhysicalDevice pdevice,
   VkPhysicalDeviceMemoryProperties *memory_properties,
   VkPhysicalDeviceMemoryBudgetPropertiesEXT *memory_budget_optional)
{
   if (unlikely(instance_data->static_heap_size ==
                VRAM_LIMIT_STATIC_HEAP_SIZE_DEFAULT))
      return;

   const struct vram_limit_pdevice_data *pdevice_data = NULL;
   for (uint32_t i = 0; i < instance_data->active_pdevices_count; i++) {
      if (instance_data->active_pdevices_array[i].pdevice != pdevice)
         continue;

      pdevice_data = &instance_data->active_pdevices_array[i];
   }

   if (pdevice_data == NULL) {
      /* The device wasn't selected by the user so don't tweak any values. */
      return;
   }

   for (uint32_t i = 0; i < VK_MAX_MEMORY_HEAPS; i++) {
      if (i > memory_properties->memoryHeapCount)
         break;

      assert(instance_data->static_heap_size !=
             VRAM_LIMIT_STATIC_HEAP_SIZE_DEFAULT);
      memory_properties->memoryHeaps[i].size = instance_data->static_heap_size;

      if (memory_budget_optional) {
         const double percentage = pdevice_data->per_heap_budget_percentage[i];
         vram_limit_apply_budget_percentage(
            percentage, &memory_budget_optional->heapBudget[i]);

         assert(memory_budget_optional->heapBudget[i] <=
                memory_properties->memoryHeaps[i].size);
      }
   }
}

static VKAPI_ATTR void VKAPI_CALL
vram_limit_GetPhysicalDeviceMemoryProperties(
   VkPhysicalDevice physicalDevice,
   VkPhysicalDeviceMemoryProperties *pMemoryProperties)
{
   struct vram_limit_instance_data *instance_data =
      FIND(struct vram_limit_instance_data, physicalDevice);

   instance_data->pd_vtable.GetPhysicalDeviceMemoryProperties(
      physicalDevice, pMemoryProperties);

   vram_limit_tweak_memory_properties(instance_data, physicalDevice,
                                      pMemoryProperties, NULL);
}

static VKAPI_ATTR void VKAPI_CALL
vram_limit_GetPhysicalDeviceMemoryProperties2(
   VkPhysicalDevice physicalDevice,
   VkPhysicalDeviceMemoryProperties2 *pMemoryProperties)
{
   struct vram_limit_instance_data *instance_data =
      FIND(struct vram_limit_instance_data, physicalDevice);

   instance_data->pd_vtable.GetPhysicalDeviceMemoryProperties2(
      physicalDevice, pMemoryProperties);

   struct VkPhysicalDeviceMemoryBudgetPropertiesEXT *budget_properties =
      vk_find_struct(pMemoryProperties->pNext,
                     PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT);

   vram_limit_tweak_memory_properties(instance_data, physicalDevice,
                                      &pMemoryProperties->memoryProperties,
                                      budget_properties);
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vram_limit_GetInstanceProcAddr(VkInstance instance, const char *funcName);

static void *
find_ptr(const char *name)
{
   static const struct {
      const char *name;
      void *ptr;
   } name_to_funcptr_map[] = {
      {"vkGetInstanceProcAddr", (void *)vram_limit_GetInstanceProcAddr},
#define ADD_HOOK(fn) {"vk" #fn, (void *)vram_limit_##fn}
#define ADD_ALIAS_HOOK(alias, fn)                                              \
   {                                                                           \
      "vk" #alias, (void *)vram_limit_##fn                                     \
   }
      ADD_HOOK(GetPhysicalDeviceMemoryProperties),
      ADD_HOOK(GetPhysicalDeviceMemoryProperties2),
      ADD_ALIAS_HOOK(GetPhysicalDeviceMemoryProperties2KHR,
                     GetPhysicalDeviceMemoryProperties2),

      ADD_HOOK(CreateInstance),
      ADD_HOOK(DestroyInstance),
#undef ADD_HOOK
#undef ADD_ALIAS_HOOK
   };

   for (uint32_t i = 0; i < ARRAY_SIZE(name_to_funcptr_map); i++) {
      if (strcmp(name, name_to_funcptr_map[i].name) == 0)
         return name_to_funcptr_map[i].ptr;
   }

   return NULL;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vram_limit_GetInstanceProcAddr(VkInstance instance, const char *funcName)
{
   void *ptr = find_ptr(funcName);
   if (ptr)
      return (PFN_vkVoidFunction)(ptr);

   if (instance == NULL)
      return NULL;

   struct vram_limit_instance_data *instance_data =
      FIND(struct vram_limit_instance_data, instance);
   if (instance_data->vtable.GetInstanceProcAddr == NULL)
      return NULL;
   return instance_data->vtable.GetInstanceProcAddr(instance, funcName);
}

PUBLIC VkResult
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *pVersionStruct)
{
   if (pVersionStruct->loaderLayerInterfaceVersion < 2)
      return VK_ERROR_INITIALIZATION_FAILED;

   pVersionStruct->loaderLayerInterfaceVersion = 2;
   pVersionStruct->pfnGetInstanceProcAddr = vram_limit_GetInstanceProcAddr;

   return VK_SUCCESS;
}
