// Lane 232: the replay-thumbnail job's hook (sub_8274FA70) and the present-time delivery. See renderer_adapter\thumb_defer_core.h for the design and docs\systems\gpu_readback.md "Deferred delivery".
//
// The job is a method-table entry (0x821F1EE0) queued with sub_82C15888 and run on a worker; this hook sits in front of it. Deferred (--wwe2k14_rdr_readback_defer, owner mode): it returns at once and
// leaves the ready word obj+44 at 4, which the file writer (sub_8274FC60, `+44 == 0` required) reads as "the job is still running" [C listing]. At each present the game's thread runs the job itself for every
// deferral whose picture has landed (reader.h ThumbCollect wrote the bytes into the texture's memory first); the job then copies the rows into obj+84 and sets obj+44 = 0 exactly as it always does.
#pragma once

#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>

#include "renderer_adapter/reader.h"

REX_EXTERN(__imp__sub_8274FA70);
REX_EXTERN(__imp__sub_8274FC60);
REX_EXTERN(__imp__sub_82750580);

REX_HOOK_RAW(sub_8274FA70) {
  if (REXCVAR_GET(wwe2k14_rdr_capture) && wwe2k14::rdr_reader::OnThumbJobEnter(ctx, base)) return;  // deferred: the present runs it when its picture is here
  __imp__sub_8274FA70(ctx, base);
}

namespace wwe2k14::thumb_defer {
// Runs the job for `runs` on this thread (the caller's registers are saved and restored, the FP control bits too).
inline void RunJobs(PPCContext& ctx, uint8_t* base, const std::vector<wwe2k14::rdr_reader::ThumbRun>& runs) {
  const PPCContext saved = ctx;
  for (const wwe2k14::rdr_reader::ThumbRun& r : runs) {
    ctx = saved;
    ctx.r3.u64 = r.obj;
    wwe2k14::rdr_reader::t_thumb_replay = r.replay;     // replay: the job's lock reads the texture's memory, which now holds the picture (no read-back)
    wwe2k14::rdr_reader::t_thumb_nodefer = true;        // a blocking run must not defer itself again
    const auto tj = std::chrono::steady_clock::now();
    __imp__sub_8274FA70(ctx, base);
    const double job_ms = rdr_adapter::MsSince(tj);
    wwe2k14::rdr_reader::g_thumb_times.Update(r.obj, [&](rdr_adapter::ThumbTimes& t) { t.rerun_ms = job_ms; });
    wwe2k14::rdr_reader::t_thumb_replay = false;
    wwe2k14::rdr_reader::t_thumb_nodefer = false;
    ctx.fpscr.restoreGuestBits(saved.fpscr.csr);
  }
  ctx = saved;
}
// Called from the present hook (the game's thread; a present on another thread, e.g. the D3D worker, delivers nothing: the board filters by ctx.r13).
inline void OnPresent(PPCContext& ctx, uint8_t* base) {
  static thread_local std::vector<wwe2k14::rdr_reader::ThumbRun> run;
  wwe2k14::rdr_reader::ThumbCollect(base, ctx.r13.u64, run);
  if (!run.empty()) RunJobs(ctx, base, run);
}
}  // namespace wwe2k14::thumb_defer

// S2: the file writer. When it is entered for an object whose job is still deferred (+44 == 4), that job runs first so the thumbnail is saved, not skipped.
REX_HOOK_RAW(sub_8274FC60) {
  if (REXCVAR_GET(wwe2k14_rdr_capture) && REXCVAR_GET(wwe2k14_rdr_readback_defer)) {
    static thread_local std::vector<wwe2k14::rdr_reader::ThumbRun> run;
    wwe2k14::rdr_reader::ThumbBeforeWrite(base, ctx.r3.u32, run);
    if (!run.empty()) wwe2k14::thumb_defer::RunJobs(ctx, base, run);
    const uint32_t obj = ctx.r3.u32;
    const auto tw = std::chrono::steady_clock::now();
    __imp__sub_8274FC60(ctx, base);
    wwe2k14::rdr_reader::ThumbTimesPrint(obj, rdr_adapter::MsSince(tw));  // lane 278: the job's line, once (only an object that went through the deferral has a record)
    return;
  }
  __imp__sub_8274FC60(ctx, base);
}

// B1: the object's destructor frees its buffer and texture without touching +44 / +52: its deferral is dropped first.
REX_HOOK_RAW(sub_82750580) {
  if (REXCVAR_GET(wwe2k14_rdr_capture) && REXCVAR_GET(wwe2k14_rdr_readback_defer)) wwe2k14::rdr_reader::OnThumbObjectDestroyed(ctx.r3.u32);
  __imp__sub_82750580(ctx, base);
}
