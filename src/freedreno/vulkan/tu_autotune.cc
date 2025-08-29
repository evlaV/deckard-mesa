/*
 * Copyright © 2021 Igalia S.L.
 * SPDX-License-Identifier: MIT
 */

#include "tu_autotune.h"

#include <array>
#include <atomic>
#include <optional>
#include <string>
#include <string_view>

#include "util/rand_xor.h"

#define XXH_INLINE_ALL
#include "util/xxhash.h"

#include "tu_cmd_buffer.h"
#include "tu_cs.h"
#include "tu_device.h"
#include "tu_image.h"
#include "tu_pass.h"

/** Compile-time debug options **/

#define TU_AUTOTUNE_DEBUG_LOG_BASE      0
#define TU_AUTOTUNE_DEBUG_LOG_BANDWIDTH 0
#define TU_AUTOTUNE_DEBUG_LOG_PROFILED  0
#define TU_AUTOTUNE_DEBUG_LOG_PREEMPT   0

/* Process any pending entries on autotuner finish, could be used to gather data from traces. */
#define TU_AUTOTUNE_LOG_AT_FINISH 0

/** Global constants and helpers **/

/* How long we keep the duration entries. */
constexpr uint32_t MAX_DURATION_COUNT = 500;

/* GPU always-on timer constants */
constexpr uint64_t ALWAYS_ON_FREQUENCY_HZ = 19'200'000;
constexpr uint64_t GPU_TICKS_PER_US = ALWAYS_ON_FREQUENCY_HZ / 1'000'000;

constexpr uint64_t
ticks_to_us(uint64_t ticks)
{
   return ticks / GPU_TICKS_PER_US;
}

constexpr bool
fence_before(uint32_t a, uint32_t b)
{
   /* Essentially a < b, but handles wrapped values. */
   return (int32_t) (a - b) < 0;
}

constexpr const char *
render_mode_str(tu_autotune::render_mode mode)
{
   switch (mode) {
   case tu_autotune::render_mode::SYSMEM:
      return "SYSMEM";
   case tu_autotune::render_mode::GMEM:
      return "GMEM";
   default:
      return "UNKNOWN";
   }
}

/** Configuration **/

enum class tu_autotune::algorithm : uint8_t {
   BANDWIDTH = 0,    /* Uses estimated BW for determining rendering mode. */
   PROFILED = 1,     /* Uses dynamically profiled results for determining rendering mode. */
   PROFILED_IMM = 2, /* Same as PROFILED but immediately resolves the SYSMEM/GMEM probability. */

   DEFAULT = BANDWIDTH, /* Default algorithm, used if no other is specified. */
};

/* Modifier flags, these modify the behavior of the autotuner in a user-defined way. */
enum class tu_autotune::mod_flag : uint8_t {
   BIG_GMEM = BIT(1),          /* All RPs with >= 10 draws use GMEM. */
   SMALL_SYSMEM = BIT(2),      /* All RPs with <= 5 draws use SYSMEM. */
   PREEMPT_OPTIMIZE = BIT(3),  /* Attempts to minimize the preemption latency. */
};

/* Metric flags, for internal tracking of enabled metrics. */
enum class tu_autotune::metric_flag : uint8_t {
   SAMPLES = BIT(1), /* Enable tracking samples passed metric. */
   TS = BIT(2),      /* Enable tracking per-RP timestamp metric. */
   TS_TILE = BIT(3), /* Enable tracking per-tile timestamp metric. */
};

struct PACKED tu_autotune::config_t {
 private:
   algorithm algo = algorithm::DEFAULT;
   uint8_t mod_flags = 0;    /* See mod_flag enum. */
   uint8_t metric_flags = 0; /* See metric_flag enum. */

   constexpr void update_metric_flags()
   {
      /* Note: Always keep in sync with rp_history to prevent UB. */
      if (algo == algorithm::BANDWIDTH) {
         metric_flags |= (uint8_t) metric_flag::SAMPLES;
      } else if (algo == algorithm::PROFILED || algo == algorithm::PROFILED_IMM) {
         metric_flags |= (uint8_t) metric_flag::TS;
      }

      if (mod_flags & (uint8_t) mod_flag::PREEMPT_OPTIMIZE) {
         metric_flags |= (uint8_t) metric_flag::TS | (uint8_t) metric_flag::TS_TILE;
      }
   }

 public:
   constexpr config_t() = default;

   constexpr config_t(algorithm algo, uint8_t mod_flags): algo(algo), mod_flags(mod_flags)
   {
      update_metric_flags();
   }

   constexpr bool is_enabled(algorithm a) const
   {
      return algo == a;
   }

   constexpr bool test(mod_flag f) const
   {
      return mod_flags & (uint32_t) f;
   }

   constexpr bool test(metric_flag f) const
   {
      return metric_flags & (uint32_t) f;
   }

   constexpr bool set_algo(algorithm a)
   {
      if (algo == a)
         return false;

      algo = a;
      update_metric_flags();
      return true;
   }

   constexpr bool disable(mod_flag f)
   {
      if (!(mod_flags & (uint8_t) f))
         return false;

      mod_flags &= ~(uint8_t) f;
      update_metric_flags();
      return true;
   }

   constexpr bool enable(mod_flag f)
   {
      if (mod_flags & (uint8_t) f)
         return false;

      mod_flags |= (uint8_t) f;
      update_metric_flags();
      return true;
   }

   std::string to_string() const
   {
#define ALGO_STR(algo_name)                                                                                            \
   if (algo == algorithm::algo_name)                                                                                   \
      str += #algo_name;
#define MODF_STR(flag)                                                                                                 \
   if (mod_flags & (uint8_t) mod_flag::flag) {                                                                         \
      str += #flag " ";                                                                                                \
   }
#define METRICF_STR(flag)                                                                                              \
   if (metric_flags & (uint8_t) metric_flag::flag) {                                                                   \
      str += #flag " ";                                                                                                \
   }

      std::string str = "Algorithm: ";

      ALGO_STR(BANDWIDTH);
      ALGO_STR(PROFILED);
      ALGO_STR(PROFILED_IMM);

      str += ", Mod Flags: 0x" + std::to_string(mod_flags) + " (";
      MODF_STR(BIG_GMEM);
      MODF_STR(SMALL_SYSMEM);
      MODF_STR(PREEMPT_OPTIMIZE);
      str += ")";

      str += ", Metric Flags: 0x" + std::to_string(metric_flags) + " (";
      METRICF_STR(SAMPLES);
      METRICF_STR(TS);
      METRICF_STR(TS_TILE);
      str += ")";

      return str;

#undef ALGO_STR
#undef MODF_STR
#undef METRICF_STR
   }
};

union tu_autotune::packed_config_t {
   config_t config;
   uint32_t bits = 0;
   static_assert(sizeof(bits) >= sizeof(config));
   static_assert(std::is_trivially_copyable<config_t>::value,
                 "config_t must be trivially copyable to be automatically packed");

   constexpr packed_config_t(config_t config): config(config)
   {
   }

   constexpr packed_config_t(uint32_t bits): bits(bits)
   {
   }
};

tu_autotune::atomic_config_t::atomic_config_t(config_t initial): config_bits(packed_config_t { initial }.bits)
{
}

tu_autotune::config_t
tu_autotune::atomic_config_t::load() const
{
   return config_t(packed_config_t { config_bits.load(std::memory_order_relaxed) }.config);
}

bool
tu_autotune::atomic_config_t::compare_and_store(config_t updated, config_t expected)
{
   uint32_t expected_bits = packed_config_t { expected }.bits;
   return config_bits.compare_exchange_strong(expected_bits, packed_config_t { updated }.bits,
                                              std::memory_order_acquire, std::memory_order_relaxed);
}

tu_autotune::config_t
tu_autotune::get_env_config()
{
   static std::once_flag once;
   static config_t at_config;
   std::call_once(once, [&] {
      const char *algo_env_str = os_get_option("TU_AUTOTUNE_ALGO");
      algorithm algo = algorithm::DEFAULT;

      if (algo_env_str) {
         std::string_view algo_strv(algo_env_str);
         if (algo_strv == "bandwidth") {
            algo = algorithm::BANDWIDTH;
         } else if (algo_strv == "profiled") {
            algo = algorithm::PROFILED;
         } else if (algo_strv == "profiled_imm") {
            algo = algorithm::PROFILED_IMM;
         } else {
            mesa_logw("Unknown TU_AUTOTUNE_ALGO '%s', using default", algo_env_str);
         }

         if (TU_DEBUG(STARTUP))
            mesa_logi("TU_AUTOTUNE_ALGO=%u (%s)", (uint8_t) algo, algo_env_str);
      }

      /* Parse the flags from the environment variable. */
      const char *flags_env_str = os_get_option("TU_AUTOTUNE_FLAGS");
      uint32_t mod_flags = 0;
      if (flags_env_str) {
         static const struct debug_control tu_at_flags_control[] = {
            { "big_gmem", (uint32_t) mod_flag::BIG_GMEM },
            { "small_sysmem", (uint32_t) mod_flag::SMALL_SYSMEM },
            { "preempt_optimize", (uint32_t) mod_flag::PREEMPT_OPTIMIZE },
            { NULL, 0 }
         };

         mod_flags = parse_debug_string(flags_env_str, tu_at_flags_control);
         if (TU_DEBUG(STARTUP))
            mesa_logi("TU_AUTOTUNE_FLAGS=0x%x (%s)", mod_flags, flags_env_str);
      }

      assert((uint8_t) mod_flags == mod_flags);
      at_config = config_t(algo, (uint8_t) mod_flags);
   });

   if (TU_DEBUG(STARTUP))
      mesa_logi("TU_AUTOTUNE: %s", at_config.to_string().c_str());

   return at_config;
}

/** Global Fence and Internal CS Management **/

tu_autotune::submission_entry::submission_entry(tu_device *device): fence(0)
{
   tu_cs_init(&fence_cs, device, TU_CS_MODE_GROW, 5, "autotune fence cs");
}

tu_autotune::submission_entry::~submission_entry()
{
   assert(!is_active());
   tu_cs_finish(&fence_cs);
}

bool
tu_autotune::submission_entry::is_active() const
{
   auto &device = fence_cs.device;
   return device->global_bo_map->autotune_fence < fence;
}

template <chip CHIP>
static void
write_fence_cs(struct tu_device *dev, struct tu_cs *cs, uint32_t fence)
{
   uint64_t dst_iova = dev->global_bo->iova + gb_offset(autotune_fence);
   if (CHIP >= A7XX) {
      tu_cs_emit_pkt7(cs, CP_EVENT_WRITE7, 4);
      tu_cs_emit(cs, CP_EVENT_WRITE7_0(.event = CACHE_FLUSH_TS, .write_src = EV_WRITE_USER_32B, .write_dst = EV_DST_RAM,
                                       .write_enabled = true)
                        .value);
   } else {
      tu_cs_emit_pkt7(cs, CP_EVENT_WRITE, 4);
      tu_cs_emit(cs, CP_EVENT_WRITE_0_EVENT(CACHE_FLUSH_TS));
   }

   tu_cs_emit_qw(cs, dst_iova);
   tu_cs_emit(cs, fence);
}

struct tu_cs *
tu_autotune::submission_entry::try_get_cs(uint32_t new_fence)
{
   if (is_active()) {
      /* If the CS is already active, we cannot write to it. */
      return nullptr;
   }

   auto &device = fence_cs.device;
   tu_cs_reset(&fence_cs);
   tu_cs_begin(&fence_cs);
   TU_CALLX(device, write_fence_cs)(device, &fence_cs, new_fence);
   tu_cs_end(&fence_cs);
   assert(fence_cs.entry_count == 1); /* We expect the initial allocation to be large enough. */
   fence = new_fence;

   return &fence_cs;
}

struct tu_cs *
tu_autotune::get_cs_for_fence(uint32_t fence)
{
   for (auto &entry : submission_entries) {
      auto cs = entry.try_get_cs(fence);
      if (cs)
         return cs;
   }

   /* If we reach here, we have to allocate a new entry. */
   auto &entry = submission_entries.emplace_back(device);
   auto cs = entry.try_get_cs(fence);
   assert(cs); /* We just allocated it, so it should be available. */
   return cs;
}

/** RP Entry Management **/

#if TU_AUTOTUNE_DEBUG_PERFCTR
struct PACKED tu_perf_ctr_sample {
   uint64_t begin;
   uint64_t end;
   /* The selector value at the beginning/end, used to validate that the countable wasn't changed during a preemption. */
   uint32_t selector_begin;
   uint32_t selector_end;
};
#endif

/* The part of the per-RP entry which is written by the GPU. */
struct PACKED tu_autotune::rp_gpu_data {
   /* HW requires the sample start/stop locations to be 128b aligned. */
   alignas(16) uint64_t samples_start;
   alignas(16) uint64_t samples_end;
   uint64_t ts_start;
   uint64_t ts_end;

#if TU_AUTOTUNE_DEBUG_PERFCTR
   struct tu_perf_ctr_sample preemption_reaction_delay, num_preemptions, always_count;
   uint64_t cntrs_ready;
   constexpr static uint64_t CNTRS_READY_MAGIC = 0xABCDEFEFE;
#endif
};

/* Per-tile values for GMEM rendering, this structure is appended to the end of rp_gpu_data for each tile. */
struct PACKED tu_autotune::tile_gpu_data {
   uint64_t ts_start;
   uint64_t ts_end;

   /* A helper for the offset of this relative to BO start. */
   static constexpr uint64_t offset(uint32_t tile_index)
   {
      return sizeof(rp_gpu_data) + (tile_index * sizeof(tile_gpu_data));
   }
};

/* An "entry" of renderpass autotune results, which is used to store the results of a renderpass autotune run for a given command buffer. */
struct tu_autotune::rp_entry {
 private:
   struct tu_device *device;

   struct tu_suballoc_bo bo;
   uint8_t *map; /* A direct pointer to the BO's CPU mapping. */

   /* Ensure that GPU data layout aligns with our expectations.
    * Note: We can only check here since it's a private structure
    *       and the definition is only complete at this point.
    */
   static_assert(alignof(rp_gpu_data) == 16);
   static_assert(offsetof(rp_gpu_data, samples_start) == 0);
   static_assert(offsetof(rp_gpu_data, samples_end) == 16);
   static_assert(sizeof(rp_gpu_data) % alignof(tile_gpu_data) == 0);

 public:
   rp_history *history; /* Guaranteed to never be nullptr. */
   config_t config;     /* Configuration at the time of entry creation. */
   bool sysmem;
   uint32_t tile_count;
   uint32_t draw_count;

   rp_entry(struct tu_device *device, rp_history &history, config_t config, uint32_t draw_count)
       : device(device), map(nullptr), history(&history), config(config), draw_count(draw_count)
   {
   }

   ~rp_entry()
   {
      if (map) {
         std::scoped_lock lock(device->autotune.suballoc_mutex);
         tu_suballoc_bo_free(&device->autotune.suballoc, &bo);
      }
   }

   /* Disable the copy operators as that shouldn't be done. */
   rp_entry(const rp_entry &) = delete;
   rp_entry &operator=(const rp_entry &) = delete;

   /* Enable the move operators to allow moves from one container to another. */
   rp_entry(rp_entry &&other) noexcept
       : device(other.device), bo(other.bo), map(other.map), history(other.history), config(other.config),
         sysmem(other.sysmem), tile_count(other.tile_count), draw_count(other.draw_count)
   {
      other.map = nullptr; /* Prevent the destructor from freeing the BO. */
   }

   rp_entry &operator=(rp_entry &&other) noexcept
   {
      if (this != &other) {
         device = other.device;
         bo = other.bo;
         map = other.map;
         history = other.history;
         config = other.config;
         sysmem = other.sysmem;
         tile_count = other.tile_count;
         draw_count = other.draw_count;

         other.map = nullptr;
      }
      return *this;
   }

   void allocate(bool sysmem, uint32_t tile_count)
   {
      this->sysmem = sysmem;
      this->tile_count = tile_count;
      size_t total_size = sizeof(rp_gpu_data) + (tile_count * sizeof(tile_gpu_data));

      std::scoped_lock lock(device->autotune.suballoc_mutex);
      VkResult result = tu_suballoc_bo_alloc(&bo, &device->autotune.suballoc, total_size, alignof(rp_gpu_data));
      if (result != VK_SUCCESS) {
         mesa_loge("Failed to allocate BO for autotune rp_entry: %u", result);
         return;
      }

      map = (uint8_t *) tu_suballoc_bo_map(&bo);
      memset(map, 0, total_size);
   }

   rp_gpu_data &get_gpu_data()
   {
      assert(map);
      return *(rp_gpu_data *) map;
   }

   tile_gpu_data &get_tile_gpu_data(uint32_t tile_index)
   {
      assert(map);
      assert(tile_index < tile_count);
      auto offset = tile_gpu_data::offset(tile_index);
      return *(tile_gpu_data *) (map + offset);
   }

   /** Samples-Passed Metric **/

   uint64_t get_samples_passed()
   {
      assert(config.test(metric_flag::SAMPLES));
      auto &gpu = get_gpu_data();
      return gpu.samples_end - gpu.samples_start;
   }

   void emit_metric_samples_start(struct tu_cmd_buffer *cmd, struct tu_cs *cs, uint64_t start_iova)
   {
      tu_cs_emit_regs(cs, A6XX_RB_SAMPLE_COUNTER_CNTL(.copy = true));
      if (cmd->device->physical_device->info->a7xx.has_event_write_sample_count) {
         tu_cs_emit_pkt7(cs, CP_EVENT_WRITE7, 3);
         tu_cs_emit(cs, CP_EVENT_WRITE7_0(.event = ZPASS_DONE, .write_sample_count = true).value);
         tu_cs_emit_qw(cs, start_iova);

         /* If the renderpass contains an occlusion query with its own ZPASS_DONE, we have to provide a fake ZPASS_DONE
          * event here to logically close the previous one, preventing firmware from misbehaving due to nested events.
          * This writes into the samples_end field, which will be overwritten in tu_autotune_end_renderpass.
          */
         if (cmd->state.rp.has_zpass_done_sample_count_write_in_rp) {
            tu_cs_emit_pkt7(cs, CP_EVENT_WRITE7, 3);
            tu_cs_emit(cs, CP_EVENT_WRITE7_0(.event = ZPASS_DONE, .write_sample_count = true,
                                             .sample_count_end_offset = true, .write_accum_sample_count_diff = true)
                              .value);
            tu_cs_emit_qw(cs, start_iova);
         }
      } else {
         tu_cs_emit_regs(cs, A6XX_RB_SAMPLE_COUNTER_BASE(.qword = start_iova));
         tu_cs_emit_pkt7(cs, CP_EVENT_WRITE, 1);
         tu_cs_emit(cs, ZPASS_DONE);
      }
   }

   void emit_metric_samples_end(struct tu_cmd_buffer *cmd, struct tu_cs *cs, uint64_t start_iova, uint64_t end_iova)
   {
      tu_cs_emit_regs(cs, A6XX_RB_SAMPLE_COUNTER_CNTL(.copy = true));
      if (cmd->device->physical_device->info->a7xx.has_event_write_sample_count) {
         /* If the renderpass contains ZPASS_DONE events we emit a fake ZPASS_DONE event here, composing a pair of these
          * events that firmware handles without issue. This first event writes into the samples_end field and the
          * second event overwrites it. The second event also enables the accumulation flag even when we don't use that
          * result because the blob always sets it.
          */
         if (cmd->state.rp.has_zpass_done_sample_count_write_in_rp) {
            tu_cs_emit_pkt7(cs, CP_EVENT_WRITE7, 3);
            tu_cs_emit(cs, CP_EVENT_WRITE7_0(.event = ZPASS_DONE, .write_sample_count = true).value);
            tu_cs_emit_qw(cs, end_iova);
         }

         tu_cs_emit_pkt7(cs, CP_EVENT_WRITE7, 3);
         tu_cs_emit(cs, CP_EVENT_WRITE7_0(.event = ZPASS_DONE, .write_sample_count = true,
                                          .sample_count_end_offset = true, .write_accum_sample_count_diff = true)
                           .value);
         tu_cs_emit_qw(cs, start_iova);
      } else {
         tu_cs_emit_regs(cs, A6XX_RB_SAMPLE_COUNTER_BASE(.qword = end_iova));
         tu_cs_emit_pkt7(cs, CP_EVENT_WRITE, 1);
         tu_cs_emit(cs, ZPASS_DONE);
      }
   }

   /** RP/Tile Timestamp Metric **/

   uint64_t get_rp_duration()
   {
      assert(config.test(metric_flag::TS));
      auto &gpu = get_gpu_data();
      return gpu.ts_end - gpu.ts_start;
   }

   /* The amount of cycles spent in the longest tile. This is used to calculate the average draw duration for
    * determining the largest non-preemptible duration for GMEM rendering.
    */
   uint64_t get_max_tile_duration()
   {
      assert(config.test(metric_flag::TS_TILE));
      uint64_t max_duration = 0;
      for (uint32_t i = 0; i < tile_count; i++) {
         auto &tile = get_tile_gpu_data(i);
         max_duration = MAX2(max_duration, tile.ts_end - tile.ts_start);
      }
      return max_duration;
   }

   void emit_metric_timestamp(struct tu_cs *cs, uint64_t timestamp_iova)
   {
      tu_cs_emit_pkt7(cs, CP_REG_TO_MEM, 3);
      tu_cs_emit(cs, CP_REG_TO_MEM_0_REG(REG_A6XX_CP_ALWAYS_ON_COUNTER) | CP_REG_TO_MEM_0_CNT(2) | CP_REG_TO_MEM_0_64B);
      tu_cs_emit_qw(cs, timestamp_iova);
   }

   /** Debug Performance Counters **/

#if TU_AUTOTUNE_DEBUG_PERFCTR
   uint64_t get_preemption_reaction_delay(tu_autotune &at, size_t rp_hash)
   {
      auto &gpu = get_gpu_data();

      while (p_atomic_read(&gpu.cntrs_ready) != rp_gpu_data::CNTRS_READY_MAGIC) {
         /* Just spin until the counter values are written out. */
      }

      auto read_counter = [&](const struct tu_perf_ctr_sample &sample, const struct fd_perfcntr_countable *ctbl,
                              uint64_t &outValue, const char *name) {
         if (sample.selector_begin != sample.selector_end || sample.selector_begin != ctbl->selector) {
            mesa_loge("autotune %016" PRIx64 ": %s: selector mismatch %" PRIu32 " != %" PRIu32 " (%" PRIu32 " - %" PRIu32 ")", rp_hash, ctbl->name,
                      sample.selector_begin, sample.selector_end, sample.selector_begin, ctbl->selector);
         }

         outValue = sample.end - sample.begin;
         if (sample.end < sample.begin) {
            mesa_loge("autotune %016" PRIx64 ": %s: end < begin %" PRIu64 " < %" PRIu64, rp_hash, name, sample.end, sample.begin);
            outValue = 0;
         }
      };

      /* We read all counters for logging, even though we only need to return the preemption reaction delay. */
      uint64_t preemption_reaction_delay, num_preemptions, always_count;
      read_counter(gpu.preemption_reaction_delay, at.preemption_reaction_delay, preemption_reaction_delay,
                   "preemption_reaction_delay");
      read_counter(gpu.num_preemptions, at.num_preemptions, num_preemptions, "num_preemptions");
      read_counter(gpu.always_count, at.always_count, always_count, "always_count");

      if (preemption_reaction_delay || num_preemptions) {
         mesa_logi("autotune %016" PRIx64 ": preemption_reaction_delay: %" PRIu64 ", always_count: %" PRIu64 ", num_preemptions: %" PRIu64,
                   rp_hash, preemption_reaction_delay, always_count, num_preemptions);
      }

      return preemption_reaction_delay;
   }

   void emit_debug_perfcntr_start(struct tu_cs *cs, tu_autotune &at, uint64_t bo_iova)
   {
      auto countable_begin = [&](const struct fd_perfcntr_countable *ctbl, uint32_t cntr_idx, uint32_t offset) {
         const struct fd_perfcntr_counter *ctr = &at.cp_group->counters[cntr_idx];
         uint64_t offset_iova = bo_iova + offset;
         assert(!ctr->enable); /* CP counters shouldn't use it. */

         tu_cs_emit_pkt4(cs, ctr->select_reg, 1);
         tu_cs_emit(cs, ctbl->selector);

         tu_cs_emit_wfi(cs);

         tu_cs_emit_pkt7(cs, CP_REG_TO_MEM, 3);
         tu_cs_emit(cs, CP_REG_TO_MEM_0_REG(ctr->select_reg) | CP_REG_TO_MEM_0_CNT(1));
         tu_cs_emit_qw(cs, offset_iova + offsetof(struct tu_perf_ctr_sample, selector_begin));

         tu_cs_emit_pkt7(cs, CP_REG_TO_MEM, 3);
         tu_cs_emit(cs, CP_REG_TO_MEM_0_REG(ctr->counter_reg_lo) | CP_REG_TO_MEM_0_64B);
         tu_cs_emit_qw(cs, offset_iova + offsetof(struct tu_perf_ctr_sample, begin));
      };

      countable_begin(at.preemption_reaction_delay, 10, offsetof(rp_gpu_data, preemption_reaction_delay));
      countable_begin(at.num_preemptions, 11, offsetof(rp_gpu_data, num_preemptions));
      countable_begin(at.always_count, 12, offsetof(rp_gpu_data, always_count));

      tu_cs_emit_pkt7(cs, CP_WAIT_MEM_WRITES, 0);
      tu_cs_emit_wfi(cs);
   }

   void emit_debug_perfcntr_end(struct tu_cs *cs, tu_autotune &at, uint64_t bo_iova)
   {
      tu_cs_emit_wfi(cs);

      auto countable_end = [&](uint32_t cntr_idx, uint64_t offset) {
         const struct fd_perfcntr_counter *ctr = &at.cp_group->counters[cntr_idx];
         uint64_t offset_iova = bo_iova + offset;

         tu_cs_emit_pkt7(cs, CP_REG_TO_MEM, 3);
         tu_cs_emit(cs, CP_REG_TO_MEM_0_REG(ctr->select_reg) | CP_REG_TO_MEM_0_CNT(1));
         tu_cs_emit_qw(cs, offset_iova + offsetof(struct tu_perf_ctr_sample, selector_end));

         tu_cs_emit_pkt7(cs, CP_REG_TO_MEM, 3);
         tu_cs_emit(cs, CP_REG_TO_MEM_0_REG(ctr->counter_reg_lo) | CP_REG_TO_MEM_0_64B);
         tu_cs_emit_qw(cs, offset_iova + offsetof(struct tu_perf_ctr_sample, end));
      };

      countable_end(10, offsetof(rp_gpu_data, preemption_reaction_delay));
      countable_end(11, offsetof(rp_gpu_data, num_preemptions));
      countable_end(12, offsetof(rp_gpu_data, always_count));

      tu_cs_emit_pkt7(cs, CP_WAIT_MEM_WRITES, 0);
      tu_cs_emit_wfi(cs);

      tu_cs_emit_pkt7(cs, CP_MEM_WRITE, 4);
      tu_cs_emit_qw(cs, bo_iova + offsetof(rp_gpu_data, cntrs_ready));
      tu_cs_emit_qw(cs, rp_gpu_data::CNTRS_READY_MAGIC);
   }
#endif

   /** CS Emission **/

   void emit_rp_start(struct tu_cmd_buffer *cmd, struct tu_cs *cs)
   {
      assert(map && bo.iova);
      uint64_t bo_iova = bo.iova;
      if (config.test(metric_flag::SAMPLES))
         emit_metric_samples_start(cmd, cs, bo_iova + offsetof(rp_gpu_data, samples_start));

      if (config.test(metric_flag::TS))
         emit_metric_timestamp(cs, bo_iova + offsetof(rp_gpu_data, ts_start));

#if TU_AUTOTUNE_DEBUG_PERFCTR
      emit_debug_perfcntr_start(cs, cmd->device->autotune, bo_iova);
#endif
   }

   void emit_rp_end(struct tu_cmd_buffer *cmd, struct tu_cs *cs)
   {
      assert(map && bo.iova);
      uint64_t bo_iova = bo.iova;
      if (config.test(metric_flag::SAMPLES))
         emit_metric_samples_end(cmd, cs, bo_iova + offsetof(rp_gpu_data, samples_start), bo_iova + offsetof(rp_gpu_data, samples_end));

      if (config.test(metric_flag::TS))
         emit_metric_timestamp(cs, bo_iova + offsetof(rp_gpu_data, ts_end));

#if TU_AUTOTUNE_DEBUG_PERFCTR
      emit_debug_perfcntr_end(cs, cmd->device->autotune, bo_iova);
#endif
   }

   void emit_tile_start(struct tu_cmd_buffer *cmd, struct tu_cs *cs, uint32_t tile_index)
   {
      assert(map && bo.iova);
      assert(!sysmem);
      assert(tile_index < tile_count);
      if (config.test(metric_flag::TS_TILE))
         emit_metric_timestamp(cs, bo.iova + tile_gpu_data::offset(tile_index) + offsetof(tile_gpu_data, ts_start));
   }

   void emit_tile_end(struct tu_cmd_buffer *cmd, struct tu_cs *cs, uint32_t tile_index)
   {
      assert(map && bo.iova);
      assert(!sysmem);
      assert(tile_index < tile_count);
      if (config.test(metric_flag::TS_TILE))
         emit_metric_timestamp(cs, bo.iova + tile_gpu_data::offset(tile_index) + offsetof(tile_gpu_data, ts_end));
   }
};

tu_autotune::rp_entry_batch::rp_entry_batch(): active(false), fence(0), entries()
{
}

void
tu_autotune::rp_entry_batch::assign_fence(uint32_t new_fence)
{
   assert(!active); /* Cannot assign a fence to an active entry batch. */
   fence = new_fence;
   active = true;
}

/** Renderpass state tracking. **/

tu_autotune::rp_key::rp_key(const struct tu_render_pass *pass,
                            const struct tu_framebuffer *framebuffer,
                            const struct tu_cmd_buffer *cmd)
{
   /* Q: Why not make the key from framebuffer + renderpass pointers?
    * A: At least DXVK creates new framebuffers each frame while keeping renderpasses the same. Hashing the contents
    *    of the framebuffer and renderpass is more stable, and it maintains stability across runs, so we can reliably
    *    identify the same renderpass instance.
    */

   auto get_hash = [&](uint32_t *data, size_t size) {
      auto ptr = data;
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

      return XXH64(data, size * sizeof(uint32_t), 0);
   };

   /* We do a manual Boost-style "small vector" optimization here where the stack is used for the vast majority of
    * cases, while only extreme cases need to allocate on the heap.
    */
   size_t data_count = 3 + (pass->attachment_count * 5);
   constexpr size_t STACK_MAX_DATA_COUNT = 3 + (5 * 5); /* in u32 units. */

   if (data_count <= STACK_MAX_DATA_COUNT) {
      /* If the data is small enough, we can use the stack. */
      std::array<uint32_t, STACK_MAX_DATA_COUNT> arr;
      hash = get_hash(arr.data(), data_count);
   } else {
      /* If the data is too large, we have to allocate it on the heap. */
      std::vector<uint32_t> vec(data_count);
      hash = get_hash(vec.data(), vec.size());
   }
}

/* Draw duration tracker and sampler, built atop circular buffers.
 * Note: This is not thread‑safe, all accesses must be synchronised externally.
 */
class tu_at_durations {
 private:
   struct entry {
      uint64_t duration; /* In arbitrary GPU ticks. */
      uint32_t draw_count;
   };

   std::array<entry, MAX_DURATION_COUNT> array;
   size_t head = 0; /* Index of the oldest entry. */
   size_t size = 0; /* Amount of active entries. */

 public:
   void add(uint64_t duration, uint32_t draw_count) noexcept
   {
      // TODO: Clean up indexing calcs.
      size_t idx = (head + size) % array.size();
      if (size < array.size()) {
         /* If we have space, just add the new entry. */
         array[idx] = { duration, draw_count };
         ++size;
      } else {
         /* If we are full, overwrite the oldest entry. */
         array[head] = { duration, draw_count };
         head = (head + 1) % array.size();
      }
   }

   void clear() noexcept
   {
      /* No need to free any allocations, we can just reset the head/size. */
      head = 0;
      size = 0;
   }

   /* Calculates the average duration of the entries in the list. It can optionally divide the duration by the draw
    * count.
    *
    * It can also apply a recency bias to the average, where more recent entries are given more weight in the average
    * calculation. This is based on the recency_bias_base parameter, which is the base of the exponential function used
    * to calculate the weight.
    *
    * A value of 1.0 means no recency bias, while any value < 1.0 will apply a bias towards more recent entries, while
    * any value > 1.0 will apply a bias towards older entries. Any value <= 0.0 will lead to UB.
    *
    * Note: This function clears old entries while calculating the average, so the list will be modified after this
    *       function is called.
    */
   void get_avg(uint64_t &out_avg,
                uint32_t &out_count,
                bool divide_by_draw_count = false,
                double recency_bias_base = 1.0) const noexcept
   {
      out_count = static_cast<uint32_t>(size);
      if (size == 0) {
         out_avg = 0;
         return;
      }

      auto get_duration = [&](size_t idx) -> double {
         double d = static_cast<double>(array[idx].duration);
         if (divide_by_draw_count && array[idx].draw_count)
            d /= array[idx].draw_count;
         return d;
      };

      if (recency_bias_base == 1.0) {
         /* Unbiased arithmetic mean. */
         double total = 0.0;
         for (size_t idx = 0; idx < size; ++idx) {
            size_t array_idx = (head + idx) % array.size();
            total += get_duration(array_idx);
         }
         out_avg = static_cast<uint64_t>(total / size);
      } else {
         /* Recency‑biased mean. */
         double weighted_sum = 0.0, total_weights = 0.0, running_weight = 1.0;

         /* Iterate newest to oldest for recency weighting. */
         for (size_t idx = 0; idx < size; ++idx) {
            size_t array_idx = (head + size - 1 - idx) % array.size();

            double duration = get_duration(array_idx);
            weighted_sum += duration * running_weight;
            total_weights += running_weight;
            running_weight *= recency_bias_base;

            if (running_weight < 0.01) {
               /* Quit early when the weight becomes negligible. */
               break;
            }
         }
         out_avg = static_cast<uint64_t>(weighted_sum / total_weights);
      }
   }
};

/* All historical state pertaining to a uniquely identified RP. This integrates data from RP entries, accumulating
 * metrics over the long-term and providing autotune algorithms using the data.
 */
struct tu_autotune::rp_history {
 private:
   tu_at_durations sysmem_rp_durations;
   tu_at_durations gmem_rp_durations;
   /* Amount of duration samples for profiling before we start averaging. */
   static constexpr uint32_t MIN_PROFILE_DURATION_COUNT = 5;
   static constexpr double PROFILE_DURATION_RECENCY_BIAS = 0.99;

 public:
   size_t hash; /* The hash of the renderpass, just for debug output. */

   rp_history(size_t hash): hash(hash)
   {
   }

   /** Bandwidth Estimation Algorithm **/
   struct bandwidth_algo {
    private:
      /* The exponentially-weighted mean of samples passed. */
      std::atomic<double> mean_samples_passed = -1.0;

    public:
      void update(uint32_t samples)
      {
         /* 0 < bias < 1 (Higher = Stronger recency bias). */
         constexpr double recency_bias = 0.6;

         double mean_samples = mean_samples_passed.load(std::memory_order_relaxed);
         if (mean_samples < 0.0) {
            /* If this is the first sample, just set the mean to the sample. */
            mean_samples = (double) (samples);
         } else {
            /* Exponentially-weighted moving average of samples passed. */
            mean_samples = (mean_samples * (1.0 - recency_bias)) + (((double) samples) * recency_bias);
         }

         mean_samples_passed.store(mean_samples, std::memory_order_relaxed);
      }

      render_mode get_optimal_mode(rp_history &history,
                                   const struct tu_cmd_state *cb_state,
                                   const struct tu_render_pass *pass,
                                   const struct tu_framebuffer *framebuffer,
                                   const struct tu_render_pass_state *rp_state)
      {
         uint32_t pass_pixel_count = 0;
         if (cb_state->per_layer_render_area) {
            for (unsigned i = 0; i < cb_state->pass->num_views; i++) {
               const VkExtent2D &extent = cb_state->render_areas[i].extent;
               pass_pixel_count += extent.width * extent.height;
            }
         } else {
            const VkExtent2D &extent = cb_state->render_areas[0].extent;
            pass_pixel_count =
               extent.width * extent.height * MAX2(cb_state->pass->num_views, cb_state->framebuffer->layers);
         }

         uint64_t sysmem_bandwidth = (uint64_t) pass->sysmem_bandwidth_per_pixel * pass_pixel_count;
         uint64_t gmem_bandwidth = (uint64_t) pass->gmem_bandwidth_per_pixel * pass_pixel_count;

         uint64_t total_draw_call_bandwidth = 0;
         double mean_samples = mean_samples_passed.load(std::memory_order_relaxed);
         if (rp_state->drawcall_count && mean_samples > 0.0) {
            /* The total draw call bandwidth is estimated as the average samples (collected via tracking samples passed
             * within the CS) multiplied by the drawcall bandwidth per sample, divided by the amount of draw calls.
             *
             * This is a rough estimate of the bandwidth used by the draw calls in the renderpass for FB writes which
             * is used to determine whether to use SYSMEM or GMEM.
             */
            total_draw_call_bandwidth =
               (mean_samples * rp_state->drawcall_bandwidth_per_sample_sum) / rp_state->drawcall_count;
         }

         /* Drawcalls access the memory in SYSMEM rendering (ignoring CCU). */
         sysmem_bandwidth += total_draw_call_bandwidth;

         /* Drawcalls access GMEM in GMEM rendering, but we do not want to ignore them completely.  The state changes
          * between tiles also have an overhead.  The magic numbers of 11 and 10 are randomly chosen.
          */
         gmem_bandwidth = (gmem_bandwidth * 11 + total_draw_call_bandwidth) / 10;

         bool select_sysmem = sysmem_bandwidth <= gmem_bandwidth;
         render_mode mode = select_sysmem ? render_mode::SYSMEM : render_mode::GMEM;

         if (TU_AUTOTUNE_DEBUG_LOG_BANDWIDTH) {
            const VkExtent2D &extent = cb_state->render_areas[0].extent;
            const float drawcall_bandwidth_per_sample =
               (float) rp_state->drawcall_bandwidth_per_sample_sum / rp_state->drawcall_count;

            mesa_logi("autotune-bw %016" PRIx64 ":%" PRIu32 " selecting %s", history.hash, rp_state->drawcall_count,
                      render_mode_str(mode));
            mesa_logi("   mean_samples=%f, draw_bandwidth_per_sample=%.2f, total_draw_call_bandwidth=%" PRIu64,
                      mean_samples, drawcall_bandwidth_per_sample, total_draw_call_bandwidth);
            mesa_logi("   render_area=%" PRIu32 "x%" PRIu32 ", sysmem_bandwidth_per_pixel=%" PRIu32 ", gmem_bandwidth_per_pixel=%" PRIu32, extent.width,
                      extent.height, pass->sysmem_bandwidth_per_pixel, pass->gmem_bandwidth_per_pixel);
            mesa_logi("   sysmem_bandwidth=%" PRIu64 ", gmem_bandwidth=%" PRIu64, sysmem_bandwidth, gmem_bandwidth);
         }

         return mode;
      }
   } bandwidth;

   /** Profiled Algorithms **/
   struct profiled_algo {
    private:
      /* Range [0 (GMEM), 100 (SYSMEM)], where 50 means no preference. */
      std::atomic<uint32_t> sysmem_probability = 50;

    public:
      void update(bool immediate, uint64_t avg_gmem, uint64_t avg_sysmem, uint32_t sysmem_count, uint32_t gmem_count)
      {
         uint32_t sysmem_prob = sysmem_probability.load(std::memory_order_relaxed);
         if (immediate) {
            /* Try to immediately resolve the probability, this is useful for CI running a single trace of frames where
             * the probabilites aren't expected to change from run to run. This environment also gives us a best case
             * scenario for autotune performance, since we know the optimal decisions.
             */
            if (sysmem_count < 1) {
               sysmem_prob = 100;
            } else if (gmem_count < 1) {
               sysmem_prob = 0;
            } else {
               sysmem_prob = avg_gmem < avg_sysmem ? 0 : 100;
            }
         } else {
            if (sysmem_count < MIN_PROFILE_DURATION_COUNT || gmem_count < MIN_PROFILE_DURATION_COUNT) {
               /* Not enough data to make a decision, bias towards least used. */
               sysmem_prob = sysmem_count < gmem_count ? 75 : 25;
            } else {
               /* Adjust probability based on timing results. */
               const uint32_t delta = 5;
               if (avg_gmem < avg_sysmem && sysmem_prob > 5) {
                  sysmem_prob = MAX2(sysmem_prob - delta, 5);
               } else if (avg_sysmem < avg_gmem && sysmem_prob < 95) {
                  sysmem_prob = MIN2(sysmem_prob + delta, 95);
               }
            }
         }

         sysmem_probability.store(sysmem_prob, std::memory_order_relaxed);

         if (TU_AUTOTUNE_DEBUG_LOG_PROFILED) {
            mesa_logi("autotune-prof%s update: avg_gmem: %" PRIu64 " avg_sysmem: %" PRIu64 " = sysmem_probability: %" PRIu32,
                      immediate ? " imm" : "", avg_gmem, avg_sysmem, sysmem_prob);
         }
      }

    public:
      render_mode get_optimal_mode(rp_history &history)
      {
         uint32_t l_sysmem_probability = sysmem_probability.load(std::memory_order_relaxed);
         uint64_t seed[2] { 0x3bffb83978e24f88, 0x9238d5d56c71cd35 };
         bool select_sysmem = (rand_xorshift128plus(seed) % 100) < l_sysmem_probability;
         render_mode mode = select_sysmem ? render_mode::SYSMEM : render_mode::GMEM;

         if (TU_AUTOTUNE_DEBUG_LOG_PROFILED) {
            mesa_logi("autotune-prof %016" PRIx64 ": %" PRIu32 "%% sysmem chance, using %s", history.hash, l_sysmem_probability,
                      render_mode_str(mode));
         }

         return mode;
      }
   } profiled;

   /** Preemption Latency Optimization Mode **/
   struct preempt_optimize_mode {
    private:
      tu_at_durations gmem_tile_durations;

      /* If the renderpass has long draws which are at risk of causing high preemptible latency. */
      std::atomic<bool> latency_risk = false;
      /* The factor by which the tile size should be divided to reduce preemption latency. */
      std::atomic<uint32_t> tile_size_divisor = 1;

      /* The next timestamp to update the latency sensitivity parameters at. */
      uint64_t latency_update_ts = 0;
      /* The next timestamp where it's allowed to decrement the divisor. */
      uint64_t divisor_decrement_ts = 0;
      /* The next timestamp where it's allowed to mark the RP as no longer latency sensitive. */
      uint64_t latency_switch_ts = 0;

      static constexpr uint64_t TARGET_THRESHOLD = GPU_TICKS_PER_US * 500; /* 500us */

#if TU_AUTOTUNE_DEBUG_PERFCTR
      /* The highest preemption reaction delay recorded for the RP since the last update. */
      uint64_t max_preemption_latency = 0;

    public:
      void update_preemption_latency(uint64_t preemption_latency)
      {
         max_preemption_latency = MAX2(max_preemption_latency, preemption_latency);
      }
#endif

    public:
      void update_sysmem(rp_history &history)
      {
         bool l_latency_risk = latency_risk.load(std::memory_order_relaxed);

         if (!l_latency_risk) {
            /* Try to estimate the minimum non-preemptible duration for draw-level preemption, by dividing the total
             * time by the RP by the amount of draws. This isn't very accurate as it's skewed by the time taken by
             * commands other than draws (e.g. clears or blits), but it's a good enough estimate to catch the worst
             * offenders.
             *
             * If the average draw duration is above a certain threshold, we mark the RP as latency sensitive which
             * should bias the decision towards GMEM.
             */

            uint64_t avg_sysmem_draw;
            uint32_t sysmem_draw_count;
            history.sysmem_rp_durations.get_avg(avg_sysmem_draw, sysmem_draw_count, true, PROFILE_DURATION_RECENCY_BIAS);

            if (TU_AUTOTUNE_DEBUG_LOG_PREEMPT) {
               mesa_logi("autotune-preempt %016" PRIx64 ": avg_sysmem_draw: %" PRIu64 " us (%u), latency_risk: %u"
#if TU_AUTOTUNE_DEBUG_PERFCTR
                         ", preemption_latency: %" PRIu64
#endif
                         ,
                         history.hash, ticks_to_us(avg_sysmem_draw), avg_sysmem_draw > TARGET_THRESHOLD, l_latency_risk
#if TU_AUTOTUNE_DEBUG_PERFCTR
                         ,
                         max_preemption_latency
#endif
               );

#if TU_AUTOTUNE_DEBUG_PERFCTR
               max_preemption_latency = 0;
#endif
            }

            if (sysmem_draw_count >= MIN_PROFILE_DURATION_COUNT && avg_sysmem_draw > TARGET_THRESHOLD) {
               latency_risk.store(true, std::memory_order_relaxed);
               if (TU_AUTOTUNE_DEBUG_LOG_PREEMPT) {
                  mesa_logi("autotune-preempt %016" PRIx64 ": high sysmem draw duration %" PRIu64 " us, marking as latency sensitive",
                            history.hash, ticks_to_us(avg_sysmem_draw));
               }
            }
         }
      }

      void update_gmem(rp_history &history, uint64_t tile_duration)
      {
         constexpr uint64_t default_update_duration_ns = 100'000'000;         /* 100ms */
         constexpr uint64_t change_update_duration_ns = 500'000'000;          /* 500ms */
         constexpr uint64_t downward_update_duration_ns = 10'000'000'000;     /* 10s */
         constexpr uint64_t latency_insensitive_duration_ns = 30'000'000'000; /* 30s */

         gmem_tile_durations.add(tile_duration, 1);

         uint64_t now = os_time_get_nano();
         if (latency_update_ts > now)
            return; /* No need to update yet. */

         /* If the RP is latency sensitive and we're using GMEM, we should check if it's worth reducing the tile size to
          * reduce the latency risk further or if it's already low enough that it's not worth the performance hit.
          */

         uint64_t avg_gmem_tile; /* Average duration of the longest tile in the RP. */
         uint32_t entry_count;
         gmem_tile_durations.get_avg(avg_gmem_tile, entry_count, false);

         uint64_t update_duration_ns = default_update_duration_ns;
         if (entry_count > 1) {
            bool l_latency_risk = latency_risk.load(std::memory_order_relaxed);
            if (!l_latency_risk) {
               if (avg_gmem_tile > TARGET_THRESHOLD) {
                  latency_risk.store(true, std::memory_order_relaxed);
                  latency_switch_ts = now + latency_insensitive_duration_ns;

                  if (TU_AUTOTUNE_DEBUG_LOG_PREEMPT) {
                     mesa_logi("autotune-preempt %016" PRIx64 ": high gmem tile duration %" PRIu64 ", marking as latency sensitive",
                               history.hash, avg_gmem_tile);
                  }
               }
            } else {
               uint32_t l_tile_size_divisor = tile_size_divisor.load(std::memory_order_relaxed);
               if (TU_AUTOTUNE_DEBUG_LOG_PREEMPT) {
                  mesa_logi(
                     "autotune-preempt %016" PRIx64 ": avg_gmem_tile: %" PRIu64 " us (%u), latency_risk: %u, tile_size_divisor: %" PRIu32
#if TU_AUTOTUNE_DEBUG_PERFCTR
                     ", preemption_latency: %" PRIu64
#endif
                     ,
                     history.hash, ticks_to_us(avg_gmem_tile), avg_gmem_tile > TARGET_THRESHOLD, l_latency_risk,
                     l_tile_size_divisor
#if TU_AUTOTUNE_DEBUG_PERFCTR
                     ,
                     max_preemption_latency
#endif
                  );

#if TU_AUTOTUNE_DEBUG_PERFCTR
                  max_preemption_latency = 0;
#endif
               }

               int delta = 0;
               if (avg_gmem_tile > TARGET_THRESHOLD && l_tile_size_divisor < TU_GMEM_LAYOUT_DIVISOR_MAX) {
                  /* If the average tile duration is high, we should reduce the tile size to reduce the latency risk. */
                  delta = 1;

                  divisor_decrement_ts = now + downward_update_duration_ns;
               } else if (avg_gmem_tile * 4 < TARGET_THRESHOLD && l_tile_size_divisor > 1 &&
                          divisor_decrement_ts <= now) {
                  /* If the average tile duration is low enough that we can get away with a larger tile size, we should
                   * increase the tile size to reduce the performance hit of the smaller tiles.
                   *
                   * Note: The 4x factor is to account for the tile duration being halved when we increase the tile size
                   * divisor by 1, with an additional 2x factor to generally be conservative about reducing the divisor
                   * since it can lead to oscillation between tile sizes.
                   *
                   * Similarly, divisor_decrement_ts is used to limit how often we can reduce the divisor to avoid
                   * oscillation.
                   */
                  delta = -1;
                  latency_switch_ts = now + latency_insensitive_duration_ns;
               } else if (avg_gmem_tile * 10 < TARGET_THRESHOLD && l_tile_size_divisor == 1 &&
                          latency_switch_ts <= now) {
                  /* If the average tile duration is low enough that we no longer consider the RP latency sensitive, we
                   * can switch it back to non-latency sensitive.
                   */
                  latency_risk.store(false, std::memory_order_relaxed);
               }

               if (delta != 0) {
                  /* Clear all the results to avoid biasing the decision based on the old tile size. */
                  gmem_tile_durations.clear();

                  uint32_t new_tile_size_divisor = l_tile_size_divisor + delta;
                  if (TU_AUTOTUNE_DEBUG_LOG_PREEMPT) {
                     mesa_logi("autotune-preempt %016" PRIx64 ": updating tile size divisor: %" PRIu32 " -> %" PRIu32, history.hash,
                               l_tile_size_divisor, new_tile_size_divisor);
                  }

                  tile_size_divisor.store(new_tile_size_divisor, std::memory_order_relaxed);

                  update_duration_ns = change_update_duration_ns;
               }
            }

            latency_update_ts = now + update_duration_ns;
         }
      }

      /* If this RP has a risk of causing high preemption latency. */
      bool is_latency_sensitive() const
      {
         return latency_risk.load(std::memory_order_relaxed);
      }

      uint32_t get_tile_size_divisor() const
      {
         return tile_size_divisor.load(std::memory_order_relaxed);
      }
   } preempt_optimize;

   uint32_t sysmem_count = 0, gmem_count = 0;
   uint64_t avg_sysmem = 0, avg_gmem = 0;

   void process(rp_entry &entry, tu_autotune &at)
   {
      /* We use entry config to know what metrics it has, autotune config to know what algorithms are enabled. */
      config_t entry_config = entry.config, at_config = at.active_config.load();

      if (entry_config.test(metric_flag::SAMPLES) && at_config.is_enabled(algorithm::BANDWIDTH))
         bandwidth.update(entry.get_samples_passed());

#if TU_AUTOTUNE_DEBUG_PERFCTR
      preempt_optimize.update_preemption_latency(entry.get_preemption_reaction_delay(at, hash));
#endif

      if (entry_config.test(metric_flag::TS)) {
         if (entry.sysmem) {
            sysmem_rp_durations.add(entry.get_rp_duration(), entry.draw_count);
            sysmem_rp_durations.get_avg(avg_sysmem, sysmem_count, false, PROFILE_DURATION_RECENCY_BIAS);

            if (at_config.test(mod_flag::PREEMPT_OPTIMIZE))
               preempt_optimize.update_sysmem(*this);
         } else {
            gmem_rp_durations.add(entry.get_rp_duration(), 1);
            gmem_rp_durations.get_avg(avg_gmem, gmem_count, false, PROFILE_DURATION_RECENCY_BIAS);

            if (entry_config.test(metric_flag::TS_TILE) && at_config.test(mod_flag::PREEMPT_OPTIMIZE))
               preempt_optimize.update_gmem(*this, entry.get_max_tile_duration());
         }

         if (at_config.is_enabled(algorithm::PROFILED) || at_config.is_enabled(algorithm::PROFILED_IMM)) {
            profiled.update(at_config.is_enabled(algorithm::PROFILED_IMM), avg_gmem, avg_sysmem, sysmem_count,
                            gmem_count);
         }
      }
   }
};

tu_autotune::rp_history *
tu_autotune::find_rp_history(const rp_key &key)
{
   std::shared_lock lock(rp_mutex);
   auto it = rp_histories.find(key);
   if (it != rp_histories.end())
      return &it->second;

   return nullptr;
}

tu_autotune::rp_history &
tu_autotune::find_or_create_rp_history(const rp_key &key)
{
   auto existing = find_rp_history(key);
   if (existing)
      return *existing;

   /* If we reach here, we have to create a new history. */
   std::unique_lock lock(rp_mutex);
   auto it = rp_histories.find(key);
   if (it != rp_histories.end())
      return it->second; /* Another thread created the history while we were waiting for the lock. */
   auto history = rp_histories.emplace(std::make_pair(key, key.hash));
   return history.first->second;
}

void
tu_autotune::process_entries()
{
   uint32_t current_fence = device->global_bo_map->autotune_fence;

   while (!active_batches.empty()) {
      auto &batch = active_batches.front();
      assert(batch->active);

      if (fence_before(current_fence, batch->fence))
         break; /* Entries are allocated in sequence, next will be newer and
                   also fail so we can just directly break out of the loop. */

      for (rp_entry &entry : batch->entries)
         entry.history->process(entry, *this);

      active_batches.pop_front();
   }

   if (TU_AUTOTUNE_DEBUG_LOG_BASE && active_batches.size() > 10) {
      mesa_logw("autotune: high amount of active batches: %zu, fence: %" PRIu32 " < %" PRIu32, active_batches.size(), current_fence,
                active_batches.front()->fence);
   }
}

struct tu_cs *
tu_autotune::on_submit(struct tu_cmd_buffer **cmd_buffers, uint32_t cmd_buffer_count)
{

   /* This call occurs regularly and we are single-threaded here, so we use this opportunity to process any available
    * entries. It's also important that any entries are processed here because we always want to ensure that we've
    * processed all entries from prior CBs before we submit any new CBs with the same RP to the GPU.
    */
   process_entries();

   bool has_results = [&]() {
      for (uint32_t i = 0; i < cmd_buffer_count; i++) {
         auto &batch = cmd_buffers[i]->autotune_ctx.batch;
         if (!batch->entries.empty())
            return true;
      }
      return false;
   }();
   if (!has_results)
      return nullptr; /* No results to process, return early. */

   /* Generate a new fence and the CS for it. */
   const uint32_t new_fence = next_fence++;
   auto fence_cs = get_cs_for_fence(new_fence);
   for (uint32_t i = 0; i < cmd_buffer_count; i++) {
      /* Transfer the entries from the command buffers to the active queue. */
      auto &cmdbuf = cmd_buffers[i];
      auto &batch = cmdbuf->autotune_ctx.batch;
      if (batch->entries.empty())
         continue;

      batch->assign_fence(new_fence);
      if (cmdbuf->usage_flags & VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT) {
         /* If the command buffer is one-time submit, we can move the batch directly into the active batches, as it
          * won't be used again. This would lead to it being deallocated as early as possible.
          */
         active_batches.push_back(std::move(batch));
      } else {
         active_batches.push_back(batch);
      }
   }

   return fence_cs;
}

tu_autotune::tu_autotune(struct tu_device *device, VkResult &result): device(device), active_config(get_env_config())
{
   tu_bo_suballocator_init(&suballoc, device, 128 * 1024, TU_BO_ALLOC_INTERNAL_RESOURCE, "autotune_suballoc");

#if TU_AUTOTUNE_DEBUG_PERFCTR
   uint32_t group_count;
   const struct fd_perfcntr_group *groups = fd_perfcntrs(&device->physical_device->dev_id, &group_count);

   for (uint32_t i = 0; i < group_count; i++) {
      if (strcmp(groups[i].name, "CP") == 0) {
         cp_group = &groups[i];
         break;
      }
   }

   if (!cp_group) {
      mesa_loge("autotune: CP group not found");
      result = VK_ERROR_INITIALIZATION_FAILED;
      return;
   } else if (cp_group->num_countables < 5) {
      mesa_loge("autotune: CP group has too few countables");
      result = VK_ERROR_INITIALIZATION_FAILED;
      return;
   }

   auto get_perfcntr_countable = [](const struct fd_perfcntr_group *group,
                                    const char *name) -> const struct fd_perfcntr_countable * {
      for (uint32_t i = 0; i < group->num_countables; i++) {
         if (strcmp(group->countables[i].name, name) == 0)
            return &group->countables[i];
      }

      mesa_loge("autotune: %s not found in group %s", name, group->name);
      return nullptr;
   };

   preemption_reaction_delay = get_perfcntr_countable(cp_group, "PERF_CP_PREEMPTION_REACTION_DELAY");
   num_preemptions = get_perfcntr_countable(cp_group, "PERF_CP_NUM_PREEMPTIONS");
   always_count = get_perfcntr_countable(cp_group, "PERF_CP_ALWAYS_COUNT");

   if (!preemption_reaction_delay || !num_preemptions || !always_count) {
      mesa_loge("autotune: preemption countables not found");
      result = VK_ERROR_INITIALIZATION_FAILED;
      return;
   }
#endif

   result = VK_SUCCESS;
   return;
}

tu_autotune::~tu_autotune()
{
   if (TU_AUTOTUNE_LOG_AT_FINISH) {
      while (!active_batches.empty())
         process_entries();
      mesa_logi("autotune: finished processing all entries");
   }

   tu_bo_suballocator_finish(&suballoc);
}

tu_autotune::cmd_buf_ctx::cmd_buf_ctx(): batch(std::make_shared<rp_entry_batch>())
{
}

tu_autotune::cmd_buf_ctx::~cmd_buf_ctx()
{
   /* This is empty but it causes the implicit destructor to be compiled within this compilation unit with access to
    * internal structures. Otherwise, we would need to expose the full definition of autotuner internals in the header
    * file, which is not desirable.
    */
}

void
tu_autotune::cmd_buf_ctx::reset()
{
   batch = std::make_shared<rp_entry_batch>();
}

tu_autotune::rp_entry &
tu_autotune::cmd_buf_ctx::attach_rp_entry(struct tu_device *device,
                                          rp_history &history,
                                          config_t config,
                                          uint32_t drawcall_count)
{
   return batch->entries.emplace_back(device, history, config, drawcall_count);
}

tu_autotune::render_mode
tu_autotune::get_optimal_mode(struct tu_cmd_buffer *cmd_buffer, rp_ctx_t *rp_ctx)
{
   const struct tu_cmd_state *cb_state = &cmd_buffer->state;
   const struct tu_render_pass *pass = cb_state->pass;
   const struct tu_framebuffer *framebuffer = cb_state->framebuffer;
   const struct tu_render_pass_state *rp_state = &cb_state->rp;
   auto &cb_ctx = cmd_buffer->autotune_ctx;
   config_t config = active_config.load();

   /* Just to ensure a segfault for accesses, in case we don't set it. */
   *rp_ctx = nullptr;

   /* If a feedback loop in the subpass caused one of the pipelines used to set
    * SINGLE_PRIM_MODE(FLUSH_PER_OVERLAP_AND_OVERWRITE) or even SINGLE_PRIM_MODE(FLUSH), then that should cause
    * significantly increased SYSMEM bandwidth (though we haven't quantified it).
    */
   if (rp_state->sysmem_single_prim_mode)
      return render_mode::GMEM;

   /* If the user is using a fragment density map, then this will cause less FS invocations with GMEM, which has a
    * hard-to-measure impact on performance because it depends on how heavy the FS is in addition to how many
    * invocations there were and the density. Let's assume the user knows what they're doing when they added the map,
    * because if SYSMEM is actually faster then they could've just not used the fragment density map.
    */
   if (pass->has_fdm)
      return render_mode::GMEM;

   /* SYSMEM is always a safe default mode when we can't fully engage the autotuner. From testing, we know that for an
    * incorrect decision towards SYSMEM tends to be far less impactful than an incorrect decision towards GMEM, which
    * can cause significant performance issues.
    */
   constexpr render_mode default_mode = render_mode::SYSMEM;

   /* For VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT buffers, we would have to allocate GPU memory at the submit time
    * and copy results into it. We just disable complex autotuner in this case, which isn't a big issue since native
    * games usually don't use it, Zink and DXVK don't use it, while D3D12 doesn't even have such concept.
    *
    * We combine this with processing entries at submit time, to avoid a race where the CPU hasn't processed the results
    * from an earlier submission of the CB while a second submission of the CB is on the GPU queue.
    */
   bool simultaneous_use = cmd_buffer->usage_flags & VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;

   if (!enabled || simultaneous_use)
      return default_mode;

   /* We can return early with the decision based on the draw call count, instead of needing to hash the renderpass
    * instance and look up the history, which is far more expensive.
    *
    * However, certain options such as latency sensitive mode take precedence over any of the other autotuner options
    * and we cannot do so in those cases.
    */
   bool can_early_return = !config.test(mod_flag::PREEMPT_OPTIMIZE);
   auto early_return_mode = [&]() -> std::optional<render_mode> {
      if (config.test(mod_flag::BIG_GMEM) && rp_state->drawcall_count >= 10)
         return render_mode::GMEM;
      if (config.test(mod_flag::SMALL_SYSMEM) && rp_state->drawcall_count <= 5)
         return render_mode::SYSMEM;
      return std::nullopt;
   }();

   if (can_early_return && early_return_mode) {
      if (TU_AUTOTUNE_DEBUG_LOG_BASE) {
         mesa_logi("autotune %016" PRIx64 ": %" PRIu32 " draw calls, using %s (early)",
                   rp_key(pass, framebuffer, cmd_buffer).hash, rp_state->drawcall_count,
                   render_mode_str(*early_return_mode));
      }
      return *early_return_mode;
   }

   rp_key key(pass, framebuffer, cmd_buffer);
   auto &history = find_or_create_rp_history(key);
   *rp_ctx = &cb_ctx.attach_rp_entry(device, history, config, rp_state->drawcall_count);

   if (config.test(mod_flag::PREEMPT_OPTIMIZE) && history.preempt_optimize.is_latency_sensitive()) {
      /* Try to mitigate the risk of high preemption latency by always using GMEM, which should break up any larger
       * draws into smaller ones with tiling.
       */
      if (TU_AUTOTUNE_DEBUG_LOG_BASE)
         mesa_logi("autotune %016" PRIx64 ": high preemption latency risk, using GMEM", key.hash);
      return render_mode::GMEM;
   }

   if (early_return_mode) {
      if (TU_AUTOTUNE_DEBUG_LOG_BASE) {
         mesa_logi("autotune %016" PRIx64 ": %" PRIu32 " draw calls, using %s (late)", key.hash, rp_state->drawcall_count,
                   render_mode_str(*early_return_mode));
      }
      return *early_return_mode;
   }

   if (config.is_enabled(algorithm::PROFILED) || config.is_enabled(algorithm::PROFILED_IMM))
      return history.profiled.get_optimal_mode(history);

   if (config.is_enabled(algorithm::BANDWIDTH))
      return history.bandwidth.get_optimal_mode(history, cb_state, pass, framebuffer, rp_state);

   return default_mode;
}

uint32_t
tu_autotune::get_tile_size_divisor(struct tu_cmd_buffer *cmd_buffer)
{
   const struct tu_render_pass *pass = cmd_buffer->state.pass;
   const struct tu_framebuffer *framebuffer = cmd_buffer->state.framebuffer;

   if (!enabled || !active_config.load().test(mod_flag::PREEMPT_OPTIMIZE) || pass->has_fdm)
      return 1;

   rp_key key(pass, framebuffer, cmd_buffer);
   rp_history *history = find_rp_history(key);
   if (!history) {
      if (TU_AUTOTUNE_DEBUG_LOG_BASE)
         mesa_logi("autotune %016" PRIx64 ": no RP history found, using tile_size_divisor=1", key.hash);
      return 1;
   }

   uint32_t tile_size_divisor = history->preempt_optimize.get_tile_size_divisor();

   return tile_size_divisor;
}

void
tu_autotune::disable_preempt_optimize()
{
   config_t original, updated;
   do {
      original = updated = active_config.load();
      if (!original.test(mod_flag::PREEMPT_OPTIMIZE))
         return; /* Already disabled, nothing to do. */
      updated.disable(mod_flag::PREEMPT_OPTIMIZE);
   } while (!active_config.compare_and_store(original, updated));
}

/** RP-level CS emissions **/

void
tu_autotune::begin_renderpass(
   struct tu_cmd_buffer *cmd, struct tu_cs *cs, rp_ctx_t rp_ctx, bool sysmem, uint32_t tile_count)
{
   if (!rp_ctx)
      return;

   assert(sysmem || tile_count > 0);
   assert(!sysmem || tile_count == 0);

   rp_ctx->allocate(sysmem, tile_count);
   rp_ctx->emit_rp_start(cmd, cs);
}

void
tu_autotune::end_renderpass(struct tu_cmd_buffer *cmd, struct tu_cs *cs, rp_ctx_t rp_ctx)
{
   if (!rp_ctx)
      return;

   rp_ctx->emit_rp_end(cmd, cs);
}

/** Tile-level CS emissions **/

void
tu_autotune::begin_tile(struct tu_cmd_buffer *cmd, struct tu_cs *cs, rp_ctx_t rp_ctx, uint32_t tile_idx)
{
   if (!rp_ctx)
      return;

   rp_ctx->emit_tile_start(cmd, cs, tile_idx);
}

void
tu_autotune::end_tile(struct tu_cmd_buffer *cmd, struct tu_cs *cs, rp_ctx_t rp_ctx, uint32_t tile_idx)
{
   if (!rp_ctx)
      return;

   rp_ctx->emit_tile_end(cmd, cs, tile_idx);
}
