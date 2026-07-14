cmake_minimum_required(VERSION 3.20)
project(Blitz_Limit_Order_Book CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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
target_compile_definitions(blitz_lob PRIVATE HYDRA_LOB_VERSION="${BLITZ_LOB_GIT_HASH}")

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

# ──────────────────────────────────────────────────────────────────────────────
# Canonical correctness suite (Phase 9)
# WHY: Standalone target — includes headers directly, owns its own main().
#      Links nothing from src/. No GTest dependency.
# ──────────────────────────────────────────────────────────────────────────────
add_executable(blitz_lob_tests
    tests/test_hydra_lob.cpp
)
target_include_directories(blitz_lob_tests PRIVATE include)
target_compile_options(blitz_lob_tests PRIVATE
    -O1 -g -Wall -Wextra -Wpedantic
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
#      test_phase2_timing   → Phase 2  rdtsc clock + histogram smoke test
#      test_phase3_pool     → Phase 3  object pool acquire/release/overflow
#      test_phase4_spsc     → Phase 4  SPSC ring buffer ordering under TSan
#      test_phase5_orderbook→ Phase 5  add/cancel/best_bid/best_ask
#      test_phase6_matcher  → Phase 6  price-time, pro-rata, IOC, FOK
#      test_phase7_pipeline → Phase 7  affinity pin + verify, jthread shutdown
#      test_phase8_benchmark→ Phase 8  dataset determinism + CSV column check
#      test_phase9_suite    → Phase 9  extended edge cases beyond the 15 core
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