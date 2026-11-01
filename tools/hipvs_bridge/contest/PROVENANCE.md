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
| NN-descent 20 iterations, graph degree 2*M, intermediate 4*M | Not enabled. V5 stays at 100 / 128 / 256 until both datasets' recall and performance can be measured. The reference's quality evidence is for another graph/search path. |
| Helper startup overlap and native HNSW snapshot validation | Not imported: different process, persistence and query architecture. V5 OBCUVS02 remains authoritative. |
| CPU fallback after GPU failure | Not imported. The candidate adds no fallback; V5's existing adaptor fallback must still be excluded by actual external GPU-path validation during formal cases. |
| Reference reports and account/environment evidence | Not copied or credited as this team's execution. No score claim is inferred from the branch name or reports. |

Workspace/parameter-cache implementation and contest measurement tools were
written for this candidate. They do not modify or vendor hipVS itself.
