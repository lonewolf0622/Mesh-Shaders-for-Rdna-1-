/* SPDX-License-Identifier: MIT */
#include "radv_bc250_mesh_contract.h"

#include <stdlib.h>
#include <string.h>

#include "util/bitset.h"
#include "util/hash_table.h"
#include "util/set.h"
#include "util/u_dynarray.h"

bool
radv_bc250_mesh_intrinsic_subgroup_op(const nir_intrinsic_instr *in)
{
   if (in->intrinsic == nir_intrinsic_load_subgroup_id || in->intrinsic == nir_intrinsic_load_num_subgroups)
      return false;
   static const char *const keys[] = {
      "subgroup",
      "ballot",
      "vote",
      "reduce",
      "scan",
      "shuffle",
      "quad",
      "elect",
      "read_invocation",
      "read_first_invocation",
      "rotate",
      "swizzle",
      "first_invocation",
      "last_invocation",
      "as_uniform",
      "dpp",
      "permlane",
      "mbcnt",
      "write_invocation",
   };
   const char *name = nir_intrinsic_infos[in->intrinsic].name;
   for (unsigned i = 0; i < ARRAY_SIZE(keys); i++) {
      if (strstr(name, keys[i]))
         return true;
   }
   return false;
}

unsigned
radv_bc250_mesh_intrinsic_memory_effects(const nir_intrinsic_instr *in)
{
   const char *name = nir_intrinsic_infos[in->intrinsic].name;
   unsigned effects = 0;
   bool external_read;
   const bool deref_read = in->intrinsic == nir_intrinsic_load_deref || in->intrinsic == nir_intrinsic_copy_deref;
   const unsigned source = in->intrinsic == nir_intrinsic_copy_deref ? 1 : 0;
   if (deref_read) {
      nir_deref_instr *d = nir_src_as_deref(in->src[source]);
      external_read = nir_deref_mode_may_be(d, nir_var_mem_ssbo | nir_var_mem_global | nir_var_mem_ubo | nir_var_image);
      if (nir_deref_mode_may_be(d, nir_var_mem_shared))
         effects |= RADV_BC250_SHARED_READ;
   } else {
      external_read = !strncmp(name, "load_ssbo", 9) || !strncmp(name, "load_ubo", 8) ||
                      !strncmp(name, "load_global", 11) || !strncmp(name, "load_buffer", 11) ||
                      (strstr(name, "image") && strstr(name, "load"));
   }
   if (external_read) {
      effects |= RADV_BC250_EXTERNAL_READ;
      bool ubo = deref_read ? !nir_deref_mode_may_be(nir_src_as_deref(in->src[source]),
                                                     nir_var_mem_ssbo | nir_var_mem_global | nir_var_image)
                            : !strncmp(name, "load_ubo", 8);
      unsigned access = in->intrinsic == nir_intrinsic_copy_deref ? nir_intrinsic_src_access(in)
                        : nir_intrinsic_has_access(in)            ? nir_intrinsic_access(in)
                                                                  : 0;
      if (!ubo && !(access & ACCESS_NON_WRITEABLE))
         effects |= RADV_BC250_STORAGE_READ;
   }
   if (nir_intrinsic_writes_external_memory(in)) {
      effects |= RADV_BC250_EXTERNAL_WRITE;
      if (nir_intrinsic_has_atomic_op(in) || strstr(name, "atomic"))
         effects |= RADV_BC250_EXTERNAL_ATOMIC;
   }
   if (strstr(name, "shared")) {
      if (strstr(name, "load"))
         effects |= RADV_BC250_SHARED_READ;
      if (strstr(name, "store"))
         effects |= RADV_BC250_SHARED_WRITE;
      if (strstr(name, "atomic") || (nir_intrinsic_has_access(in) && (nir_intrinsic_access(in) & ACCESS_ATOMIC)))
         effects |= RADV_BC250_SHARED_ATOMIC | RADV_BC250_SHARED_READ | RADV_BC250_SHARED_WRITE;
   }
   if (in->intrinsic == nir_intrinsic_store_deref || in->intrinsic == nir_intrinsic_copy_deref) {
      nir_deref_instr *d = nir_src_as_deref(in->src[0]);
      if (nir_deref_mode_may_be(d, nir_var_mem_shared))
         effects |= RADV_BC250_SHARED_WRITE;
      if (d->modes & ~(nir_var_shader_out | nir_var_mem_shared | nir_var_shader_temp | nir_var_function_temp))
         effects |= RADV_BC250_EXTERNAL_WRITE;
   }
   if ((nir_intrinsic_has_access(in) && (nir_intrinsic_access(in) & ACCESS_VOLATILE)) ||
       (in->intrinsic == nir_intrinsic_copy_deref &&
        ((nir_intrinsic_src_access(in) | nir_intrinsic_dst_access(in)) & ACCESS_VOLATILE)))
      effects |= RADV_BC250_VOLATILE_ACCESS;
   if (in->intrinsic == nir_intrinsic_barrier &&
       (nir_intrinsic_memory_modes(in) & ~(nir_var_mem_shared | nir_var_shader_out)))
      effects |= RADV_BC250_EXTERNAL_BARRIER;
   return effects;
}

enum output_dependency {
   dependency_subgroup = 1,
   dependency_shared = 2,
   dependency_unknown = 4,
};

struct dependency_state {
   unsigned depth, dependencies;
   uint8_t *cache;
};

static unsigned output_dependencies(nir_def *def, unsigned depth, uint8_t *cache);

static bool
source_dependencies(nir_src *src, void *data)
{
   struct dependency_state *state = data;
   state->dependencies |= output_dependencies(src->ssa, state->depth + 1, state->cache);
   return true;
}

static unsigned
output_dependencies(nir_def *def, unsigned depth, uint8_t *cache)
{
   if (depth > 32)
      return dependency_unknown;
   if (cache[def->index])
      return cache[def->index] == 1 ? dependency_unknown : cache[def->index] - 2;
   cache[def->index] = 1;
   nir_instr *instr = nir_def_instr(def);
   struct dependency_state state = {.depth = depth, .cache = cache};
   if (instr->type == nir_instr_type_intrinsic) {
      nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);
      if (radv_bc250_mesh_intrinsic_subgroup_op(in) || in->intrinsic == nir_intrinsic_load_subgroup_id ||
          in->intrinsic == nir_intrinsic_load_num_subgroups)
         state.dependencies |= dependency_subgroup;
      const unsigned effects = radv_bc250_mesh_intrinsic_memory_effects(in);
      if (effects & RADV_BC250_SHARED_READ)
         state.dependencies |= dependency_shared | dependency_unknown;
      if (effects & (RADV_BC250_STORAGE_READ | RADV_BC250_EXTERNAL_ATOMIC))
         state.dependencies |= dependency_unknown;
      if (in->intrinsic == nir_intrinsic_load_deref &&
          nir_deref_mode_may_be(nir_src_as_deref(in->src[0]),
                                nir_var_function_temp | nir_var_shader_temp | nir_var_shader_out))
         state.dependencies |= dependency_unknown;
   }
   if (instr->type == nir_instr_type_phi) {
      nir_cf_node *prev = nir_cf_node_prev(&instr->block->cf_node);
      if (prev && prev->type == nir_cf_node_if)
         source_dependencies(&nir_cf_node_as_if(prev)->condition, &state);
      else
         state.dependencies |= dependency_unknown;
   }
   nir_foreach_src(instr, source_dependencies, &state);
   cache[def->index] = state.dependencies + 2;
   return state.dependencies;
}

enum allocation_paths {
   allocation_none = 1,
   allocation_once = 2,
   allocation_repeated = 4,
};

static unsigned
allocate(unsigned paths)
{
   return ((paths & allocation_none) ? allocation_once : 0) |
          ((paths & (allocation_once | allocation_repeated)) ? allocation_repeated : 0);
}

static bool
writes_mesh_output(const nir_intrinsic_instr *in)
{
   switch (in->intrinsic) {
   case nir_intrinsic_store_deref:
   case nir_intrinsic_copy_deref:
      return nir_deref_mode_is(nir_src_as_deref(in->src[0]), nir_var_shader_out);
   case nir_intrinsic_store_output:
   case nir_intrinsic_store_per_vertex_output:
   case nir_intrinsic_store_per_primitive_output:
      return true;
   default:
      return false;
   }
}

static void
analyze_mesh_contract(nir_shader *mesh, struct radv_bc250_mesh_contract *c)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(mesh);
   nir_foreach_function_impl (other, mesh) {
      /* The current routes require calls to be inlined. Do not silently analyze
       * the entrypoint while an allocation could be hidden in another function.
       */
      if (other != impl)
         return;
   }
   nir_metadata_require(impl, nir_metadata_block_index);
   unsigned *out = calloc(impl->num_blocks, sizeof(*out));
   if (!out)
      return;

   /* Track all reachable path counts, saturating at two. Union at joins keeps
    * mutually exclusive allocations distinct from sequential allocations. The
    * fixed point also detects allocations reached again through a loop edge.
    * No instructions, count values or output stores are moved by this analysis.
    */
   bool progress;
   do {
      progress = false;
      nir_foreach_block (block, impl) {
         unsigned paths = block == nir_start_block(impl) ? allocation_none : 0;
         nir_foreach_pred (pred, block)
            paths |= out[pred->index];
         nir_foreach_instr (instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);
            if (in->intrinsic == nir_intrinsic_set_vertex_and_primitive_count)
               paths = allocate(paths);
            else if (writes_mesh_output(in) && (paths & allocation_none))
               c->output_before_allocation = true;
         }
         c->allocation_may_repeat |= paths & allocation_repeated;
         if (out[block->index] != paths) {
            out[block->index] = paths;
            progress = true;
         }
      }
   } while (progress);

   nir_foreach_pred (pred, impl->end_block)
      c->allocation_may_be_skipped |= out[pred->index] & allocation_none;

   /* A subgroup-uniform branch can still choose different sites in different
    * waves. Mesh allocation control flow must be uniform across the workgroup.
    */
   nir_custom_divergence_analysis(mesh, nir_divergence_across_subgroups);
   nir_foreach_block (block, impl) {
      unsigned paths = block == nir_start_block(impl) ? allocation_none : 0;
      nir_foreach_pred (pred, block)
         paths |= out[pred->index];
      if (!paths)
         continue;
      nir_foreach_instr (instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);
         if (in->intrinsic != nir_intrinsic_set_vertex_and_primitive_count)
            continue;
         c->allocation_sites++;
         c->allocation_control_uniform &= !block->divergent;
         c->allocation_values_uniform &= !in->src[0].ssa->divergent && !in->src[1].ssa->divergent;
      }
   }
   c->allocation_analysis_complete = true;
   free(out);
}

void
radv_bc250_analyze_mesh_contract(nir_shader *mesh, struct radv_bc250_mesh_contract *c)
{
   radv_bc250_mesh_contract_init(mesh, c);
   radv_bc250_analyze_mesh_allocation(mesh, c);
}

void
radv_bc250_analyze_mesh_allocation(nir_shader *mesh, struct radv_bc250_mesh_contract *c)
{
   c->allocation_sites = 0;
   c->allocation_analysis_complete = false;
   c->allocation_may_repeat = c->allocation_may_be_skipped = c->output_before_allocation = false;
   c->allocation_control_uniform = c->allocation_values_uniform = true;
   /* Divergence analysis requires lowered returns. Keep the route's original
    * program and metadata intact while normalizing a private analysis copy.
    */
   nir_shader *normalized = nir_shader_clone(NULL, mesh);
   nir_lower_returns(normalized);
   analyze_mesh_contract(normalized, c);
   ralloc_free(normalized);
}

bool
radv_bc250_mesh_allocation_is_canonical(const struct radv_bc250_mesh_contract *c)
{
   return c->allocation_analysis_complete && c->allocation_sites && !c->allocation_may_repeat &&
          c->allocation_control_uniform && c->allocation_values_uniform && !c->output_before_allocation;
}

static bool
output_is_lane(nir_scalar s)
{
   s = nir_scalar_chase_movs(s);
   return nir_scalar_is_intrinsic(s) && nir_scalar_intrinsic_op(s) == nir_intrinsic_load_local_invocation_index;
}

/* The vertex index is invocation + k * threads (k >= 0): the invocation that stores vertex v is v % threads. */
static bool
output_owned(nir_scalar s, unsigned threads, unsigned depth)
{
   s = nir_scalar_chase_movs(s);
   if (depth > 6)
      return false;
   if (output_is_lane(s))
      return true;
   if (nir_scalar_is_alu(s) && nir_scalar_alu_op(s) == nir_op_iadd) {
      for (unsigned i = 0; i < 2; i++) {
         nir_scalar o = nir_scalar_chase_alu_src(s, !i);
         if (nir_scalar_is_const(o) && nir_scalar_as_uint(o) % threads == 0 &&
             output_owned(nir_scalar_chase_alu_src(s, i), threads, depth + 1))
            return true;
      }
   }
   if (nir_def_instr(s.def)->type == nir_instr_type_phi) {
      nir_phi_instr *phi = nir_instr_as_phi(nir_def_instr(s.def));
      nir_cf_node *parent = phi->instr.block->cf_node.parent;
      if (parent->type != nir_cf_node_loop || nir_loop_first_block(nir_cf_node_as_loop(parent)) != phi->instr.block)
         return false;
      unsigned inits = 0;
      nir_foreach_phi_src (src, phi) {
         nir_scalar v = nir_scalar_chase_movs(nir_get_scalar(src->src.ssa, 0));
         if (nir_scalar_is_alu(v) && nir_scalar_alu_op(v) == nir_op_iadd) {
            nir_scalar a = nir_scalar_chase_alu_src(v, 0), c = nir_scalar_chase_alu_src(v, 1);
            if ((a.def == &phi->def && nir_scalar_is_const(c) && nir_scalar_as_uint(c) % threads == 0) ||
                (c.def == &phi->def && nir_scalar_is_const(a) && nir_scalar_as_uint(a) % threads == 0))
               continue;
         }
         if (!output_owned(v, threads, depth + 1))
            return false;
         inits++;
      }
      return inits > 0;
   }
   return false;
}

/* The deref of a Mesh output array element: the vertex (or primitive) level array deref right below the variable. */
nir_deref_instr *
radv_bc250_mesh_output_element(nir_deref_instr *d)
{
   while (d && d->deref_type != nir_deref_type_var) {
      nir_deref_instr *parent = nir_deref_instr_parent(d);
      if (parent && parent->deref_type == nir_deref_type_var)
         return d->deref_type == nir_deref_type_array ? d : NULL;
      d = parent;
   }
   return NULL;
}

void
radv_bc250_mesh_contract_init(const nir_shader *mesh, struct radv_bc250_mesh_contract *c)
{
   *c = (struct radv_bc250_mesh_contract){
      .stage = mesh->info.stage,
      .primitive_type = mesh->info.mesh.primitive_type,
      .workgroup_size = {mesh->info.workgroup_size[0], mesh->info.workgroup_size[1], mesh->info.workgroup_size[2]},
      .workgroup_invocations =
         mesh->info.workgroup_size[0] * mesh->info.workgroup_size[1] * mesh->info.workgroup_size[2],
      .max_vertices = mesh->info.mesh.max_vertices_out,
      .max_primitives = mesh->info.mesh.max_primitives_out,
      .task_payload_size = mesh->info.task_payload_size,
      .shared_size = mesh->info.shared_size,
      .min_subgroup_size = mesh->info.min_subgroup_size,
      .max_subgroup_size = mesh->info.max_subgroup_size,
      .api_subgroup_size = mesh->info.api_subgroup_size,
      .nv = mesh->info.mesh.nv,
      .workgroup_size_variable = mesh->info.workgroup_size_variable,
      .shared_memory_explicit_layout = mesh->info.shared_memory_explicit_layout,
      .zero_initialize_shared_memory = mesh->info.zero_initialize_shared_memory,
      .derivative_group = mesh->info.derivative_group,
      .outputs_written = mesh->info.outputs_written,
      .outputs_read = mesh->info.outputs_read,
      .per_primitive_outputs = mesh->info.per_primitive_outputs,
      .outputs_written_16bit = mesh->info.outputs_written_16bit,
      .allocation_control_uniform = true,
      .allocation_values_uniform = true,
   };
   nir_foreach_shader_out_variable (v, mesh) {
      c->non_array_output |= !glsl_type_is_array(v->type);
      if (v->data.per_primitive) {
         if ((unsigned)v->data.location < 64)
            c->declared_per_primitive_outputs |= BITFIELD64_BIT(v->data.location);
         if (v->data.location == VARYING_SLOT_CULL_PRIMITIVE)
            c->has_per_primitive_cull = true;
         else if (v->data.location != VARYING_SLOT_PRIMITIVE_INDICES)
            c->has_per_primitive_data = true;
      }
      if (v->data.location == VARYING_SLOT_PRIMITIVE_INDICES) {
         c->triangle_index_array =
            glsl_type_is_array(v->type) && glsl_get_vector_elements(glsl_get_array_element(v->type)) == 3;
      }
      if (v->data.location >= VARYING_SLOT_VAR0) {
         const struct glsl_type *type = glsl_without_array(v->type);
         c->non_32bit_generic_output |=
            !(glsl_type_is_scalar(type) || glsl_type_is_vector(type) || glsl_type_is_matrix(type)) ||
            glsl_get_bit_size(type) != 32;
      }
   }
   nir_foreach_function_impl (impl, mesh) {
      c->function_count++;
      uint8_t *dependencies = calloc(MAX2(impl->ssa_alloc, 1), sizeof(*dependencies));
      if (!dependencies)
         c->output_dependencies_unknown = true;
      nir_foreach_block (block, impl) {
         /* Loops and early returns can control later constant output stores
          * outside the construct, without leaving an SSA value dependency.
          */
         for (nir_cf_node *node = block->cf_node.parent; node; node = node->parent) {
            if (node->type == nir_cf_node_loop)
               c->output_dependencies_unknown = true;
         }
         nir_foreach_instr (instr, block) {
            c->has_calls |= instr->type == nir_instr_type_call;
            if (instr->type == nir_instr_type_jump && nir_instr_as_jump(instr)->type == nir_jump_return)
               c->output_dependencies_unknown = true;
            if (instr->type == nir_instr_type_deref)
               c->uses_task_payload |= nir_deref_mode_may_be(nir_instr_as_deref(instr), nir_var_mem_task_payload);
            if (instr->type == nir_instr_type_tex)
               c->memory_effects |= RADV_BC250_EXTERNAL_READ;
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);
            const char *name = nir_intrinsic_infos[in->intrinsic].name;
            const unsigned effects = radv_bc250_mesh_intrinsic_memory_effects(in);
            c->memory_effects |= effects;
            c->used_external_atomic_result |= (effects & RADV_BC250_EXTERNAL_ATOMIC) && !nir_def_is_unused(&in->def);
            c->subgroup_ops |= radv_bc250_mesh_intrinsic_subgroup_op(in);
            c->observes_subgroup |= c->subgroup_ops || in->intrinsic == nir_intrinsic_load_subgroup_id ||
                                    in->intrinsic == nir_intrinsic_load_num_subgroups;
            c->uses_num_workgroups |= in->intrinsic == nir_intrinsic_load_num_workgroups;
            c->uses_task_payload |= strstr(name, "task_payload") != NULL;
            c->allocation_static_sites += in->intrinsic == nir_intrinsic_set_vertex_and_primitive_count;
            if (in->intrinsic == nir_intrinsic_store_output || in->intrinsic == nir_intrinsic_store_per_vertex_output ||
                in->intrinsic == nir_intrinsic_store_per_primitive_output) {
               c->output_dependencies_unknown = true;
               if (!c->output_store_refusal)
                  c->output_store_refusal = "lowered output store ownership unavailable";
            }
            if (in->intrinsic != nir_intrinsic_store_deref)
               continue;
            nir_deref_instr *d = nir_src_as_deref(in->src[0]);
            nir_variable *v = nir_deref_instr_get_variable(d);
            if (!v || v->data.mode != nir_var_shader_out)
               continue;
            nir_deref_instr *element = radv_bc250_mesh_output_element(d);
            const char *refusal = NULL;
            if (!element)
               refusal = "output store shape";
            else if (v->data.location == VARYING_SLOT_PRIMITIVE_INDICES) {
               if (element != d)
                  refusal = "index store shape";
            } else if (!c->workgroup_invocations ||
                       !output_owned(nir_get_scalar(element->arr.index.ssa, 0), c->workgroup_invocations, 0)) {
               refusal = "vertex store not owned by invocation % workgroup size";
            }
            if (!c->output_store_refusal)
               c->output_store_refusal = refusal;
            if (!dependencies)
               continue;
            struct dependency_state state = {.cache = dependencies};
            nir_foreach_src(instr, source_dependencies, &state);
            for (nir_cf_node *node = block->cf_node.parent; node; node = node->parent) {
               if (node->type == nir_cf_node_if)
                  source_dependencies(&nir_cf_node_as_if(node)->condition, &state);
               if (node->type == nir_cf_node_loop)
                  state.dependencies |= dependency_unknown;
            }
            const uint64_t bit = (unsigned)v->data.location < 64 ? BITFIELD64_BIT(v->data.location) : 0;
            if (state.dependencies & dependency_subgroup)
               c->subgroup_dependent_outputs |= bit;
            if (state.dependencies & dependency_shared)
               c->shared_dependent_outputs |= bit;
            c->output_dependencies_unknown |=
               (state.dependencies & dependency_unknown) || (unsigned)v->data.location >= 64;
         }
      }
      free(dependencies);
   }
   c->output_dependencies_unknown |= c->has_calls;
}

/* Split admission with workgroup-shared atomics (radv_bc250_split_mesh).
 *
 * The split runs the whole Mesh body once per piece, each piece in its own
 * workgroup, and keeps one slice of the primitive array from each piece. The
 * completion order of shared atomics may differ between pieces, so every value
 * that can depend on it ("tainted") may differ too. The pieces still agree on
 * the final picture as long as the slicing itself is the same in every piece:
 *
 *  - the primitive count, each per-primitive output's slot, and whether a
 *    per-primitive store runs at all must not be tainted;
 *  - per-primitive output values must not be tainted, except the primitive's
 *    vertex indices: every piece exports all of its own vertices from the same
 *    execution, so its indices and vertex outputs are consistent with each other.
 *
 * Taint sources: shared atomic results, atomic shared loads, and plain shared
 * loads whose byte range may overlap a range that an atomic, or a store of a
 * tainted value (or under tainted control), may write. Taint flows through every
 * SSA source, through phis after a branch on a tainted value, through every value
 * defined in a loop whose exit may depend on a tainted value, and through
 * function-temporary variables. Address ranges come from the constant part of
 * the offset (lower bound) and nir_unsigned_upper_bound (upper bound); an
 * unbounded address overlaps everything.
 */
struct bc250_split_taint {
   nir_shader *shader;
   struct hash_table *range_ht;
   BITSET_WORD *defs;
   struct set *vars;
   bool all_temps;
   bool outputs;
   struct util_dynarray ranges; /* uint32_t lo, hi pairs */
   bool changed;
   const char *failure;
};

static bool
bc250_taint_def(struct bc250_split_taint *t, const nir_def *def)
{
   return BITSET_TEST(t->defs, def->index);
}

static void
bc250_taint_set(struct bc250_split_taint *t, const nir_def *def)
{
   if (!BITSET_TEST(t->defs, def->index)) {
      BITSET_SET(t->defs, def->index);
      t->changed = true;
   }
}

static bool
bc250_taint_src_cb(nir_src *src, void *data)
{
   struct bc250_split_taint *t = data;
   return !bc250_taint_def(t, src->ssa); /* stop at the first tainted source */
}

static bool
bc250_taint_srcs(struct bc250_split_taint *t, nir_instr *instr)
{
   return !nir_foreach_src(instr, bc250_taint_src_cb, t);
}

static void
bc250_shared_range(struct bc250_split_taint *t, nir_intrinsic_instr *in, nir_src *offset, unsigned bytes,
                   uint32_t *lo, uint32_t *hi)
{
   const uint64_t base = nir_intrinsic_has_base(in) ? nir_intrinsic_base(in) : 0;
   uint64_t low = base;
   nir_scalar s = nir_get_scalar(offset->ssa, 0);
   while (true) {
      if (nir_scalar_is_const(s)) {
         low += nir_scalar_as_uint(s);
         break;
      }
      if (!nir_scalar_is_alu(s) || nir_scalar_alu_op(s) != nir_op_iadd)
         break;
      nir_scalar a = nir_scalar_chase_alu_src(s, 0), c = nir_scalar_chase_alu_src(s, 1);
      if (nir_scalar_is_const(a)) {
         low += nir_scalar_as_uint(a);
         s = c;
      } else if (nir_scalar_is_const(c)) {
         low += nir_scalar_as_uint(c);
         s = a;
      } else {
         break;
      }
   }
   const uint32_t ub = nir_unsigned_upper_bound(t->shader, t->range_ht, nir_get_scalar(offset->ssa, 0));
   const uint64_t high = ub == UINT32_MAX ? UINT32_MAX : MIN2(base + ub + bytes, UINT32_MAX);
   *lo = (uint32_t)MIN2(low, UINT32_MAX);
   *hi = (uint32_t)high;
   if (*lo >= *hi)
      *lo = 0;
}

static void
bc250_taint_add_range(struct bc250_split_taint *t, uint32_t lo, uint32_t hi)
{
   const uint32_t *r = t->ranges.data;
   const unsigned n = util_dynarray_num_elements(&t->ranges, uint32_t) / 2;
   for (unsigned i = 0; i < n; i++) {
      if (r[2 * i] <= lo && r[2 * i + 1] >= hi)
         return;
   }
   util_dynarray_append(&t->ranges, lo);
   util_dynarray_append(&t->ranges, hi);
   t->changed = true;
}

static bool
bc250_taint_overlaps(struct bc250_split_taint *t, uint32_t lo, uint32_t hi)
{
   const uint32_t *r = t->ranges.data;
   const unsigned n = util_dynarray_num_elements(&t->ranges, uint32_t) / 2;
   for (unsigned i = 0; i < n; i++) {
      if (lo < r[2 * i + 1] && r[2 * i] < hi)
         return true;
   }
   return false;
}

static void
bc250_taint_fail(struct bc250_split_taint *t, const char *why)
{
   if (!t->failure)
      t->failure = why;
}

static bool
bc250_taint_deref_indices(struct bc250_split_taint *t, nir_deref_instr *d)
{
   for (; d && d->deref_type != nir_deref_type_var; d = nir_deref_instr_parent(d)) {
      if ((d->deref_type == nir_deref_type_array || d->deref_type == nir_deref_type_ptr_as_array) &&
          bc250_taint_def(t, d->arr.index.ssa))
         return true;
   }
   return false;
}

static void
bc250_taint_temp(struct bc250_split_taint *t, nir_deref_instr *d)
{
   nir_variable *var = nir_deref_instr_get_variable(d);
   if (!var) {
      if (!t->all_temps)
         t->changed = true;
      t->all_temps = true;
   } else if (!_mesa_set_search(t->vars, var)) {
      _mesa_set_add(t->vars, var);
      t->changed = true;
   }
}

static void
bc250_taint_store_output(struct bc250_split_taint *t, nir_intrinsic_instr *in, nir_deref_instr *d,
                         nir_variable *var, bool ctrl)
{
   const bool value = bc250_taint_def(t, in->src[1].ssa);
   const bool index = bc250_taint_deref_indices(t, d);
   const bool primitive = var->data.per_primitive || var->data.location == VARYING_SLOT_PRIMITIVE_INDICES;
   if (!primitive) {
      if ((value || index || ctrl) && !t->outputs) {
         t->outputs = true;
         t->changed = true;
      }
      return;
   }
   if (ctrl)
      bc250_taint_fail(t, "shared atomics control a per-primitive store");
   else if (index)
      bc250_taint_fail(t, "shared atomics select a per-primitive slot");
   else if (value && var->data.location != VARYING_SLOT_PRIMITIVE_INDICES)
      bc250_taint_fail(t, "shared atomics reach a per-primitive output");
}

static void
bc250_taint_intrinsic(struct bc250_split_taint *t, nir_intrinsic_instr *in, bool ctrl)
{
   const bool srcs = bc250_taint_srcs(t, &in->instr);
   switch (in->intrinsic) {
   case nir_intrinsic_shared_atomic:
   case nir_intrinsic_shared_atomic_swap: {
      uint32_t lo, hi;
      bc250_shared_range(t, in, &in->src[0], in->def.bit_size / 8, &lo, &hi);
      bc250_taint_add_range(t, lo, hi);
      bc250_taint_set(t, &in->def);
      return;
   }
   case nir_intrinsic_load_shared: {
      uint32_t lo, hi;
      bc250_shared_range(t, in, &in->src[0], in->def.num_components * in->def.bit_size / 8, &lo, &hi);
      if (srcs || (nir_intrinsic_access(in) & ACCESS_ATOMIC) || bc250_taint_overlaps(t, lo, hi))
         bc250_taint_set(t, &in->def);
      return;
   }
   case nir_intrinsic_store_shared:
      if (ctrl || srcs || (nir_intrinsic_access(in) & ACCESS_ATOMIC)) {
         uint32_t lo, hi;
         if (bc250_taint_def(t, in->src[1].ssa)) {
            lo = 0;
            hi = UINT32_MAX;
         } else {
            bc250_shared_range(t, in, &in->src[1], in->src[0].ssa->num_components * in->src[0].ssa->bit_size / 8,
                               &lo, &hi);
         }
         bc250_taint_add_range(t, lo, hi);
      }
      return;
   case nir_intrinsic_set_vertex_and_primitive_count:
      if (ctrl || bc250_taint_def(t, in->src[1].ssa))
         bc250_taint_fail(t, "shared atomics reach the primitive count");
      return;
   case nir_intrinsic_load_deref: {
      nir_deref_instr *d = nir_src_as_deref(in->src[0]);
      nir_variable *var = nir_deref_instr_get_variable(d);
      bool tainted = srcs;
      if (nir_deref_mode_is(d, nir_var_shader_out))
         tainted |= t->outputs;
      else if (nir_deref_mode_may_be(d, nir_var_function_temp | nir_var_shader_temp))
         tainted |= t->all_temps || !var || _mesa_set_search(t->vars, var);
      else if (nir_deref_mode_may_be(d, nir_var_mem_shared))
         tainted = true;
      if (tainted)
         bc250_taint_set(t, &in->def);
      return;
   }
   case nir_intrinsic_store_deref: {
      nir_deref_instr *d = nir_src_as_deref(in->src[0]);
      nir_variable *var = nir_deref_instr_get_variable(d);
      if (nir_deref_mode_is(d, nir_var_shader_out)) {
         if (!var)
            bc250_taint_fail(t, "indirect output store");
         else
            bc250_taint_store_output(t, in, d, var, ctrl);
      } else if (nir_deref_mode_may_be(d, nir_var_mem_shared)) {
         bc250_taint_fail(t, "shared deref store");
      } else if (ctrl || srcs) {
         bc250_taint_temp(t, d);
      }
      return;
   }
   case nir_intrinsic_copy_deref:
      bc250_taint_fail(t, "copy_deref");
      return;
   default:
      if (nir_intrinsic_infos[in->intrinsic].has_dest && (srcs || ctrl))
         bc250_taint_set(t, &in->def);
      return;
   }
}

static void
bc250_taint_block(struct bc250_split_taint *t, nir_block *block, bool ctrl, bool phis)
{
   nir_foreach_instr(instr, block) {
      switch (instr->type) {
      case nir_instr_type_phi:
         if (phis || bc250_taint_srcs(t, instr))
            bc250_taint_set(t, &nir_instr_as_phi(instr)->def);
         break;
      case nir_instr_type_intrinsic:
         bc250_taint_intrinsic(t, nir_instr_as_intrinsic(instr), ctrl);
         break;
      case nir_instr_type_alu:
      case nir_instr_type_tex:
      case nir_instr_type_deref: {
         nir_def *def = nir_instr_def(instr);
         if (def && (ctrl || bc250_taint_srcs(t, instr)))
            bc250_taint_set(t, def);
         break;
      }
      case nir_instr_type_call:
         bc250_taint_fail(t, "uninlined call");
         break;
      default:
         break;
      }
   }
}

static bool
bc250_taint_branch_in(struct bc250_split_taint *t, struct exec_list *list)
{
   foreach_list_typed (nir_cf_node, node, node, list) {
      if (node->type == nir_cf_node_if) {
         nir_if *nif = nir_cf_node_as_if(node);
         if (bc250_taint_def(t, nif->condition.ssa) || bc250_taint_branch_in(t, &nif->then_list) ||
             bc250_taint_branch_in(t, &nif->else_list))
            return true;
      } else if (node->type == nir_cf_node_loop) {
         if (bc250_taint_branch_in(t, &nir_cf_node_as_loop(node)->body))
            return true;
      }
   }
   return false;
}

static void
bc250_taint_cf(struct bc250_split_taint *t, struct exec_list *list, bool ctrl)
{
   bool phis = false;
   foreach_list_typed (nir_cf_node, node, node, list) {
      switch (node->type) {
      case nir_cf_node_block:
         bc250_taint_block(t, nir_cf_node_as_block(node), ctrl, phis);
         phis = false;
         break;
      case nir_cf_node_if: {
         nir_if *nif = nir_cf_node_as_if(node);
         const bool tainted = ctrl || bc250_taint_def(t, nif->condition.ssa);
         bc250_taint_cf(t, &nif->then_list, tainted);
         bc250_taint_cf(t, &nif->else_list, tainted);
         phis = tainted;
         break;
      }
      case nir_cf_node_loop: {
         nir_loop *loop = nir_cf_node_as_loop(node);
         /* Values defined in such a loop (and its phis) depend on the trip count. */
         const bool tainted = ctrl || bc250_taint_branch_in(t, &loop->body);
         bc250_taint_cf(t, &loop->body, tainted);
         phis = tainted;
         break;
      }
      default:
         bc250_taint_fail(t, "unexpected control flow");
         break;
      }
   }
}

const char *
radv_bc250_split_shared_atomic_hazard(nir_shader *mesh)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(mesh);
   if (!impl)
      return "no entrypoint";
   nir_index_ssa_defs(impl);
   struct bc250_split_taint t = {
      .shader = mesh,
      .range_ht = _mesa_pointer_hash_table_create(NULL),
      .defs = calloc(BITSET_WORDS(impl->ssa_alloc), sizeof(BITSET_WORD)),
      .vars = _mesa_pointer_set_create(NULL),
   };
   util_dynarray_init(&t.ranges, NULL);
   if (!t.defs) {
      t.failure = "out of memory";
   } else {
      do {
         t.changed = false;
         bc250_taint_cf(&t, &impl->body, false);
      } while (t.changed && !t.failure);
   }
   const char *failure = t.failure;
   util_dynarray_fini(&t.ranges);
   _mesa_set_destroy(t.vars, NULL);
   _mesa_hash_table_destroy(t.range_ht, NULL);
   free(t.defs);
   return failure;
}

/* Expansion staging over dead application shared memory (radv_bc250_expand_primitive_attributes).
 *
 * The boundary is the first top-level CF node that stores an output. Shared bytes that only nodes before it
 * access are dead from the boundary on: with a workgroup barrier before the boundary, the output staging
 * arrays (first written at or after it) may reuse them. Every shared access at or after the boundary keeps
 * its bytes ([lo, hi) from the constant part of the address and nir_unsigned_upper_bound). */
static bool
bc250_alias_node_stores_output(nir_cf_node *node)
{
   bool found = false;
   nir_foreach_block_in_cf_node(block, node) {
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);
         if ((in->intrinsic == nir_intrinsic_store_deref || in->intrinsic == nir_intrinsic_copy_deref) &&
             nir_deref_mode_may_be(nir_src_as_deref(in->src[0]), nir_var_shader_out))
            found = true;
      }
   }
   return found;
}

static bool
bc250_alias_is_output_store(const nir_instr *instr)
{
   if (instr->type != nir_instr_type_intrinsic)
      return false;
   const nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);
   return (in->intrinsic == nir_intrinsic_store_deref || in->intrinsic == nir_intrinsic_copy_deref) &&
          nir_deref_mode_may_be(nir_src_as_deref(in->src[0]), nir_var_shader_out);
}

static bool
bc250_alias_late_ranges(struct bc250_split_taint *t, nir_cf_node *node, const nir_instr *from,
                        struct radv_bc250_staging_alias *alias)
{
   nir_foreach_block_in_cf_node(block, node) {
      bool late = !from;
      nir_foreach_instr(instr, block) {
         late |= instr == from;
         if (!late)
            continue;
         if (instr->type == nir_instr_type_deref) {
            if (nir_deref_mode_may_be(nir_instr_as_deref(instr), nir_var_mem_shared))
               return false; /* not lowered to explicit offsets: unknown bytes */
            continue;
         }
         if (instr->type == nir_instr_type_call)
            return false;
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);
         uint32_t lo, hi;
         switch (in->intrinsic) {
         case nir_intrinsic_load_shared:
            bc250_shared_range(t, in, &in->src[0], in->def.num_components * in->def.bit_size / 8, &lo, &hi);
            break;
         case nir_intrinsic_shared_atomic:
         case nir_intrinsic_shared_atomic_swap:
            bc250_shared_range(t, in, &in->src[0], in->def.bit_size / 8, &lo, &hi);
            break;
         case nir_intrinsic_store_shared:
            bc250_shared_range(t, in, &in->src[1], in->src[0].ssa->num_components * in->src[0].ssa->bit_size / 8,
                               &lo, &hi);
            break;
         default:
            if (strstr(nir_intrinsic_infos[in->intrinsic].name, "shared"))
               return false; /* another shared access form: unknown bytes */
            continue;
         }
         if (alias->num_ranges == ARRAY_SIZE(alias->ranges))
            return false;
         alias->ranges[alias->num_ranges][0] = lo;
         alias->ranges[alias->num_ranges][1] = hi;
         alias->num_ranges++;
      }
   }
   return true;
}

bool
radv_bc250_staging_alias_analyze(nir_shader *mesh, struct radv_bc250_staging_alias *alias)
{
   memset(alias, 0, sizeof(*alias));
   nir_function_impl *impl = nir_shader_get_entrypoint(mesh);
   if (!impl || !mesh->info.shared_size)
      return false;
   nir_cf_node *boundary = NULL;
   foreach_list_typed (nir_cf_node, node, node, &impl->body) {
      if (bc250_alias_node_stores_output(node)) {
         boundary = node;
         break;
      }
   }
   if (!boundary)
      return false;
   /* A top-level block: the boundary is right before its first output store. */
   nir_instr *first_store = NULL;
   if (boundary->type == nir_cf_node_block) {
      nir_foreach_instr(instr, nir_cf_node_as_block(boundary)) {
         if (bc250_alias_is_output_store(instr)) {
            first_store = instr;
            break;
         }
      }
   }
   /* Nothing runs before the first output store: nothing to reuse. */
   const bool first_node = boundary == exec_node_data(nir_cf_node, exec_list_get_head(&impl->body), node);
   if (first_node && (!first_store || first_store == nir_block_first_instr(nir_cf_node_as_block(boundary))))
      return false;
   struct bc250_split_taint t = {.shader = mesh, .range_ht = _mesa_pointer_hash_table_create(NULL)};
   bool ok = true;
   for (nir_cf_node *node = boundary; node && ok; node = nir_cf_node_next(node))
      ok = bc250_alias_late_ranges(&t, node, node == boundary ? first_store : NULL, alias);
   _mesa_hash_table_destroy(t.range_ht, NULL);
   if (!ok) {
      memset(alias, 0, sizeof(*alias));
      return false;
   }
   alias->boundary = boundary;
   alias->boundary_instr = first_store;
   return true;
}

bool
radv_bc250_staging_alias_free(const struct radv_bc250_staging_alias *alias, uint32_t lo, uint32_t hi)
{
   for (unsigned i = 0; i < alias->num_ranges; i++) {
      if (lo < alias->ranges[i][1] && alias->ranges[i][0] < hi)
         return false;
   }
   return true;
}
