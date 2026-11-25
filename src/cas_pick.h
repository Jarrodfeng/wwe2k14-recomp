// S110 CASPICK: `wwe2k14_cas_pick=<K>` picks item K (1-based, as the parts panel shows it: 5 = "5/51") on the Create a Superstar item page
// the go-to (src/cas_goto.h) has arrived on, with the panel's own handlers, and keeps it, as a player's presses would
// (docs\research\s110\CASPICK.md). TEST / TOOLING PATH ONLY: a route arms it with a `cvar` line after the go-to's arrival
// (`wait_state cas_record N`), and waits on `cas_pick_item K`.
//
// The parts panel P (vtable 0x821C08D8, ctor sub_833FE938: the container's inner panel, container+936, on the item page's overlay A) and its
// input mixin M = P+28 (vtable 0x821C0800). All read from the listings:
//   - the list L = [P+948]: the highlighted item is sub_833F8B08(P) = (L's vslot 2 + P+5184) mod [L+20], so [L+20] items that wrap.
//   - a cursor step: a pad press reaches the panel through the mixin's update (sub_823BD358 +418 -> slot 3 sub_823BC8C0, the dispatch: no
//     action-table bit -> slot 4 sub_823BC968, the generic move: the row M+20, the move sound (slot 10 sub_82269600 -> sub_823BD150, M+52 =
//     63) and the moved flag M+80, then slot 24 sub_833FD358). Slot 24 tests the pressed word M+56 against the pad table 0x8200F510 +
//     16 * [M+12] (+8 down, +0 up) and, with L up, no step running (P+1032 == 0) and no sub-mode (P+1028 == 0), starts one:
//     sub_833FC168(P, 1) for down, (P, 2) for up (P+1032 = the direction, P+1036 = 8). The panel's update (vslot 49 sub_834003C8) counts
//     P+1036 down and commits with sub_833FE7E8 (down: L's vslot 5, +1; up: L's vslot 6, -1; both wrap: cas_pick_core.h Dir), which clears
//     P+1032 and sets P+984 = 1: the item is to be put on.
//   - moving the cursor IS the apply: with no step running, no direction held and the model ready, the same update calls vslot 59
//     sub_833FD870, which puts the highlighted item on the superstar (sub_8338FFD8, sub_822E62C8) and clears P+984; it does not for an item
//     that is locked ([[L+212] + 4 i] != 0) or that the part rules refuse (sub_8338FC20, sub_833FC398). The panel's B puts back the part
//     it opened on (+5172, sub_833FF9C8) and the copy its ctor pushed (sub_822D3A30); its commit (vslot 6) keeps the item.
//   - the keep (CASPICK fix 3, rule 6; cas_pick_core.h KeepReady): the panel's own commit, vslot 6 sub_833FF868, the function both of the
//     game's accept paths end in (mode 1's A at once, sub_833FFE20 jump target 8340007C; modes 0 / 2 / 6 / 8 / 9 after the part's edit page
//     and its stages close accepted, sub_833FFAD8 +11C). It finalises and keeps the part and closes the panel accepted (sub_833B3A60: +16 = 3,
//     +20 = 1); then, as mode 1's A does, the list's sub_834317B8. The container closes accepted (sub_833DFDC0), the host keeps the superstar
//     (sub_833BFA48 +C30 -> sub_822D28C0) and drops the overlay, moving to the item page's parent or grandparent in the same update. The pick
//     sends no A: the edit page and its stages (COLOR, ETC. / LOGOS / the colour picker ...) write nothing until one of their rows is used,
//     so they are never built (fix 2's ACCEPT-row walk is removed). The A's own refusals are still read first (see below).
// What the pick writes: only the pressed word M+56 around its call of the mixin's dispatch (the mixin's update clears it every frame,
// sub_823BD358 +24); everything else is the game's handlers. Nothing is pressed before the go-to's arrival rules hold (PanelNotReady and
// PreviewNotReady, src/cas_goto.h). Refused before any press: not an item page's parts panel, a mode that is not a plain accept, K past the
// list, a locked item, an item whose A shows a message. Refused at the item (the cursor stays there; the panel has put nothing on): the
// A enable test 0 (sub_833FD768: pending, model, lock, sub_833FC398's part rules), the part-rule gate sub_8338FC20, no part.
// States: cas_pick (0 idle, 1 working, 2 applied, 3 stopped / refused), cas_pick_item (the item applied, set last), cas_pick_reason,
// cas_pick_page (where the editor settled: 1 parent, 2 grandparent, 3 the item page, 4 elsewhere).
// Every wait is bounded by the go-to's step bound (900 frames) from its own start. Off: one atomic load a frame (the cvar change counter).
// Game thread only: called from src/cas_launch.h Tick, after the go-to's Step. One driver at a time (S110 CASROW, src/cas_driver.h): the pick
// takes the editor's presses when it is armed and gives them back when it ends; a go-to or a row verb armed meanwhile is refused busy.
// S110 CASROW: `wwe2k14_cas_pick_mode=edit` ends the pick with the player's accepting A instead of the commit: the part's edit page S is left
// up and idle (cas_pick_page 5) for the row verb (src/cas_row.h). The pick also runs on the logo list P2 (a parts panel nested in the LOGOS
// page; categories 49 / 50 / 51, mode 3: cas_pick_core.h NestedLogoCommit), found with cas_row::FindTop; it ends when the LOGOS page is back
// on its rows (cas_pick_page 6).
#pragma once

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include <atomic>
#include <cstdint>
#include <string>

#include "cas_goto.h"       // the editor's words and the go-to's tests (Host, Busy, PanelNotReady, PreviewNotReady, PageId, Rd, Wr), sub_823BC7C0
#include "cas_pick_core.h"  // ParseItem, PlanSteps, StepLanded, PlainAccept, the route states
#include "cas_row_core.h"   // S110 CASROW: FindTop (the panel a press reaches now: the item page's, or the logo list under the LOGOS page)
#include "game_state.h"     // AddHostStateSource (the cas_pick route states)
#include "guest_call.h"     // Caller
#include "scenario.h"       // Frame, SequenceId (and fmt)

REXCVAR_DECLARE(std::string, wwe2k14_cas_pick);  // defined in src/cvars/cvars_1.cpp (tools\compile\cvar_shard.py: crc32 % 6 = 1)
REXCVAR_DECLARE(std::string, wwe2k14_cas_pick_mode);  // S110 CASROW: "" / commit / edit; defined in src/cvars/cvars_0.cpp (crc32 % 6 = 0)

REX_EXTERN(sub_823BC8C0);  // the input mixin's dispatch (slot 3, r3 = panel+28), as its update calls it with the pressed word mixin+56
REX_EXTERN(sub_833F8B08);  // the panel's highlighted item index, 0-based (r3 = panel); reads only
REX_EXTERN(sub_833FD768);  // the panel mixin's A enable test (slot 35, r3 = panel+28); reads only
REX_EXTERN(sub_833FB9B8);  // the highlighted item's part (r3 = panel); reads only
REX_EXTERN(sub_8338FC20);  // the A handler's part-rule gate (r3 = [0x839BFC7C], r4 = panel+964, r5 = part): != 0 = a message box; reads only
REX_EXTERN(sub_833FF868);  // the parts panel's commit (vslot 6, r3 = panel): finalise and keep the part, close the panel accepted
REX_EXTERN(sub_834317B8);  // the list's reset after the commit (r3 = the list [panel+948]), as mode 1's A calls it (sub_833FFE20 +268)

namespace wwe2k14::cas_pick {

using cas_goto::Rd;
using cas_goto::Wr;

constexpr uint32_t kPList = 948, kPCategory = 964, kPPart = 976, kPMode = 980, kPPending = 984, kPResult = 992, kPSubMode = 1028,
                   kPStep = 1032;                                          // the parts panel (sub_833FE938, sub_833FFE20, sub_834003C8)
constexpr uint32_t kLCount = 20, kLLocks = 212;                            // its list (sub_833F8B08 +58; sub_833FD768 +48..+58)
constexpr uint32_t kPMsg1 = 2084, kPMsg2 = 3108, kPMsg3 = 4132, kMsgWords = 256;  // the A's message words by item (sub_833FFE20 +A8..+F0)
constexpr uint32_t kMPressed = 56, kMPadSlot = 12, kPadTable = 0x8200F510, kPadUp = 0, kPadDown = 8;  // sub_833FD358 +2C..+4C, +13C..+148
constexpr uint32_t kNoPart = 0xFFFFFFFEu, kModeCommit = 1, kPClosed = 20;  // +20: the close's accepted word (sub_823BE7A0)
constexpr uint32_t kModeLayered = 3, kPEditPage = 996, kPanelEditStage = 2, kPColourVariant = 5188;  // sub_833FFE20 +230; sub_833FFAD8 +94
constexpr uint32_t kMaxA = 2;  // edit mode: the A, and a second A when the first opened the colour variants (P+1028 = 1; fix 2, run 294)

enum class Phase { kOff, kStart, kStep, kApply, kEdit, kClose, kDone };
// Game thread only, except the published route states.
inline Phase g_phase = Phase::kOff;
inline uint32_t g_k = 0, g_n = 0;                        // the item asked (1-based) and the panel's item count
inline uint32_t g_page = 0, g_cont = 0, g_panel = 0;     // the item page, its container (overlay A) and the parts panel the pick runs on
inline uint32_t g_parent = 0, g_grand = 0;               // the item page's parent and grandparent (the A's landings)
inline uint32_t g_prev = 0;                              // the index before the step in flight
inline Dir g_dir = kDown;                                // the index direction the step in flight was asked for
inline bool g_sent = false;
inline bool g_nested = false;  // S110 CASROW: the panel is the logo list P2 held by the LOGOS page g_cont (not the item page's container)
inline bool g_edit = false;    // S110 CASROW: wwe2k14_cas_pick_mode=edit, read at Arm
inline uint32_t g_a = 0;       // edit mode: the A presses sent
inline uint32_t g_steps = 0;
inline uint64_t g_since = 0;  // the start of the current wait
inline std::string g_taken;   // the wwe2k14_cas_pick value taken last
inline uint64_t g_changes = 0;
inline Report g_report;
inline std::atomic<uint32_t> g_pub[kFieldEnd];

// One state line per change; the script reads g_pub on its own thread. The item is stored LAST: a route that has waited on cas_pick_item K
// reads every other field of that pick.
inline void Publish(const Report& r, uint64_t frame) {
  if (r == g_report) return;
  g_report = r;
  g_pub[kFieldReason].store(FieldValue(r, kFieldReason), std::memory_order_release);
  g_pub[kFieldPage].store(FieldValue(r, kFieldPage), std::memory_order_release);
  g_pub[kFieldStatus].store(FieldValue(r, kFieldStatus), std::memory_order_release);
  g_pub[kFieldItem].store(FieldValue(r, kFieldItem), std::memory_order_release);
  REXLOG_INFO("[wwe2k14][cas_pick] state cas_pick={} cas_pick_item={} cas_pick_reason={} ({}) cas_pick_page={} target {} (frame {})",
              uint32_t(r.status), r.item, uint32_t(r.reason), ReasonName(r.reason), uint32_t(r.page), r.target, frame);
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

// Ends the pick: Reason::kNone = applied (an INFO line), else the STOPPED line (a refusal too) with its reason.
inline void Stop(const std::string& what, Reason why, uint64_t frame, Landing page = Landing::kNone) {
  Report r = g_report;
  if (why == Reason::kNone) {
    REXLOG_INFO("[wwe2k14][cas_pick] item {}: {} (frame {})", g_k, what, frame);
    Applied(r, page);
  } else {
    REXLOG_WARN("[wwe2k14][cas_pick] item {}: STOPPED: {} (frame {})", g_k, what, frame);
    Halt(r, why);
  }
  cas_driver::Release(cas_driver::kPick);  // S110 CASROW: the pick has ended: the editor's presses are free
  g_phase = Phase::kDone;
  Publish(r, frame);
}

// Takes the setting's value (the first frame, then whenever a route sets a different value while the pick is idle or has ended).
inline void Arm(const std::string& arg, uint64_t frame) {
  cas_driver::Release(cas_driver::kPick);  // S110 CASROW: the last request is dropped, and with it the editor's presses
  g_taken = arg;
  g_phase = Phase::kDone;
  g_page = g_cont = g_panel = g_parent = g_grand = 0;
  g_sent = g_nested = g_edit = false;
  g_steps = g_a = 0;
  g_since = frame;
  if (arg.empty()) {
    Publish(Report{}, frame);
    return;
  }
  uint32_t k = 0;
  const std::string bad = ParseItem(arg, k);
  if (!bad.empty()) {
    REXLOG_WARN("[wwe2k14][cas_pick] wwe2k14_cas_pick={}: {}; off", arg, bad);
    Report r;
    Halt(r, Reason::kBadArg);
    Publish(r, frame);
    return;
  }
  const std::string mode = rex::cvar::GetFlagByName("wwe2k14_cas_pick_mode");  // S110 CASROW: read when the pick is armed
  const std::string bad_mode = ParseMode(mode, g_edit);
  if (!bad_mode.empty()) {
    REXLOG_WARN("[wwe2k14][cas_pick] wwe2k14_cas_pick_mode={}: {}; the pick is off", mode, bad_mode);
    Report r = Armed(k);
    Halt(r, Reason::kBadArg);
    Publish(r, frame);
    return;
  }
  if (!cas_driver::Claim(cas_driver::kPick)) {  // S110 CASROW: first armed wins
    REXLOG_WARN("[wwe2k14][cas_pick] item {}: REFUSED: {} drives the editor now (one driver at a time: arm the pick after it has ended)", k,
                cas_driver::Name(cas_driver::g_owner));
    Report r = Armed(k);
    Halt(r, Reason::kBusy);
    Publish(r, frame);
    return;
  }
  g_k = k;
  REXLOG_INFO("[wwe2k14][cas_pick] item {}: waiting for an item page's parts panel to be up and idle ({} mode) (frame {})", g_k, g_edit ? "edit" : "commit",
              frame);
  g_phase = Phase::kStart;
  Publish(Armed(k), frame);
}

// One step towards item g_k through the mixin's dispatch (the pad's own path to the panel's move handler), or the end of the steps (kApply).
inline void MoveStep(scene_director::Caller& call, uint64_t frame) {
  const uint32_t pn = g_panel, m = pn + cas_goto::kPMixin;
  if (Rd(pn + kPStep)) {  // a step runs: the panel's update commits it once P+1036 has counted down
    if (frame - g_since > cas_goto::kStepFrames)
      Stop(fmt::format("not settled {} frames after a cursor step: the panel's step P+1032 = {} has not ended (P+1036 = {})", cas_goto::kStepFrames,
                       Rd(pn + kPStep), Rd(pn + kPStep + 4)),
           Reason::kStepStuck, frame);
    return;
  }
  const uint32_t cur = call(sub_833F8B08, {uint64_t(pn)});
  if (g_sent) {  // the step in flight has ended: did the index move as asked?
    g_sent = false;
    if (!StepLanded(g_prev, cur, g_dir, g_n)) {
      Stop(fmt::format("a step from item {} landed on item {} of {}, not the {} one (the list's move sub_83431080 / sub_83431138 returns without "
                       "moving while its last scroll still runs: cas_pick_core.h StepLanded)",
                       g_prev + 1, cur + 1, g_n, g_dir == kDown ? "next" : "previous"),
           Reason::kStepWrong, frame);
      return;
    }
  }
  const Plan plan = PlanSteps(cur, g_k, g_n);
  if (plan.steps == 0) {
    REXLOG_INFO("[wwe2k14][cas_pick] item {}: the cursor is on item {}/{} after {} step(s); waiting for the panel to put it on the preview (frame {})",
                g_k, cur + 1, g_n, g_steps, frame);
    g_phase = Phase::kApply;
    g_since = frame;
    return;
  }
  if (g_steps > 2 * g_n + 2) {
    Stop(fmt::format("{} steps and the cursor is on item {} of {}, not {}", g_steps, cur + 1, g_n, g_k), Reason::kStepWrong, frame);
    return;
  }
  const Dir press = plan.dir;
  const uint32_t bit = Rd(kPadTable + (Rd(m + kMPadSlot) << 4) + (press == kDown ? kPadDown : kPadUp));
  if (!bit || Rd(pn + kPSubMode)) {
    Stop(fmt::format("no step can be started: pad bit {:08X} (pad slot {}), sub-mode P+1028 = {}", bit, Rd(m + kMPadSlot), Rd(pn + kPSubMode)),
         Reason::kStepStuck, frame);
    return;
  }
  const uint32_t pressed = Rd(m + kMPressed);
  Wr(m + kMPressed, bit);  // the pressed word the dispatch reads; the mixin's update rewrites it every frame (sub_823BD358 +24)
  call(sub_823BC8C0, {uint64_t(m)});  // the pad's own path: dispatch -> generic move (row, move sound) -> the panel's move (slot 24)
  Wr(m + kMPressed, pressed);
  if (Rd(pn + kPStep) != uint32_t(press)) {
    Stop(fmt::format("the mixin's dispatch (sub_823BC8C0 -> sub_823BC968 -> the panel's move sub_833FD358) started no {} step (P+1032 = {}, list "
                     "{:08X}; the generic move reaches the panel only with its row list on: mixin+32 = {}, mixin+24 = {})",
                     press == kDown ? "down" : "up", Rd(pn + kPStep), Rd(pn + kPList), Rd(m + 32), Rd(m + 24)),
         Reason::kStepStuck, frame);
    return;
  }
  g_prev = cur;
  g_dir = plan.dir;
  g_sent = true;
  ++g_steps;
  g_since = frame;
}

// The cursor is on item g_k: wait for the panel to put it on and the preview to settle, read the A's refusals, then the panel's commit.
inline void ApplyStep(scene_director::Caller& call, uint64_t frame) {
  const uint32_t pn = g_panel, m = pn + cas_goto::kPMixin;
  const std::string preview = cas_goto::PreviewNotReady(call);
  if (!KeepReady(Rd(pn + kPPending), Rd(pn + kPStep), Rd(pn + kPSubMode), Rd(pn + kPResult), preview.empty())) {
    if (frame - g_since > cas_goto::kStepFrames)
      Stop(fmt::format("not settled {} frames after the cursor reached item {}: {} (P+984 = {}, P+1032 = {}, P+1028 = {}, P+992 = {})",
                       cas_goto::kStepFrames, g_k, preview.empty() ? "the panel has not put the item on the preview" : preview,
                       Rd(pn + kPPending), Rd(pn + kPStep), Rd(pn + kPSubMode), Rd(pn + kPResult)),
           Reason::kApplyWait, frame);
    return;
  }
  const uint32_t cur = call(sub_833F8B08, {uint64_t(pn)});
  if (cur != g_k - 1) {
    Stop(fmt::format("the cursor left item {}: it is on item {} of {}", g_k, cur + 1, g_n), Reason::kStepWrong, frame);
    return;
  }
  const uint32_t part = Rd(pn + kPPart);
  if (part == kNoPart) {
    Stop(fmt::format("REFUSED: item {}/{} has no part (P+976 = -2): the panel's A does nothing (sub_833FFE20 +F4)", g_k, g_n), Reason::kARefused, frame);
    return;
  }
  if (!call(sub_833FD768, {uint64_t(m)})) {
    Stop(fmt::format("REFUSED: the panel's A enable test (sub_833FD768) is 0 on item {}/{}: the panel refuses it (a locked item, or the part "
                     "rules, sub_833FC398: e.g. the remaining points); the cursor stays there, nothing was accepted",
                     g_k, g_n),
         Reason::kARefused, frame);
    return;
  }
  if (Rd(pn + kPMode) != kModeCommit) {
    const uint32_t ipart = call(sub_833FB9B8, {uint64_t(pn)});
    if (call(sub_8338FC20, {uint64_t(Rd(cas_goto::kCawRootPtr)), uint64_t(Rd(pn + kPCategory)), uint64_t(ipart)})) {
      Stop(fmt::format("REFUSED: item {}/{}: the panel's A would show a message box (53357 / 6654: sub_8338FC20, category {}, part {}) instead "
                       "of accepting it",
                       g_k, g_n, Rd(pn + kPCategory), int32_t(ipart)),
           Reason::kMessage, frame);
      return;
    }
  }
  if (g_edit) {  // S110 CASROW edit mode: the player's accepting A (the pad A entry: enable test, the A handler sub_833FFE20, the sound)
    REXLOG_INFO("[wwe2k14][cas_pick] item {}: item {}/{} is on the preview (part P+976 = {}): A on the parts panel {:08X} (edit mode) (frame {})", g_k, g_k,
                g_n, int32_t(part), pn, frame);
    call(sub_823BC7C0, {uint64_t(m)});
    g_a = 1;
    g_phase = Phase::kEdit;
    g_since = frame;
    return;
  }
  REXLOG_INFO("[wwe2k14][cas_pick] item {}: item {}/{} is on the preview (part P+976 = {}): the parts panel {:08X}'s commit (vslot 6 sub_833FF868, "
              "as its A ends; no edit page, the item's own colour) (frame {})",
              g_k, g_k, g_n, int32_t(part), pn, frame);
  call(sub_833FF868, {uint64_t(pn)});
  // mode 1's A resets the list after the commit (sub_833FFE20 +268); mode 3's own commit (P+972 == 10, +32C) does not (S110 CASROW)
  if (const uint32_t list = Rd(pn + kPList); list && Rd(pn + kPMode) != kModeLayered) call(sub_834317B8, {uint64_t(list)});
  if (!ClosedAccepted(Rd(pn + cas_goto::kPState), Rd(pn + kPClosed))) {
    Stop(fmt::format("the panel's commit did not close it accepted (state +16 = {}, +20 = {})", Rd(pn + cas_goto::kPState), Rd(pn + kPClosed)),
         Reason::kARefused, frame);
    return;
  }
  REXLOG_INFO("[wwe2k14][cas_pick] item {}: the parts panel closed accepted (+16 = 3, +20 = 1): the item page's overlay closes (frame {})", g_k, frame);
  g_phase = Phase::kClose;
  g_since = frame;
}

// S110 CASROW edit mode: after the panel's A, the part's edit page. The A handler sub_833FFE20: with colour variants (mode 0, sub_822E1BD0) the
// first A only opens them (sub_833FE230: P+1028 = 1) and a second A takes the variant under the cursor and starts the edit stage, as fix 2
// drove it (run 294); otherwise the A starts it at once (slot 66 sub_833F8428: P+992 = 2). The stage (slot 50 sub_833FFAD8) builds S =
// sub_833DF0E0 at P+996 once the panel's out transition has ended. Done when S is idle on its rows (S+16 = 2, S+952 = 0) and the preview is
// ready: the item is ON, not kept; the panel keeps it when S closes accepted (S's row 6315 ACCEPT: wwe2k14_cas_row 6315).
inline void EditStep(scene_director::Caller& call, uint64_t frame) {
  const uint32_t pn = g_panel, m = pn + cas_goto::kPMixin, s = Rd(pn + kPEditPage);
  if (Rd(pn + kPResult) != kPanelEditStage) {
    if (Rd(pn + kPSubMode) && g_a < kMaxA) {  // the colour variants are open: the second A, once the panel is idle again
      if (Rd(pn + kPPending) || Rd(pn + kPStep) || !cas_goto::PreviewNotReady(call).empty()) {
        if (frame - g_since > cas_goto::kStepFrames)
          Stop(fmt::format("not settled {} frames after the first A on item {}: the colour variants are open but the panel is not idle (P+984 = {}, "
                           "P+1032 = {})",
                           cas_goto::kStepFrames, g_k, Rd(pn + kPPending), Rd(pn + kPStep)),
               Reason::kEditWait, frame);
        return;
      }
      if (!call(sub_833FD768, {uint64_t(m)})) {
        Stop(fmt::format("REFUSED: the panel's A enable test (sub_833FD768) is 0 with the colour variants of item {}/{} open", g_k, g_n), Reason::kARefused,
             frame);
        return;
      }
      REXLOG_INFO("[wwe2k14][cas_pick] item {}: the item has colour variants: the panel's A opened them (P+1028 = 1, sub_833FE230, on variant P+5188 = {}); "
                  "the second A takes the variant under the cursor (frame {})",
                  g_k, Rd(pn + kPColourVariant), frame);
      call(sub_823BC7C0, {uint64_t(m)});
      ++g_a;
      g_since = frame;
      return;
    }
    if (frame - g_since > cas_goto::kStepFrames)
      Stop(fmt::format("edit mode: {} A press(es) on item {} did not start the panel's edit stage (P+992 = {}, P+1028 = {})", g_a, g_k, Rd(pn + kPResult),
                       Rd(pn + kPSubMode)),
           Reason::kEditPage, frame);
    return;
  }
  std::string busy;
  if (!s || Rd(s) != cas_row::kEditPageVtable || Rd(s + cas_goto::kPState) != cas_goto::kPageIdle || Rd(s + cas_row::kSStage))
    busy = fmt::format("the part's edit page is not up and idle (P+996 = {:08X}, vtable {:08X}, +16 = {}, S+952 = {})", s, s ? Rd(s) : 0,
                       s ? Rd(s + cas_goto::kPState) : 0, s ? Rd(s + cas_row::kSStage) : 0);
  if (busy.empty()) busy = cas_goto::PreviewNotReady(call);
  if (!busy.empty()) {
    if (frame - g_since > cas_goto::kStepFrames)
      Stop(fmt::format("not settled {} frames after the A on item {}: {}", cas_goto::kStepFrames, g_k, busy), Reason::kEditWait, frame);
    return;
  }
  Stop(fmt::format("applied item {}/{} ({} step(s), {} A on the panel, edit mode): the part's edit page {:08X} (kind {}) is up and idle with the item on "
                   "the preview; the item is kept only when the edit page closes accepted (its row 6315 ACCEPT: wwe2k14_cas_row 6315)",
                   g_k, g_n, g_steps, g_a, s, Rd(s + cas_row::kSKind)),
       Reason::kNone, frame, Landing::kEditPage);
}

// After the commit: wait until the item page's overlay has gone, the host has settled on the page it lands on, and the preview is ready.
inline void CloseStep(scene_director::Caller& call, uint32_t h, uint64_t frame) {
  if (g_nested) {  // S110 CASROW: the LOGOS page's stage wait sub_834079D0 drops the closed list (S2+952 8, then 0) and rebuilds its rows
    const uint32_t s2 = g_cont;
    std::string busy;
    if (Rd(s2) != cas_row::kEditPageVtable)
      busy = fmt::format("the LOGOS page {:08X} went away (vtable {:08X})", s2, Rd(s2));
    else if (Rd(s2 + cas_row::kSStage) || Rd(s2 + cas_row::kSChild) || Rd(s2 + cas_goto::kPState) != cas_goto::kPageIdle)
      busy = fmt::format("the LOGOS page {:08X} is not back on its rows (S2+952 = {}, S2+1088 = {:08X}, +16 = {})", s2, Rd(s2 + cas_row::kSStage),
                         Rd(s2 + cas_row::kSChild), Rd(s2 + cas_goto::kPState));
    if (busy.empty()) busy = cas_goto::PreviewNotReady(call);
    if (!busy.empty()) {
      if (frame - g_since > cas_goto::kStepFrames)
        Stop(fmt::format("not settled {} frames after the commit of item {}: {}", cas_goto::kStepFrames, g_k, busy), Reason::kCloseWait, frame);
      return;
    }
    Stop(fmt::format("applied item {}/{} ({} step(s), the logo list's commit): the logo list closed accepted and the LOGOS page {:08X} is back on its rows; "
                     "the preview is ready (the logo stays only when the item page's panel closes accepted too: B on the LOGOS page, then ACCEPT 6315 "
                     "on the part's edit page)",
                     g_k, g_n, g_steps, s2),
         Reason::kNone, frame, Landing::kOpener);
    return;
  }
  const uint32_t a = Rd(h + cas_goto::kHOverlayA), p = CloseSettledPage(a, Rd(h + cas_goto::kHPage));
  std::string busy;
  if (!p) busy = fmt::format("the item page's overlay {:08X} has not closed (its state +16 = {})", a, Rd(a + cas_goto::kPState));
  else if (p != g_page) busy = cas_goto::Busy(h, p);
  else if (Rd(h + cas_goto::kHState) != cas_goto::kPageIdle || Rd(h + cas_goto::kHMessage) || Rd(h + cas_goto::kHTransition))
    busy = fmt::format("the host is still moving on the item page (H+16 = {}, H+972 = {}, H+984 = {})", Rd(h + cas_goto::kHState),
                       Rd(h + cas_goto::kHMessage), Rd(h + cas_goto::kHTransition));
  if (busy.empty()) busy = cas_goto::PreviewNotReady(call);
  if (!busy.empty()) {
    if (frame - g_since > cas_goto::kStepFrames)
      Stop(fmt::format("not settled {} frames after the commit of item {}: {}", cas_goto::kStepFrames, g_k, busy), Reason::kCloseWait, frame);
    return;
  }
  const Landing where = LandingOf(p, g_page, g_parent, g_grand);
  Stop(fmt::format("applied item {}/{} ({} step(s), the panel's commit): the item page {:08X}'s overlay closed accepted and the editor is on {} ({}); "
                   "the preview is ready",
                   g_k, g_n, g_steps, g_page, cas_goto::PageId(p), LandingName(where)),
       Reason::kNone, frame, where);
}

// Armed: wait for an item page's parts panel up and idle and the preview ready (the go-to's arrival rules), then refuse or plan.
inline void StartStep(scene_director::Caller& call, uint32_t h, uint64_t frame) {
  if (!h) {  // the editor is not up: wait, unbounded (a go-to walking now holds the editor's presses: this pick was refused busy at Arm)
    g_since = frame;
    return;
  }
  const uint32_t p = Rd(h + cas_goto::kHPage);
  // S110 CASROW: the panel a press reaches now (cas_row_core.h FindTop): the item page's parts panel, or the logo list P2 under the LOGOS page.
  const cas_row::Top top = cas_row::FindTop([](uint32_t x) { return Rd(x); }, h);
  const bool moving = top.kind == cas_row::TopKind::kBusy || top.kind == cas_row::TopKind::kBox;
  if (top.kind == cas_row::TopKind::kBox && Rd(top.obj + cas_row::kBoxState) == 2) {  // CASROW review: a box waiting for its answer takes every press
    Stop(fmt::format("REFUSED: {}: it waits for its answer and nothing below it takes a press; answer it (wwe2k14_cas_row ok / yes / no), then arm the "
                     "pick",
                     top.why),
         Reason::kPanelBusy, frame);
    return;
  }
  if (!moving && top.kind != cas_row::TopKind::kPanel) {
    Stop(fmt::format("REFUSED: not on a parts panel: the editor shows {} ({}; {}); the pick drives the parts panel (vtable {:08X}) of an item page "
                     "(vtable {:08X}) or the logo list under the LOGOS page: arm it after the go-to's arrival on one",
                     cas_row::TopName(top.kind), cas_goto::PageId(p), top.why, cas_goto::kPartsPanelVtable, cas_goto::kItemPageVtable),
         Reason::kNotItemPage, frame);
    return;
  }
  const uint32_t in = moving ? 0 : top.obj, a = moving ? 0 : top.owner;
  const bool nested = !moving && Rd(a) == cas_row::kEditPageVtable;
  // a nested panel's own idle word stands in for the container's (PanelNotReady reads +16 of its first argument)
  std::string why = moving ? top.why : cas_goto::PanelNotReady(call, nested ? in : a, in);
  if (why.empty()) why = cas_goto::PreviewNotReady(call);
  if (!why.empty()) {
    if (frame - g_since > cas_goto::kStepFrames)
      Stop(fmt::format("not settled {} frames after arming: {} (item page {:08X})", cas_goto::kStepFrames, why, p), Reason::kPanelBusy, frame);
    return;
  }
  const uint32_t list = Rd(in + kPList), n = list ? Rd(list + kLCount) : 0, mode = Rd(in + kPMode), cat = Rd(in + kPCategory);
  if (!(PlainAccept(mode) && !nested) && !NestedLogoCommit(nested, mode, cat, g_edit)) {
    Stop(fmt::format("REFUSED: item page {:08X} (label {}): the {} parts panel's mode P+980 = {} (category {}) makes its A {}{}", p, Rd(p + cas_goto::kPLabel),
                     nested ? "nested" : "item page's", mode, cat, ModeName(mode),
                     !nested ? ", not a plain accept"
                     : g_edit ? "; edit mode is not driven on a nested panel (mode 3's A opens the placement tool, not read)"
                              : "; a nested panel is committed only as the logo list (mode 3, categories 49 / 50 / 51)"),
         Reason::kNotPlain, frame);
    return;
  }
  if (!list || n == 0 || g_k > n) {
    Stop(fmt::format("REFUSED: item {} is not on the panel: it lists {} item(s) (list {:08X}, its +20)", g_k, n, list), Reason::kOutOfRange, frame);
    return;
  }
  const uint32_t i = g_k - 1, locks = Rd(list + kLLocks), lock = locks ? Rd(locks + 4 * i) : 0;
  if (lock) {
    Stop(fmt::format("REFUSED: item {}/{} is locked ([[L+212] + 4 * {}] = {}): the panel neither puts it on (sub_833FD870) nor accepts it "
                     "(sub_833FD768); nothing was pressed",
                     g_k, n, i, lock),
         Reason::kLocked, frame);
    return;
  }
  for (uint32_t off : {kPMsg1, kPMsg2, kPMsg3}) {
    const uint32_t msg = i < kMsgWords ? Rd(in + off + 4 * i) : 0;
    if (msg) {
      Stop(fmt::format("REFUSED: item {}/{}: the panel's A shows message {} instead (P+{} + 4 * {}: sub_833F8A68); nothing was pressed", g_k, n, msg,
                       off, i),
           Reason::kMessage, frame);
      return;
    }
  }
  g_page = p, g_cont = a, g_panel = in, g_n = n, g_nested = nested;
  g_parent = Rd(p + cas_goto::kPParent);
  g_grand = g_parent ? Rd(g_parent + cas_goto::kPParent) : 0;
  const uint32_t cur = call(sub_833F8B08, {uint64_t(in)});
  const Plan plan = PlanSteps(cur, g_k, n);
  REXLOG_INFO("[wwe2k14][cas_pick] item {}: item page {:08X} (label {}), parts panel {:08X}: the cursor is on item {}/{}; {} step(s) {} (mode {}, "
              "category {}) (frame {})",
              g_k, p, Rd(p + cas_goto::kPLabel), in, cur + 1, n, plan.steps, plan.dir == kDown ? "down" : "up", mode, cat, frame);
  g_phase = Phase::kStep;
  g_since = frame;
  MoveStep(call, frame);
}

inline void Step(PPCContext& ctx, uint8_t* base, uint64_t frame) {
  if (g_phase == Phase::kOff) {  // the first frame: the value the run started with (one string read under the registry's lock)
    g_changes = rex::cvar::ChangeCount();
    Arm(rex::cvar::GetFlagByName("wwe2k14_cas_pick"), frame);
    return;
  }
  if (g_phase == Phase::kDone) {  // idle / applied / stopped: one atomic load a frame until some setting changes
    const uint64_t changes = rex::cvar::ChangeCount();
    if (changes == g_changes) return;
    g_changes = changes;
    const std::string arg = rex::cvar::GetFlagByName("wwe2k14_cas_pick");  // locked: a script's `cvar` line may be setting it
    if (NewValue(g_taken, arg)) Arm(arg, frame);
    return;
  }
  const uint32_t h = scenario::SequenceId() == cas_goto::kSeqCas ? cas_goto::Host() : 0;
  scene_director::Caller call(ctx, base);
  if (g_phase == Phase::kStart) {
    StartStep(call, h, frame);
    return;
  }
  // S110 CASROW: a go-to armed while the pick drives is refused busy (src/cas_driver.h), so no go-to walks this panel mid-pick.
  if (!h) {
    Stop(fmt::format("the editor's page host went away (sequence {}) before the pick ended", scenario::SequenceId()), Reason::kPanelGone, frame);
    return;
  }
  if (g_phase == Phase::kClose) {
    CloseStep(call, h, frame);
    return;
  }
  const bool held = g_nested ? Rd(g_cont) == cas_row::kEditPageVtable && Rd(g_cont + cas_row::kSChild) == g_panel  // the LOGOS page still holds its list
                             : Rd(h + cas_goto::kHOverlayA) == g_cont && Rd(g_cont + cas_goto::kContainerInner) == g_panel;
  if (Rd(h + cas_goto::kHPage) != g_page || !held) {
    Stop(fmt::format("the item page {:08X}, its overlay {:08X} or its panel {:08X} went away before the commit (now {}, overlay {:08X})", g_page, g_cont,
                     g_panel, cas_goto::PageId(Rd(h + cas_goto::kHPage)), Rd(h + cas_goto::kHOverlayA)),
         Reason::kPanelGone, frame);
    return;
  }
  if (g_phase == Phase::kStep)
    MoveStep(call, frame);
  else if (g_phase == Phase::kEdit)
    EditStep(call, frame);
  else
    ApplyStep(call, frame);
}

}  // namespace wwe2k14::cas_pick
