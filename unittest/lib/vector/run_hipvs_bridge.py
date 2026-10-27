"""Run the real AMD C ABI bridge and preserve its output for native import tests.

This measures the bridge only. It is not the SQL construction benchmark. Use
rocprofv3 --kernel-trace --hip-trace -- python run_hipvs_bridge.py ... to retain
kernel execution proof separately from uninstrumented timings.
"""
import argparse
import ctypes as c
import hashlib
import json
from pathlib import Path
import time

import numpy as np


class Request(c.Structure):
    _fields_ = [('abi_version', c.c_uint32), ('struct_size', c.c_uint32),
                ('rows', c.c_uint64), ('dimension', c.c_uint32),
                ('max_degree', c.c_uint32), ('ef_construction', c.c_uint32),
                ('cpu_threads', c.c_uint32), ('vectors', c.POINTER(c.c_float))]


class Result(c.Structure):
    _fields_ = [('abi_version', c.c_uint32), ('struct_size', c.c_uint32),
                ('hnsw_data', c.POINTER(c.c_uint8)), ('hnsw_bytes', c.c_uint64),
                ('cagra_build_ms', c.c_double), ('hierarchy_serialize_ms', c.c_double),
                ('device_id', c.c_int32), ('nn_descent_iterations', c.c_uint32),
                ('device_name', c.c_char * 128), ('gcn_arch', c.c_char * 64),
                ('error', c.c_char * 512)]


def sha256(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--library', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    for name, default in [('rows', 5000), ('dimension', 16), ('m', 8),
                          ('ef-construction', 80), ('cpu-threads', 8),
                          ('seed', 20261027), ('repeats', 1)]:
        parser.add_argument('--' + name, type=int, default=default)
    args = parser.parse_args()
    if args.rows < 1024 or args.dimension < 1 or args.rows * args.dimension > 100000000:
        raise ValueError('invalid or excessive test dataset shape')
    if args.repeats < 1:
        raise ValueError('repeats must be positive')
    library = args.library.resolve(strict=True)
    args.output.mkdir(parents=True, exist_ok=False)
    vectors = np.random.default_rng(args.seed).uniform(-1, 1,
        (args.rows, args.dimension)).astype(np.float32)
    vector_path = args.output / 'vectors.f32'
    vectors.tofile(vector_path)
    lib = c.CDLL(str(library))
    lib.seekdb_hipvs_build_hnsw_v1.argtypes = [c.POINTER(Request), c.POINTER(Result)]
    lib.seekdb_hipvs_build_hnsw_v1.restype = c.c_int
    lib.seekdb_hipvs_free_v1.argtypes = [c.c_void_p]
    lib.seekdb_hipvs_free_v1.restype = None
    report = dict(scope='bridge-only; not SQL end-to-end construction',
                  library_sha256=sha256(library), vectors_sha256=sha256(vector_path),
                  numpy=np.__version__, rows=args.rows, dimension=args.dimension,
                  m=args.m, ef_construction=args.ef_construction, cpu_threads=args.cpu_threads,
                  seed=args.seed, runs=[], success=False)
    for repetition in range(args.repeats):
        request = Request(1, c.sizeof(Request), args.rows, args.dimension,
                          args.m, args.ef_construction, args.cpu_threads,
                          vectors.ctypes.data_as(c.POINTER(c.c_float)))
        result = Result()
        result.abi_version = 1
        result.struct_size = c.sizeof(Result)
        started = time.perf_counter()
        status = lib.seekdb_hipvs_build_hnsw_v1(c.byref(request), c.byref(result))
        entry = dict(repetition=repetition + 1, status=status,
                     total_ms=(time.perf_counter() - started) * 1000,
                     device=bytes(result.device_name).decode(), architecture=bytes(result.gcn_arch).decode(),
                     cagra_build_ms=result.cagra_build_ms, hierarchy_serialize_ms=result.hierarchy_serialize_ms,
                     bytes=result.hnsw_bytes, nn_descent_iterations=result.nn_descent_iterations,
                     error=bytes(result.error).decode())
        report['runs'].append(entry)
        try:
            if status == 0:
                if result.abi_version != 1 or result.struct_size != c.sizeof(Result):
                    raise RuntimeError('bridge result ABI changed')
                if not result.hnsw_data or not 96 <= result.hnsw_bytes <= 100000000:
                    raise RuntimeError('invalid bridge output size')
                target = args.output / f'index-{repetition + 1}.bin'
                target.write_bytes(c.string_at(result.hnsw_data, result.hnsw_bytes))
                entry['blob_sha256'] = sha256(target)
        finally:
            if result.hnsw_data:
                lib.seekdb_hipvs_free_v1(result.hnsw_data)
            (args.output / 'manifest.json').write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps(entry), flush=True)
        if status:
            return False
    report['success'] = True
    (args.output / 'manifest.json').write_text(json.dumps(report, indent=2) + '\n')
    return True


if __name__ == '__main__':
    raise SystemExit(0 if main() else 1)
