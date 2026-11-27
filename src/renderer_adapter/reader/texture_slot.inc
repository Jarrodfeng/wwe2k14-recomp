
// S102 T15 (wwe2k14_rdr_sampler_memo): BuildSampler behind a memo of its inputs.
inline uint32_t BuildSamplerMemo(const uint32_t w[6], const TFetch& t) {
  namespace dm = rdr_adapter::slot_geo_memo;
  if (!dm::g_sampler_memo_on.load(std::memory_order_relaxed)) return BuildSampler(w, t);
  const dm::SamplerKey k = dm::MakeSamplerKey(w, t.mag, t.min, t.mip, t.aniso, g_aniso_override, g_jitter.lod_bias, g_debug_lod0, g_debug_no_aniso);
  if ((w[5] & 3) >= 2) {  // BuildSampler's census note: the memo does not stand in for it
    ++dm::g_census.sampler.declined, ++dm::g_census.sampler.by[1];
    return BuildSampler(w, t);
  }
  uint32_t index = 0;
  if (g_sampler_memo.Find(k, g_frame.generation, index)) {
    ++dm::g_census.sampler.taken;
    return index;
  }
  ++dm::g_census.sampler.declined, ++dm::g_census.sampler.by[0];
  index = BuildSampler(w, t);
  g_sampler_memo.Put(k, g_frame.generation, index);
  return index;
}

inline void AddTextureSlot(uint8_t* base, uint32_t dev, uint32_t slot, const TFetch& t) {
  // The slot as the GPU holds it (SyncGpuFetch: a null SetTexture leaves the last uploaded texture in place); the device's record when the
  // copy has not seen this slot yet.
  uint32_t w[6];
  uint32_t object = 0;
  if (slot < g_gpu_fetch.size() && g_gpu_fetch[slot].known && g_gpu_fetch_device == dev) {
    std::memcpy(w, g_gpu_fetch[slot].words, sizeof(w));
    object = g_gpu_fetch[slot].object;
    if ((db::Ld32(base, dev + kDevFetch + 24 * slot) & 3) != 2 && (w[0] & 3) == 2) Note("slot_null_gpu_keeps_previous");
  } else {
    for (uint32_t k = 0; k < 6; ++k) w[k] = db::Ld32(base, dev + kDevFetch + 24 * slot + 4 * k);
    object = db::Ld32(base, dev + kDevTexObj + 4 * slot);
    Note("slot_gpu_copy_unknown");
  }
  // S100 speed: the same words, object, header bytes and shader fetch as this slot's last add in these frame tables, with the registry
  // unchanged since (g_reg.epoch) and the image's flags as they were: its record again (EnsureContent still runs: once a frame, the watch).
  // S102 T15 (wwe2k14_rdr_slot_ways): up to 4 records per slot, the first whose key matches; a miss (or a matched record whose image flags moved)
  // overwrites that record (the matched one) or the slot's round-robin victim. With the switch off the only record is way 0: the old rule.
  namespace dm = rdr_adapter::slot_geo_memo;
  const bool ways_on = dm::g_slot_ways_on.load(std::memory_order_relaxed);
  const uint8_t* header_raw = object ? db::Host(base, object + 28) : nullptr;
  int way = 0;
  dm::SlotKey now;
  now.generation = g_frame.generation, now.epoch = g_reg.epoch, now.fetch = &t, now.object = object;
  now.aniso_override = g_aniso_override;
  std::memcpy(&now.lod_bias_bits, &g_jitter.lod_bias, 4);
  std::memcpy(now.w, w, sizeof(w));
  if (object) std::memcpy(now.header_raw, header_raw, sizeof(now.header_raw));
  SlotCache* matched = g_slot_cache.Find(slot, ways_on ? dm::kSlotWays : 1, way, [&](const SlotCache& c) { return c.kind && c.key.Equal(now); });
  if (matched) {
    SlotCache& sc = *matched;
    if (sc.kind == 2) {  // no texture (T14)
      g_frame.texture_slots.push_back(sc.out);
      ++g_times.slots_reused;
      if (ways_on) ++dm::g_census.ways.taken, dm::g_census.ways_hit_extra += way ? 1 : 0;
      return;
    }
    const Image& img = ById(sc.id);
    if (img.packed == sc.packed && img.copy_dest == sc.copy_dest && img.swap == sc.swap && img.target16 == sc.target16 &&
        std::memcmp(img.pack_host, sc.pack_host, 4) == 0) {
      g_frame.texture_slots.push_back(sc.out);
      ++g_times.slots_reused;
      if (ways_on) ++dm::g_census.ways.taken, dm::g_census.ways_hit_extra += way ? 1 : 0;
      EnsureContentChecked(sc.id, w);
      return;
    }
  }
  if (ways_on) ++dm::g_census.ways.declined, ++dm::g_census.ways.by[matched ? 1 : 0];  // 1 = the key matched but the image flags moved
  SlotCache& sc = matched ? *matched : g_slot_cache.Victim(slot, ways_on ? dm::kSlotWays : 1);
  sc.kind = 0;
  auto remember = [&](uint8_t kind, uint32_t id, const cap::TextureSlot& rec) {
    sc.kind = kind;
    sc.key = now;
    sc.key.epoch = g_reg.epoch;  // after this add's own registry changes
    sc.id = id;
    sc.out = rec;
    if (id) {
      const Image& img = ById(id);
      sc.packed = img.packed;
      sc.copy_dest = img.copy_dest;
      sc.swap = img.swap;
      sc.target16 = img.target16;
      std::memcpy(sc.pack_host, img.pack_host, 4);
    }
  };
  cap::TextureSlot out;
  out.slot = uint8_t(slot);
  out.dimension = uint8_t((w[5] >> 9) & 3);
  for (uint32_t c = 0; c < 4; ++c) out.sign[c] = uint8_t((w[0] >> (2 + 2 * c)) & 3);
  const uint32_t base_page = w[1] >> 12, mip_page = w[5] >> 12;
  // Type 0 ("invalid texture") with the texture's words still in place is a texture: SetTexture(null) clears only the type bits, and the
  // game's effect apply (0x82D04608, caller 0x82D04F00) nulls stages right after the game bound them (shadow blur, bloom, depth of field,
  // the crowd's light map: run 6851's bind trace). The GPU samples the words; the old path does the same (gpu_allow_invalid_fetch_constants,
  // set by this app since 2026-10-07: "fixes the dark arena/background and makes depth of field work"). Types 1 / 3 are vertex fetches.
  const uint32_t type = w[0] & 3;
  if ((type != 2 && type != 0) || (!base_page && !mip_page)) {  // T14
    Note(type == 0 ? "slot_invalid_type_no_texture" : "slot_vertex_type_in_texture_slot");
    g_frame.texture_slots.push_back(out);
    remember(2, 0, out);
    return;
  }
  if (type == 0) Note("slot_invalid_type_read_as_texture");
  // The object in the slot, if the slot's words still describe it (L5, L27).
  uint32_t id = 0;
  if (object) {
    id = TextureOfObject(base, object);
    Image& img = ById(id);
    // The object's words as they are now: the game can rewrite a texture header after creating it.
    uint32_t current[6];
    for (uint32_t k = 0; k < 6; ++k) current[k] = db::Ld32(base, object + 28 + 4 * k);
    if (std::memcmp(current, img.header, sizeof(current)) != 0) {
      Note("texture_header_changed");
      // lane 195: a size / format move must refresh the image record, else the next read uploads a blob of the new size into the old-size host image
      // (an Adreno hazard). Copy destinations and packed images keep their own records.
      // The legacy switch (--wwe2k14_rdr_plane_repoint_legacy) turns this off too, so the phone A/B is one variable.
      if (img.is_texture && !img.copy_dest && !img.packed && !REXCVAR_GET(wwe2k14_rdr_plane_repoint_legacy)) {
        const TexShape ns = ShapeOf(current);
        const cap::ImageRecord& r0 = img.rec;
        if (ns.format != img.game_format || ns.width != r0.width || ns.height != r0.height || ns.depth != r0.depth || ns.mips != r0.mips ||
            ns.layers != r0.layers) {
          Note("texture_header_changed_record_refreshed");
          // SF2: the old header's words keyed this image; they now describe another picture, so the key goes (a slot with those words gets its own image)
          const WordsKey old_key = KeyOfHeaderWords(img.header);
          if (auto it = g_reg.tex_by_words.find(old_key); it != g_reg.tex_by_words.end() && it->second == id) g_reg.tex_by_words.erase(it);
          // SF1: nothing read for the old shape may reach the new record: this frame's pending contents of the image go, a worker job in flight is
          // stamped with the old generation (JoinTextureJobs drops it), and the image is read again at its next bind this frame.
          ++img.read_gen;
          g_frame.contents.erase(std::remove_if(g_frame.contents.begin(), g_frame.contents.end(), [id](const cap::ContentRef& c) { return c.image == id; }),
                                 g_frame.contents.end());
          FillTextureRecord(img, current);
          img.content_written = false;
          img.content_hash = 0;
          img.checked_frame = ~0ull;
        }
      }
      std::memcpy(img.header, current, sizeof(current));
      RememberHeaderWords(current, id);
    }
    // GpuForm of the header's word 1 against the slot's (bits the merge keeps from the old record masked off, d3d_textures.h).
    const uint32_t hdr1 = wwe2k14::guest_view::ToPhysical(img.header[1]);
    if (((hdr1 ^ w[1]) & ~0x00000800u) || ((img.header[2] ^ w[2])) || ((img.header[0] ^ w[0]) & ~0x003FFC00u)) id = 0;
    if (!id) Note("slot_words_differ_from_object");
  } else {
    Note("slot_without_object");
  }
  if (!id) id = TextureOfWords(w);
  Image& img = ById(id);
  out.image = id;
  const TexShape s = ShapeOf(w);
  const Fmt f = FormatOf(s.format);
  out.min_level = uint8_t((w[4] >> 2) & 0xF);
  out.max_level = uint8_t((w[4] >> 6) & 0xF);
  // S7, T21.
  out.fetch_lod_bias = float(int32_t(w[4] << 10) >> 22) / 32.0f;
  out.t21_exp = int32_t(w[4] << 13) >> 26;
  out.word3_exp = int32_t(w[3] << 13) >> 26;
  if (out.t21_exp) Note("fetch_word4_exp_adjust_nonzero");
  if (out.word3_exp) Note("fetch_word3_exp_adjust_nonzero");
  const uint32_t fetch_swizzle = (w[3] >> 1) & 0xFFF;
  bool any_biased = false, any_gamma = false, all_signed = true, any_signed = false;
  for (uint32_t c = 0; c < 4; ++c) {
    any_biased |= out.sign[c] == 2;
    any_gamma |= out.sign[c] == 3;
    any_signed |= out.sign[c] == 1;
    all_signed &= out.sign[c] == 1;
  }
  if (any_biased) Note("fetch_sign_biased");
  if (any_gamma) Note("fetch_sign_gamma");
  if (any_signed && !all_signed) Note("fetch_sign_mixed");

  if (img.packed) {
    // S100: an attire's final texture holds the build's atlas storage; its components are the atlas's (through the atlas's own channel map).
    ComposeSwizzle(fetch_swizzle, img.pack_host, false, out.swizzle);
  } else if (img.copy_dest) {
    // A copy destination keeps the source's storage: the format's channel map over it, and any red-blue swap (C4, C8, T1, T7).
    uint8_t host[4] = {0, 1, 2, 3};
    const uint32_t bf = BaseFormat(s.format);
    if (bf == 2 || bf == 23 || bf == 22) host[1] = host[2] = host[3] = 0;
    else if (bf == 25) host[2] = host[3] = 1;
    else if (bf != 6 && bf != 15 && bf != 26) Note("copy_dest_format_other", bf);
    ComposeSwizzle(fetch_swizzle, host, img.swap, out.swizzle);
    if (img.target16) {  // T7, T8, T16, C28: no view reinterpretation (RGBA16F storage); the fetch's sign is recorded for whoever samples it
      out.target16_read = uint8_t(all_signed ? cap::kTarget16Signed : (any_signed ? cap::kTarget16Mixed : cap::kTarget16Unsigned));
      Note(all_signed ? "target16_read_signed" : (any_signed ? "target16_read_mixed" : "target16_read_unsigned"));
    } else if (any_signed) {
      Note("copy_dest_8bit_signed_fetch");
    }
  } else {
    ComposeSwizzle(fetch_swizzle, f.swz, false, out.swizzle);
    if (all_signed) {
      const rdr::Format v = SignedView(f.host);
      if (v == rdr::Format::Undefined) {
        Note("signed_fetch_no_signed_host_format");
      } else {
        out.view_format = uint8_t(v);
        if (!(img.rec.usage & rdr::kUsageMutable)) g_images_dirty = true;
        img.rec.usage |= rdr::kUsageMutable;  // read through another format of the same size (never the 16-bit targets: RGBA16F, no reinterpretation)
      }
    }
  }
  out.sampler = BuildSamplerMemo(w, t);  // S102 T15
  g_frame.texture_slots.push_back(out);
  remember(1, id, out);
  EnsureContentChecked(id, w);  // S102 T15
}
