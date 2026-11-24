// The wrestlers' liquid maps: the game plumbing (Session 103, lane SWF; docs\systems\liquid_map.md). Included by scene_skip_nodes.h, which owns the one REX_HOOK_RAW on sub_8279ACD8 (the map's draw node: it is already
// hooked there for the skip). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Wraps the pure tracker (liquid_hook.h) with the guest reads of wrestler_hook.h (ReadGuest / ReadWord: bounds- and fault-checked) and the
// physical-memory read of the pixel bytes (the same three checks as inputs_glue.h CrowdGuestMem::phys: our range check, the page table, the fault-safe copy). Registered with the scene records (wwe2k14_app.h OnPostSetup,
// --wwe2k14_rdr_liquid_map, default on); until then one relaxed load.
#pragma once
#include <rex/logging.h>

#include <atomic>

#include "guest_texture.h"
#include "liquid_contrib.h"
#include "wrestler_hook.h"   // ReadGuest / ReadWord / GuestMem / PhysRangeReadable / SafeCopy

namespace wwe2k14::liquid_glue {

inline bool ReadPhys(uint32_t addr, uint32_t n, uint8_t* dst) {
  if (!n || n > (1u << 20) || !rdr_scene::live::guest_tex::PhysRangeOk(addr, n)) return false;   // A8: the range check is ours
  const auto* host = wwe2k14::wrestlers::GuestMem()->TranslatePhysical<const uint8_t*>(addr);
  return host && wwe2k14::wrestlers::PhysRangeReadable(addr, n) && wwe2k14::wrestlers::SafeCopy(dst, host, n);
}

// Called at the ENTRY of sub_8279ACD8 (before the skip test): map = r3, frame = the calling thread's hook frame. Returns true when a row was recorded (the node would have had something to do: a live item or a
// pending clear request). Read only; the caller consumes the clear request when it skips the node.
#if defined(_MSC_VER) || defined(__clang__) || defined(__GNUC__)
__attribute__((noinline))
#endif
inline uint32_t OnNode(uint32_t map, uint64_t frame, rdr_scene::live::liquid::AltConsume* consume = nullptr) {   // bit 0 = a row recorded, bit 1 = it carried the clear request (the caller consumes only then); S108 BLD2: bit 2 = it carried the game's rebuild, whose write-back `consume` receives
  if (!rdr_scene::live::liquid::LiquidRegistered() || !rdr_scene::live::Enabled()) return 0;
  const uint32_t rec = rdr_scene::live::liquid::Liquid().tracker.OnNode(
      map, frame, [](uint32_t a, uint32_t& v) { return wwe2k14::wrestlers::ReadWord(a, v); },
      [](uint32_t a, void* dst, size_t n) { return wwe2k14::wrestlers::ReadGuest(a, dst, n); }, &ReadPhys, consume);
  if (!rec) return 0;
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1, std::memory_order_relaxed) < 8) {
    uint32_t ch = 0, skel = 0, live = 0, head = 0;
    wwe2k14::wrestlers::ReadWord(map + rdr_scene::live::liquid::kMapChar, ch);
    if (ch) wwe2k14::wrestlers::ReadWord(ch + rdr_scene::live::liquid::kCharSkeleton, skel);
    wwe2k14::wrestlers::ReadWord(map + rdr_scene::live::liquid::kMapLiveCount, live);
    wwe2k14::wrestlers::ReadWord(map + rdr_scene::live::liquid::kMapLiveHead, head);
    REXLOG_INFO("[wwe2k14][liquid] frame {}: map 0x{:08X} character 0x{:08X} skeleton 0x{:08X} live items {} head 0x{:08X}", frame, map, ch, skel, live, head);
  }
  return rec;
}

}  // namespace wwe2k14::liquid_glue
