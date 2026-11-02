# Final source build, 2026-11-02

Source: `24b972ab8500e29b4e750f8bee7a6b5c5f5193a3`, clean checkout and fresh
`build_release`. Original build logs, exit codes, output hashes, loaded-library
dependencies and the resulting native-check report are preserved byte-for-byte.

The official three commands completed with exit0 from02:31:37Z to02:40:30Z
(about533 seconds), using CMAKE_BUILD_PARALLEL_LEVEL=8 and CARGO_BUILD_JOBS=1.
The new seekdb binary's native observable checks also exited0; five internal
TRACE_OFF assertions remain UNKNOWN. No timed performance run overlapped this
full source build.

The new bridge hash equals the bridge measured in the four paired cases.
Those cases used the separately disclosed recovered CD host; their historical
JSON files are not rewritten to claim the new host was measured.
`build-and-backup-manifest.json` also identifies locally retained compressed
output parts. These are backup references, not repository build dependencies;
no compiled seekdb or bridge binary is included here.
