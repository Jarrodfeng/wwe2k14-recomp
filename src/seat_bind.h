// S110 SEAT3 lane G-B (docs\research\s110\SEAT3.md): ANY library character in ANY free roster seat, DURING a session (a BIND), and the DOWNLOADED tab of the
// exhibition select screen showing the library a PAGE at a time. The pure half (the card re-targeted to a seat, the page model, the ranks, the re-ask) is
// src\seat_bind_core.h (unit: tools\seats\bind_unit.cpp); this is the game side.
//
//   * PAGES: the DOWNLOADED tab lists the residents and the guests of the page bound now. LB / RB (the select panel's own bumper handlers sub_823BC3F0 /
//     sub_823BC558, which the stock panel accepts and ignores) flip the page; the letter jump (src\letter_jump.h, which owns LT / RT) asks this module first
//     (g_page_provider): a letter that exists only on another page flips to it, then the cursor goes to that character. Both only QUEUE a request.
//   * THE TICK (src\scenario.h g_seat_tick, the owner update, game thread): runs the request when the game allows it (a front-end menu sequence, the pac
//     worker idle, the pac list there), in the order of SEAT3.md section 2:
//       reads and re-targets first (nothing written)  ->  every leaving guest: his state exported, then the game's purge (src\slot_handover.h GuestLeaves; an
//       export that fails stops THAT seat)  ->  every arrival: the seat's character node re-pointed at his pac (rows re-targeted), his EV file + EV node,
//       the live roster row + profile (the game's own conversions), the DSBD row, the occupant table + music, the game's per-slot model release  ->  the
//       roster ranks of everybody listed (alphabetical, unique: the select list is a counting sort on them)  ->  the portrait pool's re-ask  ->  each open
//       panel's list rebuilt (sub_8285C948).
//   * NOT TOUCHED: a seat the screen holds (a participant's pick, a highlight, the Royal Rumble's picks) keeps its man through a flip.
// Every guest read / write is bounds-checked (seats 100..499, counts, Readable). Logs: `[wwe2k14][bind] ...`, once per event, nothing per frame.
#pragma once

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "guest_call.h"
#include "letter_jump.h"
#include "load_watch_dump.h"  // load_probe::SampleQueueHead (the dequeue's own "waiting" test)
#include "scenario.h"         // U32, SequenceId, g_seat_tick
#include "seat_alias_plan.h"
#include "seat_bind_core.h"
#include "seat_release.h"
#include "seats.h"
#include "slot_handover.h"

REXCVAR_DECLARE(int32_t, wwe2k14_seat_pool_cap);

REX_EXTERN(sub_8285C948);  // the select panel's list rebuild (r3 = the panel): re-sorts the lists and re-seats the cursor
REX_EXTERN(sub_82923AB8);  // r3 = the menu state object: is the selected 1100-byte block's first word 20..23 (a Royal Rumble)
REX_EXTERN(__imp__sub_823BC3F0);  // the select panel's LB handler (the stock one only plays the notification)
REX_EXTERN(__imp__sub_823BC558);  // RB

namespace wwe2k14::seat_bind {

namespace st = wwe2k14::seats;
namespace sh = wwe2k14::slot_handover;
namespace fs = std::filesystem;

constexpr uint32_t kBusyWord = 0x839400A4;     // the pac worker's current request (src/load_watch.h); 0 = idle
constexpr uint32_t kMenuStatePtr = 0x839294C0;  // the menu state object S: participants + the Royal Rumble's picks
constexpr uint64_t kBusyFrames = 600;           // a panel request waits this many ticks for the game to allow it, then is refused (never a write while busy)
constexpr uint64_t kReaskFrames = 600;          // a pool entry that is cancelling (state 3) is asked again for this many ticks
constexpr uint32_t kPanels = 6;                 // the screen keeps six panel pointers at screen + 44 + 4 i (sub_8282A780 +2CC)
constexpr uint32_t kPanelsAt = 44, kHighlights = 56832;  // screen + 56832 + 4 i = participant i's highlighted id (sub_82829758 +D8)
constexpr uint32_t kBlockSel = 11000, kBlockSize = 1100, kRecSize = 156, kRumbleCount = 11184, kRumblePicks = 11024, kRumbleMax = 40;

inline bool Plausible(uint32_t p) { return st::Plausible(p); }
inline uint32_t Rd(uint32_t a) { return Plausible(a) ? scenario::U32(a) : 0; }

// A guest call by address (an indirect call, as the generated code makes for a virtual method): the portrait loader's cancel.
inline uint32_t CallGuestAddr(PPCContext& ctx, uint8_t* base, uint32_t addr, uint32_t a0) {
  const PPCContext saved = ctx;
  ctx.r3.u64 = a0;
  ctx.lr = 0;
  REX_CALL_INDIRECT_FUNC(addr);
  const uint32_t r = ctx.r3.u32;
  ctx = saved;
  return r;
}

// ================================================================ the process state (game thread, but the request port)
struct NodeBuf {
  uint32_t node = 0, rows = 0, cap = 0;  // the pac-list node, its rows buffer and the buffer's size
};
struct Request {
  uint32_t panel = 0;       // the panel that asked (0 = none: the boot bind)
  int dir = 0;              // +1 / -1: next / previous page
  int page = -1;            // >= 0: that page (the letter jump)
  std::string jump_key;     // after the rebuild the cursor goes to this guest's row
  bool initial = false;     // the first page after a load
};
inline std::mutex g_mx;
inline std::optional<Request> g_req;
inline uint64_t g_ticks = 0, g_req_tick = 0, g_reask_tick = 0;
inline std::atomic<bool> g_ready{false};  // the catalog is built (the first row pass)
inline Catalog g_cat;
inline State g_state;                     // the guests seated now (seat -> key); screen_held is filled per flip
inline int g_page = 0;
inline bool g_need_initial = false, g_wait_logged = false;
inline std::map<std::string, const st::Resident*> g_by_key;
inline std::map<int, NodeBuf> g_char_node, g_ev_node;
inline seat_alias::RefreshRetry g_alias_retry;  // (review S110 LIBSIZE-3) an alias Refresh owed: a refused write, or a row pass that cleared the guests
inline std::set<int> g_reask;             // seats whose pool entry was cancelling: asked again
inline fs::path g_root;

inline int Cap() { return std::max(0, int(REXCVAR_GET(wwe2k14_seat_pool_cap))); }
// The highest rank of the game's own rows = the frame's lowest `rank` minus 1 (the core's hand-back); -2 = no frame seat.
inline int BaseRank() {
  int lo = 1 << 30;
  for (const st::FrameSeat& s : st::g_roster.frame.seats) lo = std::min(lo, s.rank);
  return lo == (1 << 30) ? -2 : lo - 1;
}

inline void BuildCatalog() {
  std::vector<Entry> ents;
  g_by_key.clear();
  for (const st::Resident& r : st::g_roster.res) ents.push_back({r.key, r.rows.names[0].empty() ? r.card.name : r.rows.names[0], r.seat});
  for (const st::Resident& r : st::g_roster.lib) {
    ents.push_back({r.key, r.rows.names[0].empty() ? r.card.name : r.rows.names[0], -1});
    g_by_key[r.key] = &r;
  }
  std::vector<int> frame;
  for (const st::FrameSeat& s : st::g_roster.frame.seats) frame.push_back(s.seat);
  std::set<int> held;
  for (const auto& kv : st::g_roster.held) held.insert(kv.first);
  g_cat = Catalog::Make(ents, frame, held, Cap());
}

// The roster rank (row +2) of everybody listed: the select list's counting sort drops one of two rows with the same rank, and it orders by it.
inline int WriteRanks(uint8_t* base, const std::map<int, int>& ranks) {
  const uint32_t rarr = st::U32(base, st::kRosterPtr);
  if (!Plausible(rarr)) return 0;
  int n = 0;
  for (const auto& kv : ranks) {
    uint32_t row = 0;
    if (kv.second < 0 || kv.second > 65535 || !st::RowAddr(rarr, kv.first, row) || !st::Readable(row, st::kRowSize)) continue;
    st::PutBe16(st::P(base, row + kRankOff), uint16_t(kv.second));
    ++n;
  }
  return n;
}

// ================================================================ the game allows a bind now?
// Busy = the worker's current request [0x839400A4] OR a request already QUEUED (a queued request resolved its node pointer and offsets when it was
// enqueued: re-pointing a node under it reads the new pac at the old offsets). The queue test is the dequeue's own (src/load_watch_dump.h).
inline uint32_t BusyRequest() {
  if (const uint32_t r = Rd(kBusyWord)) return r;
  return load_probe::SampleQueueHead(
      [](uint32_t a, uint32_t* w) {
        if (!Plausible(a)) return false;
        *w = scenario::U32(a);
        return true;
      },
      [](uint32_t p) { return Plausible(p); });
}
inline std::string NotNow(uint8_t* base) {
  if (!g_ready.load(std::memory_order_relaxed)) return "no library";
  if (const std::string no = seat_release::SequenceRefusal(scenario::SequenceId()); !no.empty()) return no;
  const uint32_t mgr = st::U32(base, st::kPacMgrPtr), list = mgr ? st::U32(base, mgr + 72) : 0;
  if (!Plausible(list) || !st::U32(base, list)) return "the pac manager has no node list yet";
  if (!Plausible(st::U32(base, st::kRosterPtr))) return "the roster array is not there yet";
  if (const uint32_t b = BusyRequest()) {
    char t[64];
    std::snprintf(t, sizeof t, "the pac worker is busy (request %08X)", b);
    return t;
  }
  return {};
}

// ================================================================ what the screen holds (never re-bound)
inline std::set<int> ScreenHeld(uint8_t* base, scene_director::Caller& call, uint32_t screen) {
  std::set<int> h;
  auto add = [&](uint32_t w) {
    const int id = int(w & 0x7FFFFFFFu);
    if (id >= st::kFirstId && id <= st::kLastId) h.insert(id);
  };
  const uint32_t S = st::U32(base, kMenuStatePtr);
  if (Plausible(S)) {
    const uint32_t blk = st::U32(base, S + kBlockSel);
    if (blk < 16)
      for (uint32_t i = 0; i < kPanels; ++i) {
        const uint32_t rec = S + blk * kBlockSize + (i + 1) * kRecSize;  // sub_82921ED8: the participant's record
        if (st::Readable(rec, 4)) add(st::U32(base, rec));
      }
    if (call(sub_82923AB8, {S}) != 0) {  // a Royal Rumble: the 40 picks
      const uint32_t n = std::min(st::U32(base, S + kRumbleCount), kRumbleMax);
      for (uint32_t k = 0; k < n; ++k) add(st::U32(base, S + kRumblePicks + 4 * k));
    }
  }
  if (Plausible(screen))
    for (uint32_t i = 0; i < kPanels; ++i) add(st::U32(base, screen + kHighlights + 4 * i));
  return h;
}

// ================================================================ one seat's arrival, prepared (reads only), then written
struct Prepared {
  Step step;
  const st::Resident* card = nullptr;
  std::unique_ptr<st::Occupant> occ;
  std::array<uint8_t, st::kDsbdRow> dsbd{};
  std::vector<uint8_t> node_rows, ev_rows;
  EvFile ev;
  std::string path, ev_path;
  int rank = 0, words = 0, actors = 0, blobs = 0, t = 0, bank_t = 0, bank_s = 0;
};

inline bool Prepare(const Step& step, int rank, Prepared& p, std::string& why) {
  const auto it = g_by_key.find(step.arrive);
  if (it == g_by_key.end()) return why = "'" + step.arrive + "' is not a library character", false;
  const st::Resident& B = *it->second;
  const st::FrameSeat* fsd = st::g_roster.frame.Find(step.seat);
  if (!fsd) return why = "the seat is not a seat of the frame", false;
  p.step = step, p.card = &B, p.rank = rank, p.t = sc::kRefSeat, p.bank_t = sc::kRefBank, p.bank_s = fsd->bank;  // (every library file is made for the reference identity)
  sc::SeatRows rows;
  if (!RetargetCard(B.rows, p.t, p.bank_t, step.seat, fsd->bank, st::g_roster.pack_id, rank, rows, why, &p.words)) return false;
  // the seat's character node: his pac's rows (HD or SD as the game's list), the model names made the seat's; the EVP rows go (the content is in the EV file)
  if (!RetargetDirRows(st::g_sd ? B.sd : B.hd, p.t, step.seat, p.bank_t, fsd->bank, false, p.node_rows, why)) return false;
  // the card's EVP blobs re-targeted to the seat, laid out as the seat's EV file (the importer stores each with io::BpeIdentity; a blob of any other form refuses the bind)
  if (!st::SeatEvFile(st::g_roster.library, B.card, step.seat, fsd->bank, p.ev, why, &p.actors, &p.blobs)) return false;
  const uint8_t term[4] = {0xFF, 0xFF, 0xFF, 0xFF};
  p.ev_rows = EvNodeRows(p.ev, term);
  p.path = B.node_path;
  char b[40];
  std::snprintf(b, sizeof b, "SSEAT:\\S%d.EV", step.seat);
  p.ev_path = b;
  p.dsbd = st::DsbdMemoryRow(rows.dsbd);
  p.occ = std::make_unique<st::Occupant>();
  p.occ->key = B.key, p.occ->seat = step.seat, p.occ->bank = fsd->bank, p.occ->music = B.card.inst.music, p.occ->guest = true;
  p.occ->rows = std::move(rows);
  return true;
}

// A node of the game's pac list for this seat: made the first time (the game's own sub_82C525A8), re-pointed after.
inline bool PutNode(scene_director::Caller& call, uint8_t* base, NodeBuf& nb, const std::vector<uint8_t>& rows, const std::string& path, std::string& why) {
  if (rows.size() < 8 || rows.size() > (8u << 20) || path.empty() || path.size() > st::kMaxNodePath) return why = "a node's rows or path are not usable", false;
  uint8_t pbytes[st::kPathBytes + 16] = {};
  std::memcpy(pbytes, path.data(), path.size());
  const uint32_t mgr = st::U32(base, st::kPacMgrPtr), list = mgr ? st::U32(base, mgr + 72) : 0;
  if (!Plausible(list) || !st::U32(base, list)) return why = "the pac manager has no node list", false;
  if (nb.node == 0) {
    const uint32_t rbuf = st::Alloc(rows.data(), uint32_t(rows.size())), pbuf = st::Alloc(pbytes, sizeof pbytes);
    if (!rbuf || !pbuf) return why = "the guest heap gave no buffer for a node", false;
    call(sub_82C525A8, {mgr, rbuf, uint64_t(rows.size() - kTermBytes), pbuf});
    const uint32_t node = st::U32(base, list);  // the new head node
    if (!st::Readable(node, 64)) return why = "the new node is not readable", false;
    nb.node = node, nb.rows = st::U32(base, node + st::kNodeRows), nb.cap = uint32_t(rows.size());
    return true;
  }
  if (!st::Readable(nb.node, 64)) return why = "the seat's node is not readable", false;
  // (S110 SEAT3-R review) a node that exists is re-pointed in place: tested HERE, before the first write to it, that the worker has no request out and none
  // queued (a request resolved its node pointer and offsets when it was enqueued; NotNow tested it once at the start of the flip, a few steps ago)
  if (const uint32_t busy = BusyRequest()) {
    char t[80];
    std::snprintf(t, sizeof t, "the pac worker is busy (request %08X): the seat's node was not re-pointed", busy);
    return why = t, false;
  }
  if (rows.size() > nb.cap || !Plausible(nb.rows)) {
    const uint32_t nbuf = st::Alloc(nullptr, uint32_t((rows.size() + 255) & ~size_t(255)));
    if (!nbuf) return why = "the guest heap gave no buffer for a node", false;
    nb.rows = nbuf, nb.cap = uint32_t((rows.size() + 255) & ~size_t(255));
    st::PutBe32(st::P(base, nb.node + st::kNodeRows), nbuf);
  }
  std::memcpy(st::P(base, nb.rows), rows.data(), rows.size());
  st::PutBe32(st::P(base, nb.node + st::kNodeLen), uint32_t(rows.size() - kTermBytes));  // the game's convention: no terminator
  std::memset(st::P(base, nb.node + st::kNodePath), 0, st::kPathBytes);
  std::memcpy(st::P(base, nb.node + st::kNodePath), path.data(), path.size());
  return true;
}

// Writes the arrival (the leave of the old man is done). false = the seat could not be seated (it is left empty by the caller).
inline bool Arrive(PPCContext& ctx, uint8_t* base, scene_director::Caller& call, Prepared& p, std::string& why, std::string& state_words) {
  const int s = p.step.seat;
  const uint32_t rarr = st::U32(base, st::kRosterPtr);
  uint32_t row = 0, prof = 0;
  if (!st::RowAddr(rarr, s, row) || !st::ProfileAddr(s, prof) || !st::Readable(row, st::kRowSize) || !st::Readable(prof, st::kProfileSize))
    return why = "the seat's row / profile are not readable", false;
  // the files first: a failure here leaves the live rows as they were
  std::string werr;
  bool wrote = false;
  const fs::path evfile = st::g_seatev_dir / ("S" + std::to_string(s) + ".ev");
  // (written unless the file there carries the same stamp: seat_alias_plan.h)
  if (st::g_seatev_dir.empty() || !seat_alias::WriteEvIfChanged(evfile, p.ev, p.card->key, s, p.bank_s, wrote, werr)) return why = "the EV file could not be written: " + werr, false;
  if (!st::PublishSeatEv(p.ev_path)) return why = "the EV file is not visible to the game on SSEAT: (" + p.ev_path + ")", false;  // S110 SEATEV (run 142)
  if (!PutNode(call, base, g_char_node[s], p.node_rows, p.path, why)) return false;
  if (!PutNode(call, base, g_ev_node[s], p.ev_rows, p.ev_path, why)) return false;
  // the live row and profile: the game's own conversions, as for a resident (seats.h SeatResident)
  const sc::SeatRows& rows = p.occ->rows;
  std::memcpy(st::P(base, row), rows.dat.data() + st::kDatHeader, st::kRowSize);
  call(sub_829C3578, {row});
  if (!st::ConvertProfile(call, base, rows.pro.data(), rows.pro.size(), prof)) return why = "no scratch buffer for the profile", false;
  state_words = sh::GuestArrives(base, call, s, p.step.arrive);  // his own earlier state, if he has one, laid over the card's rows
  uint32_t off = 0;
  const uint32_t buf = st::g_dsbd_buf.load();
  if (buf && st::DsbdOffset(s, off) && st::Readable(buf + off, st::kDsbdRow)) std::memcpy(st::P(base, buf + off), p.dsbd.data(), st::kDsbdRow);
  const int music = p.occ->music;
  st::PublishOcc(std::move(p.occ));
  superstars::g_music.Set(s, music >= 0 && music < 10000 ? music : -1);
  return true;
}

// A seat with no arrival: nobody sits there, the row is inactive (the lists and the row pass skip it), the frame seat is hidden again.
inline void Vacate(uint8_t* base, int s) {
  st::ClearOcc(s);
  superstars::g_music.Set(s, -1);
  uint32_t row = 0;
  if (st::RowAddr(st::U32(base, st::kRosterPtr), s, row) && st::Readable(row, st::kRowSize)) *st::P(base, row + kActiveOff) = 0;
}

// ================================================================ the portraits and the lists
// Every pool entry holding a touched seat is asked again (sub_8292A910's own stores for a new key; a loading one is cancelled first). A cancelling entry
// (state 3) is not touched: its seat goes to `again` and is asked on a later tick.
inline int ReaskPool(PPCContext& ctx, uint8_t* base, const std::set<int>& seats, std::set<int>& again) {
  const uint32_t ui = Rd(seat_release::kUiPtr), pool = ui ? Rd(ui + seat_release::kPoolOff) : 0;
  if (!Plausible(pool)) return 0;
  const uint32_t count = Rd(pool + seat_release::kPoolCount), arr = Rd(pool + seat_release::kPoolArray);
  if (!Plausible(arr) || count > seat_release::kPoolMax) return 0;
  int n = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t e = arr + seat_release::kPoolStride * i;
    if (!st::Readable(e, seat_release::kPoolStride)) continue;
    const uint32_t id = st::U32(base, e + kPoolId), state = st::U32(base, e + kPoolState);
    for (int s : seats) {
      const Reask r = ReaskStores(id, state, s);
      if (!r.touch) continue;
      if (r.again) {
        again.insert(s);
        continue;
      }
      if (r.cancel_first) {  // the loader's cancel: vcall [[[e + 28]]] + 8 (sub_8292A910 +50..+60)
        const uint32_t obj = st::U32(base, e + 28), vt = Plausible(obj) ? st::U32(base, obj) : 0, fn = Plausible(vt) ? st::U32(base, vt + 8) : 0;
        if (Plausible(fn)) CallGuestAddr(ctx, base, fn, obj);
      }
      for (const Store& w : r.stores) st::PutBe32(st::P(base, e + w.off), w.value);
      ++n;
    }
  }
  return n;
}

inline int RebuildPanels(scene_director::Caller& call, uint32_t screen) {
  int n = 0;
  if (!Plausible(screen)) return 0;
  for (uint32_t i = 0; i < kPanels; ++i) {
    const uint32_t panel = st::U32(call.base, screen + kPanelsAt + 4 * i);
    if (!Plausible(panel) || !st::Readable(panel, 4)) continue;
    call(sub_8285C948, {panel});
    ++n;
  }
  return n;
}

// After a rebuild: the row of the id in the list the panel shows, -1 = not in it.
inline int RowOfId(uint8_t* base, uint32_t panel, uint32_t id) {
  uint32_t list = 0, count = 0;
  if (!letter_jump::Rd(base, panel + letter_jump::kListShown, list) || list > 5) return -1;
  const uint32_t lbase = panel + letter_jump::kListCount + letter_jump::kListStride * list;
  if (!letter_jump::Rd(base, lbase, count) || count > uint32_t(letter_jump::kMaxRows)) return -1;
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t w = 0;
    if (letter_jump::Rd(base, lbase + 4 + 4 * i, w) && (w & 0x7FFFFFFFu) == id) return int(i);
  }
  return -1;
}

inline std::string KeyOr(const std::string& k) { return k.empty() ? std::string("nobody") : k; }

// The alias nodes made to match the seats now (src\seat_alias.h Refresh, through the pointer); a Refresh that could not write every node is owed (g_alias_retry).
inline void RefreshAlias(scene_director::Caller& call, uint8_t* base) {
  if (!st::g_alias_refresh) return;
  if (g_alias_retry.Done(st::g_alias_refresh(call, base)))
    REXLOG_WARN("[wwe2k14][alias] the alias nodes could not be written in {} tries with the flip gate open: left as they are until the next page flip", seat_alias::RefreshRetry::kTries);
}

// ================================================================ one flip
inline void DoFlip(PPCContext& ctx, uint8_t* base, const Request& rq) {
  scene_director::Caller call(ctx, base);
  uint32_t screen = 0;
  if (rq.panel && Plausible(rq.panel)) screen = st::U32(base, rq.panel + letter_jump::kScreen);
  State state = g_state;
  state.screen_held = ScreenHeld(base, call, screen);
  const int pages = g_cat.PageCount();
  if (pages == 0) {
    REXLOG_INFO("[wwe2k14][bind] no page: {} library character(s), {} bindable seat(s)", g_cat.library.size(), g_cat.Bindable().size());
    st::g_page_total.store(0);
    return;
  }
  const int target = rq.page >= 0 ? rq.page : g_cat.Flip(g_page, rq.dir);
  const Plan plan = g_cat.Assign(target, state);
  const int base_rank = BaseRank();
  const std::map<int, int> ranks = base_rank >= -1 ? g_cat.Ranks(base_rank, plan.page, state) : std::map<int, int>();
  auto rank_of = [&](int seat) {
    const auto it = ranks.find(seat);
    return it == ranks.end() ? 0 : it->second;
  };

  // 1. reads and re-targets: nothing is written (a seat that cannot be prepared keeps its man)
  std::vector<Prepared> prep;
  std::set<int> refused;
  for (const Step& s : plan.steps) {
    if (s.arrive.empty()) continue;
    Prepared p;
    std::string why;
    if (!Prepare(s, rank_of(s.seat), p, why)) {
      REXLOG_WARN("[wwe2k14][bind] seat {} refused: {}", s.seat, why);
      refused.insert(s.seat);
      continue;
    }
    prep.push_back(std::move(p));
  }
  // 2. every leaving guest first: state exported, then the game's purge; an export that fails stops that seat
  for (const Step& s : plan.steps) {
    if (s.leave.empty() || refused.count(s.seat)) continue;
    size_t bytes = 0;
    std::string why;
    if (!sh::GuestLeaves(base, call, s.seat, s.leave, bytes, why)) {
      REXLOG_WARN("[wwe2k14][bind] seat {} refused: the export of {} failed ({}): he stays", s.seat, s.leave, why);
      refused.insert(s.seat);
    }
  }
  // 3. every arrival, or the seat emptied
  int rebound = 0, arrived_n = 0, left_n = 0, failed_n = 0;
  std::set<int> touched;
  for (const Step& s : plan.steps) {
    if (refused.count(s.seat)) continue;
    std::string arrived = s.arrive, words = "-";
    int nwords = 0;
    if (!s.arrive.empty()) {
      Prepared* p = nullptr;
      for (Prepared& q : prep)
        if (q.step.seat == s.seat) p = &q;
      std::string why;
      if (p) nwords = p->words;
      if (!p || !Arrive(ctx, base, call, *p, why, words)) {
        REXLOG_WARN("[wwe2k14][bind] seat {} refused: {}: the seat is left empty", s.seat, p ? why : std::string("not prepared"));
        arrived.clear();
        ++failed_n;
      }
    }
    if (arrived.empty()) Vacate(base, s.seat);
    if (arrived.empty()) g_state.guests.erase(s.seat);
    else g_state.guests[s.seat] = arrived;
    sh::OnSessionBind(s.seat, s.leave, arrived, true);
    touched.insert(s.seat);
    ++rebound;
    arrived_n += !arrived.empty(), left_n += !s.leave.empty();
    if (rebound > 3) continue;  // (S110 SEAT3-R review) the first three seats of a flip are logged one by one, the rest only in the summary line below
    if (!arrived.empty()) REXLOG_INFO("[wwe2k14][bind] seat {}: {} leaves; {} arrives (rank {}; {}; {} word(s) re-targeted)", s.seat, KeyOr(s.leave), arrived, rank_of(s.seat), words, nwords);
    else REXLOG_INFO("[wwe2k14][bind] seat {}: {} leaves; nobody arrives", s.seat, KeyOr(s.leave));
  }
  // (S110 LIBSIZE) the alias rows of the seats seated now: ONCE per flip, after every arrival is published and every vacated seat is cleared (an Arrive publishes its
  // occupant at its end, so a call inside it would not see him); Refresh logs its own refusals; a refused write is owed and retried by Tick (RefreshAlias)
  if (!touched.empty() || g_alias_retry.pending) RefreshAlias(call, base);
  g_page = plan.page;
  st::g_page_now.store(plan.page);
  st::g_page_total.store(plan.pages);
  REXLOG_INFO("[wwe2k14][bind] page {} of {}: {} seat(s) rebound ({} arrived, {} left, {} refused or left empty, {} refused before any change)", plan.page + 1, plan.pages, rebound, arrived_n,
              left_n, failed_n, refused.size());

  // 4. the ranks of everybody listed, BEFORE the lists are rebuilt (the rebuild re-runs the counting sort on them)
  WriteRanks(base, ranks);
  // 5. the game's own per-slot model release, the portraits asked again
  for (int s : touched) seat_release::ReleaseSeatModels(ctx, base, s);
  std::set<int> again;
  const int asked = ReaskPool(ctx, base, touched, again);
  if (asked) REXLOG_INFO("[wwe2k14][bind] portraits re-asked: {}", asked);
  if (!again.empty()) g_reask.insert(again.begin(), again.end()), g_reask_tick = g_ticks;
  // 6. every open panel's list rebuilt, then the cursor to the character the letter jump asked for
  if (screen && (!touched.empty() || !rq.jump_key.empty())) {
    const int rebuilt = RebuildPanels(call, screen);
    REXLOG_INFO("[wwe2k14][bind] lists rebuilt for {} panel(s)", rebuilt);
    if (!rq.jump_key.empty() && rq.panel) {
      int seat = -1;
      for (const auto& kv : g_state.guests)
        if (kv.second == rq.jump_key) seat = kv.first;
      if (seat < 0 && g_by_key.count(rq.jump_key) == 0)
        for (const st::Resident& r : st::g_roster.res)
          if (r.key == rq.jump_key) seat = r.seat;
      const int row = seat >= 0 ? RowOfId(base, rq.panel, uint32_t(seat)) : -1;
      if (row >= 0) {
        letter_jump::MoveCursor(call, rq.panel, uint32_t(row));
        REXLOG_INFO("[wwe2k14][bind] cursor to row {} ({}, seat {})", row, rq.jump_key, seat);
      } else {
        REXLOG_WARN("[wwe2k14][bind] cursor: {} is not in the list the panel shows", rq.jump_key);
      }
    }
  }
}

// ================================================================ the tick (game thread, once per owner update)
inline void Tick(PPCContext& ctx, uint8_t* base) {
  if (!g_ready.load(std::memory_order_relaxed)) return;
  ++g_ticks;
  std::optional<Request> rq;
  {
    std::lock_guard lock(g_mx);
    if (g_need_initial && !g_req) {
      Request r;
      r.initial = true, r.page = 0;
      g_req = r, g_req_tick = g_ticks, g_need_initial = false, g_wait_logged = false;
    }
    rq = g_req;
  }
  if (rq) {
    const std::string no = NotNow(base);
    if (!no.empty()) {
      if (!g_wait_logged) {
        REXLOG_INFO("[wwe2k14][bind] waiting: {}", no);
        g_wait_logged = true;
      }
      if (!rq->initial && g_ticks - g_req_tick > kBusyFrames) {
        REXLOG_WARN("[wwe2k14][bind] page request refused: {} (waited {} ticks)", no, kBusyFrames);
        std::lock_guard lock(g_mx);
        g_req.reset();
      }
    } else {
      {
        std::lock_guard lock(g_mx);
        g_req.reset();
      }
      g_wait_logged = false;
      DoFlip(ctx, base, *rq);
    }
  }
  if (g_alias_retry.pending && NotNow(base).empty()) {  // (review S110 LIBSIZE-3) an alias Refresh owed: run under the flip's own gate (the pac worker idle)
    scene_director::Caller call(ctx, base);
    RefreshAlias(call, base);
  }
  if (!g_reask.empty()) {  // a pool entry that was cancelling: asked again until it is not (bounded)
    std::set<int> again;
    ReaskPool(ctx, base, g_reask, again);
    g_reask = again;
    if (!g_reask.empty() && g_ticks - g_reask_tick > kReaskFrames) {
      REXLOG_WARN("[wwe2k14][bind] portraits: {} seat(s) still cancelling after {} ticks: left", g_reask.size(), kReaskFrames);
      g_reask.clear();
    }
  }
}

// ================================================================ the row pass (a boot or a load replaced the live rows)
inline void OnRowPass(PPCContext& ctx, uint8_t* base) {
  (void)ctx;
  sh::OnSessionReset();
  for (const auto& kv : g_state.guests) {  // the guests of the last load are not in the rows any more (the load's own hand-over left them)
    st::ClearOcc(kv.first);
    superstars::g_music.Set(kv.first, -1);
  }
  // (review S110 LIBSIZE-3) their alias rows go with the next Refresh: owed now, run at the first tick the flip gate is open (the initial flip's own Refresh pays it
  // when it seats anybody); until then their seats hold nobody (the load replaced the rows)
  if (!g_state.guests.empty()) g_alias_retry.Owe();
  g_state = State();
  g_page = 0;
  BuildCatalog();
  const State none;
  const int base_rank = BaseRank();
  const std::map<int, int> ranks = base_rank >= -1 ? g_cat.Ranks(base_rank, 0, none) : std::map<int, int>();
  const int written = WriteRanks(base, ranks);  // the residents' ranks, alphabetical with the first page's guests
  const int pages = g_cat.PageCount();
  st::g_page_now.store(0);
  st::g_page_total.store(pages);
  REXLOG_INFO("[wwe2k14][bind] library {} card(s), {} resident(s), bindable seats {} (cap {}): {} page(s), {} rank(s) written", g_cat.library.size(), g_cat.residents.size(),
              g_cat.Bindable().size(), Cap(), pages, written);
  {
    std::lock_guard lock(g_mx);
    g_req.reset();
    g_need_initial = pages > 0;
  }
  g_ready.store(true);
}

// ================================================================ the pager's inputs
inline void Queue(Request r) {
  std::lock_guard lock(g_mx);
  if (g_req) return;  // one at a time: a press while one waits is dropped
  g_req = std::move(r);
  g_req_tick = g_ticks;
  g_wait_logged = false;
}

// The gates the letter jump uses (the select panel's mixin, the exhibition screen's list mode, picking) plus the DOWNLOADED tab (panel +37068 == 3).
inline bool PanelGates(uint8_t* base, uint32_t mixin, uint32_t& panel) {
  uint32_t vt = 0, screen = 0, guard = 1, state = 0, tab = 0;
  if (!Plausible(mixin) || !letter_jump::Rd(base, mixin, vt) || vt != letter_jump::kMixinVtable) return false;
  panel = mixin - 44;
  if (!letter_jump::Rd(base, mixin + 220, screen) || !Plausible(screen) || !letter_jump::Rd(base, screen + letter_jump::kGuard, guard) || guard != 0) return false;
  if (!letter_jump::Rd(base, panel + letter_jump::kState, state) || state < 2 || state > 4) return false;
  return letter_jump::Rd(base, panel + letter_jump::kTab, tab) && tab == 3;
}

// LB / RB: the page flip (the stock handlers play the accept notification and nothing else; the hook always calls them afterwards).
inline void OnBumper(uint8_t* base, uint32_t mixin, int dir) {
  uint32_t panel = 0;
  if (!g_ready.load(std::memory_order_relaxed) || g_cat.library.empty() || !PanelGates(base, mixin, panel)) return;
  Request r;
  r.panel = panel, r.dir = dir;
  Queue(std::move(r));
}

// The letter jump's coupling (src\letter_jump.h g_page_provider, called after its gates, before it looks at the list): when the letter it would go to has no
// row on this page but a library character of that letter sits on another page, flip there and put the cursor on him. true = handled.
inline bool Provider(uint32_t panel, int dir, PPCContext& ctx, uint8_t* base) {
  if (!g_ready.load(std::memory_order_relaxed) || g_cat.library.empty()) return false;
  uint32_t tab = 0, screen = 0;
  if (!letter_jump::Rd(base, panel + letter_jump::kTab, tab) || tab != 3) return false;
  if (!letter_jump::Rd(base, panel + letter_jump::kScreen, screen) || !Plausible(screen)) return false;
  scene_director::Caller call(ctx, base);
  letter_jump::Gathered g;
  if (!letter_jump::GatherRows(base, call, panel, screen, tab, g)) return false;
  bool present[256] = {}, any[256] = {};
  for (const letter_jump::Row& r : g.rows)
    if (r.selectable && r.letter) present[uint8_t(r.letter)] = any[uint8_t(r.letter)] = true;
  for (const Entry& e : g_cat.library)
    if (const char c = letter_jump::Initial(e.name)) any[uint8_t(c)] = true;
  const char cl = g.cur < g.rows.size() ? g.rows[g.cur].letter : char(0);
  if (dir < 0 && cl && present[uint8_t(cl)]) {  // from the middle of a letter's run the jump goes to its start first: the letter jump's own business
    for (const letter_jump::Row& r : g.rows)
      if (r.selectable && r.letter == cl) {
        if (r.index != int(g.cur)) return false;
        break;
      }
  }
  int target = 0;
  if (dir > 0) {
    for (int c = int(uint8_t(cl)) + 1; c < 256 && !target; ++c)
      if (any[c]) target = c;
    for (int c = 0; c < 256 && !target; ++c)
      if (any[c]) target = c;
  } else {
    for (int c = int(uint8_t(cl)) - 1; c >= 0 && !target; --c)
      if (any[c]) target = c;
    for (int c = 255; c >= 0 && !target; --c)
      if (any[c]) target = c;
  }
  if (!target || present[target]) return false;  // no such letter, or this page has it: the letter jump goes there
  State state = g_state;
  state.screen_held = ScreenHeld(base, call, screen);
  const PageRef pr = PageOfLetter(g_cat, char(target), state);
  const Entry* first = FirstOfLetter(g_cat, char(target));
  if (!pr.found || pr.seated || pr.page < 0 || !first) return false;
  Request r;
  r.panel = panel, r.page = pr.page, r.jump_key = first->key;
  Queue(std::move(r));
  return true;
}

inline void Install(const fs::path& user_data_root) {
  g_root = user_data_root;
  st::g_seatev_dir = user_data_root / "superstars" / "seatev";
  REXLOG_INFO("[wwe2k14][bind] seat binds: the guests' EV files go to {} (--wwe2k14_seat_pool_cap={})", sc::detail::U8(st::g_seatev_dir), Cap());
}

inline const bool g_registered = (wwe2k14::scenario::g_seat_tick = &Tick, wwe2k14::letter_jump::g_page_provider = &Provider, st::g_row_pass_hook = &OnRowPass, true);

}  // namespace wwe2k14::seat_bind

// ---------------------------------------------------------------------------------------------------------------- the hooks
REX_HOOK_RAW(sub_823BC3F0) {  // LB: the previous page
  wwe2k14::seat_bind::OnBumper(base, ctx.r3.u32, -1);
  __imp__sub_823BC3F0(ctx, base);
}

REX_HOOK_RAW(sub_823BC558) {  // RB: the next page
  wwe2k14::seat_bind::OnBumper(base, ctx.r3.u32, +1);
  __imp__sub_823BC558(ctx, base);
}
