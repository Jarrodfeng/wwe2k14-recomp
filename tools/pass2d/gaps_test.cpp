// Session 101 lane 23: the 2D pass's gap tests (tools\pass2d\selftest.py runs it; build: tools\pass2d\build.ps1). Five scenarios, each drawn through
// src\renderer headless at 1280x720, 2560x1440 and 3440x1440, each written as a PNG and a layout JSON that selftest.py checks against INDEPENDENT readings
// (the DDS bytes of the pac, the .yfb advance table, closed-form blend and YUV formulas):
//   a4     a real A4R4G4B4 texture from the player's files, drawn 1:1 opaque (item 1: the channel order)
//   blend  five solid sprites over a known background: straight, additive, opaque, premultiplied, premultiplied faded (item 4)
//   gpu    a sprite that shows a surface the scene made (a 4-colour picture drawn by this pass first), with a stale file texture on the same sprite (item 2)
//   movie  a synthetic YUV pattern as a full-screen backdrop movie and an alpha movie on a sprite's rect (item 3; no game video)
//   text   wrapped text (a normal break, a word wider than the line, a centred one) and a clip box (item 5)
// Everything it writes holds game pictures (the A4 texture): it goes under out\ (CLAUDE.md rule 1), never into the repo.
//
//   pass2d_gaps_test.exe --game DIR --out DIR [--validation N] [--a4 "pac\cab\CAB.pac|p144/0:Mask1"] [--base DIR] [--fault NAME]
// Faults (each a planted defect the selftest's checks must catch; exit codes as pass2d_test): a4_raw (the file's 16-bit words uploaded as they are),
// gpu_as_file (the sprite loses its target and draws its file texture), yuv_swap (cR and cB exchanged), blend_wrong (additive drawn as straight),
// wrap_off, clip_off, advance_w (the old pen rule: advance = w); bad_role (a sprite shows a Present surface: exit 5, the role named), no_planes (the movie planes are not available: exit 5).
// Exit codes: 0 ok; 2 could not start; 3 an asset failed to load; 4 validation errors; 5 the pass reported problems (printed); 6 could not write.
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../../src/renderer/frame_builder.h"
#include "../../src/renderer/player/png.h"
#include "../../src/renderer/renderer.h"
#include "../../src/renderer_scene/pass2d.h"
#include "../../src/renderer_scene/ui_assets.h"

using namespace rdr_scene;
namespace fs = std::filesystem;

namespace {

struct StringBlob {
  std::vector<char> bytes;
  uint32_t Add(const std::string& s) {
    const uint32_t field = uint32_t(bytes.size()) + 1;
    bytes.insert(bytes.end(), s.begin(), s.end());
    bytes.push_back(0);
    return field;
  }
};

struct Resources : Pass2dResources {
  std::vector<Pass2dTexture> textures;  // by asset index
  Pass2dFont font;
  std::map<uint32_t, Pass2dTexture> targets;
  std::map<uint32_t, Pass2dMoviePlanes> movies;
  bool no_planes = false;
  const Pass2dTexture* Texture(uint32_t asset) override { return asset < textures.size() ? &textures[asset] : nullptr; }
  const Pass2dFont* Font(uint32_t asset) override { return asset == 1000 ? &font : nullptr; }
  const std::string* StringById(uint32_t) override { return nullptr; }
  const Pass2dTexture* Target(uint32_t index) override {
    auto it = targets.find(index);
    return it == targets.end() ? nullptr : &it->second;
  }
  bool MoviePlanes(uint32_t index, Pass2dMoviePlanes& out) override {
    if (no_planes) return false;
    auto it = movies.find(index);
    if (it == movies.end()) return false;
    return out = it->second, true;
  }
};

std::string Esc(const std::string& s) {
  std::string o;
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') o += '\\', o += char(c);
    else if (c == '\n') o += "\\n";
    else if (c < 32) o += '?';
    else o += char(c);
  }
  return o;
}

int Fail(int code, const std::string& what) {
  std::fprintf(stderr, "[gaps] FAIL: %s\n", what.c_str());
  return code;
}

// ------------------------------------------------------------------------------------------------ synthetic images
UiImage Solid(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  UiImage im;
  im.width = im.height = 4, im.mips = 1, im.format = rdr::Format::RGBA8;
  for (int i = 0; i < 16; ++i) im.data.insert(im.data.end(), {r, g, b, a});
  return im;
}

// A single-channel plane w x h; value(x, y) gives each texel.
template <class F>
UiImage Plane(uint32_t w, uint32_t h, F value) {
  UiImage im;
  im.width = w, im.height = h, im.mips = 1, im.format = rdr::Format::R8;
  im.data.resize(size_t(w) * h);
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x) im.data[size_t(y) * w + x] = value(x, y);
  return im;
}

// The inverse of ui_assets.cpp's Repack4444: the file's own A4R4G4B4 words, as a wrong loader would upload them (planted fault a4_raw).
void UnrepackTo4444Raw(std::vector<uint8_t>& data) {
  for (size_t i = 0; i + 1 < data.size(); i += 2) {
    const uint16_t o = uint16_t(data[i] | (data[i + 1] << 8));
    const uint16_t b = o >> 12, g = (o >> 8) & 0xF, r = (o >> 4) & 0xF, a = o & 0xF;
    const uint16_t w = uint16_t((a << 12) | (r << 8) | (g << 4) | b);
    data[i] = uint8_t(w), data[i + 1] = uint8_t(w >> 8);
  }
}

// The test movie: four (Y, Cb, Cr) quadrants. Index: 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right. selftest.py holds the same table.
constexpr uint8_t kQuad[4][3] = {{81, 90, 240}, {145, 54, 34}, {41, 240, 110}, {235, 128, 128}};
int Quadrant(uint32_t x, uint32_t y, uint32_t w, uint32_t h) { return (y >= h / 2 ? 2 : 0) + (x >= w / 2 ? 1 : 0); }

// ------------------------------------------------------------------------------------------------ one drawn scenario
struct Scene {
  StringBlob blob;
  std::vector<Sprite> sprites;
  std::vector<Text> texts;
  std::vector<Pass2dTextExtra> extra;
  std::vector<Target> targets;
  std::vector<Movie> movies;
  float clear[4] = {0.05f, 0.05f, 0.08f, 1.0f};
  bool picture = false;  // the gpu scenario: draw the 128x128 picture first
  Pass2dScene View(bool with_extra = true) const {
    Pass2dScene s;
    s.strings = blob.bytes.data(), s.strings_bytes = blob.bytes.size();
    s.sprites = sprites.data(), s.sprite_count = sprites.size();
    s.texts = texts.data(), s.text_count = texts.size();
    s.targets = targets.data(), s.target_count = targets.size();
    s.movies = movies.data(), s.movie_count = movies.size();
    s.text_extra = with_extra && !extra.empty() ? extra.data() : nullptr;
    s.text_extra_count = with_extra ? extra.size() : 0;
    return s;
  }
};

Sprite MakeSprite(const char* widget, StringBlob& blob, uint32_t texture, Anchor anchor, float x, float y, float w, float h, uint16_t order, const float color[4], uint8_t blend) {
  Sprite s;
  s.texture = texture, s.widget = blob.Add(widget);
  s.rect[0] = x, s.rect[1] = y, s.rect[2] = w, s.rect[3] = h;
  std::memcpy(s.color, color, sizeof(s.color));
  s.order = order, s.anchor = uint8_t(anchor), s.blend = blend;
  return s;
}

void WriteJson(const fs::path& path, const std::string& scenario, uint32_t w, uint32_t h, const Pass2dScene& scene, const Pass2dReport& report, const StringBlob& blob) {
  FILE* f = std::fopen(path.string().c_str(), "wb");
  if (!f) return;
  std::fprintf(f, "{\"scenario\":\"%s\",\"width\":%u,\"height\":%u,\"scale\":%.6f,\"draws\":%u,\n\"problems\":[", scenario.c_str(), w, h, report.scale, report.draws);
  bool first = true;
  for (const std::string& p : report.problems) std::fprintf(f, "%s\"%s\"", first ? "" : ",", Esc(p).c_str()), first = false;
  std::fprintf(f, "],\n\"sprites\":[\n");
  first = true;
  for (const Pass2dBox& b : report.boxes) {
    if (b.kind != Pass2dBox::kSprite) continue;
    const Sprite& s = scene.sprites[b.item];
    const char* widget = s.widget ? blob.bytes.data() + (s.widget - 1) : "";
    std::fprintf(f, "%s{\"item\":%u,\"widget\":\"%s\",\"source\":%u,\"source_index\":%u,\"rect\":[%.4f,%.4f,%.4f,%.4f]}", first ? "" : ",\n", b.item, Esc(widget).c_str(), unsigned(b.source), b.source_index, b.x, b.y, b.w, b.h);
    first = false;
  }
  std::fprintf(f, "],\n\"movies\":[\n");
  first = true;
  for (const Pass2dBox& b : report.boxes) {
    if (b.kind != Pass2dBox::kMovie) continue;
    std::fprintf(f, "%s{\"item\":%u,\"rect\":[%.4f,%.4f,%.4f,%.4f]}", first ? "" : ",\n", b.item, b.x, b.y, b.w, b.h);
    first = false;
  }
  std::fprintf(f, "],\n\"texts\":[\n");
  first = true;
  for (const Pass2dTextInfo& t : report.texts) {
    std::fprintf(f, "%s{\"item\":%u,\"utf8\":\"%s\",\"size_px\":%.4f,\"scale\":%.6f,\"anchor_x\":%.4f,\"anchor_y\":%.4f,\"align\":%u,\"lines\":%u,\"wrap_px\":%.4f,\"has_clip\":%d,\"clip\":[%d,%d,%d,%d],\"glyphs\":[",
                 first ? "" : ",\n", t.item, Esc(t.utf8).c_str(), t.size_px, t.scale, t.anchor_x, t.anchor_y, t.align, t.lines, t.wrap_px, t.has_clip ? 1 : 0, t.clip_px[0], t.clip_px[1], t.clip_px[2], t.clip_px[3]);
    first = false;
    bool gfirst = true;
    for (const Pass2dBox& b : report.boxes) {
      if (b.kind != Pass2dBox::kGlyph || b.item != t.item) continue;
      std::fprintf(f, "%s[%u,%u,%.4f,%.4f,%.4f,%.4f]", gfirst ? "" : ",", b.codepoint, b.line, b.x, b.y, b.w, b.h);
      gfirst = false;
    }
    std::fprintf(f, "]}");
  }
  std::fprintf(f, "]}\n");
  std::fclose(f);
}

std::vector<uint8_t> Resize(const std::vector<uint8_t>& src, uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh) {
  std::vector<uint8_t> out(size_t(dw) * dh * 4);
  const double fx = double(sw) / dw, fy = double(sh) / dh;
  for (uint32_t y = 0; y < dh; ++y)
    for (uint32_t x = 0; x < dw; ++x) {
      const uint32_t x0 = uint32_t(x * fx), x1 = std::max(x0 + 1, std::min(sw, uint32_t((x + 1) * fx + 0.999)));
      const uint32_t y0 = uint32_t(y * fy), y1 = std::max(y0 + 1, std::min(sh, uint32_t((y + 1) * fy + 0.999)));
      double acc[4] = {};
      for (uint32_t yy = y0; yy < y1; ++yy)
        for (uint32_t xx = x0; xx < x1; ++xx)
          for (int c = 0; c < 4; ++c) acc[c] += src[(size_t(yy) * sw + xx) * 4 + size_t(c)];
      const double n = double(x1 - x0) * double(y1 - y0);
      for (int c = 0; c < 4; ++c) out[(size_t(y) * dw + x) * 4 + size_t(c)] = uint8_t(acc[c] / n + 0.5);
    }
  return out;
}

struct Picture {
  std::string label;
  std::vector<uint8_t> rgba;
  uint32_t w = 0, h = 0;
};

}  // namespace

int main(int argc, char** argv) {
  std::string game, out_dir, fault, a4_spec = "pac\\cab\\CAB.pac|p144/0:Mask1", base_dir, only;
  int validation = 1;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--game" && i + 1 < argc) game = argv[++i];
    else if (a == "--out" && i + 1 < argc) out_dir = argv[++i];
    else if (a == "--validation" && i + 1 < argc) validation = std::atoi(argv[++i]);
    else if (a == "--fault" && i + 1 < argc) fault = argv[++i];
    else if (a == "--a4" && i + 1 < argc) a4_spec = argv[++i];
    else if (a == "--base" && i + 1 < argc) base_dir = argv[++i];
    else if (a == "--only" && i + 1 < argc) only = argv[++i];  // one scenario (the fault runs); no contact sheet
  }
  if (game.empty() || out_dir.empty()) return Fail(2, "usage: pass2d_gaps_test --game DIR --out DIR [--validation N] [--a4 PAC|ENTRY] [--base DIR] [--fault NAME]");
  fs::create_directories(out_dir);

  // ------------------------------------------------------------------------------------------------ the files
  UiAssets files(game);
  std::string err;
  UiFont font;
  if (!files.Font(font, err)) return Fail(3, "font: " + err);
  if (fault == "advance_w") font.advance_trim = 0.0f, font.kerns.clear();  // S101 U1: the old pen rule (advance = w), planted
  const size_t bar = a4_spec.find('|');
  if (bar == std::string::npos) return Fail(2, "--a4 wants PAC|ENTRY");
  UiImage a4;
  if (!files.Texture(a4_spec.substr(0, bar), a4_spec.substr(bar + 1), a4, err)) return Fail(3, "a4 texture " + a4_spec + ": " + err);
  if (a4.format != rdr::Format::RGBA4) return Fail(3, "the a4 texture did not come back as RGBA4");
  if (fault == "a4_raw") UnrepackTo4444Raw(a4.data);

  // ------------------------------------------------------------------------------------------------ the renderer and the synthetic resources
  rdr::RendererOptions options;
  options.validation = validation;
  options.vsync = false;
  std::string rerr;
  std::unique_ptr<rdr::Renderer> renderer = rdr::Renderer::Create(options, rerr);
  if (!renderer) return Fail(2, "renderer: " + rerr);
  Pass2d pass(*renderer);
  Resources res;
  res.no_planes = fault == "no_planes";
  auto upload = [&](const UiImage& im, const char* what) -> Pass2dTexture {
    Pass2dTexture t = pass.Upload(im);
    if (t.image == rdr::kNoImage) std::fprintf(stderr, "[gaps] could not upload %s\n", what);
    return t;
  };
  res.textures.push_back(upload(Solid(255, 0, 255, 255), "stale"));      // 0: the stale file texture (magenta): what a GPU picture read as a file looks like
  res.textures.push_back(upload(Solid(255, 255, 255, 255), "white"));    // 1
  res.textures.push_back(upload(Solid(128, 0, 0, 128), "premult red"));  // 2: red already multiplied by its 50 % alpha
  res.textures.push_back(upload(a4, "a4"));                              // 3: the real A4R4G4B4 texture
  res.font = pass.Upload(font);
  for (const Pass2dTexture& t : res.textures)
    if (t.image == rdr::kNoImage) return Fail(3, "an upload failed");
  if (res.font.sheet.image == rdr::kNoImage) return Fail(3, "could not upload the font sheet");

  // movie planes: 64 x 36 luma, 32 x 18 chroma, four quadrants (kQuad); an alpha movie: constant red, alpha 255 on the left half and 0 on the right
  auto make_planes = [&](bool constant_red, bool alpha, bool swap) {
    Pass2dMoviePlanes p;
    // PADDED planes, like the game's (Y 1280 x 280 with chroma 640 x 144 in a capture): the picture is the top 64 x 36 (luma) / 32 x 18 (chroma) of a taller
    // image whose padding rows hold garbage (bright luma, zero chroma / alpha). A pass that maps uv 0..1 onto the whole image shows the garbage.
    const uint32_t yw = 64, yh = 36, cw = 32, ch = 18, ypad = 48, cpad = 24;
    auto quad_of = [&](uint32_t x, uint32_t y, uint32_t w, uint32_t h) { return constant_red ? 0 : Quadrant(x, y, w, h); };
    p.y = upload(Plane(yw, ypad, [&](uint32_t x, uint32_t y) { return y >= yh ? uint8_t(255) : kQuad[quad_of(x, y, yw, yh)][0]; }), "Y");
    p.cb = upload(Plane(cw, cpad, [&](uint32_t x, uint32_t y) { return y >= ch ? uint8_t(0) : kQuad[quad_of(x, y, cw, ch)][1]; }), "Cb");
    p.cr = upload(Plane(cw, cpad, [&](uint32_t x, uint32_t y) { return y >= ch ? uint8_t(0) : kQuad[quad_of(x, y, cw, ch)][2]; }), "Cr");
    if (alpha) p.alpha = upload(Plane(yw, ypad, [&](uint32_t x, uint32_t y) { return uint8_t(y < yh && x < yw / 2 ? 255 : 0); }), "A");
    if (fault != "pad_ignored") {
      p.valid_y[0] = yw, p.valid_y[1] = yh, p.valid_cb[0] = p.valid_cr[0] = cw, p.valid_cb[1] = p.valid_cr[1] = ch;
      p.valid_alpha[0] = yw, p.valid_alpha[1] = yh;
    }
    if (swap) std::swap(p.cb, p.cr), std::swap(p.valid_cb, p.valid_cr);  // (arrays: element-wise below)
    return p;
  };
  res.movies[0] = make_planes(false, false, fault == "yuv_swap");
  res.movies[1] = make_planes(true, true, false);
  const rdr::ImageDesc picture_desc = [] {
    rdr::ImageDesc d;
    d.width = d.height = 128, d.format = rdr::Format::RGBA8, d.usage = rdr::kUsageTarget | rdr::kUsageSampled;
    return d;
  }();
  const rdr::ImageId picture_image = renderer->CreateImage(picture_desc);
  if (picture_image == rdr::kNoImage) return Fail(3, "could not create the picture surface");
  res.targets[0] = {picture_image, 128, 128};

  // ------------------------------------------------------------------------------------------------ the five scenarios (layout units, 1280 x 720)
  const float white[4] = {1, 1, 1, 1}, red[4] = {1, 0, 0, 1}, green[4] = {0, 1, 0, 1}, blue[4] = {0, 0, 1, 1}, yellow[4] = {1, 1, 0, 1};
  std::map<std::string, Scene> scenes;
  {  // a4: the texture 1:1 at the top left, opaque
    Scene& s = scenes["a4"];
    s.sprites.push_back(MakeSprite("a4", s.blob, 3, Anchor::TopLeft, 0, 0, float(a4.width), float(a4.height), 1, white, 2));
    s.clear[0] = 0.0f, s.clear[1] = 0.0f, s.clear[2] = 0.0f;
  }
  {  // blend: six solid sprites, 100 x 100 units, over the background (0.2, 0.4, 0.6)
    Scene& s = scenes["blend"];
    s.clear[0] = 0.2f, s.clear[1] = 0.4f, s.clear[2] = 0.6f;
    const float half_red[4] = {1, 0, 0, 0.5f}, half_white[4] = {1, 1, 1, 0.5f};
    const uint8_t wrong = fault == "blend_wrong" ? 0 : 1;
    s.sprites.push_back(MakeSprite("straight", s.blob, 1, Anchor::TopLeft, 50, 100, 100, 100, 1, half_red, 0));
    s.sprites.push_back(MakeSprite("additive", s.blob, 1, Anchor::TopLeft, 200, 100, 100, 100, 1, half_red, wrong));
    s.sprites.push_back(MakeSprite("opaque", s.blob, 1, Anchor::TopLeft, 350, 100, 100, 100, 1, half_red, 2));
    s.sprites.push_back(MakeSprite("premult", s.blob, 2, Anchor::TopLeft, 500, 100, 100, 100, 1, white, 3));
    s.sprites.push_back(MakeSprite("premult_faded", s.blob, 2, Anchor::TopLeft, 650, 100, 100, 100, 1, half_white, 3));
    s.sprites.push_back(MakeSprite("premult_as_straight", s.blob, 2, Anchor::TopLeft, 800, 100, 100, 100, 1, white, 0));
  }
  {  // gpu: a sprite with a file texture (stale) AND a target (the picture); and a sprite that shows the same picture from a right-anchored place
    Scene& s = scenes["gpu"];
    s.picture = true;
    s.clear[0] = 0.2f, s.clear[1] = 0.4f, s.clear[2] = 0.6f;
    Target t;
    t.role = fault == "bad_role" ? rdr_adapter::SurfaceRole::Present : rdr_adapter::SurfaceRole::MenuPicture;
    t.width = t.height = 128;
    s.targets.push_back(t);
    Sprite a = MakeSprite("picture", s.blob, 0, Anchor::TopLeft, 100, 100, 256, 256, 1, white, 2);
    if (fault != "gpu_as_file") a.target = 0;
    s.sprites.push_back(a);
    Sprite b = MakeSprite("picture_right", s.blob, 0, Anchor::Right, 1280 - 100 - 128, 100, 128, 128, 1, white, 2);
    if (fault != "gpu_as_file") b.target = 0;
    s.sprites.push_back(b);
    if (fault == "self_sample") {  // a Ui-role target that IS the image this pass draws into (set per frame below): the pass must refuse it
      Target self;
      self.role = rdr_adapter::SurfaceRole::Ui;
      s.targets.push_back(self);
      Sprite c = MakeSprite("self", s.blob, 0, Anchor::TopLeft, 600, 400, 100, 100, 1, white, 2);
      c.target = 1;
      s.sprites.push_back(c);
    }
  }
  {  // movie: backdrop + alpha movie on a sprite (a host with no picture of its own)
    Scene& s = scenes["movie"];
    s.clear[0] = 0.2f, s.clear[1] = 0.4f, s.clear[2] = 0.6f;
    Movie full;
    full.asset = 0, full.show = uint8_t(MovieShow::FullScreen), full.has_alpha = 0;
    s.movies.push_back(full);
    Movie host;
    host.asset = 1, host.show = uint8_t(MovieShow::OnSprite), host.has_alpha = 1, host.target = 0;
    s.movies.push_back(host);
    // The host is turned half a turn about its centre (the rect stays put, the picture is mirrored both ways: the opaque half of the alpha plane lands on the right).
    Sprite hs = MakeSprite("movie_host", s.blob, 0xFFFFFFFFu, Anchor::TopLeft, 900, 40, 320, 180, 1, white, 0);
    hs.rotation = 3.14159265358979f;
    s.sprites.push_back(hs);
    // A sprite listed AFTER the host at the same order is drawn over the movie (record order): an opaque blue square on the host's right half.
    const float blue_tint[4] = {0, 0, 1, 1};
    s.sprites.push_back(MakeSprite("over_movie", s.blob, 1, Anchor::TopLeft, 1100, 100, 60, 60, 1, blue_tint, 2));
  }
  {  // text: wrapped, a word wider than the line, centred, a clip box and its unclipped twin
    Scene& s = scenes["text"];
    auto text = [&](const char* widget, const std::string& utf8, Anchor anchor, uint8_t align, float x, float y, float size, float wrap, const float* clip) {
      Text t;
      t.utf8 = s.blob.Add(utf8), t.font = 1000, t.widget = s.blob.Add(widget);
      t.pos[0] = x, t.pos[1] = y, t.size = size;
      std::memcpy(t.color, white, sizeof(t.color));
      t.order = 4, t.anchor = uint8_t(anchor), t.align = align;
      s.texts.push_back(t);
      Pass2dTextExtra e;
      e.wrap_width = fault == "wrap_off" ? 0.0f : wrap;
      if (clip && fault != "clip_off") e.has_clip = true, std::memcpy(e.clip, clip, sizeof(e.clip));
      s.extra.push_back(e);
    };
    const float clip_box[4] = {60, 455, 300, 20};
    text("wrap", "The quick brown fox jumps over the lazy dog and keeps on running far beyond the edge of the field.", Anchor::Left, 0, 60, 40, 24, 500, nullptr);
    text("long_word", "SUPERCALIFRAGILISTICEXPIALIDOCIOUS and short words", Anchor::Left, 0, 60, 300, 24, 260, nullptr);
    text("clipped", "CLIPPED TEXT RUNS ON AND ON PAST THE BOX EDGE", Anchor::Left, 0, 60, 450, 36, 0, clip_box);
    text("unclipped", "CLIPPED TEXT RUNS ON AND ON PAST THE BOX EDGE", Anchor::Left, 0, 60, 560, 36, 0, nullptr);
    text("centred", "alpha beta gamma delta epsilon zeta eta theta", Anchor::Center, 1, 640, 620, 24, 300, nullptr);
    {  // an indented word that fits a line alone must not be cut: the limit is the word + two spaces, the indent is four spaces
      // the pass's pen rule (S101 U1): each glyph advances w - trim - kern(this, next); the word, then two spaces, as one logical line
      const char* line = "ABCDEFGHIJKLM  ";
      float run = 0;
      for (const char* c = line; *c; ++c)
        run += float(font.glyphs.at(uint32_t(uint8_t(*c))).w) - font.advance_trim - (c[1] ? float(font.Kern(uint32_t(uint8_t(*c)), uint32_t(uint8_t(c[1])))) : 0.0f);
      const float limit_units = run * 24.0f / font.cell;
      text("indent", "    ABCDEFGHIJKLM nopq", Anchor::Left, 0, 60, 150, 24, limit_units, nullptr);
    }
    {  // a clip box far beyond the display (and, planted, a NaN one): the text is still drawn, the scissor is the display
      const float huge[4] = {0.0f, 0.0f, 1e30f, 1e30f}, nan_box[4] = {0.0f, 0.0f, std::nanf(""), 10.0f};
      text("huge_clip", "HUGE CLIP BOX", Anchor::Left, 0, 60, 680, 24, 0, fault == "clip_nan" ? nan_box : huge);
    }
    if (fault == "extra_short") s.extra.pop_back();  // a side table one entry short: the pass must refuse it, not read past it
  }

  // ------------------------------------------------------------------------------------------------ draw: every scenario at three sizes
  const uint32_t sizes[3][2] = {{1280, 720}, {2560, 1440}, {3440, 1440}};
  const char* order[5] = {"a4", "blend", "gpu", "movie", "text"};
  rdr::FrameBuilder builder;
  int rc = 0;
  uint64_t tick = 0;
  std::vector<Picture> sheet_pictures;
  for (const char* name : order) {
    if (!only.empty() && only != name) continue;
    for (int k = 0; k < 3; ++k) {
      const uint32_t w = sizes[k][0], h = sizes[k][1];
      Scene& sc = scenes[name];
      rdr::ImageDesc desc;
      desc.width = uint16_t(w), desc.height = uint16_t(h), desc.format = rdr::Format::RGBA8;
      desc.usage = rdr::kUsageTarget | rdr::kUsageSampled;
      const rdr::ImageId target = renderer->CreateImage(desc);
      res.targets[1] = {target, w, h};  // (only the self_sample fault's sprite names target 1)
      builder.Begin(tick++);
      Pass2dReport report, picture_report;
      bool ok = true;
      if (sc.picture) {
        // the scene renderer's picture, here made by this pass: four solid quadrants, drawn into the 128 x 128 surface before the screen pass samples it
        Scene pic;
        pic.sprites.push_back(MakeSprite("tl", pic.blob, 1, Anchor::Stretch, 0, 0, 640, 360, 0, red, 2));
        pic.sprites.push_back(MakeSprite("tr", pic.blob, 1, Anchor::Stretch, 640, 0, 640, 360, 0, green, 2));
        pic.sprites.push_back(MakeSprite("bl", pic.blob, 1, Anchor::Stretch, 0, 360, 640, 360, 0, blue, 2));
        pic.sprites.push_back(MakeSprite("br", pic.blob, 1, Anchor::Stretch, 640, 360, 640, 360, 0, yellow, 2));
        const float black[4] = {0, 0, 0, 1};
        ok = pass.Draw(pic.View(), res, &builder, picture_image, 128, 128, black, picture_report) && ok;
      }
      const Pass2dScene view = sc.View();
      ok = pass.Draw(view, res, &builder, target, w, h, sc.clear, report) && ok;
      rdr::PresentOp present;
      present.source.image = target;
      present.source_rect.width = w, present.source_rect.height = h;
      builder.Present(present);
      renderer->RenderFrame(builder.Finish());
      renderer->WaitIdle();
      std::printf("[gaps] %s %ux%u: %u draws, %zu boxes, %zu problems, validation errors so far %llu\n", name, w, h, report.draws, report.boxes.size(), report.problems.size(),
                  static_cast<unsigned long long>(renderer->validation_errors()));
      for (const std::string& p : report.problems) std::fprintf(stderr, "[gaps] problem (%s): %s\n", name, p.c_str());
      for (const std::string& p : picture_report.problems) std::fprintf(stderr, "[gaps] problem (picture): %s\n", p.c_str());
      if (!ok) rc = 5;
      char file[96];
      std::snprintf(file, sizeof file, "gaps_%s_%ux%u", name, w, h);
      WriteJson(fs::path(out_dir) / (std::string(file) + ".json"), name, w, h, view, report, sc.blob);
      rdr::ImageRef ref;
      ref.image = target;
      std::vector<uint8_t> pixels;
      uint32_t pw = 0, ph = 0;
      if (!renderer->ReadImage(ref, pixels, pw, ph) || pw != w || ph != h) return Fail(6, "could not read the picture back");
      if (!rdr::WritePng((fs::path(out_dir) / (std::string(file) + ".png")).string().c_str(), pixels.data(), w, h)) return Fail(6, "could not write the PNG");
      const bool want_panel = k == 0 || (std::string(name) == "movie" && k == 2);
      if (want_panel) sheet_pictures.push_back({std::string(name) + " " + std::to_string(w) + "x" + std::to_string(h), std::move(pixels), w, h});
      renderer->DestroyImage(target);
    }
  }

  // ------------------------------------------------------------------------------------------------ the contact sheet: every panel labelled (the label is drawn by the pass itself)
  if (only.empty()) {
    std::vector<Picture> panels;
    if (!base_dir.empty()) {
      const char* base_names[3] = {"pass2d_1280x720", "pass2d_2560x1440", "pass2d_3440x1440"};
      for (const char* bn : base_names) {
        const std::string path = (fs::path(base_dir) / (std::string(bn) + ".png")).string();
        int bw = 0, bh = 0, comp = 0;
        uint8_t* data = stbi_load(path.c_str(), &bw, &bh, &comp, 4);
        if (!data) continue;
        Picture p;
        p.label = std::string("menu page ") + (bn + 7), p.w = uint32_t(bw), p.h = uint32_t(bh);
        p.rgba.assign(data, data + size_t(bw) * size_t(bh) * 4);
        stbi_image_free(data);
        panels.push_back(std::move(p));
      }
    }
    for (Picture& p : sheet_pictures) panels.push_back(std::move(p));
    const uint32_t cell_w = 636, cell_h = 358, label_h = 22, gap = 8, cols = 2, sheet_w = 1280;
    const uint32_t rows = uint32_t((panels.size() + cols - 1) / cols), sheet_h = rows * (cell_h + label_h + gap) + gap;
    std::vector<uint8_t> sheet(size_t(sheet_w) * sheet_h * 4, 24);
    for (size_t i = 0; i < sheet.size(); i += 4) sheet[i + 3] = 255;
    // a label strip: text drawn with the game's font into a small target by the pass
    rdr::ImageDesc ld;
    ld.width = uint16_t(cell_w), ld.height = uint16_t(label_h), ld.format = rdr::Format::RGBA8, ld.usage = rdr::kUsageTarget | rdr::kUsageSampled;
    for (size_t i = 0; i < panels.size(); ++i) {
      const uint32_t cx = uint32_t(i % cols) * (cell_w + gap + 0) + gap / 2, cy = uint32_t(i / cols) * (cell_h + label_h + gap) + gap;
      // the label
      Scene lab;
      Text t;
      t.utf8 = lab.blob.Add(panels[i].label), t.font = 1000, t.widget = lab.blob.Add("label");
      t.pos[0] = 6, t.pos[1] = 0, t.size = 700.0f * float(label_h - 4) / float(label_h) * 0.75f;  // ~ the strip's height in layout units (s = label_h / 720)
      std::memcpy(t.color, white, sizeof(t.color));
      t.order = 0, t.anchor = uint8_t(Anchor::TopLeft);
      lab.texts.push_back(t);
      const rdr::ImageId li = renderer->CreateImage(ld);
      builder.Begin(tick++);
      Pass2dReport lr;
      const float strip[4] = {0.18f, 0.18f, 0.22f, 1};
      pass.Draw(lab.View(false), res, &builder, li, cell_w, label_h, strip, lr);
      rdr::PresentOp present;
      present.source.image = li;
      present.source_rect.width = cell_w, present.source_rect.height = label_h;
      builder.Present(present);
      renderer->RenderFrame(builder.Finish());
      renderer->WaitIdle();
      rdr::ImageRef lref;
      lref.image = li;
      std::vector<uint8_t> strip_pixels;
      uint32_t lw = 0, lh = 0;
      if (renderer->ReadImage(lref, strip_pixels, lw, lh) && lw == cell_w && lh == label_h)
        for (uint32_t y = 0; y < label_h; ++y) std::memcpy(sheet.data() + (size_t(cy + y) * sheet_w + cx) * 4, strip_pixels.data() + size_t(y) * cell_w * 4, size_t(cell_w) * 4);
      renderer->DestroyImage(li);
      // the panel, scaled to fit the cell (aspect kept), centred
      const double fit = std::min(double(cell_w) / panels[i].w, double(cell_h) / panels[i].h);
      const uint32_t dw = std::max(1u, uint32_t(panels[i].w * fit)), dh = std::max(1u, uint32_t(panels[i].h * fit));
      const std::vector<uint8_t> small = Resize(panels[i].rgba, panels[i].w, panels[i].h, dw, dh);
      const uint32_t ox = cx + (cell_w - dw) / 2, oy = cy + label_h + (cell_h - dh) / 2;
      for (uint32_t y = 0; y < dh; ++y) std::memcpy(sheet.data() + (size_t(oy + y) * sheet_w + ox) * 4, small.data() + size_t(y) * dw * 4, size_t(dw) * 4);
    }
    if (!rdr::WritePng((fs::path(out_dir) / "sheet.png").string().c_str(), sheet.data(), sheet_w, sheet_h)) return Fail(6, "could not write the sheet");
  }

  const uint64_t errors = renderer->validation_errors();
  std::printf("[gaps] device %s, validation errors %llu\n", renderer->device_name(), static_cast<unsigned long long>(errors));
  if (errors) return 4;
  return rc;
}
