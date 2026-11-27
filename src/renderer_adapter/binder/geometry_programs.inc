
  // Geometry lifetime (docs\renderer\LIVE_ADAPTER.md piece 3). Inline draws (vertices the CPU wrote for that draw) go into this frame's own
  // geometry blob in the ring. Everything else is a guest buffer the reader already de-duplicates by content (its offset is the key): one
  // renderer buffer, freed after kGeometryKeepFrames frames unused.
  static constexpr uint64_t kGeometryKeepFrames = 120;
  struct GeometryEntry {
    rdr::BufferId buffer = rdr::kNoBuffer;
    uint64_t last_frame = 0;
    uint32_t size = 0;  // S103 IXF: the bytes the buffer was made with
  };

  // Old captures (no kFrameSixteenPairsHighFirst): every 16-bit pair element (guest k_16_16[_FLOAT], k_16_16_16_16[_FLOAT]) gets its halves
  // swapped, which is what the reader now does when it converts.
  static void SwapSixteenPairs(const LoadedFrame& frame, const VertexLayout& layout, std::vector<uint8_t>& bytes, uint32_t size) {
    if (!layout.host_stride) return;
    for (uint32_t e = 0; e < layout.element_count; ++e) {
      const VertexElement& element = frame.elements[layout.element_first + e];
      const uint32_t words = element.source_format == 31 || element.source_format == 25 ? 1 : (element.source_format == 32 || element.source_format == 26 ? 2 : 0);
      for (uint32_t at = element.host_offset; words && at + 4 * words <= size; at += layout.host_stride) {
        for (uint32_t w = 0; w < words; ++w) {
          uint16_t half[2];
          std::memcpy(half, bytes.data() + at + 4 * w, 4);
          std::swap(half[0], half[1]);
          std::memcpy(bytes.data() + at + 4 * w, half, 4);
        }
      }
    }
  }

  template <class Source>
  rdr::BufferRange Geometry(uint64_t offset, uint32_t size, bool per_frame, const Source& capture, rdr::FrameBuilder& builder, std::vector<uint8_t>& bytes,
                            const LoadedFrame* legacy_frame = nullptr, const VertexLayout* legacy_layout = nullptr) {
    if (per_frame) {
      capture.ReadGeometry(offset, size, bytes);
      if (legacy_layout) SwapSixteenPairs(*legacy_frame, *legacy_layout, bytes, size);
      return {rdr::kFrameGeometry, builder.PushGeometry(bytes.data(), size, 16)};
    }
    GeometryEntry& entry = geometry_[offset];
    entry.last_frame = frame_number_;
    // S103 IXF: the cache is keyed by offset only; a hit smaller than this draw's range would be drawn past its end (a30_match: validation error)
    if (rdr_adapter::geosize::Classify(entry.buffer != rdr::kNoBuffer, entry.size, size) == rdr_adapter::geosize::Cache::kMismatch) {
      rdr::Problem("geometry_offset_size_mismatch", "geometry at offset %llu cached with %u bytes, a draw wants %u: served from its own buffer", static_cast<unsigned long long>(offset), entry.size, size);
      capture.ReadGeometry(offset, size, bytes);
      if (legacy_layout) SwapSixteenPairs(*legacy_frame, *legacy_layout, bytes, size);
      return {rdr::kFrameGeometry, builder.PushGeometry(bytes.data(), size, 16)};
    }
    if (entry.buffer == rdr::kNoBuffer) {
      capture.ReadGeometry(offset, size, bytes);
      if (legacy_layout) SwapSixteenPairs(*legacy_frame, *legacy_layout, bytes, size);
      entry.buffer = renderer_.CreateBuffer(size, bytes.data());
      entry.size = size;
    }
    return {entry.buffer, 0};
  }

  void EvictGeometry() {
    // render-clock: lookup - every 60th frame sweeps our own geometry cache
    if (frame_number_ % 60) return;
    for (auto it = geometry_.begin(); it != geometry_.end();) {
      // render-clock: lookup - how long we keep our own geometry copy, not game progress
      if (frame_number_ - it->second.last_frame > kGeometryKeepFrames) {
        if (it->second.buffer != rdr::kNoBuffer) renderer_.DestroyBuffer(it->second.buffer);
        it = geometry_.erase(it);
      } else {
        ++it;
      }
    }
  }

  // S102 G10 (review F2): is this stream draw's output alpha provably 1? The rule: (1) the registry says both variants take their alpha from constants x the vertex colour only
  // (compile_shaders.py alpha_meta); (2) MdlAlpha.x (c185) * MatAmbCol.w (c189) * MatDifCol.w (c190) >= 254.5/255 in this draw's own bank; (3) the minimum alpha of the draw's static vertex
  // colours (the capture's geometry: the arena hook verifies its meshes equal the stream's) is 255 (a per-frame vertex stream is refused). Only runs with the cut on; every refusal is counted by
  // program (colour_cuts.h g_alpha_census, printed by rplay). The test plant (g_plant_mdl_alpha) halves MdlAlpha of one yBG_DefaultLM_base draw a frame IN THE FRAME'S BANK, cut on or off.
  template <class Source>
  uint8_t AlphaOpaque(const LoadedFrame& frame, const Draw& draw, int vs, int ps, const Source& capture, bool cuts_on, bool overdraw = false, bool* exact_out = nullptr, bool* texture_used = nullptr) {  // S102 R18-B3: exact_out = the proof holds with alpha EXACTLY 1 (arena_fragments.h ExactConstants)
    namespace cc = rdr::colour_cuts;
    const rdr::fx::Variant& v = rdr::fx::kVariants[vs];
    if (cc::g_plant_mdl_alpha.load(std::memory_order_relaxed) == 5 && v.out_alpha && ps >= 0 && rdr::fx::kVariants[ps].out_alpha && std::strncmp(v.name, "yBG_", 4) == 0) {
      ConstantBank& bank = const_cast<ConstantBank&>(frame.constant_banks[draw.vs_bank]);
      const float half = 0.5f;
      std::memcpy(&bank.dwords[185 * 4], &half, 4);
    }
    if (!cuts_on && !overdraw) return 0;
    cc::AlphaRow* row;  // review F5: a counter row per (vs, ps) pair, cached here; no string and no lock per draw
    {
      const uint32_t pair_key = uint32_t(vs) << 16 | uint32_t(ps + 1);
      const auto found = alpha_rows_.find(pair_key);
      row = found != alpha_rows_.end() ? found->second : (alpha_rows_[pair_key] = cc::AlphaRowFor(vs, ps));
    }
    const bool skip_rule = cc::g_skip_alpha_rule.load(std::memory_order_relaxed);
    // S102 C4: out_alpha 2 (the fragment multiplies in its slot-0 texture's alpha) is admitted only under the arena overdraw switch, and only with the texture proven alpha-free below
    const bool texture_alpha = ps >= 0 && rdr::fx::kVariants[ps].out_alpha == 2;
    if (!v.out_alpha || ps < 0 || !rdr::fx::kVariants[ps].out_alpha || (texture_alpha && !overdraw)) return cc::NoteAlpha(row, cc::kAlphaNotSimple), 0;
    if (skip_rule) return cc::NoteAlpha(row, cc::kAlphaTaken), 1;
    float k[3];  // c185.x, c189.w, c190.w
    {
      uint8_t buf[16];
      const BankRef bank = frame.constant_banks.Ref(draw.vs_bank);
      bank.CopyBytes(185 * 16, 16, buf), std::memcpy(&k[0], buf, 4);
      bank.CopyBytes(189 * 16, 16, buf), std::memcpy(&k[1], buf + 12, 4);
      bank.CopyBytes(190 * 16, 16, buf), std::memcpy(&k[2], buf + 12, 4);
    }
    if (k[0] * k[1] * k[2] == 0.0f) ++row->zero;  // S102 R18-B3: census only: a draw whose constants product is exactly 0 (its output alpha is 0)
    if (!(k[0] * k[1] * k[2] >= cc::kOpaqueAlpha)) return cc::NoteAlpha(row, cc::kAlphaProduct), 0;
    const bool exact_one = rdr::arena_fragments::ExactConstants(k[0], k[1], k[2], rdr::arena_fragments::g_plant.load(std::memory_order_relaxed));  // S102 R18-B3
    if (texture_alpha) {  // S102 C4: the diffuse texture (slot 0) must have no alpha: the slot's swizzle / format / content scan (texture_alpha.h SlotAlphaOne)
      bool one = false;
      for (uint32_t t = 0; t < draw.texture_count; ++t) {
        const TextureSlot& slot = frame.texture_slots[draw.texture_first + t];
        if (slot.slot != 0) continue;
        if (slot.image == 0 || slot.target16_read || slot.t21_exp != 0 || slot.sign[3] != 0) break;
        const auto image = synced_format_of(slot.image);
        if (slot.view_format != 0 && rdr::Format(slot.view_format) != image) break;
        const auto found_rec = rec_index_.find(slot.image);
        if (found_rec == rec_index_.end() || found_rec->second >= synced_records_.size()) break;
        const ImageRecord& rec = synced_records_[found_rec->second];
        if (!rdr::texture_alpha::RecordMayBeProven(rec.usage, rec.flags)) break;  // review F2: a render target or a copy destination is written by the GPU, never proven
        const rdr::SamplerDesc& sampler = frame.samplers[slot.sampler];
        const rdr::texture_alpha::ImageAlpha scan = tex_alpha_.Get(slot.image);
        one = rdr::texture_alpha::SlotAlphaOne(image, slot.swizzle[3], scan, sampler);  // review F3: includes the sampler's border rule (also for swizzle ONE)
        const bool base_one = rdr::texture_alpha::SlotAlphaOne(image, slot.swizzle[3], scan);
        if (base_one && rdr::texture_alpha::SamplerUsesBorder(sampler)) ++(one ? cc::g_border_proven : cc::g_border_refused);  // offline census: proven slots that clamp to a border
        break;
      }
      if (cc::g_arena_overdraw_plant.load(std::memory_order_relaxed) == 1) one = true;  // PLANTED FAULT (rplay --arena-overdraw-plant 1)
      if (!one && !skip_rule) return ++cc::g_overdraw_texture_refused, cc::NoteAlpha(row, cc::kAlphaTexture), 0;
      ++cc::g_overdraw_texture_taken;  // (counted before the vertex-alpha scan below: a draw refused there is counted in the census as refused)
    }
    if ((draw.flags & kDrawInline) || draw.layout == 0xFFFFFFFFu || !draw.vertex_bytes) return cc::NoteAlpha(row, cc::kAlphaPerFrame), 0;
    const VertexLayout& layout = frame.layouts[draw.layout];
    if (v.vc_fetch >= layout.element_count) return cc::NoteAlpha(row, cc::kAlphaNoColour), 0;
    const VertexElement& element = frame.elements[layout.element_first + v.vc_fetch];
    const uint64_t key = ((draw.vertex_offset * 0x100000001b3ull) ^ (uint64_t(draw.vertex_bytes) << 20) ^ (uint64_t(layout.host_stride) << 40) ^ (uint64_t(element.host_offset) << 8) ^ element.vk_format) * 0x9E3779B97F4A7C15ull;
    auto found = valpha_.find(key);
    int lowest;
    if (found != valpha_.end()) {
      lowest = found->second;
    } else {
      capture.ReadGeometry(draw.vertex_offset, draw.vertex_bytes, alpha_scan_);
      lowest = cc::MinVertexAlpha(alpha_scan_.data(), alpha_scan_.size(), layout.host_stride, element.host_offset, element.vk_format);
      valpha_.emplace(key, lowest);
    }
    if (lowest > 255) return cc::NoteAlpha(row, cc::kAlphaNoColour), 0;
    if (lowest < 255) return cc::NoteAlpha(row, cc::kAlphaVertex), 0;
    if (exact_out) *exact_out = exact_one;  // S102 R18-B3
    if (texture_used) *texture_used = texture_alpha;  // S102 R18-B3 review F2: this draw was admitted by C4's texture-alpha proof
    ++(exact_one ? rdr::arena_fragments::g_proof_exact : rdr::arena_fragments::g_proof_inexact);
    return cc::NoteAlpha(row, cc::kAlphaTaken), 1;
  }
  rdr::Format synced_format_of(uint32_t image) const {  // S102 C4: the format of a game image's synced record (Undefined when there is none)
    const auto found = rec_index_.find(image);
    return found != rec_index_.end() && found->second < synced_records_.size() ? rdr::Format(synced_records_[found->second].format) : rdr::Format::Undefined;
  }
  std::unordered_map<uint32_t, rdr::colour_cuts::AlphaRow*> alpha_rows_;  // S102 C4 (review F5)
  std::unordered_map<uint64_t, int> valpha_;  // S102 G10: (geometry range, colour element) -> minimum vertex alpha (static geometry: cached for the run)
  std::vector<uint8_t> alpha_scan_;

  uint16_t Program(int vs, int ps, bool alpha) {
    const uint64_t key = uint64_t(uint32_t(vs)) << 32 | uint64_t(uint32_t(ps + 1)) << 1 | uint64_t(alpha);
    auto found = programs_.find(key);
    if (found != programs_.end()) return found->second;
    const rdr::fx::Variant& vertex = rdr::fx::kVariants[vs];
    rdr::ProgramDesc desc;
    names_.push_back(std::string(vertex.name) + " + " + (ps >= 0 ? rdr::fx::kVariants[ps].name : "(no fragment)"));  // validation messages name both halves
    desc.name = names_.back().c_str();
    desc.vertex_spirv = rdr::fx::Words(size_t(vs), false);
    desc.vertex_bytes = vertex.bytes;
    if (!overrides_.empty()) ApplyOverride(vs, false, desc.vertex_spirv, desc.vertex_bytes);  // S101 lane 2 (fview)
    if (ps >= 0) {
      const rdr::fx::Variant& fragment = rdr::fx::kVariants[ps];
      const bool twin = alpha && TwinOf(ps);  // S101 fview: TwinOf is the compiled table's answer unless a reload dropped the twin
      // S100 (the far crowd's cards, capture s100cards): an alpha test the game asks for must never be dropped quietly - the cards' transparent
      // margins then wrote depth and hid the cards behind them. Every effect now builds its alpha twins; a missing one is said once.
      if (alpha && !fragment.bytes_alpha)  // (a twin a reload dropped on purpose is fview's own plain build: not said)
        rdr::Log("[bind] %s: the game asks for an alpha test but this fragment variant has no alpha-test build (drawn WITHOUT the test)", desc.name);
      desc.fragment_spirv = rdr::fx::Words(size_t(ps), twin);
      desc.fragment_bytes = twin ? fragment.bytes_alpha : fragment.bytes;
      if (!overrides_.empty()) ApplyOverride(ps, twin, desc.fragment_spirv, desc.fragment_bytes);  // S101 lane 2 (fview)
    }
    uint16_t id = 0;
    if (!overrides_.empty()) {  // S101 fview: a reloaded shader the driver refuses is said (override_errors) and the compiled program stands in
      if (!renderer_.AddProgramChecked(desc, id)) {
        override_errors_.push_back(std::string(desc.name) + ": the driver refused a reloaded shader module (or the program table is full)");
        desc.vertex_spirv = rdr::fx::Words(size_t(vs), false), desc.vertex_bytes = vertex.bytes;
        if (ps >= 0) {
          const rdr::fx::Variant& fragment = rdr::fx::kVariants[ps];
          const bool compiled_twin = alpha && fragment.bytes_alpha != 0;
          desc.fragment_spirv = rdr::fx::Words(size_t(ps), compiled_twin);
          desc.fragment_bytes = compiled_twin ? fragment.bytes_alpha : fragment.bytes;
        }
        id = renderer_.AddProgram(desc);
      }
    } else {
      id = renderer_.AddProgram(desc);
    }
    programs_.emplace(key, id);
    return id;
  }

  // -1 when the layout cannot be built.
  int Layout(const LoadedFrame& frame, const Draw& draw, int vs, BindStats& stats) {
    const rdr::fx::Variant& vertex = rdr::fx::kVariants[vs];
    rdr::VertexLayoutDesc desc{};
    uint64_t key = 0xcbf29ce484222325ull;
    const auto mix = [&](uint64_t value) { key = (key ^ value) * 0x100000001b3ull; };
    mix(reinterpret_cast<uintptr_t>(rdr::fx::Words(size_t(vs), false)));
    if (draw.layout != 0xFFFFFFFFu) {
      const VertexLayout& layout = frame.layouts[draw.layout];
      if (vertex.attribute_count > rdr::kMaxVertexAttributes) return -1;
      desc.strides[0] = layout.host_stride;
      mix(layout.host_stride);
      // Captures from before the reader matched fetches to the original order (no kFrameFetchOrderOriginal): the one shape known to differ is a
      // full fetch followed by a mini fetch at the same offset with another format, where the patched copy put the original's second fetch
      // first (rigid props, planar shadows: the weights read at offset 0). Seen and checked on the S100 Brock captures: the ring's posts and ropes.
      bool swap01 = false;
      if (!(frame.header.flags & kFrameFetchOrderOriginal) && layout.element_count >= 2) {
        const VertexElement& e0 = frame.elements[layout.element_first];
        const VertexElement& e1 = frame.elements[layout.element_first + 1];
        swap01 = !e0.mini && e1.mini && e0.guest_offset == e1.guest_offset && e0.source_format != e1.source_format;
      }
      for (uint32_t a = 0; a < vertex.attribute_count; ++a) {
        const rdr::fx::Attribute& attribute = vertex.attributes[a];
        if (attribute.fetch >= layout.element_count) return -1;
        const uint32_t fetch = swap01 && attribute.fetch < 2 ? 1 - attribute.fetch : attribute.fetch;
        const VertexElement& element = frame.elements[layout.element_first + fetch];
        desc.attributes[a] = {attribute.location, 0, element.vk_format, element.host_offset};
        mix(uint64_t(attribute.location) << 32 | uint64_t(element.vk_format) << 16 | element.host_offset);
      }
      desc.attribute_count = vertex.attribute_count;
    } else if (vertex.attribute_count) {
      return -1;
    }
    auto found = layouts_.find(key);
    if (found != layouts_.end()) return found->second;
    for (uint32_t a = 0; a < desc.attribute_count; ++a) {
      if (rdr::VertexFormatIsInteger(desc.attributes[a].format) != (vertex.attributes[a].integer != 0))
        ++stats.type_mismatches[uint64_t(uint32_t(vs)) << 32 | uint64_t(desc.attributes[a].location) << 16 | desc.attributes[a].format];
    }
    const uint16_t id = renderer_.AddVertexLayout(desc);
    layouts_.emplace(key, id);
    return id;
  }

  // Decoupling (frame_pump.h): the vertex constants the build marked as interpolable (transforms, camera and light positions), and a key that
  // finds the same draw in the previous game tick: its shaders, its geometry (content-keyed, stable across frames) and how many such draws came
  // before it this frame. Inline geometry is new every frame, so those draws never match and show the newest tick.
  void BlendRanges(const rdr::fx::Variant& vertex, const Draw& draw, rdr::FrameBuilder& builder, rdr::DrawOp& op, const uint64_t* thin_base = nullptr,
                   const rdr::BlendRange* own_ranges = nullptr, uint32_t own_count = 0) {  // lane 159: own_ranges = an own draw's ranges into its own block (salted key)
    if (rdr::FrameFactsOn()) {   // S102 R21-A: no consumer of the tween / DLSS keys is on: no key, no occurrence count, no keyed draw (op.blend_* stay 0)
      if ((!blend_keys_wanted_ && particle_fault_ != particles::Fault::TweenRuns) || rdr::g_frame_facts_plant.load(std::memory_order_relaxed) == 3) {   // (review F3: the particle plant TweenRuns keys merged draws: keep it working)
        thin_blend_base_ = 0;
        ++ff_blend_skipped_;
        return;
      }
      ++ff_blend_kept_;
    }
    rdr::BlendRange ranges[16];
    uint32_t count = 0;
    if (own_ranges) {
      for (; count < own_count && count < 4; ++count) ranges[count] = own_ranges[count];
    } else {
      for (uint32_t r = 0; r < vertex.constant_count && count < 16; ++r) {
        const rdr::fx::ConstantRange& range = vertex.constants[r];
        if (range.blend) ranges[count++] = {range.offset, uint32_t(range.count) * 16};
      }
    }
    thin_blend_base_ = 0;  // lane 122: the key before its occurrence, for the thin template (0 = this draw has no blend ranges)
    if (!count) return;
    uint64_t key = BlendKeyBase(draw.vs_hash, draw.ps_hash, draw.vertex_offset, draw.index_offset, velocity_mode_, own_ranges != nullptr);  // S10b: velocity rebinds keyed apart; lane 159: an own draw's block has another layout, never paired with the stream draw's
    if (thin_base) key = *thin_base;  // lane 122: a thin draw carries blank hashes / offsets: the key its template learned
    thin_blend_base_ = key;
    const uint32_t occurrence = occurrences_[key]++;
    keyed_draws_.push_back({builder.DrawCount(), key});  // (cleared at the frame's end when the key is not unique)
    key = (key ^ occurrence) * 0x100000001b3ull;
    key &= ~rdr::kOwnBlendKeyBit;   // S103 P7 tween: the top bit is the own-draw bit (FrameBuilder::TagBlend), never a stream key
    if (!key) key = 1;  // 0 means "never interpolated"
    op.blend_first = builder.PushBlendRanges(ranges, count);
    op.blend_count = uint16_t(count);
    op.blend_key_low = uint32_t(key);
    op.blend_key_high = uint32_t(key >> 32);
  }

  // S101 lane 35: the same block PackConstants builds, into a caller's buffer (8192 bytes), for the crowd hook. Returns the block's size.
  static uint32_t PackBlock(const rdr::fx::Variant& variant, const BankRef& bank, const BoolLoop& bools, uint8_t* block, uint32_t capacity = 8192) {
    const uint32_t bytes = std::min<uint32_t>(std::max<uint32_t>(variant.block_bytes, 16), 8192);
    if (bytes > capacity) return 0;   // lane 151: a buffer that cannot hold the block gets nothing written (the caller refuses), never an overflow
    std::memset(block, 0, bytes);
    for (uint32_t r = 0; r < variant.constant_count; ++r) {
      const rdr::fx::ConstantRange& range = variant.constants[r];
      bank.CopyBytes(uint32_t(range.reg) * 16, uint32_t(range.count) * 16, block + range.offset);   // S102 T1b: block by block through the bank's map when it has one
    }
    if (variant.bools_offset != 0xFFFF) std::memcpy(block + variant.bools_offset, bools.words, 32);
    return bytes;
  }

  static uint32_t PackConstants(const rdr::fx::Variant& variant, const BankRef& bank, const BoolLoop& bools, rdr::FrameBuilder& builder) {
    // S100 speed: only the bytes pushed are zeroed (the whole 8 KB was, twice a draw); the ranges lie inside the variant's block.
    const uint32_t bytes = std::min<uint32_t>(std::max<uint32_t>(variant.block_bytes, 16), 8192);
    if (builder.PackOnce()) {  // S102 T5 (wwe2k14_rdr_pack_once): packed once, straight into the builder's block (zeroed by the reserve): no stack block, no second copy
      return PackOnceInto(variant, bank, bools, builder);  // pack_once.h (clamped to the reserved bytes; review F1)
    }
    uint8_t block[8192];
    std::memset(block, 0, bytes);
    for (uint32_t r = 0; r < variant.constant_count; ++r) {
      const rdr::fx::ConstantRange& range = variant.constants[r];
      bank.CopyBytes(uint32_t(range.reg) * 16, uint32_t(range.count) * 16, block + range.offset);   // S102 T1b: block by block through the bank's map when it has one
    }
    if (variant.bools_offset != 0xFFFF) std::memcpy(block + variant.bools_offset, bools.words, 32);
    return builder.PushConstants(block, bytes);
  }

  // lane 164 (fview only, g_const_override.active): PackConstants with the override applied to the block before it is pushed (the bank is never written).
  static uint32_t PackConstantsOverridden(const rdr::fx::Variant& variant, const ConstantBank& bank, const BoolLoop& bools, rdr::FrameBuilder& builder, const const_override::Ctx& ctx, bool pixel) {
    uint8_t block[8192];
    const uint32_t bytes = PackBlock(variant, bank, bools, block, sizeof(block));
    g_const_override.Apply(ctx, pixel, variant, block);
    return builder.PushConstants(block, bytes);
  }

  void CountPipeline(const LoadedFrame& frame, const Draw& draw, BindStats& stats) const {
    uint64_t key = (draw.vs_hash * 0x9E3779B97F4A7C15ull) ^ draw.ps_hash;
    stats.shader_pairs.insert(key);
    const auto mix = [&](const void* data, size_t size) {
      const uint8_t* byte = static_cast<const uint8_t*>(data);
      for (size_t i = 0; i < size; ++i) key = (key ^ byte[i]) * 0x100000001b3ull;
    };
    const DrawFixed& fixed = frame.fixeds[draw.fixed];
    mix(&fixed.state, sizeof(fixed.state));
    const uint8_t alpha = fixed.alpha_test != 0;
    mix(&alpha, 1);
    if (draw.layout != 0xFFFFFFFFu) {
      const VertexLayout& layout = frame.layouts[draw.layout];
      mix(&layout.host_stride, sizeof(layout.host_stride));
      for (uint32_t e = 0; e < layout.element_count; ++e) {
        const VertexElement& element = frame.elements[layout.element_first + e];
        mix(&element.vk_format, sizeof(element.vk_format));
        mix(&element.host_offset, sizeof(element.host_offset));
      }
    }
    const uint32_t format = listed_target_ < image_formats_.size() ? image_formats_[listed_target_] : 0;
    mix(&format, sizeof(format));
    stats.pipelines.insert(key);
  }
