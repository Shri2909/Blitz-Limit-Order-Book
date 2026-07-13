cmake_minimum_required(VERSION 3.20)
project(Blitz_Limit_Order_Book CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ENABLE_AFXDP "Build with AF_XDP support" OFF)

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

if(ENABLE_AFXDP)
    target_sources(blitz_lob PRIVATE net/xdp_socket_user.cpp)
    target_compile_definitions(blitz_lob PRIVATE ENABLE_AFXDP)
    target_link_libraries(blitz_lob PRIVATE bpf)
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
# Test binary
# WHY: Standalone target that links NOTHING from src/ — tests include headers
#      directly and own their own main(). No GTest dependency.
# ──────────────────────────────────────────────────────────────────────────────
add_executable(blitz_lob_tests
    tests/test_hydra_lob.cpp
)
target_include_directories(blitz_lob_tests PRIVATE include)
target_compile_options(blitz_lob_tests PRIVATE
    -O1 -g -Wall -Wextra -Wpedantic
)

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
