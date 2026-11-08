/* SPDX-License-Identifier: MIT */
#include "tests/nir_test.h"
#include "radv_bc250_mesh_contract.h"

class radv_bc250_mesh_contract_test : public nir_test {
protected:
   radv_bc250_mesh_contract_test(): nir_test("radv_bc250_mesh_contract_test")
   {
      b->shader->info.stage = MESA_SHADER_MESH;
      b->shader->info.workgroup_size[0] = 128;
      b->shader->info.workgroup_size[1] = b->shader->info.workgroup_size[2] = 1;
      b->shader->info.mesh.max_vertices_out = 128;
      b->shader->info.mesh.max_primitives_out = 64;
      b->shader->info.mesh.primitive_type = MESA_PRIM_TRIANGLES;
   }

   nir_def *uniform_condition()
   {
      return nir_ine_imm(b, nir_channel(b, nir_load_workgroup_id(b), 0), 0);
   }

   void allocation(unsigned vertices = 128, unsigned primitives = 64)
   {
      nir_set_vertex_and_primitive_count(b, nir_imm_int(b, vertices), nir_imm_int(b, primitives), nir_imm_int(b, 0));
   }

   void output()
   {
      nir_variable *v =
         nir_variable_create(b->shader, nir_var_shader_out, glsl_array_type(glsl_vec4_type(), 128, 0), "position");
      v->data.location = VARYING_SLOT_POS;
      nir_store_deref(b, nir_build_deref_array(b, nir_build_deref_var(b, v), nir_load_local_invocation_index(b)),
                      nir_imm_vec4(b, 0, 0, 0, 1), 0xf);
   }

   void position(nir_def *index, nir_def *value)
   {
      nir_variable *v =
         nir_variable_create(b->shader, nir_var_shader_out, glsl_array_type(glsl_vec4_type(), 128, 0), "position");
      v->data.location = VARYING_SLOT_POS;
      nir_store_deref(b, nir_build_deref_array(b, nir_build_deref_var(b, v), index), value, 0xf);
   }

   struct radv_bc250_mesh_contract analyze()
   {
      nir_validate_shader(b->shader, "before Mesh contract analysis");
      struct radv_bc250_mesh_contract c;
      radv_bc250_analyze_mesh_contract(b->shader, &c);
      nir_validate_shader(b->shader, "after Mesh contract analysis");
      EXPECT_TRUE(c.allocation_analysis_complete);
      return c;
   }
};

TEST_F(radv_bc250_mesh_contract_test, single_allocation)
{
   allocation();
   output();
   auto c = analyze();
   EXPECT_TRUE(radv_bc250_mesh_allocation_is_canonical(&c));
   EXPECT_EQ(c.allocation_sites, 1u);
   EXPECT_FALSE(c.allocation_may_be_skipped);
   EXPECT_EQ(c.workgroup_size[0], 128u);
   EXPECT_EQ(c.max_vertices, 128u);
   EXPECT_EQ(c.max_primitives, 64u);
}

TEST_F(radv_bc250_mesh_contract_test, mutually_exclusive_counts)
{
   nir_push_if(b, uniform_condition());
   allocation();
   nir_push_else(b, NULL);
   allocation(64, 32);
   nir_pop_if(b, NULL);
   output();
   auto c = analyze();
   EXPECT_TRUE(radv_bc250_mesh_allocation_is_canonical(&c));
   EXPECT_EQ(c.allocation_sites, 2u);
   EXPECT_FALSE(c.allocation_may_be_skipped);
}

TEST_F(radv_bc250_mesh_contract_test, nested_exclusive_counts)
{
   nir_def *cond = uniform_condition();
   nir_push_if(b, cond);
   nir_push_if(b, nir_ine_imm(b, nir_channel(b, nir_load_workgroup_id(b), 1), 0));
   allocation();
   nir_push_else(b, NULL);
   allocation(64, 32);
   nir_pop_if(b, NULL);
   nir_push_else(b, NULL);
   allocation(0, 0);
   nir_pop_if(b, NULL);
   auto c = analyze();
   EXPECT_TRUE(radv_bc250_mesh_allocation_is_canonical(&c));
   EXPECT_EQ(c.allocation_sites, 3u);
}

TEST_F(radv_bc250_mesh_contract_test, empty_return_path)
{
   nir_def *w = nir_channel(b, nir_load_workgroup_id(b), 0);
   nir_push_if(b, nir_ieq_imm(b, nir_iand_imm(b, w, 3), 0));
   nir_jump(b, nir_jump_return);
   nir_pop_if(b, NULL);
   nir_push_if(b, nir_ieq_imm(b, nir_iand_imm(b, w, 1), 0));
   allocation();
   nir_push_else(b, NULL);
   allocation(64, 32);
   nir_pop_if(b, NULL);
   output();
   auto c = analyze();
   EXPECT_TRUE(radv_bc250_mesh_allocation_is_canonical(&c));
   EXPECT_TRUE(c.allocation_may_be_skipped);
}

TEST_F(radv_bc250_mesh_contract_test, analysis_preserves_original_returns)
{
   nir_push_if(b, uniform_condition());
   nir_jump_instr *ret = nir_jump_instr_create(b->shader, nir_jump_return);
   nir_builder_instr_insert(b, &ret->instr);
   nir_pop_if(b, NULL);
   allocation();
   auto c = analyze();
   EXPECT_TRUE(radv_bc250_mesh_allocation_is_canonical(&c));
   EXPECT_EQ(ret->type, nir_jump_return);
   EXPECT_NE(ret->instr.block, nullptr);
}

TEST_F(radv_bc250_mesh_contract_test, divergent_return_before_allocation)
{
   nir_push_if(b, nir_ine_imm(b, nir_load_local_invocation_index(b), 0));
   nir_jump(b, nir_jump_return);
   nir_pop_if(b, NULL);
   nir_push_if(b, uniform_condition());
   allocation();
   nir_push_else(b, NULL);
   allocation(64, 32);
   nir_pop_if(b, NULL);
   auto c = analyze();
   EXPECT_FALSE(c.allocation_control_uniform);
   EXPECT_FALSE(radv_bc250_mesh_allocation_is_canonical(&c));
}

TEST_F(radv_bc250_mesh_contract_test, sequential_counts_are_not_last_writer_wins)
{
   allocation();
   allocation(64, 32);
   auto c = analyze();
   EXPECT_TRUE(c.allocation_may_repeat);
   EXPECT_FALSE(radv_bc250_mesh_allocation_is_canonical(&c));
}

TEST_F(radv_bc250_mesh_contract_test, one_branch_repeats)
{
   nir_push_if(b, uniform_condition());
   allocation();
   allocation(64, 32);
   nir_push_else(b, NULL);
   allocation();
   nir_pop_if(b, NULL);
   auto c = analyze();
   EXPECT_TRUE(c.allocation_may_repeat);
   EXPECT_FALSE(radv_bc250_mesh_allocation_is_canonical(&c));
}

TEST_F(radv_bc250_mesh_contract_test, allocation_on_loop_backedge)
{
   nir_push_loop(b);
   allocation();
   nir_push_if(b, uniform_condition());
   nir_jump(b, nir_jump_break);
   nir_pop_if(b, NULL);
   nir_pop_loop(b, NULL);
   auto c = analyze();
   EXPECT_TRUE(c.allocation_may_repeat);
   EXPECT_FALSE(radv_bc250_mesh_allocation_is_canonical(&c));
}

TEST_F(radv_bc250_mesh_contract_test, loop_with_one_iteration)
{
   nir_push_loop(b);
   allocation();
   nir_jump(b, nir_jump_break);
   nir_pop_loop(b, NULL);
   auto c = analyze();
   EXPECT_FALSE(c.allocation_may_repeat);
   EXPECT_TRUE(radv_bc250_mesh_allocation_is_canonical(&c));
}

TEST_F(radv_bc250_mesh_contract_test, output_before_allocation)
{
   output();
   allocation();
   auto c = analyze();
   EXPECT_TRUE(c.output_before_allocation);
   EXPECT_FALSE(radv_bc250_mesh_allocation_is_canonical(&c));
}

TEST_F(radv_bc250_mesh_contract_test, missing_allocation_on_output_path)
{
   nir_push_if(b, uniform_condition());
   allocation();
   nir_pop_if(b, NULL);
   output();
   auto c = analyze();
   EXPECT_TRUE(c.allocation_may_be_skipped);
   EXPECT_TRUE(c.output_before_allocation);
   EXPECT_FALSE(radv_bc250_mesh_allocation_is_canonical(&c));
}

TEST_F(radv_bc250_mesh_contract_test, divergent_allocation_control)
{
   nir_push_if(b, nir_ine_imm(b, nir_load_local_invocation_index(b), 0));
   allocation();
   nir_push_else(b, NULL);
   allocation(64, 32);
   nir_pop_if(b, NULL);
   auto c = analyze();
   EXPECT_FALSE(c.allocation_control_uniform);
   EXPECT_FALSE(radv_bc250_mesh_allocation_is_canonical(&c));
}

TEST_F(radv_bc250_mesh_contract_test, subgroup_uniform_is_not_workgroup_uniform)
{
   nir_push_if(b, nir_ine_imm(b, nir_load_subgroup_id(b), 0));
   allocation();
   nir_push_else(b, NULL);
   allocation(64, 32);
   nir_pop_if(b, NULL);
   auto c = analyze();
   EXPECT_FALSE(c.allocation_control_uniform);
   EXPECT_FALSE(radv_bc250_mesh_allocation_is_canonical(&c));
}

TEST_F(radv_bc250_mesh_contract_test, divergent_values_need_first_invocation_handling)
{
   nir_set_vertex_and_primitive_count(b, nir_imm_int(b, 128), nir_load_local_invocation_index(b), nir_imm_int(b, 0));
   auto c = analyze();
   EXPECT_FALSE(c.allocation_values_uniform);
   EXPECT_FALSE(radv_bc250_mesh_allocation_is_canonical(&c));
}

TEST_F(radv_bc250_mesh_contract_test, no_allocation)
{
   auto c = analyze();
   EXPECT_EQ(c.allocation_sites, 0u);
   EXPECT_TRUE(c.allocation_may_be_skipped);
   EXPECT_FALSE(radv_bc250_mesh_allocation_is_canonical(&c));
}

TEST_F(radv_bc250_mesh_contract_test, branch_outputs_stay_after_their_own_allocation)
{
   nir_push_if(b, uniform_condition());
   allocation();
   output();
   nir_push_else(b, NULL);
   allocation(64, 32);
   output();
   nir_pop_if(b, NULL);
   auto c = analyze();
   EXPECT_TRUE(radv_bc250_mesh_allocation_is_canonical(&c));
   EXPECT_EQ(c.allocation_sites, 2u);
}

TEST_F(radv_bc250_mesh_contract_test, loop_without_allocation)
{
   nir_push_loop(b);
   nir_push_if(b, uniform_condition());
   nir_jump(b, nir_jump_break);
   nir_pop_if(b, NULL);
   nir_pop_loop(b, NULL);
   nir_push_if(b, uniform_condition());
   allocation();
   nir_push_else(b, NULL);
   allocation(64, 32);
   nir_pop_if(b, NULL);
   output();
   auto c = analyze();
   EXPECT_TRUE(radv_bc250_mesh_allocation_is_canonical(&c));
   EXPECT_FALSE(c.allocation_may_repeat);
}

TEST_F(radv_bc250_mesh_contract_test, summary_is_lazy_and_preserves_returns)
{
   nir_push_if(b, uniform_condition());
   nir_jump_instr *ret = nir_jump_instr_create(b->shader, nir_jump_return);
   nir_builder_instr_insert(b, &ret->instr);
   nir_pop_if(b, NULL);
   allocation();
   output();
   struct radv_bc250_mesh_contract c;
   radv_bc250_mesh_contract_init(b->shader, &c);
   EXPECT_EQ(c.allocation_static_sites, 1u);
   EXPECT_FALSE(c.allocation_analysis_complete);
   EXPECT_EQ(c.workgroup_invocations, 128u);
   EXPECT_EQ(ret->type, nir_jump_return);
   EXPECT_EQ(c.output_store_refusal, nullptr);
   nir_validate_shader(b->shader, "summary preserves original Mesh");
}

TEST_F(radv_bc250_mesh_contract_test, primitive_declarations_include_culling)
{
   nir_variable *v =
      nir_variable_create(b->shader, nir_var_shader_out, glsl_array_type(glsl_bool_type(), 64, 0), "cull");
   v->data.location = VARYING_SLOT_CULL_PRIMITIVE;
   v->data.per_primitive = true;
   auto c = analyze();
   EXPECT_TRUE(c.has_per_primitive_cull);
   EXPECT_FALSE(c.has_per_primitive_data);
   EXPECT_EQ(c.declared_per_primitive_outputs, VARYING_BIT_CULL_PRIMITIVE);
}

TEST_F(radv_bc250_mesh_contract_test, output_ownership_accepts_strided_invocations)
{
   allocation();
   position(nir_iadd_imm(b, nir_load_local_invocation_index(b), 128), nir_imm_vec4(b, 0, 0, 0, 1));
   auto c = analyze();
   EXPECT_EQ(c.output_store_refusal, nullptr);
}

TEST_F(radv_bc250_mesh_contract_test, output_ownership_rejects_shared_vertex)
{
   allocation();
   position(nir_imm_int(b, 0), nir_imm_vec4(b, 0, 0, 0, 1));
   auto c = analyze();
   ASSERT_NE(c.output_store_refusal, nullptr);
   EXPECT_STREQ(c.output_store_refusal, "vertex store not owned by invocation % workgroup size");
}

TEST_F(radv_bc250_mesh_contract_test, subgroup_identity_is_observable)
{
   nir_def *id = nir_load_subgroup_id(b);
   position(nir_load_local_invocation_index(b),
            nir_vec4(b, nir_u2f32(b, id), nir_imm_float(b, 0), nir_imm_float(b, 0), nir_imm_float(b, 1)));
   auto c = analyze();
   EXPECT_FALSE(c.subgroup_ops);
   EXPECT_TRUE(c.observes_subgroup);
   EXPECT_EQ(c.subgroup_dependent_outputs, VARYING_BIT_POS);
}

TEST_F(radv_bc250_mesh_contract_test, ballot_dependency_stays_with_indices)
{
   allocation();
   output();
   nir_variable *v =
      nir_variable_create(b->shader, nir_var_shader_out, glsl_array_type(glsl_uvec_type(3), 64, 0), "indices");
   v->data.location = VARYING_SLOT_PRIMITIVE_INDICES;
   v->data.per_primitive = true;
   nir_def *ballot = nir_ballot(b, 4, 32, nir_imm_true(b));
   nir_def *i = nir_channel(b, ballot, 0);
   nir_store_deref(b, nir_build_deref_array(b, nir_build_deref_var(b, v), nir_load_local_invocation_index(b)),
                   nir_vec3(b, i, i, i), 7);
   auto c = analyze();
   EXPECT_TRUE(c.subgroup_ops);
   EXPECT_TRUE(c.triangle_index_array);
   EXPECT_EQ(c.subgroup_dependent_outputs, VARYING_BIT_PRIMITIVE_INDICES);
   EXPECT_FALSE(c.output_dependencies_unknown);
}

TEST_F(radv_bc250_mesh_contract_test, subgroup_control_dependency)
{
   nir_push_if(b, nir_ieq_imm(b, nir_load_subgroup_id(b), 0));
   output();
   nir_pop_if(b, NULL);
   auto c = analyze();
   EXPECT_EQ(c.subgroup_dependent_outputs, VARYING_BIT_POS);
}

TEST_F(radv_bc250_mesh_contract_test, subgroup_phi_control_dependency)
{
   nir_push_if(b, nir_ieq_imm(b, nir_load_subgroup_id(b), 0));
   nir_def *a = nir_imm_vec4(b, 0, 0, 0, 1);
   nir_push_else(b, NULL);
   nir_def *other = nir_imm_vec4(b, 1, 1, 0, 1);
   nir_pop_if(b, NULL);
   position(nir_load_local_invocation_index(b), nir_if_phi(b, a, other));
   auto c = analyze();
   EXPECT_EQ(c.subgroup_dependent_outputs, VARYING_BIT_POS);
}

TEST_F(radv_bc250_mesh_contract_test, shared_output_dependencies_are_conservative)
{
   b->shader->info.shared_size = 16;
   nir_def *v = nir_load_shared(b, 1, 32, nir_imm_int(b, 0), .align_mul = 4);
   position(nir_load_local_invocation_index(b),
            nir_vec4(b, nir_u2f32(b, v), nir_imm_float(b, 0), nir_imm_float(b, 0), nir_imm_float(b, 1)));
   auto c = analyze();
   EXPECT_TRUE(c.memory_effects & RADV_BC250_SHARED_READ);
   EXPECT_EQ(c.shared_dependent_outputs, VARYING_BIT_POS);
   EXPECT_TRUE(c.output_dependencies_unknown);
}

TEST_F(radv_bc250_mesh_contract_test, readonly_storage_is_an_external_read)
{
   nir_load_ssbo(b, 1, 32, nir_imm_int(b, 0), nir_imm_int(b, 0), .access = ACCESS_NON_WRITEABLE, .align_mul = 4);
   auto c = analyze();
   EXPECT_TRUE(c.memory_effects & RADV_BC250_EXTERNAL_READ);
   EXPECT_FALSE(c.memory_effects & RADV_BC250_STORAGE_READ);
}

TEST_F(radv_bc250_mesh_contract_test, writable_storage_and_volatile_reads)
{
   nir_load_ssbo(b, 1, 32, nir_imm_int(b, 0), nir_imm_int(b, 0), .access = ACCESS_VOLATILE, .align_mul = 4);
   auto c = analyze();
   EXPECT_TRUE(c.memory_effects & RADV_BC250_STORAGE_READ);
   EXPECT_TRUE(c.memory_effects & RADV_BC250_VOLATILE_ACCESS);
}

TEST_F(radv_bc250_mesh_contract_test, external_atomic_result_usage)
{
   nir_def *v =
      nir_ssbo_atomic(b, 32, nir_imm_int(b, 0), nir_imm_int(b, 0), nir_imm_int(b, 1), .atomic_op = nir_atomic_op_iadd);
   auto unused = analyze();
   EXPECT_TRUE(unused.memory_effects & RADV_BC250_EXTERNAL_ATOMIC);
   EXPECT_FALSE(unused.used_external_atomic_result);
   nir_iadd_imm(b, v, 1);
   auto used = analyze();
   EXPECT_TRUE(used.used_external_atomic_result);
}

TEST_F(radv_bc250_mesh_contract_test, shared_atomic_and_external_barrier)
{
   b->shader->info.shared_size = 16;
   nir_shared_atomic(b, 32, nir_imm_int(b, 0), nir_imm_int(b, 1), .atomic_op = nir_atomic_op_iadd);
   nir_barrier(b, .execution_scope = SCOPE_WORKGROUP, .memory_scope = SCOPE_DEVICE,
               .memory_semantics = NIR_MEMORY_ACQ_REL, .memory_modes = nir_var_mem_ssbo);
   auto c = analyze();
   EXPECT_TRUE(c.memory_effects & RADV_BC250_SHARED_ATOMIC);
   EXPECT_TRUE(c.memory_effects & RADV_BC250_EXTERNAL_BARRIER);
   EXPECT_FALSE(c.memory_effects & RADV_BC250_EXTERNAL_ATOMIC);
}

TEST_F(radv_bc250_mesh_contract_test, task_payload_is_separate_from_external_storage)
{
   b->shader->info.task_payload_size = 16;
   nir_load_task_payload(b, 1, 32, nir_imm_int(b, 0), .align_mul = 4);
   auto c = analyze();
   EXPECT_EQ(c.task_payload_size, 16u);
   EXPECT_TRUE(c.uses_task_payload);
   EXPECT_FALSE(c.memory_effects & RADV_BC250_EXTERNAL_READ);
}

TEST_F(radv_bc250_mesh_contract_test, rewritten_shader_needs_a_fresh_snapshot)
{
   struct radv_bc250_mesh_contract original, rewritten;
   radv_bc250_mesh_contract_init(b->shader, &original);
   b->shader->info.mesh.max_vertices_out = 64;
   b->shader->info.workgroup_size[0] = 32;
   radv_bc250_mesh_contract_init(b->shader, &rewritten);
   EXPECT_EQ(original.max_vertices, 128u);
   EXPECT_EQ(original.workgroup_invocations, 128u);
   EXPECT_EQ(rewritten.max_vertices, 64u);
   EXPECT_EQ(rewritten.workgroup_invocations, 32u);
}

TEST_F(radv_bc250_mesh_contract_test, copy_deref_reads_source_and_writes_destination)
{
   nir_variable *src = nir_variable_create(b->shader, nir_var_mem_global, glsl_uint_type(), "external");
   nir_variable *dst = nir_variable_create(b->shader, nir_var_mem_shared, glsl_uint_type(), "shared");
   nir_copy_deref_with_access(b, nir_build_deref_var(b, dst), nir_build_deref_var(b, src), (gl_access_qualifier)0,
                              (gl_access_qualifier)(ACCESS_NON_WRITEABLE | ACCESS_VOLATILE));
   auto c = analyze();
   EXPECT_TRUE(c.memory_effects & RADV_BC250_EXTERNAL_READ);
   EXPECT_TRUE(c.memory_effects & RADV_BC250_SHARED_WRITE);
   EXPECT_TRUE(c.memory_effects & RADV_BC250_VOLATILE_ACCESS);
   EXPECT_FALSE(c.memory_effects & RADV_BC250_STORAGE_READ);
   EXPECT_FALSE(c.memory_effects & RADV_BC250_EXTERNAL_WRITE);
}

TEST_F(radv_bc250_mesh_contract_test, early_return_control_dependencies_are_unknown)
{
   nir_push_if(b, nir_ieq_imm(b, nir_load_subgroup_id(b), 0));
   nir_jump(b, nir_jump_return);
   nir_pop_if(b, NULL);
   output();
   auto c = analyze();
   EXPECT_TRUE(c.output_dependencies_unknown);
}

TEST_F(radv_bc250_mesh_contract_test, output_after_loop_dependencies_are_unknown)
{
   nir_push_loop(b);
   nir_push_if(b, nir_ieq_imm(b, nir_load_subgroup_id(b), 0));
   nir_jump(b, nir_jump_break);
   nir_pop_if(b, NULL);
   nir_pop_loop(b, NULL);
   output();
   auto c = analyze();
   EXPECT_TRUE(c.output_dependencies_unknown);
}

/* radv_bc250_split_shared_atomic_hazard: shared atomics behind a multi-piece split. */
class radv_bc250_split_atomic_test : public radv_bc250_mesh_contract_test {
protected:
   nir_variable *indices = nullptr, *pcolor = nullptr;

   radv_bc250_split_atomic_test()
   {
      b->shader->info.shared_size = 1024;
      indices = nir_variable_create(b->shader, nir_var_shader_out, glsl_array_type(glsl_uvec_type(3), 64, 0),
                                    "indices");
      indices->data.location = VARYING_SLOT_PRIMITIVE_INDICES;
      indices->data.per_primitive = true;
      pcolor = nir_variable_create(b->shader, nir_var_shader_out, glsl_array_type(glsl_vec4_type(), 64, 0), "pcolor");
      pcolor->data.location = VARYING_SLOT_VAR0;
      pcolor->data.per_primitive = true;
   }

   nir_def *lane() { return nir_load_local_invocation_index(b); }
   nir_def *lane_in(unsigned n) { return nir_umin_imm(b, lane(), n - 1); }

   /* atomicCompSwap on a 128-entry table at [base, base + 512). */
   nir_def *table_swap(unsigned base = 0)
   {
      return nir_shared_atomic_swap(b, 32, nir_imul_imm(b, lane_in(128), 4), nir_imm_int(b, 0), lane(),
                                    .base = (int)base, .atomic_op = nir_atomic_op_cmpxchg);
   }

   void store_indices(nir_def *slot, nir_def *value)
   {
      nir_store_deref(b, nir_build_deref_array(b, nir_build_deref_var(b, indices), slot), value, 0x7);
   }

   void store_pcolor(nir_def *slot, nir_def *value)
   {
      nir_store_deref(b, nir_build_deref_array(b, nir_build_deref_var(b, pcolor), slot), value, 0xf);
   }

   const char *hazard()
   {
      nir_validate_shader(b->shader, "before split atomic analysis");
      const char *r = radv_bc250_split_shared_atomic_hazard(b->shader);
      nir_validate_shader(b->shader, "after split atomic analysis");
      return r;
   }
};

TEST_F(radv_bc250_split_atomic_test, dedup_indices_allowed)
{
   table_swap();
   nir_barrier(b, .execution_scope = SCOPE_WORKGROUP, .memory_scope = SCOPE_WORKGROUP,
               .memory_semantics = NIR_MEMORY_ACQ_REL, .memory_modes = nir_var_mem_shared);
   nir_def *v = nir_load_shared(b, 1, 32, nir_imul_imm(b, lane_in(128), 4), .base = 0);
   allocation();
   position(lane_in(128), nir_imm_vec4(b, 0, 0, 0, 1));
   store_indices(lane_in(64), nir_vec3(b, v, v, v));
   store_pcolor(lane_in(64), nir_imm_vec4(b, 1, 0, 0, 1));
   EXPECT_EQ(hazard(), nullptr);
}

TEST_F(radv_bc250_split_atomic_test, unused_atomic_allowed)
{
   nir_shared_atomic(b, 32, nir_imm_int(b, 0), nir_imm_int(b, 1), .base = 0, .atomic_op = nir_atomic_op_iadd);
   allocation();
   store_indices(lane_in(64), nir_vec3(b, lane(), lane(), lane()));
   EXPECT_EQ(hazard(), nullptr);
}

TEST_F(radv_bc250_split_atomic_test, header_broadcast_allowed)
{
   /* Meshlet header at [0, 16) from ordinary data; the atomic table lives at [16, 528). */
   nir_store_shared(b, nir_channel(b, nir_load_workgroup_id(b), 0), nir_imm_int(b, 0), .base = 0);
   table_swap(16);
   nir_barrier(b, .execution_scope = SCOPE_WORKGROUP, .memory_scope = SCOPE_WORKGROUP,
               .memory_semantics = NIR_MEMORY_ACQ_REL, .memory_modes = nir_var_mem_shared);
   nir_def *count = nir_umin_imm(b, nir_load_shared(b, 1, 32, nir_imm_int(b, 0), .base = 0), 64);
   nir_set_vertex_and_primitive_count(b, nir_imm_int(b, 128), count, nir_imm_int(b, 0));
   store_indices(lane_in(64), nir_vec3(b, lane(), lane(), lane()));
   EXPECT_EQ(hazard(), nullptr);
}

TEST_F(radv_bc250_split_atomic_test, compaction_slot_refused)
{
   nir_def *slot = nir_shared_atomic(b, 32, nir_imm_int(b, 0), nir_imm_int(b, 1), .base = 0,
                                     .atomic_op = nir_atomic_op_iadd);
   allocation();
   store_indices(nir_umin_imm(b, slot, 63), nir_vec3(b, lane(), lane(), lane()));
   EXPECT_STREQ(hazard(), "shared atomics select a per-primitive slot");
}

TEST_F(radv_bc250_split_atomic_test, atomic_count_refused)
{
   nir_shared_atomic(b, 32, nir_imm_int(b, 0), nir_imm_int(b, 1), .base = 0, .atomic_op = nir_atomic_op_iadd);
   nir_barrier(b, .execution_scope = SCOPE_WORKGROUP, .memory_scope = SCOPE_WORKGROUP,
               .memory_semantics = NIR_MEMORY_ACQ_REL, .memory_modes = nir_var_mem_shared);
   nir_def *count = nir_umin_imm(b, nir_load_shared(b, 1, 32, nir_imm_int(b, 0), .base = 0), 64);
   nir_set_vertex_and_primitive_count(b, nir_imm_int(b, 128), count, nir_imm_int(b, 0));
   EXPECT_STREQ(hazard(), "shared atomics reach the primitive count");
}

TEST_F(radv_bc250_split_atomic_test, per_primitive_value_refused)
{
   nir_def *v = table_swap();
   allocation();
   store_pcolor(lane_in(64), nir_vec4(b, nir_u2f32(b, v), nir_imm_float(b, 0), nir_imm_float(b, 0),
                                      nir_imm_float(b, 1)));
   EXPECT_STREQ(hazard(), "shared atomics reach a per-primitive output");
}

TEST_F(radv_bc250_split_atomic_test, controlled_store_refused)
{
   nir_def *v = table_swap();
   allocation();
   nir_push_if(b, nir_ine_imm(b, v, 0));
   store_indices(lane_in(64), nir_vec3(b, lane(), lane(), lane()));
   nir_pop_if(b, NULL);
   EXPECT_STREQ(hazard(), "shared atomics control a per-primitive store");
}

TEST_F(radv_bc250_split_atomic_test, loop_count_refused)
{
   /* The trip count of the loop depends on the atomic: the counter leaving it is tainted. */
   nir_variable *n = nir_local_variable_create(b->impl, glsl_uint_type(), "n");
   nir_store_var(b, n, nir_imm_int(b, 0), 1);
   nir_loop *loop = nir_push_loop(b);
   {
      nir_def *v = nir_shared_atomic(b, 32, nir_imm_int(b, 0), nir_imm_int(b, 1), .base = 0,
                                     .atomic_op = nir_atomic_op_iadd);
      nir_push_if(b, nir_uge_imm(b, v, 8));
      nir_jump(b, nir_jump_break);
      nir_pop_if(b, NULL);
      nir_store_var(b, n, nir_iadd_imm(b, nir_load_var(b, n), 1), 1);
   }
   nir_pop_loop(b, loop);
   nir_set_vertex_and_primitive_count(b, nir_imm_int(b, 128), nir_umin_imm(b, nir_load_var(b, n), 64),
                                      nir_imm_int(b, 0));
   EXPECT_STREQ(hazard(), "shared atomics reach the primitive count");
}

/* radv_bc250_staging_alias_analyze: shared bytes still used at or after the first output store. */
class radv_bc250_staging_alias_test : public radv_bc250_mesh_contract_test {
protected:
   radv_bc250_staging_alias_test() { b->shader->info.shared_size = 16384; }

   nir_def *lane() { return nir_umin_imm(b, nir_load_local_invocation_index(b), 127); }
   void barrier()
   {
      nir_barrier(b, .execution_scope = SCOPE_WORKGROUP, .memory_scope = SCOPE_WORKGROUP,
                  .memory_semantics = NIR_MEMORY_ACQ_REL, .memory_modes = nir_var_mem_shared);
   }
};

TEST_F(radv_bc250_staging_alias_test, early_scratch_is_free)
{
   /* scratch [0, 15360) used before the outputs, table [15360, 15872) read while writing them. */
   nir_store_shared(b, lane(), nir_imul_imm(b, lane(), 4), .base = 0);
   barrier();
   nir_def *value = nir_load_shared(b, 1, 32, nir_imul_imm(b, lane(), 4), .base = 0);
   barrier();
   allocation();
   nir_push_if(b, nir_ine_imm(b, value, 0)); /* the first output store's top-level node */
   position(lane(), nir_vec4(b, nir_u2f32(b, nir_load_shared(b, 1, 32, nir_imul_imm(b, lane(), 4), .base = 15360)),
                             nir_imm_float(b, 0), nir_imm_float(b, 0), nir_imm_float(b, 1)));
   nir_pop_if(b, NULL);
   nir_validate_shader(b->shader, "alias");
   struct radv_bc250_staging_alias alias;
   ASSERT_TRUE(radv_bc250_staging_alias_analyze(b->shader, &alias));
   EXPECT_NE(alias.boundary, nullptr);
   EXPECT_TRUE(radv_bc250_staging_alias_free(&alias, 0, 15360));
   EXPECT_FALSE(radv_bc250_staging_alias_free(&alias, 15360, 15364));
   EXPECT_FALSE(radv_bc250_staging_alias_free(&alias, 15868, 15872));
}

TEST_F(radv_bc250_staging_alias_test, load_before_first_store_is_free)
{
   /* The load runs before the first output store; only its value is used after it. */
   nir_store_shared(b, lane(), nir_imul_imm(b, lane(), 4), .base = 0);
   barrier();
   allocation();
   position(lane(), nir_vec4(b, nir_u2f32(b, nir_load_shared(b, 1, 32, nir_imul_imm(b, lane(), 4), .base = 0)),
                             nir_imm_float(b, 0), nir_imm_float(b, 0), nir_imm_float(b, 1)));
   struct radv_bc250_staging_alias alias;
   ASSERT_TRUE(radv_bc250_staging_alias_analyze(b->shader, &alias));
   EXPECT_NE(alias.boundary_instr, nullptr);
   EXPECT_TRUE(radv_bc250_staging_alias_free(&alias, 0, 16384));
}

TEST_F(radv_bc250_staging_alias_test, access_after_first_store_is_reserved)
{
   nir_store_shared(b, lane(), nir_imul_imm(b, lane(), 4), .base = 0);
   barrier();
   allocation();
   position(lane(), nir_imm_vec4(b, 0, 0, 0, 1));
   position(lane(), nir_vec4(b, nir_u2f32(b, nir_load_shared(b, 1, 32, nir_imul_imm(b, lane(), 4), .base = 0)),
                             nir_imm_float(b, 0), nir_imm_float(b, 0), nir_imm_float(b, 1)));
   struct radv_bc250_staging_alias alias;
   ASSERT_TRUE(radv_bc250_staging_alias_analyze(b->shader, &alias));
   EXPECT_FALSE(radv_bc250_staging_alias_free(&alias, 0, 4));
   EXPECT_FALSE(radv_bc250_staging_alias_free(&alias, 508, 512));
   EXPECT_TRUE(radv_bc250_staging_alias_free(&alias, 512, 16384));
}

TEST_F(radv_bc250_staging_alias_test, unbounded_late_access_reserves_everything)
{
   nir_def *index = nir_channel(b, nir_load_workgroup_id(b), 0); /* no upper bound */
   allocation();
   position(lane(), nir_imm_vec4(b, 0, 0, 0, 1));
   position(lane(), nir_vec4(b, nir_u2f32(b, nir_load_shared(b, 1, 32, nir_imul_imm(b, index, 4), .base = 0)),
                             nir_imm_float(b, 0), nir_imm_float(b, 0), nir_imm_float(b, 1)));
   struct radv_bc250_staging_alias alias;
   ASSERT_TRUE(radv_bc250_staging_alias_analyze(b->shader, &alias));
   EXPECT_FALSE(radv_bc250_staging_alias_free(&alias, 0, 4));
   EXPECT_FALSE(radv_bc250_staging_alias_free(&alias, 16380, 16384));
}

TEST_F(radv_bc250_staging_alias_test, output_store_inside_loop_bounds_at_the_loop)
{
   nir_store_shared(b, lane(), nir_imul_imm(b, lane(), 4), .base = 4096);
   allocation();
   nir_loop *loop = nir_push_loop(b);
   position(lane(), nir_vec4(b, nir_u2f32(b, nir_load_shared(b, 1, 32, nir_imul_imm(b, lane(), 4), .base = 4096)),
                             nir_imm_float(b, 0), nir_imm_float(b, 0), nir_imm_float(b, 1)));
   nir_jump(b, nir_jump_break);
   nir_pop_loop(b, loop);
   struct radv_bc250_staging_alias alias;
   ASSERT_TRUE(radv_bc250_staging_alias_analyze(b->shader, &alias));
   EXPECT_EQ(alias.boundary_instr, nullptr); /* the barrier goes before the loop */
   EXPECT_TRUE(radv_bc250_staging_alias_free(&alias, 0, 4096));
   EXPECT_FALSE(radv_bc250_staging_alias_free(&alias, 4096, 4100));
}
