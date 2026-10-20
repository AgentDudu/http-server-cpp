#!/usr/bin/env bash
# Local Linux gate: builds the container image and runs the full sanitizer
# suite inside it (the same commands the ci linux job runs).
# Run from git-bash. The build happens in a container copy, not your tree.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) docker_bin=docker.exe;; *) docker_bin=docker;; esac
command -v "$docker_bin" >/dev/null 2>&1 || { echo "docker not found" >&2; exit 127; }
"$docker_bin" build -q -t http-cpp-asan -f tools/Dockerfile.asan tools >/dev/null
# git-bash mangles POSIX-looking docker args; give docker a Windows source path
# and disable conversion for the container-side /src.
if [ -n "${MSYSTEM:-}" ]; then src="$(pwd -W)"; else src="$PWD"; fi
MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*' "$docker_bin" run --rm -v "$src:/src" -w /src http-cpp-asan bash tools/asan-inside.sh
