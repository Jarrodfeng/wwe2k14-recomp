
// ---- shaders: facts from the microcode (docs: rex\graphics\format\ucode.h) ------------------------------------------------------------
struct TFetch {
  uint8_t used = 0;
  uint8_t mag = 3, min = 3, mip = 3, aniso = 7;  // 3 / 7 = use the fetch constant
  uint8_t dimension = 1;
  uint8_t pad = 0;
};
struct VFetch {
  uint32_t fetch_constant = 0;
  uint32_t format = 0;
  uint32_t stride = 0;       // dwords
  int32_t offset = 0;        // dwords
  bool is_signed = false, normalized = true, mini = false;
  int32_t exp_adjust = 0;
  uint32_t rounding = 0;
  uint8_t dest = 0xFF;         // the register it writes, and its component selects (3 bits per destination component: 0-3 X-W, 4 zero, 5 one, 7 keep)
  uint16_t dest_swizzle = 0;
  uint16_t original_dest_swizzle = 0xFFFF;  // the ORIGINAL microcode's selects for the same fetch (0xFFFF: not known); the patched copy can differ
};
struct ShaderFacts {
  std::vector<VFetch> vfetches;
  std::array<TFetch, 32> tfetch{};
};

// Walks the control flow and each exec's instructions (big-endian dwords in guest order).
inline void ParseMicrocode(const uint8_t* words, uint32_t dwords, bool vertex, ShaderFacts& out) {
  uint32_t bound = dwords / 3;  // control-flow pairs precede the first exec's address (both in 3-dword units)
  for (uint32_t pair = 0; pair < bound; ++pair) {
    const uint32_t w0 = LoadBE32(words + 12 * pair), w1 = LoadBE32(words + 12 * pair + 4), w2 = LoadBE32(words + 12 * pair + 8);
    const uint64_t cf[2] = {uint64_t(w0) | (uint64_t(w1 & 0xFFFF) << 32), uint64_t(w1 >> 16) | (uint64_t(w2) << 16)};
    for (uint64_t c : cf) {
      const uint32_t opcode = uint32_t(c >> 44) & 0xF;
      if ((opcode < 1 || opcode > 6) && opcode != 13 && opcode != 14) continue;  // exec, exec end, cond exec, cond exec end, cond exec pred, cond exec pred end, and the two pred-clean forms
      const uint32_t address = uint32_t(c) & 0xFFF, count = (uint32_t(c) >> 12) & 7, sequence = (uint32_t(c) >> 16) & 0xFFF;
      if (!count) continue;
      bound = std::min(bound, address);
      for (uint32_t k = 0; k < count; ++k) {
        if (!((sequence >> (2 * k)) & 1)) continue;  // ALU
        const uint32_t at = (address + k) * 3;
        if (at + 3 > dwords) continue;
        const uint32_t f0 = LoadBE32(words + 4 * at), f1 = LoadBE32(words + 4 * at + 4), f2 = LoadBE32(words + 4 * at + 8);
        const uint32_t fop = f0 & 0x1F;
        if (fop == 0) {  // vfetch
          if (!vertex) {
            Note("pixel_shader_vfetch");
            continue;
          }
          VFetch v;
          v.fetch_constant = ((f0 >> 20) & 0x1F) * 3 + ((f0 >> 25) & 3);
          v.is_signed = (f1 >> 12) & 1;
          v.normalized = ((f1 >> 13) & 1) == 0;
          v.rounding = (f1 >> 14) & 3;
          v.format = (f1 >> 16) & 0x3F;
          v.exp_adjust = int32_t(f1 << 2) >> 26;
          v.mini = (f1 >> 30) & 1;
          v.stride = f2 & 0xFF;
          v.offset = int32_t(f2 << 1) >> 9;
          v.dest = uint8_t((f0 >> 12) & 0x3F);
          v.dest_swizzle = uint16_t(f1 & 0xFFF);
          out.vfetches.push_back(v);
        } else if (fop == 1 || fop == 16 || fop == 17 || fop == 18 || fop == 19) {  // tfetch and the texture queries
          const uint32_t slot = (f0 >> 20) & 0x1F;
          TFetch t;
          t.used = 1;
          t.mag = (f1 >> 12) & 3;
          t.min = (f1 >> 14) & 3;
          t.mip = (f1 >> 16) & 3;
          t.aniso = (f1 >> 18) & 7;
          t.dimension = (f2 >> 14) & 3;
          TFetch& have = out.tfetch[slot];
          if (have.used && (have.mag != t.mag || have.min != t.min || have.mip != t.mip || have.aniso != t.aniso)) {
            Note("tfetch_filter_differs_on_one_slot");  // the first fetch's filters are kept
          }
          if (!have.used) have = t;
        }
      }
    }
  }
}

struct VertexShaderInfo {
  uint64_t original_hash = 0, patched_hash = 0;
  ShaderFacts facts;
  // the layout, once
  std::vector<cap::VertexElement> elements;
  // Per element: host component k takes the data's component select[k] ({0,1,2,3}: as fetched). The hand-written shaders read each
  // input as the ORIGINAL microcode's fetch delivers it; the library's patch can change a fetch's component selects (D3DCOLOR: red and
  // blue exchanged), so the converted data carries the patched selects in the original's terms (BuildLayout).
  std::vector<std::array<uint8_t, 4>> element_select;
  uint32_t host_stride = 0, guest_stride = 0, fetch_constant = 95;
  uint64_t layout_hash = 0;
};
struct PixelShaderInfo {
  uint64_t hash = 0;
  ShaderFacts facts;
  bool writes_depth = false;
};

// The one pixel shader that exports depth (D10, census CU).
inline constexpr uint64_t kDepthExportPs = 0x9A836B68E3F8F0B0ull;

// Element sizes in the converted vertex, and what one element becomes (V5-V12).
struct HostElement {
  uint16_t vk;
  uint16_t bytes;
  uint8_t components;
};
inline bool HostOfVertexFormat(const VFetch& v, HostElement& h) {
  switch (v.format) {
    case 38: h = {kVkRGBA32Float, 16, 4}; return true;
    case 57: h = {kVkRGB32Float, 12, 3}; return true;
    case 37: h = {kVkRG32Float, 8, 2}; return true;
    case 36: h = {kVkR32Float, 4, 1}; return true;
    case 31: h = {kVkRG16Float, 4, 2}; return true;
    case 32: h = {kVkRGBA16Float, 8, 4}; return true;
    case 6:  // V6: bytes in order after the swap
      if (!v.normalized) {
        h = {v.is_signed ? kVkRGBA8Sint : kVkRGBA8Uint, 4, 4};  // V8: float(value) is the shader's job
      } else {
        h = {v.is_signed ? kVkRGBA8Snorm : kVkRGBA8Unorm, 4, 4};
      }
      return true;
    case 16:
    case 17:  // V9-V10: converted to 16-bit normalised, w = 0
      h = {v.is_signed ? kVkRGBA16Snorm : kVkRGBA16Unorm, 8, 3};
      return true;
    case 25:
      h = {v.is_signed ? kVkRG16Snorm : kVkRG16Unorm, 4, 2};
      return true;
    case 26:
      h = {v.is_signed ? kVkRGBA16Snorm : kVkRGBA16Unorm, 8, 4};
      return true;
    case 7:
      h = {kVkRGB10A2Unorm, 4, 4};
      return true;
    default:
      return false;
  }
}

inline void BuildLayout(VertexShaderInfo& info) {
  uint32_t host_offset = 0;
  bool have_full = false;
  std::vector<std::pair<uint64_t, uint16_t>> placed;  // (guest offset + format) -> host offset: two fetches of one element share it
  for (const VFetch& v : info.facts.vfetches) {
    if (!v.mini && !have_full) {
      have_full = true;
      info.guest_stride = v.stride;
      info.fetch_constant = v.fetch_constant;
    } else if (!v.mini) {
      if (v.fetch_constant != info.fetch_constant) Note("vfetch_second_stream");
      if (v.stride != info.guest_stride) Note("vfetch_second_stride");
    }
    if (v.exp_adjust) Note("vfetch_exp_adjust");
    HostElement h{};
    if (!HostOfVertexFormat(v, h)) {
      Note("vfetch_format_unknown", v.format);
      h = {kVkR32Float, 4, 1};
    }
    if (v.format != 38 && v.format != 57 && v.format != 37 && v.format != 36 && v.format != 31 && v.format != 6 && v.format != 17) Note("vfetch_format_outside_census", v.format);
    // The patched fetch's selects in the original's terms (S100: the ramp lights drew blue where the game draws red). Destination component
    // c gets data component patched[c] on the GPU; our shader applies original[c] to what we hand it, so host component original[c] must
    // hold data component patched[c]. Selects 0-3 are X-W, 4 zero, 5 one, 7 not written.
    // Run 6849: D3DCOLOR fetches come patched xyzw -> zyxw (red / blue), every k_16_16_FLOAT pair with its two halves exchanged (the
    // "high half first" of the morning was this), and fetches of elements the declaration lacks patched to constants (0,0,0,1): select 4
    // writes 0, 5 writes 1.
    std::array<uint8_t, 4> select = {0, 1, 2, 3};
    if (v.original_dest_swizzle != 0xFFFF && v.original_dest_swizzle != v.dest_swizzle) {
      for (uint32_t c = 0; c < 4; ++c) {
        const uint32_t o = (v.original_dest_swizzle >> (3 * c)) & 7, p = (v.dest_swizzle >> (3 * c)) & 7;
        if (o <= 3 && (p <= 3 || p == 4 || p == 5)) select[o] = uint8_t(p);
        else if (o != p && o <= 5) Note("vfetch_patched_selects_unmapped", (uint32_t(v.original_dest_swizzle) << 16) | v.dest_swizzle);
      }
      if (select != std::array<uint8_t, 4>{0, 1, 2, 3}) Note("vfetch_patched_selects_applied", (uint32_t(v.format) << 24) | (uint32_t(v.original_dest_swizzle) << 12) | v.dest_swizzle);
    }
    const uint64_t key = (uint64_t(uint32_t(v.offset)) << 32) | (uint64_t(select[0] | select[1] << 3 | select[2] << 6 | select[3] << 9) << 12) | (uint64_t(v.format) << 2) |
                         (v.is_signed ? 2 : 0) | (v.normalized ? 1 : 0);
    uint16_t at = 0xFFFF;
    for (const auto& [k, off] : placed) {
      if (k == key) at = off;
    }
    if (at == 0xFFFF) {
      at = uint16_t(host_offset);
      placed.emplace_back(key, at);
      host_offset += h.bytes;
    }
    cap::VertexElement e;
    e.vk_format = h.vk;
    e.host_offset = at;
    e.source_format = uint8_t(v.format);
    e.components = h.components;
    e.flags = uint8_t((v.is_signed ? cap::kElementSigned : 0) | (v.normalized ? 0 : cap::kElementInteger));
    e.mini = v.mini ? 1 : 0;
    e.guest_offset = v.offset;
    info.elements.push_back(e);
    info.element_select.push_back(select);
  }
  info.host_stride = host_offset;
  info.layout_hash = db::Hash64(reinterpret_cast<const uint8_t*>(info.elements.data()), info.elements.size() * sizeof(cap::VertexElement)) ^ (uint64_t(info.host_stride) << 40) ^
                     db::Hash64(reinterpret_cast<const uint8_t*>(info.element_select.data()), info.element_select.size() * 4);
}

struct VsKey {
  uint64_t code;  // the microcode's bytes: a reused object address with new microcode is a different shader
  uint32_t vs, decl;
  uint8_t seven_c;
  uint8_t strides[24];
  uint8_t pad[7];
  bool operator==(const VsKey& o) const { return std::memcmp(this, &o, sizeof(VsKey)) == 0; }
};
struct VsKeyHash {
  size_t operator()(const VsKey& k) const { return size_t(db::Hash64(reinterpret_cast<const uint8_t*>(&k), sizeof(k))); }
};
inline std::unordered_map<VsKey, VertexShaderInfo, VsKeyHash> g_vs;
inline std::unordered_map<uint64_t, PixelShaderInfo> g_ps;  // by object address and microcode bytes

// The vertex shader as it is for this draw: which microcode the library would bind (the same decisions as Plan() in d3d_shader_bind.h) and
// its facts. `ctx` is only used for the guest stack below r1 by the library's patch routine; every register is restored.
// S100 speed: the last draw's shaders, kept within a frame part (bumped by FrameTables::Reset). Run 6927: 81% of WM30's match draws repeat
// the last draw's vertex shader key and 95% its shader pair, and each lookup hashed the whole microcode. A repeat must match the object,
// declaration, variant bit, strides, the microcode's address and size and its first 32 bytes (a shader object made again at the same address
// inside one frame part would have to match all of them). In owner mode the library no longer patches microcode in place (OwnerNullDraw);
// elsewhere a repeat of the same key gives the same facts (the original is rebuilt from the pristine fetch words, the patch from them).
inline uint64_t g_shader_repeat_epoch = 1;
struct LastShader {
  uint64_t epoch = 0;
  uint32_t object = 0, decl = 0, code_addr = 0, bytes = 0;
  bool seven_c = false;
  uint8_t strides[24] = {};
  uint8_t head[32] = {};
  const void* info = nullptr;
};
inline LastShader g_last_vs, g_last_ps;
inline rdr_adapter::draw_memos::ShaderTable<VertexShaderInfo> g_vs_memo;   // S102 T14 (wwe2k14_rdr_shader_memo)
inline rdr_adapter::draw_memos::ShaderTable<PixelShaderInfo> g_ps_memo;
// S102 T14: `[rdr][<lever>] frames A-B, taken T, declined D (by reason)` for each memo that is on (a Surface "no gain" must be told apart from "took 0").
inline void DrawMemosLog(uint64_t from, uint64_t to) {
  if (rdr_adapter::draw_memos::g_note_on.load(std::memory_order_relaxed)) {
    const rdr_adapter::draw_memos::NoteStats& s = g_note_memo.stats;
    REXLOG_INFO("[rdr][note_memo] frames {}-{}, taken {}, declined {} (first sightings, slot clashes and names over 47 characters; of those names {}), cells remembered {}", from, to, s.hits, s.misses, s.declined_long, s.remembered);
  }
  g_note_memo.stats = {};
  if (rdr_adapter::draw_memos::g_shader_serial_on.load(std::memory_order_relaxed)) {   // S102 T14b: names built here, every 600 frames; the counters are plain fields
    const rdr_adapter::draw_memos::ShaderStats& v = g_vs_memo.stats;
    const rdr_adapter::draw_memos::ShaderStats& p = g_ps_memo.stats;
    REXLOG_INFO("[rdr][shader_serial] frames {}-{}, trusted {} (vs {}, ps {}: the epoch rule, head compare only), verified {} (vs {}, ps {}: first hit of an entry in a frame part, whole compare), declined {} (not in the table: hash and find)", from, to,
                v.trusted + p.trusted, v.trusted, p.trusted, v.verified + p.verified, v.verified, p.verified, v.misses + p.misses);
  }
  if (rdr_adapter::draw_memos::g_shader_on.load(std::memory_order_relaxed)) {
    const rdr_adapter::draw_memos::ShaderStats& v = g_vs_memo.stats;
    const rdr_adapter::draw_memos::ShaderStats& p = g_ps_memo.stats;
    REXLOG_INFO("[rdr][shader_memo] frames {}-{}, taken {} (vs {}, ps {}), declined {} (vs {}, ps {}: the one-entry memo missed and the table did not know the shader; microcode over {} bytes {}), remembered {}", from, to, v.hits + p.hits, v.hits, p.hits,
                v.misses + p.misses, v.misses, p.misses, rdr_adapter::draw_memos::ShaderTable<VertexShaderInfo>::kMaxBytes, v.declined_big + p.declined_big, v.remembered + p.remembered);
  }
  if (wwe2k14::upd_lean::g_mask.load(std::memory_order_relaxed)) {   // S102 U2: the census, one line per window while the lane switch is on (cells are cumulative; relaxed, approximate)
    namespace ul = wwe2k14::upd_lean;
    REXLOG_INFO("[rdr][upd_lean] frames {}-{}, mask {}: marks reports {}, origin intern (memo {}, direct {}, map {}), lights (fast {}, full {}), arena memo {}; round 2: consumer palettes in place {}, arena frame swaps {} [cumulative; counted only while a census is on; EntryUnknownModel / EntryPieces read 0 under bit 4]", from, to, ul::g_mask.load(std::memory_order_relaxed),
                ul::Get(ul::kMarkReports), ul::Get(ul::kInternMemo), ul::Get(ul::kInternDirect), ul::Get(ul::kInternMap), ul::Get(ul::kLightFast), ul::Get(ul::kLightFull),
                ul::Get(ul::kArenaMemo), ul::Get(ul::kConsumerInPlace), ul::Get(ul::kArenaSwap));
  }
  if (rdr_adapter::origin::g_index_on.load(std::memory_order_relaxed)) {   // S102 T14b: the reader thread's own counters (plain, per thread)
    const rdr_adapter::origin::IndexStats& o = rdr_adapter::origin::t_index_stats;
    REXLOG_INFO("[rdr][origin_index] frames {}-{}, taken {} (lookups answered by the id), declined {} (not one of our tags {}, id past the published count {})", from, to, o.taken, o.bad_tag + o.past_count, o.bad_tag, o.past_count);
  }
  rdr_adapter::origin::t_index_stats = {};
  g_vs_memo.stats = {};
  g_ps_memo.stats = {};
  if (rdr_adapter::draw_memos::g_pin_hash_on.load(std::memory_order_relaxed)) {
    REXLOG_INFO("[rdr][pin_hash] frames {}-{}, taken {} (queries answered by the table), declined {} (no table: a set made outside Publish)", from, to, rdr_adapter::draw_memos::g_pin_stats.taken, rdr_adapter::draw_memos::g_pin_stats.declined);
  }
  rdr_adapter::draw_memos::g_pin_stats = {};
}


inline const VertexShaderInfo* GetVertexShader(PPCContext& ctx, uint8_t* base, uint32_t dev, uint32_t vs, const uint8_t strides[24]) {
  VsKey key{};
  key.vs = vs;
  key.decl = db::Ld32(base, dev + kDevDecl);
  key.seven_c = (base[dev + 11070] & 0x80) != 0;
  std::memcpy(key.strides, strides, 24);
  const uint32_t rec = db::VariantRecord(base, vs, 0);
  const uint32_t offset = db::Ld32(base, rec + 872), bytes = db::Ld32(base, rec + 876);
  if (!bytes || (bytes & 3) || bytes > (256u << 10)) {
    Note("vs_bad_microcode_size");
    return nullptr;
  }
  const uint32_t code_addr = db::Ld32(base, vs + 32) + offset;
  const uint8_t* source = db::Host(base, code_addr);
  LastShader& last = g_last_vs;
  const uint32_t head = std::min<uint32_t>(bytes, 32);
  if (last.info && last.epoch == g_shader_repeat_epoch && last.object == vs && last.decl == key.decl && last.seven_c == key.seven_c &&
      last.code_addr == code_addr && last.bytes == bytes && std::memcmp(last.strides, strides, 24) == 0 && std::memcmp(last.head, source, head) == 0) {
    return static_cast<const VertexShaderInfo*>(last.info);
  }
  const uint64_t serial_epoch = rdr_adapter::draw_memos::g_shader_serial_on.load(std::memory_order_relaxed) ? g_shader_repeat_epoch : 0;   // S102 T14b: 0 = whole compare at every hit
  const bool memo_on = serial_epoch || rdr_adapter::draw_memos::g_shader_on.load(std::memory_order_relaxed);   // S102 T14 (T14b: the serial switch implies the table)
  rdr_adapter::draw_memos::ShaderTable<VertexShaderInfo>::Key memo_key;
  if (memo_on) memo_key = rdr_adapter::draw_memos::ShaderTable<VertexShaderInfo>::MakeKey(vs, key.decl, code_addr, bytes, key.seven_c != 0, strides);   // F4
  auto remember = [&](const VertexShaderInfo* info, bool into_table = true) {
    last.epoch = g_shader_repeat_epoch;
    last.object = vs;
    last.decl = key.decl;
    last.seven_c = key.seven_c;
    last.code_addr = code_addr;
    last.bytes = bytes;
    std::memcpy(last.strides, strides, 24);
    std::memcpy(last.head, source, head);
    last.info = info;
    if (memo_on && into_table) g_vs_memo.Remember(memo_key, source, info, serial_epoch);   // S102 T14
    return info;
  };
  if (memo_on) {   // S102 T14: the same shader found by comparing the whole microcode with the kept copy (the plain path hashes it and finds it in g_vs)
    if (const VertexShaderInfo* hit = g_vs_memo.Find(memo_key, source, serial_epoch)) return remember(hit, false);
  }
  key.code = cap::Xxh3(source, bytes);  // S100 speed: was db::Hash64, a serial multiply chain (~5 cycles per 8 bytes) over the whole microcode every draw
  if (auto it = g_vs.find(key); it != g_vs.end()) return remember(&it->second);

  VertexShaderInfo info;
  // The original microcode (the inventory's key): the object's bytes with each fetch instruction the library can patch in place put back from
  // the pristine copy it keeps. List and copy per listing 0x82CBD910: count at rec+872+28, the list of instruction indices (low 12 bits) at
  // rec+872+4*([rec+872+24]+9), pristine fetch words at vs+68, 12 bytes each.
  std::vector<uint8_t> original(source, source + bytes);
  const uint32_t fetches = db::Ld32(base, rec + 872 + 28);
  if (fetches <= 29) {
    const uint32_t list = rec + 872 + 4 * (db::Ld32(base, rec + 872 + 24) + 9);
    for (uint32_t i = 0; i < fetches; ++i) {
      const uint32_t at = (db::Ld32(base, list + 4 * i) & 0xFFF) * 12;
      if (at + 12 <= bytes) std::memcpy(original.data() + at, db::Host(base, vs + 68 + 12 * i), 12);
    }
    if (std::memcmp(original.data(), source, bytes) != 0) Note("vs_microcode_restored_from_pristine");
  } else {
    Note("vs_too_many_fetches_to_restore");
  }
  info.original_hash = cap::Xxh3(original.data(), bytes);

  const bool no_patch = !key.seven_c && (!key.decl || (db::Ld32(base, vs + 872) & 0x40));
  if (!no_patch && !key.decl) Note("vs_needs_patch_without_declaration");
  // S101 2026-11-13 (run 7221): the patched copy comes back as host bytes from a fixed scratch slot (db::PatchedVertexShaderToHost):
  // the old append-only guest copies filled the 4 MB region after 5 match loads and every new shader's draws were dropped.
  std::vector<uint8_t> patched_bytes;
  const PPCContext saved = ctx;
  // wwe2k14_rdr_debug_no_patch (S100 WM30 crash diagnosis): a plain copy, the game's patch routine not called (layouts then wrong).
  const bool patch = !no_patch && key.decl != 0 && !REXCVAR_GET(wwe2k14_rdr_debug_no_patch);
  const db::Fail f = db::PatchedVertexShaderToHost(ctx, base, vs, key.decl, strides, patch, patched_bytes);
  ctx = saved;
  if (f != db::Fail::kNone) {
    Note("vs_patch_failed", uint32_t(f));
    return nullptr;
  }
  const uint8_t* patched = patched_bytes.data();
  const uint32_t patched_dwords = uint32_t(patched_bytes.size() / 4);
  info.patched_hash = cap::Xxh3(patched, patched_dwords * 4);
  ParseMicrocode(patched, patched_dwords, true, info.facts);
  // Element k must be what the ORIGINAL microcode's k-th vertex fetch reads: that is how every hand-written shader numbers its inputs
  // (SHADER_CONVENTIONS, the proof harness). The patched copy can list its fetches in another order (S100: rigid props have no weights
  // stream; the patched copy's first fetch is the weights one, pointed at offset 0, the original's first is the position), so the patched
  // fetches are put in the original's order, matched by the register and components each one writes.
  ShaderFacts original_facts;
  ParseMicrocode(original.data(), bytes / 4, true, original_facts);
  if (original_facts.vfetches.size() == info.facts.vfetches.size()) {
    std::vector<VFetch> ordered;
    std::vector<bool> used(info.facts.vfetches.size(), false);
    for (const VFetch& want : original_facts.vfetches) {
      size_t match = SIZE_MAX;
      // The patch changes selects inside a fetch's written components (run 6849), so: register and selects, then register and the
      // components written (select 7 = not written), then the register alone.
      auto written = [](uint16_t s) {
        uint32_t m = 0;
        for (uint32_t c = 0; c < 4; ++c) m |= (((s >> (3 * c)) & 7) != 7 ? 1u : 0u) << c;
        return m;
      };
      for (size_t pass = 0; pass < 3 && match == SIZE_MAX; ++pass) {
        for (size_t p = 0; p < info.facts.vfetches.size(); ++p) {
          const VFetch& have = info.facts.vfetches[p];
          if (!used[p] && have.dest == want.dest &&
              (pass == 2 || (pass == 1 ? written(have.dest_swizzle) == written(want.dest_swizzle) : have.dest_swizzle == want.dest_swizzle))) {
            match = p;
            break;
          }
        }
      }
      if (match == SIZE_MAX) break;
      used[match] = true;
      ordered.push_back(info.facts.vfetches[match]);
      ordered.back().original_dest_swizzle = want.dest_swizzle;
    }
    if (ordered.size() == info.facts.vfetches.size()) {
      bool moved = false;
      for (size_t k = 0; k < ordered.size(); ++k) moved |= ordered[k].dest != info.facts.vfetches[k].dest || ordered[k].dest_swizzle != info.facts.vfetches[k].dest_swizzle;
      if (moved) Note("vfetch_order_from_original");
      info.facts.vfetches = std::move(ordered);
    } else {
      Note("vfetch_order_unmatched");
    }
  } else {
    Note("vfetch_count_differs_from_original");
  }
  BuildLayout(info);
  auto [it, inserted] = g_vs.emplace(key, std::move(info));
  // F5: the patch routine may have written the microcode in place since it was hashed: the table keeps a copy only of bytes that still hash to the key's code
  return remember(&it->second, memo_on && cap::Xxh3(source, bytes) == key.code);
}

inline const PixelShaderInfo* GetPixelShader(uint8_t* base, uint32_t ps) {
  const uint32_t blk = ps + db::Ld32(base, ps + 64);
  const uint32_t bytes = db::Ld32(base, blk + 44);
  if (!bytes || (bytes & 3) || bytes > (256u << 10)) {
    Note("ps_bad_microcode_size");
    return nullptr;
  }
  const uint32_t code_addr = db::Ld32(base, blk + 40) + db::Ld32(base, ps + 24);
  const uint8_t* source = db::Host(base, code_addr);
  LastShader& last = g_last_ps;  // (S100 speed: see GetVertexShader)
  const uint32_t head = std::min<uint32_t>(bytes, 32);
  if (last.info && last.epoch == g_shader_repeat_epoch && last.object == ps && last.code_addr == code_addr && last.bytes == bytes &&
      std::memcmp(last.head, source, head) == 0) {
    return static_cast<const PixelShaderInfo*>(last.info);
  }
  const uint64_t serial_epoch = rdr_adapter::draw_memos::g_shader_serial_on.load(std::memory_order_relaxed) ? g_shader_repeat_epoch : 0;   // S102 T14b (see GetVertexShader)
  const bool memo_on = serial_epoch || rdr_adapter::draw_memos::g_shader_on.load(std::memory_order_relaxed);   // S102 T14 (see GetVertexShader)
  rdr_adapter::draw_memos::ShaderTable<PixelShaderInfo>::Key memo_key;
  if (memo_on) memo_key = rdr_adapter::draw_memos::ShaderTable<PixelShaderInfo>::MakeKey(ps, 0, code_addr, bytes, false, nullptr);   // F4
  auto remember = [&](const PixelShaderInfo* found, bool into_table = true) {
    last.epoch = g_shader_repeat_epoch;
    last.object = ps;
    last.code_addr = code_addr;
    last.bytes = bytes;
    std::memcpy(last.head, source, head);
    last.info = found;
    if (memo_on && into_table) g_ps_memo.Remember(memo_key, source, found, serial_epoch);   // S102 T14
    return found;
  };
  if (memo_on) {
    if (const PixelShaderInfo* hit = g_ps_memo.Find(memo_key, source, serial_epoch)) return remember(hit, false);
  }
  const uint64_t code = cap::Xxh3(source, bytes);  // S100 speed: was db::Hash64 every draw (see GetVertexShader); the inventory's hash
  const uint64_t key = (uint64_t(ps) << 32) ^ code;
  if (auto it = g_ps.find(key); it != g_ps.end()) return remember(&it->second);
  PixelShaderInfo info;
  info.hash = code;
  info.writes_depth = info.hash == kDepthExportPs;
  ParseMicrocode(source, bytes / 4, false, info.facts);
  auto [it, inserted] = g_ps.emplace(key, std::move(info));
  return remember(&it->second, memo_on && cap::Xxh3(source, bytes) == code);   // S102 T14 F5 (see GetVertexShader)
}

// The literal constants of a shader, over its bank (registers are the 512 vec4 of the two banks, as the library's literal table addresses them).
inline void OverlayLiterals(const uint8_t* base, uint32_t rec, uint32_t data_base, uint32_t* vs_bank, uint32_t* ps_bank) {
  const uint32_t off = db::Ld32(base, rec + 20);
  if (!off) return;
  const uint32_t table = rec + off;
  uint32_t at = table + 20;
  // Bounded: a wrong record once walked gigabytes of guest memory here (S100 WM30 crash, runs 6861-6877: the walk touched pages the SDK
  // guards, and the game's command lists came out corrupted). A real table is a few hundred bytes of 8-byte entries.
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
      const uint32_t v = db::Ld32(base, src + 4 * i);
      if (reg < 1024) vs_bank[reg] = v;
      else if (reg < 2048) ps_bank[reg - 1024] = v;
    }
  }
}
