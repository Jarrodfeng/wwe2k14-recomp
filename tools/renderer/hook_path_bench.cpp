// S102 K3 review F5: what a D3 call decision costs the SWAP thread while the skips are not applied, offline (0 game runs): the hook's real helpers on a fake guest memory.
//   full  = PredictPath (the guest reads) + g_marks.Acquire() (mutex + shared_ptr) + BeginGuestCall (the ledger Begin, SwitchOff) + EndGuestCall   (what an armed-off call paid before the rework)
//   idle  = path Unreadable, no Acquire, BeginGuestCall + EndGuestCall                                                                              (what it pays now: HookIdle())
// The guest-read lambda here is a plain array read; the game's is wwe2k14::GuestWordReadableFast + Ld32 (a range check and a byte swap), so the full number is a LOWER bound.
//   clang++ -std=c++20 -O2 -I src -I src/renderer_adapter tools/renderer/hook_path_bench.cpp
#include <chrono>
#include <cstdio>
#include <vector>

#include "d3_guest_core.h"

namespace d3 = rdr_adapter::d3;

int main() {
  std::vector<uint32_t> mem(1 << 16);
  for (size_t i = 0; i < mem.size(); ++i) mem[i] = uint32_t(i * 2654435761u);
  auto rd = [&](uint32_t a, uint32_t& v) {
    v = mem[(a >> 2) & 0xFFFF];
    return true;
  };
  d3::GuestObjects objs;
  d3::PacketTable packets;
  d3::GuestTally tally;
  d3::Gate gate;
  const int kObjects = 300, kFrames = 3000;
  struct R { double ns_call; };
  auto run = [&](bool idle) {
    uint64_t sink = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (int f = 1; f <= kFrames; ++f) {
      for (int o = 0; o < kObjects; ++o) {
        d3::CallInputs in;
        in.object = 0x82000000u + uint32_t(o) * 16, in.frame = uint64_t(f), in.owner = true, in.state_readable = true, in.state_count = 0;
        in.path = idle ? d3::CachePath::Unreadable : d3::PredictPath(rd, in.object, 0x7000u);
        const std::shared_ptr<const d3::MarkSet> marks = idle ? nullptr : d3::g_marks.Acquire();
        d3::GuestCall call = d3::BeginGuestCall(objs, in, marks.get(), packets, tally, gate);
        d3::EndGuestCall(objs, call, d3::CallInfo{2, 0}, d3::Fault::None, &tally);
        sink += uint64_t(call.out.why);
      }
    }
    const double ns = double(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    std::printf("%s: %.1f ns a call (%d calls a frame: %.1f us a frame) [sink %llu]\n", idle ? "idle" : "full", ns / (double(kFrames) * kObjects), kObjects, ns / kFrames / 1e3, (unsigned long long)sink);
    return ns / (double(kFrames) * kObjects);
  };
  d3::g_arena_d3 = true;   // the class switch armed, as under K3
  skips_auto::g_enabled.store(true);
  skips_auto::g_armed_by_k3.store(true);   // Allows() is false (nothing applied): DecideCall answers SwitchOff, the ledger still learns
  const double full = run(false), idle = run(true);
  std::printf("saved %.1f ns a call (%.0f %%)\n", full - idle, full > 0 ? 100.0 * (full - idle) / full : 0.0);
  return 0;
}
