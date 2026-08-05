/*
 * Copyright © 2025 Igalia S.L.
 * SPDX-License-Identifier: MIT
 */

#ifndef FREEDRENO_LRZ_LAYOUT_H_
#define FREEDRENO_LRZ_LAYOUT_H_

#include <stdint.h>

#include "freedreno_layout.h"

BEGINC;

struct fdl_lrz_layout {
   uint32_t lrz_offset;
   uint32_t lrz_pitch;
   uint32_t lrz_slice_pitch;   /* gen8+ */
   uint32_t lrz_height;
   uint32_t lrz_layer_size;
   uint32_t lrz_buffer_size;
   uint32_t lrz_fc_offset;
   uint32_t lrz_fc_size;
   /* What the fast-clear buffer would need to cover the whole LRZ image. When
    * this exceeds the flag RAM, lrz_fc_size is the covered prefix and the
    * blocks past it have to be cleared by other means.
    */
   uint32_t lrz_fc_full_size;
   /* Per-layer fast-clear stride, and the block grid one layer covers. Needed
    * to work out which LRZ rows a partially-covering flag RAM leaves behind.
    */
   uint32_t lrz_fc_layer_stride;
   uint32_t lrz_fc_blocks_x;
   uint32_t lrz_fc_blocks_y;
   uint32_t lrz_total_size;
};

/* First LRZ row of `layer` that the fast-clear buffer does not cover, or
 * lrz_height if the layer is covered completely.
 *
 * The flag buffer is addressed as a flat byte range and the hardware only reads
 * the first lrz_fc_size bytes of it, with each layer's bits starting at a
 * 64-byte aligned stride. One bit covers a 16x4 block of LRZ rows, so a covered
 * bit count converts to a covered row count through blocks_x.
 */
static inline uint32_t
fdl6_lrz_fc_first_uncovered_row(const struct fdl_lrz_layout *lrz, uint32_t layer)
{
   uint32_t rows_per_block = 4;
   uint32_t layer_start = layer * lrz->lrz_fc_layer_stride;

   if (layer_start >= lrz->lrz_fc_size)
      return 0; /* nothing of this layer is covered */

   uint32_t covered_bytes = lrz->lrz_fc_size - layer_start;
   uint32_t layer_bytes = DIV_ROUND_UP(
      lrz->lrz_fc_blocks_x * lrz->lrz_fc_blocks_y, 8);

   if (covered_bytes >= layer_bytes)
      return lrz->lrz_height; /* fully covered */

   /* Flag bits are not row-major over blocks: they are tiled in groups of 8
    * block columns by 4 block rows. Measured by clearing from one block row
    * past the boundary, which left exactly blocks_x blocks over-culling but
    * shaped 8 wide and 4 tall rather than blocks_x wide and 1 tall.
    *
    * So round the covered block rows down to a multiple of 4: a partially
    * covered tile row has to be cleared the slow way. The bracketing test cases
    * all happen to land on a multiple of 4 already, but e.g. HL:A's stereo depth
    * covers 99.55 block rows and does not.
    */
   uint32_t covered_rows =
      (covered_bytes * 8) / MAX2(lrz->lrz_fc_blocks_x, 1);
   covered_rows &= ~(uint32_t) 3;

   return MIN2(covered_rows * rows_per_block, lrz->lrz_height);
}

void
fdl5_lrz_layout_init(struct fdl_lrz_layout *lrz_layout, uint32_t width,
                     uint32_t height, uint32_t nr_samples);
ENDC;

#ifdef __cplusplus
#include "common/freedreno_lrz.h"

static inline void
fdl6_lrz_get_super_sampled_size(uint32_t *width, uint32_t *height,
                                uint32_t nr_samples)
{
   switch (nr_samples) {
   case 8:
      *height *= 2;
      FALLTHROUGH;
   case 4:
      *width *= 2;
      FALLTHROUGH;
   case 2:
      *height *= 2;
      break;
   default:
      break;
   }
}

/* Size the fast-clear buffer would need to cover the whole LRZ image, before
 * the flag-RAM limit is applied.
 */
static inline uint32_t
fdl6_lrz_get_full_fc_size(uint32_t width, uint32_t height, uint32_t nr_samples,
                          uint32_t array_layers)
{
   fdl6_lrz_get_super_sampled_size(&width, &height, nr_samples);

   unsigned nblocksx = DIV_ROUND_UP(DIV_ROUND_UP(width, 8), 16);
   unsigned nblocksy = DIV_ROUND_UP(DIV_ROUND_UP(height, 8), 4);

   return DIV_ROUND_UP(nblocksx * nblocksy, 8) * array_layers;
}

template <chip CHIP>
static uint32_t
fdl6_lrz_get_fc_size(uint32_t width, uint32_t height, uint32_t nr_samples,
                     uint32_t array_layers)
{
   uint32_t lrz_fc_size =
      fdl6_lrz_get_full_fc_size(width, height, nr_samples, array_layers);

   /* The flag RAM holds at most 512 bytes on A6XX, 1024 on A7XX and 2048 on
    * A8XX. Past that only a prefix of the LRZ image can be fast-cleared; the
    * caller clears the rest (see fdl6_lrz_fc_first_uncovered_row).
    */
   lrz_fc_size = MIN2(lrz_fc_size, (uint32_t) fd_lrzfc_layout<CHIP>::FC_SIZE);

   return lrz_fc_size;
}

/* Whether the flag RAM covers the whole LRZ image, i.e. no remainder has to be
 * cleared separately.
 */
template <chip CHIP>
static bool
fdl6_lrz_fc_fully_covered(uint32_t width, uint32_t height, uint32_t nr_samples,
                          uint32_t array_layers)
{
   return fdl6_lrz_get_full_fc_size(width, height, nr_samples, array_layers) <=
          fd_lrzfc_layout<CHIP>::FC_SIZE;
}

struct fdl_lrz_fdm_extra_size {
   uint32_t extra_width;
   uint32_t extra_height;
};

/* Get maximum size of the extra tile for VK_QCOM_fragment_density_map_offset,
 * that keeps LRZ fast-clear enabled, if possible.
 */
template <chip CHIP>
static struct fdl_lrz_fdm_extra_size
fdl6_lrz_get_max_fdm_extra_size(const struct fd_dev_info *dev_info,
                                uint32_t width, uint32_t height,
                                uint32_t nr_samples, uint32_t array_layers)
{
   constexpr uint32_t MIN_TILE_SIZE_FOR_FDM_OFFSET = 192;

   if (!fdl6_lrz_fc_fully_covered<CHIP>(width, height, nr_samples,
                                        array_layers)) {
      return {dev_info->tile_max_w, dev_info->tile_max_h};
   }

   uint32_t max_extra_size = MIN2(dev_info->tile_max_w, dev_info->tile_max_h);
   uint32_t step = MIN2(dev_info->gmem_align_w, dev_info->gmem_align_h);
   uint32_t min_extra_size = MAX2(step, MIN_TILE_SIZE_FOR_FDM_OFFSET);

   while (max_extra_size > min_extra_size) {
      if (fdl6_lrz_fc_fully_covered<CHIP>(width + max_extra_size,
                                          height + max_extra_size, nr_samples,
                                          array_layers)) {

         return {util_round_down_npot(max_extra_size, dev_info->gmem_align_w),
                 util_round_down_npot(max_extra_size, dev_info->gmem_align_h)};
      }

      max_extra_size -= step;
   }

   return {dev_info->tile_max_w, dev_info->tile_max_h};
}

template <chip CHIP>
static void
fdl6_lrz_layout_init(struct fdl_lrz_layout *lrz_layout,
                     struct fdl_layout *layout, uint32_t extra_width,
                     uint32_t extra_height,
                     const struct fd_dev_info *dev_info, uint32_t lrz_offset,
                     uint32_t array_layers)
{
   if (CHIP >= A8XX) {
      static const struct lrz_block {
         uint16_t width;
         uint16_t height;
      } lrz_block_sizes[4][4] = {
         [0] = {
            [MSAA_ONE]   = {  64, 128 },
            [MSAA_TWO]   = {  64,  64 },
            [MSAA_FOUR]  = {  32,  64 },
            [MSAA_EIGHT] = {  32,  32 },
         },
         [1] = {
            [MSAA_ONE]   = { 128, 128 },
            [MSAA_TWO]   = { 128,  64 },
            [MSAA_FOUR]  = {  64,  64 },
            [MSAA_EIGHT] = {  64,  32 },
         },
         [2] = {
            [MSAA_ONE]   = { 192, 128 },
            [MSAA_TWO]   = { 192,  64 },
            [MSAA_FOUR]  = {  96,  64 },
            [MSAA_EIGHT] = {  96,  32 },
         },
         [3] = {
            [MSAA_ONE]   = { 128, 256 },
            [MSAA_TWO]   = { 128, 128 },
            [MSAA_FOUR]  = {  64, 128 },
            [MSAA_EIGHT] = {  64,  64 },
         },
      };
      const struct lrz_block *lrz_block = &lrz_block_sizes[dev_info->num_slices - 1][ffs(layout->nr_samples) - 1];

      const unsigned per_slice_block_width_in_tiles = 8;
      const unsigned per_slice_block_height_in_tiles = 16;

      const unsigned surface_width_in_blocks =
         DIV_ROUND_UP(layout->width0 + extra_width, lrz_block->width);
      const unsigned surface_height_in_blocks =
         DIV_ROUND_UP(layout->height0 + extra_height, lrz_block->height);

      lrz_layout->lrz_pitch =
         align(surface_width_in_blocks * per_slice_block_width_in_tiles, 64);

      /* Construct a "fake" height to use for fallback lrz clear on
       * the blitter.  Since we use lrz_pitch as the width, this is
       * just lrz_layer_size / lrz_pitch
       */
      lrz_layout->lrz_height = dev_info->num_slices *
         surface_height_in_blocks * per_slice_block_height_in_tiles;

      lrz_layout->lrz_slice_pitch = surface_height_in_blocks *
         per_slice_block_height_in_tiles * lrz_layout->lrz_pitch * 2;

      lrz_layout->lrz_layer_size = lrz_layout->lrz_slice_pitch * dev_info->num_slices;
   } else {
      unsigned width = layout->width0 + extra_width;
      unsigned height = layout->height0 + extra_height;
      fdl6_lrz_get_super_sampled_size(&width, &height, layout->nr_samples);

      lrz_layout->lrz_pitch = align(DIV_ROUND_UP(width, 8), 32);
      lrz_layout->lrz_height = align(DIV_ROUND_UP(height, 8), 32);

      lrz_layout->lrz_layer_size =
         lrz_layout->lrz_pitch * lrz_layout->lrz_height * sizeof(uint16_t);
   }

   lrz_layout->lrz_offset = lrz_offset;
   lrz_layout->lrz_buffer_size = lrz_layout->lrz_layer_size * array_layers;

   /* Fast-clear buffer is 1bit/block */
   lrz_layout->lrz_fc_size = fdl6_lrz_get_fc_size<CHIP>(
      layout->width0 + extra_width, layout->height0 + extra_height,
      layout->nr_samples, array_layers);
   lrz_layout->lrz_fc_full_size = fdl6_lrz_get_full_fc_size(
      layout->width0 + extra_width, layout->height0 + extra_height,
      layout->nr_samples, array_layers);

   {
      uint32_t fc_w = layout->width0 + extra_width;
      uint32_t fc_h = layout->height0 + extra_height;
      fdl6_lrz_get_super_sampled_size(&fc_w, &fc_h, layout->nr_samples);
      lrz_layout->lrz_fc_blocks_x = DIV_ROUND_UP(DIV_ROUND_UP(fc_w, 8), 16);
      lrz_layout->lrz_fc_blocks_y = DIV_ROUND_UP(DIV_ROUND_UP(fc_h, 8), 4);
      lrz_layout->lrz_fc_layer_stride =
         align(DIV_ROUND_UP(lrz_layout->lrz_fc_blocks_x *
                            lrz_layout->lrz_fc_blocks_y, 8), 64);
   }

   if (!dev_info->props.enable_lrz_fast_clear) {
      lrz_layout->lrz_fc_size = 0;
   }

   /* Allocate 2 LRZ buffers for double-buffering on a7xx. */
   uint32_t lrz_size = lrz_layout->lrz_buffer_size *
      (CHIP >= A7XX ? 2 : 1);

   if (dev_info->props.enable_lrz_fast_clear ||
       dev_info->props.has_lrz_dir_tracking) {
      lrz_layout->lrz_fc_offset =
         lrz_layout->lrz_offset + lrz_size;
      lrz_size += sizeof(fd_lrzfc_layout<CHIP>);
   }

   lrz_layout->lrz_total_size = lrz_size;

   uint32_t lrz_clear_height = lrz_layout->lrz_height * array_layers;
   if (((lrz_clear_height - 1) >> 14) > 0) {
      /* For simplicity bail out if LRZ cannot be cleared in one go. */
      lrz_layout->lrz_height = 0;
      lrz_layout->lrz_total_size = 0;
   }
}
#endif

#endif
