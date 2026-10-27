"""Actual AMD SQL persistence after an intentional process crash; no graceful-shutdown claim."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

import numpy as np
import pymysql


def file_sha256(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


parser = argparse.ArgumentParser(description=__doc__)
for name in ('binary', 'bridge', 'vectors', 'base-dir', 'output'):
    parser.add_argument('--' + name, type=Path, required=True)
parser.add_argument('--source-revision', required=True)
args = parser.parse_args()
binary = args.binary.resolve(strict=True)
bridge = args.bridge.resolve(strict=True)
base = args.base_dir.resolve()
out = args.output.resolve()
assert not base.exists() and not out.exists()
base.mkdir(parents=True)
out.parent.mkdir(parents=True, exist_ok=True)
root = out.parent
raw_vectors = np.fromfile(args.vectors, dtype=np.float32)
if raw_vectors.size != 5000 * 16:
    raise ValueError('expected the bridge fixture with 5000 float32 vectors of dimension16')
vectors = raw_vectors.reshape(5000, 16)[:2048]
ids = [i + (1 << 54) if i % 2 == 0 else -i - 1 for i in range(2048)]
new_id = -(1 << 62)
deleted_id = ids[7]
report = dict(success=False, scope='required GPU SQL build, committed DML, intentional SIGKILL, CPU-only restart',
              source=args.source_revision, rows=2048, dimension=16,
              binary_sha256=file_sha256(binary),
              bridge_sha256=file_sha256(bridge),
              vectors_sha256=hashlib.sha256(vectors.tobytes()).hexdigest())
process = connection = None


def start(mode):
    environment = os.environ.copy()
    environment['SEEKDB_HIPVS_BUILD'] = mode
    environment.pop('SEEKDB_HIPVS_LIBRARY', None)
    if mode == 'required':
        environment['SEEKDB_HIPVS_LIBRARY'] = str(bridge)
    command = [str(binary), '--nodaemon', '--base-dir', str(base), '--log-level', 'INFO']
    for parameter in ['mysql_port_mode=disabled', 'memory_budget=1G', 'datafile_size=512M',
                      'datafile_maxsize=2G', 'log_disk_size=2G', 'net_thread_count=2']:
        command += ['--parameter', parameter]
    with (root / f'{out.stem}-{mode}-startup.log').open('w') as log:
        child = subprocess.Popen(command, env=environment, stdout=log, stderr=subprocess.STDOUT)
    try:
        deadline = time.monotonic() + 180
        while time.monotonic() < deadline:
            if child.poll() is not None:
                raise RuntimeError(f'{mode} server exited {child.returncode}')
            try:
                client = pymysql.connect(unix_socket=str(base / 'run/sql.sock'), user='root',
                    autocommit=True, connect_timeout=2, read_timeout=120)
                return child, client
            except pymysql.MySQLError:
                time.sleep(1)
        raise RuntimeError('server startup timed out')
    except BaseException:
        if child.poll() is None:
            child.kill()
        child.wait()
        raise


def query(cursor, vector, explain=False):
    cursor.execute(('EXPLAIN ' if explain else '') +
        'SELECT id FROM persisted ORDER BY l2_distance(embedding,%s) APPROXIMATE LIMIT 1',
        (json.dumps(np.asarray(vector).tolist()),))
    return cursor.fetchall()


try:
    process, connection = start('required')
    with connection.cursor() as cursor:
        cursor.execute('CREATE DATABASE hipvs_persist')
        cursor.execute('USE hipvs_persist')
        cursor.execute('SET ob_query_timeout=120000000')
        cursor.execute('SET ob_hnsw_ef_search=128')
        cursor.execute('CREATE TABLE persisted(id BIGINT PRIMARY KEY,embedding VECTOR(16)) ORGANIZATION HEAP')
        for start_at in range(0, len(ids), 256):
            cursor.executemany('INSERT INTO persisted VALUES(%s,%s)',
                [(ids[i], json.dumps(vectors[i].tolist())) for i in range(start_at, min(len(ids), start_at + 256))])
        cursor.execute('CREATE VECTOR INDEX idx_hipvs ON persisted(embedding) '
                       'WITH(distance=l2,type=hnsw,lib=vsag,m=8,ef_construction=80,sync_mode=immediate)')
        for i in range(0, 2048, 51):
            assert query(cursor, vectors[i]) == ((ids[i],),)
        cursor.execute('INSERT INTO persisted VALUES(%s,%s)', (new_id, json.dumps([3.0] * 16)))
        cursor.execute('UPDATE persisted SET embedding=%s WHERE id=%s', (json.dumps([-3.0] * 16), new_id))
        cursor.execute('DELETE FROM persisted WHERE id=%s', (deleted_id,))
        assert query(cursor, [-3.0] * 16) == ((new_id,),)
    log_path = base / 'log/seekdb.log'
    gpu_evidence = [line for line in log_path.read_text(errors='replace').splitlines() if 'HIPVS_BUILD_' in line]
    assert any('HIPVS_BUILD_COMPLETE' in line and 'rows=2048,' in line and 'cpu_fallback=false' in line for line in gpu_evidence)
    assert not any('HIPVS_BUILD_NOT_USED' in line for line in gpu_evidence)
    report['build_path_evidence'] = gpu_evidence
    connection.close()
    connection = None
    process.kill()  # Deliberate crash after acknowledged commits; preserve the actual data directory.
    report['intentional_crash_exit_code'] = process.wait()
    process = None
    restarted = time.perf_counter()
    process, connection = start('off')
    with connection.cursor() as cursor:
        cursor.execute('USE hipvs_persist')
        cursor.execute('SET ob_hnsw_ef_search=128')
        cursor.execute('SELECT id FROM persisted')
        expected = set(ids) - {deleted_id} | {new_id}
        assert set(row[0] for row in cursor.fetchall()) == expected
        plan = '\n'.join(str(row[0]) for row in query(cursor, vectors[0], explain=True))
        assert 'VECTOR INDEX' in plan
        report['recovered_query_plan'] = plan
        for i in range(0, 2048, 51):
            assert query(cursor, vectors[i]) == ((ids[i],),)
        assert query(cursor, [-3.0] * 16) == ((new_id,),)
        extra_id = new_id + 1
        cursor.execute('INSERT INTO persisted VALUES(%s,%s)', (extra_id, json.dumps([4.0] * 16)))
        assert query(cursor, [4.0] * 16) == ((extra_id,),)
        cursor.execute('DELETE FROM persisted WHERE id=%s', (extra_id,))
        cursor.execute('SELECT COUNT(*) FROM persisted')
        assert cursor.fetchone() == (2048,)
    assert 'libseekdb_hipvs_bridge' not in Path(f'/proc/{process.pid}/maps').read_text()
    report.update(success=True, recovered_rows=2048, bridge_loaded_during_recovered_queries=False,
                  recovery_and_verification_seconds=time.perf_counter() - restarted)
except Exception as error:
    report['error'] = str(error)
finally:
    if connection is not None:
        connection.close()
    if process is not None:
        if process.poll() is None:
            process.kill()  # Test cleanup, not evidence of graceful shutdown.
        report['cleanup_exit_code'] = process.wait()
    out.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report), flush=True)
raise SystemExit(0 if report['success'] else 1)
