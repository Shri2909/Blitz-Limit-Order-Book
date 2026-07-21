#pragma once

#include <cerrno>
#include <cstring>
#include <pthread.h>
#include <sched.h>
#include <sstream>
#include <stdexcept>
#include <string>

namespace hydra
{

    namespace detail
    {

        inline std::string describe_mask(const cpu_set_t &mask)
        {
            std::ostringstream out;
            out << '{';
            bool first = true;
            for (int core = 0; core < CPU_SETSIZE; ++core)
            {
                if (CPU_ISSET(static_cast<size_t>(core), &mask))
                {
                    if (!first)
                    {
                        out << ", ";
                    }
                    out << core;
                    first = false;
                }
            }
            out << '}';
            return out.str();
        }

    } // namespace detail

    // Signals a pthread_setaffinity_np() failure only via errno (this
    // function itself never throws/returns status) -- callers MUST follow
    // this with verify_affinity(core_id), which independently re-derives
    // the actual affinity mask from the kernel and throws on any mismatch,
    // including a silently-failed pin_to_core(). Every call site in this
    // codebase already follows this pairing; a lone pin_to_core() call
    // with no verify_affinity() after it can fail silently.
    inline void pin_to_core(int core_id)
    {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(static_cast<size_t>(core_id), &cpuset);

        const pthread_t self = pthread_self();
        const int rc = pthread_setaffinity_np(self, sizeof(cpu_set_t), &cpuset);
        if (rc != 0)
        {
            errno = rc;
        }
    }

    inline void verify_affinity(int expected_core_id)
    {
        cpu_set_t actual;
        CPU_ZERO(&actual);

        if (sched_getaffinity(0, sizeof(cpu_set_t), &actual) != 0)
        {
            throw std::runtime_error(
                std::string("verify_affinity: sched_getaffinity() failed: ") +
                std::strerror(errno));
        }

        const int actual_count = CPU_COUNT(&actual);
        const bool matches_exactly =
            (actual_count == 1) && CPU_ISSET(static_cast<size_t>(expected_core_id), &actual);

        if (!matches_exactly)
        {
            std::ostringstream msg;
            msg << "verify_affinity: expected thread to be pinned to exactly "
                   "core "
                << expected_core_id
                << ", but its actual affinity mask is "
                << detail::describe_mask(actual)
                << " (" << actual_count << " core(s) set). "
                << "Check that pin_to_core(" << expected_core_id
                << ") was called on this thread before verify_affinity(), and "
                   "that core "
                << expected_core_id
                << " actually exists and is online on this machine (see the "
                   "isolcpus= GRUB parameter in config.hpp's "
                   "ENVIRONMENT_REQUIREMENTS block).";
            throw std::runtime_error(msg.str());
        }
    }

} // namespace hydra