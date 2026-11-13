// Particles, P2a (Session 101, lane 67; docs\renderer\PARTICLES_PROMOTION_MAP.md sections 3-6): a RUN of particle draws drawn as ONE draw by our own vertex stage.
//
// The game draws one particle per draw (op 19, 4 inline vertices, a triangle strip), up to 2,750 draws a frame, and between two particles of a run only the vertex
// constants c228-c231 (LocalWorld) change. This header is the contract the binder's look-ahead (binder.h, marked "S101 lane 67") works against: which draws are particle
// draws, when two draws are one run (the key), how a run's vertices are merged into one buffer, and what the census counts. Header only; no game or SDK includes (like
// crowd_hook.h); `tools\particles\runs_check.py` proves it on Check B and Bryan through rplay.
//
//   key        stream draws that are ADJACENT in the op list (draws only: a non-draw op ends the run), the same recorder call site, and byte-equal programs, fixed state,
//              dynamic state, bool / loop constants, vertex layout, texture slots (image, view, swizzle, ...; the sampler by content), PS bank and VS bank with c200.yzw and
//              c228-c231 masked (c200.x = ZOffset is KEPT: the vertex stage reads it per run; the call site is in the key too: sprite vs quad runs differ only
//              by c200.y, so this costs 112 runs on Check B, the stated choice: different sites are different shader paths). COMPARE BANKS AS BYTES: the game leaves a NaN in c200.y of the camera-facing sprites (lane 65, docs\systems\particles.md), and a float compare
//              (NaN != NaN) splits every sprite run (the plant `float_key`).
//   merged     vertex stream 0 = the run's K x 4 stream vertices, each 32 bytes of the game's vertex (as the binder would have uploaded it) followed by that particle's four
//              LocalWorld rows (64 bytes, c228-c231 of its own draw), stride 96; an index list of 6 per quad (0,1,2 and 2,1,3: the strip's triangles, same winding) as a
//              triangle list. A particle keeps its place (stream order = index order), so blending order is the stream's exactly.
//   unchanged  the run's first draw's program set, state, dynamic state, textures, constants and target (the key proves them equal across the run).
//   not merged a run of one draw; ySoftParticle (its per-particle rows are not measured: no capture has one); the third method (0x82A0ED2C, never seen); a draw whose shape
//              is not the measured one (counted `other`).
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "capture_reader.h"

namespace rdr_adapter {

// The switch (default off, PC and phone). Live: --wwe2k14_rdr_particles_ours (owner mode; needs no scene record). rplay / fview: --particles-ours.
inline bool g_particles_ours = false;

namespace particles {

// The recorder calls that submit a particle (DrawOrigin::call_site), docs\systems\particles.md "The particle draws".
inline constexpr uint32_t kSpriteSite = 0x82A0DA58u;  // camera-facing sprites, sub_82A0D0E0
inline constexpr uint32_t kQuadSite = 0x82A0E338u;    // oriented quads (their own matrix), sub_82A0DAC8
inline constexpr uint32_t kThirdSite = 0x82A0ED2Cu;   // the third method, sub_82A0E3D8: 0 draws in every capture read
// The shader pair of every measured particle draw (yParticle / base) and the soft variant's (ySoftParticle): src\renderer\shaders\fx\yParticle.fx.json, ySoftParticle.fx.json.
inline constexpr uint64_t kParticleVs = 0x5F1EF76E69256C9Cull, kParticlePs = 0x34B29856453FE506ull;
inline constexpr uint64_t kSoftVs = 0xC163CC4A57EC9044ull, kSoftPs = 0xFCA328BC237D8B19ull;

inline constexpr uint8_t kTopologyStrip = 4;        // VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP
inline constexpr uint32_t kVertexBytes = 32;        // the stream's vertex: position float3 +0, colour +12, uv0 +16, uv1 +24
inline constexpr uint32_t kQuadVertices = 4;
inline constexpr uint32_t kMergedStride = 96;       // 32 + 64
inline constexpr uint32_t kLocalWorldOffset = 32;
inline constexpr uint16_t kLocalWorldFormat = 109;  // VK_FORMAT_R32G32B32A32_SFLOAT
inline constexpr uint32_t kLocalWorldReg = 228;     // c228-c231
inline constexpr uint32_t kZOffsetReg = 200;        // c200: x is read; y holds a NaN in the sprites, z / w vary: masked
// S101 lane 132 (N5): the soft variant's merged vertex: the game's 24-byte vertex (position float3 +0, colour +12, uv +16), then LocalWorld (c228-c231) at +24 and LocalWorldView
// (c203-c206) at +88; stride 152. Measured on the CAW capture (tools\particles\soft_probe.cpp): 3 elements, 4 inline vertices, a strip, 2 textures (the sprite, the scene depth).
inline constexpr uint32_t kSoftVertexBytes = 24, kSoftMergedStride = 152, kSoftViewOffset = 88, kLocalWorldViewReg = 203;
inline constexpr uint32_t kMaxParticlesPerDraw = 16000;  // 16-bit indices: 4 x 16000 < 65536; a longer run is two merged draws

// The planted faults (--particles-fault NAME). Each must fail its exact check; all but tween_runs / float_key also move the picture (map section 5).
enum class Fault : uint8_t { None = 0, TweenRuns, FirstTexture, FloatKey, WorldFirst, DropLast, StripAsList, Reorder, ZOffsetMasked, ReadFail, SoftAsHard, SingleDropped, SoftMaskWide, ViewFirst, SoftRowsAt32, Count };  // lane 132: SoftAsHard, SingleDropped
inline const char* FaultName(Fault f) {
  static const char* names[] = {"none", "tween_runs", "first_texture", "float_key", "world_first", "drop_last", "strip_as_list", "reorder", "zoffset_masked", "read_fail", "soft_as_hard", "single_dropped", "soft_mask_wide", "view_first", "soft_rows_at_32"};
  return uint8_t(f) < uint8_t(Fault::Count) ? names[uint8_t(f)] : "?";
}
inline bool ParseFault(const std::string& name, Fault& out) {
  for (uint8_t f = 1; f < uint8_t(Fault::Count); ++f)
    if (name == FaultName(Fault(f))) return out = Fault(f), true;
  return false;
}

// What a draw is to the particle pass.
enum class Kind : uint8_t {
  NotParticle = 0,  // not drawn from a particle recorder call
  Supported,        // a measured particle draw: it can join a run
  Soft,             // ySoftParticle at a particle site but not the measured shape: stays the stream's
  SoftRun,          // S101 lane 132: a measured soft particle draw: it joins a run of its own (ySoftParticleRuns)
  Third,            // the third method's site: stays the stream's
  Other,            // a particle site but not the measured shape / shaders: stays the stream's
};

// S101 lane 132: the measured soft draw: both soft shaders, op 19, 4 inline vertices of 24 bytes as a triangle strip, the 3 fetches at +0 / +12 / +16.
inline bool SoftShape(const LoadedFrame& frame, const Draw& d, const DrawOrigin& origin) {
  if (d.vs_hash != kSoftVs || d.ps_hash != kSoftPs) return false;
  if (origin.op != 19 || !(d.flags & kDrawInline) || (d.flags & kDrawNoGeometry) || d.vertex_count != kQuadVertices || d.index_bytes != 0 ||
      d.vertex_bytes != kQuadVertices * kSoftVertexBytes || d.layout == 0xFFFFFFFFu)
    return false;
  if (d.source_prim != 6 || frame.fixeds[d.fixed].state.topology != kTopologyStrip) return false;
  const VertexLayout& layout = frame.layouts[d.layout];
  if (layout.host_stride != kSoftVertexBytes || layout.element_count != 3) return false;
  static constexpr uint16_t kOffsets[3] = {0, 12, 16};
  for (uint32_t e = 0; e < 3; ++e)
    if (frame.elements[layout.element_first + e].host_offset != kOffsets[e]) return false;
  return true;
}
inline bool IsRunKind(Kind k) { return k == Kind::Supported || k == Kind::SoftRun; }

inline Kind Classify(const LoadedFrame& frame, uint32_t draw_index) {
  const DrawOrigin* origin = frame.OriginOf(draw_index);
  if (!origin) return Kind::NotParticle;
  const uint32_t site = origin->call_site;
  if (site != kSpriteSite && site != kQuadSite && site != kThirdSite) return Kind::NotParticle;
  const Draw& d = frame.draws[draw_index];
  if (site == kThirdSite) return Kind::Third;  // S101 lane 132 (review 6): the third method stays the stream's, soft or not, as the header says
  if (d.vs_hash == kSoftVs || d.ps_hash == kSoftPs) return SoftShape(frame, d, *origin) ? Kind::SoftRun : Kind::Soft;
  if (d.vs_hash != kParticleVs || d.ps_hash != kParticlePs) return Kind::Other;
  if (origin->op != 19 || !(d.flags & kDrawInline) || (d.flags & kDrawNoGeometry) || d.vertex_count != kQuadVertices || d.index_bytes != 0 ||
      d.vertex_bytes != kQuadVertices * kVertexBytes || d.layout == 0xFFFFFFFFu)
    return Kind::Other;
  // The measured shape is a triangle STRIP (source prim 6, Vulkan topology 4): a fan or a list of 4 vertices would be indexed wrongly by the merged strip indices.
  if (d.source_prim != 6 || frame.fixeds[d.fixed].state.topology != kTopologyStrip) return Kind::Other;
  const VertexLayout& layout = frame.layouts[d.layout];
  if (layout.host_stride != kVertexBytes || layout.element_count < 4) return Kind::Other;
  // The four fetches sit where the measured vertex has them (position +0, colour +12, uv0 +16, uv1 +24): the merged vertex keeps the first 32 bytes whole, and the
  // LocalWorld rows start at +32, so an element anywhere else (or wider) could overlap them.
  static constexpr uint16_t kOffsets[4] = {0, 12, 16, 24};
  for (uint32_t e = 0; e < 4; ++e)
    if (frame.elements[layout.element_first + e].host_offset != kOffsets[e]) return Kind::Other;
  return Kind::Supported;
}

// ---- the key --------------------------------------------------------------------------------------------------------------------------------

inline bool SameBytes(const void* a, const void* b, size_t n) { return a == b || std::memcmp(a, b, n) == 0; }

// The VS bank outside what a run may differ in: c200.yzw and c228-c231 (dwords 801-803 and 912-927). `floats`: the planted float compare of c200.xy.
inline bool SameVsBank(const ConstantBank& a, const ConstantBank& b, Fault fault, bool soft = false) {
  const bool floats = fault == Fault::FloatKey;
  if (&a == &b) return true;
  const uint32_t* x = a.dwords;
  const uint32_t* y = b.dwords;
  constexpr uint32_t z = kZOffsetReg * 4, w = kLocalWorldReg * 4;
  if (std::memcmp(x, y, z * 4) != 0) return false;                               // c0-c199
  if (floats) {
    float fx[2], fy[2];
    std::memcpy(fx, x + z, 8), std::memcpy(fy, y + z, 8);
    if (!(fx[0] == fy[0] && fx[1] == fy[1])) return false;                       // (NaN != NaN: the plant)
  } else if (fault != Fault::ZOffsetMasked && std::memcmp(x + z, y + z, 4) != 0) {  // c200.x = ZOffset, read by the vertex stage and per run: KEPT in the key (the plant masks it)
    return false;
  }
  if (soft) {  // S101 lane 132: the soft variant also takes c203-c206 (LocalWorldView) per particle
    constexpr uint32_t v = kLocalWorldViewReg * 4;
    if (fault != Fault::SoftMaskWide) {  // (planted: the whole of c201-c227 masked, not only c203-c206: merges runs whose UseNormal / LightVector / projection differ)
      if (std::memcmp(x + z + 4, y + z + 4, (v - z - 4) * 4) != 0) return false;   // c201-c202
      if (std::memcmp(x + v + 16, y + v + 16, (w - v - 16) * 4) != 0) return false;  // c207-c227
    }
  } else if (std::memcmp(x + z + 4, y + z + 4, (w - z - 4) * 4) != 0) {
    return false;                                                                // c201-c227
  }
  return std::memcmp(x + w + 16, y + w + 16, (1024 - w - 16) * 4) == 0;          // c232-c255
}

// 0 same; 1 the image differs; 2 the sampler content differs; 3 another part of the slot (S101 lane 67 live follow-up: the census names which).
inline int SlotDiff(const LoadedFrame& frame, const TextureSlot& a, const TextureSlot& b) {
  if (a.image != b.image) return 1;
  if (!(a.sampler == b.sampler || std::memcmp(&frame.samplers[a.sampler], &frame.samplers[b.sampler], sizeof(rdr::SamplerDesc)) == 0)) return 2;
  return 0;
}
inline bool SameSlot(const LoadedFrame& frame, const TextureSlot& a, const TextureSlot& b) {
  if (a.image != b.image || a.slot != b.slot || a.view_format != b.view_format || a.dimension != b.dimension || a.min_level != b.min_level || a.max_level != b.max_level ||
      a.target16_read != b.target16_read || a.fetch_lod_bias != b.fetch_lod_bias || a.t21_exp != b.t21_exp || a.word3_exp != b.word3_exp)
    return false;
  if (std::memcmp(a.swizzle, b.swizzle, sizeof(a.swizzle)) != 0 || std::memcmp(a.sign, b.sign, sizeof(a.sign)) != 0) return false;
  return a.sampler == b.sampler || std::memcmp(&frame.samplers[a.sampler], &frame.samplers[b.sampler], sizeof(rdr::SamplerDesc)) == 0;
}

// Why two draws are not one run (the census breakdown of "key differs", S101 lane 67 live follow-up).
enum class Why : uint8_t { Same = 0, Site, Hashes, Fixed, Dynamic, BoolLoop, Layout, Textures, PsBank, VsBank, TexImage, TexSampler, TexOther, Count };  // (Textures = a slot count; the Tex* are which part of a slot)
inline const char* WhyName(Why w) {
  static const char* names[] = {"same", "site", "hashes", "fixed", "dynamic", "bool_loop", "layout", "textures", "ps_bank", "vs_bank", "tex_image", "tex_sampler", "tex_other"};
  return uint8_t(w) < uint8_t(Why::Count) ? names[uint8_t(w)] : "?";
}
// The first VS register (c0-c255) whose dwords differ outside the masked ones, or -1.
inline int FirstVsDiff(const ConstantBank& a, const ConstantBank& b, bool soft = false) {
  for (uint32_t d = 0; d < 1024; ++d) {
    const uint32_t reg = d / 4;
    if (reg == kZOffsetReg && d % 4 != 0) continue;
    if (reg >= kLocalWorldReg && reg < kLocalWorldReg + 4) continue;
    if (soft && reg >= kLocalWorldViewReg && reg < kLocalWorldViewReg + 4) continue;
    if (a.dwords[d] != b.dwords[d]) return int(reg);
  }
  return -1;
}

// Are draws a and b (both Kind::Supported) one run? Everything but the particle's own LocalWorld rows (and the junk lanes of c200) byte-equal.
inline bool SameRun(const LoadedFrame& frame, uint32_t a, uint32_t b, Fault fault, Why* why = nullptr, int* reg = nullptr) {
  Why dummy;
  if (!why) why = &dummy;
  *why = Why::Same;
  const Draw& da = frame.draws[a];
  const Draw& db = frame.draws[b];
  if (frame.origins[a].call_site != frame.origins[b].call_site) return *why = Why::Site, false;
  if (da.vs_hash != db.vs_hash || da.ps_hash != db.ps_hash || da.vs_patched_hash != db.vs_patched_hash) return *why = Why::Hashes, false;
  if (da.fixed != db.fixed && std::memcmp(&frame.fixeds[da.fixed], &frame.fixeds[db.fixed], sizeof(DrawFixed)) != 0) return *why = Why::Fixed, false;
  if (da.dynamic != db.dynamic && std::memcmp(&frame.dynamics[da.dynamic], &frame.dynamics[db.dynamic], sizeof(DrawDynamic)) != 0) return *why = Why::Dynamic, false;
  if (da.bool_loop != db.bool_loop && std::memcmp(&frame.bool_loops[da.bool_loop], &frame.bool_loops[db.bool_loop], sizeof(BoolLoop)) != 0) return *why = Why::BoolLoop, false;
  if (da.layout != db.layout) {
    const VertexLayout& la = frame.layouts[da.layout];
    const VertexLayout& lb = frame.layouts[db.layout];
    if (la.host_stride != lb.host_stride || la.element_count != lb.element_count) return *why = Why::Layout, false;
    if (std::memcmp(&frame.elements[la.element_first], &frame.elements[lb.element_first], la.element_count * sizeof(VertexElement)) != 0) return *why = Why::Layout, false;
  }
  if (da.texture_count != db.texture_count) return *why = Why::Textures, false;
  for (uint32_t t = 0; t < da.texture_count; ++t)
    if (!SameSlot(frame, frame.texture_slots[da.texture_first + t], frame.texture_slots[db.texture_first + t])) {
      const int d = SlotDiff(frame, frame.texture_slots[da.texture_first + t], frame.texture_slots[db.texture_first + t]);
      return *why = d == 1 ? Why::TexImage : d == 2 ? Why::TexSampler : Why::TexOther, false;
    }
  if (da.ps_bank != db.ps_bank && std::memcmp(&frame.constant_banks[da.ps_bank], &frame.constant_banks[db.ps_bank], sizeof(ConstantBank)) != 0) return *why = Why::PsBank, false;
  if (!SameVsBank(frame.constant_banks[da.vs_bank], frame.constant_banks[db.vs_bank], fault, da.vs_hash == kSoftVs)) {
    *why = Why::VsBank;
    if (reg) *reg = FirstVsDiff(frame.constant_banks[da.vs_bank], frame.constant_banks[db.vs_bank], da.vs_hash == kSoftVs);
    return false;
  }
  return true;
}

// ---- the census -----------------------------------------------------------------------------------------------------------------------------

// S101 lane 132: how a run ended (its first non-member): the census names it for runs of one.
enum class EndWhy : uint8_t { FrameEnd = 0, NonDrawOp, OtherDraw, KeySite, KeyHashes, KeyState, KeyTexImage, KeyTexOther, KeyPsBank, KeyVsBank, Count };
inline const char* EndWhyName(EndWhy w) {
  static const char* names[] = {"frame_end", "non_draw_op", "other_draw", "key_site", "key_hashes", "key_state", "key_tex_image", "key_tex_other", "key_ps_bank", "key_vs_bank"};
  return uint8_t(w) < uint8_t(EndWhy::Count) ? names[uint8_t(w)] : "?";
}
inline EndWhy EndWhyOf(Why w) {
  switch (w) {
    case Why::Site: return EndWhy::KeySite;
    case Why::Hashes: return EndWhy::KeyHashes;
    case Why::TexImage: return EndWhy::KeyTexImage;
    case Why::TexSampler: case Why::TexOther: case Why::Textures: return EndWhy::KeyTexOther;
    case Why::PsBank: return EndWhy::KeyPsBank;
    case Why::VsBank: return EndWhy::KeyVsBank;
    default: return EndWhy::KeyState;
  }
}

// Draws are counted in draws; runs in runs. Invariant (the check reads it): draws == merged_draws + lone + soft + third + other.
struct Census {
  uint64_t frames = 0;        // bound frames that held at least one particle-site draw
  uint64_t draws = 0;         // particle-site draws seen
  uint64_t runs = 0;          // runs found among the supported draws (a lone draw is a run of one)
  uint64_t merged = 0;        // merged draw calls issued (a run of two or more; a run over kMaxParticlesPerDraw is two)
  uint64_t merged_draws = 0;  // stream draws folded into merged calls
  uint64_t particles_drawn = 0;  // particles the merged calls draw (must equal merged_draws: the plant drop_last)
  uint64_t lone = 0;          // runs of one draw: the stream's draw stays
  uint64_t soft = 0, third = 0, other = 0;  // draws kept: ySoftParticle / the third method / not the measured shape (or not drawable as a run: a skipped target, hidden HUD)
  uint64_t op_between = 0;    // runs that would have continued but for a non-draw op between two draws
  uint64_t why[uint8_t(Why::Count)] = {};  // key differs, by the first field that differs (Why)
  uint32_t vs_reg[256] = {};              // ... for vs_bank, by the first differing register
  uint64_t key_differs = 0;   // runs that ended at a particle draw whose key differs
  uint64_t scans = 0;         // FindRun calls (must equal runs: a run is scanned once, merged or not)
  uint64_t blend_keyed = 0;   // merged draws that carry a blend key (must be 0: the plant tween_runs)
  double ms = 0;              // time in the look-ahead and the packer
  // S101 lane 132 (N5): a run of one is drawn by the merged path too (`single`, a part of `merged`); `soft_drawn` = soft particles folded into merged calls; `single_end[]` = what ended each
  // run of one (EndWhy), the reasons table. `lone` now counts only runs of one that stayed the stream's (a failed read, a skipped target).
  uint64_t single = 0, soft_drawn = 0;
  uint64_t single_end[uint8_t(EndWhy::Count)] = {};
  EndWhy last_end = EndWhy::FrameEnd;  // set by FindRun
  uint64_t Kept() const { return lone + soft + third + other; }
};

inline std::string CensusLine(const Census& c) {
  char line[520];
  std::snprintf(line, sizeof line,
                "[rdr][particles] frames %llu, draws %llu, runs %llu (merged draws %llu, folding %llu, particles %llu), kept %llu (lone %llu, op between %llu, soft %llu, third method %llu, key differs %llu, other %llu), scans %llu, blend keyed %llu, ms %.2f",
                (unsigned long long)c.frames, (unsigned long long)c.draws, (unsigned long long)c.runs, (unsigned long long)c.merged, (unsigned long long)c.merged_draws, (unsigned long long)c.particles_drawn,
                (unsigned long long)c.Kept(), (unsigned long long)c.lone, (unsigned long long)c.op_between, (unsigned long long)c.soft, (unsigned long long)c.third,
                (unsigned long long)c.key_differs, (unsigned long long)c.other, (unsigned long long)c.scans, (unsigned long long)c.blend_keyed, c.ms);
  return line;
}

// The second census line: why runs ended at a particle draw whose key differs, by the first field that differs, and (vs_bank) the top registers.
inline std::string BreakdownLine(const Census& c) {
  std::string line = "[rdr][particles] key differs by field:";
  for (uint8_t w = 1; w < uint8_t(Why::Count); ++w) line += std::string(" ") + WhyName(Why(w)) + " " + std::to_string((unsigned long long)c.why[w]) + ",";
  line += " vs_bank registers:";
  uint32_t regs[256];
  std::memcpy(regs, c.vs_reg, sizeof regs);
  for (int k = 0; k < 5; ++k) {
    int best = -1;
    for (int r = 0; r < 256; ++r)
      if (regs[r] && (best < 0 || regs[r] > regs[best])) best = r;
    if (best < 0) break;
    line += " c" + std::to_string(best) + "=" + std::to_string(regs[best]);
    regs[best] = 0;
  }
  return line;
}

// S101 lane 132 (N5): the third census line: runs of one drawn by ours (and by what each run ended), soft particles folded, and what is still the stream's.
inline std::string ReasonsLine(const Census& c) {
  std::string line = "[rdr][particles] runs of one " + std::to_string((unsigned long long)c.single) + " (ended by:";
  for (uint8_t w = 0; w < uint8_t(EndWhy::Count); ++w) line += std::string(" ") + EndWhyName(EndWhy(w)) + " " + std::to_string((unsigned long long)c.single_end[w]) + ",";
  line += "), soft drawn " + std::to_string((unsigned long long)c.soft_drawn) + ", kept by the stream: lone " + std::to_string((unsigned long long)c.lone) + ", soft (shape) " +
          std::to_string((unsigned long long)c.soft) + ", third " + std::to_string((unsigned long long)c.third) + ", other " + std::to_string((unsigned long long)c.other);
  return line;
}

// ---- the look-ahead -------------------------------------------------------------------------------------------------------------------------

// The run that starts at op `first_op` (a draw op whose draw is Kind::Supported): the op indices of its members, in order, first included. `max_non_draw`: how far past
// non-draw ops the look-ahead reads to count a would-be continuation (census only; the run never crosses them). The reorder plant also takes later same-key draws of the
// contiguous particle block, over draws whose key differs.
inline void FindRun(const LoadedFrame& frame, uint32_t first_op, Fault fault, Census& census, std::vector<uint32_t>& members) {
  members.clear();
  members.push_back(first_op);
  census.last_end = EndWhy::FrameEnd;
  const uint32_t first_draw = frame.ops[first_op].index;
  const uint32_t op_count = frame.ops.count;
  for (uint32_t j = first_op + 1; j < op_count && members.size() < kMaxParticlesPerDraw; ++j) {
    const Op& op = frame.ops[j];
    if (op.kind != OpKind::Draw) {
      census.last_end = EndWhy::NonDrawOp;
      // A non-draw op ends the run. Count it when the next draw would have joined (so the census shows what the stream's structure cost us).
      for (uint32_t k = j + 1; k < op_count && k < j + 16; ++k) {
        if (frame.ops[k].kind != OpKind::Draw) continue;
        if (IsRunKind(Classify(frame, frame.ops[k].index)) && SameRun(frame, first_draw, frame.ops[k].index, fault)) ++census.op_between;
        break;
      }
      return;
    }
    if (!IsRunKind(Classify(frame, op.index))) { census.last_end = EndWhy::OtherDraw; return; }  // another draw: the run ends (nothing to count)
    Why why = Why::Same;
    int reg = -1;
    if (SameRun(frame, first_draw, op.index, fault, &why, &reg)) {
      members.push_back(j);
    } else if (fault == Fault::Reorder) {
      continue;  // planted: look past a draw of another key for more of this one's
    } else {
      ++census.key_differs;
      census.last_end = EndWhyOf(why);
      ++census.why[uint8_t(why)];
      if (why == Why::VsBank && reg >= 0 && reg < 256) ++census.vs_reg[reg];
      return;
    }
  }
}

// ---- the merged buffers ---------------------------------------------------------------------------------------------------------------------

struct Packed {
  std::vector<uint8_t> vertices;   // particles x 4 x kMergedStride
  std::vector<uint16_t> indices;   // particles x 6
  uint32_t particles = 0;
};

// `read_draw(draw, bytes)`: the draw's 128 vertex bytes exactly as the binder would upload them (the binder's geometry read, 16-bit-pair fix included).
using ReadDrawVertices = std::function<bool(const Draw& draw, std::vector<uint8_t>& bytes)>;

inline bool Pack(const LoadedFrame& frame, const std::vector<uint32_t>& members, Fault fault, const ReadDrawVertices& read_draw, Packed& out, std::vector<uint8_t>& scratch, bool soft = false) {
  if (fault == Fault::ReadFail) return false;  // planted: a failed geometry read (the live failure that leaves a run on the stream)
  uint32_t count = uint32_t(members.size());
  if (fault == Fault::DropLast && count > 1) --count;  // planted: the last particle of the run left out
  out.particles = count;
  const uint32_t vbytes = soft ? kSoftVertexBytes : kVertexBytes, stride = soft ? kSoftMergedStride : kMergedStride;  // S101 lane 132
  out.vertices.resize(size_t(count) * kQuadVertices * stride);
  out.indices.resize(size_t(count) * 6);
  const Draw& first = frame.draws[frame.ops[members[0]].index];
  for (uint32_t p = 0; p < count; ++p) {
    const Draw& d = frame.draws[frame.ops[members[p]].index];
    if (!read_draw(d, scratch) || scratch.size() < kQuadVertices * vbytes) return false;
    const uint32_t* bank = frame.constant_banks[(fault == Fault::WorldFirst ? first : d).vs_bank].dwords;  // planted: every particle takes the first one's rows
    uint8_t* at = out.vertices.data() + size_t(p) * kQuadVertices * stride;
    for (uint32_t v = 0; v < kQuadVertices; ++v) {
      std::memcpy(at + v * stride, scratch.data() + v * vbytes, vbytes);
      std::memcpy(at + v * stride + vbytes, bank + kLocalWorldReg * 4, 64);
      if (soft) {  // S101 lane 132: LocalWorldView (c203-c206); plants: every member takes the first one's rows / the rows at the hard path's offset 32 (over LocalWorld)
        const uint32_t* view = frame.constant_banks[(fault == Fault::ViewFirst ? first : d).vs_bank].dwords;
        std::memcpy(at + v * stride + (fault == Fault::SoftRowsAt32 ? kLocalWorldOffset : kSoftViewOffset), view + kLocalWorldViewReg * 4, 64);
      }
    }
    uint16_t* idx = out.indices.data() + size_t(p) * 6;
    const uint16_t b = uint16_t(p * kQuadVertices);
    if (fault == Fault::StripAsList) {  // planted: the strip read as a fan: (0,1,2) and (0,2,3)
      idx[0] = b, idx[1] = uint16_t(b + 1), idx[2] = uint16_t(b + 2), idx[3] = b, idx[4] = uint16_t(b + 2), idx[5] = uint16_t(b + 3);
    } else {  // the strip's triangles, as a list: (0,1,2) and (2,1,3)
      idx[0] = b, idx[1] = uint16_t(b + 1), idx[2] = uint16_t(b + 2), idx[3] = uint16_t(b + 2), idx[4] = uint16_t(b + 1), idx[5] = uint16_t(b + 3);
    }
  }
  return true;
}

}  // namespace particles
}  // namespace rdr_adapter
