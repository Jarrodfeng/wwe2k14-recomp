// S102 lane P4P5 (docs\renderer\OWN_SHADOW_RIM.md): the node switch on the shadow module's and the rim map's draw nodes, game side. COMPILED AND REVIEWED, NOT RUN (0 game runs in this lane).
//
// Two hooks, one per draw node's slot-1 method (R4 1.2; nothing else hooks them: grep of src\ and config\):
//   sub_827E9740  the shadow module's render entry (vtable word 0x82033BA4). The game's own off tests are its first two: the word at offset 22692 of r3 (node = module + 4; read as module + 22696) equal to
//                 zero, or the word at offset 284 of r3 (module + 288) equal to 2 -> return. What those two words MEAN is [U]: db shadows.json and docs\systems\shadows.md call them "circle object" and "module
//                 mode" but they are read relative to the node, not the module (the module's own +22692 / +284 are other words). The hook repeats the tests exactly as offsets of r3 and gives them no name.
//                 The frame function sub_827E95D8 takes module = r3 - 4.
//   sub_827EA7B8  the rim map's render entry (vtable word 0x82033BF8). Its own tests: the word at offset 76 of r3 equal to zero, or the word at offset 84 non-zero -> return (meaning [U]); else it
//                 tail-calls sub_827EA098 with r3 - 4.
// Each hook, with its switch off, calls the original and does one table read (rdr_sw::BoolSeen). With the switch on and the game's own tests passing, ONE decision is made per (pass, frame)
// (node_switch.h Board::Once: a second walk of the node in the same frame reads the first stamp; the frame number is read once per walk). It answers Ours only when the render side is ready AND every
// registered input carries this frame's stamp. In THIS STEP the render side is never ready (the classes still take their anchors from stream draws) and no input is ever marked captured (the readers
// that move the values out of the walker are the next piece: OWN_SHADOW_RIM.md section 5), so Decide() answers NotReady / InputsMissing and the game walks. Also refused: the D3 caster skip on
// (d3::g_caster_d3), which would drop the same caster passes at another place (two cuts of one pass): one log line, the game walks.
// The Ours branches below are the first design of what the live run will exercise:
//   shadow module  does NOT simply return: the colour pass reads the render camera record R's shadow block (the matrices at offsets 912 / 976 / 1040, the floats at 1104 / 1120, the six trapezoid
//                  floats at 1140..1160; sub_82BD7B50 through the getters 0x82BCCA30 / CA78 / CAC0 / CC290 / CB08 / CBE0 ... CD48) of ITS thread slot. The setters' only caller is sub_827E74F8 (the walker's
//                  last sub_827E7DF0 writes the A / B light), BUT R blocks are also COPIED between slots: sub_82BCD180 copies a whole block; it is reached through sub_82BCD870 (slot 0 into the master)
//                  and through sub_82BCD388 in the CParallelDraw jobs (sub_82BB4F00: the master into a slot). So which slot a colour call reads, and whose write it carries, is [U] (OWN_SHADOW_RIM.md
//                  section 2). The hook therefore runs the install pair alone: the current camera of the thread slot is read, sub_827E7DF0(module, module + 64, module + 304) installs the A / B light and the
//                  camera is put back, the calls the walker makes at 0x827E9620 / 0x827E96A0 / 0x827E96BC, minus every draw. The props light's install (the walker's first, flag 0) is overwritten by the second.
//                  THE CLOSING CHECK (the main session's one run) must log R+1140 and the slot id at the colour entry of a wrestler call and of a floor call, with the hook on and off.
//   rim map        returns: the walker saves the device states it touches and puts them back (listing of sub_827EA098: the saves from offset 0x827EA0C4 on, the restores 0x827EA484..0x827EA4E0; the db entry
//                  names 0x827EA0C4, the first prose draft said 0x827EA0B4: the loads at 0x827EA0B4 / 0x827EA0B8 / 0x827EA0BC are the camera table and the node's own words, the getters start at 0x827EA0CC);
//                  no write of R was found in the walker or in the callees named in that listing (sub_827E97A0, sub_827E99B8 and sub_82813BD0 were read for their call lists only [U]). Model-call side effects
//                  of the caster passes (the per-model constant blocks, cached command lists, the walker's op 45 / 46 / 48 / 49 / 58 records) were NOT audited [U].
// Switches (read at the point of use every frame; default off): wwe2k14_rdr_own_shadow_module, wwe2k14_rdr_own_rim_map (rdr_switches.h, PerFrame).
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/xmemory.h>

#include "../frame_stats.h"  // g_guest_frame
#include "../renderer_adapter/d3_arena.h"  // d3::g_caster_d3 (the refusal below)
#include "../renderer_adapter/own_group.h"      // Refusal()
#include "../renderer_adapter/rdr_switches.h"
#include "../renderer_game/own_shadow_rim.h"

REXCVAR_DEFINE_BOOL(wwe2k14_rdr_own_shadow_module, false, "wwe2k14",
                    "Session 102 lane P4: the shadow module (props map, maps A / B, coverage reductions, min blurs, circle blobs) owned at the draw node: when on, and the render side is ready, and every input "
                    "the pass needs is captured for this frame, the game does not walk the node (its R camera install still runs). Until the inputs are captured the game walks it. Read every frame. Off by default");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_own_rim_map, false, "wwe2k14",
                    "Session 102 lane P5: the rim light map (its caster pass, two reductions, blur) owned at the draw node: when on, the render side is ready and every input is captured for this frame, the "
                    "game does not walk the node. Until the inputs are captured the game walks it. Read every frame. Off by default");

REX_EXTERN(__imp__sub_827E9740);
REX_EXTERN(__imp__sub_827EA7B8);
REX_EXTERN(sub_827E7DF0);   // shadow_install_light_camera(module, light descriptor, light record): R's shadow block + the thread slot's current camera
REX_EXTERN(sub_82BA7748);   // the thread slot's current camera (r3 = the table object)
REX_EXTERN(sub_82BA7DE0);   // set the thread slot's current camera (r3 = the table object, r4 = camera)

namespace wwe2k14::own_node {

inline constexpr uint32_t kCameraTablePtr = 0x83929A68u;   // the word that holds the camera table object: 0x83930000 - 26008, loaded at 0x827E9614

inline uint32_t Rd(uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, rex::memory::GuestPtr(base, a), 4);
  return __builtin_bswap32(v);
}

// The D3 caster skip drops the same passes at another place: refused (one line per run), the game walks. own_group::Refusal is the text the offline tools print too.
inline bool Refused(bool shadow, bool rim) {
  static std::atomic<bool> said{false};
  const char* why = rdr_adapter::own_group::Refusal(rdr_adapter::d3::g_caster_d3, shadow, rim);
  if (!why) return false;
  if (!said.exchange(true)) REXLOG_ERROR("[own] {}", why);
  return true;
}

inline uint64_t Frame() { return wwe2k14::g_guest_frame.load(std::memory_order_relaxed); }

// The Ours branch of the shadow module: the install pair alone (no draw, no target, no state change but R and the thread slot's camera, which the walker also leaves as it found it).
inline void KeepShadowInstall(PPCContext& ctx, uint8_t* base, uint32_t module) {
  const PPCContext saved = ctx;
  const uint32_t table = Rd(base, kCameraTablePtr);
  ctx.r3.u64 = table;
  sub_82BA7748(ctx, base);
  const uint32_t previous = ctx.r3.u32;
  ctx.r3.u64 = module;
  ctx.r4.u64 = module + 64;
  ctx.r5.u64 = module + 304;
  sub_827E7DF0(ctx, base);
  ctx.r3.u64 = table;
  ctx.r4.u64 = previous;
  sub_82BA7DE0(ctx, base);
  ctx = saved;
}

inline void MaybeLog(const char* what, const rdr_own::OwnedPassStamp& st) {
  const uint64_t n = rdr_own::S(rdr_own::Pass(st.pass)).ours.load(std::memory_order_relaxed) + rdr_own::S(rdr_own::Pass(st.pass)).walks.load(std::memory_order_relaxed);
  if (n % 600 != 1) return;
  const rdr_own::PassState& s = rdr_own::S(rdr_own::Pass(st.pass));
  REXLOG_INFO("[own][{}] frame {}: ours {}, the game walks {} (why not now: {}), inputs mask {:#x}, misses {}, holes {}, doubles {}", what, (unsigned long long)st.guest_frame,
              (unsigned long long)s.ours.load(), (unsigned long long)s.walks.load(), int(st.why_not), st.inputs_mask, (unsigned long long)s.misses.load(), (unsigned long long)s.holes.load(),
              (unsigned long long)s.doubles.load());
}

}  // namespace wwe2k14::own_node

REX_HOOK_RAW(sub_827E9740) {
  using namespace wwe2k14::own_node;
  const uint64_t guest_frame = Frame();
  if (!rdr_sw::BoolSeen(rdr_sw::kOwnShadowModule, REXCVAR_GET(wwe2k14_rdr_own_shadow_module), guest_frame)) {
    __imp__sub_827E9740(ctx, base);
    return;
  }
  const uint32_t node = ctx.r3.u32;   // saved: the original reuses r3
  // the game's own off switch first (the original's first two tests): the node does not walk, so there is nothing to decide
  if (Rd(base, node + 22692) == 0 || Rd(base, node + 284) == 2) {
    __imp__sub_827E9740(ctx, base);
    return;
  }
  if (Refused(true, false)) {
    __imp__sub_827E9740(ctx, base);
    return;
  }
  rdr_own::shadow_rim::RegisterInputs();
  const rdr_own::OwnedPassStamp st = rdr_own::DecideNode(rdr_own::Pass::ShadowModule, guest_frame, true);
  MaybeLog("shadow_module", st);
  if (st.decision != uint8_t(rdr_own::Decision::Ours)) {
    __imp__sub_827E9740(ctx, base);
    return;
  }
  KeepShadowInstall(ctx, base, node - 4);
}

REX_HOOK_RAW(sub_827EA7B8) {
  using namespace wwe2k14::own_node;
  const uint64_t guest_frame = Frame();
  if (!rdr_sw::BoolSeen(rdr_sw::kOwnRimMap, REXCVAR_GET(wwe2k14_rdr_own_rim_map), guest_frame)) {
    __imp__sub_827EA7B8(ctx, base);
    return;
  }
  const uint32_t node = ctx.r3.u32;
  if (Rd(base, node + 76) == 0 || Rd(base, node + 84) != 0) {   // the game's own off switch (the original's first two tests)
    __imp__sub_827EA7B8(ctx, base);
    return;
  }
  if (Refused(false, true)) {
    __imp__sub_827EA7B8(ctx, base);
    return;
  }
  rdr_own::shadow_rim::RegisterInputs();
  const rdr_own::OwnedPassStamp st = rdr_own::DecideNode(rdr_own::Pass::Rim, guest_frame, true);
  MaybeLog("rim_map", st);
  if (st.decision != uint8_t(rdr_own::Decision::Ours)) __imp__sub_827EA7B8(ctx, base);
  // Ours: the node does not walk. Nothing else to run (see the header).
}
