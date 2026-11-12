#pragma once

// Shared assertions and result reporting for standalone test executables.

#include <cstdio>
#include <exception>
#include <sstream>
#include <string>

namespace hydra::test
{

    struct CheckFailure
    {
        std::string message;
    };

    inline int g_pass_count = 0;
    inline int g_fail_count = 0;

    // Print the tally and return nonzero if any test failed.
    [[nodiscard]] inline int report_and_exit_code() noexcept
    {
        std::fprintf(stderr, "\n%d passed, %d failed (of %d total)\n", g_pass_count, g_fail_count,
                     g_pass_count + g_fail_count);
        return (g_fail_count == 0) ? 0 : 1;
    }

} // namespace hydra::test

#define HYDRA_CHECK(cond)                                                                          \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
        {                                                                                          \
            std::ostringstream _hydra_oss;                                                         \
            _hydra_oss << "CHECK(" #cond ") failed at " __FILE__ ":" << __LINE__;                  \
            throw ::hydra::test::CheckFailure{_hydra_oss.str()};                                   \
        }                                                                                          \
    } while (0)

#define HYDRA_CHECK_EQ(actual, expected)                                                           \
    do                                                                                             \
    {                                                                                              \
        auto _hydra_actual = (actual);                                                             \
        auto _hydra_expected = (expected);                                                         \
        if (!(_hydra_actual == _hydra_expected))                                                   \
        {                                                                                          \
            std::ostringstream _hydra_oss;                                                         \
            _hydra_oss << "CHECK_EQ(" #actual ", " #expected ") failed at " __FILE__ ":"           \
                       << __LINE__ << " -- actual=" << _hydra_actual                               \
                       << ", expected=" << _hydra_expected;                                        \
            throw ::hydra::test::CheckFailure{_hydra_oss.str()};                                   \
        }                                                                                          \
    } while (0)

#define RUN_TEST(fn)                                                                               \
    do                                                                                             \
    {                                                                                              \
        try                                                                                        \
        {                                                                                          \
            fn();                                                                                  \
            std::fprintf(stderr, "[PASS] %s\n", #fn);                                              \
            ++::hydra::test::g_pass_count;                                                         \
        }                                                                                          \
        catch (const ::hydra::test::CheckFailure &cf)                                              \
        {                                                                                          \
            std::fprintf(stderr, "[FAIL] %s: %s\n", #fn, cf.message.c_str());                      \
            ++::hydra::test::g_fail_count;                                                         \
        }                                                                                          \
        catch (const std::exception &e)                                                            \
        {                                                                                          \
            std::fprintf(stderr, "[FAIL] %s: unexpected exception: %s\n", #fn, e.what());          \
            ++::hydra::test::g_fail_count;                                                         \
        }                                                                                          \
        catch (...)                                                                                \
        {                                                                                          \
            std::fprintf(stderr,                                                                   \
                         "[FAIL] %s: unexpected non-exception "                                    \
                         "failure\n",                                                              \
                         #fn);                                                                     \
            ++::hydra::test::g_fail_count;                                                         \
        }                                                                                          \
    } while (0)
