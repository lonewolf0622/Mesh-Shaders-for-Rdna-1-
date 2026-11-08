#!/usr/bin/env bash
# Build a self-contained BC-250 GPU gate kit (frozen driver, shaders, index_gate, gpu_gate.py).
# usage: make_gpu_kit.sh <mesa build dir> <output kit dir>
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "$here/../../.." && pwd)
build=$(cd "${1:?build dir}" && pwd)
kit=${2:?kit dir}
[ -e "$kit" ] && { echo "refusing to overwrite $kit" >&2; exit 1; }
mkdir -p "$kit/driver" "$kit/shaders" "$kit/state"
kit=$(cd "$kit" && pwd)
cp "$build/src/amd/vulkan/libvulkan_radeon.so" "$kit/driver/"
sed "s|\"library_path\": *\"[^\"]*\"|\"library_path\": \"$kit/driver/libvulkan_radeon.so\"|" \
   "$build/src/amd/vulkan/radeon_devenv_icd.x86_64.json" > "$kit/driver/icd.json"
grep -q "$kit/driver/libvulkan_radeon.so" "$kit/driver/icd.json"
fx="$src/tests/bc250-mesh/split-admission"
for m in dedup_task128 early_scratch_task128 dedup_task128_wide_np; do
   glslangValidator -S mesh --target-env vulkan1.3 -o "$kit/shaders/$m.mesh.spv" "$fx/$m.mesh" >/dev/null
done
for f in f early_scratch_task128 dedup_task128_wide_np; do
   glslangValidator -S frag --target-env vulkan1.3 -o "$kit/shaders/$f.frag.spv" "$fx/$f.frag" >/dev/null
done
glslangValidator -S task --target-env vulkan1.3 -o "$kit/shaders/t.task.spv" "$fx/t.task" >/dev/null
cc -O2 -o "$kit/index_gate" "$src/tests/bc250-mesh/index-pass/index_gate.c" -lvulkan
cp "$here/gpu_gate.py" "$kit/run.py"
chmod +x "$kit/run.py"
commit=$(git -C "$src" rev-parse --short HEAD)
cat > "$kit/README.txt" <<EOF
BC-250 GPU gate: Task index route, shared atomics in the split, staging over dead shared memory.
Driver: source $commit, frozen copy in driver/ (the installed driver is not touched).

Run once, on a quiet system (close games, Steam, builds):
    python3 $kit/run.py

8 steps, lowest risk first, one GPU submission each; stops at the first failure. Steps 3, 4 and 6 render a
shader through the Task index route and must match the same shader on the Mesh route pixel for pixel.
If the machine freezes: reboot and send state/ (the last .log is the step that was running). Do not rerun:
the kit refuses to repeat a step that started and never finished.
EOF
(cd "$kit" && sha256sum driver/libvulkan_radeon.so index_gate shaders/*.spv run.py > SHA256SUMS)
echo "kit: $kit"
