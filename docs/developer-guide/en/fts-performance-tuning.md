# Full-text tokenizer hot-path tuning

The full-text parser is shared by index construction, `TOKENIZE`, and query-token
preparation. This change reduces repeated work there without changing the
posting-list format, token rules, stopword filtering, or relevance calculation.

## Bottlenecks and changes

* IK already reads a character's address, length and classification before
  dispatching its processors. Pass those values to `do_process` rather than
  fetching them twice again in each of the four processors. Processor order is
  unchanged and none of the processors advances the cursor.
* Character classification used to decode the same Unicode character separately
  for each category predicate. Decode it once and evaluate the original category
  predicates in their original order. UTF-8 and UTF-16 surrogate handling remain
  distinct, including error propagation from decoding.
* IK dictionaries are split into ordered, non-overlapping first-character ranges.
  Binary-search their inclusive ends, then check the candidate's start. This
  preserves both range endpoints and gaps while reducing collation comparisons.
* BENG tokens consisting entirely of ASCII without uppercase letters already
  have their lowercase form. Borrow the input slice for these tokens, retaining
  charset conversion for uppercase and non-ASCII input. As before, the returned
  normalizer buffer is valid until the next stream call; the full-text parser
  continues to make its owning copy.
* Try inserting a word before doing a separate lookup. Distinct words require
  one lookup. Duplicate words still use the existing overwrite operation, which
  preserves the last surface spelling when distinct byte strings compare equal
  under a case/accent-insensitive collation. First-occurrence and duplicate
  frequency accounting are unchanged.

No query-planning or relevance-calculation changes are included.

A second pass replaces the per-segment IK arbitration hash table with a small
array and a forward cursor. Conflict groups are appended in strictly increasing
offset order; optimization chooses a subset of each group, so it preserves this
ordering. Exact-offset matching and skipped characters are unchanged.

For retrieval merge and BMW document-id comparisons, the 16-byte, non-null
CHAR/VARCHAR case under binary collation uses a fixed-size byte comparison.
Other lengths, collations, data types, nulls and sentinel values retain their
original comparator. The result is normalized to -1/0/1. The binary collation
implementation itself uses memcmp and length comparison; equal fixed lengths
therefore have identical ordering.

These two directions were also discussed in the public design notes of
[upstream PR #2235](https://github.com/oceanbase/seekdb/pull/2235). This change
implements them locally and retains the existing scoring and execution pipeline.

## Correctness verification

`fts_index/fts_hot_path_semantics.test` covers token frequencies, repeated words,
case and accent variants, stopwords, punctuation, empty input, fullwidth
letters/digits, CJK, emoji, dictionary boundaries, long documents, both IK modes,
scored queries, ranking, thresholds, Boolean queries, prepared statements, index
construction, and transactional updates/rollback. Its expected results were
recorded from the unmodified parent binary before comparing the optimized binary.

The public benchmark, generator, SQL, scoring script and fixed baseline are
unchanged. Full benchmark runs use all defaults: 20,000 rows, batches of 500,
3,000 tokenizer repetitions, 200 query repetitions, three samples, 30 warmups,
and `SKIP_LOAD=0`. Local runs use the same native release build and dedicated
2 GiB test server for both versions. On macOS, an external Bash 5 runtime and a
`date +%s%N` compatibility executable provide the facilities required by the
unmodified Linux-oriented benchmark script.

Local measurements and official evaluation results are recorded with the
submission. Local timings cannot be treated as the official CI score.

## Local full-run comparison

The following pair used two newly initialized, otherwise identical test
instances (parent binary `1412f7e`, then this patch). Values are seconds for
index construction and milliseconds for tokenizer/query measurements.

| Metric | Parent | Optimized | Time reduction |
| --- | ---: | ---: | ---: |
| build_ik_all_sec | 9.9030 | 8.7760 | 11.38% |
| build_ik_content_sec | 7.8210 | 6.7090 | 14.22% |
| build_beng_en_sec | 3.7740 | 3.3740 | 10.60% |
| tokenize_ik_avg_ms | 0.2139 | 0.1399 | 34.60% |
| tokenize_beng_avg_ms | 0.1387 | 0.0820 | 40.88% |
| query_cn_avg_ms | 2.1865 | 1.9585 | 10.43% |
| query_beng_avg_ms | 3.1597 | 2.9687 | 6.04% |
| query_mixed_avg_ms | 2.4208 | 2.1815 | 9.89% |
| query_limit_avg_ms | 2.6388 | 2.6212 | 0.67% |

All four query hit counts matched: 8,001 / 11,000 / 7,332 / 20.
An earlier run reusing a test instance had a noisy LIMIT query measurement
(4.6840 ms); the clean-instance repetition measured 2.6212 ms. Both full reports
are retained rather than treating the outlier as a semantic or plan change.
The two captured query plans are byte-identical. The checked-in CI-baseline
scoring script gives this Mac run 100/100; this is a cross-machine reference
only, not an official submission score.

Eleven regression cases passed unchanged, including the new baseline-oracle
case. The partitioned-query case has an existing EXPLAIN placeholder-number
difference (`:0` versus `:1`); its complete output is byte-identical between
the parent and optimized binaries. The large basic-DML case also reaches an
existing missing legacy mapping-table metadata assertion on both binaries.

## Second-pass validation and evaluation discrepancy

The second pass again passed all eleven regression cases. The oracle case adds
binary keys containing NUL and high bytes, multi-term merging, score ordering,
ties and Boolean exclusion. Expected results were recorded with the parent
binary. The full benchmark was repeated with fresh instances in the same session
(candidate followed by parent). Earlier timings were about twice as slow even
for unchanged operations, so they must not be used to attribute second-pass gains.

| Metric | Parent (same session) | Current | Time reduction |
| --- | ---: | ---: | ---: |
| build_ik_all_sec | 4.8890 | 4.6730 | 4.42% |
| build_ik_content_sec | 3.8850 | 3.5520 | 8.57% |
| build_beng_en_sec | 2.0430 | 1.9430 | 4.89% |
| tokenize_ik_avg_ms | 0.0843 | 0.0751 | 10.91% |
| tokenize_beng_avg_ms | 0.0524 | 0.0444 | 15.27% |
| query_cn_avg_ms | 1.2433 | 0.9840 | 20.86% |
| query_beng_avg_ms | 1.5594 | 1.4101 | 9.57% |
| query_mixed_avg_ms | 1.1976 | 1.0850 | 9.40% |
| query_limit_avg_ms | 1.3730 | 1.2443 | 9.37% |

Online submission 12af99f returned success but 0/50, with mean improvement
-0.413077. The unmodified official base 5bce9e0 was independently submitted
and also returned 0/50, with mean improvement -0.104091. The first online
metric set scores 100/100 (mean +0.785393) with the checked-in, unmodified
public scoring function and baseline. The actual online scoring baseline is
therefore unresolved. No benchmark, timing, baseline or scoring file was changed.
