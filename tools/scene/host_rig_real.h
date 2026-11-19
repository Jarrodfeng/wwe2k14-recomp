// host_rig_real.h: the REAL picture resolver and the REAL movie planes in the scene host's headless rig (S103 landing 2, piece PN4; docs\renderer\PICTURES_NO_STREAM.md 8-9). Rig only.
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// host_rig_2d.h's CapSource is a STAND-IN: it answers a row's picture straight from the capture's images. This header makes the host run what the game runs:
//   pictures  the player's file index (BuildPacIndex over the game folder), a PictureSource fed the way the game's taps feed it (OnBorn at the first sight of a key, Seen at every row) over FORGED GUEST MEMORY
//             (picture_forge.h: the registry, the wrapper, the header, the fetch dwords, the pixel bytes tiled and endian-swapped; the pictures' bytes are the capture's own texture blobs of the row's draw), and a
//             PictureResolver over them: HostOptions::picture = resolver->Function(), HostOptions::frame_done = resolver->EndFrame (SceneHost calls it after each record's rows).
//   planes    the movie draws' four planes written into a FORGED DISPLAY SET (movie_planes_unit.cpp's layout: the decoder's crossed plane order, pitch padding) and flushed into movieplanes::Store() with
//             OnFlush, as the game's plane flush hook does; the host's PlaneOf then serves them through ServeGuest (HostOptions has no plane seam set).
// What the rig cannot feed (told in the report): a movie row's set hint (the fview rows dump carries no flush stamp: the draw takes the one set flushed near it, and two movies in one frame read "ambiguous").
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "host_rig_2d.h"
#include "picture_forge.h"
#include "../../src/renderer_scene/picture_resolver.h"
#include "../../src/renderer_scene/ui_file_index.h"
#include "../../src/scene_live/movie_planes.h"

namespace host_rig_real {

namespace ps = rdr_scene::live::picsrc;
namespace mp = rdr_scene::live::movieplanes;
namespace sfx = rdr_scene::live::stagefx;

// ---- a forged display set (movie_planes_unit.cpp: the layout of screen_planes_core.h) ------------------------------------------------------------------------------
struct Guest {
  std::map<uint32_t, uint32_t> words;
  struct Region {
    uint32_t base;
    std::vector<uint8_t> bytes;
  };
  std::vector<Region> regions;
  bool Read(uint32_t a, void* dst, size_t n) const {
    auto* p = static_cast<uint8_t*>(dst);
    for (const Region& r : regions)
      if (a >= r.base && uint64_t(a) + n <= uint64_t(r.base) + r.bytes.size()) return std::memcpy(p, r.bytes.data() + (a - r.base), n), true;
    if ((a & 3) || (n & 3)) return false;
    for (size_t i = 0; i < n; i += 4) {
      const auto it = words.find(a + uint32_t(i));
      if (it == words.end()) return false;
      const uint32_t be = sfx::Swap32(it->second);
      std::memcpy(p + i, &be, 4);
    }
    return true;
  }
  Region& Alloc(uint32_t base, size_t n) {
    for (Region& r : regions)
      if (r.base == base) return r.bytes.assign(n, 0), r;
    regions.push_back(Region{base, std::vector<uint8_t>(n, 0)});
    return regions.back();
  }
  Region* Find(uint32_t base) {
    for (Region& r : regions)
      if (r.base == base) return &r;
    return nullptr;
  }
};
inline Guest& TheGuest() {
  static Guest g;
  return g;
}
inline bool ReadThunk(uint32_t a, void* dst, size_t n) { return TheGuest().Read(a, dst, n); }

struct SetSpec {
  uint32_t set, w, h, bufs, pitch_pad, cw, ch;
  bool alpha;
};
inline uint32_t PlanePtr(const SetSpec& s, uint32_t buf, uint32_t k) { return 0x50000000u + ((s.set >> 12) & 0x7) * 0x4000000u + buf * 0x1000000u + k * 0x200000u; }
inline uint32_t DW(const SetSpec& s, uint32_t k) { return (k == 0 || k == 3) ? s.w : s.cw; }
inline uint32_t DH(const SetSpec& s, uint32_t k) { return (k == 0 || k == 3) ? s.h : s.ch; }
inline uint32_t Pitch(const SetSpec& s, uint32_t k) { return DW(s, k) + s.pitch_pad; }
inline void BuildSet(const SetSpec& s) {
  Guest& g = TheGuest();
  g.words[s.set + 128] = s.bufs;
  g.words[s.set + 132] = s.w, g.words[s.set + 136] = s.h, g.words[s.set + 140] = s.cw, g.words[s.set + 144] = s.ch;
  g.words[s.set + 148] = 0;
  for (uint32_t b = 0; b < s.bufs; ++b)
    for (uint32_t k = 0; k < 4; ++k) {
      const uint32_t base = s.set + 152 + 12 * k + 48 * b;
      const bool present = k < 3 || s.alpha;
      g.words[base] = present ? 1 : 0, g.words[base + 4] = present ? PlanePtr(s, b, k) : 0, g.words[base + 8] = present ? Pitch(s, k) : 0;
      if (present) g.Alloc(PlanePtr(s, b, k), size_t(Pitch(s, k)) * DH(s, k));
    }
}
// What the decoder writes: data plane k (0 Y, 1 cR, 2 cB, 3 A), the pitch padding a marker that must never be copied, then the display index.
inline void Decode(const SetSpec& s, uint32_t buf, const std::vector<uint8_t> src[4]) {
  Guest& g = TheGuest();
  g.words[s.set + 148] = buf;
  for (uint32_t k = 0; k < (s.alpha ? 4u : 3u); ++k) {
    Guest::Region* r = g.Find(PlanePtr(s, buf, k));
    for (uint32_t y = 0; y < DH(s, k); ++y) {
      std::memcpy(r->bytes.data() + size_t(y) * Pitch(s, k), src[k].data() + size_t(y) * DW(s, k), DW(s, k));
      std::memset(r->bytes.data() + size_t(y) * Pitch(s, k) + DW(s, k), 0xEE, Pitch(s, k) - DW(s, k));
    }
  }
}

struct Real {
  rdr_scene::UiFileIndex index;
  ps::PictureSource source{&index};
  std::unique_ptr<rdr_scene::PictureResolver> resolver;
  picforge::Forge forge;
  struct Placed {
    uint32_t image = 0;
    uint64_t offset = ~0ull;
  };
  std::map<uint32_t, Placed> placed;           // key -> the capture content its forged texture holds
  uint64_t rows = 0, fed = 0, skipped_no_texture = 0, skipped_format = 0, births = 0, refills = 0;
  uint64_t movie_rows = 0, movie_flushes = 0, movie_skipped = 0;
  struct SetState {
    bool built = false;
    uint32_t w = 0, h = 0, buf = 0;
  };
  std::map<uint32_t, SetState> sets;           // forged display set address -> state
  std::string plant;                           // "" | "wrong_entry" | "resolver_null"

  std::vector<std::pair<uint8_t, std::vector<rdr_scene::UiFileEntry>>> staged;   // BuildIndex(stage_only): the pacs built but not yet added (the resolver unit's rejoin twin adds them later)
  bool BuildIndex(const std::string& game, const std::string& cache, bool stage_only = false) {
    rdr_scene::UiIndexOptions o;
    o.game_root = game, o.cache_dir = cache;
    for (uint8_t pac = 0; pac <= uint8_t(rdr_scene::kUiBgEtc); ++pac) {   // tier 1 + bgEtc, as the host's worker does (host_start.cpp StartPictures)
      std::vector<rdr_scene::UiFileEntry> entries;
      std::string err;
      if (!rdr_scene::BuildPacIndex(o, pac, entries, err)) return std::fprintf(stderr, "host_rig: index %s\n", err.c_str()), false;
      if (stage_only) staged.emplace_back(pac, std::move(entries));
      else index.AddPac(pac, std::move(entries));
    }
    if (stage_only) return !staged.empty();
    std::printf("      real resolver: index: %s\n", index.Summary().c_str());
    return index.texture_count() > 0;
  }
  void AddStaged() {   // the staged pacs join the index (its generation moves: the source rejoins its guest copies at their next Seen)
    for (auto& [pac, entries] : staged) index.AddPac(pac, std::move(entries));
    staged.clear();
  }

  // The game's side at the op-19 of every textured sprite / glyph row of this frame: the first sight of the key (its birth, the forged texture), then Seen on the drawing thread.
  // The frame stamps are the RECORD's guest frame (the number the host is posted with and EndFrame trims at), not the capture's frame index.
  void FeedPictures(host_rig_2d::CapSource& cap, const rdr_scene::SceneTables& rec, uint64_t /*capture frame*/) {
    const uint64_t n = rec.header.guest_frame;
    const auto phys = [this](uint32_t a, uint32_t len, uint8_t* dst) { return forge.phys(a, len, dst); };
    for (const rdr_scene::Quad2D& r : rec.quads2d) {
      if (r.kind == rdr_scene::kQkYuv || r.kind == rdr_scene::kQkYuvAlpha) continue;
      if (!(r.have & rdr_scene::kQhPlanes) || !r.tex_key[0]) continue;
      ++rows;
      const uint32_t key = r.tex_key[0];
      const auto fo = cap.of_seq.find(r.seq);
      const auto bo = cap.by_object.find(r.object);
      const rdr_adapter::TextureSlot* ts = fo != cap.of_seq.end() && fo->second.have ? &fo->second.tex : (bo != cap.by_object.end() ? &bo->second : nullptr);
      if (!ts || !ts->image) {
        ++skipped_no_texture;
        continue;
      }
      const auto c = cap.content.find(ts->image);
      const rdr_adapter::ImageRecord* ir = cap.reader.FindImage(ts->image);
      if (c == cap.content.end() || !ir) {
        ++skipped_no_texture;
        continue;
      }
      const rdr::Format fmt = rdr::Format(ir->format);
      if (!(fmt == rdr::Format::BC1 || fmt == rdr::Format::BC2 || fmt == rdr::Format::BC3 || fmt == rdr::Format::RGBA8 || fmt == rdr::Format::BGRA8)) {
        ++skipped_format;
        continue;
      }
      Placed& pl = placed[key];
      const bool known = pl.offset != ~0ull;
      if (!known || pl.image != ts->image || pl.offset != c->second.offset) {   // a new life of the key (or its first): the texture is made / refilled WITH its birth announced, as the game's creator does
        rdr_adapter::LoadedTexture blob;
        if (!cap.reader.ReadTexture(c->second, blob) || blob.header.subresource_count < 1) {
          ++skipped_no_texture;
          continue;
        }
        const rdr_adapter::Subresource& sr = blob.subresources[0];
        uint32_t fswz = 0;   // the game's fetch selectors of the row's slot (picture_source_unit.cpp: slot swizzle 3..6 = X..W -> memory byte 0..3, 1 zero, 2 one, 0 identity)
        for (int i = 0; i < 4; ++i) {
          const uint32_t s = ts->swizzle[i], sel = s == 0 ? uint32_t(i) : (s >= 3 && s <= 6) ? s - 3 : s == 1 ? 4u : 5u;
          fswz |= sel << (3 * i);
        }
        if (known) source.OnDied(key, n), ++refills;
        forge.Place(key, fmt, ir->width, ir->height, blob.data + sr.offset, (key & 1) == 0, 1 + (key >> 4) % 3, fswz);   // tiled on even keys, endian 1 / 2 / 3 by key
        source.OnBorn(key, 0x82BAA908u), ++births;
        pl.image = ts->image, pl.offset = c->second.offset;
      }
      source.Seen(key, n, forge, phys);
      ++fed;
    }
  }

  // The movie present's side: every movie draw's four planes into its forged display set, flushed into the store at guest frame n.
  void FeedMovies(host_rig_2d::CapSource& cap, const rdr_scene::SceneTables& rec, uint64_t /*capture frame*/) {
    const uint64_t n = rec.header.guest_frame;
    for (const rdr_scene::Quad2D& r : rec.quads2d) {
      if (r.kind != rdr_scene::kQkYuv && r.kind != rdr_scene::kQkYuvAlpha) continue;
      ++movie_rows;
      const auto at = cap.movie_of_seq.find(r.seq);
      if (at == cap.movie_of_seq.end() || at->second >= cap.movies.size()) {
        ++movie_skipped;
        continue;
      }
      const auto& slots = cap.movies[at->second];
      rdr_adapter::LoadedTexture tex[4];
      std::vector<uint8_t> plane[4];
      uint32_t pw[4] = {}, ph[4] = {};
      bool ok = slots.size() >= 4;
      for (int s = 0; s < 4 && ok; ++s) {
        const auto c = cap.content.find(slots[size_t(s)].image);
        if (!slots[size_t(s)].image || c == cap.content.end() || !cap.reader.ReadTexture(c->second, tex[s]) || tex[s].header.subresource_count < 1) { ok = false; break; }
        const rdr_adapter::Subresource& sr = tex[s].subresources[0];
        if (sr.row_bytes != sr.width || sr.rows != sr.height) { ok = false; break; }
        pw[s] = sr.width, ph[s] = sr.height;
        plane[s].assign(tex[s].data + sr.offset, tex[s].data + sr.offset + size_t(sr.width) * sr.height);
      }
      if (!ok || pw[1] != pw[2] || ph[1] != ph[2] || pw[0] != pw[3] || ph[0] != ph[3] || pw[1] < (pw[0] + 1) / 2 || ph[1] < (ph[0] + 1) / 2) {
        ++movie_skipped;
        continue;
      }
      const uint32_t set_addr = 0x41000000u + uint32_t(at->second) * 0x1000u;
      SetSpec spec{set_addr, pw[0], ph[0], 2, 64, pw[1], ph[1], true};
      SetState& st = sets[set_addr];
      if (!st.built || st.w != pw[0] || st.h != ph[0]) BuildSet(spec), st.built = true, st.w = pw[0], st.h = ph[0], st.buf = 0;
      st.buf ^= 1;
      const bool uncrossed = plant == "swap_chroma";   // twin: the decoder's crossed order forgotten (data plane 1 = draw slot 1): the chroma planes swap
      const std::vector<uint8_t> data[4] = {plane[0], plane[uncrossed ? 1 : 2], plane[uncrossed ? 2 : 1], plane[3]};   // the game's crossed order: data plane 1 = Cr = draw slot 2, data plane 2 = Cb = draw slot 1
      Decode(spec, st.buf, data);
      if (plant == "no_flush") continue;   // twin: the plane flush never reaches the store: the host has no planes for the movie row
      mp::Store().OnFlush(&ReadThunk, set_addr, n);
      ++movie_flushes;
    }
  }
};

}  // namespace host_rig_real
