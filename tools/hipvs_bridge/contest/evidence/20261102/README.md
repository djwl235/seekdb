# Selected real-run evidence, 2026-11-02

Product source: `24b972ab8500e29b4e750f8bee7a6b5c5f5193a3`, uniform
graph192/intermediate192/NN40/MULTI_CTA/itopk1024/width4. This is a selected,
byte-preserved evidence package, not the original 62-file inventory or a new run.
All 38 included original files match that source inventory's SHA256 and size.
`manifest.json` identifies their original relative locations and package storage.

`paired/` contains the comparison, two resource summaries, artifact combination,
and four reports/lifecycles/loaded-library maps. `native/` contains the native
persistence/CPU-restart/delete/insert results and ID comparisons. The original
native `executed-sql.jsonl` is intentionally omitted because it contains vector
literals. No vectors, binaries, SSH connection files, credentials, or private
configuration are packaged. Run-directory and mapped-library paths in historical
logs describe the original environment; they are not required local package files.

`raw-queries-and-timing.zip` contains all four original `measurement/queries.jsonl`
files (32,768 total SQL answer/latency records, ID results only) and all four
original `resources.jsonl` files, plus its own manifest. Its 6,266,107 bytes have
SHA256 `7992a7ee07c8d36513b852bc486a75591bc0380c56b17a98827b5eef94b3b762`.
ZIP CRC and every extracted file's size/SHA256 were checked by readback.
`packaging-qa.json` records packaging checks, not an engine test or official score.

| Case, in actual run order | Build through first query (s) | QPS@10 / @100 | Recall@10 / @100 |
| --- | ---: | ---: | ---: |
| SIFT baseline | 46.995653306 | 148.777302 / 137.141741 | 0.999951172 / 0.999973145 |
| SIFT candidate | 9.880297942 | 938.092002 / 693.219053 | 0.999975586 / 0.999953613 |
| Diagnostic256 candidate | 12.505215913 | 683.155429 / 723.363160 | 0.990527344 / 0.977819824 |
| Diagnostic256 baseline | 48.109904557 | 129.114505 / 122.106722 | 0.951074219 / 0.898615723 |

The legacy `hidden` filename/JSON slot denotes **diagnostic-256**, a locally
generated Gaussian development dataset, not the competition's private Hidden250K.
Each report's `dataset_role` preserves this distinction. The 0.4/0.6 weighted
local speedups are build4.1879456854x/query5.6173347881x; these are not official scores.

| Dataset | Baseline/candidate sampled peak VRAM (bytes) | Baseline/candidate sampled peak host RSS (bytes) |
| --- | ---: | ---: |
| SIFT | 732938240 / 792985600 | 2796929024 / 2478972928 |
| Diagnostic256 | 861114368 / 921649152 | 3311579136 / 3378331648 |

These 20ms polling observations pass the recorded sampled limits; polling can
miss true peaks. Official GPU-path/Hidden/resource acceptance remains unproved
by this package. Original `UNKNOWN`, `PENDING_WINNER_SELECTION`, trace-OFF and
external-evidence-required fields are preserved verbatim, even where subsequent
work has progressed. In particular the original artifact-combination's pending
three-command build field is historical, not a claim about a later build.

Both paired variants used the identical recovered host binary from
`cd3323c7181c0eb76941741875d922a878164017`, with SHA256
`a0dc4e7f216ca4fc7f7d35e7a1ab2e6271a2feacda72aec3a3aa199979de03d1`.
It was not a fresh official-base host compile. Non-bridge source identity and
the actual baseline/candidate bridge hashes are recorded in the original artifact
combination and reports. These timing results do not stand in for the separate
final official three-command source build. Native observables passed, while
internal trace checks remain `UNKNOWN_TRACE_OFF`; the original Docker test was
not run by that native transport adapter.

`assemble.py` only copies, hashes, scans and archives supplied evidence. It never
starts a database or GPU workload. To rebuild the same archive from an available
original round directory, run `python assemble.py --source /path/to/round-g192n40
--output /path/to/selected-evidence`. The generated package manifest intentionally
does not claim to include its own hash or every original asset.
