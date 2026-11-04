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

static unsigned
get_task_payload_stride(nir_shader *shader)
{
   /* Align to 4 bytes to make sure that accesses are not unaligned. */
   return align(shader->info.task_payload_size, 4);
}

static nir_def *
is_invocation_zero(nir_builder *b)
{
   nir_def *cmp = nir_ieq_imm(b, nir_load_local_invocation_id(b), 0);

   nir_def *result = nir_imm_bool(b, true);
   if (b->shader->info.workgroup_size[0] != 1)
      result = nir_channel(b, cmp, 0);
   if (b->shader->info.workgroup_size[1] != 1)
      result = nir_iand(b, result, nir_channel(b, cmp, 1));
   if (b->shader->info.workgroup_size[2] != 1)
      result = nir_iand(b, result, nir_channel(b, cmp, 2));
   return result;
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
                                                get_task_payload_stride(b->shader)));
      nir_def *ptr_base = nir_load_task_payload_base_ir3(b);
      switch (intrin->intrinsic) {
      case nir_intrinsic_store_task_payload:
         nir_store_global_offset(b, intrin->src[0].ssa, ptr_base, offset,
                                 .base = nir_intrinsic_base(intrin),
                                 .write_mask = nir_intrinsic_write_mask(intrin),
                                 .access = nir_intrinsic_access(intrin),
                                 .align_mul = intrin->src[0].ssa->bit_size / 8);
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
      /* This must be workgroup uniform, but different invocations are allowed to have different values. Just write out
       * the counts from the first invocation in the workgroup.
       */
      nir_if *if_elected = nir_push_if(b, is_invocation_zero(b));
      {
         /* The mesh count payload buffer is 6 dwords per invocation, but the last 3 are unused. */
         nir_def *task_idx = nir_channel(b, nir_load_workgroup_id(b), 0);
         nir_def *offset = nir_umul24(b, task_idx, nir_imm_int(b, 6 * 4));
         nir_def *ptr_base = nir_load_mesh_count_base_ir3(b);

         /* Calculate the total workgroup count. This is bounded by maxMeshWorkGroupTotalCount so we can use 24-bit
          * multiplication.
          */
         nir_def *wgcount_x = nir_channel(b, intrin->src[0].ssa, 0);
         nir_def *wgcount_y = nir_channel(b, intrin->src[0].ssa, 1);
         nir_def *wgcount_z = nir_channel(b, intrin->src[0].ssa, 2);
         nir_def *wgsize = nir_umul24(b, wgcount_x, nir_umul24(b, wgcount_y, wgcount_z));

         /* The firmware expects 16-bit workgroup sizes, and adds 1 to them. If any component is 0 then the overall
          * count will be 0 and it will skip, so we don't need to worry about that case here.
          */
         nir_def *wgcount_encoded = nir_u2u16(b, nir_iadd_imm(b, intrin->src[0].ssa, -1));

         /* The actual payload read by the firmware is:
          * word 0: total workgroup count (x * y * z)
          * word 1: encoded x in low 16 bits and y in high 16 bits
          * word 2: encoded z in low 16 bits
          * words 3-5: ??? unused
          */
         nir_def *payload = nir_vec3(b, wgsize, nir_pack_32_2x16(b, nir_trim_vector(b, wgcount_encoded, 2)),
                                     nir_u2u32(b, nir_channel(b, wgcount_encoded, 2)));
         nir_store_global_offset(b, payload, ptr_base, offset, .align_mul = 4);
      }
      nir_pop_if(b, if_elected);
      return NIR_LOWER_INSTR_PROGRESS_REPLACE;
   } else if (intrin->intrinsic == nir_intrinsic_load_workgroup_index) {
      return get_mesh_workgroup_index(b);
   }

   return NULL;
}

static bool
filter_task_intrinsic(const nir_instr *instr, UNUSED const void *_state)
{
   if (instr->type != nir_instr_type_intrinsic)
      return false;

   nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
   return intrin->intrinsic == nir_intrinsic_load_task_payload ||
          intrin->intrinsic == nir_intrinsic_store_task_payload ||
          intrin->intrinsic == nir_intrinsic_task_payload_atomic ||
          intrin->intrinsic == nir_intrinsic_task_payload_atomic_swap ||
          intrin->intrinsic == nir_intrinsic_load_workgroup_index ||
          intrin->intrinsic == nir_intrinsic_launch_mesh_workgroups;
}

bool
tu_nir_lower_task(nir_shader *nir)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);

   const nir_lower_task_shader_options task_options = {};
   bool progress = nir_lower_task_shader(nir, task_options);

   /* Lower workgroup index to a linear ID, which will then get lowered to the HW workgroup index plus the chunk offset.
    */
   const nir_lower_compute_system_values_options options = {
      .lower_workgroup_id_to_index = true,
   };
   progress |= nir_lower_compute_system_values(nir, &options);

   progress |= nir_shader_lower_instructions(nir, filter_task_intrinsic, lower_task_intrinsic, NULL);

   return nir_progress(progress, impl, nir_metadata_control_flow);
}

struct mesh_state {
   unsigned num_per_vertex_outputs;

   struct {
      uint8_t dword_offset;
      uint8_t num_components;
      gl_varying_slot slot;
   } per_vertex_outputs[32];

   unsigned per_prim_map_size;

   /* a7xx: the AQE firmware only draws line and triangle lists, so a point primitive i is stored as the triangle
    * (i, V, V + 1) and rasterized with the point polygon mode. V and V + 1 are two extra vertices per meshlet placed
    * outside the viewport (but inside the guardband, so nothing is clipped): their points are scissored away and only
    * point i is drawn. A zero-area (i, i, i) triangle would be culled before the polygon mode applies.
    */
   bool points_as_tris;
   unsigned extra_vtx; /* V: the application's max_vertices_out */
   /* With the point polygon mode the VS point size output of the passthrough VS results in no points being drawn, so
    * the point size is not passed on and GRAS_SU_POINT_SIZE (1.0) is used: gl_PointSize other than 1 is ignored.
    */
   bool drop_psiz;
};

static unsigned
get_mesh_vis_stride(nir_shader *shader)
{
   /* There is one bit per primitive. We align to dword size so that we can use atomics. */
   return DIV_ROUND_UP(shader->info.mesh.max_primitives_out, 32) * 4;
}

/* These per-primitive outputs are written to different buffers from the normal varying buffers. */
#define SPECIAL_OUTPUTS (VARYING_BIT_CULL_PRIMITIVE | VARYING_BIT_PRIMITIVE_INDICES)

static nir_def *
lower_mesh_intrinsic(nir_builder *b, nir_instr *instr, void *_state)
{
   struct mesh_state *state = static_cast<struct mesh_state *>(_state);
   nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);

   if (intrin->intrinsic == nir_intrinsic_store_per_vertex_output ||
       intrin->intrinsic == nir_intrinsic_store_per_primitive_output) {
      nir_io_semantics sem = nir_intrinsic_io_semantics(intrin);

      if (state->drop_psiz && sem.location == VARYING_SLOT_PSIZ)
         return NIR_LOWER_INSTR_PROGRESS_REPLACE;

      nir_def *ptr_base, *ptr_offset;

      unsigned component_offset = nir_intrinsic_component(intrin);
      unsigned write_mask = nir_intrinsic_write_mask(intrin);

      nir_def *store_val = intrin->src[0].ssa;
      nir_def *arr_index = nir_get_io_arrayed_index_src(intrin)->ssa;
      nir_src *base_off_src = nir_get_io_offset_src(intrin);

      nir_def *meshlet_idx = nir_channel(b, nir_load_workgroup_id(b), 0);
      nir_def *meshlet_stride, *array_stride;
      unsigned const_offset;

      if (sem.location == VARYING_SLOT_PRIMITIVE_INDICES) {
         if (state->points_as_tris) {
            nir_def *idx = nir_channel(b, store_val, 0);
            store_val = nir_vec3(b, idx, nir_imm_int(b, state->extra_vtx), nir_imm_int(b, state->extra_vtx + 1));
            write_mask = 0x7;
         }
         store_val = nir_u2u16(b, store_val);
         /* There is an extra index for each primitive type. Add a zero at the end.
          * TODO: What is this for?
          */
         store_val = nir_pad_vector_imm_int(b, store_val, 0, store_val->num_components + 1);
         ptr_base = nir_load_index_base_ir3(b);
         ptr_offset = nir_imm_int(b, 0);
         unsigned prim_stride = tu_mesh_vertices_per_prim(b->shader->info.mesh.primitive_type) * sizeof(uint16_t);
         meshlet_stride = nir_imm_int(b, prim_stride * b->shader->info.mesh.max_primitives_out);
         array_stride = nir_imm_int(b, prim_stride);
         const_offset = 0;
      } else if (sem.location == VARYING_SLOT_CULL_PRIMITIVE) {
         nir_if *if_cull = nir_push_if(b, nir_ine_imm(b, store_val, 0));
         {
            ptr_base = nir_load_mesh_vis_base_ir3(b);

            unsigned stride = get_mesh_vis_stride(b->shader);
            nir_def *offset = nir_umul24(b, meshlet_idx, nir_imm_int(b, stride));
            /* Calculate byte offset of dword and bit within dword to set to 0 */
            offset = nir_iadd(b, offset, nir_ishl_imm(b, nir_ushr_imm(b, arr_index, 5), 2));
            nir_def *bit = nir_iand_imm(b, arr_index, 0x1f);

            /* Calculate mask to AND with */
            nir_def *mask = nir_inot(b, nir_ishl(b, nir_imm_int(b, 1), bit));

            /* Do the atomic to set the bit to 0. */
            nir_def *ptr = nir_iadd(b, ptr_base, nir_u2u64(b, offset));
            nir_global_atomic(b, 32, ptr, mask, .atomic_op = nir_atomic_op_iand); 
         }
         nir_pop_if(b, if_cull);
         return NIR_LOWER_INSTR_PROGRESS_REPLACE;
      } else if (intrin->intrinsic == nir_intrinsic_store_per_vertex_output) {
         unsigned vertex_stride = state->num_per_vertex_outputs * 16;
         ptr_base = nir_load_varying_base_ir3(b);
         ptr_offset = nir_imul_imm(b, base_off_src->ssa, 16);
         array_stride = nir_imm_int(b, vertex_stride);
         meshlet_stride = nir_imm_int(b, vertex_stride * b->shader->info.mesh.max_vertices_out);
         for (unsigned i = 0; i < ARRAY_SIZE(state->per_vertex_outputs); i++) {
            if (state->per_vertex_outputs[i].slot == sem.location) {
               const_offset = (state->per_vertex_outputs[i].dword_offset + component_offset) * 4;
               break;
            }
         }

         for (unsigned i = 0; i < state->num_per_vertex_outputs; i++) {
            if (state->per_vertex_outputs[i].slot >= sem.location &&
                state->per_vertex_outputs[i].slot < sem.location + sem.num_slots) {
               state->per_vertex_outputs[i].num_components =
                  MAX2(state->per_vertex_outputs[i].num_components,
                       component_offset + util_last_bit(write_mask));
            }
         }
      } else {
         ptr_base = nir_load_prim_varying_base_ir3(b);
         unsigned map_index = util_bitcount64(
            b->shader->info.per_primitive_outputs & ~SPECIAL_OUTPUTS & BITFIELD64_MASK(sem.location));
         ptr_offset = nir_load_per_primitive_location_ir3(b, base_off_src->ssa,
               .driver_location = map_index,
               .range = sem.num_slots);
         const_offset = component_offset * 4;
         array_stride = nir_load_per_primitive_stride_ir3(b);
         meshlet_stride = nir_umul24(b, nir_load_per_primitive_stride_ir3(b),
                                     nir_imm_int(b, b->shader->info.mesh.max_primitives_out));
      }

      ptr_offset = nir_iadd(b, ptr_offset, nir_umul24(b, arr_index, array_stride));
      ptr_offset = nir_iadd(b, ptr_offset, nir_umul24(b, meshlet_idx, meshlet_stride));
      nir_store_global_offset(b, store_val, ptr_base, ptr_offset,
         .base = const_offset,
         .write_mask = write_mask,
         .align_mul = store_val->bit_size / 8);
      return NIR_LOWER_INSTR_PROGRESS_REPLACE;
   } else if (intrin->intrinsic == nir_intrinsic_set_vertex_and_primitive_count) {
      nir_def *num_vtx = nir_read_first_invocation(b, intrin->src[0].ssa);
      nir_def *num_prm = nir_read_first_invocation(b, intrin->src[1].ssa);

      nir_if *if_elected = nir_push_if(b, is_invocation_zero(b));
      {
         nir_def *meshlet_idx = nir_channel(b, nir_load_workgroup_id(b), 0);

         if (b->shader->info.per_primitive_outputs & VARYING_BIT_CULL_PRIMITIVE) {
            unsigned stride = get_mesh_vis_stride(b->shader);
            nir_def *ptr_base = nir_load_mesh_vis_base_ir3(b);
            nir_def *offset = nir_umul24(b, meshlet_idx, nir_imm_int(b, stride));
            
            /* Initialize meshlet visibility to all ones */
            for (unsigned dword = 0; dword * 4 < stride; dword += 4) {
               nir_def *ones = nir_imm_int(b, ~0);
               nir_def *ones_src[4] = { ones, ones, ones, ones };
               unsigned components = MIN2(4, stride * 4  - dword);
               nir_def *ones_vec = nir_vec(b, ones_src, components);
               nir_store_global_offset(b, ones_vec, ptr_base, offset,
                                       .base = dword * 4,
                                       .align_mul = 4);
            }
         }

         /* Set the number of primitives to zero if the number of vertices is zero. */
         nir_def *is_zero = nir_ieq_imm(b, num_vtx, 0);
         num_prm = nir_bcsel(b, is_zero, nir_imm_int(b, 0), num_prm);
         //num_prm = nir_ishl_imm(b, num_prm, 1);

         nir_def *ptr_base = nir_load_prim_count_base_ir3(b);
         nir_store_global_offset(b, num_prm, ptr_base, nir_imul_imm(b, meshlet_idx, 4),
                                 .align_mul = 4);

         if (state->points_as_tris) {
            /* the two off-screen vertices of the point triangles */
            unsigned vertex_stride = state->num_per_vertex_outputs * 16;
            nir_def *vbase = nir_load_varying_base_ir3(b);
            nir_def *moff = nir_umul24(b, meshlet_idx,
                                       nir_imm_int(b, vertex_stride * b->shader->info.mesh.max_vertices_out));
            for (unsigned v = 0; v < 2; v++) {
               for (unsigned i = 0; i < state->num_per_vertex_outputs; i++) {
                  nir_def *val;
                  if (state->per_vertex_outputs[i].slot == VARYING_SLOT_POS)
                     val = nir_imm_vec4(b, 2.0f, v ? -2.0f : 2.0f, 0.0f, 1.0f);
                  else if (state->per_vertex_outputs[i].slot == VARYING_SLOT_PSIZ)
                     val = nir_imm_vec4(b, 1.0f, 0.0f, 0.0f, 0.0f);
                  else
                     continue;
                  nir_store_global_offset(b, val, vbase, moff,
                                          .base = (state->extra_vtx + v) * vertex_stride +
                                                  state->per_vertex_outputs[i].dword_offset * 4,
                                          .align_mul = 4);
               }
            }
         }
      }
      nir_pop_if(b, if_elected);

      if (b->shader->info.per_primitive_outputs & VARYING_BIT_CULL_PRIMITIVE) {
         /* Wait for writes to meshlet visibility to land. */
         nir_barrier(b, .execution_scope = SCOPE_WORKGROUP,
                        .memory_scope = SCOPE_WORKGROUP,
                        .memory_semantics = (nir_memory_semantics)(NIR_MEMORY_ACQUIRE | NIR_MEMORY_MAKE_AVAILABLE),
                        .memory_modes = nir_var_mem_global);
      }

      return NIR_LOWER_INSTR_PROGRESS_REPLACE;
   } else if (intrin->intrinsic == nir_intrinsic_load_workgroup_index) {
      return get_mesh_workgroup_index(b);
   } else if (intrin->intrinsic == nir_intrinsic_load_task_payload) {
      nir_def *offset = intrin->src[0].ssa;
      offset = nir_iadd(b, offset, nir_imul_imm(b, nir_load_task_id_ir3(b), get_task_payload_stride(b->shader)));

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
          intrin->intrinsic == nir_intrinsic_load_task_payload ||
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
      if (!state->per_vertex_outputs[i].num_components)
         continue;

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

   b->shader->num_inputs = state->num_per_vertex_outputs;
   b->shader->num_outputs = state->num_per_vertex_outputs;

   return b->shader;
}

bool
tu_nir_lower_mesh(nir_shader *nir, struct tu_shader *tu_shader, nir_shader **passthrough_vs_out,
                  bool points_as_tris)
{
   struct mesh_state state = {};
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);

   if (points_as_tris && nir->info.mesh.primitive_type == MESA_PRIM_POINTS) {
      state.points_as_tris = true;
      state.extra_vtx = nir->info.mesh.max_vertices_out;
      nir->info.mesh.max_vertices_out += 2;
      nir->info.mesh.primitive_type = MESA_PRIM_TRIANGLES;
      state.drop_psiz = true;
   }

   uint64_t per_vertex_outputs = nir->info.outputs_written & ~nir->info.per_primitive_outputs;
   if (state.drop_psiz)
      per_vertex_outputs &= ~VARYING_BIT_PSIZ;

   u_foreach_bit64(slot, per_vertex_outputs) {
      state.per_vertex_outputs[state.num_per_vertex_outputs].dword_offset =
         state.num_per_vertex_outputs * 4;
      state.per_vertex_outputs[state.num_per_vertex_outputs].slot = (gl_varying_slot)slot;
      state.num_per_vertex_outputs++;
   }

   /* Lower workgroup index to a linear ID, which will then get lowered to the HW workgroup index plus the chunk offset.
    */
   const nir_lower_compute_system_values_options options = {
      .lower_workgroup_id_to_index = true,
   };
   bool progress = nir_lower_compute_system_values(nir, &options);

   progress |= nir_shader_lower_instructions(nir, filter_mesh_intrinsic, lower_mesh_intrinsic, &state);

   *passthrough_vs_out = create_passthrough_vs(&state, nir);

   tu_shader->mesh.num_per_vertex_outputs = state.num_per_vertex_outputs;
   for (unsigned i = 0; i < tu_shader->mesh.num_per_vertex_outputs; i++) {
      tu_shader->mesh.per_vertex_outputs[i].num_components = state.per_vertex_outputs[i].num_components;
      tu_shader->mesh.per_vertex_outputs[i].dword_offset = state.per_vertex_outputs[i].dword_offset;
   }

   tu_shader->mesh.per_prim_varyings_written = nir->info.per_primitive_outputs & ~SPECIAL_OUTPUTS;
   tu_shader->mesh.primitive_count = nir->info.mesh.max_primitives_out;
   tu_shader->mesh.vertex_count = nir->info.mesh.max_vertices_out;
   tu_shader->mesh.mesh_vis_stride =
      (nir->info.per_primitive_outputs & VARYING_BIT_CULL_PRIMITIVE) ? get_mesh_vis_stride(nir) : 0;
   tu_shader->mesh.task_payload_stride = get_task_payload_stride(nir);
   tu_shader->mesh.prim_type = nir->info.mesh.primitive_type;
   tu_shader->mesh.points_as_tris = state.points_as_tris;

   return nir_progress(progress, impl, nir_metadata_none);
}

