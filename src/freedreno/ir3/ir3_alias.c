/*
 * Copyright 2024 Igalia S.L.
 * SPDX-License-Identifier: MIT
 */

#include "ir3.h"
#include "ir3_shader.h"

static bool
supports_alias_srcs(struct ir3_instruction *instr)
{
   return is_tex(instr) && !is_tex_shuffle(instr);
}

static bool
can_alias_src(struct ir3_register *src)
{
   return reg_gpr(src) && !(src->flags & IR3_REG_SHARED);
}

static bool
can_alias_srcs_of_def(struct ir3_register *src)
{
   if (!can_alias_src(src)) {
      return false;
   }

   assert(src->flags & IR3_REG_SSA);
   struct ir3_instruction *def_instr = src->def->instr;

   if (def_instr->opc == OPC_META_COLLECT) {
      return true;
   }
   if (def_instr->opc == OPC_MOV) {
      return is_same_type_mov(def_instr) &&
             !(def_instr->srcs[0]->flags & IR3_REG_SHARED);
   }

   return false;
}

static bool
alias_srcs(struct ir3_instruction *instr)
{
   bool progress = false;

   /* All sources that come from collects are replaced by the sources of the
    * collects. So allocate a new srcs array to hold all the collect'ed sources
    * as well.
    */
   unsigned new_srcs_count = 0;

   foreach_src_n (src, src_n, instr) {
      if (can_alias_srcs_of_def(src)) {
         new_srcs_count += util_last_bit(src->wrmask);
      } else {
         new_srcs_count++;
      }
   }

   struct ir3_register **old_srcs = instr->srcs;
   unsigned old_srcs_count = instr->srcs_count;
   instr->srcs =
      ir3_alloc(instr->block->shader, new_srcs_count * sizeof(instr->srcs[0]));
   instr->srcs_count = 0;

#ifndef NDEBUG
   instr->srcs_max = new_srcs_count;
#endif

   for (unsigned src_n = 0; src_n < old_srcs_count; src_n++) {
      struct ir3_register *src = old_srcs[src_n];
      bool can_alias = can_alias_src(src);

      if (!can_alias || !can_alias_srcs_of_def(src)) {
         if (can_alias) {
            src->flags |= (IR3_REG_FIRST_ALIAS | IR3_REG_ALIAS);
            progress = true;
         }

         instr->srcs[instr->srcs_count++] = src;
         continue;
      }

      struct ir3_instruction *collect = src->def->instr;
      assert(collect->opc == OPC_META_COLLECT || collect->opc == OPC_MOV);

      foreach_src_n (collect_src, collect_src_n, collect) {
         struct ir3_register *alias_src;

         if (collect_src->flags & IR3_REG_SSA) {
            alias_src =
               __ssa_src(instr, collect_src->def->instr, collect_src->flags);
         } else {
            alias_src =
               ir3_src_create(instr, collect_src->num, collect_src->flags);
            alias_src->uim_val = collect_src->uim_val;
         }

         alias_src->flags |= IR3_REG_ALIAS;

         if (collect_src_n == 0) {
            alias_src->flags |= IR3_REG_FIRST_ALIAS;
         }
      }

      progress = true;
   }

   return progress;
}

/* First alias.tex pass: replace sources of tex instructions with alias sources
 * (IR3_REG_ALIAS):
 * - movs from const/imm: replace with the const/imm;
 * - collects: replace with the sources of the collect;
 * - GPR sources: simply mark as alias.
 *
 * This way, RA won't be forced to allocate consecutive registers for collects
 * and useless collects/movs can be DCE'd. Note that simply lowering collects to
 * aliases doesn't work because RA would assume that killed sources of aliases
 * are dead, while they are in fact live until the tex instruction that uses
 * them.
 */
bool
ir3_create_alias_tex_regs(struct ir3 *ir)
{
   if (!ir->compiler->has_alias)
      return false;
   if (ir3_shader_debug & IR3_DBG_NOALIASTEX)
      return false;

   bool progress = false;

   foreach_block (block, &ir->block_list) {
      foreach_instr (instr, &block->instr_list) {
         if (supports_alias_srcs(instr)) {
            progress |= alias_srcs(instr);
         }
      }
   }

   return progress;
}

#define FIRST_ALIAS_REG regid(40, 0)

struct alias_table_entry {
   unsigned alias_reg;
   struct ir3_register *src;
};

struct alias_table_state {
   struct alias_table_entry entries[16];
   unsigned num_entries;
};

static void
add_table_entry(struct alias_table_state *state, unsigned alias_reg,
                struct ir3_register *src)
{
   assert(state->num_entries < ARRAY_SIZE(state->entries));
   struct alias_table_entry *entry = &state->entries[state->num_entries++];
   entry->alias_reg = alias_reg;
   entry->src = src;
}

static void
alloc_aliases(struct alias_table_state *state, struct ir3_instruction *instr,
              unsigned *regs)
{
   unsigned next_alias_reg = FIRST_ALIAS_REG;

   foreach_src_n (src, src_n, instr) {
      if (src->flags & IR3_REG_ALIAS) {
         unsigned alias_reg = next_alias_reg++;
         regs[src_n] = alias_reg;
         add_table_entry(state, alias_reg, instr->srcs[src_n]);
      }
   }
}

static bool
insert_aliases(struct ir3_instruction *instr)
{
   bool progress = false;

   if (instr->flags & IR3_INSTR_IMM_OFFSET) {
      /* Adding an immediate offset to an immediate alias doesn't seem to work:
       * alias.tex r40.x, 0
       * isam.v ..., r40.x+4, ...
       * Just add the offset to the alias.
       */
      assert(instr->opc == OPC_ISAM);

      unsigned offset_src_n = (instr->flags & IR3_INSTR_S2EN) ? 2 : 0;
      /* 1 coordinate + 1 imm offset */
      assert(instr->srcs_count >= 1 + 1 + offset_src_n);

      struct ir3_register *offset = instr->srcs[offset_src_n];
      struct ir3_register *imm_offset = instr->srcs[offset_src_n + 1];
      assert(imm_offset->flags & IR3_REG_IMMED);

      if (offset->flags & IR3_REG_IMMED) {
         offset->uim_val += imm_offset->uim_val;
         instr->flags &= ~IR3_INSTR_IMM_OFFSET;
         instr->srcs_count--;
      }
   }

   struct alias_table_state state = {0};
   struct ir3_cursor cursor = ir3_before_instr(instr);

   unsigned regs[instr->srcs_count];
   alloc_aliases(&state, instr, regs);

   for (unsigned i = 0; i < state.num_entries; i++) {
      struct alias_table_entry *entry = &state.entries[i];

      struct ir3_instruction *alias =
         ir3_instr_create_at(cursor, OPC_ALIAS, 1, 2);
      alias->cat7.alias_scope = ALIAS_TEX;
      struct ir3_register *src = ir3_src_create(
         alias, entry->src->num,
         entry->src->flags & ~(IR3_REG_FIRST_ALIAS | IR3_REG_ALIAS));
      src->uim_val = entry->src->uim_val;
      ir3_dst_create(alias, entry->alias_reg,
                     (entry->src->flags & IR3_REG_HALF) | IR3_REG_ALIAS);

      if (i == 0) {
         alias->cat7.alias_table_size_minus_one = state.num_entries - 1;
      }

      progress = true;
   }

   unsigned next_src_n = 0;

   for (unsigned src_n = 0; src_n < instr->srcs_count;) {
      struct ir3_register *src0 = instr->srcs[src_n];
      unsigned num_srcs = 0;

      if (src0->flags & IR3_REG_FIRST_ALIAS) {
         foreach_src_in_alias_group (src, instr, src_n) {
            num_srcs++;
         }

         src0->num = regs[src_n];
         src0->flags &= ~(IR3_REG_IMMED | IR3_REG_CONST);
         src0->wrmask = MASK(num_srcs);
      } else {
         num_srcs = 1;
      }

      instr->srcs[next_src_n++] = src0;
      src_n += num_srcs;
   }

   instr->srcs_count = next_src_n;
   return progress;
}

static bool
has_alias_srcs(struct ir3_instruction *instr)
{
   if (!supports_alias_srcs(instr)) {
      return false;
   }

   foreach_src (src, instr) {
      if (src->flags & IR3_REG_FIRST_ALIAS) {
         return true;
      }
   }

   return false;
}

/* Second alias.tex pass: insert alias.tex instructions in front of the tex
 * instructions that need them and fix up the tex instruction's sources. This
 * pass needs to run post-RA (see ir3_create_alias_tex_regs). It also needs to
 * run post-legalization as all the sync flags need to be inserted based on the
 * registers instructions actually use, not on the alias registers they have as
 * sources.
 */
bool
ir3_insert_alias_tex(struct ir3 *ir)
{
   if (!ir->compiler->has_alias)
      return false;
   if (ir3_shader_debug & IR3_DBG_NOALIASTEX)
      return false;

   bool progress = false;

   foreach_block (block, &ir->block_list) {
      foreach_instr_safe (instr, &block->instr_list) {
         if (has_alias_srcs(instr)) {
            progress |= insert_aliases(instr);
         }
      }
   }

   return progress;
}
