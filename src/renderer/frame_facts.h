// S102 R21-A: facts the render thread re-derived every frame whose answer does not change, derived once (docs\renderer\FRAME_FACTS.md).
// The switch (wwe2k14_rdr_frame_facts, PerFrame, default off), its census counters and the planted faults of the offline proof. No Vulkan, no game, no SDK.
//   1. the scene record's arena ids are classified by a byte test + digit parse (record_ids.h), not a std::string + sscanf per object;
//   2. Binder::BlendRanges does nothing while no consumer of the tween / DLSS keys is on;
//   3. thin::ComputeKeys counts into a flat table that keeps its storage across frames.
// Off = the code as it was.
#pragma once
#include <atomic>
#include <cstdint>

namespace rdr {

// --wwe2k14_rdr_frame_facts (default OFF, PerFrame): set once per frame by live.cpp; rplay / fview: --frame-facts on|off.
inline std::atomic<bool> g_frame_facts{false};
// Tests only (fview / rplay --frame-facts-plant N), 0 = off: 1 the arena prefix test accepts "arena:slo" (a non-arena object becomes a slot), 2 the ":r" twin tail is ignored,
// 3 BlendRanges is skipped even while a tween / DLSS consumer is on.
inline std::atomic<int> g_frame_facts_plant{0};
// Census (relaxed counters in fixed slots: nothing is built per draw; the line is made every 600 frames).
inline std::atomic<uint64_t> g_ff_records{0}, g_ff_objects{0}, g_ff_arena{0}, g_ff_blend_skipped{0}, g_ff_blend_kept{0}, g_ff_keys_flat{0}, g_ff_same{0};

// Tool only (--frame-facts-verify): SetRecord also runs the old classification and counts the records whose sets or worlds differ.
inline std::atomic<bool> g_ff_verify{false};
inline std::atomic<uint64_t> g_ff_verified{0}, g_ff_verify_bad{0};

inline bool FrameFactsOn() { return g_frame_facts.load(std::memory_order_relaxed); }

}  // namespace rdr
