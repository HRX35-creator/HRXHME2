#!/usr/bin/env python3
"""Compare multi-batch fulltext unions for integer and composite primary keys."""
import argparse
import json
import os
from pathlib import Path
import subprocess

CLIENT = os.environ.get('MYSQL_CLIENT', str(Path(__file__).resolve().parents[2] /
                        'deps/3rd/u01/obclient/bin/obclient'))
PORT = os.environ.get('SEEKDB_TEST_PORT', '10105')
DB = 'fts_buffered_union_regression'


def execute(query, database=True):
    command = [CLIENT, '-h127.0.0.1', '-P' + PORT, '-uroot', '-N', '-s', '--raw',
               '--default-character-set=utf8mb4']
    prefix = 'SET NAMES utf8mb4; ' + ('USE ' + DB + '; ' if database else '')
    result = subprocess.run(command, input=prefix + query, capture_output=True, text=True, timeout=90)
    if result.returncode:
        raise AssertionError({'sql': query, 'error': result.stderr})
    return result.stdout.splitlines()


def snapshot():
    execute('CREATE DATABASE IF NOT EXISTS ' + DB + ';', database=False)
    execute('''DROP TABLE IF EXISTS binary_docs;
CREATE TABLE binary_docs(tenant VARCHAR(20), id INT, body TEXT,
  PRIMARY KEY(tenant,id), FULLTEXT INDEX ft(body) WITH PARSER beng);''')
    vocabulary = ['alpha','bravo','charlie','delta','echo','foxtrot','golf','hotel','india']
    for start in range(0, 1537, 250):
        rows = []
        for n in range(start, min(start + 250, 1537)):
            body = ' '.join(word for i, word in enumerate(vocabulary) if n % (i + 2) == 0)
            rows.append("('tenant%02d',%d,'%s')" % (n % 11, n, body))
        execute('INSERT INTO binary_docs VALUES ' + ','.join(rows) + ';')
    results = []
    texts = ['alpha', 'alpha bravo', ' '.join(vocabulary[:8]), ' '.join(vocabulary),
             'alpha alpha bravo absentword', 'absentword missingtoken']
    for words in texts:
        match = "MATCH(body) AGAINST('" + words + "')"
        queries = [
            f'SELECT COUNT(*),SUM(id) FROM binary_docs WHERE {match}',
            f'SELECT tenant,id FROM binary_docs WHERE {match} ORDER BY tenant,id',
            f'SELECT tenant,id,body FROM binary_docs WHERE {match} AND id%29=0 ORDER BY tenant,id',
            f'SELECT tenant,id FROM binary_docs WHERE {match} ORDER BY tenant,id LIMIT 41 OFFSET 255',
            f'SELECT tenant,id,ROUND({match},8) FROM binary_docs WHERE {match} ORDER BY tenant,id LIMIT 19',
            f'SELECT COUNT(*) FROM (SELECT tenant,id FROM binary_docs WHERE {match} LIMIT 513 OFFSET 7) q',
        ]
        for query in queries:
            results.append([query, execute(query + ';')])
    query = "PREPARE s FROM 'SELECT COUNT(*),SUM(id) FROM binary_docs WHERE MATCH(body) AGAINST(?)';\n"
    for words in ['alpha bravo', ' '.join(vocabulary[:8]), 'absentword', 'alpha', ' '.join(vocabulary)]:
        query += "SET @w='" + words + "'; EXECUTE s USING @w;\n"
    query += 'DEALLOCATE PREPARE s;'
    results.append([query, execute(query)])
    query = '''BEGIN;
UPDATE binary_docs SET body='absentword' WHERE id%37=0;
DELETE FROM binary_docs WHERE id%41=0;
SELECT COUNT(*),SUM(id) FROM binary_docs WHERE MATCH(body) AGAINST('alpha bravo');
ROLLBACK;
SELECT COUNT(*),SUM(id) FROM binary_docs WHERE MATCH(body) AGAINST('alpha bravo');'''
    results.append([query, execute(query)])
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=['record','verify'])
    parser.add_argument('path', type=Path)
    parser.add_argument('--check-plan', action='store_true')
    args = parser.parse_args()
    results = snapshot()
    if args.mode == 'record':
        args.path.write_text(json.dumps(results, ensure_ascii=False, indent=2))
    else:
        expected = json.loads(args.path.read_text())
        assert len(expected) == len(results)
        for before, after in zip(expected, results):
            if before != after:
                raise AssertionError({'baseline':before, 'candidate':after})
    print(f'{args.mode}: {len(results)} composite-key union result groups checked')
    if args.check_plan:
        plan = '\n'.join(execute("EXPLAIN SELECT COUNT(*) FROM binary_docs WHERE MATCH(body) AGAINST('alpha bravo');"))
        assert 'calc_relevance=false' in plan, plan
        print('unscored MATCH plan checked')


if __name__ == '__main__':
    main()
