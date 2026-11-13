// Session 101 lane 9: the 2D pass's offline test program (tools\pass2d\selftest.py runs it; build: tools\pass2d\build.ps1).
// Builds a 2D scene record's tables from the player's own game files only (a menu background, menu sprites, strings by id in two sizes and three
// anchors), draws it through src\renderer headless at 1280x720, 2560x1440 and 3440x1440, writes a PNG and a layout JSON per size and one contact
// sheet. Everything it writes holds game pictures: it goes under out\ (CLAUDE.md rule 1), never into the repo.
//
//   pass2d_test.exe --game DIR --out DIR [--validation N] [--font 0|1|2] [--fault string|texture|glyph|advance_w|advance_c2]
// Exit codes: 0 ok; 2 could not start; 3 an asset or string failed to load (printed); 4 validation errors; 5 the pass reported problems (printed,
// a missing glyph by code point); 6 could not write.
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <algorithm>
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

// The record's string section, built as the scene record builds it: NUL-separated, a field = offset + 1.
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
  std::map<uint32_t, std::string> by_id;
  const Pass2dTexture* Texture(uint32_t asset) override { return asset < textures.size() ? &textures[asset] : nullptr; }
  const Pass2dFont* Font(uint32_t asset) override { return asset == 1000 ? &font : nullptr; }
  const std::string* StringById(uint32_t id) override {
    auto it = by_id.find(id);
    return it == by_id.end() ? nullptr : &it->second;
  }
};

std::string Esc(const std::string& s) {
  std::string o;
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') o += '\\', o += char(c);
    else if (c == '\n') o += "\\n";
    else if (c < 32) o += '?';
    else o += char(c);  // (UTF-8 bytes pass through)
  }
  return o;
}

void WriteJson(const fs::path& path, uint32_t w, uint32_t h, const Pass2dScene& scene, const Pass2dReport& report, const StringBlob& blob) {
  FILE* f = std::fopen(path.string().c_str(), "wb");
  if (!f) return;
  std::fprintf(f, "{\"width\":%u,\"height\":%u,\"scale\":%.6f,\"draws\":%u,\n\"sprites\":[\n", w, h, report.scale, report.draws);
  bool first = true;
  for (const Pass2dBox& b : report.boxes) {
    if (b.kind != Pass2dBox::kSprite) continue;
    const Sprite& s = scene.sprites[b.item];
    const char* widget = s.widget ? blob.bytes.data() + (s.widget - 1) : "";
    std::fprintf(f, "%s{\"item\":%u,\"widget\":\"%s\",\"anchor\":%u,\"rect\":[%.4f,%.4f,%.4f,%.4f],\"layout_rect\":[%.4f,%.4f,%.4f,%.4f]}", first ? "" : ",\n", b.item,
                 Esc(widget).c_str(), s.anchor, b.x, b.y, b.w, b.h, s.rect[0], s.rect[1], s.rect[2], s.rect[3]);
    first = false;
  }
  std::fprintf(f, "],\n\"texts\":[\n");
  first = true;
  for (const Pass2dTextInfo& t : report.texts) {
    std::fprintf(f, "%s{\"item\":%u,\"string_id\":%u,\"utf8\":\"%s\",\"size_px\":%.4f,\"scale\":%.6f,\"anchor_x\":%.4f,\"anchor_y\":%.4f,\"align\":%u,\"glyphs\":[", first ? "" : ",\n",
                 t.item, t.string_id, Esc(t.utf8).c_str(), t.size_px, t.scale, t.anchor_x, t.anchor_y, t.align);
    first = false;
    bool gfirst = true;
    for (const Pass2dBox& b : report.boxes) {
      if (b.kind != Pass2dBox::kGlyph || b.item != t.item) continue;
      std::fprintf(f, "%s[%u,%u,%.4f,%.4f,%.4f,%.4f]", gfirst ? "" : ",", b.codepoint, b.line, b.x, b.y, b.w, b.h);
      gfirst = false;
    }
    std::fprintf(f, "]}");
  }
  std::fprintf(f, "],\n\"missing_glyphs\":[");
  first = true;
  for (uint32_t cp : report.missing_glyphs) std::fprintf(f, "%s%u", first ? "" : ",", cp), first = false;
  std::fprintf(f, "]}\n");
  std::fclose(f);
}

// Area-average resample to dst_w wide (keeps the aspect).
std::vector<uint8_t> Resize(const std::vector<uint8_t>& src, uint32_t sw, uint32_t sh, uint32_t dw, uint32_t& dh) {
  dh = std::max(1u, uint32_t(uint64_t(sh) * dw / sw));
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

int Fail(int code, const std::string& what) {
  std::fprintf(stderr, "[pass2d] FAIL: %s\n", what.c_str());
  return code;
}

}  // namespace

int main(int argc, char** argv) {
  std::string game, out_dir, fault;
  uint32_t font_index = 0;  // S101 U1: --font N draws the page with font00N (the pen trim is per font)
  int validation = 1;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--game" && i + 1 < argc) game = argv[++i];
    else if (a == "--out" && i + 1 < argc) out_dir = argv[++i];
    else if (a == "--validation" && i + 1 < argc) validation = std::atoi(argv[++i]);
    else if (a == "--fault" && i + 1 < argc) fault = argv[++i];
    else if (a == "--font" && i + 1 < argc) font_index = uint32_t(std::atoi(argv[++i]));
  }
  if (game.empty() || out_dir.empty()) return Fail(2, "usage: pass2d_test --game DIR --out DIR [--validation N] [--font 0|1|2] [--fault string|texture|glyph|advance_w|advance_c2]");
  fs::create_directories(out_dir);

  // ------------------------------------------------------------------------------------------------ the files
  UiAssets files(game);
  std::string err;
  UiFont font;
  if (!files.Font(font, err, font_index)) return Fail(3, "font: " + err);
  if (fault == "advance_c2") font.advance_trim = 2.0f;  // S101 U1: font002 with the other fonts' trim (c = 2 instead of 7), planted
  // S101 U1 planted fault advance_w: the pass's OLD pen rule (advance = w, no trim, no kerning), the one UI_PROMOTION_MAP.md section 6 found wrong.
  if (fault == "advance_w") font.advance_trim = 0.0f, font.kerns.clear();

  const std::string menu_lay = "pac\\menu\\MnLayHD.pac", menu_hd = "pac\\menu\\menuHD.pac";
  std::vector<std::pair<std::string, std::string>> tex_specs = {
      {menu_lay, fault == "texture" ? "p16/7001:no_such_texture" : "p16/7001:bg"},  // 0: the page's background (1024 x 512)
      {menu_hd, "p2/0/5"},                                                           // 1: a 128 x 128 panel
      {menu_hd, "p1/0"},                                                             // 2: 256 x 128
      {menu_hd, "p2/0/0"},                                                           // 3: 128 x 128
  };
  for (int id : {100, 101, 102, 103, 104, 107, 109, 114}) tex_specs.push_back({menu_hd, "p0/0/" + std::to_string(id)});  // 4..11: select icons 64 x 64
  std::vector<UiImage> images(tex_specs.size());
  for (size_t i = 0; i < tex_specs.size(); ++i)
    if (!files.Texture(tex_specs[i].first, tex_specs[i].second, images[i], err)) return Fail(3, "texture " + tex_specs[i].second + ": " + err);

  std::map<uint32_t, std::string> strings;
  for (uint32_t id : {282u, 303u, 304u, 60100u, 60101u, 228u, 263u, 264u, 265u, 274u, 284u, fault == "string" ? 999999u : 0u, fault == "glyph" ? 262u : 0u}) {
    if (id == 0) continue;
    std::string s;
    if (!files.String(id, s, err)) return Fail(3, "string: " + err);
    strings[id] = s;
  }

  // ------------------------------------------------------------------------------------------------ the renderer
  rdr::RendererOptions options;
  options.validation = validation;
  options.vsync = false;
  std::string rerr;
  std::unique_ptr<rdr::Renderer> renderer = rdr::Renderer::Create(options, rerr);
  if (!renderer) return Fail(2, "renderer: " + rerr);
  Pass2d pass(*renderer);
  Resources res;
  for (const UiImage& im : images) {
    res.textures.push_back(pass.Upload(im));
    if (res.textures.back().image == rdr::kNoImage) return Fail(3, "could not upload a texture");
  }
  res.font = pass.Upload(font);
  if (res.font.sheet.image == rdr::kNoImage) return Fail(3, "could not upload the font sheet");
  res.by_id = strings;

  // ------------------------------------------------------------------------------------------------ the record's 2D tables (layout units: 1280 x 720)
  StringBlob blob;
  std::vector<Sprite> sprites;
  auto sprite = [&](const char* widget, uint32_t texture, Anchor anchor, float x, float y, float w, float h, uint16_t order, const float color[4] = nullptr, uint8_t blendm = 0) {
    Sprite s;
    s.texture = texture, s.widget = blob.Add(widget);
    s.rect[0] = x, s.rect[1] = y, s.rect[2] = w, s.rect[3] = h;
    if (color) std::memcpy(s.color, color, sizeof(s.color));
    s.order = order, s.anchor = uint8_t(anchor), s.blend = blendm;
    sprites.push_back(s);
  };
  const float scrim[4] = {0, 0, 0, 0.55f};
  sprite("background", 0, Anchor::Stretch, 0, 0, 1280, 720, 0);
  sprite("scrim", 1, Anchor::Center, 40, 90, 1200, 560, 1, scrim);
  for (int i = 0; i < 8; ++i) sprite(("icon" + std::to_string(i)).c_str(), 4 + uint32_t(i), Anchor::Left, 60.0f + 80.0f * float(i), 150, 64, 64, 2);
  sprite("right_edge", 2, Anchor::Right, 1280 - 256, 300, 256, 128, 2);           // its right edge is the layout's right edge = the screen's
  sprite("centre", 3, Anchor::Center, (1280 - 128) / 2.0f, 420, 128, 128, 2);       // centred on the layout
  sprite("top_left", 3, Anchor::TopLeft, 0, 0, 96, 96, 3);                         // the corner
  sprite("bottom_right", 3, Anchor::BottomRight, 1280 - 96, 720 - 96, 96, 96, 3);  // the opposite corner

  std::vector<Text> texts;
  auto text = [&](const char* widget, uint32_t id, Anchor anchor, uint8_t align, float x, float y, float size, const float color[4], uint16_t order = 4) {
    Text t;
    t.string_id = id, t.font = 1000, t.widget = blob.Add(widget);
    t.pos[0] = x, t.pos[1] = y, t.size = size;
    std::memcpy(t.color, color, sizeof(t.color));
    t.order = order, t.anchor = uint8_t(anchor), t.align = align;
    texts.push_back(t);
  };
  const float white[4] = {1, 1, 1, 1}, gold[4] = {1.0f, 0.82f, 0.25f, 1}, cyan[4] = {0.45f, 0.9f, 1, 1};
  text("title", 282, Anchor::Center, 1, 640, 98, 48, white);
  text("left_head", 303, Anchor::Left, 0, 60, 230, 28, gold);
  text("right_head", 304, Anchor::Right, 2, 1220, 230, 28, cyan);
  text("name0", 60100, Anchor::Left, 0, 60, 330, 20, white);
  text("name1", 60101, Anchor::Left, 0, 200, 330, 20, white);
  text("rules_small", 228, Anchor::Left, 0, 60, 380, 20, white);
  text("rules_large", 228, Anchor::Left, 0, 60, 410, 40, white);
  text("pace0", 263, Anchor::Center, 1, 480, 590, 28, white);
  text("pace1", 264, Anchor::Center, 1, 640, 590, 28, gold);
  text("pace2", 265, Anchor::Center, 1, 800, 590, 28, white);
  text("two_lines", 274, Anchor::Right, 2, 1220, 450, 24, white);
  text("play", 284, Anchor::Bottom, 1, 640, 660, 36, gold);
  if (fault == "glyph") text("glyph_fault", 262, Anchor::Left, 0, 60, 520, 24, white);
  if (fault == "string") text("string_fault", 999999, Anchor::Left, 0, 60, 520, 24, white);

  Pass2dScene scene;
  scene.strings = blob.bytes.data(), scene.strings_bytes = blob.bytes.size();
  scene.sprites = sprites.data(), scene.sprite_count = sprites.size();
  scene.texts = texts.data(), scene.text_count = texts.size();

  // ------------------------------------------------------------------------------------------------ draw the three sizes
  const uint32_t sizes[3][2] = {{1280, 720}, {2560, 1440}, {3440, 1440}};
  const float clear[4] = {0.05f, 0.05f, 0.08f, 1.0f};
  rdr::FrameBuilder builder;
  std::vector<std::vector<uint8_t>> pictures(3);
  uint32_t picture_w[3] = {}, picture_h[3] = {};
  int rc = 0;
  for (int k = 0; k < 3; ++k) {
    const uint32_t w = sizes[k][0], h = sizes[k][1];
    rdr::ImageDesc desc;
    desc.width = uint16_t(w), desc.height = uint16_t(h);
    desc.format = rdr::Format::RGBA8;
    desc.usage = rdr::kUsageTarget | rdr::kUsageSampled;
    const rdr::ImageId target = renderer->CreateImage(desc);
    builder.Begin(uint64_t(k));
    Pass2dReport report;
    const bool ok = pass.Draw(scene, res, &builder, target, w, h, clear, report);
    rdr::PresentOp present;
    present.source.image = target;
    present.source_rect.width = w, present.source_rect.height = h;
    builder.Present(present);
    renderer->RenderFrame(builder.Finish());
    renderer->WaitIdle();
    std::printf("[pass2d] %ux%u: %u draws, %zu boxes, %zu problems, validation errors so far %llu\n", w, h, report.draws, report.boxes.size(), report.problems.size(),
                static_cast<unsigned long long>(renderer->validation_errors()));
    for (const std::string& p : report.problems) std::fprintf(stderr, "[pass2d] problem: %s\n", p.c_str());
    if (!ok) rc = 5;
    char name[64];
    std::snprintf(name, sizeof name, "pass2d_%ux%u", w, h);
    WriteJson(fs::path(out_dir) / (std::string(name) + ".json"), w, h, scene, report, blob);
    rdr::ImageRef ref;
    ref.image = target;
    if (!renderer->ReadImage(ref, pictures[size_t(k)], picture_w[k], picture_h[k]) || picture_w[k] != w || picture_h[k] != h) return Fail(6, "could not read the picture back");
    if (!rdr::WritePng((fs::path(out_dir) / (std::string(name) + ".png")).string().c_str(), pictures[size_t(k)].data(), w, h)) return Fail(6, "could not write the PNG");
    renderer->DestroyImage(target);
  }

  // ------------------------------------------------------------------------------------------------ the contact sheet: the three, 1280 wide, stacked with a gap
  {
    const uint32_t sheet_w = 1280, gap = 8;
    std::vector<std::vector<uint8_t>> small(3);
    uint32_t hs[3] = {}, total = 0;
    for (int k = 0; k < 3; ++k) small[size_t(k)] = Resize(pictures[size_t(k)], picture_w[k], picture_h[k], sheet_w, hs[k]), total += hs[k] + gap;
    std::vector<uint8_t> sheet(size_t(sheet_w) * total * 4, 255);
    size_t y = 0;
    for (int k = 0; k < 3; ++k) {
      std::memcpy(sheet.data() + y * sheet_w * 4, small[size_t(k)].data(), small[size_t(k)].size());
      y += hs[k] + gap;
    }
    if (!rdr::WritePng((fs::path(out_dir) / "sheet.png").string().c_str(), sheet.data(), sheet_w, total)) return Fail(6, "could not write the sheet");
  }

  const uint64_t errors = renderer->validation_errors();
  std::printf("[pass2d] device %s, validation errors %llu\n", renderer->device_name(), static_cast<unsigned long long>(errors));
  if (errors) return 4;
  return rc;
}
