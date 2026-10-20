# Building & verifying

C++20, CMake ≥ 3.25, no third-party runtime dependencies. Tests use
[doctest](https://github.com/doctest/doctest) as a vendored single header
under `external/doctest` — fully offline builds.

## Linux

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
./build/unit_tests
cmake --preset gcc-asan && cmake --build --preset gcc-asan   # ASan+UBSan+LSan
```

GCC 12+/Clang 15+. `_GNU_SOURCE` is set by CMake (glibc hides `accept4`
under `-std=c++20`).

## Windows

- **MinGW-w64** (primary CI gate): `cmake --preset mingw-x64-debug` with
  `mingw64/bin` on `PATH` — presets `mingw-x64-release`, `mingw-x64-asan`
  too (the ASan preset is skipped automatically if the toolchain has no
  sanitizer runtimes; `tools/ck.ps1` warns instead).
- **MSVC**: `cmake --preset msvc-x64-debug` (from a Developer prompt, or
  use CMake Tools). W4 + `/WX` in CI.

## Verification gates

| Gate                     | What                                                                        |
| ------------------------ | --------------------------------------------------------------------------- |
| `tools/ck.ps1`           | Windows: configure+build (warnings-as-errors), clang-format `--dry-run -Werror` on `src/ tests/`, full suite |
| `tools/ck-asan-docker.sh`| Ubuntu 24.04 container: full suite under ASan+UBSan+LSan + SIGINT graceful-stop smoke; `SAN=thread` runs TSan |
| `tools/soak-inside.sh`   | 60 s churn soak, RSS drift assertion (`HTTP_SERVER_SOAK_SECONDS` also gates the in-suite test) |
| `tools/bench-inside.sh`  | Release build throughput numbers (needs `wrk`, `apache2-utils`)             |
| GitHub Actions           | `windows-native`, `linux-asan`, `linux-tsan`, `linux-soak` on every push/PR |

Notes:

- Docker Desktop on Windows: from Git Bash prefix container runs with
  `MSYS_NO_PATHCONV=1` or `/src` mounts get rewritten.
- Local TSan in a Docker Desktop kernel may die with `unexpected memory
  mapping` (ASLR vs TSan runtime): run with `--privileged` and
  `sysctl -w kernel.randomize_va_space=0` inside. CI runners are unaffected.
- clang-format: `tools/ck.ps1` gates `src/` and `tests/` with
  `clang-format --dry-run -Werror` (verified against LLVM 23). Different
  major versions can disagree on details — reformat with
  `clang-format -i $(git ls-files 'src/**.h' 'src/**.cpp' 'tests/**.h' 'tests/**.cpp')`
  using your local binary before pushing.
- The soak test skips unless `HTTP_SERVER_SOAK_SECONDS=60` is set, keeping
  the default suite fast; the CI `linux-soak` job sets it.
