// The particle TextureNamer (Session 103, lane F4; docs\formats\particle_items.md "The texture"). Pure: no game or SDK includes; the game build supplies GuestMem and a ContentHasher.
//
// What a slot-33 table entry is [C: run 22 log wwe2k14_080.log + the listing of sub_82294F20 + docs\native\d3d\E_inline_callbacks.md section 3]: a handle {id, cached pointer} of the game's
// TEXTURE registry (sub_822951F0 creates them; table global 0x8392DD9C; entries 8 bytes {id, wrapper pointer} from [mgr+8] to [mgr+16]; index = id & 0xFFFF; the stored id must equal the handle's).
// The log's one texture handle is 0x091903BD (index 957), in all 16 sampled items. The entry's pointer is the 96-byte texture WRAPPER (vtable 0x8206ADB8): +52 kind (0 2D, 1 cube, 2 volume), the
// library texture header pointer at +80 / +84 / +88 for kind 0 / 1 / 2 (sub_82BD4600, the SetTexture the op-43 record calls); the header's six fetch dwords sit at +28..+48 [C: H_textures.md 1.1].
//
// Naming is by CONTENT, never by the cached pointer (its writer is unknown): handle id -> wrapper -> header -> fetch dwords -> the game side's ContentHasher (untile + endian-swap the mip 0, hash it)
// -> a NameIndex built from the player's own prtcls.pac sprite directories (AddSpriteDirectory) -> {pac, entry}. Every step is counted. A handle that fails any step stays unnamed (the white
// stand-in, kPiTexture): never a guess. A named handle is cached by (id, wrapper pointer, base-address dword) so a reused registry slot is read again.
// S108 PRT: an index MISS is no longer the end: the index holds only the disc's prtcls.pac, and a DLC pack's (prtcls_dlcPP.pac) or a mod's particle pictures can never be in it. The game's own bound texture
// (the same fetch dwords, its whole mip chain) is read then, kept in GuestSprites by content and named {kGuestSpritePac, key}. Unnamed now means the game's texture itself could not be read (guest_failed).
// [U] The ContentHasher's game-side implementation (renderer_adapter DecodeTexture + the hash below) and the NameIndex build at boot are NOT written or run in this lane (0 game runs).
#pragma once
#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "particle_guest_sprites.h"   // S108 PRT: the pictures read from the game's own texture objects
#include "particle_reader.h"
#include "texture_registry.h"

namespace rdr_scene::live::particles {


// Test only: 1 = the namer trusts the entry's cached pointer as the wrapper instead of resolving the handle id (the fault the contract forbids); 2 = the PF miss diagnostic ignores its 8-line cap.
// S108 PRT: 3 = the old rule, the file index only (an index miss stays unnamed: a DLC / mod particle texture is never drawn).
inline int g_namer_plant = 0;

// FNV-1a 64 over (width, height, fourcc, then the mip-0 block bytes). The DDS side and the guest side must use this one function.
inline uint64_t ContentKey(uint32_t width, uint32_t height, uint32_t fourcc, const uint8_t* bytes, size_t n) {
  uint64_t h = 1469598103934665603ull;
  const auto mix = [&](const uint8_t* p, size_t k) {
    for (size_t i = 0; i < k; ++i) h = (h ^ p[i]) * 1099511628211ull;
  };
  const uint32_t head[3] = {width, height, fourcc};
  mix(reinterpret_cast<const uint8_t*>(head), sizeof head);
  mix(bytes, n);
  return h;
}

// A DDS file's mip 0 (header 128 bytes, DXT1 / DXT3 / DXT5 only, as the sprite textures are): its key, or false.
inline bool DdsMipZeroKey(const uint8_t* p, size_t n, uint64_t& key, uint32_t* ow = nullptr, uint32_t* oh = nullptr, uint32_t* ofourcc = nullptr) {
  if (n < 128 || std::memcmp(p, "DDS ", 4) != 0) return false;
  uint32_t h = 0, w = 0, fourcc = 0;
  std::memcpy(&h, p + 12, 4), std::memcpy(&w, p + 16, 4), std::memcpy(&fourcc, p + 84, 4);
  const size_t blk = fourcc == 0x31545844u ? 8 : (fourcc == 0x33545844u || fourcc == 0x35545844u) ? 16 : 0;   // DXT1 / DXT3 / DXT5
  if (!blk || !w || !h || w > 8192 || h > 8192) return false;
  const size_t bytes = size_t((w + 3) / 4) * ((h + 3) / 4) * blk;
  if (n - 128 < bytes) return false;
  key = ContentKey(w, h, fourcc, p + 128, bytes);
  if (ow) *ow = w;
  if (oh) *oh = h;
  if (ofourcc) *ofourcc = fourcc;
  return true;
}

// content key -> the sprite texture's file. The first entry added for a key wins (equal bytes are the same picture, e.g. Bo03 / Bo_01).
class NameIndex {
 public:
  void Add(uint64_t key, const std::string& pac, const std::string& entry, uint32_t w = 0, uint32_t h = 0, uint32_t fourcc = 0) {
    if (map_.emplace(key, Name{pac, entry}).second) ++added_, meta_[key] = Meta{w, h, fourcc};
    else ++duplicates_;
  }
  // S103 PF (dev diagnostic only): the `n` index keys nearest to `key`: entries with the same width / height / fourcc first, then by numeric distance of the 64-bit key. A line each:
  // "key=HEX shape=WxH/FOURCC entry".
  std::vector<std::string> Nearest(uint64_t key, uint32_t w, uint32_t h, uint32_t fourcc, size_t n) const {
    struct C { bool same; uint64_t d; uint64_t k; };
    std::vector<C> c;
    c.reserve(map_.size());
    for (const auto& kv : map_) {
      const auto m = meta_.find(kv.first);
      const bool same = m != meta_.end() && m->second.w == w && m->second.h == h && m->second.fourcc == fourcc;
      c.push_back({same, kv.first > key ? kv.first - key : key - kv.first, kv.first});
    }
    const size_t k = std::min(n, c.size());
    std::partial_sort(c.begin(), c.begin() + k, c.end(), [](const C& a, const C& b) { return a.same != b.same ? a.same : a.d < b.d; });
    std::vector<std::string> out;
    for (size_t i = 0; i < k; ++i) {
      const auto& nm = map_.at(c[i].k);
      const Meta& m = meta_.at(c[i].k);
      char b[64];
      std::snprintf(b, sizeof b, "key=%016llX shape=%ux%u/%08X ", (unsigned long long)c[i].k, m.w, m.h, m.fourcc);
      out.push_back(std::string(b) + nm.entry);
    }
    return out;
  }
  // A texture directory (prtcls.pac sprite directory, research_models.md 3: LE32 count at +0, 32-byte records at +0x10 {name[16], "dds\0", LE32 size, LE32 offset, LE32 0}); returns the entries added.
  // `leaf` = the directory's own path in the pac (e.g. "p0107/2/1"): the entry stored is leaf + "/" + name, because the NAME alone is ambiguous (prtcls.pac has 23 names with more than one distinct picture:
  // Smoke0001 x15, Light0005 x7, Light0001 x4 ...).
  uint32_t AddSpriteDirectory(const uint8_t* dir, size_t n, const std::string& pac, const std::string& leaf) {
    if (n < 0x30) return 0;
    uint32_t count = 0;
    std::memcpy(&count, dir, 4);
    if (!count || count > 4096 || 0x10 + size_t(count) * 32 > n) return 0;
    uint32_t added = 0;
    for (uint32_t i = 0; i < count; ++i) {
      const uint8_t* r = dir + 0x10 + size_t(i) * 32;
      if (std::memcmp(r + 16, "dds", 3) != 0) continue;
      uint32_t size = 0, off = 0;
      std::memcpy(&size, r + 20, 4), std::memcpy(&off, r + 24, 4);
      if (off > n || size > n - off) continue;
      uint64_t key = 0;
      uint32_t dw = 0, dh = 0, dfc = 0;
      if (!DdsMipZeroKey(dir + off, size, key, &dw, &dh, &dfc)) continue;
      char nm[17] = {};
      std::memcpy(nm, r, 16);
      const size_t before = added_;
      Add(key, pac, leaf + "/" + nm, dw, dh, dfc);
      added += added_ > before;
    }
    return added;
  }
  struct Name { std::string pac, entry; };
  const Name* Find(uint64_t key) const {
    const auto it = map_.find(key);
    return it == map_.end() ? nullptr : &it->second;
  }
  template <class F> void ForEach(F&& f) const {   // S103 F5: every distinct {key, name}
    for (const auto& kv : map_) f(kv.first, kv.second);
  }
  size_t size() const { return map_.size(); }
  size_t duplicates() const { return duplicates_; }
 private:
  struct Meta { uint32_t w = 0, h = 0, fourcc = 0; };
  std::unordered_map<uint64_t, Name> map_;
  std::unordered_map<uint64_t, Meta> meta_;
  size_t added_ = 0, duplicates_ = 0;
};

// The game side: the content key of the texture whose six fetch dwords are `fetch` (untile + endian-swap the mip 0 from guest memory, then ContentKey with the format's fourcc). False when the
// texture is not a plain 2D block-compressed one or its memory is unreadable.
struct ContentHasher {
  virtual ~ContentHasher() = default;
  virtual bool MipZeroKey(const uint32_t fetch[6], uint64_t& key) const = 0;
  // S103 PF (dev diagnostic): the shape and the first bytes the key was made of. Default: nothing to say.
  struct KeyInfo { uint32_t width = 0, height = 0, fourcc = 0, mip0_bytes = 0; uint8_t head[16] = {}; };
  virtual bool DescribeKey(const uint32_t /*fetch*/[6], KeyInfo& /*out*/) const { return false; }
  // S108 PRT: the texture itself, the way the game bound it: the whole mip chain (guest_texture_mips.h ReadChain) as a decoded BC picture. False when it is not a plain 2D BC texture or a read is refused.
  virtual bool ReadSprite(const uint32_t /*fetch*/[6], assets::Texture& /*out*/) const { return false; }
};

struct NamerStats {
  uint64_t asked = 0, named = 0, cache_hits = 0;
  uint64_t no_handle = 0, wrapper_unreadable = 0, not_2d = 0, no_header = 0, no_key = 0, not_in_index = 0;   // why a handle stayed unnamed (one counter each); S108: not_in_index counts index misses, the guest source then names most
  uint64_t diag_lines = 0;   // S103 PF: miss diagnostics produced (never more than kMaxDiagLines)
  uint64_t guest = 0, guest_failed = 0;   // S108 PRT: index misses named from the game's own texture (fresh reads, not cache hits) / whose texture could not be read (a fault: the batch is skipped)
  uint64_t guest_check_equal = 0, guest_check_differ = 0;   // S108 PRT (dev): fresh index hits whose guest chain equals / differs from the file sprite's levels (the live proof of the guest mip reader)
  uint64_t guest_check_short = 0;   // S108 PRT review (dev): the guest held FEWER levels than the file and those it held are equal: mip 0 (and any held level) proven, the rest of the chain NOT exercised
};
inline constexpr size_t kMaxDiagLines = 8;

class HandleTextureNamer final : public TextureNamer {
 public:
  HandleTextureNamer(const GuestMem& mem, const ContentHasher& hasher, const NameIndex& index) : mem_(mem), hasher_(hasher), index_(index) {}
  bool Name(uint32_t handle_id, uint32_t cached_ptr /* informational: used only by the planted fault */, std::string& pac, std::string& entry) const override {
    ++stats_.asked;
    TextureObject t;
    if (g_namer_plant == 1 && !cached_ptr) return ++stats_.no_handle, false;   // the fault: trust the cached pointer (0 here) instead of the registry
    switch (WalkTextureHandle(mem_, handle_id, t)) {   // the shared walk (texture_registry.h): resolved fresh from the id, never from the cache word
      case TexWalk::Ok: break;
      case TexWalk::NoHandle: return ++stats_.no_handle, false;
      case TexWalk::NotWrapper2D: return ++stats_.not_2d, false;
      case TexWalk::NoHeader: return ++stats_.no_header, false;
      case TexWalk::Unreadable: return ++stats_.wrapper_unreadable, false;
    }
    const auto hit = cache_.find(handle_id);
    if (hit != cache_.end() && hit->second.wrapper == t.wrapper && hit->second.base == t.fetch[1] && (!hit->second.guest || (guest_ && guest_->Has(hit->second.key)))) {   // only POSITIVE results are cached; a miss is asked again (cheap); a guest picture the store let go is read again
      ++stats_.cache_hits;
      pac = hit->second.pac, entry = hit->second.entry;
      return ++stats_.named, true;
    }
    uint64_t key = 0;
    if (!hasher_.MipZeroKey(t.fetch, key)) return ++stats_.no_key, false;
    const NameIndex::Name* n = index_.Find(key);
    Cached c;
    c.wrapper = t.wrapper, c.base = t.fetch[1], c.key = key;
    if (!n) {
      ++stats_.not_in_index;
      if (diag_on_ && (diag_.size() < kMaxDiagLines || g_namer_plant == 2)) DiagMiss(handle_id, t, key);   // S103 PF: dev builds only (the glue turns it on under IsDevBuild())
      // S108 PRT: the index holds only the disc's prtcls.pac; a DLC pack's or a mod's particle file is not in it. The game's own texture object is the source then: read it as it is bound now (the latch:
      // game threads parked), check its mip 0 is the picture just hashed (a texture rewritten between the two reads is refused, never mixed), keep it by CONTENT, name it by that key.
      if (!guest_ || g_namer_plant == 3) return false;
      auto pic = std::make_shared<assets::Texture>();
      if (!hasher_.ReadSprite(t.fetch, *pic) || !SameKey(*pic, key)) return ++stats_.guest_failed, false;   // the game's texture cannot be read: a fault (live_particles.cpp skips the batch, the shortfall hook)
      guest_->Put(key, pic);
      ++stats_.guest;
      c.guest = true, c.pac = kGuestSpritePac, c.entry = GuestSpriteEntry(key);
    } else {
      c.pac = n->pac, c.entry = n->entry;
      if (file_sprite_) CheckAgainstFile(t, c.entry);   // S108 PRT (dev): the guest reader against the file, once per newly named handle
    }
    cache_[handle_id] = c;
    pac = c.pac, entry = c.entry;
    return ++stats_.named, true;
  }
  // S108 PRT: the store guest pictures go to (null = the old index-only rule). Not owned.
  void SetGuestStore(GuestSprites* s) { guest_ = s; }
  // S108 PRT (dev builds): the file sprite of an index entry (the published map), for the guest-chain self-check. Null = no check.
  void SetFileSprite(std::function<std::shared_ptr<const assets::Texture>(const std::string& entry)> f) { file_sprite_ = std::move(f); }
  const NamerStats& stats() const { return stats_; }
  // S103 PF: the dev-only miss diagnostic. Off by default; the glue switches it on only when IsDevBuild(). TakeDiag() hands over the lines made since the last call (at most 8 in all).
  void EnableDiag(bool on) { diag_on_ = on; }
  std::vector<std::string> TakeDiag() const {
    std::vector<std::string> out;
    out.swap(diag_pending_);
    return out;
  }
 private:
  static uint32_t FourccOf(assets::TexFormat f) { return f == assets::TexFormat::kBC1 ? 0x31545844u : f == assets::TexFormat::kBC2 ? 0x33545844u : 0x35545844u; }
  static size_t Mip0Bytes(const assets::Texture& p) { return size_t((p.width + 3) / 4) * ((p.height + 3) / 4) * (p.format == assets::TexFormat::kBC1 ? 8 : 16); }
  static bool SameKey(const assets::Texture& p, uint64_t key) {
    const size_t n = Mip0Bytes(p);
    return p.data.size() >= n && ContentKey(p.width, p.height, FourccOf(p.format), p.data.data(), n) == key;
  }
  // The levels both hold, byte for byte (each level tightly packed, mip 0 first, in both).
  void CheckAgainstFile(const TextureObject& t, const std::string& entry) const {
    const std::shared_ptr<const assets::Texture> file = file_sprite_(entry);
    if (!file) return;
    assets::Texture g;
    if (!hasher_.ReadSprite(t.fetch, g)) return void(++stats_.guest_check_differ);
    size_t n = 0;
    const uint32_t levels = std::min(g.mips, file->mips);
    for (uint32_t i = 0; i < levels; ++i) {
      const uint32_t w = std::max(1u, g.width >> i), h = std::max(1u, g.height >> i);
      n += size_t((w + 3) / 4) * ((h + 3) / 4) * (g.format == assets::TexFormat::kBC1 ? 8 : 16);
    }
    const bool same = g.width == file->width && g.height == file->height && g.format == file->format && g.data.size() >= n && file->data.size() >= n && std::memcmp(g.data.data(), file->data.data(), n) == 0;
    // review: `equal` only when the guest held the file's whole chain; fewer levels that match = `short` (the mip reader past those levels was not exercised: never a silent pass)
    ++(!same ? stats_.guest_check_differ : g.mips < file->mips ? stats_.guest_check_short : stats_.guest_check_equal);
  }
  void DiagMiss(uint32_t id, const TextureObject& t, uint64_t key) const {
    ContentHasher::KeyInfo ki;
    const bool have = hasher_.DescribeKey(t.fetch, ki);
    const uint32_t fw = (t.fetch[2] & 0x1FFFu) + 1u, fh = ((t.fetch[2] >> 13) & 0x1FFFu) + 1u;
    char b[512];
    int n = std::snprintf(b, sizeof b, "[scene][particles] namer miss %zu: handle %08X, key %016llX, fetch size %ux%u format %u base %08X", diag_.size() + 1, id, (unsigned long long)key, fw, fh, t.fetch[1] & 0x3Fu, t.fetch[1] & 0xFFFFF000u);
    if (have) {
      n += std::snprintf(b + n, sizeof b - n, ", hashed %ux%u fourcc %08X mip0 %u bytes, head", ki.width, ki.height, ki.fourcc, ki.mip0_bytes);
      for (int i = 0; i < 16 && n < int(sizeof b) - 4; ++i) n += std::snprintf(b + n, sizeof b - n, " %02X", ki.head[i]);
    }
    std::string line = b;
    for (const std::string& s : index_.Nearest(key, have ? ki.width : fw, have ? ki.height : fh, have ? ki.fourcc : 0, 3)) line += "\n  nearest: " + s;
    diag_.push_back(line);
    diag_pending_.push_back(line);
    ++stats_.diag_lines;
  }
  bool diag_on_ = false;
  mutable std::vector<std::string> diag_, diag_pending_;
  struct Cached { uint32_t wrapper = 0, base = 0; uint64_t key = 0; bool guest = false; std::string pac, entry; };
  const GuestMem& mem_;
  const ContentHasher& hasher_;
  const NameIndex& index_;
  GuestSprites* guest_ = nullptr;
  std::function<std::shared_ptr<const assets::Texture>(const std::string&)> file_sprite_;
  mutable NamerStats stats_;
  mutable std::unordered_map<uint32_t, Cached> cache_;
};

}  // namespace rdr_scene::live::particles
