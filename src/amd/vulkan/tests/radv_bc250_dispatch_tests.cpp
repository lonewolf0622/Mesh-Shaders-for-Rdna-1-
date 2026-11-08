/* SPDX-License-Identifier: MIT */
#include "../../../../tests/bc250-mesh/dgc/nir_interpreter.h"
#include "tests/nir_test.h"
#include "radv_bc250_dispatch.h"
#include "radv_directmesh.h"

class radv_bc250_dispatch_test : public nir_test {
protected:
   radv_bc250_dispatch_test(): nir_test("radv_bc250_dispatch_test")
   {
   }

   nir_def *parameter(unsigned word, unsigned bits = 32, unsigned components = 1)
   {
      return nir_load_push_constant(b, components, bits, nir_imm_int(b, word * (bits / 8)), .range = 64);
   }

   void result(unsigned slot, nir_def *value)
   {
      nir_store_output(b, value, nir_imm_int(b, 0), .base = (int)slot,
                       .write_mask = BITFIELD_MASK(value->num_components));
   }

   template <typename T> void coordinates(const std::vector<std::array<T, 3>> &grids)
   {
      const unsigned bits = sizeof(T) * 8;
      nir_def *id = parameter(0, bits, 3), *size = parameter(3, bits, 3);
      nir_def *x = nir_channel(b, size, 0), *y = nir_channel(b, size, 1), *z = nir_channel(b, size, 2);
      nir_def *linear = radv_bc250_dispatch_flatten(b, id, size);
      nir_def *count = radv_bc250_dispatch_count(b, x, y, z);
      result(0, bits == 64 ? nir_unpack_64_2x32(b, linear) : linear);
      result(1, bits == 64 ? nir_unpack_64_2x32(b, count) : count);
      result(2, nir_u2u32(b, radv_bc250_dispatch_unflatten(b, parameter(6, bits), x, nir_imul(b, x, y))));
      result(3, nir_u2u32(b, radv_bc250_dispatch_unflatten_modulo(b, parameter(6, bits), x, y)));
      nir_validate_shader(b->shader, "logical dispatch coordinates");
      bc250_dgc_nir_interpreter vm(b->shader);
      for (auto grid : grids) {
         for (T iz : {T(0), T(grid[2] / 2), T(grid[2] - 1)}) {
            for (T iy : {T(0), T(grid[1] / 2), T(grid[1] - 1)}) {
               for (T ix : {T(0), T(grid[0] / 2), T(grid[0] - 1)}) {
                  uint64_t expected = uint64_t(ix) + uint64_t(grid[0]) * (uint64_t(iy) + uint64_t(grid[1]) * iz);
                  T constants[8] = {ix, iy, iz, grid[0], grid[1], grid[2], T(expected), 0};
                  vm.push_constants.assign((uint8_t *)constants, (uint8_t *)(constants + 8));
                  vm.run(b->shader, 0, 0);
                  EXPECT_EQ(vm.outputs[0], uint32_t(expected));
                  uint64_t expected_count = uint64_t(grid[0]) * grid[1] * grid[2];
                  EXPECT_EQ(vm.outputs[4], uint32_t(expected_count));
                  if (bits == 64) {
                     EXPECT_EQ(vm.outputs[1], expected >> 32);
                     EXPECT_EQ(vm.outputs[5], expected_count >> 32);
                  }
                  for (unsigned slot : {2u, 3u}) {
                     EXPECT_EQ(vm.outputs[slot * 4], ix);
                     EXPECT_EQ(vm.outputs[slot * 4 + 1], iy);
                     EXPECT_EQ(vm.outputs[slot * 4 + 2], iz);
                  }
               }
            }
         }
      }
   }
};

TEST_F(radv_bc250_dispatch_test, row_major_32bit)
{
   coordinates<uint32_t>({{1, 1, 1}, {17, 1, 1}, {1, 13, 1}, {1, 1, 11}, {7, 5, 3}, {31, 19, 17}});
}

TEST_F(radv_bc250_dispatch_test, dimension_and_chunk_boundaries)
{
   coordinates<uint32_t>({{65535, 1, 1}, {1, 65535, 1}, {65535, 32, 2}, {65535, 65535, 1}, {4096, 1024, 1}});
}

TEST_F(radv_bc250_dispatch_test, task_products_use_64bit)
{
   coordinates<uint64_t>({{65535, 65535, 65535}, {65536, 65536, 3}, {UINT32_MAX, 2, 2}});
}

TEST_F(radv_bc250_dispatch_test, empty_grid_has_zero_groups)
{
   nir_def *size = parameter(0, 32, 3);
   result(0, radv_bc250_dispatch_count(b, nir_channel(b, size, 0), nir_channel(b, size, 1), nir_channel(b, size, 2)));
   bc250_dgc_nir_interpreter vm(b->shader);
   for (unsigned axis = 0; axis < 3; axis++) {
      uint32_t constants[3] = {65535, 32, 2};
      constants[axis] = 0;
      vm.push_constants.assign((uint8_t *)constants, (uint8_t *)(constants + 3));
      vm.run(b->shader, 0, 0);
      EXPECT_EQ(vm.outputs[0], 0u);
   }
}

TEST_F(radv_bc250_dispatch_test, merge_and_split_preserve_group_and_instance)
{
   nir_def *group = parameter(0), *instance = parameter(1);
   const unsigned factors[] = {1, 2, 3, 4, 8, 32};
   for (unsigned slot = 0; slot < ARRAY_SIZE(factors); slot++) {
      unsigned factor = factors[slot];
      nir_def *physical = radv_bc250_dispatch_interleave(b, group, factor, instance);
      result(slot, nir_vec3(b, physical, nir_udiv_imm(b, physical, factor), nir_umod_imm(b, physical, factor)));
   }
   nir_validate_shader(b->shader, "interleaved dispatch groups");
   bc250_dgc_nir_interpreter vm(b->shader);
   for (uint32_t group : {0u, 1u, 65534u, 65535u, (1u << 22) - 1}) {
      for (uint32_t instance = 0; instance < 32; instance++) {
         uint32_t constants[2] = {group, instance};
         vm.push_constants.assign((uint8_t *)constants, (uint8_t *)(constants + 2));
         vm.run(b->shader, 0, 0);
         for (unsigned slot = 0; slot < ARRAY_SIZE(factors); slot++) {
            unsigned factor = factors[slot];
            if (instance >= factor)
               continue;
            EXPECT_EQ(vm.outputs[slot * 4], group * factor + instance);
            EXPECT_EQ(vm.outputs[slot * 4 + 1], group);
            EXPECT_EQ(vm.outputs[slot * 4 + 2], instance);
         }
      }
   }
}

TEST_F(radv_bc250_dispatch_test, chunk_base_recovers_full_dispatch_coordinates)
{
   nir_def *chunk_local = radv_bc250_dispatch_flatten(b, parameter(0, 32, 3), parameter(3, 32, 3));
   nir_def *global = nir_iadd(b, parameter(6), chunk_local);
   result(0, nir_u2u32(b, radv_bc250_dispatch_unflatten(b, nir_u2u64(b, global), nir_imm_int64(b, 257),
                                                        nir_imm_int64(b, 257 * 127))));
   nir_validate_shader(b->shader, "chunk dispatch coordinates");
   bc250_dgc_nir_interpreter vm(b->shader);
   for (uint32_t base : {0u, 256u, 257u, 257u * 127 - 1, 257u * 127, (1u << 22) - 64}) {
      for (uint32_t local = 0; local < 64; local++) {
         uint32_t constants[7] = {local % 8, (local / 8) % 4, local / 32, 8, 4, 2, base};
         vm.push_constants.assign((uint8_t *)constants, (uint8_t *)(constants + 7));
         vm.run(b->shader, 0, 0);
         uint32_t expected = base + local;
         EXPECT_EQ(vm.outputs[0], expected % 257);
         EXPECT_EQ(vm.outputs[1], (expected / 257) % 127);
         EXPECT_EQ(vm.outputs[2], expected / (257 * 127));
      }
   }
}

TEST(radv_directmesh, hardware_scope)
{
   for (auto family : {CHIP_NAVI10, CHIP_NAVI12, CHIP_GFX1013})
      EXPECT_TRUE(radv_directmesh_family(family));
   /* Navi14 stays out until the Mesh-only NGG path has run on that chip. */
   for (auto family : {CHIP_NAVI14, CHIP_POLARIS10, CHIP_RENOIR, CHIP_NAVI21, CHIP_NAVI31})
      EXPECT_FALSE(radv_directmesh_family(family));
   for (unsigned id : {0x13fe, 0x143f, 0x7310, 0x7312, 0x7318, 0x7319, 0x731a, 0x731b,
                       0x731e, 0x731f, 0x7360, 0x7362}) {
      EXPECT_TRUE(radv_directmesh_pci(0x1002, id));
      EXPECT_FALSE(radv_directmesh_pci(0x10de, id));
   }
   for (unsigned id : {0x7340, 0x7341, 0x7347, 0x734f, 0x13ff, 0x7311, 0x7342, 0x7361, 0x15e7, 0x73bf, 0x744c})
      EXPECT_FALSE(radv_directmesh_pci(0x1002, id));
}

TEST(radv_directmesh, activation_override)
{
   EXPECT_TRUE(radv_directmesh_preset(nullptr, true));
   EXPECT_FALSE(radv_directmesh_preset(nullptr, false));
   for (bool detected : {false, true}) {
      EXPECT_TRUE(radv_directmesh_preset("1", detected));
      EXPECT_FALSE(radv_directmesh_preset("0", detected));
      EXPECT_FALSE(radv_directmesh_preset("", detected));
      EXPECT_FALSE(radv_directmesh_preset("invalid", detected));
   }
}

/* The settings table must stay value-for-value identical to the preset that
 * radv_bc250_directmesh_env() used to write with setenv() (v1.3.1 + auto IDX), plus the Task index route. */
TEST(radv_directmesh, preset_table_matches_previous_setenv_preset)
{
   static const struct radv_directmesh_setting expected[] = {
      {"RADV_BC250_NATIVE_TASK", "0"},
      {"BC250_EXPERIMENTAL_COMPUTE_CU_MODE", "false"},
      {"BC250_BALANCED_SLICES", "true"},
      {"BC250_SINGLE_PIECE", "true"},
      {"BC250_CACHE_PLAN", "true"},
      {"BC250_TRANSIENT_ARENA", "true"},
      {"BC250_PARALLEL_CULL", "true"},
      {"BC250_COMPACT_VERTICES", "false"},
      {"BC250_OUTPUT_REGIONS", "true"},
      {"BC250_EXPERIMENTAL_ZEROED_PRIVATE_GTT", "false"},
      {"RADV_BC250_MESH_ONCE", "false"},
      {"BC250_EXPERIMENTAL_DIRECT_SPLIT", "true"},
      {"BC250_EXPERIMENTAL_SKIP_INACTIVE_CHUNKS", "true"},
      {"RADV_BC250_SPLIT_MESH", "true"},
      {"RADV_BC250_EXPAND_PRIMITIVES", "true"},
      {"BC250_EXPERIMENTAL_CULL_COMPACT", "true"},
      {"BC250_EXPERIMENTAL_PACK_TRIANGLE_VERTICES", "true"},
      {"BC250_EXPERIMENTAL_POST_MESH_VGT_FLUSH", "0"},
      {"BC250_CAPTURE_RAW_IBS", "false"},
      {"RADV_BC250_EXPOSE_FAST_BINDING", "1"},
      {"RADV_BC250_SUBMIT_KNOWN_SIGNALS", "1"},
      {"RADV_BC250_LOCAL_BOS", "1"},
      {"RADV_BC250_KEEP_IB_KB", "640"},
      {"RADV_BC250_PERF_PIECE_PRIMS", "64"},
      {"RADV_BC250_SCRATCH_REUSE", "1"},
      {"RADV_BC250_MESH_DIRECT_READ", "full"},
      {"RADV_BC250_MESH_AUTOCULL", "1"},
      {"RADV_BC250_MESH_AUTOCULL_WIDE", "1"},
      {"RADV_BC250_MESH_SAFE_FAST", "1"},
      {"RADV_BC250_MESH_SAFE_PIECES", "1"},
      {"RADV_BC250_MESH_SAFE_OWNED", "1"},
      {"RADV_BC250_MESH_SAFE_LOCAL", "1"},
      {"RADV_BC250_MESH_SAFE_CHECK", "1"},
      {"RADV_BC250_MESH_SAFE_CORNERS", "1"},
      {"RADV_BC250_MESH_SAFE_AUTOCULL", "1"},
      {"RADV_BC250_MESH_SAFE_PARALLEL", "1"},
      {"RADV_BC250_MESH_SAFE_BARY", "1"},
      {"RADV_BC250_BARY_IO16", "1"},
      {"RADV_BC250_MESH_SAFE_BARY_TINY", "1"},
      {"RADV_BC250_MESH_SAFE_BARY_AFFINE", "1"},
      {"RADV_BC250_MESH_SAFE_BARY_LAST", "1"},
      {"RADV_BC250_BARY_CORNER_ID", "1"},
      {"RADV_BC250_MESH_ALLOW_POS1", "1"},
      {"RADV_BC250_MESH_SPLIT_ANY", "1"},
      {"RADV_BC250_MESH_NESTED_SLICE", "1"},
      {"RADV_BC250_MESH_SAFE_SPLIT_PIECES", "1"},
      {"RADV_BC250_MESH_SAFE_ADAPTIVE", "1"},
      {"RADV_BC250_MESH_LDS_PLAN", "1"},
      {"RADV_BC250_MESH_LDS_COVER", "1"},
      {"RADV_BC250_MESH_MERGED_CHECK", "1"},
      {"RADV_BC250_MESH_PP_DIRECT", "1"},
      {"RADV_BC250_MESH_PIECES_65", "1"},
      {"RADV_BC250_MESH_IDXPASS", "1"},
      {"RADV_BC250_MESH_IDXPASS_STORE", "auto"},
      {"RADV_BC250_MESH_IDXPASS_SETUP_BATCH", "1"},
      {"RADV_BC250_MESH_FAIL_CLOSED", "1"},
      {"RADV_BC250_MESH_DEAD_PAYLOAD", "1"},
      {"RADV_BC250_MESH_PIECE_PRIMID", "1"},
      {"RADV_BC250_MESH_SAFE_PIECES_EXT", "1"},
      {"RADV_BC250_TASK_GRID_FOLD", "1"},
      {"RADV_BC250_MESH_SAFE_COMPACT", "1"},
      {"RADV_BC250_MESH_PP_SHARE", "1"},
      {"RADV_BC250_MESH_LEAN_CHECK", "1"},
      {"RADV_BC250_EXPOSE_MULTIVIEW_MESH", "1"},
      {"RADV_BC250_MESH_SPLIT_STORES", "1"},
      {"RADV_BC250_MESH_MULTIVIEW_VTX", "1"},
      {"RADV_BC250_TASK_MULTIVIEW", "1"},
      {"RADV_BC250_TASK_TAIL", "1"},
      {"RADV_BC250_EXPOSE_DGC", "1"},
      {"RADV_BC250_NGGC_WIDE", "1"},
      {"RADV_BC250_DCC_32B", "1"},
      {"RADV_BC250_MESH_TASK_IDX", "1"},
      {"RADV_BC250_MESH_TASK_IDX_MESH_ONLY", "1"},
   };
   unsigned count = 0;
   const struct radv_directmesh_setting *table = radv_directmesh_settings_table(&count);
   ASSERT_EQ(count, sizeof(expected) / sizeof(expected[0]));
   for (unsigned i = 0; i < count; i++) {
      EXPECT_STREQ(table[i].name, expected[i].name);
      EXPECT_STREQ(table[i].value, expected[i].value);
      EXPECT_STREQ(radv_directmesh_default(expected[i].name), expected[i].value);
   }
   EXPECT_EQ(radv_directmesh_default("RADV_PERFTEST"), nullptr);
   EXPECT_EQ(radv_directmesh_default("RADV_BC250_HYBRID_TASK"), nullptr);
   EXPECT_EQ(radv_directmesh_default("RADV_DIRECTMESH"), nullptr);
}

TEST(radv_directmesh, resolve_environment_wins)
{
   /* Inactive preset: only the environment counts. */
   EXPECT_EQ(radv_directmesh_resolve(nullptr, "RADV_BC250_MESH_IDXPASS", false), nullptr);
   EXPECT_STREQ(radv_directmesh_resolve("0", "RADV_BC250_MESH_IDXPASS", false), "0");
   /* Active preset: the environment still wins, including an explicit 0 or empty value. */
   EXPECT_STREQ(radv_directmesh_resolve(nullptr, "RADV_BC250_MESH_IDXPASS", true), "1");
   EXPECT_STREQ(radv_directmesh_resolve("0", "RADV_BC250_MESH_IDXPASS", true), "0");
   EXPECT_STREQ(radv_directmesh_resolve("", "RADV_BC250_KEEP_IB_KB", true), "");
   EXPECT_STREQ(radv_directmesh_resolve(nullptr, "RADV_BC250_KEEP_IB_KB", true), "640");
   /* Names outside the preset never get a value. */
   EXPECT_EQ(radv_directmesh_resolve(nullptr, "RADV_BC250_MESH_REFERENCE", true), nullptr);
}

TEST(radv_directmesh, explicit_switch)
{
   EXPECT_TRUE(radv_directmesh_explicit("1"));
   EXPECT_FALSE(radv_directmesh_explicit(nullptr));
   EXPECT_FALSE(radv_directmesh_explicit("0"));
   EXPECT_FALSE(radv_directmesh_explicit(""));
   EXPECT_FALSE(radv_directmesh_explicit("true"));
}
