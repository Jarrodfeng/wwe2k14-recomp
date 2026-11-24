// S110 SEAT2 lane H-B (docs\research\s110\SEAT2.md sections 1-3, with the adversary's corrections): the GAME side of "state follows the character".
// When the character sitting in a roster seat differs from the one the loaded save's rows belong to, the old character's own state is exported to a
// file beside the save, the game's own purge runs for the seat, the new character is seated from his card, and his own earlier state (if any) is laid back
// over it. The pure half (the side record, the state file, the plan, the re-targeting) is src\slot_handover_core.h (unit: tools\seats\handover_unit.cpp).
//
// Hooks (all game logic, no renderer; each does nothing for a game with USE MY SUPERSTARS off beyond one hash of the save image):
//   * sub_8290CBC0 (save load, vtable slot +372) and sub_829166C0 (the load that follows when sub_8290CBC0 returned 3, slot +376, r4 != 0): a LOAD SCOPE
//     (thread-local depth) that remembers the save manager and stamps the image BEFORE the original runs (sub_829166C0 writes payload bytes +203..+205
//     before its copy). The row pass the game reaches inside either (our hook of sub_82818798, src\superstars_save.h) calls BeforeSeating.
//   * sub_8290C9E8 (the ONE save packer, vtable slot +368): after it, the written image's stamp + the owner map go into the side record.
//   * sub_82B75228 (the Universe roster check, moved here from src\seats.h) and sub_82B62720 (its validity answer): a seat handed over since the Universe
//     last looked answers "gone" inside that scope, so the game's own loop flags him (+7) on the current card and runs its own purge for the seat. The game
//     rebuilds the unplayed auto matches (builder mode 1); special (bit-3) matches are stripped by universe_strip.h (S110 UNISTRIP).
//     The builder's jobs see the seat row-inactive for the rest of that Universe day (universe_rebuild.h, S110 UNIAVAIL), so the new occupant is not booked anew.
// Every guest read / write is bounds-checked against the settled tables (seats 100..499; the payload range). Nothing logs per frame.
#pragma once

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "guest_call.h"
#include "seats.h"
#include "slot_handover_core.h"
#include "superstars.h"
#include "universe_rebuild.h"
#include "universe_strip.h"

REX_EXTERN(sub_827DB798);  // the game's purge of a gone character id (r3 = id)
REX_EXTERN(sub_827DA3E8);  // remove-and-compact of the 256-entry list (r3 = the list, r4 = id): the first match only, returns 0 when none
REX_EXTERN(sub_822554F8);  // title record writer: [r3 + 4 (r4 + 41)] = r5
REX_EXTERN(sub_822C0B18);  // index of r3 in the u16 table 0x82006A00 (235 entries), -1 = none
REX_EXTERN(sub_827DB390);  // the game's init of one 64-byte O4 entry (r3 = the entry): what the new-save init of block B does per id (sub_827F2DC8 +D4)
REX_EXTERN(__imp__sub_8290CBC0);
REX_EXTERN(__imp__sub_829166C0);
REX_EXTERN(__imp__sub_8290C9E8);
REX_EXTERN(__imp__sub_82B75228);
REX_EXTERN(__imp__sub_82B62720);

namespace wwe2k14::slot_handover {

namespace fs = std::filesystem;

// ---- the settled addresses (SEAT2.md section 1; the adversary re-derived each from the listings) ----
constexpr uint32_t kPayloadPtr = 32284;                                    // [save manager + 32284] = the payload (header 28, block A, block B, footer)
constexpr uint32_t kBlockB = 0x83891878;                                   // the profile block's base (B)
constexpr uint32_t kO3 = 0x8390DBFC, kO3Stride = 40;                       // O3: + 40 (id - 100)
constexpr uint32_t kO4 = 0x839155C4, kO4Stride = 64;                       // O4: + 64 id
constexpr uint32_t kO5 = 0x8391D2C4, kO5Stride = 8;                        // O5: + 8 id
constexpr uint32_t kCntA = kBlockB + 0x811C0, kCntB = kBlockB + 0x81398;   // O6: u16 at + 2c each
constexpr uint32_t kCntC = kBlockB + 0x92C30;                              // O6: u8 at + c
constexpr int kCompactCount = 235;                                         // the table 0x82006A00 holds 235 ids (sub_822C0B18)
constexpr uint32_t kTitles = 0x112828, kTitleStride = 676, kTitleAlt = 164, kTitleCount = 86, kTitleSlots = 2;  // the alternate holders of titles 0..85
constexpr uint32_t kHolderList = 0x121874;                                 // block A + this = the 256 x 12-byte list sub_827DA3E8 compacts
constexpr int kListCap = 256;
constexpr uint32_t kNoTitleHolder = 512;
constexpr size_t kProBankWord = 576;                                       // a profile's entrance bank word (superstars_migrate::kPackBankWord)

// ================================================================ the process state
struct LoadScope {
  int depth = 0;           // > 0 while one of the two load functions runs on this thread
  uint32_t mgr = 0;        // the save manager (r3 at entry)
  uint64_t stamp = 0;      // ImageStamp of the payload, taken BEFORE the original ran
  bool stamped = false;    // the payload was readable
  bool done = false;       // BeforeSeating already ran in this scope (a second row pass in one load never hands over again)
};
inline thread_local LoadScope g_scope;

struct Session {
  bool have = false;                                         // this process loaded a save image and handled it
  uint64_t stamp = 0;                                        // that image
  bool rec_ok = true;                                        // the side record could be read (false: it is never written this boot)
  bool have_entry = false;                                   // the image has an entry (found, or made by adopting)
  Entry entry;                                               // (the loaded entry)
  std::optional<std::map<int, std::string>> next_owners;     // NextOwners of the load: what the next save write records (nullopt = no entry)
};
inline std::mutex g_mx;
inline fs::path g_root;                 // <user data root>
inline Session g_sess;
inline Record g_rec;                    // the side record as this process knows it (published by a load, extended by a write)
inline bool g_rec_known = false;
inline std::set<int> g_pending;         // seats handed over whose Universe check has not looked yet
inline std::set<int> g_answered_logged;
inline std::set<int> g_answered_in_check;  // (g_mx) pending seats the current Universe roster check asked about (answered gone)
inline const st::Roster g_off{};        // loaded == false: MakePlan CARRIES (no hand-over)

inline bool Active() { return superstars::g_active && seats::g_roster.loaded && !seats::g_roster.old_pack; }

// ================================================================ the image stamp
inline bool ImageStampOf(uint8_t* base, uint32_t mgr, uint64_t& out) {
  if (!seats::Plausible(mgr)) return false;
  const uint32_t pay = seats::U32(base, mgr + kPayloadPtr);
  if (!seats::Plausible(pay) || !seats::Readable(pay, uint32_t(kImgEnd))) return false;
  out = ImageStamp(seats::P(base, pay));
  return true;
}

// ================================================================ a seat's own state in guest memory
struct SeatAddrs {
  uint32_t row = 0, pro = 0, e40 = 0, e64 = 0, m8 = 0;
  int c = -1;  // compact index (O6), -1 = none
  uint32_t ca = 0, cb = 0, cc = 0;
};
// false = a table the seat needs is not readable (the roster array pointer empty, an address out of range).
inline bool SeatAt(uint8_t* base, scene_director::Caller& call, int seat, SeatAddrs& a) {
  a = SeatAddrs();
  if (!ValidSeat(seat)) return false;
  const uint32_t rarr = seats::U32(base, seats::kRosterPtr);
  if (!seats::Plausible(rarr) || !seats::RowAddr(rarr, seat, a.row) || !seats::ProfileAddr(seat, a.pro)) return false;
  a.e40 = kO3 + kO3Stride * uint32_t(seat - kFirstSeat);
  a.e64 = kO4 + kO4Stride * uint32_t(seat);
  a.m8 = kO5 + kO5Stride * uint32_t(seat);
  if (!seats::Readable(a.row, seats::kRowSize) || !seats::Readable(a.pro, seats::kProfileSize) || !seats::Readable(a.e40, kE40Bytes) ||
      !seats::Readable(a.e64, kE64Bytes) || !seats::Readable(a.m8, kM8Bytes))
    return false;
  const int32_t c = int32_t(call(sub_822C0B18, {uint64_t(seat)}));  // the game's own lookup (never rebuilt here)
  if (c >= 0 && c < kCompactCount) {
    a.ca = kCntA + 2u * uint32_t(c), a.cb = kCntB + 2u * uint32_t(c), a.cc = kCntC + uint32_t(c);
    if (seats::Readable(a.ca, 2) && seats::Readable(a.cb, 2) && seats::Readable(a.cc, 1)) a.c = c;
  }
  return true;
}

inline std::string Stamp() {  // a file-name stamp: seconds since the epoch
  return std::to_string(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

// O1..O6 of a seat, read from live memory (= the loaded image, before any purge).
inline bool ReadOwnState(uint8_t* base, const SeatAddrs& a, int seat, State& s) {
  s = State();
  s.seat = uint16_t(seat);
  std::memcpy(s.row.data(), seats::P(base, a.row), kRowBytes);
  std::memcpy(s.pro.data(), seats::P(base, a.pro), kProBytes);
  std::memcpy(s.e40.data(), seats::P(base, a.e40), kE40Bytes);
  std::memcpy(s.e64.data(), seats::P(base, a.e64), kE64Bytes);
  std::memcpy(s.m8.data(), seats::P(base, a.m8), kM8Bytes);
  s.bank = seats::Be16(s.pro.data() + kProBankWord);
  s.frame_sig = seats::g_roster.frame.has_sig ? seats::g_roster.frame.sig : 0;
  if (a.c >= 0) {
    s.has_cnt = true;
    std::memcpy(s.cnt.data(), seats::P(base, a.ca), 2);
    std::memcpy(s.cnt.data() + 2, seats::P(base, a.cb), 2);
    s.cnt[4] = *seats::P(base, a.cc);
  }
  return true;
}

// Exports the seat's state for `key` to <root>\superstars\state\<KEY>.bin (written, read back, compared). false = it failed and the seat must stay as it is.
inline bool ExportSeat(uint8_t* base, scene_director::Caller& call, int seat, const std::string& key, State& out, size_t& bytes, std::string& why) {
  SeatAddrs a;
  if (!SeatAt(base, call, seat, a)) { why = "the seat's tables are not readable"; return false; }
  const fs::path path = StatePath(g_root, key);
  if (path.empty()) { why = "'" + key + "' is not a character key"; return false; }
  ReadOwnState(base, a, seat, out);
  if (!WriteState(path, out, why)) return false;
  std::error_code ec;
  bytes = size_t(fs::file_size(path, ec));
  return true;
}

// The game's purge for a left seat (SEAT2.md 3.3 + the corrections): the game's own sub_827DB798, then the 256-entry list, then the alternate title
// holders. Then the seat's O3..O6 are cleared (the old man's bytes are exported; they must not stay on the seat for the next man).
inline void PurgeSeat(uint8_t* base, scene_director::Caller& call, int seat, int& list_removed, int& titles_cleared) {
  list_removed = 0, titles_cleared = 0;
  call(sub_827DB798, {uint64_t(seat)});
  const uint32_t rarr = seats::U32(base, seats::kRosterPtr);
  if (!seats::Plausible(rarr)) return;
  for (int i = 0; i < kListCap; ++i) {  // the game's remover takes the first match only
    if (call(sub_827DA3E8, {uint64_t(rarr) + kHolderList, uint64_t(seat)}) == 0) break;
    ++list_removed;
  }
  const uint32_t titles = rarr + kTitles;
  if (seats::Readable(titles, kTitleStride * kTitleCount)) {
    for (uint32_t t = 0; t < kTitleCount; ++t)
      for (uint32_t k = 0; k < kTitleSlots; ++k)
        if (seats::Be32(seats::P(base, titles + kTitleStride * t + kTitleAlt + 4 * k)) == uint32_t(seat)) {
          call(sub_822554F8, {uint64_t(titles) + kTitleStride * t, k, kNoTitleHolder});
          ++titles_cleared;
        }
  }
  // The seat's O3..O6 back to what a never-edited character holds (the game's new-save init of block B, sub_827F2DC8): O3 / O5 cleared (+20 memset),
  // O4 = the game's own record (sub_827DB390, +D4: NOT zero: +4 = 42, +44 = 1), O6 = FreshCnt (the u8 is 1: +11C sub_827D8A38). (review of H-B)
  SeatAddrs a;
  if (SeatAt(base, call, seat, a)) {
    std::memset(seats::P(base, a.e40), 0, kE40Bytes);
    call(sub_827DB390, {a.e64});
    std::memset(seats::P(base, a.m8), 0, kM8Bytes);
    if (a.c >= 0) {
      const std::array<uint8_t, 5> f = FreshCnt();
      std::memcpy(seats::P(base, a.ca), f.data(), 2);
      std::memcpy(seats::P(base, a.cb), f.data() + 2, 2);
      *seats::P(base, a.cc) = f[4];
    }
  }
}

// Lays a state over the seat the card was just seated at (SEAT2.md 2.5). The live row / profile now hold the card's.
inline void LayState(uint8_t* base, const SeatAddrs& a, int seat, const State& s, std::string& what) {
  uint8_t card_row[kRowBytes], card_pro[kProBytes], row[kRowBytes], pro[kProBytes];
  std::memcpy(card_row, seats::P(base, a.row), kRowBytes);
  std::memcpy(card_pro, seats::P(base, a.pro), kProBytes);
  RetargetRow(s.row.data(), card_row, s.seat, seat, row);
  std::string pw;
  RetargetProfile(s.pro.data(), card_pro, s.seat, int(s.bank), seat, pro, &pw);
  std::memcpy(seats::P(base, a.row), row, kRowBytes);
  std::memcpy(seats::P(base, a.pro), pro, kProBytes);
  std::memcpy(seats::P(base, a.e40), s.e40.data(), kE40Bytes);
  std::memcpy(seats::P(base, a.e64), s.e64.data(), kE64Bytes);
  std::memcpy(seats::P(base, a.m8), s.m8.data(), kM8Bytes);
  if (s.has_cnt && a.c >= 0) {
    std::memcpy(seats::P(base, a.ca), s.cnt.data(), 2);
    std::memcpy(seats::P(base, a.cb), s.cnt.data() + 2, 2);
    *seats::P(base, a.cc) = s.cnt[4];
  }
  what = pw;
}

// The player-facing note: a clause on LINE 1 of <mods>\superstars\game_status.txt ("in N, ..., K imported superstar(s) may show ..."), the line the
// launchers read and word (src\importer\screen_text.h GameStatusSentence). 0 removes it. (review of H-B: a second line was read by nobody)
inline void SetStatusNote(int unseen) {
  try {
    if (mods::g_dir.empty()) return;
    const std::string line = superstars_fit::ReadStatus(mods::g_dir);
    const std::string want = WithUnseenClause(line, unseen);
    if (want != line) superstars::WriteStatus(want);
  } catch (...) {
  }
}

// The state a character left on disk (state\<KEY>.bin). false = none, or refused: a state file that is there and refused is NEVER overwritten by his next
// export; it is put aside (renamed) and said so.
inline bool ReadStateOf(int seat, const std::string& key, State& s) {
  const fs::path path = StatePath(g_root, key);
  std::error_code ec;
  if (path.empty() || !fs::exists(path, ec)) return false;
  std::string e2;
  if (ReadState(path, s, e2)) return true;
  const fs::path aside = path.parent_path() / (key + ".bad-" + Stamp());
  fs::rename(path, aside, ec);
  REXLOG_WARN("[wwe2k14][handover] seat {}: the state file of {} is refused ({}): {} as {}", seat, key, e2, ec ? "could NOT be renamed aside" : "renamed aside",
              sc::detail::U8(aside.filename()));
  return false;
}

// ================================================================ a guest in session (src\seat_bind.h, SEAT3.md section 2)
// The guests seated now (a page bound them): seat -> key. (g_mx) A save written now records them as the owners of those seats, with the word `guest`.
inline std::map<int, std::string> g_guest_owner;

// A guest leaves his seat in session: the same two steps a load makes for a man who leaves (BeforeSeating): his state is exported to state\<KEY>.bin (written,
// read back), THEN the game's own purge runs for the seat. false = the export failed: nothing was purged and the seat is as it was (Settle's rule).
inline bool GuestLeaves(uint8_t* base, scene_director::Caller& call, int seat, const std::string& key, size_t& bytes, std::string& why) {
  if (g_root.empty()) { why = "no user data folder (the state cannot be kept)"; return false; }
  State s;
  if (!ExportSeat(base, call, seat, key, s, bytes, why)) return false;
  int removed = 0, titles = 0;
  PurgeSeat(base, call, seat, removed, titles);
  return true;
}

// A guest arrives at a seat his card's rows were just written to (row + profile): his own earlier state, if he has one, is laid over them (BeforeSeating's step 3).
// Returns the words for the log.
inline std::string GuestArrives(uint8_t* base, scene_director::Caller& call, int seat, const std::string& key) {
  SeatAddrs ad;
  State s;
  if (g_root.empty() || !SeatAt(base, call, seat, ad) || !ReadStateOf(seat, key, s)) return "from the card";
  std::string what;
  LayState(base, ad, seat, s, what);
  return "own state laid over" + (what.empty() ? std::string() : " (" + what + ")");
}

// What the in-memory owner map and the pending list learn from a bind: the leaving man's seat awaits the Universe check's purge (BeforeSeating's own rule for a
// seat a load purged); the guest seated now is the owner a save written now records.
inline void OnSessionBind(int s, const std::string& leave_key, const std::string& arrive_key, bool guest) {
  std::lock_guard lock(g_mx);
  if (!leave_key.empty()) g_pending.insert(s);
  if (!guest) return;
  if (arrive_key.empty()) g_guest_owner.erase(s);
  else g_guest_owner[s] = arrive_key;
}
// A load replaced the rows: no guest is seated any more (the load's own plan left them).
inline void OnSessionReset() {
  std::lock_guard lock(g_mx);
  g_guest_owner.clear();
}

// ================================================================ the hand-over (in a load scope, at the row pass)
inline void BeforeSeating(PPCContext& ctx, uint8_t* base) {
  LoadScope& ls = g_scope;
  if (ls.depth <= 0 || ls.done) return;  // (the DLC pass end reaches the row pass with no load: nothing to hand over there)
  ls.done = true;
  if (g_root.empty()) return;
  if (!ls.stamped) {
    REXLOG_WARN("[wwe2k14][handover] load: the save image was not readable at the load: no hand-over this time");
    std::lock_guard lock(g_mx);
    g_sess = Session();
    universe_rebuild::LoadHeld(false, {});  // S110 UNIAVAIL: no entry, no hold
    return;
  }
  scene_director::Caller call(ctx, base);
  const uint64_t stamp = ls.stamp;
  Record rec;
  std::string err;
  const bool rec_ok = ReadRecord(RecordPath(g_root), rec, err);
  if (!rec_ok) REXLOG_WARN("[wwe2k14][handover] the side record is not usable ({}): nothing is handed over and the record is not written this boot", err);
  for (const std::string& n : rec.notes) REXLOG_WARN("[wwe2k14][handover] {}", n);
  const Entry* owner = rec_ok ? rec.Find(stamp) : nullptr;
  const bool active = Active() && seats::Plausible(seats::U32(base, seats::kRosterPtr));
  const st::Roster& R = active ? seats::g_roster : g_off;

  const Plan plan = MakePlan(owner, R);
  const char* kind = plan.carry ? "carried" : owner ? "found" : (!rec_ok || rec.entries.empty()) ? "adopted" : "unknown";
  int unknown_seats = 0;
  if (plan.adopt && rec_ok && !rec.entries.empty()) {  // an image the record never saw, and the newest one it knew says other men: say so
    const Entry& newest = rec.entries.front();
    for (const st::Resident& x : seats::g_roster.res) {
      const std::string* o = OwnerOf(newest, x.seat);
      if (!o || *o != x.key) ++unknown_seats;
    }
    if (unknown_seats)
      REXLOG_WARN("[wwe2k14][handover] {} imported superstar(s) may show another superstar's saved edits: this save was not seen before (it is adopted as it is)",
                  unknown_seats);
  }
  SetStatusNote(unknown_seats);

  // 1. export every leaving man FIRST (the purge rewrites his profile +1256..+1271, his O10 and his title rows)
  std::set<int> failed;
  std::map<std::string, State> exported;
  std::map<int, size_t> bytes;
  std::map<int, std::string> why;
  for (const SeatKey& l : plan.leaves) {
    State s;
    size_t n = 0;
    std::string w;
    if (ExportSeat(base, call, l.seat, l.key, s, n, w)) exported[l.key] = s, bytes[l.seat] = n;
    else failed.insert(l.seat), why[l.seat] = w;
  }
  const Outcome out = Settle(plan, failed);

  // 2. purge every left seat (a seat whose export failed is kept as it was)
  std::map<int, std::string> left_text;
  for (const SeatAction& a : out.seats) {
    if (!a.proceed || a.leave.empty()) continue;
    int lr = 0, tc = 0;
    PurgeSeat(base, call, a.seat, lr, tc);
    left_text[a.seat] = a.leave + " leaves (state exported, " + std::to_string(bytes[a.seat]) + " bytes), purged";
  }
  // 3. seat every arrival from his card, then lay his own state over it
  int moved = 0;
  for (const SeatAction& a : out.seats) {
    if (!a.proceed) {
      REXLOG_WARN("[wwe2k14][handover] seat {}: export failed ({}): kept as it was", a.seat, failed.count(a.seat) ? why[a.seat] : a.why);
      continue;
    }
    ++moved;
    std::string arrive = "nobody arrives";
    if (!a.arrive.empty()) {
      seats::Resident* r = seats::g_roster.BySeat(a.seat);
      SeatAddrs ad;
      if (!r || !SeatAt(base, call, a.seat, ad)) {
        arrive = a.arrive + " could not be seated (his seat's tables are not readable)";
      } else {
        seats::SeatResident(call, base, *r, ad.row, ad.pro);
        State s;
        bool have = false;
        const auto ex = exported.find(a.arrive);
        if (ex != exported.end()) s = ex->second, have = true;
        else have = ReadStateOf(a.seat, a.arrive, s);
        std::string what;
        if (have) LayState(base, ad, a.seat, s, what);
        arrive = a.arrive + " arrives (" + (have ? "own state laid over" : "from the card") + ")";
        if (have && !what.empty()) REXLOG_INFO("[wwe2k14][handover] seat {}: re-targeted from seat {}:{}", a.seat, s.seat, what);
      }
    }
    REXLOG_INFO("[wwe2k14][handover] seat {}: {}; {}", a.seat, left_text.count(a.seat) ? left_text[a.seat] : std::string("nobody left"), arrive);
  }

  // 4. the next save write's owner map, the pending list, the loaded entry
  Session sess;
  sess.have = true, sess.stamp = stamp, sess.rec_ok = rec_ok;
  sess.next_owners = NextOwners(owner, R, plan, out);
  std::vector<int> pend = NextPending(owner, out);
  std::set<int> pending;
  for (int p : pend) {
    if (st::KeepUniverseState(seats::g_roster, uint32_t(p))) {  // a held seat is never handed over (MakePlan): both at once is a fault, not a case
      REXLOG_ERROR("[wwe2k14][handover] seat {} is both held and pending a Universe purge: the purge is dropped (a held seat keeps its state)", p);
      continue;
    }
    pending.insert(p);
  }
  if (owner) sess.entry = *owner, sess.have_entry = true;
  if (plan.adopt && rec_ok && sess.next_owners) {  // the entry is written at once: a library change before the first save write is still handed over next boot
    Entry e = MakeEntry(stamp, FrameWord(R), *sess.next_owners, {}, nullptr);
    sess.entry = e, sess.have_entry = true;
    Prepend(rec, e, stamp);
    std::string werr;
    if (!WriteRecord(RecordPath(g_root), rec, &werr)) REXLOG_WARN("[wwe2k14][handover] the side record could not be written: {}", werr);
  }
  {
    std::lock_guard lock(g_mx);
    g_sess = std::move(sess);
    g_rec = std::move(rec);
    g_rec_known = rec_ok;
    g_pending = pending;
    g_answered_logged.clear();
    universe_rebuild::LoadHeld(g_sess.have_entry, g_sess.entry.held);  // S110 UNIAVAIL: this save's hold, never the last save's
  }
  REXLOG_INFO("[wwe2k14][handover] load: image {} entry {}; {} seat(s) change hands", Hex16(stamp), kind, moved);
}

// ================================================================ the save write
inline void OnSaveWritten(uint8_t* base, uint32_t mgr) {
  if (g_root.empty()) return;
  uint64_t stamp = 0;
  if (!ImageStampOf(base, mgr, stamp)) return;
  std::lock_guard lock(g_mx);
  const bool active = Active() && seats::Plausible(seats::U32(base, seats::kRosterPtr));
  const st::Roster& R = active ? seats::g_roster : g_off;
  std::optional<std::map<int, std::string>> owners;
  const Entry* prev = nullptr;
  std::optional<uint64_t> protect;
  bool rec_ok = true;
  if (g_sess.have) {
    owners = g_sess.next_owners;
    prev = g_sess.have_entry ? &g_sess.entry : nullptr;
    protect = g_sess.stamp;
    rec_ok = g_sess.rec_ok;
  } else {  // no load in this process (a new save): the seated men own the rows he writes
    std::string err;
    rec_ok = ReadRecord(RecordPath(g_root), g_rec, err);
    g_rec_known = rec_ok;
    owners = NextOwners(nullptr, R, MakePlan(nullptr, R), Outcome());
  }
  if (!rec_ok) return;  // (logged at the load; an unreadable record is never overwritten blindly)
  if (!owners) {
    REXLOG_INFO("[wwe2k14][handover] save written: image {}, no entry (imports off)", Hex16(stamp));
    return;
  }
  const std::vector<int> pend(g_pending.begin(), g_pending.end());
  for (const auto& kv : g_guest_owner) (*owners)[kv.first] = kv.second;  // S110 SEAT3: the guests seated now own their rows (a load left the old ones)
  Entry made = MakeEntry(stamp, FrameWord(R), *owners, pend, prev);
  made.held = universe_rebuild::HeldNow();  // S110 UNIAVAIL
  for (const auto& kv : g_guest_owner) made.owner_words[kv.first] = "guest";
  const bool added = Prepend(g_rec, std::move(made), protect);
  std::string err;
  if (added && !WriteRecord(RecordPath(g_root), g_rec, &err)) {
    REXLOG_WARN("[wwe2k14][handover] save written: image {}, but the side record could not be written: {}", Hex16(stamp), err);
    return;
  }
  REXLOG_INFO("[wwe2k14][handover] save written: image {}, {}", Hex16(stamp), added ? "entry added" : "entry already there");
}

// ================================================================ the Universe answer
// Inside the roster-check scope only: a seat handed over since the Universe last checked answers "gone" (sub_82B62720(id, 1) = 0), so the game's own loop
// flags the seat's participant (+7) on the 10 matches of the current card and runs its own purge sub_82B72538 for the seat, exactly as for a removed
// Created Superstar. The game rebuilds the unplayed auto matches (builder mode 1); special (bit-3) matches are stripped by universe_strip.h (S110 UNISTRIP).
// The builder's jobs see the seat row-inactive for the rest of that Universe day (universe_rebuild.h, S110 UNIAVAIL): the new occupant is not booked anew.
// The answer is 0 to every ask inside one check. KeepHeldSeat answers first (a held
// seat is never pending: BeforeSeating drops such a seat from the list).
inline bool PendingGone(PPCContext& ctx) {
  if (seats::g_roster_check <= 0 || ctx.r4.u32 != 1) return false;
  const uint32_t id = ctx.r3.u32;
  std::lock_guard lock(g_mx);
  if (!g_pending.count(int(id))) return false;
  if (g_answered_logged.insert(int(id)).second) REXLOG_INFO("[wwe2k14][handover] universe: seat {} answered gone once (handed over)", id);
  g_answered_in_check.insert(int(id));
  ctx.r3.u64 = 0;
  return true;
}
// A seat leaves the pending list only when this check asked about it (the game's own purge then ran for it); one it never reached stays pending, in
// memory and in the next save write's entry (review of H-B: clearing the whole list lost a purge the game had not run).
inline void AfterUniverseCheck() {
  std::lock_guard lock(g_mx);
  g_pending = PendingAfterCheck(g_pending, g_answered_in_check);
  g_answered_in_check.clear();
}
// The seats the current check answered gone (a copy; read BEFORE AfterUniverseCheck clears it).
inline std::set<int> AnsweredGone() {
  std::lock_guard lock(g_mx);
  return g_answered_in_check;
}

inline void Install(const fs::path& user_data_root) {
  g_root = user_data_root;
  REXLOG_INFO("[wwe2k14][handover] side record: {}", sc::detail::U8(RecordPath(g_root)));
}

// ================================================================ the load scope
inline void EnterLoad(PPCContext& ctx, uint8_t* base) {
  LoadScope& ls = g_scope;
  if (ls.depth++ != 0) return;
  ls.mgr = ctx.r3.u32, ls.done = false;
  ls.stamped = ImageStampOf(base, ls.mgr, ls.stamp);  // BEFORE the original: sub_829166C0 writes payload bytes before its copy
}
// `loaded` = the load really put the image in place (sub_8290CBC0 returned 0; sub_829166C0 always).
inline void LeaveLoad(bool loaded) {
  LoadScope& ls = g_scope;
  if (ls.depth <= 0) return;
  if (--ls.depth != 0) return;
  if (loaded && ls.stamped && !ls.done && !g_root.empty()) {  // the load ran no row pass (no DLC manager): nothing was handed over
    REXLOG_INFO("[wwe2k14][handover] load: image {} entry unseen (the load reached no row pass); 0 seat(s) change hands", Hex16(ls.stamp));
    std::lock_guard lock(g_mx);
    g_sess = Session();
    universe_rebuild::LoadHeld(false, {});  // S110 UNIAVAIL: no entry, no hold
  }
}

}  // namespace wwe2k14::slot_handover

// ---------------------------------------------------------------------------------------------------------------- the hooks
REX_HOOK_RAW(sub_8290CBC0) {
  wwe2k14::slot_handover::EnterLoad(ctx, base);
  __imp__sub_8290CBC0(ctx, base);
  wwe2k14::slot_handover::LeaveLoad(ctx.r3.u32 == 0);
}

REX_HOOK_RAW(sub_829166C0) {
  if (ctx.r4.u32 == 0) {
    __imp__sub_829166C0(ctx, base);
    return;
  }
  wwe2k14::slot_handover::EnterLoad(ctx, base);
  __imp__sub_829166C0(ctx, base);
  wwe2k14::slot_handover::LeaveLoad(true);
}

REX_HOOK_RAW(sub_8290C9E8) {
  const uint32_t mgr = ctx.r3.u32;
  __imp__sub_8290C9E8(ctx, base);
  wwe2k14::slot_handover::OnSaveWritten(base, mgr);
}

// D1 + SEAT2: the Universe roster check's scope (moved here from src\seats.h), and its validity answer
REX_HOOK_RAW(sub_82B75228) {
  const uint32_t umode = ctx.r3.u32;
  ++wwe2k14::seats::g_roster_check;
  __imp__sub_82B75228(ctx, base);
  if (--wwe2k14::seats::g_roster_check == 0) {
    const std::set<int> gone = wwe2k14::slot_handover::AnsweredGone();
    wwe2k14::universe_strip::StripSpecialBookings(ctx, base, umode, gone);  // S110 UNISTRIP: bit-3 matches
    wwe2k14::universe_rebuild::NoteCheck(base, gone);                      // S110 UNIAVAIL: the builder's jobs of today see them inactive
    wwe2k14::slot_handover::AfterUniverseCheck();
  }
}

REX_HOOK_RAW(sub_82B62720) {
  if (wwe2k14::seats::KeepHeldSeat(ctx)) return;
  if (wwe2k14::slot_handover::PendingGone(ctx)) return;
  __imp__sub_82B62720(ctx, base);
}
