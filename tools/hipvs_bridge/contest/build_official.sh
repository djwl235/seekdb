#!/usr/bin/env bash
# Same commands/options as official V5; run from the repository root.
set -Eeuo pipefail
[[ $# == 1 ]] || { echo 'usage: build_official.sh <absolute-output-directory>' >&2; exit 2; }
[[ $1 == /* ]] || { echo 'output directory must be absolute' >&2; exit 2; }
OUT=$1
[[ ! -e build_release && ! -e "$OUT/libseekdb_cuvs_bridge.so" ]] || {
  echo 'fresh source/build_release and a new bridge output are required' >&2; exit 2;
}
mkdir -p "$OUT"
exec > >(tee "$OUT/build.log") 2>&1
trap 'printf "%s\n" "$?" > "$OUT/build-exit-code.txt"' EXIT
git merge-base --is-ancestor 634888d9b037153fa143d2c913ba4b25503e1d62 HEAD
git rev-parse HEAD > "$OUT/source-commit.txt"
[[ -z $(git status --porcelain) ]] || { echo 'source must be clean and committed'; exit 2; }
HIPVS_ROOT=/opt/hipvs ROCM_ROOT=/opt/rocm tools/hipvs_bridge/build.sh "$OUT/libseekdb_cuvs_bridge.so"
cmake -S . -B build_release -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DOB_ENABLE_UNITY=ON -DOB_USE_LLD=ON -DOB_BUILD_CUVS=ON \
  -DOB_BUILD_CUVS_TRACE=OFF -DCUVS_BRIDGE_LIB="$OUT/libseekdb_cuvs_bridge.so"
cmake --build build_release --target seekdb
sha256sum build_release/src/observer/seekdb "$OUT/libseekdb_cuvs_bridge.so" > "$OUT/binaries.sha256"
ldd build_release/src/observer/seekdb > "$OUT/seekdb-ldd.txt"
ldd "$OUT/libseekdb_cuvs_bridge.so" > "$OUT/bridge-ldd.txt"
