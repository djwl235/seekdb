# Sources and license

The candidate is based on the official V5 seekdb source, frozen commit
`634888d9b037153fa143d2c913ba4b25503e1d62`, from
https://github.com/zihaomu/seekdb/tree/amd-oceanbase-official-contest-base-v5 .
The original bridge, adaptor and Apache-2.0 repository license remain present.

The user-designated reference was inspected at the complete immutable commit
`f9fe575be66f0ce789c894590aedeaa6d3fe30de`:
https://github.com/Terrified-of-Logic/seekdb-ans/tree/amd-hipvs-build .
`sk_cpu_affinity.h` adapts the cgroup budget / compact allowed-CPU mask idea and
implementation from `tools/hipvs_build/build_helper.cpp` under Apache-2.0,
retaining its 2026 OceanBase copyright. The candidate restores the calling
build thread's affinity, uses no standalone helper, and does not convert CAGRA
to a CPU HNSW graph. See the repository root LICENSE for the complete license.

Reference mapping:

| Reference mechanism | V5 decision |
| --- | --- |
| GPU NN-descent, then `cuvsHnswFromCagra` + CPU HNSW search | Not imported: violates V5's fixed GPU CAGRA search requirement. |
| Compact CPU affinity when cgroup quota is narrower than allowed mask | Adapted only around actual V5 CAGRA build. No change if quota is unavailable/unlimited or mask already fits. Numeric CPU IDs are not a topology guarantee; GPU paired measurement is still required. |
| NN-descent 20 iterations, graph degree 2*M, intermediate 4*M | The old CPU-HNSW settings were not imported. The final V5 settings were selected by this team's real paired GPU checks, as recorded below. |
| Helper startup overlap and native HNSW snapshot validation | Not imported: different process, persistence and query architecture. V5 OBCUVS02 remains authoritative. |
| CPU fallback after GPU failure | Not imported. The candidate adds no fallback; V5's existing adaptor fallback must still be excluded by actual external GPU-path validation during formal cases. |
| Reference reports and account/environment evidence | Not copied or credited as this team's execution. No score claim is inferred from the branch name or reports. |

Workspace/parameter-cache implementation and contest measurement tools were
written for this candidate. They do not modify or vendor hipVS itself.

## Historical V5 graph-shape reference and first selection

The later public V5 reference was inspected at the immutable commit
`3c8ed145d733f71917b0d9b9c837b98d1505de55`:
https://github.com/tlyyxjz/ob2026-seekdb/blob/3c8ed145d733f71917b0d9b9c837b98d1505de55/tools/hipvs_bridge/seekdb_cuvs_bridge.c .
Its Apache-2.0 bridge uses graph/intermediate degree192/192, NN12,
AUTO/width4 and an itopk floor512. This answer adopted the 192/192 graph shape
and retained its own bounded cache, parameter reset and persistent GPU worker.
The complete reference parameter combination was actually measured and rejected:
the 256-dimensional diagnostic recalls fell more than0.005 below the paired
frozen-baseline bridge. The selected product24b972ab uses NN40,
MULTI_CTA/width4 and floor1024 and passed that new paired comparison.

The other public V5 implementation was reviewed at
`70bbc4290789640b7ee98f7241f058e84c77501b`:
https://github.com/Terrified-of-Logic/seekdb-ans/tree/70bbc4290789640b7ee98f7241f058e84c77501b .
Its more aggressive dimension-dependent settings were not imported. No reference
author's score, report, screenshot or execution identity was reused. The public
GitHub author-to-full-score-team/actual-submitted-SHA chain is not independently
confirmed; these links identify source mechanisms, not this team's grade.

The [fixed C API](https://github.com/AMD-Ecosystem/hipVS/blob/f96a663d0a4441d6f5344bd3ef552b71ae7b370b/cpp/include/cuvs/neighbors/cagra.h)
exposes supported CAGRA search parameters, including MULTI_CTA.
The implementation continues to call its original CAGRA search API. Neither
hipVS/cuVS nor the official SQL/index options are modified. Source and measured
results for that historical product are bound in RESULTS-20261102.md. It later
received official 10.425/20 (build1.415x/query3.560x/gates pass).

## Revision 2: independent uniform small-graph experiment

The fixed `70bbc4290789640b7ee98f7241f058e84c77501b` reference above provides a
public clue that lower graph degrees can reduce build work. This revision uses
uniform graph/intermediate degree128/128 for every supported dimension, NN12,
and this team's existing MULTI_CTA1024/width4 search/worker/cache implementation.
Its business logic, data, tests, screenshots and score were not copied; the
reference's dimension-dependent routing was not imported. Parameter selection
was an independently implemented, measured change from this team's 192/192
NN12 candidate. The reference-to-full-score-submission chain remains unverified.

The CPU-budget follow-up independently adds calling-thread cgroup membership,
mount-root and ancestor-limit handling, plus task-local OpenMP save/limit/restore
to the attributed affinity foundation. It changes no global environment or
fixed library. CPU-boundary tests and a separate real-GPU preinitialized-OpenMP
diagnostic were retained, with old and final-header identities distinguished.

The measured product source is `16433ab78c539466b7123e1d3f6673cac41812ff`.
This integration is a descendant of the prior documented answer and contains
the identical five changed product/test blobs. The new paired results and all
source/artifact distinctions are in [RESULTS-20261102-v2.md](RESULTS-20261102-v2.md).
Official Hidden250K, GPU-path acceptance and the new score remain unclaimed.
