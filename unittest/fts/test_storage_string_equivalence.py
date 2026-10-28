#!/usr/bin/env python3
"""Compare persisted string scans, indexes and mutations against an original build."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import time

CLIENT = os.environ.get('MYSQL_CLIENT', str(Path(__file__).resolve().parents[2] /
                        'deps/3rd/u01/obclient/bin/obclient'))
PORT = os.environ.get('SEEKDB_TEST_PORT', '10105')
DB = 'fts_storage_string_regression'


def execute(query, database=True):
    command = [CLIENT, '-h127.0.0.1', '-P' + PORT, '-uroot', '-N', '-s', '--raw',
               '--default-character-set=utf8mb4']
    prefix = 'SET NAMES utf8mb4; ' + ('USE ' + DB + '; ' if database else '')
    result = subprocess.run(command, input=prefix + query, capture_output=True, text=True, timeout=120)
    if result.returncode:
        raise AssertionError({'sql': query, 'error': result.stderr})
    return result.stdout.splitlines()


def literal(value):
    return 'NULL' if value is None else "CONVERT(UNHEX('" + value.encode().hex() + "') USING utf8mb4)"


def snapshot():
    execute('CREATE DATABASE IF NOT EXISTS ' + DB + ';', database=False)
    execute('''DROP TABLE IF EXISTS strings;
CREATE TABLE strings(id INT PRIMARY KEY, short_text VARCHAR(100) COLLATE utf8mb4_bin,
  optional_text VARCHAR(100) COLLATE utf8mb4_general_ci, long_text VARCHAR(500),
  INDEX idx_short(short_text));''')
    for start in range(0, 2401, 200):
        rows = []
        for n in range(start, min(start + 200, 2401)):
            short = 'fixed-prefix-%04d-suffix-%02d' % (n // 29, n % 29)
            if n % 5 == 0:
                short += ' 中文'
            optional = None if n % 7 == 0 else ('group%03d-' % (n % 113) + ('x ' * (n % 9)))
            long = ('common-' * (n % 35)) + ('尾部%05d' % n) + ('z' * (n % 17))
            rows.append('(%d,%s,%s,%s)' % (n, literal(short), literal(optional), literal(long)))
        execute('INSERT INTO strings VALUES ' + ','.join(rows) + ';')
    # Persist rows so scan paths exercise encoded storage instead of only memtables.
    execute('ALTER SYSTEM MINOR FREEZE;', database=False)
    for _ in range(80):
        if execute("SELECT COUNT(*) FROM oceanbase.__all_virtual_tablet_memstore_info WHERE is_active='NO';", database=False) == ['0']:
            break
        time.sleep(0.25)
    else:
        raise AssertionError('minor freeze did not finish within 20 seconds')
    queries = [
        'SELECT id,HEX(short_text),IFNULL(HEX(optional_text),\'NULL\'),HEX(long_text) FROM strings ORDER BY id',
        'SELECT COUNT(*),COUNT(optional_text),SUM(LENGTH(long_text)) FROM strings',
        'SELECT short_text,COUNT(*) FROM strings GROUP BY short_text ORDER BY short_text LIMIT 70',
        "SELECT id,HEX(short_text) FROM strings FORCE INDEX(idx_short) WHERE short_text >= 'fixed-prefix-0020' ORDER BY short_text,id LIMIT 150",
        'SELECT a.id,b.id,HEX(a.optional_text) FROM strings a JOIN strings b ON a.id+1=b.id WHERE a.id%97=0 ORDER BY a.id',
        'SELECT id,HEX(SUBSTR(long_text,3,30)) FROM strings WHERE id%53=0 ORDER BY id',
    ]
    results = [[query, execute(query + ';')] for query in queries]
    query = '''BEGIN;
UPDATE strings SET short_text=CONCAT(short_text,'-updated') WHERE id%73=0;
DELETE FROM strings WHERE id%89=0;
SELECT COUNT(*),SUM(LENGTH(short_text)),COUNT(optional_text) FROM strings;
ROLLBACK;
SELECT COUNT(*),SUM(LENGTH(short_text)),COUNT(optional_text) FROM strings;'''
    results.append([query, execute(query)])
    return results


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('mode', choices=['record','verify'])
    p.add_argument('path', type=Path)
    args = p.parse_args()
    results = snapshot()
    if args.mode == 'record':
        args.path.write_text(json.dumps(results, ensure_ascii=False, indent=2))
    else:
        expected = json.loads(args.path.read_text())
        assert len(expected) == len(results)
        for before, after in zip(expected, results):
            if before != after:
                raise AssertionError({'baseline':before, 'candidate':after})
    print(f'{args.mode}: {len(results)} persisted string result groups checked')


if __name__ == '__main__':
    main()
