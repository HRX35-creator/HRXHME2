"""Reproducible *construction* measurements, not an official passing score.

Requires NumPy and PyMySQL. Prepare once, copy the same data to the AMD node,
then run both modes on that node with identical arguments. Each run starts its
own native seekdb process. The output preserves failures and raw query IDs.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import signal
import statistics
import subprocess
import time

import numpy as np
import pymysql


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def save(path, value):
    Path(path).write_text(json.dumps(value, indent=2) + '\n')


def prepare(args):
    if args.rows < 1024 or args.dim < 1 or not 0 < args.k <= args.rows or args.queries < 1:
        raise ValueError('invalid dataset shape')
    target = Path(args.data)
    if target.exists():
        raise FileExistsError(target)
    rng = np.random.default_rng(args.seed)
    vectors = rng.standard_normal((args.rows, args.dim)).astype(np.float32)
    queries = rng.standard_normal((args.queries, args.dim)).astype(np.float32)
    ids = np.arange(args.rows, dtype=np.int64)
    ids = np.where(ids % 2, -ids - 1, ids + (1 << 54))
    truth = np.empty((args.queries, args.k), dtype=np.int64)
    # Float64 exact squared L2; no ANN index or library used for ground truth.
    base64 = vectors.astype(np.float64)
    for i, query in enumerate(queries):
        distances = np.sum((base64 - query.astype(np.float64)) ** 2, axis=1)
        truth[i] = ids[np.lexsort((ids, distances))[:args.k]]
    np.savez(target, vectors=vectors, queries=queries, ids=ids, truth=truth,
             seed=np.array(args.seed, dtype=np.int64))
    save(str(target) + '.json', dict(sha256=digest(target), rows=args.rows,
         dimension=args.dim, queries=args.queries, k=args.k, seed=args.seed,
         precision='float32', metric='squared_l2', numpy=np.__version__))


def stop(process):
    if process.poll() is None:
        process.send_signal(signal.SIGTERM)
        try:
            process.wait(timeout=30)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
    return process.returncode


def run(args):
    if args.repeats < 3:
        raise ValueError('at least three fresh builds are required')
    binary = Path(args.binary).resolve(strict=True)
    base = Path(args.base_dir).resolve()
    if base.exists() and any(base.iterdir()):
        raise ValueError('base-dir must be a new disposable instance')
    out = Path(args.output).resolve()
    if out.exists():
        raise FileExistsError(out)
    data_path = Path(args.data).resolve(strict=True)
    with np.load(data_path, allow_pickle=False) as data:
        vectors, queries, ids, truth = (data[key] for key in ('vectors', 'queries', 'ids', 'truth'))
        seed = int(data['seed'])
    n, dim = vectors.shape
    if vectors.dtype != np.float32 or queries.dtype != np.float32 or ids.dtype != np.int64:
        raise ValueError('unexpected dataset dtypes')
    if queries.shape[1] != dim or ids.shape != (n,) or truth.shape[0] != len(queries):
        raise ValueError('inconsistent dataset shape')
    if len(np.unique(ids)) != n or not np.isfinite(vectors).all() or not np.isfinite(queries).all():
        raise ValueError('invalid vectors or IDs')
    k = truth.shape[1]
    if not 0 < k <= n or not set(truth.ravel()).issubset(set(ids)):
        raise ValueError('invalid ground truth')
    if not 2 <= args.m <= 256 or 4 * args.m >= n or args.ef_construction < args.m or args.ef_search < k:
        raise ValueError('unsupported/comparatively invalid HNSW parameters')
    base.mkdir(parents=True, exist_ok=True)
    out.parent.mkdir(parents=True, exist_ok=True)
    environment = os.environ.copy()
    environment['SEEKDB_HIPVS_BUILD'] = args.mode
    environment.pop('SEEKDB_HIPVS_LIBRARY', None)
    bridge = None
    if args.mode == 'required':
        if not args.bridge:
            raise ValueError('--bridge is required in GPU mode')
        bridge = Path(args.bridge).resolve(strict=True)
        environment['SEEKDB_HIPVS_LIBRARY'] = str(bridge)
    command = [str(binary), '--nodaemon', '--base-dir', str(base), '--log-level', 'INFO']
    for value in ['mysql_port_mode=disabled', f'memory_budget={args.memory}',
                  'datafile_size=1G', 'datafile_maxsize=8G', 'log_disk_size=2G',
                  f'net_thread_count={args.net_threads}']:
        command += ['--parameter', value]
    report = dict(mode=args.mode, dataset_sha256=digest(data_path), binary_sha256=digest(binary),
                  bridge_sha256=digest(bridge) if bridge else None,
                  source_revision=args.source_revision, harness_sha256=digest(__file__),
                  numpy=np.__version__, python=platform.python_version(), platform=platform.platform(),
                  cpu_count=os.cpu_count(), cpu_affinity=sorted(os.sched_getaffinity(0))
                  if hasattr(os, 'sched_getaffinity') else None,
                  configuration=dict(rows=n, dimension=dim, queries=len(queries), k=k, seed=seed,
                                     m=args.m, ef_construction=args.ef_construction, ef_search=args.ef_search,
                                     precision='float32', metric='squared_l2', memory=args.memory,
                                     net_threads=args.net_threads), command=command, runs=[], success=False)
    process = None
    connection = None
    log_path = base / 'log/seekdb.log'
    try:
        startup_start = time.perf_counter()
        with out.with_suffix('.startup.log').open('w') as stream:
            process = subprocess.Popen(command, env=environment, stdout=stream, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 180
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise RuntimeError('seekdb exited during startup')
            try:
                connection = pymysql.connect(unix_socket=str(base / 'run/sql.sock'), user='root',
                                             autocommit=True, connect_timeout=2, read_timeout=3600)
                break
            except pymysql.MySQLError:
                time.sleep(1)
        else:
            raise RuntimeError('seekdb startup timed out')
        report['startup_seconds'] = time.perf_counter() - startup_start
        with connection.cursor() as cursor:
            cursor.execute('SELECT VERSION()')
            report['server_version'] = cursor.fetchone()[0]
            cursor.execute('CREATE DATABASE hipvs_benchmark')
            cursor.execute('USE hipvs_benchmark')
            cursor.execute('SET ob_query_timeout=3600000000')
            cursor.execute('SET ob_hnsw_ef_search=%s', (args.ef_search,))
            query_sql = ('SELECT id FROM hipvs_measure ORDER BY '
                         f'l2_distance(embedding,%s) APPROXIMATE LIMIT {k}')
            for repetition in range(args.repeats):
                entry = dict(repetition=repetition + 1, success=False)
                report['runs'].append(entry)
                cursor.execute(f'CREATE TABLE hipvs_measure(id BIGINT PRIMARY KEY,embedding VECTOR({dim})) '
                               'ORGANIZATION HEAP')
                import_start = time.perf_counter()
                for start in range(0, n, 256):
                    cursor.executemany('INSERT INTO hipvs_measure VALUES(%s,%s)',
                        [(int(ids[i]), json.dumps(vectors[i].tolist())) for i in range(start, min(n, start + 256))])
                entry['import_seconds'] = time.perf_counter() - import_start
                cursor.execute('SELECT COUNT(*),COUNT(DISTINCT id),COUNT(embedding) FROM hipvs_measure')
                if cursor.fetchone() != (n, n, n):
                    raise RuntimeError('raw import lost rows or IDs')
                cursor.execute("SHOW INDEX FROM hipvs_measure WHERE Key_name='idx_hipvs'")
                if cursor.fetchall():
                    raise RuntimeError('measurement must start with no vector index')
                log_info = log_path.stat()
                offset = log_info.st_size
                started = time.perf_counter()
                cursor.execute('CREATE VECTOR INDEX idx_hipvs ON hipvs_measure(embedding) '
                               f'WITH(distance=l2,type=hnsw,lib=vsag,m={args.m},'
                               f'ef_construction={args.ef_construction},sync_mode=immediate)')
                entry['create_index_seconds'] = time.perf_counter() - started
                first_query = json.dumps(queries[0].tolist())
                cursor.execute('EXPLAIN ' + query_sql, (first_query,))
                entry['query_plan'] = '\n'.join(str(row[0]) for row in cursor.fetchall())
                if 'VECTOR INDEX' not in entry['query_plan']:
                    raise RuntimeError('query does not use the vector index')
                cursor.execute(query_sql, (first_query,))
                first_result = cursor.fetchall()
                entry['query_ready_build_seconds'] = time.perf_counter() - started
                if len(first_result) != k:
                    raise RuntimeError('first indexed query returned an incomplete result')
                if log_path.stat().st_ino != log_info.st_ino or log_path.stat().st_size < offset:
                    raise RuntimeError('server log rotated during the build; preserve/reconcile logs before scoring')
                with log_path.open('rb') as log:
                    log.seek(offset)
                    lines = log.read().decode('utf-8', errors='replace').splitlines()
                evidence = [line for line in lines if 'HIPVS_BUILD_' in line]
                entry['build_path_evidence'] = evidence
                if args.mode == 'required':
                    if not any('HIPVS_BUILD_COMPLETE' in line and f'rows={n},' in line
                               and 'cpu_fallback=false' in line for line in evidence):
                        raise RuntimeError('no matching completed GPU build evidence')
                    if any('HIPVS_BUILD_NOT_USED' in line for line in evidence):
                        raise RuntimeError('GPU build failure/fallback occurred')
                elif evidence:
                    raise RuntimeError('CPU baseline unexpectedly requested hipVS')
                results = []
                hits = 0
                all_ids = set(int(value) for value in ids)
                for i, query in enumerate(queries):
                    cursor.execute(query_sql, (json.dumps(query.tolist()),))
                    neighbors = [int(row[0]) for row in cursor.fetchall()]
                    if len(set(neighbors)) != k or not set(neighbors).issubset(all_ids):
                        raise RuntimeError('invalid or duplicate document IDs')
                    results.append(neighbors)
                    hits += len(set(neighbors) & set(int(value) for value in truth[i]))
                entry.update(query_ids=results, recall_at_k=hits / (len(queries) * k), success=True)
                save(out, report)
                cursor.execute('DROP TABLE hipvs_measure')
        report['success'] = True
        report['median_query_ready_build_seconds'] = statistics.median(
            entry['query_ready_build_seconds'] for entry in report['runs'])
        report['minimum_recall_at_k'] = min(entry['recall_at_k'] for entry in report['runs'])
        report['official_threshold_verdict'] = 'not assessed; apply the published evaluation configuration'
    except Exception as error:
        report['error'] = str(error)
    finally:
        if connection is not None:
            connection.close()
        if process is not None:
            report['server_exit_code'] = stop(process)
        save(out, report)
    print(json.dumps({key: report[key] for key in ('mode', 'success', 'runs')}
                     if not report['success'] else dict(mode=args.mode, success=True,
                     median_seconds=report['median_query_ready_build_seconds'],
                     minimum_recall_at_k=report['minimum_recall_at_k'])))
    return report['success']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='operation', required=True)
    prepare_parser = commands.add_parser('prepare')
    prepare_parser.add_argument('--data', required=True)
    for name, default in [('rows', 10000), ('dim', 128), ('queries', 200), ('k', 10), ('seed', 20261027)]:
        prepare_parser.add_argument('--' + name, type=int, default=default)
    run_parser = commands.add_parser('run')
    for name in ('data', 'binary', 'base-dir', 'output', 'source-revision'):
        run_parser.add_argument('--' + name, required=True)
    run_parser.add_argument('--mode', choices=['off', 'required'], required=True)
    run_parser.add_argument('--bridge')
    run_parser.add_argument('--memory', default='4G')
    for name, default in [('m', 16), ('ef-construction', 128), ('ef-search', 128), ('repeats', 3), ('net-threads', 2)]:
        run_parser.add_argument('--' + name, type=int, default=default)
    args = parser.parse_args()
    if args.operation == 'prepare':
        prepare(args)
        return True
    return run(args)


if __name__ == '__main__':
    raise SystemExit(0 if main() else 1)
