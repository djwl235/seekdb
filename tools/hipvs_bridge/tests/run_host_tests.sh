#!/usr/bin/env bash
set -Eeuo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
OUT=$(mktemp -d "${TMPDIR:-/var/tmp}/seekdb-amd-host-XXXXXX")
trap 'rm -rf -- "$OUT"' EXIT
for path in cuvs/core/c_api.h cuvs/neighbors/cagra.h dlpack/dlpack.h cuda_runtime.h; do
  mkdir -p "$OUT/include/$(dirname "$path")"
  printf '#include "mock_api.h"\n' > "$OUT/include/$path"
done
"${CXX:-g++}" -std=gnu++17 -Wall -Wextra -Werror -Wno-misleading-indentation -pthread \
  -fsanitize=address,undefined -fno-omit-frame-pointer -g \
  -I"$HERE" -I"$OUT/include" "$HERE/test_workspace.cpp" -o "$OUT/test-workspace"
ASAN_OPTIONS=detect_leaks=1 "$OUT/test-workspace"

"${CXX:-g++}" -std=gnu++17 -Wall -Wextra -Werror -pthread \
  -fsanitize=address,undefined -fno-omit-frame-pointer -g \
  "$HERE/test_cpu_budget.cpp" -o "$OUT/test-cpu-budget"
ASAN_OPTIONS=detect_leaks=1 "$OUT/test-cpu-budget"
