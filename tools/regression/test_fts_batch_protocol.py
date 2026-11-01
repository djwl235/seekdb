#!/usr/bin/env python3
"""TEST_ONLY: execute verbatim production FTS batch methods with bounded fake interfaces.

This validates method-level batch protocol, not a linked seekdb server, actual
tokenization, LOB decoding, SQL planning, or performance. Uses Clang and LLD.
"""
import argparse
import ctypes
import hashlib
import json
from pathlib import Path
import subprocess

METHODS = ['inner_get_next_fts_index_batch', 'fetch_next_fts_index_batch_doc',
           'fill_fts_passthrough_cols', 'reset_fts_output_exprs',
           'init_fts_output_exprs', 'fill_generated_fts_cols',
           'get_output_fts_col_expr_by_type', 'inner_rescan', 'inner_close']
CASES = ['aux/cap1', 'aux/cap2', 'aux/cap256', 'docword/cap1',
         'docword/cap2', 'docword/cap256', 'scalar-generated-fallback',
         'scalar-passthrough', 'source-end-next-call', 'rescan-mid-document',
         'failed-close-drops-continuation', 'status-error', 'segment-error',
         'posting-error-after-partial-output', 'conversion-error', 'LOB-error',
         'source-eval-error', 'passthrough-eval-error',
         'partial-init-retry', 'invalid-capacity']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--compiler', required=True)
    parser.add_argument('--linker', required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    receipt = args.out / 'protocol.json'
    assert not receipt.exists(), 'preserve earlier failures'
    source = args.repo / 'src/sql/engine/table/ob_table_scan_op.cpp'
    text = source.read_text()
    blocks = []
    records = []
    for method in METHODS:
        prefix = ('void' if method == 'reset_fts_output_exprs' else 'int') + ' ObTableScanOp::' + method + '('
        start = text.index(prefix)
        end = text.index('\n}\n', start) + 3
        block = text[start:end]
        blocks.append(block)
        records.append(dict(method=method, source_line=text[:start].count('\n') + 1,
                            sha256=hashlib.sha256(block.encode()).hexdigest()))
    template = Path(__file__).with_name('fts_batch_protocol_fixture.hpp').read_text()
    assert template.count('/* PRODUCTION_METHODS */') == 1
    generated = args.out / 'protocol.cpp'
    generated.write_text(template.replace('/* PRODUCTION_METHODS */', '\n\n'.join(blocks)))
    library = args.out / 'protocol.so'
    command = [args.compiler, '-std=c++17', '-O1', '-fPIC', '-shared', '-nostdlib',
               '-fno-exceptions', '-fno-rtti', '-fno-stack-protector',
               '-fuse-ld=' + args.linker, str(generated), '-o', str(library)]
    state = dict(scope='TEST_ONLY/verbatim method protocol, bounded fake interfaces; not engine SQL or performance',
                 source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                 fixture_sha256=hashlib.sha256(template.encode()).hexdigest(),
                 extracted_methods=records, command=command, cases=[], status='running')

    def save():
        receipt.write_text(json.dumps(state, indent=2) + '\n')

    save()
    with (args.out / 'protocol-build.log').open('x') as log:
        process = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=90)
    state['compile_exit_code'] = process.returncode
    if process.returncode:
        state['status'] = 'failed'; save(); raise SystemExit(process.returncode)
    lib = ctypes.CDLL(str(library))
    lib.run_case.argtypes = [ctypes.c_int]
    lib.run_case.restype = ctypes.c_int
    for index, name in enumerate(CASES):
        result = lib.run_case(index)
        state['cases'].append(dict(name=name, passed=result == 0, fixture_failure_line=result))
        save()
    state['status'] = 'passed' if all(case['passed'] for case in state['cases']) else 'failed'
    save()
    print(json.dumps(dict(status=state['status'], cases=state['cases'], scope=state['scope'])))
    raise SystemExit(0 if state['status'] == 'passed' else 1)


if __name__ == '__main__':
    main()
