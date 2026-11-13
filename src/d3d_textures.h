// wwe2k14 - the D3D layer, STAGE 4 "textures in our own form" (Session 87; docs\native\d3d\S4_TEXTURES_CONTRACT.md, the map
// docs\native\d3d\H_textures.md, option (c)). Glued LAST (half2_hooks.h includes it after d3d_noring.h).
//
// What it is: an object registry for the game's texture headers, built from hooks on the library's create (0x82CA87C0) and destroy
// (0x82CAD760) functions, and the typed items that carry it to the SDK in the no-ring queue (stage 2 level 3):
//   Define   an object's identity words (the header's six fetch dwords, converted the way SetTexture converts them), its kind, an id
//   Bind     slot n of the next draw record holds object id (the id is valid only for the slot write that follows it)
//   Destroy  the id is retired
//   Changed  the engine flushed the object's memory (the unlock flush 0x82CA6BE0): its contents are new
// The items travel in the queue's Registers item (kind 9) as extra ops (16-22), under their own capability cvar gpu_d3d_tex_abi: the
// queue's layout and ABI are untouched. The SDK keeps an id -> identity table (include\rex\graphics\d3d_tex.h); the texture cache
// binds a slot with a Bind id by the OBJECT's identity key instead of deriving it from the fetch constant, and counts every bind by id,
// every fallback to the fetch constant by reason, every disagreement between the two keys.
//
// Switch --wwe2k14_d3d_textures (default 0 = off, every hook below runs its original at once):
//   1 = registry + trace: the hooks log creates / destroys / flushes / the draw-time classification (nothing is sent to the SDK).
//   2 = the cut: registry + items to the SDK (needs wwe2k14_d3d_layer, wwe2k14_d3d_noring 3 and an SDK with gpu_d3d_tex_abi).
//
// Why the Bind id is only sent when the object's header still describes the slot's fetch words (OnDraw): the bound slot's record is
// SetTexture's merge of the object's words with the slot's old record (the listing of 0x82CA8B40, read in Session 87: word 1 and 5 are
// the object's base / mip address in GPU form with the old record's bit 11 / low 9 bits kept; word 0 keeps the old clamp bits 10-21;
// word 3 the old filter bits 19-30; word 4 is mostly the old record's). A slot whose identity words differ from the object's was not
// written from it (the other writers of the fetch shadow): no id, a counted fetch fallback in the SDK.
#pragma once

#include <algorithm>
#include <bit>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "d3d_noring.h"  // d3d_layer helpers, the no-ring queue (Push / PublishPiece / Level), g_guest_frame
#include "guest_view.h"  // S100 strip-360 tier C: the 0xE0000000 view rule, one place
#include "renderer_adapter/reader.h"  // Session 100: the adapter reader (the one guarded call in each hook below)
#include "thumb_defer.h"  // lane 232: the replay-thumbnail job hook (sub_8274FA70) and the present-time delivery

REXCVAR_DEFINE_INT32(wwe2k14_d3d_textures, 0, "wwe2k14",
                     "Session 87 (D3D layer stage 4, textures): 0 = off; 1 = registry + trace logging only; 2 = the cut: texture "
                     "objects registered from the create / destroy hooks, a slot's object id carried to the SDK, whose texture "
                     "cache binds by the object's identity (needs the layer, no-ring level 3 and gpu_d3d_tex_abi)")
    .range(0, 2);
REXCVAR_DEFINE_INT32(wwe2k14_d3d_textures_log_max, 6000, "wwe2k14",
                     "Session 87 (D3D layer stage 4, textures): at most N create / destroy / flush lines in the trace mode (the rest "
                     "are counted in the census)");

REX_EXTERN(__imp__sub_82CA87C0);
REX_EXTERN(__imp__sub_82CAD760);
REX_EXTERN(__imp__sub_82CA6BE0);
REX_EXTERN(__imp__sub_82BD47F0);
REX_EXTERN(__imp__sub_827FB540);
REX_EXTERN(__imp__sub_82CCCF30);

namespace wwe2k14::d3d_tex {

namespace dl = wwe2k14::d3d_layer;
namespace dn = wwe2k14::d3d_noring;

// ------------------------------------------------------------------------------------------------------------------------------
// The exe's copy of the item ops (include\rex\graphics\d3d_tex.h in the SDK is the other copy; tied by gpu_d3d_tex_abi)
// ------------------------------------------------------------------------------------------------------------------------------
inline constexpr int32_t kAbi = 1;
inline constexpr uint32_t kOpDefine0 = 16;   // {16, id, kind | flags << 8, word0}
inline constexpr uint32_t kOpDefine1 = 17;   // {17, id, word1, word2}
inline constexpr uint32_t kOpDefine2 = 18;   // {18, id, word3, word4}
inline constexpr uint32_t kOpDefine3 = 19;   // {19, id, word5, 0}
inline constexpr uint32_t kOpDestroy = 20;   // {20, id, 0, 0}
inline constexpr uint32_t kOpChanged = 21;   // {21, id, 0, 0}
inline constexpr uint32_t kOpBind = 22;      // {22, slot, id, 0}

enum Kind : uint8_t { kUnknown = 0, kRenderTarget = 1, kFileStatic = 2, kEngineDynamic = 3, kKindCount };
inline constexpr const char* kKindNames[kKindCount] = {"unknown", "render_target", "file_static", "engine_dynamic"};

// The creators of map H section 1.3 (function entry, size from `q fn`): the kind of texture each makes. The kinds are the map's
// provisional table; the trace run's census (creator -> create count, formats) is what settles the "unknown" ones.
struct Creator {
  uint32_t entry, size;
  Kind kind;
  const char* name;
};
inline constexpr Creator kCreators[] = {
    {0x825BB398, 532, kRenderTarget, "rt512_fmt6"},       {0x827506A0, 740, kUnknown, "in_ring_only"},
    {0x82799890, 480, kRenderTarget, "rt_fmt15"},         {0x827DC770, 720, kRenderTarget, "present_texture"},
    {0x827EE5C0, 652, kRenderTarget, "rt_k8_512_256"},    {0x827FD3C8, 252, kRenderTarget, "rt_fmt6"},
    {0x82825500, 444, kUnknown, "never_seen"},            {0x82BC39E0, 244, kRenderTarget, "screen_rt_pair"},
    {0x82BCB5B8, 312, kUnknown, "never_seen"},            {0x82BCB9B8, 228, kRenderTarget, "screen_rt"},
    {0x82BD4690, 140, kEngineDynamic, "wrapper_empty_2d"}, {0x82BD4720, 124, kEngineDynamic, "wrapper_empty_cube"},
    {0x82C1C278, 1260, kRenderTarget, "post_chain"},      {0x82C22980, 116, kRenderTarget, "boot_k8"},
    {0x82C26858, 108, kUnknown, "never_seen"},            {0x82C296C0, 344, kRenderTarget, "post_rt"},
    {0x82C2AB90, 512, kRenderTarget, "post_rt2"},         {0x82CC0B20, 636, kUnknown, "lib_0B20"},
    {0x82CCCF30, 2004, kFileStatic, "d3dx_load"},         {0x82CD4CD0, 1308, kUnknown, "d3dx_from_surface"},  /* (Session 87 review: never traced - not classed a static, so its watch is never dropped) */
};
inline const Creator* FindCreator(uint32_t lr) {
  for (const Creator& c : kCreators) {
    if (lr > c.entry && lr <= c.entry + c.size) return &c;
  }
  return nullptr;
}

struct Obj {
  uint32_t addr = 0, id = 0, gen = 0;
  Kind kind = kUnknown;
  const char* creator = "";
  uint32_t creator_lr = 0;
  uint32_t args[8] = {};   // r3..r10 of the create
  uint32_t words[6] = {};  // the header's six fetch dwords at the create's return
  uint32_t defined[6] = {};  // what the SDK was last told (conv. words)
  bool live = false, defined_sent = false, bound_ever = false;
  uint64_t created_frame = 0;
  uint32_t est_begin = 0, est_end = 0;  // the data block's estimated extent (alias check)
};

struct Counters {
  std::atomic<uint64_t> creates{0}, create_fail{0}, destroys{0}, destroy_unknown{0}, addr_reuse_live{0}, addr_reuse_dead{0},
      alias_hits{0};
  std::array<std::atomic<uint64_t>, kKindCount> creates_by_kind{}, live_by_kind{};
  std::atomic<uint64_t> flushes{0}, flush_registered{0}, flush_after_bind{0}, flush_before_bind{0}, flush_via_unmap{0},
      flush_in_loader{0}, plane_flushes{0};
  std::array<std::atomic<uint64_t>, kKindCount> flush_by_kind{};
  std::atomic<uint64_t> loader_calls{0}, loader_objects{0};
  // draw time
  std::atomic<uint64_t> draws{0}, slots_dirty{0}, slots_texture{0}, slots_null{0}, slots_unregistered{0}, slots_words_differ{0},
      slots_with_id{0}, slot_ptr_changed{0}, silent_changes{0}, redefines{0}, defines{0}, binds_pushed{0}, items_pushed{0};
  std::array<std::atomic<uint64_t>, kKindCount> with_id_by_kind{};
  std::array<std::atomic<uint64_t>, 6> word_diff_words{};  // dirty texture slots whose shadow word k differs from the converted header word
  std::array<std::atomic<uint64_t>, 32> word_diff_bits_lo{};  // per bit of the XOR (all six words OR-ed per slot class: see OnDraw)
  std::atomic<uint64_t> changed_pushed{0}, destroy_pushed{0}, level_off{0}, recording_off{0}, race_skips{0};
};

struct Registry {
  std::mutex m;
  std::unordered_map<uint32_t, uint32_t> by_addr;  // guest address -> index of the LIVE object
  std::vector<Obj> objs;                           // index = id - 1 (ids are never reused)
  std::unordered_map<uint32_t, uint32_t> gens;     // guest address -> registrations so far
  std::vector<uint32_t> dynamic_idx;               // indices into objs of the engine-dynamic objects (dead ones pruned by the plane flush)
  Counters c;
  std::atomic<uint64_t> census_frame{0};
  std::atomic<uint64_t> log_lines{0};
  // the per-creator census of the trace
  struct CreatorStat {
    uint64_t creates = 0, cubes = 0, tiled = 0;
    std::unordered_map<uint32_t, uint64_t> formats;
  };
  std::unordered_map<std::string, CreatorStat> by_creator;
  uint64_t xor_word[6][32] = {};  // dirty texture slots: XOR of (shadow word vs converted header word), per bit, counted
};
inline Registry g_reg;

inline thread_local uint32_t t_loader_depth = 0;     // inside 0x82CCCF30 (the D3DX load)
inline thread_local uint32_t t_loader_lr = 0;
inline thread_local bool t_via_unmap = false;        // inside 0x82BD47F0
inline thread_local uint64_t t_loader_created = 0;   // objects created by this loader call

inline int Mode() { return REXCVAR_GET(wwe2k14_d3d_textures); }
inline bool LogRoom() { return g_reg.log_lines.fetch_add(1, std::memory_order_relaxed) < uint64_t(std::max(0, REXCVAR_GET(wwe2k14_d3d_textures_log_max))); }

// The SDK's capability: items are produced only for an SDK that reports gpu_d3d_tex_abi == kAbi, and only while the no-ring queue
// carries the record tokens (level 3), so every item is in order with the draws.
inline bool SdkOk() {
  static const bool ok = [] {
    const auto s = rex::cvar::GetFlagByName("gpu_d3d_tex_abi");
    return !s.empty() && s == std::to_string(kAbi);
  }();
  return ok;
}
inline bool Sending() {
  if (Mode() < 2 || dn::Level() < 3 || !SdkOk()) return false;
  static std::once_flag once;
  std::call_once(once, [] {  // the SDK's own switch, set by name before the first item (the SDK ignores the ops while it is 0)
    rex::cvar::SetFlagByName("gpu_d3d_textures", std::to_string(Mode()));
    REXLOG_INFO("[d3d][tex] the cut is on: items go to the SDK in the no-ring queue (gpu_d3d_textures {})", Mode());
  });
  return true;
}

// The GPU form of an address, as the library's SetTexture puts it into record words 1 and 5.
inline uint32_t GpuForm(uint32_t a) { return wwe2k14::guest_view::ToPhysical(a); }

// The six header dwords as SetTexture would put them into a fresh slot record, for the words that describe the image: word 1 =
// GpuForm(header word 1) (the old record's bit 11 is kept by the merge, so the identity is bits 12-31 and 0-10), word 5 =
// (header word 5 & 0x1FFFFE00) + the GPU-form region bit (the old low 9 bits are kept). The other words are the header's.
inline void ConvertWords(const uint32_t in[6], uint32_t out[6]) {
  out[0] = in[0];
  out[1] = GpuForm(in[1]);
  out[2] = in[2];
  out[3] = in[3];
  out[4] = in[4];
  out[5] = (in[5] & 0x1FFFFE00u) + wwe2k14::guest_view::ViewOffset(in[5]);  // (trace run 001: >> 12 here was wrong, the region bit is the address's top 12 bits)
}

// Which bits of the converted header word k describe the image (the slot's record keeps the old record's value for the rest).
inline constexpr uint32_t kIdentityMask[6] = {
    ~0x003FFC00u,   // word 0: type, signs, pitch, tiled (the old record's clamp bits 10-21 are kept)
    ~0x00000800u,   // word 1: base address, format, endian (bit 11 kept)
    0xFFFFFFFFu,    // word 2: size
    ~0x7FF80000u,   // word 3: swizzle, number format, exp adjust (the old filter bits 19-30 are kept)
    0u,             // word 4: sampler / view (mostly the old record's): not identity
    ~0x000001FFu,   // word 5: mip address (low 9 bits kept)
};

inline uint32_t BitsPerTexel(uint32_t fmt) {
  switch (fmt) {
    case 18: return 4;              // DXT1
    case 19: case 20: return 8;     // DXT3 / DXT5
    case 2: case 0: return 8;       // k_8, k_1_REVERSE (guess)
    case 4: case 5: case 15: case 3: return 16;  // 5_6_5, 6_5_5, 4_4_4_4, 1_5_5_5
    case 6: case 7: case 8: case 23: case 25: return 32;
    case 26: case 27: case 28: return 64;
    default: return 32;
  }
}

// ------------------------------------------------------------------------------------------------------------------------------
// Registry
// ------------------------------------------------------------------------------------------------------------------------------
inline void LogCensusIfDue(bool force = false);

inline void OnCreate(uint8_t* base, uint32_t obj, uint32_t lr, const uint32_t args[8]) {
  Registry& r = g_reg;
  Counters& c = r.c;
  if (!obj) {
    c.create_fail.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  c.creates.fetch_add(1, std::memory_order_relaxed);
  Obj o;
  o.addr = obj;
  o.creator_lr = lr;
  const Creator* cr = t_loader_depth ? nullptr : FindCreator(lr);
  if (t_loader_depth) {
    o.kind = kFileStatic;
    o.creator = "d3dx_load";
  } else if (cr) {
    o.kind = cr->kind;
    o.creator = cr->name;
  } else {
    o.kind = kUnknown;
    o.creator = "unmapped";
  }
  std::memcpy(o.args, args, sizeof(o.args));
  for (uint32_t k = 0; k < 6; ++k) o.words[k] = dl::Ld32(base, obj + 28 + 4 * k);
  // The engine wrapper's two empty-texture creators make both kinds (trace run 001, map H section 8): tiled objects (8888, depth, float
  // targets, every empty cube: 64 x 64 x 6 faces) are resolve destinations; linear k_8 ones are the movie planes the CPU fills.
  if (cr && (cr->entry == 0x82BD4690 || cr->entry == 0x82BD4720)) o.kind = (o.words[0] >> 31) ? kRenderTarget : kEngineDynamic;
  o.created_frame = wwe2k14::g_guest_frame.load(std::memory_order_relaxed);
  // The data block's extent, estimated from the header: base address (GPU form, +32) .. + width x height x layers x bits.
  {
    const uint32_t fmt = o.words[1] & 0x3F;
    const uint32_t w = (o.words[2] & 0x1FFF) + 1, h = ((o.words[2] >> 13) & 0x1FFF) + 1;
    const uint32_t layers = args[7] == 18 ? 6 : 1;  // r10 = the type code (3 2D, 18 cube, 17 volume)
    uint64_t bytes = (uint64_t(w) * h * BitsPerTexel(fmt) / 8) * layers;
    if (args[3] > 1) bytes += bytes / 3;
    o.est_begin = o.words[1] & 0x1FFFF000u;
    o.est_end = o.est_begin + uint32_t(std::min<uint64_t>(bytes, 0x10000000u));
  }
  std::string alias;
  {
    std::lock_guard<std::mutex> lock(r.m);
    uint32_t& g = r.gens[obj];
    o.gen = ++g;
    if (auto it = r.by_addr.find(obj); it != r.by_addr.end()) {  // a live object at this address: its destroy was never seen
      c.addr_reuse_live.fetch_add(1, std::memory_order_relaxed);
      r.objs[it->second].live = false;
      c.live_by_kind[r.objs[it->second].kind].fetch_sub(1, std::memory_order_relaxed);
      if (LogRoom()) {
        REXLOG_INFO("[d3d][tex] create at {:08X} while object id {} is still live there (its destroy was never seen): retired", obj,
                    r.objs[it->second].id);
      }
    } else if (o.gen > 1) {
      c.addr_reuse_dead.fetch_add(1, std::memory_order_relaxed);
    }
    o.id = uint32_t(r.objs.size()) + 1;
    o.live = true;
    if (Mode() == 1 && o.est_end > o.est_begin) {  // the alias check (trace only: a scan of the live objects)
      for (const Obj& other : r.objs) {
        if (!other.live || other.est_end <= other.est_begin) continue;
        if (o.est_begin < other.est_end && other.est_begin < o.est_end) {
          alias += " alias_with_id=" + std::to_string(other.id) + "(" + other.creator + ")";
          c.alias_hits.fetch_add(1, std::memory_order_relaxed);
          break;
        }
      }
    }
    r.by_addr[obj] = uint32_t(r.objs.size());
    if (o.kind == kEngineDynamic) r.dynamic_idx.push_back(uint32_t(r.objs.size()));
    r.objs.push_back(o);
    c.creates_by_kind[o.kind].fetch_add(1, std::memory_order_relaxed);
    c.live_by_kind[o.kind].fetch_add(1, std::memory_order_relaxed);
    auto& st = r.by_creator[o.creator];
    ++st.creates;
    ++st.formats[o.words[1] & 0x3F];
    if (args[7] == 18) ++st.cubes;
    if (o.words[0] >> 31) ++st.tiled;
  }
  if (t_loader_depth) ++t_loader_created;
  if (Mode() == 1 && LogRoom()) {
    REXLOG_INFO("[d3d][tex] create id={} gen={} obj={:08X} lr={:08X} creator={} kind={} frame={} args(w,h,depth,levels,flag,fmtword,r9,type)="
                "{},{},{},{},{:X},{:08X},{:X},{} words={:08X} {:08X} {:08X} {:08X} {:08X} {:08X} tiled={} fmt={} est={:08X}-{:08X}{}",
                o.id, o.gen, obj, lr, o.creator, kKindNames[o.kind], o.created_frame, args[0], args[1], args[2], args[3], args[4],
                args[5], args[6], args[7], o.words[0], o.words[1], o.words[2], o.words[3], o.words[4], o.words[5], o.words[0] >> 31,
                o.words[1] & 0x3F, o.est_begin, o.est_end, alias);
  }
}

// Returns the live object at `addr` (copy) or nullptr-like id 0.
inline bool FindLive(uint32_t addr, Obj& out) {
  Registry& r = g_reg;
  std::lock_guard<std::mutex> lock(r.m);
  const auto it = r.by_addr.find(addr);
  if (it == r.by_addr.end()) return false;
  out = r.objs[it->second];
  return true;
}

inline void PushOps(uint8_t* base, uint32_t dev, bool render_thread, const std::vector<uint32_t>& q) {
  if (q.empty()) return;
  if (render_thread) dn::PublishPiece(base, dev);
  const uint32_t f[6] = {uint32_t(q.size() / 4), 0, 0, 0, 0, 0};
  dn::Push(dn::lay::kRegs, f, q.data(), uint32_t(q.size()));
  dn::Count(dn::kPushRegs);
  g_reg.c.items_pushed.fetch_add(1, std::memory_order_relaxed);
}

inline void AddDefine(std::vector<uint32_t>& q, const Obj& o, const uint32_t w[6]) {
  q.insert(q.end(), {kOpDefine0, o.id, uint32_t(o.kind), w[0]});
  q.insert(q.end(), {kOpDefine1, o.id, w[1], w[2]});
  q.insert(q.end(), {kOpDefine2, o.id, w[3], w[4]});
  q.insert(q.end(), {kOpDefine3, o.id, w[5], 0});
}

inline void OnDestroy(uint8_t* base, uint32_t addr) {
  Registry& r = g_reg;
  Counters& c = r.c;
  Obj o;
  bool sent = false;
  {
    std::lock_guard<std::mutex> lock(r.m);
    const auto it = r.by_addr.find(addr);
    if (it == r.by_addr.end()) return;  // not a registered texture (the function handles every resource kind)
    Obj& live = r.objs[it->second];
    live.live = false;
    o = live;
    sent = live.defined_sent;
    r.by_addr.erase(it);
    c.live_by_kind[o.kind].fetch_sub(1, std::memory_order_relaxed);
  }
  c.destroys.fetch_add(1, std::memory_order_relaxed);
  if (Mode() == 1 && LogRoom()) {
    REXLOG_INFO("[d3d][tex] destroy id={} obj={:08X} creator={} kind={} age_frames={} bound_ever={}", o.id, addr, o.creator,
                kKindNames[o.kind], wwe2k14::g_guest_frame.load(std::memory_order_relaxed) - o.created_frame, o.bound_ever);
  }
  // Retire the id in the SDK before the library frees the blocks (queue order = program order), whatever the no-ring level is now: an
  // id the SDK was told must not outlive its object because the level dropped (Session 87 review D).
  if (sent && Mode() >= 2 && SdkOk()) {
    const std::vector<uint32_t> q = {kOpDestroy, o.id, 0, 0};
    PushOps(base, 0, false, q);
    c.destroy_pushed.fetch_add(1, std::memory_order_relaxed);
  }
}

// ------------------------------------------------------------------------------------------------------------------------------
// The draw writers' call (F1 IndexedDraw, F3 inline Begin): classify the dirty texture slots and send Define / Bind items.
// m_fetch: the draw's dirty fetch mask as the writers read it (slot n = bit 31 - n of the low 32 bits).
// ------------------------------------------------------------------------------------------------------------------------------
inline thread_local std::array<uint32_t, 32> t_last_ptr{};
inline thread_local std::array<std::array<uint32_t, 6>, 32> t_last_words{};
inline thread_local std::vector<uint32_t> t_q;

inline void OnDraw(uint8_t* base, uint32_t dev, uint64_t m_fetch) {
  const int mode = Mode();
  if (!mode) return;
  Registry& r = g_reg;
  Counters& c = r.c;
  c.draws.fetch_add(1, std::memory_order_relaxed);
  const bool trace = mode == 1;
  bool send = Sending();
  if (mode >= 2 && !send) c.level_off.fetch_add(1, std::memory_order_relaxed);
  // Inside the library's recording modes (Recording(): its commands go to a recording buffer; t_original_depth: an original function we
  // called as a fallback) nothing is typed: every other typed hook stays out of them, so these draws take the fetch path (counted).
  if (send && (dn::Recording(base, dev) || dn::t_original_depth)) {
    send = false;
    c.recording_off.fetch_add(1, std::memory_order_relaxed);
  }
  std::vector<uint32_t>& q = t_q;
  q.clear();
  static thread_local std::vector<uint32_t> binds;
  binds.clear();
  const uint32_t dirty = uint32_t(m_fetch & 0xFFFFFFFFull);
  // Session 94 (wwe2k14_d3d_record_lean): outside the trace mode a clean slot is never looked at (its shadow words only feed the trace's
  // silent-change count), so the walk visits the dirty slots only instead of reading 32 x 6 words every draw.
  const bool lean = !trace && REXCVAR_GET(wwe2k14_d3d_record_lean);
  auto next_slot = [&](uint32_t from) -> uint32_t {
    if (!lean) return from;
    if (from >= 32) return 32;
    const uint32_t m = dirty & (0xFFFFFFFFu >> from);
    return m ? uint32_t(std::countl_zero(m)) : 32u;
  };
  for (uint32_t n = next_slot(0); n < 32; n = next_slot(n + 1)) {
    uint32_t shadow[6];
    const uint32_t rec = dev + 1152 + 24 * n;
    for (uint32_t k = 0; k < 6; ++k) shadow[k] = dl::Ld32(base, rec + 4 * k);
    const bool is_dirty = (dirty >> (31 - n)) & 1;
    if ((shadow[0] & 3) != 2) {  // not a texture fetch constant (vertex buffers, a cleared slot)
      t_last_words[n] = {shadow[0], shadow[1], shadow[2], shadow[3], shadow[4], shadow[5]};
      continue;
    }
    if (trace && !is_dirty) {
      bool same = true;
      for (uint32_t k = 0; k < 6; ++k) same &= t_last_words[n][k] == shadow[k];
      if (!same && t_last_ptr[n] != 0) c.silent_changes.fetch_add(1, std::memory_order_relaxed);  // changed with no dirty bit
    }
    t_last_words[n] = {shadow[0], shadow[1], shadow[2], shadow[3], shadow[4], shadow[5]};
    if (!is_dirty) continue;
    c.slots_dirty.fetch_add(1, std::memory_order_relaxed);
    c.slots_texture.fetch_add(1, std::memory_order_relaxed);
    const uint32_t ptr = dl::Ld32(base, dev + 12920 + 4 * n);
    if (ptr != t_last_ptr[n]) c.slot_ptr_changed.fetch_add(1, std::memory_order_relaxed);
    t_last_ptr[n] = ptr;
    if (!ptr) {
      c.slots_null.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    Obj o;
    if (!FindLive(ptr, o)) {
      c.slots_unregistered.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    // The header now vs the slot's record: do the identity words agree after SetTexture's conversion and merge?
    uint32_t now[6], conv[6];
    for (uint32_t k = 0; k < 6; ++k) now[k] = dl::Ld32(base, ptr + 28 + 4 * k);
    ConvertWords(now, conv);
    bool differ = false;
    for (uint32_t k = 0; k < 6; ++k) {
      const uint32_t x = (shadow[k] ^ conv[k]);
      if (x & kIdentityMask[k]) {
        differ = true;
        c.word_diff_words[k].fetch_add(1, std::memory_order_relaxed);
      }
      if (trace && x) {
        std::lock_guard<std::mutex> lock(r.m);
        for (uint32_t b = 0; b < 32; ++b) r.xor_word[k][b] += (x >> b) & 1;
      }
    }
    if (differ) {
      c.slots_words_differ.fetch_add(1, std::memory_order_relaxed);
      if (trace && LogRoom()) {
        REXLOG_INFO("[d3d][tex] slot {} obj {:08X} (id {} {} {}) record differs from the header: shadow {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} "
                    "header-converted {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
                    n, ptr, o.id, o.creator, kKindNames[o.kind], shadow[0], shadow[1], shadow[2], shadow[3], shadow[4], shadow[5],
                    conv[0], conv[1], conv[2], conv[3], conv[4], conv[5]);
      }
      continue;
    }
    c.slots_with_id.fetch_add(1, std::memory_order_relaxed);
    c.with_id_by_kind[o.kind].fetch_add(1, std::memory_order_relaxed);
    if (!send) {
      // (trace only) remember that this object has been bound
      std::lock_guard<std::mutex> lock(r.m);
      if (auto it = r.by_addr.find(ptr); it != r.by_addr.end()) r.objs[it->second].bound_ever = true;
      continue;
    }
    // Define when the SDK has not been told this object, or the header's identity words changed since.
    bool need_define = !o.defined_sent;
    if (!need_define) {
      for (uint32_t k = 0; k < 6 && !need_define; ++k) need_define = (o.defined[k] ^ conv[k]) & kIdentityMask[k];
      if (need_define) c.redefines.fetch_add(1, std::memory_order_relaxed);
    }
    // The object at the slot's pointer is looked at again under the lock: the id must still be the one this draw classified (a destroy /
    // create on another thread between FindLive and here would otherwise Bind a retired id, or mark the new object as told).
    bool still_there = false;
    {
      std::lock_guard<std::mutex> lock(r.m);
      if (auto it = r.by_addr.find(ptr); it != r.by_addr.end() && r.objs[it->second].id == o.id) {
        still_there = true;
        Obj& live = r.objs[it->second];
        live.bound_ever = true;
        if (need_define) {
          std::memcpy(live.defined, conv, sizeof(conv));
          live.defined_sent = true;
        }
      }
    }
    if (!still_there) {
      c.race_skips.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    if (need_define) {
      AddDefine(q, o, conv);
      c.defines.fetch_add(1, std::memory_order_relaxed);
    }
    binds.insert(binds.end(), {kOpBind, n, o.id, 0});
  }
  if (send && (!q.empty() || !binds.empty())) {
    q.insert(q.end(), binds.begin(), binds.end());
    c.binds_pushed.fetch_add(binds.size() / 4, std::memory_order_relaxed);
    PushOps(base, dev, true, q);
  }
  LogCensusIfDue();
}

// ------------------------------------------------------------------------------------------------------------------------------
// Census (every 600 guest frames, from the draw writers)
// ------------------------------------------------------------------------------------------------------------------------------
inline void LogCensusIfDue(bool force) {
  Registry& r = g_reg;
  const uint64_t frame = wwe2k14::g_guest_frame.load(std::memory_order_relaxed);
  uint64_t last = r.census_frame.load(std::memory_order_relaxed);
  if (!force && (frame < last + 600 || !r.census_frame.compare_exchange_strong(last, frame))) return;
  Counters& c = r.c;
  auto take = [](std::atomic<uint64_t>& a) { return a.exchange(0, std::memory_order_relaxed); };
  std::string live;
  for (uint32_t k = 0; k < kKindCount; ++k) {
    live += std::string(k ? ", " : "") + kKindNames[k] + " " + std::to_string(c.live_by_kind[k].load(std::memory_order_relaxed));
  }
  std::string wd;
  for (uint32_t k = 0; k < 6; ++k) wd += (k ? " " : "") + std::to_string(take(c.word_diff_words[k]));
  std::string wk;
  for (uint32_t k = 0; k < kKindCount; ++k) wk += std::string(k ? ", " : "") + kKindNames[k] + " " + std::to_string(take(c.with_id_by_kind[k]));
  REXLOG_INFO(
      "[d3d][tex] frames {}-{}: mode {}; live objects ({}); creates {} (failed {}), destroys {} (unknown-address destroys not counted), address reuse "
      "while live {} / after destroy {}, alias hits {}; draws {}, dirty texture slots {}: with a matching object {} ({}), pointer changed {}, "
      "null {}, unregistered {}, header words differ {} (per word {}), silent record changes {}; sent: defines {} (redefines {}), binds {}, "
      "items {}, destroys {}, changed {}, level-off draws {} (recording-mode draws {}, id races {}); flushes {} (registered {}, via unmap {}, in the loader {}, after first bind {}, "
      "before {}; plane flushes {})",
      last, frame, Mode(), live, take(c.creates), take(c.create_fail), take(c.destroys), take(c.addr_reuse_live), take(c.addr_reuse_dead),
      take(c.alias_hits), take(c.draws), take(c.slots_texture), take(c.slots_with_id), wk, take(c.slot_ptr_changed), take(c.slots_null),
      take(c.slots_unregistered), take(c.slots_words_differ), wd, take(c.silent_changes), take(c.defines), take(c.redefines),
      take(c.binds_pushed), take(c.items_pushed), take(c.destroy_pushed), take(c.changed_pushed), take(c.level_off), take(c.recording_off), take(c.race_skips), take(c.flushes),
      take(c.flush_registered), take(c.flush_via_unmap), take(c.flush_in_loader), take(c.flush_after_bind), take(c.flush_before_bind),
      take(c.plane_flushes));
  if (Mode() == 1) {  // the trace's per-creator table and the XOR heat map of header vs record
    std::string cs;
    std::string xs;
    {
      std::lock_guard<std::mutex> lock(r.m);
      for (auto& [name, st] : r.by_creator) {
        std::string f;
        for (auto& [fmt, n] : st.formats) f += (f.empty() ? "" : ",") + std::to_string(fmt) + "x" + std::to_string(n);
        cs += (cs.empty() ? "" : "; ") + name + " creates " + std::to_string(st.creates) + " cubes " + std::to_string(st.cubes) + " tiled " +
              std::to_string(st.tiled) + " fmts " + f;
      }
      for (uint32_t k = 0; k < 6; ++k) {
        std::string bits;
        for (uint32_t b = 0; b < 32; ++b) {
          if (r.xor_word[k][b]) bits += " b" + std::to_string(b) + "=" + std::to_string(r.xor_word[k][b]);
        }
        xs += "w" + std::to_string(k) + ":" + (bits.empty() ? " none" : bits) + "; ";
      }
    }
    REXLOG_INFO("[d3d][tex] trace creators (cumulative): {}", cs);
    REXLOG_INFO("[d3d][tex] trace record-vs-header XOR bits over dirty texture slots (cumulative; bits the merge keeps from the old record show here): {}", xs);
  }
}

// ------------------------------------------------------------------------------------------------------------------------------
// The hooks
// ------------------------------------------------------------------------------------------------------------------------------
// 0x82CA87C0 create texture (r3 width, r4 height, r5 depth / array, r6 levels, r7 allocation flag, r8 format word, r9 0, r10 type code;
// returns the object in r3 or 0; map H 1.1): the original runs, the object is registered at its return.
REX_HOOK_RAW(sub_82CA87C0) {
  if (!Mode()) {
    __imp__sub_82CA87C0(ctx, base);
    if (REXCVAR_GET(wwe2k14_rdr_capture)) wwe2k14::rdr_reader::OnTextureCreated(ctx, base);
    return;
  }
  const uint32_t lr = uint32_t(ctx.lr);
  const uint32_t args[8] = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32};
  __imp__sub_82CA87C0(ctx, base);
  OnCreate(base, ctx.r3.u32, lr, args);
  if (REXCVAR_GET(wwe2k14_rdr_capture)) wwe2k14::rdr_reader::OnTextureCreated(ctx, base);
}

// 0x82CAD760 destroy resource (r3 = the resource; every kind goes through it, only registered textures are acted on): the id is retired
// before the library waits for the GPU and frees the blocks.
REX_HOOK_RAW(sub_82CAD760) {
  if (REXCVAR_GET(wwe2k14_rdr_capture)) wwe2k14::rdr_reader::OnResourceReleased(ctx, base);
  if (Mode()) OnDestroy(base, ctx.r3.u32);
  __imp__sub_82CAD760(ctx, base);
}

// 0x82CA7EB8 the library's texture lock (Session 100; reached by branch from LockRect 0x82CA8628 and the surface lock 0x82CA8AB0): r3 the
// texture, r5 face / slice, r6 level. Owner mode: a picture our renderer made is read back into memory before the CPU reads it.
REX_HOOK_RAW(sub_82CA7EB8) {
  if (REXCVAR_GET(wwe2k14_rdr_capture)) wwe2k14::rdr_reader::OnTextureLock(ctx, base);
  __imp__sub_82CA7EB8(ctx, base);
}

// 0x82CA6BE0 the unlock flush (r3 = the object; tail-called by the engine wrapper's unmap 0x82BD47F0): the engine says the object's
// memory was written. Trace: which objects, when relative to the first bind. Cut: a Changed item for a defined object.
REX_HOOK_RAW(sub_82CA6BE0) {
  if (REXCVAR_GET(wwe2k14_rdr_capture)) wwe2k14::rdr_reader::OnUnlockFlush(ctx, base);
  const int mode = Mode();
  if (!mode) {
    __imp__sub_82CA6BE0(ctx, base);
    return;
  }
  const uint32_t addr = ctx.r3.u32;
  const uint32_t lr = uint32_t(ctx.lr);
  Counters& c = g_reg.c;
  c.flushes.fetch_add(1, std::memory_order_relaxed);
  if (t_via_unmap) c.flush_via_unmap.fetch_add(1, std::memory_order_relaxed);
  if (t_loader_depth) c.flush_in_loader.fetch_add(1, std::memory_order_relaxed);
  Obj o;
  const bool reg = FindLive(addr, o);
  if (reg) {
    c.flush_registered.fetch_add(1, std::memory_order_relaxed);
    c.flush_by_kind[o.kind].fetch_add(1, std::memory_order_relaxed);
    (o.bound_ever ? c.flush_after_bind : c.flush_before_bind).fetch_add(1, std::memory_order_relaxed);
    if (mode == 1 && LogRoom()) {
      REXLOG_INFO("[d3d][tex] flush obj={:08X} id={} creator={} kind={} lr={:08X} via_unmap={} in_loader={} bound_ever={}", addr, o.id, o.creator,
                  kKindNames[o.kind], lr, t_via_unmap, t_loader_depth != 0, o.bound_ever);
    }
  }
  __imp__sub_82CA6BE0(ctx, base);
  if (reg && o.defined_sent && Sending()) {
    const std::vector<uint32_t> q = {kOpChanged, o.id, 0, 0};
    PushOps(base, 0, false, q);
    c.changed_pushed.fetch_add(1, std::memory_order_relaxed);
  }
}

// 0x82BD47F0 the engine wrapper's unmap (vtable +72): marks the flush above as coming through it.
REX_HOOK_RAW(sub_82BD47F0) {
  if (!Mode()) {
    __imp__sub_82BD47F0(ctx, base);
    return;
  }
  const bool prev = t_via_unmap;
  t_via_unmap = true;
  __imp__sub_82BD47F0(ctx, base);
  t_via_unmap = prev;
}

// The movie planes' flush (0x827FB540, texset_flush_planes; r3 = the texture set): it names no texture object, only the memory of each plane
// of each frame buffer of the set (the listing's loop, read in Session 87: the frame-buffer count at +128; per buffer i the four plane pointers
// at +156 / +168 / +180 / +192 + 48 i and their byte sizes at +32 / +36 / +40 / +44 + 64 i). The registered engine-dynamic objects whose
// data block overlaps a flushed range are the planes of this flush: each gets a Changed item (the CPU has just finished writing them).
inline void PlaneFlushChanged(uint8_t* base, uint32_t set) {
  const uint32_t count = dl::Ld32(base, set + 128);
  if (!count || count > 8) return;
  struct Range { uint32_t begin, end; };
  Range ranges[32];
  uint32_t n = 0;
  for (uint32_t i = 0; i < count && n < 32; ++i) {
    for (uint32_t k = 0; k < 4 && n < 32; ++k) {
      const uint32_t ptr = dl::Ld32(base, set + 156 + 12 * k + 48 * i);
      const uint32_t size = dl::Ld32(base, set + 32 + 4 * k + 64 * i);
      if (!ptr || !size) continue;
      ranges[n++] = {ptr & 0x1FFFFFFFu, (ptr & 0x1FFFFFFFu) + size};
    }
  }
  if (!n) return;
  std::vector<uint32_t> q;
  {
    Registry& r = g_reg;
    std::lock_guard<std::mutex> lock(r.m);
    for (size_t li = 0; li < r.dynamic_idx.size();) {
      const Obj& o = r.objs[r.dynamic_idx[li]];
      if (!o.live) {  // destroyed / retired: out of the list for good
        r.dynamic_idx[li] = r.dynamic_idx.back();
        r.dynamic_idx.pop_back();
        continue;
      }
      ++li;
      if (!o.defined_sent) continue;
      const uint32_t b = o.est_begin & 0x1FFFFFFFu, e = o.est_end & 0x1FFFFFFFu;
      for (uint32_t k = 0; k < n; ++k) {
        // (the E-range's 4 KB quirk: a page of slack at both ends of the match)
        if (ranges[k].begin < e + 0x1000u && b < ranges[k].end + 0x1000u) {
          q.insert(q.end(), {kOpChanged, o.id, 0, 0});
          break;
        }
      }
    }
  }
  if (q.empty()) return;
  PushOps(base, 0, false, q);
  g_reg.c.changed_pushed.fetch_add(q.size() / 4, std::memory_order_relaxed);
}

// 0x827FB540 the video display set's plane flush (once a frame on the display set; texset_flush_planes): counted; with the cut, the planes of
// the flush are marked changed (above).
REX_HOOK_RAW(sub_827FB540) {
  if (REXCVAR_GET(wwe2k14_rdr_capture)) wwe2k14::rdr_reader::OnPlaneFlush(ctx, base);
  if (Mode()) g_reg.c.plane_flushes.fetch_add(1, std::memory_order_relaxed);
  const uint32_t set = ctx.r3.u32;
  __imp__sub_827FB540(ctx, base);
  if (Sending()) PlaneFlushChanged(base, set);
}

// 0x82CCCF30 the D3DX-style create from file image (called by the wrapper slot 0 through 0x82CCD708 / 82CCD778 / 82CCD7F0): the
// textures created inside it are the file-loaded statics; the trace logs the caller and what the call made / flushed.
REX_HOOK_RAW(sub_82CCCF30) {
  if (!Mode()) {
    __imp__sub_82CCCF30(ctx, base);
    return;
  }
  const uint32_t lr = uint32_t(ctx.lr);
  ++t_loader_depth;
  t_loader_lr = lr;
  const uint64_t created_before = t_loader_created;
  g_reg.c.loader_calls.fetch_add(1, std::memory_order_relaxed);
  __imp__sub_82CCCF30(ctx, base);
  --t_loader_depth;
  const uint64_t made = t_loader_created - created_before;
  g_reg.c.loader_objects.fetch_add(made, std::memory_order_relaxed);
  if (Mode() == 1 && LogRoom()) {
    REXLOG_INFO("[d3d][tex] loader 0x82CCCF30 called from {:08X}: made {} texture object(s), result {:08X}", lr, made, ctx.r3.u32);
  }
}

}  // namespace wwe2k14::d3d_tex
