
// Converts one vertex's elements to host layout (V1-V12). `src` points at the vertex in guest memory; `limit` = bytes readable from there (V23).
inline void ConvertVertex(const VertexShaderInfo& vs, const uint8_t* src, int64_t available, uint32_t endian, uint8_t* dst) {
  auto word = [&](int32_t dw, uint32_t& out) -> bool {
    const int64_t at = int64_t(dw) * 4;
    if (at < 0 || at + 4 > available) {
      out = 0;
      return false;
    }
    uint32_t v;
    std::memcpy(&v, src + at, 4);
    out = Swap32For(v, endian);
    return true;
  };
  for (size_t i = 0; i < vs.elements.size(); ++i) {
    const cap::VertexElement& e = vs.elements[i];
    uint8_t* out = dst + e.host_offset;
    uint32_t v = 0;
    switch (e.source_format) {
      case 38: case 57: case 37: case 36: {  // V12
        const uint32_t n = e.source_format == 38 ? 4 : (e.source_format == 57 ? 3 : (e.source_format == 37 ? 2 : 1));
        for (uint32_t c = 0; c < n; ++c) {
          word(e.guest_offset + int32_t(c), v);
          std::memcpy(out + 4 * c, &v, 4);
        }
        break;
      }
      case 6: case 7:  // V11, V6: one word, the swapped layout is the host's
        word(e.guest_offset, v);
        std::memcpy(out, &v, 4);
        break;
      case 31: case 25: {  // two 16-bit components in one word, X the low half (as 8888's X is the low byte). The patched fetch's selects
        // exchange them for every k_16_16_FLOAT uv in the Brock match (run 6849): applied below, not here
        word(e.guest_offset, v);
        const uint16_t halves[2] = {uint16_t(v & 0xFFFF), uint16_t(v >> 16)};
        std::memcpy(out, halves, 4);
        break;
      }
      case 32: case 26: {  // four 16-bit components in two words, low half first in each (as 31 / 25; unconfirmed: not in the captures)
        for (uint32_t c = 0; c < 2; ++c) {
          word(e.guest_offset + int32_t(c), v);
          const uint16_t halves[2] = {uint16_t(v & 0xFFFF), uint16_t(v >> 16)};
          std::memcpy(out + 4 * c, halves, 4);
        }
        break;
      }
      case 17: case 16: {  // V9, V10; 16 is the mirrored field order
        word(e.guest_offset, v);
        int32_t x, y, z;
        double dx, dy, dz;
        const bool sgn = e.flags & cap::kElementSigned;
        if (e.source_format == 17) {
          x = sgn ? int32_t(v << 22) >> 22 : int32_t(v & 0x3FF);
          y = sgn ? int32_t(v << 11) >> 21 : int32_t((v >> 10) & 0x7FF);
          z = sgn ? int32_t(v) >> 21 : int32_t((v >> 21) & 0x7FF);
          dx = sgn ? 511.0 : 1023.0;
          dy = dz = sgn ? 1023.0 : 2047.0;
        } else {
          x = sgn ? int32_t(v << 21) >> 21 : int32_t(v & 0x7FF);
          y = sgn ? int32_t(v << 10) >> 21 : int32_t((v >> 11) & 0x7FF);
          z = sgn ? int32_t(v) >> 22 : int32_t((v >> 22) & 0x3FF);
          dx = dy = sgn ? 1023.0 : 2047.0;
          dz = sgn ? 511.0 : 1023.0;
        }
        const double f[3] = {std::max(double(x) / dx, -1.0), std::max(double(y) / dy, -1.0), std::max(double(z) / dz, -1.0)};
        for (uint32_t c = 0; c < 3; ++c) {
          const double scaled = f[c] * (sgn ? 32767.0 : 65535.0);
          const uint16_t q = uint16_t(int32_t(scaled < 0 ? scaled - 0.5 : scaled + 0.5));
          std::memcpy(out + 2 * c, &q, 2);
        }
        std::memset(out + 6, 0, 2);
        break;
      }
      default:
        word(e.guest_offset, v);
        std::memcpy(out, &v, 4);
        break;
    }
    // The patched fetch's component selects (BuildLayout), on the converted components: 4-byte floats, 16-bit pairs, 8-bit fours.
    if (i < vs.element_select.size()) {
      const std::array<uint8_t, 4>& s = vs.element_select[i];
      if (s != std::array<uint8_t, 4>{0, 1, 2, 3}) {
        uint32_t size = 0, count = 0;
        switch (e.source_format) {
          case 38: size = 4; count = 4; break;
          case 57: size = 4; count = 3; break;
          case 37: size = 4; count = 2; break;
          case 31: case 25: size = 2; count = 2; break;
          case 32: case 26: size = 2; count = 4; break;
          case 6: size = 1; count = 4; break;
          default: break;  // packed fields (10-11-11, 2-10-10-10) and single components: left as fetched (BuildLayout noted the selects)
        }
        if (count) {
          uint8_t copy[16];
          std::memcpy(copy, out, size * count);
          // The constant one in this element's numbers (select 5); zero is all-zero bytes in every one of them.
          uint8_t one[4] = {};
          const bool integer = e.flags & cap::kElementInteger, sgn = e.flags & cap::kElementSigned;
          if (size == 4) {
            const float f = 1.0f;
            std::memcpy(one, &f, 4);
          } else if (size == 2) {
            const uint16_t h = (e.source_format == 31 || e.source_format == 32) ? 0x3C00 : (integer ? 1 : (sgn ? 0x7FFF : 0xFFFF));
            std::memcpy(one, &h, 2);
          } else {
            one[0] = integer ? 1 : (sgn ? 0x7F : 0xFF);
          }
          for (uint32_t k = 0; k < count; ++k) {
            if (s[k] < count) std::memcpy(out + size * k, copy + size * s[k], size);
            else if (s[k] == 4) std::memset(out + size * k, 0, size);
            else if (s[k] == 5) std::memcpy(out + size * k, one, size);
          }
        }
      }
    }
  }
}

// P3 in input space: the fourth vertex = v[i1] - v[i0] + v[i2], per component of every element.
inline void ExtrapolateElement(const cap::VertexElement& e, const uint8_t* v0, const uint8_t* v1, const uint8_t* v2, uint8_t* out) {
  const uint8_t* a = v0 + e.host_offset;
  const uint8_t* b = v1 + e.host_offset;
  const uint8_t* c = v2 + e.host_offset;
  uint8_t* o = out + e.host_offset;
  const uint16_t vk = e.vk_format;
  if (vk == kVkRGBA32Float || vk == kVkRGB32Float || vk == kVkRG32Float || vk == kVkR32Float) {
    const uint32_t n = vk == kVkRGBA32Float ? 4 : (vk == kVkRGB32Float ? 3 : (vk == kVkRG32Float ? 2 : 1));
    for (uint32_t k = 0; k < n; ++k) {
      float x, y, z;
      std::memcpy(&x, a + 4 * k, 4);
      std::memcpy(&y, b + 4 * k, 4);
      std::memcpy(&z, c + 4 * k, 4);
      const float r = y - x + z;
      std::memcpy(o + 4 * k, &r, 4);
    }
  } else if (vk == kVkRG16Float || vk == kVkRGBA16Float) {
    const uint32_t n = vk == kVkRG16Float ? 2 : 4;
    for (uint32_t k = 0; k < n; ++k) {
      uint16_t x, y, z;
      std::memcpy(&x, a + 2 * k, 2);
      std::memcpy(&y, b + 2 * k, 2);
      std::memcpy(&z, c + 2 * k, 2);
      const uint16_t r = FloatToHalf(HalfToFloat(y) - HalfToFloat(x) + HalfToFloat(z));
      std::memcpy(o + 2 * k, &r, 2);
    }
  } else if (vk == kVkRGBA8Unorm || vk == kVkRGBA8Uint) {
    for (uint32_t k = 0; k < 4; ++k) o[k] = uint8_t(std::clamp(int32_t(b[k]) - int32_t(a[k]) + int32_t(c[k]), 0, 255));
  } else if (vk == kVkRGBA16Snorm) {
    for (uint32_t k = 0; k < 4; ++k) {
      int16_t x, y, z;
      std::memcpy(&x, a + 2 * k, 2);
      std::memcpy(&y, b + 2 * k, 2);
      std::memcpy(&z, c + 2 * k, 2);
      const int16_t r = int16_t(std::clamp(int32_t(y) - int32_t(x) + int32_t(z), -32767, 32767));
      std::memcpy(o + 2 * k, &r, 2);
    }
  } else {
    std::memcpy(o, a, 4);
  }
}

struct DrawArgs {
  Prim prim = Prim::TriList;
  int32_t base_vertex = 0;
  uint32_t start_index = 0, index_count = 0;
  const uint8_t* inline_vertices = nullptr;
  uint32_t inline_count = 0, inline_stride = 0;
  bool unmarked = false;  // strip-360 C7: no setter since the last draw (the library's marks at the draw's flush point), ReadMarks
};

// Settings parity S5: the draw's viewport state squeezed by the screen shape when the old path's rule says so (WideState). A copy of the
// dynamic entry, so the state cache keeps the plain one.
// lane 177: the live twin of wide_replay.h FullViewDraw: a T_Rect-shaped draw (prim Quad, four inline vertices of 12 bytes = one RGB32F element) whose corners reach the
// view's four edges. The vertices are the game's own (big-endian, in the inline ring), PosScale / fUseProjMatrix the device's vertex constants c0.xy / c28.x.
inline bool LiveFullViewQuad(const uint8_t* base, uint32_t dev, const DrawArgs& a, const VertexShaderInfo* vs) {
  if (a.prim != Prim::Quad || !a.inline_vertices || a.inline_count != 4 || a.inline_stride != 12 || !vs || vs->elements.size() != 1 || vs->guest_stride != 3) return false;
  cap::wide::FlatQuad q;
  for (uint32_t i = 0; i < 4; ++i) {
    uint32_t xb, yb;
    std::memcpy(&xb, a.inline_vertices + 12 * i, 4), std::memcpy(&yb, a.inline_vertices + 12 * i + 4, 4);
    q.x[i] = F(__builtin_bswap32(xb)), q.y[i] = F(__builtin_bswap32(yb));
  }
  q.pos_x = F(db::Ld32(base, dev + kDevVsFloat)), q.pos_y = F(db::Ld32(base, dev + kDevVsFloat + 4)), q.use_proj = F(db::Ld32(base, dev + kDevVsFloat + 16 * 28));
  return cap::wide::FullViewQuad(q);
}

// lane 185: the DLSS jitter's full-view test: a quad list of four inline vertices (any stride of at least 12 bytes, the position first: T_Rect with or without texture coordinates, e.g. the
// audience atlas copy yRect_texcoord / yAudience_color_alpha, PS 7ABDFEFBCADB4BF9) whose corners reach the view's four edges, pixel-placed. A screen-space composite is not the 3D scene: not jittered.
inline bool LiveFullViewRect(const uint8_t* base, uint32_t dev, const DrawArgs& a) {
  if (a.prim != Prim::Quad || !a.inline_vertices || a.inline_count != 4 || a.inline_stride < 12 || (a.inline_stride & 3)) return false;
  cap::wide::FlatQuad q;
  for (uint32_t i = 0; i < 4; ++i) {
    uint32_t xb, yb;
    std::memcpy(&xb, a.inline_vertices + a.inline_stride * i, 4), std::memcpy(&yb, a.inline_vertices + a.inline_stride * i + 4, 4);
    q.x[i] = F(__builtin_bswap32(xb)), q.y[i] = F(__builtin_bswap32(yb));
  }
  q.pos_x = F(db::Ld32(base, dev + kDevVsFloat)), q.pos_y = F(db::Ld32(base, dev + kDevVsFloat + 4)), q.use_proj = F(db::Ld32(base, dev + kDevVsFloat + 16 * 28));
  return cap::wide::FullViewQuad(q);
}

inline void ApplyWide(const uint8_t* base, uint32_t dev, const DrawArgs& a, const VertexShaderInfo* vs, uint64_t vs_sdk_hash, cap::Draw& d) {
  const Prim prim = a.prim;
  FrameTables& t = g_frame;
  const cap::DrawDynamic from = t.dynamics[d.dynamic];
  const bool screen_space = prim == Prim::Rect || !(db::Ld32(base, dev + kDevVteCntl) & 1) || (from.flags & cap::kDynClipOff);  // lane 171: as the offline replay (a clip-off draw is pixel-placed)
  const bool full_view_quad = !g_wide.no_full_view && LiveFullViewQuad(base, dev, a, vs);  // lane 177: found by shape, read once per quad-list draw
  const cap::wide::Verdict v = cap::wide::Decide(g_wide, screen_space, from.host.viewport[2], vs_sdk_hash, full_view_quad, from.flags & cap::kDynClipOff);  // lane 244: clip-off draws are never squeezed (wide_rule.h)  // lane 171: the rule is wide_rule.h's
  if (v.full_view_first)
    REXLOG_INFO("[rdr][wide] a full-view rectangle (fade / flash / tint: its corners reach the view's four edges) is left unsqueezed so it covers the whole screen: VS {:016X} PS {:016X} (our hashes)",
                d.vs_hash, d.ps_hash);
  if (v.post_after_first)
    REXLOG_INFO("[rdr][wide] a full-screen pass after the HUD marker left alone (the 30 Years filter): VS {:016X} PS {:016X} (our hashes)", d.vs_hash,
                d.ps_hash);
  if (v.squeeze && !(from.flags & cap::kDynEmptyViewport)) {
    cap::DrawDynamic copy = from;
    if (g_wide.wide > 1.001f) copy.ndc_scale[0] /= g_wide.wide, copy.ndc_offset[0] /= g_wide.wide;
    if (g_wide.tall > 1.001f) copy.ndc_scale[1] /= g_wide.tall, copy.ndc_offset[1] /= g_wide.tall;
    d.dynamic = Dedupe(t.dynamics, t.dynamic_map, copy, reinterpret_cast<const uint8_t*>(&copy), sizeof(copy));
  }
  if (v.marker_first) REXLOG_INFO("[rdr][wide] the post chain's last pass met: HUD draws squeezed after it");
}

// ---- geometry builds (S100 speed step 2: the big ones converted on the reader workers) -------------------------------------------
// A build in three parts. Gather (game thread, at the draw): the device words the build reads and the vertex range the draw uses (a min /
// max pass over its indices); for a build that goes to a worker, copies of the index bytes and of that vertex range (the game may rewrite
// either right after the draw: ring buffers, the inline ring). Convert (a worker, or the game thread for small builds): the indices
// resolved, the content key, the vertices converted, the primitive conversion. Commit (the game thread: at once, or at JoinReaderWork):
// the content-key dedupe, the bytes to the sink, g_cap.geo. A draw whose build is pending is pushed without its geometry and patched at
// the join (JoinGeometryJobs), before the frame is serialized; later draws of the same memo key in the frame take the same pending build.
inline constexpr uint32_t kGeoReset = 0xFFFFFFFFu;  // a restart, inside a build
struct GeoJob : ReaderJob {
  // inputs (Gather)
  const VertexShaderInfo* vs = nullptr;  // g_vs entries are never changed or erased once made
  Prim prim = Prim::TriList;
  bool is_inline = false, idx32 = false, restart_on = false;
  uint32_t endian = 2, ib_endian = 0, reset_value = 0, base_vertex = 0, vmin = 0, vmax = 0, guest_stride_bytes = 0;
  uint32_t count = 0;            // indices to read (indexed) or vertices (inline)
  uint32_t lo = 0, v_count = 0;  // the resolved vertex range
  uint32_t hash_len = 0;         // vertex bytes from vertex lo the content key covers (clipped at the stream's end)
  uint32_t avail = 0;            // vertex bytes from vertex lo that may be read (the stream's rest, or the copy's length)
  const uint8_t* vsrc = nullptr;  // vertex bytes from vertex lo: guest memory, or v_copy
  const uint8_t* isrc = nullptr;  // raw index bytes: guest memory, or i_copy
  std::vector<uint8_t> v_copy, i_copy;
  // outputs (Convert)
  bool ok = false, emit_indices = false;
  uint64_t content_key = 0;
  GeoRef ref;  // its shape; the offsets come at commit
  std::vector<uint8_t> vout, iout;
  // the join
  uint64_t memo_key = 0;
  bool memo = false;
  std::vector<uint32_t> draws;  // this frame's draws (g_frame.draws) waiting for it
  // strip-360 E1 census (--wwe2k14_rdr_debug_source_census): a rebuild of a watched entry whose pages were written
  bool census_rewatch = false;
  uint64_t census_old_key = 0;
  uint32_t census_addr = 0, census_bytes = 0;
  bool census_named = false;
  // S102 T15 (wwe2k14_rdr_geo_hash_once): the hash of exactly this job's vertex bytes (vsrc, hash_len), made by BuildGeometry's memo check
  bool pre_vhash_ok = false;
  uint64_t pre_vhash = 0;
  void Clear() {
    pre_vhash_ok = false;
    pre_vhash = 0;
    vs = nullptr;
    prim = Prim::TriList;
    is_inline = idx32 = restart_on = ok = emit_indices = memo = census_rewatch = false;
    endian = 2;
    ib_endian = reset_value = base_vertex = vmin = vmax = guest_stride_bytes = count = lo = v_count = hash_len = avail = 0;
    vsrc = isrc = nullptr;
    v_copy.clear();
    i_copy.clear();
    vout.clear();
    iout.clear();
    content_key = memo_key = 0;
    ref = {};
    draws.clear();
    notes.clear();
  }
  void Run() override;
};

inline uint32_t GeoResolve1(const GeoJob& j, uint32_t idx) {
  const uint32_t v = (idx + j.base_vertex) & 0xFFFFFFu;
  return std::clamp(v, std::min(j.vmin, j.vmax), j.vmax);
}
inline uint32_t GeoRawIndex(const GeoJob& j, uint32_t i) {
  if (j.idx32) {
    uint32_t v;
    std::memcpy(&v, j.isrc + 4 * size_t(i), 4);
    return Swap32For(v, j.ib_endian);
  }
  uint16_t h;
  std::memcpy(&h, j.isrc + 2 * size_t(i), 2);
  return Swap16For(h, j.ib_endian);
}

// The raw (byte-swapped) min / max of 16-bit indices; with skip_ffff the restart value 0xFFFF is left out. False: none left.
inline bool GeoMinMax16(const uint8_t* src, uint32_t n, bool swap, bool skip_ffff, uint32_t& lo, uint32_t& hi) {
  uint16_t mn = 0xFFFF, mx = 0, mx1 = 0;  // mx1: the max of v + 1, where 0xFFFF wraps to 0 and so drops out
  if (swap) {
    for (uint32_t i = 0; i < n; ++i) {
      uint16_t v;
      std::memcpy(&v, src + 2 * size_t(i), 2);
      v = uint16_t((v << 8) | (v >> 8));
      mn = std::min(mn, v);
      mx = std::max(mx, v);
      mx1 = std::max(mx1, uint16_t(v + 1));
    }
  } else {
    for (uint32_t i = 0; i < n; ++i) {
      uint16_t v;
      std::memcpy(&v, src + 2 * size_t(i), 2);
      mn = std::min(mn, v);
      mx = std::max(mx, v);
      mx1 = std::max(mx1, uint16_t(v + 1));
    }
  }
  if (!n) return false;
  if (skip_ffff) {
    if (!mx1) return false;  // every index a restart
    lo = mn;
    hi = uint32_t(mx1) - 1;
    return true;
  }
  lo = mn;
  hi = mx;
  return true;
}

// The resolved vertex range [lo, hi] of the draw. False: no vertex (empty, or every index a restart).
inline bool GeoRange(const GeoJob& j, uint32_t& lo, uint32_t& hi) {
  const uint32_t clamp_lo = std::min(j.vmin, j.vmax);
  uint32_t raw_lo = 0, raw_hi = 0;
  bool fast = false;
  if (j.is_inline) {
    if (!j.count) return false;
    raw_hi = j.count - 1;
    fast = true;
  } else if (!j.idx32 && (!j.restart_on || j.reset_value == 0xFFFF)) {
    if (!GeoMinMax16(j.isrc, j.count, j.ib_endian == 1 || j.ib_endian == 2, j.restart_on, raw_lo, raw_hi)) return false;
    fast = true;
  }
  if (fast) {
    // resolve is monotonic in the raw index while (index + base) stays inside 24 bits: the range's ends resolve to the range.
    const int64_t s_lo = int64_t(raw_lo) + int32_t(j.base_vertex), s_hi = int64_t(raw_hi) + int32_t(j.base_vertex);
    if (s_lo >= 0 && s_hi <= 0xFFFFFF) {
      lo = std::clamp(uint32_t(s_lo), clamp_lo, j.vmax);
      hi = std::clamp(uint32_t(s_hi), clamp_lo, j.vmax);
      return true;
    }
  }
  lo = 0xFFFFFFFFu;
  hi = 0;
  for (uint32_t i = 0; i < j.count; ++i) {
    uint32_t v = i;
    if (!j.is_inline) {
      v = GeoRawIndex(j, i);
      if (j.restart_on && v == j.reset_value) continue;
    }
    const uint32_t r = GeoResolve1(j, v);
    lo = std::min(lo, r);
    hi = std::max(hi, r);
  }
  return lo <= hi;
}

// Gather (the game thread, at the draw). False: nothing to draw (the notes say why).
inline bool GeoGather(const uint8_t* base, uint32_t dev, const DrawArgs& a, const VertexShaderInfo& vs, GeoJob& j) {
  if (vs.elements.empty()) return false;
  j.vs = &vs;
  j.prim = a.prim;
  j.is_inline = a.inline_vertices != nullptr;
  // The vertex stream (V2, V3).
  const uint8_t* stream = nullptr;
  uint32_t stream_bytes = 0;
  j.endian = 2;
  if (j.is_inline) {
    stream = a.inline_vertices;
    stream_bytes = a.inline_count * a.inline_stride;
  } else {
    const uint32_t c = vs.fetch_constant;
    const uint32_t at = dev + kDevFetch + 24 * (c / 3) + 8 * (c % 3);
    const uint32_t w0 = db::Ld32(base, at), w1 = db::Ld32(base, at + 4);
    if ((w0 & 3) != 3) Note("vfetch_constant_type_not_vertex");
    j.endian = w1 & 3;
    stream_bytes = ((w1 >> 2) & 0xFFFFFF) * 4;
    if (!(w0 & ~3u)) {
      Note("vfetch_constant_address_zero");
      return false;
    }
    stream = Phys(w0 & ~3u);
  }
  j.guest_stride_bytes = (vs.guest_stride ? vs.guest_stride * 4 : (j.is_inline ? a.inline_stride : 0));
  if (!j.guest_stride_bytes) {
    Note("vfetch_stride_zero");
    return false;
  }
  // The index state (P11-P14, V15).
  j.vmin = db::Ld32(base, dev + kDevVgtMin);
  j.vmax = db::Ld32(base, dev + kDevVgtMax);
  j.restart_on = (db::Ld32(base, dev + kDevScMode) >> 21) & 1;
  j.reset_value = db::Ld32(base, dev + kDevReset);
  j.base_vertex = uint32_t(a.base_vertex);
  if (j.is_inline) {
    j.count = a.inline_count;
  } else {
    const uint32_t ib = db::Ld32(base, dev + kDevIb);
    const uint32_t flags = db::Ld32(base, ib);
    j.idx32 = (flags >> 31) != 0;
    j.ib_endian = (flags >> 29) & 3;
    const uint32_t ib_addr = db::Ld32(base, ib + 24);
    const uint32_t ib_size = db::Ld32(base, ib + 28);
    const uint32_t unit = j.idx32 ? 4 : 2;
    j.count = a.index_count;
    if (ib_size && (a.start_index + a.index_count) * unit > ib_size) {  // P13
      Note("index_buffer_short");
      j.count = ib_size / unit > a.start_index ? ib_size / unit - a.start_index : 0;
    }
    j.isrc = db::Host(base, ib_addr + a.start_index * unit);
    if (!j.idx32 && j.restart_on && j.reset_value > 0xFFFF) Note("reset_index_above_16bit");  // P11
    if (!j.idx32) j.reset_value &= 0xFFFF;
    if (j.restart_on && !j.idx32 && j.reset_value != 0xFFFF) Note("reset_index_not_ffff");
  }
  // Rebase: the converted vertices start at the smallest vertex this draw uses.
  uint32_t lo = 0, hi = 0;
  if (!GeoRange(j, lo, hi)) return false;
  j.lo = lo;
  j.v_count = hi - lo + 1;
  if (j.v_count > 8u * 1024 * 1024) {
    Note("draw_vertex_range_huge");
    return false;
  }
  const uint64_t src_at = uint64_t(lo) * j.guest_stride_bytes;
  const uint64_t rest = stream_bytes > src_at ? stream_bytes - src_at : 0;
  j.hash_len = uint32_t(std::min<uint64_t>(uint64_t(j.v_count) * j.guest_stride_bytes, rest));
  j.avail = uint32_t(rest);
  j.vsrc = stream + src_at;
  return true;
}

// The bytes a worker will read, copied now (the game may rewrite them right after the draw). An element reads at most 4 words from its
// offset (ConvertVertex), so the last vertex reads at most max(stride, (largest offset + 4) * 4) bytes.
inline void GeoCopyInputs(GeoJob& j) {
  int32_t max_offset = 0;
  for (const cap::VertexElement& e : j.vs->elements) max_offset = std::max<int32_t>(max_offset, e.guest_offset);
  const uint64_t last = std::max<uint64_t>(j.guest_stride_bytes, uint64_t(max_offset + 4) * 4);
  const uint32_t len = uint32_t(std::min<uint64_t>(j.avail, uint64_t(j.v_count - 1) * j.guest_stride_bytes + last));
  j.v_copy.assign(j.vsrc, j.vsrc + len);
  j.vsrc = j.v_copy.data();
  j.avail = len;
  if (!j.is_inline) {
    const size_t ilen = size_t(j.count) * (j.idx32 ? 4 : 2);
    j.i_copy.assign(j.isrc, j.isrc + ilen);
    j.isrc = j.i_copy.data();
  }
}

// Convert, part 1: the indices resolved (absolute) and the content key: the guest bytes and everything that shapes the output.
inline void GeoKey(GeoJob& j, std::vector<uint32_t>& indices) {
  indices.resize(j.count);
  if (j.is_inline) {
    for (uint32_t i = 0; i < j.count; ++i) indices[i] = GeoResolve1(j, i);
  } else {
    for (uint32_t i = 0; i < j.count; ++i) {
      const uint32_t v = GeoRawIndex(j, i);
      indices[i] = (j.restart_on && v == j.reset_value) ? kGeoReset : GeoResolve1(j, v);
    }
  }
  uint64_t key = j.pre_vhash_ok ? j.pre_vhash : cap::Xxh3(j.vsrc, j.hash_len);  // S102 T15: the same hash, made once
  key ^= j.vs->layout_hash * 0x9E3779B97F4A7C15ull;
  key ^= uint64_t(uint32_t(j.prim)) * 0xC2B2AE3D27D4EB4Full;
  key ^= cap::Xxh3(indices.data(), indices.size() * 4) * 0xFF51AFD7ED558CCDull;
  j.content_key = key;
}

// Convert, part 2: the converted vertices and indices into j.vout / j.iout, the GeoRef's shape into j.ref. False: nothing to draw.
inline bool GeoConvert(GeoJob& j, std::vector<uint32_t>& indices) {
  const VertexShaderInfo& vs = *j.vs;
  const uint32_t lo = j.lo, v_count = j.v_count;
  uint32_t host_vertices = v_count;
  const uint32_t stride = vs.host_stride;
  std::vector<uint8_t>& vout = j.vout;
  vout.resize(size_t(host_vertices) * stride);
  for (uint32_t v = 0; v < v_count; ++v) {
    const int64_t at = int64_t(v) * j.guest_stride_bytes;
    ConvertVertex(vs, j.vsrc + at, int64_t(j.avail) - at, j.endian, vout.data() + size_t(v) * stride);
  }
  for (uint32_t& v : indices) {
    if (v != kGeoReset) v -= lo;
  }

  // Primitive conversion.
  static thread_local std::vector<uint32_t> list;
  list.clear();
  uint8_t flags = 0;
  bool emit_indices = !j.is_inline || v_count != j.count;  // (clamped inline vertices keep their order through indices)
  switch (j.prim) {
    case Prim::Quad:  // P6: 0,1,2 / 0,2,3
      for (size_t i = 0; i + 4 <= indices.size(); i += 4) {
        list.insert(list.end(), {indices[i], indices[i + 1], indices[i + 2], indices[i], indices[i + 2], indices[i + 3]});
      }
      flags |= cap::kDrawQuadsToList;
      indices.swap(list);
      emit_indices = true;
      break;
    case Prim::QuadStrip:
      for (size_t i = 0; i + 4 <= indices.size(); i += 2) {
        list.insert(list.end(), {indices[i], indices[i + 1], indices[i + 3], indices[i], indices[i + 3], indices[i + 2]});
      }
      indices.swap(list);
      emit_indices = true;
      break;
    case Prim::TriFan:
    case Prim::Polygon:  // P2
      for (size_t i = 1; i + 1 < indices.size(); ++i) list.insert(list.end(), {indices[0], indices[i], indices[i + 1]});
      indices.swap(list);
      emit_indices = true;
      break;
    case Prim::Rect: {  // P3, in input space: only exact when the vertex shader is affine in its inputs
      if (vs.elements[0].vk_format != kVkRGBA32Float && vs.elements[0].vk_format != kVkRGB32Float && vs.elements[0].vk_format != kVkRG32Float) Note("rect_list_position_not_float32");
      const size_t rects = indices.size() / 3;
      vout.resize(size_t(host_vertices + rects) * stride);
      const uint32_t pos_at = vs.elements[0].host_offset;
      for (size_t r = 0; r < rects; ++r) {
        const uint32_t v[3] = {indices[r * 3], indices[r * 3 + 1], indices[r * 3 + 2]};
        if (v[0] == kGeoReset || v[1] == kGeoReset || v[2] == kGeoReset) continue;
        float p[3][2];
        for (uint32_t k = 0; k < 3; ++k) std::memcpy(p[k], vout.data() + size_t(v[k]) * stride + pos_at, 8);
        // The longest XY edge decides; the vertex opposite it is the rectangle's corner.
        const float d[3] = {(p[1][0] - p[2][0]) * (p[1][0] - p[2][0]) + (p[1][1] - p[2][1]) * (p[1][1] - p[2][1]),
                            (p[2][0] - p[0][0]) * (p[2][0] - p[0][0]) + (p[2][1] - p[0][1]) * (p[2][1] - p[0][1]),
                            (p[0][0] - p[1][0]) * (p[0][0] - p[1][0]) + (p[0][1] - p[1][1]) * (p[0][1] - p[1][1])};
        uint32_t i0 = 0;
        if (d[1] > d[0]) i0 = 1;
        if (d[2] > d[i0]) i0 = 2;
        const uint32_t i1 = (i0 + 1) % 3, i2 = (i0 + 2) % 3;
        const uint32_t fourth = host_vertices + uint32_t(r);
        uint8_t* dst = vout.data() + size_t(fourth) * stride;
        std::memset(dst, 0, stride);
        for (const cap::VertexElement& e : vs.elements) {
          ExtrapolateElement(e, vout.data() + size_t(v[i0]) * stride, vout.data() + size_t(v[i1]) * stride, vout.data() + size_t(v[i2]) * stride, dst);
        }
        list.insert(list.end(), {v[i0], v[i1], v[i2], v[i1], fourth, v[i2]});
      }
      host_vertices += uint32_t(rects);
      flags |= cap::kDrawRectsExpanded;
      indices.swap(list);
      emit_indices = true;
      break;
    }
    default:
      break;
  }
  if (!j.restart_on || !(j.prim == Prim::TriStrip || j.prim == Prim::LineStrip)) {  // a restart in a list has no host form: the triangle goes degenerate
    for (uint32_t& v : indices) {
      if (v == kGeoReset) {
        Note("restart_index_in_non_strip");
        v = 0;
      }
    }
  }

  GeoRef& g = j.ref;
  g = {};
  g.flags = flags;
  g.v_count = host_vertices;
  g.v_bytes = uint32_t(vout.size());
  j.emit_indices = emit_indices;
  j.iout.clear();
  if (emit_indices) {
    uint32_t top = 0;
    for (uint32_t v : indices) {
      if (v != kGeoReset) top = std::max(top, v);
    }
    const bool out32 = j.idx32 || top >= 0xFFFF;
    g.idx32 = out32;
    g.i_count = uint32_t(indices.size());
    g.i_bytes = uint32_t(indices.size() * (out32 ? 4 : 2));
    j.iout.resize(g.i_bytes);
    if (out32) {
      for (size_t i = 0; i < indices.size(); ++i) std::memcpy(j.iout.data() + 4 * i, &indices[i], 4);
    } else {
      for (size_t i = 0; i < indices.size(); ++i) {
        const uint16_t v = indices[i] == kGeoReset ? 0xFFFF : uint16_t(indices[i]);
        std::memcpy(j.iout.data() + 2 * i, &v, 2);
      }
    }
  }
  return true;
}

inline void GeoJob::Run() {
  static thread_local std::vector<uint32_t> indices;
  GeoKey(*this, indices);
  ok = GeoConvert(*this, indices);
}

// Commit, part 1 (game thread): the same bytes already stored (any frame, any draw): that copy. Part 2: the job's bytes to the sink.
inline bool GeoFindStored(const GeoJob& j, GeoRef& out) {
  auto it = g_cap.geo.find(j.content_key);
  if (it == g_cap.geo.end()) return false;
  if (g_live.active() && g_geo_used.insert_or_assign(j.content_key, g_record_frame).second) g_geo_expiry.Note(j.content_key, g_record_frame);  // lane 266: every insert is queued
  out = it->second;
  return true;
}
inline bool GeoStore(GeoJob& j, GeoRef& out) {
  GeoRef g = j.ref;
  const uint64_t v_at = SinkAppend(cap::Stream::Geometry, std::move(j.vout));
  if (v_at == UINT64_MAX) return false;
  g.v_off = v_at;
  if (j.emit_indices) {
    const uint64_t i_at = SinkAppend(cap::Stream::Geometry, std::move(j.iout));
    if (i_at == UINT64_MAX) return false;
    g.i_off = i_at;
  }
  g_cap.geo[j.content_key] = g;
  if (g_live.active() && g_geo_used.insert_or_assign(j.content_key, g_record_frame).second) g_geo_expiry.Note(j.content_key, g_record_frame);  // lane 266: a new key is queued once
  out = g;
  return true;
}

// This frame's geometry jobs (the game thread's; pooled: their copy buffers are kept between frames).
struct GeoJobPool {
  std::vector<std::unique_ptr<GeoJob>> jobs;
  size_t used = 0;
  GeoJob* Take() {
    if (used == jobs.size()) jobs.push_back(std::make_unique<GeoJob>());
    GeoJob* j = jobs[used++].get();
    j->Clear();
    return j;
  }
};
inline GeoJobPool g_geo_jobs;
