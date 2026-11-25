// wwe2k14 - `--wwe2k14_launch=@cas`: the Create a Superstar editor for a NEW superstar, opened from the idle root main menu with no menu input
// (S110 CASENTRY, docs\research\s110\CASENTRY.md; the menu tree it rests on: docs\research\s110\MENUTREE.md section 5).
// S110 EXITMAP: the launch itself is now the "cas" row of src/mode_table.h, sent by src/mode_launch.h Step (from Tick below) with the same idle
// test and refusals (src/launch_core.h RunCheck), plus the SmartMenu's enter gates (Gates, EXITMAP review); its "exit 8 sent at frame F" and
// "sequence 76 is current" lines are unchanged, its first line and refusal wording are mode_launch's. This header keeps the flow dump and go-to.
//
// The game's own way in: CREATIONS > SUPERSTAR > NEW (screen 1502) -> screen controller mode 9 sub_8286A0A8 runs a 60-byte save task (update
// sub_8291F0E8: storage, space and slot checks with their message boxes) and, when it reports success, calls sub_8283CFF0(SM, 8); the main menu's
// dispatcher case 8 builds the editor sequence sub_833979B8(seq, 0) (sequence 76, param 0 = NEW). We make that same exit request at the idle main
// menu, as the @quick launch requests exit 1 (src/scenario.h FireLaunch) and the 30 Years start exit 3 (src/wm30_boot.h Exit). Skipped: only the
// save task, which writes nothing the editor reads (its own object, its own save manager, and a device-space query posted on the save worker).
// Its refusals are made here instead, with the game's own tests (src/launch_core.h RunCheck): no storage ([P+8] == 0) and no free superstar slot
// (sub_827DA810 == -1); without the second the editor's SAVE would write over created superstar 0. Not made: the space check (a device query,
// op 63). TEST / TOOLING PATH: reached only by the switch; the editor's own SAVE stays a press.
//
// --wwe2k14_cas_flow_dump=PATH (dev, default empty = off): in sequence 76, once the editor's menu object M = [[0x839BFC7C]+4588] has adopted the
// flow table of holder slot 1 (M+16 == 2, data at M+1376: sub_833A0360), writes the raw 76-byte records ([data+8], count [data+4]: the reader
// sub_83424C88) to PATH, once per run, whichever way the editor was entered. Read it with `py -I tools\menus\flow_graph.py --records PATH --text`.
//
// Game thread only: called from the owner update (src/scenario.h, g_cas_tick) before the game's own update of the frame, like the @quick launcher.
#pragma once

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

#include "guest_call.h"  // Caller
#include "scenario.h"    // U32, Guest, Frame, SequenceId, the main-menu constants, g_cas_tick, wwe2k14_launch(_idle_frames), sub_8283CFF0
#include "cas_goto.h"    // S110 CASNAV: --wwe2k14_cas_goto=<record> (the editor go-to; its Step runs from Tick below)
#include "cas_pick.h"    // S110 CASPICK: wwe2k14_cas_pick=<item> (the item pick on an item page's parts panel; its Step runs from Tick below)
#include "cas_row.h"     // S110 CASROW: wwe2k14_cas_row=<label> / back / yes / no / ok (the rows of the editor's edit pages; its Step runs from Tick below)
#include "mode_launch.h"  // S110 EXITMAP: --wwe2k14_launch=@cas / @mode:<name> (its Step runs from Tick below)

REXCVAR_DECLARE(std::string, wwe2k14_cas_flow_dump);

// sub_827DA810 (the game's free superstar slot) is declared in src/launch_core.h (S110 EXITMAP: the @cas refusal moved there).

namespace wwe2k14::cas_launch {

constexpr uint32_t kSeqCas = 76;              // sub_833979B8 +20: sequence_base_ctor(seq, 76, ...)
constexpr uint32_t kCawRootPtr = 0x839BFC7C;  // the CAW root; M = [root+4588] (sub_83390AF0 +1A8)
constexpr uint32_t kRootMenu = 4588, kMState = 16, kMFlow = 1376;  // M+16 == 2: slot 1 adopted, data at M+1376 (sub_833A0360 +48 / +54)
constexpr uint32_t kFlowCount = 4, kFlowRecords = 8, kRecordSize = 76;  // sub_83424C88 reads [data+4] / [data+8]
constexpr uint32_t kMaxRecords = 4096;

// Game thread only (the owner update), so no locking.
inline uint64_t g_last_frame = 0;
inline bool g_dumped = false;

inline bool Plausible(uint32_t p) { return p >= 0x10000 && p < 0xC0000000u; }
inline uint32_t Rd(uint32_t a) { return Plausible(a) ? scenario::U32(a) : 0; }

// S110 EXITMAP: the launch that was LaunchStep here is the "cas" row of src/mode_table.h, sent by src/mode_launch.h Step (called from Tick below)
// with the same idle test and the same two refusals (src/launch_core.h RunCheck, Check::kStorageSuperstarSlot: the save task's state 1 and
// state 5, the CASENTRY review's reason), after the enter gates of the records CREATIONS > SUPERSTAR > NEW (src/launch_core.h Gates).

inline void DumpStep(uint64_t frame) {
  if (g_dumped || scenario::SequenceId() != kSeqCas) return;
  const std::string path = REXCVAR_GET(wwe2k14_cas_flow_dump);
  if (path.empty()) return;
  const uint32_t root = Rd(kCawRootPtr);
  const uint32_t m = root ? Rd(root + kRootMenu) : 0;
  if (!m || Rd(m + kMState) != 2) return;  // the editor's menu object has not adopted slot 1 yet
  g_dumped = true;                         // one attempt per run, whatever it finds
  const uint32_t data = Rd(m + kMFlow);
  const uint32_t count = data ? Rd(data + kFlowCount) : 0, recs = data ? Rd(data + kFlowRecords) : 0;
  if (count == 0 || count > kMaxRecords || !Plausible(recs) || !Plausible(recs + count * kRecordSize)) {
    REXLOG_WARN("[wwe2k14][launch] cas flow dump: no usable table (M {:08X} data {:08X} count {} records {:08X}); nothing written", m, data, count,
                recs);
    return;
  }
  std::error_code ec;
  const std::filesystem::path p(path);
  if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) {
    REXLOG_WARN("[wwe2k14][launch] cas flow dump: cannot open {} for writing", path);
    return;
  }
  const size_t want = size_t(count) * kRecordSize;
  const size_t wrote = std::fwrite(scenario::Guest(recs), 1, want, f);
  std::fclose(f);
  REXLOG_INFO("[wwe2k14][launch] cas flow dump: {} records of holder slot 1 (data {:08X}, records {:08X}) -> {} ({} of {} bytes) frame={}", count,
              data, recs, path, wrote, want, frame);
}

// Called from the owner update (src/scenario.h); once per guest frame. Off: the sequence id (three reads) every frame for the dump switch, and
// one string read per frame until a launch name is seen; nothing is written while both switches are empty.
inline void Tick(PPCContext& ctx, uint8_t* base) {
  const uint64_t frame = scenario::Frame();
  if (frame == g_last_frame) return;
  g_last_frame = frame;
  mode_launch::Step(ctx, base, frame);  // S110 EXITMAP: @cas / @mode:<name> (src/mode_launch.h); off = one string read, then nothing
  DumpStep(frame);
  cas_goto::Step(ctx, base, frame);  // S110 CASNAV / CASARRIVE: off = one locked string read, then one atomic load a frame (src/cas_goto.h)
  cas_pick::Step(ctx, base, frame);  // S110 CASPICK: after the go-to, in the same frame; off = one atomic load a frame (src/cas_pick.h)
  cas_row::Step(ctx, base, frame);   // S110 CASROW: after the pick; off = one atomic load a frame. One driver per request (src/cas_driver.h)
}

inline const bool g_registered = (scenario::g_cas_tick = &Tick, true);

}  // namespace wwe2k14::cas_launch
