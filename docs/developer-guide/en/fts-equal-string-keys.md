# Equal string keys in datum comparisons

This change follows the officially measured 46.805/50 version, merged at `96814f6d04dbac546c54d1f98d612845d07a04dc`. All constant-TOKENIZE plan-cache and runtime-state guards are retained.

Full-text index construction sorts many duplicate word keys. For a supported collation, strings with identical bytes and lengths compare equal, including when their buffers are distinct. The datum comparator now returns zero for that case before repeating the charset walk. Different bytes or lengths retain the original `ObCharset::strcmpsp` path, including case/accent rules and trailing spaces.

The collation check runs first. Empty strings never reach memcmp, and nonempty null pointers retain original validation. SQL NULL handling remains in the unchanged null-safe wrappers before string payload access. No FTS-specific literals, benchmark parameters, sort memory limits or query semantics are introduced.

The equality shortcut is adapted from Apache-2.0 [Terrified-of-Logic/seekdb-ans@f91d5654a13b00e36c4703f415315d9b80ca47d7](https://github.com/Terrified-of-Logic/seekdb-ans/blob/f91d5654a13b00e36c4703f415315d9b80ca47d7/src/share/datum/ob_datum_cmp_func_def.h), with explicit collation/empty/null-data guards. Original license notices are retained. That reference's team identity and current official full-score result have not been independently established.

Validation scope: affected binary/UCA production factory objects are compiled separately, with source hashes recorded by the team. The independent standalone driver initially failed before main in the dynamic loader; it is retained as a failed verification-environment attempt and is not claimed as a passing comparator test. Source-level review of the unchanged NULL wrappers and charset fallback is separate from execution.

The five official workload/generator/setup/scoring/baseline files are unchanged. This revision requires official compilation and the original complete benchmark to establish runtime acceptance and any score change; no new speedup is claimed here.
