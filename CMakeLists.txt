cmake_minimum_required(VERSION 3.25)
project(http_server_cpp VERSION 0.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(HTTP_SERVER_ENABLE_SANITIZERS "Build with address and undefined sanitizers" OFF)

include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "-fsanitize=address,undefined")
check_cxx_source_compiles("int main() { return 0; }" SANITIZERS_AVAILABLE)
set(CMAKE_REQUIRED_FLAGS "")

add_compile_definitions(_WIN32_WINNT=0x0A00)

if(HTTP_SERVER_ENABLE_SANITIZERS)
  if(NOT SANITIZERS_AVAILABLE)
    message(FATAL_ERROR
      "Sanitizer runtimes (libasan/libubsan) are not installed in this toolchain. "
      "MinGW-w64 builds here ship without them; run the whole tree under ASan in "
      "the Linux container (tools/ck-asan-docker.sh) instead.")
  endif()
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

set(HTTP_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror)

# http layer is platform-neutral.
add_library(http STATIC
  src/http/status.cpp
  src/http/headers.cpp
  src/http/response.cpp)
target_include_directories(http PUBLIC src)
target_compile_options(http PRIVATE ${HTTP_WARNINGS})

# net: portable API in src/net/, one implementation directory per OS.
if(WIN32)
  set(net_platform_srcs
    src/net/raw_win.cpp
    src/net/error_win.cpp
    src/net/context_win.cpp)
else()
  set(net_platform_srcs
    src/net/raw_posix.cpp
    src/net/error_posix.cpp
    src/net/context_posix.cpp)
endif()

add_library(net STATIC
  src/net/socket_common.cpp
  ${net_platform_srcs})
target_include_directories(net PUBLIC src)
if(WIN32)
  target_link_libraries(net PUBLIC ws2_32)
endif()
target_compile_options(net PRIVATE ${HTTP_WARNINGS})

add_executable(http_server src/main.cpp)
target_link_libraries(http_server PRIVATE net http)
target_compile_options(http_server PRIVATE ${HTTP_WARNINGS})

enable_testing()

add_executable(unit_tests
  tests/test_main.cpp
  tests/test_socket.cpp
  tests/test_response.cpp)
target_include_directories(unit_tests SYSTEM PRIVATE external/doctest)
# doctest is a vendored header: warnings from it are not ours to fix,
# hence SYSTEM above; our own test code still builds warning-clean.
target_compile_options(unit_tests PRIVATE -Wall -Wextra -Werror)
target_link_libraries(unit_tests PRIVATE net http)

add_test(NAME unit_tests COMMAND unit_tests)
