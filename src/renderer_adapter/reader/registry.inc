
// ---- texture formats (T1-T13) -----------------------------------------------------------------------------------------------------
enum Decode : uint8_t { kNoDecode = 0, kRaw = 1, kRgba4Rotate = 2 };
struct Fmt {
  uint8_t bw, bh, bpb;
  rdr::Format host;
  uint8_t swz[4];  // for guest component i, the host component: 0-3 R G B A, 4 zero, 5 one
  Decode decode;
};
inline constexpr Fmt kFmtNone{1, 1, 4, rdr::Format::Undefined, {0, 1, 2, 3}, kNoDecode};
// T12: the _AS_16_16_16_16 variants read as their base format.
inline uint32_t BaseFormat(uint32_t f) {
  switch (f) {
    case 50: return 6;
    case 51: return 18;
    case 52: return 19;
    case 53: return 20;
    case 54: return 7;
    case 55: return 16;
    case 56: return 17;
    default: return f;
  }
}
inline Fmt FormatOf(uint32_t f) {
  using rdr::Format;
  switch (BaseFormat(f)) {
    case 2: return {1, 1, 1, Format::R8, {0, 0, 0, 0}, kRaw};                     // T1
    case 3: return {1, 1, 2, Format::A1RGB5, {0, 1, 2, 3}, kNoDecode};            // T10: swapped at load, never bound in the bench
    case 4: return {1, 1, 2, Format::R5G6B5, {0, 1, 2, 2}, kNoDecode};            // T10
    case 5: return {1, 1, 2, Format::R5G6B5, {0, 2, 1, 1}, kNoDecode};            // T10
    case 6: return {1, 1, 4, Format::RGBA8, {0, 1, 2, 3}, kRaw};                  // T2
    case 7: return {1, 1, 4, Format::RGB10A2, {0, 1, 2, 3}, kRaw};                // T10
    case 10: return {1, 1, 2, Format::RG8, {0, 1, 1, 1}, kRaw};                   // T10
    case 15: return {1, 1, 2, Format::RGBA4, {0, 1, 2, 3}, kRgba4Rotate};         // T3
    case 18: return {4, 4, 8, Format::BC1, {0, 1, 2, 3}, kRaw};                   // T4
    case 19: return {4, 4, 16, Format::BC2, {0, 1, 2, 3}, kRaw};
    case 20: return {4, 4, 16, Format::BC3, {0, 1, 2, 3}, kRaw};
    case 22: case 23: return {1, 1, 4, Format::R32F, {0, 0, 0, 0}, kNoDecode};    // T5, T6: depth texels come from copies
    case 24: return {1, 1, 2, Format::R16_UNORM, {0, 0, 0, 0}, kRaw};
    case 25: return {1, 1, 4, Format::RG16_UNORM, {0, 1, 1, 1}, kRaw};            // T7
    case 26: return {1, 1, 8, Format::RGBA16_UNORM, {0, 1, 2, 3}, kRaw};          // T8
    case 30: return {1, 1, 2, Format::R16F, {0, 0, 0, 0}, kRaw};
    case 31: return {1, 1, 4, Format::RG16F, {0, 1, 1, 1}, kRaw};
    case 32: return {1, 1, 8, Format::RGBA16F, {0, 1, 2, 3}, kRaw};
    case 36: return {1, 1, 4, Format::R32F, {0, 0, 0, 0}, kRaw};                  // T9
    case 37: return {1, 1, 8, Format::RG32F, {0, 1, 1, 1}, kRaw};
    case 38: return {1, 1, 16, Format::RGBA32F, {0, 1, 2, 3}, kRaw};
    case 49: return {4, 4, 16, Format::BC5, {0, 1, 1, 1}, kRaw};
    case 59: return {4, 4, 8, Format::BC4, {0, 0, 0, 0}, kRaw};
    case 58: return {4, 4, 8, Format::R8, {0, 0, 0, 0}, kNoDecode};               // T10: by decode, never bound in the bench
    case 60: return {4, 4, 8, Format::RG8, {0, 1, 1, 1}, kNoDecode};
    case 61: return {4, 4, 8, Format::RGBA4, {0, 1, 2, 3}, kNoDecode};
    default: return kFmtNone;
  }
}
inline rdr::Format SignedView(rdr::Format f) {
  using rdr::Format;
  switch (f) {
    case Format::R8: return Format::R8_SNORM;
    case Format::RGBA8: return Format::RGBA8_SNORM;
    case Format::RG16_UNORM: return Format::RG16_SNORM;
    case Format::RGBA16_UNORM: return Format::RGBA16_SNORM;
    default: return Format::Undefined;
  }
}

// ---- registries -------------------------------------------------------------------------------------------------------------------
struct Image {
  cap::ImageRecord rec;
  uint32_t object = 0;
  uint32_t game_format = 0;
  bool live = true, is_texture = false, copy_dest = false, swap = false, depth = false;
  bool target16 = false;          // a copy destination whose source is a k_16_16 / k_16_16_16_16 target
  uint64_t copy_count = 0, copy_first = 0;  // lane 232: copies into this image (OnResolve) and the guest frame of the first: the lock line's copies per 100 frames
  bool gpu_newer = false;         // S100: a copy wrote it since its memory was last written back (OnTextureLock)
  bool watched = false;           // S100 speed: its pages are write-watched since its last read (EnsureContent)
  uint32_t watch_pages[2][2] = {};  // first and last 4 KB page of its base level and mip block (first > last: none)
  bool dest_watched = false;      // S100 (L26 both ways): a copy destination whose pages are watched since the copy (WatchCopyDest)
  uint32_t dest_pages[2][2] = {};  // those pages (first > last: none)
  bool packed = false;            // S100: an attire's final texture filled on the GPU from the build's atlas (OnAttirePacked), not from memory
  uint8_t pack_host[4] = {0, 1, 2, 3};  // for each of its fetch components, the atlas storage component (0-3) or 4 zero / 5 one
  bool linear_static = false;     // L8, L9: final at first bind
  bool flushed = false;           // an unlock flush named it since its contents were last read
  bool flush_seen = false;        // S101 lane17: the game flushed it at least once (provenance: a FILE texture flushed after its load is CPU)
  uint32_t flush_frame = 0;       // S101 lane17: the guest frame of its newest unlock flush
  uint32_t flush_seq = 0;         // S101 lane17 review: that flush's event number (prov::PageMap::Stamp)
  uint32_t first_read_seq = 0;    // S101 lane17 review: event number of the first time the reader read its contents (0 = not yet)
  uint32_t copy_source = 0;       // S101 lane17: a copy destination: the ImageRecord id of the surface its newest copy read (provenance source_image)
  bool flush_driven = false;      // strip-360 D8: a movie display plane: read only after the decoder's flush names it (OnPlaneFlush)
  uint32_t read_gen = 0;          // lane 195: bumped when the image record is refreshed for a new shape (TextureJob stamps it)
  uint32_t plane_key[2] = {0, 0}; // lane 172: base page and size words of the flush-driven plane as last bound (census only: a repoint is noted)
  bool read_notified = false;     // strip-360 E2: this read follows a host writer's report on its pages (census / verify only)
  bool copy_dest_seen = false;    // S101 names: a copy has set this destination's usage before (sticky roles)
  bool role_keeps_size = false;   // S101 names: a declared non-scaling source copied into it: it never scales
  uint32_t surf_key = 0;
  uint32_t header[6] = {};
  // per capture
  uint64_t content_hash = 0;
  uint64_t content_sample = 0;     // SampleTexture of the guest bytes at the last full read (S100 speed)
  uint64_t content_read_frame = 0; // the recorded frame of the last full read
  bool content_written = false;
  uint64_t checked_frame = ~0ull;
  bool written_in_capture = false;
};

struct WordsKey {
  uint32_t w[5];
  bool operator==(const WordsKey& o) const { return std::memcmp(w, o.w, sizeof(w)) == 0; }
};
struct WordsKeyHash {
  size_t operator()(const WordsKey& k) const { return size_t(db::Hash64(reinterpret_cast<const uint8_t*>(k.w), sizeof(k.w))); }
};

struct Registry {
  std::vector<Image> images;  // id = index + 1, ids are never reused
  std::unordered_map<uint32_t, uint32_t> tex_by_object, surf_by_object;
  std::unordered_map<WordsKey, uint32_t, WordsKeyHash> tex_by_words;  // L27: fetch slots with no object
  std::unordered_map<uint64_t, uint32_t> surf_by_edram;
  uint64_t epoch = 0;  // S100 speed: bumped by every change to the maps above or to an image's liveness (the slot cache's key)
};
inline Registry g_reg;

inline Image& ById(uint32_t id) { return g_reg.images[id - 1]; }

// S100 speed: an image record was made or changed since the last LiveSendImages (set at every write of `Image::rec`); the present then
// compares the table. Clean frames skip the compare (it walked every image ever made, every frame); a sweep every 60 frames counts any
// change made without the mark (census image_record_change_unmarked).
inline bool g_images_dirty = true;
inline uint32_t NewImage() {
  ++g_reg.epoch;
  g_images_dirty = true;
  g_reg.images.emplace_back();
  const uint32_t id = uint32_t(g_reg.images.size());
  g_reg.images.back().rec.id = id;
  return id;
}

// The fetch words of a texture (object header or slot record) decoded: size, kind, mips.
struct TexShape {
  uint32_t dim, format, width, height, depth, layers, mips;
  bool tiled, border;
  rdr::ImageKind kind;
};
inline TexShape ShapeOf(const uint32_t w[6]) {
  TexShape s{};
  s.dim = (w[5] >> 9) & 3;
  s.format = w[1] & 0x3F;
  s.tiled = (w[0] >> 31) != 0;
  s.border = (w[3] >> 31) != 0;
  const uint32_t adj = s.border ? 3 : 1;
  s.depth = 1;
  s.layers = 1;
  s.kind = rdr::ImageKind::Image2D;
  switch (s.dim) {
    case 0:  // 1D
      s.width = (w[2] & 0xFFFFFF) + adj;
      s.height = 1;
      break;
    case 2:  // 3D
      s.width = (w[2] & 0x7FF) + adj;
      s.height = ((w[2] >> 11) & 0x7FF) + adj;
      s.depth = ((w[2] >> 22) & 0x3FF) + adj;
      s.kind = rdr::ImageKind::Volume;
      break;
    case 3:  // cube
      s.width = (w[2] & 0x1FFF) + adj;
      s.height = ((w[2] >> 13) & 0x1FFF) + adj;
      s.layers = 6;
      s.kind = rdr::ImageKind::Cube;
      break;
    default:  // 2D or stacked
      s.width = (w[2] & 0x1FFF) + adj;
      s.height = ((w[2] >> 13) & 0x1FFF) + adj;
      if ((w[1] >> 10) & 1) s.layers = ((w[2] >> 26) & 0x3F) + 1;
      break;
  }
  // L12: the level count from the longest axis; no mip address means one level.
  const uint32_t top = Log2Floor(std::max({s.width, s.height, s.depth}));
  uint32_t max_level = (w[4] >> 6) & 0xF;
  const uint32_t mip_page = w[5] >> 12;
  if (!mip_page) max_level = 0;
  s.mips = std::min(max_level, top) + 1;
  return s;
}

inline void FillTextureRecord(Image& img, const uint32_t w[6]) {
  const TexShape s = ShapeOf(w);
  const Fmt f = FormatOf(s.format);
  img.is_texture = true;
  img.game_format = s.format;
  std::memcpy(img.header, w, sizeof(img.header));
  g_images_dirty = true;
  cap::ImageRecord& r = img.rec;
  r.width = uint16_t(s.width);
  r.height = uint16_t(s.height);
  r.depth = uint16_t(s.depth);
  r.mips = uint8_t(s.mips);
  r.layers = uint8_t(s.layers);
  r.format = uint8_t(f.host);
  r.kind = uint8_t(s.kind);
  r.usage = rdr::kUsageSampled;
  r.source = cap::kSourceTexture;
  r.game_format = uint8_t(s.format);
  r.flags = uint8_t((s.tiled ? cap::kImageTiled : 0) | (s.border ? cap::kImageBorder : 0));
  img.linear_static = !s.tiled && BaseFormat(s.format) != 2;
  if (!s.tiled && BaseFormat(s.format) == 2) r.flags |= cap::kImageMoviePlane;
  if (s.border) Note("texture_border_size");
  if (f.host == rdr::Format::Undefined) Note("texture_format_unknown", s.format);
}

// The slot record's words as a key: the bits SetTexture keeps from the slot's old record (0x82CA8B40 merge masks) and the type bits left out.
inline WordsKey KeyOfSlotWords(const uint32_t w[6]) {
  return WordsKey{{w[0] & ~0x003FFC03u, w[1] & ~0x00000800u, w[2], w[3] & ~0x7FF80000u, w[5] & ~0x000001FFu}};
}
// A texture header's words in the form SetTexture writes them into a slot (listing 0x82CA8B6C-0x82CA8BEC: words 1 and 5 become physical,
// +4 KB above 0xE0000000), keyed as above: every texture object is findable by the words a slot holds after the game nulls the stage.
inline WordsKey KeyOfHeaderWords(const uint32_t h[6]) {
  uint32_t w[6];
  std::memcpy(w, h, sizeof(w));
  w[1] = wwe2k14::guest_view::ToPhysical(h[1]);
  w[5] = wwe2k14::guest_view::ViewOffset(h[5]) + (h[5] & 0x1FFFFE00u);
  return KeyOfSlotWords(w);
}
inline void RememberHeaderWords(const uint32_t h[6], uint32_t id) {
  g_reg.tex_by_words[KeyOfHeaderWords(h)] = id;
  ++g_reg.epoch;
}

inline uint32_t RegisterTextureObject(uint8_t* base, uint32_t obj) {
  uint32_t w[6];
  for (uint32_t k = 0; k < 6; ++k) w[k] = db::Ld32(base, obj + 28 + 4 * k);
  const uint32_t id = NewImage();
  Image& img = ById(id);
  img.object = obj;
  FillTextureRecord(img, w);
  g_reg.tex_by_object[obj] = id;
  RememberHeaderWords(w, id);
  return id;
}

// The image of a texture object; a creation we did not see (the switch came on late) registers it on sight.
inline uint32_t TextureOfObject(uint8_t* base, uint32_t obj) {
  if (auto it = g_reg.tex_by_object.find(obj); it != g_reg.tex_by_object.end()) return it->second;
  Note("texture_unregistered_at_use");
  return RegisterTextureObject(base, obj);
}

// A slot whose words came from no object (L27), or whose object no longer describes it: identified by its contents' words.
// The type bits stay out of the key: a stage the game nulled keeps its texture's words with type 0, and the GPU samples them (run 6851).
// Texture objects are keyed too (RememberHeaderWords), so such a slot finds the image the game copied into, not a new empty one.
// S100 texture coherence pass (census first): the first page of each copy destination's base level, so a texture whose words match no
// copy but whose memory a copy wrote (GPU output read through another header) is noted before any rule is written for it.
inline std::unordered_map<uint32_t, uint32_t> g_copy_dest_by_page;
inline uint32_t TextureOfWords(const uint32_t w[6]) {
  const WordsKey k = KeyOfSlotWords(w);
  if (auto it = g_reg.tex_by_words.find(k); it != g_reg.tex_by_words.end() && ById(it->second).live) return it->second;
  if (auto c = g_copy_dest_by_page.find(w[1] >> 12); c != g_copy_dest_by_page.end()) {
    const Image& d = ById(c->second);
    if (d.live && d.copy_dest) {
      Note("slot_words_miss_copy_destination");  // the census: the same memory, other words
      static uint32_t said = 0;
      if (said++ < 16)
        REXLOG_INFO("[rdr][coherence] a texture at {:08X} ({:08X} {:08X} {:08X}) matches no copy's words but copy image {} ({}x{}) wrote that page",
                    w[1] & 0xFFFFF000u, w[0], w[2], w[3], c->second, d.rec.width, d.rec.height);
    }
  }
  uint32_t words[6];
  std::memcpy(words, w, sizeof(words));
  words[0] = (words[0] & ~3u) | 2u;
  const uint32_t id = NewImage();
  FillTextureRecord(ById(id), words);
  g_reg.tex_by_words[k] = id;
  return id;
}

// Colour surface format (RB_COLOR_INFO bits 16-19) -> storage. The game's 16-bit fixed targets are RGBA16F: we write both ends (Session 100 design change).
inline rdr::Format SurfaceStorage(uint32_t f) {
  using rdr::Format;
  switch (f) {
    case 0: case 1: return Format::RGBA8;
    case 2: case 3: case 10: case 12: return Format::RGB10A2;
    case 4: case 5: return Format::RGBA16F;
    case 6: return Format::RG16F;
    case 7: return Format::RGBA16F;
    case 14: return Format::R32F;
    case 15: return Format::RG32F;
    default: return Format::Undefined;
  }
}

// S100 (Cena's lines, runs 7024-7028; Jarrod confirmed gone at 1x on the phone): only the game's SCREEN images follow the render scale -
// 16:9 (1280x720 and its halves, quarters, ...; the height within 16 pixels of rounding). Images laid out like a model's texture (the
// sweat / blood map, which the game rebuilds each frame from itself read half a texel over: exact only at the game's own size) and the
// square shadow maps keep the game's size: at 1.5x / 2x the liquid map drifted across the UV-island borders frame by frame.
inline bool ScreenShapedRule(uint32_t width, uint32_t height) {
  if (width < 64) return false;
  const int64_t diff = int64_t(width) * 9 - int64_t(height) * 16;
  return diff >= -16 * 16 && diff <= 16 * 16;
}
// S100 diagnosis (the Undertaker's blocks, run 7030): --wwe2k14_rdr_scale_all restores the old rule (every image scales); each size the
// rule leaves at the game's size is logged once ([rdr][scale] lines), so a screen effect it missed shows by name.
inline bool g_scale_all = false;  // read at StartLive
inline bool ScreenShaped(uint32_t width, uint32_t height, const char* kind = "image") {
  if (g_scale_all) return true;
  const bool screen = ScreenShapedRule(width, height);
  if (!screen) {
    static std::mutex m;
    static std::set<uint64_t> seen;
    std::lock_guard lock(m);
    if (seen.insert((uint64_t(width) << 32) | height).second)
      REXLOG_INFO("[rdr][scale] {} {}x{} keeps the game's size (not 16:9)", kind, width, height);
  }
  return screen;
}

// S101 names (A1b, docs\renderer\NAMES.md "Surface roles"): the role a surface's creator declares (surface_roles_hook.h records header ->
// creating call site; surface_role_table.h maps the function to a role). The role rides in the image record (ImageRecord::role /
// role_flags), so the renderer and every capture have it. Unknown creators keep the S100 16:9 rule, flagged guessed and counted.
inline uint8_t SurfaceRoleUsage(uint32_t obj, uint32_t width, uint32_t height, bool depth, cap::ImageRecord& r) {
  namespace ro = rdr_adapter::roles;
  ro::Created c;
  const ro::CreatorRow* row = ro::Global().Find(obj, c) ? ro::FindCreator(c.call_site) : nullptr;
  // A header rewritten in place for another size keeps its creator's entry: trust the row only for the size it was created with (the
  // creator may double a side for MSAA, sub_82CA82F0) (S101 review 7).
  if (row && c.width && c.height) {
    const bool same = (width == c.width || width == 2 * c.width) && (height == c.height || height == 2 * c.height);
    if (!same) {
      Note("surface_role_size_differs_from_create");
      row = nullptr;
    }
  }
  if (!row) {
    ++g_roles.guessed;
    r.role_flags = cap::kRoleGuessed;
    const bool scales = ScreenShaped(width, height, depth ? "depth target" : "target");
    if (scales) r.role_flags |= cap::kRoleScales;
    return scales ? rdr::kUsageScaled : 0;
  }
  ++g_roles.declared;
  cap::SurfaceRole role = row->role;
  if (role == cap::SurfaceRole::SceneColor && depth) role = cap::SurfaceRole::SceneDepth;
  const bool scales = g_scale_all || row->scales == ro::ScaleRule::Yes ||  // (--wwe2k14_rdr_scale_all reaches declared roles too, review 5)
                      (row->scales == ro::ScaleRule::IfScreenShaped && ScreenShapedRule(width, height));
  r.role = uint8_t(role);
  r.role_flags = uint8_t(cap::kRoleDeclared | (scales ? cap::kRoleScales : 0) | (row->lasting ? cap::kRoleLasting : 0));
  static std::mutex m;
  static std::set<uint64_t> seen;
  {
    std::lock_guard lock(m);
    if (seen.insert((uint64_t(row->start) << 32) | (uint64_t(width) << 16) | height).second)
      REXLOG_INFO("[rdr][role] {} {}x{} {}: {} (creator call 0x{:08X}){}{}", depth ? "depth target" : "target", width, height, row->name,
                  cap::SurfaceRoleName(role), c.call_site, scales ? ", scales" : ", keeps the game's size", row->lasting ? ", lasting" : "");
  }
  return uint8_t((scales ? rdr::kUsageScaled : 0) | (row->lasting ? rdr::kUsageLasting : 0));
}

// The image of a surface header. The header's own words decide: a header rewritten for another size or format is a new image.
inline uint32_t SurfaceOfObject(uint8_t* base, uint32_t obj, bool depth) {
  const uint32_t info = db::Ld32(base, obj + 24);
  const uint32_t edram = db::Ld32(base, obj + 28);
  const uint32_t size_word = db::Ld32(base, obj + 36);
  const uint32_t width = ((size_word >> 18) & 0x3FFF) + 1, height = ((size_word >> 3) & 0x7FFF) + 1;
  const uint32_t game_format = depth ? (0xD0u | ((edram >> 16) & 1)) : ((edram >> 16) & 0xF);
  if (auto it = g_reg.surf_by_object.find(obj); it != g_reg.surf_by_object.end()) {
    const Image& img = ById(it->second);
    if (img.rec.width == width && img.rec.height == height && img.game_format == game_format && img.surf_key == size_word) return it->second;
    Note("surface_header_rewritten");
  }
  const uint32_t id = NewImage();
  Image& img = ById(id);
  img.object = obj;
  img.depth = depth;
  img.game_format = game_format;
  img.surf_key = size_word;
  g_images_dirty = true;
  cap::ImageRecord& r = img.rec;
  r.width = uint16_t(width);
  r.height = uint16_t(height);
  r.format = uint8_t(depth ? rdr::Format::Depth : SurfaceStorage(game_format));
  r.kind = uint8_t(rdr::ImageKind::Image2D);
  r.usage = uint8_t(rdr::kUsageTarget | SurfaceRoleUsage(obj, width, height, depth, r));  // S101 names: the creator's declared role (else the S100 16:9 rule)
  r.source = cap::kSourceSurface;
  r.game_format = uint8_t(game_format);
  if (!depth && r.format == uint8_t(rdr::Format::Undefined)) Note("surface_format_unknown", game_format);
  if ((info >> 16) & 3) Note("surface_msaa_view");  // C24: unconfirmed unit
  g_reg.surf_by_object[obj] = id;
  // C3: surfaces at the same EDRAM base do not alias; two headers of one (base, pitch, format) would be one surface to the game.
  const uint64_t key = (uint64_t(edram & 0xFFF) << 32) | (uint64_t(info & 0x3FFF) << 8) | game_format;
  auto [it, inserted] = g_reg.surf_by_edram.try_emplace(key, id);
  if (!inserted && it->second != id) Note("surface_two_headers_one_edram_identity");
  return id;
}

// ---- The bind trace (wwe2k14_rdr_bind_trace; S100 visual parity): every SetTexture, fetch flush, copy and draw of N whole frames ----------
inline std::atomic<uint32_t> g_trace_left{0};
inline uint32_t g_trace_draw = 0;
inline bool TraceOn() { return g_trace_left.load(std::memory_order_relaxed) != 0; }

// A fetch record in short: type, base address, format, size (2D field layout), dimension.
inline std::string TraceWords(const uint32_t w[6]) {
  char s[96];
  std::snprintf(s, sizeof(s), "t%u @%08X f%u %ux%u d%u", w[0] & 3, w[1] & 0xFFFFF000u, w[1] & 0x3F, (w[2] & 0x1FFF) + 1, ((w[2] >> 13) & 0x1FFF) + 1,
                (w[5] >> 9) & 3);
  return s;
}

// The SetTexture observer (src\nf\observers.h): runs before the library's own work, so the record is as it was before this call.
inline void OnSetTextureTrace(PPCContext& ctx, uint8_t* base) {
  if (!TraceOn()) return;
  const uint32_t dev = ctx.r3.u32, stage = ctx.r4.u32, ptr = ctx.r5.u32;
  std::string what = "null";
  if (ptr) {
    uint32_t w[6];
    for (uint32_t k = 0; k < 6; ++k) w[k] = db::Ld32(base, ptr + 28 + 4 * k);
    what = TraceWords(w);
  }
  const uint32_t before = stage < 32 ? db::Ld32(base, dev + kDevFetch + 24 * stage) : 0;
  const uint32_t dirty = uint32_t(db::Ld64(base, dev + 24) & 0xFFFFFFFFull);
  REXLOG_INFO("[rdr][trace] set stage {} ptr {:08X} [{}] lr {:08X}; record type before {}, dirty before {}", stage, ptr, what, uint32_t(ctx.lr), before & 3,
              stage < 32 ? (dirty >> (31 - stage)) & 1 : 0);
}

// What the GPU holds in each fetch slot (S100: Brock lost his self-shadowing and the post chain its inputs; Jarrod saw "ambient
// occlusion" and contrast missing). The library's draw writers upload a slot's 24-byte record only when its dirty bit is set (device+24,
// slot n = bit 31 - n of the low word; indexed draw 0x82CB1B30-0x82CB1B7C, inline Begin 0x82CB1570; src\d3d_layer.h keeps the rule), then
// clear the mask. SetTexture(stage, null) (0x82CA8B40, path 0x82CA8C48) only clears the record's type bits and does NOT set the dirty bit:
// the GPU goes on reading the last texture uploaded for that stage. The game's shadow-map blur and post passes bind their inputs that way
// (40 such slot reads a frame in the Brock match). So slots are read from this copy, updated at the points the library uploads.
struct GpuFetchSlot {
  uint32_t words[6] = {};
  uint32_t object = 0;  // the device's object pointer when the record was uploaded (0 once that object is released)
  bool known = false;
};
inline std::array<GpuFetchSlot, 32> g_gpu_fetch{};
inline uint32_t g_gpu_fetch_device = 0;

// At a draw writer's entry, before it flushes (its mask is read here as the writer reads it). A clean slot whose record is a texture is
// taken too: a type-2 record can only differ from the GPU's after a SetTexture, which sets the dirty bit, so a clean one was uploaded by a
// writer we do not see. Only a clean slot whose record is not a texture keeps the copy: that is the null SetTexture.
inline void SyncGpuFetch(uint8_t* base, uint32_t dev) {
  if (dev != g_gpu_fetch_device) {
    g_gpu_fetch = {};
    g_gpu_fetch_device = dev;
  }
  const uint32_t dirty = uint32_t(db::Ld64(base, dev + 24) & 0xFFFFFFFFull);
  if (TraceOn() && (dirty & 0xFFFFF000u)) {
    std::string up;
    for (uint32_t n = 0; n < 20; ++n) {
      if (!((dirty >> (31 - n)) & 1)) continue;
      uint32_t w[6];
      for (uint32_t k = 0; k < 6; ++k) w[k] = db::Ld32(base, dev + kDevFetch + 24 * n + 4 * k);
      up += " s" + std::to_string(n) + "[" + TraceWords(w) + "]";
    }
    REXLOG_INFO("[rdr][trace] flush uploads{}", up);
  }
  for (uint32_t n = 0; n < 20; ++n) {  // the texture slots (pixel 0-15, vertex 16-19)
    GpuFetchSlot& g = g_gpu_fetch[n];
    const uint32_t rec = dev + kDevFetch + 24 * n;
    const uint32_t w0 = db::Ld32(base, rec);
    const bool is_dirty = (dirty >> (31 - n)) & 1;
    if (!is_dirty && (w0 & 3) != 2 && g.known) continue;
    for (uint32_t k = 0; k < 6; ++k) g.words[k] = db::Ld32(base, rec + 4 * k);
    g.object = db::Ld32(base, dev + kDevTexObj + 4 * n);
    g.known = true;
  }
}

inline void OnReleased(uint32_t addr) {
  ++g_reg.epoch;
  for (GpuFetchSlot& g : g_gpu_fetch) {
    if (g.object == addr) g.object = 0;
  }
  if (auto it = g_reg.tex_by_object.find(addr); it != g_reg.tex_by_object.end()) {
    ById(it->second).live = false;
    if (g_live.active()) {
      g_live.Async().Unwatch(it->second);  // lane 232
      g_live.ReleaseImage(it->second);
    }
    g_reg.tex_by_object.erase(it);
  }
  if (auto it = g_reg.surf_by_object.find(addr); it != g_reg.surf_by_object.end()) {
    ById(it->second).live = false;
    if (g_live.active()) {
      g_live.Async().Unwatch(it->second);  // lane 232
      g_live.ReleaseImage(it->second);
    }
    g_reg.surf_by_object.erase(it);
  }
}
