/*
 * Copyright © 2021 Google, Inc.
 * SPDX-License-Identifier: MIT
 */

#include <err.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "rnn.h"
#include "rnndec.h"

#include "qrisc.h"
#include "util.h"

static struct rnndeccontext *ctx;
static struct rnndb *db;
static struct rnndomain *control_regs;
static struct rnndomain *sqe_regs;
static struct rnndomain *pipe_regs;
struct rnndomain *dom[2];
static struct rnnenum *pm4_packets;

static int
find_reg(struct rnndomain *dom, const char *name)
{
   for (int i = 0; i < dom->subelemsnum; i++) {
      if (!rnndec_varmatch(ctx, &dom->subelems[i]->varinfo))
         continue;
      if (!strcmp(name, dom->subelems[i]->name))
         return dom->subelems[i]->offset;
   }
   return -1;
}

/*
 * Optional symbol file (-s): names for labels (instruction offsets) and
 * control registers, e.g. for firmware-defined scratch registers that are
 * not hardware registers and so are not in adreno_control_regs.xml.
 *
 *    # comment
 *    label 0x1fd9 cmdq_reserve
 *    creg  0x4c1  CMDQ_PACKETS
 *    reloc 0x45   aqe_init_data 4
 *
 * "reloc" marks an absolute code address in an immediate: the immediate of
 * the instruction at 0x45 is the offset of label aqe_init_data times 4, and
 * is printed as "#aqe_init_data * 4" so it follows the label when code moves.
 */
enum qrisc_symbol_kind { SYM_LABEL, SYM_CREG, SYM_RELOC };

struct qrisc_symbol {
   enum qrisc_symbol_kind kind;
   unsigned val;      /* label: instruction offset, creg: register, reloc: instruction offset */
   char *name;        /* reloc: the label it refers to */
   unsigned scale;    /* reloc only */
};

static struct qrisc_symbol *symbols;
static unsigned num_symbols;

static bool
valid_identifier(const char *s)
{
   if (!(s[0] == '_' || (s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z')))
      return false;
   for (; *s; s++) {
      if (!(*s == '_' || (*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
            (*s >= '0' && *s <= '9')))
         return false;
   }
   return true;
}

int
qrisc_load_symbols(const char *path)
{
   FILE *f = fopen(path, "r");
   if (!f) {
      fprintf(stderr, "could not open symbol file \"%s\"\n", path);
      return -1;
   }

   char line[512];
   unsigned lineno = 0;
   while (fgets(line, sizeof(line), f)) {
      char kind[16], name[256];
      unsigned val, scale = 1;

      lineno++;
      char *hash = strchr(line, '#');
      if (hash)
         *hash = '\0';
      if (sscanf(line, " %15s", kind) != 1)
         continue;   /* blank or comment */

      int n = sscanf(line, " %15s %i %255s %i", kind, &val, name, &scale);
      bool is_reloc = !strcmp(kind, "reloc");
      if (n < 3 || (n == 4 && !is_reloc) || scale == 0 ||
          (strcmp(kind, "label") && strcmp(kind, "creg") && !is_reloc) ||
          !valid_identifier(name)) {
         fprintf(stderr, "%s:%u: expected \"label|creg <offset> <identifier>\" or "
                 "\"reloc <offset> <label> [scale]\"\n", path, lineno);
         fclose(f);
         return -1;
      }

      enum qrisc_symbol_kind sk = is_reloc ? SYM_RELOC : !strcmp(kind, "creg") ? SYM_CREG : SYM_LABEL;
      for (unsigned i = 0; i < num_symbols; i++) {
         if (symbols[i].kind == sk &&
             (symbols[i].val == val || (sk != SYM_RELOC && !strcmp(symbols[i].name, name)))) {
            fprintf(stderr, "%s:%u: %s 0x%x / %s defined twice\n", path, lineno, kind, val, name);
            fclose(f);
            return -1;
         }
      }

      symbols = realloc(symbols, (num_symbols + 1) * sizeof(*symbols));
      symbols[num_symbols++] = (struct qrisc_symbol) {
         .kind = sk, .val = val, .name = strdup(name), .scale = scale,
      };
   }

   fclose(f);

   /* every reloc must refer to a label */
   for (unsigned i = 0; i < num_symbols; i++) {
      if (symbols[i].kind != SYM_RELOC)
         continue;
      bool found = false;
      for (unsigned j = 0; j < num_symbols; j++)
         found |= symbols[j].kind == SYM_LABEL && !strcmp(symbols[j].name, symbols[i].name);
      if (!found) {
         fprintf(stderr, "%s: reloc at 0x%x refers to unknown label %s\n", path,
                 symbols[i].val, symbols[i].name);
         return -1;
      }
   }
   return 0;
}

/**
 * The label relocation of the immediate of the instruction at offset, if any:
 * returns the label name and sets *target (its instruction offset) and *scale.
 */
const char *
qrisc_reloc_symbol(unsigned offset, unsigned *target, unsigned *scale)
{
   for (unsigned i = 0; i < num_symbols; i++) {
      if (symbols[i].kind != SYM_RELOC || symbols[i].val != offset)
         continue;
      for (unsigned j = 0; j < num_symbols; j++) {
         if (symbols[j].kind == SYM_LABEL && !strcmp(symbols[j].name, symbols[i].name)) {
            *target = symbols[j].val;
            *scale = symbols[i].scale;
            return symbols[i].name;
         }
      }
   }
   return NULL;
}

/**
 * Iterate over the labels that relocations refer to (they must be printed
 * even when they are not branch targets): returns the n-th, or NULL.
 */
const char *
qrisc_reloc_target(unsigned n, unsigned *target)
{
   for (unsigned i = 0; i < num_symbols; i++) {
      if (symbols[i].kind != SYM_LABEL)
         continue;
      bool referenced = false;
      for (unsigned j = 0; j < num_symbols; j++)
         referenced |= symbols[j].kind == SYM_RELOC && !strcmp(symbols[j].name, symbols[i].name);
      if (referenced && n-- == 0) {
         *target = symbols[i].val;
         return symbols[i].name;
      }
   }
   return NULL;
}

/**
 * Name of the label at instruction offset from the symbol file, or NULL
 */
const char *
qrisc_label_symbol(unsigned offset)
{
   for (unsigned i = 0; i < num_symbols; i++)
      if (symbols[i].kind == SYM_LABEL && symbols[i].val == offset)
         return symbols[i].name;
   return NULL;
}

static const char *
creg_symbol_name(unsigned id)
{
   for (unsigned i = 0; i < num_symbols; i++)
      if (symbols[i].kind == SYM_CREG && symbols[i].val == id)
         return symbols[i].name;
   return NULL;
}

static int
creg_symbol(const char *name)
{
   for (unsigned i = 0; i < num_symbols; i++)
      if (symbols[i].kind == SYM_CREG && !strcmp(symbols[i].name, name))
         return symbols[i].val;
   return -1;
}

static unsigned
reg(struct rnndomain *dom, const char *type, const char *name)
{
   int val = find_reg(dom, name);
   if (val < 0) {
      char *endptr = NULL;
      val = strtol(name, &endptr, 0);
      if (endptr && *endptr) {
         printf("invalid %s reg: %s\n", type, name);
         exit(2);
      }
   }
   return (unsigned)val;
}

static char *
reg_name(struct rnndomain *dom, unsigned id)
{
   if (rnndec_checkaddr(ctx, dom, id, 0)) {
      struct rnndecaddrinfo *info = rnndec_decodeaddr(ctx, dom, id, 0);
      char *name = info->name;
      free(info);
      return name;
   } else {
      return NULL;
   }
}

/**
 * Map control reg name to offset.
 */
unsigned
qrisc_control_reg(const char *name)
{
   int sym = creg_symbol(name);
   if (sym >= 0)
      return sym;
   return reg(control_regs, "control", name);
}

/**
 * Map offset to SQE reg name (or NULL), caller frees
 */
char *
qrisc_sqe_reg_name(unsigned id)
{
   return reg_name(sqe_regs, id);
}

/**
 * Map SQE reg name to offset.
 */
unsigned
qrisc_sqe_reg(const char *name)
{
   return reg(sqe_regs, "SQE", name);
}

/**
 * Map offset to control reg name (or NULL), caller frees
 */
char *
qrisc_control_reg_name(unsigned id)
{
   const char *sym = creg_symbol_name(id);
   if (sym)
      return strdup(sym);
   return reg_name(control_regs, id);
}

/**
 * Map pipe reg name to offset.
 */
unsigned
qrisc_pipe_reg(const char *name)
{
   return reg(pipe_regs, "pipe", name);
}

/**
 * "void" pipe regs don't have a value written, the $addr right is
 * enough to trigger what they do
 */
bool
qrisc_pipe_reg_is_void(unsigned id)
{
   if (rnndec_checkaddr(ctx, pipe_regs, id, 0)) {
      struct rnndecaddrinfo *info = rnndec_decodeaddr(ctx, pipe_regs, id, 0);
      free(info->name);
      bool ret = !strcmp(info->typeinfo->name, "void");
      free(info);
      return ret;
   } else {
      return false;
   }
}

/**
 * Map offset to pipe reg name (or NULL), caller frees
 */
char *
qrisc_pipe_reg_name(unsigned id)
{
   return reg_name(pipe_regs, id);
}

/**
 * Map GPU reg name to offset.
 */
unsigned
qrisc_gpu_reg(const char *name)
{
   int val = find_reg(dom[0], name);
   if (val < 0)
      val = find_reg(dom[1], name);
   if (val < 0) {
      char *endptr = NULL;
      val = strtol(name, &endptr, 0);
      if (endptr && *endptr) {
         printf("invalid control reg: %s\n", name);
         exit(2);
      }
   }
   return (unsigned)val;
}

/**
 * Map offset to gpu reg name (or NULL), caller frees
 */
char *
qrisc_gpu_reg_name(unsigned id)
{
   struct rnndomain *d = NULL;

   if (rnndec_checkaddr(ctx, dom[0], id, 0)) {
      d = dom[0];
   } else if (rnndec_checkaddr(ctx, dom[1], id, 0)) {
      d = dom[1];
   }

   if (d) {
      struct rnndecaddrinfo *info = rnndec_decodeaddr(ctx, d, id, 0);
      if (info) {
         char *name = info->name;
         free(info);
         return name;
      }
   }

   return NULL;
}

unsigned
qrisc_gpr_reg(const char *name)
{
   /* If it starts with '$' just swallow it: */
   if (name[0] == '$')
      name++;

   /* handle aliases: */
   if (!strcmp(name, "rem")) {
      return REG_REM;
   } else if (!strcmp(name, "memdata")) {
      return REG_MEMDATA;
   } else if (!strcmp(name, "addr")) {
      return REG_ADDR;
   } else if (!strcmp(name, "regdata")) {
      return REG_REGDATA;
   } else if (!strcmp(name, "usraddr")) {
      return REG_USRADDR;
   } else if (!strcmp(name, "data")) {
      return REG_DATA;
   } else {
      char *endptr = NULL;
      unsigned val = strtol(name, &endptr, 16);
      if (endptr && *endptr) {
         printf("invalid gpr reg: %s\n", name);
         exit(2);
      }
      return val;
   }
}

static int
find_enum_val(struct rnnenum *en, const char *name)
{
   int i;

   for (i = 0; i < en->valsnum; i++)
      if (en->vals[i]->valvalid && !strcmp(name, en->vals[i]->name))
         return en->vals[i]->value;

   return -1;
}

/**
 * Map pm4 packet name to id
 */
int
qrisc_pm4_id(const char *name)
{
   return find_enum_val(pm4_packets, name);
}

const char *
qrisc_pm_id_name(unsigned id)
{
   return rnndec_decode_enum(ctx, "adreno_pm4_type3_packets", id);
}

void
qrisc_printc(enum qrisc_color c, const char *fmt, ...)
{
   va_list args;
   if (c == QRISC_ERR) {
      printf("%s", ctx->colors->err);
   } else if (c == QRISC_LBL) {
      printf("%s", ctx->colors->btarg);
   }
   va_start(args, fmt);
   vprintf(fmt, args);
   va_end(args);
   printf("%s", ctx->colors->reset);
}

int qrisc_util_init(enum qrisc_fwid fw_id, int *gpuver_out, bool colors)
{
   char *name, *control_reg_name, *variant;
   char *pipe_reg_name = NULL;

   switch (fw_id) {
   case QRISC_GEN80000:
   case QRISC_GEN80100:
   case QRISC_GEN80200:
      name = "A6XX";
      variant = "A8XX";
      control_reg_name = "A7XX_GEN3_CONTROL_REG";
      pipe_reg_name = "A7XX_PIPE_REG";
      *gpuver_out = 8;
      break;
   case QRISC_A750:
      name = "A6XX";
      variant = "A7XX";
      control_reg_name = "A7XX_GEN3_CONTROL_REG";
      pipe_reg_name = "A7XX_PIPE_REG";
      *gpuver_out = 7;
      break;
   case QRISC_A730:
   case QRISC_A740:
   case QRISC_GEN70500:
   case QRISC_GEN71500:
   case QRISC_GEN71700:
      name = "A6XX";
      variant = "A7XX";
      control_reg_name = "A7XX_CONTROL_REG";
      pipe_reg_name = "A7XX_PIPE_REG";
      *gpuver_out = 7;
      break;
   case QRISC_A630:
   case QRISC_A650:
   case QRISC_A660:
   case QRISC_A702:
      name = "A6XX";
      variant = "A6XX";
      control_reg_name = "A6XX_CONTROL_REG";
      pipe_reg_name = "A6XX_PIPE_REG";
      *gpuver_out = 6;
      break;
   case QRISC_A530:
      name = "A5XX";
      variant = "A5XX";
      control_reg_name = "A5XX_CONTROL_REG";
      pipe_reg_name = "A5XX_PIPE_REG";
      *gpuver_out = 5;
      break;
   default:
      fprintf(stderr, "unknown GPU version %03x!\n", fw_id);
      return -1;
   }

   rnn_init();
   db = rnn_newdb();

   ctx = rnndec_newcontext(db);
   ctx->colors = colors ? &envy_def_colors : &envy_null_colors;

   rnn_parsefile(db, "adreno.xml");
   rnn_prepdb(db);
   if (db->estatus)
      errx(db->estatus, "failed to parse register database");
   dom[0] = rnn_finddomain(db, name);
   dom[1] = rnn_finddomain(db, "AXXX");
   control_regs = rnn_finddomain(db, control_reg_name);
   sqe_regs = rnn_finddomain(db, "A6XX_SQE_REG");
   pipe_regs = rnn_finddomain(db, pipe_reg_name);

   rnndec_varadd(ctx, "chip", variant);

   pm4_packets = rnn_findenum(ctx->db, "adreno_pm4_type3_packets");

   return 0;
}

