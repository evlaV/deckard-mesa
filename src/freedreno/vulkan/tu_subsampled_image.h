/*
 * Copyright © 2026 Valve Corporation.
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "tu_common.h"

/* Describe the format used for subsampled image metadata. This is attached to
 * subsampled images, via a separate UBO descriptor after the image
 * descriptor. It is written after the render pass which writes to the image,
 * and is read via code injected into the shader when sampling from a
 * subsampled image.
 */

/* The maximum number of bins a subsampled image can have before we disable
 * subsampling.
 */
#define TU_SUBSAMPLED_MAX_BINS 512

/* The maximum number of layers a view of a subsampled image can have.
 *
 * There is one metadata structure per layer, and the view uses a UBO for the
 * metadata, so this is bounded by the maximum UBO size.
 *
 * TODO: When we implement fdm2, we should expose this as
 * maxSubsampledArrayLayers. The Vulkan spec says that the minimum value for
 * maxSubsampledArrayLayers is 2, so users can only rely on 2 layers even
 * though we support more.
 */
#define TU_SUBSAMPLED_MAX_LAYERS 6

/* This is 2 to allow for floating-point precision errors and in case the user
 * uses bicubic filtering.
 */
#define APRON_SIZE 2

struct tu_subsampled_bin {
   float scale_x;
   float scale_y;
   float offset_x;
   float offset_y;
};

/* We can reuse the subsampled metadata header and layout for query metadata,
 * but it's for querying we need to just store the frag size directly as an
 * integer. The scale is derived from the frag size but is not an exact
 * inverse for various reasons.
 */
struct tu_query_bin {
   uint32_t frag_size_x;
   uint32_t frag_size_y;
   uint32_t pad0, pad1;
};

struct tu_subsampled_header {
   /* The bin coordinate to use is calculated as:
    * bin = int(coord * scale + offset)
    */
   float scale_x;
   float scale_y;
   float offset_x;
   float offset_y;

   uint32_t bin_stride;
   uint32_t pad0[3];
};

struct tu_subsampled_metadata {
   struct tu_subsampled_header hdr;

   struct tu_subsampled_bin bins[TU_SUBSAMPLED_MAX_BINS];
};

struct tu_query_metadata {
   struct tu_subsampled_header hdr;

   struct tu_query_bin bins[TU_SUBSAMPLED_MAX_BINS];
};

void
tu_emit_subsampled_metadata(struct tu_cmd_buffer *cmd,
                            struct tu_cs *cs,
                            unsigned a,
                            const struct tu_tile_config *tiles,
                            const struct tu_tiling_config *tiling,
                            const struct tu_vsc_config *vsc,
                            const struct tu_framebuffer *fb,
                            const VkOffset2D *fdm_offsets);

void
tu_emit_query_metadata(struct tu_cmd_buffer *cmd,
                       struct tu_cs *cs,
                       unsigned a,
                       const struct tu_tile_config *tiles,
                       const struct tu_tiling_config *tiling,
                       const struct tu_vsc_config *vsc,
                       const struct tu_framebuffer *fb,
                       const VkOffset2D *fdm_offsets);

unsigned
tu_calc_subsampled_aprons(VkRect2D *dst,
                          struct tu_rect2d_float *src,
                          unsigned view,
                          const struct tu_tile_config *tiles,
                          const struct tu_tiling_config *tiling,
                          const struct tu_vsc_config *vsc,
                          const struct tu_framebuffer *fb,
                          const VkOffset2D *fdm_offsets);

#define tu_get_subsampled_coordinates(...) MESA_CALL_WITH_LTR_ARGS(_tu_get_subsampled_coordinates, __VA_ARGS__)
nir_def *
_tu_get_subsampled_coordinates(nir_builder *b,
                              nir_def *coords,
                              nir_def *descriptor,
                              bool can_speculate);

#define tu_get_frag_size(...) MESA_CALL_WITH_LTR_ARGS(_tu_get_frag_size, __VA_ARGS__)
nir_def *
_tu_get_frag_size(nir_builder *b,
                  nir_def *coords,
                  nir_def *descriptor,
                  bool can_speculate);
