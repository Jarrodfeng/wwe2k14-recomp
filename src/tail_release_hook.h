// wwe2k14 - S102 U1: the hook that runs our present work after the game releases its update thread (docs\renderer\SWAP_TAIL.md).
// Switch --wwe2k14_rdr_tail_release (PerFrame, default off; reader.h TailDecide). Included once, from wwe2k14_app.h.
//
// sub_82C3BB38 (db frame_pacing.json frame_sync_release_clients, medium): the render-command thread's release of every barrier client (the update thread
// CGameMainThread, CSkinModelUtil, CRenderParticleManager, CParallelDraw, OnlineUpdateOtherThread). One caller: sub_82BB5CC0 at 0x82BB5DE4
// (tools\q callers 0x82C3BB38; the data word at 0x82211FB8 is its function-table entry). The original runs first and unchanged, so the clients are woken
// exactly where they always were; then a tail the present latched (reader.h TailLatch) is finished here, on the same thread, while the update thread runs.
// With the switch off nothing is pending: OnRelease (two atomic stores) and one atomic exchange.
#pragma once

#include <rex/hook.h>
#include <rex/ppc/context.h>

#include "keep_stream.h"  // S103 P9: WWE2K14_KEEP_STREAM
#if WWE2K14_STREAM()  // S103 P9 (A16)
#include "renderer_adapter/reader.h"
#endif
#include "renderer_adapter/tail_release.h"
#include "renderer_game/frame_tick.h"   // S103 P6

REX_EXTERN(__imp__sub_82C3BB38);
REX_HOOK_RAW(sub_82C3BB38) {
  rdr_adapter::tail::The().OnRelease();   // the window closes: a latch from here on is late (counted, never deferred)
  __imp__sub_82C3BB38(ctx, base);
  wwe2k14::frame_tick::FinishTailAtRelease();   // S103 P6: the finish the latch registered (reader.h TailFinishAt today), not a reader call
}
