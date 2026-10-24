/*
 * Copyright © 2026 Valve Corporation
 * SPDX-License-Identifier: MIT
 */

#include "tu_shader.h"
#include "nir/nir_builder.h"

/* Both mesh and task shaders process workgroups in "chunks" small enough so that everything fits in GMEM and always
 * launch a 1-dimensional dispatch. Compute gl_WorkgroupInvocation using the linear per-chunk index. We assume
 * the 3D index has already been flattened to 1D by lower_compute_system_values.
 */

static nir_def *
get_mesh_workgroup_index(nir_builder *b)
{
   return nir_iadd(b, nir_imul(b, nir_load_chunk_idx_ir3(b), nir_load_workgroups_per_chunk_ir3(b)),
                   nir_channel(b, nir_load_workgroup_id(b), 0));
}

static nir_def *
lower_task_intrinsic(nir_builder *b, nir_instr *instr, void *_state)
{
   nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
   if (intrin->intrinsic == nir_intrinsic_store_task_payload ||
       intrin->intrinsic == nir_intrinsic_load_task_payload ||
       intrin->intrinsic == nir_intrinsic_task_payload_atomic ||
       intrin->intrinsic == nir_intrinsic_task_payload_atomic_swap) {
      nir_def *offset = nir_get_io_offset_src(intrin)->ssa;
      nir_def *task_idx = nir_channel(b, nir_load_workgroup_id(b), 0);
      offset = nir_iadd(b, offset, nir_imul_imm(b, task_idx,
                                                b->shader->info.task_payload_size));
      nir_def *ptr_base = nir_load_task_payload_base_ir3(b);
      switch (intrin->intrinsic) {
      case nir_intrinsic_store_task_payload:
         nir_store_global_offset(b, intrin->src[0].ssa, ptr_base, offset,
                                 .base = nir_intrinsic_base(intrin),
                                 .align_mul = intrin->src[0].ssa->bit_size / 8,
                                 .write_mask = nir_intrinsic_write_mask(intrin),
                                 .access = nir_intrinsic_access(intrin));
         return NIR_LOWER_INSTR_PROGRESS_REPLACE;
      case nir_intrinsic_load_task_payload:
         return nir_load_global_offset(b, intrin->def.num_components, intrin->def.bit_size,
                                       ptr_base, offset,
                                       .base = nir_intrinsic_base(intrin),
                                       .access = nir_intrinsic_access(intrin));
      case nir_intrinsic_task_payload_atomic:
      case nir_intrinsic_task_payload_atomic_swap: {
         nir_def *ptr = nir_iadd(b, ptr_base, nir_u2u64(b, nir_iadd_imm(b, offset, nir_intrinsic_base(intrin))));
         if (intrin->intrinsic == nir_intrinsic_task_payload_atomic) {
            return nir_global_atomic(b, intrin->def.bit_size, ptr, intrin->src[1].ssa,
                                     .access = nir_intrinsic_access(intrin),
                                     .atomic_op = nir_intrinsic_atomic_op(intrin));
         } else {
            return nir_global_atomic_swap(b, intrin->def.bit_size, ptr, intrin->src[1].ssa, intrin->src[2].ssa,
                                          .access = nir_intrinsic_access(intrin),
                                          .atomic_op = nir_intrinsic_atomic_op(intrin));
         }
      }
      }
   } else if (intrin->intrinsic == nir_intrinsic_launch_mesh_workgroups) {
      nir_if *if_elected = nir_push_if(b, nir_ieq_imm(b, nir_load_local_invocation_index(b), 0));
      {
         /* The mesh count buffer is 6 dwords per invocation, but the last 3 are unused. */
         nir_def *task_idx = nir_channel(b, nir_load_workgroup_id(b), 0);
         nir_def *offset = nir_umul24(b, task_idx, nir_imm_int(b, 6 * 4));
         nir_def *ptr_base = nir_load_prim_count_base_ir3(b);

         /* Calculate the total workgroup count. This is bounded by maxMeshWorkGroupTotalCount so we can use 24-bit
          * multiplication.
          */
         nir_def *wgcount_x = nir_channel(b, intrin->src[0].ssa, 0);
         nir_def *wgcount_y = nir_channel(b, intrin->src[0].ssa, 1);
         nir_def *wgcount_z = nir_channel(b, intrin->src[0].ssa, 2);
         nir_def *wgsize = nir_umul24(b, wgcount_x, nir_umul24(b, wgcount_y, wgcount_z));

         /* The firmware expects 16-bit workgroup sizes, and adds 1 to them. If any component is 0 then the overall size
          * will be 0 and it will skip.
          */
         nir_def *wgcount_encoded = nir_u2u16(b, nir_iadd_imm(b, intrin->src[0].ssa, -1));
         nir_def *payload = nir_vec3(b, wgsize, nir_pack_32_2x16(b, nir_trim_vector(b, wgcount_encoded, 2)),
                                     nir_u2u32(b, nir_channel(b, wgcount_encoded, 2)));
         nir_store_global_offset(b, payload, ptr_base, offset, .align_mul = 4);
      }
      nir_pop_if(b, if_elected);
      return NIR_LOWER_INSTR_PROGRESS_REPLACE;
   }

   return NULL;
}

struct mesh_state {
   unsigned num_per_vertex_outputs;

   struct {
      uint8_t dword_offset;
      uint8_t num_components;
      gl_varying_slot slot;
   } per_vertex_outputs[32];

   unsigned per_prim_map_size;
};

static nir_def *
lower_mesh_intrinsic(nir_builder *b, nir_instr *instr, void *_state)
{
   struct mesh_state *state = static_cast<struct mesh_state *>(_state);
   nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);

   if (intrin->intrinsic == nir_intrinsic_store_per_vertex_output ||
       intrin->intrinsic == nir_intrinsic_store_per_primitive_output) {
      nir_io_semantics sem = nir_intrinsic_io_semantics(intrin);

      nir_def *ptr_base, *ptr_offset;
      unsigned bit_size = 32;

      unsigned component_offset = nir_intrinsic_component(intrin);
      unsigned write_mask = nir_intrinsic_write_mask(intrin);

      nir_def *store_val = intrin->src[0].ssa;
      nir_def *arr_index = nir_get_io_arrayed_index_src(intrin)->ssa;
      nir_src *base_off_src = nir_get_io_offset_src(intrin);

      nir_def *meshlet_idx = nir_channel(b, nir_load_workgroup_id(b), 0);
      nir_def *meshlet_stride;

      if (sem.location == VARYING_BIT_PRIMITIVE_INDICES) {
         store_val = nir_u2u16(b, store_val);
         ptr_base = nir_load_index_base_ir3(b);
         meshlet_stride = nir_imm_int(b, b->shader->info.mesh.max_primitives_out *
            mesa_vertices_per_prim(b->shader->info.mesh.primitive_type) * 2);
         ptr_offset = nir_ishl_imm(b, arr_index, 1);
      } else if (sem.location == VARYING_BIT_CULL_PRIMITIVE) {
         /* TODO */
      } else if (intrin->intrinsic == nir_intrinsic_store_per_vertex_output) {
         ptr_base = nir_load_varying_base_ir3(b);
         ptr_offset = nir_iadd_imm(b, nir_imul_imm(b, arr_index, 16),
               (state->per_vertex_outputs[sem.location].dword_offset + component_offset) * 4);
      } else {
         ptr_base = nir_load_prim_varying_base_ir3(b);
         ptr_offset = nir_iadd_imm(b, nir_load_per_primitive_location_ir3(b, base_off_src,
               .driver_location = sem.location - VARYING_SLOT_VAR0), component_offset);
         meshlet_stride = nir_load_per_primitive_stride_ir3(b);

         for (unsigned i = 0; i < state->num_per_vertex_outputs; i++) {
            if (state->per_vertex_outputs[i].slot >= sem.location &&
                state->per_vertex_outputs[i].slot < sem.location + nir_intrinsic_range(intrin)) {
               state->per_vertex_outputs[i].num_components =
                  MAX2(state->per_vertex_outputs[i].num_components,
                       component_offset + util_last_bit(write_mask));
            }
         }
      }

      ptr_offset = nir_iadd(b, ptr_offset, nir_umul24(b, meshlet_idx, meshlet_stride));
      nir_store_global_offset(b, store_val, ptr_base, ptr_offset,
         .write_mask = write_mask,
         .align_mul = store_val->bit_size / 8);
      return NIR_LOWER_INSTR_PROGRESS_REPLACE;
   } else if (intrin->intrinsic == nir_intrinsic_set_vertex_and_primitive_count) {
      nir_def *num_vtx = nir_read_first_invocation(b, intrin->src[0].ssa);
      nir_def *num_prm = nir_read_first_invocation(b, intrin->src[1].ssa);

      nir_if *if_elected = nir_push_if(b, nir_ieq_imm(b, nir_load_local_invocation_index(b), 0));
      {
         /* Set the number of primitives to zero if the number of vertices is zero. */
         nir_def *is_zero = nir_ieq_imm(b, num_vtx, 0);
         num_prm = nir_bcsel(b, is_zero, nir_imm_int(b, 0), num_prm);

         nir_def *meshlet_idx = nir_channel(b, nir_load_workgroup_id(b), 0);
         nir_def *ptr_base = nir_load_prim_count_base_ir3(b);
         nir_store_global_offset(b, num_prm, ptr_base, nir_imul_imm(b, meshlet_idx, 4),
                                 .align_mul = 4);
      }
      nir_pop_if(b, if_elected);
      return NIR_LOWER_INSTR_PROGRESS_REPLACE;
   } else if (intrin->intrinsic == nir_intrinsic_load_workgroup_index) {
      return get_mesh_workgroup_index(b);
   } else if (intrin->intrinsic == nir_intrinsic_load_task_payload) {
      nir_def *offset = intrin->src[0].ssa;
      offset = nir_iadd(b, offset, nir_imul_imm(b, nir_load_task_id_ir3(b), b->shader->info.task_payload_size));

      /* If the offset is constant and in-bounds, we can guarantee that it exists and it's safe to speculate:
       * TODO: For non-constant offsets, pass through a range, and add a new load_global variant to capture that we can
       * prefetch the array.
       */
      return nir_load_global_offset(b, intrin->def.num_components, intrin->def.bit_size,
                                    nir_load_task_payload_base_ir3(b),
                                    offset,
                                    .base = nir_intrinsic_base(intrin),
                                    .access =
                                     (gl_access_qualifier)((nir_def_is_const(offset) ? ACCESS_CAN_SPECULATE : 0) |
                                                           ACCESS_NON_WRITEABLE | ACCESS_CAN_REORDER));
   }

   return NULL;
}

static bool
filter_mesh_intrinsic(const nir_instr *instr, UNUSED const void *_state)
{
   if (instr->type != nir_instr_type_intrinsic)
      return false;

   nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
   return intrin->intrinsic == nir_intrinsic_store_per_vertex_output ||
          intrin->intrinsic == nir_intrinsic_store_per_primitive_output ||
          intrin->intrinsic == nir_intrinsic_load_workgroup_index ||
          intrin->intrinsic == nir_intrinsic_set_vertex_and_primitive_count;
}

static nir_shader *
create_passthrough_vs(const struct mesh_state *state, nir_shader *shader)
{
   nir_builder _b =
      nir_builder_init_simple_shader(MESA_SHADER_VERTEX, shader->options, "passthrough VS (%s)", shader->info.name);
   nir_builder *b = &_b;
   memcpy(b->shader->info.source_blake3, shader->info.source_blake3, sizeof(shader->info.source_blake3));

   for (unsigned i = 0; i < state->num_per_vertex_outputs; i++) {
      nir_def *def = nir_load_input(b, state->per_vertex_outputs[i].num_components, 32, nir_imm_int(b, 0),
            .base = i,
            .range = 1,
            .dest_type = nir_type_uint32,
            .io_semantics = {
               .location = VERT_ATTRIB_GENERIC0 + i,
               .num_slots = 1,
            });

      nir_store_output(b, def, nir_imm_int(b, 0),
            .base = i,
            .range = 1,
            .write_mask = BITSET_MASK(def->num_components),
            .src_type = nir_type_uint32,
            .io_semantics = {
               .location = state->per_vertex_outputs[i].slot,
               .num_slots = 1,
            });
   }

   return b->shader;
}

static bool
tu_nir_lower_mesh(nir_shader *shader, struct tu_shader *tu_shader, nir_shader **passthrough_vs_out)
{
   struct mesh_state state = {};
   nir_function_impl *impl = nir_shader_get_entrypoint(shader);

   /* These outputs are written to different buffers from the normal varying buffer. */
   const uint64_t special_outputs = VARYING_BIT_CULL_PRIMITIVE | VARYING_BIT_PRIMITIVE_INDICES;

   uint64_t per_vertex_outputs = shader->info.outputs_written & ~shader->info.per_primitive_outputs & ~special_outputs;

   u_foreach_bit64(slot, per_vertex_outputs) {
      state.per_vertex_outputs[state.num_per_vertex_outputs].dword_offset =
         state.num_per_vertex_outputs * 4;
      state.per_vertex_outputs[state.num_per_vertex_outputs].slot = (gl_varying_slot)slot;
      state.num_per_vertex_outputs++;
   }

   const nir_lower_compute_system_values_options options = {
      .lower_workgroup_id_to_index = true,
   };
   bool progress = nir_lower_compute_system_values(shader, &options);

   progress |= nir_shader_lower_instructions(shader, filter_mesh_intrinsic, lower_mesh_intrinsic, &state);

   *passthrough_vs_out = create_passthrough_vs(&state, shader);

   tu_shader->mesh.num_per_vertex_outputs = state.num_per_vertex_outputs;
   for (unsigned i = 0; i < tu_shader->mesh.num_per_vertex_outputs; i++) {
      tu_shader->mesh.per_vertex_outputs[i].num_components = state.per_vertex_outputs[i].num_components;
      tu_shader->mesh.per_vertex_outputs[i].dword_offset = state.per_vertex_outputs[i].dword_offset;
   }
   tu_shader->mesh.per_prim_map_size = util_last_bit(shader->info.per_primitive_outputs >> VARYING_SLOT_VAR0);

   return nir_progress(progress, impl, nir_metadata_none);
}

