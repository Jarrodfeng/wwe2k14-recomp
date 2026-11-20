// Quads2DTap (Session 103, lane I1, landing 2; docs\renderer\REMNANT_MAP.md sections 5 and 10): the hooks' side of the 2D layer's record. Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// --wwe2k14_rdr_2d_record (default FALSE; off = one relaxed load per call, byte-identical behaviour): the 2D leaves T_Rect (sub_82BC3C18), T_SpriteYUVAlpha (sub_82BC7A10) and (S103 I2) T_Sprite (sub_82BC4890) open a scope; inside it
// the op-19 recorder (sub_82C1B350, hooked in draw_origin.h) hands over the vertices, the op-21 recorder (sub_82BB27D8, hooked in scene_crowd.h) the constants c0 / c1 / c28, the op-28 recorder (sub_82BB1CF8, hooked in draw_origin.h; S103 I2) the sprite's hue-shift pixel constant, and the movie plane flush
// (sub_827FB540, hooked in d3d_textures.h) stamps the flush. The assembly is src\renderer_scene\quads2d_core.h (pure, unit-tested); this file is the registers, the guest memory and the cvar.
// Read only: nothing is written to the guest, nothing is skipped, the recorders run exactly as before. Rows wait in the store until the frame's record takes them (Drain: the main session's
// wiring at landing 2; this piece does not touch the scene snapshot).
#pragma once

#include <rex/cvar.h>
#include <rex/ppc/context.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "../dev_build.h"     // wwe2k14::IsDevBuild
#include "../keep_stream.h"  // WWE2K14_STREAM() (S103 PN4: the OFF-only arming in Active())
#include "../frame_stats.h"  // g_guest_frame
#include "../renderer_scene/quads2d_core.h"
#include "../renderer_scene/walk_cat.h"   // S105 A4B
#include "../renderer_game/fallback_census.h"   // S103 L2b
#if !WWE2K14_STREAM()
#include "../renderer_game/thumb_scope.h"   // S103 RBC: the replay-thumbnail render scope (the frame-restore draw is a 360 habit, not a hole)
#endif
#include "wrestler_hook.h"  // wwe2k14::wrestlers::ReadGuest
#include "picture_tap.h"   // S103 PN3: the first sight of a row's picture key (the stream-free picture source; dev only, default off)

REXCVAR_DEFINE_BOOL(wwe2k14_rdr_2d_record, false, "wwe2k14",
                    "S103 I1 DEV ONLY (ignored by a release build; REMNANT_MAP.md): record the 2D layer (T_Rect fades / panels, the movie YUV quad) as Quad2D rows (scene record section 58) at the recorders; read only, off = no change");

namespace wwe2k14::q2d_tap {

namespace q = rdr_scene::q2d;

inline std::atomic<int> g_state{-1};   // -1 = the cvar not read yet, 0 off, 1 on (read once: a start-only switch)
inline bool Active() {
  const int s = g_state.load(std::memory_order_relaxed);
  if (s >= 0) return s != 0;
#if !WWE2K14_STREAM()
  const int v = 1;   // S103 PN4: the 2D rows are the host's only 2D layer in a build without the stream: armed always. (The ON build / player default: the line below, unchanged.)
#else
  const int v = (wwe2k14::IsDevBuild() && REXCVAR_GET(wwe2k14_rdr_2d_record)) ? 1 : 0;
#endif
  g_state.store(v, std::memory_order_relaxed);
  return v != 0;
}

inline q::Store& TheStore() {
  static q::Store s;
  return s;
}
inline thread_local q::Scope t_scope;
inline thread_local q::Scissor t_scissor;   // S103 SC: this thread's last SetScissorRect (op-79 record), unknown until the first one (fail closed). The enable half is read from the state object at each op-19.
inline thread_local q::TargetState t_target;     // S103 SC: this thread's colour target + viewport (SetRenderTarget / SetViewport recorders)
inline thread_local q::Saves t_saves;           // S103 PFR: this thread's op-46 saves (holder -> the target it had bound); not cleared by the frame reset
inline thread_local uint32_t t_frame = 0xFFFFFFFFu;   // S103 PFR: the guest frame this thread last ran a tap op in
inline thread_local q::Shadow t_shadow;   // S103 I3: the 2D programs' constant registers as THIS thread last set them (the glyph rows' constants)
inline thread_local rdr_scene::walkcat::Walk t_walk;   // S105 A4B: the category of the MAIN frame walk (sub_82C2FA10) this thread is inside; set by the begin hook (sub_82C3B200), reset by the end hook (sub_82C3B218): the main walk never nests (A4B_ADV E4)
inline void OnWalkBegin(uint32_t cat) { t_walk.Begin(cat); }
inline void OnWalkEnd() { t_walk.End(); }
inline q::Counters g_counters;   // process-wide atomics: a log from any thread reads them all

// The guest memory the core reads: wrestler_hook.h's ReadGuest (MMIO heap excluded, fault-safe SafeCopy; the plane and circle hooks read the same way). A failed read is a counted refusal in the core.
struct GuestMem {
  bool ReadWords(uint32_t a, uint32_t* out, uint32_t n) const {
    if (!wwe2k14::wrestlers::ReadGuest(a, out, size_t(n) * 4)) return false;
    for (uint32_t i = 0; i < n; ++i) out[i] = __builtin_bswap32(out[i]);
    return true;
  }
};

inline uint32_t Frame() { return uint32_t(wwe2k14::g_guest_frame.load(std::memory_order_relaxed)); }

// S103 MP (B): the running 2D row count of the CURRENT guest frame, for the particle node's hook (a menu's particle groups sit between 2D rows). Rows are counted by the process-wide row counter; the frame's base is taken at the first 2D
// op of the frame (TouchFrame, from the leaf scope / op-19 hooks), so RowsThisFrame() = rows recorded since. [U: the leaf, the node and the rows are on one thread; the unit and the first live census say] 0xFFFFFFFF = the tap is off.
// BHF1 B5: (frame << 32 | base's low 32 bits) in ONE atomic word, so a reader never pairs a new frame with an old base and two threads' TouchFrame cannot tear it; the row difference is taken in 32 bits.
inline std::atomic<uint64_t> g_tally{0xFFFFFFFFull << 32};
inline void TouchFrame() {
  const uint32_t f = Frame();
  uint64_t cur = g_tally.load(std::memory_order_relaxed);
  if (int32_t(f - uint32_t(cur >> 32)) <= 0) return;   // BHF1 fix: the tally only moves FORWARD (a thread still on an older frame never rewinds it)
  const uint64_t mine = (uint64_t(f) << 32) | uint32_t(g_counters.rows.load(std::memory_order_relaxed));
  g_tally.compare_exchange_strong(cur, mine, std::memory_order_relaxed);   // a lost race = another thread opened the same (or a newer) frame: its base stands
}
inline uint32_t RowsThisFrame() {
  if (!Active()) return 0xFFFFFFFFu;
  const uint64_t t = g_tally.load(std::memory_order_relaxed);
  if (uint32_t(t >> 32) != Frame()) return 0;   // no 2D op yet this frame
  return uint32_t(g_counters.rows.load(std::memory_order_relaxed)) - uint32_t(t);
}

// S103 PFR (docs\research\s103\PFR.md): every replay starts on the default target (sub_82BA7B40 each frame), so a thread's first tap op of a new guest frame resets its modelled target to the default and its scissor to unknown. Replaces the once-per-thread seed. Called under Active().
inline void EnsureFrame() { q::OnFrameBoundary(t_frame, Frame(), t_target, t_scissor, GuestMem{}, &g_counters); }

// T3: opened by the two leaf hooks (draw_origin.h) for the length of the leaf's call.
class LeafScope {
 public:
  LeafScope(const PPCContext& ctx, uint8_t kind) {
    if (!Active()) return;
    EnsureFrame();   // S103 PFR
    if (t_scope.open || ctx.r3.u32 < 0x10000u) return;
    TouchFrame();   // S103 MP
    q::OpenScope(t_scope, kind, ctx.r3.u32);
    mine_ = true;
  }
  ~LeafScope() {
    if (mine_) q::CloseScope(t_scope, TheStore(), Frame(), g_counters);
  }
  LeafScope(const LeafScope&) = delete;
  LeafScope& operator=(const LeafScope&) = delete;

 private:
  bool mine_ = false;
};

// T1: before the original op-19 recorder runs (r4 primitive, r5 vertex count, r6 stride in bytes, r7 the vertex pointer; ctx.lr = the caller's return address).
inline void OnOp19(const PPCContext& ctx, const uint8_t* base) {   // q2d-lint: census-first (the fallback census sees every op-19 site, the 2D record on or off; the 2D part below tests Active())
#if !WWE2K14_STREAM()   // S103 RBC fix: stream-free only (the scope hook is not built in the stream build)
  if (wwe2k14::thumb_scope::Active() && !t_scope.open && uint32_t(ctx.lr) - 4 == wwe2k14::thumb_scope::kRestoreSite) {   // S103 RBC / P2D: the thumbnail render's frame restore (an EDRAM restore, a 360 habit): counted, never a hole / unknown_site
    const uint64_t n = wwe2k14::thumb_scope::NoteRestore(uint32_t(ctx.lr) - 4);
    if (n <= 4 || (n & (n - 1)) == 0) REXLOG_INFO("[rdr][habit] habit_thumb_restore {}: the replay-thumbnail render's full-frame redraw (op-19 site {:08X}) dropped, an EDRAM restore the 360 needed after the 128x128 pass; {} thumbnail renders", n, uint32_t(ctx.lr) - 4, wwe2k14::thumb_scope::g_scopes.load(std::memory_order_relaxed));
    return;
  }
#endif
  fallback_census::OnOp19Site(Frame(), uint32_t(ctx.lr) - 4, t_scope.open);   // S103 L2b: an op-19 from a 2D site that makes no row = unknown_site (the fallback census; one relaxed load when the scene is not ours)
  if (Active()) EnsureFrame();   // S103 PFR: the thread's first tap op of a new guest frame
  if (!t_scope.open) {   // (a thread_local flag; Active() was tested when the scope opened)
    // a 2D draw with no scope: a hole. S103 L2a: every 2D leaf site and every [U] site of the 36-site table (quads2d_core.h kOp19Sites), not only T_RectRGradation. Off = one relaxed load.
    if (Active()) TouchFrame();   // S103 MP (MPBF 11): inside the scope-less Active() branch: the default (tap off) costs nothing more than the Active() it already paid
    if (Active() && q::IsHoleSite(uint32_t(ctx.lr) - 4)) q::OnOp19(t_scope, GuestMem{}, TheStore(), Frame(), uint32_t(ctx.lr) - 4, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, g_counters);
    return;
  }
  q::SeedTargetIfUnseen(t_target, GuestMem{}, g_counters);   // PBS: a thread with no target op yet draws into the default back buffer (boot / title / menu screens)
  q::OnOp19(t_scope, GuestMem{}, TheStore(), Frame(), uint32_t(ctx.lr) - 4, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, g_counters, &t_shadow, ctx.r13.u32, &t_scissor, &t_target);   // (S103 I3: &t_shadow; S103 L2a: r13 -> the state object's blend / alpha test; S103 SC: + the state object's scissor enable and this thread's last rect)
  if (t_scope.drawn) rdr_scene::walkcat::Mark(t_scope.cur, t_walk);   // S105 A4B: a scene-colour row recorded below the first wrestler category gets kQhPre3DCat (before the census key and the CloseScope commit)
  if (t_scope.drawn && t_scope.cur.pass == rdr_scene::kQpSceneColour) {   // PFR review 6: the FIRST scene-colour row's site, once (the host drops these rows: this says what is lost)
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) REXLOG_INFO("[q2d] first scene-colour 2D row (op 73 target): frame {}, op-19 site 0x{:08X}, row site 0x{:08X}, kind {}", Frame(), uint32_t(ctx.lr) - 4, t_scope.cur.site, unsigned(t_scope.cur.kind));
    // S103 SCS (RFF part B census): every scene-colour row counted per (row site, kind, blend code, z test, z write); the table is logged at 600, 6000 and 60000 rows (the flashes + lens flares expected: 0x82BC3EE8 rects, 0x82BC5688 sprites)
    {
      struct Cell { std::atomic<uint64_t> key{0}, n{0}; };
      static Cell cells[24];
      static std::atomic<uint64_t> total{0};
      const uint64_t key = (uint64_t(t_scope.cur.site) << 16) | (uint64_t(t_scope.cur.kind) << 8) | uint64_t(t_scope.cur.state & 0x3Fu) | (1ull << 62) | ((t_scope.cur.have & rdr_scene::kQhPre3DCat) ? (1ull << 61) : 0ull);   // S105 A4B: bit 61 = early;   // state: code (4 bits) + z test (16) + z write (32)
      for (Cell& c : cells) {
        uint64_t k = c.key.load(std::memory_order_relaxed);
        if (k == 0 && c.key.compare_exchange_strong(k, key)) k = key;
        if (k == key) { c.n.fetch_add(1, std::memory_order_relaxed); break; }
      }
      // S103 SCS fix 2: a MATRIX-placed, z-tested scene row (no capture holds one: every 0x82BC5688 draw in c2d_entrance_s15 / a44 / d02 is pixel-placed): the near plane its matrix's z column implies, forward
      // (clip z = C z + D, w = z: near = -D / C) and reversed (C = -n / (f - n): near = D / (1 - C)), rounded to a whole unit; the line tells whether the game's depth is the mirror of ours (near 10 both ways).
      struct Cell2 { std::atomic<int64_t> key{INT64_MIN}, n{0}; };
      static Cell2 cells2[8];
      const rdr_scene::Quad2D& cq = t_scope.cur;
      if (cq.use_proj != 0.0f && (cq.have & rdr_scene::kQhMatrix) && (cq.state & 16) && cq.proj[14] == 1.0f && cq.proj[15] == 0.0f && cq.proj[10] != 0.0f && cq.proj[10] != 1.0f) {
        const double nf = -double(cq.proj[11]) / double(cq.proj[10]), nr = double(cq.proj[11]) / (1.0 - double(cq.proj[10]));
        const int64_t k = (int64_t(std::llround(nf)) << 32) ^ int64_t(uint32_t(int32_t(std::llround(nr))));
        for (Cell2& c : cells2) {
          int64_t e = c.key.load(std::memory_order_relaxed);
          if (e == INT64_MIN && c.key.compare_exchange_strong(e, k)) e = k;
          if (e == k) { c.n.fetch_add(1, std::memory_order_relaxed); break; }
        }
      }
      const uint64_t t = total.fetch_add(1, std::memory_order_relaxed) + 1;
      if (t == 600 || t == 6000 || t == 60000) {
        std::string line;
        for (const Cell2& c : cells2) {
          const int64_t k = c.key.load(std::memory_order_relaxed);
          if (k == INT64_MIN) continue;
          char b[96];
          std::snprintf(b, sizeof b, " [matrix z-tested row: near fwd %d / rev %d: %llu]", int(k >> 32), int(int32_t(uint32_t(k & 0xFFFFFFFFll))), (unsigned long long)c.n.load(std::memory_order_relaxed));
          line += b;
        }
        for (const Cell& c : cells) {
          const uint64_t k = c.key.load(std::memory_order_relaxed);
          if (!k) continue;
          char b[96];
          std::snprintf(b, sizeof b, " [site 0x%08X kind %u blend %u z_test %u z_write %u early %u: %llu]", unsigned((k >> 16) & 0xFFFFFFFFu), unsigned((k >> 8) & 0xFFu), unsigned(k & 15u), unsigned((k >> 4) & 1u), unsigned((k >> 5) & 1u), unsigned((k >> 61) & 1u), (unsigned long long)c.n.load(std::memory_order_relaxed));
          line += b;
        }
        REXLOG_INFO("[q2d] scene-colour rows by site / kind / blend / z ({} rows):{}", (unsigned long long)t, line);
      }
    }
  }
  // S103 PN3: the row's picture key, on this (the drawing) thread, while the texture is alive and filled: the source reads and joins it once per key life (off = one relaxed load).
  if (t_scope.drawn && (t_scope.cur.kind == rdr_scene::kQkSprite || t_scope.cur.kind == rdr_scene::kQkGlyph) && (t_scope.cur.have & rdr_scene::kQhPlanes)) wwe2k14::picture_tap::OnRowKey(t_scope.cur.tex_key[0]);
}
// S103 SC: before the SetRenderTarget recorder (sub_82BB3978: r4 = slot, r5 = a pointer to the SLOT that holds the surface pointer, review F1) and the SetViewport recorder (sub_82BCBB68: r4 = the 6-word viewport). Read only.
// A slot-0 SetRenderTarget, op 47 (sub_82BB3FC0) and op 48 (sub_82BB3B08) all end in sub_82CABC68, which rewrites the device scissor from a default table (review F2): this thread's last op-79 rect becomes unknown.
inline void OnSetRenderTarget(const PPCContext& ctx, const uint8_t* base) {
  if (!Active()) return;
  EnsureFrame();
  if (q::OnSetRenderTarget(t_target, GuestMem{}, ctx.r4.u32, ctx.r5.u32, &g_counters, &t_saves, Frame())) q::OnTargetResetTo(t_scissor, GuestMem{}, &g_counters);
}
// S103 PFR review 5: op 58 (sub_82BB3C98: r4 = the holder; the executor releases *holder): the save under that holder is dead.
inline void OnReleaseHolder(const PPCContext& ctx, const uint8_t* base) {
  if (!Active()) return;
  q::OnReleaseHolder(t_saves, ctx.r4.u32, &g_counters);
}
// S103 PFR: op 46 GetRenderTarget (sub_82BB3A40: r4 = slot, r5 = the holder the executor stores the bound target into at REPLAY): the thread's current modelled target is saved under the holder; a later op-45 restore from it takes this, not the guest's (empty / stale) *holder.
inline void OnGetRenderTarget(const PPCContext& ctx, const uint8_t* base) {
  if (!Active()) return;
  EnsureFrame();
  q::OnGetRenderTarget(t_saves, t_target, ctx.r4.u32, ctx.r5.u32, &g_counters, Frame());
}
// S103 PFR: op 73 (sub_82BB6FC8, recorder of sub_82BA7488: binds the scene colour surface when [engine+292] == 0 and [engine+312] != 0) and op 74 (sub_82BB7078, executor sub_82BA74D8: resolves and binds the DEFAULT again, like op 47).
inline void OnSceneBegin(const PPCContext& ctx, const uint8_t* base) {
  if (!Active()) return;
  EnsureFrame();
  q::OnSceneBeginOp(t_target, t_scissor, GuestMem{}, &g_counters);
}
inline void OnSceneEnd(const PPCContext& ctx, const uint8_t* base) {
  if (!Active()) return;
  EnsureFrame();
  q::OnSceneEndOp(t_target, t_scissor, GuestMem{}, &g_counters);
}
inline void OnResetRtDs(const PPCContext& ctx, const uint8_t* base) {   // op 47, sub_82BB3FC0
  if (!Active()) return;
  EnsureFrame();
  q::OnResetRtDs(t_target, GuestMem{});
  q::OnTargetResetTo(t_scissor, GuestMem{}, &g_counters);
}
inline void OnSetDepthStencil(const PPCContext& ctx, const uint8_t* base) {   // op 48, sub_82BB3B08
  if (!Active()) return;
  EnsureFrame();
  q::OnTargetResetTo(t_scissor, GuestMem{}, &g_counters);
}
inline void OnSetViewport(const PPCContext& ctx, const uint8_t* base) {
  if (!Active()) return;
  EnsureFrame();
  q::OnSetViewport(t_target, GuestMem{}, ctx.r4.u32);
}
// S103 SC: before the op-79 recorder (sub_82C2C4B8: r3 = the record, r4 = the four ints left / top / right / bottom) runs. Read only; the recorder runs exactly as before. Not scope-bound: the scissor is device state, set between the 2D leaves.
inline void OnOp79(const PPCContext& ctx, const uint8_t* base) {
  if (!Active()) return;
  EnsureFrame();
  if (q::OnOp79(t_scissor, GuestMem{}, ctx.r4.u32)) ++g_counters.sc_rects;
  else ++g_counters.sc_rect_unreadable;
}
// T2: before the original op-21 recorder runs (r4 first register, r5 payload, r6 register count).
// S103 I3 review: the scope part only (as in I1 / I2). The thread's register file is kept by OnOp21Regs below, which the op-21 hook calls ONLY where the original recorder actually runs.
inline void OnOp21(const PPCContext& ctx, const uint8_t* base) {
  if (!t_scope.open) return;
  q::OnOp21(t_scope, GuestMem{}, ctx.r4.u32, ctx.r6.u32, ctx.r5.u32, g_counters);
}
// S103 I3 review: the thread's register file (c0 / c1 / c4 / c28, each stamped with the guest frame of its upload). Called by the op-21 hook (scene_crowd.h) immediately before each call of the ORIGINAL recorder and nowhere else: an
// upload the hook skips (the crowd record-skip, the mode-1 run path that never calls the recorder) never reached the stream's register file and must not reach the shadow. Off = one relaxed load.
inline void OnOp21Regs(const PPCContext& ctx, const uint8_t* base) {
  if (!Active()) return;
  q::OnOp21Shadow(t_shadow, GuestMem{}, ctx.r4.u32, ctx.r6.u32, ctx.r5.u32, g_counters, Frame());
}
// T2b (S103 I2): before the original op-28 recorder runs (sub_82BB1CF8: r4 first pixel-constant register, r5 payload, r6 register count). Inside a sprite scope: the hue-shift triple (c2 x 1) and the blur / gradation marker (c3 x 8).
inline void OnOp28(const PPCContext& ctx, const uint8_t* base) {
  if (!t_scope.open) return;
  q::OnOp28(t_scope, GuestMem{}, ctx.r4.u32, ctx.r6.u32, ctx.r5.u32, g_counters);
}
// T5: the movie plane flush (r3 = the display set).
inline void OnFlush(uint32_t set) {
  if (!Active()) return;
  TheStore().NoteFlush(set);
  ++g_counters.flushes;
}

// The rows of the guest frames <= `upto`, ascending by call order (the main session's wiring into the frame's record at landing 2; this piece does not call it).
inline void Drain(uint32_t upto, std::vector<rdr_scene::Quad2D>& out) { TheStore().Drain(upto, out); }
// S103 I3 review: the 2D draws seen but not recorded in the guest frames <= `upto` (per frame and site; removed). Landing 2 keeps the stream's 2D for every frame that has one (REMNANT_MAP 12.4).
inline void DrainHoles(uint32_t upto, std::vector<q::Store::Hole>& out) { TheStore().DrainHoles(upto, out); }
inline std::string StateReport();
// The counters (all threads) as a log fragment (rows, scopes, refusals by reason).
inline std::string Report() {
  const q::Counters& c = g_counters;
  const auto n = [](const std::atomic<uint64_t>& v) { return std::to_string(v.load(std::memory_order_relaxed)); };
  return "2d record: rows " + n(c.rows) + ", scopes " + n(c.scopes) + ", flushes " + n(c.flushes) + "; refused: site " + n(c.ignored_site) + ", shape " + n(c.bad_shape) + ", unreadable " + n(c.unreadable) +
         "; rows dropped by the ring (oldest frame) " + n(c.over_cap) + "; rows without all constants " + n(c.no_constants) + "; sprite scope op-28 uploads " + n(c.op28_in_scope) + ", hue-shifted rows " + n(c.hsl_rows) +
         ", blur / gradation rows " + n(c.variant_rows) + "; glyph rows " + n(c.glyph_rows) + " (register file incomplete at the op-19: " + n(c.glyph_no_shadow) + "), shadow reads " + n(c.shadow_reads) + ", glyph-M site draws counted (not recorded) " + n(c.glyph_m_site) + ", glyph registers refused as stale (another frame) " + n(c.glyph_stale) + ", holes waiting " + std::to_string(TheStore().HoleCount()) + ", variant-leaf draws held back (c4 / c2 / c3 / any op-28 in the scope) " + n(c.m_variant) +
         StateReport();
}
// S103 L2a: the state fill's census (blend code histogram: code:rows, 15 = outside R8's table).
inline std::string StateReport() {
  const q::Counters& c = g_counters;
  const auto n = [](const std::atomic<uint64_t>& v) { return std::to_string(v.load(std::memory_order_relaxed)); };
  std::string codes;
  for (int i = 0; i < 16; ++i)
    if (const uint64_t v = c.code[i].load(std::memory_order_relaxed)) codes += (codes.empty() ? "" : " ") + std::to_string(i) + ":" + std::to_string(v);
  return "; state: filled " + n(c.state_rows) + ", unreadable " + n(c.state_unreadable) + ", blend code outside " + n(c.blend_outside) + ", alpha func outside " + n(c.func_outside) + ", ref outside " + n(c.ref_outside) +
         ", scissor: on " + n(c.sc_rows) + " off " + n(c.sc_off) + " unknown (holes) " + n(c.sc_unknown) + ", target: frame " + n(c.tg_frame) + " offscreen " + n(c.tg_offscreen) + " unknown (holes) " + n(c.tg_unknown) + " (seeded from the default back buffer " + n(c.tg_seeded) + ", rows drawn under a seed " + n(c.tg_frame_seeded) + ", seed failed " + n(c.tg_seed_failed) + ", op-45 holder null " + n(c.rt_holder_null) + ", scissor resets " + n(c.sc_resets) + "), frame resets " + n(c.frame_resets) + ", op-46 saves " + n(c.saves_made) + ", op-45 restores from save " + n(c.restore_from_save) + " from memory " + n(c.restore_from_memory) + " disagree (fail closed) " + n(c.restore_disagree) + ", saves released (op 58) " + n(c.saves_released) + " expired " + n(c.saves_expired) + ", frame-start default unreadable " + n(c.frame_reset_failed) + ", scene target (op 73) binds " + n(c.scene_begins) + " unreadable " + n(c.scene_unreadable) + " rows " + n(c.tg_scene) + ", unknown by reason (never set " + n(c.tg_unknown_never) + ", holder null " + n(c.tg_unknown_holder) + ", surface unreadable " + n(c.tg_unknown_surface) + ", default unreadable " + n(c.tg_unknown_default) + ", restore disagree " + n(c.tg_unknown_disagree) + ", viewport unreadable " + n(c.tg_unknown_vp) + "), op-79 rects " + n(c.sc_rects) + " unreadable " + n(c.sc_rect_unreadable) + ", z on " + n(c.z_on) + ", codes (" + (codes.empty() ? "-" : codes) + "); unscoped hole-site draws " + n(c.hole_unscoped) + " (unknown sites " + n(c.unknown_site) + ")";
}

}  // namespace wwe2k14::q2d_tap
