#pragma once

// Affinity ablation: no-op pinning, threads keep the inherited OS affinity mask.

namespace hydra
{

    inline void pin_to_core(int /*core_id*/)
    {
    }

    inline void verify_affinity(int /*expected_core_id*/)
    {
    }

} // namespace hydra
