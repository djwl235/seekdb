"""Package selected immutable local evidence; never executes a workload."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import zipfile


def digest(data):
    return hashlib.sha256(data).hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--source', type=Path, required=True, help='round-g192n40 root')
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    src, out = args.source.resolve(), args.output.resolve()
    pair = src / 'amd-evidence/paired-g192n40-recovery006-001'
    native = src / 'amd-evidence/native-g192n40-recovery006-001/p0-evidence'
    original_manifest = json.loads((pair / 'evidence-manifest.json').read_text(encoding='utf-8'))
    cases = ['1-sift-baseline', '2-sift-candidate', '3-hidden-candidate', '4-hidden-baseline']
    mappings = []
    for name in ['artifact-combination.json', 'paired-comparison.json', 'resources-sift.json', 'resources-diagnostic256.json']:
        mappings.append((pair / name, 'paired/' + name))
    for case in cases:
        for name in ['lifecycle.json', 'loaded-maps-after-ddl.txt', 'measurement/report.json']:
            mappings.append((pair / case / name, 'paired/' + case + '/' + name))
    # executed-sql.jsonl contains vector literals, which this submission deliberately omits.
    for path in sorted(native.iterdir()):
        if path.is_file() and path.name != 'executed-sql.jsonl':
            mappings.append((path, 'native/' + path.name))
    raw = []
    for case in cases:
        for name in ['resources.jsonl', 'measurement/queries.jsonl']:
            raw.append((pair / case / name, case + '/' + name))
    patterns = {
        'private-key': rb'-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----',
        'github-token': rb'(?:gh[pousr]_[A-Za-z0-9]{25,}|github_pat_[A-Za-z0-9_]{35,})',
        'aws-access-key': rb'AKIA[0-9A-Z]{16}',
        'nonempty-json-credential': rb'"(?:password|access_token|refresh_token|api_key|private_key)"\s*:\s*"[^"\s][^"]*"',
        'credential-url': rb'https?://[^\s/]+:[^\s/@]+@',
    }
    entries = []
    for path, target in mappings + raw:
        data = path.read_bytes()
        rel = path.relative_to(src).as_posix()
        old = original_manifest[rel]
        assert old['bytes'] == len(data) and old['sha256'] == digest(data), rel
        assert data[:4] != b'\x7fELF', rel
        for label, pattern in patterns.items():
            assert not re.search(pattern, data, re.I), (label, rel)
        if path.name == 'queries.jsonl':
            rows = [json.loads(line) for line in data.splitlines()]
            assert len(rows) == 8192, rel
            assert all(set(row) == {'k', 'query', 'ids', 'seconds'} for row in rows), rel
            assert all(isinstance(row['query'], int) for row in rows), rel
        entries.append({'source': rel, 'path': target, 'bytes': len(data), 'sha256': digest(data),
                        'storage': 'raw-queries-and-timing.zip' if (path, target) in raw else 'file'})
    out.mkdir(parents=True, exist_ok=True)
    for path, target in mappings:
        dest = out / target
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, dest)
        assert dest.read_bytes() == path.read_bytes()
    zip_manifest = {'scope': 'All four original query ID/answer/latency logs and 20ms resource logs; no vectors or binaries.',
                    'files': [entry for entry in entries if entry['storage'] != 'file']}
    zip_path = out / 'raw-queries-and-timing.zip'
    with zipfile.ZipFile(zip_path, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        for path, target in raw:
            info = zipfile.ZipInfo(target, date_time=(2026, 11, 2, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            zf.writestr(info, path.read_bytes())
        info = zipfile.ZipInfo('manifest.json', date_time=(2026, 11, 2, 0, 0, 0))
        info.compress_type = zipfile.ZIP_DEFLATED
        zf.writestr(info, json.dumps(zip_manifest, indent=2).encode('utf-8') + b'\n')
    with zipfile.ZipFile(zip_path) as zf:
        assert zf.testzip() is None
        assert len(zf.namelist()) == len(raw) + 1
        for entry in zip_manifest['files']:
            data = zf.read(entry['path'])
            assert len(data) == entry['bytes'] and digest(data) == entry['sha256']
        assert json.loads(zf.read('manifest.json')) == zip_manifest
    manifest = {'candidate_product_commit': '24b972ab8500e29b4e750f8bee7a6b5c5f5193a3',
                'scope': 'Selected evidence only; not the original 62-file asset inventory.',
                'source_manifest_sha256': digest((pair / 'evidence-manifest.json').read_bytes()),
                'files': entries,
                'archive': {'path': zip_path.name, 'bytes': zip_path.stat().st_size, 'sha256': digest(zip_path.read_bytes())}}
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    summary = []
    for case in cases:
        report = json.loads((pair / case / 'measurement/report.json').read_text(encoding='utf-8'))
        summary.append({'case': case, 'role': report['dataset_role'],
                        'build_to_first_query_seconds': report['build_to_first_query_seconds'],
                        'qps10': report['query']['10']['qps'], 'recall10': report['query']['10']['recall'],
                        'qps100': report['query']['100']['qps'], 'recall100': report['query']['100']['recall']})
    qa = {'classification': 'PACKAGING_QA_NOT_NEW_BENCHMARK', 'original_files_verified': len(entries),
          'plain_original_files': len(mappings), 'archive_original_files': len(raw),
          'query_rows_verified': 4 * 8192, 'zip_crc_and_sha256_readback': 'PASS',
          'selected_files_match_original_manifest': 'PASS', 'credential_pattern_scan': 'PASS',
          'binaries_vectors_auth_configuration_included': False,
          'original_unknown_pending_fields': 'PRESERVED_BYTE_FOR_BYTE', 'summary': summary}
    (out / 'packaging-qa.json').write_text(json.dumps(qa, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'archive': manifest['archive'], 'qa': qa}, indent=2))


if __name__ == '__main__':
    main()
