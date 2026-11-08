#!/usr/bin/env python3
"""One-shot BC-250 hardware gate for the Task index route, shared atomics in the split and staging over dead
shared memory. Run from a kit made by make_gpu_kit.sh, on a quiet system (no game, no build).

Steps run in order, lowest risk first, one GPU submission each, and the gate stops at the first failure.
Before each submission it checks that the intended route was compiled; only then does it release the submission.
Logs are written and synced to disk before every submission, so a hang leaves evidence. A step that started but
never finished (a hang or freeze) blocks every later run of this kit: do not retry it, report the log instead."""
import hashlib, json, os, select, subprocess, sys, time

KIT = os.path.dirname(os.path.abspath(__file__))
STATE = os.path.join(KIT, 'state')
SH = lambda name: os.path.join(KIT, 'shaders', name)

# name, mesh, fragment, indirect mode (None = direct), extra environment, required trace, image to match
STEPS = [
    ('1-split-atomics-direct', 'dedup_task128.mesh.spv', 'f.frag.spv', None, {},
     'split shared atomics admitted: pieces=2', None),
    ('2-staging-alias-direct', 'early_scratch_task128.mesh.spv', 'early_scratch_task128.frag.spv', None, {},
     'MESH STAGING ALIAS: applied', None),
    ('3-task-idx-direct', 'dedup_task128.mesh.spv', 'f.frag.spv', None, {'RADV_BC250_MESH_TASK_IDX': 'force'},
     'BC250 TASK IDX: applied', '1-split-atomics-direct'),
    ('4-task-idx-alias-shape', 'early_scratch_task128.mesh.spv', 'early_scratch_task128.frag.spv', None,
     {'RADV_BC250_MESH_TASK_IDX': 'force'}, 'BC250 TASK IDX: applied', '2-staging-alias-direct'),
    ('5-split-atomics-indirect', 'dedup_task128.mesh.spv', 'f.frag.spv', 'count', {},
     'split shared atomics admitted: pieces=2', None),
    ('6-task-idx-indirect', 'dedup_task128.mesh.spv', 'f.frag.spv', 'count', {'RADV_BC250_MESH_TASK_IDX': 'force'},
     'BC250 TASK IDX: applied', '5-split-atomics-indirect'),
    ('7-wide-task-idx-direct', 'dedup_task128_wide_np.mesh.spv', 'dedup_task128_wide_np.frag.spv', None,
     {'RADV_BC250_MESH_TASK_IDX': '1'}, 'BC250 TASK IDX: applied', None),
    ('8-wide-task-idx-indirect', 'dedup_task128_wide_np.mesh.spv', 'dedup_task128_wide_np.frag.spv', 'count',
     {'RADV_BC250_MESH_TASK_IDX': '1'}, 'BC250 TASK IDX: applied', None),
    # Task index route features against the same shader on the Mesh route ('!' = must not appear)
    ('9-cull-mesh-route', 'feat_cull_task.mesh.spv', 'f.frag.spv', None, {}, '!BC250 TASK IDX: applied', None),
    ('10-cull-task-idx', 'feat_cull_task.mesh.spv', 'f.frag.spv', None, {'RADV_BC250_MESH_TASK_IDX': 'force'}, 'BC250 TASK IDX: applied', '9-cull-mesh-route'),
    ('11-2d-mesh-route', 'feat_2d_task.mesh.spv', 'f.frag.spv', None, {}, '!BC250 TASK IDX: applied', None),
    ('12-2d-task-idx', 'feat_2d_task.mesh.spv', 'f.frag.spv', None, {'RADV_BC250_MESH_TASK_IDX': 'force'}, 'BC250 TASK IDX: applied', '11-2d-mesh-route'),
    ('13-numwg-mesh-route', 'feat_numwg_task.mesh.spv', 'f.frag.spv', None, {}, '!BC250 TASK IDX: applied', None),
    ('14-numwg-task-idx', 'feat_numwg_task.mesh.spv', 'f.frag.spv', None, {'RADV_BC250_MESH_TASK_IDX': 'force'}, 'BC250 TASK IDX: applied', '13-numwg-mesh-route'),
    ('15-primid-mesh-route', 'feat_primid_task.mesh.spv', 'feat_primid_task.frag.spv', None, {}, '!BC250 TASK IDX: applied', None),
    ('16-primid-task-idx', 'feat_primid_task.mesh.spv', 'feat_primid_task.frag.spv', None, {'RADV_BC250_MESH_TASK_IDX': 'force'}, 'BC250 TASK IDX: applied', '15-primid-mesh-route'),
    ('17-bary-mesh-route', 'feat_bary_task.mesh.spv', 'feat_bary_task.frag.spv', None, {}, '!BC250 TASK IDX: applied', None),
    ('18-bary-task-idx', 'feat_bary_task.mesh.spv', 'feat_bary_task.frag.spv', None, {'RADV_BC250_MESH_TASK_IDX': 'force'}, 'BC250 TASK IDX: applied', '17-bary-mesh-route'),
    ('19-layer-mesh-route', 'feat_layer_task.mesh.spv', 'f.frag.spv', None, {}, '!BC250 TASK IDX: applied', None),
    ('20-layer-task-idx', 'feat_layer_task.mesh.spv', 'f.frag.spv', None, {'RADV_BC250_MESH_TASK_IDX': 'force'}, 'BC250 TASK IDX: applied', '19-layer-mesh-route'),
    ('21-multiview-mesh-route', 'feat_view_task.mesh.spv', 'f.frag.spv', None, {'INDEX_MULTIVIEW': '1'}, '!BC250 TASK IDX: applied', None),
    ('22-multiview-task-idx', 'feat_view_task.mesh.spv', 'f.frag.spv', None, {'INDEX_MULTIVIEW': '1', 'RADV_BC250_MESH_TASK_IDX': 'force'}, 'BC250 TASK IDX: applied', '21-multiview-mesh-route'),
    ('23-compact-task-idx-direct', 'compact_task128.mesh.spv', 'f.frag.spv', None, {'RADV_BC250_MESH_TASK_IDX': '1'}, 'BC250 TASK IDX: applied', None),
    ('24-compact-task-idx-indirect', 'compact_task128.mesh.spv', 'f.frag.spv', 'count', {'RADV_BC250_MESH_TASK_IDX': '1'}, 'BC250 TASK IDX: applied', None),
    # Mesh-only pipelines (no Task stage) behind the built-in Task stage. mesh_only_wide is the reported shape (13 KB
    # shared, ballot, no Mesh route); its picture equals mesh_only_ballot on the Mesh route.
    ('25-mesh-only-mesh-route', 'mesh_only_ballot.mesh.spv', 'mesh_only_wide.frag.spv', None, {}, '!BC250 TASK IDX: applied', None),
    ('26-mesh-only-task-idx', 'mesh_only_ballot.mesh.spv', 'mesh_only_wide.frag.spv', None, {'RADV_BC250_MESH_TASK_IDX': 'force'}, 'BC250 TASK IDX: applied (Mesh-only)', '25-mesh-only-mesh-route'),
    ('27-mesh-only-wide', 'mesh_only_wide.mesh.spv', 'mesh_only_wide.frag.spv', None, {}, 'BC250 TASK IDX: applied (Mesh-only)', '25-mesh-only-mesh-route'),
    ('28-mesh-only-mesh-route-indirect', 'mesh_only_ballot.mesh.spv', 'mesh_only_wide.frag.spv', 'count', {}, '!BC250 TASK IDX: applied', None),
    ('29-mesh-only-wide-indirect', 'mesh_only_wide.mesh.spv', 'mesh_only_wide.frag.spv', 'count', {}, 'BC250 TASK IDX: applied (Mesh-only)', '28-mesh-only-mesh-route-indirect'),
    # Twelve recordings hold index storage at once (about 230 MiB; the old 128 MiB budget skipped draws after six).
    ('30-storage-12-recordings', 'mesh_only_wide.mesh.spv', 'mesh_only_wide.frag.spv', None, {'INDEX_RECORDINGS': '12'}, 'BC250 TASK IDX: applied (Mesh-only)', '25-mesh-only-mesh-route'),
]


def synced_write(path, text):
    with open(path, 'w') as f:
        f.write(text)
        f.flush()
        os.fsync(f.fileno())


def image_stats(path):
    data = open(path, 'rb').read()
    lit = sum(1 for i in range(0, len(data), 4) if data[i:i + 3] != b'\0\0\0')
    return hashlib.sha256(data).hexdigest(), lit


def run_step(name, mesh, frag, indirect, extra, marker):
    log_path = os.path.join(STATE, name + '.log')
    image = os.path.join(STATE, name + '.rgba')
    env = {'PATH': '/usr/bin:/bin', 'HOME': os.environ.get('HOME', '/tmp'),
           'VK_DRIVER_FILES': os.path.join(KIT, 'driver', 'icd.json'), 'MESA_SHADER_CACHE_DISABLE': '1',
           'BC250_TRACE_COMPILE': '1', 'BC250_IDX_TRACE': '1', 'BC250_INDEX_OWNER_AUTHORIZED': 'INDEX-once',
           'RADV_BC250_MESH_SAFE_FAST': '1', 'INDEX_PRIMITIVES': '64'}
    if indirect:
        env['INDEX_INDIRECT'] = indirect
    env.update(extra)
    # Mesh-only fixtures run without a Task stage.
    task = [] if mesh.startswith('mesh_only') else [SH('t.task.spv')]
    cmd = [os.path.join(KIT, 'index_gate'), '--authorized-gpu-INDEX', SH(mesh), SH(frag), image, *task]
    log = open(log_path, 'w')
    log.write('$ ' + ' '.join(f'{k}={v}' for k, v in extra.items()) + ' ' + ' '.join(cmd) + '\n')
    proc = subprocess.Popen(cmd, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    # Raw reads from the pipe: a buffered reader can hold INDEX_READY while select() sees an empty pipe.
    fd = proc.stdout.fileno()
    text, ready, pending, deadline = [], False, b'', time.time() + 300
    while not ready and time.time() < deadline:
        if not select.select([fd], [], [], 1)[0]:
            if proc.poll() is not None:
                break
            continue
        chunk = os.read(fd, 65536)
        if not chunk:
            break
        pending += chunk
        while b'\n' in pending and not ready:
            raw, pending = pending.split(b'\n', 1)
            line = raw.decode(errors='replace') + '\n'
            if not line.startswith('BC250POLICY'):
                log.write(line)
                text.append(line)
            ready = line.startswith('INDEX_READY')
    joined = ''.join(text)
    route_ok = (marker[1:] not in joined) if marker.startswith('!') else (marker in joined)
    if not ready or not route_ok:
        proc.kill()
        log.write(f'GATE: not released (ready={ready}, route as intended={route_ok})\n')
        log.close()
        return False, 'not released: ' + ('route not compiled as intended' if ready else 'compile/validation refused')
    # Evidence first: the log and the "started" marker reach the disk before the GPU sees anything.
    log.write('GATE: route verified, releasing the submission\n')
    log.flush()
    os.fsync(log.fileno())
    synced_write(os.path.join(STATE, name + '.started'), time.strftime('%Y-%m-%dT%H:%M:%S\n'))
    os.sync()
    proc.stdin.write(b'S')
    proc.stdin.close()
    try:
        rest, _ = proc.communicate(timeout=120)
        rest = pending + rest
    except subprocess.TimeoutExpired:
        proc.kill()
        log.write('GATE: no exit within 120 s\n')
        log.close()
        return False, 'timeout after submission'
    log.write(''.join(l for l in rest.decode(errors='replace').splitlines(True) if not l.startswith('BC250POLICY')))
    log.write(f'GATE: exit {proc.returncode}\n')
    log.close()
    if proc.returncode != 0 or not os.path.exists(image):
        return False, f'exit {proc.returncode}'
    if 'draw skipped' in rest.decode(errors='replace'):
        return False, 'a Task index route draw was skipped'
    synced_write(os.path.join(STATE, name + '.done'), time.strftime('%Y-%m-%dT%H:%M:%S\n'))
    return True, ''


def main():
    os.makedirs(STATE, exist_ok=True)
    for name, *_ in STEPS:
        if os.path.exists(os.path.join(STATE, name + '.started')) and not os.path.exists(
                os.path.join(STATE, name + '.done')):
            sys.exit(f'STOP: step {name} started earlier and never finished (hang?). Do not retry; '
                     f'send state/{name}.log.')
    if not os.path.exists('/dev/dri'):
        sys.exit('STOP: no /dev/dri (run on the BC-250 itself)')
    busy = subprocess.run(['pgrep', '-af', 'ninja|cc1|steam_app|wine64-preloader|wineserver'],
                          capture_output=True, text=True).stdout.strip()
    if busy:
        sys.exit('STOP: system not quiet, close these first:\n' + busy)
    summary = []
    for name, mesh, frag, indirect, extra, marker, match in STEPS:
        if os.path.exists(os.path.join(STATE, name + '.done')):
            print(f'SKIP {name} (passed in an earlier run)')
            ok, why = True, ''
        else:
            print(f'RUN  {name} ...', flush=True)
            ok, why = run_step(name, mesh, frag, indirect, extra, marker)
        row = {'step': name, 'ok': ok, 'why': why}
        if ok:
            digest, lit = image_stats(os.path.join(STATE, name + '.rgba'))
            row.update(sha256=digest, lit_pixels=lit)
            if lit == 0:
                ok, row['ok'], row['why'] = False, False, 'image is empty'
            elif match:
                other = image_stats(os.path.join(STATE, match + '.rgba'))[0]
                row['matches'] = match
                if other != digest:
                    ok, row['ok'], row['why'] = False, False, f'image differs from {match}'
        summary.append(row)
        print(('PASS ' if ok else 'FAIL ') + name + ('' if ok else ': ' + row['why']) +
              (f"  (lit pixels {row['lit_pixels']}" + (f", identical to {match}" if match and ok else '') + ')'
               if 'lit_pixels' in row else ''), flush=True)
        if not ok:
            break
    synced_write(os.path.join(KIT, 'summary.json'), json.dumps(summary, indent=2) + '\n')
    passed = all(r['ok'] for r in summary) and len(summary) == len(STEPS)
    print('GATE PASS: all steps' if passed else 'GATE STOPPED: see state/ and summary.json')
    sys.exit(0 if passed else 1)


if __name__ == '__main__':
    main()
