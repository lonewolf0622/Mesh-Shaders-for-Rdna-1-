# DirectMesh: Mesh shaders for AMD BC-250 and RDNA 1 on Linux

DirectMesh adds Mesh and Task shader support (`VK_EXT_mesh_shader`) to RADV, Mesa's Vulkan driver, on GPUs that do
not have it upstream. Games that need Mesh shaders (DirectX 12 Ultimate games through Steam/Proton, for example) can
then run. It works with normal Steam and Proton, and it does not replace your system driver: you choose which games
use it.

**GPUs:** AMD BC-250 (tested on hardware), Navi 10 (RX 5600 XT, 5700, 5700 XT) and Navi 12 (not yet tested on
hardware; reports welcome). Navi 14 (RX 5500, 5300) is not enabled yet. Other GPUs are not affected.

## Install

Open a terminal and run:

```
curl -fsSL https://raw.githubusercontent.com/lonewolf0622/Mesh-Shaders-for-Rdna-1-/main/install.sh | bash
```

That's it. The script:

- installs the build packages it needs, if any are missing (it shows the command and asks first; this is the only
  step that uses `sudo`);
- downloads DirectMesh and builds the driver for your system (2 to 20 minutes, depending on the CPU);
- installs it for your user in `~/.local/share/directmesh`, without touching the system driver;
- prints the exact line to paste into Steam.

Works on Arch, CachyOS, Manjaro, EndeavourOS, Debian, Ubuntu, Linux Mint, Pop!_OS, Fedora, Nobara and openSUSE. On
other distributions it tells you which development packages to install. Not supported by the script: immutable
systems (SteamOS, Bazzite, Fedora Silverblue), where it cannot install build packages, and Flatpak Steam, which cannot
see the driver folder.

## Use it in Steam

Right-click the game > **Properties** > **Launch Options**, and paste the line the installer printed. It looks like
this (with your own user name):

```
VK_DRIVER_FILES=/home/you/.local/share/directmesh/share/vulkan/icd.d/radeon_icd.x86_64.json %command%
```

The line is also saved in `~/.local/share/directmesh/vk_driver_files`.

**All games at once** (and every other Vulkan program): install with `--global`, then log out and back in:

```
curl -fsSL https://raw.githubusercontent.com/lonewolf0622/Mesh-Shaders-for-Rdna-1-/main/install.sh | bash -s -- --global
```

**Programs outside Steam:** `directmesh-run <program>`.

## Check that it works

```
directmesh-run vulkaninfo --summary
```

You should see your GPU and `Mesa 26.2.1`. The installer also prints `Mesh shaders: yes` at the end.

## Turn it off, update, uninstall

- **One game without DirectMesh:** remove the launch option, or put `RADV_DIRECTMESH=0` in front of `%command%`.
- **Update:** run the install command again.
- **Uninstall:**
  ```
  curl -fsSL https://raw.githubusercontent.com/lonewolf0622/Mesh-Shaders-for-Rdna-1-/main/install.sh | bash -s -- --uninstall
  ```
  Then remove the launch options you added.

## Troubleshooting

- **A game reports a DirectX 12 / feature level error on an RX 5700 / 5700 XT:** these cards have no ray tracing
  hardware, and some games refuse to start without DirectX 12 Ultimate. Add `VKD3D_FEATURE_LEVEL=12_2` in front of
  `%command%`, or `RADV_DIRECTMESH=1` to turn on RADV's (slow) emulated ray tracing.
- **Objects missing or drawn wrong:** try `RADV_BC250_MESH_TASK_IDX=0` in front of `%command%` and report whether it
  changes anything.
- **The build failed:** the error and the log location are printed. Open an issue with the log attached.
- **The game hangs the GPU:** see [If a game hangs](#if-a-game-hangs).

Please report results, good or bad, in the [issues](https://github.com/lonewolf0622/Mesh-Shaders-for-Rdna-1-/issues):
game, GPU, distribution and launch options.

## Build it yourself

The repository is Mesa 26.2.1 with the DirectMesh changes. Only the Vulkan driver needs to be built:

```sh
git clone https://github.com/lonewolf0622/Mesh-Shaders-for-Rdna-1-.git directmesh
cd directmesh
meson setup build -Dvulkan-drivers=amd -Dgallium-drivers= -Dplatforms=x11,wayland -Dbuildtype=release \
      -Dllvm=disabled -Dvideo-codecs= -Dallow-fallback-for=libdrm
ninja -C build src/amd/vulkan/libvulkan_radeon.so src/amd/vulkan/radeon_devenv_icd.x86_64.json
```

Then use `VK_DRIVER_FILES=$PWD/build/src/amd/vulkan/radeon_devenv_icd.x86_64.json` (keep the build folder in place).
Each [release](https://github.com/lonewolf0622/Mesh-Shaders-for-Rdna-1-/releases) also has a patch against stock
Mesa 26.2.1. Distribution packagers can install it as the system RADV; then nothing needs to be set.

## Options

DirectMesh is on by default on the supported GPUs. These go in front of `%command%`:

- `RADV_DIRECTMESH=0`: turn DirectMesh off. `RADV_DIRECTMESH=1`: force it on (also reports emulated ray tracing on
  Navi 10/12).
- `RADV_BC250_MESH_TASK_IDX=0`: no Task index route (pipelines no Mesh route can take are refused instead).
- `RADV_BC250_MESH_IDXPASS=0`: no indexed Mesh draws.
- `RADV_DEBUG=nomeshshader`: hide Mesh shaders, so the game uses its non-Mesh path (for comparisons).

## If a game hangs

The BC-250 can lock up hard on a GPU hang. If you want to report one:

1. Set the kernel parameter `amdgpu.gpu_recovery=0` (the machine then stays reachable, e.g. over SSH).
2. Collect `dmesg` and, if possible, a `umr` wave dump.
3. Include the game and the launch options you used.

## How it works

- `VK_EXT_mesh_shader` (Mesh + Task) on the BC-250, usable by D3D12 games through normal Proton / vkd3d-proton.
- `VK_KHR_fragment_shader_barycentric`, so vkd3d-proton keeps Mesh shaders enabled in UE5 games.
- `VK_EXT_device_generated_commands` with Mesh and Task draws, so D3D12 `ExecuteIndirect` works through vkd3d-proton
  (Crimson Desert needs it).
- Multiview with Mesh shaders, so vkd3d-proton keeps D3D12 view instancing when Mesh shaders are on.
- Indirect Task draws that scale: the rarely used extra Task chunks are recorded once per call and skipped with one
  check per draw, instead of being recorded for every draw.
- **Direct Mesh path:** each Mesh workgroup is drawn in a single launch. Per workgroup, after culling, the driver
  picks the cheapest export that it can prove safe:
  - shared vertices, when the surviving triangles use every vertex with small index backjumps;
  - renumbered vertices, when culling or a split left unused vertices: only the used ones are exported, in order;
  - shared vertices plus every triangle, when only a few triangles were culled;
  - otherwise private triangle corners, which rule out the chip's hang patterns by construction.
- **Automatic Mesh shader converter:** the driver rewrites any application Mesh (and Task) shader into a form the
  direct path accepts, with no per-game profiles. It splits large meshlets into pieces (Task pipelines included),
  keeps written `gl_PrimitiveID` values across pieces, runs subgroup-free wave32 shaders in wave64, drops task payload
  declarations a Mesh shader never reads, and folds Task launch counts that have no compile-time bound into their own
  grid dimension.
- **Fail closed:** a shape nothing can prove safe takes the older split/expansion path (also private corners) or, as a
  last resort, is refused at pipeline creation. It is never drawn on the unprotected raw route. Split Mesh shaders
  that write images run those writes once, in the first piece.
- **Task index route (v1.4):** a Task + Mesh pipeline that no Mesh route can take runs the Task stage as a compute
  producer and the Mesh stage as a compute pass that stores its outputs; an ordinary vertex pipeline draws them. It
  handles large shared memory, shared atomics, primitive compaction, `gl_CullPrimitiveEXT`, multiview, fragment
  `gl_PrimitiveID`, `gl_Layer`, `gl_ViewportIndex` and barycentrics, 2D/3D workgroups and storage writes. It is slower
  than the Mesh route and only used when no Mesh route fits.

## Tested

Vulkan CTS on real hardware (with `RADV_DIRECTMESH=1`):

| Test set | Result |
|---|---|
| `dEQP-VK.mesh_shader.ext.*` + fragment-barycentric Mesh cases that run on this device | 3,558 / 3,558 pass, 0 hangs, every pipeline direct (v1.1) |
| Mesh/Task stages in other groups (binding model, subgroups, SPIR-V, atomics, dynamic state, ...) | 10,036 pass, 0 fail, 0 hangs |

v1.2 hardware image checks (each byte-identical against the proven route): device-generated Mesh and Task draws,
descriptor buffers with device-generated commands, graphics pipeline libraries and shader objects with Mesh, and many
indirect Task draws including one large enough to need the extra chunks (old path, new path and device-generated
commands all identical).

Each converter step also passed a one-shot hardware image check: the same scene drawn by the proven route and by the
new route must be byte-identical.

Public Mesh shader samples, compiled offline (Microsoft D3D12 Mesh shader samples, Khronos Vulkan-Samples, Sascha
Willems' Vulkan examples, niagara, NVIDIA vk_lod_clusters; 85 shader/size variants that build):

| Result | v1.0 | v1.1 |
|---|---|---|
| Direct | 33 | 83 |
| Older split/expansion path | 4 | 0 |
| Unprotected raw route | 2 | 0 |
| Refused | 46 | 2 |

The two refused variants: one fragment shader reads `gl_PrimitiveID` that its Mesh shader never writes (invalid
usage), and one debug-statistics variant updates global counters with atomics that a split would repeat.

Real meshlet data (meshoptimizer on 44 models) showed that culling usually leaves unused vertices in a meshlet; the
renumbered export keeps most of those workgroups on shared vertices instead of private corners.

Games (Steam / Proton, D3D12):

| Game | Result |
|---|---|
| Final Fantasy VII Rebirth | runs, Mesh direct (its shaders are unchanged by the adaptive export) |
| Control | runs, Mesh direct; about 8% less Mesh GPU time per draw than private corners alone |
| Hellblade 2 | runs, all Mesh pipelines direct, including barycentric shaders (unchanged by the adaptive export) |
| Alan Wake 2 | runs, Mesh direct; about 10% less Mesh GPU time per draw than private corners alone |
| Crimson Desert | runs with Mesh shaders on (v1.2), including Medium settings |

The game figures were measured with v1.0. With v1.1, Final Fantasy VII Rebirth and Hellblade 2 compile to the same
shaders, and Control was played again with the renumbered export.

## Version history

- **v1.4:** the Task index route (on by default), shared atomics in the Mesh split, staging over unused shared
  memory, defaults applied per GPU instead of written into the environment, Navi 14 no longer enabled, VRS and
  optional emulated ray tracing reported on Navi 10/12, unsupported device-generated commands skipped instead of
  failing. See the [release notes](https://github.com/lonewolf0622/Mesh-Shaders-for-Rdna-1-/releases/tag/directmesh-v1.4).
- **v1.3.1:** the index route's storage (index pages and indirect pools, up to 128 MiB per device) is allocated in
  GTT instead of VRAM. The BC-250's VRAM carveout is 512 MiB and the desktop alone nearly fills it, so at 4K the
  VRAM-only storage pushed the game's and the display's buffers out and the GPU could hang. GTT is the same memory on
  this APU: the index-route speed is unchanged (all bench cases within about 1%). DirectMesh is now on by default on a
  BC-250 (`RADV_DIRECTMESH=0` turns it off). The per-draw check for a pending index-route restore is done inline in
  the draw path (from issue #2). Mesh CTS: 1,902 passed, 0 failed.
- **v1.3:** eligible Mesh-only triangle pipelines can run their index work as compute and render with
  ordinary indexed draws. Primitive attributes use private provoking corners and flat fragment inputs. Vertex
  work that still requires another invocation, small meshlets (at most 32 vertices), single-record indirect calls,
  externally visible memory writes and simultaneous command buffers keep the protected Mesh route. Persistent
  index storage shares a 128 MiB device budget (in GTT since v1.3.1), with automatic Mesh fallback on allocation or indirect-pool overflow.
  The preset also enables LDS planning and coverage, merged checks, direct primitive attributes and 65-primitive
  pieces, and admits multiview Mesh pipelines (per-vertex view layer; Task once, Mesh once per view). All additions
  can be switched off individually; see [release notes](docs/directmesh-v1.3.md).

- **v1.2:** `VK_EXT_device_generated_commands` for Mesh/Task, multiview with Mesh shaders, split Mesh shaders that write
  images, indirect Task draws recorded once per call (many indirect Task draws were very slow), vertex sharing for
  per-primitive data, leaner per-workgroup checks, wider NGG culling, graphics pipeline libraries kept for DXVK games.
- **v1.1:** automatic Mesh shader converter (renumbered-vertex export, Task pieces on the direct path, `gl_PrimitiveID`
  with pieces, wave64 promotion, unread task payloads, unbounded Task launch counts); fail-closed refusal of any Mesh
  pipeline without a protected route (in v1.0 two rare shapes could reach the raw route: Mesh-only lines above the
  private-corner capacity, and triangles with an explicit shared-memory layout and 65-85 primitives).
- **v1.0:** safe direct Mesh path, adaptive per-workgroup export, `RADV_DIRECTMESH=1`.

## Not included

- Mesh pipeline statistics queries.
- Per-primitive shading rate from Mesh shaders.
- Device-generated commands with pipelines that need the Task index route (those draws are skipped).

## Offline tests

`tests/bc250-mesh/` holds offline suites that run under Mesa's amdgpu noop drm-shim (`AMDGPU_GPU_ID=gfx1013`) without
touching the GPU. Each suite's README lists its variables (`ICD=`, `BUILD=`, `SHIM=`).

## Credits

- Luckiskind ([github.com/luckiskind](https://github.com/luckiskind)): [bc250-radv-r2](https://github.com/luckiskind/bc250-radv-r2), BC-250 RADV Mesh work referenced during this project.

## License

MIT, like Mesa. See `docs/license.rst`. Mesa 3D and RADV are MIT-licensed; AMD PAL and LLPC sources were used as a
programming reference for GFX10 NGG.
