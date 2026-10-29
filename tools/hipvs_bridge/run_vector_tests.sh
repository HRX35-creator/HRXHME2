#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
TEST_BIN=${TEST_BIN:-$ROOT/build_release/unittest/oblib/oblib_tests}
HIPVS_ROOT=${HIPVS_ROOT:-/opt/hipvs}
ROCM_ROOT=${ROCM_ROOT:-/opt/rocm}
BRIDGE_ROOT=${BRIDGE_ROOT:-/seekdb_workspace/bridge}
SYSTEM_STDCXX=${SYSTEM_STDCXX:-/usr/lib/x86_64-linux-gnu/libstdc++.so.6}
COMPAT_DIR="$ROOT/build_release/hipvs-test-runtime"
mkdir -p "$COMPAT_DIR"
if [[ ! -e "$COMPAT_DIR/libaio.so.1" && -e /usr/lib/x86_64-linux-gnu/libaio.so.1t64 ]]; then
  ln -s /usr/lib/x86_64-linux-gnu/libaio.so.1t64 "$COMPAT_DIR/libaio.so.1"
fi
[[ -x "$TEST_BIN" ]] || { echo "Build oblib_tests first" >&2; exit 2; }
[[ -f "$SYSTEM_STDCXX" ]] || { echo "Set SYSTEM_STDCXX to a ROCm-compatible libstdc++.so.6" >&2; exit 2; }
FILTER='TestVsagAdaptor.*:HipvsBulkBuilder.*'
EXTRA=()
if [[ $# -gt 0 ]]; then
  [[ $# -eq 3 && "$1" == --gpu ]] || { echo "Usage: $0 [--gpu /absolute/worker /dataset/directory]" >&2; exit 2; }
  export SEEKDB_HIPVS_TEST_WORKER="$2" SEEKDB_HIPVS_TEST_DATASET="$3"
  FILTER="$FILTER:HipvsBulkImport.DISABLED_gpu_native_roundtrip"
  EXTRA+=(--gtest_also_run_disabled_tests)
fi
exec env LD_PRELOAD="$SYSTEM_STDCXX" \
  LD_LIBRARY_PATH="$COMPAT_DIR:$BRIDGE_ROOT:$HIPVS_ROOT/lib:$ROCM_ROOT/lib" \
  OMP_NUM_THREADS=8 "$TEST_BIN" "--gtest_filter=$FILTER" "${EXTRA[@]}"
