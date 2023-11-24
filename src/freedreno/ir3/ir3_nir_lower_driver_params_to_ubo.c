/*
 * Copyright © 2023 Igalia S.L.
 * SPDX-License-Identifier: MIT
 */

#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "util/u_math.h"
#include "ir3_compiler.h"
#include "ir3_nir.h"

static bool
lower_driver_param_to_ubo(nir_builder *b, nir_intrinsic_instr *intr, void *in)
{
   struct ir3_const_state *const_state = in;

   struct dirver_param_info param_info;
   if (!ir3_get_driver_param_info(b->shader, intr, &param_info))
      return false;

   if (b->shader->info.stage == MESA_SHADER_VERTEX &&
       param_info.type != IR3_UBO_IDX_DYNAMIC_OFFSETS)
      return false;

   if (param_info.type != IR3_UBO_IDX_DRIVER_PARAMS &&
       param_info.type != IR3_UBO_IDX_PRIMITIVE_MAP &&
       param_info.type != IR3_UBO_IDX_PRIMITIVE_PARAM &&
       param_info.type != IR3_UBO_IDX_FRAG_FDM &&
       param_info.type != IR3_UBO_IDX_DYNAMIC_OFFSETS)
      return false;

   b->cursor = nir_instr_remove(&intr->instr);

   unsigned dest_components = nir_intrinsic_dest_components(intr);

   uint32_t ubo_index = ir3_const_state_alloc_ubo(
      const_state, param_info.type, param_info.offset + param_info.size);
   const_state->num_driver_params =
      MAX2(const_state->num_driver_params, param_info.offset + param_info.size);

   nir_def *index = nir_imm_int(b, ubo_index);
   nir_def *offset = nir_imm_int(b, param_info.offset * 4);

   nir_def *result =
      nir_load_ubo(b, dest_components, 32, index, offset,
                   .align_mul = 16,
                   .align_offset = (param_info.offset % 4) * 4,
                   .range_base = param_info.offset * 4,
                   .range = param_info.size * 4);

   nir_def_rewrite_uses(&intr->def, result);

   return true;
}

bool
ir3_nir_lower_driver_params_to_ubo(nir_shader *nir,
                                   struct ir3_shader_variant *v)
{
   bool result = nir_shader_intrinsics_pass(
      nir, lower_driver_param_to_ubo,
      nir_metadata_block_index | nir_metadata_dominance, ir3_const_state(v));

   return result;
}