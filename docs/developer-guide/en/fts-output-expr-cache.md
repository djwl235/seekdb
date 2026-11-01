# FTS output-expression lookup cache

The FTS index/doc-word auxiliary-table build emits four generated columns for
each token row. `ObTableScanOp` now resolves their expression pointers once per
operator opening/rescan instead of scanning the output-expression array four
times per token. It retains the original scalar row generation, tokenization,
datum conversion, evaluation flags and index contents.

The lookup uses the existing auxiliary-table-specific expression-order arrays.
Only a fully resolved four-pointer map is published. The pointers refer to the
operator's specification, not row data or LOB storage. Opening, closing,
rescanning and destroying the operator invalidate the map, including a failed
close. The per-document expression lookup remains unchanged.

This small mechanism is adapted from the output-expression caching portion of
Terrified-of-Logic/seekdb-ans commit
`f91d5654a13b00e36c4703f415315d9b80ca47d7`,
`src/sql/engine/table/ob_table_scan_op.cpp`, under Apache-2.0. Existing OceanBase
copyright and license notices are retained. Its batch-generation, optimizer,
memory-policy and bounded-query changes are not included.

`tools/regression/test_fts_output_expr_cache.py` exercises small real SQL index
builds, including both auxiliary column orders, NULL/empty/stop-word documents,
a long document, updates, deletion and index reconstruction. Run it only against
an explicitly identified linked candidate server and an isolated database. A
successful object compilation or a test against an older server is not a
candidate-runtime or performance result. The official benchmark inputs are
unchanged; performance must be measured on the actual candidate binary.
