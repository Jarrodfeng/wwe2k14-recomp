// The file index of the player's own menu pacs (KEPT: reaches only renderer\api.h, image_content_table.h and src\assets\; no stream header). Split out of
// ui_ours_core.h (S103 PN1, COMPILE_OUT_LIST.md B4a; docs\renderer\PICTURES_NO_STREAM.md 5.4). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//   UiFileIndex    every texture of the menu pacs as (width, height, format, mips, hash of the bytes) -> pac + entry path (BuildPacIndex);
//   UiFileLoader   one indexed texture's bytes, decoded again from the pac, with the loader plants (UiFileFaults: rb_swap, short_mips).
// pac.h needs a zlib: ui_file_index.cpp holds its own STATIC stb_image like crowd_body.cpp (the game exe and the phone library must not export stbi_*).
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../renderer/api.h"

namespace rdr_scene {

struct UiFileFaults {   // the loader's share of the planted faults (UiFaults derives from this)
  bool rb_swap = false;       // 4-byte-per-texel pictures loaded with R and B exchanged
  bool short_mips = false;    // our upload keeps mip 0 only
  uint32_t fail_loads = 0;    // S103 PN4F (rig only): the first N Load calls fail (a transient read error), to prove a failed picture is retried
};

// The pacs indexed, in the order a worker does them (small and hot first). HD pacs only: the SD pacs hold the SD set of the same art (whether the game ever draws the SD set is [U]; an SD pac is not indexed, so a picture only SD holds would stay unjoined, counted).
inline constexpr const char* kUiPacs[] = {"pac\\menu\\fontHD.pac", "pac\\menu\\MnUIHD.pac", "pac\\menu\\menuHD.pac", "pac\\menu\\MnLayHD.pac",
                                          "pac\\menu\\LoadHD.pac", "pac\\menu\\MatchHD.pac", "pac\\menu\\CAE.pac",   "pac\\menu\\menu.pac",
                                          // lane 126: 2D art outside the menu pacs (Bryan's entrance zip: the lower-third's glyph sheets in evt/evtgdhd.pac, the effect
                                          // sprites in gm.pac). Cheap (9.5 MB + 14 MB packed): the first tier.
                                          "pac\\evt\\evtgdhd.pac", "pac\\gm.pac",
                                          // S103 PN5: game2dHD.pac (10.8 MB) holds the pause background (pauseBG, 1024x512 DXT5, leaves p0..p4/41: key 92603064 of c2d_pause, the one
                                          // picture the other tier-1 pacs did not join; found by tools\ui\ui_pacs.py hash index, out\s103\pn5\). First tier, indices 0-9 unchanged.
                                          "pac\\game2dHD.pac",
                                          // lane 126: the second tier, on demand (UiOurs::MaybeStartDeep): bgEtc (arena-independent effect sprites, all textures) ...
                                          "pac\\bg\\bgEtc.pac"};
inline constexpr size_t kUiPacCount = sizeof(kUiPacs) / sizeof(kUiPacs[0]);
// ... and the CURRENT arena's bgNN (review F1: never all of them; the arena id comes from the scene record, ArenaCore::ArenaOfRecord). Pac index kUiBgBase + N is
// "pac\bg\bgNN.pac" for any N 0..99 (so the bg66-68 / bg76-80 pacs a fixed list missed are covered); only its texture-directory members named "*lightmap*" are indexed.
inline constexpr size_t kUiTier1Count = 11;   // lane 126 (review F2): pacs [0, kUiTier1Count) are indexed at start-up; kUiTier1Count is bgEtc, the first of the second tier
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
// PN5 review B3: what the walk of a pac's EPAC-header roster directories (SSFA / SSFB / SSFC) found: directories, entries read, names skipped (not 4 alphanumerics),
// duplicate names (the first is kept), and mismatches between a directory row's count and its rows (note says which). Filled only when the pac is walked (not from the cache).
struct UiRosterStats {
  uint32_t dirs = 0, entries = 0, bad_names = 0, dupes = 0, mismatches = 0;
  std::string note;
};
bool BuildPacIndex(const UiIndexOptions& options, uint8_t pac, std::vector<UiFileEntry>& out, std::string& err, bool* from_cache = nullptr, UiRosterStats* roster = nullptr);

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
  UiFileLoader(const std::string& game_root, const UiFileFaults& faults);
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

}  // namespace rdr_scene
