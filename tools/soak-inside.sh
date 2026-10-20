#!/usr/bin/env bash
# 60 s churn soak with RSS sampling (Linux, plain build — sanitizers change
# the memory picture). Run inside the tools/Dockerfile.asan container:
#   docker run --rm -v "$PWD:/src" -w /src http-cpp-asan bash tools/soak-inside.sh
set -euo pipefail
: "${HTTP_SERVER_SOAK_SECONDS:=60}"
cmake -S . -B build-soak -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-soak
HTTP_SERVER_SOAK_SECONDS="$HTTP_SERVER_SOAK_SECONDS" ctest --test-dir build-soak --output-on-failure
