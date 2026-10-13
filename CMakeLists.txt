cmake_minimum_required(VERSION 3.24)
project(osie VERSION 0.2.0 LANGUAGES C CXX)

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
option(OSIE_PORTABLE "Release build: statically link the C++ runtime on Linux" OFF)
option(OSIE_FUZZ "Build libFuzzer harnesses (clang with libclang-rt)" OFF)
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

if(OSIE_FUZZ)
  # Coverage feedback must cover the library, not just the harness.
  add_compile_options(-fsanitize=fuzzer-no-link)
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
  src/state/history.cpp
  src/architecture/graph.cpp
  src/architecture/model.cpp
  src/reasoning/reasoner.cpp
  src/intent/frame.cpp
  src/intent/rule_parser.cpp
  src/intent/safety.cpp
  src/intent/understand.cpp
  src/inference/neural_parser.cpp
  src/inference/model_locator.cpp
  src/prediction/features.cpp
  src/prediction/world_model.cpp
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
target_compile_definitions(osie PRIVATE OSIE_VERSION="${PROJECT_VERSION}")
# The binary never embeds a source-tree path. In the build tree the model is
# copied to the same relative location as an installation
# (<build>/share/osie/models), so lookup works identically in both.
add_custom_command(TARGET osie POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/models/intent.osiem
          ${CMAKE_BINARY_DIR}/share/osie/models/intent.osiem)
if(EXISTS ${CMAKE_SOURCE_DIR}/models/world_cpu.osiewm)
  add_custom_command(TARGET osie POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/models/world_cpu.osiewm
            ${CMAKE_BINARY_DIR}/share/osie/models/world_cpu.osiewm)
endif()
if(OSIE_PORTABLE AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # Release archives: avoid depending on the target system's libstdc++ version.
  target_link_options(osie PRIVATE -static-libstdc++ -static-libgcc)
endif()

add_executable(osie_wm_lab tools/world_model_lab.cpp)  # world-model data collection (serve: Linux only) and encoding
target_link_libraries(osie_wm_lab PRIVATE osie_core)

add_executable(osie_eval tools/osie_eval.cpp)
target_link_libraries(osie_eval PRIVATE osie_core)
add_executable(osie_perf tools/osie_perf.cpp)
target_link_libraries(osie_perf PRIVATE osie_core)

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

# ------------------------------------------------------------------- fuzz ---
if(OSIE_FUZZ)
  add_executable(osie_fuzz_model tests/fuzz/fuzz_model_loader.cpp)
  target_link_libraries(osie_fuzz_model PRIVATE osie_core)
  target_compile_options(osie_fuzz_model PRIVATE -fsanitize=fuzzer)
  target_link_options(osie_fuzz_model PRIVATE -fsanitize=fuzzer)
  add_executable(osie_fuzz_world_model tests/fuzz/fuzz_world_model_loader.cpp)
  target_link_libraries(osie_fuzz_world_model PRIVATE osie_core)
  target_compile_options(osie_fuzz_world_model PRIVATE -fsanitize=fuzzer)
  target_link_options(osie_fuzz_world_model PRIVATE -fsanitize=fuzzer)
endif()

# ---------------------------------------------------------------- install ---
include(GNUInstallDirs)
install(TARGETS osie RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES models/intent.osiem DESTINATION ${CMAKE_INSTALL_DATADIR}/osie/models)
install(FILES models/world_cpu.osiewm DESTINATION ${CMAKE_INSTALL_DATADIR}/osie/models OPTIONAL)
install(FILES models/SHA256SUMS DESTINATION ${CMAKE_INSTALL_DATADIR}/osie/models OPTIONAL)
install(FILES LICENSE README.md DESTINATION ${CMAKE_INSTALL_DOCDIR})

# ------------------------------------------------------------------ cpack ---
set(CPACK_PACKAGE_NAME osie)
set(CPACK_PACKAGE_VENDOR "OSIE contributors")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Local, read-only OS intelligence engine (native macOS/Linux observations)")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/deosha/osie")
set(CPACK_RESOURCE_FILE_LICENSE ${CMAKE_SOURCE_DIR}/LICENSE)
set(CPACK_PACKAGE_CONTACT "https://github.com/deosha/osie/issues")
set(CPACK_STRIP_FILES ON)
# Archive names carry OS and architecture: osie-<version>-<os>-<arch>.tar.gz
if(APPLE)
  set(_osie_arch "${CMAKE_OSX_ARCHITECTURES}")
  if(NOT _osie_arch)
    set(_osie_arch "${CMAKE_SYSTEM_PROCESSOR}")
  endif()
  set(CPACK_SYSTEM_NAME "macos-${_osie_arch}")
else()
  set(CPACK_SYSTEM_NAME "linux-${CMAKE_SYSTEM_PROCESSOR}")
endif()
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_SECTION utils)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_RPM_PACKAGE_LICENSE "Apache-2.0")
set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
set(CPACK_RPM_PACKAGE_AUTOREQ ON)
include(CPack)
