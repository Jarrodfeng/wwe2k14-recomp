// The crowd promotion's speed half, game side (Session 101, lane 50; docs\renderer\CROWD_PROMOTION_MAP.md sections 3-4; the readers of the skinned
// ring: docs\systems\crowd.md "Who reads the skinned ring"). With ours drawing the near crowd (--wwe2k14_rdr_crowd_ours), stop the game doing
// work ours replaced. Each piece has its own switch, default off, and acts only while crowd_skip_core.h's gate says ours drew the recent
// frames with nothing kept; otherwise (and on any doubt) the game's work stays.
//
//  P2c  --wwe2k14_rdr_crowd_skip_skin (the cvar is declared in skin_skip_hook.h): the four CPU skin loops of the crowd manager update
//       (sub_8250BAF0 -> 0x825085F8 / 0x82508738 / 0x82508958 / 0x82508C08; r3 = the manager, r4 = the model, r5 = the palette matrices, r6 = the
//       destination vertex buffer) do not run for a group nothing reads. The loops already have hooks (native_fn.h Dispatch for the two native ones,
//       crowd_skin_report.h for the other two); they ask skin_skip_hook.h, and the decision installed here answers. sub_82509CB8 (the palette copy ours
//       reads) is NOT touched. Readers of the skinned ring: the block draw sub_82518340 (ours replaces it), the sign / floor people (sub_82518950 /
//       sub_82518EB8 read arrays at other words, not the loops' family), and sub_8276EAA0 through the getter sub_82512E80 (hooked below: a group it asked
//       for in the last 2 s is still skinned). Census: one line every 10 s while the switch is on (CrowdSkinCensus).
//  P2b  --wwe2k14_rdr_crowd_drop_draws: REFUSED. The decision code (DrawDecider) and the hook in scene_crowd.h's op-15 recorder stay, but kP2bEnabled is
//       false: CrowdOurs::Inject takes each member's material and the kept set from the frame's own stream crowd draws, so it needs a record-driven Inject
//       and a dropped-frame flag first (docs\systems\crowd.md "P2b is refused").
// Off: one cvar read per call. Not for a determinism run once P2b exists: dropping records changes the command stream (the det hash).
#pragma once

#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include "crowd_hooks.h"  // wwe2k14::crowd_live::Be32
#include "crowd_skip_core.h"
#include "skin_skip_hook.h"

REXCVAR_DEFINE_BOOL(wwe2k14_rdr_crowd_drop_draws, false, "wwe2k14",
                    "Session 101 lane 50 (P2b of the crowd's promotion): REFUSED, the switch does nothing (logs once): it needs a record-driven crowd Inject first "
                    "(docs/systems/crowd.md 'P2b is refused')");

REX_EXTERN(__imp__sub_82512E80);

namespace wwe2k14::crowd_skip {

inline constexpr bool kP2bEnabled = false;  // see the header comment

// The op-15 recorder hook (scene_crowd.h): true = do not record this draw. lr = the recorder's return address.
inline bool DropDraw(uint32_t lr) {
  const bool block = lr == 0x82518578u, card = lr == 0x825130C8u;  // listing: bl 0x82BB10C0 at 0x82518574 / 0x825130C4 (scene_crowd.h kOp15Return / kCardOp15Return)
  if (!block && !card) return false;
  if (!REXCVAR_GET(wwe2k14_rdr_crowd_drop_draws)) return false;
  if constexpr (!kP2bEnabled) {
    static bool said = false;
    if (!said) {
      said = true;
      REXLOG_WARN("[rdr][crowd] P2b disabled: --wwe2k14_rdr_crowd_drop_draws needs the record-driven inject, see docs/systems/crowd.md");
    }
    return false;
  }
  return t_draws.Drop(block, card, true, false, NowMs());
}

// The census line (P2c): frames proven, skin loops skipped / total, why the proof was voided. Once per 10 s while the skin switch is on.
inline void MaybeCensus(int64_t now_ms) {
  static int64_t last = 0;
  if (last == 0) last = now_ms;
  if (now_ms - last < 10000) return;
  last = now_ms;
  const Gate& g = g_gate;
  REXLOG_INFO("[rdr][crowd] skin skip: frames proven {}, skin loops skipped {} of {} (unknown group {}), proof voided: ours did not draw {}, stream draw kept {}, binder silent {}",
              g.proven_frames.load(), g_loops_skipped.load(), g_loops_total.load(), g_loops_unknown_group.load(), g.void_not_drawn.load(), g.void_kept.load(),
              g.void_stale.load());
}

// The decision the loops' hooks ask (skin_skip_hook.h SkinLoopSkipped tests the cvar before calling this): true = skip.
inline bool SkipSkinLoop(PPCContext& ctx, uint8_t* base) {
  const int64_t now = NowMs();
  MaybeCensus(now);
  auto read32 = [base](uint32_t a) { return wwe2k14::crowd_live::Be32(base, a); };
  return SkipSkin(read32, ctx.r3.u32, ctx.r6.u32, true, now);
}

// The seam (skin_skip_hook.h) is installed at start-up.
inline const bool g_installed = (g_skip_skin_loop = &SkipSkinLoop, true);

}  // namespace wwe2k14::crowd_skip

// The getter sub_82512E80 (r3 = the manager, r4 = the group; only caller sub_8276EAA0): notes that someone reads the group's skinned buffer.
REX_HOOK_RAW(sub_82512E80) {
  if (REXCVAR_GET(wwe2k14_rdr_crowd_skip_skin)) wwe2k14::crowd_skip::g_reads.NoteRead(ctx.r4.u32, wwe2k14::crowd_skip::NowMs());
  __imp__sub_82512E80(ctx, base);
}
