/* SPDX-License-Identifier: MIT */
#include "radv_directmesh.h"

#include <stdlib.h>
#include <string.h>

#include "util/macros.h"
#include "util/u_debug.h"

/* DirectMesh defaults: BC-250 base settings plus every validated direct-path
 * switch. They are looked up here instead of being written into the process
 * environment, so they never reach other GPUs' code paths through getenv, never
 * leak into child processes and never race with getenv on other threads.
 * A variable set in the environment always wins. The switches themselves only
 * act on DirectMesh devices (radv_physical_device::bc250_native_mesh or
 * CHIP_GFX1013 checks at each read site). */
static const struct radv_directmesh_setting radv_directmesh_settings[] = {
   /* BC-250 base: Mesh through hybrid Task; split/expansion stays the automatic fallback. */
   {"RADV_BC250_NATIVE_TASK", "0"},
   {"BC250_EXPERIMENTAL_COMPUTE_CU_MODE", "false"},
   {"BC250_BALANCED_SLICES", "true"}, {"BC250_SINGLE_PIECE", "true"}, {"BC250_CACHE_PLAN", "true"},
   {"BC250_TRANSIENT_ARENA", "true"}, {"BC250_PARALLEL_CULL", "true"}, {"BC250_COMPACT_VERTICES", "false"},
   /* BC250_EXPERIMENTAL_PRIVATE_GTT is not set: it moved every private allocation out of the VRAM
    * carveout into snooped GTT (a hang experiment, never shown to help). */
   {"BC250_OUTPUT_REGIONS", "true"},
   {"BC250_EXPERIMENTAL_ZEROED_PRIVATE_GTT", "false"}, {"RADV_BC250_MESH_ONCE", "false"},
   {"BC250_EXPERIMENTAL_DIRECT_SPLIT", "true"}, {"BC250_EXPERIMENTAL_SKIP_INACTIVE_CHUNKS", "true"},
   {"RADV_BC250_SPLIT_MESH", "true"}, {"RADV_BC250_EXPAND_PRIMITIVES", "true"},
   {"BC250_EXPERIMENTAL_CULL_COMPACT", "true"}, {"BC250_EXPERIMENTAL_PACK_TRIANGLE_VERTICES", "true"},
   {"BC250_EXPERIMENTAL_POST_MESH_VGT_FLUSH", "0"}, {"BC250_CAPTURE_RAW_IBS", "false"},
   {"RADV_BC250_EXPOSE_FAST_BINDING", "1"}, {"RADV_BC250_SUBMIT_KNOWN_SIGNALS", "1"},
   {"RADV_BC250_LOCAL_BOS", "1"}, {"RADV_BC250_KEEP_IB_KB", "640"},
   {"RADV_BC250_PERF_PIECE_PRIMS", "64"}, {"RADV_BC250_SCRATCH_REUSE", "1"},
   {"RADV_BC250_MESH_DIRECT_READ", "full"}, {"RADV_BC250_MESH_AUTOCULL", "1"},
   {"RADV_BC250_MESH_AUTOCULL_WIDE", "1"},
   /* Direct Mesh path. */
   {"RADV_BC250_MESH_SAFE_FAST", "1"}, {"RADV_BC250_MESH_SAFE_PIECES", "1"},
   {"RADV_BC250_MESH_SAFE_OWNED", "1"}, {"RADV_BC250_MESH_SAFE_LOCAL", "1"},
   {"RADV_BC250_MESH_SAFE_CHECK", "1"}, {"RADV_BC250_MESH_SAFE_CORNERS", "1"},
   {"RADV_BC250_MESH_SAFE_AUTOCULL", "1"}, {"RADV_BC250_MESH_SAFE_PARALLEL", "1"},
   {"RADV_BC250_MESH_SAFE_BARY", "1"}, {"RADV_BC250_BARY_IO16", "1"},
   {"RADV_BC250_MESH_SAFE_BARY_TINY", "1"}, {"RADV_BC250_MESH_SAFE_BARY_AFFINE", "1"},
   {"RADV_BC250_MESH_SAFE_BARY_LAST", "1"}, {"RADV_BC250_BARY_CORNER_ID", "1"},
   {"RADV_BC250_MESH_ALLOW_POS1", "1"}, {"RADV_BC250_MESH_SPLIT_ANY", "1"},
   {"RADV_BC250_MESH_NESTED_SLICE", "1"}, {"RADV_BC250_MESH_SAFE_SPLIT_PIECES", "1"},
   {"RADV_BC250_MESH_SAFE_ADAPTIVE", "1"},
   /* Additional direct routes; an explicit zero still overrides this preset. */
   {"RADV_BC250_MESH_LDS_PLAN", "1"}, {"RADV_BC250_MESH_LDS_COVER", "1"},
   {"RADV_BC250_MESH_MERGED_CHECK", "1"}, {"RADV_BC250_MESH_PP_DIRECT", "1"},
   {"RADV_BC250_MESH_PIECES_65", "1"}, {"RADV_BC250_MESH_IDXPASS", "1"},
   /* Default selection is driver-side. Numeric values remain diagnostic overrides. */
   {"RADV_BC250_MESH_IDXPASS_STORE", "auto"},
   {"RADV_BC250_MESH_IDXPASS_SETUP_BATCH", "1"},
   /* Never the raw route: a Mesh pipeline without a protected route is refused. */
   {"RADV_BC250_MESH_FAIL_CLOSED", "1"},
   /* More shapes admitted to the safe routes (gates PP1, TG1, PX1). */
   {"RADV_BC250_MESH_DEAD_PAYLOAD", "1"}, {"RADV_BC250_MESH_PIECE_PRIMID", "1"},
   {"RADV_BC250_MESH_SAFE_PIECES_EXT", "1"}, {"RADV_BC250_TASK_GRID_FOLD", "1"},
   /* Renumbered referenced vertices after culling instead of private corners (gate CM1). */
   {"RADV_BC250_MESH_SAFE_COMPACT", "1"},
   /* Shared vertices on the owned route, private provoking corners (gates PS1, PS2). */
   {"RADV_BC250_MESH_PP_SHARE", "1"},
   /* Skip adaptive checks whose result is already known (gate LC1). */
   {"RADV_BC250_MESH_LEAN_CHECK", "1"},
   {"RADV_BC250_EXPOSE_MULTIVIEW_MESH", "1"}, {"RADV_BC250_MESH_SPLIT_STORES", "1"},
   /* Multiview Mesh: the view layer is a per-vertex export (GFX10.1 has no layer in the primitive export), and
    * Task+Mesh draws run the Task stage once and the Mesh stage once per view. */
   {"RADV_BC250_MESH_MULTIVIEW_VTX", "1"}, {"RADV_BC250_TASK_MULTIVIEW", "1"},
   {"RADV_BC250_TASK_TAIL", "1"}, {"RADV_BC250_EXPOSE_DGC", "1"},
   /* GDDR6-class settings for this APU-reported GPU: wider NGG culling admission, 32B DCC blocks. */
   {"RADV_BC250_NGGC_WIDE", "1"}, {"RADV_BC250_DCC_32B", "1"},
   /* Task + Mesh pipelines without a protected Mesh route run as the Task index route (GPU gate: 24/24, images
    * identical to the Mesh route). */
   {"RADV_BC250_MESH_TASK_IDX", "1"},
   /* Mesh-only pipelines without a protected route take it behind a built-in one-launch Task stage. */
   {"RADV_BC250_MESH_TASK_IDX_MESH_ONLY", "1"},
};

/* Written once per instance creation before any option is read; only ever
 * changes from false to true within a process. */
static bool radv_directmesh_active;

const struct radv_directmesh_setting *
radv_directmesh_settings_table(unsigned *count)
{
   *count = ARRAY_SIZE(radv_directmesh_settings);
   return radv_directmesh_settings;
}

const char *
radv_directmesh_default(const char *name)
{
   for (unsigned i = 0; i < ARRAY_SIZE(radv_directmesh_settings); i++) {
      if (!strcmp(radv_directmesh_settings[i].name, name))
         return radv_directmesh_settings[i].value;
   }
   return NULL;
}

const char *
radv_directmesh_resolve(const char *env_value, const char *name, bool active)
{
   if (env_value)
      return env_value;
   return active ? radv_directmesh_default(name) : NULL;
}

void
radv_directmesh_set_active(bool active)
{
   if (active)
      __atomic_store_n(&radv_directmesh_active, true, __ATOMIC_RELEASE);
}

bool
radv_directmesh_is_active(void)
{
   return __atomic_load_n(&radv_directmesh_active, __ATOMIC_ACQUIRE);
}

const char *
radv_dm_option(const char *name)
{
   return radv_directmesh_resolve(getenv(name), name, radv_directmesh_is_active());
}

bool
radv_dm_bool(const char *name, bool dfault)
{
   return debug_parse_bool_option(radv_dm_option(name), dfault);
}

int64_t
radv_dm_num(const char *name, int64_t dfault)
{
   return debug_parse_num_option(radv_dm_option(name), dfault);
}
