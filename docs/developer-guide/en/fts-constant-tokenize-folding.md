# Constant TOKENIZE projections

This change builds on `4ba924f2aa069994dd070ffc4071e3da4b94d62a`, the team's existing full-text tuning branch. It does not change the benchmark, inputs, timers, scoring, parser rules, posting traversal, or existing caches.

A top-level SELECT-list TOKENIZE with static scalar arguments can be evaluated during planning. The JSON result is copied into the plan allocator using the existing SQL constant-expression evaluator. Identical statements can then reuse the plan-owned value instead of repeating segmentation on every execution.

The SELECT resolver registers every parameterized literal below TOKENIZE as a plan-cache constant constraint, including literals nested in CONCAT or other constant arguments. Changing text, parser, or properties must select a compatible plan. Real prepared-statement dynamic parameters and expressions containing user or system variables stay on the normal execution path. A recursive guard also rejects the engine's non-pure system functions, current-time and last-insert-ID flags; session identity and runtime state are never folded into cached tokens. Trees deeper than 64 levels conservatively retain runtime evaluation. Other SELECT expressions and relation predicates are unchanged.

The evaluator retains each argument's resolved metadata and normal parser/dictionary loading. The existing session plan-cache dependency on connection collation remains in force. This optimization relies on the current publish-once built-in dictionaries; support for mutable dictionaries would require a version dependency or disabling this fold. Failed pre-evaluation retains ordinary runtime evaluation.

Source attribution: the SELECT-list folding and constant-parameter constraint mechanism is adapted from [Terrified-of-Logic/seekdb-ans, fixed f91d5654a13b00e36c4703f415315d9b80ca47d7](https://github.com/Terrified-of-Logic/seekdb-ans/tree/f91d5654a13b00e36c4703f415315d9b80ca47d7), Apache-2.0. Original source license notices are retained. No score or repository-to-team mapping is inferred from this reference.

A differential real-SQL regression helper is provided separately from the official benchmark:

```sh
python3 tools/deploy/mysql_test/extra/fts_const_tokenize.py \
  --port <isolated-source-server-port> --source-sha <full-server-source-sha> \
  --output <new-evidence-directory>
```

It exercises changing literals and user variables, nested literal expressions, repeated EXECUTE bindings, parsers and properties, NULL, unsupported inputs, and two connection collations. Its supplied source SHA must be linked externally to the actual server binary receipt. The explicit reference-only mode labels shared/prebuilt runs as harness diagnostics, not candidate acceptance. The companion fts_runtime_values.py checks current time, LAST_INSERT_ID, a changing session system variable, and distinct connections against a runtime-variable oracle; its --reference-only mode has the same diagnostic boundary.

Local object compilation and this helper do not establish an end-to-end performance improvement. A source-linked candidate run of the unchanged official workload and semantic regressions is required. No new speedup or score is claimed in this document.
