#!/usr/bin/env bash
# Runs inside the sanitizer container: full-tree ASan/UBSan build + ctest.
# Used by tools/ck-asan-docker.sh locally and by the ci linux job.
set -euo pipefail
cmake -S . -B /tmp/asan -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DHTTP_SERVER_ENABLE_SANITIZERS=ON
cmake --build /tmp/asan -j
export ASAN_OPTIONS=detect_leaks=1:detect_stack_use_after_return=1
ctest --test-dir /tmp/asan --output-on-failure
