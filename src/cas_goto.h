// S110 CASNAV: `--wwe2k14_cas_goto=<record id>` opens any page of the Create a Superstar editor by its flow record (holder slot 1, the
// table src/cas_launch.h dumps), with the game's OWN handlers called from the owner update, one level per settled page, and logs the
// arrival from the editor's own words (docs\research\s110\CASNAV.md). TEST / TOOLING PATH ONLY: reached by the switch, never by play.
//
// The editor's words (all read from the listings):
//   H = [seq+76], vtable 0x821BB248 (ctor sub_833BE878, built by sub_83395F50): the editor's page host. H+948 = the current page;
//   H+940 = the tab bar T (T+940 = the tab 0..3, T+936 = tabs allowed); H+972 / +976 / +984 / +952 / +956 / +1004 = message flow, tab change,
//   page transition, two overlays, overlay pending: all 0 when nothing is moving (sub_833BFA48, sub_833BEBF8).
//   A list page P (vtable 0x821BB3E8, ctor sub_833C16C0 -> sub_833DE360): P+968 its record's label, P+972 its row count, P+976 its rows
//   (148-byte copies: +0 row kind, +4 label), P+984 its child pages (by row), P+980 its parent page (0 = the tab's root), P+988 its depth,
//   P+48 the cursor row (its input mixin at P+28: mixin+20), P+992 the page's result (0 = none; the list's enter sub_833DE800 clears it),
//   P+16 == 2 the state the host waits for before it calls a new tab root settled (sub_833BFA48 +8F0).
// The moves (the game's own functions, called the way the game calls them):
//   enter a row: the pad A bit's own entry sub_823BC7C0(P+28) (action table 0x8200F580, bit 0x10): the mixin's enable test (slot 35
//     sub_833C1BE8 -> sub_833C1A20: rows labelled HEAD / CRANIUM / HEIGHT / SIZE / SHAPE are refused while [root+4592] is set, and drawn
//     disabled by the page's enter sub_833C1C30 +2D0), then the A handler (slot 22 sub_833C1830: reads the row P+48 and, for a row of kind
//     0, sets P+996 = the row and P+992 = 1), then the accept / refuse sound. The host's dispatcher sub_833BEBF8 then makes
//     H+948 = [P+984][row] with its transition;
//   back: the pad B bit's entry sub_823BC838(P+28) (enable slot 36 = 1, then slot 23 sub_833C14B0: P+992 = 2, 5 when the depth P+988 is 0:
//     exit the editor, so it is never sent at the root, where P+980 = 0 and P+988 = 0 together, sub_833DE7D0); the dispatcher makes
//     H+948 = P+980 and allows the tabs (sub_833C7060(T, 1));
//   tab: the dispatcher's own tab steps sub_833BE790(H, 0) (next) and sub_833BE6A8(H, 0) (previous), refused by the game when T+936 == 0.
// Refused before any call: a path through a template chooser (cas_goto_core.h RefusePath). A row is picked as the builder built it
// (PickRow with the superstar's bit, read with the builder's own getters sub_822D28A0 / sub_822D2AE0 / sub_822EC988).
// Row kinds (sub_833A0628 by code; the A handler by kind): kind 0 enters a page (codes 1001 / 1002 / 1003 / 1007 / 1008); a slider (1004),
// DEFAULT (1005), EDIT LAYERS (1006) or BODY TEMPLATES (1009) row opens an overlay or a message instead: the go-to puts the cursor on such a
// target row and sends no A.
// A NEW superstar's editor (S110 CASNAV fix1, run 227): the host's start (vslot 4 sub_833BE3B8, when root+8 / +24 / +4612 / +14224 are all 0)
// sets [root+4596] = 1 (template mode), turns the tabs off and makes H+948 the TEMPLATES list (row 1 of the HEAD root). Until its start ends
// (vslot 5 sub_833BE4D8 -> sub_833B3020: H+16 = 2) H+948 is still the HEAD root the ctor entered, which looks settled: Busy waits for H+16 == 2.
// In template mode the host's update (sub_833BFA48 +338..+754) takes the page results itself and never runs the dispatcher: B on TEMPLATES
// (result 2) LEAVES the editor (sub_8286B6E8, or the exit question 50055 when [root+4540] is set), so the go-to never sends B there; result 1
// enters the preset page [P+984][P+48] (sub_822D3520(0): a superstar's base body), and the preset page's own A (sub_833C4448, then its update
// sub_833C4050 applies the shown preset and sets result 12) makes the host rebuild tab 0 with the tabs on and [root+4596] = 0 (+5E8..+6AC).
// That pick is the game's mandatory first step for a NEW superstar (no save: in memory only; [root+4540] = 1, so the editor's exit asks):
// the go-to makes it, SUPERSTAR (label 6593, the constant the dispatcher tests at +128) and the preset the page shows, then goes on.
// Game thread only: called from src/cas_launch.h Tick (the owner update), before the game's own update of the frame.
#pragma once

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc/context.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "cas_goto_core.h"
#include "cas_driver.h"     // S110 CASROW: one owner of the editor's presses (first armed wins)
#include "game_state.h"  // S110 CASARRIVE: AddHostStateSource (the cas_* route states)
#include "guest_call.h"  // Caller
#include "scenario.h"    // U32, Guest, Frame, SequenceId, kSequenceManagerPtr (and fmt)

REXCVAR_DECLARE(std::string, wwe2k14_cas_goto);

REX_EXTERN(sub_823BC7C0);  // the pad A bit's entry (r3 = the page's input mixin, page+28): enable test slot 35, A handler slot 22, sound
REX_EXTERN(sub_823BC838);  // the pad B bit's entry (r3 = page+28): enable test slot 36, B handler slot 23 (sub_833C14B0), sound
REX_EXTERN(sub_822D28A0);  // [[0x836F2DB8]+16] or 0 (reads only): the page builder's +28 gate is on when non-zero
REX_EXTERN(sub_822D2AE0);  // [[0x836F2DB8]+300] when +16 is set, else 0 (reads only)
REX_EXTERN(sub_822EC988);  // bit 0 of the byte +48 of [[r3+68]+48] (reads only): the superstar's bit the builder keeps 6561 / 6562 rows by
REX_EXTERN(sub_833BE790);  // the editor host's next-tab step (r3 = H, r4 = 0), as its dispatcher calls it for page result 3
REX_EXTERN(sub_833BE6A8);  // the editor host's previous-tab step (r3 = H, r4 = 0), page result 4
REX_EXTERN(sub_833C3F38);  // the preset page mixin's enable test (vslot 35, r3 = page+28): the row test and the model ready (sub_822D38A0)
REX_EXTERN(sub_833FC6B8);  // the parts panel mixin's B enable test (slot 36, r3 = panel+28): mixin+956 == 0 and the model ready (reads only)
REX_EXTERN(sub_822D2B40);  // [[0x836F2DB8]+308] when +16 is set, else 0 (reads only): the part data the panel's B tests
REX_EXTERN(sub_822DC1A8);  // that object ready (reads only): the parts panel's B does nothing while it is 0 (sub_834001F0 +80..+8C)
REX_EXTERN(sub_822D38A0);  // the superstar model ready (reads only; called only when [[0x836F2DB8]+16] != 0): S110 CASREADY

namespace wwe2k14::cas_goto {

constexpr uint32_t kSeqCas = 76, kSeqHost = 76;  // sub_83395F50 +EC: [seq+76] = the host
constexpr uint32_t kHostVtable = 0x821BB248;  // kListVtable, kItemPageVtable, kContainerVtable, kPartsPanelVtable: cas_goto_core.h
constexpr uint32_t kHTabBar = 940, kHPage = 948, kHOverlayA = 952, kHOverlayB = 956, kHMessage = 972, kHTabChange = 976, kHTransition = 984,
                   kHOverlayPending = 1004;
constexpr uint32_t kTAllowed = 936, kTIndex = 940;
constexpr uint32_t kPState = 16, kPMixin = 28, kPRow = 48, kPLabel = 968, kPCount = 972, kPRows = 976, kPChildren = 984, kPParent = 980,
                   kPDepth = 988, kPResult = 992;
constexpr uint32_t kRowKind = 0, kRowLabel = 4;
constexpr uint32_t kPageIdle = 2;
constexpr uint32_t kCawRootPtr = 0x839BFC7C, kRootMenu = 4588, kMState = 16, kMFlow = 1376, kFlowCount = 4, kFlowRecords = 8;
constexpr uint32_t kMaxRecords = 4096, kMaxRows = 64;
constexpr uint32_t kHState = 16;                // the host's own state: 2 once its start has ended (sub_833BE4D8 -> sub_833B3020)
constexpr uint32_t kRootPageSet = 4788, kPageSet512 = 512;  // [root+4788]: the page builder's first gate (cas_goto_core.h KeptByPageSet)
constexpr uint32_t kRootTemplateMode = 4596;    // [root+4596] != 0: template mode (sub_833BE3B8 +58; cleared at sub_833BFA48 +6A4)
constexpr uint32_t kPresetVtable = 0x821BB9B8;  // the preset page (code 1003, sub_833C4308 +8C)
constexpr uint32_t kPPresetState = 1108, kPPresetShown = 1124;  // its pick state (0 idle .. 4, then result 12) and the preset it shows
constexpr uint32_t kLabelTemplates = 6354, kLabelSuperstar = 6593;
constexpr uint64_t kStepFrames = 900;  // a step (or the first page) not settled by then is reported and the go-to stops

enum class Phase { kOff, kWait, kUp, kTab, kDown, kDone };
// Game thread only (the owner update), so no locking.
inline Phase g_phase = Phase::kOff;
inline uint32_t g_target = 0;
inline std::vector<Rec> g_recs, g_path;
inline int g_tab = -1;
inline size_t g_k = 0;              // the path index of the page we stand on (kDown)
inline uint32_t g_expect_page = 0;  // the page a sent step must land on (0 = any)
inline int g_expect_tab = -1;       // the tab a sent tab step must land on (-1 = none)
inline uint64_t g_since = 0;        // the frame of the last step (or of the editor's first frame)
inline bool g_sent = false;         // a step is in flight
inline bool g_picking = false;      // the preset's A has been sent: the template pick runs until the host leaves template mode
inline bool g_tpl_a = false;        // the step in flight is the A on SUPERSTAR of the TEMPLATES list (run 228: it can end the pick by itself)
inline uint32_t g_page_now = 0;     // the current page, named in every STOPPED line

inline bool g_picker_b = false;    // the step in flight is a parts panel's B (an item page's way up): settled only once the overlay is gone
inline std::string g_taken;        // the wwe2k14_cas_goto value taken last (a different value is a new target)
inline uint64_t g_changes = 0;     // rex::cvar::ChangeCount() when the setting was last looked at
inline Report g_report;            // S110 CASARRIVE: the route states (game thread); g_pub is what the script's reads see
inline std::atomic<uint32_t> g_pub[kFieldEnd];

// One state line per change; the script reads g_pub on another thread (src/game_state.h ReadGuestState), one field per read, so a route never
// sees two fields as one snapshot. The rule (S110 CASARRIVE review): level, reason and target are stored first, then cas_goto, then cas_record
// LAST (each a release store, each read an acquire load). So a route that has waited on cas_record N (it moves only on an arrival) reads
// every other field of that arrival; one that has waited on cas_goto 3 / 4 reads that stop's cas_reason / cas_level / cas_target. After a
// wait on cas_goto 2, cas_record may still be the old value for a moment: wait on cas_record for an arrival.
inline void Publish(const Report& r, uint64_t frame) {
  if (r == g_report) return;
  g_report = r;
  for (uint32_t f : {uint32_t(kFieldLevel), uint32_t(kFieldReason), uint32_t(kFieldTarget)})
    g_pub[f].store(FieldValue(r, f), std::memory_order_relaxed);
  g_pub[kFieldStatus].store(FieldValue(r, kFieldStatus), std::memory_order_release);
  g_pub[kFieldRecord].store(FieldValue(r, kFieldRecord), std::memory_order_release);
  REXLOG_INFO("[wwe2k14][cas_goto] state cas_goto={} cas_record={} cas_level={} cas_reason={} ({}) cas_target={} (frame {})", uint32_t(r.status),
              r.record, r.level, uint32_t(r.reason), ReasonName(r.reason), r.target, frame);
}
inline void EnteredLevel(uint64_t frame) {
  Report r = g_report;
  Entered(r);
  Publish(r, frame);
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

inline bool Plausible(uint32_t p) { return p >= 0x10000 && p < 0xC0000000u; }
inline uint32_t Rd(uint32_t a) { return Plausible(a) ? scenario::U32(a) : 0; }
inline void Wr(uint32_t a, uint32_t v) {
  if (Plausible(a)) rex::memory::store_and_swap<uint32_t>(scenario::Guest(a), v);
}

inline uint32_t Host() {
  const uint32_t mgr = Rd(scenario::kSequenceManagerPtr);
  const uint32_t seq = mgr ? Rd(mgr) : 0;
  const uint32_t h = seq ? Rd(seq + kSeqHost) : 0;
  return h && Rd(h) == kHostVtable ? h : 0;
}

// "" = nothing moving and the current page settled; else what is still moving (logged when a step does not settle).
inline std::string Busy(uint32_t h, uint32_t p) {
  if (Rd(h + kHState) != kPageIdle) return fmt::format("the host's start has not ended (H+16 = {}, 2 when sub_833BE4D8 has run)", Rd(h + kHState));
  if (!p) return "no current page (H+948 = 0)";
  if (Rd(h + kHMessage)) return fmt::format("message flow H+972 = {}", Rd(h + kHMessage));
  if (Rd(h + kHTabChange)) return fmt::format("tab change H+976 = {}", Rd(h + kHTabChange));
  if (Rd(h + kHTransition)) return fmt::format("page transition H+984 = {}", Rd(h + kHTransition));
  // Run 229 (S110): an ITEM page (record code 1002; vtable 0x821BBBA0 as that run's STOPPED line printed it for TOPS, label 6396) shows its
  // part picker as the host's overlay A (H+952) for as long as it is the current page ("1/51" on screen): that is the page's settled state,
  // not something still moving. So with only overlay A up on an item page and no result pending, the page is where a player would be.
  // [L]: that P+16 is not 2 there was not read; the picker being up is taken as the page's readiness.
  if (Rd(p) == kItemPageVtable && Rd(h + kHOverlayA) && !Rd(h + kHOverlayB) && !Rd(h + kHOverlayPending))
    return "";  // run 230: P+992 reads 7 on the item page for as long as it is shown: +992 is the LIST page's result word, not this class's
  if (Rd(h + kHOverlayA) || Rd(h + kHOverlayB) || Rd(h + kHOverlayPending))
    return fmt::format("an overlay is open (H+952 {:08X}, H+956 {:08X}, H+1004 {})", Rd(h + kHOverlayA), Rd(h + kHOverlayB), Rd(h + kHOverlayPending));
  if (Rd(p + kPResult)) return fmt::format("page result P+992 = {} not yet taken", Rd(p + kPResult));
  if (Rd(p + kPState) != kPageIdle) return fmt::format("page state P+16 = {} (the host waits for 2)", Rd(p + kPState));
  return "";
}

// Why the parts panel's B would be a no-op now ("" = it will be taken): the tests its B handler sub_834001F0 and the pad entry sub_823BC838
// make, read before the press (cas_goto_core.h kPartsPanelVtable). `a` = the container (overlay A), `in` = its panel (container+936).
constexpr uint32_t kPanelSubMode = 1000, kPadHeld = 60, kPadSlot = 12, kPanelPadMask = 0x8200F510;  // mixin words, sub_834001F0 +C..+58
inline std::string PanelNotReady(scene_director::Caller& call, uint32_t a, uint32_t in) {
  if (!in) return fmt::format("the overlay {:08X} has built no panel yet (+936 = 0; its state +16 = {})", a, Rd(a + kPState));
  if (Rd(a + kPState) != kPageIdle) return fmt::format("the overlay {:08X} is not idle (+16 = {})", a, Rd(a + kPState));
  if (Rd(in + kPState) != kPageIdle) return fmt::format("the parts panel {:08X} is not idle (+16 = {})", in, Rd(in + kPState));
  const uint32_t m = in + kPMixin;
  if (Rd(m + kPanelSubMode)) return fmt::format("the parts panel {:08X} is in a sub-mode (mixin+1000 = {}): its B would only leave that", in, Rd(m + kPanelSubMode));
  if (!call(sub_833FC6B8, {uint64_t(m)})) return fmt::format("the parts panel's B enable test (sub_833FC6B8) is 0 (mixin+956 = {})", Rd(m + 956));
  const uint32_t held = Rd(m + kPadHeld), row = kPanelPadMask + (Rd(m + kPadSlot) << 4);
  if ((Rd(row) & held) || (Rd(row + 8) & held)) return fmt::format("a pad bit the parts panel's B refuses is held (mixin+60 = {:08X})", held);
  const uint32_t parts = call(sub_822D2B40, {});
  if (!parts || !call(sub_822DC1A8, {uint64_t(parts)})) return fmt::format("the part data is not ready ({:08X}: sub_822DC1A8 = 0)", parts);
  return "";
}

// One page in one line: its vtable, label, depth and parent.
inline std::string PageId(uint32_t p) {
  return fmt::format("page {:08X} (vtable {:08X}, label {}, depth {}, parent {:08X})", p, Rd(p), Rd(p + kPLabel), Rd(p + kPDepth), Rd(p + kPParent));
}

// Ends the walk for this target: `why` = Reason::kNone is the arrival (an INFO line), else the STOPPED line (a refusal too), with its reason.
inline void Stop(const std::string& what, Reason why) {
  Report r = g_report;
  if (why != Reason::kNone) {
    REXLOG_WARN("[wwe2k14][cas_goto] record {}: STOPPED: {}; current {}; template mode {} (frame {})", g_target, what, PageId(g_page_now),
                Rd(Rd(kCawRootPtr) + kRootTemplateMode), scenario::Frame());
    Halt(r, why);
  } else {
    REXLOG_INFO("[wwe2k14][cas_goto] record {}: {} (frame {})", g_target, what, scenario::Frame());
    Arrive(r);
  }
  cas_driver::Release(cas_driver::kGoto);  // S110 CASROW: the walk has ended: the editor's presses are free
  g_phase = Phase::kDone;
  Publish(r, scenario::Frame());
}

// S110 CASREADY: the game's screen requester G (cas_goto_core.h PreviewAtArrival): its state, request and current screen words.
constexpr uint32_t kLoadingRequester = 0x83924744, kReqState = 4, kReqPending = 8, kReqScreen = 84;

// Why the preview is not ready yet ("" = ready): the superstar model's ready word, then the game's loading screen (the W spinner).
inline std::string PreviewNotReady(scene_director::Caller& call) {
  const bool model = !call(sub_822D28A0, {}) || call(sub_822D38A0, {});  // no superstar object (+16 = 0): no preview model to wait for
  const uint32_t g = Rd(kLoadingRequester);
  const uint32_t st = g ? Rd(g + kReqState) : 0, pend = g ? Rd(g + kReqPending) : 0, scr = g ? Rd(g + kReqScreen) : 0;
  switch (PreviewAtArrival(model, g, st, pend, scr)) {
    case PreviewWait::kModel:
      return "the preview model is not ready (sub_822D38A0 = 0: the superstar is still being rebuilt or loaded)";
    case PreviewWait::kLoadingScreen:
      return fmt::format("the game's loading screen is up (requester {:08X}: state +4 = {}, request +8 = {}, screen +84 = {:08X})", g, st, pend, scr);
    default:
      return "";
  }
}

// S110 SCENESRC + CASREADY: true when the arrival on page p may be published; false = not yet, or the go-to has stopped (not settled,
// once the step bound kStepFrames from the page's settle is spent). First an item page's panel (cas_goto_core.h PanelAtArrival, with
// PanelNotReady's words), then on every page the preview (PreviewNotReady). The INFO lines are written once, when the arrival is
// published: the panel waited for (or the class not waited for), then the preview.
inline bool ArrivalReady(scene_director::Caller& call, uint32_t h, uint32_t p, uint64_t frame) {
  const uint32_t a = Rd(h + kHOverlayA), in = a ? Rd(a + kContainerInner) : 0;
  const ArrivalPanel kind = PanelAtArrival(Rd(p), a ? Rd(a) : 0, in ? Rd(in) : 0);
  const bool panel = kind == ArrivalPanel::kBuild || kind == ArrivalPanel::kParts;
  const std::string why = panel ? PanelNotReady(call, a, in) : std::string();
  if (!why.empty()) {
    if (frame - g_since > kStepFrames)
      Stop(fmt::format("not settled {} frames after the last step: {} (page {:08X}; the arrival waits for the item page's panel)", kStepFrames, why, p),
           Reason::kNotSettled);
    return false;
  }
  const std::string preview = PreviewNotReady(call);
  if (!preview.empty()) {
    if (frame - g_since > kStepFrames)
      Stop(fmt::format("not settled {} frames after the last step: {} (page {:08X}; the arrival waits for the preview model)", kStepFrames, preview, p),
           Reason::kNotSettled);
    return false;
  }
  if (kind == ArrivalPanel::kOther)
    REXLOG_INFO("[wwe2k14][cas_goto] record {}: item page {:08X}: overlay {:08X} (vtable {:08X}) holds panel {:08X} (vtable {:08X}), not the parts "
                "panel: its readiness is not read, published as built (frame {})",
                g_target, p, a, Rd(a), in, in ? Rd(in) : 0, frame);
  if (panel)
    REXLOG_INFO("[wwe2k14][cas_goto] record {}: the parts panel {:08X} (overlay {:08X}) of item page {:08X} is up and idle, {} frames after the "
                "page settled (frame {})",
                g_target, in, a, p, frame - g_since, frame);
  REXLOG_INFO("[wwe2k14][cas_goto] record {}: the preview model is ready and the game's loading screen is down, {} frames after the page settled "
              "(frame {})",
              g_target, frame - g_since, frame);
  return true;
}

// The page builder's +28 gate bit, read with the builder's own getters the way it calls them (sub_833A0628 +118..+128): -1 = the gate is
// off (sub_822D28A0 == 0), else 0 / 1; -2 = the object the builder reads is not there (never seen by the builder, which reads it unchecked).
inline int GateBit(scene_director::Caller& call) {
  if (!call(sub_822D28A0, {})) return -1;
  const uint32_t obj = call(sub_822D2AE0, {});
  if (!Plausible(obj)) return -2;
  return int(call(sub_822EC988, {uint64_t(obj)}) & 1);
}

// Template mode (a NEW superstar): SUPERSTAR on the TEMPLATES list, then the preset page's own A; never B. See the header.
inline void TemplateStep(scene_director::Caller& call, uint32_t p, uint64_t frame) {
  if (g_picking) {  // the preset page applies its preset (state P+1108) and sets result 12; the host then leaves template mode
    if (frame - g_since > kStepFrames)
      Stop(fmt::format("the template pick did not end {} frames after the preset's A (preset state P+1108 = {}, P+992 = {})", kStepFrames,
                       Rd(p + kPPresetState), Rd(p + kPResult)),
           Reason::kTemplate);
    return;
  }
  if (Rd(p) == kListVtable && Rd(p + kPLabel) == kLabelTemplates) {
    const uint32_t n = Rd(p + kPCount), rows = Rd(p + kPRows), kids = Rd(p + kPChildren);
    int row = -1;
    for (uint32_t i = 0; i < n && i < kMaxRows && row < 0; ++i)
      if (Rd(Rd(rows + 4 * i) + kRowLabel) == kLabelSuperstar) row = int(i);
    const uint32_t kind = row < 0 ? 0 : Rd(Rd(rows + 4 * uint32_t(row)) + kRowKind), next = row < 0 ? 0 : Rd(kids + 4 * uint32_t(row));
    if (row < 0 || kind != 0 || !next) {
      Stop(fmt::format("template mode: no SUPERSTAR row (label {}) of kind 0 with a page on the TEMPLATES list ({} rows; row {}, kind {}, page "
                       "{:08X})",
                       kLabelSuperstar, n, row, kind, next),
           Reason::kTemplate);
      return;
    }
    REXLOG_INFO("[wwe2k14][cas_goto] record {}: template mode (a NEW superstar; B here would leave the editor): A on SUPERSTAR, row {} of {} -> "
                "preset page {:08X} (frame {})",
                g_target, row, PageId(p), next, frame);
    Wr(p + kPRow, uint32_t(row));  // the template-mode enter takes [P+984][P+48] (sub_833BFA48 +53C..+574)
    call(sub_823BC7C0, {uint64_t(p + kPMixin)});
    g_expect_page = next;
    g_tpl_a = true;
    g_sent = true;
    g_since = frame;
    return;
  }
  if (Rd(p) == kPresetVtable) {
    if (!call(sub_833C3F38, {uint64_t(p + kPMixin)})) {  // the pad's A would be refused now (the shown preset's model still loading)
      if (frame - g_since > kStepFrames)
        Stop(fmt::format("the preset page's enable test (sub_833C3F38) stayed 0 for {} frames", kStepFrames), Reason::kTemplate);
      return;
    }
    REXLOG_INFO("[wwe2k14][cas_goto] record {}: A on the preset page (cursor row {}, shown preset {}): the game's template pick, {} (frame {})",
                g_target, Rd(p + kPRow), Rd(p + kPPresetShown), PageId(p), frame);
    call(sub_823BC7C0, {uint64_t(p + kPMixin)});
    g_picking = true;
    g_since = frame;
    return;
  }
  Stop("template mode on a page that is neither the TEMPLATES list nor a preset page", Reason::kTemplate);
}

// The path, from the table the editor holds (the reader sub_83424C88's words: count [data+4], records [data+8]). false = not yet / stopped.
inline bool LoadPath(uint64_t frame) {
  const uint32_t root = Rd(kCawRootPtr);
  const uint32_t m = root ? Rd(root + kRootMenu) : 0;
  if (!m || Rd(m + kMState) != 2) return false;  // the editor's menu object has not adopted slot 1 yet (sub_833A0360)
  const uint32_t data = Rd(m + kMFlow);
  const uint32_t count = data ? Rd(data + kFlowCount) : 0, recs = data ? Rd(data + kFlowRecords) : 0;
  if (count == 0 || count > kMaxRecords || !Plausible(recs) || !Plausible(recs + count * kRecordSize)) {
    Stop(fmt::format("the editor's flow table is not usable (data {:08X}, count {}, records {:08X})", data, count, recs), Reason::kTable);
    return false;
  }
  g_recs = ParseRecords(scenario::Guest(recs), count);
  std::string why = BuildPath(g_recs, g_target, g_path, g_tab);
  Reason reason = Reason::kNotInTable;
  if (why.empty()) why = RefusePath(g_path), reason = Reason::kTemplateChooser;
  if (!why.empty()) {
    Stop("REFUSED: " + why, reason);
    return false;
  }
  std::string chain;
  for (const Rec& r : g_path) chain += fmt::format("{}{} (label {})", chain.empty() ? "" : " > ", r.id, r.label);
  REXLOG_INFO("[wwe2k14][cas_goto] record {}: tab {}, path {} (frame {})", g_target, g_tab, chain, frame);
  return true;
}

// Takes the setting's value as the target (S110 CASARRIVE: at the first frame, and again whenever a route sets a different value while the walk
// is off, arrived, stopped or refused). Everything of the last target is dropped; the walk starts from wherever the editor stands.
inline void Arm(const std::string& arg, uint64_t frame) {
  cas_driver::Release(cas_driver::kGoto);  // S110 CASROW: the last target is dropped, and with it the editor's presses
  g_taken = arg;
  g_path.clear();
  g_recs.clear();
  g_tab = -1;
  g_k = 0;
  g_expect_page = 0;
  g_expect_tab = -1;
  g_sent = g_picking = g_tpl_a = g_picker_b = false;
  g_since = frame;
  g_phase = Phase::kDone;
  if (arg.empty()) {  // off (at boot, or a route cleared the setting)
    Publish(Report{}, frame);
    return;
  }
  uint32_t target = 0;
  const std::string bad = ParseTarget(arg, target);
  if (!bad.empty()) {
    REXLOG_WARN("[wwe2k14][cas_goto] --wwe2k14_cas_goto={}: {}; off", arg, bad);
    Report r;
    Halt(r, Reason::kBadArg);
    Publish(r, frame);
    return;
  }
  if (!cas_driver::Claim(cas_driver::kGoto)) {  // S110 CASROW: first armed wins; the pick or the row verb drives the editor now
    REXLOG_WARN("[wwe2k14][cas_goto] --wwe2k14_cas_goto={}: REFUSED: {} drives the editor now (one driver at a time: arm the go-to after it has ended)",
                arg, cas_driver::Name(cas_driver::g_owner));
    Report r = Armed(target);
    Halt(r, Reason::kBusy);
    Publish(r, frame);
    return;
  }
  g_target = target;
  REXLOG_INFO("[wwe2k14][cas_goto] record {}: waiting for the Create a Superstar editor", g_target);
  g_phase = Phase::kWait;
  Publish(Armed(target), frame);
}

inline void Step(PPCContext& ctx, uint8_t* base, uint64_t frame) {
  if (g_phase == Phase::kOff) {  // the first frame: the value the run started with (one string read under the registry's lock)
    g_changes = rex::cvar::ChangeCount();
    Arm(rex::cvar::GetFlagByName("wwe2k14_cas_goto"), frame);
    return;
  }
  if (g_phase == Phase::kDone) {  // off / arrived / stopped / refused: one atomic load a frame until some setting changes
    const uint64_t changes = rex::cvar::ChangeCount();
    if (changes == g_changes) return;
    g_changes = changes;
    const std::string arg = rex::cvar::GetFlagByName("wwe2k14_cas_goto");  // locked: a script's `cvar` line may be setting it
    if (NewTarget(g_taken, arg)) Arm(arg, frame);
    return;
  }
  const uint32_t h = scenario::SequenceId() == kSeqCas ? Host() : 0;
  if (!h) {  // the editor's page host is not up (another screen, or not built yet); once the go-to has started on it, it ends there
    g_page_now = 0;
    if (!g_path.empty())
      Stop(fmt::format("the editor's page host went away (sequence {}) before the go-to ended", scenario::SequenceId()), Reason::kHostGone);
    return;
  }
  g_page_now = Rd(h + kHPage);
  if (g_path.empty()) {
    if (!LoadPath(frame)) return;
    g_since = frame;
  }
  const uint32_t p = Rd(h + kHPage), t = Rd(h + kHTabBar);
  std::string busy = Busy(h, p);
  if (busy.empty() && g_picker_b && Rd(h + kHOverlayA))  // the overlay closes while its item page still reads as settled (Busy's item-page rule)
    busy = fmt::format("the item page's overlay {:08X} has not closed (its state +16 = {}, its panel's {}; the host drops it at 5)",
                       Rd(h + kHOverlayA), Rd(Rd(h + kHOverlayA) + kPState), Rd(Rd(Rd(h + kHOverlayA) + kContainerInner) + kPState));
  if (!busy.empty()) {
    if (frame - g_since > kStepFrames)
      Stop(fmt::format("not settled {} frames after the last step: {} (page {:08X})", kStepFrames, busy, p), Reason::kNotSettled);
    return;
  }
  if (g_sent) {  // the step in flight has settled: did it land where the game's handler sends it?
    g_sent = false;
    g_picker_b = false;
    if (g_tpl_a && g_expect_page && p != g_expect_page && !Rd(Rd(kCawRootPtr) + kRootTemplateMode)) {
      // Run 228 (S110): the A on SUPERSTAR did not settle on the preset page: 230 frames later the host was on the HEAD root (label 6362, depth
      // 0) with template mode already 0. The game took the pick through by itself; that end state is the one the template step is for, so
      // it is accepted, named, and the go-to goes on (the tab step refuses with its own STOPPED line if the tabs are still off).
      REXLOG_INFO("[wwe2k14][cas_goto] record {}: template picked by the SUPERSTAR row itself (no preset page settled): {}, tab {}, tabs {} "
                  "(frame {})",
                  g_target, PageId(p), Rd(t + kTIndex), Rd(t + kTAllowed) ? "on" : "off", frame);
      g_expect_page = 0;
    }
    g_tpl_a = false;
    if (g_expect_page && p != g_expect_page) {
      Stop(fmt::format("the step settled on page {:08X} (label {}), not the expected page {:08X} (the game refused the press or sent it "
                       "elsewhere: a disabled row, a confirm message on the page, or the host's redirect)",
                       p, Rd(p + kPLabel), g_expect_page),
           Reason::kWrongPage);
      return;
    }
    if (g_expect_tab >= 0 && int(Rd(t + kTIndex)) != g_expect_tab) {
      Stop(fmt::format("the tab step settled on tab {}, not {} (the game refused it)", Rd(t + kTIndex), g_expect_tab), Reason::kWrongTab);
      return;
    }
    if (g_phase == Phase::kDown) {
      REXLOG_INFO("[wwe2k14][cas_goto] record {}: entered level {}: record {} on page {:08X} (page word {:08X}, label {}) (frame {})", g_target,
                  g_k, g_path[g_k].id, p, Rd(p + kPMixin), Rd(p + kPLabel), frame);
      EnteredLevel(frame);
    }
    g_since = frame;  // each wait gets its own bound
  }
  g_expect_page = 0;
  g_expect_tab = -1;
  scene_director::Caller call(ctx, base);
  if (Rd(Rd(kCawRootPtr) + kRootTemplateMode)) {
    TemplateStep(call, p, frame);
    return;
  }
  if (g_picking) {
    g_picking = false;
    REXLOG_INFO("[wwe2k14][cas_goto] record {}: template picked: {}, tab {}, tabs {} (frame {})", g_target, PageId(p), Rd(t + kTIndex),
                Rd(t + kTAllowed) ? "on" : "off", frame);
    g_since = frame;
  }
  if (g_phase == Phase::kWait || g_phase == Phase::kUp) {
    g_phase = Phase::kUp;
    // Up to the tab's root (no parent; the root's B would exit the editor, so it is never sent there): a list's own B, or on an item page the
    // B of the parts panel inside its overlay (cas_goto_core.h PlanUp).
    const uint32_t parent = Rd(p + kPParent), a = Rd(h + kHOverlayA), in = a ? Rd(a + kContainerInner) : 0;
    const UpMove move = PlanUp(Rd(p), parent, a ? Rd(a) : 0, in ? Rd(in) : 0);
    if (move == UpMove::kNotList) {
      Stop(fmt::format("the current page {:08X} (vtable {:08X}) is not a list page; only a list's B is driven", p, Rd(p)), Reason::kNotList);
      return;
    }
    if (move == UpMove::kNoPanel) {
      Stop(fmt::format("item page {:08X}: overlay A {:08X} (vtable {:08X}) holds panel {:08X} (vtable {:08X}), not the parts panel (overlay "
                       "{:08X}, panel {:08X}); only the parts panel's B is driven (it restores the superstar)",
                       p, a, a ? Rd(a) : 0, in, in ? Rd(in) : 0, kContainerVtable, kPartsPanelVtable),
           Reason::kNoPicker);
      return;
    }
    if (move == UpMove::kPanelB) {
      const std::string why = PanelNotReady(call, a, in);
      if (!why.empty()) {  // the press would be a no-op now: wait, under the same bound as any step
        if (frame - g_since > kStepFrames)
          Stop(fmt::format("not settled {} frames after the last step: {} (page {:08X})", kStepFrames, why, p), Reason::kNotSettled);
        return;
      }
      REXLOG_INFO("[wwe2k14][cas_goto] record {}: B on the parts panel {:08X} (overlay {:08X}) of item page {:08X} (label {}, depth {}) -> its "
                  "parent {:08X} (frame {})",
                  g_target, in, a, p, Rd(p + kPLabel), Rd(p + kPDepth), parent, frame);
      call(sub_823BC838, {uint64_t(in + kPMixin)});  // the pad B entry on the panel's mixin (panel+28, sub_833FE938 +3C)
      g_expect_page = parent;  // only the parent: a grandparent landing means the overlay reported ACCEPTED (cas_goto_core.h): a stop
      g_picker_b = true;
      g_sent = true;
      g_since = frame;
      return;
    }
    if (move == UpMove::kListB) {
      REXLOG_INFO("[wwe2k14][cas_goto] record {}: B on page {:08X} (label {}, depth {}) -> its parent {:08X} (frame {})", g_target, p,
                  Rd(p + kPLabel), Rd(p + kPDepth), parent, frame);
      call(sub_823BC838, {uint64_t(p + kPMixin)});
      g_expect_page = parent;
      g_sent = true;
      g_since = frame;
      return;
    }
    g_phase = Phase::kTab;
  }
  if (g_phase == Phase::kTab) {
    const int cur = int(Rd(t + kTIndex));
    const int steps = TabSteps(cur, g_tab);
    if (steps != 0) {
      if (!Rd(t + kTAllowed)) {
        Stop(fmt::format("tabs are off on page {:08X} (T+936 = 0): the game refuses a tab change here", p), Reason::kTabsOff);
        return;
      }
      g_expect_tab = (cur + (steps > 0 ? 1 : -1) + kTabs) % kTabs;
      REXLOG_INFO("[wwe2k14][cas_goto] record {}: tab {} -> {} (the game's {} step) (frame {})", g_target, cur, g_expect_tab,
                  steps > 0 ? "next-tab" : "previous-tab", frame);
      call(steps > 0 ? sub_833BE790 : sub_833BE6A8, {uint64_t(h), 0});
      g_sent = true;
      g_since = frame;
      return;
    }
    if (Rd(p) != kListVtable || Rd(p + kPLabel) != g_path[0].label) {
      Stop(fmt::format("tab {}'s root page {:08X} (vtable {:08X}, label {}) is not the list of top record {} (label {})", cur, p, Rd(p),
                       Rd(p + kPLabel), g_path[0].id, g_path[0].label),
           Reason::kWrongRoot);
      return;
    }
    REXLOG_INFO("[wwe2k14][cas_goto] record {}: entered level 0: record {} on page {:08X} (page word {:08X}, tab {}) (frame {})", g_target,
                g_path[0].id, p, Rd(p + kPMixin), cur, frame);
    EnteredLevel(frame);
    g_phase = Phase::kDown;
    g_k = 0;
  }
  // kDown: p is the page of g_path[g_k].
  if (g_k + 1 >= g_path.size()) {
    if (!ArrivalReady(call, h, p, frame)) return;  // S110 SCENESRC + CASREADY: the arrival waits for the panel, then the preview
    Stop(fmt::format("arrived at record {} (page word {:08X}; page {:08X}, label {}, tab {}, cursor row {})", g_target, Rd(p + kPMixin), p,
                     Rd(p + kPLabel), Rd(t + kTIndex), Rd(p + kPRow)),
         Reason::kNone);
    return;
  }
  if (Rd(p) != kListVtable) {
    Stop(fmt::format("page {:08X} of record {} is not a list page (vtable {:08X})", p, g_path[g_k].id, Rd(p)), Reason::kNotList);
    return;
  }
  const Rec& child = g_path[g_k + 1];
  const uint32_t n = Rd(p + kPCount), rows = Rd(p + kPRows), kids = Rd(p + kPChildren);
  std::vector<uint32_t> labels;
  for (uint32_t i = 0; i < n && i < kMaxRows; ++i) labels.push_back(Rd(Rd(rows + 4 * i) + kRowLabel));
  const int bit = GateBit(call);
  if (bit == -2) {
    Stop(fmt::format("the superstar object the page builder reads ([[0x836F2DB8]+300]) is not there; the row of record {} cannot be told", child.id),
         Reason::kNoSuperstar);
    return;
  }
  const int word512 = Rd(Rd(kCawRootPtr) + kRootPageSet) == kPageSet512 ? 1 : 0;  // the builder's first gate (sub_8338F060)
  const int row = PickRow(g_recs, child, labels, bit, word512);
  if (row < 0 && !KeptByPageSet(child, word512)) {
    Stop(fmt::format("REFUSED: record {} (label {}, +28 word {}) is left out by the page builder while [root+4788] {} 512 (sub_833A0628 "
                     "+88..+CC, sub_8338F060): it is not a row of page {:08X}",
                     child.id, child.label, child.gate, word512 ? "==" : "!=", p),
         Reason::kBuilderGate);
    return;
  }
  if (row < 0) {
    Stop(fmt::format("record {} (label {}, +28 word {}) is not a row of page {:08X} ({} rows, superstar bit {}; the builder leaves rows out "
                     "on save / edit state and by that bit)",
                     child.id, child.label, child.gate, p, n, bit),
         Reason::kNoRow);
    return;
  }
  const uint32_t kind = Rd(Rd(rows + 4 * uint32_t(row)) + kRowKind);
  const uint32_t next = Rd(kids + 4 * uint32_t(row));
  Wr(p + kPRow, uint32_t(row));  // the cursor row the A handler reads (mixin+20), as the game's own cursor holds it
  if (kind != 0 || !next) {
    if (g_k + 2 == g_path.size()) {
      // The page holding the row was entered by the last step (the A below, or the tab / up steps): its preview can still be loading,
      // as on a page arrival (run 273), so the row arrival takes the same wait (a list page: no panel, the preview only).
      if (!ArrivalReady(call, h, p, frame)) return;  // CASREADY review: a row arrival waits for the preview too
      Stop(fmt::format("arrived at record {}: row {} of page {:08X} (page word {:08X}), cursor on it; row kind {}{}: it opens an overlay or a "
                       "message, not a page, so no A is sent",
                       g_target, row, p, Rd(p + kPMixin), kind, next ? "" : ", no page"),
           Reason::kNone);
    } else {
      Stop(fmt::format("record {} (row {}, kind {}) opens no page, but the path goes on below it", child.id, row, kind), Reason::kNoPage);
    }
    return;
  }
  REXLOG_INFO("[wwe2k14][cas_goto] record {}: A on row {} of page {:08X} (record {}, label {}) -> page {:08X} (frame {})", g_target, row, p,
              child.id, child.label, next, frame);
  call(sub_823BC7C0, {uint64_t(p + kPMixin)});
  g_expect_page = next;
  g_sent = true;
  g_since = frame;
  ++g_k;
}

}  // namespace wwe2k14::cas_goto
