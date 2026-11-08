# Split admission regression (drm-shim only)

The base driver's Mesh split (`radv_bc250_split_mesh`) runs the whole Mesh body once per piece, and each piece runs in its own workgroup. Before this change, the admission check refused the application's shared memory (`store_shared`), `load_num_workgroups`, and every external atomic. It now admits three classes:

1. **Non-atomic application shared memory with shared barriers.** Each piece leaves the same values in its own shared memory. Two things stay refused: shared atomics and atomic shared loads/stores. An explicit (block) shared layout is also refused.
2. **NumWorkGroups.** It is rewritten to the application grid, the same way `load_workgroup_id` already is:
   - direct_split: the draw's recorded dimensions.
   - Every other path: x divided by the piece count. This covers the hybrid Task path and the single-piece path.
   - The internal mesh-only amplifier launches (1,1,1) per group, so it has no grid to use there. It refuses NumWorkGroups.
3. **Write-only external atomics (the result is unused).** They are wrapped in `if (base == 0)`, so only the first piece runs them, exactly once per original workgroup. A body that also reads external memory (buffer, global or image loads, or texture instructions) is refused. So is any external atomic whose result is used.

Plain external stores (`store_ssbo`) are still refused. Three of the original G1 probes also write an SSBO, so they are still refused, now for `store_ssbo`. The isolated variants below cover their classes.

## Layout proof

`radv_shader_spirv_to_nir` lowers the application's shared variables to explicit offsets `[0, info.shared_size)` before the split runs. It uses `nir_lower_vars_to_explicit_types` and then `nir_lower_explicit_io`. The split lays out its own variables with `nir_lower_vars_to_explicit_types` too (moved outputs, counters and the cull prefix), and so does the expansion (staging). For non-block shared memory, that pass starts at `offset = shader->info.shared_size` (`nir_lower_explicit_io.c`, `lower_vars_to_explicit`). The NGG Mesh lowering then places its own LDS after the final `shared_size`. As a result:

- The regions cannot overlap. A debug build asserts that every split variable has `driver_location >= application size`. With `BC250_TRACE_COMPILE`, the layout is printed as `BC250 split shared layout: application=[0,A) split=[A,B)`.
- The split now drops the application's dead shared variables before its layout. Before, the cull path re-placed them after `shared_size`, which wasted a second copy of the application region.
- An explicit layout (`shared_memory_explicit_layout`) goes through `nir_assign_shared_var_locations` instead. That function asserts that every shared variable is a Block, and it aborts on the split's variables (this was observed). The split and the expansion refuse such shaders.

Final NIR evidence, from `BC250_TRACE_SPLIT_NIR=1`, which prints the NIR after the split:

- `shared_cull64`: the application stores `sh[i]` at `i*4` in `[0,256)`. The split count and vertex count are at 2304 and 2308, and the split region is `[256,2440)`.
- `shared_barrier256`: the application region is `[0,512)`. The split count is at 0x200 (512).
- `numwg128` (direct_split, 3 pieces): `load_num_workgroups` is gone. `pcol.xyz` reads `load_global(push_constant[32] + draw_id * push_constant[16]).xyz`, the draw's recorded grid.
- `numwg_task` (hybrid Task): `NumWorkGroups.x = udiv(load_num_workgroups.x, 3)`, and y and z are unchanged.
- `tri128_atomic` and `atomic_cull_shared64`: the `ssbo_atomic` is inside `if (umod(workgroup_index, pieces) * limit == 0)`.

## Shared atomics (class 4)

Shared atomics and atomic shared loads/stores were refused outright. Their results depend on the order the
atomics complete in, which may differ between pieces. They are now admitted in two cases:

1. **One piece.** The body runs exactly once per original workgroup, as without the split.
2. **Several pieces, when the slicing is proven independent of the atomics**
   (`radv_bc250_split_shared_atomic_hazard`, unit-tested in `radv_bc250_mesh_contract_tests.cpp`). The primitive
   count, every per-primitive slot, whether each per-primitive store runs, and every per-primitive value except the
   vertex indices must not depend on an atomic result. Vertex indices and vertex outputs may: each piece exports all
   of its own vertices from the same execution. Taint flows through SSA sources, phis after tainted branches, loops
   whose exit depends on taint, temporaries, and shared loads whose byte range may overlap a range an atomic (or a
   tainted store) writes. Ranges use the constant part of the address and `nir_unsigned_upper_bound`.

One-wave workgroups are not a useful test: Mesa folds their shared atomics into subgroup scans before the split
sees them. The fixtures use 128 lanes.

| Case | 975ec91 | Now |
|---|---|---|
| tri128_shatomic (atomic result unused, 2 pieces) | -8 shared_atomic | 0 |
| dedup_task64 (Task, V=128 P=64, compare-swap dedup) | 0 (safe owned route) | 0 (unchanged) |
| dedup_task128 (Task, V=160 P=128, dedup, header broadcast, 2 pieces) | -8 shared_atomic_swap | 0 |
| compact_task128 (Task, primitive compaction through a shared counter) | -8 shared_atomic | -8 shared atomics select a per-primitive slot |
| dedup_task64_wide (as reported: 13 KB shared, 10 vertex outputs) | -8 shared_atomic_swap | -8 required vertex expansion unavailable |
| dedup_task128_wide (13 KB shared, 10 vertex outputs, P=128) | -8 shared_atomic_swap | -8 required vertex expansion unavailable |

The wide cases now pass the atomics check but exceed the expansion's 28 KiB staging limit (application shared memory
plus every output array) at every piece count from 1 to 5: each piece stages all vertices. That limit is the NGG Mesh
LDS window, not the split, and needs a different route.

Older rows of `run.sh` print trace messages that predate the current driver; their results match `975ec91`.

## Results

These results use the the base driver launcher policy, run in bwrap with `/dev/dri` hidden under the noop drm-shim (GFX1013), with `NIR_DEBUG=validate ACO_DEBUG=validateir,validatera`. Pipelines are created and a draw is recorded. Nothing is ever submitted.

| Case | Before (be99b15) | After |
|---|---|---|
| tri128 (control) | 0 | 0 |
| shared_cull64 (shared + barrier + cull, 2 pieces) | -8 store_shared | 0 |
| shared_barrier256 (shared + barrier, 5 pieces) | -8 store_shared | 0 |
| explicit_layout128 (shared Block) | -8 store_shared | -8 explicit shared memory layout |
| numwg128 (direct_split) | -8 load_num_workgroups | 0 |
| numwg_single (single piece) | -8 load_num_workgroups | 0 |
| numwg_task (hybrid Task, t.task) | -8 load_num_workgroups | 0 |
| numwg128 with DIRECT_SPLIT=false (internal amplifier) | -8 load_num_workgroups | -8 load_num_workgroups (internal amplifier) |
| atomic_cull_shared64 (all three classes) | -8 load_num_workgroups | 0 |
| atomic_used128 (atomic result used) | -8 ssbo_atomic | -8 external atomic result used |
| atomic_read128 (atomic + SSBO read) | -8 ssbo_atomic | -8 external atomic with external memory reads |
| G1 tri64_cull_shared | -8 store_shared | -8 store_ssbo |
| G1 tri256_barrier | -8 store_shared | -8 store_ssbo |
| G1 tri128_numwg | -8 load_num_workgroups | -8 store_ssbo |
| G1 tri128_atomic | -8 ssbo_atomic | 0 |
| G1 tri128_shatomic | -8 shared_atomic | -8 shared_atomic |

Other checks are unchanged:

- The 51-combo proof matrix gives 30 created, and 21 refused for the same reasons as before.
- `nanite-staging` gives 0/0/0/-8/-8, as documented.
- `meson test` passes 29/29.

Run: `ICD=<build>/src/amd/vulkan/radeon_devenv_icd.x86_64.json ./run.sh`. It exits non-zero on any unexpected result, rejection message, or validation failure.

Hardware validation is still pending. These results only show that the pipelines are admitted and lowered as intended. They do not show that the split pieces render or count correctly on GFX1013.
