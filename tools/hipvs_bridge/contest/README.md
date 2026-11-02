# AMD V5 answer and reproducible validation

This answer is a descendant of frozen V5
`634888d9b037153fa143d2c913ba4b25503e1d62`. It preserves FP32/L2, the fixed
hipVS CAGRA search implementation and OBCUVS02 persistence. The selected product
content matches `16433ab78c539466b7123e1d3f6673cac41812ff`; integration and
documentation descendants retain those exact product blobs.

The new run in [RESULTS-20261102-v2.md](RESULTS-20261102-v2.md) passed measured
quality, query-regression and 20ms-sampled resource checks on public SIFT and a
separately identified 256-dimensional diagnostic set. Both variants used the
same 16-CPU affinity; local weighted build/query ratios were 3.0543210620x /
6.7091265084x. Its shared host is the previously rebuilt 24b binary; its candidate
bridge was freshly compiled. The earlier 24b submission officially scored
10.425/20, with build=1.415x/query=3.560x/gates=pass. Its earlier local ratio was
inflated by unequal CPU affinity and is retained as historical evidence. The
new candidate's official Hidden250K, GPU-path checker and score remain pending.

This public collaboration branch contains source and an aggregate result summary.
The revision-2 raw logs, query answers and deployment receipts remain in the
private team competition repository; no new raw evidence is published here.

## Implemented changes

- The build worker reads its own thread cgroup membership and mount mapping,
  then takes the tightest readable v1/v2 ancestor quota. It temporarily limits
  its allowed-CPU mask and its task-local OpenMP thread count when appropriate,
  restoring both on success/error. It preserves caller settings and makes no
  global environment, affinity, system-policy or quota change. Unreadable or
  unlimited controls remain best-effort; runtime applicability is measured.
- Each index reuses three RMM buffers and one search-parameter object, with a
  mutex protecting the resource/parameters. At most 16 MiB per buffer is retained;
  requests exceeding any cap allocate transient buffers. An individual buffer
  growth temporarily holds old+new allocations. Formal single-query requests at
  dimension256/top100 retain 1,824 bytes, excluding allocator overhead. The fixed
  library's own allocations are unchanged and require actual VRAM measurement.
- Shape/size overflow and pointer checks run before allocation. The previously
  ignored distance-copy error returns an error. Parameters reset per call, so a
  large top-k does not alter later smaller searches. Cached state is released on
  handle destruction and lazily initialized for deserialized handles.
- Uniform parameters are `NN_DESCENT`, iterations12, graph/intermediate
  degree128/128, `MULTI_CTA`, search width4 and
  `itopk=max(library_default,1024,4*k)`. There is no dimension/dataset routing.
  The official SQL and its index options are unchanged. `lib=vsag`, the adaptor,
  CMake and Rust sources are unchanged from V5.
- Each index has one joinable 32MiB-stack worker. Resource creation, build,
  search, persistence and teardown execute on that worker/device, followed by
  stream synchronization before returning borrowed caller buffers. A bounded
  job slot and registered-caller draining protect shutdown. Ownership must
  prevent new calls after `free` begins.

## Actual CPU checks

From the repository root on Linux:

```bash
bash tools/hipvs_bridge/tests/run_host_tests.sh
(cd tools/hipvs_bridge/contest && python3 -m unittest -v test_contract)
```

The first compiles **the actual bridge source** using a TEST_ONLY CPU shim with
ASAN/UBSAN. It checks repeated allocation reuse, growth, large-request eviction,
copy/allocation failures/retry, parameter preservation, shape overflow, shared
handle concurrency, deserialized cleanup and CPU affinity parsing/restoration.
The second checks ID uniqueness, exact-pair identity/order, frozen baseline,
recall floors/deltas, query regression and conditional scoring arithmetic.
Neither proves real header ABI, GPU execution, quality or performance.

## Required GPU environment

Use the competition's `seekdb-hipvs` Radeon Cloud template, verify **W7900D /
ROCm7.2.4**, and retain the provided fixed hipVS commit
`f96a663d0a4441d6f5344bd3ef552b71ae7b370b` at `/opt/hipvs`, ROCm at `/opt/rocm`.
Fixed offline V5 `deps/3rd` and Rust vendor must be present. Toolchain is GCC13.3,
CMake3.31, Rust1.97. Compilation limits:64 CPUs/128GB/2h. Evaluation limits:24
CPUs/96GB/45GB VRAM and <=1.25x paired-baseline VRAM; total GPU phase15min.
Do not reuse another template's prebuilt binary as proof of rebuilding this SHA.

After the candidate is reviewed/committed, expand the sparse worktree on the GPU
machine (or clone that full SHA), prepare a separate frozen-baseline checkout,
and run in each root:

```bash
bash tools/hipvs_bridge/contest/build_official.sh /workspace/artifacts/candidate
```

The build wrapper contains the official commands/options unchanged. The baseline
does not contain this wrapper, so run those identical three commands directly
against the frozen checkout. Keep build logs, full commit, clean Git status,
binary/bridge/library SHA256, `ldd`, GPU inventory and runtime/software settings.
The supplied template can expose an installed `/opt/hipvs` prefix without a
`.git` directory. Record the actual loaded library hash and leave source-commit
attestation UNKNOWN if it is unavailable; do not create fake Git metadata.
`gpu_preflight.sh` and `first_gpu_call.sh` are optional strict diagnostic tools
that additionally require that metadata. Their rejection of an installation
prefix is not a prerequisite of the official build or evidence of its failure.

Before timing, run the **unchanged existing**
`docs/gpu-vector-index-hipvs-cuvs/validation/run_ddl_persistence_smoke.sh` on an
isolated GPU container, using its documented arguments. It covers DDL, GPU
snapshot reopen, freshness and CPU `lib=vsag`. Compile/run the existing
`test_persistence.cpp` against the resulting bridge for write/read smoke.
Do not change official smoke SQL/expected results to match the candidate.

## Local paired measurement

The helper below measures SQL work on an externally started **dedicated fresh**
instance. It never starts/stops a server or clears caches. This avoids silently
attaching to or restarting a shared database. Use a fresh native data directory
and no reused GPU graph for every run. Start the actual matching binary/bridge
with the same configuration, retain its PID/command/environment/log/profiler,
and independently verify its loaded library. Cache cleanliness and process
identity are external evidence, not facts inferred by the Python helper.

Prepare SIFT250Kx128 and a representative 250Kx256 development dataset. The real
Hidden250K inputs/order/IDs are private: a local substitute is diagnostic only
and cannot establish the hidden-data gate. Never rename a substitute as actual
Hidden250K in a report. For each dataset, prepare these shared, hash-bound files:

- `base.tsv`: 250000 lines `integer_id<TAB>[FP32 components...]`.
- `queries.f32`:4096 row-major little-endian FP32 vectors, dimension128 or256.
- `ids.json`:the 250000 distinct signed integer IDs in the source table.
- `truth.json`:4096 arrays of at least100 exact FP32-L2 nearest-neighbor IDs,
  in distance order. The program validates shape/legal IDs but does not prove
  oracle correctness. Retain the oracle generator/provenance.

Using `mysql --local-infile=1` in a dedicated `amd_v5_*` database, perform this
**outside** the measured interval; change dimension only for the second dataset:

```sql
CREATE DATABASE amd_v5_sift_baseline;
USE amd_v5_sift_baseline;
CREATE TABLE items(id BIGINT PRIMARY KEY,v VECTOR(128));
LOAD DATA LOCAL INFILE '/workspace/data/sift/base.tsv' INTO TABLE items
FIELDS TERMINATED BY '\t' LINES TERMINATED BY '\n' (id,v);
```

Local data/measurement tools need NumPy and PyMySQL. The recorded environment
used NumPy2.5.4 and PyMySQL1.2.3; HDF5 reconstruction additionally used
h5py3.16.0. These are local measurement dependencies, separate from the offline
seekdb build. No credential is embedded; if needed provide
`AMD_DB_USER`/`AMD_DB_PASSWORD` to the process.

```bash
python3 tools/hipvs_bridge/contest/run_sql_benchmark.py \
  --dataset sift --variant baseline --ordinal 1 \
  --dataset-role public-sift --base-tsv /workspace/data/sift/base.tsv \
  --database amd_v5_sift_baseline --port 2981 \
  --queries-f32 /workspace/data/sift/queries.f32 \
  --ids-json /workspace/data/sift/ids.json --truth-json /workspace/data/sift/truth.json \
  --source-commit 634888d9b037153fa143d2c913ba4b25503e1d62 \
  --seekdb-binary /workspace/baseline/build_release/src/observer/seekdb \
  --bridge /workspace/artifacts/baseline/libseekdb_cuvs_bridge.so \
  --out /workspace/evidence/sift-baseline
```

Invoke the candidate's helper (it is outside the server source) for all four
runs. Change the binary/bridge/commit/database/out consistently. Exact order:
SIFT baseline ordinal1 → SIFT candidate2 → hidden candidate3 → hidden baseline4.
The helper times the complete fixed `CREATE VECTOR INDEX idx_items...lib=cuvs`
through the first legal Top10 query, then executes4096 **individual SQL queries**
at each Top10/100, preserving every answer and timing. First-query legality is
checked and included; it is not used in the recall denominator. No batch SQL or
changed index/search parameters are substituted.

```bash
python3 tools/hipvs_bridge/contest/compare_reports.py \
  /workspace/evidence/sift-baseline/report.json \
  /workspace/evidence/sift-candidate/report.json \
  /workspace/evidence/hidden-candidate/report.json \
  /workspace/evidence/hidden-baseline/report.json \
  --out /workspace/evidence/local-pairs.json
```

This reports numeric recall/delta/QPS checks and weighted ratios. GPU execution,
fixed-search provenance, no-CPU-fallback, peak VRAM/host memory/CPU/time, stability,
fresh caches/same hardware remain UNKNOWN until backed by external profiler,
server logs and sampled resource traces. `amd-smi` polling is resource evidence,
not sufficient proof that a kernel generated/queried the actual graph. Keep
build and query profiler intervals, raw traces and loaded-library identity.
The comparator never assigns official points; its conditional performance
formula is useful only after all external hard gates are established.

## Official submission and honest limits

The private answer repository must contain a descendant of frozen V5. Configure
the required repository-scoped Contents:read-only GitHub credential only in the
official registration form; never store it in this package/logs/chat. A local
candidate is not formally submitted by running these tools. The competition
rebuilds source offline, checks the fixed GPU search and assigns the final score.

The selected source passed a new four-case real GPU comparison after the old
cloud instance failed. The 256-dimensional input is diagnostic, not the private
Hidden250K. The local speedups exceed the 3.0x / 1.5x performance saturation
thresholds; they are not an official 20-point award. Exact source/binary reuse,
input hashes, recalls, resource samples and rejected predecessors are recorded
in RESULTS-20261102.md and its evidence files. No result or timing from another
team is credited to this answer.

## Executable GPU-window continuation

Clone/check out the submitted source normally. No author-local capsule is
required. `restore_v5_snapshot.sh` is an optional offline recovery helper for a
caller-supplied frozen-V5 pack and its expected SHA256; the pack is not bundled
in this repository. It restores into a new directory. The selected source is
committed. The helpers do not push, register accounts, rent resources or submit
official evaluations.

At actual resource activation, retain the original activation epoch. If the
installation also provides the Git metadata required by the optional strict
first-call helper, it can be invoked before the long seekdb build:

```bash
bash tools/hipvs_bridge/contest/first_gpu_call.sh \
  /workspace/evidence/first-real-gpu "$RESOURCE_STARTED_EPOCH"
```

It builds the real bridge against fixed hipVS and executes the existing
10Kx32 CAGRA persistence/write/search/read probe. It records start/end against
the original resource clock; a delayed or failed attempt remains delayed or
failed. It does not establish250K Recall, throughput, VRAM or the GPU-path hard
gate. With no `/dev/kfd` it exits2 before any GPU invocation. Do not launch an
instance merely to run preflight or wait for unprepared code/data.

The optional `gpu_preflight.sh` checks the fixed hipVS SHA, W7900D inventory,
ROCm7.2.4, GCC13.3, CMake3.31 and Rust1.97 (V5 source pins1.97.1). The official
build needs the supplied headers/libraries and toolchain, not a fabricated
`.git` installation. The build wrapper refuses an existing `build_release`,
dirty source or stale bridge output, and
records exact commands, exit, binary hashes and dynamic dependencies. The three
official build commands/options remain unchanged. A process-local
`CMAKE_BUILD_PARALLEL_LEVEL` within the64-core compile allowance may be set;
the official frozen `deps/3rd` and Rust vendor must already exist.

`prepare_data.py` accepts supplied250K FP32 `.npy` rows,4096 queries and explicit
int64 IDs. It writes round-trip9-digit FP32 TSV and little-endian query bytes,
plus a32-query-chunk FP64-L2 oracle with deterministic ID tie-breaking. Keep its
input/output hashes and provenance. It does not generate or infer the private
Hidden250K. Use a clearly identified256-d development set only as diagnostics.

After both real source builds and input preparation, bind their actual paths:

```bash
python3 tools/hipvs_bridge/contest/prepare_plan.py \
  --baseline-root /workspace/baseline --candidate-root /workspace/candidate \
  --artifacts /workspace/artifacts --data-root /workspace/data \
  --card-device /sys/class/drm/card0/device --out /workspace/paired-plan.json
python3 tools/hipvs_bridge/contest/run_pairs.py \
  --plan /workspace/paired-plan.json --out /workspace/evidence/paired-001
```

The input directories are `sift/` and `diagnostic-256/`; each contains
`base.tsv`, `queries.f32`, `ids.json`, `truth.json`. Verify that the SIFT input is
actually the supplied public SIFT data before labeling it `public-sift`.
The runner enforces B1/C2/C3/B4 order and matching input paths; SQL reports also
bind **base vectors**, queries, truth and IDs by SHA256. Each run starts its own
fresh process/data directory, performs LOAD DATA outside build timing, verifies
loaded bridge/fixed-library mappings, and always stops only that owned process.
Failed data/index directories are preserved and cannot be reused by the runner.
The suite has a conservative900-second overall local wall budget, including
local startup/import; timeout requests controlled source-process cleanup.

Query timing excludes Recall computation and transcript disk I/O; all8192
individual SQL answers and latencies are still retained afterward. GPU sysfs
VRAM and process RSS/HWM use the helper's default100ms period; the recorded
selected-source run used a separate20ms sampler. `summarize_resources.py` compares
the sampled45GB/1.25x/96GB limits but labels missed-peak risk and keeps GPU proof
UNKNOWN. CPU quota, exclusive device ownership and precise peaks still need
external environment/profiler evidence.

For a separate diagnostic kernel trace, invoke `run_cold_case.py --profiler`
with the same case arguments and a new output directory. It uses `rocprofv3`
kernel/HIP/memory-copy trace options and retains raw traces; do not mix this
profiled run with unprofiled timing pairs. Verify tool help against the supplied
ROCm version. Command reference: AMD's
[ROCm7.2 ROCprofiler-SDK documentation](https://rocm.docs.amd.com/projects/rocprofiler-sdk/en/docs-7.2.1/how-to/using-rocprofv3.html).
Kernel traces plus loaded-library/source identities must show the actual SQL
build/search graph path; utilization polling alone cannot establish that gate.

Historical preparatory CPU evidence included15 targeted checks, offline V5 Git
restore and expected no-GPU rejection. Keep it separate from the later real
GPU evidence in RESULTS-20261102.md.
