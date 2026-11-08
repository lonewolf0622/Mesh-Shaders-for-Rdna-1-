#!/usr/bin/env python3
"""RADV_BC250_MESH_TASK_IDX recording gate (noop drm-shim only, never the GPU).

Task + Mesh pipelines whose Mesh stage has no protected route (13 KB of application
shared memory plus 10 vertex outputs: over the expansion's 28 KiB NGG window) are
created as the Task index route and draws are recorded, direct and indirect, with
NIR/ACO validation and the Vulkan validation layer's error count from index_gate.
Passes are sized per pipeline; the drawn output itself is checked by the interpreter
unit tests (radv_bc250_idx_tests: task_payload_*, task_chain_*)."""
import argparse, re, subprocess
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--build', type=Path, required=True)
p.add_argument('--shim', type=Path, required=True)
p.add_argument('--gate', type=Path, required=True, help='index_gate built from tests/bc250-mesh/index-pass')
p.add_argument('--out', type=Path, required=True)
a = p.parse_args()
src = Path(__file__).resolve().parents[1] / 'split-admission'
out = a.out.resolve()
out.mkdir(parents=True, exist_ok=True)
icd = a.build.resolve() / 'src/amd/vulkan/radeon_devenv_icd.x86_64.json'

def spv(stage, path, name):
    target = out / name
    subprocess.run(['glslangValidator', '-S', stage, '--target-env', 'vulkan1.3', '-o', str(target), str(path)],
                   check=True, capture_output=True)
    return target

task = spv('task', src / 't.task', 't.spv')
rows = 0
# fixture, fragment shader, RADV_BC250_MESH_TASK_IDX value ('force' on shapes the Mesh route also takes)
FIXTURES = [('dedup_task64_wide', 'dedup_task64_wide', '1'), ('dedup_task128_wide', 'dedup_task128_wide', '1'),
            # feat_ssbo_task needs a bound storage buffer, which index_gate does not provide: its pipeline creation is
            # covered by ../split-admission/run.sh.
            ('compact_task128', 'f', '1'), ('feat_2d_task', 'f', 'force'),
            ('feat_cull_task', 'f', 'force'), ('feat_numwg_task', 'f', 'force'),
            ('feat_view_task', 'f', 'force', {'INDEX_MULTIVIEW': '1'}), ('feat_primid_task', 'feat_primid_task', 'force'),
            ('feat_layer_task', 'f', 'force'), ('feat_bary_task', 'feat_bary_task', 'force')]
for fixture, fragment, switch, *more in FIXTURES:
    mesh = spv('mesh', src / (fixture + '.mesh'), fixture + '.spv')
    frag = spv('frag', src / (fragment + '.frag'), fragment + '.frag.spv')
    for mode in ('direct', 'count'):
        env = dict(PATH='/usr/bin', HOME='/tmp', LD_PRELOAD=str(a.shim.resolve()), AMDGPU_GPU_ID='gfx1013',
                   VK_DRIVER_FILES=str(icd), MESA_SHADER_CACHE_DISABLE='1', NIR_DEBUG='validate',
                   ACO_DEBUG='validateir,validatera', BC250_TRACE_COMPILE='1', BC250_IDX_TRACE='1',
                   RADV_DEBUG='dumpibs', INDEX_PRIMITIVES='64', RADV_BC250_MESH_TASK_IDX=switch)
        if mode != 'direct':
            env['INDEX_INDIRECT'] = mode
        env.update(more[0] if more else {})
        cmd = ['bwrap', '--ro-bind', '/', '/', '--dev', '/dev', '--proc', '/proc', '--tmpfs', '/tmp',
               '--bind', str(out), str(out), '--unshare-pid', '--die-with-parent', '--', 'env', '-i',
               *(f'{k}={v}' for k, v in env.items()),
               str(a.gate.resolve()), '--offline', str(mesh), str(frag), str(out / 'o.rgba'), str(task)]
        r = subprocess.run(cmd, capture_output=True, timeout=300)
        log = r.stdout.decode() + r.stderr.decode()
        tag = f'{fixture}-{mode}'
        (out / (tag + '.log')).write_text(log)
        assert r.returncode == 0, tag
        assert 'NOOP_COMPLETE pixels_not_rendered' in log and 'VALIDATION_ERRORS=0' in log, tag
        assert not re.search(r'(?i)validation failed|ACO ERROR|Assertion|Segmentation|VALIDATION:.*Error', log), tag
        assert 'BC250 TASK IDX: applied' in log and 'draw skipped' not in log, tag
        passes = len(re.findall(r'^BC250 TASK IDX: pass ', log, re.M))
        assert passes > 0, tag
        rows += 1
        print('PASS', tag, 'index passes recorded:', passes, flush=True)
print('PASS', rows, 'Task index recordings; no GPU execution')
