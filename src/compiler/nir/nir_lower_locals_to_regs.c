/*
 * Copyright © 2014 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Jason Ekstrand (jason@jlekstrand.net)
 *
 */

#include "nir.h"
#include "nir_builder.h"

struct locals_to_regs_state {
   nir_builder builder;

   /* A hash table mapping derefs to registers */
   struct hash_table *regs_table;

   bool progress;
};

/* The following two functions implement a hash and equality check for
 * variable dreferences.  When the hash or equality function encounters an
 * array, it ignores the offset and whether it is direct or indirect
 * entirely.
 */
static uint32_t
hash_deref(const void *void_deref)
{
   uint32_t hash = _mesa_fnv32_1a_offset_bias;

   for (const nir_deref_instr *deref = void_deref; deref;
        deref = nir_deref_instr_parent(deref)) {
      switch (deref->deref_type) {
      case nir_deref_type_var:
         return _mesa_fnv32_1a_accumulate(hash, deref->var);

      case nir_deref_type_array:
         continue; /* Do nothing */

      case nir_deref_type_struct:
         hash = _mesa_fnv32_1a_accumulate(hash, deref->strct.index);
         continue;

      default:
         unreachable("Invalid deref type");
      }
   }

   unreachable("We should have hit a variable dereference");
}

static bool
derefs_equal(const void *void_a, const void *void_b)
{
   for (const nir_deref_instr *a = void_a, *b = void_b; a || b;
        a = nir_deref_instr_parent(a), b = nir_deref_instr_parent(b)) {
      if (a->deref_type != b->deref_type)
         return false;

      switch (a->deref_type) {
      case nir_deref_type_var:
         return a->var == b->var;

      case nir_deref_type_array:
         continue; /* Do nothing */

      case nir_deref_type_struct:
         if (a->strct.index != b->strct.index)
            return false;
         continue;

      default:
         unreachable("Invalid deref type");
      }
   }

   unreachable("We should have hit a variable dereference");
}

static nir_register *
get_reg_for_deref(nir_deref_instr *deref, struct locals_to_regs_state *state)
{
   uint32_t hash = hash_deref(deref);

   assert(nir_deref_instr_get_variable(deref)->constant_initializer == NULL);

   struct hash_entry *entry =
      _mesa_hash_table_search_pre_hashed(state->regs_table, hash, deref);
   if (entry)
      return entry->data;

   unsigned array_size = 1;
   for (nir_deref_instr *d = deref; d; d = nir_deref_instr_parent(d)) {
      if (d->deref_type == nir_deref_type_array)
         array_size *= glsl_get_length(nir_deref_instr_parent(d)->type);
   }

   assert(glsl_type_is_vector_or_scalar(deref->type));

   nir_register *reg = nir_local_reg_create(state->builder.impl);
   reg->num_components = glsl_get_vector_elements(deref->type);
   reg->num_array_elems = array_size > 1 ? array_size : 0;
   reg->bit_size = glsl_get_bit_size(deref->type);

   _mesa_hash_table_insert_pre_hashed(state->regs_table, hash, deref, reg);

   return reg;
}

static nir_src
get_deref_reg_src(nir_deref_instr *deref, struct locals_to_regs_state *state)
{
   nir_builder *b = &state->builder;

   nir_src src;

   src.is_ssa = false;
   src.reg.reg = get_reg_for_deref(deref, state);
   src.reg.base_offset = 0;
   src.reg.indirect = NULL;

   /* It is possible for a user to create a shader that has an array with a
    * single element and then proceed to access it indirectly.  Indirectly
    * accessing a non-array register is not allowed in NIR.  In order to
    * handle this case we just convert it to a direct reference.
    */
   if (src.reg.reg->num_array_elems == 0)
      return src;

   unsigned inner_array_size = 1;
   for (const nir_deref_instr *d = deref; d; d = nir_deref_instr_parent(d)) {
      if (d->deref_type != nir_deref_type_array)
         continue;

      nir_const_value *const_index = nir_src_as_const_value(d->arr.index);
      if (const_index && !src.reg.indirect) {
         src.reg.base_offset += const_index->u32[0] * inner_array_size;
      } else {
         if (src.reg.indirect) {
            assert(src.reg.base_offset == 0);
         } else {
            src.reg.indirect = ralloc(b->shader, nir_src);
            *src.reg.indirect =
               nir_src_for_ssa(nir_imm_int(b, src.reg.base_offset));
            src.reg.base_offset = 0;
         }

         assert(src.reg.indirect->is_ssa);
         src.reg.indirect->ssa =
            nir_iadd(b, src.reg.indirect->ssa,
                        nir_imul(b, nir_ssa_for_src(b, d->arr.index, 1),
                                    nir_imm_int(b, inner_array_size)));
      }

      inner_array_size *= glsl_get_length(nir_deref_instr_parent(d)->type);
   }

   return src;
}

static bool
lower_locals_to_regs_block(nir_block *block,
                           struct locals_to_regs_state *state)
{
   nir_builder *b = &state->builder;

   nir_foreach_instr_safe(instr, block) {
      if (instr->type != nir_instr_type_intrinsic)
         continue;

      nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);

      switch (intrin->intrinsic) {
      case nir_intrinsic_load_deref: {
         nir_deref_instr *deref = nir_src_as_deref(intrin->src[0]);
         if (deref->mode != nir_var_local)
            continue;

         b->cursor = nir_before_instr(&intrin->instr);

         nir_alu_instr *mov = nir_alu_instr_create(b->shader, nir_op_imov);
         mov->src[0].src = get_deref_reg_src(deref, state);
         mov->dest.write_mask = (1 << intrin->num_components) - 1;
         if (intrin->dest.is_ssa) {
            nir_ssa_dest_init(&mov->instr, &mov->dest.dest,
                              intrin->num_components,
                              intrin->dest.ssa.bit_size, NULL);
            nir_ssa_def_rewrite_uses(&intrin->dest.ssa,
                                     nir_src_for_ssa(&mov->dest.dest.ssa));
         } else {
            nir_dest_copy(&mov->dest.dest, &intrin->dest, &mov->instr);
         }
         nir_builder_instr_insert(b, &mov->instr);

         nir_instr_remove(&intrin->instr);
         state->progress = true;
         break;
      }

      case nir_intrinsic_store_deref: {
         nir_deref_instr *deref = nir_src_as_deref(intrin->src[0]);
         if (deref->mode != nir_var_local)
            continue;

         b->cursor = nir_before_instr(&intrin->instr);

         nir_src reg_src = get_deref_reg_src(deref, state);

         nir_alu_instr *mov = nir_alu_instr_create(b->shader, nir_op_imov);
         nir_src_copy(&mov->src[0].src, &intrin->src[1], mov);
         mov->dest.write_mask = nir_intrinsic_write_mask(intrin);
         mov->dest.dest.is_ssa = false;
         mov->dest.dest.reg.reg = reg_src.reg.reg;
         mov->dest.dest.reg.base_offset = reg_src.reg.base_offset;
         mov->dest.dest.reg.indirect = reg_src.reg.indirect;

         nir_builder_instr_insert(b, &mov->instr);

         nir_instr_remove(&intrin->instr);
         state->progress = true;
         break;
      }

      case nir_intrinsic_copy_deref:
         unreachable("There should be no copies whatsoever at this point");
         break;

      default:
         continue;
      }
   }

   return true;
}

static bool
nir_lower_locals_to_regs_impl(nir_function_impl *impl)
{
   struct locals_to_regs_state state;

   nir_builder_init(&state.builder, impl);
   state.progress = false;
   state.regs_table = _mesa_hash_table_create(NULL, hash_deref, derefs_equal);

   nir_metadata_require(impl, nir_metadata_dominance);

   nir_foreach_block(block, impl) {
      lower_locals_to_regs_block(block, &state);
   }

   nir_metadata_preserve(impl, nir_metadata_block_index |
                               nir_metadata_dominance);

   _mesa_hash_table_destroy(state.regs_table, NULL);

   return state.progress;
}

bool
nir_lower_locals_to_regs(nir_shader *shader)
{
   bool progress = false;

   nir_foreach_function(function, shader) {
      if (function->impl)
         progress = nir_lower_locals_to_regs_impl(function->impl) || progress;
   }

   return progress;
}
