// Quads2DPictures (Session 103, lane I2, landing 2; docs\renderer\REMNANT_MAP.md section 10): a sprite row's texture KEY -> OUR picture. Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// A Quad2D sprite row names its picture by the game's texture handle key (word 0 of the handle at sprite + 400). This file is the join from that key to something we can draw, with no stream draw to
// ask. Pure (no renderer, no game or SDK includes): the guest memory is a template parameter and the registry walk is lane F4's ONE walk through lane D6's reader (src\scene_live\guest_texture.h; never a
// second copy of it). Three sources, tried in this order:
//   File    the player's own pac: UiOurs' content join (key -> index entry), given to the table by the caller (JoinFile). Menu / editor pictures.
//   Guest   the texture OBJECT read from guest memory: handle -> registry -> wrapper -> header -> fetch dwords -> mip 0 of a plain 2D block-compressed (BC1 / 2 / 3) picture. For the pictures that are in no
//           indexed pac (the match HUD's keys, UI_OURS.md "Facts"). A render-target / uncompressed / cube / tiny-packed texture is REFUSED by guest_tex (format / size / kind), which is also the guard against
//           reading a surface the GPU draws into (a stale picture is the one wrong answer this must never give).
//   (none)  a counted refusal by reason. Never a guess.
// The Guest cache is keyed on (key, wrapper, fetch[1], fetch[2]): a key whose texture object changed (the refill that keeps the key, REMNANT_MAP U7) is read again, counted as `changed`.
// Render thread only (a map without a lock).
#pragma once
#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

#include "../scene_live/guest_texture.h"
#include "quads2d_plants.h"

namespace rdr_scene::q2d {

enum class PicSource : uint8_t { None, File, Guest };
enum class PicWhy : uint8_t { Ok, NoKey, Unknown, GuestRefused, Count };
inline const char* PicWhyName(PicWhy w) {
  static const char* const k[] = {"ok", "no key", "unknown key", "guest texture refused"};
  return uint8_t(w) < uint8_t(PicWhy::Count) ? k[uint8_t(w)] : "?";
}

struct FileRef {
  uint32_t entry = 0;          // the caller's index entry (UiOurs: the UiFileIndex entry of the content join)
  uint32_t width = 0, height = 0;
};

struct Picture {
  PicSource source = PicSource::None;
  uint32_t key = 0;
  FileRef file;                              // source File
  rdr_scene::live::guest_tex::Shape shape;   // source Guest: the decoded fetch dwords
  uint32_t fetch1 = 0, fetch2 = 0, wrapper = 0;
  const std::vector<uint8_t>* blocks = nullptr;   // source Guest: mip 0, linear little-endian blocks (valid until the table is next changed)
  uint64_t hash = 0;                         // source Guest: FNV-1a of the blocks (the caller's upload cache key)
};

struct PictureCounters {
  uint64_t asked = 0, file = 0, guest = 0, guest_read = 0, changed = 0, refused[uint8_t(PicWhy::Count)] = {};
  uint64_t guest_why[16] = {};               // refusals of the guest read by guest_tex::Why
};

inline uint64_t HashBlocks(const std::vector<uint8_t>& b) {
  uint64_t h = 1469598103934665603ull;
  for (uint8_t c : b) h = (h ^ c) * 1099511628211ull;
  return h;
}

class PictureTable {
 public:
  // The file join for a key (UiOurs' answer). A later call for the same key replaces it.
  void JoinFile(uint32_t key, const FileRef& f) { files_[key] = f; }
  void ForgetFile(uint32_t key) { files_.erase(key); }
  size_t files() const { return files_.size(); }

  // `Mem`: u32(addr) host value of the big-endian word, mutable `fail` (guest_texture.h's contract); `Phys`: bool(addr, n, dst) reads pixel memory at a PHYSICAL address.
  template <class Mem, class Phys>
  PicWhy Resolve(uint32_t key, const Mem& mem, Phys&& phys, Picture& out) {
    out = Picture{};
    ++n_.asked;
    if (!key) return ++n_.refused[uint8_t(PicWhy::NoKey)], PicWhy::NoKey;
    uint32_t k = key;
    if (Q2D_PLANT(12)) {   // plant pic_key_shift: the table answers the NEXT key's picture
      auto it = files_.upper_bound(key);
      if (it != files_.end()) k = it->first;
    }
    if (auto it = files_.find(k); it != files_.end()) {
      out.source = PicSource::File, out.key = key, out.file = it->second;
      return ++n_.file, PicWhy::Ok;
    }
    namespace gt = rdr_scene::live::guest_tex;
    gt::Resolved r;
    gt::Shape shape;
    if (const gt::Why w = gt::ResolveHandle(mem, key, r); w != gt::Why::Ok) return Refuse(w);
    if (const gt::Why w = gt::ShapeOfFetch(r.fetch, shape); w != gt::Why::Ok) return Refuse(w);
    auto it = guest_.find(key);
    bool rehash_changed = false;
    if (it != guest_.end() && it->second.wrapper == r.wrapper && it->second.fetch1 == r.fetch[1] && it->second.fetch2 == r.fetch[2]) {
      // F2: a refill in place (same address and size) keeps every fetch dword: mip 0 is read and hashed again on every hit; a different hash is a changed picture
      std::vector<uint8_t> now;
      if (gt::ReadMip0(it->second.shape, phys, now) == gt::Why::Ok && HashBlocks(now) == it->second.hash) {
        FillGuest(out, key, it->second);
        return ++n_.guest, PicWhy::Ok;
      }
      rehash_changed = true;
    }
    if (rehash_changed) ++n_.changed;
    GuestEntry e;
    e.wrapper = r.wrapper, e.fetch1 = r.fetch[1], e.fetch2 = r.fetch[2], e.shape = shape;
    if (const gt::Why w = gt::ReadMip0(shape, phys, e.blocks); w != gt::Why::Ok) return Refuse(w);
    e.hash = HashBlocks(e.blocks);
    ++n_.guest_read;
    if (it != guest_.end() && !rehash_changed) ++n_.changed;
    it = guest_.insert_or_assign(key, std::move(e)).first;
    FillGuest(out, key, it->second);
    return ++n_.guest, PicWhy::Ok;
  }
  // A key nothing could resolve is counted here by the caller that has no memory at all (the rig): the same reason as a failed walk.
  PicWhy RefuseUnknown() { return ++n_.asked, ++n_.refused[uint8_t(PicWhy::Unknown)], PicWhy::Unknown; }
  const PictureCounters& counters() const { return n_; }
  void Clear() { files_.clear(), guest_.clear(), n_ = PictureCounters{}; }

 private:
  struct GuestEntry {
    uint32_t wrapper = 0, fetch1 = 0, fetch2 = 0;
    rdr_scene::live::guest_tex::Shape shape;
    std::vector<uint8_t> blocks;
    uint64_t hash = 0;
  };
  void FillGuest(Picture& out, uint32_t key, const GuestEntry& e) {
    out.source = PicSource::Guest, out.key = key, out.shape = e.shape, out.blocks = &e.blocks, out.hash = e.hash;
    out.fetch1 = e.fetch1, out.fetch2 = e.fetch2, out.wrapper = e.wrapper;
  }
  PicWhy Refuse(rdr_scene::live::guest_tex::Why w) {
    ++n_.refused[uint8_t(PicWhy::GuestRefused)];
    ++n_.guest_why[uint8_t(w) < 16 ? uint8_t(w) : 15];
    return PicWhy::GuestRefused;
  }
  std::map<uint32_t, FileRef> files_;
  std::map<uint32_t, GuestEntry> guest_;
  PictureCounters n_;
};

}  // namespace rdr_scene::q2d
