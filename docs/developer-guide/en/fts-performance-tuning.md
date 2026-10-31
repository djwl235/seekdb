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
online, despite IK tokenization improving from 0.0747 to 0.0712 ms. The then-highest
verified score was 793011f's **15.078/50**. Under the public positive-weight
formula, the two nine-metric sets and reported improvements cannot both use the
same fixed baseline. The actual online scoring inputs remain unresolved; this
construction change still requires its own online evaluation.


## Short posting-list unions

Online 85d1048 completed successfully at **21.849/50** (mean 0.2184891284).
Its total construction time fell from the previous submission's 16.451 s to
8.672 s, with unchanged hit counts. This was the verified online score before the short-union follow-up.

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


## Predecoded dictionary-range boundaries

Online 88b18ce completed successfully at **33.698/50** (mean 0.3369793941).
Chinese/English/mixed query times were 0.8023/1.2243/0.9438 ms, compared with
1.1997/1.8277/1.3366 ms in the preceding run. LIMIT retained scoring and measured
1.7389 ms. All four hit counts matched. Total construction was 8.169 s and
IK/BENG TOKENIZE was 0.0663/0.0387 ms. This is the highest verified online score;
it remains below 50/50.

UTF8MB4_BIN dictionary ranges now retain decoded single-character boundaries.
The search validates and decodes the input once, then binary-searches integer
endpoints rather than decoding both operands in every collation comparison.
The shortcut requires valid, ordered, disjoint single-character endpoints and
the expected binary UTF-8 dictionary metadata. A space, invalid character,
multi-character input or unsupported range layout retains the original collation
comparison, including PAD SPACE behavior. Dictionary contents and cached DAT
serialization do not change. Shared range metadata remains immutable.

The full benchmark experienced a simultaneous SQL baseline increase from 0.0349
to 0.1118 ms; those runs cannot attribute absolute differences to this change.
An additional adjacent diagnostic used the SQL BENCHMARK function for 10,000
TOKENIZE evaluations per sample, three samples after a 1,000-evaluation warmup.
IK averaged 0.04657 ms before and 0.03980 ms after (about 14.5% reduction),
while unchanged BENG measured 0.01165 and 0.01138 ms. This diagnostic reduces
client round-trip effects and supplements, rather than replaces, the unchanged
official benchmark and actual online evaluation.

The standalone range-boundary candidate passed the native release build, all
eleven regression cases, and the 204-case Unicode/collation/mode differential
against the original binary. An experimental CJK prefix-array change was removed
because its separate performance benefit was not established.


## Reusing final scalar property conversion

The final normalized-JSON-to-parser-property conversion retains one successful
entry per worker, bounded to 2 KiB of JSON plus the exact parser name/version and
scalar values. Caller validation/rebuilding remains unchanged, including explicit
TOKENIZE configuration checks. A hit only updates the fields the original
conversion updates for that parser; IK dictionary-name strings refer to static
literals. No JSON tree, document pointer, token output or error is retained.
Key reads and publication allocate nothing and cannot yield. Oversized inputs
and cache misses use the original conversion.

The native build and eleven regression cases passed, including default/custom
configuration changes, invalid configuration followed by recovery, both IK modes,
and ngram/space/ngram2 properties. A cold instance completed 120 concurrent and
reconnect calls with output identical to the original binary. The diagnostic
inner loop measured IK at about 0.03725 ms after range decoding's 0.03980 ms;
BENG remained around 0.0115 ms without an established improvement. The complete
official benchmark also ran, but its elevated SQL baseline prevents attributing
absolute timing differences to this change.


## Immutable character-code lookup

UTF-8 binary cache readers optionally build a 65,536-entry table of existing DAT
character codes during cold range construction. Each table uses 256 KiB, owned
by the pinned range dictionary's allocator. Its entries are populated through
the original hash lookup, preserving its result even for duplicate cache entries.
Only validated single-character BMP UTF-8 input uses direct lookup; supplementary
characters, invalid encodings and multi-character input retain the original hash
path. Allocation failure retains that path as well. The serialized DAT and
its original hash map are unchanged. Tables are immutable before publication.

The native build, eleven regression cases, 204-case Unicode differential and
120 cold/concurrent/reconnect calls passed against the original binary. The
semantic fixture also passed with a 1 GiB memory budget. An adjacent full official
benchmark against the saved scalar-property version measured IK TOKENIZE at
0.0685 -> 0.0661 ms (3.5%); the additional inner-loop diagnostic measured
0.02156 -> 0.02015 ms (6.6%). BENG remained unchanged. Build totals were
5.755 -> 5.788 s and query times 0.7130/1.0497/0.8175/1.2673 ->
0.6949/1.0351/0.8055/1.2829 ms, with matching hit counts. These variations do
not establish new construction or retrieval improvements. Raw maximum-child RSS
was 417,628,160 -> 415,170,560 bytes; it includes clients and is too noisy to
measure this table's memory cost.

The eighth online submission c4609dd executed successfully but received
**29.712/50**, below the highest verified **33.698/50**. Its complete online
metrics are being compared before attributing the score change.


## Compact membership posting IDs

Membership-only binary CHAR/VARCHAR posting batches use owned 16-byte ID buffers
with an associated datum (32 bytes per slot), instead of the general 40-byte ID
buffer plus datum (56 bytes). The unused relevance array is also omitted on this
path. Every row must be unskipped, non-null, non-extension and exactly 16 bytes;
otherwise the complete batch uses the original general-purpose cache, allocated
lazily when needed. Scalar/non-batch input uses that fallback. Each compact copy
rebinds its datum to its own storage, and reset/reuse clear the selected format.
Numeric relevance, sorting, LIMIT and other general retrieval paths are retained.

The release build and all eleven regression cases passed, including the existing
1024-row overlapping posting lists, prepared statements, exact scores, ranked
queries and transaction rollback. A supplementary fixed-work query diagnostic
ran control/candidate and candidate/control pairs, each with 500 warmups followed
by three samples of 3000 queries per kind. All hit counts matched. The first
pair measured CN/BENG/mixed at 0.7558/1.0518/0.7834 ->
0.6360/0.9733/0.7192 ms. In the reverse pair the candidate measured
0.6870/0.9745/0.7253 versus control 0.6946/1.0755/0.8250 ms;
CN's smaller repeat benefit demonstrates remaining timing noise. Unchanged LIMIT
remained around 1.3 ms. Child CPU time (server and SQL clients, including startup,
warmup and shutdown) decreased 37.542 -> 34.666 s and 37.308 -> 35.524 s.
These diagnostics supplement the unchanged official benchmark; they are not
online scores or measurements of server CPU alone.

The ninth online submission a731a19 completed with **30.212/50** and correct hit
counts, leaving **33.698/50** as the highest verified result. Its build total was
8.499 s, IK/BENG TOKENIZE 0.0641/0.0405 ms, and CN/BENG/mixed/LIMIT queries
0.8990/1.3679/1.0435/1.8196 ms. No full-score claim is made.

The unchanged complete official benchmark also passed with defaults: 20,000
documents, batch 500, TOKENIZE 3,000 rounds, queries 200 rounds, three samples
and 30 warmups. Build total was 8.630 s, IK/BENG TOKENIZE 0.0747/0.0461 ms,
and four queries 0.7026/1.1124/0.8735/1.4542 ms with exact expected counts.
SQL-loop baseline was around 0.050 ms and timing variance remained material;
this run is retained as validation, without inferring an online score.


## Invocation-local word maps

The five word-map creation sites in TOKENIZE, document-row generation, the two
retrieval implementations and optimizer token selectivity estimation are stack
variables used synchronously. They now use the shared ObFTWordMap alias with
NoPthreadDefendMode for both buckets and its invocation-owned node allocator.
No map or allocator is shared by workers. Hashing, collation equality, bucket
counts, node insertion order and iteration order are retained. The process-wide
stopword set retains its existing synchronization. Raw template declarations in
both query implementations and the optimizer are updated to the same alias.

A complete native rebuild passed. Twelve established-suite regressions passed,
including a new stopword/case/accent/NUL/collation/IK-mode fixture whose expected
output was recorded with the original binary. All 204 Unicode differential cases
and 120 cold/concurrent/reconnect calls matched the original. The original build
does not support latin1, so no Latin1 coverage is claimed.

An adjacent supplemental TOKENIZE inner loop measured IK 0.019369 -> 0.018007 ms
(7.0%) and BENG 0.006421 -> 0.004843 ms (24.6%). The complete unchanged official
benchmark measured IK 0.0681 -> 0.0656 ms and BENG 0.0469 -> 0.0436 ms. Build
sums were 5.658 -> 5.776 s, without a measured construction gain; query timings
were 0.6696/0.9965/0.7556/1.2941 -> 0.6360/0.9401/0.7152/1.2729 ms.
All hit counts matched, but timing noise prevents assigning the small query
variation to this tokenizer change. These are local results, not online points.

The tenth online submission 8be514c completed with **36.685/50**, the highest verified score at that submission. Its complete online metrics are retained separately:
7.494 s total construction, 0.0603/0.0382 ms IK/BENG TOKENIZE and
0.7834/1.1880/0.9257/1.7312 ms for CN/BENG/mixed/LIMIT, with correct hit counts.


## Immutable ASCII stopword keys

Stopword initialization derives a sorted array of 64-bit ASCII letter keys from
the same built-in list used by the original set. The array is read-only before
workers use it. If a future list contains non-letter or longer-than-eight-byte
entries the shortcut stays disabled. For binary and the three tested UTF-8
collations, ASCII-letter input uses case-folded keys; long ASCII words cannot
match a shorter stored key. Accents, fullwidth characters, punctuation, NUL and
other charsets retain the original conversion and general-ci set lookup. The
original shared set and its synchronization are retained. No input or token
result is cached.

The native build, twelve regressions, 204 Unicode cases and 120 concurrent/cold/
reconnect calls passed against the original binary. An adjacent inner-loop
control measured IK 0.021079 and BENG 0.005946 ms; the candidate measured
0.020976 and 0.004813 ms. IK is unaffected by this stopword path; BENG's reduction
was about 19%. A reverse-order repeat experienced simultaneous control slowdown
and is retained without attributing its larger difference to this change.

The unchanged default full benchmark also completed with expected hit counts.
BENG TOKENIZE was 0.0440 -> 0.0428 ms. Build totals were 6.255 -> 6.357 s.
Query samples, especially mixed and LIMIT, were noisy (candidate standard
deviations 0.5118 and 0.4052 ms); no retrieval improvement is inferred from
these runs. Official benchmark scripts, SQL, scoring and baselines are unchanged.


## Cardinality-only limited derived scans

The eleventh online submission c268468 completed with **38.786/50**. Its
unmodified report records 8.298 s construction, IK/BENG TOKENIZE 0.0598/0.0371 ms,
and queries 0.8589/1.3167/0.9998/1.8382 ms with correct hit counts.

A direct COUNT(*) over a single ordinary derived table does not observe that
table's projected values. For a plain natural-language MATCH with LIMIT/OFFSET,
no other filters and column/literal projections, preprocessing replaces those
unused projections with a constant and omits the implicit relevance ordering.
The proof excludes value-reading parents (including COUNT(expr), SUM and outer
filters), shared CTEs/views, explicit ordering, grouping, windows, DISTINCT,
WITH TIES, percent limits, subqueries, assignments and locking/output side
effects. LIMIT/OFFSET execution is retained. Code generation allows the existing
membership-only execution mode for an unordered all-constant limited projection;
scored or value-producing queries retain their original execution paths.

The plan becomes a covering posting scan (access(nil), is_index_back=false),
without a relevance sort. All existing child definitions/expressions remain
available. Nothing caches query results, documents or token output.

The native build passed. The new limited-count regression was recorded with the
original binary, then matched by the candidate. It covers zero/short/oversized
limits, offsets including cross-batch cases, no hits, empty terms, NULL, parents
that observe values, explicit ordering, Boolean mode, dynamic prepared limits
and transaction rollback. Twelve of thirteen selected regressions matched their
checked-in expected results. The remaining drop_index test compares metadata
rows without ORDER BY; its complete actual output matches the saved previous
binary byte for byte, and its old expected ordering was left unchanged.

Adjacent complete official default runs measured LIMIT 2.3002 -> 0.2511 ms,
with expected hits 8001/11000/7332/20. Other query times were
1.2107/1.7613/1.3796 -> 1.1863/1.7298/1.3333 ms. Build and tokenizer timing
fluctuated and no gains there are attributed to this query-only change.
A supplemental fixed-work run (500 warmups and 3 x 3000 per query group)
measured LIMIT about 2.4840 -> 0.1579 ms and total child CPU 65.860 -> 47.242 s.
That CPU includes server, SQL clients, startup, warmup and shutdown. These local
measurements do not predict an online score. Official benchmark scripts, SQL,
timers, scoring and baseline files remain unchanged.

The suggested public reference repository Apical-7280/seekdb-2026-competition
was inspected read-only. At inspection master ce31fae differed from its official
parent 3bdb363 only by README verification text, and its 2026-competition ref
still pointed at 5bce9e0. No competition optimization commit was identified and
no source was copied or cherry-picked from that repository. This change was
derived from the local execution plans and independent cardinality analysis.


## Inlined posting cursor

Posting advancement now keeps the owned-batch cursor increment in a small
inline method and refills through a separate private function only at batch
boundaries. Current-ID lookup is inline too. The original increment order,
scalar/vector refill logic, buffer ownership, error returns and relevance
calculation are retained. No document or query results are cached.

The native build passed. Seven relevant regressions passed in both the normal
256-row configuration and the scalar configuration (_rowsets_max_rows=0).
The scalar runs also matched the previous saved binary's results. Coverage
includes cross-batch overlapping postings, empty/no hits, prepared statements,
exact relevance, Boolean expressions, limited counts, DML and rollback.

Adjacent fixed-work queries measured CN 0.6115 -> 0.5899, BENG 0.8983 -> 0.8632,
and mixed 0.6917 -> 0.6783 ms. LIMIT stayed near 0.086 ms. Total child CPU was
22.190 -> 21.456 s (server/clients/startup/warmup/shutdown). A second adjacent
comparison using the complete unchanged official default benchmark measured
CN 0.6257 -> 0.6125, BENG 0.9401 -> 0.9199, mixed 0.7339 -> 0.7235 ms and
LIMIT 0.1472 -> 0.1473 ms. Construction was 5.646 -> 5.643 s; no construction
or tokenizer improvement is attributed to this cursor change. Hit counts were
correct in every group. These are local observations, not online points.

The preceding limited-count submission 9ab0101 completed online at 37.995/50,
while the highest remains c268468's 38.786. Online LIMIT improved from 1.8382
to 0.1294 ms, other queries were 0.8572/1.2994/0.9993 ms, construction 7.847 s,
and IK/BENG TOKENIZE 0.0606/0.0376 ms. The report alone does not explain the
slightly lower overall score; no baseline/scoring changes were made.
