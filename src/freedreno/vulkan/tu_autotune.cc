/*
 * Copyright © 2021 Igalia S.L.
 * SPDX-License-Identifier: MIT
 */

#include "tu_autotune.h"

#include "tu_cmd_buffer.h"
#include "tu_cs.h"
#include "tu_device.h"
#include "tu_image.h"
#include "tu_pass.h"

#define XXH_INLINE_ALL
#include "util/xxhash.h"
#include "util/rand_xor.h"

/* How does it work?
 *
 * - For each renderpass we calculate the number of samples passed
 *   by storing the number before and after in GPU memory.
 * - To store the values each command buffer holds GPU memory which
 *   expands with more renderpasses being written.
 * - For each renderpass we create tu_renderpass_result entry which
 *   points to the results in GPU memory.
 *   - Later on tu_renderpass_result would be added to the
 *     tu_renderpass_history entry which aggregate results for a
 *     given renderpass.
 * - On submission:
 *   - Process results which fence was signalled.
 *   - Free per-submission data which we now don't need.
 *
 *   - Create a command stream to write a fence value. This way we would
 *     know when we could safely read the results.
 *   - We cannot rely on the command buffer's lifetime when referencing
 *     its resources since the buffer could be destroyed before we process
 *     the results.
 *   - For each command buffer:
 *     - Reference its GPU memory.
 *     - Move if ONE_TIME_SUBMIT or copy all tu_renderpass_result to the queue.
 *
 * Since the command buffers could be recorded on different threads
 * we have to maintaining some amount of locking history table,
 * however we change the table only in a single thread at the submission
 * time, so in most cases there will be no locking.
 */

void
tu_autotune_free_results_locked(struct tu_device *dev, struct list_head *results);

#define TU_AUTOTUNE_DEBUG_LOG 0
/* Dump history entries on autotuner finish,
 * could be used to gather data from traces.
 */
#define TU_AUTOTUNE_LOG_AT_FINISH 0

/* How many last renderpass stats are taken into account. */
constexpr uint32_t MAX_HISTORY_RESULTS = 5;
/* For how many submissions we store renderpass stats. */
constexpr uint32_t MAX_HISTORY_LIFETIME = 128;
/* How long we keep the duration entries. */
constexpr uint32_t MAX_DURATION_COUNT = 500;
constexpr uint64_t DEFAULT_SAMPLE_EXPIRY_TIME = 500'000'000; /* 500ms */

/* GPU always-on timer constants */
constexpr uint64_t ALWAYS_ON_FREQUENCY_HZ = 19'200'000;
constexpr uint64_t GPU_TICKS_PER_US = ALWAYS_ON_FREQUENCY_HZ / 1'000'000;

enum tu_at_flags {
   TU_AT_BGMEM = 1U << 0,
   TU_AT_PROF = 1U << 1,
   TU_AT_PROF_IMM = 1U << 2,
   TU_AT_LAT_SEN = 1U << 3,
};

static const struct debug_control tu_at_options[] = {
   { "bgmem", TU_AT_BGMEM },
   { "prof", TU_AT_PROF },
   { "prof_imm", TU_AT_PROF_IMM },
   { "latsen", TU_AT_LAT_SEN },
   { NULL, 0 }
};

static uint32_t tu_at_option;

static void
tu_at_options_init_once()
{
   tu_at_option =
      parse_debug_string(os_get_option("TU_AUTOTUNE"), tu_at_options);
   if (TU_DEBUG(STARTUP)) {
      mesa_logi("TU_AUTOTUNE=0x%x", tu_at_option);
   }
}

static inline void
tu_at_options_init()
{
   static once_flag once = ONCE_FLAG_INIT;
   call_once(&once, tu_at_options_init_once);
}

#define TU_AT_OPTION(x) (tu_at_option & (TU_AT_ ## x))

struct tu_duration_entry {
   struct list_head node;
   uint64_t duration;
   uint32_t draw_count;
   uint64_t expiry_timestamp; /* The timestamp when the sample will be deleted. */
};

struct tu_at_durations {
   struct list_head list;
   struct list_head *tail;
   uint32_t entry_count;
   uint64_t expiry_duration; /* The duration after which the entry is deleted. */
};

/**
 * Note: If expiry_duration is set to 0, the entry will never be deleted.
 */
static void
tu_at_durations_init(struct tu_at_durations *durations,
                     uint64_t expiry_duration = 0)
{
   list_inithead(&durations->list);
   durations->tail = &durations->list;
   durations->expiry_duration = expiry_duration;
   durations->entry_count = 0;
}

static void
tu_at_durations_del(struct tu_at_durations *durations,
                     struct tu_duration_entry *entry)
{
   if (durations->tail == &entry->node)
      durations->tail = entry->node.prev;

   list_del(&entry->node);
   free(entry);
   durations->entry_count--;
}

static void
tu_at_durations_clear(struct tu_at_durations *durations)
{
   list_for_each_entry_safe(struct tu_duration_entry, entry, &durations->list,
                            node)
   {
      tu_at_durations_del(durations, entry);
   }
   assert(durations->entry_count == 0);
}

static void
tu_at_durations_add(struct tu_at_durations *durations,
                      uint64_t duration,
                      uint32_t draw_count)
{
   struct tu_duration_entry *entry =
      (struct tu_duration_entry *) calloc(1, sizeof(*entry));
   entry->duration = duration;
   entry->draw_count = draw_count;
   if (durations->expiry_duration) {
      entry->expiry_timestamp =
         os_time_get_nano() + durations->expiry_duration;
   }

   if (++durations->entry_count >= MAX_DURATION_COUNT) {
      /* Once above the limit, start popping old results off the
       * tail of the list.
       */
      assert(durations->tail != &durations->list);
      struct tu_duration_entry *tail_entry =
         list_entry(durations->tail, struct tu_duration_entry, node);
      tu_at_durations_del(durations, tail_entry);
   }

   list_add(&entry->node, &durations->list);
   if (durations->tail == &durations->list)
      durations->tail = &entry->node;
}

/**
 * Calculates the average duration of the entries in the list.
 * It can optionally divide the duration by the draw count.
 *
 * It can also apply a recency bias to the average, where more recent
 * entries are given more weight in the average calculation. This is
 * based on the recency_bias_base parameter, which is the base of
 * the exponential function used to calculate the weight.
 *
 * A value of 1.0 means no recency bias, while any value < 1.0 will apply a
 * bias towards more recent entries, while any value > 1.0 will apply a bias
 * towards older entries. Any value <= 0.0 will lead to UB.
 *
 * Note: This function clears old entries while calculating the average, so
 *       the list will be modified after this function is called.
 */
static void
tu_at_durations_get_avg(struct tu_at_durations *durations,
                        uint64_t *avg_duration,
                        uint32_t *entry_count,
                        bool divide_by_draw_count = false,
                        double recency_bias_base = 1.0)
{
   uint64_t total_duration = 0;
   uint32_t count = 0;
   double total_weighted = 0.0;
   double total_weight = 0.0;
   double weight = 1.0;
   uint64_t now = os_time_get_nano();

   /* Clear old entries while calculating the average, since we don't depend
    * on stable indices for the entries, we can safely mutate the list while
    * iterating over it.
    */
   list_for_each_entry_safe(struct tu_duration_entry, entry, &durations->list,
                            node)
   {
      if (entry->expiry_timestamp && entry->expiry_timestamp < now) {
         if (TU_AUTOTUNE_DEBUG_LOG) {
            mesa_logi(
               "autotune: removing old draw duration entry %lu ticks - %u draws, %lu < %lu",
               entry->duration, entry->draw_count, entry->expiry_timestamp,
               now);
         }

         tu_at_durations_del(durations, entry);
         continue;
      }

      uint64_t effective_duration = entry->duration;
      if (divide_by_draw_count) {
         if (entry->draw_count > 0)
            effective_duration /= entry->draw_count;
         else
            effective_duration = 0;
      }

      if (recency_bias_base == 1.0) {
         /* Apply recency exponential bias with the supplied base to the
          * duration. We implicitly depend on the order of iteration going
          * from latest to oldest. */
         total_weighted += (double) effective_duration * weight;
         total_weight += weight;

         weight *= recency_bias_base;
         if (weight < 0.01) {
            /* Quit early when the weight becomes negligible. */
            break;
         }
      } else {
         /* No recency bias, just use the effective duration. */
         total_duration += effective_duration;
      }

      count++;
   }

   if (count > 0) {
      if (recency_bias_base == 1.0 && total_weight > 0.0) {
         /* Calculate the average using the weighted sum. */
         *avg_duration = total_weighted / total_weight;
      } else {
         /* Calculate the average using the total duration. */
         *avg_duration = total_duration / count;
      }
      *entry_count = count;
   } else {
      *avg_duration = 0;
      *entry_count = 0;
   }
}

/**
 * Tracks results for a given renderpass key
 */
struct tu_renderpass_history {
   uint64_t key;

   /* We would delete old history entries */
   uint32_t last_fence;

   /**
    * List of recent fd_renderpass_result's
    */
   struct list_head results;
   uint32_t num_results;

   uint32_t avg_samples;

   struct tu_at_durations gmem_tile_durations;
   struct tu_at_durations gmem_durations;
   struct tu_at_durations sysmem_durations;
   uint32_t sysmem_probability; /* Range [0 (GMEM), 100 (SYSMEM)], where 50
                                   means no preference. */
   bool latency_risk; /* If the renderpass has long draws which are at risk of
                         causing high preemptible latency. */
   uint32_t tile_size_divisor; /* The factor by which the tile size was
                                 divided to reduce preemption latency. */
   uint64_t latency_update_ts; /* The next timestamp to update the latency
                                  sensitivity parameters at. */
   uint64_t divisor_decrement_ts; /* The timestamp where it's allowed to next
                                     decrement the divisor. */
   uint64_t latency_switch_ts; /* The timestamp where it's allowed to mark
                                  the RP as no longer latency sensitive. */
   
#ifdef TU_AUTOTUNE_DEBUG_PERFCTR
   uint64_t preemption_latency; /* The highest preemption reaction delay
                                   recorded for the RP since the last update. */
#endif
};

/* Holds per-submission cs which writes the fence. */
struct tu_submission_data {
   struct list_head node;
   uint32_t fence;

   struct tu_cs fence_cs;
};

static bool
fence_before(uint32_t a, uint32_t b)
{
   /* essentially a < b, but handle wrapped values */
   return (int32_t)(a - b) < 0;
}

static uint32_t
get_autotune_fence(struct tu_autotune *at)
{
   return at->device->global_bo_map->autotune_fence;
}

template <chip CHIP>
static void
create_submission_fence(struct tu_device *dev,
                        struct tu_cs *cs,
                        uint32_t fence)
{
   uint64_t dst_iova = dev->global_bo->iova + gb_offset(autotune_fence);
   if (CHIP >= A7XX) {
      tu_cs_emit_pkt7(cs, CP_EVENT_WRITE7, 4);
      tu_cs_emit(cs,
         CP_EVENT_WRITE7_0(.event = CACHE_FLUSH_TS,
                           .write_src = EV_WRITE_USER_32B,
                           .write_dst = EV_DST_RAM,
                           .write_enabled = true).value);
   } else {
      tu_cs_emit_pkt7(cs, CP_EVENT_WRITE, 4);
      tu_cs_emit(cs, CP_EVENT_WRITE_0_EVENT(CACHE_FLUSH_TS));
   }

   tu_cs_emit_qw(cs, dst_iova);
   tu_cs_emit(cs, fence);
}

static struct tu_submission_data *
create_submission_data(struct tu_device *dev, struct tu_autotune *at,
                       uint32_t fence)
{
   struct tu_submission_data *submission_data = NULL;
   if (!list_is_empty(&at->submission_data_pool)) {
      submission_data = list_first_entry(&at->submission_data_pool,
                                         struct tu_submission_data, node);
      list_del(&submission_data->node);
   } else {
      submission_data = (struct tu_submission_data *) calloc(
         1, sizeof(struct tu_submission_data));
      tu_cs_init(&submission_data->fence_cs, dev, TU_CS_MODE_GROW, 5, "autotune fence cs");
   }
   submission_data->fence = fence;

   struct tu_cs* fence_cs = &submission_data->fence_cs;
   tu_cs_begin(fence_cs);
   TU_CALLX(dev, create_submission_fence)(dev, fence_cs, fence);
   tu_cs_end(fence_cs);

   list_addtail(&submission_data->node, &at->pending_submission_data);

   return submission_data;
}

static void
finish_submission_data(struct tu_autotune *at,
                       struct tu_submission_data *data)
{
   list_del(&data->node);
   list_addtail(&data->node, &at->submission_data_pool);
   tu_cs_reset(&data->fence_cs);
}

static void
free_submission_data(struct tu_submission_data *data)
{
   list_del(&data->node);
   tu_cs_finish(&data->fence_cs);

   free(data);
}

static uint64_t
hash_renderpass_instance(const struct tu_render_pass *pass,
                         const struct tu_framebuffer *framebuffer,
                         const struct tu_cmd_buffer *cmd) {
   uint32_t data[3 + pass->attachment_count * 5];
   uint32_t* ptr = data;

   *ptr++ = framebuffer->width;
   *ptr++ = framebuffer->height;
   *ptr++ = framebuffer->layers;

   for (unsigned i = 0; i < pass->attachment_count; i++) {
      *ptr++ = cmd->state.attachments[i]->view.width;
      *ptr++ = cmd->state.attachments[i]->view.height;
      *ptr++ = cmd->state.attachments[i]->image->vk.format;
      *ptr++ = cmd->state.attachments[i]->image->vk.array_layers;
      *ptr++ = cmd->state.attachments[i]->image->vk.mip_levels;
   }

   return XXH64(data, sizeof(data), pass->autotune_hash);
}

static void
free_result(struct tu_device *dev, struct tu_renderpass_result *result)
{
   tu_suballoc_bo_free(&dev->autotune_suballoc, &result->bo);
   list_del(&result->node);
   free(result);
}

static void
free_history(struct tu_device *dev, struct tu_renderpass_history *history)
{
   tu_at_durations_clear(&history->gmem_tile_durations);
   tu_at_durations_clear(&history->gmem_durations);
   tu_at_durations_clear(&history->sysmem_durations);
   tu_autotune_free_results_locked(dev, &history->results);
   free(history);
}

static bool
get_history(struct tu_autotune *at,
            uint64_t rp_key,
            uint32_t *avg_samples,
            uint32_t *sysmem_probability,
            bool *latency_risk)
{
   bool has_history = false;

   /* If the lock contantion would be found in the wild -
    * we could use try_lock here.
    */
   u_rwlock_rdlock(&at->ht_lock);
   struct hash_entry *entry =
      _mesa_hash_table_search(at->ht, &rp_key);
   if (entry) {
      struct tu_renderpass_history *history =
         (struct tu_renderpass_history *) entry->data;
      if (history->num_results > 0) {
         *avg_samples = p_atomic_read(&history->avg_samples);
         *sysmem_probability = p_atomic_read(&history->sysmem_probability);
         *latency_risk = p_atomic_read(&history->latency_risk);
         has_history = true;
      }
   }
   u_rwlock_rdunlock(&at->ht_lock);

   return has_history;
}

static struct tu_renderpass_result *
create_history_result(struct tu_autotune *at,
                      uint64_t rp_key,
                      uint32_t draw_count)
{
   struct tu_renderpass_result *result =
      (struct tu_renderpass_result *) calloc(1, sizeof(*result));
   result->rp_key = rp_key;
   result->draw_count = draw_count;
   result->tile_duration = 0;

   return result;
}

static uint64_t
ticks_to_us(uint64_t ticks) {
   return ticks / GPU_TICKS_PER_US;
}

static void
history_add_result(struct tu_device *dev, struct tu_renderpass_history *history,
                      struct tu_renderpass_result *result)
{
   list_delinit(&result->node);
   list_add(&result->node, &history->results);

   if (history->num_results < MAX_HISTORY_RESULTS) {
      history->num_results++;
   } else {
      /* Once above the limit, start popping old results off the
       * tail of the list:
       */
      struct tu_renderpass_result *old_result =
         list_last_entry(&history->results, struct tu_renderpass_result, node);
      mtx_lock(&dev->autotune_mutex);
      free_result(dev, old_result);
      mtx_unlock(&dev->autotune_mutex);
   }

   /* Do calculations here to avoid locking history in tu_autotune_use_bypass */
   uint32_t total_samples = 0;
   list_for_each_entry(struct tu_renderpass_result, result,
                       &history->results, node) {
      total_samples += result->samples_passed;
   }

   float avg_samples = (float)total_samples / (float)history->num_results;
   p_atomic_set(&history->avg_samples, (uint32_t)avg_samples);

   /* Profiled mode: Track separate GMEM/SYSMEM durations */
   if (result->sysmem) {
      /** These stores don't need to be atomic since they are only
       *  accessed from this function which should be single-threaded.
       */
      tu_at_durations_add(&history->sysmem_durations,
                               result->duration, result->draw_count);
   } else {
      if (result->tile_count > 0) {
         tu_at_durations_add(&history->gmem_tile_durations,
                               result->tile_duration, result->draw_count);
      } else {
         mesa_loge("autotune %016" PRIx64 ": zero tile duration", history->key);
      }
      tu_at_durations_add(&history->gmem_durations,
                               result->duration, result->draw_count);
   }

   uint32_t sysmem_probability = history->sysmem_probability;
   const uint32_t min_count = 5;
   const double recency_bias_default = 0.99;

   uint32_t gmem_count, sysmem_count;
   uint64_t avg_gmem, avg_sysmem;
   tu_at_durations_get_avg(&history->gmem_durations, &avg_gmem, &gmem_count,
                           false, recency_bias_default);
   tu_at_durations_get_avg(&history->sysmem_durations, &avg_sysmem,
                           &sysmem_count, false, recency_bias_default);

   if (TU_AT_OPTION(PROF_IMM)) {
      /* Try to immediately resolve the probability, this is useful for CI
       * running a single trace of frames where the probablities aren't
       * expected to change from run to run.
       */

      if (sysmem_count < 1) {
         sysmem_probability = 100;
      } else if (gmem_count < 1) {
         sysmem_probability = 0;
      } else {
         sysmem_probability = avg_gmem < avg_sysmem ? 0 : 100;

         if (TU_AUTOTUNE_DEBUG_LOG) {
            mesa_logi(
               "autotune imm: avg_gmem: %lu avg_sysmem: %lu = sysmem_probability: %u",
               avg_gmem, avg_sysmem, sysmem_probability);
         }
      }
   } else {
      if (sysmem_count < min_count || gmem_count < min_count) {
         /* Not enough data to make a decision, bias towards least used. */
         sysmem_probability = sysmem_count < gmem_count ? 75 : 25;
      } else {
         /* Adjust probability based on timing results */
         const uint32_t delta = 5;
         if (avg_gmem < avg_sysmem && history->sysmem_probability > 5) {
            sysmem_probability = MAX2(history->sysmem_probability - delta, 5);
         } else if (avg_sysmem < avg_gmem &&
                    history->sysmem_probability < 95) {
            sysmem_probability =
               MIN2(history->sysmem_probability + delta, 95);
         }

         if (TU_AUTOTUNE_DEBUG_LOG) {
            mesa_logi(
               "autotune update: avg_gmem: %lu avg_sysmem: %lu = sysmem_probability: %u",
               avg_gmem, avg_sysmem, sysmem_probability);
         }
      }
   }

   p_atomic_set(&history->sysmem_probability, sysmem_probability);

   if (TU_AT_OPTION(LAT_SEN)) {
      const uint64_t target_threshold = GPU_TICKS_PER_US * 500; /* 500us */
      
      const uint64_t default_update_duration_ns = 100'000'000; /* 100ms */
      const uint64_t change_update_duration_ns = 500'000'000; /* 500ms */
      const uint64_t downward_update_duration_ns = 10'000'000'000; /* 10s */
      const uint64_t latency_insensitive_duration_ns = 30'000'000'000; /* 30s */

#if TU_AUTOTUNE_DEBUG_PERFCTR
      history->preemption_latency =
         MAX2(history->preemption_latency, result->preemption_reaction_delay);
#endif

      if (TU_AUTOTUNE_DEBUG_LOG) {
         mesa_logi(
            "autotune %016" PRIx64
            ": latrisk: %u, sysmem: %u, sysmem_count: %u, gmem_count: %u"
#if TU_AUTOTUNE_DEBUG_PERFCTR
            ", preemption_latency: %lu us"
#endif
            ,
            history->key, history->latency_risk, result->sysmem, 
            sysmem_count, gmem_count
#if TU_AUTOTUNE_DEBUG_PERFCTR
            , ticks_to_us(history->preemption_latency)
#endif
         );
      }

      uint64_t now = os_time_get_nano();
      if (result->sysmem && !history->latency_risk) {
         /* Try to estimate the minimum non-preemptible duration for
          * draw-level preemption, by dividing the total time by the RP by the
          * amount of draws. This isn't very accurate as it's skewed by the
          * time taken by commands other than draws (e.g. clears or blits),
          * but it's a good enough estimate to catch the worst offenders.
          *
          * If the average draw duration is above a certain threshold, we mark
          * the RP as latency sensitive which should bias the decision towards
          * GMEM.
          */

         uint64_t avg_sysmem_draw;
         uint32_t sysmem_draw_count;
         tu_at_durations_get_avg(&history->sysmem_durations, &avg_sysmem_draw,
                                 &sysmem_draw_count, true,
                                 recency_bias_default);

         if (TU_AUTOTUNE_DEBUG_LOG) {
            mesa_logi(
               "autotune %016" PRIx64
               ": avg_sysmem_draw: %lu us (%u), latency_risk: %u"
#if TU_AUTOTUNE_DEBUG_PERFCTR
               ", preemption_latency: %lu us"
#endif
               ,
               history->key, ticks_to_us(avg_sysmem_draw),
               avg_sysmem_draw > target_threshold,
               p_atomic_read(&history->latency_risk),
#if TU_AUTOTUNE_DEBUG_PERFCTR
               ticks_to_us(history->preemption_latency)
#endif
            );

            history->preemption_latency = 0;
         }

         if (sysmem_draw_count >= min_count &&
             avg_sysmem_draw > target_threshold) {
            p_atomic_set(&history->latency_risk, true);
            if (TU_AUTOTUNE_DEBUG_LOG) {
               mesa_logi("autotune %016" PRIx64
                         ": high sysmem draw duration %lu us, marking as latency sensitive",
                         ticks_to_us(avg_sysmem_draw), history->key);
            }
         }
      } else if (!result->sysmem && history->latency_update_ts <= now) {
         /* If the RP is latency sensitive and we're using GMEM, we should
          * check if it's worth reducing the tile size to reduce the latency
          * risk further or if it's already low enough that it's not worth the
          * performance hit.
          */

         uint64_t avg_gmem_tile; /* Average duration of the longest tile in
                                    the RP. */
         uint32_t entry_count;
         tu_at_durations_get_avg(&history->gmem_tile_durations,
                                 &avg_gmem_tile, &entry_count, false);

         uint64_t update_duration_ns = default_update_duration_ns;
         if (entry_count > 1) {
            if (!p_atomic_read(&history->latency_risk)) {
               if (avg_gmem_tile > target_threshold) {
                  p_atomic_set(&history->latency_risk, true);
                  history->latency_switch_ts =
                     now + latency_insensitive_duration_ns;
                  if (TU_AUTOTUNE_DEBUG_LOG) {
                     mesa_logi(
                        "autotune %016" PRIx64
                        ": high gmem tile duration %lu, marking as latency sensitive",
                        avg_gmem_tile, history->key);
                  }
               }
            } else {
               uint32_t tile_size_divisor =
                  p_atomic_read(&history->tile_size_divisor);
               if (TU_AUTOTUNE_DEBUG_LOG) {
                  mesa_logi(
                     "autotune %016" PRIx64
                     ": avg_gmem_tile: %lu us (%u), latency_risk: %u, tile_size_divisor: %u"
#if TU_AUTOTUNE_DEBUG_PERFCTR
                     ", preemption_latency: %lu us"
#endif
                     ,
                     history->key, ticks_to_us(avg_gmem_tile),
                     avg_gmem_tile > target_threshold,
                     p_atomic_read(&history->latency_risk), tile_size_divisor,
#if TU_AUTOTUNE_DEBUG_PERFCTR
                     ticks_to_us(history->preemption_latency)
#endif
                  );

                  history->preemption_latency = 0;
               }

               int delta = 0;
               if (avg_gmem_tile > target_threshold &&
                   tile_size_divisor < TU_GMEM_LAYOUT_DIVISOR_MAX) {
                  /* If the average tile duration is high, we should reduce
                   * the tile size to reduce the latency risk.
                   */
                  delta = 1;

                  history->divisor_decrement_ts =
                     now + downward_update_duration_ns;
               } else if (avg_gmem_tile * 4 < target_threshold &&
                          tile_size_divisor > 1 &&
                          history->divisor_decrement_ts <= now) {
                  /* If the average tile duration is low enough that we can
                   * get away with a larger tile size, we should increase the
                   * tile size to reduce the performance hit of the smaller
                   * tiles.
                   *
                   * Note: The 4x factor is to account for the tile duration
                   * being halved when we increase the tile size divisor by 1,
                   * with an additional 2x factor to generally be conservative
                   * about reducing the divisor since it can lead to
                   * oscillation between tile sizes.
                   *
                   * Similarly, the divisor_decrement_ts is used to limit how
                   * often we can reduce the divisor to avoid oscillation.
                   */
                  delta = -1;
                  history->latency_switch_ts =
                     now + latency_insensitive_duration_ns;
               } else if (avg_gmem_tile * 10 < target_threshold &&
                          tile_size_divisor == 1 &&
                          history->latency_switch_ts <= now) {
                  /* If the average tile duration is low enough that we no
                   * longer consider the RP latency sensitive, we can switch
                   * it back to non-latency sensitive.
                   */
                  p_atomic_set(&history->latency_risk, false);
               }

               if (delta != 0) {
                  /* Clear all the results to avoid biasing the decision based
                   * on the old tile size.
                   */
                  tu_at_durations_clear(&history->gmem_tile_durations);

                  if (TU_AUTOTUNE_DEBUG_LOG) {
                     mesa_logi("autotune %016" PRIx64
                               ": updating tile size divisor: %u -> %u",
                               history->key, tile_size_divisor,
                               tile_size_divisor + delta);
                  }

                  p_atomic_set(&history->tile_size_divisor,
                               tile_size_divisor + delta);

                  update_duration_ns = change_update_duration_ns;
               }
            }

            history->latency_update_ts = now + update_duration_ns;
         }
      }
   }
}

static void
process_results(struct tu_autotune *at, uint32_t current_fence)
{
   struct tu_device *dev = at->device;

   list_for_each_entry_safe(struct tu_renderpass_result, result,
                            &at->pending_results, node) {
      if (fence_before(current_fence, result->fence))
         break;

      struct tu_renderpass_history *history = result->history;
      result->samples_passed =
         result->samples->samples_end - result->samples->samples_start;
      result->duration = result->samples->ts_end - result->samples->ts_start;
      struct tu_tile_samples *tiles =
         (struct tu_tile_samples *) (result->samples + 1);
      for (uint32_t i = 0; i < result->tile_count; i++) {
         struct tu_tile_samples *tile = &tiles[i];
         result->tile_duration =
            MAX2(result->tile_duration, tile->ts_end - tile->ts_start);
      }

#if TU_AUTOTUNE_DEBUG_PERFCTR
      /* Just spin until the counter values are written out. */
      while (p_atomic_read(&result->samples->cntrs_ready) !=
             tu_renderpass_samples::CNTRS_READY_MAGIC) {
      }

      auto read_counter = [&](const struct tu_perf_ctr_sample &sample,
                              const struct fd_perfcntr_countable *ctbl,
                              uint64_t &outValue, const char *name) {
         if (sample.selector_begin != sample.selector_end ||
             sample.selector_begin != ctbl->selector) {
            mesa_loge("autotune %016" PRIx64
                      ": %s: selector mismatch %u != %u (%u - %u)",
                      result->rp_key, ctbl->name, sample.selector_begin,
                      sample.selector_end, sample.selector_begin,
                      ctbl->selector);
         }

         outValue = sample.end - sample.begin;
         if (sample.end < sample.begin) {
            mesa_loge("autotune %016" PRIx64 ": %s: end < begin %lu < %lu",
                      result->rp_key, name, sample.end, sample.begin);
            outValue = 0;
         }
      };

      uint64_t num_preemptions, always_count;
      read_counter(result->samples->preemption_reaction_delay,
                   at->preemption_reaction_delay,
                   result->preemption_reaction_delay,
                   "preemption_reaction_delay");
      read_counter(result->samples->num_preemptions, at->num_preemptions,
                   num_preemptions, "num_preemptions");
      read_counter(result->samples->always_count, at->always_count,
                   always_count, "always_count");

      if (result->preemption_reaction_delay || num_preemptions) {
         mesa_logi(
            "autotune %016" PRIx64
            ": preemption_reaction_delay: %lu us, always_count: %lu us, num_preemptions: %lu",
            result->rp_key, ticks_to_us(result->preemption_reaction_delay),
            ticks_to_us(always_count), num_preemptions);
      }
#endif

      history_add_result(dev, history, result);
   }

   list_for_each_entry_safe(struct tu_submission_data, submission_data,
                            &at->pending_submission_data, node) {
      if (fence_before(current_fence, submission_data->fence))
         break;

      finish_submission_data(at, submission_data);
   }
}

static void
queue_pending_results(struct tu_autotune *at, struct tu_cmd_buffer *cmdbuf)
{
   bool one_time_submit = cmdbuf->usage_flags &
         VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

   if (one_time_submit) {
      /* We can just steal the list since it won't be resubmitted again */
      list_splicetail(&cmdbuf->renderpass_autotune_results,
                        &at->pending_results);
      list_inithead(&cmdbuf->renderpass_autotune_results);
   } else {
      list_for_each_entry_safe(struct tu_renderpass_result, result,
                              &cmdbuf->renderpass_autotune_results, node) {
         /* TODO: copying each result isn't nice */
         struct tu_renderpass_result *copy =
            (struct tu_renderpass_result *) malloc(sizeof(*result));
         *copy = *result;
         tu_bo_get_ref(copy->bo.bo);
         list_addtail(&copy->node, &at->pending_results);
      }
   }
}

struct tu_cs *
tu_autotune_on_submit(struct tu_device *dev,
                      struct tu_autotune *at,
                      struct tu_cmd_buffer **cmd_buffers,
                      uint32_t cmd_buffer_count)
{
   /* We are single-threaded here */

   const uint32_t gpu_fence = get_autotune_fence(at);
   const uint32_t new_fence = at->fence_counter++;

   process_results(at, gpu_fence);

   /* Create history entries here to minimize work and locking being
    * done on renderpass end.
    */
   for (uint32_t i = 0; i < cmd_buffer_count; i++) {
      struct tu_cmd_buffer *cmdbuf = cmd_buffers[i];
      list_for_each_entry_safe(struct tu_renderpass_result, result,
                          &cmdbuf->renderpass_autotune_results, node) {
         struct tu_renderpass_history *history;
         struct hash_entry *entry =
            _mesa_hash_table_search(at->ht, &result->rp_key);
         if (!entry) {
            history =
               (struct tu_renderpass_history *) calloc(1, sizeof(*history));
            history->key = result->rp_key;
            list_inithead(&history->results);
            tu_at_durations_init(&history->gmem_tile_durations, DEFAULT_SAMPLE_EXPIRY_TIME);
            tu_at_durations_init(&history->gmem_durations);
            tu_at_durations_init(&history->sysmem_durations);
            history->tile_size_divisor = 1;

            u_rwlock_wrlock(&at->ht_lock);
            _mesa_hash_table_insert(at->ht, &history->key, history);
            u_rwlock_wrunlock(&at->ht_lock);
         } else {
            history = (struct tu_renderpass_history *) entry->data;
         }

         history->last_fence = new_fence;

         result->fence = new_fence;
         result->history = history;
      }
   }

   struct tu_submission_data *submission_data =
      create_submission_data(dev, at, new_fence);

   for (uint32_t i = 0; i < cmd_buffer_count; i++) {
      struct tu_cmd_buffer *cmdbuf = cmd_buffers[i];
      if (list_is_empty(&cmdbuf->renderpass_autotune_results))
         continue;

      queue_pending_results(at, cmdbuf);
   }

   if (TU_AUTOTUNE_DEBUG_LOG)
      mesa_logi("Total history entries: %u", at->ht->entries);

   /* Cleanup old entries from history table. The assumption
    * here is that application doesn't hold many old unsubmitted
    * command buffers, otherwise this table may grow big.
    */
   hash_table_foreach(at->ht, entry) {
      struct tu_renderpass_history *history =
         (struct tu_renderpass_history *) entry->data;
      if (fence_before(gpu_fence, history->last_fence + MAX_HISTORY_LIFETIME))
         continue;

      if (TU_AUTOTUNE_DEBUG_LOG)
         mesa_logi("Removed old history entry %016" PRIx64 "", history->key);

      u_rwlock_wrlock(&at->ht_lock);
      _mesa_hash_table_remove_key(at->ht, &history->key);
      u_rwlock_wrunlock(&at->ht_lock);

      mtx_lock(&dev->autotune_mutex);
      free_history(dev, history);
      mtx_unlock(&dev->autotune_mutex);
   }

   return &submission_data->fence_cs;
}

static bool
renderpass_key_equals(const void *_a, const void *_b)
{
   return *(uint64_t *)_a == *(uint64_t *)_b;
}

static uint32_t
renderpass_key_hash(const void *_a)
{
   return *((uint64_t *) _a) & 0xffffffff;
}

VkResult
tu_autotune_init(struct tu_autotune *at, struct tu_device *dev)
{
   tu_at_options_init();

   at->enabled = true;
   if (TU_AT_OPTION(BGMEM))
      at->mode = TU_AUTOTUNE_MODE_GMEM_BIG;
   else if (TU_AT_OPTION(PROF) || TU_AT_OPTION(PROF_IMM))
      at->mode = TU_AUTOTUNE_MODE_PROFILED;
   else
      at->mode = TU_AUTOTUNE_MODE_NORMAL;
   at->device = dev;
   at->ht = _mesa_hash_table_create(NULL,
                                    renderpass_key_hash,
                                    renderpass_key_equals);
   u_rwlock_init(&at->ht_lock);

   list_inithead(&at->pending_results);
   list_inithead(&at->pending_submission_data);
   list_inithead(&at->submission_data_pool);

   /* start from 1 because tu6_global::autotune_fence is initialized to 0 */
   at->fence_counter = 1;

#if TU_AUTOTUNE_DEBUG_PERFCTR
   uint32_t group_count;
   const struct fd_perfcntr_group *groups =
      fd_perfcntrs(&dev->physical_device->dev_id, &group_count);

   const struct fd_perfcntr_group *cp_group = NULL;
   for (uint32_t i = 0; i < group_count; i++) {
      if (strcmp(groups[i].name, "CP") == 0) {
         cp_group = &groups[i];
         break;
      }
   }

   at->cp_group = cp_group;

   if (!cp_group) {
      mesa_loge("autotune: CP group not found");
      return VK_ERROR_INITIALIZATION_FAILED;
   } else if (cp_group->num_countables < 5) {
      mesa_loge("autotune: CP group has too few countables");
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   auto get_perfcntr_countable = [](const struct fd_perfcntr_group *group,
                                    const char *name) {
      for (uint32_t i = 0; i < group->num_countables; i++) {
         if (strcmp(group->countables[i].name, name) == 0)
            return &group->countables[i];
      }

      mesa_loge("autotune: %s not found in group %s", name, group->name);
      return (const struct fd_perfcntr_countable *) NULL;
   };

   at->preemption_reaction_delay =
      get_perfcntr_countable(cp_group, "PERF_CP_PREEMPTION_REACTION_DELAY");
   at->num_preemptions =
      get_perfcntr_countable(cp_group, "PERF_CP_NUM_PREEMPTIONS");
   at->always_count =
      get_perfcntr_countable(cp_group, "PERF_CP_ALWAYS_COUNT");

   if (!at->preemption_reaction_delay || !at->num_preemptions ||
       !at->always_count) {
      mesa_loge("autotune: preemption countables not found");
      return VK_ERROR_INITIALIZATION_FAILED;
   }
#endif

   return VK_SUCCESS;
}

void
tu_autotune_fini(struct tu_autotune *at, struct tu_device *dev)
{
   if (TU_AUTOTUNE_LOG_AT_FINISH) {
      while (!list_is_empty(&at->pending_results)) {
         const uint32_t gpu_fence = get_autotune_fence(at);
         process_results(at, gpu_fence);
      }

      hash_table_foreach(at->ht, entry) {
         struct tu_renderpass_history *history =
            (struct tu_renderpass_history *) entry->data;

         mesa_logi("%016" PRIx64 " \tavg_passed=%u results=%u",
                   history->key, history->avg_samples, history->num_results);
      }
   }

   tu_autotune_free_results(dev, &at->pending_results);

   mtx_lock(&dev->autotune_mutex);
   hash_table_foreach(at->ht, entry) {
      struct tu_renderpass_history *history =
         (struct tu_renderpass_history *) entry->data;
      free_history(dev, history);
   }
   mtx_unlock(&dev->autotune_mutex);

   list_for_each_entry_safe(struct tu_submission_data, submission_data,
                            &at->pending_submission_data, node) {
      free_submission_data(submission_data);
   }

   list_for_each_entry_safe(struct tu_submission_data, submission_data,
                            &at->submission_data_pool, node) {
      free_submission_data(submission_data);
   }

   _mesa_hash_table_destroy(at->ht, NULL);
   u_rwlock_destroy(&at->ht_lock);
}

bool
tu_autotune_submit_requires_fence(struct tu_cmd_buffer **cmd_buffers,
                                  uint32_t cmd_buffer_count)
{
   for (uint32_t i = 0; i < cmd_buffer_count; i++) {
      struct tu_cmd_buffer *cmdbuf = cmd_buffers[i];
      if (!list_is_empty(&cmdbuf->renderpass_autotune_results))
         return true;
   }

   return false;
}

void
tu_autotune_free_results_locked(struct tu_device *dev, struct list_head *results)
{
   list_for_each_entry_safe(struct tu_renderpass_result, result,
                            results, node) {
      free_result(dev, result);
   }
}

void
tu_autotune_free_results(struct tu_device *dev, struct list_head *results)
{
   mtx_lock(&dev->autotune_mutex);
   tu_autotune_free_results_locked(dev, results);
   mtx_unlock(&dev->autotune_mutex);
}

static bool
fallback_use_bypass(const struct tu_render_pass *pass,
                    const struct tu_framebuffer *framebuffer,
                    const struct tu_cmd_buffer *cmd_buffer)
{
   if (cmd_buffer->state.rp.drawcall_count > 5)
      return false;

   for (unsigned i = 0; i < pass->subpass_count; i++) {
      if (pass->subpasses[i].samples != VK_SAMPLE_COUNT_1_BIT)
         return false;
   }

   return true;
}

static uint32_t
get_render_pass_pixel_count(const struct tu_cmd_buffer *cmd)
{
   if (cmd->state.per_layer_render_area) {
      uint32_t pixels = 0;
      for (unsigned i = 0; i < cmd->state.pass->num_views; i++) {
         const VkExtent2D *extent = &cmd->state.render_areas[i].extent;
         pixels += extent->width * extent->height;
      }
      return pixels;
   } else {
      const VkExtent2D *extent = &cmd->state.render_areas[0].extent;
      return extent->width * extent->height *
         MAX2(cmd->state.pass->num_views, cmd->state.framebuffer->layers);
   }
}

static uint64_t
estimate_drawcall_bandwidth(const struct tu_cmd_buffer *cmd,
                            uint32_t avg_renderpass_sample_count)
{
   const struct tu_cmd_state *state = &cmd->state;

   if (!state->rp.drawcall_count)
      return 0;

   /* sample count times drawcall_bandwidth_per_sample */
   return (uint64_t)avg_renderpass_sample_count *
      state->rp.drawcall_bandwidth_per_sample_sum / state->rp.drawcall_count;
}

bool
tu_autotune_use_bypass(struct tu_autotune *at,
                       struct tu_cmd_buffer *cmd_buffer,
                       struct tu_renderpass_result **autotune_result)
{
   const struct tu_render_pass *pass = cmd_buffer->state.pass;
   const struct tu_framebuffer *framebuffer = cmd_buffer->state.framebuffer;

   /* If a feedback loop in the subpass caused one of the pipelines used to set
    * SINGLE_PRIM_MODE(FLUSH_PER_OVERLAP_AND_OVERWRITE) or even
    * SINGLE_PRIM_MODE(FLUSH), then that should cause significantly increased
    * sysmem bandwidth (though we haven't quantified it).
    */
   if (cmd_buffer->state.rp.sysmem_single_prim_mode)
      return false;

   /* If the user is using a fragment density map, then this will cause less
    * FS invocations with GMEM, which has a hard-to-measure impact on
    * performance because it depends on how heavy the FS is in addition to how
    * many invocations there were and the density. Let's assume the user knows
    * what they're doing when they added the map, because if sysmem is
    * actually faster then they could've just not used the fragment density
    * map.
    */
   if (pass->has_fdm)
      return false;

   /* For VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT buffers
    * we would have to allocate GPU memory at the submit time and copy
    * results into it.
    * Native games ususally don't use it, Zink and DXVK don't use it,
    * D3D12 doesn't have such concept.
    */
   bool simultaneous_use =
      cmd_buffer->usage_flags & VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;

   if (!at->enabled || simultaneous_use)
      return fallback_use_bypass(pass, framebuffer, cmd_buffer);

   if (at->mode == TU_AUTOTUNE_MODE_GMEM_BIG &&
       cmd_buffer->state.rp.drawcall_count >= 10) {
      if (TU_AUTOTUNE_DEBUG_LOG) {
         mesa_logi("autotune %016" PRIx64 ": %u draw calls >= 10, using gmem",
               hash_renderpass_instance(pass, framebuffer, cmd_buffer),
               cmd_buffer->state.rp.drawcall_count);
      }
      return false;
   }

   /* We use 64bit hash as a key since we don't fear rare hash collision,
    * the worst that would happen is sysmem being selected when it should
    * have not, and with 64bit it would be extremely rare.
    *
    * Q: Why not make the key from framebuffer + renderpass pointers?
    * A: At least DXVK creates new framebuffers each frame while keeping
    *    renderpasses the same. Also we want to support replaying a single
    *    frame in a loop for testing.
    */
   uint64_t renderpass_key = hash_renderpass_instance(pass, framebuffer, cmd_buffer);

   *autotune_result = create_history_result(
      at, renderpass_key, cmd_buffer->state.rp.drawcall_count);

   uint32_t avg_samples = 0, sysmem_probability = 50;
   bool latency_risk = false;
   if (get_history(at, renderpass_key, &avg_samples, &sysmem_probability,
                   &latency_risk)) {
      if (TU_AT_OPTION(LAT_SEN) && latency_risk) {
         /* Try to mitigate the risk of high preemption latency by always
          * using GMEM, which should break up any larger draws into smaller
          * ones with tiling.
          */
         if (TU_AUTOTUNE_DEBUG_LOG) {
            mesa_logi("autotune %016" PRIx64
                      ": high preemption latency risk, using GMEM",
                      renderpass_key);
         }
         return false;
      }

      if (at->mode == TU_AUTOTUNE_MODE_PROFILED) {
         uint64_t seed[2] { 0x3bffb83978e24f88, 0x9238d5d56c71cd35 };
         bool select_sysmem =
            (rand_xorshift128plus(seed) % 100) < sysmem_probability;

         if (TU_AUTOTUNE_DEBUG_LOG) {
            mesa_logi("autotune %016" PRIx64 ": %u%% sysmem chance, using %s",
                      renderpass_key, sysmem_probability,
                      select_sysmem ? "sysmem" : "gmem");
         }

         return select_sysmem;
      }

      const uint32_t pass_pixel_count =
         get_render_pass_pixel_count(cmd_buffer);
      uint64_t sysmem_bandwidth =
         (uint64_t)pass->sysmem_bandwidth_per_pixel * pass_pixel_count;
      uint64_t gmem_bandwidth =
         (uint64_t)pass->gmem_bandwidth_per_pixel * pass_pixel_count;

      const uint64_t total_draw_call_bandwidth =
         estimate_drawcall_bandwidth(cmd_buffer, avg_samples);

      /* drawcalls access the memory in sysmem rendering (ignoring CCU) */
      sysmem_bandwidth += total_draw_call_bandwidth;

      /* drawcalls access gmem in gmem rendering, but we do not want to ignore
       * them completely.  The state changes between tiles also have an
       * overhead.  The magic numbers of 11 and 10 are randomly chosen.
       */
      gmem_bandwidth = (gmem_bandwidth * 11 + total_draw_call_bandwidth) / 10;

      const bool select_sysmem = sysmem_bandwidth <= gmem_bandwidth;
      if (TU_AUTOTUNE_DEBUG_LOG) {
         const VkExtent2D *extent = &cmd_buffer->state.render_areas[0].extent;
         const float drawcall_bandwidth_per_sample =
            (float)cmd_buffer->state.rp.drawcall_bandwidth_per_sample_sum /
            cmd_buffer->state.rp.drawcall_count;

         mesa_logi("autotune %016" PRIx64 ":%u selecting %s",
               renderpass_key,
               cmd_buffer->state.rp.drawcall_count,
               select_sysmem ? "sysmem" : "gmem");
         mesa_logi("   avg_samples=%u, draw_bandwidth_per_sample=%.2f, total_draw_call_bandwidth=%" PRIu64,
               avg_samples,
               drawcall_bandwidth_per_sample,
               total_draw_call_bandwidth);
         mesa_logi("   render_area=%ux%u, sysmem_bandwidth_per_pixel=%u, gmem_bandwidth_per_pixel=%u",
               extent->width, extent->height,
               pass->sysmem_bandwidth_per_pixel,
               pass->gmem_bandwidth_per_pixel);
         mesa_logi("   sysmem_bandwidth=%" PRIu64 ", gmem_bandwidth=%" PRIu64,
               sysmem_bandwidth, gmem_bandwidth);
      }

      return select_sysmem;
   }

   return fallback_use_bypass(pass, framebuffer, cmd_buffer);
}

uint32_t
tu_autotune_get_tile_size_divisor(struct tu_autotune *at,
                                 struct tu_cmd_buffer *cmd_buffer)
{
   const struct tu_render_pass *pass = cmd_buffer->state.pass;
   const struct tu_framebuffer *framebuffer = cmd_buffer->state.framebuffer;

   if (!at->enabled || !TU_AT_OPTION(LAT_SEN) || pass->has_fdm)
      return 1;

   uint64_t renderpass_key = hash_renderpass_instance(pass, framebuffer, cmd_buffer);

   uint32_t tile_size_divisor = 1;
   u_rwlock_rdlock(&at->ht_lock);
   struct hash_entry *entry = _mesa_hash_table_search(at->ht, &renderpass_key);
   if (entry) {
      struct tu_renderpass_history *history =
         (struct tu_renderpass_history *) entry->data;
      tile_size_divisor = p_atomic_read(&history->tile_size_divisor);
   }
   u_rwlock_rdunlock(&at->ht_lock);

   if (TU_AUTOTUNE_DEBUG_LOG) {
      mesa_logi("autotune %016" PRIx64 ": tile_size_divisor=%u",
                renderpass_key, tile_size_divisor);
   }

   return tile_size_divisor;
}

template <chip CHIP>
void
tu_autotune_begin_renderpass(struct tu_cmd_buffer *cmd,
                             struct tu_cs *cs,
                             struct tu_renderpass_result *autotune_result,
                             bool sysmem, uint32_t tile_count)
{
   if (!autotune_result)
      return;

   autotune_result->sysmem = sysmem;
   autotune_result->tile_count = tile_count;

   struct tu_device *dev = cmd->device;

   uint32_t size = sizeof(struct tu_renderpass_samples);
   if (!sysmem)
      size += sizeof(struct tu_tile_samples) * tile_count;

   mtx_lock(&dev->autotune_mutex);
   VkResult ret = tu_suballoc_bo_alloc(&autotune_result->bo, &dev->autotune_suballoc, size, 32);
   mtx_unlock(&dev->autotune_mutex);
   if (ret != VK_SUCCESS) {
      autotune_result->bo.iova = 0;
      return;
   }

   uint64_t result_iova = autotune_result->bo.iova;

   autotune_result->samples =
      (struct tu_renderpass_samples *) tu_suballoc_bo_map(
         &autotune_result->bo);
   memset(autotune_result->samples, 0, size);

   tu_cs_emit_regs(cs, A6XX_RB_SAMPLE_COUNTER_CNTL(.copy = true));
   if (cmd->device->physical_device->info->a7xx.has_event_write_sample_count) {
      tu_cs_emit_pkt7(cs, CP_EVENT_WRITE7, 3);
      tu_cs_emit(cs, CP_EVENT_WRITE7_0(.event = ZPASS_DONE,
                                       .write_sample_count = true).value);
      tu_cs_emit_qw(cs, result_iova);

      /* If the renderpass contains an occlusion query with its own ZPASS_DONE,
       * we have to provide a fake ZPASS_DONE event here to logically close the
       * previous one, preventing firmware from misbehaving due to nested events.
       * This writes into the samples_end field, which will be overwritten in
       * tu_autotune_end_renderpass.
       */
      if (cmd->state.rp.has_zpass_done_sample_count_write_in_rp) {
         tu_cs_emit_pkt7(cs, CP_EVENT_WRITE7, 3);
         tu_cs_emit(cs, CP_EVENT_WRITE7_0(.event = ZPASS_DONE,
                                          .write_sample_count = true,
                                          .sample_count_end_offset = true,
                                          .write_accum_sample_count_diff = true).value);
         tu_cs_emit_qw(cs, result_iova);
      }
   } else {
      tu_cs_emit_regs(cs,
                        A6XX_RB_SAMPLE_COUNTER_BASE(.qword = result_iova));
      tu_cs_emit_pkt7(cs, CP_EVENT_WRITE, 1);
      tu_cs_emit(cs, ZPASS_DONE);
   }

   tu_cs_emit_pkt7(cs, CP_REG_TO_MEM, 3);
   tu_cs_emit(cs, CP_REG_TO_MEM_0_REG(REG_A6XX_CP_ALWAYS_ON_COUNTER) |
                  CP_REG_TO_MEM_0_CNT(2) |
                  CP_REG_TO_MEM_0_64B);
   tu_cs_emit_qw(cs, result_iova + offsetof(struct tu_renderpass_samples, ts_start));
   
#if TU_AUTOTUNE_DEBUG_PERFCTR
   struct tu_autotune *at = &dev->autotune;

   auto countable_begin = [&](const struct fd_perfcntr_countable *ctbl,
                              uint32_t cntr_idx, uint32_t offset) {
      const struct fd_perfcntr_counter *ctr =
         &at->cp_group->counters[cntr_idx];
      uint64_t offset_iova = result_iova + offset;
      assert(!ctr->enable); /* CP counters shouldn't use it. */

      tu_cs_emit_pkt4(cs, ctr->select_reg, 1);
      tu_cs_emit(cs, ctbl->selector);

      tu_cs_emit_wfi(cs);

      tu_cs_emit_pkt7(cs, CP_REG_TO_MEM, 3);
      tu_cs_emit(
         cs, CP_REG_TO_MEM_0_REG(ctr->select_reg) | CP_REG_TO_MEM_0_CNT(1));
      tu_cs_emit_qw(cs, offset_iova + offsetof(struct tu_perf_ctr_sample,
                                               selector_begin));

      tu_cs_emit_pkt7(cs, CP_REG_TO_MEM, 3);
      tu_cs_emit(
         cs, CP_REG_TO_MEM_0_REG(ctr->counter_reg_lo) | CP_REG_TO_MEM_0_64B);
      tu_cs_emit_qw(cs,
                    offset_iova + offsetof(struct tu_perf_ctr_sample, begin));
   };

   countable_begin(
      at->preemption_reaction_delay, 10,
      offsetof(struct tu_renderpass_samples, preemption_reaction_delay));
   countable_begin(at->num_preemptions, 11,
                   offsetof(struct tu_renderpass_samples, num_preemptions));
   countable_begin(at->always_count, 12,
                   offsetof(struct tu_renderpass_samples, always_count));

   tu_cs_emit_pkt7(cs, CP_WAIT_MEM_WRITES, 0);
   tu_cs_emit_wfi(cs);
#endif
}
TU_GENX(tu_autotune_begin_renderpass);

template <chip CHIP>
void tu_autotune_end_renderpass(struct tu_cmd_buffer *cmd,
                                struct tu_cs *cs,
                                struct tu_renderpass_result *autotune_result)
{
   if (!autotune_result)
      return;

   if (!autotune_result->bo.iova)
      return;

   uint64_t result_iova = autotune_result->bo.iova;

   tu_cs_emit_regs(cs, A6XX_RB_SAMPLE_COUNTER_CNTL(.copy = true));

   if (cmd->device->physical_device->info->a7xx.has_event_write_sample_count) {
      /* If the renderpass contains ZPASS_DONE events we emit a fake ZPASS_DONE
       * event here, composing a pair of these events that firmware handles without
       * issue. This first event writes into the samples_end field and the second
       * event overwrites it. The second event also enables the accumulation flag
       * even when we don't use that result because the blob always sets it.
       */
      if (cmd->state.rp.has_zpass_done_sample_count_write_in_rp) {
         tu_cs_emit_pkt7(cs, CP_EVENT_WRITE7, 3);
         tu_cs_emit(cs, CP_EVENT_WRITE7_0(.event = ZPASS_DONE,
                                          .write_sample_count = true).value);
         tu_cs_emit_qw(cs, result_iova + offsetof(struct tu_renderpass_samples, samples_end));
      }

      tu_cs_emit_pkt7(cs, CP_EVENT_WRITE7, 3);
      tu_cs_emit(cs, CP_EVENT_WRITE7_0(.event = ZPASS_DONE,
                                       .write_sample_count = true,
                                       .sample_count_end_offset = true,
                                       .write_accum_sample_count_diff = true).value);
      tu_cs_emit_qw(cs, result_iova);
   } else {
      tu_cs_emit_regs(
         cs,
         A6XX_RB_SAMPLE_COUNTER_BASE(.qword =
                                       result_iova +
                                       offsetof(struct tu_renderpass_samples,
                                                samples_end)));
      tu_cs_emit_pkt7(cs, CP_EVENT_WRITE, 1);
      tu_cs_emit(cs, ZPASS_DONE);
   }

   tu_cs_emit_pkt7(cs, CP_REG_TO_MEM, 3);
   tu_cs_emit(cs, CP_REG_TO_MEM_0_REG(REG_A6XX_CP_ALWAYS_ON_COUNTER) |
                  CP_REG_TO_MEM_0_CNT(2) |
                  CP_REG_TO_MEM_0_64B);
   tu_cs_emit_qw(cs, result_iova + offsetof(struct tu_renderpass_samples, ts_end));

#if TU_AUTOTUNE_DEBUG_PERFCTR
   tu_cs_emit_wfi(cs);

   auto countable_end = [&, at = &cmd->device->autotune](
                                 uint32_t cntr_idx, uint64_t offset) {
      const struct fd_perfcntr_counter *ctr =
         &at->cp_group->counters[cntr_idx];
      uint64_t offset_iova = result_iova + offset;

      tu_cs_emit_pkt7(cs, CP_REG_TO_MEM, 3);
      tu_cs_emit(
         cs, CP_REG_TO_MEM_0_REG(ctr->select_reg) | CP_REG_TO_MEM_0_CNT(1));
      tu_cs_emit_qw(
         cs, offset_iova + offsetof(struct tu_perf_ctr_sample, selector_end));

      tu_cs_emit_pkt7(cs, CP_REG_TO_MEM, 3);
      tu_cs_emit(
         cs, CP_REG_TO_MEM_0_REG(ctr->counter_reg_lo) | CP_REG_TO_MEM_0_64B);
      tu_cs_emit_qw(cs,
                    offset_iova + offsetof(struct tu_perf_ctr_sample, end));
   };

   countable_end(
      10, offsetof(struct tu_renderpass_samples, preemption_reaction_delay));
   countable_end(11, offsetof(struct tu_renderpass_samples, num_preemptions));
   countable_end(12, offsetof(struct tu_renderpass_samples, always_count));

   tu_cs_emit_pkt7(cs, CP_WAIT_MEM_WRITES, 0);
   tu_cs_emit_wfi(cs);

   tu_cs_emit_pkt7(cs, CP_MEM_WRITE, 4);
   tu_cs_emit_qw(
      cs, result_iova + offsetof(struct tu_renderpass_samples, cntrs_ready));
   tu_cs_emit_qw(cs, tu_renderpass_samples::CNTRS_READY_MAGIC);
#endif
}
TU_GENX(tu_autotune_end_renderpass);

template <chip CHIP>
void
tu_autotune_begin_tile(struct tu_cmd_buffer *cmd,
                       struct tu_cs *cs,
                       struct tu_renderpass_result *autotune_result,
                       uint32_t tile_idx)
{
   if (!autotune_result || autotune_result->tile_count < tile_idx)
      return;

   assert(!autotune_result->sysmem);

   uint32_t offset = sizeof(struct tu_renderpass_samples) +
                     (sizeof(struct tu_tile_samples) * tile_idx);
   tu_cs_emit_pkt7(cs, CP_REG_TO_MEM, 3);
   tu_cs_emit(cs, CP_REG_TO_MEM_0_REG(REG_A6XX_CP_ALWAYS_ON_COUNTER) |
                  CP_REG_TO_MEM_0_CNT(2) |
                  CP_REG_TO_MEM_0_64B);
   tu_cs_emit_qw(cs, autotune_result->bo.iova + offset +
                        offsetof(struct tu_tile_samples, ts_start));
}
TU_GENX(tu_autotune_begin_tile);

template <chip CHIP>
void
tu_autotune_end_tile(struct tu_cmd_buffer *cmd,
                       struct tu_cs *cs,
                       struct tu_renderpass_result *autotune_result,
                       uint32_t tile_idx)
{
   if (!autotune_result || autotune_result->tile_count < tile_idx)
      return;

   assert(!autotune_result->sysmem);

   uint32_t offset = sizeof(struct tu_renderpass_samples) +
                     (sizeof(struct tu_tile_samples) * tile_idx);
   tu_cs_emit_pkt7(cs, CP_REG_TO_MEM, 3);
   tu_cs_emit(cs, CP_REG_TO_MEM_0_REG(REG_A6XX_CP_ALWAYS_ON_COUNTER) |
                  CP_REG_TO_MEM_0_CNT(2) |
                  CP_REG_TO_MEM_0_64B);
   tu_cs_emit_qw(cs, autotune_result->bo.iova + offset +
                        offsetof(struct tu_tile_samples, ts_end));
}
TU_GENX(tu_autotune_end_tile);
