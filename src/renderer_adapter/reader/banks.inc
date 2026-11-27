
// ---- hooks' bodies --------------------------------------------------------------------------------------------------------------------
// The draw's constant banks. Cheap per draw (S100: the full 2 x 4 KB read, overlay and hash of every draw ran inside the game's
// render-list replay, ~3,500 draws a frame in WM30, and the WM30 crash came only with it on: runs 6875 / 6877): the device's float
// shadows are compared raw with our copy (one memcmp each), only the 64-byte blocks that changed are converted, and a bank is built
// and hashed only when the constants or the bound shaders changed since the last draw.
struct BankCache {
  uint32_t dev = 0;
  bool valid = false;
  uint8_t vs_raw[4096], ps_raw[4096];
  cap::ConstantBank vs_base, ps_base;  // the device's floats, host order
  uint64_t version = 0;
  // the last banks built
  uint64_t built_version = ~0ull;
  uint32_t built_vs_object = 0, built_ps_object = 0, built_frame = ~0u;
  uint32_t vs_index = 0, ps_index = 0;
  // the banks as sent this frame (BankDef chains)
  uint64_t defs_frame = ~0ull;
  uint32_t vs_def = cap::kBankNoBase, ps_def = cap::kBankNoBase;
  cap::ConstantBank vs_last, ps_last;
  // S100 speed round 2: which 64-byte blocks can differ from the last bank sent: device floats changed since (dirty) and the blocks the
  // last bank's literals covered; with the current literals' blocks only those are rebuilt and compared (the bank as before, by
  // construction: every other block equals the last one sent).
  uint64_t vs_dirty = ~0ull, ps_dirty = ~0ull;
  uint64_t vs_lit_last = 0, ps_lit_last = 0;
  uint64_t full_generation = ~0ull;  // S100 speed: the frame tables' generation of the last full compare (one per frame part)
  // S102 T12 (bank_dirty.h): "a dirty block differs from the last bank sent" holds only after an emit followed a refresh that started from a real copy of
  // the device (not the pattern fill of a new device pointer); the literal table's serial at the last emit; the switch, read once a frame.
  cap::bank_dirty::State bd;  // dirty_exact, the literal serials (bank_dirty.h: the unit test runs these very calls)
  uint64_t sw_frame = ~0ull;
  bool sw_on = false;
  uint64_t declined_frame = ~0ull;
};
inline BankCache g_bank_cache;
inline census_flush::Site g_cf_bank_dirty, g_cf_tail;   // S102 CEN1
inline cap::bank_dirty::Counters g_bank_dirty;  // S102 T12: the census (the [rdr][bank_dirty] line every 600 frames while on)
// S102 T12: the switch, read once a frame by the first draw of it (a PerFrame switch of the S0-4 table, acknowledged with a [switch] seen line).
inline bool BankDirtyOn(BankCache& c) {
  const uint64_t guest_frame = wwe2k14::g_guest_frame.load(std::memory_order_relaxed);  // F5: keyed on the guest frame (a frame part's draws all see one value)
  if (c.sw_frame == guest_frame) return c.sw_on;
  c.sw_frame = guest_frame;
  c.sw_on = rdr_sw::BoolSeen(rdr_sw::kBankDirty, false, guest_frame);
  if (c.sw_on) {
    cap::bank_dirty::Counters& k = g_bank_dirty;
    if (++k.frames % 600 == 0 || census_flush::Due(g_cf_bank_dirty)) {   // S102 CEN1
      REXLOG_INFO(
          "[rdr][bank_dirty] frames {}, taken {} emits, declined {} emits (device pointer changed: dirty not exact; {} frames); blocks: old plan {}, built {}, compared {}, skipped unchanged {}, "
          "patch without compare {}; refresh compares inline {}; patches {}",
          k.frames, k.emits - k.emits_old_plan, k.emits_old_plan, k.frames_declined, k.blocks_touch, k.blocks_built, k.blocks_compared, k.blocks_skipped_same, k.blocks_skipped_known,
          k.refresh_blocks, k.patches);
    }
  }
  return c.sw_on;
}

// S100 speed: which 64-byte constant blocks the library marked dirty since the reader last refreshed its banks (bit b = block b).
// The library's writers copy into the shadows and OR the block's bit (bit 63 - block, big-endian order) into the device's masks, +0
// vertex and +8 pixel: SetVertex/PixelShaderConstantF 0x82CB0658 / 0x82CB0730 and the single-float writers (0x82BB23C0 / 0x82BB2460:
// listings). The masks are consumed by the float flush 0x82CBD7C0 (r4 the mask, r5 the first register: 0x4000 vertex, 0x4400 pixel;
// half2_hooks.h calls OnFloatFlush) or, with the native layer on, cleared by its indexed draw and inline Begin, after the reader's hooks
// (which read them first: OnDrawIndexed's PushBanks, OnBeginVertices). So the blocks that can differ at a draw = what the flushes took
// since + what the masks hold now. A full compare at each frame part's first draw counts any change outside the marks
// (bank_change_unmarked_at_frame_start); --wwe2k14_rdr_debug_bank_verify compares fully at every draw and counts them.
inline std::atomic<uint64_t> g_bank_marked[2];
inline uint64_t BlocksOfDeviceMask(uint64_t device_mask) { return __builtin_bitreverse64(device_mask); }
inline void OnFloatFlush(uint64_t mask, uint32_t first_register) {
  if (!mask) return;
  if (first_register == 0x4000 || first_register == 0x4400) {
    g_bank_marked[first_register == 0x4400].fetch_or(BlocksOfDeviceMask(mask), std::memory_order_relaxed);
    return;
  }
  // another first register: each bit's 16 registers, the block(s) they fall in
  for (uint64_t m = mask; m; m &= m - 1) {
    const uint32_t i = 63u - uint32_t(std::countr_zero(m));
    for (uint32_t reg : {first_register + 16 * i, first_register + 16 * i + 15}) {
      if (reg < 0x4000 || reg >= 0x4800) continue;
      const uint32_t block = (reg - 0x4000) / 16;
      g_bank_marked[block >> 6].fetch_or(1ull << (block & 63), std::memory_order_relaxed);
    }
  }
}
inline void NoteDeviceFloatMasks(const uint8_t* base, uint32_t dev) {
  if (const uint64_t m = db::Ld64(base, dev + 0)) g_bank_marked[0].fetch_or(BlocksOfDeviceMask(m), std::memory_order_relaxed);
  if (const uint64_t m = db::Ld64(base, dev + 8)) g_bank_marked[1].fetch_or(BlocksOfDeviceMask(m), std::memory_order_relaxed);
}

// The 64-byte blocks of `candidates` whose device bytes differ from our copy.
inline uint64_t ChangedBankBlocks(const uint8_t* device_raw, const uint8_t* raw, uint64_t candidates) {
  uint64_t mask = 0;
  for (uint64_t m = candidates; m; m &= m - 1) {
    const uint32_t b = uint32_t(std::countr_zero(m));
    if (std::memcmp(device_raw + 64 * b, raw + 64 * b, 64) != 0) mask |= 1ull << b;
  }
  return mask;
}
// Our copy of the blocks in `mask` refreshed from the device (raw, and as host-order dwords).
inline void ApplyBankBlocks(const uint8_t* device_raw, uint8_t* raw, uint32_t* dwords, uint64_t mask, bool& changed, uint64_t& dirty) {
  if (!mask) return;
  for (uint64_t m = mask; m; m &= m - 1) {
    const uint32_t b = uint32_t(std::countr_zero(m));
    const uint8_t* d = device_raw + 64 * b;
    std::memcpy(raw + 64 * b, d, 64);
    for (uint32_t k = 0; k < 16; ++k) {
      uint32_t v;
      std::memcpy(&v, d + 4 * k, 4);
      dwords[16 * b + k] = __builtin_bswap32(v);
    }
  }
  dirty |= mask;
  changed = true;
}

// S100 speed: one pass over the 4 KB builds the mask of changed 64-byte blocks (was a 4 KB memcmp, then 64 block memcmps whenever any
// block changed: ~0.35 us a draw for both banks in WM30's match, run 6927). Returns the changed blocks.
inline uint64_t RefreshBankBlocks(const uint8_t* device_raw, uint8_t* raw, uint32_t* dwords, bool& changed, uint64_t& dirty) {
  uint64_t mask = 0;
#if defined(_M_X64) || defined(__x86_64__)
  // SSE2 (every x64 CPU): run 6931 measured the 64-bit loop below at ~0.27 us a draw for both banks (not vectorised).
  const __m128i zero = _mm_setzero_si128();
  for (uint32_t b = 0; b < 64; ++b) {
    const uint8_t* p = device_raw + 64 * b;
    const uint8_t* q = raw + 64 * b;
    const __m128i x0 = _mm_xor_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p)), _mm_loadu_si128(reinterpret_cast<const __m128i*>(q)));
    const __m128i x1 = _mm_xor_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 16)), _mm_loadu_si128(reinterpret_cast<const __m128i*>(q + 16)));
    const __m128i x2 = _mm_xor_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 32)), _mm_loadu_si128(reinterpret_cast<const __m128i*>(q + 32)));
    const __m128i x3 = _mm_xor_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 48)), _mm_loadu_si128(reinterpret_cast<const __m128i*>(q + 48)));
    const __m128i x = _mm_or_si128(_mm_or_si128(x0, x1), _mm_or_si128(x2, x3));
    mask |= uint64_t(_mm_movemask_epi8(_mm_cmpeq_epi8(x, zero)) != 0xFFFF) << b;
  }
#else
  for (uint32_t b = 0; b < 64; ++b) {
    uint64_t x = 0;
    for (uint32_t k = 0; k < 8; ++k) {
      uint64_t p, q;
      std::memcpy(&p, device_raw + 64 * b + 8 * k, 8);
      std::memcpy(&q, raw + 64 * b + 8 * k, 8);
      x |= p ^ q;
    }
    mask |= uint64_t(x != 0) << b;
  }
#endif
  ApplyBankBlocks(device_raw, raw, dwords, mask, changed, dirty);
  return mask;
}

// The literal constants of a shader's table as (dword, value) entries in table order: dword < 1024 the vertex bank, 1024-2047 the pixel
// bank (the same walk and bounds as OverlayLiterals; applying the entries in order equals the overlay).
struct LitEntry {
  uint32_t dword, value;
};
inline void LiteralList(const uint8_t* base, uint32_t rec, uint32_t data_base, std::vector<LitEntry>& out) {
  const uint32_t off = db::Ld32(base, rec + 20);
  if (!off) return;
  const uint32_t table = rec + off;
  uint32_t at = table + 20;
  const uint32_t length = db::Ld32(base, table + 16);
  if (length > 65536) {
    Note("literal_table_implausible");
    return;
  }
  const uint32_t end = at + length;
  while (at < end) {
    const uint32_t index = db::Ld16(base, at), count = db::Ld16(base, at + 2);
    if (!count) break;
    if (count > 256 || index > 512) {
      Note("literal_entry_implausible");
      return;
    }
    const uint32_t src = db::Ld32(base, at + 4) + data_base;
    at += 8;
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t reg = index * 4 + i;
      if (reg < 2048) out.push_back({reg, db::Ld32(base, src + 4 * i)});
    }
  }
}

inline uint32_t PushBanks(uint8_t* base, uint32_t dev, const VertexShaderInfo* vs, uint32_t ps_object, uint32_t& ps_bank_out) {
  BankCache& c = g_bank_cache;
  const uint8_t* vs_dev = db::Host(base, dev + kDevVsFloat);
  const uint8_t* ps_dev = db::Host(base, dev + kDevPsFloat);
  bool changed = false;
  const bool dirty_on = BankDirtyOn(c);  // S102 T12
  bool full = REXCVAR_GET(wwe2k14_rdr_debug_bank_verify) || !REXCVAR_GET(wwe2k14_rdr_bank_marks) || c.full_generation != g_frame.generation;
  if (!c.valid || c.dev != dev) {
    std::memset(c.vs_raw, 0xA5, sizeof(c.vs_raw));  // differs from anything: every block converted below
    std::memset(c.ps_raw, 0xA5, sizeof(c.ps_raw));
    c.dev = dev;
    c.valid = true;
    c.bd.OnDeviceChange();  // S102 T12: every block reads as changed against the pattern, not against the last bank sent (always, switch on or off)
    full = true;
    c.full_generation = ~0ull;  // (no count of unmarked changes against the fill pattern)
  }
  {
    FineNs fine(g_fine.bank_refresh);
    // The blocks the library marked since the last refresh (S100 speed): its flushes' masks and the device's masks now.
    NoteDeviceFloatMasks(base, dev);
    const uint64_t marked[2] = {g_bank_marked[0].exchange(0, std::memory_order_relaxed), g_bank_marked[1].exchange(0, std::memory_order_relaxed)};
    if (full) {
      const bool count = c.full_generation != ~0ull;
      const uint64_t vs_changed = RefreshBankBlocks(vs_dev, c.vs_raw, c.vs_base.dwords, changed, c.vs_dirty);
      const uint64_t ps_changed = RefreshBankBlocks(ps_dev, c.ps_raw, c.ps_base.dwords, changed, c.ps_dirty);
      if (const uint64_t unmarked = count ? std::popcount(vs_changed & ~marked[0]) + std::popcount(ps_changed & ~marked[1]) : 0) {
        NoteCounted(c.full_generation == g_frame.generation ? "bank_change_unmarked" : "bank_change_unmarked_at_frame_start", unmarked);
      }
      c.full_generation = g_frame.generation;
    } else {
      // S102 T12: the same blocks, compared inline (bank_dirty.h Equal64) when the switch is on
      ApplyBankBlocks(vs_dev, c.vs_raw, c.vs_base.dwords, dirty_on ? cap::bank_dirty::ChangedBlocks(vs_dev, c.vs_raw, marked[0], true, &g_bank_dirty) : ChangedBankBlocks(vs_dev, c.vs_raw, marked[0]), changed, c.vs_dirty);
      ApplyBankBlocks(ps_dev, c.ps_raw, c.ps_base.dwords, dirty_on ? cap::bank_dirty::ChangedBlocks(ps_dev, c.ps_raw, marked[1], true, &g_bank_dirty) : ChangedBankBlocks(ps_dev, c.ps_raw, marked[1]), changed, c.ps_dirty);
    }
  }
  if (changed) ++c.version;
  const uint32_t vs_object = db::Ld32(base, dev + kDevVs);
  const uint32_t frame = uint32_t(g_record_frame);
  // The bank indices are per frame (g_frame is reset at each present): reuse only within the frame.
  if (c.built_version == c.version && c.built_vs_object == (vs ? vs_object : 0) && c.built_ps_object == ps_object && c.built_frame == frame) {
    g_fine.banks_reused += g_fine_on;
    ps_bank_out = c.ps_index;
    return c.vs_index;
  }
  // The literal constants the bind loads (the same records as d3d_shader_bind.h's Plan: vs + 872, ps + 40), as (dword, value) entries in
  // the overlay's order (VS table, then PS table; a later entry wins).
  // S100 speed: kept for the last shader pair within the frame tables' generation (run 6927: 95% of WM30's match draws repeat the last
  // draw's pair). The key holds what the walk starts from: both objects, their data bases and table offsets (a shader object is not
  // rewritten between two draws of one frame; a reused address with a new shader brings a new data base or table).
  struct LitCache {
    uint64_t generation = ~0ull;
    uint32_t key[6] = {};
    std::vector<LitEntry> lits;
    uint64_t mask[2] = {0, 0};
    std::vector<LitEntry> by_block[2];  // per stage, ordered by block, table order kept within a block (a later entry wins)
    uint32_t start[2][65] = {};
  };
  static LitCache lc;
  const std::vector<LitEntry>& lits = lc.lits;
  {
    FineNs fine(g_fine.bank_lits);
    const bool literals = !(REXCVAR_GET(wwe2k14_rdr_debug_live_skip) & 8);  // diagnosis bit 8: no literal tables read
    const uint32_t vs_lit = literals && vs ? vs_object : 0, ps_lit = literals ? ps_object : 0;
    const uint32_t key[6] = {vs_lit, ps_lit, vs_lit ? db::Ld32(base, vs_lit + 32) : 0, ps_lit ? db::Ld32(base, ps_lit + 24) : 0,
                             vs_lit ? db::Ld32(base, vs_lit + 872 + 20) : 0, ps_lit ? db::Ld32(base, ps_lit + 40 + 20) : 0};
    if (lc.generation == g_frame.generation && std::memcmp(key, lc.key, sizeof(key)) == 0) {
      ++g_times.lits_reused;
    } else {
      lc.generation = g_frame.generation;
      std::memcpy(lc.key, key, sizeof(key));
      lc.lits.clear();
      c.bd.OnLitRebuild();  // S102 T12: the literal table changed (same block as the clear that starts the rebuild)
      if (vs_lit) LiteralList(base, vs_lit + 872, key[2], lc.lits);
      if (ps_lit) LiteralList(base, ps_lit + 40, key[3], lc.lits);
      lc.mask[0] = lc.mask[1] = 0;
      uint32_t count[2][64] = {};
      for (const LitEntry& e : lc.lits) {
        lc.mask[e.dword >> 10] |= 1ull << ((e.dword & 1023) >> 4);
        ++count[e.dword >> 10][(e.dword & 1023) >> 4];
      }
      for (uint32_t s = 0; s < 2; ++s) {
        lc.start[s][0] = 0;
        for (uint32_t b = 0; b < 64; ++b) lc.start[s][b + 1] = lc.start[s][b] + count[s][b];
        lc.by_block[s].resize(lc.start[s][64]);
        uint32_t fill[64];
        std::memcpy(fill, lc.start[s], sizeof(fill));
        for (const LitEntry& e : lc.lits) {
          if ((e.dword >> 10) == s) lc.by_block[s][fill[(e.dword & 1023) >> 4]++] = e;
        }
      }
    }
  }
  const uint64_t lit_mask[2] = {lc.mask[0], lc.mask[1]};
  if (g_fine_on) {
    g_fine.lits += lits.size();
    g_fine.touched += std::popcount(c.vs_dirty | lit_mask[0] | c.vs_lit_last) + std::popcount(c.ps_dirty | lit_mask[1] | c.ps_lit_last);
  }
  FineNs emit_fine(g_fine.bank_emit);
  // As changes against this stage's previous bank in the frame (cap::BankDef): the first of each stage in a frame goes whole.
  if (c.defs_frame != frame) {
    c.defs_frame = frame;
    c.vs_def = c.ps_def = cap::kBankNoBase;
  }
  // One stage's bank: the device floats with this draw's literals for that stage (stage 0 vertex, 1 pixel).
  auto block_of = [&](const cap::ConstantBank& base_bank, uint32_t stage, uint32_t b, uint32_t out[16]) {
    std::memcpy(out, base_bank.dwords + 16 * b, 64);
    for (uint32_t i = lc.start[stage][b]; i < lc.start[stage][b + 1]; ++i) out[lc.by_block[stage][i].dword & 15] = lc.by_block[stage][i].value;
  };
  if (dirty_on) {
    g_bank_dirty.emits += 2;  // (a call is two stage emits)
    if (!c.bd.dirty_exact) {
      g_bank_dirty.emits_old_plan += 2;
      g_bank_dirty.frames_declined += c.sw_frame != c.declined_frame;
      c.declined_frame = c.sw_frame;
    }
  }
  auto emit = [&](const cap::ConstantBank& base_bank, uint32_t stage, uint64_t dirty, uint64_t lit_now, uint64_t lit_last, cap::ConstantBank& last, uint32_t& last_def) -> uint32_t {
    FrameTables& t = g_frame;
    if (last_def == cap::kBankNoBase) {
      cap::BankDef d;
      d.full = uint32_t(t.constant_banks.size());
      cap::ConstantBank& full = t.constant_banks.emplace_back(base_bank);
      for (const LitEntry& e : lits) {
        if ((e.dword >> 10) == stage) full.dwords[e.dword & 1023] = e.value;
      }
      last = full;
      last_def = uint32_t(t.bank_defs.size());
      t.bank_defs.push_back(d);
      return last_def;
    }
    // S102 T12: the blocks this emit builds and compares (bank_dirty.h: the old set = dirty | literal blocks, or fewer with the switch on)
    const cap::bank_dirty::Plan plan = c.bd.PlanFor(dirty_on, dirty, lit_now, lit_last);
    last_def = cap::bank_dirty::EmitPatches(
        t.bank_defs, t.bank_patches, plan, dirty_on ? uint64_t(std::popcount(dirty | lit_now | lit_last)) : 0, [&](uint32_t b, uint32_t out[16]) { block_of(base_bank, stage, b, out); }, last, last_def,
        dirty_on ? &g_bank_dirty : nullptr);
    return last_def;
  };
  c.vs_index = emit(c.vs_base, 0, c.vs_dirty, lit_mask[0], c.vs_lit_last, c.vs_last, c.vs_def);
  c.ps_index = emit(c.ps_base, 1, c.ps_dirty, lit_mask[1], c.ps_lit_last, c.ps_last, c.ps_def);
  c.bd.AfterEmit();  // S102 T12
  c.vs_dirty = c.ps_dirty = 0;
  c.vs_lit_last = lit_mask[0];
  c.ps_lit_last = lit_mask[1];
  c.built_version = c.version;
  c.built_vs_object = vs ? vs_object : 0;
  c.built_ps_object = ps_object;
  c.built_frame = frame;
  ps_bank_out = c.ps_index;
  return c.vs_index;
}
