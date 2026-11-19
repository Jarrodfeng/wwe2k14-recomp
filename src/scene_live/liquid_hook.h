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
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../renderer_scene/scene_record.h"
#include "guest_texture.h"

namespace rdr_scene::live::liquid {

// Fields of the map object [C SWB 1a]
inline constexpr uint32_t kMapEnable = 2572, kMapLiveCount = 88, kMapLiveHead = 92, kMapClear = 136, kMapAlt = 200, kMapWidth = 124, kMapHeight = 128, kMapLevels = 132, kMapChar = 48;
inline constexpr uint32_t kMapResources = 152;   // words +152, +156 (A), +160, +164 (B), +168, +172 (C): sub_82726170
inline constexpr uint32_t kCharSkeleton = 48, kCharDimA = 5076, kCharDimB = 5072;
// Fields of an item [C SWB 1d]
inline constexpr uint32_t kItemBytes = 160, kItNext = 8, kItType = 20, kItState = 60, kItTex = 64, kItLocal = 80, kItWorld = 112, kItRect = 128, kItPass = 144;
inline constexpr uint32_t kStateBitsNotDrawn = kLiStateNotDrawn;   // bit30, bit29, bit26 (sub_82725E28 / E38 / E58): paused + not drawn / not drawn [C SWB 1d]
inline constexpr uint32_t kMaxItems = 16;          // the pool's cap of live items [C sub_82726DF0 0x82726E68]
inline constexpr uint32_t kWrapperVtableA = 0x8206ADB8u, kWrapperVtableB = 0x82210B40u;   // the texture wrapper's vtables (texture_registry.h)
inline constexpr uint32_t kWrapperHeader0 = 80, kWrapperHeader1 = 88, kWrapperHeader2 = 84;   // sub_82BD4288 returns the first non-null of these fields
inline constexpr size_t kMaxMaps = 16;             // maps a hook frame keeps (a match has 2 wrestlers + 7 other slots)
inline constexpr size_t kRing = 8;                 // closed frames kept for Emit
inline constexpr size_t kMaxPictures = 64, kCacheMax = 128;

enum class Plant : uint32_t {
  None,
  FirstCall,        // a second call of the same map in a frame does not replace the first                       -> unit [last call wins]
  NoVtableCheck,    // the source texture object is not checked for the wrapper class                           -> unit [texture class]
  StaleFrame,       // the row is stamped with the previous hook frame                                           -> unit [frame stamp]
  NoEnableCheck,    // the enable word and the empty-list early out are ignored                                  -> unit [early outs]
};

struct Stats {
  uint64_t calls = 0, recorded = 0, replaced = 0, early_out = 0, disabled = 0, unreadable = 0, late = 0, overflow = 0, frames = 0;
  uint64_t items = 0, items_undrawn = 0, items_overflow = 0, clears = 0;
  uint64_t pictures_read = 0, picture_hits = 0, tex_class = 0, tex_null = 0, tex_fail = 0, no_object = 0;
};

struct MapEntry {
  LiquidMapRow row;
  std::vector<LiquidItemRow> items;
};
struct FrameBuffer {
  uint64_t frame = 0;
  bool valid = false;
  std::vector<MapEntry> maps;
  std::vector<CrowdPictureRow> pictures;   // distinct source textures of this hook frame (key = the wrapper address)
  void Clear() { frame = 0, valid = false, maps.clear(), pictures.clear(); }
};

enum class Decode : uint8_t { Ok, EarlyOut, Unreadable };

// One source texture, read once and kept (a texture the game builds at character load never changes): key = (wrapper, fetch words 1 and 2).
struct CachedPicture {
  bool ok = false;
  CrowdPictureRow row;
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
  template <class ReadWordFn, class ReadBytesFn, class ReadPhysFn>
  uint32_t OnNode(uint32_t map, uint64_t frame, ReadWordFn read_word, ReadBytesFn read_bytes, ReadPhysFn read_phys) {   // 0 = nothing recorded; bit 0 = a row recorded; bit 1 = the row carried the clear request (the glue consumes only then)
    MapEntry e;
    std::vector<CrowdPictureRow> pics;   // pictures this call needs (copied into the frame's list under the lock)
    Stats local;
    const Decode d = DecodeMap(map, uint32_t(frame), read_word, read_bytes, read_phys, e, pics, local);
    std::lock_guard<std::mutex> lk(mu_);
    ++stats_.calls;
    AddLocal(local);
    if (d == Decode::EarlyOut) { ++stats_.early_out; return 0; }
    if (d != Decode::Ok) { ++stats_.unreadable; return 0; }
    if (!Advance(frame)) { ++stats_.late; return 0; }   // a call of an already closed frame
    // The clear generation of this map (review SWF-6): every row carries the latest one, a clear request raises it, so a clear whose row was dropped is still seen by the host as a changed generation.
    uint32_t& gen = clear_gen_[map];
    if (e.row.flags & kLmClear) ++gen;
    e.row.reserved[0] = gen;
    const uint32_t ret = 1u | ((e.row.flags & kLmClear) ? 2u : 0u);
    for (MapEntry& m : cur_.maps) {
      if (m.row.map != e.row.map) continue;
      ++stats_.replaced;
      if (plant != Plant::FirstCall) {
        const bool had_clear = (m.row.flags & kLmClear) != 0;   // a clear seen by an earlier call of the same frame is not lost by the later one (the request is an edge: the glue consumed it)
        m = std::move(e);
        if (had_clear) m.row.flags |= kLmClear;
      }
      MergePictures(pics);
      return ret;
    }
    if (cur_.maps.size() >= kMaxMaps) { ++stats_.overflow; return ret & 2u; }   // (a dropped row still consumed its clear generation: the glue consumes the request)
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
  Decode DecodeMap(uint32_t map, uint32_t frame, ReadWordFn read_word, ReadBytesFn read_bytes, ReadPhysFn read_phys, MapEntry& e, std::vector<CrowdPictureRow>& pics, Stats& st) {
    e = MapEntry{};
    uint32_t enable = 0, head = 0, clear = 0, alt = 0, live = 0, w = 0, h = 0, levels = 0, ch = 0;
    if (!read_word(map + kMapEnable, enable) || !read_word(map + kMapLiveHead, head) || !read_word(map + kMapClear, clear)) return Decode::Unreadable;
    // The node's own early outs [C q asm 0x8279ACE8..0x8279AD08]: +2572 == 0, or (+92 == 0 and +136 == 0).
    if (plant != Plant::NoEnableCheck) {
      if (enable == 0) { ++st.disabled; return Decode::EarlyOut; }
      if (head == 0 && clear == 0) return Decode::EarlyOut;
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
      LiquidItemRow it;
      it.type = BE32(raw + kItType), it.state = BE32(raw + kItState), it.pass = BE32(raw + kItPass);
      for (int k = 0; k < 4; ++k) it.local_area[k] = BEF(raw + kItLocal + 4 * k), it.world_param[k] = BEF(raw + kItWorld + 4 * k), it.rect[k] = BEF(raw + kItRect + 4 * k);
      ++st.items;
      if (it.state & kStateBitsNotDrawn) ++st.items_undrawn;
      const uint32_t tex = BE32(raw + kItTex);
      if (tex) it.picture = ReadPicture(tex, read_word, read_phys, pics, st);
      else ++st.tex_null;
      if (it.picture) it.flags |= kLiHasSource;
      e.items.push_back(it);
      item = BE32(raw + kItNext);
    }
    r.item_count = uint32_t(e.items.size());
    return Decode::Ok;
  }

  // wrapper -> (vtable check) -> header = first non-null of +80 / +88 / +84 -> six fetch dwords -> BC block bytes. Returns the picture key (the wrapper address) or 0.
  template <class ReadWordFn, class ReadPhysFn>
  uint32_t ReadPicture(uint32_t wrapper, ReadWordFn read_word, ReadPhysFn read_phys, std::vector<CrowdPictureRow>& pics, Stats& st) {
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
        if (!it->second.ok) return ++st.tex_fail, 0;
        pics.push_back(it->second.row);
        return wrapper;
      }
    }
    CachedPicture cp;
    guest_tex::Shape s;
    std::vector<uint8_t> blocks;
    if (guest_tex::ShapeOfFetch(f, s) == guest_tex::Why::Ok && guest_tex::ReadMip0(s, read_phys, blocks, kCrowdPictureBytes) == guest_tex::Why::Ok && blocks.size() <= kCrowdPictureBytes) {
      cp.row.key = wrapper, cp.row.width = uint16_t(s.width), cp.row.height = uint16_t(s.height), cp.row.format = s.game_format, cp.row.nbytes = uint32_t(blocks.size());
      std::memcpy(cp.row.bytes, blocks.data(), blocks.size());
      cp.ok = CrowdPictureValid(cp.row);
    }
    ++st.pictures_read;
    {
      std::lock_guard<std::mutex> lk(cache_mu_);
      if (cache_.size() >= kCacheMax) cache_.clear();   // bounded: a match has a handful of textures; a flush only costs a re-read
      cache_[ck] = cp;
    }
    if (!cp.ok) return ++st.tex_fail, 0;
    pics.push_back(cp.row);
    return wrapper;
  }

  void MergePictures(const std::vector<CrowdPictureRow>& pics) {   // mu_ held
    for (const CrowdPictureRow& p : pics) {
      bool have = false;
      for (const CrowdPictureRow& q : cur_.pictures) have = have || q.key == p.key;
      if (!have && cur_.pictures.size() < kMaxPictures) cur_.pictures.push_back(p);
    }
  }
  void AddLocal(const Stats& l) {   // mu_ held
    stats_.items += l.items, stats_.items_undrawn += l.items_undrawn, stats_.items_overflow += l.items_overflow, stats_.clears += l.clears, stats_.disabled += l.disabled;
    stats_.pictures_read += l.pictures_read, stats_.picture_hits += l.picture_hits, stats_.tex_class += l.tex_class, stats_.tex_null += l.tex_null, stats_.tex_fail += l.tex_fail, stats_.no_object += l.no_object;
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
    }
    return true;
  }

  mutable std::mutex mu_;
  FrameBuffer cur_, ring_[kRing];
  uint64_t closed_count_ = 0, taken_ = 0;
  Stats stats_;
  std::unordered_map<uint32_t, uint32_t> clear_gen_;   // map -> clear generation (mu_)
  std::mutex cache_mu_;
  std::unordered_map<uint64_t, CachedPicture> cache_;
};

}  // namespace rdr_scene::live::liquid
