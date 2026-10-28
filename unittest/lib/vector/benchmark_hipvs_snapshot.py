"""Real AMD snapshot build diagnostic; retain all attempts, no formal pass threshold."""
import argparse
import ctypes
import datetime
import hashlib
import json
import os
from pathlib import Path
import socket
import statistics
import subprocess
import threading
import time
import traceback

import numpy as np

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parent)
parser.add_argument('--rows', type=int, default=10000)
parser.add_argument('--queries', type=int, default=100)
parser.add_argument('--repeats', type=int, default=3)
parser.add_argument('--port', type=int, default=2907)
args = parser.parse_args()
args.root = args.root.resolve()
assert args.rows > 64 and args.queries > 0 and args.repeats > 0
stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%d_%H%M%S')
out = args.root / 'evidence' / ('gpu-' + stamp)
out.mkdir(parents=True, exist_ok=False)
report = {'scope': 'real GPU candidate diagnostic; not formal competition acceptance', 'runs': [], 'complete': False,
          'rows': args.rows, 'dim': 128, 'queries': args.queries, 'repeats': args.repeats,
          'm': 16, 'ef_construction': 200, 'ef_search': 64,
          'timing': 'CREATE VECTOR INDEX through first valid ANN query; all conversion and synchronization included',
          'warmup': 'explicit direct GPU smoke before SQL runs; same server process, alternating library order'}
env = dict(os.environ)
runtime_paths = [args.root / 'runtime', Path('/opt/hipvs/lib'), Path('/opt/hipvs/lib64'), Path('/opt/rocm/lib')]
base_env = dict(env, LD_LIBRARY_PATH=':'.join(map(str, runtime_paths)) + ':' + env.get('LD_LIBRARY_PATH', ''))
gpu_env = dict(base_env, LD_LIBRARY_PATH=str(args.root / 'build_hipvs') + ':' + base_env['LD_LIBRARY_PATH'])
server = None

def save():
    (out / 'result.json').write_text(json.dumps(report, ensure_ascii=False, indent=2))

def command(argv, timeout=60, **kwargs):
    p = subprocess.run(argv, capture_output=True, text=True, timeout=timeout, **kwargs)
    if p.returncode:
        raise RuntimeError(f'{argv[0]} exit {p.returncode}: {p.stderr[-4000:]}')
    return p.stdout.strip()

def sql(query, timeout=300):
    return command(['mysql', '-h127.0.0.1', '-P' + str(args.port), '-uroot', '-N', '-B'],
                   input=query, timeout=timeout, env=base_env)

def start(environment):
    global server
    with socket.socket() as probe:
        if probe.connect_ex(('127.0.0.1', args.port)) == 0:
            raise RuntimeError('Refuse to share an occupied port')
    runtime = out / 'database'
    runtime.mkdir(exist_ok=True)
    with (out / 'server-console.log').open('ab') as logfile:
        server = subprocess.Popen([str(args.root / 'bin/seekdb'), '--nodaemon', '--base-dir', str(runtime),
                                   '--port', str(args.port), '--parameter', 'memory_limit=8G',
                                   '--parameter', 'cpu_count=8', '--log-level', 'INFO'],
                                  cwd=runtime, env=environment, stdin=subprocess.DEVNULL,
                                  stdout=logfile, stderr=logfile)
    deadline = time.monotonic() + 150
    while time.monotonic() < deadline:
        if server.poll() is not None:
            raise RuntimeError('Candidate exited; inspect server-console.log')
        try:
            sql('SELECT 1;', timeout=3)
            return
        except (RuntimeError, subprocess.TimeoutExpired):
            time.sleep(1)
    raise RuntimeError('Candidate startup timeout')

def stop():
    global server
    if server and server.poll() is None:
        server.terminate()
        server.wait(timeout=120)
    server = None

def events():
    result = []
    for path in sorted((out / 'database/log').glob('seekdb.log*')):
        if path.is_file():
            result.extend(line.rstrip() for line in path.open(errors='replace') if 'hipVS ' in line)
    return result

def vector_text(row):
    return '[' + ','.join(format(float(v), '.9g') for v in row) + ']'

try:
    report['manifest'] = json.loads((args.root / 'manifest.json').read_text())
    plugin = args.root / 'build_hipvs/libseekdb_hipvs_build.so'
    report['plugin_sha256'] = hashlib.sha256(plugin.read_bytes()).hexdigest()
    report['plugin_ldd'] = command(['ldd', str(plugin)], env=gpu_env)
    report['gpu_inventory'] = command(['/opt/rocm/bin/rocminfo'], env=gpu_env)
    report['cpu_quota'] = Path('/sys/fs/cgroup/cpu.max').read_text() if Path('/sys/fs/cgroup/cpu.max').exists() else None
    report['memory_limit'] = Path('/sys/fs/cgroup/memory.max').read_text() if Path('/sys/fs/cgroup/memory.max').exists() else None
    # Use the template's fixed vectors when their known shape matches.
    dataset = Path('/seekdb_workspace/datasets/base.f32')
    queryfile = Path('/seekdb_workspace/datasets/query.f32')
    if (dataset.exists() and queryfile.exists()
            and dataset.stat().st_size >= args.rows * 128 * 4
            and queryfile.stat().st_size >= args.queries * 128 * 4):
        base = np.fromfile(dataset, dtype='<f4').reshape(-1, 128)[:args.rows].copy()
        queries = np.fromfile(queryfile, dtype='<f4').reshape(-1, 128)[:args.queries].copy()
        assert len(base) == args.rows and len(queries) == args.queries
        report['dataset_origin'] = 'template fixed dataset, leading rows as requested'
    else:
        rng = np.random.default_rng(20261028)
        base = rng.standard_normal((args.rows, 128), dtype=np.float32)
        queries = rng.standard_normal((args.queries, 128), dtype=np.float32)
        report['dataset_origin'] = 'generated Gaussian seed 20261028; exploratory, not official dataset'
    report['data_sha256'] = hashlib.sha256(base.tobytes()).hexdigest()
    report['query_sha256'] = hashlib.sha256(queries.tobytes()).hexdigest()
    base.tofile(out / 'base.f32')
    queries.tofile(out / 'query.f32')
    # Run public C-ABI directly on a 32 MiB worker stack before touching SQL.
    def probe_gpu():
        result = {}
        try:
            lib = ctypes.CDLL(str(plugin))
            build = lib.seekdb_hipvs_build_v1
            build.argtypes = [ctypes.POINTER(ctypes.c_float), ctypes.c_int64, ctypes.c_int32,
                              ctypes.c_int32, ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_size_t),
                              ctypes.c_char_p, ctypes.c_size_t]
            build.restype = ctypes.c_int
            lib.seekdb_hipvs_free_v1.argtypes = [ctypes.c_void_p]
            output, size, error = ctypes.c_void_p(), ctypes.c_size_t(), ctypes.create_string_buffer(2048)
            small = np.ascontiguousarray(base[:min(2000, len(base))])
            began = time.perf_counter()
            status = build(small.ctypes.data_as(ctypes.POINTER(ctypes.c_float)), len(small), 128, 16,
                           ctypes.byref(output), ctypes.byref(size), error, len(error))
            result.update(status=status, error=error.value.decode(errors='replace'), seconds=time.perf_counter()-began,
                          rows=len(small), bytes=size.value)
            try:
                assert status == 0 and output.value and size.value > 96, result
                assert size.value < 16_000_000, result
                raw = ctypes.string_at(output, size.value)
                (out / 'smoke-export.bin').write_bytes(raw)
                result['export_sha256'] = hashlib.sha256(raw).hexdigest()
            finally:
                if output.value:
                    lib.seekdb_hipvs_free_v1(output)
        except BaseException:
            result['exception'] = traceback.format_exc()
        report['direct_gpu_smoke'] = result
    threading.stack_size(32 * 1024 * 1024)
    worker = threading.Thread(target=probe_gpu)
    worker.start()
    worker.join()
    save()
    assert 'exception' not in report['direct_gpu_smoke'], report['direct_gpu_smoke']
    truths = [set(np.argsort(np.sum((base.astype(np.float64)-q.astype(np.float64))**2, axis=1), kind='stable')[:10].tolist()) for q in queries]
    serialized = list(map(vector_text, base))
    probes = list(map(vector_text, queries))
    start(gpu_env)
    report['version'] = sql('SELECT VERSION();')
    sql('CREATE DATABASE amd_candidate;')
    for repeat in range(args.repeats):
        for library in (('vsag', 'hipvs') if repeat % 2 == 0 else ('hipvs', 'vsag')):
            table = f'amd_candidate.t_{library}_{repeat}'
            row = {'library': library, 'repeat': repeat, 'table': table}
            report['runs'].append(row)
            save()
            sql(f'CREATE TABLE {table}(id BIGINT PRIMARY KEY, v VECTOR(128));')
            for first in range(0, len(base), 250):
                values = ','.join(f"({i},'{serialized[i]}')" for i in range(first, min(first+250, len(base))))
                sql(f'INSERT INTO {table} VALUES {values};')
            assert int(sql(f'SELECT COUNT(*) FROM {table};')) == len(base)
            def query(index):
                return f"SELECT id FROM {table} ORDER BY l2_distance(v,'{probes[index]}') APPROXIMATE LIMIT 10;"
            before_events = set(events())
            began = time.perf_counter()
            sql(f'CREATE VECTOR INDEX idx ON {table}(v) WITH (distance=l2,type=hnsw,lib={library},m=16,ef_construction=200,ef_search=64,sync_mode=immediate);')
            row['ddl_seconds'] = time.perf_counter() - began
            first_ids = [int(x) for x in sql(query(0)).splitlines()]
            row['through_first_query_seconds'] = time.perf_counter() - began
            assert len(first_ids) == len(set(first_ids)) == 10 and all(0 <= i < len(base) for i in first_ids)
            row['plan'] = sql('EXPLAIN ' + query(0))
            assert 'VECTOR INDEX SCAN' in row['plan'], row['plan']
            row['queries'] = []
            for index in range(len(queries)):
                ids = first_ids if index == 0 else [int(x) for x in sql(query(index)).splitlines()]
                assert len(ids) == len(set(ids)) == 10 and all(0 <= i < len(base) for i in ids)
                row['queries'].append({'ids': ids, 'truth': sorted(truths[index]), 'recall': len(set(ids) & truths[index])/10})
            row['recall_at_10'] = statistics.mean(q['recall'] for q in row['queries'])
            row['build_events'] = [event for event in events() if event not in before_events]
            if library == 'hipvs':
                assert any('hipVS GPU graph imported' in event for event in row['build_events']), 'GPU graph import event missing'
            save()
    stop()
    # Restart without the optional builder directory: persisted indexes must work.
    start(base_env)
    report['restart_checks'] = []
    for row in report['runs']:
        if row['library'] == 'hipvs':
            table = row['table']
            ids = [int(x) for x in sql(f"SELECT id FROM {table} ORDER BY l2_distance(v,'{probes[0]}') APPROXIMATE LIMIT 10;").splitlines()]
            assert ids == row['queries'][0]['ids'], (table, ids)
            report['restart_checks'].append(table)
    report['medians_seconds'] = {lib: statistics.median(r['through_first_query_seconds'] for r in report['runs'] if r['library'] == lib) for lib in ['vsag', 'hipvs']}
    report['complete'] = True
except BaseException:
    report['error'] = traceback.format_exc()
    raise
finally:
    try:
        stop()
    finally:
        report['all_build_events'] = events()
        save()
print(json.dumps({'report': str(out / 'result.json'), 'complete': report['complete']}))
