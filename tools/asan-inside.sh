#!/usr/bin/env bash
# Runs inside the sanitizer container: full-tree sanitizer build + ctest.
# SAN selects the sanitizer set: default address,undefined; thread also valid.
# Used by tools/ck-asan-docker.sh locally and by the ci linux jobs.
set -euo pipefail
SAN="${SAN:-address,undefined}"
if [ "$SAN" = "thread" ]; then
    OPT=HTTP_SERVER_ENABLE_TSAN
else
    OPT=HTTP_SERVER_ENABLE_SANITIZERS
fi
cmake -S . -B /tmp/asan -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -D${OPT}=ON
cmake --build /tmp/asan -j
export ASAN_OPTIONS=detect_leaks=1:detect_stack_use_after_return=1
export TSAN_OPTIONS=halt_on_error=1

# Graceful-shutdown smoke: SIGINT must drain, exit 0 (leak check rides on exit).
echo "== SIGINT graceful stop smoke =="
/tmp/asan/http_server 18099 &
srv=$!
sleep 0.5
line=$(printf 'GET / HTTP/1.1\r\n\r\n' | timeout 5 nc 127.0.0.1 18099 | head -1)
echo "$line" | grep -q "200" || { echo "smoke: expected 200, got: $line"; kill -9 $srv; exit 1; }
kill -INT $srv
for _ in $(seq 40); do kill -0 $srv 2>/dev/null || break; sleep 0.1; done
if kill -0 $srv 2>/dev/null; then
    echo "smoke: server still running 4s after SIGINT"
    kill -9 $srv
    exit 1
fi
wait $srv
echo "smoke ok"

ctest --test-dir /tmp/asan --output-on-failure
