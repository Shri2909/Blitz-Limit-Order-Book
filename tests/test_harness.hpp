#pragma once

// tests/test_harness.hpp
//
// Minimal shared machinery reused by every tests/test_phaseN_*.cpp file.
// Each phase's test file is its own standalone binary with its own main()
// (see CMakeLists.txt's per-phase target block) so that phase's exit
// condition can be built and run independently -- this header exists only
// so the CHECK/RUN_TEST boilerplate isn't copy-pasted verbatim into every
// one of them. Header-only, no .cpp of its own, not a target itself.

#include <cstdio>
#include <sstream>
#include <string>

namespace hydra::test
{

    struct CheckFailure
    {
        std::string message;
    };

    // WHY inline (not static/anonymous-namespace): each phase's .cpp is
    // compiled into its own standalone binary, so there's never a
    // multi-TU-in-one-binary ODR concern here -- inline is simply the
    // correct, warning-free way to define a header-only namespace-scope
    // variable in C++17+.
    inline int g_pass_count = 0;
    inline int g_fail_count = 0;

    // Prints the final tally to stderr and returns the process exit code a
    // phase's main() should return: 0 iff every RUN_TEST in this binary
    // passed, matching the "fail the job on any [FAIL] line" contract the
    // roadmap's CI phase relies on.
    [[nodiscard]] inline int report_and_exit_code() noexcept
    {
        std::fprintf(stderr, "\n%d passed, %d failed (of %d total)\n",
                     g_pass_count, g_fail_count, g_pass_count + g_fail_count);
        return (g_fail_count == 0) ? 0 : 1;
    }

} // namespace hydra::test

#define HYDRA_CHECK(cond)                                            \
    do                                                               \
    {                                                                \
        if (!(cond))                                                 \
        {                                                            \
            std::ostringstream _hydra_oss;                           \
            _hydra_oss << "CHECK(" #cond ") failed at " __FILE__ ":" \
                       << __LINE__;                                  \
            throw ::hydra::test::CheckFailure{_hydra_oss.str()};     \
        }                                                            \
    } while (0)

#define HYDRA_CHECK_EQ(actual, expected)                                                                             \
    do                                                                                                               \
    {                                                                                                                \
        auto _hydra_actual = (actual);                                                                              \
        auto _hydra_expected = (expected);                                                                          \
        if (!(_hydra_actual == _hydra_expected))                                                                     \
        {                                                                                                            \
            std::ostringstream _hydra_oss;                                                                          \
            _hydra_oss << "CHECK_EQ(" #actual ", " #expected ") failed at " __FILE__ ":" << __LINE__ << " -- actual=" \
                       << _hydra_actual << ", expected=" << _hydra_expected;                                         \
            throw ::hydra::test::CheckFailure{_hydra_oss.str()};                                                     \
        }                                                                                                            \
    } while (0)

#define RUN_TEST(fn)                                                         \
    do                                                                       \
    {                                                                        \
        try                                                                  \
        {                                                                    \
            fn();                                                            \
            std::fprintf(stderr, "[PASS] %s\n", #fn);                        \
            ++::hydra::test::g_pass_count;                                   \
        }                                                                    \
        catch (const ::hydra::test::CheckFailure &cf)                        \
        {                                                                    \
            std::fprintf(stderr, "[FAIL] %s: %s\n", #fn, cf.message.c_str()); \
            ++::hydra::test::g_fail_count;                                   \
        }                                                                    \
        catch (const std::exception &e)                                     \
        {                                                                    \
            std::fprintf(stderr, "[FAIL] %s: unexpected exception: %s\n",   \
                         #fn, e.what());                                    \
            ++::hydra::test::g_fail_count;                                  \
        }                                                                    \
        catch (...)                                                          \
        {                                                                    \
            std::fprintf(stderr, "[FAIL] %s: unexpected non-exception "     \
                                 "failure\n",                                \
                         #fn);                                              \
            ++::hydra::test::g_fail_count;                                  \
        }                                                                    \
    } while (0)
