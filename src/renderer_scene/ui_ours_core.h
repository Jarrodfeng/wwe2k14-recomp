// Our UI textures, the GPU-free half (Session 101, lane 66; docs\renderer\UI_PROMOTION_MAP.md sections 3-5, docs\renderer\UI_OURS.md).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// The UI's 2D draws (op 19 at the three sprite / glyph functions) keep the stream's quad, colour, blend, order and target (ReplaceOut::keep_stream_geometry);
// only the texture in slot 0 is replaced by a picture read from the player's own menu pacs. Three parts, all here, none touching a renderer:
//   UiFileIndex    every texture of the menu pacs as (width, height, format, mips, hash of the bytes) -> pac + entry path, built per pac (BuildPacIndex, on a worker
//                  or on the calling thread, with an optional hash cache under out\) and published pac by pac;
//   UiFileLoader   one indexed texture's bytes, decoded again from the pac (the index only knows hashes), with the plants that change them (rb_swap, short_mips);
//   UiCore         the decisions: which draw is claimed, which file picture it joins (declared key first, else content), the per-draw checks (size, format, mips,
//                  the bytes of OUR upload against the stream's content hash, key -> image still 1:1 this frame), the census, and the join-side plants.
// The GPU half (ui_ours.h) uploads what UiCore asks for and fills ReplaceOut. A unit test (tools\ui\ui_ours_test.cpp) runs this file on the files and the captures.
//
// The pairing key is the draw's origin `part` (the texture handle key the game stores in the sprite, word 0 of the 64-bit handle at sprite+400 / +416). It names ONE
// image for its life (UI_PROMOTION_MAP.md 2.1 [C]); the key is what a declared join (lane UD, not built) attaches a file to, and what the content join caches by.
//
// pac.h needs a zlib: ui_ours_core.cpp holds its own STATIC stb_image like crowd_body.cpp / arena_game_space.cpp (the game exe and the phone library must not export
// stbi_*). It does NOT use ui_assets.cpp (not in the game or fview builds; it wants the outside stbi). No game or SDK includes.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../renderer/api.h"
#include "../renderer_adapter/draw_replace_hook.h"
#include "../renderer_adapter/image_content_feed.h"

namespace rdr_scene {

// The call sites of the three functions that draw a textured 2D quad (UI_PROMOTION_MAP.md 1.1). Origin op 19 (inline vertices).
inline constexpr uint32_t kUiSpriteSite = 0x82BC5688u, kUiGlyphSite = 0x82BC3784u, kUiSpriteMSite = 0x82BC8BECu;

// The pacs indexed, in the order a worker does them (small and hot first). HD pacs only: the SD pacs hold the SD set of the same art (whether the game ever draws the SD set is [U]; an SD pac is not indexed, so a picture only SD holds would stay unjoined, counted).
inline constexpr const char* kUiPacs[] = {"pac\\menu\\fontHD.pac", "pac\\menu\\MnUIHD.pac", "pac\\menu\\menuHD.pac", "pac\\menu\\MnLayHD.pac",
                                          "pac\\menu\\LoadHD.pac", "pac\\menu\\MatchHD.pac", "pac\\menu\\CAE.pac",   "pac\\menu\\menu.pac",
                                          // lane 126: 2D art outside the menu pacs (Bryan's entrance zip: the lower-third's glyph sheets in evt/evtgdhd.pac, the effect
                                          // sprites in gm.pac). Cheap (9.5 MB + 14 MB packed): the first tier.
                                          "pac\\evt\\evtgdhd.pac", "pac\\gm.pac",
                                          // lane 126: the second tier, on demand (UiOurs::MaybeStartDeep): bgEtc (arena-independent effect sprites, all textures) ...
                                          "pac\\bg\\bgEtc.pac"};
inline constexpr size_t kUiPacCount = sizeof(kUiPacs) / sizeof(kUiPacs[0]);
// ... and the CURRENT arena's bgNN (review F1: never all of them; the arena id comes from the scene record, ArenaCore::ArenaOfRecord). Pac index kUiBgBase + N is
// "pac\bg\bgNN.pac" for any N 0..99 (so the bg66-68 / bg76-80 pacs a fixed list missed are covered); only its texture-directory members named "*lightmap*" are indexed.
inline constexpr size_t kUiTier1Count = 10;   // lane 126 (review F2): pacs [0, kUiTier1Count) are indexed at start-up; kUiTier1Count is bgEtc, the first of the second tier
inline constexpr size_t kUiBgEtc = kUiTier1Count;
inline constexpr size_t kUiBgBase = kUiPacCount;   // the first arena pac index
inline constexpr size_t kUiMaxArena = 99;
constexpr bool UiConstEq(const char* a, const char* b) { return *a == *b && (*a == 0 || UiConstEq(a + 1, b + 1)); }
static_assert(kUiTier1Count < kUiPacCount, "the second tier must have at least bgEtc");
static_assert(UiConstEq(kUiPacs[kUiTier1Count], "pac\\bg\\bgEtc.pac"), "the tier boundary moved: kUiPacs[kUiTier1Count] must be bgEtc");
static_assert(kUiBgBase + kUiMaxArena < 255, "UiFileEntry::pac is a uint8_t");
inline std::string UiPacPath(size_t pac) {
  if (pac < kUiPacCount) return kUiPacs[pac];
  char buf[40];
  std::snprintf(buf, sizeof buf, "pac\\bg\\bg%02d.pac", int(pac - kUiBgBase));
  return buf;
}
inline bool UiPacValid(size_t pac) { return pac < kUiBgBase + kUiMaxArena + 1; }


// The 360's A8R8G8B8 pictures sit in the capture as storage LABELLED RGBA8 holding the file's bytes (B, G, R, A in memory order), read through the slot swizzle B, G, R, A
// (measured: Check B's menu zip, image with 512 x 1024; every BC draw reads R, G, B, A). Ours is the file's own BGRA8. The two labels hold the same bytes, so the join and the
// size check compare byte classes; UiSwizzleMatches (below) then proves the colours come out the same, per draw.
inline rdr::Format UiByteClass(rdr::Format f) { return f == rdr::Format::BGRA8 ? rdr::Format::RGBA8 : f; }
// For each of the view's channels R, G, B, A: which memory byte of a texel it reads, given the storage label and the slot swizzle (VkComponentSwizzle 0 = identity,
// 3 R, 4 G, 5 B, 6 A; 1 zero, 2 one -> 0xFF). false for a format that is not 4 bytes a texel (BC formats: byte = the block's own channel order, compared by value).
inline bool UiChannelBytes(rdr::Format label, const uint8_t* swizzle, uint8_t out[4]) {
  if (label != rdr::Format::RGBA8 && label != rdr::Format::BGRA8) return false;
  static const uint8_t kRgba[4] = {0, 1, 2, 3}, kBgra[4] = {2, 1, 0, 3};   // component R, G, B, A -> memory byte
  for (int i = 0; i < 4; ++i) {
    const uint8_t s = swizzle ? swizzle[i] : 0;
    const uint8_t comp = s == 0 ? uint8_t(3 + i) : s;
    out[i] = comp >= 3 && comp <= 6 ? (label == rdr::Format::RGBA8 ? kRgba : kBgra)[comp - 3] : 0xFF;
  }
  return true;
}
// Does OUR upload (label `ours`, identity swizzle) show the same colours as the stream's slot (label `stream`, swizzle `swizzle`)?
inline bool UiSwizzleMatches(rdr::Format ours, rdr::Format stream, const uint8_t* swizzle) {
  uint8_t a[4], b[4];
  if (UiChannelBytes(ours, nullptr, a) && UiChannelBytes(stream, swizzle, b)) {
    for (int i = 0; i < 4; ++i)
      if (a[i] != b[i]) return false;
    return true;
  }
  if (ours != stream) return false;   // a block format: the labels are equal (the class check) and the slot must read R, G, B, A
  for (int i = 0; i < 4; ++i)
    if (swizzle && swizzle[i] != 0 && swizzle[i] != 3 + i) return false;
  return true;
}

struct UiFaults {          // the planted faults (UI_PROMOTION_MAP.md 4); fview --replace-fault NAME. stale_after_join is the SESSION's plant (it changes the stream), not the hook's.
  bool first_texture = false;   // every claimed draw takes the first joined texture (the crowd's "one texture for every member")
  bool icon_as_font = false;    // fontHD font000's glyph sheet joined to its icon sheet (same size and format)
  bool rb_swap = false;         // 4-byte-per-texel pictures loaded with R and B exchanged
  bool key_shift = false;       // the join table off by one key
  bool short_mips = false;      // our upload keeps mip 0 only
  bool names_stripped = false;  // no origins: the hook claims nothing
  bool stale_after_join = false;  // (session plant; the hook only accepts the name)
  bool gpu_copy_after_join = false;  // (test plant: the content feed does NOT forget an image a GPU copy overwrites; the hook only accepts the name)
  bool no_view_check = false;     // lane 126 (plant no_view_check = the "no_u_sign" idea: a signed / biased VIEW of the stream image is not refused, ours is drawn plain)
  bool deep_wrong_arena = false;  // lane 126 (plant): the second tier indexes the NEIGHBOURING arena's pac (N + 1): the page sprites stay unjoined
  bool tier_boundary = false;     // lane 126 (plant): the name filter starts at bgEtc (kUiTier1Count): bgEtc's effect sprites are filtered out and stay unjoined
  bool no_deep = false;           // lane 126 (plant no_deep): the second tier (bg pacs) never starts, the lightmap page pass stays unjoined
  bool no_checks = false;         // "unchecked_<plant>": the per-draw checks only COUNT, the draw is replaced anyway (the picture then shows what the plant does; test only)
  // Names: the seven above; "unchecked_<one of the first six>"; "stale_stream" = stale_after_join for the stream arm (the hook claims nothing: names_stripped).
  static bool Parse(const std::string& name, UiFaults& out);   // false: unknown name
};

// One indexed picture.
struct UiFileEntry {
  uint8_t pac = 0;                 // index into kUiPacs
  std::string path;                // "p<N>/<id>[/<id>...]" (a DDS leaf) or "p<N>/<id>...:name" (a member of a texture directory)
  uint16_t width = 0, height = 0;
  rdr::Format format = rdr::Format::Undefined;
  uint8_t mips = 1;
  uint32_t bytes = 0;              // all mips, tightly packed
  std::vector<uint64_t> prefix;    // prefix[k-1] = ImageContentHash (xxh3) of the first k mips' bytes
  uint32_t same = 1;               // leaves of the index that hold this exact picture (the class; joins go to the first)
};

class UiFileIndex {
 public:
  // Publishes one pac's entries (the order inside a pac is its walk order; duplicates of an earlier entry only bump its `same`).
  void AddPac(uint8_t pac, std::vector<UiFileEntry>&& entries);
  // The first entry holding exactly this picture (the first `mips` mips of it are the stream's content). false: not indexed.
  bool Find(uint32_t width, uint32_t height, rdr::Format format, uint32_t mips, uint64_t hash, uint32_t& id) const;
  // Every entry holding it, in id order (lane 66 review F4: a BGRA8 and an RGBA8 file may hold the same bytes; the caller takes the first whose colours come out right).
  void FindAll(uint32_t width, uint32_t height, rdr::Format format, uint32_t mips, uint64_t hash, std::vector<uint32_t>& ids) const;
  bool Get(uint32_t id, UiFileEntry& out) const;
  bool FindPath(uint8_t pac, const std::string& path, uint32_t& id) const;
  uint32_t generation() const { return generation_.load(); }   // bumps when entries were added
  size_t texture_count() const;
  uint32_t pacs_done() const { return pacs_done_.load(); }
  std::string Summary() const;

 private:
  static uint64_t Key(uint32_t w, uint32_t h, rdr::Format f, uint32_t mips, uint64_t hash) {
    f = UiByteClass(f);
    return hash ^ (uint64_t(w) * 0x9E3779B185EBCA87ull) ^ (uint64_t(h) * 0xC2B2AE3D27D4EB4Full) ^ (uint64_t(uint8_t(f)) << 56) ^ (uint64_t(mips) << 48);
  }
  mutable std::mutex mu_;
  std::deque<UiFileEntry> entries_;
  std::unordered_multimap<uint64_t, uint32_t> by_key_;       // prefix keys of every entry (first occurrence only)
  std::unordered_map<std::string, uint32_t> by_path_;        // "<pac>|<path>"
  std::atomic<uint32_t> generation_{0}, pacs_done_{0};
};

// The folder-or-image the pacs are read from (GameFiles); `cache_dir` (may be empty) holds one hash cache file per pac: valid while the pac's size and time are the same.
struct UiIndexOptions {
  std::string game_root;
  std::string cache_dir;
  size_t names_only_from = kUiBgBase;   // lane 126: pacs at or above this index keep only the texture-directory members named "*lightmap*" (the arena bgNN pacs; bgEtc keeps everything). Plant tier_boundary lowers it to bgEtc.
};
// Builds the entries of kUiPacs[pac]. false + err: the pac is missing or unreadable (the index then lacks it, loudly: the caller logs err).
bool BuildPacIndex(const UiIndexOptions& options, uint8_t pac, std::vector<UiFileEntry>& out, std::string& err, bool* from_cache = nullptr);

// Bytes of one mip level of a picture the UI uploads (BC1 / BC2 / BC3 / 4-byte).
inline size_t UiMipBytes(rdr::Format f, uint32_t w, uint32_t h) {
  w = w ? w : 1, h = h ? h : 1;
  switch (f) {
    case rdr::Format::BC1: return size_t((w + 3) / 4) * ((h + 3) / 4) * 8;
    case rdr::Format::BC2:
    case rdr::Format::BC3: return size_t((w + 3) / 4) * ((h + 3) / 4) * 16;
    case rdr::Format::BGRA8:
    case rdr::Format::RGBA8: return size_t(w) * h * 4;
    default: return 0;
  }
}

// A texture's bytes as the GPU gets them (mip 0 first, tightly packed), after the loader's plants.
struct UiLoaded {
  uint32_t width = 0, height = 0, mips = 1;
  rdr::Format format = rdr::Format::Undefined;
  std::vector<uint8_t> data;
  std::vector<uint64_t> prefix;    // ImageContentHash (xxh3) of the first k mips of the FULL file chain AS LOADED (after rb_swap): the per-draw bytes check reads this; `data` / `mips` hold only the mips asked for
};

class UiFileLoader {
 public:
  UiFileLoader(const std::string& game_root, const UiFaults& faults);
  ~UiFileLoader();
  // Re-reads the entry from its pac and checks it is the indexed picture (the stored hash): false + err for a stale index or a read failure.
  // `want_mips`: how many mips of the chain to keep for the upload (the stream slot's max_level + 1, capped by the chain and by what the stream holds; short_mips forces 1).
  bool Load(const UiFileEntry& entry, uint32_t want_mips, UiLoaded& out, std::string& err);
  void ReleasePacs();   // drops the decoded pacs (hundreds of MB): called when a batch of loads is done
  size_t resident_pacs() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// ---- the decisions ------------------------------------------------------------------------------------------------------------------------------------------
// What the GPU half gives the core for a joined entry.
struct UiTexture {
  uint32_t handle = 0;                 // the renderer's ImageId (opaque here)
  uint32_t width = 0, height = 0, mips = 1;
  rdr::Format format = rdr::Format::Undefined;
  std::vector<uint64_t> prefix;        // as UiLoaded::prefix
  mutable uint64_t last_used = 0;      // UiCore's frame number of the last draw that used it (the GPU half releases pictures nobody uses)
};
class UiTextureProvider {
 public:
  virtual ~UiTextureProvider() = default;
  // The uploaded texture for an indexed entry, or null with `pending` = true when it is being loaded (the draw stays the stream's this frame), or null / false when it cannot be loaded.
  virtual const UiTexture* Texture(uint32_t entry, const UiFileEntry& e, uint32_t mips, bool& pending) = 0;
  // Bumps whenever a texture pointer handed out may no longer be valid (a release): UiCore caches the pointer per key until it moves.
  virtual uint32_t Epoch() const { return 0; }
};

struct UiCensus {
  uint64_t claimed = 0, replaced = 0;
  uint64_t unjoined = 0, mismatch = 0, movie = 0, gpu = 0, untextured = 0, pending = 0, nokey = 0;
  uint64_t joined_declared = 0, joined_content = 0, changed_after_join = 0, rejoined = 0;
  uint64_t mismatch_size = 0, mismatch_mips = 0, mismatch_bytes = 0, mismatch_key = 0, mismatch_swizzle = 0, mismatch_view = 0, load_failed = 0, unchecked = 0;
};

class UiCore {
 public:
  using ContentFn = std::function<bool(uint32_t, rdr_adapter::ImageContent&)>;
  UiCore(const UiFaults& faults, ContentFn content, const UiFileIndex* index) : faults_(faults), content_(std::move(content)), index_(index) {}

  bool Claims(const rdr_adapter::DrawOrigin& o) const {
    return !faults_.names_stripped && o.op == 19 && (o.call_site == kUiSpriteSite || o.call_site == kUiGlyphSite || o.call_site == kUiSpriteMSite);
  }
  void BeginFrame() { frame_keys_.clear(), ++frame_; }
  uint64_t frame() const { return frame_; }
  // A declared join (lane UD's table): key -> index entry; wins over a content join.
  void Declare(uint32_t key, uint32_t entry) { declared_[key] = entry; }

  struct Verdict {
    bool ours = false;
    rdr_adapter::KeepWhy why = rdr_adapter::KeepWhy::Unjoined;
    const UiTexture* texture = nullptr;
    uint32_t entry = 0;
    bool declared = false;
  };
  // One claimed draw: `stream_image` = the capture image id behind the draw's slot 0 (0: none).
  // `swizzle`: the stream slot's composed swizzle (4 VkComponentSwizzle values; null = identity), checked against what our upload gives.
  // `min_level` / `max_level`: the slot's LOD range. Ours is uploaded with min(chain, max_level + 1, mips the stream holds) mips (lane 70: the game's images often hold fewer mips than the file's chain); a slot with min_level != 0 stays the stream's.
  Verdict Decide(uint32_t key, uint32_t stream_image, const uint8_t* swizzle, uint32_t view_format, uint32_t min_level, uint32_t max_level, UiTextureProvider& provider);

  const UiCensus& census() const { return census_; }
  // lane 126: the last Decide returned Unjoined and the caller indexed more pacs and will ask again for the SAME draw (offline, the second tier runs synchronously): the first ask is not counted.
  void RetractUnjoined() { if (census_.claimed) --census_.claimed; if (census_.unjoined) --census_.unjoined; }
  std::string Report(const UiFileIndex& index) const;
  size_t joined_keys() const { return joins_.size(); }
  const UiFaults& faults() const { return faults_; }

 private:
  struct Join {
    uint32_t image = 0;        // the stream image the join was made for
    uint64_t generation = 0;   // its content generation then (table-wide, monotonic)
    // The hot path (lane 66 review C2): the texture last used for this key, so a draw does not Get the entry or ask the provider again.
    const UiTexture* tex = nullptr;
    uint32_t tex_entry = 0, tex_want = 0, chain_mips = 0, tex_epoch = 0;
    uint32_t index_gen = 0;    // the index generation when an unjoined verdict was made (retry when it moves)
    int64_t entry = -1;        // -1: unjoined
    bool declared = false;
  };
  bool JoinByContent(const rdr_adapter::ImageContent& c, const uint8_t* swizzle, uint32_t& entry);

  UiFaults faults_;
  ContentFn content_;
  const UiFileIndex* index_;
  std::unordered_map<uint32_t, Join> joins_;        // by key
  std::unordered_map<uint32_t, uint32_t> frame_keys_;  // key -> stream image, this frame
  std::unordered_map<uint32_t, uint32_t> declared_;
  std::vector<uint32_t> join_order_;                // keys in the order they first joined (key_shift)
  uint64_t frame_ = 0;
  int64_t first_entry_ = -1;                        // first_texture
  UiCensus census_;
};

}  // namespace rdr_scene

