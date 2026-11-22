#!/bin/bash
# Cross-process test of python/blah2_iq.py. Usage: test/shm/run.sh [rapidjson-include-dir]
set -euo pipefail
cd "$(dirname "$0")/../.."
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
extra=()
if [ $# -ge 1 ]; then extra=(-isystem "$1"); fi
g++ -std=c++20 -O2 -Wall -Wextra -Werror -Isrc "${extra[@]}" \
  test/shm/IqShmWriter.cpp src/data/IqCache.cpp src/data/IqData.cpp src/data/TuningLog.cpp \
  -lpthread -o "$out/IqShmWriter"
python3 python/test_blah2_iq.py "$out/IqShmWriter"
