// S110 GOTO: open a front-end menu page by its flow record id, through the game's own page-change mailbox and the game's own goto
// (docs\research\s110\REACH.md; the derivation is at the top of menu_goto_core.h).
// TEST / TOOLING PATH ONLY: reached by the live channel's `!page rec=N [back]` and the input script's `page N [back]` (the goto
// resolver's `menu:goto N` lowers to that line), never by a player's play.
// What we do: store the mailbox words (code 0, label kByIdLabel, back, armed 1) once, from the owner update BEFORE the game's own
// update of that frame (same thread), and serve the game's own call of sub_828426D0 at lr 0x82842CD4 with sub_828424F0(SM, id).
// What the game does: every test before that call, and everything after it (enter the page WITH its gates, back, clear the words).
// The hook below is inert unless a request of ours is armed: one atomic flag, then the game's own function unchanged.
#pragma once

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc/context.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "live_control.h"  // g_page_live
#include "menu_goto_core.h"
#include "menu_home.h"
#include "scenario.h"      // U32, Guest, Frame, SequenceId, kSmartMenuWord, g_page_tick

REXCVAR_DEFINE_BOOL(wwe2k14_menu_goto_list, false, "wwe2k14",
                    "S110 GOTO: log every menu flow record once per table, at the first frame with a front end "
                    "([wwe2k14][menu_goto] rec N menu M item I code C label L parent P enterable yes|no(reason)), so the ids "
                    "for `!page rec=N` and the script line `page N` can be read from the run log. Log only. See src/menu_goto.h");

namespace wwe2k14::menu_goto {

// kHolderPtr / kSmLockPtr / kSmMenu / kHolderState: menu_goto_core.h (shared with src/menu_home.h).
constexpr uint32_t kSmCursor = 352, kSmScreen = 11412;  // menus.md "Two layers" (cursor / screen controller)
constexpr int kMaxListings = 4;              // tables listed per run (a new record array, e.g. GRAPHICS patched in, is listed again)

// Game-thread state (the owner update and the hook run on the same thread: sub_8283E6A0 seen called from sub_82C302A0 0x82C303C0).
inline bool g_have = false;
inline uint32_t g_id = 0;
inline bool g_back = false;
inline uint64_t g_ticket_now = 0;
inline uint64_t g_since = 0;
inline bool g_watching = false;
inline uint64_t g_watch_since = 0;
inline uint32_t g_watch_id = 0;
inline uint64_t g_watch_ticket = 0;
inline uint32_t g_listed_arr = 0;
inline int g_listings = 0;
// The hook's side: armed by Tick, consumed by the one call it serves.
inline std::atomic<bool> g_serve_armed{false};
inline std::atomic<uint32_t> g_serve_id{0};
inline std::atomic<bool> g_served{false};

inline bool Plausible(uint32_t p) { return p >= 0x10000 && p < 0xC0000000u; }
inline uint32_t Rd(uint32_t a) { return Plausible(a) ? scenario::U32(a) : 0; }
inline void Wr(uint32_t a, uint32_t v) { if (Plausible(a)) rex::memory::store_and_swap<uint32_t>(scenario::Guest(a), v); }

inline bool ReadTable(std::vector<Rec>& recs, uint32_t& menu_count) {  // the layout and its sources: menu_goto_core.h ReadTableWith
  return ReadTableWith([](uint32_t a) { return Rd(a); }, Rd(kHolderPtr), recs, menu_count);
}

inline void Fail(uint32_t id, uint64_t ticket, const std::string& why) {
  REXLOG_ERROR("[wwe2k14][menu_goto] record {} FAILED: {} (frame {})", id, why, scenario::Frame());
  g_have = false;
  SetAnswer("error: " + why);
  Finish(ticket, -1, why);
}

// --wwe2k14_menu_goto_list: every record once per record array.
inline void List() {
  if (!REXCVAR_GET(wwe2k14_menu_goto_list) || g_listings >= kMaxListings) return;
  const uint32_t holder = Rd(kHolderPtr);
  const uint32_t w20 = holder ? Rd(holder + 20) : 0, r = w20 ? w20 : (holder ? Rd(holder + 16) : 0);
  const uint32_t arr = r ? Rd(r + 4 + kTabRecArray) : 0;
  if (!arr || arr == g_listed_arr) return;
  std::vector<Rec> recs;
  uint32_t menu_count = 0;
  if (!ReadTable(recs, menu_count)) return;
  g_listed_arr = arr;
  ++g_listings;
  REXLOG_INFO("[wwe2k14][menu_goto] listing: {} records, {} menus, record array {:08X} (frame {})", recs.size(), menu_count, arr, scenario::Frame());
  for (const Rec& x : recs)
    REXLOG_INFO("[wwe2k14][menu_goto] rec {} menu {} item {} code {} label {} parent {} {}", x.id, int32_t(x.menu), x.item, int32_t(x.code), x.label,
                int32_t(x.parent), Enterable(recs, x.id));
}

// Game thread, once per owner update (scenario.h g_page_tick), before the game's own update of the frame.
inline void Tick(PPCContext&, uint8_t*) {
  const uint64_t frame = scenario::Frame();
  const uint32_t sm = Rd(scenario::kSmartMenuWord);
  const uint32_t seq = scenario::SequenceId();
  const bool front = sm && (seq == 75 || seq == 80 || seq == 81);  // REACH claim 7: SM non-null, sequence 75 / 80 / 81 (sub_8283ABD8)
  if (front) List();
  if (g_watching) {
    if (g_served.load()) {  // the hook served the game's call; the list update then entered (or its gates refused) and cleared the words
      REXLOG_INFO("[wwe2k14][menu_goto] record {} page change done: menu {} cursor {} screen mode {} (frame {}, {} frames after the request)", g_watch_id,
                  Rd(sm + kSmMenu), Rd(sm + kSmCursor), Rd(Rd(sm + kSmScreen) + 32), frame, frame - g_watch_since);
      g_watching = false;
      Finish(g_watch_ticket, 1, "served");
    } else if (sm && Rd(sm + kMbArmed) == 0) {  // cleared without our call: another request took the mailbox first, or the hook was not reached
      g_serve_armed = false;
      g_watching = false;
      Fail(g_watch_id, g_watch_ticket, "the mailbox was cleared without the game's goto being handed our record (another request took it, or the call at lr 0x82842CD4 was not reached)");
    } else if (frame > g_watch_since + kWatchFrames) {
      // Not served (the list update did not run, or its top gate stayed closed): take back OUR request, so it cannot fire on a later
      // frame in a state nobody asked for. Only when the words are still exactly ours; the ctor defaults are 0 / 0 / 0 / 0x1869F.
      g_serve_armed = false;
      const bool ours = sm && Rd(sm + kMbArmed) == 1 && Rd(sm + kMbCode) == 0 && Rd(sm + kMbLabel) == kByIdLabel;
      if (ours) {
        Wr(sm + kMbArmed, 0);
        Wr(sm + kMbCode, 0);
        Wr(sm + kMbLabel, kLabelNone);
        Wr(sm + kMbBack, 0);
      }
      g_watching = false;
      Fail(g_watch_id, g_watch_ticket, "page change not seen within " + std::to_string(kWatchFrames) + " frames (mailbox still armed or front end gone); " +
                                           (ours ? "our request was taken back" : "the mailbox no longer holds our words, left as it is"));
    }
    if (g_watching) return;
  }
  bool first = false;
  if (!g_have) {
    if (!Take(g_id, g_back, g_ticket_now)) return;
    g_have = true;
    g_since = frame;
    first = true;
  }
  const uint32_t id = g_id;
  std::vector<Rec> recs;
  uint32_t menu_count = 0;
  const bool table = front && ReadTable(recs, menu_count);
  // S110 BACK (docs/research/s110/BACK.md): SM is read again every frame (leaving a screen builds a brand-new SmartMenu, never cached);
  // only list state 2 serves requests. Not ready (any reason, incl. a still-armed mailbox): WAIT for the game's own menu, bounded.
  const std::string why_not = ReadyWhy(sm, seq, table, front ? Rd(Rd(kHolderPtr) + kHolderState) : 0, front ? Rd(Rd(sm + kSmLockPtr) + 56) : 0,
                                       front ? Rd(sm + kSmListState) : 0, front && Rd(sm + kMbArmed) != 0, front ? Rd(sm + kSmMenu) : 0);
  if (!why_not.empty()) {
    if (frame > g_since + kReturnFrames)
      Fail(id, g_ticket_now, "the front end was not ready within " + std::to_string(kReturnFrames) + " frames: " + why_not);
    else if (first)
      SetAnswer("ok: record " + std::to_string(id) + " waiting for the front end");
    return;
  }
  Words w;
  const std::string why = Plan(recs, id, w);
  if (!why.empty()) {
    Fail(id, g_ticket_now, "REFUSED: " + why);
    return;
  }
  g_serve_id = id;
  g_served = false;
  g_serve_armed = true;
  Wr(sm + kMbCode, w.code);
  Wr(sm + kMbLabel, w.label);
  Wr(sm + kMbBack, g_back ? 1u : 0u);
  Wr(sm + kMbArmed, 1);  // as sub_8283A4A0 stores it; the list update serves only when armed
  REXLOG_INFO("[wwe2k14][menu_goto] record {} armed by id (mailbox label {:08X}, back {}) (frame {})", id, w.label, g_back ? 1 : 0, frame);
  g_watching = true;
  g_watch_since = frame;
  g_watch_id = id;
  g_watch_ticket = g_ticket_now;
  g_have = false;
  SetAnswer("ok: record " + std::to_string(id) + " armed by id");
}

// The hook's body: the game's own call of sub_828426D0 from the list update (lr kGotoLr) with OUR mailbox words is answered with the
// game's own goto for our record, as sub_828426D0 does for a hit (`mr r3,r24; bl sub_828424F0` with r4 = the record id; return 1).
inline bool Serve(PPCContext& ctx, uint8_t* base) {
  if (!g_serve_armed.load(std::memory_order_acquire)) return false;  // the cheap test first: no request, no guest read
  if (!ServesOurCall(true, uint32_t(ctx.lr), ctx.r3.u32, Rd(scenario::kSmartMenuWord), ctx.r4.u32, ctx.r5.u32)) return false;
  g_serve_armed = false;
  const uint32_t id = g_serve_id.load();
  // S110 PROFILE (docs/research/s110/PROFILE.md, all [C]): the enter gate sub_8283CCF0 compares SM+796 (the pad that pressed
  // this entry) with the active profile's pad [[0x8376AAF0]+0] and shows "only ... the active profile" when they differ. A
  // press stores its pad at SM+796 (0x828434C4); the mailbox path stores nothing, and the main menu's reset leaves -1
  // (0x8283E2B0). The game's own re-entry caller copies the profile pad there before its goto (sub_82842868 at 0x82842978):
  // we do the same, with the game's own value, at the same point.
  const uint32_t sm = ctx.r3.u32, game = Rd(kGamePtr);
  uint32_t pad = 0xFFFFFFFFu;
  if (game) {
    pad = Rd(game);
    Wr(sm + kSmPad, pad);
  }
  ctx.r4.u64 = id;
  sub_828424F0(ctx, base);
  ctx.r3.u64 = 1;
  g_served = true;
  REXLOG_INFO("[wwe2k14][menu_goto] record {} handed to the game's goto (sub_828424F0) at its mailbox call, entry pad {} {} (frame {})", id,
              int32_t(pad), game ? "= the active profile's pad" : "NOT SET: the game object is null, the profile gate will refuse", scenario::Frame());
  return true;
}

// `!page rec=N [back]` (the pipe thread): parse, hand to the game thread, wait for its arm answer. Reads no guest memory.
inline std::string LiveCommand(const std::string& args) {
  uint32_t id = 0;
  bool back = false;
  const std::string bad = ParseArgs(args, id, back);
  if (!bad.empty()) return bad;
  uint64_t ticket = 0;
  const std::string busy = Request(id, back, ticket);
  if (!busy.empty()) return busy;
  std::unique_lock<std::mutex> lock(g_mu);
  if (!g_cv.wait_for(lock, std::chrono::seconds(10), [] { return g_answered; })) {
    if (Withdraw()) return "error: the game thread did not take the request within 10 s (is the game running frames?)";
    g_cv.wait_for(lock, std::chrono::seconds(5), [] { return g_answered; });
  }
  return g_answered ? g_answer : "error: the request was taken but no answer came";
}

// S110 HOMESTEP: the readers src/menu_home.h uses (it cannot include scenario.h before the event bus).
inline const bool g_home_registered =
    (wwe2k14::menu_home::g_env = {[](uint32_t a) -> uint32_t { return scenario::U32(a); }, []() -> uint32_t { return scenario::SequenceId(); },
                                  [](uint32_t a) -> const uint8_t* { return scenario::Guest(a); }}, true);
inline const bool g_registered = (wwe2k14::live::g_page_live = &LiveCommand, wwe2k14::scenario::g_page_tick = &Tick, true);

}  // namespace wwe2k14::menu_goto

// The one hook: inert unless Tick armed a request of ours (one atomic load, then the game's own function; ServesOurCall for the rest).
REX_HOOK_RAW(sub_828426D0) {
  if (wwe2k14::menu_goto::Serve(ctx, base)) return;
  __imp__sub_828426D0(ctx, base);
}
