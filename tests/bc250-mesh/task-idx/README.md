# Task index route (`RADV_BC250_MESH_TASK_IDX`, opt-in)

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
- storage buffer and image writes and external atomics.

Reading back Mesh outputs is also handled, but valid SPIR-V never does it (VUID-StandaloneSpirv-MeshEXT-07107).

Limits:

- Indirect Task draws cover 16 producer chunks (65,536 Task workgroups per record). Groups beyond them
  are not drawn.
- `SIMULTANEOUS_USE` command buffers and device-generated commands skip these draws, with one warning.
- Pipeline statistics do not count the index pass as Mesh invocations.
- Multiview, and fragment shaders reading PrimitiveID, Layer, Viewport or barycentrics, stay refused.

Checks (all CPU, no GPU execution):

- `radv_tests`: `task_compacted_slots`, `task_cull_primitive`, `task_output_read_back` and `task_num_workgroups`
  interpret the pass with each feature. `task_payload_*` interprets the Task-mode index pass and the fetch vertex shader and
  checks every stored output, including the payload of the right record. `task_chain_*` runs the setup
  shader pass after pass and checks that every record is drawn exactly once, in order, with the pool
  restarted each pass.
- `run.py`: creates both wide fixtures as Task index pipelines and records direct and indirect draws under
  the noop shim with NIR/ACO validation and the Vulkan validation layer.
- `../split-admission/run.sh`: the `*_idx` rows.

Hardware: not run yet. The first GPU gate should be one of the wide fixtures through `index_gate` on BC-250.
