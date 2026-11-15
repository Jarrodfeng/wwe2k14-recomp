// S102 lane G11 (docs\renderer\FRAME_CLASS.md): two cuts in the `frame` GPU class (the frame buffer, images 8 / 9, 1280x720: post, sprites, text, particles, the movie quad).
// No game or SDK includes. The walk (binder.h) feeds every op of the frame to `Tracker` and asks it about the draws below; every question defaults to "keep the draw".
//
// Both cuts replace a FULL-SCREEN draw that only moves pixels the frame already holds somewhere else. Measured on Bryan's entrance (rplay --frame-trace, a precise occlusion query
// per draw): the class writes 4.6 frame buffers of pixels a frame, and 3.0 of them are these draws (docs\renderer\FRAME_CLASS.md).
//
//  SELF BLIT (wwe2k14_rdr_frame_selfblit): the plain sprite draw (ps 7ABDFEFBCADB4BF9 "texture x colour" with the vertex shader AFD5268C040B9C22) that draws the whole of image X back 1:1
//      into the target T, when X is a whole copy of T made earlier in this frame and nothing has written T or X since. T already holds those exact pixels: the draw is left out.
//        rule: both shaders, vertex constants c28.x == 0 (no projection), c0.xy == 2/size, c1 == (1,1,1,1) (the tint); 1 texture (plain view, identity swizzle); no blend, write mask 0xF, no
//        depth / stencil / alpha test, one colour target; the vertices are the four corners (0,0)-(W,H) with the uv corners and the index list 0,1,2,0,2,3; the viewport is the target exactly,
//        not clip-off, the scissor covers it; X is a whole copy of T (same size, format and scaling, mip 0) that the walk really issued, and the write version of T and X has not moved since.
//  RESTORE (wwe2k14_rdr_frame_restore): the colour + depth restore (ps 9A836B68E3F8F0B0: colour = fetch constant 1, depth = constant 0's x; docs\systems\post_effects.md and
//      docs\native\PASS_GAPS_S72.md; vertex shader 1B1DE457CA90EE5F) is a full-screen shader pass that writes two images from two textures. It becomes two image copies (colour C -> target
//      colour, depth D -> target depth): the same bits (the formats are equal and the depth shader writes the sampled value), no pass, no shader depth export.
//        rule: both shaders; textures: slot 0 depth read as R in every channel, slot 1 colour read as identity (plain views); no blend, mask 0xF, depth test on with compare ALWAYS and write
//        on, no stencil / alpha test; one colour target + a depth target of its size; C and D have the target's size, format and scaling; the draw is the 360's pixel-space rectangle: clip-off
//        with the standard mapping (ndc scale 2/8192, offset -1 + 1/8192), the viewport 8192 x 8192 from 0,0, the scissor covers the target, the four vertices at (-0.5,-0.5) .. (W-0.5,H-0.5)
//        with the uv corners and the index list 0,1,2,0,2,3; no draw of the frame tests stencil against that depth target (a depth COPY also moves the stencil aspect).
// Both decline (and count why) on: isolate / velocity / persist-only / split frames, an image a CPU upload touches this frame, a thin draw, a draw or clear with no known targets, two images that
// the binder maps to one, and an image the frame's other plans write in a way the tracker was told about only through Tracker::Foreign.
#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "capture_reader.h"

namespace rdr_adapter::frame_cuts {

inline constexpr uint64_t kBlitPs = 0x7ABDFEFBCADB4BF9ull;     // "copies (texture x colour)", docs\systems\post_effects.md
inline constexpr uint64_t kBlitVs = 0xAFD5268C040B9C22ull;     // the sprite vertex shader of that draw (frame_class_census.py on Bryan: p46's draw)
inline constexpr uint64_t kRestorePs = 0x9A836B68E3F8F0B0ull;  // "the colour + depth restore", docs\systems\post_effects.md
inline constexpr uint64_t kRestoreVs = 0x1B1DE457CA90EE5Full;  // the post rectangle's vertex shader of the restore (frame_class_census.py: ops 1570 / 1594 / 1654 of Bryan frame 50)

// fview's / rplay's fallback for the switches (the live table is not running there): --frame-selfblit / --frame-restore.
inline std::atomic<bool> g_fview_blit{false};
inline std::atomic<bool> g_fview_restore{false};

// Planted faults (tools only; each must change a picture, a count or a unit-test verdict).
enum class Fault : uint8_t {
  None,
  BlitAlways,          // every draw with the sprite ps is left out, whatever it draws and wherever (the pictures must differ)
  RestoreSwap,         // the restore's colour and depth sources swapped (fview crashes / validation)
  NoVersion,           // the write-version rule is skipped (unit test)
  NoTint,              // the tint rule is skipped (unit test)
  NoRect,              // the geometry / viewport rule is skipped (unit test)
  NoStencil,           // the stencil rule is skipped (unit test)
  OpWritesOff,         // draws, clears and copies register no write (unit test: a draw into T between the copy and the blit must still decline)
  NoteUnissued,        // a copy the walk did not issue is booked as a provenance anyway (unit test)
  RestoreOtherSource,  // a LEGAL restore twin: the colour source is another colour image of the same size and format (pictures differ, fview exits 0)
  RestoreNoDepth,      // the restore copies the colour only (the depth targets are hashed by the check: they differ)
  NoProj,              // the c28.x rule is skipped (unit test)
  NoScaled,            // the scaling rule is skipped (unit test)
  NoClipOff,           // the clip-off / viewport rule of the restore is skipped (unit test)
  NoUnknownTargets,    // a draw with no known targets does not make the frame unusable (unit test)
  Count
};
inline Fault g_fault = Fault::None;
inline bool ParseFault(const char* name, Fault& out) {
  static const char* const k[] = {"none", "blit_always", "restore_swap", "no_version", "no_tint", "no_rect", "no_stencil", "op_writes_off", "note_unissued", "restore_other_source", "restore_no_depth",
                                  "no_proj", "no_scaled", "no_clip_off", "no_unknown_targets"};
  static_assert(sizeof(k) / sizeof(k[0]) == size_t(Fault::Count), "a fault needs its name");
  for (int i = 0; i < int(Fault::Count); ++i)
    if (std::strcmp(name, k[i]) == 0) return out = Fault(i), true;
  return false;
}

enum class Why : uint8_t { Taken, Unusable, State, Textures, Rect, Tint, Provenance, Stale, Format, Stencil, Shader, Alias, Count };
inline const char* WhyName(int w) {
  static const char* const k[] = {"taken", "unusable frame", "state", "textures", "rect", "tint", "no whole copy of the target", "written since", "size / format / scaling", "stencil", "shader / constants", "mapped to one image", "?"};
  return w >= 0 && w < int(Why::Count) ? k[w] : "?";
}

struct Census {
  uint64_t frames = 0, frames_unusable = 0, unknown_targets = 0;
  uint64_t blit_seen = 0, restore_seen = 0;
  uint64_t blit_why[int(Why::Count)] = {}, restore_why[int(Why::Count)] = {};
  uint64_t blit_px = 0, restore_px = 0;   // pixels the taken draws would have written (their target's area)
};

// What the rules read of one draw, as plain values (the adapter below fills it from a LoadedFrame; the unit test fills it by hand).
struct Facts {
  uint64_t ps_hash = 0, vs_hash = 0;
  bool thin = false;
  uint32_t tex_count = 0;
  uint32_t tex_image[2] = {0, 0};
  uint8_t tex_slot[2] = {0, 0};
  bool tex_plain = true;           // every texture: view_format 0, unsigned, 2D, level 0, no lod bias / exponent
  uint8_t tex_swz[2][4] = {};      // VkComponentSwizzle (3 R, 4 G, 5 B, 6 A; 0 identity): the colour source reads identity, the depth source R in every channel
  bool blend = false;
  uint8_t write_mask = 0xF;
  bool depth_test = false, depth_write = false, stencil_test = false, alpha_test = false, alpha_to_coverage = false;
  uint8_t depth_compare = 0;       // VkCompareOp
  uint32_t color_count = 0;
  uint32_t color_image = 0, depth_image = 0;
  uint32_t target_w = 0, target_h = 0, color_format = 0, depth_format = 0;   // the targets' records (0: unknown)
  bool scaled_color = false, scaled_depth = false, scaled_tex[2] = {false, false};   // kUsageScaled of the records (F3)
  uint32_t tex_w[2] = {0, 0}, tex_h[2] = {0, 0}, tex_format[2] = {0, 0};     // the textures' records
  bool rect_ok = false;            // the scissor covers the target
  bool vp_exact = false;           // the blit: the viewport is the target exactly; the restore: 0,0,8192,8192
  bool clip_off = false;           // kDynClipOff
  bool clip_std = false;           // the clip-off mapping is the standard one (ndc scale 2/8192, offset -1 + 1/8192)
  bool quad_ok = false;            // the vertices / uv / indices (and, for the blit, c0) of the full-target rectangle
  bool tint_ok = false;            // vs c1 == (1,1,1,1) (the blit only)
  bool proj_zero = false;          // vs c28.x == 0 (the blit only)
  bool same_colour = false, same_depth = false;   // the binder maps source and destination to ONE image (F8): colour pair / depth pair (the blit: texture and target)
};

// One decision.
struct Verdict {
  enum Kind : uint8_t { Keep, Blit, Restore } kind = Keep;
  Why why = Why::Taken;
  uint32_t colour_src = 0, colour_dst = 0, depth_src = 0, depth_dst = 0;   // Restore
  bool copy_depth = true;   // (the restore_no_depth plant clears it)
};

// The write versions of a frame (per image, counted per op that writes it), the whole copies the walk really issued and the walk's target state.
// The ops reach it through Op*() / CopyDone() / Foreign() in the order of the frame, whatever other plans do with them; binder.h and the unit test feed the SAME calls.
class Tracker {
 public:
  Census census;
  bool usable = false;

  void BeginFrame(bool ok) {
    ++census.frames;
    usable = ok;
    if (!ok) ++census.frames_unusable;
    ver_.clear();
    copy_.clear();
    dirty_.clear();
    stencil_depths_.clear();
    pending_.clear();
    have_targets_ = false;
    color_count_ = 0;
    depth_ = 0;
  }
  void MarkDirty(uint32_t image) { if (image) dirty_.insert(image); }          // a CPU upload into it this frame
  void MarkStencil(uint32_t depth_image) { if (depth_image) stencil_depths_.insert(depth_image); }   // a draw tests stencil against it

  // --- the walk's calls, at the TOP of each op: the previous op's writes are applied now, this op's wait for the next op, so a decision about this op sees the state before it.
  void OpSetTargets(const uint32_t* color, uint32_t count, uint32_t depth) {
    Flush();
    color_count_ = std::min<uint32_t>(count, 4);
    for (uint32_t c = 0; c < color_count_; ++c) color_[c] = color[c];
    depth_ = depth;
    have_targets_ = true;
  }
  void OpClear() { Flush(), WriteTargets(); }
  void OpDraw() { Flush(), WriteTargets(); }
  void OpCopy(uint32_t dest) {
    Flush();
    if (g_fault != Fault::OpWritesOff) Write(dest);
  }
  // Another plan writes images at this point of the walk that the capture's own ops do not say (the post group's pass, drawn at the first claimed draw, writes the targets of EVERY claimed
  // draw): F5. The write is applied at the next op like any other.
  void Foreign(uint32_t image) { Write(image); }
  // After the walk handled a copy: did it really issue builder.Copy (not an alias, not a drop), and is it a whole copy of one image into another of the same size, format and scaling?
  void CopyDone(uint32_t src, uint32_t dst, bool issued, bool whole) {
    if (!whole || (!issued && g_fault != Fault::NoteUnissued)) return;
    Flush();   // (the copy's own write to dst)
    copy_[dst] = {src, Ver(src), Ver(dst)};
  }
  void Flush() {
    for (uint32_t image : pending_) ++ver_[image];
    pending_.clear();
  }

  static bool Identity(const uint8_t* z) { return (z[0] == 0 && z[1] == 0 && z[2] == 0 && z[3] == 0) || (z[0] == 3 && z[1] == 4 && z[2] == 5 && z[3] == 6); }
  static bool RRRR(const uint8_t* z) { return z[0] == 3 && z[1] == 3 && z[2] == 3 && z[3] == 3; }

  // The rules.
  Verdict Decide(const Facts& f, bool count = true) {   // S102 R18-B1: `count` false = a lane other than G11 asks (its switches are off): the census stays G11's
    Verdict v;
    const bool is_blit = f.ps_hash == kBlitPs, is_restore = f.ps_hash == kRestorePs;
    if (!is_blit && !is_restore) return v;
    if (is_blit && g_fault == Fault::BlitAlways) { v.kind = Verdict::Blit; return v; }   // (planted: no census either)
    uint64_t* why_counts = is_blit ? census.blit_why : census.restore_why;
    if (count) (is_blit ? census.blit_seen : census.restore_seen)++;
    const auto decline = [&](Why w) { if (count) ++why_counts[int(w)]; v.kind = Verdict::Keep; v.why = w; return v; };
    if (!usable) return decline(Why::Unusable);
    if (f.thin || f.color_count != 1 || !f.color_image) return decline(Why::State);
    if (f.blend || f.write_mask != 0xF || f.alpha_test || f.alpha_to_coverage || f.stencil_test) return decline(Why::State);
    if (dirty_.count(f.color_image) || (f.depth_image && dirty_.count(f.depth_image))) return decline(Why::State);
    if (!f.target_w || !f.target_h) return decline(Why::Format);
    if (g_fault != Fault::NoRect && !f.rect_ok) return decline(Why::Rect);
    if (is_blit) {
      if (f.vs_hash != kBlitVs) return decline(Why::Shader);
      if (g_fault != Fault::NoProj && !f.proj_zero) return decline(Why::Shader);
      if (f.tex_count != 1 || !f.tex_plain || !Identity(f.tex_swz[0]) || !f.tex_image[0] || f.depth_test || f.depth_write) return decline(Why::Textures);
      if (g_fault != Fault::NoRect && (!f.quad_ok || !f.vp_exact || f.clip_off)) return decline(Why::Rect);
      if (g_fault != Fault::NoTint && !f.tint_ok) return decline(Why::Tint);
      const uint32_t x = f.tex_image[0], t = f.color_image;
      if (dirty_.count(x)) return decline(Why::State);
      if (f.tex_w[0] != f.target_w || f.tex_h[0] != f.target_h || f.tex_format[0] != f.color_format) return decline(Why::Format);
      if (g_fault != Fault::NoScaled && f.scaled_tex[0] != f.scaled_color) return decline(Why::Format);
      if (f.same_colour) return decline(Why::Alias);
      const auto c = copy_.find(x);
      if (c == copy_.end() || c->second.src != t) return decline(Why::Provenance);
      if (g_fault != Fault::NoVersion && (Ver(t) != c->second.src_ver || Ver(x) != c->second.dst_ver)) return decline(Why::Stale);
      if (count) ++census.blit_why[int(Why::Taken)], census.blit_px += uint64_t(f.target_w) * f.target_h;
      v.kind = Verdict::Blit;
      return v;
    }
    // restore: slot 0 = depth, slot 1 = colour
    if (f.vs_hash != kRestoreVs) return decline(Why::Shader);
    if (f.tex_count != 2 || !f.tex_plain || !f.depth_image) return decline(Why::Textures);
    if (!f.depth_test || !f.depth_write || f.depth_compare != 7 /* VK_COMPARE_OP_ALWAYS */) return decline(Why::State);
    int d = -1, c = -1;
    for (int i = 0; i < 2; ++i) {
      if (f.tex_slot[i] == 0) d = i;
      if (f.tex_slot[i] == 1) c = i;
    }
    if (d < 0 || c < 0 || !f.tex_image[d] || !f.tex_image[c] || !RRRR(f.tex_swz[d]) || !Identity(f.tex_swz[c])) return decline(Why::Textures);
    if (g_fault != Fault::NoClipOff && (!f.clip_off || !f.clip_std || !f.vp_exact)) return decline(Why::Rect);
    if (g_fault != Fault::NoRect && !f.quad_ok) return decline(Why::Rect);
    if (dirty_.count(f.tex_image[d]) || dirty_.count(f.tex_image[c])) return decline(Why::State);
    if (f.tex_w[c] != f.target_w || f.tex_h[c] != f.target_h || f.tex_format[c] != f.color_format) return decline(Why::Format);
    if (f.tex_w[d] != f.target_w || f.tex_h[d] != f.target_h || f.tex_format[d] != f.depth_format || !f.depth_format) return decline(Why::Format);
    if (g_fault != Fault::NoScaled && (f.scaled_tex[c] != f.scaled_color || f.scaled_tex[d] != f.scaled_depth || f.scaled_color != f.scaled_depth)) return decline(Why::Format);
    if (f.same_colour || f.same_depth) return decline(Why::Alias);
    if (g_fault != Fault::NoStencil && stencil_depths_.count(f.depth_image)) return decline(Why::Stencil);
    if (count) ++census.restore_why[int(Why::Taken)], census.restore_px += uint64_t(f.target_w) * f.target_h;
    v.kind = Verdict::Restore;
    v.colour_src = f.tex_image[c], v.colour_dst = f.color_image, v.depth_src = f.tex_image[d], v.depth_dst = f.depth_image;
    if (g_fault == Fault::RestoreSwap) std::swap(v.colour_src, v.depth_src);
    if (g_fault == Fault::RestoreNoDepth) v.copy_depth = false;
    return v;
  }

  // The census line: once at the end of a replay, and every 600 frames in the game (binder.h).
  std::string Report(bool blit_on, bool restore_on) const {
    char b[1100];
    int n = std::snprintf(b, sizeof b, "[rdr][frame_cuts] frames %llu (unusable %llu, of them with unknown targets %llu); self blit %s: seen %llu, taken %llu (%.2f Mpx), declined",
                          static_cast<unsigned long long>(census.frames), static_cast<unsigned long long>(census.frames_unusable), static_cast<unsigned long long>(census.unknown_targets), blit_on ? "on" : "off",
                          static_cast<unsigned long long>(census.blit_seen), static_cast<unsigned long long>(census.blit_why[0]), double(census.blit_px) / 1e6);
    for (int w = 1; w < int(Why::Count); ++w)
      if (census.blit_why[w] && n < int(sizeof b) - 80) n += std::snprintf(b + n, sizeof b - size_t(n), " %s %llu,", WhyName(w), static_cast<unsigned long long>(census.blit_why[w]));
    n += std::snprintf(b + n, sizeof b - size_t(n), " restore %s: seen %llu, taken %llu (%.2f Mpx), declined", restore_on ? "on" : "off", static_cast<unsigned long long>(census.restore_seen),
                       static_cast<unsigned long long>(census.restore_why[0]), double(census.restore_px) / 1e6);
    for (int w = 1; w < int(Why::Count); ++w)
      if (census.restore_why[w] && n < int(sizeof b) - 80) n += std::snprintf(b + n, sizeof b - size_t(n), " %s %llu,", WhyName(w), static_cast<unsigned long long>(census.restore_why[w]));
    if (g_fault != Fault::None && n < int(sizeof b) - 12) std::snprintf(b + n, sizeof b - size_t(n), " FAULT");
    return b;
  }

 private:
  struct CopyRec { uint32_t src = 0, src_ver = 0, dst_ver = 0; };
  uint32_t Ver(uint32_t image) const {
    const auto v = ver_.find(image);
    return v == ver_.end() ? 0 : v->second;
  }
  void Write(uint32_t image) { if (image) pending_.push_back(image); }
  // A draw or a clear writes the current targets. With no target switch seen yet the targets are unknown: whatever it wrote, the frame is unusable from here on (F6).
  void WriteTargets() {
    if (g_fault == Fault::OpWritesOff) return;
    if (!have_targets_) {
      if (g_fault != Fault::NoUnknownTargets && usable) usable = false, ++census.unknown_targets, ++census.frames_unusable;
      return;
    }
    for (uint32_t c = 0; c < color_count_; ++c) Write(color_[c]);
    Write(depth_);
  }
  std::unordered_map<uint32_t, uint32_t> ver_;
  std::unordered_map<uint32_t, CopyRec> copy_;
  std::unordered_set<uint32_t> dirty_, stencil_depths_;
  std::vector<uint32_t> pending_;
  bool have_targets_ = false;
  uint32_t color_[4] = {}, color_count_ = 0, depth_ = 0;
};

// ---- the adapter: Facts from a LoadedFrame ------------------------------------------------------------------------------------------------------------------------------------------------
// `record(id)`: an image id's ImageRecord (null: unknown). `read_geometry(offset, bytes, out)`: the draw's vertex / index bytes. `targets`: the walk's current target switch (the game's ids).
inline Facts FactsOf(const LoadedFrame& f, uint32_t draw_index, const rdr::TargetsOp& targets, const std::function<const ImageRecord*(uint32_t)>& record,
                     const std::function<bool(uint64_t, uint32_t, std::vector<uint8_t>&)>& read_geometry) {
  Facts out;
  const Draw& d = f.draws[draw_index];
  out.ps_hash = d.ps_hash, out.vs_hash = d.vs_hash;
  out.thin = (d.flags & kDrawThin) != 0;
  if (d.ps_hash != kBlitPs && d.ps_hash != kRestorePs) return out;
  const auto scaled = [](const ImageRecord* r) { return r && (r->usage & rdr::kUsageScaled) != 0; };
  out.tex_count = d.texture_count;
  out.tex_plain = d.texture_count <= 2;
  for (uint32_t k = 0; k < d.texture_count && k < 2; ++k) {
    const TextureSlot& s = f.texture_slots[d.texture_first + k];
    out.tex_image[k] = s.image, out.tex_slot[k] = s.slot;
    std::memcpy(out.tex_swz[k], s.swizzle, 4);
    out.tex_plain = out.tex_plain && s.view_format == 0 && s.sign[0] == 0 && s.sign[1] == 0 && s.sign[2] == 0 && s.sign[3] == 0 && s.dimension == 1 && s.min_level == 0 && s.max_level == 0 &&
                    s.target16_read == 0 && s.fetch_lod_bias == 0.0f && s.t21_exp == 0 && s.word3_exp == 0;
    if (const ImageRecord* r = record ? record(s.image) : nullptr) out.tex_w[k] = r->width, out.tex_h[k] = r->height, out.tex_format[k] = r->format, out.scaled_tex[k] = scaled(r);
  }
  const rdr::DrawState& st = f.fixeds[d.fixed].state;
  out.blend = st.blend[0].enable != 0;
  out.write_mask = st.blend[0].write_mask;
  out.depth_test = st.depth_test != 0, out.depth_write = st.depth_write != 0, out.stencil_test = st.stencil_test != 0, out.depth_compare = st.depth_compare;
  out.alpha_test = f.fixeds[d.fixed].alpha_test != 0, out.alpha_to_coverage = f.fixeds[d.fixed].alpha_to_coverage != 0;
  out.color_count = targets.color_count;
  out.color_image = targets.color_count ? targets.color[0].image : 0;
  out.depth_image = targets.depth.image;
  const bool mip0 = targets.color_count && targets.color[0].mip == 0 && targets.color[0].layer == 0;
  if (const ImageRecord* r = record && out.color_image && mip0 ? record(out.color_image) : nullptr) out.target_w = r->width, out.target_h = r->height, out.color_format = r->format, out.scaled_color = scaled(r);
  if (out.depth_image)
    if (const ImageRecord* r = record ? record(out.depth_image) : nullptr) {
      out.depth_format = r->format;
      out.scaled_depth = scaled(r);
      if (r->width != out.target_w || r->height != out.target_h) out.depth_format = 0;   // a depth target of another size: not a restore
    }
  const DrawDynamic& dyn = f.dynamics[d.dynamic];
  const rdr::DynamicState& dy = dyn.host;
  const float W = float(out.target_w), H = float(out.target_h);
  out.rect_ok = out.target_w && dy.scissor.x <= 0 && dy.scissor.y <= 0 && dy.scissor.width >= out.target_w && dy.scissor.height >= out.target_h;
  out.clip_off = (dyn.flags & kDynClipOff) != 0;
  const auto approx = [](float a, float b) { return std::fabs(a - b) <= 1e-5f * std::fabs(b); };
  out.clip_std = approx(dyn.ndc_scale[0], 2.0f / 8192.0f) && approx(dyn.ndc_scale[1], 2.0f / 8192.0f) && approx(dyn.ndc_offset[0], -1.0f + 1.0f / 8192.0f) && approx(dyn.ndc_offset[1], -1.0f + 1.0f / 8192.0f);
  const bool is_blit = d.ps_hash == kBlitPs;
  out.vp_exact = is_blit ? (dy.viewport[0] == 0.0f && dy.viewport[1] == 0.0f && dy.viewport[2] == W && dy.viewport[3] == H)
                         : (dy.viewport[0] == 0.0f && dy.viewport[1] == 0.0f && dy.viewport[2] == 8192.0f && dy.viewport[3] == 8192.0f);
  // the rectangle: blit 4 vertices stride 20 (x y z u v) at (0,0)-(W,H); restore 4 vertices stride 24 (x y z w, u v) at (-0.5,-0.5)-(W-0.5,H-0.5); the uv corners; the index list 0,1,2,0,2,3
  const VertexLayout* lay = d.layout != 0xFFFFFFFFu && d.layout < f.layouts.count ? &f.layouts[d.layout] : nullptr;
  const uint32_t stride = is_blit ? 20u : 24u, fmt0 = is_blit ? 106u : 109u, off1 = is_blit ? 12u : 16u;
  bool quad = lay && lay->host_stride == stride && lay->element_count == 2 && d.vertex_count == 4 && d.vertex_bytes >= 4 * stride && !(d.flags & kDrawThin);
  if (quad) {
    const VertexElement& e0 = f.elements[lay->element_first];
    const VertexElement& e1 = f.elements[lay->element_first + 1];
    quad = e0.vk_format == fmt0 && e0.host_offset == 0 && e1.vk_format == 103 /* R32G32_SFLOAT */ && e1.host_offset == off1;
  }
  if (quad) {
    std::vector<uint8_t> vb, ib;
    quad = read_geometry && read_geometry(d.vertex_offset, d.vertex_bytes, vb) && vb.size() >= 4 * stride;
    if (quad) {
      const float lo = is_blit ? 0.0f : -0.5f;
      const float px[4][2] = {{lo, lo}, {W + lo, lo}, {W + lo, H + lo}, {lo, H + lo}}, uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
      for (int i = 0; i < 4 && quad; ++i) {
        float v[6];
        std::memcpy(v, vb.data() + size_t(i) * stride, stride);
        const float* t = is_blit ? v + 3 : v + 4;
        quad = v[0] == px[i][0] && v[1] == px[i][1] && t[0] == uv[i][0] && t[1] == uv[i][1];
      }
    }
    if (quad) {
      static const uint16_t want[6] = {0, 1, 2, 0, 2, 3};
      quad = d.index_count == 6 && !d.index_32bit && d.index_bytes >= 12 && read_geometry && read_geometry(d.index_offset, d.index_bytes, ib) && ib.size() >= 12 && std::memcmp(ib.data(), want, 12) == 0;
    }
  }
  if (is_blit) {
    const uint32_t* b0 = f.constant_banks.Block(d.vs_bank, 0);   // c0..c3 of the vertex bank (16 dwords)
    const uint32_t* b7 = f.constant_banks.Block(d.vs_bank, 7);   // c28..c31
    float r0[4] = {}, r1[4] = {}, r28[4] = {};
    if (b0) std::memcpy(r0, b0, 16), std::memcpy(r1, b0 + 4, 16);
    if (b7) std::memcpy(r28, b7, 16);
    const bool scale = out.target_w && std::fabs(r0[0] - 2.0f / W) <= 2e-7f * 2.0f / W + 1e-9f && std::fabs(r0[1] - 2.0f / H) <= 2e-7f * 2.0f / H + 1e-9f;
    quad = quad && scale;
    out.tint_ok = b0 && r1[0] == 1.0f && r1[1] == 1.0f && r1[2] == 1.0f && r1[3] == 1.0f;
    out.proj_zero = b7 && r28[0] == 0.0f;
  }
  out.quad_ok = quad;
  return out;
}

}  // namespace rdr_adapter::frame_cuts
