cmake_minimum_required(VERSION 3.20)
project(Blitz_Limit_Order_Book CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# WHY no use of CMAKE_BUILD_TYPE anywhere below: every target here has
# genuinely different, independently-justified flags (Release/-march=native
# for blitz_lob, -O1 -g -fsanitize=... for the sanitizer/per-phase targets,
# -O2 with no -march=native for the byte-reproducible dataset generator) --
# a single global Debug/Release switch can't express that. Target name
# selects the build flavor instead; -DCMAKE_BUILD_TYPE=... is intentionally
# a no-op in this project, not an oversight.

# WHY: 10 per-phase exit-condition targets used to mean a developer had to
# know and invoke every target name by hand (see the shell loop this used
# to require). ctest wiring below turns that into one command: `ctest
# --test-dir build --output-on-failure`. Per-phase granularity is still
# available (`ctest -R phase6`) -- this does not collapse the targets, only
# the invocation surface.
enable_testing()

option(ENABLE_AFXDP "Build with AF_XDP support" OFF)

# ──────────────────────────────────────────────────────────────────────────────
# Build version string
# WHY: benchmark.cpp's CSV output stamps every row with this string so results
#      stay traceable back to the exact commit that produced them. Falls back
#      to "unknown" (not silently to a stale/misleading hash) if git isn't
#      available or this isn't a git checkout (e.g. a source tarball).
# ──────────────────────────────────────────────────────────────────────────────
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

# ──────────────────────────────────────────────────────────────────────────────
# Dirty-tree detection
# WHY: BLITZ_LOB_GIT_HASH alone can look like a trustworthy commit pin while
#      actually being built from a working tree that has diverged from that
#      commit -- exactly what produced this project's own "measured against
#      an uncommitted working tree" caveat. Recomputed at every configure, so
#      it reflects the tree as it stood at build time, matching what the
#      resulting binary actually contains. Untracked files don't count as
#      dirty (a generated results/ CSV sitting in the tree shouldn't flag
#      every subsequent build) -- only modifications to tracked files do.
# ──────────────────────────────────────────────────────────────────────────────
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

# ──────────────────────────────────────────────────────────────────────────────
# Release binary
# WHY: -march=native only here — sanitizer targets run on CI runners that may
#      lack extended instruction sets; native optimizations are meaningless for
#      correctness builds anyway.
# ──────────────────────────────────────────────────────────────────────────────
add_executable(blitz_lob
    src/main.cpp
    src/pipeline.cpp
    src/benchmark.cpp
)
target_include_directories(blitz_lob PRIVATE include)
target_compile_options(blitz_lob PRIVATE
    -O3 -DNDEBUG -march=native -Wall -Wextra -Wpedantic
)
target_compile_definitions(blitz_lob PRIVATE
    HYDRA_LOB_VERSION="${BLITZ_LOB_GIT_HASH}"
    HYDRA_LOB_DIRTY=${BLITZ_LOB_DIRTY}
)

if(ENABLE_AFXDP)
    # ──────────────────────────────────────────────────────────────────────
    # AF_XDP dependency discovery (Phase 10)
    # WHY both libbpf AND libxdp: modern libbpf (>=1.0, matches the
    #      libbpf-dev/libxdp-dev packages this targets) no longer ships
    #      xsk.h -- AF_XDP ring/socket support (xsk_umem__create,
    #      xsk_socket__create, ...) moved to libxdp. Linking -lbpf alone is
    #      not sufficient; xdp_socket_user.cpp needs both.
    # WHY pkg-config: both packages ship .pc files (libbpf-dev, libxdp-dev
    #      on Debian/Ubuntu); this avoids hardcoding /usr/include paths
    #      that only work on one distro.
    # ──────────────────────────────────────────────────────────────────────
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBBPF REQUIRED libbpf)
    pkg_check_modules(LIBXDP REQUIRED libxdp)

    target_sources(blitz_lob PRIVATE net/xdp_socket_user.cpp)
    target_compile_definitions(blitz_lob PRIVATE ENABLE_AFXDP)
    target_include_directories(blitz_lob PRIVATE ${LIBBPF_INCLUDE_DIRS} ${LIBXDP_INCLUDE_DIRS})
    target_link_libraries(blitz_lob PRIVATE ${LIBBPF_LIBRARIES} ${LIBXDP_LIBRARIES})

    # ──────────────────────────────────────────────────────────────────────
    # Manual AF_XDP test traffic generator (Phase 10) -- mirrors
    # blitz_gen_dataset's target shape below. Only meaningful alongside a
    # real or veth AF_XDP RX path (blitz_lob --xdp-iface ...), so it's only
    # built in this ENABLE_AFXDP block. No libbpf/libxdp linking needed --
    # it only uses OrderWireFormat's layout, never calls into either
    # library.
    # ──────────────────────────────────────────────────────────────────────
    add_executable(blitz_send_test_orders
        tools/send_test_orders.cpp
    )
    target_include_directories(blitz_send_test_orders PRIVATE include
        ${LIBBPF_INCLUDE_DIRS} ${LIBXDP_INCLUDE_DIRS})
    target_compile_definitions(blitz_send_test_orders PRIVATE ENABLE_AFXDP)
    target_compile_options(blitz_send_test_orders PRIVATE
        -O2 -Wall -Wextra -Wpedantic
    )
endif()

# ──────────────────────────────────────────────────────────────────────────────
# ASan + UBSan binary
# WHY: Separate target so address and thread sanitizers NEVER share a compile;
#      combining -fsanitize=address and -fsanitize=thread in one binary is
#      explicitly unsupported by Clang/GCC and produces false positives.
# ──────────────────────────────────────────────────────────────────────────────
add_executable(blitz_lob_asan
    src/main.cpp
    src/pipeline.cpp
    src/benchmark.cpp
)
target_include_directories(blitz_lob_asan PRIVATE include)
target_compile_options(blitz_lob_asan PRIVATE
    -O1 -g -fsanitize=address,undefined -Wall -Wextra -Wpedantic
)
target_link_options(blitz_lob_asan PRIVATE
    -fsanitize=address,undefined
)
target_compile_definitions(blitz_lob_asan PRIVATE
    HYDRA_LOB_VERSION="${BLITZ_LOB_GIT_HASH}"
    HYDRA_LOB_DIRTY=${BLITZ_LOB_DIRTY}
)

# ──────────────────────────────────────────────────────────────────────────────
# TSan + UBSan binary  (SEPARATE from asan — never combine address + thread)
# ──────────────────────────────────────────────────────────────────────────────
add_executable(blitz_lob_tsan
    src/main.cpp
    src/pipeline.cpp
    src/benchmark.cpp
)
target_include_directories(blitz_lob_tsan PRIVATE include)
target_compile_options(blitz_lob_tsan PRIVATE
    -O1 -g -fsanitize=thread,undefined -Wall -Wextra -Wpedantic
)
target_link_options(blitz_lob_tsan PRIVATE
    -fsanitize=thread,undefined
)
target_compile_definitions(blitz_lob_tsan PRIVATE
    HYDRA_LOB_VERSION="${BLITZ_LOB_GIT_HASH}"
    HYDRA_LOB_DIRTY=${BLITZ_LOB_DIRTY}
)

# ──────────────────────────────────────────────────────────────────────────────
# Per-phase exit-condition test binaries
# WHY: each file below owns its own main() and is its own executable target —
#      NOT linked together into one binary — so each phase's exit condition
#      can be built and run independently as that phase is completed, without
#      an empty/not-yet-written later-phase file's absent main() (or a
#      not-yet-existing target) blocking an earlier phase's test from
#      building and running. A phase's target is added here only once its
#      test file actually has content; still-empty phase files are not yet
#      listed as targets (an empty .cpp with no main() would fail to link).
#      test_phase1_types    → Phase 1  config/types static_assert validation
#      test_phase2_timing   → Phase 2  rdtsc clock smoke test
#      test_phase3_pool     → Phase 3  object pool acquire/release/overflow
#      test_phase4_spsc     → Phase 4  SPSC ring buffer ordering under TSan
#      test_phase5_orderbook→ Phase 5  add/cancel/best_bid/best_ask
#      test_phase6_matcher  → Phase 6  price-time, pro-rata, IOC, FOK,
#                                       price improvement, empty-book matching
#      test_phase7_pipeline → Phase 7  affinity pin + verify, jthread shutdown
#      test_phase8_benchmark→ Phase 8  dataset determinism + CSV column check
#      test_phase9_suite    → Phase 9  extended edge cases beyond phase 5/6
#      test_phase10_xdp     → Phase 10 AF_XDP compile-gate smoke test
# ──────────────────────────────────────────────────────────────────────────────
add_executable(blitz_lob_test_phase1_types
    tests/test_phase1_types.cpp
)
target_include_directories(blitz_lob_test_phase1_types PRIVATE include)
target_compile_options(blitz_lob_test_phase1_types PRIVATE
    -O1 -g -Wall -Wextra -Wpedantic
)

add_executable(blitz_lob_test_phase2_timing
    tests/test_phase2_timing.cpp
)
target_include_directories(blitz_lob_test_phase2_timing PRIVATE include)
target_compile_options(blitz_lob_test_phase2_timing PRIVATE
    -O1 -g -Wall -Wextra -Wpedantic
)

# WHY ASan+UBSan baked directly into this one target (unlike the other
# per-phase test binaries): Phase 3's exit condition explicitly says "zero
# leaks reported ... under ASan" -- an uninstrumented build can only prove
# the pool's logic looks right, not that ASan itself found nothing.
add_executable(blitz_lob_test_phase3_pool
    tests/test_phase3_pool.cpp
)
target_include_directories(blitz_lob_test_phase3_pool PRIVATE include)
target_compile_options(blitz_lob_test_phase3_pool PRIVATE
    -O1 -g -fsanitize=address,undefined -Wall -Wextra -Wpedantic
)
target_link_options(blitz_lob_test_phase3_pool PRIVATE
    -fsanitize=address,undefined
)

# WHY TSan+UBSan (never combined with ASan -- see the WHY note above
# blitz_lob_asan/blitz_lob_tsan): Phase 4's exit condition explicitly says
# "passes under TSan with zero races" -- an uninstrumented run of a
# producer/consumer stress test proves nothing about data races, since a
# race can be "correct" by luck on any single untooled run.
add_executable(blitz_lob_test_phase4_spsc
    tests/test_phase4_spsc.cpp
)
target_include_directories(blitz_lob_test_phase4_spsc PRIVATE include)
target_compile_options(blitz_lob_test_phase4_spsc PRIVATE
    -O1 -g -fsanitize=thread,undefined -Wall -Wextra -Wpedantic
)
target_link_options(blitz_lob_test_phase4_spsc PRIVATE
    -fsanitize=thread,undefined
)

add_executable(blitz_lob_test_phase5_orderbook
    tests/test_phase5_orderbook.cpp
)
target_include_directories(blitz_lob_test_phase5_orderbook PRIVATE include)
target_compile_options(blitz_lob_test_phase5_orderbook PRIVATE
    -O1 -g -Wall -Wextra -Wpedantic
)

# WHY ASan+UBSan here too (not required by Phase 6's exit condition text,
# but justified by this file's specific history): matcher.hpp's pro-rata
# path previously had a real use-after-free and a crossed-book bug, both
# fixed -- this target exists to make a regression of either impossible to
# pass silently.
add_executable(blitz_lob_test_phase6_matcher
    tests/test_phase6_matcher.cpp
)
target_include_directories(blitz_lob_test_phase6_matcher PRIVATE include)
target_compile_options(blitz_lob_test_phase6_matcher PRIVATE
    -O1 -g -fsanitize=address,undefined -Wall -Wextra -Wpedantic
)
target_link_options(blitz_lob_test_phase6_matcher PRIVATE
    -fsanitize=address,undefined
)

# WHY this target also lists src/pipeline.cpp (unlike every other per-phase
# target): rx_thread_fn/matching_thread_fn are declared in
# hydra/pipeline.hpp but DEFINED in src/pipeline.cpp -- a real .cpp, not a
# header -- so this is the one phase whose test needs an extra translation
# unit linked in to exercise the real thread bodies.
add_executable(blitz_lob_test_phase7_pipeline
    tests/test_phase7_pipeline.cpp
    src/pipeline.cpp
)
target_include_directories(blitz_lob_test_phase7_pipeline PRIVATE include)
target_compile_options(blitz_lob_test_phase7_pipeline PRIVATE
    -O1 -g -Wall -Wextra -Wpedantic
)

# WHY this target also lists src/benchmark.cpp AND src/pipeline.cpp: like
# Phase 7's target above, run_benchmark() is a real .cpp definition (not
# header-only) and internally spawns matching_thread_fn, defined in the
# separate pipeline.cpp -- both translation units must be linked in.
add_executable(blitz_lob_test_phase8_benchmark
    tests/test_phase8_benchmark.cpp
    src/benchmark.cpp
    src/pipeline.cpp
)
target_include_directories(blitz_lob_test_phase8_benchmark PRIVATE include)
target_compile_options(blitz_lob_test_phase8_benchmark PRIVATE
    -O1 -g -Wall -Wextra -Wpedantic
)

# WHY ASan+UBSan (same rationale as Phase 6's target): this file exercises
# matcher.hpp's pool-exhaustion-mid-match paths (order/level/fill pools all
# drained to empty, then matched against) and multi-level sweeps -- exactly
# the shape of edge case where a subtle use-after-free or double-release
# would hide behind "the numbers happened to still be right."
add_executable(blitz_lob_test_phase9_suite
    tests/test_phase9_suite.cpp
)
target_include_directories(blitz_lob_test_phase9_suite PRIVATE include)
target_compile_options(blitz_lob_test_phase9_suite PRIVATE
    -O1 -g -fsanitize=address,undefined -Wall -Wextra -Wpedantic
)
target_link_options(blitz_lob_test_phase9_suite PRIVATE
    -fsanitize=address,undefined
)

# WHY this target always exists, regardless of ENABLE_AFXDP: Phase 10's own
#      exit condition requires that "with ENABLE_AFXDP undefined, the rest
#      of the system builds and passes the full test suite untouched" --
#      this target (and test_phase10_xdp.cpp's own #ifdef ENABLE_AFXDP
#      guards) is what proves that, rather than the test simply not
#      existing in that configuration. With ENABLE_AFXDP ON, it also needs
#      libbpf/libxdp's headers (transitively via zero_copy_parser.hpp ->
#      xdp_socket.hpp) even though it never opens a real socket -- see
#      test_phase10_xdp.cpp's own header comment for why a synthetic
#      in-memory frame is enough to test parse_order_zero_copy() without
#      root or a real interface.
add_executable(blitz_lob_test_phase10_xdp
    tests/test_phase10_xdp.cpp
)
target_include_directories(blitz_lob_test_phase10_xdp PRIVATE include)
target_compile_options(blitz_lob_test_phase10_xdp PRIVATE
    -O1 -g -Wall -Wextra -Wpedantic
)
if(ENABLE_AFXDP)
    target_compile_definitions(blitz_lob_test_phase10_xdp PRIVATE ENABLE_AFXDP)
    target_include_directories(blitz_lob_test_phase10_xdp PRIVATE ${LIBBPF_INCLUDE_DIRS} ${LIBXDP_INCLUDE_DIRS})
    target_link_libraries(blitz_lob_test_phase10_xdp PRIVATE ${LIBBPF_LIBRARIES} ${LIBXDP_LIBRARIES})
endif()

# ──────────────────────────────────────────────────────────────────────────────
# ctest registration -- the one-command correctness gate.
# WHY each test's own binary IS the "command": every target above already
# has its own RUN_TEST/[PASS]/[FAIL] harness (test_harness.hpp) and returns
# non-zero on any failure, so ctest needs no special assertions of its own
# -- it just needs to run each binary and trust its exit code, which is
# exactly what a bare COMMAND add_test() does.
# ──────────────────────────────────────────────────────────────────────────────
add_test(NAME phase1_types COMMAND blitz_lob_test_phase1_types)
add_test(NAME phase2_timing COMMAND blitz_lob_test_phase2_timing)
add_test(NAME phase3_pool COMMAND blitz_lob_test_phase3_pool)
add_test(NAME phase4_spsc COMMAND blitz_lob_test_phase4_spsc)
add_test(NAME phase5_orderbook COMMAND blitz_lob_test_phase5_orderbook)
add_test(NAME phase6_matcher COMMAND blitz_lob_test_phase6_matcher)
add_test(NAME phase7_pipeline COMMAND blitz_lob_test_phase7_pipeline)
add_test(NAME phase8_benchmark COMMAND blitz_lob_test_phase8_benchmark)
add_test(NAME phase9_suite COMMAND blitz_lob_test_phase9_suite)
add_test(NAME phase10_xdp COMMAND blitz_lob_test_phase10_xdp)

# ──────────────────────────────────────────────────────────────────────────────
# Dataset generator tool
# WHY: -march=native intentionally OMITTED — this tool writes binary files that
#      must be byte-identical when regenerated on any machine given the same
#      seed + config; native SIMD instructions could produce different floating-
#      point rounding and break the reproducibility contract.
# ──────────────────────────────────────────────────────────────────────────────
add_executable(blitz_gen_dataset
    tools/gen_dataset.cpp
)
target_include_directories(blitz_gen_dataset PRIVATE include)
target_compile_options(blitz_gen_dataset PRIVATE
    -O2 -Wall -Wextra -Wpedantic
)
# WHY this doesn't threaten the byte-reproducibility contract above: the
# version string is only ever printed to stdout / the manifest-entry
# suggestion (tools/gen_dataset.cpp) -- DatasetGenerator::write_to_file()
# never writes it into the .bin file itself, so two builds with different
# git hashes still produce byte-identical output for the same seed+config.
target_compile_definitions(blitz_gen_dataset PRIVATE
    HYDRA_LOB_VERSION="${BLITZ_LOB_GIT_HASH}"
)

# ──────────────────────────────────────────────────────────────────────────────
# Automated ablation suite (see ablation/ and scripts/run_ablations.sh)
# WHY EXCLUDE_FROM_ALL, not an ENABLE_ABLATION_* option: unlike ENABLE_AFXDP
#      (gated because it needs libbpf/libxdp, external deps not every host
#      has), these targets have no external dependency and always compile
#      fine -- the only thing that needs gating is "don't build these by
#      default," which EXCLUDE_FROM_ALL does directly. `cmake --build build`
#      with no --target never builds any of them; `cmake --build build
#      --target blitz_lob_ablation_mutex_queue` (or via
#      scripts/run_ablations.sh) does.
# WHY the include-directory order matters: each target lists its
#      ablation/<name>/include directory BEFORE the real include/ -- the
#      compiler's quote-include search (#include "hydra/...") then resolves
#      the one header that directory shadows from there, and falls through
#      to the real include/ for everything else. See
#      ablation/mutex_queue/include/hydra/spsc_queue.hpp's own WHY for the
#      full mechanism.
# ──────────────────────────────────────────────────────────────────────────────
set(BLITZ_LOB_ABLATION_SOURCES src/main.cpp src/pipeline.cpp src/benchmark.cpp)

add_executable(blitz_lob_ablation_mutex_queue EXCLUDE_FROM_ALL ${BLITZ_LOB_ABLATION_SOURCES})
target_include_directories(blitz_lob_ablation_mutex_queue PRIVATE
    ablation/mutex_queue/include include)
target_compile_options(blitz_lob_ablation_mutex_queue PRIVATE
    -O3 -DNDEBUG -march=native -Wall -Wextra -Wpedantic
)
target_compile_definitions(blitz_lob_ablation_mutex_queue PRIVATE
    HYDRA_LOB_VERSION="${BLITZ_LOB_GIT_HASH}-ablation-mutex_queue"
    HYDRA_LOB_DIRTY=${BLITZ_LOB_DIRTY}
)

add_executable(blitz_lob_ablation_unpinned EXCLUDE_FROM_ALL ${BLITZ_LOB_ABLATION_SOURCES})
target_include_directories(blitz_lob_ablation_unpinned PRIVATE
    ablation/unpinned/include include)
target_compile_options(blitz_lob_ablation_unpinned PRIVATE
    -O3 -DNDEBUG -march=native -Wall -Wextra -Wpedantic
)
target_compile_definitions(blitz_lob_ablation_unpinned PRIVATE
    HYDRA_LOB_VERSION="${BLITZ_LOB_GIT_HASH}-ablation-unpinned"
    HYDRA_LOB_DIRTY=${BLITZ_LOB_DIRTY}
)

add_executable(blitz_lob_ablation_malloc_pool EXCLUDE_FROM_ALL ${BLITZ_LOB_ABLATION_SOURCES})
target_include_directories(blitz_lob_ablation_malloc_pool PRIVATE
    ablation/malloc_pool/include include)
target_compile_options(blitz_lob_ablation_malloc_pool PRIVATE
    -O3 -DNDEBUG -march=native -Wall -Wextra -Wpedantic
)
target_compile_definitions(blitz_lob_ablation_malloc_pool PRIVATE
    HYDRA_LOB_VERSION="${BLITZ_LOB_GIT_HASH}-ablation-malloc_pool"
    HYDRA_LOB_DIRTY=${BLITZ_LOB_DIRTY}
)

# WHY this ablation ALSO needs its own blitz_gen_dataset build: flattening
# Order changes its on-disk size (embedded in DatasetRecord), so a dataset
# generated by the real blitz_gen_dataset is not byte-compatible with this
# ablation's Order layout -- see ablation/flat_layout/include/hydra/
# dataset_generator.hpp's own WHY.
add_executable(blitz_lob_ablation_flat_layout EXCLUDE_FROM_ALL ${BLITZ_LOB_ABLATION_SOURCES})
target_include_directories(blitz_lob_ablation_flat_layout PRIVATE
    ablation/flat_layout/include include)
target_compile_options(blitz_lob_ablation_flat_layout PRIVATE
    -O3 -DNDEBUG -march=native -Wall -Wextra -Wpedantic
)
target_compile_definitions(blitz_lob_ablation_flat_layout PRIVATE
    HYDRA_LOB_VERSION="${BLITZ_LOB_GIT_HASH}-ablation-flat_layout"
    HYDRA_LOB_DIRTY=${BLITZ_LOB_DIRTY}
)

add_executable(blitz_gen_dataset_ablation_flat_layout EXCLUDE_FROM_ALL
    tools/gen_dataset.cpp
)
target_include_directories(blitz_gen_dataset_ablation_flat_layout PRIVATE
    ablation/flat_layout/include include)
target_compile_options(blitz_gen_dataset_ablation_flat_layout PRIVATE
    -O2 -Wall -Wextra -Wpedantic
)
target_compile_definitions(blitz_gen_dataset_ablation_flat_layout PRIVATE
    HYDRA_LOB_VERSION="${BLITZ_LOB_GIT_HASH}-ablation-flat_layout"
)