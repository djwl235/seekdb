#!/usr/bin/env bash
# Read-only machine/source inspection; never launch, stop, rent or register a GPU.
set -Eeuo pipefail
[[ $# == 1 && ! -e $1 ]] || { echo 'usage: gpu_preflight.sh <new-evidence-directory>' >&2; exit 2; }
mkdir -p "$1"
OUT=$(cd "$1" && pwd)
git merge-base --is-ancestor 634888d9b037153fa143d2c913ba4b25503e1d62 HEAD
git rev-parse HEAD > "$OUT/source-commit.txt"
git status --porcelain=v1 > "$OUT/source-status.txt"
git diff --binary HEAD > "$OUT/uncommitted.patch"
sha256sum "$OUT/uncommitted.patch" > "$OUT/patch.sha256"
uname -a > "$OUT/uname.txt"
lscpu > "$OUT/lscpu.txt"
free -b > "$OUT/memory.txt"
for tool in /opt/rocm/bin/hipcc /usr/bin/g++ /usr/bin/cmake; do
  if [[ -x $tool ]]; then "$tool" --version >> "$OUT/toolchain.txt" 2>&1; else echo "MISSING $tool" >> "$OUT/toolchain.txt"; fi
done
if command -v amd-smi >/dev/null; then amd-smi static > "$OUT/gpu.txt" 2>&1
elif [[ -x /opt/rocm/bin/rocm-smi ]]; then /opt/rocm/bin/rocm-smi --showproductname --showmeminfo vram > "$OUT/gpu.txt" 2>&1
else echo 'GPU inventory UNKNOWN: AMD SMI missing' > "$OUT/gpu.txt"; fi
if [[ -d /opt/hipvs/.git ]]; then git -C /opt/hipvs rev-parse HEAD > "$OUT/hipvs-source-commit.txt"; fi
if [[ -f /opt/hipvs/lib/libcuvs_c.so ]]; then sha256sum /opt/hipvs/lib/libcuvs_c.so > "$OUT/hipvs-library.sha256"; fi
[[ ! -f /opt/rocm/.info/version ]] || cat /opt/rocm/.info/version > "$OUT/rocm-version.txt"
command -v rustc >/dev/null && rustc --version >> "$OUT/toolchain.txt" || true
command -v rocprofv3 >/dev/null && rocprofv3 --help > "$OUT/rocprofv3-help.txt" || true
printf '%s\n' 'Preflight collected. This does not prove GPU execution, ROCm compatibility, recall or performance.'
[[ -x /opt/rocm/bin/hipcc && -f /opt/hipvs/include/cuvs/neighbors/cagra.h && -f /opt/hipvs/lib/libcuvs_c.so ]]
[[ -f "$OUT/hipvs-source-commit.txt" && $(cat "$OUT/hipvs-source-commit.txt") == f96a663d0a4441d6f5344bd3ef552b71ae7b370b ]]
grep -Eq '7\.2\.4([.-]|$)' "$OUT/rocm-version.txt"
grep -Eq 'W7900D' "$OUT/gpu.txt"
[[ $(g++ -dumpfullversion) == 13.3.* ]]
cmake --version | head -1 | grep -Eq '3\.31\.'
rustc --version | grep -Eq '^rustc 1\.97\.'
