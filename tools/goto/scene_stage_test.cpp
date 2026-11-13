// S101 lane 38: drives the shipped stage logic (src/scene_stage_logic.h, the header src/scene_director.h includes) from stdin lines. Built and run by
// tools/goto/stage_selftest.py (clang++ -std=c++17 -I src tools/goto/scene_stage_test.cpp). No game, no SDK.
//   "ready held seq r g_mode cmd_obj cmd_vtable read write slot0 listener pending m participants ui_manager ui_gate"  -> "READY" | "NOT <reason>"
//   "story bank arena n c0 a0 c1 a1 ..."  -> "OK <hex of the 25360-byte story>" | "REFUSED"  (the buffer starts zeroed, as the director's does)
//   "holds BANK" -> "HOLD" | "ENDS" (S101 lane 59: does the bank get the story command class)
//   "consts" -> "name=value" lines (the offsets the push relies on), for the gate to pin against the design
#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "scene_stage_logic.h"

int main() {
  using namespace wwe2k14::scene_stage;
  std::string line;
  while (std::getline(std::cin, line)) {
    std::istringstream in(line);
    std::string verb;
    in >> verb;
    if (verb == "ready") {
      uint32_t held;
      StageWords w;
      in >> held >> w.seq >> w.r >> w.g_mode >> w.cmd_obj >> w.cmd_vtable >> w.ring_read >> w.ring_write >> w.slot0 >> w.listener >> w.pending >> w.story_root >>
          w.participants >> w.ui_manager >> w.ui_gate;
      const std::string why = NotReady(w, held);
      std::cout << (why.empty() ? std::string("READY") : "NOT " + why) << "\n";
    } else if (verb == "story" || verb == "storyd") {  // storyd: the buffer starts with the game's performer defaults (+4, +5 = 255) in all 6 entries
      uint32_t bank, arena;
      int n;
      in >> bank >> arena >> n;
      int32_t c[16] = {0}, a[16] = {0};
      for (int i = 0; i < n && i < 16; ++i) in >> c[i] >> a[i];
      std::vector<uint8_t> f(kFakeStorySize, 0);
      if (verb == "storyd") {
        for (uint32_t j = 0; j < kMaxPerformers; ++j) f[kBodyOffset + kBodyPerformers + kPerformerSize * j + 4] = f[kBodyOffset + kBodyPerformers + kPerformerSize * j + 5] = 255;
      }
      if (!FillStory(f.data(), bank, arena, n, c, a)) {
        std::cout << "REFUSED\n";
        continue;
      }
      std::string hex;
      char b[3];
      for (uint8_t x : f) {
        std::snprintf(b, sizeof b, "%02x", x);
        hex += b;
      }
      std::cout << "OK " << hex << "\n";
    } else if (verb == "holds") {  // S101 lane 59: "holds BANK" -> HOLD | ENDS
      uint32_t bank;
      in >> bank;
      std::cout << (HoldsInStage(bank) ? "HOLD" : "ENDS") << "\n";
    } else if (verb == "arena") {  // S101 lane 72: "arena N" -> LOAD | REFUSE <text>
      uint32_t n;
      in >> n;
      std::cout << (ArenaLoadable(n) ? std::string("LOAD") : "REFUSE " + ArenaRefusal(n)) << "\n";
    } else if (verb == "arenanote") {  // "arenanote N given" -> the answer sentence
      uint32_t n, given;
      in >> n >> given;
      std::cout << ArenaNote(n, given != 0) << "\n";
    } else if (verb == "leave") {  // "leave seq r g_mode p p_vtable p_state p_owns q q_vtable q_life q_mode q_slot7 m m_state pending ours" -> OK | NOT <reason>
      LeaveWords w;
      uint32_t ours;
      in >> w.seq >> w.r >> w.g_mode >> w.p >> w.p_vtable >> w.p_state >> w.p_owns >> w.q >> w.q_vtable >> w.q_life >> w.q_mode >> w.q_slot7 >> w.m >> w.m_state >> w.pending >> ours >> w.q_child;
      w.play_pending_ours = ours != 0;
      const std::string why = LeaveRefusal(w);
      std::cout << (why.empty() ? std::string("OK") : "NOT " + why) << "\n";
    } else if (verb == "back") {  // "back listener h l m" -> CLEAN | LEFT <text>
      uint32_t l1, h, l, m;
      in >> l1 >> h >> l >> m;
      const std::string why = BackAtMenuProblem(l1, h, l, m);
      std::cout << (why.empty() ? std::string("CLEAN") : "LEFT " + why) << "\n";
    } else if (verb == "consts") {
      std::cout << "kFakeStorySize=" << kFakeStorySize << "\nkBodyOffset=" << kBodyOffset << "\nkRingRead=" << kRingRead << "\nkRingWrite=" << kRingWrite
                << "\nkCtxBase=" << kCtxBase << "\nkCtxSize=" << kCtxSize << "\nkCtxSlot0=" << kCtxSlot0 << "\nkCtxType=" << kCtxType << "\nkCtxArena=" << kCtxArena
                << "\nkPending=" << kPending << "\nkBodyValid=" << kBodyValid << "\nkStoryListener=" << kStoryListener << "\nkStoryClassObj=" << kStoryClassObj
                << "\nkStoryClassVtable=" << kStoryClassVtable << "\nkGameMode=" << kGameMode << "\nkStageLoadMgrPtr=" << kStageLoadMgrPtr << "\nkSLArena=" << kSLArena << "\nkLoadedStagePtr=" << kLoadedStagePtr << "\nkGArena=" << kGArena
                << "\nkStoryProgramVtable=" << kStoryProgramVtable << "\nkStoryObjectVtable=" << kStoryObjectVtable << "\nkQSlot7Word=" << kQSlot7Word << "\nkQCancelFunction=" << kQCancelFunction
                << "\nkProgState=" << kProgState << "\nkProgOwnsRoot=" << kProgOwnsRoot << "\nkProgQ=" << kProgQ << "\nkQLife=" << kQLife << "\nkQMode=" << kQMode << "\nkQExit=" << kQExit
                << "\nkQExitCancel=" << kQExitCancel << "\nkRootState=" << kRootState << "\nkLoaderH=" << kLoaderH << "\nkLoaderL=" << kLoaderL << "\nkIngameProgramPtr=" << kIngameProgramPtr
                << "\nkPGNext=" << kPGNext << "\n";
    } else {
      std::cout << "ERR\n";
    }
  }
  return 0;
}
