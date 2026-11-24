// S108 lane CAP: a created arena's textures as the GAME holds them (docs\research\s108\CAP_plan.md). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Pure: std + our pure headers only (no game or SDK includes); the unit tools\scene\car_paint_unit.cpp runs THIS code.
//
// The game paints the player's Create-an-Arena choices (colour, pattern, logo) INTO the stage's own texture objects, after loading them from the part files [C, CAP_plan.md "What the game does"]:
//   the list    [stage + 14452] (CStageCAR::TextureSetup_CAR sub_827648C8 +0x368): +4 the entries, +8 the count; an entry = 8 bytes {registry handle, cached wrapper} (sub_82BA99B8 / sub_82BA9B60),
//               the name = the wrapper's 16 bytes at +16 (the game compares lowercased; the FIRST match wins, sub_82BA99B8 0x82BA9AC8)
//   the writers the save copy sub_822A1DD8, the editor's copy sub_822A3ED8 (after the DXT encode sub_822B8018 / sub_822B5808), sub_83372438: each locks the texture header ([wrapper + 80], sub_82BD4288),
//               writes and calls the unlock flush sub_82CA6BE0 with it (0x822A1EB4, 0x822A407C / 0x822A4088): d3d_textures.h's hook notes it in guest_tex_dirty (NoteFlush)
// So the stage's list is the source of every picture it holds. At the frame's latch (game threads parked) the reader walks the list, reads each entry's whole mip chain with the shared readers
// (texture_registry.h / guest_texture.h ResolveHandle, guest_texture_mips.h ReadChain), watches each header and re-reads an entry when the game flushes it. The applier (rf_c_core.cpp) puts the pictures
// into the created arena's model by name (Apply) before the set is cut into family models.
// Bound [C, the player's own files: q asset pac parts_ent / parts_studium / parts_ring]: the largest stage set is CAEN 400 (<= 3.72 MB, 48 textures) + CAEN 401 (<= 74 KB, 3) + CASD 400 (<= 1.89 MB, 72)
// + CARG 400 (4.56 MB, 41) + CARG 402 (350 KB, 3) = about 10.6 MB / 167 textures: 3 takes at kTakeBudget.
// S108 CAP3 (docs\research\s108\CAP2_plan.md): a snapshot is READY when every entry of the list was tried once ("complete"): exactly that, no wait on the game. CAP2's wait for the game's paint manager was
// removed: a run showed the latch sees the stage BEFORE the game creates and starts its manager (a null manager read as "done"), and the live paint outlasts any take bound. The game's later paint reaches
// us through the unlock-flush re-reads and the debounced rebuild. The manager's words are read fresh at every take as EVIDENCE only (the census: `game:`, `seen_busy`, `built_take`, `built_game`): they gate nothing.
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "../assets/asset_model.h"
#include "guest_tex_dirty.h"
#include "../feed_seen.h"   // S110 FEEDREC
#include "guest_texture_mips.h"

namespace rdr_scene::live::carpaint {

// Test only (tools\scene\car_paint_unit.cpp): 1 file_kept (Apply leaves the file picture: the old rule); 2 no_reread (a flush is ignored); 3 silent_fail (a texture not taken is not reported);
// 4 budget_ignored (the whole list in one take); 5 last_dup_wins (the last of two same-named entries is taken); 6 gen_ignored (a new stage object at the SAME address keeps the old one's state: the
// address alone was the key); 7 not2d_reported (a cube / volume entry of the list is reported as a picture not taken); 9 never_ready_unread (a list never read is never ready: CAP3's hazard); 8 late_paint_ignored (once the set is built, a flush of a painted texture is no longer read again).
inline int g_paint_plant = 0;

inline constexpr uint32_t kStageTexList = 14452, kListData = 4, kListCount = 8, kWrapperName = 16, kMaxEntries = 2048;
inline constexpr uint32_t kTakeBudget = 4u << 20;   // picture bytes read in one take (at least one entry is always read)
inline constexpr uint32_t kPhysSlice = 1u << 20;     // the shared physical reader takes at most 1 MB a call (inputs_glue.h CrowdGuestMem::phys)

// S108 CAP2: the CAA paint manager [C, CAP2_plan.md "What the game does"]. The ctor sub_822A18F8 stores it at 0x836F2C04 (+40 = 4, +41 = 255, +48 = 0, +72 = 5, +76 = 8, +156 = 0).
//   +72  the load machine (sub_822A2760): 0 / 1 / 2 = set up (sub_822A2BA8 writes 0 at the start of a match load or an editor load), 3 = painting (waits for +48 == 0), 4 = painted from the save (waits
//        sub_822AB370), 5 = idle. 2 -> 3 writes +48 = 1, +41 = 0 (0x822A2AD4).
//   +48  the category loop runs (sub_822A3738 returns at once when 0, +0x18); +41 its category, 255 = stopped (+0x24). The loop's end (0x822A3978 / 0x822A397C) writes +48 = 0, +41 = 255 after
//        +156 >= 28 (the copy from the save, sub_822A3A70) or +41 == 28 (the live paint sub_822A5380, every encode finished: its state 3 waits for the async job, [mgr+144] == 0).
//   +76  the editor's one-shot edit repaint (sub_822A5E08; 8 = idle; started only by sub_822A3DA0(mgr, k, 1) +0x8C): not the load, reported only (an edit's repaint arrives through the flush).
inline constexpr uint32_t kPaintMgrGlobal = 0x836F2C04u, kMgrSubCat = 40, kMgrLoop = 48, kMgrLoad = 72, kMgrEdit = 76, kMgrCopied = 156;

struct GameState {
  uint32_t mgr = 0, load = 0, loop = 0, edit = 0, category = 0, copied = 0;   // [0x836F2C04]; +72; +48; +76; +41 byte; +156 byte
  bool readable = false;
  bool Busy() const { return mgr && (load <= 3 || (loop != 0 && category != 255)); }   // evidence only (the census's seen_busy): the load machine before its end, or the category loop's run test (sub_822A3738 +0x18 / +0x24)
  bool operator==(const GameState& o) const {
    return mgr == o.mgr && load == o.load && loop == o.loop && edit == o.edit && category == o.category && copied == o.copied && readable == o.readable;
  }
};
template <class M>
inline GameState ReadGameState(const M& mem) {   // every word fresh (the fixed global only; no pointer kept between takes)
  GameState g;
  mem.fail = false;
  g.mgr = mem.u32(kPaintMgrGlobal);
  if (!mem.fail && g.mgr) {
    g.category = (mem.u32(g.mgr + kMgrSubCat) >> 16) & 255u;
    g.loop = mem.u32(g.mgr + kMgrLoop), g.load = mem.u32(g.mgr + kMgrLoad), g.edit = mem.u32(g.mgr + kMgrEdit);
    g.copied = mem.u32(g.mgr + kMgrCopied) >> 24;
  }
  g.readable = !mem.fail;
  return g;
}

using ReadFn = ::wwe2k14::feedseen::ReadFnT;   // S110 FEEDREC: the plain pointer outside coverage (feed_seen.h)
using PhysFn = bool (*)(uint32_t addr, uint32_t n, uint8_t* dst);
inline std::atomic<PhysFn> g_phys{nullptr};   // the game glue installs it once (stage_fx_glue.h Register)
inline void SetPhys(PhysFn f) { g_phys.store(f, std::memory_order_release); }

inline std::string Lower(const std::string& s) {
  std::string o = s;
  for (char& c : o)
    if (c >= 'A' && c <= 'Z') c = char(c + 32);
  return o;
}

struct Pic {
  std::string name;   // lowercased, as the game compares it; empty when the handle never resolved
  uint32_t handle = 0, wrapper = 0, header = 0, gen = 0;
  uint32_t width = 0, height = 0, game_format = 0, mips = 0;
  std::shared_ptr<const std::vector<uint8_t>> data;   // linear little-endian BC blocks, mip 0 first, each level tightly packed (assets::Texture::data)
  uint64_t hash = 0;
  guest_tex::Why why = guest_tex::Why::Ok;   // the last read's answer
  bool ok = false, tried = false, watched = false, retry = false, fail_counted = false;   // retry: the last failure may pass next time (memory / registry), not a refused shape
  // S108 CAP review: a cube or volume texture of the list (wrapper kind 1 / 2: CAEN 401 cube_00 / cube_01 / cube_ref, CARG 402 ji_bl / ji_mon / ji_top, created by sub_82BA9D68 r6 = 1 / 2 ->
  // sub_82BD4360 r7 = the kind). Not a 2D picture: nothing of ours binds it as one (Apply takes faces == 1 only), so it is counted (`not2d`), never a shortfall.
  bool not2d = false;
  uint32_t fetch4 = 0, fetch5 = 0;   // the fetch dwords that name the level count (mip max level bits 6-9; the mip page), kept for the census's `short` example
};

struct Snapshot {
  uint32_t stage = 0, list = 0, takes = 0;
  bool list_read = false, complete = false;
  uint32_t list_why = 0;   // S108 CAP3b: why the list is not read at the last take: 0 read, 1 the pointer [stage+14452] is null, 2 the list or its entries are refused by the reader (`list` holds the address seen)
  bool ready = false;                       // S108 CAP3: ready = every entry tried once (sticky for the stage); nothing else
  GameState game;                           // the manager's words at the last take (evidence only)
  bool seen_busy = false;                   // the manager was seen painting at some take of this stage (census word)
  uint32_t built_take = 0;                  // the take at which the set first became ready (0 = not yet)
  GameState built_game;                     // the manager's words at that take (mgr 0 = `none`)
  std::vector<Pic> pics;   // in list order
  uint64_t hash = 0;       // the content of every picture (the arena set's identity)
};

struct Counters {
  uint64_t read = 0, reread = 0, failed = 0, list_unread = 0, deferred = 0, unwatched = 0, resets = 0, takes = 0, not2d = 0;
};

inline uint64_t HashBytes(uint64_t h, const uint8_t* p, size_t n) {   // 8 bytes a step (the latch hashes up to ~10 MB once per stage)
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    uint64_t v;
    std::memcpy(&v, p + i, 8);
    h = (h ^ v) * 0x100000001B3ull;
    h ^= h >> 29;
  }
  for (; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
  return h;
}
inline uint64_t HashWord(uint64_t h, uint64_t v) {
  for (int i = 0; i < 8; ++i) h = (h ^ ((v >> (8 * i)) & 255)) * 1099511628211ull;
  return h;
}

template <class RW>
struct WordMem {   // guest_texture.h's memory contract (u32 + mutable fail) over a byte reader
  RW& rw;
  mutable bool fail = false;
  uint32_t u32(uint32_t a) const {
    uint8_t b[4];
    if (!rw(a, b, 4)) return fail = true, 0u;
    return uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | b[3];
  }
};

class Store {
 public:
  // The latch (game threads parked): `stage` = the created arena's stage object, `rw(addr, dst, n)` = the page-checked guest reader, `phys(addr, n, dst)` = the physical pixel reader (<= 1 MB a call).
  // `gen` = the contributor's stage generation (stage_fx_contrib.h stage_gen_: bumped by SetStage and OnStageFreed): S108 CAP review, a new stage object built at the address of a freed one is a new stage
  // (the address alone kept the old stage's sticky `settled` and its pictures by handle).
  template <class RW, class PH>
  void OnLatch(uint32_t stage, RW&& rw, PH&& phys, uint32_t gen = 0) {
    if (!stage) return;
    std::lock_guard<std::mutex> lk(mu_);
    if (stage != stage_ || (gen != gen_ && g_paint_plant != 6)) Reset(stage, gen);
    ++takes_, ++c_.takes;
    bool changed = false;
    const WordMem<std::remove_reference_t<RW>> mem{rw};
    uint32_t list = mem.u32(stage + kStageTexList), data = 0, count = 0;
    if (!mem.fail && list) data = mem.u32(list + kListData), count = mem.u32(list + kListCount);
    std::vector<uint8_t> raw;
    bool list_ok = !mem.fail && list && count <= kMaxEntries && (count == 0 || data);
    if (list_ok && count) {
      raw.resize(size_t(count) * 8);
      list_ok = rw(data, raw.data(), raw.size());
    }
    list_why_ = list_ok ? 0 : (!mem.fail && !list) ? 1 : 2;
    if (!list_ok) {
      if (list) list_ = list_read_ ? list_ : list;   // the address seen, for the shortfall's words (a list once read keeps its own)
      ++c_.list_unread;
    } else {
      std::vector<uint32_t> handles(count);
      for (uint32_t i = 0; i < count; ++i) handles[i] = uint32_t(raw[8 * i]) << 24 | uint32_t(raw[8 * i + 1]) << 16 | uint32_t(raw[8 * i + 2]) << 8 | raw[8 * i + 3];
      if (!list_read_ || list != list_ || handles != handles_) {   // a new or changed list: keep what is known by handle, drop the rest
        std::unordered_map<uint32_t, Pic> old;
        for (Pic& p : pics_) old.emplace(p.handle, std::move(p));
        pics_.clear();
        for (uint32_t h : handles) {
          auto it = old.find(h);
          if (it != old.end()) pics_.push_back(std::move(it->second)), old.erase(it);
          else pics_.emplace_back(), pics_.back().handle = h;
        }
        for (auto& kv : old) Drop(kv.second);
        list_ = list, handles_ = std::move(handles), list_read_ = true, changed = true;
      }
      // pass 1: entries never tried; pass 2: entries the game flushed since they were read, and failed ones (asked again)
      uint64_t used = 0;
      bool stop = false;
      const auto budget_left = [&]() { return g_paint_plant == 4 || used < kTakeBudget; };
      for (int pass = 0; pass < 2 && !stop; ++pass)
        for (Pic& p : pics_) {
          const bool due = pass == 0 ? !p.tried : (p.tried && ((!p.ok && p.retry) || (g_paint_plant != 2 && !(g_paint_plant == 8 && ready_) && p.header && p.watched && guest_tex_dirty::Gen(p.header) != p.gen)));
          if (!due) continue;
          if (used && !budget_left()) { stop = true, ++c_.deferred; break; }
          used += Read(p, mem, rw, phys);
          changed = true;
        }
    }
    bool complete = list_read_;
    for (const Pic& p : pics_) complete = complete && p.tried;
    // S108 CAP3: ready = every entry tried once, sticky for the stage (a list that changes later, or the game's later paint, never takes a built set away: it arrives through the flush re-reads and the
    // set identity). The manager's words (read fresh now) are evidence for the census only.
    const WordMem<std::remove_reference_t<RW>> gm{rw};
    const GameState game = ReadGameState(gm);
    seen_busy_ = seen_busy_ || game.Busy();
    // S108 CAP3b: one rule: a list that was READ is ready when every entry was tried once (sticky: done_); a list never read (null pointer / refused by the reader) is ready with no pictures, so the
    // applier binds the part files' (and reports it once); the store keeps reading every latch and the normal rule takes over at the take that reads the list.
    done_ = done_ || complete;
    const bool ready = done_ || (!list_read_ && g_paint_plant != 9);
    if (ready && !built_take_) built_take_ = takes_, built_game_ = game;
    if (changed || complete != complete_ || ready != ready_ || list_why_ != why_pub_ || !(game == game_)) complete_ = complete, ready_ = ready, why_pub_ = list_why_, game_ = game, Publish();
  }

  // The applier (render thread): the snapshot of `stage` (the current one, or the one before it: the record lags the latch), null = none read for it.
  std::shared_ptr<const Snapshot> For(uint32_t stage) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (pub_ && pub_->stage == stage) return pub_;
    if (prev_ && prev_->stage == stage) return prev_;
    return nullptr;
  }
  std::shared_ptr<const Snapshot> Current() const {
    std::lock_guard<std::mutex> lk(mu_);
    return pub_;
  }
  Counters Counts() const {
    std::lock_guard<std::mutex> lk(mu_);
    return c_;
  }
  void Clear() {
    std::lock_guard<std::mutex> lk(mu_);
    Reset(0);
    prev_.reset();
  }

 private:
  template <class M, class RW, class PH>
  uint64_t Read(Pic& p, const M& mem, RW&& rw, PH&& phys) {
    const bool first = !p.tried;
    p.tried = true;
    guest_tex::Resolved r;
    const guest_tex::Why rw_why = guest_tex::ResolveHandle(mem, p.handle, r);
    if (rw_why == guest_tex::Why::NotPlain2D && g_paint_plant != 7) {   // a cube / volume (kind 1 / 2): decided, not a failure (see Pic::not2d)
      if (!p.not2d) ++c_.not2d;
      p.not2d = true, p.why = rw_why, p.retry = false;
      return 0;
    }
    if (rw_why != guest_tex::Why::Ok) return Fail(p, rw_why), 0;
    if (p.name.empty()) {
      char n[16] = {};
      if (!rw(r.wrapper + kWrapperName, n, 16)) return Fail(p, guest_tex::Why::WrapperUnreadable), 0;
      p.name = Lower(std::string(n, size_t(std::find(n, n + 16, '\0') - n)));
    }
    if (p.header != r.header) {
      if (p.header && p.watched) guest_tex_dirty::Unwatch(p.header);
      p.watched = false;
    }
    p.wrapper = r.wrapper, p.header = r.header;
    if (!p.watched) {
      p.watched = guest_tex_dirty::Watch(r.header);
      if (!p.watched) ++c_.unwatched;
    }
    const uint32_t gen = guest_tex_dirty::Gen(r.header);   // taken BEFORE the read: a flush during it is seen at the next take
    const auto sliced = [&](uint32_t a, uint32_t n, uint8_t* d) {
      for (uint32_t off = 0; off < n; off += kPhysSlice)
        if (!phys(a + off, std::min(kPhysSlice, n - off), d + off)) return false;
      return true;
    };
    guest_tex::Chain c;
    const guest_tex::Why w = guest_tex::ReadChain(r.fetch, sliced, c, guest_tex::kMaxReadHard);
    if (w != guest_tex::Why::Ok) return Fail(p, w), 0;   // (a re-read that fails keeps the picture read before; the census counts it)
    p.why = w, p.ok = true, p.retry = false, p.gen = gen;
    p.width = c.width, p.height = c.height, p.game_format = c.game_format, p.mips = c.mips, p.fetch4 = r.fetch[4], p.fetch5 = r.fetch[5];
    p.hash = HashBytes(HashWord(HashWord(1469598103934665603ull, uint64_t(c.width) << 32 | c.height), uint64_t(c.game_format) << 32 | c.mips), c.data.data(), c.data.size());
    const uint64_t n = c.data.size();
    p.data = std::make_shared<const std::vector<uint8_t>>(std::move(c.data));
    first ? ++c_.read : ++c_.reread;
    return n ? n : 1;
  }
  void Fail(Pic& p, guest_tex::Why w) {
    p.why = w;
    p.retry = w == guest_tex::Why::Memory || w == guest_tex::Why::NoRegistry || w == guest_tex::Why::NoHandle || w == guest_tex::Why::WrapperUnreadable || w == guest_tex::Why::NoHeader;
    if (!p.fail_counted) p.fail_counted = true, ++c_.failed;   // once per entry (a retried texture is not counted every take)
  }
  void Drop(Pic& p) {
    if (p.header && p.watched) guest_tex_dirty::Unwatch(p.header);
    p.watched = false;
  }
  void Reset(uint32_t stage, uint32_t gen = 0) {
    for (Pic& p : pics_) Drop(p);
    if (pub_ && pub_->stage && pub_->stage != stage) prev_ = pub_;   // the record lags the latch: the old stage's snapshot stays for it (For keys on the address)
    if (prev_ && prev_->stage == stage) prev_.reset();               // ...never under the address of the new stage (its record would take the old stage's pictures)
    pics_.clear(), handles_.clear();
    pub_.reset();
    stage_ = stage, gen_ = gen, list_ = 0, takes_ = 0, list_read_ = false, complete_ = false, ready_ = false, done_ = false, list_why_ = 0, why_pub_ = 0, seen_busy_ = false, built_take_ = 0, game_ = GameState(), built_game_ = GameState();
    if (stage) ++c_.resets;
  }
  void Publish() {
    auto s = std::make_shared<Snapshot>();
    s->stage = stage_, s->list = list_, s->takes = takes_, s->list_read = list_read_, s->complete = complete_, s->list_why = list_why_, s->ready = ready_, s->game = game_, s->seen_busy = seen_busy_, s->built_take = built_take_, s->built_game = built_game_;
    s->pics = pics_;
    uint64_t h = HashWord(1469598103934665603ull, uint64_t(list_read_) | uint64_t(ready_) << 1);   // (the game's words are not the set's identity: only the pictures and readiness are)
    for (const Pic& p : pics_) h = HashWord(HashBytes(h, reinterpret_cast<const uint8_t*>(p.name.data()), p.name.size()), p.ok ? p.hash : 0);
    s->hash = h ? h : 1;
    pub_ = std::move(s);
  }

  mutable std::mutex mu_;
  uint32_t stage_ = 0, gen_ = 0, list_ = 0, takes_ = 0;
  bool list_read_ = false, complete_ = false, ready_ = false, done_ = false, seen_busy_ = false;
  uint32_t list_why_ = 0, why_pub_ = 0;
  uint32_t built_take_ = 0;
  GameState game_, built_game_;
  std::vector<uint32_t> handles_;
  std::vector<Pic> pics_;
  std::shared_ptr<const Snapshot> pub_, prev_;
  Counters c_;
};

inline Store& TheStore() {
  static Store s;
  return s;
}

// The latch's one call (stage_fx_contrib.h): the stage of a created arena, the contributor's own guest reader; the physical reader is the glue's. None installed = every picture read fails (each one
// reported when the set builds), never a snapshot that is never ready (the applier would wait for it).
inline bool NoPhys(uint32_t, uint32_t, uint8_t*) { return false; }
inline void OnLatch(uint32_t stage, ReadFn rd, uint32_t gen = 0) {
  const PhysFn ph = g_phys.load(std::memory_order_acquire);
  if (!stage || !rd) return;
  TheStore().OnLatch(stage, rd, ph ? ph : &NoPhys, gen);
}

// ---- the bind (render thread) ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
struct Missing { std::string name, why; };
struct ApplyResult {
  uint32_t bound = 0, file_only = 0, dup = 0, same = 0, differs = 0, shorter = 0;   // same / differs / shorter: the guest chain against the file's picture of that name (shorter = the game's header names fewer levels)
  // S108 CAP review (run 7: equal 0 / differ 45 / short 60): which part differs. differ_shape = another format / size; differ_mip0 = mip 0 differs (the game's picture is another one: the paint);
  // differ_levels = mip 0 equal, a later level differs (the mip-chain reader's fault signature on an unpainted texture). short_*: the level counts of the `shorter` ones (the game's header vs the file).
  uint32_t differ_shape = 0, differ_mip0 = 0, differ_levels = 0;
  uint32_t short_guest_min = 0, short_guest_max = 0, short_file_min = 0, short_file_max = 0;
  std::string short_example;   // the first `shorter` one: name, levels (game / file), its fetch dwords 4 and 5
  std::string levels_names, shape_names;   // CAP2 review (run 17: levels_only 3 / shape 4, unnamed): each levels_only one "name@first differing level", each shape one "name game WxH fN file WxH fN" (at most 8 each)
  std::vector<Missing> missing;   // a drawn name the list holds whose picture could not be taken (the file's is drawn): each one a DrawShortfall("arena_paint", name)
};

inline assets::TexFormat FormatOf(uint32_t game_format) {
  return game_format == 18 ? assets::TexFormat::kBC1 : game_format == 19 ? assets::TexFormat::kBC2 : game_format == 20 ? assets::TexFormat::kBC3 : assets::TexFormat::kUnknown;
}
inline size_t ChainBytes(uint32_t w, uint32_t h, uint32_t mips, uint32_t bpb) {
  size_t n = 0;
  for (uint32_t l = 0; l < mips; ++l) n += size_t((std::max(w >> l, 1u) + 3) / 4) * ((std::max(h >> l, 1u) + 3) / 4) * bpb;
  return n;
}

// Every texture of `m` whose (lowercased) name the snapshot holds takes the game's picture. A name the list holds whose picture is not there (a failed read, or not read when the snapshot settled) keeps
// the file's picture and is listed in `out.missing`. Names the list does not hold keep the file (`file_only`). Live textures (screens, the page) are never touched.
inline void Apply(assets::Model& m, const Snapshot& s, ApplyResult& out) {
  out = ApplyResult();
  std::unordered_map<std::string, size_t> first;
  for (size_t i = 0; i < s.pics.size(); ++i) {
    const Pic& p = s.pics[i];
    if (p.name.empty()) continue;
    auto it = first.find(p.name);
    if (it == first.end()) first.emplace(p.name, i);
    else ++out.dup, it->second = g_paint_plant == 5 ? i : it->second;
  }
  for (const Pic& p : s.pics)   // an entry whose handle never resolved has no name: it cannot be bound to anything, but it is still a texture of the stage the game holds
    if (p.name.empty() && p.tried && !p.not2d && g_paint_plant != 3) out.missing.push_back({"handle " + std::to_string(p.handle), guest_tex::WhyText(p.why)});
  for (assets::Texture& t : m.textures) {
    if (t.live || t.faces != 1) continue;
    const auto it = first.find(Lower(t.name));
    if (it == first.end()) { ++out.file_only; continue; }
    const Pic& p = s.pics[it->second];
    if (!p.ok || !p.data || FormatOf(p.game_format) == assets::TexFormat::kUnknown) {
      if (g_paint_plant != 3) out.missing.push_back({t.name, p.tried ? guest_tex::WhyText(p.why) : "not read when the set was built"});
      continue;
    }
    const assets::TexFormat f = FormatOf(p.game_format);
    if (t.format == f && t.width == p.width && t.height == p.height) {   // the dev self-check (the mip-chain reader's live proof): equal / differs / the game binds fewer levels
      const uint32_t bpb = f == assets::TexFormat::kBC1 ? 8 : 16;
      const size_t common = ChainBytes(p.width, p.height, std::min(p.mips, t.mips), bpb);
      const bool prefix = common <= p.data->size() && common <= t.data.size() && std::memcmp(p.data->data(), t.data.data(), common) == 0;
      if (prefix && p.mips == t.mips) {
        ++out.same;
      } else if (prefix && p.mips < t.mips) {
        out.short_guest_min = out.shorter ? std::min(out.short_guest_min, p.mips) : p.mips, out.short_guest_max = std::max(out.short_guest_max, p.mips);
        out.short_file_min = out.shorter ? std::min(out.short_file_min, t.mips) : t.mips, out.short_file_max = std::max(out.short_file_max, t.mips);
        if (!out.shorter) {
          char e[96];
          std::snprintf(e, sizeof e, "%s %u/%u fetch4 %08X fetch5 %08X", p.name.c_str(), p.mips, t.mips, p.fetch4, p.fetch5);
          out.short_example = e;
        }
        ++out.shorter;
      } else {
        const size_t m0 = ChainBytes(p.width, p.height, 1, bpb);
        const bool mip0 = m0 <= p.data->size() && m0 <= t.data.size() && std::memcmp(p.data->data(), t.data.data(), m0) == 0;
        ++out.differs, ++(mip0 ? out.differ_levels : out.differ_mip0);
        if (mip0 && out.differ_levels <= 8) {   // the first level that differs (the mip-chain reader's question needs the names)
          uint32_t lv = 1;
          for (size_t at = m0; lv < std::min(p.mips, t.mips); ++lv) {
            const size_t n = ChainBytes(std::max(p.width >> lv, 1u), std::max(p.height >> lv, 1u), 1, bpb);
            if (at + n > p.data->size() || at + n > t.data.size() || std::memcmp(p.data->data() + at, t.data.data() + at, n) != 0) break;
            at += n;
          }
          out.levels_names += (out.levels_names.empty() ? "" : " ") + p.name + "@" + std::to_string(lv);
        }
      }
    } else {
      ++out.differs, ++out.differ_shape;
      if (out.differ_shape <= 8) {
        char e[128];
        std::snprintf(e, sizeof e, "%s%s game %ux%u f%u file %ux%u f%d", out.shape_names.empty() ? "" : " ", p.name.c_str(), p.width, p.height, p.game_format, t.width, t.height, int(t.format));
        out.shape_names += e;
      }
    }
    if (g_paint_plant == 1) continue;   // the old rule: the file's picture stays
    t.format = f, t.width = p.width, t.height = p.height, t.mips = p.mips, t.faces = 1, t.data = *p.data;
    ++out.bound;
  }
}

// The census line of one bind (rf_c_core.cpp prints it with every arena-set build of a created arena).
// S108 CAP3: the game's words (the manager [0x836F2C04]: +72 load, +48 loop, +41 category, +156 copied, +76 edit) at the last take, whether it was ever seen painting, and the take (and the manager's
// words) at which the set first became ready: evidence of whether it was built before, during or after the game's paint. Nothing here gates anything.
inline std::string GameText(const Snapshot& s) {
  char b[300], bg[96];
  const GameState& g = s.game;
  const GameState& q = s.built_game;
  if (q.mgr) std::snprintf(bg, sizeof bg, "load72 %u loop48 %u cat41 %u", q.load, q.loop, q.category);
  else std::snprintf(bg, sizeof bg, "none");
  std::snprintf(b, sizeof b, "ready %d built_take %u built_game %s game: mgr %08X readable %d seen_busy %d load72 %u loop48 %u cat41 %u copied156 %u edit76 %u", int(s.ready), s.built_take, bg, g.mgr,
                int(g.readable), int(s.seen_busy), g.load, g.loop, g.category, g.copied, g.edit);
  return b;
}
inline std::string CensusLine(const Snapshot& s, const ApplyResult& a, const Counters& c) {
  char b[2048];
  std::snprintf(b, sizeof b,
                "[wwe2k14][car_paint] stage %08X list %08X entries %zu takes %u %s complete %d read %llu reread %llu failed %llu not2d %llu unwatched %llu deferred %llu bound %u missing %zu file_only %u "
                "dup %u check_equal %u check_differ %u check_short %u (differ: shape %u mip0 %u levels_only %u; short: game levels %u..%u, file levels %u..%u; e.g. %s) levels_only: %s shape: %s",
                s.stage, s.list, s.pics.size(), s.takes, GameText(s).c_str(), int(s.complete), static_cast<unsigned long long>(c.read), static_cast<unsigned long long>(c.reread),
                static_cast<unsigned long long>(c.failed), static_cast<unsigned long long>(c.not2d), static_cast<unsigned long long>(c.unwatched), static_cast<unsigned long long>(c.deferred), a.bound, a.missing.size(), a.file_only, a.dup, a.same,
                a.differs, a.shorter, a.differ_shape, a.differ_mip0, a.differ_levels, a.short_guest_min, a.short_guest_max, a.short_file_min, a.short_file_max,
                a.short_example.empty() ? "-" : a.short_example.c_str(), a.levels_names.empty() ? "-" : a.levels_names.c_str(), a.shape_names.empty() ? "-" : a.shape_names.c_str());
  return b;
}

}  // namespace rdr_scene::live::carpaint
