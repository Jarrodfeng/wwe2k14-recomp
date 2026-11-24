// The wrestlers' liquid maps (sweat / blood / damage), recorded (Session 103, lane SWF; docs\research\s103\SWB.md; docs\systems\liquid_map.md). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Header-only, no game or SDK includes: the guest reads are template parameters (as sphere_hook.h), so tools\scene\swf_unit.cpp runs this code against a fake guest.
//
// WHAT IT RECORDS. The map object is a 'CCLM' component (one per character; vtable 0x82031CBC; slot 10 = sub_8279ACD8, the node that draws it). At the node's ENTRY (r3 = the map object, db sweat_map_node_render_entry;
// SWB section 2: the items are final after the per-frame update, slot 6 sub_82727E60) this hook reads:
//   [C SWB 1a, q asm 0x8279ACE8..0x8279AD08]  +2572 enable (the node returns at once when 0), +92 head of the LIVE item list (+88 count), +136 the clear request, +200 the alt-path request,
//                                             +124 / +128 / +132 the map's width / height / levels, +48 the character (its +48 = the skeleton object, +5072 / +5076 the dim predicate sub_8259F0E0).
//   [C SWB 1d, sub_82798748 / sub_82798888 / sub_82799B60]  an item = 160 bytes: +8 next, +20 type, +60 flags, +64 the source texture object, +80..92 LocalArea, +112..124 WorldParam, +128..140 the dirty rect, +144 the pass select.
//   [C q asm sub_82726170]                    the resources exist when words (+152, +160, +168) and (+156, +164, +172) are each all non-zero (the node draws only then).
// The source texture: item+64 is a texture WRAPPER of the class whose vtable slot 2 is sub_82BD4288 = "first non-null of fields +80, +88, +84" [C q vtable 0x8206ADB8: the wrapper class the registry walk also checks]. The
// header it returns holds the six fetch dwords at +28 (guest_texture.h). [L] that [item+64] is that class: the hook CHECKS the vtable word (0x8206ADB8 / 0x82210B40) and counts `tex_class` when it is anything else (the item is
// then recorded with no picture: the host draws nothing for it, counted, never a guess).
//
// WHEN. One row per map per hook frame, the LAST call of the frame wins (the node runs on up to 2 walk threads plus main; frames, ring, TakeNext and CloseBefore are sphere_hook.h's). The hook never writes the guest
// itself: the glue (liquid_glue.h) consumes the clear request after recording it when the node is skipped (the game's own node does that at 0x8279AE00).
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../renderer_scene/rows_liquid.h"
#include "../renderer_scene/rows_pictures.h"
#include "guest_texture.h"

namespace rdr_scene::live::liquid {

// Fields of the map object [C SWB 1a]
inline constexpr uint32_t kMapEnable = 2572, kMapLiveCount = 88, kMapLiveHead = 92, kMapClear = 136, kMapAlt = 200, kMapWidth = 124, kMapHeight = 128, kMapLevels = 132, kMapChar = 48;
inline constexpr uint32_t kMapResources = 152;   // words +152, +156 (A), +160, +164 (B), +168, +172 (C): sub_82726170
inline constexpr uint32_t kCharSkeleton = 48, kCharDimA = 5076, kCharDimB = 5072;
// Fields of an item [C SWB 1d]
inline constexpr uint32_t kItemBytes = 160, kItNext = 8, kItType = 20, kItState = 60, kItTex = 64, kItLocal = 80, kItWorld = 112, kItRect = 128, kItPass = 144;
inline constexpr uint32_t kStateBitsNotDrawn = kLiStateNotDrawn;   // bit30, bit29, bit26 (sub_82725E28 / E38 / E58): paused + not drawn / not drawn [C SWB 1d]
inline constexpr uint32_t kMaxItems = kLiquidLiveMax;   // the pool's cap of live items [C sub_82726DF0 0x82726E68]
// S108 BLD2: the game's REBUILD draw sub_8279A328 (docs\research\s108\BLD_plan.md section 10, all [C q asm]). The 160-byte pool [map+84] is two sub-pools (sub_82798A50, k = 0 blood, 1 sweat): sub-pool k = items
// [ [map+68+4k], +[map+60+4k] ). Loop 1 walks the SWEAT sub-pool (start +72, count +64). Loop 2 walks, per list k, the entries [map+76+4k] (20 bytes: +0 first item, +4 count, +8 the region's texture = the first item's
// +64, +12 dirty, +16 flag; built by sub_82727178) against the region records [[map+160+4k]+8] + 20j (+4..+16 = the corners in map UV; count [[map+160+4k]+4]).
inline constexpr uint32_t kMapPool = 84, kMapSweatStart = 72, kMapSweatCount = 64, kMapEntries = 76, kMapRegionLists = 160;
inline constexpr uint32_t kItBit27 = 0x08000000u, kItBit30 = 0x40000000u;   // sub_82725E48 / sub_82725E18 (bit 27: repaint once), sub_82725E28 (bit 30)
inline constexpr uint32_t kEntryBytes = 20, kEntTex = 8, kEntDirty = 12, kEntFlag = 16, kRegionBytes = 20, kRegionCorners = 4, kListCount = 4, kListRecords = 8;
inline constexpr uint32_t kMaxPoolWalk = kLiquidRebuildMax, kMaxRegionWalk = kLiquidRegionMax / 2;   // bounds on guest counts (a torn read never walks far): the sweat sub-pool, each of the two region lists
inline constexpr uint32_t kAltPage = 64, kLoop1PerCall = 63;   // S108 BLD3 review [C q asm sub_8279A328]: loop 2's draws per call (0x8279A6F4 / 0x8279A7D0 / 0x8279A8B0), loop 1's per call (0x8279A5D8: the 64th ends it)

// What the game's ALT draw writes back to the game's state (sub_8279A328 0x8279A5E4 bit 27 cleared on every sweat item it walks with bit 27; 0x8279A7F8 entry +12 = 0; sub_8279AC78 0x8279ACC0 +200 = 0). Our skip
// replaces the node, so the glue performs exactly these writes after the row is recorded (as it does for +136); the tracker itself never writes the guest.
struct AltConsume {
  uint32_t map = 0;
  std::vector<uint32_t> items;     // sweat items whose +60 loses bit 27
  std::vector<uint32_t> entries;   // entries whose +12 becomes 0
  bool Empty() const { return map == 0; }
};
// The writes, in the game's order (loop 1, loop 2, then +200). read_word / write_word: host values.
template <class ReadWordFn, class WriteWordFn>
inline void ApplyAltConsume(const AltConsume& c, ReadWordFn read_word, WriteWordFn write_word) {
  if (c.Empty()) return;
  for (uint32_t it : c.items) {
    uint32_t s = 0;
    if (read_word(it + kItState, s)) write_word(it + kItState, s & ~kItBit27);
  }
  for (uint32_t e : c.entries) write_word(e + kEntDirty, 0u);
  write_word(c.map + kMapAlt, 0u);
}
inline constexpr uint32_t kWrapperVtableA = 0x8206ADB8u, kWrapperVtableB = 0x82210B40u;   // the texture wrapper's vtables (texture_registry.h)
inline constexpr uint32_t kWrapperHeader0 = 80, kWrapperHeader1 = 88, kWrapperHeader2 = 84;   // sub_82BD4288 returns the first non-null of these fields
inline constexpr size_t kMaxMaps = 16;             // maps a hook frame keeps (a match has 2 wrestlers + 7 other slots)
inline constexpr size_t kRing = 8;                 // closed frames kept for Emit
inline constexpr size_t kCacheMax = 512;   // S108 BLD3: was 128 (a rebuild row names a picture per dirty region; run 18: > 128 distinct in one frame). S108 BLD3 review: a hook frame's picture list has NO count cap: it holds
                                           // exactly the distinct pictures its rows name (dedup by key; the rows themselves are bounded by the guest's counts), so no named picture is ever left out
inline constexpr uint64_t kResendFrames = 1800;    // S108 BLD3: a rebuild package is re-sent until the host acknowledges it, at most this many hook frames (30 s); then dropped and counted (`expired`). S108 BLD3 review: checked
                                                   // at every frame close for every package (a map the game freed is never called again: its package, up to its pictures, would otherwise be kept for the session)

enum class Plant : uint32_t {
  None,
  FirstCall,        // a second call of the same map in a frame does not replace the first                       -> unit [last call wins]
  NoVtableCheck,    // the source texture object is not checked for the wrapper class                           -> unit [texture class]
  StaleFrame,       // the row is stamped with the previous hook frame                                           -> unit [frame stamp]
  OldPictureCap,    // the liquid picture read keeps the crowd's 4096-byte cap (S105 A12)                          -> unit [blood size]
  NoEnableCheck,    // the enable word and the empty-list early out are ignored                                  -> unit [early outs]
  AltNotCarried,    // S108 BLD2: a later call of the same frame drops the earlier call's rebuild rows             -> unit [rebuild last call]
  NoResend,         // S108 BLD3: the rebuild package rides only on its own frame's row (BLD2: lost when that record is not drawn) -> unit [rebuild resend]
  Loop1OneCall,     // S108 BLD3 review: loop 1 capped at one call's 63 whatever the regions (the game pages: 63 per call)       -> unit [rebuild pages]
  RegionsCapped,    // S108 BLD3 review: the dirty regions capped at 256 (BLD3's bound; the game draws them all)               -> unit [rebuild pages]
  NoPrune,          // S108 BLD3 review: a package is dropped only on a later call of its own map (a freed map's package is kept) -> unit [rebuild prune]
  PicturesCapped,   // S108 BLD3 review: a hook frame keeps at most 256 pictures (BLD3's cap)                                    -> unit [rebuild pages]
};

struct Stats {
  uint64_t calls = 0, recorded = 0, replaced = 0, early_out = 0, disabled = 0, unreadable = 0, late = 0, overflow = 0, frames = 0;
  uint64_t items = 0, items_undrawn = 0, items_overflow = 0, clears = 0;
  uint64_t items_blood = 0;   // S106 C3: type 0 items (blood, A12 3)
  uint64_t pictures_read = 0, picture_hits = 0, tex_class = 0, tex_null = 0, tex_fail = 0, tex_size = 0, no_object = 0;   // S105 A12: tex_size = a picture refused for its size alone (counted apart from tex_fail)
  uint64_t alt_rows = 0, rebuild_items = 0, regions = 0, alt_overflow = 0, alt_carried = 0;   // S108 BLD2: rows with the rebuild, its sweat discs, its dirty regions, rows past the caps, an earlier call's rebuild kept
  uint64_t resent = 0, resend_acked = 0, resend_expired = 0, resend_superseded = 0;   // S108 BLD3: rows that re-sent a package; packages the host acknowledged / that expired unacknowledged / that a later clear made old
  uint64_t alt_left = 0;   // S108 BLD3 review: rebuilds whose sweat items outnumber the game's own loop-1 reach (63 per call): the rest keep bit 27 in the game too (counted, not a shortfall)
};

struct MapEntry {
  LiquidMapRow row;
  std::vector<LiquidItemRow> items;
  AltConsume consume;   // S108 BLD2: the rebuild's write-back (empty unless this call decoded the rebuild)
};
struct FrameBuffer {
  uint64_t frame = 0;
  bool valid = false;
  std::vector<MapEntry> maps;
  std::vector<std::shared_ptr<const LiquidPictureRow>> pictures;   // (shared with the hook's cache: one copy of the 32 KB row, S105 A12 F2) distinct source textures of this hook frame (key = the wrapper address)
  void Clear() { frame = 0, valid = false, maps.clear(), pictures.clear(); }
};

enum class Decode : uint8_t { Ok, EarlyOut, Unreadable };

// One source texture, read once and kept (a texture the game builds at character load never changes): key = (wrapper, fetch words 1 and 2).
struct CachedPicture {
  bool ok = false, size = false;   // size: refused by the size cap alone (S105 A12)
  std::shared_ptr<const LiquidPictureRow> row;
};

inline uint32_t BE32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
inline float BEF(const uint8_t* p) {
  const uint32_t w = BE32(p);
  float f;
  std::memcpy(&f, &w, 4);
  return f;
}

class Tracker {
 public:
  Plant plant = Plant::None;
  Tracker() { cur_.maps.reserve(kMaxMaps); }

  // The hook: `frame` = the calling thread's guest frame. `read_word(addr, uint32_t&)` (host value), `read_bytes(addr, void*, n)` (guest bytes as stored), `read_phys(addr, n, uint8_t*)` (pixel memory at a physical address).
  // Returns true when a row was recorded (the glue then consumes the clear request if it skips the node).
  // S108 BLD2: bit 2 = the row carried the game's rebuild; `consume` (optional) then receives its write-back (ApplyAltConsume), which the glue applies only for a recorded row.
  template <class ReadWordFn, class ReadBytesFn, class ReadPhysFn>
  uint32_t OnNode(uint32_t map, uint64_t frame, ReadWordFn read_word, ReadBytesFn read_bytes, ReadPhysFn read_phys, AltConsume* consume = nullptr) {   // 0 = nothing recorded; bit 0 = a row recorded; bit 1 = the row carried the clear request (the glue consumes only then)
    MapEntry e;
    std::vector<std::shared_ptr<const LiquidPictureRow>> pics;   // pictures this call (pointers into the cache) needs (copied into the frame's list under the lock)
    Stats local;
    bool has_package = false;   // S108 BLD3: a package still to re-send: the row is recorded even when the node itself would return at once (no live item, no clear), or the package could not travel
    {
      std::lock_guard<std::mutex> lk(mu_);
      has_package = pending_.count(map) != 0;
    }
    const Decode d = DecodeMap(map, uint32_t(frame), read_word, read_bytes, read_phys, e, pics, local, has_package);
    if (consume) *consume = AltConsume{};
    if (d == Decode::Ok && consume && !e.consume.Empty()) *consume = e.consume;   // (handed out only with a recorded row: every return below that records nothing clears it again)
    const auto none = [&](uint32_t r) { if (consume && !(r & 1u)) *consume = AltConsume{}; return r; };
    std::lock_guard<std::mutex> lk(mu_);
    ++stats_.calls;
    AddLocal(local);
    if (d == Decode::EarlyOut) { ++stats_.early_out; return none(0); }
    if (d != Decode::Ok) { ++stats_.unreadable; return none(0); }
    if (!Advance(frame)) { ++stats_.late; return none(0); }   // a call of an already closed frame
    // The clear generation of this map (review SWF-6): every row carries the latest one, a clear request raises it, so a clear whose row was dropped is still seen by the host as a changed generation.
    uint32_t& gen = clear_gen_[map];
    if (e.row.flags & kLmClear) ++gen;
    e.row.reserved[0] = gen;
    const uint32_t ret = 1u | ((e.row.flags & kLmClear) ? 2u : 0u) | (e.consume.Empty() ? 0u : 4u);
    if (!e.consume.Empty()) ++stats_.alt_rows;
    // S108 BLD3: the rebuild PACKAGE (rows_liquid.h kLmRebuild). Run 18: the hook recorded 4 rebuild rows and the host saw none: the rows of a record the host does not draw (the scene change's 2D / missed /
    // mailbox-dropped frames) are gone, and only the clear survived (its generation rides on every later row). So the package is kept here and re-sent on the map's later rows until the host acknowledges it.
    if (!e.consume.Empty()) {
      Pending& p = pending_[map];
      p.gen = ++rebuild_gen_[map], p.frame = frame, p.rows.clear(), p.pics.clear();
      for (const LiquidItemRow& it : e.items)
        if (it.flags & (kLiRebuild | kLiRegion)) p.rows.push_back(it);
      for (const auto& pic : pics)
        for (const LiquidItemRow& it : p.rows)
          if (it.picture == pic->key) { p.pics.push_back(pic); break; }
      e.row.flags |= kLmRebuild, e.row.reserved[1] = p.gen;
    } else if (auto pit = pending_.find(map); pit != pending_.end()) {
      Pending& p = pit->second;
      if (e.row.flags & kLmClear) {   // a later clear (a level-0 clear, the Swet reset): the game's map was cleared again, the package is old
        ++stats_.resend_superseded;
        pending_.erase(pit);
      } else if (RebuildAck().Acked(map, p.gen)) {
        ++stats_.resend_acked;
        pending_.erase(pit);
      } else if (plant != Plant::NoResend) {   // (a package past kResendFrames was dropped by Advance before this call)
        e.items.insert(e.items.end(), p.rows.begin(), p.rows.end());
        e.row.item_count = uint32_t(e.items.size());
        e.row.flags |= kLmRebuild, e.row.reserved[1] = p.gen;
        pics.insert(pics.end(), p.pics.begin(), p.pics.end());
        ++stats_.resent;
      }
    }
    for (MapEntry& m : cur_.maps) {
      if (m.row.map != e.row.map) continue;
      ++stats_.replaced;
      if (plant != Plant::FirstCall) {
        const bool had_clear = (m.row.flags & kLmClear) != 0;   // a clear seen by an earlier call of the same frame is not lost by the later one (the request is an edge: the glue consumed it)
        // S108 BLD2: so is the rebuild (the earlier call's glue consumed +200: the later call reads a normal map). Its rows are kept after the later call's live items (BLD3: unless the later call re-sent the package itself).
        const bool had_alt = (m.row.flags & kLmAlt) != 0;
        const uint32_t had_gen = m.row.reserved[1];
        std::vector<LiquidItemRow> alt;
        const bool carry = had_alt && !(e.row.flags & (kLmAlt | kLmRebuild)) && plant != Plant::AltNotCarried;
        if (carry)
          for (const LiquidItemRow& it : m.items)
            if (it.flags & (kLiRebuild | kLiRegion)) alt.push_back(it);
        m = std::move(e);
        if (had_clear) m.row.flags |= kLmClear;
        if (had_alt && plant != Plant::AltNotCarried) m.row.flags |= kLmAlt;
        if (carry) {
          m.items.insert(m.items.end(), alt.begin(), alt.end());
          m.row.item_count = uint32_t(m.items.size());
          m.row.flags |= kLmRebuild, m.row.reserved[1] = had_gen;
          ++stats_.alt_carried;
        }
        if (plant == Plant::AltNotCarried) {   // (the twin: the later call's row holds no rebuild rows)
          m.items.erase(std::remove_if(m.items.begin(), m.items.end(), [](const LiquidItemRow& it) { return (it.flags & (kLiRebuild | kLiRegion)) != 0; }), m.items.end());
          m.row.item_count = uint32_t(m.items.size());
          m.row.flags &= ~uint32_t(kLmRebuild | kLmAlt);
        }
      }
      MergePictures(pics);
      return ret;
    }
    if (cur_.maps.size() >= kMaxMaps) { ++stats_.overflow; return none(ret & 2u); }   // (a dropped row still consumed its clear generation: the glue consumes the request; the rebuild is NOT consumed: no row holds it, the next frame records it again)
    cur_.maps.push_back(std::move(e));
    MergePictures(pics);
    ++stats_.recorded;
    return ret;
  }

  void CloseBefore(uint64_t frame) {
    std::lock_guard<std::mutex> lk(mu_);
    if (cur_.valid && cur_.frame < frame) Advance(frame);
  }
  bool TakeNext(FrameBuffer& out) {   // the oldest closed frame not yet taken; false when none (swaps, no copy)
    std::lock_guard<std::mutex> lk(mu_);
    if (taken_ >= closed_count_) return false;
    FrameBuffer& b = ring_[taken_ % kRing];
    std::swap(out, b);
    b.Clear();
    ++taken_;
    return true;
  }
  Stats GetStats() const {
    std::lock_guard<std::mutex> lk(mu_);
    return stats_;
  }

 private:
  template <class ReadWordFn, class ReadBytesFn, class ReadPhysFn>
  Decode DecodeMap(uint32_t map, uint32_t frame, ReadWordFn read_word, ReadBytesFn read_bytes, ReadPhysFn read_phys, MapEntry& e, std::vector<std::shared_ptr<const LiquidPictureRow>>& pics, Stats& st, bool has_package = false) {
    e = MapEntry{};
    uint32_t enable = 0, head = 0, clear = 0, alt = 0, live = 0, w = 0, h = 0, levels = 0, ch = 0;
    if (!read_word(map + kMapEnable, enable) || !read_word(map + kMapLiveHead, head) || !read_word(map + kMapClear, clear)) return Decode::Unreadable;
    // The node's own early outs [C q asm 0x8279ACE8..0x8279AD08]: +2572 == 0, or (+92 == 0 and +136 == 0).
    if (plant != Plant::NoEnableCheck) {
      if (enable == 0) { ++st.disabled; return Decode::EarlyOut; }
      if (head == 0 && clear == 0 && !has_package) return Decode::EarlyOut;   // (S108 BLD3: a map with a package still to re-send is recorded anyway: its row carries the package)
    }
    if (!read_word(map + kMapAlt, alt) || !read_word(map + kMapLiveCount, live) || !read_word(map + kMapWidth, w) || !read_word(map + kMapHeight, h) || !read_word(map + kMapLevels, levels) || !read_word(map + kMapChar, ch))
      return Decode::Unreadable;
    LiquidMapRow& r = e.row;
    r.map = map, r.frame = plant == Plant::StaleFrame ? frame - 1 : frame;
    r.flags = (enable ? kLmEnabled : 0) | (clear ? kLmClear : 0) | (alt ? kLmAlt : 0);
    r.width = uint16_t(std::min<uint32_t>(w, 65535)), r.height = uint16_t(std::min<uint32_t>(h, 65535)), r.levels = levels, r.live_count = live;
    if (clear) ++st.clears;
    // The resources exist [C q asm sub_82726170]: for i in {0, 1}: words +152+4i, +160+4i, +168+4i all non-zero, for at least one i.
    for (uint32_t i = 0; i < 2; ++i) {
      uint32_t a = 0, b = 0, c = 0;
      if (read_word(map + kMapResources + 4 * i, a) && read_word(map + kMapResources + 8 + 4 * i, b) && read_word(map + kMapResources + 16 + 4 * i, c) && a && b && c) { r.flags |= kLmResources; break; }
    }
    // The character: skeleton object (the record's engine object of the body), dim predicate (sub_8259F0E0).
    if (ch) {
      uint32_t skel = 0, d1 = 0, d2 = 0;
      if (read_word(ch + kCharSkeleton, skel) && skel) r.object = skel;
      else ++st.no_object;
      if ((read_word(ch + kCharDimA, d1) && d1) || (read_word(ch + kCharDimB, d2) && d2)) r.flags |= kLmDim;
    } else {
      ++st.no_object;
    }
    // The live items, in list order (the game draws them in this order).
    uint32_t item = head;
    for (uint32_t n = 0; item && n < kMaxItems + 2; ++n) {
      if (e.items.size() >= kMaxItems) { ++st.items_overflow; break; }
      uint8_t raw[kItemBytes];
      if (!read_bytes(item, raw, kItemBytes)) return Decode::Unreadable;
      LiquidItemRow it = ItemRowOf(raw, read_word, read_phys, pics, st);
      ++st.items;
      if (it.type == 0) ++st.items_blood;   // S106 C3
      if (it.state & kStateBitsNotDrawn) ++st.items_undrawn;
      e.items.push_back(it);
      item = BE32(raw + kItNext);
    }
    // S108 BLD2: the game's REBUILD (+200, set with +136 by sub_82727F30): the node draws sub_8279A328 instead of the normal paste, and only when the resources exist (sub_8279ACD8 calls sub_8279AC78 only then).
    if (alt && (r.flags & kLmResources) && !DecodeRebuild(map, read_word, read_bytes, read_phys, e, pics, st)) return Decode::Unreadable;
    r.item_count = uint32_t(e.items.size());
    return Decode::Ok;
  }

  // One 160-byte item as a row (the live list's and the rebuild's).
  template <class ReadWordFn, class ReadPhysFn>
  LiquidItemRow ItemRowOf(const uint8_t* raw, ReadWordFn read_word, ReadPhysFn read_phys, std::vector<std::shared_ptr<const LiquidPictureRow>>& pics, Stats& st) {
    LiquidItemRow it;
    it.type = BE32(raw + kItType), it.state = BE32(raw + kItState), it.pass = BE32(raw + kItPass);
    for (int k = 0; k < 4; ++k) it.local_area[k] = BEF(raw + kItLocal + 4 * k), it.world_param[k] = BEF(raw + kItWorld + 4 * k), it.rect[k] = BEF(raw + kItRect + 4 * k);
    const uint32_t tex = BE32(raw + kItTex);
    if (tex) it.picture = ReadPicture(tex, read_word, read_phys, pics, st);
    else ++st.tex_null;
    if (it.picture) it.flags |= kLiHasSource;
    return it;
  }

  // S108 BLD2 (BLD_plan.md section 10): what sub_8279A328 draws, read at the node's entry (sub_82727F30 has marked the items and the entries; nothing has drawn them yet).
  //   loop 1 (0x8279A590..0x8279A620): the SWEAT sub-pool [+72, +72 + [+64]) of the pool [+84], stride 160: each item with bit 27 has bit 27 cleared (consume) and, with bit 30, is drawn as its own disc (kLiRebuild).
  //   loop 2 (0x8279A6E0..0x8279A8AC): per list k, per entry j of [map+76+4k] whose +12 is set: +12 cleared (consume), the region j quad with the entry's +8 texture (kLiRegion).
  // S108 BLD3 review: the game PAGES the rebuild [C q asm]: loop 2 stops at its 64th draw of a call (r20, 0x8279A6F4 / 0x8279A7D0) and then returns 0 (0x8279A8B0..0x8279A8B8, r16), so sub_8279AC78 leaves +200 set
  // (0x8279ACB4..0x8279ACC0) and the next frame's node runs sub_8279A328 again (no clear: +136 was consumed); loop 1 ends at the 64th bit-27 item OF A CALL (r25, 0x8279A5D4..0x8279A5DC, that item keeps bit 27) and starts
  // over each call, skipping the items already drawn (their bit 27 is cleared). So over the calls of one rebuild the game draws EVERY dirty region (R of them: floor(R / 64) + 1 calls) and the first 63 x (floor(R / 64) + 1)
  // bit-27 sweat items. That end state is recorded here in one row (the paging is a 360 HABIT, a cost spread); nothing is capped by us: kLiquidRebuildMax / kLiquidRegionMax are torn-read guards on the guest's counts.
  template <class ReadWordFn, class ReadBytesFn, class ReadPhysFn>
  bool DecodeRebuild(uint32_t map, ReadWordFn read_word, ReadBytesFn read_bytes, ReadPhysFn read_phys, MapEntry& e, std::vector<std::shared_ptr<const LiquidPictureRow>>& pics, Stats& st) {
    AltConsume c;
    c.map = map;
    // loop 2 first (its count R decides how many calls the game makes, so how far loop 1 reaches); its rows go after loop 1's (the game's order).
    std::vector<LiquidItemRow> region_rows;
    std::vector<uint32_t> region_entries;
    for (uint32_t k = 0; k < 2; ++k) {
      uint32_t list = 0, ents = 0, n = 0, recs = 0;
      if (!read_word(map + kMapRegionLists + 4 * k, list) || !read_word(map + kMapEntries + 4 * k, ents)) return false;
      if (!list || !ents) continue;   // (0x8279A6F0 / the entries of a list that has none)
      if (!read_word(list + kListCount, n) || !read_word(list + kListRecords, recs)) return false;
      if (n > kMaxRegionWalk) ++st.alt_overflow;   // a torn count: the walk stops at the guard (counted; the census and the host's shortfall line)
      for (uint32_t j = 0; j < std::min<uint32_t>(n, kMaxRegionWalk); ++j) {
        uint8_t ent[kEntryBytes];
        if (!read_bytes(ents + kEntryBytes * j, ent, kEntryBytes)) return false;
        if (BE32(ent + kEntDirty) == 0) continue;
        if (plant == Plant::RegionsCapped && region_rows.size() >= 256) break;   // (the twin: BLD3's bound)
        uint8_t corners[16];
        if (!read_bytes(recs + kRegionBytes * j + kRegionCorners, corners, 16)) return false;
        LiquidItemRow it;
        it.flags = kLiRegion, it.type = k, it.state = BE32(ent + kEntFlag);
        for (int q = 0; q < 4; ++q) it.world_param[q] = BEF(corners + 4 * q);
        const uint32_t tex = BE32(ent + kEntTex);
        if (tex) it.picture = ReadPicture(tex, read_word, read_phys, pics, st);
        else ++st.tex_null;
        if (it.picture) it.flags |= kLiHasSource;
        region_rows.push_back(it);
        region_entries.push_back(ents + kEntryBytes * j);
        ++st.regions;
      }
    }
    const uint32_t calls = uint32_t(region_rows.size()) / kAltPage + 1;   // the game's calls of sub_8279A328 for this rebuild
    const uint32_t loop1_max = plant == Plant::Loop1OneCall ? kLoop1PerCall : kLoop1PerCall * calls;
    uint32_t pool = 0, start = 0, count = 0;
    if (!read_word(map + kMapPool, pool) || !read_word(map + kMapSweatStart, start) || !read_word(map + kMapSweatCount, count)) return false;
    if (pool && int32_t(start) >= 0 && int32_t(count) > 0) {
      if (count > kMaxPoolWalk) ++st.alt_overflow;   // a torn count (as above)
      uint32_t walked = 0;
      for (uint32_t i = 0; i < std::min<uint32_t>(count, kMaxPoolWalk); ++i) {
        const uint32_t at = pool + kItemBytes * (start + i);
        uint8_t raw[kItemBytes];
        if (!read_bytes(at, raw, kItemBytes)) return false;
        const uint32_t s = BE32(raw + kItState);
        if (!(s & kItBit27)) continue;
        if (++walked > loop1_max) { ++st.alt_left; break; }   // the game's own end: its last call stops here too; the rest keep bit 27 (the game's, not a shortfall)
        c.items.push_back(at);
        if (!(s & kItBit30)) continue;
        LiquidItemRow it = ItemRowOf(raw, read_word, read_phys, pics, st);
        it.flags |= kLiRebuild;
        e.items.push_back(it);
        ++st.rebuild_items;
      }
    }
    e.items.insert(e.items.end(), region_rows.begin(), region_rows.end());
    c.entries = std::move(region_entries);
    e.row.flags |= kLmAlt;
    e.consume = std::move(c);
    return true;
  }

  // wrapper -> (vtable check) -> header = first non-null of +80 / +88 / +84 -> six fetch dwords -> BC block bytes. Returns the picture key (the wrapper address) or 0.
  template <class ReadWordFn, class ReadPhysFn>
  uint32_t ReadPicture(uint32_t wrapper, ReadWordFn read_word, ReadPhysFn read_phys, std::vector<std::shared_ptr<const LiquidPictureRow>>& pics, Stats& st) {
    uint32_t vt = 0, header = 0;
    if (!read_word(wrapper, vt)) return ++st.tex_fail, 0;
    if (plant != Plant::NoVtableCheck && vt != kWrapperVtableA && vt != kWrapperVtableB) return ++st.tex_class, 0;
    for (uint32_t off : {kWrapperHeader0, kWrapperHeader1, kWrapperHeader2}) {
      uint32_t v = 0;
      if (read_word(wrapper + off, v) && v) { header = v; break; }
    }
    if (!header) return ++st.tex_null, 0;
    uint32_t f[6];
    for (uint32_t i = 0; i < 6; ++i)
      if (!read_word(header + particles::kHeaderFetch + 4 * i, f[i])) return ++st.tex_fail, 0;
    particles::FetchToGpuForm(f);
    const uint64_t ck = (uint64_t(wrapper) << 32) ^ (uint64_t(f[1]) * 0x9E3779B1u) ^ f[2];
    {
      std::lock_guard<std::mutex> lk(cache_mu_);
      const auto it = cache_.find(ck);
      if (it != cache_.end()) {
        ++st.picture_hits;
        if (!it->second.ok) return ++(it->second.size ? st.tex_size : st.tex_fail), 0;
        pics.push_back(it->second.row);
        return wrapper;
      }
    }
    CachedPicture cp;
    guest_tex::Shape s;
    std::vector<uint8_t> blocks;
    // S105 A12: the cap is the liquid row's (256x256 DXT1: the blood decals are 4-32 KB); it was the crowd's 4096 bytes, which refused every blood texture but the chest ones.
    const uint32_t cap = plant == Plant::OldPictureCap ? kCrowdPictureBytes : kLiquidPictureBytes;
    if (guest_tex::ShapeOfFetch(f, s) == guest_tex::Why::Ok) {
      const guest_tex::Why rw = guest_tex::ReadMip0(s, read_phys, blocks, cap);
      if (rw == guest_tex::Why::Ok && blocks.size() <= cap) {
        auto row = std::make_shared<LiquidPictureRow>();
        row->key = wrapper, row->width = uint16_t(s.width), row->height = uint16_t(s.height), row->format = s.game_format, row->nbytes = uint32_t(blocks.size());
        std::memcpy(row->bytes, blocks.data(), blocks.size());
        LiquidPictureSeal(*row);   // the hash, once (review F3)
        cp.ok = LiquidPictureValid(*row);
        cp.row = std::move(row);
      } else {
        cp.size = rw == guest_tex::Why::Size;
      }
    }
    ++st.pictures_read;
    {
      std::lock_guard<std::mutex> lk(cache_mu_);
      if (cache_.size() >= kCacheMax) cache_.clear();   // bounded: a match has a handful of textures; a flush only costs a re-read
      cache_[ck] = cp;
    }
    if (!cp.ok) return ++(cp.size ? st.tex_size : st.tex_fail), 0;
    pics.push_back(cp.row);
    return wrapper;
  }

  void MergePictures(const std::vector<std::shared_ptr<const LiquidPictureRow>>& pics) {   // mu_ held
    for (const auto& p : pics) {
      bool have = false;
      for (const auto& q : cur_.pictures) have = have || q->key == p->key;
      if (!have && (plant != Plant::PicturesCapped || cur_.pictures.size() < 256)) cur_.pictures.push_back(p);   // (S108 BLD3 review: no count cap: every picture a row of the frame names travels with it)
    }
  }
  void AddLocal(const Stats& l) {   // mu_ held
    stats_.items_blood += l.items_blood;   // S106 C3
    stats_.rebuild_items += l.rebuild_items, stats_.regions += l.regions, stats_.alt_overflow += l.alt_overflow, stats_.alt_left += l.alt_left;   // S108 BLD2 (alt_rows / alt_carried are counted under the lock in OnNode)
    stats_.items += l.items, stats_.items_undrawn += l.items_undrawn, stats_.items_overflow += l.items_overflow, stats_.clears += l.clears, stats_.disabled += l.disabled;
    stats_.pictures_read += l.pictures_read, stats_.picture_hits += l.picture_hits, stats_.tex_class += l.tex_class, stats_.tex_null += l.tex_null, stats_.tex_fail += l.tex_fail, stats_.tex_size += l.tex_size, stats_.no_object += l.no_object;
  }
  bool Advance(uint64_t frame) {   // false for a call of an already closed frame; mu_ held
    if (!cur_.valid) {
      cur_.valid = true, cur_.frame = frame;
      return true;
    }
    if (frame < cur_.frame) return false;
    if (frame > cur_.frame) {
      if (closed_count_ - taken_ >= kRing) {   // the oldest closed frame is overwritten before Emit took it
        ++taken_;
        ++stats_.late;
      }
      std::swap(ring_[closed_count_ % kRing], cur_);
      ++closed_count_;
      cur_.Clear();
      cur_.valid = true, cur_.frame = frame;
      ++stats_.frames;
      if (plant != Plant::NoPrune)   // S108 BLD3 review: every package past its time, whether or not its map is still called (a freed map never is)
        for (auto it = pending_.begin(); it != pending_.end();) {
          if (frame > it->second.frame + kResendFrames) ++stats_.resend_expired, it = pending_.erase(it);
          else ++it;
        }
    }
    return true;
  }

  mutable std::mutex mu_;
  FrameBuffer cur_, ring_[kRing];
  uint64_t closed_count_ = 0, taken_ = 0;
  Stats stats_;
  std::unordered_map<uint32_t, uint32_t> clear_gen_;   // map -> clear generation (mu_)
  // S108 BLD3: the latest rebuild package of each map, re-sent until the host acknowledges it (mu_). Only the map ADDRESS is kept as a key: nothing is read from it later.
  // S108 BLD3 review: a NEW map at a freed map's address never receives the old package: the map's setup sub_82799A70 writes +136 = 1 (0x82799AC0, [C q decomp]) and +2572 = 1, so the new map's first recorded row
  // carries kLmClear, which drops the package (OnNode) before any re-send; the generations of an address only grow (rebuild_gen_ is never erased), so an old acknowledgement never matches a new package.
  struct Pending {
    uint32_t gen = 0;
    uint64_t frame = 0;
    std::vector<LiquidItemRow> rows;
    std::vector<std::shared_ptr<const LiquidPictureRow>> pics;
  };
  std::unordered_map<uint32_t, Pending> pending_;
  std::unordered_map<uint32_t, uint32_t> rebuild_gen_;   // map -> rebuild generation (mu_)
  std::mutex cache_mu_;
  std::unordered_map<uint64_t, CachedPicture> cache_;
};

}  // namespace rdr_scene::live::liquid
