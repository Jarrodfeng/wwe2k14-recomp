// host_rig_2d.h: the 2D side of the scene host's headless rig (S103 landing 2, piece P7 H2; docs\renderer\P7_DESIGN.md section 5 H2). Rig-only. Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// The host has no resolver for a sprite's picture or a movie's planes without the game (P1 / P3 / P4 live pieces), so the rig supplies them from the CAPTURE the records came from: the capture
// frame N's op-19 draws say which texture each sprite object / movie draw bound (the same fact the stream's binder used), the content refs say its bytes, and the rig uploads them into the host's
// renderer (a stand-in for the live resolver; the host's own code is unchanged). Rows with no such draw get no picture and are dropped `picture`, counted.
#pragma once
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../../src/renderer/renderer.h"
#include "../../src/renderer_adapter/capture_reader.h"
#include "../../src/renderer_scene/rows_quad2d.h"

namespace host_rig_2d {

constexpr uint32_t kMovieSite = 0x82BC7E50u;

struct CapSource {
  rdr_adapter::CaptureReader reader;
  rdr::Renderer* renderer = nullptr;
  std::map<uint32_t, rdr_adapter::ContentRef> content;   // image -> its newest content as of the prepared frame
  uint32_t loaded = 0;                                   // frames 0..loaded-1 have contributed content
  std::map<uint32_t, rdr_adapter::TextureSlot> by_object;   // sprite / glyph object -> its slot-0 texture, this frame (the fallback when the per-site order does not line up)
  struct Facts { rdr_adapter::TextureSlot tex; rdr::Rect scissor{}; bool have = false; };
  std::map<uint32_t, Facts> of_seq;                         // row.seq -> the stream draw it came from, matched in draw order per call site (rows and draws are both in draw order): its picture and its scissor
  std::vector<std::vector<rdr_adapter::TextureSlot>> movies;   // the movie draws of this frame in draw order: slots 0..3
  std::map<uint32_t, uint32_t> movie_of_seq;             // row.seq -> index into movies (the n-th movie row of the record <-> the n-th movie draw)
  std::map<std::pair<uint32_t, uint64_t>, rdr::ImageId> uploaded;
  uint64_t uploads = 0, pic_hit = 0, pic_miss = 0, plane_hit = 0, plane_miss = 0;
  // stale-planes plant: the planes of the previous prepared frame's first movie
  bool stale_planes = false;
  std::vector<std::vector<rdr_adapter::TextureSlot>> prev_movies;
  std::map<uint32_t, rdr_adapter::ContentRef> prev_content;

  bool Open(const std::string& dir) { return reader.Open(dir); }

  // Frame n of the capture becomes the current one (content accumulated up to it).
  bool Prepare(uint32_t n, const rdr_scene::SceneTables& rec) {
    rdr_adapter::LoadedFrame f;
    prev_movies = movies;
    prev_content = content;
    for (; loaded <= n; ++loaded) {
      if (!reader.LoadFrame(loaded, f)) return false;
      for (uint32_t i = 0; i < f.contents.count; ++i) content[f.contents[i].image] = f.contents[i];
    }
    if (!reader.LoadFrame(n, f)) return false;
    by_object.clear(), movies.clear(), movie_of_seq.clear(), of_seq.clear();
    std::map<uint32_t, std::vector<Facts>> per_site;
    for (uint32_t d = 0; d < f.draws.count; ++d) {
      const rdr_adapter::DrawOrigin* o = f.OriginOf(d);
      if (!o || o->op != 19) continue;
      const rdr_adapter::Draw& dr = f.draws[d];
      std::vector<rdr_adapter::TextureSlot> slots(4);
      bool have0 = false;
      for (uint32_t t = 0; t < dr.texture_count && dr.texture_first + t < f.texture_slots.count; ++t) {
        const rdr_adapter::TextureSlot& ts = f.texture_slots[dr.texture_first + t];
        if (ts.slot < 4) slots[ts.slot] = ts;
        if (ts.slot == 0 && ts.image) have0 = true;
      }
      if (o->call_site == kMovieSite) movies.push_back(slots);
      else if (have0 && o->object && !by_object.count(o->object)) by_object[o->object] = slots[0];
      if (o->call_site != kMovieSite) {
        Facts fa;
        fa.tex = slots[0], fa.have = have0, fa.scissor = f.dynamics[dr.dynamic].host.scissor;
        per_site[o->call_site].push_back(fa);
      }
    }
    uint32_t m = 0;
    std::map<uint32_t, size_t> next;
    for (const rdr_scene::Quad2D& r : rec.quads2d) {
      if (r.kind == rdr_scene::kQkYuv || r.kind == rdr_scene::kQkYuvAlpha) { movie_of_seq[r.seq] = m++; continue; }
      const auto it = per_site.find(r.site);
      size_t& k = next[r.site];
      if (it != per_site.end() && k < it->second.size()) of_seq[r.seq] = it->second[k];
      ++k;
    }
    return true;
  }

  rdr::TextureSlot Upload(const rdr_adapter::TextureSlot& ts, const std::map<uint32_t, rdr_adapter::ContentRef>& from) {
    rdr::TextureSlot out{};
    const auto c = from.find(ts.image);
    const rdr_adapter::ImageRecord* rec = reader.FindImage(ts.image);
    if (c == from.end() || !rec) return out;
    const auto key = std::make_pair(ts.image, c->second.offset);
    auto it = uploaded.find(key);
    if (it == uploaded.end()) {
      rdr_adapter::LoadedTexture lt;
      if (!reader.ReadTexture(c->second, lt)) return out;
      rdr::ImageDesc d;
      d.width = rec->width, d.height = rec->height, d.depth = rec->depth, d.mips = rec->mips, d.layers = rec->layers;
      d.format = rdr::Format(rec->format), d.kind = rdr::ImageKind(rec->kind), d.usage = uint8_t(rec->usage | rdr::kUsageSampled);
      const rdr::ImageId id = renderer->CreateImage(d);
      for (uint32_t s = 0; s < lt.header.subresource_count; ++s) {
        const rdr_adapter::Subresource& sub = lt.subresources[s];
        renderer->UpdateImage(id, sub.mip, sub.layer, lt.data + sub.offset, size_t(sub.row_bytes) * sub.rows);
      }
      it = uploaded.emplace(key, id).first;
      ++uploads;
    }
    out.image = it->second;
    out.view_format = rdr::Format(ts.view_format);
    std::memcpy(out.swizzle, ts.swizzle, sizeof out.swizzle);
    return out;
  }

  rdr::TextureSlot Picture(const rdr_scene::Quad2D& row) {
    const auto fo = of_seq.find(row.seq);
    const auto it = by_object.find(row.object);
    const rdr_adapter::TextureSlot* src = fo != of_seq.end() && fo->second.have ? &fo->second.tex : (it != by_object.end() ? &it->second : nullptr);
    if (!src) return ++pic_miss, rdr::TextureSlot{};
    const rdr::TextureSlot t = Upload(*src, content);
    ++(t.image == rdr::kNoImage ? pic_miss : pic_hit);
    return t;
  }

  // The scissor the stream drew the row's draw with (the rows carry none: a Quad2D layout gap; this is the stand-in the future field will replace). false = whole target.
  bool Scissor(const rdr_scene::Quad2D& row, rdr::Rect& out) const {
    const auto fo = of_seq.find(row.seq);
    if (fo == of_seq.end()) return false;
    out = fo->second.scissor;
    return true;
  }

  rdr::TextureSlot Plane(const rdr_scene::Quad2D& row, int slot) {
    const auto at = movie_of_seq.find(row.seq);
    if (at == movie_of_seq.end() || slot < 0 || slot > 3) return ++plane_miss, rdr::TextureSlot{};
    const auto& set = stale_planes && at->second < prev_movies.size() ? prev_movies[at->second] : (at->second < movies.size() ? movies[at->second] : std::vector<rdr_adapter::TextureSlot>());
    if (set.size() < 4 || !set[size_t(slot)].image) return ++plane_miss, rdr::TextureSlot{};
    const rdr::TextureSlot t = Upload(set[size_t(slot)], stale_planes ? prev_content : content);
    ++(t.image == rdr::kNoImage ? plane_miss : plane_hit);
    return t;
  }
};

}  // namespace host_rig_2d
