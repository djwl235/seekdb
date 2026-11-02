#!/usr/bin/env bash
# Restore the exact frozen V5 Git snapshot offline; never touch an existing root.
set -Eeuo pipefail
[[ $# == 2 && -f $1 && $2 == /* && ! -e $2 ]] || {
  echo 'usage: restore_v5_snapshot.sh <snapshot.pack> <new-absolute-source-root>' >&2; exit 2;
}
[[ ${AMD_EXPECTED_PACK_SHA256:-} =~ ^[a-f0-9]{64}$ ]] || {
  echo 'set AMD_EXPECTED_PACK_SHA256 from the locally retained package receipt' >&2; exit 2;
}
ACTUAL=$(sha256sum "$1" | cut -d' ' -f1)
[[ $ACTUAL == "$AMD_EXPECTED_PACK_SHA256" ]] || { echo 'snapshot hash mismatch'; exit 2; }
PACK=$(realpath "$1")
ROOT=$2
BASE=634888d9b037153fa143d2c913ba4b25503e1d62
mkdir -p "$ROOT"
git init --template= "$ROOT"
git -C "$ROOT" index-pack --stdin < "$PACK"
printf '%s\n' "$BASE" > "$ROOT/.git/shallow"
git -C "$ROOT" update-ref refs/heads/amd-v5-offline "$BASE"
git -C "$ROOT" symbolic-ref HEAD refs/heads/amd-v5-offline
git -C "$ROOT" reset --hard "$BASE"
[[ $(git -C "$ROOT" rev-parse HEAD) == "$BASE" && -z $(git -C "$ROOT" status --porcelain) ]]
echo "Exact clean frozen V5 source restored offline: $ROOT"
