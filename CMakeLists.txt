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
      "MinGW-w64 builds here ship without them; run platform-neutral unit tests "
      "under ASan in a Linux container instead (see PLAN.md).")
  endif()
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

add_library(http STATIC
  src/http/status.cpp
  src/http/headers.cpp
  src/http/response.cpp)
target_include_directories(http PUBLIC src)
target_compile_options(http PRIVATE
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror)

# The net layer and the server binary are Windows-only by design.
if(WIN32)
  add_library(net STATIC src/net/socket.cpp src/net/wsa.cpp)
  target_include_directories(net PUBLIC src)
  target_link_libraries(net PUBLIC ws2_32)
  target_compile_options(net PRIVATE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror)

  add_executable(http_server src/main.cpp)
  target_link_libraries(http_server PRIVATE net)
  target_compile_options(http_server PRIVATE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror)
endif()

enable_testing()

set(unit_srcs
  tests/test_main.cpp
  tests/test_response.cpp)
if(WIN32)
  list(APPEND unit_srcs tests/test_socket.cpp)
endif()

add_executable(unit_tests ${unit_srcs})
target_include_directories(unit_tests SYSTEM PRIVATE external/doctest)
# doctest is a vendored header: warnings from it are not ours to fix,
# hence SYSTEM above; our own test code still builds warning-clean.
target_compile_options(unit_tests PRIVATE -Wall -Wextra -Werror)
if(WIN32)
  target_link_libraries(unit_tests PRIVATE net http)
else()
  target_link_libraries(unit_tests PRIVATE http)
endif()

add_test(NAME unit_tests COMMAND unit_tests)
