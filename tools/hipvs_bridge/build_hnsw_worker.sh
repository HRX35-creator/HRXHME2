#!/usr/bin/env bash
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
HIPVS_ROOT=${HIPVS_ROOT:-/opt/hipvs}
ROCM_ROOT=${ROCM_ROOT:-/opt/rocm}
OUTPUT=${1:-$HERE/seekdb-hipvs-hnsw-build}
mkdir -p "$(dirname "$OUTPUT")"
"$ROCM_ROOT/bin/hipcc" -x hip -std=c++17 -O2 \
  -I"$HIPVS_ROOT/include" -I"$HIPVS_ROOT/include/cuvs" \
  "$HERE/hnsw_build_worker.cpp" -L"$HIPVS_ROOT/lib" -lcuvs_c \
  -Wl,-rpath,"$HIPVS_ROOT/lib:$ROCM_ROOT/lib" -o "$OUTPUT"
echo "Built $OUTPUT"
