// Session 96: loads wrestlers / arenas with src\assets\asset_load.h and reports what came out (the census).
//   asset_dump.exe --game DIR [--wrestler N [--attire A]] [--arena N] [--all] [--json OUT]
// Build: tools\assets\build.ps1. Reads game files only; writes only the optional --json (keep it under out\).
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "../../src/assets/asset_load.h"

using namespace assets;

namespace {

struct Item {
  std::string label;
  bool ok = false;
  std::string err;
  size_t meshes = 0, verts = 0, tris = 0, mats = 0, tex_loaded = 0, tex_missing = 0, bones = 0;
  float bmin[3] = {}, bmax[3] = {};
  size_t warns = 0;
  double ms = 0;
  size_t nan_verts = 0;
  std::vector<std::string> warnings;
};

// Warning text with digits and quoted names collapsed, so equal causes count together.
std::string Normalise(const std::string& w) {
  std::string o;
  bool inq = false;
  for (char c : w) {
    if (c == '\'') {
      inq = !inq;
      o += c;
      continue;
    }
    if (inq) continue;
    o += (c >= '0' && c <= '9') ? '#' : c;
  }
  return o;
}

Item Run(launcher::GameFiles& files, bool arena, int id, int attire) {
  Item it;
  Model m;
  std::string err;
  const bool ok = arena ? LoadArena(files, id, m, err) : LoadWrestler(files, id, attire, m, err);
  it.label = m.label.empty() ? (arena ? "arena " + std::to_string(id) : "wrestler " + std::to_string(id)) : m.label;
  it.ok = ok;
  it.err = err;
  it.ms = m.load_ms;
  it.meshes = m.meshes.size();
  for (const Mesh& me : m.meshes) {
    it.verts += me.vertices.size(), it.tris += me.indices.size() / 3;
    for (const Vertex& v : me.vertices)
      if (!std::isfinite(v.pos[0] + v.pos[1] + v.pos[2])) ++it.nan_verts;
  }
  it.mats = m.materials.size();
  it.tex_loaded = m.textures.size();
  it.bones = m.bones.size();
  for (int k = 0; k < 3; ++k) it.bmin[k] = m.bounds_min[k], it.bmax[k] = m.bounds_max[k];
  it.warnings = m.warnings;
  it.warns = m.warnings.size();
  for (const std::string& w : m.warnings)
    if (w.rfind("texture '", 0) == 0 && w.find("not found") != std::string::npos) ++it.tex_missing;
  if (ok && (it.verts == 0 || it.tris == 0)) it.ok = false, it.err = "no vertices or triangles";
  if (ok && it.nan_verts) it.ok = false, it.err = "NaN positions";
  return it;
}

void Line(const Item& i) {
  if (!i.ok && i.meshes == 0) {
    std::printf("%-22s FAIL %s\n", i.label.c_str(), i.err.c_str());
    return;
  }
  std::printf("%-22s %s meshes %3zu verts %7zu tris %7zu mats %3zu tex %3zu+%zu missing bones %3zu bounds (%.0f %.0f %.0f)-(%.0f %.0f %.0f) warns %3zu %7.1f ms%s%s\n",
              i.label.c_str(), i.ok ? "ok  " : "FAIL", i.meshes, i.verts, i.tris, i.mats, i.tex_loaded, i.tex_missing,
              i.bones, i.bmin[0], i.bmin[1], i.bmin[2], i.bmax[0], i.bmax[1], i.bmax[2], i.warns, i.ms,
              i.ok ? "" : "  ", i.ok ? "" : i.err.c_str());
}

std::string JsonEsc(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\', o += c;
    else if (c == '\n') o += "\\n";
    else if (static_cast<unsigned char>(c) < 32) o += ' ';
    else o += c;
  }
  return o;
}

}  // namespace

int main(int argc, char** argv) {
  {  // Session 98 (phone port): stage_media no longer reads %LOCALAPPDATA% itself - the launcher's tron cache folder, as UTF-8
#ifdef _WIN32
    if (const wchar_t* la = _wgetenv(L"LOCALAPPDATA")) {
      const auto u = (std::filesystem::path(la) / "wwe2k14-launcher" / "trons").u8string();
      assets::stage_media::Cfg().default_dir = std::string(u.begin(), u.end());
    }
#endif
  }
  bool materials = false, meshes_out = false, lightsets = false;
  int entry_key = -1;
  std::string scan;
  std::string entry_out;
  std::string tex_out;
  std::string game, json;
  int wrestler = -1, arena = -1, attire = -1;
  bool all = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
    if (a == "--game") game = next();
    else if (a == "--wrestler") wrestler = std::atoi(next().c_str());
    else if (a == "--attire") attire = std::atoi(next().c_str());
    else if (a == "--arena") arena = std::atoi(next().c_str());
    else if (a == "--all") all = true;
    else if (a == "--json") json = next();
    else if (a == "--materials") materials = true;
    else if (a == "--entry") entry_key = std::atoi(next().c_str());  // Session 97: with --arena N and --entry-out FILE: one decoded top-level pac entry to a file (analysis only, under out\)
    else if (a == "--scan") scan = next();
    else if (a == "--entry-out") entry_out = next();
    else if (a == "--lightsets") lightsets = true;  // Session 97: every light set (pac entry 30002) of --arena N as raw float rows
    else if (a == "--meshes") meshes_out = true;  // Session 97: one line per mesh of --arena N (slot, counts, material, light set): for matching a draw probe
    else if (a == "--tex-out") tex_out = next();  // Session 96: list each material's textures (one item)
    else {
      std::fprintf(stderr, "usage: asset_dump --game DIR [--wrestler N [--attire A]] [--arena N] [--all] [--json OUT]\n");
      return 2;
    }
  }
  if (game.empty()) {
    std::fprintf(stderr, "--game DIR is required\n");
    return 2;
  }
  launcher::GameFiles files;
  files.SetRoot(game);
  if (!scan.empty() && arena >= 0) {  // Session 97: --scan v1,v2: where do these float values sit in the arena's top-level entries (BE and LE, 0.1% tolerance)
    std::string err;
    const assets::pac::Pac* p = assets::load_detail::OpenCached(files, assets::load_detail::ArenaRel(arena), err);
    if (!p) return std::printf("open failed: %s\n", err.c_str()), 1;
    std::vector<double> vals;
    for (size_t pos = 0; pos < scan.size();) {
      size_t c = scan.find(',', pos);
      if (c == std::string::npos) c = scan.size();
      vals.push_back(std::atof(scan.substr(pos, c - pos).c_str()));
      pos = c + 1;
    }
    for (const assets::pac::Ent& e : p->pachs[0].ents) {
      const assets::Bytes d = assets::pac::Decode(p->bytes.data() + e.off, e.size);
      for (size_t o = 0; o + 4 <= d.size(); o += 4)
        for (double v : vals)
          for (int le = 0; le < 2; ++le) {
            uint32_t w = le ? uint32_t(d[o]) | uint32_t(d[o + 1]) << 8 | uint32_t(d[o + 2]) << 16 | uint32_t(d[o + 3]) << 24 : uint32_t(d[o]) << 24 | uint32_t(d[o + 1]) << 16 | uint32_t(d[o + 2]) << 8 | d[o + 3];
            float f;
            std::memcpy(&f, &w, 4);
            if (std::isfinite(f) && std::fabs(f - v) <= std::fabs(v) * 1e-3) std::printf("entry %u +0x%zx %s %g (want %g)\n", unsigned(e.id), o, le ? "LE" : "BE", f, v);
          }
    }
    return 0;
  }
  if (entry_key >= 0 && arena >= 0 && !entry_out.empty()) {
    std::string err;
    const assets::pac::Pac* p = assets::load_detail::OpenCached(files, assets::load_detail::ArenaRel(arena), err);
    if (!p) return std::printf("open failed: %s\n", err.c_str()), 1;
    const assets::pac::Ent* e = assets::pac::Find(p->pachs[0], uint32_t(entry_key));
    if (!e) return std::printf("no entry %d\n", entry_key), 1;
    const assets::Bytes d = assets::pac::Decode(p->bytes.data() + e->off, e->size);
    std::FILE* f = std::fopen(entry_out.c_str(), "wb");
    if (!f) return std::printf("cannot write\n"), 1;
    std::fwrite(d.data(), 1, d.size(), f);
    std::fclose(f);
    return std::printf("entry %d: %zu bytes\n", entry_key, d.size()), 0;
  }
  if (lightsets && arena >= 0) {
    std::string err;
    const assets::pac::Pac* p = assets::load_detail::OpenCached(files, assets::load_detail::ArenaRel(arena), err);
    if (!p) return std::printf("open failed: %s\n", err.c_str()), 1;
    const assets::pac::Pach& h = p->pachs[0];
    assets::Bytes nb;
    assets::pac::Pach np;
    if (!assets::pac::GetNested(p->bytes, h, 30002, nb, np)) return std::printf("no 30002\n"), 1;
    for (const assets::pac::Ent& e : np.ents) {
      const assets::Bytes d = assets::pac::Decode(nb.data() + e.off, e.size);
      std::printf("set %u size %zu\n", unsigned(e.id), d.size());
      for (size_t o = 0; o + 16 <= d.size(); o += 16) {
        std::printf("  %03zx", o);
        for (int k = 0; k < 4; ++k) std::printf(" %.6g", assets::BeF(d.data() + o + 4 * k));
        std::printf("\n");
      }
    }
    return 0;
  }
  if (meshes_out && arena >= 0) {
    assets::Model m;
    std::string err;
    if (!assets::LoadArena(files, arena, m, err)) return std::printf("load failed: %s\n", err.c_str()), 1;
    for (size_t k = 0; k < m.meshes.size(); ++k) {
      const assets::Mesh& me = m.meshes[k];
      const assets::Material* mt = me.material >= 0 && size_t(me.material) < m.materials.size() ? &m.materials[size_t(me.material)] : nullptr;
      const int ls = me.light_set;
      double ny = 0, py = 0, nx = 0, nz = 0, px = 0, pz = 0;
      for (const assets::Vertex& v : me.vertices) nx += v.normal[0], ny += v.normal[1], nz += v.normal[2], px += v.pos[0], py += v.pos[1], pz += v.pos[2];
      const double nvv = double(std::max<size_t>(1, me.vertices.size()));
      std::printf("%zu\t%s\t%zu\t%zu\t%s\tset=%d\tlm=%d\tamb=%g,%g,%g,%g\tdif=%g,%g,%g,%g\tspec=%g\tplace=%d\tblend=%d%d\tn=%.2f,%.2f,%.2f\tp=%.0f,%.0f,%.0f\n", k, me.part.c_str(), me.vertices.size(), me.indices.size(),
                  mt ? mt->shader.c_str() : "-", ls >= 0 && size_t(ls) < m.light_sets.size() ? m.light_sets[size_t(ls)].id : -1,
                  mt ? int(mt->has_lm_slot) : -1, mt ? mt->ambient[0] : 0, mt ? mt->ambient[1] : 0, mt ? mt->ambient[2] : 0, mt ? mt->ambient[3] : 0,
                  mt ? mt->diffuse_color[0] : 0, mt ? mt->diffuse_color[1] : 0, mt ? mt->diffuse_color[2] : 0, mt ? mt->diffuse_color[3] : 0,
                  mt ? mt->spec_lev : 0, me.placement, mt ? int(mt->alpha_blend) : 0, mt ? int(mt->additive) : 0, nx / nvv, ny / nvv, nz / nvv, px / nvv, py / nvv, pz / nvv);
    }
    return 0;
  }
  if (materials) {  // Session 96 integration: what each material samples (names only; no game data printed beyond names)
    assets::Model m;
    std::string err;
    const bool ok = arena >= 0 ? assets::LoadArena(files, arena, m, err) : assets::LoadWrestler(files, wrestler, attire < 0 ? 0 : attire, m, err);
    if (!ok) return std::printf("load failed: %s\n", err.c_str()), 1;
    auto nm = [&](int i) { return i >= 0 && i < int(m.textures.size()) ? m.textures[size_t(i)].name + "(" + std::to_string(int(m.textures[size_t(i)].format)) + ")" : std::string("-"); };
    if (!tex_out.empty()) {  // --tex-out DIR: each texture as a .dds (local inspection only; under out\, never committed)
      for (const assets::Texture& tx : m.textures) {
        if (tx.format != assets::TexFormat::kBC1 && tx.format != assets::TexFormat::kBC3) continue;
        uint32_t h[32] = {};
        h[0] = 124; h[1] = 0x1007 | 0x20000; h[2] = tx.height; h[3] = tx.width; h[6] = tx.mips;
        h[18] = 32; h[19] = 4; std::memcpy(&h[20], tx.format == assets::TexFormat::kBC1 ? "DXT1" : "DXT5", 4); h[26] = 0x1000;
        std::FILE* f = std::fopen((tex_out + "\\" + tx.name + ".dds").c_str(), "wb");
        if (!f) continue;
        std::fwrite("DDS ", 1, 4, f); std::fwrite(h, 4, 31, f); std::fwrite(tx.data.data(), 1, tx.data.size(), f); std::fclose(f);
      }
    }
    for (size_t k = 0; k < m.materials.size(); ++k) {
      const assets::Material& mt = m.materials[k];
      std::printf("%3zu %-18s diffuse %-22s normal %-22s mask %-18s lightmap %s\n", k, mt.shader.c_str(), nm(mt.diffuse).c_str(), nm(mt.normal).c_str(), nm(mt.mask).c_str(), nm(mt.lightmap).c_str());
    }
    return 0;
  }
  std::vector<Item> items;
  const auto t0 = std::chrono::steady_clock::now();
  auto add = [&](bool ar, int id, int at) {
    items.push_back(Run(files, ar, id, at));
    Line(items.back());
    std::fflush(stdout);
  };
  if (all) {
    for (int id : ListWrestlerIds(files)) {
      const int n = AttireCount(files, id);
      if (n == 0) {
        Item it;
        it.label = "wrestler " + std::to_string(id);
        it.err = "no attire with a model (or the pac could not be read)";
        items.push_back(it);
        Line(it);
        continue;
      }
      for (int a = 0; a < n; ++a) add(false, id, a);
    }
    for (int id : ListArenaIds(files)) add(true, id, 0);
  } else {
    if (wrestler >= 0) {
      if (attire >= 0) add(false, wrestler, attire);
      else
        for (int a = 0, n = AttireCount(files, wrestler); a < n; ++a) add(false, wrestler, a);
    }
    if (arena >= 0) add(true, arena, 0);
  }
  const double total_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

  size_t nw = 0, na = 0, fails = 0, verts = 0, tris = 0, tex = 0, missing = 0, warns = 0;
  std::map<std::string, size_t> wc;
  for (const Item& i : items) {
    (i.label.rfind("arena", 0) == 0 ? na : nw)++;
    if (!i.ok) ++fails;
    verts += i.verts, tris += i.tris, tex += i.tex_loaded, missing += i.tex_missing, warns += i.warns;
    for (const std::string& w : i.warnings) ++wc[Normalise(w)];
  }
  std::printf("\nTOTALS: %zu items (%zu wrestler attires, %zu arenas), %zu failed, %zu vertices, %zu triangles, %zu textures loaded, "
              "%zu texture names missing, %zu warnings, %.1f s total\n",
              items.size(), nw, na, fails, verts, tris, tex, missing, warns, total_ms / 1000.0);
  if (fails) {
    std::printf("FAILURES:\n");
    for (const Item& i : items)
      if (!i.ok) std::printf("  %s: %s\n", i.label.c_str(), i.err.c_str());
  }
  std::vector<std::pair<size_t, std::string>> top;
  for (auto& [k, v] : wc) top.push_back({v, k});
  std::sort(top.rbegin(), top.rend());
  std::printf("MOST COMMON WARNINGS (digits as #, quoted names dropped):\n");
  for (size_t k = 0; k < top.size() && k < 10; ++k) std::printf("  %6zu x %s\n", top[k].first, top[k].second.c_str());

  if (!json.empty()) {
    std::ofstream f(json);
    f << "[\n";
    for (size_t k = 0; k < items.size(); ++k) {
      const Item& i = items[k];
      f << " {\"label\":\"" << JsonEsc(i.label) << "\",\"ok\":" << (i.ok ? "true" : "false") << ",\"error\":\""
        << JsonEsc(i.err) << "\",\"meshes\":" << i.meshes << ",\"vertices\":" << i.verts << ",\"triangles\":" << i.tris
        << ",\"materials\":" << i.mats << ",\"textures_loaded\":" << i.tex_loaded << ",\"textures_missing\":"
        << i.tex_missing << ",\"bones\":" << i.bones << ",\"bounds_min\":[" << i.bmin[0] << "," << i.bmin[1] << ","
        << i.bmin[2] << "],\"bounds_max\":[" << i.bmax[0] << "," << i.bmax[1] << "," << i.bmax[2]
        << "],\"warning_count\":" << i.warns << ",\"load_ms\":" << i.ms << ",\"warnings\":[";
      std::map<std::string, size_t> own;
      for (const std::string& w : i.warnings) ++own[w];
      size_t n = 0;
      for (auto& [w, c] : own) f << (n++ ? "," : "") << "\"" << JsonEsc(w) << (c > 1 ? " x" + std::to_string(c) : "") << "\"";
      f << "]}" << (k + 1 < items.size() ? "," : "") << "\n";
    }
    f << "]\n";
  }
  return fails ? 1 : 0;
}
