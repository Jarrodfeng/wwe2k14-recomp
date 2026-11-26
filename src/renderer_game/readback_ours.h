// S103 lane RBC / RBC fix: the stream-free build's GPU-to-CPU read-backs (docs\research\s103\RBK.md; sites R1 .. R9).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// The game renders a picture, resolves it into a texture and reads the texture's bits with the CPU through the library's lock (0x82CA7EB8). The stream build served those
// reads (reader.h OnTextureLock); the stream-free build has no command processor, so no resolve ever runs and the CPU read black or stale memory. This file is the OFF build's lock body:
//   1. a SITE TABLE keyed by the lock's return address (lr = the caller of LockRect 0x82CA8628, which reaches the library lock by a branch);
//   2. the served sites write OUR SCENE (`out` of the newest OURS 3D frame: no 2D rows, no HUD, no display curve; SceneHost::RequestScene / CaptureScene) into the texture's guest memory, where the 360's
//      resolve would have left it (readback_tile.h: format / size / tiling / endian / channel order from the fetch dwords at texture + 28 .. + 48); the game's own CPU code then reads it unchanged.
//      No 3D scene (a menu, a loading screen) = nothing is written and the lock is counted unserved: a menu is never saved into the player's custom arena;
//   3. every known site this build does not serve is COUNTED (`readback_unserved{lr}`); a lock from a return address the table does not name is `lock_other` (uploads and inits lock textures too: it is
//      NOT an unserved read-back, and stays out of UnservedTotal; the census lists them so a reader this table missed shows up by name).
//
// R1 (replay-clip thumbnail, sub_8274FA70, lock return 0x8274FADC, 128x128 8888) is DEFERRED, never blocking: thumb_defer_off.h answers the job's entry with a scene request and runs the job itself at a present
// once the picture has landed (the stream path's thumb_defer.h design); the job's own lock then finds the picture in memory (t_thumb_replay) and this file does nothing.
// A scene is only ever the picture of a RECENT OURS 3D frame (SceneHost::SceneFresh: `out` complete and drawn within 2 guest frames of the request): a menu, a loading screen or a replay the game draws itself has none.
// R2 (Create an Arena screenshot, sub_822A61C8, lock return 0x822A635C, 1280x720 or 640x480 by [0x8392DC3C] == 1): a menu save, blocking up to ~0.5 s for one scene copy.
//   [L] whether the Create an Arena preview is an OURS 3D frame in the stream-free build (COV5 2e: never run stream-free). If it is not (or the last OURS frame is older than 2 guest frames), there is no scene,
//   R2 is counted unserved and nothing is written: the last match's arena is never saved as the player's custom arena.
// R3 (CAW portrait) is the CPB lane's, served at the untile that follows the lock (hook G3): `at_untile`. R4 / R6 (composite attires) are composed on the host (CMB): `composed`, nothing written. R5: unserved, counted.
// R7 (the Create a Title thumbnail, sub_833438B0, lock return 0x8334398C, 256x128 8888; render thread) is DEFERRED by title_thumb_off.h (S111 CACTHUMB); its lock writes the delivered picture or BLACK (title_thumb_core.h).
// R9 (Create a Highlight reel picture, sub_8228F2F8, lock return 0x8228F3FC; docs\agents\s103\RBQ.md) runs on a WORKER job (sub_8228FF80, queued by sub_82C3C078 on the 0x8394012C pool), never the render thread: it is served like R2 (a scene copy, blocking
//   up to 0.5 s) into [this+88] at the screen size [[0x83929A68]+300 / +304]. With no 3D scene it writes BLACK (the picture goes into the player's CAHS save: never garbage). The picture, at the texture's size, is kept for R8.
// R8 (the 128x128 reel thumbnail, sub_822959C0, lock return 0x82295A2C) runs on the RENDER thread (op-68 callback): it NEVER waits. It crops R9's kept picture by the rect the game keeps at obj + 64 .. + 76 (obj = [0x836F2BD0]; left, top, right, bottom;
//   the default rect 0x836F2BE0 .. EC = the whole picture, or its centre 4:3 when [0x8392DC3C] != 0: sub_82295B50 +9C .. +1A0), box-scales it to 128x128 and writes it. No R9 picture / a source of another size / an unusable rect = BLACK, counted `zeros`.
//
// The core (the table, the counters, Prepare / Write / ServeSite) has no game, REX or SDK includes: tools\readback\rbc_unit.cpp builds THIS file with WWE2K14_READBACK_CORE_ONLY.
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "../scene_live/safe_guest_pages.h"   // PhysReadable: the page walk of the physical heap
#include "readback_tile.h"

namespace wwe2k14::readback_ours {

enum class Kind : uint8_t { Thumb, ArenaShot, Unserved, Composed, ServedAtUntile, ReelPicture, ReelThumb, TitleShot };   // ReelPicture = R9 (worker, scene copy), ReelThumb = R8 (render thread, never waits)   // ServedAtUntile: served by another lane at the untile that follows the lock (R3: hook G3 on sub_82CFA318 at lr 0x8341B780)   // TitleShot = R7 (render thread; deferred to a present by title_thumb_off.h)
struct Site {
  uint32_t lr;
  Kind kind;
  const char* id;     // R1 .. R9
  const char* what;
};
// Every lr = the instruction after the `bl 0x82ca8628` at the lock site, confirmed in the listing (q asm).
inline constexpr Site kSites[] = {
    {0x8274FADC, Kind::Thumb, "R1", "sub_8274FA70 replay-clip thumbnail, 128x128"},
    {0x822A635C, Kind::ArenaShot, "R2", "sub_822A61C8 Create an Arena screenshot, 1280x720"},
    {0x8341B75C, Kind::ServedAtUntile, "R3", "sub_8341B6A8 Create a Superstar portrait, 736x1280 (served at the untile by the CAW portrait lane, hook G3)"},
    {0x83418024, Kind::Composed, "R4a", "sub_83417E20 attire atlas lock (composed on the host)"},
    {0x83418088, Kind::Composed, "R4b", "sub_83417E20 attire atlas lock (composed on the host)"},
    {0x833A1B58, Kind::Composed, "R6a", "sub_833A1A40 Superstar Threads (composed on the host)"},
    {0x833A1BB0, Kind::Composed, "R6b", "sub_833A1A40 Superstar Threads (composed on the host)"},
    {0x833A1BC8, Kind::Composed, "R6c", "sub_833A1A40 Superstar Threads (composed on the host)"},
    {0x83327B20, Kind::Unserved, "R5a", "sub_83327A08 Create a Title composite"},
    {0x83327B78, Kind::Unserved, "R5b", "sub_83327A08 Create a Title composite"},
    {0x83327B90, Kind::Unserved, "R5c", "sub_83327A08 Create a Title composite"},
    {0x8334398C, Kind::TitleShot, "R7", "sub_833438B0 Create a Title thumbnail, 256x128 (render thread: deferred to a present, title_thumb_off.h)"},
    {0x82295A2C, Kind::ReelThumb, "R8", "sub_822959C0 Create a Highlight reel thumbnail, 128x128 (render thread: crops R9's picture, never waits)"},
    {0x8228F3FC, Kind::ReelPicture, "R9", "sub_8228F2F8 Create a Highlight reel picture (worker job: a scene copy)"},
};
inline constexpr uint32_t kSiteCount = uint32_t(sizeof(kSites) / sizeof(kSites[0]));

inline int SiteIndex(uint32_t lr) {
  for (uint32_t i = 0; i < kSiteCount; ++i)
    if (kSites[i].lr == lr) return int(i);
  return -1;
}
inline const Site* FindSite(uint32_t lr) {
  const int i = SiteIndex(lr);
  return i < 0 ? nullptr : &kSites[i];
}

enum class Action : uint8_t { Serve, Unserved, Composed, AtUntile, Other };

inline constexpr uint32_t kWhyCount = 14;   // readback_tile::Why values
struct Counters {
  std::atomic<uint64_t> locks[kSiteCount] = {};      // every lock seen from a table site
  std::atomic<uint64_t> served[kSiteCount] = {};     // the picture was written
  std::atomic<uint64_t> zeros[kSiteCount] = {};      // R8 / R9: black was written instead of a picture (also counted in unserved{}, with its reason); refused = unserved - zeros (the guard refused: nothing written)
  std::atomic<uint64_t> unserved[kSiteCount] = {};   // readback_unserved{lr}: a known site whose read-back was not served (a site this build never serves, or a serve that refused / had no scene)
  std::atomic<uint64_t> why[kSiteCount][kWhyCount] = {};   // by refusal reason (readback_tile::Why)
  std::atomic<uint64_t> composed{0}, at_untile{0};   // at_untile: locks of a site another hook serves at the untile (R3); apart from unserved
  static constexpr uint32_t kOtherSlots = 128;
  std::atomic<uint32_t> other_lr[kOtherSlots] = {};
  std::atomic<uint64_t> other_n[kOtherSlots] = {};
  std::atomic<uint64_t> lock_other{0};               // every lock from a return address the table does not name (uploads, inits)
  std::atomic<uint64_t> thumb_deferred{0}, thumb_delivered{0}, thumb_no_scene{0}, thumb_before_write{0}, thumb_dropped{0}, thumb_timed_out{0}, thumb_refused{0};   // R1 (thumb_defer_off.h)
  std::atomic<uint64_t> title_deferred{0}, title_delivered{0}, title_no_scene{0}, title_timed_out{0}, title_dropped{0}, title_refused{0};   // R7 (title_thumb_off.h, S111 CACTHUMB)
};
inline Counters g_counters;
inline thread_local bool t_thumb_replay = false;   // thumb_defer_off.h is running the thumbnail job itself on a delivered picture: its lock reads memory, nothing is served

// The count of an other lr: its new count (0 = the table is full: counted in lock_other only).
inline uint64_t NoteOther(uint32_t lr) {
  Counters& c = g_counters;
  c.lock_other.fetch_add(1, std::memory_order_relaxed);
  for (uint32_t i = 0; i < Counters::kOtherSlots; ++i) {
    uint32_t cur = c.other_lr[i].load(std::memory_order_acquire);
    if (cur == 0) {
      uint32_t expected = 0;
      if (c.other_lr[i].compare_exchange_strong(expected, lr, std::memory_order_acq_rel)) cur = lr;
      else cur = expected;
    }
    if (cur == lr) return c.other_n[i].fetch_add(1, std::memory_order_relaxed) + 1;
  }
  return 0;
}

// The decision for a lock from `lr`; counts it. `n` = how many times this lr has been seen (for the log's rate limit).
inline Action Classify(uint32_t lr, uint64_t* n = nullptr) {
  Counters& c = g_counters;
  const int i = SiteIndex(lr);
  if (i < 0) {
    const uint64_t k = NoteOther(lr);
    if (n) *n = k;
    return Action::Other;
  }
  const uint64_t k = c.locks[i].fetch_add(1, std::memory_order_relaxed) + 1;
  if (n) *n = k;
  switch (kSites[i].kind) {
    case Kind::Thumb:
    case Kind::ArenaShot:
    case Kind::ReelPicture:
    case Kind::TitleShot:
    case Kind::ReelThumb: return Action::Serve;
    case Kind::Composed: c.composed.fetch_add(1, std::memory_order_relaxed); return Action::Composed;
    case Kind::ServedAtUntile: c.at_untile.fetch_add(1, std::memory_order_relaxed); return Action::AtUntile;
    default: c.unserved[i].fetch_add(1, std::memory_order_relaxed); return Action::Unserved;
  }
}
inline void NoteUnserved(int i, readback_tile::Why why) {
  g_counters.unserved[i].fetch_add(1, std::memory_order_relaxed);
  g_counters.why[i][uint32_t(why) < kWhyCount ? uint32_t(why) : 0].fetch_add(1, std::memory_order_relaxed);
}
inline void NoteServed(int i) { g_counters.served[i].fetch_add(1, std::memory_order_relaxed); }
inline void NoteZeros(int i, readback_tile::Why why) {   // black was written (the picture the site wanted was not available): an unserved read-back, but the save is clean
  g_counters.zeros[i].fetch_add(1, std::memory_order_relaxed);
  NoteUnserved(i, why);
}
// Known read-back sites not served (readback_unserved{lr}); `lock_other` is NOT in it.
inline uint64_t UnservedTotal() {
  uint64_t t = 0;
  for (uint32_t i = 0; i < kSiteCount; ++i) t += g_counters.unserved[i].load(std::memory_order_relaxed);
  return t;
}
inline uint64_t ServedTotal() {
  uint64_t t = 0;
  for (uint32_t i = 0; i < kSiteCount; ++i) t += g_counters.served[i].load(std::memory_order_relaxed);
  return t;
}
// The full census: the served sites, readback_unserved{lr} per known site (with its refusal reasons), the R1 deferral counters, lock_other with its busiest return addresses.
inline std::string CensusLine() {
  std::string s = "[rdr][readback] served";
  char b[200];
  for (uint32_t i = 0; i < kSiteCount; ++i) {
    const uint64_t v = g_counters.served[i].load(std::memory_order_relaxed);
    if (v) {
      std::snprintf(b, sizeof b, " %s=%llu", kSites[i].id, static_cast<unsigned long long>(v));
      s += b;
    }
  }
  s += "; unserved:";
  bool any = false;
  for (uint32_t i = 0; i < kSiteCount; ++i) {
    const uint64_t v = g_counters.unserved[i].load(std::memory_order_relaxed);
    if (!v) continue;
    std::snprintf(b, sizeof b, " readback_unserved{%08X %s}=%llu", kSites[i].lr, kSites[i].id, static_cast<unsigned long long>(v));
    s += b, any = true;
    std::string why;
    for (uint32_t k = 1; k < kWhyCount; ++k) {
      const uint64_t n = g_counters.why[i][k].load(std::memory_order_relaxed);
      if (n) {
        std::snprintf(b, sizeof b, "%s%s %llu", why.empty() ? "" : ", ", readback_tile::WhyText(readback_tile::Why(k)), static_cast<unsigned long long>(n));
        why += b;
      }
    }
    if (!why.empty()) s += " (" + why + ")";
  }
  if (!any) s += " none";
  s += "; reel:";   // R8 / R9 (Create a Highlight reel): what was written, black written, nothing written (the guard refused)
  for (uint32_t i = 0; i < kSiteCount; ++i) {
    if (kSites[i].kind != Kind::ReelPicture && kSites[i].kind != Kind::ReelThumb) continue;
    const uint64_t sv = g_counters.served[i].load(std::memory_order_relaxed), z = g_counters.zeros[i].load(std::memory_order_relaxed), un = g_counters.unserved[i].load(std::memory_order_relaxed);
    std::snprintf(b, sizeof b, " %s served %llu zeros %llu refused %llu", kSites[i].id, static_cast<unsigned long long>(sv), static_cast<unsigned long long>(z), static_cast<unsigned long long>(un >= z ? un - z : 0));
    s += b;
  }
  for (uint32_t i = 0; i < kSiteCount; ++i) {   // R7 (Create a Title thumbnail): written, black written, refused; then its deferral
    if (kSites[i].kind != Kind::TitleShot) continue;
    const uint64_t sv = g_counters.served[i].load(std::memory_order_relaxed), z = g_counters.zeros[i].load(std::memory_order_relaxed), un = g_counters.unserved[i].load(std::memory_order_relaxed);
    std::snprintf(b, sizeof b, "; title: %s served %llu zeros %llu refused %llu deferred %llu delivered %llu no_scene %llu timed_out %llu dropped %llu not_deferred %llu", kSites[i].id, static_cast<unsigned long long>(sv),
                  static_cast<unsigned long long>(z), static_cast<unsigned long long>(un >= z ? un - z : 0), static_cast<unsigned long long>(g_counters.title_deferred.load()), static_cast<unsigned long long>(g_counters.title_delivered.load()),
                  static_cast<unsigned long long>(g_counters.title_no_scene.load()), static_cast<unsigned long long>(g_counters.title_timed_out.load()), static_cast<unsigned long long>(g_counters.title_dropped.load()),
                  static_cast<unsigned long long>(g_counters.title_refused.load()));
    s += b;
  }
  std::snprintf(b, sizeof b, "; R1 deferred %llu delivered %llu no_scene %llu before_write %llu dropped %llu timed_out %llu refused %llu; composed %llu; at_untile %llu; lock_other %llu",
                static_cast<unsigned long long>(g_counters.thumb_deferred.load()), static_cast<unsigned long long>(g_counters.thumb_delivered.load()), static_cast<unsigned long long>(g_counters.thumb_no_scene.load()),
                static_cast<unsigned long long>(g_counters.thumb_before_write.load()), static_cast<unsigned long long>(g_counters.thumb_dropped.load()), static_cast<unsigned long long>(g_counters.thumb_timed_out.load()),
                static_cast<unsigned long long>(g_counters.thumb_refused.load()), static_cast<unsigned long long>(g_counters.composed.load(std::memory_order_relaxed)), static_cast<unsigned long long>(g_counters.at_untile.load(std::memory_order_relaxed)), static_cast<unsigned long long>(g_counters.lock_other.load(std::memory_order_relaxed)));
  s += b;
  // the busiest other return addresses (at most 6)
  uint32_t used[6] = {};
  for (int k = 0; k < 6; ++k) {
    uint64_t best = 0;
    int bi = -1;
    for (uint32_t i = 0; i < Counters::kOtherSlots; ++i) {
      const uint64_t v = g_counters.other_n[i].load(std::memory_order_relaxed);
      bool taken = false;
      for (int u = 0; u < k; ++u) taken = taken || used[u] == i + 1;
      if (v > best && !taken) best = v, bi = int(i);
    }
    if (bi < 0) break;
    used[k] = uint32_t(bi) + 1;
    std::snprintf(b, sizeof b, "%s%08X=%llu", k ? " " : " (", g_counters.other_lr[bi].load(std::memory_order_relaxed), static_cast<unsigned long long>(best));
    s += b;
  }
  if (used[0]) s += ")";
  return s;
}
inline void ResetCountersForTest() {
  Counters& c = g_counters;
  for (uint32_t i = 0; i < kSiteCount; ++i) {
    c.locks[i] = 0, c.served[i] = 0, c.unserved[i] = 0, c.zeros[i] = 0;
    for (uint32_t k = 0; k < kWhyCount; ++k) c.why[i][k] = 0;
  }
  c.composed = 0, c.at_untile = 0, c.lock_other = 0;
  for (uint32_t i = 0; i < Counters::kOtherSlots; ++i) c.other_lr[i] = 0, c.other_n[i] = 0;
  c.thumb_deferred = c.thumb_delivered = c.thumb_no_scene = c.thumb_before_write = c.thumb_dropped = c.thumb_timed_out = c.thumb_refused = 0;
  c.title_deferred = c.title_delivered = c.title_no_scene = c.title_timed_out = c.title_dropped = c.title_refused = 0;
}
inline bool LogDue(uint64_t n) { return n != 0 && (n <= 4 || (n & (n - 1)) == 0); }

// ---- the guard and the write ------------------------------------------------------------------------------------------------------------------------------------------------------------
using PhysFn = uint8_t* (*)(uint32_t physical);
using WritableFn = bool (*)(uint32_t physical, uint64_t bytes);   // the game build: every page of the physical range is committed and writable (PhysWritable)
struct Guard {
  uint32_t want_w = 0, want_h = 0;   // the size this site writes (0 = any)
  WritableFn writable = nullptr;     // null = not checked (the unit's plain buffers)
};

// The fetch dwords -> a plan this site may write (2D, format 6 / 2, the site's own size, no packed tail). Nothing is touched.
inline readback_tile::Why Prepare(const uint32_t w[6], const Guard& g, readback_tile::Plan& p, uint32_t level = 0) {
  using readback_tile::Why;
  if (level != 0) return Why::Level;
  const Why why = readback_tile::MakePlan(w, p);
  if (why != Why::Ok) return why;
  if (RBC_PLANT(5)) return Why::Ok;   // (plant: no site guard)
  if (g.want_w && g.want_h && (p.width != g.want_w || p.height != g.want_h)) return Why::SizeMismatch;
  if (p.packed && std::min(p.width, p.height) <= 16) return Why::Packed;
  return Why::Ok;
}

// Writes a picture (our RGBA, rw x rh) into the planned texture: its pages must be committed and writable (a freed or reserved page is a host fault), then it is scaled (box filter) to the texture's own
// size when it differs and tiled / swizzled / endian-swapped into guest memory.
inline readback_tile::Why Write(const readback_tile::Plan& p, PhysFn phys, const Guard& g, const uint8_t* rgba, uint32_t rw, uint32_t rh) {
  using readback_tile::Why;
  if (!rgba || !rw || !rh) return Why::NoPicture;
  uint8_t* mem = phys ? phys(p.base) : nullptr;
  if (!mem) return Why::NoAddress;
  if (!RBC_PLANT(5) && g.writable && !g.writable(p.base, p.ExtentBytes())) return Why::NotWritable;
  if (rw == p.width && rh == p.height) return readback_tile::WriteLevel0(p, mem, rgba, rw, rh);
  std::vector<uint8_t> scaled;
  readback_tile::ScaleBox(rgba, rw, rh, p.width, p.height, scaled);
  return readback_tile::WriteLevel0(p, mem, scaled.data(), p.width, p.height);
}
// Every page of the physical range [phys, phys + bytes) passes `page_writable(address_inside_the_page)` (the physical heap's own table; safe_guest_pages.h PhysReadable walks it). Pure: the game build asks the heap, the unit a table.
template <typename Pred>
inline bool PhysWritable(uint32_t phys, uint64_t bytes, Pred&& page_writable) {
  return wwe2k14::safe_pages::PhysReadable(phys, bytes, page_writable);
}
// Prepare + Write in one call (the unit's entry; the callers that already hold a plan use Write).
inline readback_tile::Why WritePicture(const uint32_t w[6], PhysFn phys, const Guard& g, const uint8_t* rgba, uint32_t rw, uint32_t rh, uint32_t level = 0) {
  readback_tile::Plan p;
  const readback_tile::Why why = Prepare(w, g, p, level);
  return why != readback_tile::Why::Ok ? why : Write(p, phys, g, rgba, rw, rh);
}

struct ScenePic {
  const uint8_t* rgba = nullptr;
  uint32_t w = 0, h = 0;
};
// Black (0, 0, 0, 255) over the whole planned texture: what a site writes when its picture is not available and what it writes into is saved (R8 / R9: never garbage into the player's reel).
inline readback_tile::Why WriteBlack(const readback_tile::Plan& p, PhysFn phys, const Guard& g) {
  std::vector<uint8_t> black(size_t(p.width) * p.height * 4, 0);
  for (size_t k = 3; k < black.size(); k += 4) black[k] = 255;
  return Write(p, phys, g, black.data(), p.width, p.height);
}

// A served site's whole decision, counted. `get_picture(ScenePic&)` is asked only after the guard accepts the texture, and returns false when there is no 3D scene: then NOTHING is written (R2) and the site
// is counted unserved (Why::NoScene). Returns the reason (Ok = written).
// `zero_when_none` (R9): with no scene the texture gets BLACK and the site is counted zeros (+ unserved{NoScene}); if the black write itself fails, the write's own reason is counted.
template <class Fn>
inline readback_tile::Why ServeSite(int site, const uint32_t w[6], PhysFn phys, const Guard& g, Fn&& get_picture, uint32_t level = 0, bool zero_when_none = false) {
  using readback_tile::Why;
  readback_tile::Plan p;
  Why why = Prepare(w, g, p, level);
  if (why == Why::Ok) {
    ScenePic pic;
    if (!get_picture(pic) || !pic.rgba) {
      why = Why::NoScene;
      if ((zero_when_none && !RBC_PLANT(12)) || RBC_PLANT(6)) {   // (plant 6: a site without a scene writes black anyway; plant 12: R9 leaves the memory as it was = garbage into the save)
        const Why wb = WriteBlack(p, phys, g);
        if (zero_when_none) {
          if (wb == Why::Ok) NoteZeros(site, why);
          else NoteUnserved(site, wb);
          return wb == Why::Ok ? why : wb;
        }
      }
    } else {
      why = Write(p, phys, g, pic.rgba, pic.w, pic.h);
    }
  }
  if (why == Why::Ok) NoteServed(site);
  else NoteUnserved(site, why);
  return why;
}

// ---- Create a Highlight reel: R9's picture is kept for R8 --------------------------------------------------------------------------------------------------------------------------------
// R9 (a worker thread) stores the picture it wrote, at the texture's own size; R8 (the render thread) reads it. A mutex guards the buffer; R8 only ever copies (never waits for a picture).
struct ReelStore {
  std::mutex mu;
  std::vector<uint8_t> rgba;
  uint32_t w = 0, h = 0;
};
inline ReelStore g_reel;
inline void ReelDrop() {   // R9's lock starts a new reel: R8 must never crop an earlier reel's picture
  std::lock_guard<std::mutex> lock(g_reel.mu);
  g_reel.rgba.clear(), g_reel.w = g_reel.h = 0;
}
inline void ReelSet(const uint8_t* rgba, uint32_t w, uint32_t h) {
  std::lock_guard<std::mutex> lock(g_reel.mu);
  g_reel.rgba.assign(rgba, rgba + size_t(w) * h * 4), g_reel.w = w, g_reel.h = h;
}
inline bool ReelCopy(std::vector<uint8_t>& out, uint32_t& w, uint32_t& h) {
  std::lock_guard<std::mutex> lock(g_reel.mu);
  if (g_reel.rgba.empty() || !g_reel.w || !g_reel.h) return false;
  out = g_reel.rgba, w = g_reel.w, h = g_reel.h;
  return true;
}

inline std::atomic<int> g_r8_waits{0};   // test only (plant 10): how often R8 would have waited on the render thread; production never touches it

struct ThumbRect {
  int32_t l = 0, t = 0, r = 0, b = 0;
};
// The crop the game's own quad draw makes (sub_82295B50 +9C .. +1A0): `rect` = obj + 64 .. + 76 (left, top, right, bottom: R9 hands over x, y, x + w, y + h); when it equals the default rect
// (0x836F2BE0 .. EC) the crop is the whole W x H, or, when [0x8392DC3C] != 0, its centre 4:3: x = trunc((W - H * 4/3) * 0.5) (fnmsubs + fctiwz, single precision), rect (x, 0, W - x, H).
// Clamped to the picture; an empty result is Why::Rect.
// `*is_default` (nullable) returns whether the rect is the default one: it also picks the letterbox the game draws the crop into (ThumbDest).
inline readback_tile::Why ThumbCrop(const int32_t rect[4], const int32_t def[4], bool four_three, uint32_t W, uint32_t H, ThumbRect& o, bool* is_default_out = nullptr) {
  const bool is_default = RBC_PLANT(11) || (rect[0] == def[0] && rect[1] == def[1] && rect[2] == def[2] && rect[3] == def[3]);
  if (is_default_out) *is_default_out = is_default;
  if (is_default) {
    int32_t x = 0;
    if (four_three) x = int32_t(std::trunc(std::fmaf(-float(H), 1.3333334f, float(W)) * 0.5f));
    o.l = x, o.t = 0, o.r = int32_t(W) - x, o.b = int32_t(H);
  } else {
    o.l = rect[0], o.t = rect[1], o.r = rect[2], o.b = rect[3];
  }
  o.l = std::max<int32_t>(o.l, 0), o.t = std::max<int32_t>(o.t, 0), o.r = std::min<int32_t>(o.r, int32_t(W)), o.b = std::min<int32_t>(o.b, int32_t(H));
  return (o.r <= o.l || o.b <= o.t) ? readback_tile::Why::Rect : readback_tile::Why::Ok;
}
// Where the game draws the crop inside the 128x128 target (sub_82295B50 0x82295CC4..CD8 and 0x82295D00..D10, constants read from the image): the target is cleared to black and the quad is LETTERBOXED, not stretched:
// the default rect is drawn 128x96 at (0, 16), a rect the game set 90x128 at (19, 0).
struct ThumbDest {
  uint32_t x, y, w, h;
};
inline ThumbDest ThumbDestFor(bool is_default) {
  if (RBC_PLANT(13)) return {0, 0, 128, 128};   // (plant 13: the destination rect is ignored: the crop is stretched over the whole target)
  return is_default ? ThumbDest{0, 16, 128, 96} : ThumbDest{19, 0, 90, 128};
}
inline void CropRgba(const uint8_t* src, uint32_t W, const ThumbRect& r, std::vector<uint8_t>& out) {
  const uint32_t cw = uint32_t(r.r - r.l), ch = uint32_t(r.b - r.t);
  out.resize(size_t(cw) * ch * 4);
  for (uint32_t y = 0; y < ch; ++y) std::copy_n(src + (size_t(r.t + y) * W + r.l) * 4, size_t(cw) * 4, out.data() + size_t(y) * cw * 4);
}

// R9's whole decision, in the order the game runs it: the lock drops the previous reel's picture, the guard (the game's screen size is the texture's size), a scene copy (`capture(rgba)` returns its width / height, {0, 0} = no
// scene), the write (BLACK when there is no scene), and the picture, at the texture's size, is kept for R8. A texture of another size or format is refused and nothing is written.
template <class Capture>
inline readback_tile::Why ServeReelPictureCore(int site, const uint32_t w[6], PhysFn phys, const Guard& g, Capture&& capture, uint32_t level = 0) {
  using readback_tile::Why;
  ReelDrop();
  // The picture's size is the TEXTURE's own (its fetch dwords): the game's live screen size changes every frame on the game thread (stage_page_draw), so it is never read here (the guard's want_w / want_h stay 0).
  readback_tile::Plan plan;
  const Why pre = Prepare(w, g, plan, level);
  if (pre != Why::Ok) {   // not a plain 2D 8888 texture, a packed tail, a level other than 0: refused, nothing written, no scene asked for
    NoteUnserved(site, pre);
    return pre;
  }
  const uint32_t tw = plan.width, th = plan.height;
  std::vector<uint8_t> rgba, scaled;
  const uint8_t* kept = nullptr;
  const Why why = ServeSite(site, w, phys, g, [&](ScenePic& pic) {
    const std::pair<uint32_t, uint32_t> sz = capture(rgba);
    if (!sz.first || !sz.second || rgba.size() < size_t(sz.first) * sz.second * 4) return false;
    if (sz.first == tw && sz.second == th) {
      kept = rgba.data();
    } else {   // the picture R8 crops is at the texture's own size (the rect the game hands over is in that size's pixels)
      readback_tile::ScaleBox(rgba.data(), sz.first, sz.second, tw, th, scaled);
      kept = scaled.data();
    }
    pic.rgba = kept, pic.w = tw, pic.h = th;
    return true;
  }, level, true);
  if (why == Why::Ok && kept) ReelSet(kept, tw, th);
  return why;
}

// R8's whole decision (render thread: no waiting, no capture). The guard first (a texture that is not ours is never touched: counted refused, nothing written); then R9's kept picture is cropped by the
// game's rect, box-scaled to the texture and written. No picture / a source of another size ([obj+44], [obj+48] = the size the game's quad draw read the source at; 0 = unknown) / an unusable rect = BLACK, counted zeros.
inline readback_tile::Why ServeThumb(int site, const uint32_t w[6], PhysFn phys, const Guard& g, const int32_t rect[4], const int32_t def[4], bool four_three, uint32_t src_w, uint32_t src_h) {
  using readback_tile::Why;
  readback_tile::Plan p;
  Why why = Prepare(w, g, p, 0);
  if (why != Why::Ok) {
    NoteUnserved(site, why);
    return why;
  }
  // The crop and the scale read straight from the kept picture under its lock (no copy of the whole picture); the 128x128 canvas is black and the scaled crop sits in the game's letterbox (ThumbDest).
  std::vector<uint8_t> canvas(size_t(p.width) * p.height * 4, 0), crop, scaled;
  for (size_t k = 3; k < canvas.size(); k += 4) canvas[k] = 255;
  {
    std::lock_guard<std::mutex> lock(g_reel.mu);
    const uint32_t pw = g_reel.w, ph = g_reel.h;
    ThumbRect r;
    bool is_default = false;
    if (g_reel.rgba.empty() || !pw || !ph) {
      why = Why::NoPicture;
      if (RBC_PLANT(10)) g_r8_waits.fetch_add(1);   // (plant 10: R8 waits for a scene on the render thread when it has no picture)
    } else if ((src_w && src_w != pw) || (src_h && src_h != ph)) why = Why::SizeMismatch;
    else why = ThumbCrop(rect, def, four_three, pw, ph, r, &is_default);
    if (why == Why::Ok) {
      const ThumbDest d = ThumbDestFor(is_default);
      CropRgba(g_reel.rgba.data(), pw, r, crop);
      readback_tile::ScaleBox(crop.data(), uint32_t(r.r - r.l), uint32_t(r.b - r.t), d.w, d.h, scaled);
      for (uint32_t y = 0; y < d.h && d.y + y < p.height; ++y)
        for (uint32_t x = 0; x < d.w && d.x + x < p.width; ++x) std::copy_n(&scaled[(size_t(y) * d.w + x) * 4], 4, &canvas[(size_t(d.y + y) * p.width + d.x + x) * 4]);
    }
  }
  if (why != Why::Ok) {   // the canvas is black: write it (never garbage into the save)
    const Why wb = Write(p, phys, g, canvas.data(), p.width, p.height);
    if (wb == Why::Ok) NoteZeros(site, why);
    else NoteUnserved(site, wb);
    return why;
  }
  why = Write(p, phys, g, canvas.data(), p.width, p.height);
  if (why == Why::Ok) {
    NoteServed(site);
    ReelDrop();   // this reel is done: the next one never crops it
  } else {
    NoteUnserved(site, why);
  }
  return why;
}

}  // namespace wwe2k14::readback_ours

// ================================================================================================================================================
// The game part: the lock body, the switch, the scene source. Not in the unit.
// ================================================================================================================================================
#if !defined(WWE2K14_READBACK_CORE_ONLY)
#include <chrono>


#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "../d3d_layer.h"   // Ld32
#include "../shown_frame.h"  // g_scene_capture_owner / g_scene_request_owner
#include "title_thumb_core.h"  // S111 CACTHUMB: R7's lock (ServeTitleShot, t_delivered)

#include "readbacks_start.h"   // S111 RBSWITCH: wwe2k14_rdr_readbacks is start-only; every reader calls readbacks_start::On()

REXCVAR_DECLARE(bool, wwe2k14_rdr_reel_readbacks);

namespace wwe2k14::readback_ours {

inline uint8_t* PhysHost(uint32_t physical) { return REX_KERNEL_MEMORY()->TranslatePhysical<uint8_t*>(physical); }

// Every page of the physical range is committed and read-write in the PHYSICAL heap's own table (xmemory QueryProtect, the pattern of wrestler_hook.h PhysRangeReadable; the pixel memory is read through
// TranslatePhysical, so it is the physical heap that controls it). Works on every platform (Windows, Android): no VirtualQuery.
inline bool HostWritable(uint32_t physical, uint64_t bytes) {
  auto* ph = REX_KERNEL_MEMORY()->GetPhysicalHeap();
  if (!ph) return false;
  return PhysWritable(physical, bytes, [ph](uint32_t page) {
    uint32_t pr = 0;
    return ph->QueryProtect(page, &pr) && (pr & rex::memory::kMemoryProtectRead) != 0 && (pr & rex::memory::kMemoryProtectWrite) != 0;
  });
}

inline void FetchWords(uint8_t* base, uint32_t tex, uint32_t w[6]) {
  for (uint32_t k = 0; k < 6; ++k) w[k] = wwe2k14::d3d_layer::Ld32(base, tex + 28 + 4 * k);
}

// R9 (worker thread): a scene copy into [this+88] at the texture's own size; black when there is no scene; the picture is kept for R8.
inline void ServeReelPicture(uint8_t* base, int i, uint32_t lr, uint32_t tex, uint32_t level, uint64_t n) {
  const Site& site = kSites[i];
  const uint64_t z0 = g_counters.zeros[i].load(std::memory_order_relaxed);
  uint32_t w[6];
  FetchWords(base, tex, w);
  Guard g;   // no wanted size: the picture is the texture's own (the live screen size [[0x83929A68]+300 / +304] changes every frame on the game thread and is never read)
  g.writable = &HostWritable;
  const uint32_t tw = (w[2] & 0x1FFF) + 1, th = ((w[2] >> 13) & 0x1FFF) + 1;   // for the log only
  const auto t0 = std::chrono::steady_clock::now();
  uint32_t rw = 0, rh = 0;
  uint64_t scene_frame = 0;
  const readback_tile::Why why = ServeReelPictureCore(i, w, &PhysHost, g, [&](std::vector<uint8_t>& rgba) {
    return wwe2k14::g_scene_capture_owner && wwe2k14::g_scene_capture_owner(rgba, rw, rh, 500, &scene_frame) ? std::make_pair(rw, rh) : std::make_pair(0u, 0u);
  }, level);
  const double ms = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(std::chrono::steady_clock::now() - t0).count();
  if (!LogDue(n)) return;
  if (why == readback_tile::Why::Ok)
    REXLOG_INFO("[rdr][readback] lock {} from {:08X} ({}): texture {:08X} {}x{} written from the 3D scene {}x{} (frame {}) in {:.1f} ms; kept for R8", n, lr, site.id, tex, tw, th, rw, rh, scene_frame, ms);
  else
    REXLOG_WARN("[rdr][readback] lock {} from {:08X} ({}): texture {:08X} {}x{} NOT served: {} ({}; fetch {:08X} {:08X} {:08X} {:08X}, level {}) in {:.1f} ms", n, lr, site.id, tex, tw, th, readback_tile::WhyText(why),
                g_counters.zeros[i].load(std::memory_order_relaxed) > z0 ? "black written" : "memory left as it was", w[0], w[1], w[2], w[3], level, ms);
}

// R8 (render thread, op-68 callback): R9's kept picture, cropped by the game's rect, box-scaled to 128x128. Never waits.
inline void ServeReelThumb(uint8_t* base, int i, uint32_t lr, uint32_t tex, uint32_t level, uint64_t n) {
  const Site& site = kSites[i];
  const uint64_t z0 = g_counters.zeros[i].load(std::memory_order_relaxed);
  using wwe2k14::d3d_layer::Ld32;
  uint32_t w[6];
  FetchWords(base, tex, w);
  const uint32_t obj = Ld32(base, 0x836F2BD0);   // the reel thumbnail object (sub_82295B50 reads its rect from it)
  int32_t rect[4] = {0, 0, 0, 0}, def[4] = {0, 0, 0, 0};
  uint32_t src_w = 0, src_h = 0;
  if (obj) {
    for (uint32_t k = 0; k < 4; ++k) rect[k] = int32_t(Ld32(base, obj + 64 + 4 * k)), def[k] = int32_t(Ld32(base, 0x836F2BE0 + 4 * k));
    src_w = Ld32(base, obj + 44), src_h = Ld32(base, obj + 48);
  }
  const bool four_three = Ld32(base, 0x8392DC3C) != 0;
  Guard g;
  g.want_w = 128, g.want_h = 128;
  g.writable = &HostWritable;
  readback_tile::Why why;
  if (!obj || level != 0) {   // no object: no rect to apply, nothing ours to write; a level other than 0 is not the picture
    why = obj ? readback_tile::Why::Level : readback_tile::Why::NoAddress;
    NoteUnserved(i, why);
  } else {
    why = ServeThumb(i, w, &PhysHost, g, rect, def, four_three, src_w, src_h);
  }
  if (!LogDue(n)) return;
  if (why == readback_tile::Why::Ok)
    REXLOG_INFO("[rdr][readback] lock {} from {:08X} ({}): texture {:08X} 128x128 written from R9's picture, rect {} {} {} {}{} (source {}x{}; [obj] {:08X})", n, lr, site.id, tex, rect[0], rect[1], rect[2], rect[3],
                (rect[0] == def[0] && rect[1] == def[1] && rect[2] == def[2] && rect[3] == def[3]) ? (four_three ? " (default, centre 4:3)" : " (default, whole)") : "", src_w, src_h, Ld32(base, obj));
  else
    REXLOG_WARN("[rdr][readback] lock {} from {:08X} ({}): texture {:08X} NOT served: {} ({}; source {}x{}, rect {} {} {} {}, [obj] {:08X}, fetch {:08X} {:08X} {:08X} {:08X}, level {})", n, lr, site.id, tex, readback_tile::WhyText(why),
                g_counters.zeros[i].load(std::memory_order_relaxed) > z0 ? "black written" : "memory left as it was", src_w, src_h, rect[0], rect[1], rect[2], rect[3], obj ? Ld32(base, obj) : 0u, w[0], w[1], w[2], w[3], level);
}

// R7 (render thread): the Create a Title thumbnail's lock. Inside title_thumb_off.h's re-run the delivered picture (or black) is written; a lock outside it (the callback ran un-deferred) writes BLACK. Never waits.
inline void ServeTitleLock(uint8_t* base, int i, uint32_t lr, uint32_t tex, uint32_t level, uint64_t n) {
  const Site& site = kSites[i];
  const uint64_t z0 = g_counters.zeros[i].load(std::memory_order_relaxed);
  uint32_t w[6];
  FetchWords(base, tex, w);
  Guard g;
  g.want_w = title_thumb::kW, g.want_h = title_thumb::kH;
  g.writable = &HostWritable;
  const title_thumb::Delivered d = title_thumb::t_delivered;
  readback_tile::Why why;
  if (level != 0) {
    why = readback_tile::Why::Level;
    NoteUnserved(i, why);
  } else {
    why = title_thumb::ServeTitleShot(i, w, &PhysHost, g, d.active ? d.rgba : nullptr, d.active ? readback_tile::Why::NoScene : readback_tile::Why::NoPicture);
  }
  if (!LogDue(n)) return;
  if (why == readback_tile::Why::Ok)
    REXLOG_INFO("[rdr][readback] lock {} from {:08X} ({}): texture {:08X} 256x128 written from the 3D scene's top-left (the resolve rect)", n, lr, site.id, tex);
  else
    REXLOG_WARN("[rdr][readback] lock {} from {:08X} ({}): texture {:08X} NOT served: {} ({}; {}; fetch {:08X} {:08X} {:08X} {:08X}, level {})", n, lr, site.id, tex, readback_tile::WhyText(why),
                g_counters.zeros[i].load(std::memory_order_relaxed) > z0 ? "black written" : "memory left as it was", d.active ? "deferred" : "the callback ran un-deferred", w[0], w[1], w[2], w[3], level);
}

// The OFF build's lock body (d3d_textures.h REX_HOOK_RAW(sub_82CA7EB8), before the original runs): r3 = the texture, r6 = the level, lr = the caller of LockRect.
inline void OnLock(PPCContext& ctx, uint8_t* base) {
  if (!readbacks_start::On()) return;
  const uint32_t lr = uint32_t(ctx.lr), tex = ctx.r3.u32, level = ctx.r6.u32;
  uint64_t n = 0;
  const Action a = Classify(lr, &n);
  if (a == Action::Composed || a == Action::AtUntile) return;   // (nothing written here; R3 is served at the untile, no "not served" line)
  if (a == Action::Other) {
    if (n == 1 && g_counters.lock_other.load(std::memory_order_relaxed) <= 64) REXLOG_INFO("[rdr][readback] lock_other from {:08X}", lr);   // (uploads and inits; the census lists the busiest)
    return;
  }
  const int i = SiteIndex(lr);
  const Site& site = kSites[i];
  if (a == Action::Unserved) {
    if (LogDue(n)) REXLOG_INFO("[rdr][readback] lock {} from {:08X} ({}): NOT served in this build{}", n, lr, site.id, n > 4 ? "; " + CensusLine() : std::string());
    return;
  }
  if (site.kind == Kind::TitleShot) {   // R7 (S111 CACTHUMB): the Create a Title thumbnail; render thread, never waits
    ServeTitleLock(base, i, lr, tex, level, n);
    return;
  }
  if (site.kind == Kind::ReelPicture || site.kind == Kind::ReelThumb) {   // R9 (worker) / R8 (render thread, never waits): the Create a Highlight reel save
    if (!REXCVAR_GET(wwe2k14_rdr_reel_readbacks)) {
      NoteUnserved(i, readback_tile::Why::SwitchedOff);
      if (LogDue(n)) REXLOG_INFO("[rdr][readback] lock {} from {:08X} ({}): reel read-backs are switched off (wwe2k14_rdr_reel_readbacks), memory as it is", n, lr, site.id);
      return;
    }
    if (site.kind == Kind::ReelPicture) ServeReelPicture(base, i, lr, tex, level, n);
    else ServeReelThumb(base, i, lr, tex, level, n);
    return;
  }
  if (site.kind == Kind::Thumb) {   // R1 is the deferral's (thumb_defer_off.h). Its own re-run of the job (t_thumb_replay) finds the picture in memory. A lock that reaches here un-deferred (no scene owner, the board full, a size the guard refused) is counted.
    if (t_thumb_replay) return;
    NoteUnserved(i, readback_tile::Why::NoScene);
    if (LogDue(n)) REXLOG_INFO("[rdr][readback] lock {} from {:08X} (R1): the thumbnail job ran un-deferred, memory as it is{}", n, lr, n > 4 ? "; " + CensusLine() : std::string());
    return;
  }
  // R2: the Create an Arena screenshot. 1280x720, or 640x480 when [0x8392DC3C] != 1 (sub_822A61C8 +1C).
  uint32_t w[6];
  FetchWords(base, tex, w);
  Guard g;
  const bool hd = wwe2k14::d3d_layer::Ld32(base, 0x8392DC3C) == 1;
  g.want_w = hd ? 1280 : 640, g.want_h = hd ? 720 : 480;
  g.writable = &HostWritable;
  const auto t0 = std::chrono::steady_clock::now();
  std::vector<uint8_t> rgba;
  uint32_t rw = 0, rh = 0;
  uint64_t scene_frame = 0;
  const readback_tile::Why why = ServeSite(i, w, &PhysHost, g, [&](ScenePic& pic) {
    if (!wwe2k14::g_scene_capture_owner || !wwe2k14::g_scene_capture_owner(rgba, rw, rh, 500, &scene_frame) || rgba.size() < size_t(rw) * rh * 4) return false;
    pic.rgba = rgba.data(), pic.w = rw, pic.h = rh;
    return true;
  }, level);
  const double ms = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(std::chrono::steady_clock::now() - t0).count();
  if (!LogDue(n)) return;
  if (why == readback_tile::Why::Ok)
    REXLOG_INFO("[rdr][readback] lock {} from {:08X} ({}): texture {:08X} {}x{} format {} swizzle {:03X} endian {} written from the 3D scene {}x{} (frame {}) in {:.1f} ms", n, lr, site.id, tex, g.want_w, g.want_h, w[1] & 0x3F, (w[3] >> 1) & 0xFFF, (w[1] >> 6) & 3, rw, rh, scene_frame, ms);
  else
    REXLOG_WARN("[rdr][readback] lock {} from {:08X} ({}): texture {:08X} NOT written: {} (fetch {:08X} {:08X} {:08X} {:08X}, level {})", n, lr, site.id, tex, readback_tile::WhyText(why), w[0], w[1], w[2], w[3], level);
}

}  // namespace wwe2k14::readback_ours
#endif  // !WWE2K14_READBACK_CORE_ONLY
