"""Source-built SQL lifecycle check with an explicitly synthetic graph plugin.

This validates plumbing/persistence only; it is NOT GPU or performance evidence.
"""
import argparse
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import time
import socket

import pymysql

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--repo', type=Path, required=True)
parser.add_argument('--binary', type=Path, required=True)
parser.add_argument('--fixture-plugin-dir', type=Path, required=True)
parser.add_argument('--runtime', type=Path, required=True)
parser.add_argument('--report', type=Path, required=True)
parser.add_argument('--port', type=int, default=2907)
parser.add_argument('--synthetic-fixture', action='store_true', required=True)
args = parser.parse_args()
if args.runtime.exists():
    raise SystemExit('Refuse to reuse an existing test runtime')
report = {'mode': 'TEST_ONLY_SYNTHETIC_GRAPH_NOT_GPU', 'checks': [], 'passed': False}
original_env = dict(os.environ)
fixture_env = dict(original_env)
fixture_env['LD_LIBRARY_PATH'] = str(args.fixture_plugin_dir) + ':' + original_env.get('LD_LIBRARY_PATH', '')
connection = None
server = None

def start(env):
    global server, connection
    with socket.socket() as probe:
        if probe.connect_ex(('127.0.0.1', args.port)) == 0:
            raise RuntimeError('Test port already occupied')
    args.runtime.mkdir(parents=True, exist_ok=True)
    with (args.runtime / 'console.log').open('ab') as output:
        server = subprocess.Popen([str(args.binary), '--nodaemon', '--base-dir', str(args.runtime),
                                   '--port', str(args.port), '--parameter', 'memory_limit=3G',
                                   '--parameter', 'cpu_count=2', '--log-level', 'INFO'],
                                  cwd=args.runtime, env=env, stdin=subprocess.DEVNULL,
                                  stdout=output, stderr=output)
    deadline = time.monotonic() + 150
    while time.monotonic() < deadline:
        if server.poll() is not None:
            raise RuntimeError('Test server exited; inspect ' + str(args.runtime / 'console.log'))
        try:
            connection = pymysql.connect(host='127.0.0.1', port=args.port, user='root', autocommit=True,
                                         connect_timeout=2, read_timeout=120, write_timeout=60)
            q('SELECT 1')
            return
        except pymysql.MySQLError:
            if connection:
                connection.close()
                connection = None
            time.sleep(.5)
    raise RuntimeError('Test server readiness timeout')

def stop():
    global server, connection
    if connection:
        connection.close()
        connection = None
    if server and server.poll() is None:
        server.terminate()
        try:
            server.wait(timeout=120)
        except subprocess.TimeoutExpired:
            server.kill()
            server.wait(timeout=10)
            raise RuntimeError('Test server required forced termination')
    server = None

def q(sql, values=None):
    with connection.cursor() as cursor:
        cursor.execute(sql, values)
        return cursor.fetchall()

def f32(value):
    return struct.unpack('f', struct.pack('f', value))[0]

def build_events():
    events = []
    for path in sorted((args.runtime / 'log').glob('seekdb.log*')):
        if path.is_file():
            for line in path.open(errors='replace'):
                if 'hipVS ' in line:
                    events.append(line.rstrip())
    return events

data = {str((1 << 40) + row * 101): [f32(round(math.sin((row * 7 + d) * .3), 6)) for d in range(7)]
        for row in range(65)}

def verify(table, label):
    assert q(f'SELECT COUNT(*) FROM {table}')[0][0] == len(data)
    for vector in list(data.values())[::13]:
        value = json.dumps(vector)
        sql = f"SELECT id FROM {table} ORDER BY l2_distance(v, %s) APPROXIMATE LIMIT 10"
        expected = sorted(data, key=lambda key: sum((a-b)**2 for a,b in zip(vector, data[key])))[:10]
        result = [row[0] for row in q(sql, (value,))]
        assert result == expected, (label, expected, result)
    report['checks'].append(label)

try:
    start(fixture_env)
    report['version'] = q('SELECT VERSION()')
    q('CREATE DATABASE hipvs_lifecycle')
    q('USE hipvs_lifecycle')
    q('CREATE TABLE invalid_options(id INT PRIMARY KEY, v VECTOR(7))')
    for number, options in enumerate(['distance=cosine,type=hnsw', 'distance=l2,type=hnsw_sq']):
        try:
            q(f'CREATE VECTOR INDEX bad_{number} ON invalid_options(v) WITH (lib=hipvs,{options})')
        except pymysql.MySQLError as error:
            assert error.args[0] == 1235, error
        else:
            raise AssertionError('unsupported HIPVS options accepted: ' + options)
    report['checks'].append('unsupported_options_rejected')
    for table, library in [('cpu', 'vsag'), ('gpu_fixture', 'hipvs')]:
        q(f'CREATE TABLE {table}(id VARCHAR(32) PRIMARY KEY, v VECTOR(7))')
        with connection.cursor() as cursor:
            cursor.executemany(f'INSERT INTO {table} VALUES(%s,%s)', [(key, json.dumps(v)) for key,v in data.items()])
        q(f'CREATE VECTOR INDEX idx_v ON {table}(v) WITH (distance=l2,type=hnsw,lib={library},m=16,ef_construction=200,ef_search=100,sync_mode=immediate)')
        verify(table, library + '_post_import_create')
        report[table + '_plan'] = q(f"EXPLAIN SELECT id FROM {table} ORDER BY l2_distance(v, '[0,0,0,0,0,0,0]') APPROXIMATE LIMIT 10")
        assert 'VECTOR INDEX SCAN' in str(report[table + '_plan']), report[table + '_plan']
    deadline = time.monotonic() + 10
    while not any('hipVS GPU graph imported' in line for line in build_events()) and time.monotonic() < deadline:
        time.sleep(.1)
    assert any('hipVS GPU graph imported' in line for line in build_events()), 'snapshot import path not exercised'
    stop()
    # Fresh process, without the fixture plugin: persisted index must be usable
    # on a CPU-only host. No deferred GPU construction is possible here.
    start(original_env)
    q('USE hipvs_lifecycle')
    verify('gpu_fixture', 'restart_without_gpu_plugin')
    new_id = str((1 << 55) + 7)
    data[new_id] = [7.0] * 7
    q('INSERT INTO gpu_fixture VALUES(%s,%s)', (new_id, json.dumps(data[new_id])))
    verify('gpu_fixture', 'incremental_insert')
    old_id = next(iter(data))
    q('DELETE FROM gpu_fixture WHERE id=%s', (old_id,))
    del data[old_id]
    verify('gpu_fixture', 'delete_filter')
    stop()
    start(original_env)
    q('USE hipvs_lifecycle')
    verify('gpu_fixture', 'restart_after_mutations')
    report['passed'] = True
except Exception as error:
    report['error'] = repr(error)
    raise
finally:
    try:
        stop()
    finally:
        report['build_events'] = build_events()
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2, ensure_ascii=False, default=str))
print(json.dumps({'passed': report['passed'], 'checks': report['checks']}))
