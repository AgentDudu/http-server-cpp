#!/usr/bin/env bash
# Throughput numbers for the README — indicative only (container CPU).
# Scenarios x {1 reactor, N reactors}. Release build.
set -euo pipefail
cd "$(dirname "$0")/.."

command -v wrk >/dev/null 2>&1 || {
    echo "need wrk (apt-get install -y wrk)" >&2
    exit 1
}
command -v ab >/dev/null 2>&1 || {
    echo "need ab (apt-get install -y apache2-utils)" >&2
    exit 1
}

cmake -S . -B /tmp/bench -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build /tmp/bench -j >/dev/null

mkdir -p /tmp/web
printf 'bench ok\n' >"/tmp/web/hi.txt"
head -c 1024 /dev/zero | tr '\0' 'A' >/tmp/web/1k.txt
head -c 65536 /dev/zero | tr '\0' 'B' >/tmp/web/64k.txt

PORT=18080
run_server() { # threads
    /tmp/bench/http_server "$PORT" /tmp/web "$1" >/tmp/server.log 2>&1 &
    echo $!
}
wait_ready() {
    for _ in $(seq 1 50); do
        # no curl in the image: a TCP connect is enough
        (exec 3<>/dev/tcp/127.0.0.1/"$PORT") 2>/dev/null && return 0
        sleep 0.1
    done
    echo "server did not come up" >&2
    exit 1
}

bench() { # threads label path  -> wrk keep-alive
    local T=$1 label=$2 path=$3
    local pid
    pid=$(run_server "$T")
    wait_ready
    echo "### threads=$T $label wrk keep-alive"
    wrk -t4 -c100 -d15s --latency "http://127.0.0.1:$PORT$path" |
        grep -E "Requests/sec|Latency|99%|Transfer/sec"
    kill "$pid"
    wait "$pid" 2>/dev/null || true
}

fresh() { # threads label path -> ab without keep-alive
    local T=$1 label=$2 path=$3
    local pid
    pid=$(run_server "$T")
    wait_ready
    echo "### threads=$T $label fresh-connections (ab -n 20000 -c 50)"
    ab -n 20000 -c 50 "http://127.0.0.1:$PORT$path" |
        grep -E "Requests per second|Failed requests|P99"
    kill "$pid"
    wait "$pid" 2>/dev/null || true
}

for T in 1 4; do
    bench "$T" "1KiB file" /1k.txt
    bench "$T" "64KiB file" /64k.txt
    fresh "$T" "1KiB file" /1k.txt
done
echo "cores: $(nproc)"
