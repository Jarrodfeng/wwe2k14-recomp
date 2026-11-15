// S102 T6: the members-instanced switch (wwe2k14_rdr_members_instanced, per frame; rdr_switches.h MembersInstanced). crowd_ours.cpp reads it through rdr_sw::BoolSeen with this as the fallback
// (fview --members-instanced / rplay --members-instanced set it; the game's cvar refresh overrides it once the table is live). No includes beyond <atomic>.
#pragma once
#include <atomic>

namespace rdr_adapter {
inline std::atomic<bool> g_members_instanced{false};
inline std::atomic<int> g_crowd_workers{0};         // S102 R21-C step 3: wwe2k14_rdr_crowd_workers (0-2), the fallback of the per-frame read in crowd_ours.cpp (fview / rplay --crowd-workers N)
inline std::atomic<bool> g_crowd_workers_unjoined{false};  // S102 R21-C TEST ONLY (twin workers_unjoined): the pool is not joined before the pose is used
inline std::atomic<bool> g_members_perturb{false};   // test only (fview / rplay --members-perturb): in Phase A every second member of each class gets its fragment-block float constants x 1.01, in BOTH arms, so classes are non-uniform and the split path runs
}  // namespace rdr_adapter
