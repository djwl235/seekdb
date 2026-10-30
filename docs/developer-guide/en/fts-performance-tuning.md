# Full-text path tuning

Full-text work repeatedly decodes characters, creates parser-local dictionary
wrappers, hashes duplicate words, compares binary document IDs through generic
collation dispatch, and computes relevance for some aggregate queries that only
observe membership. This change reduces that work while preserving tokenization,
index contents, document membership, projected scores and ranking.

## Tokenization and dictionary changes

* Reuse the character address, length and classification already read before IK
  processor dispatch. Decode Unicode once per classification, retaining the
  original predicate order and UTF-8/UTF-16 surrogate distinctions. For the
  common UTF-8 collations, one existing validated Unicode decoder now supplies
  both character width and classification. Invalid input and other collations
  retain the original validation and error path. The fixed four IK processors
  use an inline pointer array and concrete calls in the original order.
* Binary-search the ordered, disjoint first-character dictionary ranges using
  their inclusive ends, then check the candidate start to preserve gaps.
* Borrow already-lowercase ASCII BENG input slices. Uppercase and non-ASCII
  input still use charset conversion; owning parser copies retain their lifetime.
* Update duplicate-token key spelling and frequency in the existing hash-map
  set_or_update callback under its bucket write lock. This preserves the last
  surface spelling for collation-equal tokens, including cafe/café, while
  avoiding repeated hashing and replacement-node allocation. Initial frequency
  accounting is unchanged. Identical byte strings under supported binary or
  UTF-8 metadata can compare equal directly; byte differences still use collation.
* Replace IK arbitration's per-segment hash table with a small array and forward
  cursor. Conflict groups are appended in increasing, disjoint offset order;
  smart-mode selection takes a subset within each group and preserves that order.
  The result lookup retains exact-start-offset and skipped-character behavior.
* Share the three immutable built-in UTF-8 IK dictionaries through the process
  parser hub. A dedicated initialization lock serializes cold construction;
  only successful initialization is published with release/acquire semantics.
  The hub retains the KV-cache handles alongside the range wrappers, so parser
  destruction and cache eviction cannot invalidate borrowed dictionary data.
  Failed initialization is not published and can be retried. There is no runtime
  update path for these built-in dictionaries.

TOKENIZE also retains one bounded, successful default-configuration normalization
per worker, keyed by the exact versioned parser name. Normalization depends on
fixed UTF-8 binary metadata and compiled defaults. Explicit configurations still
receive full validation; token input, output and errors are never cached. Cached
bytes are copied to call-owned storage before use, including a stack snapshot
before allocation to tolerate cooperative scheduling.

The server releases the parser hub before destroying the global caches and their
hazard-pointer domain. Shared DAT readers only read dictionary state; token-hit
state remains local to each parser.

## Retrieval changes

Fixed-length, non-null 16-byte CHAR/VARCHAR document IDs under binary collation
use byte comparison in retrieval merge and BMW comparisons. This exactly matches
that collation's memcmp-plus-length ordering. Other lengths, collations, types,
nulls and sentinel values retain their original comparison paths.

A conservative execution flag applies only when a scalar aggregate uses a plain
natural-language MATCH solely as its positive WHERE predicate. Code generation
checks all statement relation expressions, rejecting score projections, numeric
comparisons, HAVING score uses, Boolean mode, boosts, subqueries, sorting, limits,
functional lookup and index merge. All generated child definitions, expressions
and result columns remain present; the optimizer does not change the plan shape.

For that path, DAAT skips document-frequency estimation, BM25 calculation and
reinterpreting BOOL(MATCH) for every row. Each posting contributes an internal
positive value. Membership is equivalent because BM25 clamps IDF to a positive
epsilon and average document length to a positive default, while posting term
frequency is positive. Numeric scores are unobservable under the eligibility
checks. The posting union, minimum-match accounting and projected document IDs
remain unchanged. Scored and top-k queries retain their original calculations.
The DAAT wrapper also reuses relevance already evaluated by its token iterator.

Related directions appear in the public design notes of
[upstream PR #2235](https://github.com/oceanbase/seekdb/pull/2235); the changes here
were implemented locally and validated against the unmodified parent.

## Verification

The native release build passed. Eleven regression cases passed, including
fts_hot_path_semantics, tokenize_function, parser_properties, cn_word,
basic_dml_sequel, create_fts_index_afterward, create_table_with_fts_index,
drop_index, index_view, domain_id_merge_with_compare_null and simple_query.

The added oracle case covers token frequencies, case/accent variants, stopwords,
punctuation, empty input, fullwidth characters, CJK, emoji, dictionary boundaries,
long text, IK modes, binary keys containing NUL/high bytes, exact SUM/AVG scores,
ranking/ties, thresholds, HAVING, nested scores, Boolean queries, prepared
statements, index construction and transactional rollback. Expected results were
recorded with the unmodified parent binary. A cold instance also completed 120
concurrent/reconnect tokenizer calls across 16 clients, with output byte-identical
to the parent. Another 204-case differential run covered valid Unicode width
boundaries, random mixed text, three UTF-8 collations, both IK modes and SQL
conversion of malformed byte strings. Returned text, status and diagnostics
matched the unmodified binary. These SQL conversion cases do not prove direct
coverage of every invalid-byte decoder path. The CLI stop signal returned exit 0; its process exit bypasses
cache teardown, so destructor ordering was checked in the server lifecycle code.

The existing partitioned-query expected result has a placeholder-number mismatch
(:0/:1), with complete parent/candidate outputs byte-identical. The large
basic-DML case reaches a legacy missing mapping-table metadata assertion on both
versions. Existing tests and expected results were not altered to mask these.

## Full benchmark

The official benchmark, generator, SQL, scoring script and baseline are unchanged.
Runs use all defaults: 20,000 documents, batches of 500, 3,000 tokenizer repetitions,
200 query repetitions, three samples, 30 warmups and SKIP_LOAD=0. Mac compatibility
executables supply GNU Bash and actual nanosecond time to the unmodified script.
Both versions use the same native build configuration and dedicated 2 GiB server.

The following fresh-instance comparison used parent 1412f7e immediately after the
first membership candidate, followed by the final predicate-elision candidate.
Values are seconds for construction and milliseconds for tokenizer/queries.
Machine load varied markedly across earlier runs; those earlier timings must
not be used to attribute current gains.

| Metric | Parent | Optimized | Time reduction |
| --- | ---: | ---: | ---: |
| build_ik_all_sec | 4.6790 | 4.4690 | 4.49% |
| build_ik_content_sec | 3.5500 | 3.3550 | 5.49% |
| build_beng_en_sec | 1.9350 | 1.9380 | -0.16% |
| tokenize_ik_avg_ms | 0.0788 | 0.0752 | 4.57% |
| tokenize_beng_avg_ms | 0.0428 | 0.0431 | -0.70% |
| query_cn_avg_ms | 0.9737 | 0.8459 | 13.13% |
| query_beng_avg_ms | 1.4490 | 1.2442 | 14.13% |
| query_mixed_avg_ms | 1.0948 | 0.9378 | 14.34% |
| query_limit_avg_ms | 1.2731 | 1.2151 | 4.56% |

All hit counts matched: 8,001 / 11,000 / 7,332 / 20. Local timings are not official
scores. The independent submission branch is based directly on official 5bce9e0,
containing only this task's changes; local native builds reuse a parent checkout
that also contains earlier AI/file features.

## Online scoring discrepancy

Online 12af99f, official unmodified base 5bce9e0 and second-pass 930e576 all
returned successful execution but 0/50. Their reported mean improvements were
-0.413077, -0.104091 and -0.091495. The first raw metric set instead scores 100/100
(mean +0.785393) using the checked-in, unmodified public scoring function and
baseline. The actual online baseline remains unresolved. Raw reports and local
comparisons are retained; no reference score is represented as an online score.
The combined execution-path version 793011f completed its online evaluation on
2026-10-30 at 13:05 and received **15.078/50**, with reported mean improvement
+0.150782329 and raw performance score 30.156466/100. The complete online metrics
are retained separately. That is the verified online result, below full marks.

## Follow-up tokenizer measurements

A fresh adjacent comparison against the saved 793011f-equivalent binary, using
all the same benchmark defaults, measured IK TOKENIZE at 0.0767 -> 0.0731 ms
(4.69% reduction) and BENG at 0.0430 -> 0.0423 ms (1.63%). IK build times were
4.377 -> 4.374 s and 3.348 -> 3.256 s; BENG was 1.839 -> 1.838 s. Query times were
0.8297/1.2124/0.9322/1.2007 -> 0.8087/1.1652/0.9069/1.1930 ms, with identical hit
counts. Only tokenization/configuration changed in this follow-up; differences
in construction and retrieval timings are not attributed to a new query change.
These local measurements are not a new online score.


## Bounded auxiliary-index build concurrency

Phase timing identified the two large, independent auxiliary-index INSERT SELECT
jobs as the dominant construction cost. Mini mode previously had one local-build
worker, so both synchronous inner SQL jobs ran sequentially. The same queue
already supports 16 concurrent workers outside mini mode; its tasks and completion
notifications support out-of-order completion.

Mini mode now uses one worker per existing minimum-size (1 GiB) memory budget,
clamped to [1, 2]. Budgets below 2 GiB keep one worker; normal mode keeps 16.
Memory quotas, per-job SQL parallelism, snapshots, dependency checks and completion
handling are unchanged. The mapping-table dependency still completes before the
parent submits its independent auxiliary tables. This also permits independent
ordinary index jobs to overlap; the scope is the local DDL build queue, not only
FTS tokenization.

The release build and eleven regression cases passed after temporary phase
instrumentation was removed. The oracle case additionally overlaps two FTS
creates and one ordinary-index create, then checks exact membership, scores,
normal-index contents and subsequent DML. The same case passes at 1 GiB,
exercising the serial fallback. Its expected output was recorded with the
unmodified parent binary.

A fresh adjacent, uninstrumented comparison against the saved 80a277c-equivalent
binary, both with the same 2 GiB budget and all official benchmark defaults,
measured:

| Metric | One worker | Bounded two workers | Time reduction |
| --- | ---: | ---: | ---: |
| build_ik_all_sec | 4.785 | 3.196 | 33.21% |
| build_ik_content_sec | 3.557 | 2.141 | 39.81% |
| build_beng_en_sec | 1.937 | 1.229 | 36.55% |
| build_total_sec | 10.296 | 6.583 | 36.06% |

All four hit counts matched. Tokenization and retrieval remained within the
nearby timing range. The wrapper's Darwin maximum-child-RSS readings were
390,250,496 and 422,690,816 bytes respectively; these include the server and
short-lived client children, not a whole-system memory measurement.

The preceding tokenizer-only submission 80a277c actually received **4.050/50**
online, despite IK tokenization improving from 0.0747 to 0.0712 ms. The highest
verified score remains 793011f's **15.078/50**. Under the public positive-weight
formula, the two nine-metric sets and reported improvements cannot both use the
same fixed baseline. The actual online scoring inputs remain unresolved; this
construction change still requires its own online evaluation.


## Short posting-list unions

Online 85d1048 completed successfully at **21.849/50** (mean 0.2184891284).
Its total construction time fell from the previous submission's 16.451 s to
8.672 s, with unchanged hit counts. This is the current verified online score.

The execution-only membership path now returns a positive posting contribution
directly, avoiding temporary relevance-datum writes and copies. For one to eight
natural-language posting lists, with no filter, limit or functional lookup, the
text iterator directly selects the minimum of the current document-ID heads and
advances every tied list together. This bypasses the generic heap protocol while
retaining its ordered union, duplicate handling, minimum-match collector, batch
buffer ownership, counters and result projection. Scored queries, Boolean mode,
limits, larger term sets and other sparse-retrieval implementations retain their
original merge paths. Reuse reloads every head before comparing it.

The release build and eleven regression cases passed. A 1024-row oracle fixture
adds interleaved/tied postings across scan batches, one/two/three/four/eight/nine
terms, repeated and absent terms, prepared-statement transitions across the fast
path boundary, and transactional deletion/rollback. Expected output comes from
the unmodified parent binary.

A fresh adjacent comparison against the saved 85d1048-equivalent binary measured
query_cn 0.8299 -> 0.6734 ms, query_beng 1.2039 -> 0.9941 ms and query_mixed
0.9263 -> 0.7714 ms (16.7-18.9% reductions). LIMIT used the original path and
measured 1.2086 -> 1.2301 ms. All hit counts matched. The official benchmark and
scoring inputs are unchanged; these local readings are not an online score.
