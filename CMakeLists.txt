cmake_minimum_required(VERSION 3.20)
project(Blitz_Limit_Order_Book CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

# --- Build provenance: stamp the Git revision and dirty state into binaries ---
find_package(Git QUIET)
if(Git_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE BLITZ_LOB_GIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE BLITZ_LOB_GIT_RESULT
    )
endif()
if(NOT BLITZ_LOB_GIT_HASH OR NOT BLITZ_LOB_GIT_RESULT EQUAL 0)
    set(BLITZ_LOB_GIT_HASH "unknown")
endif()

# Untracked files are ignored: generated datasets and results must not mark a build dirty.
set(BLITZ_LOB_DIRTY 0)
if(Git_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} status --porcelain --untracked-files=no
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE BLITZ_LOB_GIT_STATUS
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(BLITZ_LOB_GIT_STATUS)
        set(BLITZ_LOB_DIRTY 1)
        message(WARNING "Working tree has uncommitted changes to tracked files -- "
                         "any benchmark result built from this configure will be "
                         "stamped dirty=yes and should not be treated as pinned to "
                         "${BLITZ_LOB_GIT_HASH}.")
    endif()
endif()

# --- Helpers ------------------------------------------------------------------
set(BLITZ_WARNINGS -Wall -Wextra -Wpedantic)
set(BLITZ_LOB_SOURCES src/main.cpp src/pipeline.cpp src/benchmark.cpp)

# blitz_configure(<target> [SANITIZE <list>] [COMPILE_OPTIONS ...] [INCLUDE_DIRS ...])
function(blitz_configure target)
    cmake_parse_arguments(ARG "" "SANITIZE" "COMPILE_OPTIONS;INCLUDE_DIRS" ${ARGN})
    target_include_directories(${target} PRIVATE ${ARG_INCLUDE_DIRS} include)
    target_compile_options(${target} PRIVATE ${ARG_COMPILE_OPTIONS} ${BLITZ_WARNINGS})
    if(ARG_SANITIZE)
        target_compile_options(${target} PRIVATE -fsanitize=${ARG_SANITIZE})
        target_link_options(${target} PRIVATE -fsanitize=${ARG_SANITIZE})
    endif()
endfunction()

# Binaries that report benchmark results carry the revision and dirty flag.
function(blitz_stamp target version)
    target_compile_definitions(${target} PRIVATE
        HYDRA_LOB_VERSION="${version}"
        HYDRA_LOB_DIRTY=${BLITZ_LOB_DIRTY}
    )
endfunction()

# --- Benchmark binary and sanitizer builds ------------------------------------
# Release build; -march=native is limited to performance targets.
add_executable(blitz_lob ${BLITZ_LOB_SOURCES})
blitz_configure(blitz_lob COMPILE_OPTIONS -O3 -DNDEBUG -march=native)
blitz_stamp(blitz_lob ${BLITZ_LOB_GIT_HASH})

# TSan is built apart from ASan/UBSan because the runtimes conflict.
add_executable(blitz_lob_asan ${BLITZ_LOB_SOURCES})
blitz_configure(blitz_lob_asan COMPILE_OPTIONS -O1 -g SANITIZE address,undefined)
blitz_stamp(blitz_lob_asan ${BLITZ_LOB_GIT_HASH})

add_executable(blitz_lob_tsan ${BLITZ_LOB_SOURCES})
blitz_configure(blitz_lob_tsan COMPILE_OPTIONS -O1 -g SANITIZE thread)
blitz_stamp(blitz_lob_tsan ${BLITZ_LOB_GIT_HASH})

# --- Dataset generator --------------------------------------------------------
# Portable build without -march=native; the revision never enters the dataset bytes.
add_executable(blitz_gen_dataset tools/gen_dataset.cpp)
blitz_configure(blitz_gen_dataset COMPILE_OPTIONS -O2)
target_compile_definitions(blitz_gen_dataset PRIVATE HYDRA_LOB_VERSION="${BLITZ_LOB_GIT_HASH}")

# --- Component tests ----------------------------------------------------------
# blitz_add_test(<name> [SANITIZE <list>] [SOURCES <extra sources>...])
# Registers the executable blitz_lob_test_<name> and a CTest entry named <name>.
function(blitz_add_test name)
    cmake_parse_arguments(ARG "" "SANITIZE" "SOURCES" ${ARGN})
    set(target blitz_lob_test_${name})
    add_executable(${target} tests/test_${name}.cpp ${ARG_SOURCES})
    blitz_configure(${target} COMPILE_OPTIONS -O1 -g SANITIZE "${ARG_SANITIZE}")
    add_test(NAME ${name} COMMAND ${target})
endfunction()

blitz_add_test(phase1_types)
blitz_add_test(phase2_timing)
blitz_add_test(phase3_pool SANITIZE address,undefined)
blitz_add_test(phase4_spsc SANITIZE thread)
blitz_add_test(phase5_orderbook)
blitz_add_test(phase6_matcher SANITIZE address,undefined)
blitz_add_test(phase7_pipeline SOURCES src/pipeline.cpp)
blitz_add_test(phase8_benchmark SOURCES src/benchmark.cpp src/pipeline.cpp)
blitz_add_test(phase9_suite SANITIZE address,undefined)

# --- Ablation variants (EXCLUDE_FROM_ALL; built by scripts/run_ablations.sh) --
# Each variant shadows one header via include order. flat_layout also changes
# DatasetRecord, so it needs its own dataset generator.
foreach(variant mutex_queue unpinned malloc_pool flat_layout)
    set(target blitz_lob_ablation_${variant})
    add_executable(${target} EXCLUDE_FROM_ALL ${BLITZ_LOB_SOURCES})
    blitz_configure(${target}
        COMPILE_OPTIONS -O3 -DNDEBUG -march=native
        INCLUDE_DIRS ablation/${variant}/include)
    blitz_stamp(${target} ${BLITZ_LOB_GIT_HASH}-ablation-${variant})
endforeach()

add_executable(blitz_gen_dataset_ablation_flat_layout EXCLUDE_FROM_ALL tools/gen_dataset.cpp)
blitz_configure(blitz_gen_dataset_ablation_flat_layout
    COMPILE_OPTIONS -O2
    INCLUDE_DIRS ablation/flat_layout/include)
target_compile_definitions(blitz_gen_dataset_ablation_flat_layout PRIVATE
    HYDRA_LOB_VERSION="${BLITZ_LOB_GIT_HASH}-ablation-flat_layout")
