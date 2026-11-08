# DirectMesh on v1.3.1: automatic IDX and RDNA 1

This tree merges the DirectMesh development work with public v1.3.1
(`3977ab4`). Its BC-250 GTT storage fix and inline pending-restore checks are
preserved. The merge uses the byte-identical v1.3 trees at `ed58ea5` and `872468f`
as its common source base (tree `61e7d6c29b3e50ebd07c1e18b8fd9e9b6f1507af`).
The previous `74d1bc1` candidate did not include the v1.3.1 fixes and is superseded.

This source build activates DirectMesh automatically on BC-250 (GFX1013) and
RDNA 1 (Navi 10 and Navi 12). Navi 14 is left out: upstream keeps NGG off on that
chip because of hardware bugs, and its Mesh-only NGG path has not been run on hardware. `RADV_DIRECTMESH=1` is no longer required.
`RADV_DIRECTMESH=0` disables DirectMesh. Existing individual environment options,
including explicit zeroes, still override defaults. Selecting a staged ICD is
still necessary when testing a driver that has not been installed.

DRM PCI metadata selects the preset before cached driver options are read. The
allowlist uses exact AMD display-device IDs from the Linux amdgpu PCI table;
family checks also gate compiler and command-buffer paths. An unrelated GPU
alone does not enable the preset. The preset is never written into the process
environment: DirectMesh switches are read through `radv_dm_option()`
(`radv_directmesh.c`), which returns the environment value when one is set and the
preset value otherwise. Nothing leaks into child processes, nothing races with
`getenv` on other threads, and the preset's NIR cache default applies to
DirectMesh devices only. Index-route switches are read once per device, not per draw.

The shared protected Mesh, split, merge, Task and indexed paths now accept RDNA 1.
BC-250 memory, submission, CU/slice topology and ray instruction exceptions remain
limited to BC-250. The 1x1-only VRS exposure applies to every DirectMesh device, so
vkd3d-proton sees VRS tier 2 on RDNA 1 as on BC-250. vkd3d-proton reports feature
level 12_2 only with ray tracing as well: with an explicit `RADV_DIRECTMESH=1`,
Navi 10/12 also expose RADV's emulated ray tracing. The automatic default does not,
because games that see ray tracing may enable it and emulation is slow; use
`RADV_DIRECTMESH=1` or vkd3d-proton's `VKD3D_FEATURE_LEVEL=12_2` for games that
require 12_2.
The BC-250 per-primitive interpolation register exception is also retained only
for BC-250; the RDNA 1 fixtures use the protected per-vertex representations.

DXVK keeps graphics pipeline libraries, shader objects and pipeline binaries when
hybrid Task is implicit. An explicit `RADV_BC250_HYBRID_TASK=1` still enables Task.
The implicit Task default is not written into the environment, so it remains
implicit even if the Vulkan loader unloads/reloads the driver between instances.

## Validation and limits

The merged gate passed 110 driver unit tests and 291 fresh-device noop launches:
47 activation/override/feature probes (each creates two successive Vulkan
instances), 76 pairs of shader/command recordings, and 92 extended recordings.
On BC-250, all 19 automatic recordings match the `74d1bc1` candidate with
its explicit preset; changing storage placement preserves shaders and command words, including shader uploads and command words. Each RDNA 1
target matches its explicit preset on the same 19 workloads. Extended checks
cover primitive attributes and both provoking modes, shared memory, ballots,
branches, dynamic/empty output, query and multiview fallback, Task direct/indirect,
barycentrics, per-vertex reads, PrimitiveID and a conventional vertex reference.
Recorded Navi 14 NGG stages have zero late allocation. Both BC-250 PCI IDs from
v1.3.1 (`13fe` and `143f`) activate the preset. Allocation traces assert GTT with
write combining for BC-250 index/output pages and indirect pools, local VRAM
for discrete RDNA 1, and CPU-visible GTT for diagnostic pools on all targets.

These are CPU compiler, Vulkan validation and command-recording checks. They do
not render or measure GPU time. Navi 10 uses Mesa's captured shim profile;
Navi 12, Navi 14 and GFX1013 profiles use synthetic identities with Navi 10 topology.
RDNA 1 rendering, stability, conformance and performance remain unverified on
hardware. In particular, the automatic indexed cost thresholds were measured on
BC-250 and are not tuned separately for RDNA 1. Earlier BC-250 hardware results do
not establish RDNA 1 correctness or speed. Treat this as an RDNA 1 development
candidate, not a hardware-validated release for those cards.

The installed driver and existing frozen owner benchmark kits are unchanged.
The query recording test omits `dumpibs`: the existing optional GFX10 packet
pretty-printer asserts on a query counter enumeration. Vulkan validation and
route-usage checks still cover that recording.

The fork-safety update (no environment writes, per-device switches, Navi 14
excluded, RDNA 1 VRS/ray tracing exposure) passed 113 unit tests, 96 activation and
recording checks (on BC-250, 19 automatic recordings byte-identical in shaders and
command words to the released `975ec91` driver with its explicit preset) and 69
extended recordings. CPU evidence: `/home/deck/mesh-test/directmesh-fork-safety-evidence/`.

Task + Mesh pipelines with no protected Mesh route can run as the opt-in Task index route
(`RADV_BC250_MESH_TASK_IDX=1`, `tests/bc250-mesh/task-idx/README.md`). Shared atomics are admitted in
the Mesh split when every piece slices the same primitives (`tests/bc250-mesh/split-admission/README.md`).

Reproduction is in `tests/bc250-mesh/directmesh-defaults/README.md`. CPU evidence:
`/home/deck/mesh-test/directmesh-v131-rdna-evidence/`.
