#!/usr/bin/env bash
# ASan+UBSan gate for the platform-neutral code (http layer) on Linux.
# Docker Desktop must be running. Native MinGW lacks sanitizer runtimes.
set -euo pipefail
cd "$(dirname "$0")/.."
docker build -q -t http-cpp-asan -f tools/Dockerfile.asan tools >/dev/null
# MSYS guards: stop git-bash from rewriting the container-side /src paths.
MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*' docker run --rm -v "$PWD:/src" -w /src http-cpp-asan bash -c '
  set -euo pipefail
  cmake -S . -B /tmp/asan -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug -DHTTP_SERVER_ENABLE_SANITIZERS=ON
  cmake --build /tmp/asan -j
  ASAN_OPTIONS=detect_leaks=1 /tmp/asan/unit_tests | tail -3
'
