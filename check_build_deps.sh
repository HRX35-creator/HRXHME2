#!/bin/bash
set -e

echo "===== Check SeekDB build dependencies ====="
REQUIRED_CMDS=("bison" "flex" "gcc" "g++" "cmake" "make")
MISSING=()

for cmd in "${REQUIRED_CMDS[@]}"; do
  if ! command -v $cmd &> /dev/null; then
    MISSING+=("$cmd")
  fi
done

if [ ${#MISSING[@]} -ne 0 ]; then
  echo "❌ Missing build dependencies: ${MISSING[*]}"
  echo "Please install them first before compiling SeekDB"
  exit 1
fi

echo "✅ All required build dependencies are ready!"
exit 0
