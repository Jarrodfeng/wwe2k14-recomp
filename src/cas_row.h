// S110 CASROW: `wwe2k14_cas_row=<label id>` takes the row with that game label on the Create a Superstar editor's CURRENT edit page (the
// part's edit page S, the LOGOS page S2, any S-class page: src/cas_row_core.h FindTop), through that page's own handlers, and waits until
// the editor has settled on whatever the game shows next; `back` is the page's own B; `yes` / `no` / `ok` answer the message box that is
// up. TEST / TOOLING PATH ONLY: a route arms it with a `cvar` line (docs\research\s110\CASROW.md: the logo route).
//
// All read from the listings (CASROW.md claims):
//   - rows: the edit page's element table [S+656] and base S+1108; row i is the game's element getter sub_823BF6B8([S+656], [S+1108] + i)
//     and its label the element's word +180 (sub_83407298 +274..+28C / +30C..+31C); S+52 rows (mixin+24), the cursor S+48 (mixin+20), the
//     wrap word S+56 (mixin+28), the rows-on word S+60 (mixin+32); kind S+948; a kind-1 row's enable word S+956 + 4 * row.
//   - a cursor step: the pressed word mixin+56 around the mixin's dispatch sub_823BC8C0 (no action-table bit -> slot 37 = 1 -> slot 4,
//     the generic move sub_823BC968: the row, the move sound slot 10, then S's slot 24 sub_83407818), the pad's own path, as src/cas_pick.h.
//   - A: the pad A entry sub_823BC7C0(S+28): enable slot 35 (1 on S), A slot 22 sub_83407298, the sound. B: sub_823BC838(S+28): enable slot
//     36 (1), slot 23 sub_8286B6E8 -> S vslot 7 sub_833B3AF8 (closed not accepted: +16 = 4).
//   - the box: its mixin box+28; LEFT / RIGHT the same dispatch with the pad table's +12 / +4 bit (the column box+64); A sub_823BC7C0(box+28):
//     enable slot 35 sub_8291F7C8 (box+1856), slot 22 -> box vslot 6 sub_8291F7D8 (+16 = 3, +20 = 1).
// What the verb writes: only the pressed word mixin+56 around a dispatch call (the mixin's update rewrites it every frame). Every wait is
// bounded by the go-to's step bound (900 frames) from its own start. Off: one atomic load a frame. One driver at a time (src/cas_driver.h).
// States: cas_row (0 idle, 1 working, 2 settled, 3 stopped / refused, 4 a box waits), cas_row_label (the request, stored last),
// cas_row_reason, cas_row_top, cas_row_kind, cas_row_cat, cas_row_box. Game thread only: from src/cas_launch.h Tick, after the pick.
#pragma once

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "cas_driver.h"    // the one owner of the editor's presses
#include "cas_goto.h"      // Host, Busy, PreviewNotReady, Rd, Wr, kStepFrames, sub_823BC7C0 / sub_823BC838
#include "cas_row_core.h"  // FindTop, the rows, the plans, the route states
#include "game_state.h"    // AddHostStateSource
#include "guest_call.h"    // Caller
#include "scenario.h"      // Frame, SequenceId (and fmt)

REXCVAR_DECLARE(std::string, wwe2k14_cas_row);  // defined in src/cvars/cvars_5.cpp (tools\compile\cvar_shard.py: crc32 % 6 = 5)

REX_EXTERN(sub_823BF6B8);  // the element getter (r3 = an element table, r4 = index): the 192-byte element or 0; reads only
REX_EXTERN(sub_823BC8C0);  // the input mixin's dispatch (r3 = mixin), as its update calls it with the pressed word mixin+56

namespace wwe2k14::cas_row {

using cas_goto::Rd;
using cas_goto::Wr;

constexpr uint32_t kSRow = 48, kSCount = 52, kSWrap = 56, kSRowsOn = 60, kSElemTable = 656, kSElemBase = 1108, kSRowOn = 956, kElemLabel = 180;
constexpr uint32_t kMPressed = 56, kMPadSlot = 12, kPadTable = 0x8200F510, kPadUp = 0, kPadDown = 8, kPadRight = 4, kPadLeft = 12;
constexpr uint32_t kMColumnsOn = 48, kMColumns = 40;  // the box mixin's column words (mixin+48 on, mixin+40 count: sub_823BC968 +120..+134)
constexpr uint32_t kPCategory = 964, kPMode = 980, kPPending = 984, kPStepWord = 1032, kMaxRows = 16;

enum class Phase { kOff, kStart, kMove, kSettle, kDone };
inline Phase g_phase = Phase::kOff;
inline Request g_req;
inline uint32_t g_obj = 0;   // the object the verb presses (an edit page, or the box)
inline uint32_t g_row = 0;   // the row asked (0-based)
inline uint32_t g_steps = 0;
inline uint64_t g_since = 0;
inline std::string g_taken;
inline uint64_t g_changes = 0;
inline Report g_report;
inline std::atomic<uint32_t> g_pub[kFieldEnd];

// One state line per change; the request code is stored LAST: a route that has waited on cas_row_label L reads that request's fields.
inline void Publish(const Report& r, uint64_t frame) {
  if (r == g_report) return;
  g_report = r;
  for (uint32_t f : {uint32_t(kFieldReason), uint32_t(kFieldTop), uint32_t(kFieldKind), uint32_t(kFieldCategory), uint32_t(kFieldBox)})
    g_pub[f].store(FieldValue(r, f), std::memory_order_release);
  g_pub[kFieldStatus].store(FieldValue(r, kFieldStatus), std::memory_order_release);
  g_pub[kFieldLabel].store(FieldValue(r, kFieldLabel), std::memory_order_release);
  REXLOG_INFO("[wwe2k14][cas_row] state cas_row={} cas_row_label={} cas_row_reason={} ({}) cas_row_top={} cas_row_kind={} cas_row_cat={} cas_row_box={} (frame {})",
              uint32_t(r.status), r.request, uint32_t(r.reason), ReasonName(r.reason), uint32_t(r.top), r.kind, r.category, r.box, frame);
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

inline Top Now(uint32_t h) {
  return FindTop([](uint32_t a) { return Rd(a); }, h);
}
// An edit page's rows, through the game's own element getter (kind 1 also reads each row's enable word).
inline std::vector<Row> RowsOf(scene_director::Caller& call, uint32_t s) {
  std::vector<Row> rows;
  const uint32_t n = Rd(s + kSCount), table = Rd(s + kSElemTable), base = Rd(s + kSElemBase), kind = Rd(s + kSKind);
  for (uint32_t i = 0; i < n && i < kMaxRows; ++i) {
    const uint32_t el = table ? call(sub_823BF6B8, {uint64_t(table), uint64_t(base + i)}) : 0;
    Row r;
    r.label = el ? Rd(el + kElemLabel) : 0;
    r.on = kind != 1 || Rd(s + kSRowOn + 4 * i) != 0;
    rows.push_back(r);
  }
  return rows;
}
// The object in one line, for every arrival and every stop: what a route asserts and a human reads.
inline std::string Describe(scene_director::Caller& call, const Top& t) {
  switch (t.kind) {
    case TopKind::kEditPage:
      return fmt::format("edit page {:08X} (vtable {:08X}, kind {}, held by {:08X}, depth {}): rows [{}]", t.obj, Rd(t.obj), Rd(t.obj + kSKind), t.owner,
                         t.depth, RowList(RowsOf(call, t.obj), Rd(t.obj + kSRow)));
    case TopKind::kPanel:
      return fmt::format("parts panel {:08X} (category {}, mode {}, held by {:08X} vtable {:08X}, depth {})", t.obj, Rd(t.obj + kPCategory),
                         Rd(t.obj + kPMode), t.owner, Rd(t.owner), t.depth);
    case TopKind::kBox:
      return fmt::format("message box {:08X} (state {}, text id {}, kind {}, column {})", t.obj, Rd(t.obj + kBoxState), Rd(t.obj + kBoxText),
                         Rd(t.obj + kBoxKind), Rd(t.obj + kBoxColumn));
    case TopKind::kList: return cas_goto::PageId(t.obj);
    default: return fmt::format("{} ({})", TopName(t.kind), t.why);
  }
}

inline void Stop(const std::string& what, Reason why, uint64_t frame) {
  REXLOG_WARN("[wwe2k14][cas_row] request {}: STOPPED: {} (frame {})", RequestCode(g_req), what, frame);
  Report r = g_report;
  Halt(r, why);
  cas_driver::Release(cas_driver::kRow);
  g_phase = Phase::kDone;
  Publish(r, frame);
}
inline void End(scene_director::Caller& call, const Top& t, uint64_t frame) {
  Report r = g_report;
  if (t.kind == TopKind::kBox) {
    BoxUp(r, Rd(t.obj + kBoxText));
    REXLOG_INFO("[wwe2k14][cas_row] request {}: a message box is up and waits for its answer: {} (answer with wwe2k14_cas_row yes / no / ok) (frame {})",
                RequestCode(g_req), Describe(call, t), frame);
  } else {
    Settled(r, t.kind, t.kind == TopKind::kEditPage ? Rd(t.obj + kSKind) : 0, t.kind == TopKind::kPanel ? Rd(t.obj + kPCategory) : 0);
    REXLOG_INFO("[wwe2k14][cas_row] request {}: settled on {}; the preview is ready (frame {})", RequestCode(g_req), Describe(call, t), frame);
  }
  cas_driver::Release(cas_driver::kRow);
  g_phase = Phase::kDone;
  Publish(r, frame);
}

inline void Arm(const std::string& arg, uint64_t frame) {
  cas_driver::Release(cas_driver::kRow);
  g_taken = arg;
  g_phase = Phase::kDone;
  g_obj = g_row = g_steps = 0;
  g_since = frame;
  g_req = Request{};
  if (arg.empty()) {
    Publish(Report{}, frame);
    return;
  }
  const std::string bad = ParseRequest(arg, g_req);
  if (!bad.empty()) {
    REXLOG_WARN("[wwe2k14][cas_row] wwe2k14_cas_row={}: {}; off", arg, bad);
    Report r;
    Halt(r, Reason::kBadArg);
    Publish(r, frame);
    return;
  }
  if (!cas_driver::Claim(cas_driver::kRow)) {
    REXLOG_WARN("[wwe2k14][cas_row] wwe2k14_cas_row={}: REFUSED: {} drives the editor now (one driver at a time: set the row after it has ended)", arg,
                cas_driver::Name(cas_driver::g_owner));
    Report r = Armed(RequestCode(g_req));
    Halt(r, Reason::kBusy);
    Publish(r, frame);
    return;
  }
  REXLOG_INFO("[wwe2k14][cas_row] wwe2k14_cas_row={}: waiting for the editor's top object to be ready (frame {})", arg, frame);
  g_phase = Phase::kStart;
  Publish(Armed(RequestCode(g_req)), frame);
}

// Ready to take a press: an edit page on its rows and idle, or the box waiting; and the preview ready.
inline std::string NotReady(scene_director::Caller& call, const Top& t) {
  if (t.kind == TopKind::kEditPage && Rd(t.obj + kState) != 2)
    return fmt::format("the edit page {:08X} is not idle (+16 = {}, 2 when its in-transition has ended)", t.obj, Rd(t.obj + kState));
  if (t.kind == TopKind::kBox && Rd(t.obj + kBoxState) != 2) return fmt::format("the message box is not waiting yet (+16 = {})", Rd(t.obj + kBoxState));
  return cas_goto::PreviewNotReady(call);
}

// Armed: wait for a top object that takes this request, then refuse or plan.
inline void StartStep(scene_director::Caller& call, uint32_t h, uint64_t frame) {
  const Top t = Now(h);
  const bool answer = g_req.ask == Ask::kYes || g_req.ask == Ask::kNo || g_req.ask == Ask::kOk;
  std::string wait;
  if (t.kind == TopKind::kBusy || t.kind == TopKind::kNone) wait = t.why;
  else if (t.kind == TopKind::kEditPage || t.kind == TopKind::kBox) wait = NotReady(call, t);
  if (!wait.empty()) {
    if (frame - g_since > cas_goto::kStepFrames)
      Stop(fmt::format("not ready {} frames after arming: {}", cas_goto::kStepFrames, wait), t.kind == TopKind::kNone ? Reason::kHostGone : Reason::kNotReady,
           frame);
    return;
  }
  if (!answer && t.kind == TopKind::kBox) {  // a box is on top: nothing below it takes a press until it is answered
    End(call, t, frame);
    return;
  }
  if (answer) {
    if (t.kind != TopKind::kBox) {
      Stop(fmt::format("REFUSED: no message box is up; the editor shows {}", Describe(call, t)), Reason::kNotServed, frame);
      return;
    }
    const uint32_t box = t.obj, kind = Rd(box + kBoxKind), col = Rd(box + kBoxColumn), m = box + kMixin;
    if (Rd(m) != kBoxMixinVtable || PlanBox(kind, col, g_req.ask) == BoxMove::kRefuse || !Rd(box + kBoxAEnable)) {
      Stop(fmt::format("REFUSED: {}: the answer {} does not fit it (kinds 0 / 1 take ok, 2 / 3 take yes / no, 4..7 are not answered), or its A is off "
                       "(box+1856 = {}), or its mixin {:08X} is not {:08X}",
                       Describe(call, t), RequestCode(g_req), Rd(box + kBoxAEnable), Rd(m), kBoxMixinVtable),
           Reason::kBoxRefused, frame);
      return;
    }
    REXLOG_INFO("[wwe2k14][cas_row] request {}: answering {} (frame {})", RequestCode(g_req), Describe(call, t), frame);
    g_obj = box;
    g_phase = Phase::kMove;
    g_since = frame;
    return;
  }
  if (t.kind != TopKind::kEditPage) {
    Stop(fmt::format("REFUSED: the editor's top object is {}: this verb drives the edit pages' rows (and their B) only", Describe(call, t)), Reason::kNotServed,
         frame);
    return;
  }
  const uint32_t s = t.obj, kind = Rd(s + kSKind);
  if (Rd(s + kMixin) != kEditMixinVtable) {
    Stop(fmt::format("REFUSED: {}: its mixin S+28 has vtable {:08X}, not {:08X} (whose A / B / move this verb calls)", Describe(call, t), Rd(s + kMixin),
                     kEditMixinVtable),
         Reason::kNotServed, frame);
    return;
  }
  if (g_req.ask == Ask::kRow) {
    if (!KindByLabel(kind)) {
      Stop(fmt::format("REFUSED: {}: kind {}'s A does not go by the row's label (kind 0 accepts on every row, kind 3 passes S+360; sub_83407298)",
                       Describe(call, t), kind),
           Reason::kNotServed, frame);
      return;
    }
    const std::vector<Row> rows = RowsOf(call, s);
    const int row = FindRow(rows, g_req.label);
    if (row < 0) {
      Stop(fmt::format("REFUSED: no row labelled {} on {}", g_req.label, Describe(call, t)), Reason::kNoRow, frame);
      return;
    }
    if (!Rd(s + kSRowsOn) || !Rd(s + kSCount)) {
      Stop(fmt::format("REFUSED: {}: its generic move is off (rows-on S+60 = {}, rows S+52 = {})", Describe(call, t), Rd(s + kSRowsOn), Rd(s + kSCount)),
           Reason::kNotServed, frame);
      return;
    }
    if (!rows[size_t(row)].on)
      REXLOG_INFO("[wwe2k14][cas_row] request {}: row {} is off (S+956 + 4 * {} = 0): its A shows the game's message 50064 (frame {})", g_req.label, row,
                  row, frame);
    g_row = uint32_t(row);
  }
  REXLOG_INFO("[wwe2k14][cas_row] request {}: on {} (frame {})", RequestCode(g_req), Describe(call, t), frame);
  g_obj = s;
  g_phase = Phase::kMove;
  g_since = frame;
  g_steps = 0;
}

// One dispatch through `m` with the pad bit at table offset `pad` (the mixin's own pressed word, put back after).
inline uint32_t Press(scene_director::Caller& call, uint32_t m, uint32_t pad) {
  const uint32_t bit = Rd(kPadTable + (Rd(m + kMPadSlot) << 4) + pad), pressed = Rd(m + kMPressed);
  Wr(m + kMPressed, bit);
  call(sub_823BC8C0, {uint64_t(m)});
  Wr(m + kMPressed, pressed);
  return bit;
}

// One press a frame: cursor steps, then the A (or the B, or the box's column then its A).
inline void MoveStep(scene_director::Caller& call, uint32_t h, uint64_t frame) {
  const Top t = Now(h);
  if (t.obj != g_obj || !NotReady(call, t).empty()) {
    Stop(fmt::format("the object the verb was pressing ({:08X}) is no longer the ready top: the editor shows {}", g_obj, Describe(call, t)), Reason::kNoChange,
         frame);
    return;
  }
  const uint32_t o = g_obj, m = o + kMixin;
  if (t.kind == TopKind::kBox) {
    const uint32_t col = Rd(o + kBoxColumn);
    const BoxMove mv = PlanBox(Rd(o + kBoxKind), col, g_req.ask);
    if (mv == BoxMove::kLeft || mv == BoxMove::kRight) {
      if (!Rd(m + kMColumnsOn) || !Rd(m + kMColumns) || ++g_steps > 2) {
        Stop(fmt::format("the box's column cannot be moved (mixin+48 = {}, mixin+40 = {}, {} presses)", Rd(m + kMColumnsOn), Rd(m + kMColumns), g_steps),
             Reason::kStepStuck, frame);
        return;
      }
      const uint32_t bit = Press(call, m, mv == BoxMove::kLeft ? kPadLeft : kPadRight);
      if (Rd(o + kBoxColumn) == col)
        Stop(fmt::format("a {} press (pad bit {:08X}) left the box's column on {}", mv == BoxMove::kLeft ? "LEFT" : "RIGHT", bit, col), Reason::kStepStuck,
             frame);
      return;
    }
    REXLOG_INFO("[wwe2k14][cas_row] request {}: A on the message box (text id {}, kind {}, column {}) (frame {})", RequestCode(g_req), Rd(o + kBoxText),
                Rd(o + kBoxKind), col, frame);
    call(sub_823BC7C0, {uint64_t(m)});
    if (Rd(o + kBoxState) != 3) {
      Stop(fmt::format("the box's A did not answer it (+16 = {}, 3 when answered)", Rd(o + kBoxState)), Reason::kNoChange, frame);
      return;
    }
    g_phase = Phase::kSettle;
    g_since = frame;
    return;
  }
  if (g_req.ask == Ask::kBack) {
    REXLOG_INFO("[wwe2k14][cas_row] request back: B on the edit page {:08X} (kind {}) (frame {})", o, Rd(o + kSKind), frame);
    call(sub_823BC838, {uint64_t(m)});
    if (Rd(o + kState) == 2) {
      Stop("the edit page's B changed nothing (+16 = 2)", Reason::kNoChange, frame);
      return;
    }
    g_phase = Phase::kSettle;
    g_since = frame;
    return;
  }
  const uint32_t n = Rd(o + kSCount), cur = Rd(o + kSRow);
  const bool wrap = Rd(o + kSWrap) != 0;
  const RowPlan plan = PlanRow(cur, g_row, n, wrap);
  if (plan.steps) {
    if (++g_steps > 2 * n + 2) {
      Stop(fmt::format("{} presses and the cursor is on row {} of {}, not {}", g_steps, cur, n, g_row), Reason::kStepStuck, frame);
      return;
    }
    const uint32_t bit = Press(call, m, plan.dir == kDown ? kPadDown : kPadUp);
    const uint32_t now = Rd(o + kSRow), want = NextRow(cur, plan.dir, n, wrap);
    if (now != want)
      Stop(fmt::format("a {} press (pad bit {:08X}) from row {} landed on row {} of {}, not {}", plan.dir == kDown ? "down" : "up", bit, cur, now, n, want),
           Reason::kStepStuck, frame);
    return;
  }
  REXLOG_INFO("[wwe2k14][cas_row] request {}: A on row {} (label {}) of the edit page {:08X} (kind {}) after {} cursor press(es) (frame {})", g_req.label, cur,
              g_req.label, o, Rd(o + kSKind), g_steps, frame);
  call(sub_823BC7C0, {uint64_t(m)});
  if (Rd(o + kSStage) == 0 && Rd(o + kState) == 2) {
    Stop(fmt::format("the A on row {} changed nothing (S+952 = 0, S+16 = 2)", cur), Reason::kNoChange, frame);
    return;
  }
  g_phase = Phase::kSettle;
  g_since = frame;
}

// After the press: the game's own update takes it (CASROW.md "After our last action"); wait until the editor shows a settled top.
inline void SettleStep(scene_director::Caller& call, uint32_t h, uint64_t frame) {
  const Top t = Now(h);
  std::string wait;
  switch (t.kind) {
    case TopKind::kBusy: wait = t.why; break;
    case TopKind::kNone: Stop("the editor's page host went away after the press", Reason::kHostGone, frame); return;
    case TopKind::kBox:
      if (Rd(t.obj + kBoxState) != 2) wait = fmt::format("the message box is moving (+16 = {})", Rd(t.obj + kBoxState));
      break;
    case TopKind::kEditPage: wait = NotReady(call, t); break;
    case TopKind::kPanel:
      if (Rd(t.obj + kState) != 2 || Rd(t.obj + kPPending) || Rd(t.obj + kPStepWord))
        wait = fmt::format("the parts panel {:08X} is not idle (+16 = {}, P+984 = {}, P+1032 = {})", t.obj, Rd(t.obj + kState), Rd(t.obj + kPPending),
                           Rd(t.obj + kPStepWord));
      break;
    case TopKind::kList: wait = cas_goto::Busy(h, t.obj); break;
    default: break;  // an item page's own rows or a class not read: published as built (Describe names it)
  }
  if (wait.empty() && t.kind != TopKind::kBox) wait = cas_goto::PreviewNotReady(call);
  if (!wait.empty()) {
    if (frame - g_since > cas_goto::kStepFrames)
      Stop(fmt::format("not settled {} frames after the press: {}", cas_goto::kStepFrames, wait), Reason::kSettleWait, frame);
    return;
  }
  End(call, t, frame);
}

inline void Step(PPCContext& ctx, uint8_t* base, uint64_t frame) {
  if (g_phase == Phase::kOff) {  // the first frame: the value the run started with
    g_changes = rex::cvar::ChangeCount();
    Arm(rex::cvar::GetFlagByName("wwe2k14_cas_row"), frame);
    return;
  }
  if (g_phase == Phase::kDone) {  // idle / settled / stopped: one atomic load a frame until some setting changes
    const uint64_t changes = rex::cvar::ChangeCount();
    if (changes == g_changes) return;
    g_changes = changes;
    const std::string arg = rex::cvar::GetFlagByName("wwe2k14_cas_row");
    if (NewValue(g_taken, arg)) Arm(arg, frame);
    return;
  }
  const uint32_t h = scenario::SequenceId() == cas_goto::kSeqCas ? cas_goto::Host() : 0;
  scene_director::Caller call(ctx, base);
  if (g_phase == Phase::kStart) {
    StartStep(call, h, frame);
    return;
  }
  if (!h) {
    Stop(fmt::format("the editor's page host went away (sequence {})", scenario::SequenceId()), Reason::kHostGone, frame);
    return;
  }
  if (g_phase == Phase::kMove)
    MoveStep(call, h, frame);
  else
    SettleStep(call, h, frame);
}

}  // namespace wwe2k14::cas_row
