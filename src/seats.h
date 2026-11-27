// S110 SEAT-B (docs\research\s110\SEAT1.md sections 2 and 4): the game seats imported characters from their LIBRARY cards instead of from one rebuilt
// pack. This is the guest side (the hooks and the writes into the game's own tables); the pure half is src\seats_core.h (unit: tools\seats\seats_unit.cpp).
//
// At boot (superstars::Load) the host reads the frame's `seat` lines and every library card (seats::g_roster). In the game:
//   * AfterRowPass (the hook of sub_82818798, src\superstars_save.h): once per process one pac-list node per resident (the game's own sub_82C525A8,
//     path SSLIB:\<KEY>\PAC\CH\CH<S>_DLC<PP>.PAC, rows = his library pac's directory), then for every resident whose live row is not active (byte +16 == 0:
//     the game's own rule for a row it keeps) the live roster row (B1: file-form row, swapped by sub_829C3578) and the live profile (B2: converted by
//     sub_827DAF20 + sub_827DBCB0, the live reader's own two calls);
//   * sub_827E0CD8 (the DSBD read-end): the residents' 80-byte attire rows (B1');
//   * sub_822701D0 (the ENEX BIN load callback) and the LSD hook in src\music_slots.h: the residents' rows are appended (B8);
//   * sub_8231B330 (the one reader of /CSDT/DATA table 102): a resident's bank is asked as his entrance wrestler's (B9);
//   * sub_82C38588 (the text manager's find): his three name texts (B5);
//   * sub_82817598 (a character's availability in the DLC bundle): a frame seat with no occupant is never offered.
// Every guest write is bounds-checked against the settled tables (seats 100..499; the DSBD buffer's 40000 bytes). Every hook logs once what it did.
// Nothing here runs unless the superstars pack was offered to the game (superstars::g_active), except the two answers for HELD seats (availability
// and the Universe roster check), which key on the roster alone: an unmigrated old pack is never offered, yet its stars' seats are held (D1).
#pragma once

#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "guest_call.h"
#include "guest_read.h"
#include "mods.h"
#include "seat_alias_plan.h"  // S110 LIBSIZE lane 3: the EV file's stamp (WriteEvIfChanged)
#include "seat_occ.h"  // S110 SEAT3: who sits in each seat now (every reader below asks it)
#include "seats_core.h"
#include "superstars.h"

REX_EXTERN(sub_82C525A8);  // pac list: add a node (r3 = the manager, r4 = rows, r5 = rows length without the terminator, r6 = path)
REX_EXTERN(sub_829C3578);  // the roster row's in-place swap (204 bytes)
REX_EXTERN(sub_827DAF20);  // the profile reader's two calls (sub_827F2DC8 +A4 / +AC): r3 = dest, r4 = the decoded file row
REX_EXTERN(sub_827DBCB0);
REX_EXTERN(sub_8226FFC0);  // BIN: the 16-byte row swap
REX_EXTERN(sub_822700A0);  // BIN: the 20-byte row swap
REX_EXTERN(__imp__sub_827E0CD8);
REX_EXTERN(__imp__sub_822701D0);
REX_EXTERN(__imp__sub_8231B330);
REX_EXTERN(__imp__sub_82C38588);
REX_EXTERN(__imp__sub_82817598);
// (S110 SEAT2-HB: the hooks of sub_82B75228 (the Universe roster check) and sub_82B62720 (its validity answer) moved to src\slot_handover.h, beside the
// hand-over's own answer; KeepHeldSeat and g_roster_check stay here, and that hook calls KeepHeldSeat first, unchanged.)

namespace wwe2k14::seats {

constexpr uint32_t kPacMgrPtr = 0x839400C0;      // the pac manager singleton; [mgr + 72] = the list, [list] = the head node, node +60 = next
constexpr uint32_t kRosterPtr = 0x8376F3C0;      // [it] = the 204-byte rows array, row = 204 x (id - 100)
constexpr uint32_t kTextManagerPtr = 0x836F3A50; // [[it] + 76] = the text manager's find owner (B5 gate)
constexpr uint32_t kNodePath = 0, kNodeRows = 48, kNodeLen = 52, kNodeNext = 60, kPathBytes = 48, kMaxNodes = 8192;
constexpr uint32_t kBinLeaf5Rows = 72, kBinLeaf5Count = 96, kBinPartRows = 76, kBinPartCount = 100;  // sub_822701D0's object (+4k per part leaf)
constexpr uint32_t kLsdRecords = 128, kLsdCount = 208, kLsdLists = 20;                                // the LSD list object (music_slots.h)

inline bool Plausible(uint32_t p) { return p >= 0x10000 && p < 0xC0000000u; }
inline uint8_t* P(uint8_t* base, uint32_t a) { return rex::memory::GuestPtr(base, a); }
inline bool Readable(uint32_t a, uint32_t n) {
  return Plausible(a) && n < (64u << 20) && Plausible(a + n) && GuestWordReadableFast(a & ~3u) && GuestWordReadableFast((a + (n ? n - 1 : 0)) & ~3u);
}
inline uint32_t U32(uint8_t* base, uint32_t a) { return Readable(a, 4) ? Be32(P(base, a)) : 0; }

inline uint32_t Alloc(const void* data, uint32_t size) {
  const uint32_t a = REX_KERNEL_MEMORY()->SystemHeapAlloc(size);
  if (a) {
    uint8_t* h = REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(a);
    if (data) std::memcpy(h, data, size);
    else std::memset(h, 0, size);
  }
  return a;
}

inline std::mutex g_mutex;
inline std::map<std::string, uint32_t> g_arrays;  // rows after our change (bytes, tagged) -> the guest copy, reused while unchanged (as music_slots.h)
inline std::map<std::string, uint32_t> g_strings; // a name -> its NUL-terminated guest copy
inline bool g_nodes_done = false;                 // the residents' nodes are in the pac list
inline std::atomic<bool> g_log_dsbd{false}, g_log_bin{false}, g_log_lsd{false}, g_log_t102{false}, g_log_name{false}, g_log_hide{false};

inline bool g_sd = false;                                    // S110 SEAT3: the game's pac list holds the frame's SD node (AddNodes' answer): the guests' nodes use the same rows
inline std::atomic<uint32_t> g_dsbd_buf{0};                  // the DSBD buffer the read-end filled (OnDsbd): a bind writes its seat's 80 bytes into it
inline void (*g_row_pass_hook)(PPCContext&, uint8_t*) = nullptr;  // src\seat_bind.h: runs at the end of every row pass (the page resets, the ranks are written)
inline rex::filesystem::VirtualFileSystem* g_seatev_fs = nullptr;  // set by MountSeatEv: the file system the seatev device is on (PublishSeatEv)
inline std::filesystem::path g_seatev_dir;                  // <user data root>\superstars\seatev (src\seat_bind.h Install): the guests' EV files
inline bool (*g_alias_refresh)(scene_director::Caller&, uint8_t*) = nullptr;  // src\seat_alias.h Refresh: the alias nodes (the pointer: seat_alias.h includes this file)
inline std::map<int, uint32_t> g_res_ev_node;               // seat -> the residents' EV file nodes (AddNodes made them; seat_alias.h flags them as ours)
inline bool PublishSeatEv(const std::string& guest_path);  // (below)

inline bool Ready() { return superstars::g_active && g_roster.loaded && (!g_roster.res.empty() || !g_roster.lib.empty()); }

// ================================================================ the row pass
inline bool EndsWithNoCase(const char* path48, const std::string& tail) {
  size_t n = 0;
  while (n < kPathBytes && path48[n]) ++n;
  if (n < tail.size()) return false;
  for (size_t i = 0; i < tail.size(); ++i) {
    char c = path48[n - tail.size() + i];
    if (c >= 'a' && c <= 'z') c = char(c - 32);
    if (c != tail[i]) return false;
  }
  return true;
}

// S110 LIBSIZE lane 3: a resident's EV file (his EVP blobs re-targeted to his seat and bank, SeatEvFile) written to <seatev>\S<seat>.ev unless the file there carries the same
// stamp (seat_alias_plan.h), made visible to the game (PublishSeatEv), then its node (the guests' file and node, src\seat_bind.h Arrive). A failure leaves him with no entrance banks.
inline void AddResidentEv(scene_director::Caller& call, uint8_t* base, uint32_t mgr, uint32_t list, const Resident& r) {
  sb::EvFile ev;
  std::string err;
  bool wrote = false;
  char ev_path[40];
  std::snprintf(ev_path, sizeof ev_path, "SSEAT:\\S%d.EV", r.seat);
  if (g_seatev_dir.empty()) err = "no seatev folder";
  else if (!SeatEvFile(g_roster.library, r.card, r.seat, r.bank, ev, err)) {
  } else if (!seat_alias::WriteEvIfChanged(g_seatev_dir / ("S" + std::to_string(r.seat) + ".ev"), ev, r.key, r.seat, r.bank, wrote, err)) {
  } else if (!PublishSeatEv(ev_path)) err = "the file is not visible to the game on SSEAT:";
  if (!err.empty()) {
    REXLOG_WARN("[wwe2k14][seats] seat {}: no EV file ({}): his entrance banks are not served", r.seat, err);
    return;
  }
  const uint8_t term[4] = {0xFF, 0xFF, 0xFF, 0xFF};
  const std::vector<uint8_t> rows = sb::EvNodeRows(ev, term);
  uint8_t path[kPathBytes + 16] = {};
  std::memcpy(path, ev_path, std::strlen(ev_path));
  const uint32_t rbuf = Alloc(rows.data(), uint32_t(rows.size())), pbuf = Alloc(path, sizeof path);
  if (!rbuf || !pbuf) {
    REXLOG_WARN("[wwe2k14][seats] the guest heap gave no buffer for seat {}'s EV node: his entrance banks are not served", r.seat);
    return;
  }
  call(sub_82C525A8, {mgr, rbuf, uint64_t(rows.size() - 4), pbuf});
  g_res_ev_node[r.seat] = U32(base, list);
  REXLOG_INFO("[wwe2k14][seats] seat {}: EV file {} ({} blob(s), {} bytes), node added", r.seat, wrote ? "written" : "kept (same stamp)", ev.rows.size(), ev.bytes.size());
}

// One node per resident through the game's own sub_82C525A8, then his EV file and its node, then the alias nodes. Returns false while the pac manager's list does not exist yet (the next call retries).
inline bool AddNodes(scene_director::Caller& call, uint8_t* base, int& added, bool& sd) {
  const uint32_t mgr = U32(base, kPacMgrPtr), list = mgr ? U32(base, mgr + 72) : 0, head = list ? U32(base, list) : 0;
  if (!head) return false;
  char sdname[40];
  std::snprintf(sdname, sizeof sdname, "DLC_SD_DLC%02d.PAC", g_roster.pack_id);
  sd = false;
  std::set<std::string> have;
  int nodes = 0;
  for (uint32_t n = head; n && nodes < int(kMaxNodes); n = U32(base, n + kNodeNext), ++nodes) {
    if (!Readable(n, 64)) break;
    const char* path = reinterpret_cast<const char*>(P(base, n + kNodePath));
    if (EndsWithNoCase(path, sdname)) sd = true;
    size_t len = 0;
    while (len < kPathBytes && path[len]) ++len;
    have.insert(std::string(path, len));
  }
  for (Resident& r : g_roster.res) {
    if (have.count(r.node_path)) continue;  // (already there: a second pass, or a dev swap's node)
    const std::vector<uint8_t>& rows = sd ? r.sd : r.hd;
    uint8_t path[kPathBytes + 16] = {};
    std::memcpy(path, r.node_path.data(), r.node_path.size());
    const uint32_t rbuf = Alloc(rows.data(), uint32_t(rows.size())), pbuf = Alloc(path, sizeof path);
    if (!rbuf || !pbuf) {
      REXLOG_WARN("[wwe2k14][seats] the guest heap gave no buffer for seat {}'s node: left out of the pac list", r.seat);
      continue;
    }
    call(sub_82C525A8, {mgr, rbuf, uint64_t(rows.size() - 4), pbuf});
    r.node_addr = U32(base, list);  // the new head node (the list's first entry)
    ++added;
    AddResidentEv(call, base, mgr, list, r);
  }
  if (g_alias_refresh) g_alias_refresh(call, base);  // S110 LIBSIZE: the residents' EVZ / GMEV / PCE6 / MOVN / base renders, pointed at the game's own entries
  return true;
}

// A card's profile (the 1276-byte file row) converted by the game's own two calls (sub_827F2DC8 +A4 / +AC) into the 1272 live bytes at `dest`. Used for a
// resident (WriteProfile) and for a guest a bind seats (src\seat_bind.h). false = no scratch buffer.
inline bool ConvertProfile(scene_director::Caller& call, uint8_t* base, const uint8_t* pro_file, size_t n, uint32_t dest) {
  static uint32_t src = 0;  // the decoded file row's buffer: the game's reader decodes up to 3320 bytes; ours is the 1276 of rows.bin, zero padded
  if (!src) src = Alloc(nullptr, 3328);
  if (!src || n > 3328) return false;
  std::memset(P(base, src), 0, 3328);
  std::memcpy(P(base, src), pro_file, n);
  // The game's reader converts into a zeroed block (sub_827F2DC8 +20 clears all of block B first); sub_827DAF20's "kind > 100" branch leaves dest
  // +448..+567 unwritten, so without this a reused scratch slot (EnsureProfile) or a live slot would keep another profile's bytes there.
  std::memset(P(base, dest), 0, kProfileSize);
  call(sub_827DAF20, {dest, src});
  call(sub_827DBCB0, {dest});
  return true;
}

// The live profile of a resident: converted by the game's own two calls (sub_827F2DC8 +A4 / +AC) into the LIVE slot the first time (exactly what the
// game's reader does), then kept as bytes (the BANKID shim's "pack row", and every later seating).
inline void WriteProfile(scene_director::Caller& call, uint8_t* base, Resident& r, uint32_t dest) {
  if (r.profile_be.size() == kProfileSize) {
    std::memcpy(P(base, dest), r.profile_be.data(), kProfileSize);
    return;
  }
  if (!ConvertProfile(call, base, r.rows.pro.data(), r.rows.pro.size(), dest)) return;
  r.profile_be.assign(P(base, dest), P(base, dest) + kProfileSize);
}
// A resident whose live row is kept (an active saved row) still needs his pack profile for the shim: converted in a scratch slot.
inline void EnsureProfile(scene_director::Caller& call, uint8_t* base, Resident& r) {
  if (r.profile_be.size() == kProfileSize) return;
  static uint32_t scratch = 0;
  if (!scratch) scratch = Alloc(nullptr, 1280);
  if (scratch) WriteProfile(call, base, r, scratch);
}

// One resident's card rows into his seat: the row (file form, swapped by the game's own sub_829C3578) and the live profile. Used by AfterRowPass and by
// the hand-over (src\slot_handover.h: a character arriving at a seat another one left).
inline void SeatResident(scene_director::Caller& call, uint8_t* base, Resident& r, uint32_t row, uint32_t prof) {
  std::memcpy(P(base, row), r.rows.dat.data() + kDatHeader, kRowSize);
  call(sub_829C3578, {row});
  WriteProfile(call, base, r, prof);
}

inline void AfterRowPass(PPCContext& ctx, uint8_t* base) {
  if (!Ready()) return;
  scene_director::Caller call(ctx, base);
  int added = 0;
  if (!g_nodes_done) {
    bool sd = false;
    if (!AddNodes(call, base, added, sd)) {
      REXLOG_INFO("[wwe2k14][seats] row pass: the pac manager has no node list yet: {} resident(s) not seated this time", g_roster.res.size());
      return;
    }
    g_nodes_done = true;
    g_sd = sd;
    REXLOG_INFO("[wwe2k14][seats] nodes added {} ({} renders: the frame's DLC_SD_DLC{:02d}.PAC node is {} the list)", added, sd ? "SD" : "HD", g_roster.pack_id,
                sd ? "in" : "not in");
  }
  const uint32_t rarr = U32(base, kRosterPtr);
  if (!Plausible(rarr)) {
    REXLOG_WARN("[wwe2k14][seats] row pass: the roster array pointer [0x8376F3C0] is empty: nobody seated");
    return;
  }
  int seated = 0, kept = 0, skipped = 0;
  for (Resident& r : g_roster.res) {
    uint32_t row = 0, prof = 0;
    if (!RowAddr(rarr, r.seat, row) || !ProfileAddr(r.seat, prof) || !Readable(row, kRowSize) || !Readable(prof, kProfileSize)) {
      ++skipped;
      continue;
    }
    if (!ShouldSeat(*P(base, row + 16))) {
      EnsureProfile(call, base, r);
      ++kept;
      continue;
    }
    SeatResident(call, base, r, row, prof);
    ++seated;
  }
  REXLOG_INFO("[wwe2k14][seats] row pass: residents seated {}, kept {} (an active saved row), skipped {}, of {}", seated, kept, skipped, g_roster.res.size());
  if (g_row_pass_hook) g_row_pass_hook(ctx, base);  // S110 SEAT3: the guests of the last load are gone from the rows: the page starts again, the ranks are written
}

// What the BANKID shim reads as "the pack's row" for a seat (src\superstars_save.h): the resident's converted profile, nullptr = none yet.
inline const uint8_t* PackProfile(int seat) {
  const Resident* r = g_roster.BySeat(seat);
  return r && r->profile_be.size() == kProfileSize ? r->profile_be.data() : nullptr;
}

// ================================================================ DSBD (B1')
inline void OnDsbd(uint8_t* base, uint32_t self) {
  if (!Ready()) return;
  const uint32_t buf = U32(base, self + 48);
  if (!Plausible(buf) || !Readable(buf, kDsbdBytes)) return;
  g_dsbd_buf.store(buf);
  int n = 0;
  for (const Occupant* o : Occupants()) {  // S110 SEAT3: the residents and the guests seated now
    uint32_t off = 0;
    if (!DsbdOffset(o->seat, off)) continue;
    const auto m = DsbdMemoryRow(o->rows.dsbd);
    std::memcpy(P(base, buf + off), m.data(), kDsbdRow);
    ++n;
  }
  if (!g_log_dsbd.exchange(true)) REXLOG_INFO("[wwe2k14][seats] DSBD: {} resident row(s) of 80 bytes written into the buffer at {:08X}", n, buf);
}

// ================================================================ BIN (B8)
// One leaf: the rows after our change in a guest copy (cached), the leaf's rows pointer and count pointed at it. `swap` = the game's own swap of one new row.
inline bool InstallRows(scene_director::Caller& call, uint8_t* base, char tag, const Appended& a, size_t rowsize, uint32_t rows_at, uint32_t cnt_at,
                        void (*swap)(PPCContext&, uint8_t*)) {
  std::string key(1, tag);
  key.append(reinterpret_cast<const char*>(a.bytes.data()), a.bytes.size());
  uint32_t addr = 0;
  {
    std::lock_guard lock(g_mutex);
    uint32_t& copy = g_arrays[key];
    if (!copy) {
      copy = Alloc(a.bytes.data(), uint32_t(a.bytes.size()));
      if (copy)
        for (size_t at : a.swap_at) call(swap, {uint64_t(copy) + at});
    }
    addr = copy;
  }
  if (!addr) return false;
  (void)rowsize;
  PutBe32(P(base, rows_at), addr);
  PutBe32(P(base, cnt_at), a.count);
  return true;
}

inline void OnBin(PPCContext& ctx, uint8_t* base, uint32_t self) {
  if (!Ready() || !Plausible(self)) return;
  scene_director::Caller call(ctx, base);
  std::string said;
  {
    const uint32_t rows_at = self + kBinLeaf5Rows, cnt_at = self + kBinLeaf5Count;
    const uint32_t old = U32(base, rows_at), cnt = U32(base, cnt_at);
    if (old && cnt <= 0xFFFF && Readable(old, cnt * 16)) {
      const Appended a = BuildBin5(P(base, old), cnt, Bin5RowsOcc());
      if (a.count && InstallRows(call, base, '5', a, 16, rows_at, cnt_at, sub_8226FFC0)) said += " leaf5 " + std::to_string(cnt) + "->" + std::to_string(a.count);
    }
  }
  for (int k = 0; k < sc::kBinpLeaves; ++k) {
    const std::vector<const uint8_t*> add = BinpRowsOcc(k);
    if (add.empty()) continue;
    const uint32_t rows_at = self + kBinPartRows + 4u * uint32_t(k), cnt_at = self + kBinPartCount + 4u * uint32_t(k);
    const uint32_t old = U32(base, rows_at), cnt = U32(base, cnt_at);
    if (!old || cnt > 0xFFFF || !Readable(old, cnt * 20)) continue;
    const Appended a = BuildBinPart(P(base, old), cnt, add);
    if (InstallRows(call, base, char('0' + k), a, 20, rows_at, cnt_at, sub_822700A0)) said += " part" + std::to_string(k) + " " + std::to_string(cnt) + "->" + std::to_string(a.count);
  }
  if (!g_log_bin.exchange(true)) REXLOG_INFO("[wwe2k14][seats] ENEX BIN (object {:08X}): rows appended:{}", self, said.empty() ? " none" : said);
}

// ================================================================ LSD (B8): called from the /ENEX/LSD load hook (src\music_slots.h), BEFORE the music hook
// (today's pack held the residents' rows in the file, so they come before the songs the music hook appends).
inline void LsdAppend(uint8_t* base, uint32_t self) {
  if (!Ready() || !Plausible(self)) return;
  std::string said;
  for (int list : LsdListsOcc()) {
    if (list < 0 || uint32_t(list) >= kLsdLists) continue;
    const uint32_t count_at = self + kLsdCount + 2u * uint32_t(list), recs_at = self + kLsdRecords + 4u * uint32_t(list);
    if (!Readable(count_at & ~3u, 8) || !Readable(recs_at, 4)) continue;
    const uint32_t count = Be16(P(base, count_at)), recs = Be32(P(base, recs_at));
    if (!recs || count > 32000 || !Readable(recs, count * 8)) continue;
    const auto add = LsdRowsOcc(list);
    if (add.empty()) continue;
    const std::vector<uint8_t> bytes = BuildLsd(P(base, recs), count, add);
    uint32_t addr = 0;
    {
      std::lock_guard lock(g_mutex);
      uint32_t& copy = g_arrays[std::string(1, char('A' + list)) + std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size())];
      if (!copy) copy = Alloc(bytes.data(), uint32_t(bytes.size()));
      addr = copy;
    }
    if (!addr) continue;
    PutBe32(P(base, recs_at), addr);
    PutBe16(P(base, count_at), uint16_t(count + add.size()));
    said += " " + std::to_string(list) + "(+" + std::to_string(add.size()) + ")";
  }
  if (!g_log_lsd.exchange(true)) REXLOG_INFO("[wwe2k14][seats] ENEX LSD (object {:08X}): rows appended to lists{}", self, said.empty() ? " none" : said);
}

// ================================================================ B9, B5, availability
// sub_8231B330 (r4 = the key): a resident's bank is asked as his entrance wrestler's.
inline void OnTable102(PPCContext& ctx) {
  if (!Ready()) return;
  const int64_t src = SrcBankOcc(ctx.r4.u32);
  if (src < 0) return;
  if (!g_log_t102.exchange(true)) REXLOG_INFO("[wwe2k14][seats] table 102: key {} answered as his entrance wrestler's bank {} (first of the run)", ctx.r4.u32 & 0xFFFF, src);
  ctx.r4.u64 = uint64_t(src);
}

// sub_82C38588 (r3 = the owner, r5 = the text id): true = answered (r3 = the guest string).
inline bool OnTextFind(PPCContext& ctx, uint8_t* base) {
  if (!Ready()) return false;
  const int32_t id = int32_t(ctx.r5.u32);
  std::string title;
  const std::string* nm = NameForOcc(id);
  if (!nm && id == 276 && g_page_total.load(std::memory_order_relaxed) > 1) title = PageTitle(), nm = &title;  // the DOWNLOADED list's title says the page (L: text 276)
  if (!nm) return false;
  const uint32_t mgr = U32(base, kTextManagerPtr), owner = mgr ? U32(base, mgr + 76) : 0;
  if (!owner || ctx.r3.u32 != owner) return false;
  uint32_t addr = 0;
  {
    std::lock_guard lock(g_mutex);
    uint32_t& s = g_strings[*nm];
    if (!s) s = Alloc(nm->c_str(), uint32_t(nm->size() + 1));
    addr = s;
  }
  if (!addr) return false;
  if (!g_log_name.exchange(true)) REXLOG_INFO("[wwe2k14][seats] names: text id {} answered with '{}' (first of the run)", id, *nm);
  ctx.r3.u64 = addr;
  return true;
}

// sub_82817598 (r4 = the character id): a frame seat with no occupant answers 0.
inline bool HideFrameSeat(PPCContext& ctx) {
  // (keyed on the roster, not on g_active: an unmigrated old pack is never offered, yet its held seats must answer 0; the roster is loaded only
  // with USE MY SUPERSTARS on, superstars::Load)
  if (!g_roster.loaded) return false;
  const uint32_t id = ctx.r4.u32;
  if (OccOf(int(id))) return false;  // S110 SEAT3: a guest sits there (HideSeat knows residents only)
  if (!HideSeat(g_roster, id)) return false;
  if (!g_log_hide.exchange(true))
    REXLOG_INFO("[wwe2k14][seats] availability: seat {} has no occupant: not offered (first of the run; {} seat(s) of the frame, {} occupied)", id,
                g_roster.frame.seats.size(), g_roster.res.size());
  ctx.r3.u64 = 0;
  return true;
}

// ================================================================ D1: the Universe roster check never purges a HELD seat
// GUARD (S110 D1): the game has no "installed but hidden" answer. Its Universe roster check sub_82B75228 asks sub_82B62720(id, 1) and on 0 (a) flags
// the id (+7) on the 10 matches of the current card (+1C4 -> sub_82B7A580 at +1E0; the game rebuilds the unplayed auto matches, builder mode 1; special (bit-3) matches are stripped by universe_strip.h, S110 UNISTRIP) and (b) for an id 100..499 with an active live row runs the Universe purge
// (The builder's jobs see a handed-over seat row-inactive for the rest of that Universe day: universe_rebuild.h, S110 UNIAVAIL.)
// sub_82B72538(id) (+280 -> +290). The guard is at that DECISION, not at sub_82B72538, because (a) destroys state outside the purge. sub_82B62720 has
// 248 call sites: the answer changes ONLY inside the roster check (g_roster_check, this thread) and only for a seat held by a card that was left out
// (seats_core.h KeepUniverseState), which answers "valid" until the launcher rebuilds the card. An empty seat (no card) is purged as the game rules.
inline thread_local int g_roster_check = 0;
inline std::set<int> g_kept_logged;  // (g_mutex) one line per held seat
inline bool KeepHeldSeat(PPCContext& ctx) {
  if (g_roster_check <= 0 || ctx.r4.u32 != 1) return false;  // (not gated on g_active: an unmigrated old pack's held seats need it most)
  const uint32_t id = ctx.r3.u32;
  if (!KeepUniverseState(g_roster, id)) return false;
  {
    std::lock_guard lock(g_mutex);
    if (g_kept_logged.insert(int(id)).second) {
      const auto it = g_roster.held.find(int(id));
      REXLOG_INFO("[wwe2k14][seats] seat {} is held by a card that was left out: its Universe state is kept ({})", id,
                  it == g_roster.held.end() ? std::string("?") : it->second);
    }
  }
  ctx.r3.u64 = 1;
  return true;
}

// ================================================================ the library mount
// sslib: (read-only) = <mods>\superstars\library. The nodes' paths start with it; the game opens a character's pac through it.
inline void MountLibrary(rex::filesystem::VirtualFileSystem* fs) {
  if (!fs || !Ready()) return;
  const std::filesystem::path root = mods::g_dir / "superstars" / "library";
  std::error_code ec;
  if (!std::filesystem::is_directory(root, ec)) return;
  std::string existing;
  if (fs->FindSymbolicLink("SSLIB:", existing)) {
    REXLOG_INFO("[wwe2k14][seats] sslib: already mounted ({})", existing);
    return;
  }
  constexpr std::string_view kMountPath = "\\Device\\WWE2K14SSLib";
  auto device = std::make_unique<rex::filesystem::HostPathDevice>(kMountPath, root, true);
  if (!device->Initialize() || !fs->RegisterDevice(std::move(device)) || !fs->RegisterSymbolicLink("SSLIB:", kMountPath)) {
    REXLOG_WARN("[wwe2k14][seats] sslib: could not be mounted: the residents' files cannot be read");
    return;
  }
  REXLOG_INFO("[wwe2k14][seats] sslib: mounted at {} ({} resident(s))", mods::U8(root), g_roster.res.size());
}

// seatev: (writable) = <user data root>\superstars\seatev: the guests' EV files (src\seat_bind.h: S<seat>.ev, one pac-shaped file per seat holding the
// entrance-music / entrance blobs re-targeted to the seat). The seats' EV nodes' paths start with it.
inline void MountSeatEv(rex::filesystem::VirtualFileSystem* fs) {
  if (!fs || !Ready() || g_seatev_dir.empty()) return;
  std::error_code ec;
  std::filesystem::create_directories(g_seatev_dir, ec);
  if (!std::filesystem::is_directory(g_seatev_dir, ec)) {
    REXLOG_WARN("[wwe2k14][seats] seatev: {} could not be made: a guest with an entrance cannot be seated", mods::U8(g_seatev_dir));
    return;
  }
  std::string existing;
  if (fs->FindSymbolicLink("SSEAT:", existing)) return;
  constexpr std::string_view kMountPath = "\\Device\\WWE2K14SSSeat";
  auto device = std::make_unique<rex::filesystem::HostPathDevice>(kMountPath, g_seatev_dir, false);
  if (!device->Initialize() || !fs->RegisterDevice(std::move(device)) || !fs->RegisterSymbolicLink("SSEAT:", kMountPath)) {
    REXLOG_WARN("[wwe2k14][seats] seatev: could not be mounted: a guest with an entrance cannot be seated");
    return;
  }
  g_seatev_fs = fs;
  REXLOG_INFO("[wwe2k14][seats] seatev: mounted at {}", mods::U8(g_seatev_dir));
}

// S110 SEATEV (docs\research\s110\SEATEV.md, run 142): the kernel's open (VirtualFileSystem::OpenFile) takes the FILE NAME only from the device's
// cached list, which is the folder as it was at mount time; a file we write on the host afterwards is not in it and the open returns
// STATUS_NO_SUCH_FILE (the game then shows its disc-error screen and hangs). ResolvePath goes through the device's lazy stat, which ADDS the
// file to the list (host_path_device.cpp ResolvePath), and update() refreshes its size after a rewrite: the same thing the game's own
// file-create path does. false = the file is not visible to the game: the caller refuses the bind.
inline bool PublishSeatEv(const std::string& guest_path) {
  if (!g_seatev_fs) return false;
  rex::filesystem::Entry* e = g_seatev_fs->ResolvePath(guest_path);
  if (!e) return false;
  e->update();
  return true;
}

}  // namespace wwe2k14::seats

// ---------------------------------------------------------------------------------------------------------------- the hooks
// (pre / post each; none changes what a game with no import sees: Ready() / g_active gate every one)
REX_HOOK_RAW(sub_827E0CD8) {
  const uint32_t self = ctx.r3.u32;
  __imp__sub_827E0CD8(ctx, base);
  wwe2k14::seats::OnDsbd(base, self);
}

REX_HOOK_RAW(sub_822701D0) {
  const uint32_t self = ctx.r3.u32;
  __imp__sub_822701D0(ctx, base);
  wwe2k14::seats::OnBin(ctx, base, self);
}

REX_HOOK_RAW(sub_8231B330) {
  wwe2k14::seats::OnTable102(ctx);
  __imp__sub_8231B330(ctx, base);
}

// SKIP-ORIGINAL: OnTextFind answers a seat's name (or the DOWNLOADED page title) with our guest string when the owner is the text manager's; every other lookup runs the original
REX_HOOK_RAW(sub_82C38588) {
  if (wwe2k14::seats::OnTextFind(ctx, base)) return;
  __imp__sub_82C38588(ctx, base);
}

// SKIP-ORIGINAL: HideFrameSeat answers 0 (not offered) for a frame seat with no occupant; every other id runs the original
REX_HOOK_RAW(sub_82817598) {
  if (wwe2k14::seats::HideFrameSeat(ctx)) return;
  __imp__sub_82817598(ctx, base);
}
