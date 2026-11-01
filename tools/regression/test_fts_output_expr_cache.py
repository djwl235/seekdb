#!/usr/bin/env python3
"""Small SQL regression for a linked candidate server, not a performance test.

Use an explicit isolated database, e.g.:
  SEEKDB_PASSWORD=... python3 tools/regression/test_fts_output_expr_cache.py \
    --port 12884 --database fts_expr_cache_v3 --candidate-sha <built-sha>
The SHA is an operator-supplied build identity, not independently attested here.
Requires PyMySQL. No password or connection URL is printed.
"""
import argparse
import json
import os
import re

import pymysql


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, required=True)
    parser.add_argument('--user', default='root')
    parser.add_argument('--database', required=True)
    parser.add_argument('--candidate-sha', required=True)
    args = parser.parse_args()
    assert re.fullmatch(r'fts_expr_cache_[A-Za-z0-9_]+', args.database), 'use an isolated task database'
    assert re.fullmatch(r'[0-9a-f]{40}', args.candidate_sha), 'record the actual linked build'
    connection = pymysql.connect(host=args.host, port=args.port, user=args.user,
        password=os.environ.get('SEEKDB_PASSWORD', ''), charset='utf8mb4',
        autocommit=True, connect_timeout=10, read_timeout=90)
    checks = []
    try:
        with connection.cursor() as cursor:
            cursor.execute('SELECT VERSION()')
            version = cursor.fetchone()[0]
            cursor.execute(f'CREATE DATABASE IF NOT EXISTS `{args.database}`')
            cursor.execute(f'USE `{args.database}`')
            for suffix, tokenizer, columns in (
                    ('ik_one', 'ik', 'content'),
                    ('ik_many', 'ik', 'title,content'),
                    ('beng_many', 'beng', 'title,content')):
                table = 'cache_' + suffix
                # Refuse to overwrite an earlier run or any existing fixture.
                cursor.execute(f'CREATE TABLE `{table}` (id BIGINT PRIMARY KEY, title VARCHAR(128), content LONGTEXT)')
                rows = [(1, None, None), (2, '', ''), (3, 'the and', 'the and'),
                        (4, 'alpha', 'alphacache betacache alphacache'),
                        (5, '', 'alphacache ' * 1300), (6, '中文', 'gammaprobe 中文')]
                cursor.executemany(f'INSERT INTO `{table}` VALUES (%s,%s,%s)', rows)
                cursor.execute(f'ALTER TABLE `{table}` ADD FULLTEXT INDEX fti ({columns}) WITH PARSER {tokenizer}')

                def expect(label, expected):
                    cursor.execute(f"SELECT id FROM `{table}` WHERE MATCH({columns}) AGAINST('alphacache') ORDER BY id")
                    actual = [row[0] for row in cursor.fetchall()]
                    assert actual == expected, (suffix, label, actual, expected)
                    checks.append(suffix + '/' + label)

                expect('initial', [4, 5])
                expect('repeat', [4, 5])
                cursor.execute(f"UPDATE `{table}` SET content='betacache' WHERE id=4")
                expect('updated', [5])
                cursor.execute(f'DELETE FROM `{table}` WHERE id=5')
                expect('deleted', [])
                cursor.execute(f'ALTER TABLE `{table}` DROP INDEX fti')
                cursor.execute(f'ALTER TABLE `{table}` ADD FULLTEXT INDEX fti ({columns}) WITH PARSER {tokenizer}')
                expect('rebuilt', [])
        print(json.dumps(dict(status='passed', checks=checks, server_version=version,
            operator_supplied_candidate_sha=args.candidate_sha,
            scope='SQL correctness only; no throughput, restart or binary-hash attestation'), ensure_ascii=False))
    finally:
        connection.close()


if __name__ == '__main__':
    main()
