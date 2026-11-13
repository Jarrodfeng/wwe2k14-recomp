// Session 101 lane 20 (the judge): fview's headless dump for tools\judge\judge.py. One process = one arm of one frame: the presented picture after the
// frame's last op (picture.png) and, for the pixels a list names, the chain of draws behind each one (pixels.json).
//
// The history of a pixel is computed here, in batches (review fixes S101 lane 20, items 1 and 2): the ops that reach a (surface, pixel) come from the frame's
// tables; ONE forward pass over the frame then reads every wanted texel after every op that touches any of them, all texels of an op in one submit
// (Renderer::ReadTexelsRaw). A walk level = one pass for all pixels and surfaces it needs; the old per-pixel PixelHistory replayed frames 0..F-1 per query.
#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "../renderer/player/png.h"
#include "../renderer_adapter/binder.h"
#include "app.h"

namespace fview {

namespace {

bool RectHas(const rdr::Rect& r, int x, int y) {
  return x >= r.x && y >= r.y && int64_t(x) < int64_t(r.x) + r.width && int64_t(y) < int64_t(r.y) + r.height;
}

struct Entry {
  uint32_t op = 0;
  char kind = 'd';  // d draw, c copy, x clear
  uint32_t index = 0;
  bool drawn = true;
  uint32_t slots = 0;  // the colour slots of the targets in force that hold the surface
  bool known = false;  // the value before and after could be compared
  bool wrote = false;
  uint8_t after[8] = {};
  uint32_t bytes = 0;
};

struct Hist {
  uint32_t surface = 0;
  int x = 0, y = 0;
  std::vector<Entry> e;  // oldest first, the whole frame
};

// The ops that reach the pixel, from the tables alone (the same rules as Session::PixelHistory part 1; slots kept for the blend test).
void StaticEntries(const Session& session, Hist& h) {
  const rdr_adapter::LoadedFrame& frame = session.frame();
  uint32_t slots = 0;
  for (uint32_t i = 0; i < frame.ops.count; ++i) {
    const rdr_adapter::Op& op = frame.ops[i];
    switch (op.kind) {
      case rdr_adapter::OpKind::SetTargets: {
        slots = 0;
        const rdr::TargetsOp& t = frame.targets[op.index];
        for (uint32_t c = 0; c < t.color_count; ++c)
          if (t.color[c].image == h.surface && t.color[c].mip == 0 && t.color[c].layer == 0) slots |= 1u << c;
        break;
      }
      case rdr_adapter::OpKind::Clear: {
        const rdr::ClearOp& c = frame.clears[op.index];
        if (!(c.flags & rdr::kClearColor) || !(slots & c.color_mask)) break;
        if (!c.full && !RectHas(c.rect, h.x, h.y)) break;
        Entry e;
        e.op = i, e.kind = 'x', e.index = op.index;
        h.e.push_back(e);
        break;
      }
      case rdr_adapter::OpKind::Copy: {
        const rdr::CopyOp& c = frame.copies[op.index];
        if (c.dest.image != h.surface || c.dest.mip != 0 || c.dest.layer != 0 || !RectHas(c.dest_rect, h.x, h.y)) break;
        Entry e;
        e.op = i, e.kind = 'c', e.index = op.index;
        h.e.push_back(e);
        break;
      }
      case rdr_adapter::OpKind::Draw: {
        if (!slots) break;
        const rdr_adapter::Draw& d = frame.draws[op.index];
        const rdr_adapter::DrawDynamic& dyn = frame.dynamics[d.dynamic];
        if (dyn.flags & rdr_adapter::kDynEmptyViewport) break;
        if (!RectHas(dyn.host.scissor, h.x, h.y)) break;
        if (!(dyn.flags & rdr_adapter::kDynClipOff)) {
          const float* v = dyn.host.viewport;
          if (float(h.x) + 0.5f < v[0] || float(h.y) + 0.5f < v[1] || float(h.x) + 0.5f >= v[0] + v[2] || float(h.y) + 0.5f >= v[1] + v[3]) break;
        }
        Entry e;
        e.op = i, e.kind = 'd', e.index = op.index, e.slots = slots;
        e.drawn = OpDrawn(session.trace()[i]);  // S101 lane 58
        h.e.push_back(e);
        break;
      }
      default: break;
    }
  }
}

// One forward pass for every history in `todo`: values after each drawn op, all texels of one op in one submit.
bool FillValues(Session& session, std::vector<Hist*>& todo, std::string& error) {
  if (todo.empty()) return true;
  if (!session.RenderThrough(-1)) {
    error = "RenderThrough(-1) failed";
    return false;
  }
  std::vector<Session::TexelQuery> q(todo.size());
  for (size_t i = 0; i < todo.size(); ++i) q[i].surface = todo[i]->surface, q[i].x = todo[i]->x, q[i].y = todo[i]->y;
  if (!session.ReadTexels(q)) {
    error = "texel read failed";
    return false;
  }
  std::vector<uint8_t> prev_raw(todo.size() * 8);
  std::vector<uint32_t> prev_bytes(todo.size());
  std::vector<char> prev_known(todo.size());
  std::vector<char> first_draw_seen(todo.size(), 0), covered_first(todo.size(), 0);
  for (size_t i = 0; i < todo.size(); ++i) {
    std::memcpy(&prev_raw[i * 8], q[i].raw, 8);
    prev_bytes[i] = q[i].raw_bytes;
    prev_known[i] = q[i].ok;
    // a covering clear / copy before the first draw makes the start value the game's own
    for (const Entry& e : todo[i]->e) {
      if (e.kind != 'd') {
        covered_first[i] = 1;
        break;
      }
      if (e.drawn) break;
    }
  }
  std::map<uint32_t, std::vector<std::pair<size_t, size_t>>> by_op;  // op -> (history, entry)
  for (size_t i = 0; i < todo.size(); ++i)
    for (size_t k = 0; k < todo[i]->e.size(); ++k)
      if (todo[i]->e[k].drawn) by_op[todo[i]->e[k].op].push_back({i, k});
  for (const auto& kv : by_op) {
    if (!session.RenderThrough(int(kv.first))) {
      error = "RenderThrough(" + std::to_string(kv.first) + ") failed";
      return false;
    }
    std::vector<Session::TexelQuery> now(kv.second.size());
    for (size_t n = 0; n < kv.second.size(); ++n) {
      const Hist& h = *todo[kv.second[n].first];
      now[n].surface = h.surface, now[n].x = h.x, now[n].y = h.y;
    }
    if (!session.ReadTexels(now)) {
      error = "texel read failed";
      return false;
    }
    for (size_t n = 0; n < kv.second.size(); ++n) {
      const size_t i = kv.second[n].first;
      Entry& e = todo[i]->e[kv.second[n].second];
      const bool stale_start = e.kind == 'd' && !first_draw_seen[i] && !covered_first[i];
      if (e.kind == 'd') first_draw_seen[i] = 1;
      e.known = prev_known[i] && now[n].ok && !stale_start;
      std::memcpy(e.after, now[n].raw, 8);
      e.bytes = now[n].raw_bytes;
      if (e.known) e.wrote = prev_bytes[i] != now[n].raw_bytes || std::memcmp(&prev_raw[i * 8], now[n].raw, 8) != 0;
      std::memcpy(&prev_raw[i * 8], now[n].raw, 8);
      prev_bytes[i] = now[n].raw_bytes;
      prev_known[i] = now[n].ok;
    }
  }
  return true;
}

std::string Hex(const uint8_t* raw, uint32_t n) {
  static const char* digits = "0123456789abcdef";
  std::string s;
  for (uint32_t i = 0; i < n; ++i) s += digits[raw[i] >> 4], s += digits[raw[i] & 15];
  return s;
}

// Blend-off with all of rgb written: the draw replaced the pixel (the slot is the one the surface is bound in).
bool OpaqueDraw(const Session& session, const Entry& e) {
  const rdr_adapter::Draw& d = session.frame().draws[e.index];
  uint32_t slot = 0;
  while (slot < 4 && !(e.slots & (1u << slot))) ++slot;
  if (slot >= 4) return false;
  const rdr::BlendState& b = session.frame().fixeds[d.fixed].state.blend[slot];
  return !b.enable && (b.write_mask & 7) == 7;
}

struct Request {
  uint32_t surface;
  int through;
  int depth;
};

}  // namespace

int JudgeDump(Session& session, const EffectIndex* effects, const std::string& out_dir, const std::string& pixels_file, std::string& error) {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::create_directories(out_dir, ec);
  // Both arms take the same path: the frame is always bound again and the surfaces put back (an arm with a reload does that anyway).
  if (!session.Rebind(error)) return 2;
  const uint32_t surface = session.DefaultSurface();
  const int last = int(session.frame().ops.count) - 1;
  if (!session.RenderThrough(last)) {
    error = "RenderThrough(last) failed";
    return 2;
  }
  std::vector<uint8_t> rgba;
  uint32_t w = 0, h = 0;
  if (!session.ReadSurface(surface, rgba, w, h) || !rdr::WritePng((fs::path(out_dir) / "picture.png").string().c_str(), rgba.data(), w, h)) {
    error = "could not read / write surface " + std::to_string(surface);
    return 2;
  }
  std::vector<std::pair<int, int>> pixels;
  if (!pixels_file.empty()) {
    std::ifstream in(pixels_file);
    if (!in) {
      error = "cannot read pixels file " + pixels_file;
      return 2;
    }
    // S101 lane 58 (leftovers census): a first line "grid W H" = the centres of a W x H grid over the surface (the size is not known to the caller).
    std::string first;
    const std::streampos start = in.tellg();
    if (in >> first && first == "grid") {
      int gw = 0, gh = 0;
      if (!(in >> gw >> gh) || gw <= 0 || gh <= 0) {
        error = "bad grid line in " + pixels_file;
        return 2;
      }
      for (int gy = 0; gy < gh; ++gy)
        for (int gx = 0; gx < gw; ++gx) pixels.push_back({int((gx + 0.5) * double(w) / gw), int((gy + 0.5) * double(h) / gh)});
    } else {
      in.clear();
      in.seekg(start);
      int x, y;
      while (in >> x >> y) pixels.push_back({x, y});
    }
  }
  const rdr_adapter::LoadedFrame& frame = session.frame();
  std::map<std::tuple<uint32_t, int, int>, Hist> cache;
  std::vector<std::vector<std::string>> nodes(pixels.size());
  std::set<std::tuple<size_t, uint32_t, int>> seen;  // (pixel, surface, through)
  std::vector<std::vector<Request>> level(pixels.size());
  for (size_t p = 0; p < pixels.size(); ++p) level[p].push_back({surface, -1, 0});

  const auto effect_of_op = [&](uint32_t op) {
    const OpTrace& t = session.trace()[op];
    const std::string vs = t.vs >= 0 ? session.VariantName(t.vs) : "", ps = t.ps >= 0 ? session.VariantName(t.ps) : "";
    return EffectOf(effects, ps.empty() ? vs : ps);
  };

  for (int depth = 0; depth <= 8; ++depth) {
    // The histories this level needs that are not computed yet: one forward pass.
    std::vector<Hist*> todo;
    for (size_t p = 0; p < pixels.size(); ++p)
      for (const Request& r : level[p]) {
        const auto key = std::make_tuple(r.surface, pixels[p].first, pixels[p].second);
        if (cache.count(key)) continue;
        Hist& hh = cache[key];
        hh.surface = r.surface, hh.x = pixels[p].first, hh.y = pixels[p].second;
        StaticEntries(session, hh);
        todo.push_back(&hh);
      }
    if (!FillValues(session, todo, error)) return 2;
    std::vector<std::vector<Request>> next(pixels.size());
    bool any = false;
    for (size_t p = 0; p < pixels.size(); ++p) {
      for (const Request& r : level[p]) {
        if (!seen.insert({p, r.surface, r.through}).second) continue;
        const Hist& hh = cache[std::make_tuple(r.surface, pixels[p].first, pixels[p].second)];
        const auto* rd = session.capture().FindImage(r.surface);
        // The compact value history (the writers only), for the arms' comparison: [op, effect, raw after].
        {
          JsonWriter j;
          j.Begin('{');
          j.Key("depth"), j.Int(r.depth);
          j.Key("surface"), j.Int(r.surface);
          j.Key("through"), j.Int(r.through);
          j.Key("kind"), j.Str("hist");
          j.Key("writes");
          j.Begin('[');
          for (const Entry& e : hh.e) {
            if (r.through >= 0 && int(e.op) > r.through) break;
            if (!e.drawn || !e.known || !e.wrote) continue;
            j.Begin('[');
            j.Int(e.op);
            j.Str(e.kind == 'd' ? effect_of_op(e.op) : std::string(1, e.kind));
            j.Str(Hex(e.after, e.bytes));
            j.End();
          }
          j.End();
          j.End();
          nodes[p].push_back(j.text());
        }
        // The draws that covered the pixel without changing it (depth / alpha / blend rejected them): they shaped the picture too.
        {
          std::map<std::string, int> covered;
          for (const Entry& e : hh.e) {
            if (r.through >= 0 && int(e.op) > r.through) break;
            if (e.kind == 'd' && e.drawn && e.known && !e.wrote) ++covered[effect_of_op(e.op)];
          }
          if (!covered.empty()) {
            JsonWriter j;
            j.Begin('{');
            j.Key("depth"), j.Int(r.depth);
            j.Key("surface"), j.Int(r.surface);
            j.Key("kind"), j.Str("covered");
            j.Key("effects");
            j.Begin('{');
            for (const auto& kv : covered) j.Key(kv.first.c_str()), j.Int(kv.second);
            j.End();
            j.End();
            nodes[p].push_back(j.text());
          }
        }
        // Writers, newest first, down to the first one that replaced the pixel (blend off with rgb written, a copy, a clear); the first 12 blended ones are listed,
        // the rest are counted and the walk goes on to the replacing writer.
        int emitted = 0, skipped_blended = 0;
        for (size_t k = hh.e.size(); k-- > 0;) {
          const Entry& e = hh.e[k];
          if (r.through >= 0 && int(e.op) > r.through) continue;
          bool opaque = true, writer = false;
          if (e.kind != 'd') {
            writer = true;  // a clear / copy replaces the pixel (stop here even when the value did not change)
          } else if (e.drawn) {
            opaque = OpaqueDraw(session, e);
            writer = e.wrote || (!e.known && opaque);  // an unreadable value is not "did not write": a blend-off draw counts, value_known false
          }
          if (!writer) continue;
          if (emitted >= 12 && !opaque) {
            ++skipped_blended;
            continue;
          }
          JsonWriter j;
          j.Begin('{');
          j.Key("depth"), j.Int(r.depth);
          j.Key("surface"), j.Int(r.surface);
          j.Key("op"), j.Int(e.op);
          j.Key("kind"), j.Str(e.kind == 'd' ? "draw" : e.kind == 'c' ? "copy" : "clear");
          j.Key("value_known"), j.Bool(e.known);
          j.Key("wrote"), j.Bool(e.wrote);
          if (e.known) j.Key("after"), j.Str(Hex(e.after, e.bytes));
          j.Key("blended"), j.Bool(!opaque);
          j.Key("skipped_blended"), j.Int(skipped_blended);
          if (e.kind == 'c') {
            const rdr::CopyOp& c = frame.copies[e.index];
            j.Key("source"), j.Int(c.source.image);
            const auto* rs = session.capture().FindImage(c.source.image);
            if (rs && rd && rs->width == rd->width && rs->height == rd->height) next[p].push_back({c.source.image, int(e.op), depth + 1}), any = true;
          } else if (e.kind == 'd') {
            DrawInfo info;
            std::string e2;
            if (DescribeDraw(session, effects, e.op, info, e2)) {
              j.Key("effect"), j.Str(info.effect);
              j.Key("vs_variant"), j.Str(info.vs_variant);
              j.Key("ps_variant"), j.Str(info.ps_variant);
              j.Key("origin"), j.Str(info.origin);
              j.Key("alpha_line"), j.Str(info.alpha_line);
              // A same-size surface it sampled is followed at the same pixel only for a full-screen pass (a quad covering the surface); a draw that samples at mesh
              // UVs (a model, a sprite) maps the pixel somewhere else: it is named, not followed ("uv_sampled").
              const rdr_adapter::Draw& d = frame.draws[e.index];
              const float* v = frame.dynamics[d.dynamic].host.viewport;
              const uint32_t prims = d.index_bytes ? d.index_count : d.vertex_count;
              // S101 lane CJ: a full viewport and <= 6 indices is NOT a full-screen pass (a far crowd card, a HUD sprite, a particle quad all match it; the old rule
              // skipped them, so their pixels were credited to the draw underneath: LEFTOVERS.md "cards own 0 %"). A full-screen pass is a quad that reads the
              // scene back: a post-class origin, or a draw sampling a texture of the surface's own size. Never by index count alone.
              const bool quad_viewport = rd && prims <= 6 && v[2] >= 0.99f * rd->width && v[3] >= 0.99f * rd->height;
              bool samples_same_size = false;
              for (const TextureInfo& t : info.textures)
                if (t.has_record && rd && t.record.width == rd->width && t.record.height == rd->height) samples_same_size = true;
              const rdr_adapter::DrawOrigin* org = frame.OriginOf(e.index);
              const bool post_origin = org && org->klass == rdr_adapter::DrawClass::Post;
              const bool full_screen = quad_viewport && (post_origin || samples_same_size);
              bool uv_sampled = false;
              for (const TextureInfo& t : info.textures)
                if (t.has_record && rd && t.record.width == rd->width && t.record.height == rd->height) {
                  if (full_screen) next[p].push_back({t.image, int(e.op), depth + 1}), any = true;
                  else uv_sampled = true;
                }
              j.Key("full_screen"), j.Bool(full_screen);
              j.Key("quad_viewport"), j.Bool(quad_viewport);  // S101 lane CJ: the OLD rule's input (<= 6 indices + full viewport), kept so the census's planted twin can re-apply it
              j.Key("uv_sampled"), j.Bool(uv_sampled);
            }
          }
          j.End();
          nodes[p].push_back(j.text());
          ++emitted;
          if (opaque) break;
        }
      }
    }
    if (!any) break;
    level = std::move(next);
  }

  std::ostringstream out;
  out << "{\"surface\":" << surface << ",\"size\":[" << w << "," << h << "],\"ops\":" << (last + 1) << ",\"pixels\":[";
  for (size_t p = 0; p < pixels.size(); ++p) {
    out << (p ? "," : "") << "{\"x\":" << pixels[p].first << ",\"y\":" << pixels[p].second << ",\"chain\":[";
    for (size_t n = 0; n < nodes[p].size(); ++n) out << (n ? "," : "") << nodes[p][n];
    out << "]}";
  }
  out << "]}\n";
  std::ofstream f((fs::path(out_dir) / "pixels.json").string(), std::ios::binary);
  f << out.str();
  return f ? 0 : 2;
}

}  // namespace fview
