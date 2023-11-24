/*
 * Copyright © 2023 Igalia S.L.
 * SPDX-License-Identifier: MIT
 */

#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "util/u_math.h"
#include "ir3_compiler.h"
#include "ir3_nir.h"

bool
ir3_nir_lower_push_consts_to_preamble(nir_shader *nir,
                                      struct ir3_shader_variant *v)
{
   nir_function_impl *preamble = nir_shader_get_preamble(nir);
   nir_builder _b = nir_builder_at(nir_before_impl(preamble));
   nir_builder *b = &_b;

   nir_copy_push_const_to_uniform_ir3(
      b, nir_imm_int(b, 0), .base = v->shader_options.push_consts_base,
      .range = v->shader_options.push_consts_dwords);

   nir_foreach_function_impl(impl, nir) {
      nir_metadata_preserve(impl, nir_metadata_none);
   }
   return true;
}

struct driver_param_to_preamble_data
{
   uint32_t driver_param_offset_vec4_min;
   uint32_t driver_param_offset_vec4_max;

   uint32_t prim_loc_offset_vec4_min;
   uint32_t prim_loc_offset_vec4_max;

   uint32_t prim_param_offset_vec4_min;
   uint32_t prim_param_offset_vec4_max;
};

static bool
gather_driver_param_loads(nir_builder *b, nir_intrinsic_instr *intr, void *in)
{
   struct driver_param_to_preamble_data *data = in;

   enum ir3_driver_param driver_param = -1;
   uint32_t driver_location = -1;
   uint32_t prim_param_location = -1;

   switch (intr->intrinsic) {
   case nir_intrinsic_load_base_workgroup_id: {
      driver_param = IR3_DP_BASE_GROUP_X;
      break;
   }
   case nir_intrinsic_load_num_workgroups: {
      driver_param = IR3_DP_NUM_WORK_GROUPS_X;
      break;
   }
   case nir_intrinsic_load_workgroup_size: {
      driver_param = IR3_DP_LOCAL_GROUP_SIZE_X;
      break;
   }
   case nir_intrinsic_load_subgroup_size: {
      if (b->shader->info.stage == MESA_SHADER_COMPUTE)
         driver_param = IR3_DP_CS_SUBGROUP_SIZE;
      else
         driver_param = IR3_DP_FS_SUBGROUP_SIZE;
      break;
   }
   case nir_intrinsic_load_subgroup_id_shift_ir3: {
      driver_param = IR3_DP_SUBGROUP_ID_SHIFT;
      break;
   }
   case nir_intrinsic_load_work_dim: {
      driver_param = IR3_DP_WORK_DIM;
      break;
   }
   case nir_intrinsic_load_tess_level_outer_default: {
      driver_param = IR3_DP_HS_DEFAULT_OUTER_LEVEL_X;
      break;
   }
   case nir_intrinsic_load_tess_level_inner_default: {
      driver_param = IR3_DP_HS_DEFAULT_INNER_LEVEL_X;
      break;
   }

   case nir_intrinsic_load_frag_invocation_count: {
      driver_param = IR3_DP_FS_FRAG_INVOCATION_COUNT;
      break;
   }
   case nir_intrinsic_load_frag_size_ir3:
   case nir_intrinsic_load_frag_offset_ir3: {
      unreachable("TODO");
      break;
   }

   case nir_intrinsic_load_primitive_location_ir3: {
      driver_location = nir_intrinsic_driver_location(intr);
      break;
   }

   case nir_intrinsic_load_vs_primitive_stride_ir3: {
      prim_param_location = 0;
      break;
   }
   case nir_intrinsic_load_vs_vertex_stride_ir3: {
      prim_param_location = 1;
      break;
   }
   case nir_intrinsic_load_hs_patch_stride_ir3: {
      prim_param_location = 2;
      break;
   }
   case nir_intrinsic_load_patch_vertices_in: {
      prim_param_location = 3;
      break;
   }
   case nir_intrinsic_load_tess_param_base_ir3: {
      prim_param_location = 4;
      break;
   }
   case nir_intrinsic_load_tess_factor_base_ir3: {
      prim_param_location = 6;
      break;
   }

   default:
      return false;
   }

   if (driver_param != -1) {
      data->driver_param_offset_vec4_min =
         MIN2(data->driver_param_offset_vec4_min, driver_param / 4);
      data->driver_param_offset_vec4_max =
         MAX2(data->driver_param_offset_vec4_max, driver_param / 4);
   }

   if (driver_location != -1) {
      data->prim_loc_offset_vec4_min =
         MIN2(data->prim_loc_offset_vec4_min, driver_location / 4);
      data->prim_loc_offset_vec4_max =
         MAX2(data->prim_loc_offset_vec4_max, driver_location / 4);
   }

   if (prim_param_location != -1) {
      data->prim_param_offset_vec4_min =
         MIN2(data->prim_param_offset_vec4_min, prim_param_location / 4);
      data->prim_param_offset_vec4_max =
         MAX2(data->prim_param_offset_vec4_max, prim_param_location / 4);
   }

   return true;
}

static bool
lower_driver_and_prim_consts(nir_shader *nir, struct ir3_shader_variant *v)
{
   struct driver_param_to_preamble_data data = {.driver_param_offset_vec4_min = UINT32_MAX,
                                                .driver_param_offset_vec4_max = 0,
                                                .prim_loc_offset_vec4_min = UINT32_MAX,
                                                .prim_loc_offset_vec4_max = 0,
                                                .prim_param_offset_vec4_min = UINT32_MAX,
                                                .prim_param_offset_vec4_max = 0, };
   bool gathered = nir_shader_intrinsics_pass(nir, gather_driver_param_loads,
                                              nir_metadata_all, &data);

   if (!gathered)
      return false;

   nir_function_impl *preamble = nir_shader_get_preamble(nir);
   nir_builder _b = nir_builder_at(nir_before_impl(preamble));
   nir_builder *b = &_b;

   if (b->shader->info.num_ubos == 0)
      b->shader->info.num_ubos++;

   struct ir3_const_state *const_state = ir3_const_state(v);

   if (data.driver_param_offset_vec4_min != UINT32_MAX) {
      const_state->constant_data_driver_params = b->shader->info.num_ubos;

      uint32_t load_size = data.driver_param_offset_vec4_max - data.driver_param_offset_vec4_min + 1;

      nir_copy_driver_params_to_uniform_ir3(
         b, nir_imm_int(b, const_state->constant_data_driver_params),
         .base = data.driver_param_offset_vec4_min * 4, .range = load_size);

      b->shader->info.num_ubos += 1;
   }

   if (data.prim_loc_offset_vec4_min != UINT32_MAX) {
      const_state->constant_data_primitive_map = b->shader->info.num_ubos;

      uint32_t load_size = data.prim_loc_offset_vec4_max - data.prim_loc_offset_vec4_min + 1;

      nir_copy_prim_locs_to_uniform_ir3(
         b, nir_imm_int(b, const_state->constant_data_primitive_map),
         .base = data.prim_loc_offset_vec4_min * 4, .range = load_size);

      b->shader->info.num_ubos += 1;
   }

   if (data.prim_param_offset_vec4_min != UINT32_MAX) {
      const_state->constant_data_primitive_param = b->shader->info.num_ubos;

      uint32_t load_size = data.prim_param_offset_vec4_max - data.prim_param_offset_vec4_min + 1;

      nir_copy_prim_params_to_uniform_ir3(
         b, nir_imm_int(b, const_state->constant_data_primitive_param),
         .base = data.prim_param_offset_vec4_min * 4, .range = load_size);

      b->shader->info.num_ubos += 1;
   }

   return true;
}

static bool
lower_dyn_offsets_consts(nir_shader *nir, struct ir3_shader_variant *v)
{

   if (v->shader_options.dynamic_offset_const_base == UINT32_MAX)
      return false;

   nir_function_impl *preamble = nir_shader_get_preamble(nir);
   nir_builder _b = nir_builder_at(nir_before_impl(preamble));
   nir_builder *b = &_b;

   if (b->shader->info.num_ubos == 0)
      b->shader->info.num_ubos++;

   struct ir3_const_state *const_state = ir3_const_state(v);

   const_state->constant_data_dynamic_offsets = b->shader->info.num_ubos;

   nir_copy_dyn_offsets_to_uniform_ir3(
      b, nir_imm_int(b, const_state->constant_data_dynamic_offsets),
      .base = v->shader_options.dynamic_offset_const_base,
      .range = DIV_ROUND_UP(v->shader_options.dynamic_offset_const_dwords, 4));

   b->shader->info.num_ubos += 1;

   return true;
}

bool
ir3_nir_lower_driver_param_load_to_preamble(nir_shader *nir,
                                            struct ir3_shader_variant *v)
{
   bool progress = false;
   if (v->type != MESA_SHADER_VERTEX) {
      progress |= lower_driver_and_prim_consts(nir, v);
   }

   if (v->shader_options.dynamic_offset_const_base == UINT32_MAX) {
      progress |= lower_dyn_offsets_consts(nir, v);
   }

   if (progress) {
      nir_foreach_function_impl (impl, nir) {
         nir_metadata_preserve(impl, nir_metadata_none);
      }
   }

   return progress;
}
