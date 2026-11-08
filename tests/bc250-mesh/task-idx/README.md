# Task index route (`RADV_BC250_MESH_TASK_IDX`, on by default)

Some Task + Mesh pipelines have no protected Mesh route. The reported case: a Mesh stage with about 13 KB
of application shared memory, ten vertex outputs and a shared `atomicCompSwap` table. Splitting cannot
help: every piece stages all vertices, and application shared memory plus staged outputs exceed the
expansion's 28 KiB NGG window at every piece count. Without the switch, pipeline creation fails closed
and the game draws nothing for those objects.

With `RADV_BC250_MESH_TASK_IDX=1`, such a pipeline is created as the Task index route instead:

- **Task stage:** runs as the existing hybrid producer (`bc250_create_task_producer`). It writes one Mesh
  launch record and one payload per Task workgroup, in chunks of 4096 workgroups.
- **Mesh stage:** runs as the stored index pass (`radv_bc250_idx_build_task_cs`). This is a compute shader,
  so it has the full LDS, runs the Mesh body exactly once per workgroup (shared atomics keep their
  meaning), and never goes through the NGG Mesh lowering. Payload reads use
  `constants.compute_args + record * stride`, and the application's DrawID stays the draw's own.
- **Drawing:** the stored outputs are drawn by the index route's vertex/fragment pipeline. That pipeline
  is what the application receives (`bc250_idx_gfx` points back to it).
- **Overflow:** there is no Mesh route to fall back to. Each producer chunk runs up to N index passes, and
  pass p draws pass p-1's overflow suffix. The setup shader already writes that suffix as ordinary records
  plus a GPU count. Every pass drains earlier readers and restarts the pool, and two table sets per
  command buffer alternate between passes. Pass p > 0 sits in nested `COND_EXEC` regions on a 0/1 flag
  written by pass p-1's setup (a Task-only setup variant). When nothing overflowed, the later passes,
  including their drains, are skipped on the GPU (about 170 dwords each in the command buffer). A skipped
  pass skips every later one, so no pass reads a stale flag. N defaults to 16
  (`RADV_BC250_MESH_TASK_IDX_PASSES` overrides it). Records left after N passes are not drawn.

Beyond the ordinary index route, the Task index route admits these, because it always stores outputs at the
slots the shader writes and runs each Mesh workgroup exactly once (in exactly one pass), as the hardware would:

- primitive compaction (output slots chosen through shared atomics, not owned by fixed invocations);
- 2D/3D Mesh workgroups (the pass is a compute shader with the same shape);
- `gl_CullPrimitiveEXT` (staged in the pass's shared memory; culled primitives become degenerate);
- `gl_NumWorkGroups` (the record's Mesh grid);
- storage buffer and image writes and external atomics;
- multiview: the producer runs once, then the pass chain runs once per view with that view's ViewIndex and
  draws only that view (a Task shader reading ViewIndex is refused, as on the hybrid route);
- fragment shaders reading `gl_PrimitiveID` (moved to a free generic slot as a flat primitive attribute),
  `gl_Layer`/`gl_ViewportIndex` (exported by the stored vertex shader from the provoking vertex) or
  barycentrics (the stored triangles keep the Mesh corner order).

Mesh-only pipelines (`RADV_BC250_MESH_TASK_IDX_MESH_ONLY`, on by default with the route) take the same route
behind a built-in Task stage: one invocation per application Mesh workgroup stores that workgroup's WorkGroupID and
NumWorkGroups in the payload (the producer emulates the full-dispatch values) and launches one Mesh workgroup; the
Mesh stage reads both from the payload (`radv_bc250_idx_lower_mesh_only`). This takes, for example, a V=128 P=64
shader with 13 KB of shared memory, subgroup ballot and storage writes, which the ordinary index route refuses.

Storage: each recording that uses the route holds a 16 MiB pool and its tables (about 19 MiB). The device budget is
1 GiB on the BC-250 (GPU-only GTT, at most an eighth of it) and 128 MiB on boards with local VRAM;
`RADV_BC250_IDX_STORAGE_MB` overrides it. When it runs out the draw is skipped with one warning that reports the
storage in use (with the earlier 128 MiB budget, six pending recordings were enough).

Shaders with an explicit (aliasing) shared-memory layout are refused: the pass's own shared variables cannot be
placed beside them.

Reading back Mesh outputs is also handled, but valid SPIR-V never does it (VUID-StandaloneSpirv-MeshEXT-07107).

Indirect Task draws cover the full 4,194,304 Task workgroups: Task index pipelines use larger producer
chunks (up to 32,768 workgroups, bounded by 32 MiB of records and payloads and by the pass budget), so an
indirect record has BC250_MAX_TASK_TOTAL / chunk slots (128 to 1024). Each slot carries its pass chain, so an
indirect draw records about 2.6 K dwords per slot; inactive slots are skipped on the GPU.

`SIMULTANEOUS_USE` command buffers work on one queue: every pass drains earlier work before it rewrites the pool
and the table sets. Concurrent execution of one command buffer on two queues is not protected.

Limits:

- Device-generated commands skip these draws, with one warning. A DGC capture records each sequence into a
  temporary recording; this route's pool and tables would belong to it and be freed while the GPU still uses them.
- Pipeline statistics do not count the index pass as Mesh invocations.

Checks (all CPU, no GPU execution):

- `radv_tests`: `task_compacted_slots`, `task_cull_primitive`, `task_output_read_back` and `task_num_workgroups`
  interpret the pass with each feature. `task_payload_*` interprets the Task-mode index pass and the fetch vertex shader and
  checks every stored output, including the payload of the right record. `task_chain_*` runs the setup
  shader pass after pass and checks that every record is drawn exactly once, in order, with the pool
  restarted each pass.
- `radv_tests` `task_mesh_only_payload_ids`: the Mesh-only lowering reads WorkGroupID and NumWorkGroups from the right
  record's payload.
- `run.py`: creates both wide fixtures (and the Mesh-only `mesh_only_wide` / `mesh_only_ballot`) as Task index pipelines and records direct and indirect draws under
  the noop shim with NIR/ACO validation and the Vulkan validation layer.
- `../split-admission/run.sh`: the `*_idx` rows.

Hardware: `gpu_gate.py` (kit from `make_gpu_kit.sh`). Steps 1-24 passed on BC-250 (images identical to the Mesh route).
Steps 25-30 (Mesh-only pipelines and twelve recordings holding storage at once) have not run on hardware yet.
