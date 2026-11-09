// Session 97 (video on the stage, Jarrod: "The stage art isn't textures, they're video files ... they should map onto the
// stage"): the entrance stage / video wall of an arena (pac keys 800-809) draws movie frames, not stored textures.
// Their materials name samplers that exist in no texture directory (titantron_movie, main&banner, ppv_header&wall,
// ss_header&wall, mini_tron); the loader (jboy.h) keeps that name in Material::diffuse_name, and this header fills them
// from a folder of PNGs the GAME made on the player's own machine (--wwe2k14_tron_export=DIR, src\tron_export.h):
//   DIR\NNN.png                          loose titantron NNN (Bink 2, 320x320 sheet: big screen rows 0-223 + four 320x24 strips)
//   DIR\evtmov\<GROUP>_<Mnnn>.png        pac\evt\evtmov*.pac movies: first picture; Bink 1 alpha ones also <...>_end.png
// Nothing here is shipped: the PNGs are made from the player's own files and read at run time (CLAUDE.md rule 1).
//
// Mapping (out\runtime\s97\stage_video.md section 4; [C] = by UV arithmetic, [U] = unconfirmed, settled by A/B only):
//   titantron_movie  (800, big screen, UV 0..1)   = tron rows 0-223 of the sheet                         [U: rows 0-223 vs all 320]
//   main&banner      (803 / 804 / 807, UV bands on the 320x320 sheet) = the whole tron sheet              [C by UV]
//   ppv_header&wall / ss_header&wall (801 806 808 / 802 809) = atlas 1280x814 [U]: MOVL/M<arena> (1280x280) on rows 0-279,
//                    MOVN/M<arena> (1280x400) from row 282 (band edges 0.344 = 280 / 814, wall band from 0.347)
//   mini_tron        (805)                         = [U] MOVL/M<arena> first (aspect 3.9:1), else MVAB/M<arena>
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "asset_model.h"
#include "mods_lookup.h"  // Session 98: the player's own tron mods win while USE MY MODS is on
// (needs stb_image.h included before, once per program, as pac.h does: stbi_load_from_memory)

namespace assets {
namespace stage_media {

struct Settings {
  std::string dir;  // "" = look for the launcher's tron cache (default_dir)
  // Session 98 phone port: where the launcher's tron cache lives on this platform (the PC viewer sets %LOCALAPPDATA%\wwe2k14-launcher\trons;
  // the phone its files folder); "" = none.
  std::string default_dir;
  int wrestler = -1;  // >= 0: the entrance look of this wrestler (MVSW / MVSB movies + his tron on the big screen); -1 = the match look (arena movies)
  double ent_cue = 0, ent_t = 68;  // Session 97: the entrance light key (entrance_light.h): cue 0/5/10/15/20, time in motion frames; default = the swap 6728 match
  bool ent_light = true;           // {"light":false}: keep the arena's own lighting in the entrance look
  int tron = 100;   // which titantron feeds the big screen / banner ring
  bool off = false; // {"cmd":"stage","media":false}: untextured as before
  std::string keys; // A/B: the stage leaf keys to draw, "802,804,805,807,809" (empty = the default set, StageKeys())
  // A/B overrides, {"cmd":"stage","map":{"ppv_header&wall":"1280x814:MOVL_M005@0,MOVN_M005@282"}}: sampler name -> spec
  // "[WxH:]layer[@y][,layer[@y]...]", layer = "tron" (the sheet), "tron_big" (rows 0-223) or an evtmov file stem (GROUP_Mnnn:
  // its _end.png else .png; "..._first" forces the first picture), drawn over black at row y.
  std::map<std::string, std::string> map;
};
inline Settings& Cfg() {
  static Settings s;
  return s;
}

namespace fs = std::filesystem;

// The stage leaves of bg05 drawn for the stage: entrance look = the SS set 802 (wall), 804 (big screen, v 0-0.699 = the picture rows
// of the tron sheet; 803 is the same quad over the WHOLE sheet, strips included), 805, 807, 809; match look = the PPV set 801, 800
// (video quad: a live camera feed, drawn dark), 805, 806, 808. Settings::keys overrides it (A/B).
inline std::vector<uint32_t> StageKeys() {
  std::vector<uint32_t> k;
  const std::string& s = Cfg().keys;
  for (size_t p = 0; p < s.size();) {
    const long v = std::strtol(s.c_str() + p, nullptr, 10);
    if (v >= 800 && v <= 809) k.push_back(uint32_t(v));
    const size_t c = s.find(',', p);
    if (c == std::string::npos) break;
    p = c + 1;
  }
  // Defaults (S97 A/B vs the game's entrance draw list, swap 6728: the SS set matches the picture; 22453 = the PPV set + the video quad 800):
  if (k.empty()) k = Cfg().wrestler >= 0 ? std::vector<uint32_t>{802, 804, 805, 807, 809} : std::vector<uint32_t>{801, 800, 805, 806, 808};
  return k;
}

struct Image {
  int w = 0, h = 0;
  std::vector<uint8_t> rgba;  // straight alpha
  bool ok() const { return w > 0 && h > 0 && rgba.size() == size_t(w) * h * 4; }
};

inline Image LoadPng(const fs::path& p) {
  Image im;
  std::ifstream f(p, std::ios::binary);
  if (!f) return im;
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  int w = 0, h = 0, n = 0;
  uint8_t* d = stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &n, 4);
  if (!d) return im;
  im.w = w, im.h = h;
  im.rgba.assign(d, d + size_t(w) * h * 4);
  stbi_image_free(d);
  return im;
}

inline fs::path DefaultDir() {
  if (!Cfg().dir.empty()) return fs::u8path(Cfg().dir);
  if (!Cfg().default_dir.empty()) {
    std::error_code ec;
    const fs::path d = fs::u8path(Cfg().default_dir);
    if (fs::is_directory(d, ec)) return d;
  }
  return {};
}

// Composites straight-alpha RGBA over black (the header art is alpha art: the wall behind it is dark).
inline void OverBlack(Image& im) {
  for (size_t i = 0; i < im.rgba.size(); i += 4) {
    const uint32_t a = im.rgba[i + 3];
    im.rgba[i] = uint8_t(im.rgba[i] * a / 255), im.rgba[i + 1] = uint8_t(im.rgba[i + 1] * a / 255),
    im.rgba[i + 2] = uint8_t(im.rgba[i + 2] * a / 255), im.rgba[i + 3] = 255;
  }
}

// One evtmov movie of the arena: the settled picture (<..>_end.png) for alpha ones, else the first.
inline Image Movie(const fs::path& dir, const std::string& group, int key) {
  char nm[64];
  std::snprintf(nm, sizeof nm, "%s_M%03d", group.c_str(), key);
  Image im = LoadPng(dir / "evtmov" / (std::string(nm) + "_end.png"));
  if (!im.ok()) im = LoadPng(dir / "evtmov" / (std::string(nm) + ".png"));
  if (im.ok()) OverBlack(im);
  return im;
}

inline void Blit(Image& dst, const Image& src, int y0) {
  for (int y = 0; y < src.h && y0 + y < dst.h; ++y)
    std::copy_n(&src.rgba[size_t(y) * src.w * 4], size_t(std::min(src.w, dst.w)) * 4, &dst.rgba[size_t(y0 + y) * dst.w * 4]);
}

// Builds a texture from an override spec (see Settings::map). Empty image when no layer was found.
inline Image BuildSpec(const fs::path& dir, const std::string& spec_in, const Image& sheet) {
  std::string spec = spec_in;
  int W = 0, H = 0;
  const size_t colon = spec.find(':');
  if (colon != std::string::npos) {
    std::sscanf(spec.c_str(), "%dx%d", &W, &H);
    spec = spec.substr(colon + 1);
  }
  struct L { Image im; int y; };
  std::vector<L> layers;
  size_t pos = 0;
  while (pos <= spec.size()) {
    size_t c = spec.find(',', pos);
    if (c == std::string::npos) c = spec.size();
    std::string tok = spec.substr(pos, c - pos);
    pos = c + 1;
    if (tok.empty()) continue;
    int y = 0;
    if (const size_t at = tok.find('@'); at != std::string::npos) y = std::atoi(tok.c_str() + at + 1), tok.resize(at);
    Image im;
    if (tok.size() == 7 && tok[0] == '#') {  // debug: a solid colour #rrggbb
      im.w = im.h = 16;
      const unsigned c = unsigned(std::strtoul(tok.c_str() + 1, nullptr, 16));
      for (int i = 0; i < 256; ++i) im.rgba.insert(im.rgba.end(), {uint8_t(c >> 16), uint8_t(c >> 8), uint8_t(c), 255});
    } else if (tok == "tron" || tok == "tron_big") {
      im = sheet;
      if (tok == "tron_big" && im.ok() && im.w == 320 && im.h == 320) im.h = 224, im.rgba.resize(size_t(320) * 224 * 4);
    } else {
      const bool first_only = tok.size() > 6 && tok.compare(tok.size() - 6, 6, "_first") == 0;
      if (first_only) tok.resize(tok.size() - 6);
      if (!first_only) im = LoadPng(dir / "evtmov" / (tok + "_end.png"));
      if (!im.ok()) im = LoadPng(dir / "evtmov" / (tok + ".png"));
      if (im.ok()) OverBlack(im);
    }
    if (im.ok()) layers.push_back({std::move(im), y});
  }
  Image out;
  if (layers.empty()) return out;
  if (W <= 0 || H <= 0) {
    W = 0, H = 0;
    for (const L& l : layers) W = std::max(W, l.im.w), H = std::max(H, l.y + l.im.h);
  }
  out.w = W, out.h = H;
  out.rgba.assign(size_t(W) * H * 4, 0);
  for (size_t i = 3; i < out.rgba.size(); i += 4) out.rgba[i] = 255;
  for (const L& l : layers) Blit(out, l.im, l.y);
  return out;
}

inline int AddTexture(Model& m, const std::string& name, const Image& im) {
  Texture t;
  t.name = name, t.format = TexFormat::kR8G8B8A8, t.width = uint32_t(im.w), t.height = uint32_t(im.h), t.mips = 1;
  t.data = im.rgba;
  // Session 98: the viewer's tron video rewrites these stage slots every frame (view\video_player.h, viewer_main.cpp TronUpdate)
  t.live = name == "main&banner" || name == "titantron_movie" || name == "ppv_header&wall" || name == "ss_header&wall" || name == "mini_tron";
  m.textures.push_back(std::move(t));
  return int(m.textures.size()) - 1;
}

// Fills every material of `m` whose sampler name is one of the stage names and has no texture of its own.
// Returns a one-line note for the model's warnings ("" when nothing applied).
inline std::string Apply(Model& m, int arena) {
  const Settings& cfg = Cfg();
  if (cfg.off) return "";
  fs::path dir = DefaultDir();
  // Session 98 (mod workbench): the entrance look of a wrestler whose tron is in the mods folder (toggle on) needs no game cache.
  const fs::path mod_file = cfg.wrestler >= 0 ? mods_lookup::TronFor(cfg.wrestler) : fs::path();
  if (dir.empty() && !mod_file.empty())
    dir = mods_lookup::Plat().cache_dir / "no_game_media";  // (nothing there: only the mod's sheet is used)
  if (dir.empty()) {  // no media folder: the match look's big screen (a live camera feed) is still drawn dark, never white
    if (cfg.wrestler >= 0) return "";
    Image bk;
    bk.w = bk.h = 8;
    bk.rgba.assign(size_t(8) * 8 * 4, 0);
    for (size_t i = 3; i < bk.rgba.size(); i += 4) bk.rgba[i] = 255;
    int tex = -1;
    for (Material& mt : m.materials)
      if (mt.diffuse < 0 && mt.diffuse_name == "titantron_movie") {
        if (tex < 0) tex = AddTexture(m, "titantron_movie", bk);
        mt.diffuse = tex;
      }
    return "";
  }
  bool wants = false;
  for (const Material& mt : m.materials)
    if (mt.diffuse < 0 && !mt.diffuse_name.empty()) wants = true;
  if (!wants) return "";
  std::error_code ec;
  // The tron: the configured one, else the lowest NNN.png in the folder.
  int tron = cfg.wrestler >= 0 ? cfg.wrestler : cfg.tron;
  Image sheet = LoadPng(dir / (std::to_string(tron) + ".png"));
  {  // Session 98: the mods folder first, while the toggle is on: the game's sheet layout with the mod's video (mods_lookup.h ModSheet)
    mods_lookup::Frame orig;
    if (sheet.ok()) orig.w = sheet.w, orig.h = sheet.h, orig.rgba = sheet.rgba;
    mods_lookup::Frame ms = mods_lookup::ModSheet(tron, &orig);
    if (ms.ok()) sheet.w = ms.w, sheet.h = ms.h, sheet.rgba = std::move(ms.rgba);
  }
  if (!sheet.ok()) {
    int best = -1;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
      const std::string stem = e.path().stem().string();
      if (e.path().extension() == ".png" && !stem.empty() && std::all_of(stem.begin(), stem.end(), ::isdigit)) {
        const int v = std::atoi(stem.c_str());
        if (best < 0 || v < best) best = v;
      }
    }
    if (best >= 0) tron = best, sheet = LoadPng(dir / (std::to_string(best) + ".png"));
  }
  std::map<std::string, int> made;  // sampler name -> texture index (-2 = tried, nothing)
  auto build = [&](const std::string& name) -> int {
    if (auto ov = cfg.map.find(name); ov != cfg.map.end()) {
      const Image im = BuildSpec(dir, ov->second, sheet);
      return im.ok() ? AddTexture(m, name, im) : -2;
    }
    // Session 97 mapping (run 6812's real frames; sub_8232B608: panels = MVSW/wrestler else MOV/arena, wall = MVSB/wrestler
    // else MVAB/arena). The 256x256 panel sheet IS the `ppv_header&wall` / `ss_header&wall` texture, used by UV as stored:
    // v 0-0.344 header band (806), 0.347-0.871 pillars (801), 0.879-0.995 footer (808).
    const bool entrance = cfg.wrestler >= 0;
    auto first_of = [&](const char* g1, int k1, const char* g2, int k2) {
      Image im = entrance ? Movie(dir, g1, k1) : Image();
      if (!im.ok()) im = Movie(dir, g2, k2);
      return im;
    };
    if (name == "ppv_header&wall" || name == "ss_header&wall") {
      const Image im = first_of("MVSW", cfg.wrestler, "MOV", arena);
      return im.ok() ? AddTexture(m, name, im) : -2;
    }
    if (name == "mini_tron") {  // the wall under the big screen (256x128)
      const Image im = first_of("MVSB", cfg.wrestler, "MVAB", arena);
      return im.ok() ? AddTexture(m, name, im) : -2;
    }
    if (name == "titantron_movie" && !entrance) {  // the big screen in a match = the live camera feed (not a movie): dark, never white
      Image bk;
      bk.w = bk.h = 8;
      bk.rgba.assign(size_t(8) * 8 * 4, 0);
      for (size_t i = 3; i < bk.rgba.size(); i += 4) bk.rgba[i] = 255;
      return AddTexture(m, name, bk);
    }
    if (!entrance) return -2;
    if (name == "main&banner") return sheet.ok() ? AddTexture(m, name, sheet) : -2;
    if (name == "titantron_movie") {  // [U] the video texture of the PPV set's screen: the picture rows (0-223) of the sheet (S18 screen test)
      if (!sheet.ok()) return -2;
      Image pic = sheet;
      if (pic.w == 320 && pic.h == 320) pic.h = 224, pic.rgba.resize(size_t(320) * 224 * 4);
      return AddTexture(m, name, pic);
    }
    return -2;
  };
  if (std::getenv("WWE_STAGE_MEDIA_LOG"))  // debug: which mesh part asks for which sampler name
    for (const Mesh& me : m.meshes)
      if (me.material >= 0 && m.materials[size_t(me.material)].diffuse < 0 && !m.materials[size_t(me.material)].diffuse_name.empty())
        std::fprintf(stderr, "stage_media: %s (%zu verts) wants '%s'\n", me.part.c_str(), me.vertices.size(), m.materials[size_t(me.material)].diffuse_name.c_str());
  // (Session 97, Jarrod: the movie frames are laid out for the stage's own UVs: every surface takes the WHOLE frame by its
  // stored UVs; no crops or composed atlases.)
  int applied = 0;
  for (Material& mt : m.materials) {
    if (mt.diffuse >= 0 || mt.diffuse_name.empty()) continue;
    auto it = made.find(mt.diffuse_name);
    if (it == made.end()) it = made.emplace(mt.diffuse_name, build(mt.diffuse_name)).first;
    if (it->second >= 0) mt.diffuse = it->second, ++applied;
  }
  if (!applied) return "";
  {  // The game's movie surfaces are self-lit: our own flat light set (ambient 1, no lights, no stage lightmap) for the meshes that got a movie
    LightSet flat;
    flat.id = 9000;
    for (int k = 0; k < 3; ++k) flat.sky[k] = flat.ground[k] = 1.f;
    m.light_sets.push_back(flat);
    for (Mesh& me : m.meshes) {
      if (me.material < 0) continue;
      Material& mt = m.materials[size_t(me.material)];
      if (mt.diffuse_name.empty() || mt.diffuse < 0 || !made.count(mt.diffuse_name) || made[mt.diffuse_name] != mt.diffuse) continue;
      me.light_set = int(m.light_sets.size()) - 1;
      mt.has_lm_slot = false;
      // Session 98 (stage fix): the game draws every movie surface (yMovieDefault) with back faces culled (RenderDoc capture rp_ramp,
      // run 6818: events 8025-8097 = keys 802 / 804 / 805 / 807 / 809, CullMode Back). Drawn two-sided, the twisted pillars (802 / 809,
      // slats with gaps) showed their inner faces through the gaps (the pink pillar with red gaps).
      mt.cull_back = true;
      // Session 98 (spot / beard pass): the stored vertex colour IS used: the movie vertex shader (3834DE73) writes interpolator 6 =
      // (lights x c190 + sky / ground x c189, alpha c189.w x c190.w x c185.x) x the vertex colour, and the pixel shader (B5853824)
      // multiplies the video's rgb by it and outputs its alpha; the draws blend SrcAlpha / InvSrcAlpha. [C: post-VS of capture rp_stage
      // (run 6818, arena 5): keys 801 / 806 rgb 1 alpha 1; 800 rgb 0..1; 805 (314 verts, the floor copy of the screens under the stage)
      // alpha 0..1, mean 0.34, 212 vertices below 1; 808 alpha mean 0.23]. Forced to white before, the floor copies showed as a full,
      // bright second screen. A material with any vertex alpha below 1 is blended where it stands in the draw order, depth write on
      // (the game's state for these draws; moved to the sorted blended pass instead, the big screen covered the LED dot layer).
      for (const Vertex& v : me.vertices)
        if (((v.color >> 24) & 0xFFu) < 0xFFu) mt.blend_inline = true;
    }
  }
  std::string s = "stage media from " + dir.string() + " (tron " + std::to_string(tron) + "):";
  for (const auto& [n, i] : made) s += " " + n + (i >= 0 ? "=ok" : "=missing");
  return s;
}

}  // namespace stage_media
}  // namespace assets
