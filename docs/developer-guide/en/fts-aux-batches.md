# Batched FTS auxiliary-table construction

FTS index/doc-word auxiliary-table scans previously forced scalar output. They
can now return a bounded batch of generated posting rows from one source
document. A larger document continues on the next call. A batch never fetches
the next document after producing a posting row, because that would invalidate
the current source datums, document ID and pass-through columns.

Before expression frames and physical operators are generated, an already
vectorized online-DDL INSERT into either FTS auxiliary-table kind can remove
the small-*source-document*-scan batch clamp. It recomputes capacity through the
existing expression/frame/LOB/target-size budgets. Scalar/single-row decisions,
variable-assignment restrictions and explicit ROWSETS_MAX_ROWS hints are kept.
The plan, expression frames, scan and insert therefore share a compatible
capacity. Ordinary statements do not enter the extra context/schema lookup.

The four generated columns retain the existing aux/doc-word mapping and
operator-local lookup cache. Generated scalar expressions limit output to one
row; batch bitmap operations apply only to batch expressions. Pass-through
values are evaluated at the source slot and copied within the document's
batch. Source skip bits, NULL/zero-posting documents and source end are handled
before publishing output. A final source row carrying end still drains all its
posting rows. Evaluation/source errors publish no partial batch. Closing and
rescanning discard posting continuation as well as the expression map.

The batch-generation mechanism is adapted from Apache-2.0 reference
Terrified-of-Logic/seekdb-ans commit
`f91d5654a13b00e36c4703f415315d9b80ca47d7` in the table-scan and code-generator
files. Existing OceanBase notices are retained. This change does not import its
global sort-memory policy, bounded-query optimizer/DAS changes or benchmark
changes. The earlier guarded TOKENIZE folding and comparison fast path remain.

`tools/regression/test_fts_batch_protocol.py` extracts nine production methods
verbatim and executes them with bounded C++ interfaces from
`fts_batch_protocol_fixture.hpp`. Its TEST_ONLY protocol cases cover capacities
1/2/default256, both auxiliary orders,515 posting rows across batches, two
different document/pass-through values, scalar outputs, skip/NULL/zero-posting
source states, source-end variants, mid-document rescan, failed close and error
propagation. It models source/evaluation/segmentation interfaces; it does not
execute real tokenization, decode a real LOB, plan SQL or measure performance.

Example with Clang17 and LLD already available:

```
python3 tools/regression/test_fts_batch_protocol.py --out /tmp/fts-protocol-unique \
  --compiler /path/to/clang++ --linker /path/to/ld.lld
```

The existing SQL regression requires a linked, identified candidate server.
Production object compilation and method-level protocol checks do not replace
candidate SQL or the unchanged official end-to-end benchmark.
