// wwe2k14 - `--wwe2k14_mode_pick=<roster id>` with `--wwe2k14_launch=@mode:moveset`: the move-set editor opened on one wrestler (any id the
// editor's own picker lists, an imported seat such as 397 included) with no menu input (S110 MODEPICK, docs\research\s110\MODEPICK.md; the
// game's words and the picker's test: src/mode_pick_core.h). TEST / TOOLING PATH: reached only by the two switches.
//
// Prepare (from src/mode_launch.h Step, after the row's own refusals, right before the exit is sent): the picker's test of the id, asked of
// the game's own functions (sub_827DA860 / sub_827DA788 / sub_822C1CF0 / sub_822BCFB8, all read-only); an id it would not list refuses the
// launch and nothing is written. Otherwise it writes ONE word, the controller's own preselect [G+16] = id (G = [0x8376AAF0] + 76048, the word
// Universe writes before its exit 22), which the controller's state 5 consumes and resets to 512. Watch (every frame, from Step) logs
// "[wwe2k14][launch] <value>: picked wrestler N (...)" once sequence 20's categories page is up on a work struct holding N.
// Log: the pick's detail rides on the launch's "exit N sent" line; "picked wrestler N (...)"; a WARN when the pick refuses, or when no editor
// on N is up 1800 frames after the exit (it keeps watching; when [G+16] still holds N it puts 512 back and says so: RestoreDue).
// Route states: mode_pick, mode_pick_id (src/mode_pick_core.h kStateNames).
// Nothing is pressed: the editor's ACCEPT (which writes N's move-set back to the save) is never sent.
// S110 ENTPICK (docs\research\s110\ENTPICK.md): with @mode:entrance the id gets the entrance picker's own test (EntranceListOf) and is ARMED;
// the entrance controller's update (the hook at the end of this file, EntranceTake) takes it at the frame it would build its first page,
// delivering what its first page and its picker deliver for a player's pick with EASY CREATION (the game has no preselect word there).
// Log: "pick taken at the entrance controller's first page: ...", then "picked wrestler N (...)" once the editor's preview object is up
// on the session's wrestler N; a WARN at 1800 frames (an untaken pick is dropped then). The editor's ACCEPT is never sent either.
//
// [G+16]'s readers and writers (review, S110 MODEPICK: the decomps of all 36 callers of sub_827D73B0, and G's boot init):
//   sub_827FCF58 +10   boot (sub_827D73E0 -> sub_827FD090): 512, with G+20 = G+60 = 0
//   sub_82B31D10       Universe: its pick, G+20 = 1 (then exit 22); sub_82B331A0 reads it on the return and resets 512 (and G+20 = 0)
//   sub_83432920 +164  the move-set controller's state 5 reads it; +190 resets 512 only when G+20 == 0 and G+60 == 0
//   sub_828F0D30 +60   G+60 = 1 and G+16 = its own row (+284), then its exit: an editor entry that KEEPS the pick for the page's return;
//                      the menu controller sub_8286AE48 (+320..+328) clears G+60 / G+64 / G+68
//   sub_828C9358 +7C   a roster page's enter (vslot +128): when sub_82B62720(id, 1) passes, sub_828E3028 moves its cursor to the id, then 512
// A preset entry with G+20 == 0 (ours, and sub_828F0D30's) also sets [[0x83929534]+156] = 1 (sub_829E60C8 +144; the manager is made per
// editor session, sub_8342D4A0 -> sub_829CEFA0). B out of the categories page then rebuilds the top page with 512 (sub_83432220, the param
// default sub_829C8770 +78), which closes as cancelled (vslot +28 sub_829D7030 -> sub_823BE7D0: +20 = 0) and the controller leaves the
// editor (sub_834325C0 with 0: state 1): it never shows the picker [C for the chain; L that the categories page answers 0 on B].
// A later go-to INSIDE the editor (not built) must refuse the rows that write or replace N's move-set: ACCEPT (sub_829C3B98 writes N's roster
// record, profile and per-id entry back from the work struct, and for 0..99 the created-superstar record), DEFAULT and MOVE-SET TEMPLATES
// (MODEPICK.md section 4; their row numbers are [L]).
// Game thread only (the owner update); the route states are read from the script thread (atomics).
#pragma once

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc/context.h>

#include <atomic>
#include <cstdint>
#include <string>

#include "game_state.h"      // AddHostStateSource (the mode_pick route states)
#include "guest_call.h"      // Caller
#include "launch_core.h"     // Verdict, Rd, kSaveBlobPtr, sub_827DA860
#include "mode_pick_core.h"  // Facts, Tab, Seen, Arrived, kStateNames
#include "mode_table.h"      // Mode
#include "scenario.h"        // U32, U8, Guest, SequenceId (and fmt)

REXCVAR_DECLARE(int64_t, wwe2k14_mode_pick);  // defined in src/cvars/cvars_1.cpp (tools\compile\cvar_shard.py: crc32 % 6 = 1)

REX_EXTERN(sub_827DA788);  // the roster row of an id (0..99 the created-superstar records, else the 204-byte rows); reads only
REX_EXTERN(sub_822C1CF0);  // the row-active test (r3 = id); hooked by src/universe_rebuild.h, which answers as the game outside a builder job
REX_EXTERN(sub_822BCFB8);  // is the character id available (r3 = id); reads only
// S110 ENTPICK: what the entrance picker's accept and its controller's accept call (sub_8226B518 +120..+17C, sub_8226B730 +238 / +244)
REX_EXTERN(sub_822C1348);  // signed byte +23 of the id's roster row (sub_827DA788), 2 when none; reads only
REX_EXTERN(sub_823266A8);  // session +36 = r4
REX_EXTERN(sub_827D92F0);  // a created superstar's record (r3 = id < 100); reads only
REX_EXTERN(sub_827D9A28);  // that record's byte +2829; reads only
REX_EXTERN(sub_829FCE08);  // session +52 = r4
REX_EXTERN(sub_82805268);  // session +44 = r4 (1: a superstar's entrance)
REX_EXTERN(sub_82805278);  // session +48 = r4 (1: a created entrance)
REX_EXTERN(__imp__sub_8226B730);  // the entrance controller's update: its original, called by the hook at the end of this file

namespace wwe2k14::mode_pick {

constexpr uint32_t kGamePtr = 0x8376AAF0, kGOff = 76048;           // sub_827D73B0: G = [0x8376AAF0] + 76048
constexpr uint32_t kGPick = 16, kGBack = 20, kGKeep = 60;          // sub_83432920 +164 / +174 / +180
constexpr uint32_t kMgrPtr = 0x83929534, kMgrWork = 24;            // sub_83432128 +84: [[0x83929534]+24] = owner+24 (the work struct)
constexpr uint32_t kRowDlc = 17;                                   // sub_829E5E10 +74 / +168
constexpr uint32_t kSlot0Char = 0x836F38D0;                        // the preview character (sub_829D6200 +224), logged only
constexpr uint64_t kWarnFrames = 1800;
constexpr uint32_t kEntSessionPtr = 0x836F2A0C;                // the entrance session (sequence 25's ctor sub_82267E00 makes it when null)
constexpr uint32_t kEntSessChar = 12, kEntSessSuperstar = 44;  // sub_827C17B8 / sub_828B2B80; sub_82805268
constexpr uint32_t kEntPreviewPtr = 0x839BFC3C;                // the editor's preview object (music_preview.h kGlobalM)

// Game thread only, except the published route states.
inline bool g_watch = false;
inline uint32_t g_id = 0;
inline uint64_t g_sent = 0;
inline uint32_t g_word = 0;  // the guest address of [G+16] Prepare wrote (RestoreDue)
inline std::string g_value;
inline std::atomic<uint32_t> g_pub[kFieldEnd];
inline PickRow g_row = PickRow::kNone;  // the row the pending pick is for (Watch)
// S110 ENTPICK: the entrance pick, armed by Prepare, taken by the controller's update hook (EntranceTake); both on the game thread.
inline std::atomic<bool> g_ent_pending{false};
inline uint32_t g_ent_list = 0;
inline bool g_ent_taken = false;

inline void Publish(uint32_t status, uint32_t id) {
  g_pub[kFieldId].store(id, std::memory_order_relaxed);
  g_pub[kFieldStatus].store(status, std::memory_order_release);
}
inline bool LookupState(const std::string& name, uint32_t* field, bool* is_signed, const char** meaning) {
  const StateName* s = FindStateName(name);
  if (!s) return false;
  *field = s->field;
  *is_signed = false;
  *meaning = s->meaning;
  return true;
}
inline bool ReadState(uint32_t field, uint32_t* out) {
  if (field < kFieldStatus || field >= kFieldEnd) return false;
  *out = g_pub[field].load(std::memory_order_acquire);
  return true;
}
inline const bool g_states_registered = AddHostStateSource(&LookupState, &ReadState);

// S110 ENTPICK: the entrance row's part of Prepare: the entrance picker's own test (the facts Prepare asked), then the pick is ARMED; nothing
// is written to the game until the controller takes it.
inline launch_core::Verdict PrepareEntrance(const Facts& f, const std::string& value, uint64_t frame) {
  std::string why;
  const EntranceList l = EntranceListOf(f, why);
  if (l.list < 0) {
    Publish(kStRefused, 0);
    return {false, "pick: " + why};
  }
  g_row = PickRow::kEntrance;
  g_watch = true;
  g_id = uint32_t(f.id);
  g_sent = frame;
  g_word = 0;
  g_value = value;
  g_ent_list = uint32_t(l.list);
  g_ent_taken = false;
  g_ent_pending.store(true, std::memory_order_release);
  Publish(kStSent, 0);
  return {true, fmt::format("pick: wrestler {} (the entrance picker's {} list, {}), armed: the entrance controller takes it at its first page "
                            "(sub_8226B730 state 1: list, wrestler, EASY CREATION)", f.id, l.list, l.name)};
}

// S110 ENTPICK: from the hook on the controller's update (r3 = C), before the update runs. Once, at the frame the update would build its
// first page, it IMITATES what that page and the wrestler picker deliver for a player's pick with EASY CREATION. Rule 6: this is an imitation
// of three game handlers' writes, kept because the game has no entry word for this editor and the handlers cannot be called without their
// live page objects (sub_822698F0 reads the first page's list object, page+272 through sub_8226D870; sub_822699B8 reads the picker's +44 /
// +36 / +40 and tail-calls the picker's own vslot +32). It keeps the game's order: the session words through the game's own setters, C's
// three answer words as the plain stores those handlers make; then the state the controller's own accept of its picker sets, so the update
// runs its picked state (vslot +24 sub_822690E8) in this same call.
//   the picker's accept (sub_8226B518 +120..+17C): session +36 = 0, then 1 when sub_822C1348(N) == 1; for N < 100 whose record's byte
//     +2829 is set (sub_827D92F0 / sub_827D9A28), session +52 = 1
//   the first page's answer (sub_822698F0 +2C): C+100 = the list; the picker's answer (sub_822699B8): C+112 = N, C+108 = 0 (EASY)
//   the controller's accept of its picker (sub_8226B730 +234..+26C): session +44 = 1, +48 = 0; C+48 = 5, C+52 = 0
// Not delivered: C+116 (it reaches the session as +56: sub_82269500 +40, sub_82805280). A player's accept copies the first page's +652 into
// it (sub_822698F0 +34); the page's input accept handler sub_822696D0 (vslot +88 of 0x82003170, r3 = page+44, data word 0x820031C8) sets
// +652 = word +100 of the page's input object [C]; what that word holds was not read (an indexed store; sub_823BD1E0 fills +84.. with -1)
// [U]. The ctor's C+116 kept here is [[0x8376AAF0]], the port the first page listens on (sub_8226B730 +AC..+C0: page+44+8 = 1 << it), so
// it is the same value if +100 is the accepting pad's port [L].
// Skipped with the two pages (none leaves state the picked path reads [C]): their builds (the first page's render node, list and input
// mask; the picker's select screen sub_82832D38 and its select manager [0x839294C0], made by the picker's ctor and freed back to 0 by its
// dtor sub_8226B238, so 0 after either path); the list build's request sub_827E6A88(0, 0) (sub_8226B2B8 +20); the help-layout calls
// sub_8290B4F8(9, 0) at state 1 (+84; sub_82269190 +2A4 already made it) and (0, 0) at the page's accept (+120; state 5 makes the same
// call at +3CC, so the end state is the same).
inline void EntranceTake(PPCContext& ctx, uint8_t* base) {
  if (!g_ent_pending.load(std::memory_order_acquire)) return;
  const uint32_t c = ctx.r3.u32;
  if (!CommitDue(true, scenario::U32(c + kEntState), scenario::U32(c + kEntSub))) return;
  g_ent_pending.store(false, std::memory_order_relaxed);
  const uint32_t e = scenario::U32(kEntSessionPtr);
  if (!e) {
    REXLOG_WARN("[wwe2k14][launch] {}: pick not taken: the entrance session [0x836F2A0C] is null at the controller's first page (C {:08X}); "
                "the controller shows its own pages", g_value, c);
    return;
  }
  scene_director::Caller call(ctx, base);
  const uint64_t id = g_id, se = e;
  const int32_t kind = int32_t(call(sub_822C1348, {id}));
  call(sub_823266A8, {se, 0});
  if (kind == 1) call(sub_823266A8, {se, 1});
  bool created_flag = false;
  if (id < kCawIds) {
    created_flag = call(sub_827D9A28, {uint64_t(call(sub_827D92F0, {id}))}) != 0;
    if (created_flag) call(sub_829FCE08, {se, 1});
  }
  rex::memory::store_and_swap<uint32_t>(scenario::Guest(c + kEntList), g_ent_list);
  rex::memory::store_and_swap<uint32_t>(scenario::Guest(c + kEntChar), g_id);
  rex::memory::store_and_swap<uint32_t>(scenario::Guest(c + kEntAdvanced), 0u);
  call(sub_82805268, {se, 1});
  call(sub_82805278, {se, 0});
  rex::memory::store_and_swap<uint32_t>(scenario::Guest(c + kEntState), kEntStatePicked);
  rex::memory::store_and_swap<uint32_t>(scenario::Guest(c + kEntSub), 0u);
  g_ent_taken = true;
  REXLOG_INFO("[wwe2k14][launch] {}: pick taken at the entrance controller's first page: wrestler {}, list {}, EASY (C {:08X}, session {:08X}; "
              "row byte +23 = {}, created flag {}): the controller's picked state runs next", g_value, g_id, g_ent_list, c, e, kind,
              created_flag ? 1 : 0);
}

// S110 ENTPICK: Watch for the entrance row: the arrival line once the editor is up on N. At the warn frame an untaken pick is dropped: a
// GUARD (rule 6), so a Create an Entrance opened later in the boot shows its own pages; it stands in for binding the pick to the one
// controller our exit creates.
inline void WatchEntrance(uint64_t frame) {
  EntranceSeen s;
  s.taken = g_ent_taken;
  s.session = launch_core::Rd(kEntSessionPtr);
  s.session_char = s.session ? launch_core::Rd(s.session + kEntSessChar) : 0;
  s.session_superstar = s.session ? launch_core::Rd(s.session + kEntSessSuperstar) : 0;
  s.preview = launch_core::Rd(kEntPreviewPtr);
  if (EntranceArrived(s, g_id)) {
    g_watch = false;
    REXLOG_INFO("[wwe2k14][launch] {}: picked wrestler {} ({} frames after the exit; entrance editor up: session {:08X} +12 = {}, preview object "
                "{:08X}, sequence {}, slot 0 character {:08X})", g_value, g_id, frame - g_sent, s.session, s.session_char, s.preview,
                scenario::SequenceId(), launch_core::Rd(kSlot0Char));
    Publish(kStArrived, g_id);
    return;
  }
  if (frame - g_sent == kWarnFrames) {
    const bool dropped = g_ent_pending.exchange(false);
    REXLOG_WARN("[wwe2k14][launch] {}: no entrance editor on wrestler {} {} frames after the exit (pick taken: {}; sequence {}, session {:08X} "
                "+12 = {} +44 = {}, preview object {:08X}){}; still watching", g_value, g_id, kWarnFrames, s.taken ? "yes" : "no",
                scenario::SequenceId(), s.session, s.session_char, s.session_superstar, s.preview,
                dropped ? "; the pick was never taken: dropped, so no later Create an Entrance opens on it" : "");
  }
}

// Before the exit of row `m` (launch value `value`): ok with "" when no pick is set; ok with the detail when the word is written; refused
// with the reason (nothing written) when the row takes no pick or the picker would not list the id.
inline launch_core::Verdict Prepare(PPCContext& ctx, uint8_t* base, const mode_launch::Mode& m, const std::string& value, uint64_t frame) {
  const int64_t want = REXCVAR_GET(wwe2k14_mode_pick);
  if (want == kOff) return {true, ""};
  const PickRow row = RowPick(m.name);
  if (row == PickRow::kNone) {
    Publish(kStRefused, 0);
    return {false, fmt::format("--wwe2k14_mode_pick={} is read for @mode:moveset and @mode:entrance only (src\\mode_pick_core.h RowPick)", want)};
  }
  Facts f;
  f.id = want;
  const uint32_t blob = scenario::U32(launch_core::kSaveBlobPtr), gp = scenario::U32(kGamePtr);
  if (!blob || !gp) {
    Publish(kStRefused, 0);
    return {false, fmt::format("pick: the save block ({:08X}) or the game globals ({:08X}) are null, so the picker's test cannot be asked", blob, gp)};
  }
  if (want >= 0 && want < int64_t(kIds)) {
    scene_director::Caller call(ctx, base);
    const uint64_t id = uint64_t(want);
    if (want < int64_t(kCawIds)) {
      f.created_used = call(sub_827DA860, {id}) != 0;
    } else {
      const uint32_t row = call(sub_827DA788, {id});
      f.has_row = row != 0;
      f.dlc_byte = row ? scenario::U8(row + kRowDlc) : 0;
    }
    // the picker asks both of every id: to list 100..499, and as the flag word of every listed id (sub_829E5E10 +1E0 / +1F0)
    f.row_active = call(sub_822C1CF0, {id}) != 0;
    f.available = call(sub_822BCFB8, {id}) != 0;
  }
  if (row == PickRow::kEntrance) return PrepareEntrance(f, value, frame);  // S110 ENTPICK: armed here, taken by EntranceTake
  std::string why;
  const std::string tab = Tab(f, why);
  if (tab.empty()) {
    Publish(kStRefused, 0);
    return {false, "pick: " + why};
  }
  const uint32_t g = gp + kGOff, before = scenario::U32(g + kGPick);
  rex::memory::store_and_swap<uint32_t>(scenario::Guest(g + kGPick), uint32_t(want));
  g_row = PickRow::kMoveset;
  g_watch = true;
  g_id = uint32_t(want);
  g_sent = frame;
  g_word = g + kGPick;
  g_value = value;
  Publish(kStSent, 0);
  return {true, fmt::format("pick: wrestler {} (the picker's {} tab), [G+16] {} -> {} (G {:08X}; G+20 = {}, G+60 = {}: state 5 resets it to 512 "
                            "when both are 0)", want, tab, before, want, g, scenario::U32(g + kGBack), scenario::U32(g + kGKeep))};
}

// Once per guest frame (src/mode_launch.h Step): the arrival line, once.
inline void Watch(uint64_t frame) {
  if (!g_watch) return;
  if (g_row == PickRow::kEntrance) {  // S110 ENTPICK
    WatchEntrance(frame);
    return;
  }
  Seen s;
  s.seq = scenario::SequenceId();
  const uint32_t mgr = launch_core::Rd(kMgrPtr);
  s.work = mgr ? launch_core::Rd(mgr + kMgrWork) : 0;
  s.page = s.work ? launch_core::Rd(s.work - 4) : 0;
  s.page_vtable = s.page ? launch_core::Rd(s.page) : 0;
  s.work_id = s.work ? launch_core::Rd(s.work) : 0;
  if (Arrived(s, g_id)) {
    g_watch = false;
    REXLOG_INFO("[wwe2k14][launch] {}: picked wrestler {} ({} frames after the exit; work {:08X}, categories page {:08X}, slot 0 character {:08X})",
                g_value, g_id, frame - g_sent, s.work, s.page, launch_core::Rd(kSlot0Char));
    Publish(kStArrived, g_id);
    return;
  }
  if (frame - g_sent == kWarnFrames) {
    const uint32_t now = scenario::U32(g_word);
    const bool restore = RestoreDue(frame - g_sent, kWarnFrames, now, g_id);
    if (restore) rex::memory::store_and_swap<uint32_t>(scenario::Guest(g_word), kGIdle);
    REXLOG_WARN("[wwe2k14][launch] {}: no move-set editor on wrestler {} {} frames after the exit (sequence {}, work {:08X}, page {:08X} vtable "
                "{:08X}, work id {}); [G+16] = {}{}; still watching", g_value, g_id, kWarnFrames, s.seq, s.work, s.page, s.page_vtable, s.work_id, now,
                restore ? ", unconsumed: put back to 512 so no later screen reads it" : "");
  }
}

}  // namespace wwe2k14::mode_pick

// S110 ENTPICK: the entrance controller's update (vslot +64 of 0x820033E8, run by sub_823BE6D8 while C+16 == 2; r3 = C): the pending pick is
// taken before the update of the frame its first page would be built (EntranceTake); otherwise one atomic read.
REX_HOOK_RAW(sub_8226B730) {
  wwe2k14::mode_pick::EntranceTake(ctx, base);
  __imp__sub_8226B730(ctx, base);
}
