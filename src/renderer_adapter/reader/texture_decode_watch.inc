
// ---- texture contents ---------------------------------------------------------------------------------------------------------------
// The guest layout of a texture (rex\graphics\pipeline\texture\util.cpp): where each level and layer lives, tiled or linear.
struct LevelLayout {
  uint32_t row_pitch_bytes = 0, pitch_blocks = 0, slice_stride_bytes = 0;
};

inline void GuestLevelLayout(const Fmt& f, const TexShape& s, uint32_t base_pitch_div32, bool base, uint32_t level, bool tiled, LevelLayout& l) {
  uint32_t row_texels, rows_texels;
  if (base) {
    row_texels = base_pitch_div32 << 5;
    rows_texels = s.height;
  } else {
    row_texels = std::max(NextPow2(s.width) >> level, 1u);
    rows_texels = std::max(NextPow2(s.height) >> level, 1u);
  }
  l.pitch_blocks = Align(Align(row_texels, f.bw) / f.bw, 32);
  l.row_pitch_bytes = l.pitch_blocks * f.bpb;
  if (!tiled && !base) l.row_pitch_bytes = Align(l.row_pitch_bytes, 256);
  const uint32_t block_rows = s.dim != 0 ? Align(Align(rows_texels, f.bh) / f.bh, 32) : 1;
  l.slice_stride_bytes = Align(l.row_pitch_bytes * block_rows, 4096);
}

inline void PackedMipOffset(uint32_t width, uint32_t height, const Fmt& f, uint32_t mip, uint32_t& x, uint32_t& y) {
  x = y = 0;
  const uint32_t log2_w = Log2Ceil(width), log2_h = Log2Ceil(height), log2_size = std::min(log2_w, log2_h);
  if (log2_size > 4 + mip) return;
  const uint32_t packed_base = log2_size > 4 ? log2_size - 4 : 0;
  const uint32_t packed = mip - packed_base;
  if (packed < 3) {
    if (log2_w > log2_h) y = 16 >> packed;
    else x = 16 >> packed;
  } else if (log2_w > log2_h) {
    x = (1u << (log2_w - packed_base)) >> (packed - 2);
  } else {
    y = (1u << (log2_h - packed_base)) >> (packed - 2);
  }
  x /= f.bw;
  y /= f.bh;
}

inline uint32_t TiledOffset2D(int32_t x, int32_t y, uint32_t pitch, uint32_t log2_bpb) {
  pitch = Align(pitch, 32);
  const int32_t macro = ((x >> 5) + (y >> 5) * int32_t(pitch >> 5)) << (log2_bpb + 7);
  const int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << log2_bpb;
  const int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return uint32_t(((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) + ((((((y & 8) >> 2) + (x >> 3)) & 3)) << 6) + (offset & 0x3F));
}

inline void SwapBlock(uint8_t* p, uint32_t bytes, uint32_t endian) {
  if (endian == 0) return;
  if (endian == 3 && bytes >= 4) {
    for (uint32_t i = 0; i + 4 <= bytes; i += 4) {
      uint32_t v;
      std::memcpy(&v, p + i, 4);
      v = (v << 16) | (v >> 16);
      std::memcpy(p + i, &v, 4);
    }
    return;
  }
  if (endian == 2 && bytes >= 4) {
    for (uint32_t i = 0; i + 4 <= bytes; i += 4) {
      uint32_t v;
      std::memcpy(&v, p + i, 4);
      v = std::byteswap(v);
      std::memcpy(p + i, &v, 4);
    }
    return;
  }
  if (endian == 1 || (endian == 2 && bytes == 2)) {
    for (uint32_t i = 0; i + 2 <= bytes; i += 2) {
      uint16_t v;
      std::memcpy(&v, p + i, 2);
      v = std::byteswap(v);
      std::memcpy(p + i, &v, 2);
    }
  }
}

// Decodes every level and layer to host layout: untiled, little-endian, BC blocks passed through. The blob of adapter_frame.h.
inline bool DecodeTexture(uint32_t image_id, const uint32_t w[6], std::vector<uint8_t>& blob) {
  const TexShape s = ShapeOf(w);
  const Fmt f = FormatOf(s.format);
  if (f.decode == kNoDecode) {
    Note("texture_format_no_decode", BaseFormat(s.format));
    return false;
  }
  if (s.dim == 2) {
    Note("texture_3d_no_decode");
    return false;
  }
  const uint32_t base_addr = w[1] & 0xFFFFF000u, mip_addr = w[5] & 0xFFFFF000u;
  if (!base_addr && !mip_addr) return false;
  if (!base_addr) Note("texture_mips_only");
  const uint32_t endian = (w[1] >> 6) & 3;
  const uint32_t pitch_div32 = (w[0] >> 22) & 0x1FF;
  const bool packed = (w[5] >> 11) & 1;
  const uint32_t packed_level = packed ? (Log2Ceil(std::min(s.width, s.height)) > 4 ? Log2Ceil(std::min(s.width, s.height)) - 4 : 0) : 0xFFFFFFFFu;
  const uint32_t max_level = s.mips - 1;
  const uint32_t log2_bpb = Log2Floor(f.bpb);

  // Mip offsets, as the guest layout accumulates them.
  std::array<uint32_t, 16> mip_offsets{};
  uint32_t mip_offset = 0;
  const uint32_t stored = std::min(max_level, packed_level);
  for (uint32_t level = 1; level <= stored; ++level) {
    LevelLayout l;
    GuestLevelLayout(f, s, pitch_div32, false, level, s.tiled, l);
    mip_offsets[level] = mip_offset;
    mip_offset += l.slice_stride_bytes * s.layers;
  }

  std::vector<cap::Subresource> subs;
  size_t data_bytes = 0;
  for (uint32_t level = 0; level <= max_level; ++level) {
    const uint32_t lw = std::max(s.width >> level, 1u), lh = std::max(s.height >> level, 1u);
    const uint32_t bw = Align(lw, f.bw) / f.bw, bh = Align(lh, f.bh) / f.bh;
    for (uint32_t layer = 0; layer < s.layers; ++layer) {
      cap::Subresource r;
      r.mip = level;
      r.layer = layer;
      r.width = lw;
      r.height = lh;
      r.row_bytes = bw * f.bpb;
      r.rows = bh;
      r.offset = data_bytes;
      data_bytes += size_t(r.row_bytes) * bh;
      subs.push_back(r);
    }
  }
  cap::TextureBlobHeader h;
  h.image = image_id;
  h.subresource_count = uint32_t(subs.size());
  h.format = uint32_t(f.host);
  h.data_bytes = data_bytes;
  blob.resize(sizeof(h) + subs.size() * sizeof(cap::Subresource) + data_bytes);
  std::memcpy(blob.data(), &h, sizeof(h));
  std::memcpy(blob.data() + sizeof(h), subs.data(), subs.size() * sizeof(cap::Subresource));
  uint8_t* data = blob.data() + sizeof(h) + subs.size() * sizeof(cap::Subresource);

  for (const cap::Subresource& r : subs) {
    const uint32_t level = r.mip;
    const bool is_base = level == 0;
    // The tail of a packed texture, or a plain stored level.
    const bool in_tail = packed && level >= packed_level;
    const uint32_t stored_level = in_tail ? packed_level : level;
    LevelLayout l;
    GuestLevelLayout(f, s, pitch_div32, is_base, stored_level, s.tiled, l);
    uint32_t region = is_base ? base_addr : mip_addr + (stored_level ? mip_offsets[stored_level] : 0);
    if (is_base ? !base_addr : !mip_addr) continue;  // left zero
    region += r.layer * l.slice_stride_bytes;
    uint32_t ox = 0, oy = 0;
    if (in_tail) PackedMipOffset(s.width, s.height, f, level, ox, oy);
    const uint8_t* src = Phys(region);
    uint8_t* out = data + r.offset;
    // S100 speed: an untiled level without the 4444 rotate copies whole rows; the per-texel endian swap only where it does anything
    // (SwapBlock on one texel: nothing for 1-byte texels, nothing for 2-byte texels with 16-in-32). The movie planes (untiled k_8,
    // run 6920) took ~3.6 ns a byte texel by texel.
    if (!s.tiled && f.decode != kRgba4Rotate) {
      const bool swaps = endian != 0 && f.bpb > 1 && !(f.bpb == 2 && endian == 3);
      const uint32_t texels = r.row_bytes / f.bpb;
      for (uint32_t y = 0; y < r.rows; ++y) {
        uint8_t* dst = out + y * r.row_bytes;
        std::memcpy(dst, src + (oy + y) * l.row_pitch_bytes + ox * f.bpb, r.row_bytes);
        if (swaps) {
          for (uint32_t x = 0; x < texels; ++x) SwapBlock(dst + x * f.bpb, f.bpb, endian);
        }
      }
      continue;
    }
    for (uint32_t y = 0; y < r.rows; ++y) {
      for (uint32_t x = 0; x < r.row_bytes / f.bpb; ++x) {
        const uint32_t at = s.tiled ? TiledOffset2D(int32_t(ox + x), int32_t(oy + y), l.pitch_blocks, log2_bpb)
                                    : (oy + y) * l.row_pitch_bytes + (ox + x) * f.bpb;
        uint8_t* dst = out + y * r.row_bytes + x * f.bpb;
        std::memcpy(dst, src + at, f.bpb);
        SwapBlock(dst, f.bpb, endian);
        if (f.decode == kRgba4Rotate) {
          // T3, settled S100 by picture: guest component k is nibble k (bits 4k..4k+3, after the endian swap); rotated left by 4 into
          // B4G4R4A4_PACK16 (B high, A low) the texture reads (R, G, B, A) = (nibble 0, 1, 2, 3). Evidence: Bobby Heenan's attire masks
          // (k_4_4_4_4, yComposition channel split; capture s100_heenan_build frames 17 / 21 / 25): this order gives the game's black
          // tuxedo jacket, black trousers with grey stripes and red trim; an exchange of nibbles 0 and 2 (tried earlier the same day from
          // a reading of the SDK's load shader) gave a grey jacket and red trousers (out\s100\heenan_predict.png).
          uint16_t v;
          std::memcpy(&v, dst, 2);
          v = uint16_t((v << 4) | (v >> 12));
          std::memcpy(dst, &v, 2);
        }
      }
    }
  }
  return true;
}

// ---- texture memory the CPU wrote (S100 speed) ----------------------------------------------------------------------------------
// Which textures changed, exactly: the SDK's physical-memory write watches (the old path's texture cache uses the same mechanism:
// Memory::RegisterPhysicalMemoryInvalidationCallback + EnablePhysicalMemoryAccessCallbacks write-protect a range's pages in every view
// until the first CPU write, guest, kernel or host, which calls back with the range and unprotects it). After the reader reads a texture
// it protects the texture's pages; a write marks its pages in g_written_pages (rounded out to 64 KB: fewer faults for big writes, at
// worst a neighbour re-read once); EnsureContent re-reads a texture only when a page of it is marked or the game flushed it. It replaces
// the 64-point sample with a full read every 60 frames (S100 morning): those full reads came due together and made the 15 ms frames in
// WM30 (run 6913: texture contents 6-7.5 ms in one frame).
inline constexpr uint32_t kPhysBytes = 0x20000000u;
inline std::atomic<uint64_t> g_written_pages[(kPhysBytes >> 12) / 64];
// The same marks for the geometry memo (BuildGeometry): each consumer clears only its own, so a texture re-read never hides a write to a
// vertex or index buffer that shares a 64 KB block with it.
inline std::atomic<uint64_t> g_written_pages_geo[(kPhysBytes >> 12) / 64];
// S100 speed: pages geometry protected and that no callback has unwatched since (set before protecting, cleared with every written mark).
// Every path that drops a watch calls the callbacks (access violation, guest Protect, Decommit, Release: PhysicalHeap::TriggerCallbacks),
// and the SDK unwatches only the asked pages plus the intersection of the callbacks' ranges, ours included: a page set here and not marked
// written is still protected, so WatchRange skips the system calls (run 6931: ~2.2 us a range, 60-110 ranges a frame in WM30's match).
inline std::atomic<uint64_t> g_watched_pages_geo[(kPhysBytes >> 12) / 64];
inline void* g_write_watch = nullptr;
// Strip-360 E2 / E3: the same marks from the host writers' reports (src\host_write.h: file reads, the crowd's CPU skinning, the memcpys, the
// LZX decompressor), exact ranges. g_notified_* keep only these, for the verify / census split (what the page watch alone would have seen).
inline std::atomic<uint64_t> g_notified_pages[(kPhysBytes >> 12) / 64];
inline std::atomic<uint64_t> g_notified_pages_geo[(kPhysBytes >> 12) / 64];
REXCVAR_DECLARE(bool, wwe2k14_rdr_page_watch);
REXCVAR_DECLARE(bool, wwe2k14_rdr_debug_notify_verify);
inline bool PageWatchOn() { return REXCVAR_GET(wwe2k14_rdr_page_watch) || REXCVAR_GET(wwe2k14_rdr_debug_notify_verify); }

template <class Op>
inline void ForPageMasks(uint32_t first_page, uint32_t last_page, Op op) {
  for (uint32_t p = first_page; p <= last_page;) {
    const uint32_t bit = p & 63, n = std::min<uint32_t>(64 - bit, last_page - p + 1);
    op(p >> 6, (n == 64 ? ~0ull : ((1ull << n) - 1)) << bit);
    p += n;
  }
}
inline std::pair<uint32_t, uint32_t> OnPhysicalWrite(void*, uint32_t start, uint32_t length, bool exact_range) {
  if (!length || start >= kPhysBytes) return {0u, UINT32_MAX};
  uint32_t first = start, last = uint32_t(std::min<uint64_t>(uint64_t(start) + length - 1, kPhysBytes - 1));
  if (!exact_range) {
    first &= ~0xFFFFu;
    last = std::min<uint32_t>(last | 0xFFFFu, kPhysBytes - 1);
  }
  ForPageMasks(first >> 12, last >> 12, [](uint32_t word, uint64_t mask) {
    g_written_pages[word].fetch_or(mask, std::memory_order_relaxed);
    g_written_pages_geo[word].fetch_or(mask, std::memory_order_relaxed);
    g_watched_pages_geo[word].fetch_and(~mask, std::memory_order_relaxed);
  });
  return {first, last - first + 1};
}
// A host writer's report (src\host_write.h), any thread: the physical views only (what the GPU reads; the 0xE0000000 view's physical
// address is 4 KB higher, RememberHeaderWords), exact pages.
inline void OnHostWrite(uint32_t addr, uint32_t length) {
  if (!length || addr < 0xA0000000u) return;
  const uint32_t phys = wwe2k14::guest_view::ToPhysical(addr);
  if (phys >= kPhysBytes) return;
  const uint32_t last = uint32_t(std::min<uint64_t>(uint64_t(phys) + length - 1, kPhysBytes - 1));
  if (wwe2k14::upd_lean::On(wwe2k14::upd_lean::kMarks)) {   // S102 U2 (R22 cut 2): same written marks; the census-only notified marks only while a census is on
    wwe2k14::upd_lean::MarkPagesLean(g_written_pages, g_written_pages_geo, g_notified_pages, g_notified_pages_geo, phys >> 12, last >> 12, wwe2k14::upd_lean::g_census.load(std::memory_order_relaxed));
    return;
  }
  ForPageMasks(phys >> 12, last >> 12, [](uint32_t word, uint64_t mask) {
    g_written_pages[word].fetch_or(mask, std::memory_order_relaxed);
    g_written_pages_geo[word].fetch_or(mask, std::memory_order_relaxed);
    g_notified_pages[word].fetch_or(mask, std::memory_order_relaxed);
    g_notified_pages_geo[word].fetch_or(mask, std::memory_order_relaxed);
  });
}
inline bool AllSet(uint32_t first_page, uint32_t last_page, const std::atomic<uint64_t>* bits) {
  bool all = true;
  ForPageMasks(first_page, last_page, [&](uint32_t word, uint64_t mask) { all &= (bits[word].load(std::memory_order_relaxed) & mask) == mask; });
  return all;
}
inline bool AnyWritten(uint32_t first_page, uint32_t last_page, std::atomic<uint64_t>* bits = g_written_pages) {
  bool any = false;
  ForPageMasks(first_page, last_page, [&](uint32_t word, uint64_t mask) { any |= (bits[word].load(std::memory_order_relaxed) & mask) != 0; });
  return any;
}
inline void ClearWritten(uint32_t first_page, uint32_t last_page, std::atomic<uint64_t>* bits = g_written_pages) {
  ForPageMasks(first_page, last_page, [&](uint32_t word, uint64_t mask) { bits[word].fetch_and(~mask, std::memory_order_relaxed); });
}
// A physical range watched for one consumer: its bits cleared, its pages protected (call before reading the range). With `watched` (the
// consumer's own map of pages it protected): a range already protected and unwritten since is left as it is (no system call).
// protect: -1 = PageWatchOn(); 0 / 1 = the caller's choice (strip-360 E3: large geometry keeps protection as a safety net).
inline bool WatchRange(uint32_t phys, uint32_t bytes, std::atomic<uint64_t>* bits, uint32_t pages[2], std::atomic<uint64_t>* watched = nullptr,
                       int protect_mode = -1) {
  phys &= kPhysBytes - 1;
  if (!bytes || uint64_t(phys) + bytes > kPhysBytes) {
    pages[0] = 1;
    pages[1] = 0;
    return false;
  }
  const bool protect = protect_mode < 0 ? PageWatchOn() : protect_mode != 0;
  if (protect && !g_write_watch) g_write_watch = REX_KERNEL_MEMORY()->RegisterPhysicalMemoryInvalidationCallback(&OnPhysicalWrite, nullptr);
  pages[0] = phys >> 12;
  pages[1] = uint32_t((uint64_t(phys) + bytes - 1) >> 12);
  if (watched) {
    if (AllSet(pages[0], pages[1], watched) && !AnyWritten(pages[0], pages[1], bits)) return true;
    // Marked before protecting: a write before the protection lands before the read (read as written); one after it faults and clears the
    // mark again (the callback marks it written).
    ForPageMasks(pages[0], pages[1], [&](uint32_t word, uint64_t mask) { watched[word].fetch_or(mask, std::memory_order_relaxed); });
  }
  ClearWritten(pages[0], pages[1], bits);
  ClearWritten(pages[0], pages[1], bits == g_written_pages_geo ? g_notified_pages_geo : g_notified_pages);
  if (!protect) return true;  // strip-360 E2 / E3: the host writers' reports and the game's flushes mark it from now on
  REX_KERNEL_MEMORY()->EnablePhysicalMemoryAccessCallbacks(phys, bytes, true, false);
  if (watched) ++g_times.geo_protects;
  return true;
}

// The physical byte ranges a texture's guest data spans: the base level (every layer) and the mip block (stored levels and the packed
// tail), as DecodeTexture reads them. False: not watchable (3D, no address).
inline bool TextureRanges(const uint32_t w[6], uint32_t range[2][2]) {
  const TexShape s = ShapeOf(w);
  const Fmt f = FormatOf(s.format);
  if (s.dim == 2 || f.decode == kNoDecode) return false;
  const uint32_t base_addr = (w[1] & 0xFFFFF000u) & (kPhysBytes - 1), mip_addr = (w[5] & 0xFFFFF000u) & (kPhysBytes - 1);
  const uint32_t pitch_div32 = (w[0] >> 22) & 0x1FF;
  const bool packed = (w[5] >> 11) & 1;
  const uint32_t min_log2 = Log2Ceil(std::min(s.width, s.height));
  const uint32_t packed_level = packed ? (min_log2 > 4 ? min_log2 - 4 : 0) : 0xFFFFFFFFu;
  range[0][0] = range[0][1] = range[1][0] = range[1][1] = 0;
  if (w[1] & 0xFFFFF000u) {
    LevelLayout l;
    GuestLevelLayout(f, s, pitch_div32, true, 0, s.tiled, l);
    range[0][0] = base_addr;
    range[0][1] = l.slice_stride_bytes * s.layers;
  }
  if ((w[5] & 0xFFFFF000u) && s.mips > 1) {
    uint32_t bytes = 0;
    const uint32_t stored = std::min(s.mips - 1, packed_level);
    for (uint32_t level = 1; level <= stored; ++level) {
      LevelLayout l;
      GuestLevelLayout(f, s, pitch_div32, false, level, s.tiled, l);
      bytes += l.slice_stride_bytes * s.layers;
    }
    range[1][0] = mip_addr;
    range[1][1] = bytes;
  }
  return range[0][1] || range[1][1];
}

// 64 eight-byte reads spread over the base level (and 16 over the mips): a cheap fingerprint of a texture's guest bytes. A texture whose
// fingerprint is unchanged and which no unlock flush named is not untiled and hashed again (S100 speed: every bound tiled texture was,
// every frame); every kFullReadEvery frames it is read in full anyway, for writes the sample misses.
inline constexpr uint64_t kFullReadEvery = 60;
inline uint64_t SampleTexture(const uint32_t w[6]) {
  const TexShape s = ShapeOf(w);
  const Fmt f = FormatOf(s.format);
  const uint32_t base_addr = w[1] & 0xFFFFF000u, mip_addr = w[5] & 0xFFFFF000u;
  uint64_t samples[80] = {};
  uint32_t n = 0;
  if (f.decode != kNoDecode && s.dim != 2) {
    LevelLayout l;
    GuestLevelLayout(f, s, (w[0] >> 22) & 0x1FF, true, 0, s.tiled, l);
    const uint32_t bytes0 = std::max<uint32_t>(l.slice_stride_bytes * std::max<uint32_t>(s.layers, 1), 8);
    if (base_addr) {
      const uint8_t* p = Phys(base_addr);
      for (uint32_t i = 0; i < 64; ++i) std::memcpy(&samples[n++], p + (uint64_t(bytes0 - 8) * i / 63 & ~7ull), 8);
    }
    if (mip_addr && s.mips > 1) {
      const uint8_t* p = Phys(mip_addr);
      const uint32_t bytes_m = std::max<uint32_t>(bytes0 / 4, 8);
      for (uint32_t i = 0; i < 16; ++i) std::memcpy(&samples[n++], p + (uint64_t(bytes_m - 8) * i / 15 & ~7ull), 8);
    }
  }
  return db::Hash64(reinterpret_cast<const uint8_t*>(samples), n * 8) ^ (uint64_t(w[1]) << 32 | w[2]);
}
