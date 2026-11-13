// Session 101 lane 4: the drawables atlas. Walks the pacs of the player's own game files (read-only) with OUR loaders (src\assets\pac.h
// containers, jboy.h model block + its S101 declaration hook, dds.h) and lists every drawable the files declare: meshes (effect,
// parameters, streams, index block, texture references resolved against the sibling texture directories), textures (format, size,
// mips, kind), fonts and menu textures, and whatever it could not parse (with the reason). Nothing here is shipped or committed:
// the outputs hold game-derived names and live under out\atlas\ (CLAUDE.md rule 1). Format: docs\formats\drawables_atlas.md.
//
//   atlas.exe --game DIR --all   [--out OUTDIR]     every pac under DIR\pac (367 on the disc)
//   atlas.exe --game DIR --pac REL [--out OUTDIR]   one pac, REL = "pac/ch/ch100.pac"
//   atlas.exe --game DIR --check --expect FILE [--fault]   the self test: three pacs, counts against FILE (--fault: expect +1, must FAIL)
// Build: tools\atlas\build.ps1.
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "../../src/assets/jboy.h"

using namespace assets;
namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------------------------------------------------------- json out
std::string Esc(const std::string& s) {
  std::string o;
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') o += '\\', o += char(c);
    else if (c < 32 || c >= 127) {
      char b[8];
      if (c < 32) std::snprintf(b, sizeof b, "\\u%04x", unsigned(c));
      else std::snprintf(b, sizeof b, "?");
      o += b;
    } else o += char(c);
  }
  return o;
}
std::string Q(const std::string& s) { return "\"" + Esc(s) + "\""; }
std::string Num(double v) {
  char b[40];
  std::snprintf(b, sizeof b, "%.9g", v);
  return b;
}

// ---------------------------------------------------------------------------------------------------------------- DDS header
struct DdsInfo {
  bool ok = false;
  std::string why;
  uint32_t w = 0, h = 0, depth = 1, mips = 1;
  std::string format;  // canonical name; unknown layouts are spelled out ("rgb16 r=.. g=.. b=.. a=..")
  std::string dim = "2D";
  std::string xenos;   // the Xenos-name this layout stands for when it is one of the formats the renderer lists as undecoded ("" otherwise)
};

int Bits(uint32_t m) { return __builtin_popcount(m); }

DdsInfo ClassifyDds(const uint8_t* p, size_t n) {
  DdsInfo d;
  if (n < 128 || std::memcmp(p, "DDS ", 4) != 0) return d.why = "not a DDS", d;
  auto rd = [&](size_t o) {
    uint32_t v;
    std::memcpy(&v, p + o, 4);
    return v;
  };
  const uint32_t flags = rd(8), pfflags = rd(80), fourcc = rd(84), bits = rd(88), rm = rd(92), gm = rd(96), bm = rd(100), am = rd(104),
                 caps2 = rd(112);
  d.h = rd(12), d.w = rd(16);
  d.depth = (flags & 0x800000u) ? std::max(rd(24), 1u) : 1u;
  d.mips = (flags & 0x20000u) ? std::max(rd(28), 1u) : 1u;
  if (caps2 & 0x200u) d.dim = "cube";
  else if ((caps2 & 0x200000u) || d.depth > 1) d.dim = "3D";
  if (d.w == 0 || d.h == 0 || d.w > 16384 || d.h > 16384) return d.why = "bad DDS size", d;
  char b[160];
  if (pfflags & 4u) {  // FOURCC
    char s[5] = {char(fourcc), char(fourcc >> 8), char(fourcc >> 16), char(fourcc >> 24), 0};
    bool print = true;
    for (int i = 0; i < 4; ++i)
      if (s[i] < 32 || s[i] > 126) print = false;
    if (print) d.format = s;
    else std::snprintf(b, sizeof b, "fourcc:0x%08x", fourcc), d.format = b;
    if (d.format == "CTX1") d.xenos = "CTX1";
    if (d.format == "DXT3A" || d.format == "DX3A" || d.format == "3A..") d.xenos = "DXT3A";
  } else if (pfflags & 0x40u) {  // RGB
    const int ra = Bits(rm), ga = Bits(gm), ba = Bits(bm), aa = Bits(am);
    const uint32_t a = pfflags & 1u ? am : 0;
    if (bits == 32 && rm == 0x00FF0000 && gm == 0xFF00 && bm == 0xFF) d.format = a ? "A8R8G8B8" : "X8R8G8B8";
    else if (bits == 32 && rm == 0xFF && gm == 0xFF00 && bm == 0xFF0000) d.format = a ? "A8B8G8R8" : "X8B8G8R8";
    else if (bits == 16 && rm == 0x0F00 && gm == 0xF0 && bm == 0xF && a == 0xF000) d.format = "A4R4G4B4";
    else if (bits == 16 && rm == 0xF800 && gm == 0x07E0 && bm == 0x001F && !a) d.format = "R5G6B5", d.xenos = "5_6_5";
    else if (bits == 16 && rm == 0x7C00 && gm == 0x03E0 && bm == 0x001F && a == 0x8000) d.format = "A1R5G5B5", d.xenos = "1_5_5_5";
    else if (bits == 16 && rm == 0x7C00 && gm == 0x03E0 && bm == 0x001F && !a) d.format = "X1R5G5B5", d.xenos = "1_5_5_5";
    else {
      std::snprintf(b, sizeof b, "rgb%u r=%08x g=%08x b=%08x a=%08x", bits, rm, gm, bm, a);
      d.format = b;
      // 6_5_5: three colour channels of 6 / 5 / 5 bits (any order), 16 bits
      if (bits == 16 && !aa) {
        int c[3] = {ra, ga, ba};
        std::sort(c, c + 3);
        if (c[0] == 5 && c[1] == 5 && c[2] == 6) d.xenos = "6_5_5";
      }
    }
  } else if (pfflags & 0x20000u) {  // LUMINANCE
    std::snprintf(b, sizeof b, "L%u%s", bits, (pfflags & 1u) ? "A" : ""), d.format = b;
  } else if (pfflags & 2u) {
    std::snprintf(b, sizeof b, "A%u", bits), d.format = b;
  } else {
    std::snprintf(b, sizeof b, "pf-flags:0x%x bits %u", pfflags, bits), d.format = b;
  }
  d.ok = true;
  return d;
}

// ---------------------------------------------------------------------------------------------------------------- state
struct Totals {
  size_t pacs = 0, pacs_ok = 0, pacs_stream = 0, pacs_failed = 0;
  size_t meshes = 0, meshes_loaded = 0, models = 0, models_failed = 0, models_variant = 0, meshes_variant = 0;
  size_t textures = 0, texdirs = 0, standalone_dds = 0, fonts = 0, menu_tex = 0, unparsed = 0;
  size_t tex_loader_ok = 0, tex_loader_reject = 0, tex_refs = 0, tex_refs_found = 0, tex_refs_missing = 0;
};

struct KindStat {
  size_t pacs = 0, meshes = 0, loaded = 0, textures = 0, texdirs = 0, models = 0, unparsed = 0;
};

struct TexInfo {
  std::string format, dim;
  uint32_t w = 0, h = 0, mips = 1;
};

struct Atlas {
  std::string out_path;
  std::FILE* json = nullptr;
  bool first_rec = true;
  Totals t;
  std::map<std::string, KindStat> kinds;
  struct EffectStat {
    size_t meshes = 0, loaded = 0, verts = 0;
    std::set<std::string> kinds;
    std::map<std::string, size_t> params;  // parameter name -> meshes
  };
  std::map<std::string, EffectStat> effects;
  struct FmtStat {
    size_t n = 0, loader_reject = 0;
    std::map<std::string, size_t> dims;
    std::set<std::string> pacs;
    std::map<std::string, size_t> pac_count;
  };
  std::map<std::string, FmtStat> formats;
  std::map<std::string, size_t> layouts;                         // "S0 28 | S1 8 skin | S2 8 | S3 12" -> meshes
  std::map<std::string, std::map<std::string, size_t>> leaf_tags;  // kind -> tag -> leaves (not drawable declarations)
  std::map<std::string, size_t> unparsed_reasons;
  std::vector<std::string> unparsed_lines;
  std::map<std::string, size_t> unresolved_tex_names;  // texture name used by a mesh but in no directory of its scope chain
  std::map<std::string, size_t> pac_meshes, pac_textures, pac_unparsed;  // per-pac counts (the self test reads these)
  std::vector<std::string> pac_lines;

  void Rec(const std::string& line) {
    if (!json) return;
    std::fputs(first_rec ? " " : ",\n ", json);
    std::fputs(line.c_str(), json);
    first_rec = false;
  }
};

struct Scope {  // texture directories of one PACH (and its parents)
  std::map<std::string, TexInfo> tex;
  std::map<int, std::string> flags;    // arena slot -> flag tokens (entry 50001)
  std::map<int, int> light_set, motion;
  const Scope* parent = nullptr;
  const TexInfo* Find(const std::string& lname) const {
    for (const Scope* s = this; s; s = s->parent) {
      auto it = s->tex.find(lname);
      if (it != s->tex.end()) return &it->second;
    }
    return nullptr;
  }
};

struct Ctx {
  Atlas* A;
  std::string pac, kind;
  bool arena_like;   // the JBOY streams are read with the arena rule (everything but wrestler pacs)
  bool font_pac, menu_pac;
};

void Unparsed(Ctx& c, const std::string& entry, const std::string& what, const std::string& reason) {
  Atlas& A = *c.A;
  ++A.t.unparsed;
  ++A.kinds[c.kind].unparsed;
  ++A.pac_unparsed[c.pac];
  {
    std::string key = reason.substr(0, 90);
    if (what == "pac")  // (the pac name is in the line list)
      key = reason.find("header-only") != std::string::npos ? "header-only pac (no data after the 16 KB header)" : "no PACH and no ZLIB / BPE block run (plain data or video)";
    if (const size_t cut = key.find(" ("); cut != std::string::npos && what == "model") key = key.substr(0, cut);
    A.unparsed_reasons[what + ": " + key]++;
  }
  if (A.unparsed_lines.size() < 4000) A.unparsed_lines.push_back(c.pac + " | " + entry + " | " + what + " | " + reason);
  A.Rec("{\"type\":\"unparsed\",\"pac\":" + Q(c.pac) + ",\"entry\":" + Q(entry) + ",\"what\":" + Q(what) + ",\"reason\":" + Q(reason) + "}");
}

bool IsTexDir(const Bytes& b) {
  if (b.size() < 0x30) return false;
  const uint32_t count = Le32(b, 0);
  if (count == 0 || count > 4096 || 0x10 + size_t(count) * 32 > b.size()) return false;
  for (uint32_t i = 0; i < count; ++i)
    if (std::memcmp(b.data() + 0x10 + size_t(i) * 32 + 16, "dds", 3) != 0) return false;
  return true;
}

// 50001 text: "<slot> <tokens> [l(N)] [m(N)] ... ;". Flags = the whole-word tokens, kept as written.
void ParseSlotText(const Bytes& txt, Scope& sc) {
  size_t pos = 0;
  while (pos < txt.size()) {
    size_t eol = pos;
    while (eol < txt.size() && txt[eol] != '\n') ++eol;
    const std::string line(reinterpret_cast<const char*>(txt.data()) + pos, eol - pos);
    pos = eol + 1;
    size_t i = 0;
    while (i < line.size() && line[i] == ' ') ++i;
    if (i >= line.size() || line[i] < '0' || line[i] > '9') continue;
    int slot = 0;
    while (i < line.size() && line[i] >= '0' && line[i] <= '9') slot = std::min(slot * 10 + (line[i++] - '0'), 1000000);
    std::string flags, tok;
    for (size_t k = i; k <= line.size(); ++k) {
      const char ch = k < line.size() ? line[k] : ' ';
      if (ch == ' ' || ch == '\t' || ch == '\r' || ch == ';') {
        if (!tok.empty()) {
          if (tok.size() > 2 && tok[1] == '(' && (tok[0] == 'l' || tok[0] == 'm')) {
            const int v = std::atoi(tok.c_str() + 2);
            (tok[0] == 'l' ? sc.light_set : sc.motion)[slot] = v;
          }
          flags += (flags.empty() ? "" : " ") + tok;
        }
        tok.clear();
        if (ch == ';') break;
      } else tok.push_back(ch);
    }
    sc.flags[slot] = flags;
  }
}

void Process(Ctx& c, const Bytes& d, const std::string& path, const Scope& scope);

void RecTexture(Ctx& c, const std::string& dir_entry, const std::string& name, const uint8_t* p, size_t n, Scope* sc, const char* role) {
  Atlas& A = *c.A;
  const DdsInfo di = ClassifyDds(p, n);
  if (!di.ok) {
    Unparsed(c, dir_entry + "/" + name, "texture", di.why);
    return;
  }
  Texture tx;
  std::string why;
  const bool lok = ParseDds(p, n, tx, why);
  ++A.t.textures;
  ++A.kinds[c.kind].textures;
  ++A.pac_textures[c.pac];
  (lok ? A.t.tex_loader_ok : A.t.tex_loader_reject)++;
  Atlas::FmtStat& fs_ = A.formats[di.format];
  ++fs_.n, ++fs_.dims[di.dim];
  if (!lok) ++fs_.loader_reject;
  ++fs_.pac_count[c.pac];
  if (!di.xenos.empty()) A.formats["(xenos " + di.xenos + ")"].n++, A.formats["(xenos " + di.xenos + ")"].pac_count[c.pac]++;
  if (sc) sc->tex[Lower(name)] = {di.format, di.dim, di.w, di.h, di.mips};
  A.Rec("{\"type\":\"texture\",\"pac\":" + Q(c.pac) + ",\"entry\":" + Q(dir_entry) + ",\"name\":" + Q(name) + ",\"role\":" + Q(role) +
        ",\"format\":" + Q(di.format) + ",\"xenos\":" + Q(di.xenos) + ",\"width\":" + std::to_string(di.w) + ",\"height\":" + std::to_string(di.h) +
        ",\"depth\":" + std::to_string(di.depth) + ",\"mips\":" + std::to_string(di.mips) + ",\"dim\":" + Q(di.dim) + ",\"bytes\":" + std::to_string(n) +
        ",\"loader\":" + Q(lok ? (why.empty() ? "ok" : "ok: " + why) : why) + "}");
}

void ProcessModel(Ctx& c, const Bytes& blob, const std::string& path, const Scope& scope) {
  Atlas& A = *c.A;
  Model m;
  std::vector<JboyDecl> decl;
  JboyOptions o;
  o.arena = c.arena_like;
  o.part = path;
  o.decl = &decl;
  std::string err;
  const bool ok = LoadJboy(blob, o, m, err);
  ++A.t.models;
  ++A.kinds[c.kind].models;
  if (!ok) {
    ++A.t.models_failed;
    Unparsed(c, path, "model", err);
  }
  // A model whose mesh records are not the 0xB8-byte kind (bgEtc.pac props / screens): the effect name sits 4 bytes earlier and every record
  // is 0xAC bytes [U: stride from the distance between two effect names; the loader reads these as garbage]. Not parsed: one "unparsed"
  // item per model naming how many meshes and which effects (read at +0x78 of each 0xAC record), and no mesh records.
  if (ok && !decl.empty() && blob.size() >= 8 + 0x40) {
    const uint8_t* body = blob.data() + 8;
    const size_t blen = blob.size() - 8;
    auto plausible = [](const std::string& s) {
      if (s.size() < 3) return false;
      for (char ch : s)
        if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_')) return false;
      return true;
    };
    const uint32_t nmesh = Be32(body + 0x10), tab = Be32(body + 0x14);
    auto name_at = [&](size_t off) {
      if (off + 16 > blen) return std::string();
      size_t l = 0;
      while (l < 16 && body[off + l]) ++l;
      return std::string(reinterpret_cast<const char*>(body + off), l);
    };
    if (!decl[0].loaded && decl[0].effect.rfind("y", 0) != 0 && plausible(name_at(tab + 0x78)) && name_at(tab + 0x78).rfind("y", 0) == 0) {
      std::string names;
      for (uint32_t k = 0; k < nmesh && k < 64; ++k) names += (k ? "," : "") + name_at(tab + 0xACull * k + 0x78);
      Unparsed(c, path, "model", "JBOY mesh records are a 0xAC-byte variant, not the 0xB8 kind (" + std::to_string(nmesh) + " meshes; effects at +0x78: " + names + ") [U stride]");
      A.t.meshes_variant += nmesh;
      ++A.t.models_variant;
      return;
    }
  }
  // the arena's slot flags: entry id = slot
  int slot = -1;
  {
    const size_t sp = path.find_last_of('/');
    const std::string last = sp == std::string::npos ? path : path.substr(sp + 1);
    if (!last.empty() && std::all_of(last.begin(), last.end(), [](char ch) { return ch >= '0' && ch <= '9'; })) slot = std::atoi(last.c_str());
  }
  for (size_t di = 0; di < decl.size(); ++di) {
    const JboyDecl& d = decl[di];
    const size_t wb = d.warn_begin, we = di + 1 < decl.size() ? decl[di + 1].warn_begin : m.warnings.size();
    std::string skip;
    if (!d.loaded)
      for (size_t k = wb; k < we && k < m.warnings.size(); ++k) skip += (skip.empty() ? "" : "; ") + m.warnings[k];
    if (!d.loaded && skip.empty()) skip = ok ? "skipped by the loader (no warning)" : err;
    ++A.t.meshes;
    ++A.kinds[c.kind].meshes;
    ++A.pac_meshes[c.pac];
    if (d.loaded) ++A.t.meshes_loaded, ++A.kinds[c.kind].loaded;
    else Unparsed(c, path + "#" + std::to_string(d.mesh_index), "mesh", skip);
    Atlas::EffectStat& es = A.effects[d.effect];
    ++es.meshes, es.verts += d.vertices;
    if (d.loaded) ++es.loaded;
    es.kinds.insert(c.kind);
    // stream layout string
    char lay[160];
    std::snprintf(lay, sizeof lay, "S0 %u | S1 %u %s | S2 %u | S3 %u", d.stride[0], d.stride[1], d.s1_skin ? "skin" : "uv", d.stride[2], d.stride[3]);
    A.layouts[d.tiles ? lay : std::string("(streams do not tile)")]++;
    std::string params, texs;
    for (const JboyDeclParam& p : d.params) {
      ++es.params[p.name];
      const char* tn = p.kind == 0x0F ? "sampler" : p.kind == 0x0D ? "float4" : p.kind == 0x0A ? "float" : "other";
      if (!params.empty()) params += ",";
      params += "{\"n\":" + Q(p.name) + ",\"t\":" + Q(tn);
      if (p.kind == 0x0F) {
        params += ",\"tex\":" + Q(p.tex_name) + ",\"index\":" + std::to_string(p.tex_index);
        if (p.tex_index >= 0) {
          ++A.t.tex_refs;
          const TexInfo* ti = p.tex_name.empty() ? nullptr : scope.Find(Lower(p.tex_name));
          if (!texs.empty()) texs += ",";
          texs += "{\"param\":" + Q(p.name) + ",\"name\":" + Q(p.tex_name) + ",\"found\":" + (ti ? "true" : "false");
          if (ti) {
            ++A.t.tex_refs_found;
            texs += ",\"format\":" + Q(ti->format) + ",\"width\":" + std::to_string(ti->w) + ",\"height\":" + std::to_string(ti->h) + ",\"mips\":" +
                    std::to_string(ti->mips) + ",\"dim\":" + Q(ti->dim);
          } else {
            ++A.t.tex_refs_missing;
            A.unresolved_tex_names[p.tex_name]++;
          }
          texs += "}";
        }
      } else if (p.kind == 0x0D) {
        params += ",\"v\":[" + Num(p.v[0]) + "," + Num(p.v[1]) + "," + Num(p.v[2]) + "," + Num(p.v[3]) + "]";
      } else if (p.kind == 0x0A) {
        params += ",\"v\":[" + Num(p.v[0]) + "]";
      } else {
        params += ",\"size\":" + std::to_string(p.size);
      }
      params += "}";
    }
    std::string flags;
    if (slot >= 0) {
      const Scope* hit = nullptr;
      for (const Scope* s = &scope; s; s = s->parent)
        if (s->flags.count(slot)) {
          hit = s;
          break;
        }
      if (hit) {
        flags = ",\"slot_flags\":" + Q(hit->flags.at(slot));
        auto ls = hit->light_set.find(slot);
        if (ls != hit->light_set.end()) flags += ",\"light_set\":" + std::to_string(ls->second);
        auto mo = hit->motion.find(slot);
        if (mo != hit->motion.end()) flags += ",\"motion\":" + std::to_string(mo->second);
      }
    }
    A.Rec("{\"type\":\"mesh\",\"pac\":" + Q(c.pac) + ",\"entry\":" + Q(path) + ",\"mesh\":" + std::to_string(d.mesh_index) + ",\"effect\":" + Q(d.effect) +
          ",\"vertices\":" + std::to_string(d.vertices) + ",\"palette\":" + std::to_string(d.palette) + ",\"streams\":{\"s0\":" + std::to_string(d.stride[0]) +
          ",\"s1\":" + std::to_string(d.stride[1]) + ",\"s1_kind\":" + Q(d.s1_skin ? "skin" : "uv") + ",\"s2\":" + std::to_string(d.stride[2]) + ",\"s3\":" +
          std::to_string(d.stride[3]) + ",\"tiles\":" + (d.tiles ? "true" : "false") + "},\"index_count\":" + std::to_string(d.index_count) +
          ",\"index_groups\":" + std::to_string(d.index_groups) + ",\"loaded\":" + (d.loaded ? "true" : "false") + ",\"skip\":" + Q(skip) + flags +
          ",\"params\":[" + params + "],\"textures\":[" + texs + "]}");
  }
}

void WalkPach(Ctx& c, const Bytes& buf, const pac::Pach& p, const std::string& path, const Scope* parent) {
  Atlas& A = *c.A;
  Scope sc;
  sc.parent = parent;
  std::vector<Bytes> leaves(p.ents.size());
  for (size_t i = 0; i < p.ents.size(); ++i) {
    const pac::Ent& e = p.ents[i];
    leaves[i] = pac::Decode(buf.data() + e.off, e.size);
    if (leaves[i].empty() && e.size >= 16 && (std::memcmp(buf.data() + e.off, "BPE ", 4) == 0 || std::memcmp(buf.data() + e.off, "ZLIB", 4) == 0) &&
        Le32(buf, e.off + 12) == 0) {
      A.leaf_tags[c.kind]["(empty compressed block)"]++;  // a block that expands to 0 bytes: an empty entry, not a failure
    } else if (leaves[i].empty() && e.size > 0) {
      std::string tag = e.size >= 4 ? std::string(reinterpret_cast<const char*>(buf.data() + e.off), 4) : "?";
      for (char& ch : tag)
        if (ch < 32 || ch > 126) ch = '.';
      Unparsed(c, path + "/" + std::to_string(e.id), "leaf", "decode failed (tag " + tag + ", " + std::to_string(e.size) + " bytes)");
    }
  }
  // pass 1: texture directories and the slot text of this PACH (so a model finds its textures whatever the entry order)
  for (size_t i = 0; i < p.ents.size(); ++i) {
    const Bytes& d = leaves[i];
    const std::string ep = path + "/" + std::to_string(p.ents[i].id);
    if (d.empty()) continue;
    if (p.ents[i].id == 50001 && !(d.size() >= 4 && std::memcmp(d.data(), "PACH", 4) == 0)) ParseSlotText(d, sc);
    if (IsTexDir(d)) {
      ++A.t.texdirs;
      ++A.kinds[c.kind].texdirs;
      const uint32_t count = Le32(d, 0);
      for (uint32_t k = 0; k < count; ++k) {
        const size_t r = 0x10 + size_t(k) * 32;
        char nm[17] = {};
        std::memcpy(nm, d.data() + r, 16);
        const uint32_t size = Le32(d, r + 20), off = Le32(d, r + 24);
        if (off > d.size() || size > d.size() - off) {
          Unparsed(c, ep + "/" + nm, "texture", "directory record points past the entry");
          continue;
        }
        RecTexture(c, ep, nm, d.data() + off, size, &sc, c.font_pac ? "font" : c.menu_pac ? "menu" : "model");
        if (c.font_pac) ++A.t.fonts;
        else if (c.menu_pac) ++A.t.menu_tex;
      }
    }
  }
  // pass 2: models, standalone DDS, nested PACHs, everything else
  for (size_t i = 0; i < p.ents.size(); ++i) {
    const Bytes& d = leaves[i];
    if (d.empty()) continue;
    if (IsTexDir(d)) continue;
    Process(c, d, path + "/" + std::to_string(p.ents[i].id), sc);
  }
}

void Process(Ctx& c, const Bytes& d, const std::string& path, const Scope& scope) {
  Atlas& A = *c.A;
  std::string tag = d.size() >= 4 ? std::string(reinterpret_cast<const char*>(d.data()), 4) : std::string("<4");
  for (char& ch : tag)
    if (ch < 32 || ch > 126) ch = '.';
  pac::Pach np;
  if (tag == "PACH") {
    if (pac::ParsePach(d, 0, d.size(), np)) WalkPach(c, d, np, path, &scope);
    else Unparsed(c, path, "pach", "PACH header does not parse (count / bounds)");
    return;
  }
  if (tag == "JBOY") {
    ProcessModel(c, d, path, scope);
    return;
  }
  if (tag == "DDS ") {
    ++A.t.standalone_dds;
    const char* role = c.font_pac ? "font" : c.menu_pac ? "menu" : "other";
    if (c.font_pac) ++A.t.fonts;
    else if (c.menu_pac) ++A.t.menu_tex;
    RecTexture(c, path, "(file)", d.data(), d.size(), nullptr, role);
    return;
  }
  A.leaf_tags[c.kind][tag]++;
}

// The headed pacs without a top-level PACH (chHeads, evt\Entrance*, m.pac, ...): the file's data is a run of blocks (ZLIB / BPE / plain),
// each starting on a 2 KB boundary; the game finds them through plist360.arc, which we do not read. Walk the run by the blocks' own sizes.
struct Block {
  size_t pos;
  Bytes data;
};
bool BlockStream(const Bytes& b, std::vector<Block>& out, std::string& why, size_t& skipped_bytes) {
  size_t pos = 0x4000;
  skipped_bytes = 0;
  while (pos + 16 <= b.size()) {
    const bool zl = std::memcmp(b.data() + pos, "ZLIB", 4) == 0, bpe = std::memcmp(b.data() + pos, "BPE ", 4) == 0;
    if (zl || bpe) {
      const uint32_t packed = Le32(b, pos + 8);
      Bytes d = pac::Decode(b.data() + pos, std::min<size_t>(b.size() - pos, size_t(packed) + 16 + 16));
      size_t len = size_t(packed) + 16;
      if (d.empty()) {
        why = "block at 0x" + std::to_string(pos) + " does not decode";
        pos = (pos + 0x800) & ~size_t(0x7FF);
        skipped_bytes += 0x800;
        continue;
      }
      out.push_back({pos, std::move(d)});
      pos = (pos + len + 0x7FF) & ~size_t(0x7FF);
      continue;
    }
    pac::Pach p;
    if (std::memcmp(b.data() + pos, "PACH", 4) == 0 && pac::ParsePach(b, pos, b.size(), p)) {
      Bytes whole(b.begin() + long(pos), b.begin() + long(p.end));
      out.push_back({pos, std::move(whole)});
      pos = (p.end + 0x7FF) & ~size_t(0x7FF);
      continue;
    }
    pos += 0x800;
    skipped_bytes += 0x800;
  }
  return !out.empty();
}

void DoPac(Atlas& A, launcher::GameFiles& files, const std::string& rel) {
  Ctx c;
  c.A = &A;
  c.pac = rel;
  std::string stem = fs::path(rel).stem().string();
  while (!stem.empty() && std::isdigit(static_cast<unsigned char>(stem.back()))) stem.pop_back();
  c.kind = fs::path(rel).parent_path().filename().string() + "/" + stem;
  const std::string lrel = Lower(rel);
  c.arena_like = !(lrel.rfind("pac/ch/ch", 0) == 0 && lrel.find("common") == std::string::npos && lrel.find("heads") == std::string::npos && lrel.find("etc") == std::string::npos) &&
                 lrel.rfind("pac/edit/ed", 0) != 0;
  c.font_pac = lrel.find("font") != std::string::npos;
  c.menu_pac = lrel.rfind("pac/menu/", 0) == 0 && !c.font_pac;
  ++A.t.pacs;
  ++A.kinds[c.kind].pacs;
  pac::Pac pc;
  std::string err;
  const size_t m0 = A.t.meshes, t0 = A.t.textures, u0 = A.t.unparsed;
  const bool ok = pac::Open(files, rel, pc, err);
  std::string status = "ok", reason;
  const Scope root;
  if (ok) {
    ++A.t.pacs_ok;
    for (size_t i = 0; i < pc.pachs.size(); ++i) WalkPach(c, pc.bytes, pc.pachs[i], "p" + std::to_string(i), &root);
  } else if (err.find("no PACH found") != std::string::npos && pc.bytes.size() > 0x4000) {
    std::vector<Block> blocks;
    size_t skipped = 0;
    std::string why;
    if (BlockStream(pc.bytes, blocks, why, skipped)) {
      status = "stream";
      ++A.t.pacs_stream;
      reason = std::to_string(blocks.size()) + " blocks";
      if (!why.empty()) reason += "; " + why;
      for (size_t i = 0; i < blocks.size(); ++i) Process(c, blocks[i].data, "b" + std::to_string(i), root);
    } else {
      status = "failed", reason = err + (pc.bytes.size() <= 0x4800 ? " (header-only pac: no data after the 16 KB header) in the " : " in the ") + std::to_string(pc.bytes.size()) + "-byte unpacked file (header " + std::string(reinterpret_cast<const char*>(pc.bytes.data()), 4) +
                                   "): no PACH and no ZLIB / BPE block at a 2 KB step after 0x4000 (plain data or video; not a drawable container the loaders know)" + (why.empty() ? "" : "; " + why);
      ++A.t.pacs_failed;
      Unparsed(c, "(pac)", "pac", reason);
    }
  } else {
    status = "failed", reason = err;
    ++A.t.pacs_failed;
    Unparsed(c, "(pac)", "pac", err);
  }
  char line[400];
  std::snprintf(line, sizeof line, "%-34s %-7s meshes %5zu textures %5zu unparsed %4zu %s", rel.c_str(), status.c_str(), A.t.meshes - m0, A.t.textures - t0,
                A.t.unparsed - u0, reason.c_str());
  A.pac_lines.push_back(line);
  A.Rec("{\"type\":\"pac\",\"pac\":" + Q(rel) + ",\"kind\":" + Q(c.kind) + ",\"status\":" + Q(status) + ",\"reason\":" + Q(reason) + ",\"meshes\":" +
        std::to_string(A.t.meshes - m0) + ",\"textures\":" + std::to_string(A.t.textures - t0) + ",\"unparsed\":" + std::to_string(A.t.unparsed - u0) + "}");
}

std::vector<std::string> ListPacs(const fs::path& game) {
  std::vector<std::string> v;
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(game / "pac", ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (!it->is_regular_file()) continue;
    std::string ext = Lower(it->path().extension().string());
    if (ext != ".pac") continue;
    v.push_back(fs::relative(it->path(), game).generic_string());
  }
  std::sort(v.begin(), v.end());
  return v;
}

// ---------------------------------------------------------------------------------------------------------------- summary
void WriteSummary(const Atlas& A, const fs::path& out, size_t pac_list_n, double secs) {
  std::ostringstream f;  // (written at the end with every byte >= 127 replaced: game strings may hold Shift-JIS)
  const Totals& t = A.t;
  f << "# Drawables atlas summary (S101 lane 4; game-derived names: local only)\n\n";
  f << "Run took " << int(secs) << " s. Source: `game_data\\extracted\\pac` read with src\\assets loaders; format `docs\\formats\\drawables_atlas.md`.\n\n";
  f << "## Headline numbers\n\n";
  f << "- **Pacs: " << t.pacs << "** of " << pac_list_n << " listed (read as PACH archives: " << t.pacs_ok << "; read as a block run, no top-level PACH: " << t.pacs_stream
    << "; failed: " << t.pacs_failed << "). Accounted: " << (t.pacs_ok + t.pacs_stream + t.pacs_failed) << (t.pacs == pac_list_n && t.pacs_ok + t.pacs_stream + t.pacs_failed == t.pacs ? " (matches)" : " (MISMATCH)") << ".\n";
  f << "- **Models (JBOY blocks): " << t.models << "** (" << t.models_failed << " failed as a whole; " << t.models_variant << " use a different mesh-record layout and are listed as unparsed, holding " << t.meshes_variant << " meshes). **Meshes (0xB8 records): " << t.meshes << "**, loaded by our loader " << t.meshes_loaded << ", skipped " << (t.meshes - t.meshes_loaded) << ".\n";
  f << "- **Textures: " << t.textures << "** (" << t.texdirs << " texture directories + " << t.standalone_dds << " standalone DDS files); the loader's ParseDds accepts " << t.tex_loader_ok << ", rejects " << t.tex_loader_reject << ".\n";
  f << "- Fonts (fontHD/fontSD textures): " << t.fonts << ". Menu textures (pac\\menu\\* except fonts): " << t.menu_tex << ".\n";
  f << "- Mesh texture references: " << t.tex_refs << " (resolved in a texture directory of the mesh's own PACH chain: " << t.tex_refs_found << ", not resolved there: " << t.tex_refs_missing << " - e.g. shared `chCommon` textures).\n";
  f << "- **Unparsed items: " << t.unparsed << "** (reasons below).\n\n";

  f << "## The undecoded-format answer (STATUS \"Other open problems\")\n\n";
  f << "Texture formats the renderer lists as not decoded: 1_5_5_5, 5_6_5, 6_5_5, DXT3A, CTX1, 3D. Counted by the pixel layout each DDS declares:\n\n| format | textures | files (pacs) using it |\n|---|---|---|\n";
  for (const char* x : {"1_5_5_5", "5_6_5", "6_5_5", "DXT3A", "CTX1"}) {
    auto it = A.formats.find(std::string("(xenos ") + x + ")");
    f << "| " << x << " | " << (it == A.formats.end() ? 0 : it->second.n) << " | ";
    if (it == A.formats.end() || it->second.pac_count.empty()) f << "none";
    else {
      size_t k = 0;
      for (auto& [pk, n] : it->second.pac_count) {
        if (k++ >= 12) {
          f << ", ... (" << it->second.pac_count.size() << " pacs)";
          break;
        }
        f << (k > 1 ? ", " : "") << pk << " (" << n << ")";
      }
    }
    f << " |\n";
  }
  size_t cube = 0, vol = 0;
  std::map<std::string, size_t> cube_pacs, vol_pacs;
  for (auto& [fm, fst] : A.formats) {
    if (fm[0] == '(') continue;
    auto c3 = fst.dims.find("3D"), cc = fst.dims.find("cube");
    if (c3 != fst.dims.end()) vol += c3->second;
    if (cc != fst.dims.end()) cube += cc->second;
  }
  f << "| 3D (volume) textures | " << vol << " | (see atlas.json: dim = \"3D\") |\n";
  f << "\nCube maps (for information; the loader does not decode them as cube): " << cube << ".\n\n";
  f << "Every distinct DDS pixel format found, how many textures, and how many our loader's `ParseDds` rejects:\n\n| format | textures | loader rejects | dims |\n|---|---|---|---|\n";
  for (auto& [fm, fst] : A.formats) {
    if (fm[0] == '(') continue;
    f << "| `" << fm << "` | " << fst.n << " | " << fst.loader_reject << " | ";
    size_t k = 0;
    for (auto& [dm, n] : fst.dims) f << (k++ ? ", " : "") << dm << " " << n;
    f << " |\n";
  }
  f << "\nVertex element formats: the files declare none. A JBOY mesh has four fixed planar streams (see the layouts table); the GPU vertex the "
       "game binds is built from them by game code the project has not located (docs\\systems\\assets.md, \"The JBOY model block\"), so whether the "
       "renderer's binder handles every element format the game *binds* cannot be answered from the files - it needs the capture side. "
       "What the files do say is the layout table below.\n\n";

  f << "## Per pac kind\n\n| kind | pacs | models | meshes (loaded) | texture dirs | textures | unparsed |\n|---|---|---|---|---|---|---|\n";
  for (auto& [k, s] : A.kinds) f << "| " << k << " | " << s.pacs << " | " << s.models << " | " << s.meshes << " (" << s.loaded << ") | " << s.texdirs << " | " << s.textures << " | " << s.unparsed << " |\n";

  f << "\n## Per effect (the mesh's material class name)\n\n| effect | meshes | loaded | vertices | pac kinds |\n|---|---|---|---|---|\n";
  for (auto& [e, s] : A.effects) {
    f << "| `" << e << "` | " << s.meshes << " | " << s.loaded << " | " << s.verts << " | ";
    size_t k = 0;
    for (const std::string& kk : s.kinds) f << (k++ ? ", " : "") << kk;
    f << " |\n";
  }
  f << "\n## Per effect: parameter names (meshes carrying each)\n\n";
  for (auto& [e, s] : A.effects) {
    f << "- `" << e << "`:";
    for (auto& [pn, n] : s.params) f << " " << pn << "(" << n << ")";
    f << "\n";
  }

  f << "\n## Stream layouts (bytes per vertex of S0..S3; S1 is skin pairs or UV sets)\n\n| layout | meshes |\n|---|---|\n";
  for (auto& [l, n] : A.layouts) f << "| " << l << " | " << n << " |\n";

  f << "\n## Unparsed, by reason\n\n| reason | items |\n|---|---|\n";
  for (auto& [r, n] : A.unparsed_reasons) f << "| " << r << " | " << n << " |\n";
  f << "\nFirst " << A.unparsed_lines.size() << " (pac | entry | what | reason):\n\n```\n";
  for (const std::string& l : A.unparsed_lines) f << l << "\n";
  f << "```\n";

  f << "\n## Mesh texture names found in no directory of the mesh's own PACH chain (top 40 by meshes)\n\n";
  {
    std::vector<std::pair<size_t, std::string>> v;
    for (auto& [n, c] : A.unresolved_tex_names) v.push_back({c, n});
    std::sort(v.rbegin(), v.rend());
    for (size_t i = 0; i < v.size() && i < 40; ++i) f << "- `" << v[i].second << "` x" << v[i].first << "\n";
    f << "(" << v.size() << " distinct names)\n";
  }

  f << "\n## Leaves that are not drawable declarations (tag census by pac kind)\n\nFor completeness: every leaf that is not a PACH / JBOY / DDS / texture directory, by its first four bytes (non-printable as `.`). These hold no mesh or texture declaration this tool reads.\n\n";
  for (auto& [k, m] : A.leaf_tags) {
    f << "- " << k << ":";
    std::vector<std::pair<size_t, std::string>> v;
    for (auto& [tg, n] : m) v.push_back({n, tg});
    std::sort(v.rbegin(), v.rend());
    for (size_t i = 0; i < v.size() && i < 14; ++i) f << " `" << v[i].second << "`x" << v[i].first;
    if (v.size() > 14) f << " ... (" << v.size() << " tags)";
    f << "\n";
  }
  f << "\n## Per pac\n\n```\n";
  for (const std::string& l : A.pac_lines) f << l << "\n";
  f << "```\n";
  std::string text = f.str();
  for (char& ch : text)
    if (static_cast<unsigned char>(ch) >= 127) ch = '?';
  std::ofstream(out / "summary.md", std::ios::binary) << text;
}

// ---------------------------------------------------------------------------------------------------------------- self test
// The three pacs of the self test: one wrestler, one arena, menuHD. The expected counts are in tools\atlas\expect.json (counts and our field names only).
const char* kCheckPacs[3] = {"pac/ch/ch100.pac", "pac/bg/bg05.pac", "pac/menu/menuHD.pac"};

int RunCheck(launcher::GameFiles& files, const std::string& expect_path, bool fault, bool write_expect) {
  Atlas A;  // no json output
  for (const char* p : kCheckPacs) DoPac(A, files, p);
  // counts as "pac key field value" lines, compared against the expect file's lines of the same form (see expect.json)
  std::map<std::string, size_t> got;
  for (const char* p : kCheckPacs) {
    got[std::string(p) + " meshes"] = A.pac_meshes[p];
    got[std::string(p) + " textures"] = A.pac_textures[p];
    got[std::string(p) + " unparsed"] = A.pac_unparsed[p];
  }
  got["total models"] = A.t.models;
  got["total texdirs"] = A.t.texdirs;
  got["total effects"] = A.effects.size();
  got["total formats"] = [&] {
    size_t n = 0;
    for (auto& [k, v] : A.formats)
      if (k[0] != '(') ++n;
    return n;
  }();
  if (write_expect) {  // regenerate tools/atlas/expect.json from the current counts (flat JSON: counts and our field names only)
    std::ofstream w(expect_path);
    w << "{\n";
    size_t k = 0;
    for (auto& [key, v] : got) w << " \"" << key << "\": " << v << (++k < got.size() ? "," : "") << "\n";
    w << "}\n";
    std::printf("wrote %zu counts to %s\n", got.size(), expect_path.c_str());
    return 0;
  }
  std::ifstream f(expect_path);
  if (!f) {
    std::printf("FAIL cannot read %s\n", expect_path.c_str());
    return 2;
  }
  std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  int bad = 0, checked = 0;
  for (auto& [k, v] : got) {
    // expect.json is flat: "key": number,
    const std::string needle = "\"" + k + "\":";
    const size_t pos = text.find(needle);
    if (pos == std::string::npos) {
      std::printf("FAIL no expected value for \"%s\"\n", k.c_str());
      ++bad;
      continue;
    }
    long want = std::strtol(text.c_str() + pos + needle.size(), nullptr, 10);
    if (fault) ++want;
    ++checked;
    if (size_t(want) != v) {
      std::printf("FAIL %s: got %zu, expected %ld\n", k.c_str(), v, want);
      ++bad;
    }
  }
  if (!checked) {
    std::printf("FAIL nothing checked\n");
    return 2;
  }
  if (bad) return std::printf("atlas check: %d of %d counts differ\n", bad, checked), 1;
  std::printf("atlas check: %d counts match (%zu meshes, %zu textures over 3 pacs)\n", checked, A.t.meshes, A.t.textures);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::string game, out, pac_rel, expect;
  bool all = false, check = false, fault = false, write_expect = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
    if (a == "--game") game = next();
    else if (a == "--out") out = next();
    else if (a == "--pac") pac_rel = next();
    else if (a == "--all") all = true;
    else if (a == "--check") check = true;
    else if (a == "--fault") fault = true;
    else if (a == "--write-expect") write_expect = true;
    else if (a == "--expect") expect = next();
    else {
      std::fprintf(stderr, "usage: atlas --game DIR (--all | --pac REL | --check --expect FILE [--fault]) [--out DIR]\n");
      return 2;
    }
  }
  if (game.empty()) return std::fprintf(stderr, "--game DIR is required\n"), 2;
  launcher::GameFiles files;
  files.SetRoot(game);
  if (check) return RunCheck(files, expect, fault, write_expect);
  if (!all && pac_rel.empty()) return std::fprintf(stderr, "give --all, --pac or --check\n"), 2;
  if (out.empty()) out = "out/atlas";
  fs::create_directories(out);
  const auto t0 = std::chrono::steady_clock::now();
  Atlas A;
  A.json = std::fopen((fs::path(out) / "atlas.json").string().c_str(), "wb");
  if (!A.json) return std::fprintf(stderr, "cannot write atlas.json in %s\n", out.c_str()), 2;
  std::fputs("[\n", A.json);
  std::vector<std::string> list = all ? ListPacs(game) : std::vector<std::string>{pac_rel};
  if (all && list.empty()) return std::fprintf(stderr, "no pacs under %s\\pac\n", game.c_str()), 2;
  size_t n = 0;
  for (const std::string& rel : list) {
    DoPac(A, files, rel);
    std::printf("[%zu/%zu] %s\n", ++n, list.size(), A.pac_lines.back().c_str());
    std::fflush(stdout);
  }
  std::fputs("\n]\n", A.json);
  std::fclose(A.json);
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  WriteSummary(A, out, list.size(), secs);
  if (all) {
    std::ofstream k(fs::path(out) / "KEEP");
    k << "drawables atlas, game-derived names, local only, S101\n";
  }
  const Totals& t = A.t;
  std::printf("TOTALS: %zu pacs (%zu archive, %zu block-run, %zu failed), %zu models, %zu meshes (%zu loaded), %zu textures, %zu unparsed, %.0f s\n", t.pacs, t.pacs_ok,
              t.pacs_stream, t.pacs_failed, t.models, t.meshes, t.meshes_loaded, t.textures, t.unparsed, secs);
  if (all && t.pacs != list.size()) return std::printf("FAIL pac count %zu != listed %zu\n", t.pacs, list.size()), 1;
  if (all && t.pacs_ok + t.pacs_stream + t.pacs_failed != t.pacs) return std::printf("FAIL pacs not all accounted\n"), 1;
  return 0;
}
