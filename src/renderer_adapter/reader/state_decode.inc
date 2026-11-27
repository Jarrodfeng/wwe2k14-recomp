
// ---- draw state -----------------------------------------------------------------------------------------------------------------------
inline uint8_t VkBlendFactor(uint32_t f) {
  static constexpr uint8_t map[17] = {0, 1, 0, 0, 2, 3, 6, 7, 4, 5, 8, 9, 10, 11, 12, 13, 14};  // B3 -> VkBlendFactor
  if (f > 16 || f == 2 || f == 3) Note("blend_factor_unknown", f);
  return f <= 16 ? map[f] : 1;
}
inline uint8_t VkBlendOp(uint32_t op) {
  static constexpr uint8_t map[5] = {0, 1, 3, 4, 2};  // B4 -> VkBlendOp
  if (op > 4) Note("blend_op_unknown", op);
  return op <= 4 ? map[op] : 0;
}

inline rdr::BlendState DecodeBlend(uint32_t control, uint32_t mask) {
  rdr::BlendState b{};
  b.src_color = VkBlendFactor(control & 0x1F);
  b.dst_color = VkBlendFactor((control >> 8) & 0x1F);
  b.color_op = VkBlendOp((control >> 5) & 7);
  b.src_alpha = VkBlendFactor((control >> 16) & 0x1F);
  b.dst_alpha = VkBlendFactor((control >> 24) & 0x1F);
  b.alpha_op = VkBlendOp((control >> 21) & 7);
  b.enable = control != 0x00010001u;  // B2
  b.write_mask = uint8_t(mask & 0xF);  // O12: the nibble's bits are Vulkan's
  return b;
}

inline uint8_t TopologyOf(Prim p, bool& polygonal) {
  polygonal = true;
  switch (p) {
    case Prim::TriList: case Prim::TriW: return kTopoTriList;
    case Prim::TriStrip: return kTopoTriStrip;
    case Prim::TriFan: case Prim::Polygon: return kTopoTriList;  // converted to a list with the geometry (P2)
    case Prim::Quad: case Prim::QuadStrip: return kTopoTriList;
    case Prim::Rect: polygonal = false; return kTopoTriList;
    case Prim::Line: polygonal = false; return kTopoLineList;
    case Prim::LineStrip: case Prim::LineLoop: polygonal = false; return kTopoLineStrip;
    default: polygonal = false; return kTopoTriList;
  }
}

inline void DecodeFixed(const uint8_t* base, uint32_t dev, Prim prim, bool has_ps, uint32_t depth_surface, cap::DrawFixed& f) {
  std::memset(&f.state, 0, sizeof(f.state));
  bool polygonal = true;
  f.state.topology = TopologyOf(prim, polygonal);
  const uint32_t sc = db::Ld32(base, dev + kDevScMode);
  if (polygonal) {
    f.state.cull = uint8_t((sc & 1 ? 1 : 0) | (sc & 2 ? 2 : 0));  // R3, R4: VK_CULL_MODE_FRONT / BACK
    f.state.front_face = (sc >> 2) & 1;                           // R5: bit set = clockwise
  }
  if (((sc >> 3) & 3) == 1) Note("wireframe_or_dual_polygon_mode");  // R10
  const bool restart_guest = (sc >> 21) & 1;
  const bool strip = f.state.topology == kTopoTriStrip || f.state.topology == kTopoLineStrip;
  f.state.primitive_restart = restart_guest && strip;  // P10
  const uint32_t clip = db::Ld32(base, dev + kDevClipCntl);
  f.clip_off = (clip >> 16) & 1;
  f.state.depth_clamp = f.clip_off;
  if (clip & 0x3F) Note("user_clip_planes");  // C21

  uint32_t dc = db::Ld32(base, dev + kDevDepthControl);
  if (!depth_surface) dc = 0;  // the setters' gating; a null depth surface can come with a stale word
  uint32_t z_enable = (dc >> 1) & 1, z_write = (dc >> 2) & 1, z_func = (dc >> 4) & 7;
  if (z_enable && !z_write && z_func == 7) z_enable = 0;  // D2
  f.state.depth_test = uint8_t(z_enable);
  f.state.depth_write = uint8_t(z_enable && z_write);  // D3
  f.state.depth_compare = uint8_t(z_enable ? z_func : 7);
  const uint32_t stencil_on = dc & 1;
  f.state.stencil_test = uint8_t(stencil_on);
  static constexpr uint8_t kStencilOp[8] = {0, 1, 2, 3, 4, 5, 6, 7};  // D4
  auto face = [&](uint32_t shift, rdr::StencilFace& out) {
    out.compare = uint8_t((dc >> shift) & 7);
    out.fail = kStencilOp[(dc >> (shift + 3)) & 7];
    out.pass = kStencilOp[(dc >> (shift + 6)) & 7];
    out.depth_fail = kStencilOp[(dc >> (shift + 9)) & 7];
  };
  if (stencil_on) {
    face(8, f.state.front);
    if (((dc >> 7) & 1) && polygonal) face(20, f.state.back);  // D14
    else f.state.back = f.state.front;
    Note("stencil_draw");  // D16: rare; the census shows how rare
  }

  uint32_t mask = db::Ld32(base, dev + kDevColorMask);
  if (!has_ps) mask = 0;  // D12
  f.state.blend[0] = DecodeBlend(db::Ld32(base, dev + kDevBlend0), mask & 0xF);
  for (uint32_t i = 1; i < 4; ++i) f.state.blend[i] = DecodeBlend(db::Ld32(base, dev + kDevBlend1 + 4 * (i - 1)), (mask >> (4 * i)) & 0xF);

  // R11 (polygon offset): enabled if the face's flag is set, and the state's scale or offset is non-zero; the numbers are read by DecodeDynamic.
  if (polygonal) f.state.depth_bias_enable = ((sc >> 11) & 1) || ((sc >> 12) & 1);
  else f.state.depth_bias_enable = (sc >> 13) & 1;
  if (f.state.depth_bias_enable) Note("polygon_offset_enabled");  // R13

  const uint32_t cc = db::Ld32(base, dev + kDevColorControl);
  f.alpha_test = (cc >> 3) & 1;
  f.alpha_func = uint8_t(f.alpha_test ? (cc & 7) : 7);
  f.alpha_to_coverage = (cc >> 4) & 1;
  if (f.alpha_to_coverage) Note("alpha_to_coverage");  // A8
  f.alpha_ref = F(db::Ld32(base, dev + kDevAlphaRef));
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t surf = db::Ld32(base, dev + kDevRt0 + 4 * i);
    if (!surf) continue;
    f.exp_bias[i] = int8_t(int32_t(db::Ld32(base, surf + 28) << 6) >> 26);
    if (f.exp_bias[i]) Note("color_exp_bias_nonzero");  // O10
  }
}

// Settings parity S10b: a scene draw moved by this frame's jitter (render pixels -> its viewport's NDC). A copy of the dynamic entry.
// lane 185: the window ends at the post chain's last pass (jitter_rule.h), not at the first depth copy; `vs_sdk_hash` finds that pass.
inline void ApplyJitter(const uint8_t* base, uint32_t dev, Prim prim, cap::Draw& d, uint64_t vs_sdk_hash, bool full_view) {
  cap::jitter::Window window{g_jitter.hud, g_jitter.legacy, g_jitter.armed};
  FrameTables& t = g_frame;
  const cap::DrawDynamic from = t.dynamics[d.dynamic];
  cap::jitter::DrawFacts facts;
  facts.rect = prim == Prim::Rect, facts.vte_off = !(db::Ld32(base, dev + kDevVteCntl) & 1);
  facts.clip_off = from.flags & cap::kDynClipOff, facts.empty_viewport = from.flags & cap::kDynEmptyViewport;
  facts.vw = from.host.viewport[2], facts.vh = from.host.viewport[3], facts.vs_hash = vs_sdk_hash, facts.full_view = full_view;
  const bool moves = cap::jitter::Moves(window, g_jitter.scene_done, facts, cap::jitter::kDefaultMarker);  // (the binder ends DLSS's colour copy at the same hard-coded hash, binder.h kPostChainLastVs; a static_assert there keeps them equal)
  g_jitter.hud = window.hud;
  if (!moves) return;
  const float vw = facts.vw, vh = facts.vh;
  float j[2];
  rdr::DlssJitter(g_jitter.phase, g_jitter.phases, j);
  cap::DrawDynamic copy = from;
  copy.ndc_offset[0] += 2.0f * j[0] / (vw * g_jitter.scale);
  copy.ndc_offset[1] += 2.0f * j[1] / (vh * g_jitter.scale);
  d.dynamic = Dedupe(t.dynamics, t.dynamic_map, copy, reinterpret_cast<const uint8_t*>(&copy), sizeof(copy));
}

inline void DecodeDynamic(const uint8_t* base, uint32_t dev, const PixelShaderInfo* ps, cap::DrawDynamic& d) {
  std::memset(&d.host, 0, sizeof(d.host));
  const uint32_t vte = db::Ld32(base, dev + kDevVteCntl);
  const uint32_t clip = db::Ld32(base, dev + kDevClipCntl);
  const uint32_t sc = db::Ld32(base, dev + kDevScMode);
  const uint32_t vport[6] = {db::Ld32(base, dev + kDevVport), db::Ld32(base, dev + kDevVport + 4), db::Ld32(base, dev + kDevVport + 8),
                             db::Ld32(base, dev + kDevVport + 12), db::Ld32(base, dev + kDevVport + 16), db::Ld32(base, dev + kDevVport + 20)};
  const bool clip_off = (clip >> 16) & 1;
  const bool writes_depth = ps && ps->writes_depth;
  d.vtx_xy_fmt = (vte >> 8) & 1;
  d.vtx_z_fmt = (vte >> 9) & 1;
  d.vtx_w0_fmt = (vte >> 10) & 1;

  float scale[3] = {(vte & 1) ? F(vport[0]) : 1.0f, (vte & 4) ? F(vport[2]) : 1.0f, (vte & 16) ? F(vport[4]) : 1.0f};  // C4
  float offset[3] = {(vte & 2) ? F(vport[1]) : 0.0f, (vte & 8) ? F(vport[3]) : 0.0f, (vte & 32) ? F(vport[5]) : 0.0f};
  float add = 0;
  if (!(db::Ld32(base, dev + kDevVtxCntl) & 1)) {  // C6
    add = 0.5f;
    d.flags |= cap::kDynHalfPixel;
  }
  // C7: the window offset is not in the device shadow; it is 0 unless the tiling variants are on (counted in OnDrawCommon).
  (void)sc;
  for (uint32_t i = 0; i < 2; ++i) {
    d.guest_scale[i] = scale[i];
    d.guest_offset[i] = offset[i] + add;
  }
  d.guest_scale[2] = scale[2];
  d.guest_offset[2] = offset[2];

  constexpr float kExtent = 8192.0f;  // min(8192, maxViewport / N) at N = 1, the reference; the renderer rebuilds it for its N
  float z_min, z_max;
  if (clip_off) {  // C14
    d.flags |= cap::kDynClipOff;
    d.host.viewport[0] = 0;
    d.host.viewport[1] = 0;
    d.host.viewport[2] = kExtent;
    d.host.viewport[3] = kExtent;
    for (uint32_t i = 0; i < 2; ++i) {
      const float to_ndc = 2.0f / kExtent;
      d.ndc_scale[i] = scale[i] * to_ndc;
      d.ndc_offset[i] = (offset[i] - kExtent * 0.5f + add) * to_ndc;
    }
    z_min = 0;
    z_max = 1;
    d.ndc_scale[2] = scale[2];
    d.ndc_offset[2] = offset[2];
  } else {
    bool empty = false;
    for (uint32_t i = 0; i < 2; ++i) {  // C8
      const float off = offset[i] + add, abs_s = std::abs(scale[i]);
      const uint32_t a0 = uint32_t(std::clamp(off - abs_s, 0.0f, kExtent));
      const uint32_t a1 = uint32_t(std::clamp(off + abs_s, 0.0f, kExtent));
      const uint32_t e = a1 - a0;
      if (e) {
        d.host.viewport[i] = float(a0);
        d.host.viewport[2 + i] = float(e);
        d.ndc_scale[i] = scale[i] * 2.0f / float(e);
        d.ndc_offset[i] = (off - (float(a0) + float(e) * 0.5f)) * 2.0f / float(e);
      } else {  // C9
        empty = true;
        d.host.viewport[i] = -1;
        d.host.viewport[2 + i] = 1;
        d.ndc_scale[i] = 1;
        d.ndc_offset[i] = 0;
      }
    }
    if (empty) d.flags |= cap::kDynEmptyViewport;
    float host_offset, host_scale;
    if ((clip >> 19) & 1) {  // C10
      host_offset = offset[2];
      host_scale = scale[2];
      d.ndc_scale[2] = 1;
      d.ndc_offset[2] = 0;
    } else {  // C11
      host_offset = offset[2] - scale[2];
      host_scale = 2 * scale[2];
      d.ndc_scale[2] = 0.5f;
      d.ndc_offset[2] = 0.5f;
      Note("clip_space_not_d3d");
    }
    if (writes_depth) {  // C13
      z_min = 0;
      z_max = 1;
    } else {
      z_min = Sat(host_offset);
      z_max = Sat(host_offset + host_scale);
      if (z_min > z_max) Note("viewport_z_inverted");  // C12: passed through
    }
  }
  d.host.viewport[4] = z_min;
  d.host.viewport[5] = z_max;

  // C18, C19: the window scissor the library computes: viewport ints ∩ scissor ints when enabled, then the screen scissor and the surface.
  int32_t x0 = int32_t(F(db::Ld32(base, dev + kDevViewport))), y0 = int32_t(F(db::Ld32(base, dev + kDevViewport + 4)));
  int32_t x1 = x0 + int32_t(F(db::Ld32(base, dev + kDevViewport + 8))), y1 = y0 + int32_t(F(db::Ld32(base, dev + kDevViewport + 12)));
  if (db::Ld32(base, dev + kDevScissorOn)) {
    x0 = std::max(x0, int32_t(db::Ld32(base, dev + kDevScissor)));
    y0 = std::max(y0, int32_t(db::Ld32(base, dev + kDevScissor + 4)));
    x1 = std::min(x1, int32_t(db::Ld32(base, dev + kDevScissor + 8)));
    y1 = std::min(y1, int32_t(db::Ld32(base, dev + kDevScissor + 12)));
  }
  const uint32_t screen_tl = db::Ld32(base, dev + kDevScreenTl), screen_br = db::Ld32(base, dev + kDevScreenBr);
  x0 = std::max(x0, int32_t(screen_tl & 0x7FFF));
  y0 = std::max(y0, int32_t((screen_tl >> 16) & 0x7FFF));
  x1 = std::min(x1, int32_t(screen_br & 0x7FFF));
  y1 = std::min(y1, int32_t((screen_br >> 16) & 0x7FFF));
  const uint32_t surf = db::Ld32(base, dev + kDevRt0) ? db::Ld32(base, dev + kDevRt0) : db::Ld32(base, dev + kDevDs);
  if (surf) {
    const uint32_t size_word = db::Ld32(base, surf + 36);
    x1 = std::min(x1, int32_t((size_word >> 18) & 0x3FFF) + 1);
    y1 = std::min(y1, int32_t((size_word >> 3) & 0x7FFF) + 1);
  }
  x0 = std::max(x0, 0);
  y0 = std::max(y0, 0);
  x1 = std::max(x1, x0);
  y1 = std::max(y1, y0);
  d.host.scissor = {x0, y0, uint32_t(x1 - x0), uint32_t(y1 - y0)};

  // Polygon offset (R11, R12), blend constants, stencil masks.
  const uint32_t po_front = kDevPolyOffset, po_back = kDevPolyOffset + 8;
  const uint32_t use = ((sc >> 11) & 1) ? po_front : po_back;
  const uint32_t ds = db::Ld32(base, dev + kDevDs);
  const bool float_depth = ds && ((db::Ld32(base, ds + 28) >> 16) & 1);
  d.host.depth_bias_slope = F(db::Ld32(base, dev + use)) * (1.0f / 16.0f);
  d.host.depth_bias_constant = F(db::Ld32(base, dev + use + 4)) * (float_depth ? 16777216.0f : 16777215.0f);
  d.host.depth_bias_clamp = 0;
  for (uint32_t i = 0; i < 4; ++i) d.host.blend_constants[i] = F(db::Ld32(base, dev + kDevBlendRgba + 4 * i));
  const uint32_t rm = db::Ld32(base, dev + kDevStencilFront), rm_b = db::Ld32(base, dev + kDevStencilBack);
  d.host.stencil_reference = uint8_t(rm);
  d.host.stencil_compare_mask = uint8_t(rm >> 8);
  d.host.stencil_write_mask = uint8_t(rm >> 16);
  if ((db::Ld32(base, dev + kDevDepthControl) & 1) && (rm_b & 0xFFFFFF) != (rm & 0xFFFFFF)) Note("stencil_back_refmask_differs");
}

// S100 speed: every input of DecodeFixed and DecodeDynamic, raw: the device ranges that hold the words they read (ranges, not single words:
// a superset is still exact) and the bound surfaces' words. A draw whose inputs are the last draw's (same frame tables) takes its fixed and
// dynamic entries without decoding or hashing (run 6927, WM30's match: 99% of draws repeat the last dynamic state, 93% the fixed one).
// Left out of the ranges on purpose: VGT min / max and the reset index (10444-10459: per draw, read by geometry, not by state).
struct StateWords {
  uint8_t screen[20];      // 10424-10443: screen scissor TL / BR
  uint8_t rs[140];         // 10460-10599: colour mask, blend constants, stencil, alpha ref, viewport scale / offset, depth control, blend 0, colour control, clip, SC mode, VTE, blend 1-3
  uint8_t vtx[4];          // 10688: VTX control
  uint8_t poly[16];        // 10832-10847: polygon offset front / back
  uint8_t scissor_on[4];   // 12288
  uint8_t surfaces[20];    // 12816-12835: render targets 0-3, depth surface
  uint8_t viewport[16];    // 13024-13039
  uint8_t scissor[16];     // 13052-13067
  uint32_t surface_words[6];  // RT 0-3 +28 (exp bias), (RT0 or DS) +36 (size), DS +28 (float depth)
  uint32_t prim = 0, has_ps = 0, writes_depth = 0;
};
static_assert(sizeof(StateWords) == 20 + 140 + 4 + 16 + 4 + 20 + 16 + 16 + 24 + 12, "no padding: compared with memcmp");
inline void GatherStateWords(const uint8_t* base, uint32_t dev, Prim prim, bool has_ps, const PixelShaderInfo* ps, StateWords& s) {
  std::memcpy(s.screen, db::Host(base, dev + 10424), sizeof(s.screen));
  std::memcpy(s.rs, db::Host(base, dev + 10460), sizeof(s.rs));
  std::memcpy(s.vtx, db::Host(base, dev + kDevVtxCntl), sizeof(s.vtx));
  std::memcpy(s.poly, db::Host(base, dev + kDevPolyOffset), sizeof(s.poly));
  std::memcpy(s.scissor_on, db::Host(base, dev + kDevScissorOn), sizeof(s.scissor_on));
  std::memcpy(s.surfaces, db::Host(base, dev + kDevRt0), sizeof(s.surfaces));
  std::memcpy(s.viewport, db::Host(base, dev + kDevViewport), sizeof(s.viewport));
  std::memcpy(s.scissor, db::Host(base, dev + kDevScissor), sizeof(s.scissor));
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t rt = db::Ld32(base, dev + kDevRt0 + 4 * i);
    s.surface_words[i] = rt ? db::Ld32(base, rt + 28) : 0;
  }
  const uint32_t rt0 = db::Ld32(base, dev + kDevRt0), ds = db::Ld32(base, dev + kDevDs);
  const uint32_t surf = rt0 ? rt0 : ds;
  s.surface_words[4] = surf ? db::Ld32(base, surf + 36) : 0;
  s.surface_words[5] = ds ? db::Ld32(base, ds + 28) : 0;
  s.prim = uint32_t(prim);
  s.has_ps = has_ps;
  s.writes_depth = ps && ps->writes_depth;
}
struct StateCache {
  uint64_t generation = ~0ull;
  uint64_t entry = ~0ull;  // S102 T18 (C7 hole): particle_lite::Entries() of the draw this cache was last filled from / confirmed by; by_marks needs exactly one draw entry since (this draw's own)
  StateWords words;
  uint32_t fixed = 0, dynamic = 0;
  // bool / loop constants: the device's raw 160 bytes
  uint64_t bool_generation = ~0ull;
  uint8_t bool_raw[160];
  uint32_t bool_loop = 0;
};
inline StateCache g_state_cache;

// ---- geometry -----------------------------------------------------------------------------------------------------------------------
inline float HalfToFloat(uint16_t h) {
  const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1F, m = h & 0x3FF;
  if (!e) return std::bit_cast<float>((s << 31) | (m ? 0x38000000u + (m << 13) : 0));  // (denormals: close enough for an extrapolated fourth vertex)
  if (e == 31) return std::bit_cast<float>((s << 31) | 0x7F800000u | (m << 13));
  return std::bit_cast<float>((s << 31) | ((e + 112) << 23) | (m << 13));
}
inline uint16_t FloatToHalf(float f) {
  const uint32_t b = std::bit_cast<uint32_t>(f);
  const uint32_t s = (b >> 16) & 0x8000, e = (b >> 23) & 0xFF, m = b & 0x7FFFFF;
  if (e >= 143) return uint16_t(s | 0x7C00 | (e == 255 && m ? 0x200 : 0));
  if (e <= 112) return uint16_t(s);
  return uint16_t(s | ((e - 112) << 10) | (m >> 13));
}
