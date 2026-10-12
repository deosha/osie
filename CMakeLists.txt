cmake_minimum_required(VERSION 3.24)
project(osie VERSION 0.1.0 LANGUAGES C CXX)

if(APPLE)
  enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(OSIE_BUILD_TESTS "Build unit/integration tests" ON)
option(OSIE_BUILD_BENCHMARKS "Build Google Benchmark harness" ON)
option(OSIE_WERROR "Treat warnings as errors" OFF)
set(OSIE_SANITIZE "" CACHE STRING "Sanitizers: '', 'address,undefined', or 'thread'")

set(OSIE_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion
                  -Wnon-virtual-dtor -Wold-style-cast -Wdouble-promotion)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  # GCC 13 reports -Wnull-dereference false positives inside libstdc++ <streambuf>.
  list(APPEND OSIE_WARNINGS -Wnull-dereference)
endif()
if(OSIE_WERROR)
  list(APPEND OSIE_WARNINGS -Werror)
endif()

if(OSIE_SANITIZE)
  add_compile_options(-fsanitize=${OSIE_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=all)
  add_link_options(-fsanitize=${OSIE_SANITIZE})
endif()

# ---------------------------------------------------------------- library ---
set(OSIE_CORE_SOURCES
  src/core/common.cpp
  src/core/json.cpp
  src/capabilities/capabilities.cpp
  src/state/state.cpp
  src/architecture/graph.cpp
  src/architecture/model.cpp
  src/reasoning/reasoner.cpp
  src/intent/frame.cpp
  src/intent/rule_parser.cpp
  src/inference/neural_parser.cpp
  src/engine/engine.cpp
)

if(APPLE)
  set(OSIE_PLATFORM_SOURCES
    src/platform/macos/system.cpp
    src/platform/macos/cpu_memory.cpp
    src/platform/macos/process.cpp
    src/platform/macos/storage_network.cpp
    src/platform/macos/native_bridge.mm)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(OSIE_PLATFORM_SOURCES
    src/platform/linux/procfs.cpp
    src/platform/linux/system.cpp
    src/platform/linux/cpu_memory.cpp
    src/platform/linux/process.cpp
    src/platform/linux/storage_network.cpp
    src/platform/linux/cgroups.cpp)
else()
  message(FATAL_ERROR "OSIE supports macOS and Linux only")
endif()

add_library(osie_core STATIC ${OSIE_CORE_SOURCES} ${OSIE_PLATFORM_SOURCES})
target_include_directories(osie_core PUBLIC include PRIVATE src)
target_compile_options(osie_core PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${OSIE_WARNINGS}>)
if(APPLE)
  target_link_libraries(osie_core PUBLIC "-framework Foundation")
  set_source_files_properties(src/platform/macos/native_bridge.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
endif()
find_package(Threads REQUIRED)
target_link_libraries(osie_core PUBLIC Threads::Threads)

add_executable(osie src/cli/main.cpp)
target_link_libraries(osie PRIVATE osie_core)
target_compile_options(osie PRIVATE ${OSIE_WARNINGS})
target_compile_definitions(osie PRIVATE OSIE_DEFAULT_MODEL_PATH="${CMAKE_SOURCE_DIR}/models/intent.osiem")

add_executable(osie_eval tools/osie_eval.cpp)
target_link_libraries(osie_eval PRIVATE osie_core)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(osie_cpu_lab tools/cpu_lab.cpp)  # isolated CPU-contention experiments
  target_link_libraries(osie_cpu_lab PRIVATE osie_core)
endif()

# Optional: ONNX Runtime comparison benchmark (research only; not linked into osie).
find_package(onnxruntime CONFIG QUIET)
if(onnxruntime_FOUND)
  add_executable(osie_ort_bench tools/ort_bench.cpp)
  target_link_libraries(osie_ort_bench PRIVATE osie_core onnxruntime::onnxruntime)
endif()

# ------------------------------------------------------------------ tests ---
if(OSIE_BUILD_TESTS)
  enable_testing()
  # Sanitizer builds compile GoogleTest from source: mixing an uninstrumented
  # prebuilt gtest with ASan's libc++ container annotations produces
  # container-overflow false positives inside gtest itself.
  option(OSIE_FETCH_GTEST "Build GoogleTest from source" OFF)
  if(OSIE_SANITIZE OR OSIE_FETCH_GTEST)
    include(FetchContent)
    FetchContent_Declare(googletest
      URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
      URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
  else()
    find_package(GTest CONFIG)
    if(NOT GTest_FOUND)
      find_package(GTest REQUIRED)
    endif()
  endif()
  file(GLOB OSIE_UNIT_TESTS CONFIGURE_DEPENDS tests/unit/*.cpp)
  add_executable(osie_unit_tests ${OSIE_UNIT_TESTS})
  target_include_directories(osie_unit_tests PRIVATE src tests)
  target_link_libraries(osie_unit_tests PRIVATE osie_core GTest::gtest_main)
  target_compile_definitions(osie_unit_tests PRIVATE OSIE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

  file(GLOB OSIE_PLATFORM_TESTS CONFIGURE_DEPENDS tests/platform/*.cpp tests/integration/*.cpp)
  add_executable(osie_platform_tests ${OSIE_PLATFORM_TESTS})
  target_include_directories(osie_platform_tests PRIVATE src tests)
  target_link_libraries(osie_platform_tests PRIVATE osie_core GTest::gtest_main)
  target_compile_definitions(osie_platform_tests PRIVATE OSIE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

  include(GoogleTest)
  gtest_discover_tests(osie_unit_tests DISCOVERY_TIMEOUT 30)
  gtest_discover_tests(osie_platform_tests DISCOVERY_TIMEOUT 30 PROPERTIES LABELS platform)
endif()

# ------------------------------------------------------------- benchmarks ---
if(OSIE_BUILD_BENCHMARKS)
  find_package(benchmark CONFIG)
  if(benchmark_FOUND)
    add_executable(osie_bench benchmarks/bench_main.cpp)
    target_include_directories(osie_bench PRIVATE src)
    target_link_libraries(osie_bench PRIVATE osie_core benchmark::benchmark)
    target_compile_definitions(osie_bench PRIVATE OSIE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  else()
    message(STATUS "Google Benchmark not found: osie_bench disabled")
  endif()
endif()
