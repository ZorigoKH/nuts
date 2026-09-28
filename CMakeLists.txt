cmake_minimum_required(VERSION 3.24)  # 3.24: FetchContent's DOWNLOAD_EXTRACT_TIMESTAMP
project(nuts
  VERSION 0.1.0
  DESCRIPTION "Poker hand evaluator, equity calculator and game-theory solvers"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Default to an optimized build when no build type is given.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Tests and tools are on by default when nuts is the top-level project, and off when it is
# pulled into another build. They are also off by default for WebAssembly, which has no use for
# them (the tools and the tests start threads, and the WebAssembly build has none).
if(PROJECT_IS_TOP_LEVEL AND NOT EMSCRIPTEN)
  set(NUTS_EXTRAS_DEFAULT ON)
else()
  set(NUTS_EXTRAS_DEFAULT OFF)
endif()
option(NUTS_TESTS "Build the C++ tests (Catch2)" ${NUTS_EXTRAS_DEFAULT})
option(NUTS_TOOLS "Build the command-line tools (benchmarks)" ${NUTS_EXTRAS_DEFAULT})
# The Python module nuts._core. pyproject.toml turns this on for pip builds.
option(NUTS_PYTHON "Build the Python module (needs Python and nanobind)" OFF)
set(NUTS_SANITIZE "" CACHE STRING
  "Sanitizers to build with, joined by '+': e.g. address+undefined or thread (empty = none)")

# Sanitizers are applied to every target, third-party code included: ThreadSanitizer needs all
# code to be instrumented, and it keeps AddressSanitizer's view of the heap consistent.
if(NUTS_SANITIZE)
  string(REPLACE "+" "," NUTS_SANITIZE_LIST "${NUTS_SANITIZE}")
  add_compile_options(-fsanitize=${NUTS_SANITIZE_LIST} -fno-omit-frame-pointer
                      -fno-sanitize-recover=all)
  add_link_options(-fsanitize=${NUTS_SANITIZE_LIST})
endif()

# Strict warnings for our own targets only.
function(nuts_set_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endfunction()

# ---- Core library ---------------------------------------------------------------------------
find_package(Threads REQUIRED)  # Monte Carlo equity runs on several threads

add_library(nuts STATIC
  src/card.cpp
  src/equity.cpp
  src/evaluator.cpp
  src/kuhn.cpp
  src/pushfold.cpp
  src/range.cpp)
add_library(nuts::nuts ALIAS nuts)
target_include_directories(nuts PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(nuts PUBLIC cxx_std_20)
target_link_libraries(nuts PUBLIC Threads::Threads)
# Position-independent code, so the library can also be linked into a shared Python extension.
set_target_properties(nuts PROPERTIES POSITION_INDEPENDENT_CODE ON)
nuts_set_warnings(nuts)

# ---- The equity matrix, compiled in ---------------------------------------------------------
# The Python module and the WebAssembly build solve push/fold on request, which needs the
# committed equity matrix data/equity169.bin. They cannot read it at run time (an installed
# Python package has no data/ directory next to it, and a web page has no file system), so CMake
# writes the file's bytes into a C++ source file as an array (see include/nuts/embedded_data.hpp).
set(NUTS_EQUITY169_FILE ${CMAKE_CURRENT_SOURCE_DIR}/data/equity169.bin)
# Re-run this step when the data file changes.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${NUTS_EQUITY169_FILE})
file(READ ${NUTS_EQUITY169_FILE} NUTS_EQUITY169_HEX HEX)  # "4e555453..." (2 digits per byte)
# Break the digits into lines of 16 bytes, then turn every byte "4e" into "0x4e,". (CMake regular
# expressions have no {16} repeat count, hence string(REPEAT).)
string(REPEAT "[0-9a-f][0-9a-f]" 16 NUTS_SIXTEEN_BYTES)
string(REGEX REPLACE "(${NUTS_SIXTEEN_BYTES})" "\\1\n" NUTS_EQUITY169_HEX "${NUTS_EQUITY169_HEX}")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," NUTS_EQUITY169_BYTES "${NUTS_EQUITY169_HEX}")
configure_file(src/embedded_equity169_bytes.cpp.in
               ${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_equity169_bytes.cpp @ONLY)

add_library(nuts_embedded_data STATIC
  src/embedded_data.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_equity169_bytes.cpp)
add_library(nuts::embedded_data ALIAS nuts_embedded_data)
target_link_libraries(nuts_embedded_data PUBLIC nuts)
set_target_properties(nuts_embedded_data PROPERTIES POSITION_INDEPENDENT_CODE ON)
nuts_set_warnings(nuts_embedded_data)

# ---- Tools ----------------------------------------------------------------------------------
if(NUTS_TOOLS)
  add_executable(nuts_bench tools/bench.cpp)
  target_link_libraries(nuts_bench PRIVATE nuts)
  nuts_set_warnings(nuts_bench)
  # Bake the build type and compiler flags into the benchmark so its output records them.
  string(TOUPPER "${CMAKE_BUILD_TYPE}" NUTS_BUILD_TYPE_UPPER)
  string(STRIP "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${NUTS_BUILD_TYPE_UPPER}}" NUTS_CXX_FLAGS)
  target_compile_definitions(nuts_bench PRIVATE
    NUTS_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
    NUTS_CXX_FLAGS="${NUTS_CXX_FLAGS}")

  # Generates data/equity169.bin (run by hand; takes minutes, see the file's header comment).
  add_executable(nuts_gen_equity169 tools/gen_equity169.cpp)
  target_link_libraries(nuts_gen_equity169 PRIVATE nuts)
  nuts_set_warnings(nuts_gen_equity169)

  # Generates data/pushfold.json from data/equity169.bin, and the push/fold thresholds.
  add_executable(nuts_gen_pushfold tools/gen_pushfold.cpp)
  target_link_libraries(nuts_gen_pushfold PRIVATE nuts)
  nuts_set_warnings(nuts_gen_pushfold)
endif()

# ---- Tests ----------------------------------------------------------------------------------
if(NUTS_TESTS)
  include(FetchContent)
  FetchContent_Declare(Catch2
    URL https://github.com/catchorg/Catch2/archive/refs/tags/v3.16.0.tar.gz
    URL_HASH SHA256=0957cae5821b17ce07f0833aaa52b5137643a8382203221f363a8303c109af34
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(Catch2)

  add_executable(nuts_tests
    tests/cpp/test_card.cpp
    tests/cpp/test_embedded_data.cpp
    tests/cpp/test_equity.cpp
    tests/cpp/test_evaluator.cpp
    tests/cpp/test_kuhn.cpp
    tests/cpp/test_pushfold.cpp
    tests/cpp/test_range.cpp
    tests/cpp/test_rng.cpp)
  # nuts::embedded_data brings in nuts itself.
  target_link_libraries(nuts_tests PRIVATE nuts::embedded_data Catch2::Catch2WithMain)
  nuts_set_warnings(nuts_tests)
  # The push/fold tests read the committed equity matrix from data/.
  target_compile_definitions(nuts_tests PRIVATE NUTS_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")

  # One CTest test per Catch2 test case. Test cases tagged [.] (hidden), such as the
  # [exhaustive] ones, are not registered; run them directly: nuts_tests "[exhaustive]".
  enable_testing()
  list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
  include(Catch)
  catch_discover_tests(nuts_tests DISCOVERY_MODE PRE_TEST)
endif()

# ---- Python module --------------------------------------------------------------------------
# nuts._core, wrapped by python/nuts/__init__.py. Built by `pip install .` (scikit-build-core
# runs CMake with NUTS_PYTHON=ON, see pyproject.toml).
if(NUTS_PYTHON)
  find_package(Python 3.10 REQUIRED COMPONENTS Interpreter Development.Module)
  # Ask the nanobind Python package where its CMake files are (needed when CMake is run by hand;
  # under pip, scikit-build-core also tells CMake).
  execute_process(
    COMMAND "${Python_EXECUTABLE}" -m nanobind --cmake_dir
    OUTPUT_STRIP_TRAILING_WHITESPACE OUTPUT_VARIABLE nanobind_ROOT)
  find_package(nanobind CONFIG REQUIRED)

  nanobind_add_module(_core python/src/bindings.cpp)
  target_link_libraries(_core PRIVATE nuts::embedded_data)
  nuts_set_warnings(_core)
  install(TARGETS _core LIBRARY DESTINATION nuts)
endif()

# ---- WebAssembly ----------------------------------------------------------------------------
# With Emscripten (emcmake cmake ...): nuts.mjs, an ES module that loads nuts.wasm. See
# wasm/README.md.
if(EMSCRIPTEN)
  add_executable(nuts_wasm wasm/bindings.cpp)
  target_link_libraries(nuts_wasm PRIVATE nuts::embedded_data)
  nuts_set_warnings(nuts_wasm)
  set_target_properties(nuts_wasm PROPERTIES OUTPUT_NAME nuts SUFFIX ".mjs")
  target_link_options(nuts_wasm PRIVATE
    -lembind                        # the C++ <-> JavaScript bindings in wasm/bindings.cpp
    -O3                             # optimize at link time too (wasm-opt)
    -sMODULARIZE=1                  # export a function that creates the module ...
    -sEXPORT_ES6=1                  # ... from an ES module (import createNuts from "./nuts.mjs")
    -sEXPORT_NAME=createNuts
    -sENVIRONMENT=web,worker,node   # runs in a page, a web worker and Node.js
    -sALLOW_MEMORY_GROWTH=1         # the heap grows as needed (Monte Carlo on big ranges)
    -sFILESYSTEM=0)                 # nothing reads files: the equity matrix is compiled in
endif()
