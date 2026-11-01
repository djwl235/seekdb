#!/usr/bin/env bash
# Actual fixed-CAGRA build/search/persist call before long source compilation.
set -Eeuo pipefail
[[ $# == 2 && $1 == /* && $2 =~ ^[0-9]+$ && ! -e $1 ]] || {
  echo 'usage: first_gpu_call.sh <new-absolute-evidence-dir> <resource-start-epoch-seconds>' >&2; exit 2;
}
OUT=$1
START=$2
mkdir -p "$OUT"
exec > >(tee "$OUT/first-call.log") 2>&1
trap 'code=$?; printf "%s\n" "$code" > "$OUT/exit-code.txt"' EXIT
date -u +%FT%TZ > "$OUT/started-utc.txt"
[[ -c /dev/kfd && -x /opt/rocm/bin/hipcc ]] || { echo 'GPU unavailable; no GPU invocation claimed'; exit 2; }
[[ $(git -C /opt/hipvs rev-parse HEAD) == f96a663d0a4441d6f5344bd3ef552b71ae7b370b ]]
HIPVS_ROOT=/opt/hipvs ROCM_ROOT=/opt/rocm tools/hipvs_bridge/build.sh "$OUT/libseekdb_cuvs_bridge.so"
g++ -O2 tools/hipvs_bridge/test_persistence.cpp -Itools/hipvs_bridge \
  -L"$OUT" -lseekdb_cuvs_bridge -Wl,-rpath,"$OUT:/opt/hipvs/lib:/opt/rocm/lib" -o "$OUT/persistence"
NOW=$(date +%s)
printf '{"resource_started_epoch":%s,"gpu_command_started_epoch":%s,"elapsed_seconds":%s,"within_300_seconds":%s}\n' \
  "$START" "$NOW" "$((NOW-START))" "$([[ $((NOW-START)) -ge 0 && $((NOW-START)) -le 300 ]] && echo true || echo false)" > "$OUT/start-window.json"
# This executable calls the real bridge; a failed call remains a failed GPU attempt.
"$OUT/persistence" write "$OUT/probe.cagra"
"$OUT/persistence" read "$OUT/probe.cagra"
END=$(date +%s)
printf '{"probe_completed_epoch":%s,"seconds_from_resource_start":%s,"completed_within_300_seconds":%s}\n' \
  "$END" "$((END-START))" "$([[ $((END-START)) -ge 0 && $((END-START)) -le 300 ]] && echo true || echo false)" > "$OUT/completed-window.json"
sha256sum "$OUT/libseekdb_cuvs_bridge.so" "$OUT/persistence" > "$OUT/binaries.sha256"
echo 'Actual bridge probe completed; this is not250K recall/performance or profiler attestation.'
