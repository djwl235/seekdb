"""Local V5 SQL measurement on a dedicated, already imported database.

Does not claim official execution or GPU proof. Requires externally started
isolated baseline/candidate instances, fresh tables and external GPU profiler.
Python tool dependency: PyMySQL (not a seekdb build dependency).
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import struct
import time

DDL = ('CREATE VECTOR INDEX idx_items ON items(v) WITH '
       '(distance=l2,type=hnsw,lib=cuvs,m=16,ef_construction=200,ef_search=64)')


def sha(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def validate_ids(rows, k, legal):
    if any(not row or type(row[0]) is not int for row in rows):
        raise ValueError('result_integrity: SQL BIGINT IDs must be actual integers')
    ids = [row[0] for row in rows]
    if len(ids) != k or len(set(ids)) != k or not set(ids) <= legal:
        raise ValueError('result_integrity: expected k distinct legal IDs')
    return ids


def recall(answer, truth, k):
    return len(set(answer) & set(truth[:k])) / k


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--dataset', choices=['sift', 'hidden'], required=True)
    ap.add_argument('--variant', choices=['baseline', 'candidate'], required=True)
    ap.add_argument('--ordinal', type=int, choices=range(1, 5), required=True)
    ap.add_argument('--database', required=True)
    ap.add_argument('--port', type=int, required=True)
    ap.add_argument('--queries-f32', type=Path, required=True)
    ap.add_argument('--base-tsv', type=Path, required=True)
    ap.add_argument('--dataset-role', choices=['public-sift','diagnostic-256','official-hidden'], required=True)
    ap.add_argument('--truth-json', type=Path, required=True)
    ap.add_argument('--ids-json', type=Path, required=True)
    ap.add_argument('--source-commit', required=True)
    ap.add_argument('--seekdb-binary', type=Path, required=True)
    ap.add_argument('--bridge', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    a = ap.parse_args()
    if not re.fullmatch(r'amd_v5_[a-z0-9_]+', a.database):
        ap.error('dedicated database must have prefix amd_v5_ and safe characters')
    if not re.fullmatch(r'[a-f0-9]{40}', a.source_commit):
        ap.error('source-commit must be a full actual commit')
    if (a.dataset == 'sift') != (a.dataset_role == 'public-sift'):
        ap.error('dataset role and dimension class do not match')
    dim = 128 if a.dataset == 'sift' else 256
    data = a.queries_f32.read_bytes()
    if len(data) != 4096 * dim * 4:
        ap.error('exactly 4096 little-endian FP32 queries required')
    legal_list = json.loads(a.ids_json.read_text())
    legal = set(legal_list)
    truth = json.loads(a.truth_json.read_text())
    if len(legal_list) != 250000 or len(legal) != 250000 or not all(type(x) is int for x in legal):
        ap.error('250000 distinct integer source IDs required')
    if len(truth) != 4096 or any(len(x) < 100 or any(type(v) is not int for v in x[:100]) or len(set(x[:100])) != 100 or not set(x[:100]) <= legal for x in truth):
        ap.error('truth must contain 4096 rows with at least 100 distinct legal exact-neighbor IDs')
    vectors = list(struct.iter_unpack('<'+'f'*dim, data))
    if any(not math.isfinite(x) for row in vectors for x in row):
        ap.error('queries must contain finite FP32 values')
    queries = ["'["+','.join(format(x, '.9g') for x in row)+"]'" for row in vectors]
    a.out.mkdir(parents=True, exist_ok=False)
    report = dict(dataset=a.dataset, variant=a.variant, ordinal=a.ordinal,
                  rows=250000, dim=dim, nq=4096, source_commit=a.source_commit,
                  dataset_role=a.dataset_role,
                  input_hashes={x: sha(getattr(a, x)) for x in ['base_tsv','queries_f32', 'truth_json', 'ids_json']},
                  binary_sha256=sha(a.seekdb_binary), bridge_sha256=sha(a.bridge),
                  ddl=DDL, state='RUNNING', gpu_execution='UNKNOWN',
                  vram_gate='UNKNOWN', resource_gate='UNKNOWN', official_score='UNKNOWN')
    transcript = (a.out/'queries.jsonl').open('x', encoding='utf-8', buffering=1)
    conn = None
    try:
        import pymysql
        conn = pymysql.connect(host='127.0.0.1',port=a.port,user=os.environ.get('AMD_DB_USER','root'),
                               password=os.environ.get('AMD_DB_PASSWORD',''),database=a.database,
                               autocommit=True,read_timeout=300,write_timeout=300)
        cur = conn.cursor()
        cur.execute('SELECT VERSION()'); report['version'] = cur.fetchone()[0]
        cur.execute('SELECT COUNT(*) FROM items'); assert cur.fetchone()[0] == 250000
        cur.execute('SELECT id FROM items'); assert {int(x[0]) for x in cur.fetchall()} == legal
        cur.execute('SHOW CREATE TABLE items'); report['prebuild_schema'] = str(cur.fetchone())
        cur.execute('SHOW INDEX FROM items'); before = cur.fetchall()
        if any(str(row[2]) != 'PRIMARY' for row in before):
            raise ValueError('table must start without any secondary/vector index')
        cur.execute('SET ob_query_timeout=300000000')
        def ann(q, k):
            cur.execute(f'SELECT id FROM items ORDER BY l2_distance(v,{q}) APPROXIMATE LIMIT {k}')
            return validate_ids(cur.fetchall(), k, legal)
        start=time.perf_counter()
        cur.execute(DDL)
        ddl_done=time.perf_counter()
        first=ann(queries[0], 10)
        end=time.perf_counter()
        report.update(ddl_seconds=ddl_done-start, first_query_seconds=end-ddl_done,
                      build_to_first_query_seconds=end-start, first_ids=first, query={})
        for k in [10,100]:
            latencies=[]; records=[]; began=time.perf_counter()
            for i,q in enumerate(queries):
                t=time.perf_counter(); answer=ann(q,k); latency=time.perf_counter()-t
                latencies.append(latency); records.append((i,answer,latency))
            elapsed=time.perf_counter()-began
            hits=sum(recall(answer,truth[i],k) for i,answer,_ in records)
            for i,answer,latency in records:
                transcript.write(json.dumps(dict(k=k,query=i,ids=answer,seconds=latency))+'\n')
            report['query'][str(k)] = dict(qps=4096/elapsed,seconds=elapsed,
                summed_sql_seconds=sum(latencies), evidence_io_in_timing=False,
                p95_seconds=sorted(latencies)[math.ceil(.95*4096)-1],recall=hits/4096)
        report['state']='SQL_MEASURED_GPU_AND_RESOURCE_GATES_PENDING'
    except BaseException as exc:
        report['state']='FAILED'; report['error']=str(exc); raise
    finally:
        if conn: conn.close()
        transcript.close()
        (a.out/'report.json').write_text(json.dumps(report,indent=2)+'\n')


if __name__ == '__main__':
    main()
