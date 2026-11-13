// Session 101 lane 9 (lane 23 added surfaces, movies, blend modes, wrapping and clip boxes): see pass2d.h and docs\renderer\PASS2D.md.
#include "pass2d.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "../renderer/shaders/spv/rdr_2d_quad_frag.h"
#include "../renderer/shaders/spv/rdr_2d_quad_vert.h"
#include "../renderer/shaders/spv/rdr_2d_yuv_frag.h"

namespace rdr_scene {
namespace {

constexpr float kRootW = 1280.0f, kRootH = 720.0f;
constexpr uint32_t kVkFormatRG32F = 103, kVkFormatRGBA32F = 109;
constexpr uint8_t kVkTriangleList = 3, kVkCompareAlways = 7;
constexpr uint8_t kVkZero = 0, kVkOne = 1, kVkSrcAlpha = 6, kVkOneMinusSrcAlpha = 7;
constexpr uint32_t kNone = 0xFFFFFFFFu;

// Fragment modes (rdr_2d_quad.frag / rdr_2d_yuv.frag: FragmentConstants.mode.x)
enum class Mode : uint8_t { Sprite = 0, Glyph, Premultiplied, Yuv, YuvAlpha };

struct Vertex {
  float x, y, u, v, r, g, b, a;
};

struct DrawItem {
  uint16_t order = 0;
  uint8_t kind_rank = 0;  // movies (0) before sprites (1) before texts (2) at the same order
  uint32_t seq = 0;       // record order
  const Pass2dTexture* texture = nullptr;
  Pass2dMoviePlanes planes;  // Mode::Yuv*
  float plane_const[16] = {};  // Mode::Yuv*: per plane (Y, Cb, Cr, A) valid-fraction x, y and half-texel x, y
  Mode mode = Mode::Sprite;
  uint8_t blend = 0;      // 0 alpha, 1 additive, 2 opaque, 3 premultiplied
  bool has_clip = false;
  int32_t clip[4] = {0, 0, 0, 0};  // x, y, w, h in display pixels
  std::vector<Vertex> verts;
};

void Problem(Pass2dReport& report, const std::string& text) { report.problems.push_back(text); }
std::string Hex(uint32_t cp) {
  char b[16];
  std::snprintf(b, sizeof b, "U+%04X", cp);
  return b;
}

// Where the layout's top-left lands for an anchor (see the header).
struct Frame {
  float s = 1, width = 0, height = 0;
  float OriginX(uint8_t anchor) const {
    switch (Anchor(anchor)) {
      case Anchor::Left: case Anchor::TopLeft: case Anchor::BottomLeft: return 0;
      case Anchor::Right: case Anchor::TopRight: case Anchor::BottomRight: return width - kRootW * s;
      default: return (width - kRootW * s) * 0.5f;
    }
  }
  float OriginY(uint8_t anchor) const {
    switch (Anchor(anchor)) {
      case Anchor::Top: case Anchor::TopLeft: case Anchor::TopRight: return 0;
      case Anchor::Bottom: case Anchor::BottomLeft: case Anchor::BottomRight: return height - kRootH * s;
      default: return (height - kRootH * s) * 0.5f;
    }
  }
};

const char* StringAt(const Pass2dScene& scene, uint32_t field) {
  if (field == 0 || !scene.strings || field - 1 >= scene.strings_bytes) return nullptr;
  const char* s = scene.strings + (field - 1);
  const size_t room = scene.strings_bytes - (field - 1);
  return std::memchr(s, 0, room) ? s : nullptr;  // (an unterminated string is not one)
}

// One UTF-8 code point; false on a bad sequence (the caller reports it).
bool NextCodepoint(const std::string& s, size_t& i, uint32_t& cp) {
  const uint8_t c = uint8_t(s[i]);
  size_t extra = 0;
  if (c < 0x80) cp = c;
  else if ((c & 0xE0) == 0xC0) cp = c & 0x1F, extra = 1;
  else if ((c & 0xF0) == 0xE0) cp = c & 0x0F, extra = 2;
  else if ((c & 0xF8) == 0xF0) cp = c & 0x07, extra = 3;
  else return ++i, cp = 0xFFFD, false;
  for (size_t k = 1; k <= extra; ++k) {
    if (i + k >= s.size() || (uint8_t(s[i + k]) & 0xC0) != 0x80) return ++i, cp = 0xFFFD, false;
    cp = (cp << 6) | (uint8_t(s[i + k]) & 0x3F);
  }
  i += extra + 1;
  return true;
}

void PushQuad(std::vector<Vertex>& v, const float p[4][2], const float uv[4], const float color[4]) {
  // corners 0 top-left, 1 top-right, 2 bottom-right, 3 bottom-left
  const float us[4] = {uv[0], uv[2], uv[2], uv[0]}, vs[4] = {uv[1], uv[1], uv[3], uv[3]};
  static const int kOrder[6] = {0, 1, 2, 0, 2, 3};
  for (int i : kOrder) v.push_back({p[i][0], p[i][1], us[i], vs[i], color[0], color[1], color[2], color[3]});
}

void PushRect(std::vector<Vertex>& v, float x, float y, float w, float h, const float uv[4], const float color[4], float rotation) {
  const float cx = x + w * 0.5f, cy = y + h * 0.5f, c = std::cos(rotation), s = std::sin(rotation);
  const float corner[4][2] = {{-w * 0.5f, -h * 0.5f}, {w * 0.5f, -h * 0.5f}, {w * 0.5f, h * 0.5f}, {-w * 0.5f, h * 0.5f}};
  float p[4][2];
  for (int k = 0; k < 4; ++k) p[k][0] = cx + corner[k][0] * c - corner[k][1] * s, p[k][1] = cy + corner[k][0] * s + corner[k][1] * c;
  PushQuad(v, p, uv, color);
}

// Bytes of one mip level, for the formats the pass uploads (BC1-3, the 1 / 2 / 4 byte colour formats).
size_t MipBytes(rdr::Format format, uint32_t w, uint32_t h) {
  switch (format) {
    case rdr::Format::BC1: return size_t((w + 3) / 4) * ((h + 3) / 4) * 8;
    case rdr::Format::BC2:
    case rdr::Format::BC3: return size_t((w + 3) / 4) * ((h + 3) / 4) * 16;
    case rdr::Format::R8: return size_t(w) * h;
    case rdr::Format::RG8:
    case rdr::Format::RGBA4:
    case rdr::Format::R5G6B5:
    case rdr::Format::A1RGB5: return size_t(w) * h * 2;
    default: return size_t(w) * h * 4;
  }
}

// A surface role a sprite may show (S101 lane 23; NAMES.md surface roles): the pictures a scene renders for a screen or a texture. A depth buffer, a shadow
// map, a post step's scratch, a feedback map and the present image are never something a sprite shows; an unnamed surface is refused (the role is what makes
// the choice, never a guess from the size).
bool DrawableRole(rdr_adapter::SurfaceRole role) {
  using R = rdr_adapter::SurfaceRole;
  return role == R::SceneColor || role == R::AtlasTarget || role == R::MenuPicture || role == R::TronPicture || role == R::Ui || role == R::Other;
}

// A line of glyphs: the code points and their advances (pixels).
struct Cp {
  uint32_t cp;
  float adv;
};
struct Line {
  std::vector<Cp> cps;
  float width = 0;
};

// One logical line (no '\n') broken into lines no wider than `wrap` pixels. Rule (PASS2D.md): break at spaces; a word goes on the current line when the
// line, the spaces before it and the word fit; spaces at a break are dropped; a word wider than a whole line is cut between glyphs (each piece fills a
// line); spaces at the end of the logical line are kept. wrap <= 0: one line.
void WrapLine(const std::vector<Cp>& in, float wrap, std::vector<Line>& out) {
  if (wrap <= 0) {
    Line l;
    l.cps = in;
    for (const Cp& c : in) l.width += c.adv;
    out.push_back(std::move(l));
    return;
  }
  Line cur;
  std::vector<Cp> pending;  // spaces seen since the last word
  float pending_w = 0;
  auto flush = [&]() {
    out.push_back(std::move(cur));
    cur = Line();
  };
  size_t i = 0;
  while (i < in.size()) {
    if (in[i].cp == ' ') {
      pending.push_back(in[i]), pending_w += in[i].adv;
      ++i;
      continue;
    }
    size_t j = i;
    float word_w = 0;
    while (j < in.size() && in[j].cp != ' ') word_w += in[j].adv, ++j;
    const bool line_empty = cur.cps.empty();
    if (!line_empty && cur.width + pending_w + word_w > wrap) {
      flush();  // the pending spaces are dropped at the break
      pending.clear(), pending_w = 0;
    }
    // (the spaces before the very first word of the logical line are an indent: kept only when indent + word fit the line, else dropped, so a word that fits a
    // line alone is never cut because of its indent; after a break the spaces were dropped above)
    if (line_empty && pending_w + word_w > wrap) pending.clear(), pending_w = 0;
    for (const Cp& c : pending) cur.cps.push_back(c), cur.width += c.adv;
    pending.clear(), pending_w = 0;
    if (cur.width + word_w <= wrap) {
      for (size_t k = i; k < j; ++k) cur.cps.push_back(in[k]), cur.width += in[k].adv;
    } else {
      // wider than a line: cut between glyphs
      for (size_t k = i; k < j; ++k) {
        if (!cur.cps.empty() && cur.width + in[k].adv > wrap) flush();
        cur.cps.push_back(in[k]), cur.width += in[k].adv;
      }
    }
    i = j;
  }
  for (const Cp& c : pending) cur.cps.push_back(c), cur.width += c.adv;
  out.push_back(std::move(cur));
}

}  // namespace

Pass2d::Pass2d(rdr::Renderer& renderer) : renderer_(renderer) {
  rdr::ProgramDesc program;
  program.name = "rdr_2d_quad";
  program.vertex_spirv = kSpv_rdr_2d_quad_vert;
  program.vertex_bytes = sizeof(kSpv_rdr_2d_quad_vert);
  program.fragment_spirv = kSpv_rdr_2d_quad_frag;
  program.fragment_bytes = sizeof(kSpv_rdr_2d_quad_frag);
  program_ = renderer.AddProgram(program);
  program.name = "rdr_2d_yuv";
  program.fragment_spirv = kSpv_rdr_2d_yuv_frag;
  program.fragment_bytes = sizeof(kSpv_rdr_2d_yuv_frag);
  yuv_program_ = renderer.AddProgram(program);
  rdr::VertexLayoutDesc layout{};
  layout.attribute_count = 3;
  layout.attributes[0] = {0, 0, uint16_t(kVkFormatRG32F), 0};
  layout.attributes[1] = {1, 0, uint16_t(kVkFormatRG32F), 8};
  layout.attributes[2] = {2, 0, uint16_t(kVkFormatRGBA32F), 16};
  layout.strides[0] = sizeof(Vertex);
  layout_ = renderer.AddVertexLayout(layout);
}

Pass2dTexture Pass2d::Upload(const UiImage& image) {
  Pass2dTexture out;
  if (image.width == 0 || image.height == 0 || image.format == rdr::Format::Undefined || image.data.empty()) return out;
  rdr::ImageDesc desc;
  desc.width = uint16_t(image.width), desc.height = uint16_t(image.height);
  desc.mips = uint8_t(image.mips);
  desc.format = image.format;
  desc.usage = rdr::kUsageSampled;
  const rdr::ImageId id = renderer_.CreateImage(desc);
  if (id == rdr::kNoImage) return out;
  size_t offset = 0;
  for (uint32_t m = 0; m < image.mips; ++m) {
    const uint32_t w = std::max(image.width >> m, 1u), h = std::max(image.height >> m, 1u);
    const size_t bytes = MipBytes(image.format, w, h);
    if (offset + bytes > image.data.size()) {
      renderer_.DestroyImage(id);
      return out;
    }
    renderer_.UpdateImage(id, m, 0, image.data.data() + offset, bytes);
    offset += bytes;
  }
  out.image = id, out.width = image.width, out.height = image.height;
  return out;
}

Pass2dFont Pass2d::Upload(const UiFont& font) {
  Pass2dFont out;
  out.sheet = Upload(font.sheet);
  out.cell = font.cell;
  out.glyphs = font.glyphs;
  out.advance_trim = font.advance_trim;
  out.kerns = font.kerns;
  return out;
}

bool Pass2d::Draw(const Pass2dScene& scene, Pass2dResources& resources, rdr::FrameBuilder* builder, rdr::ImageId target, uint32_t width, uint32_t height,
                  const float* clear, Pass2dReport& report) {
  report = {};
  if (width == 0 || height == 0) return Problem(report, "the target has no size"), false;
  Frame frame;
  frame.width = float(width), frame.height = float(height), frame.s = float(height) / kRootH;
  report.scale = frame.s;
  std::vector<DrawItem> items;

  // Sprites a movie shows itself on (Movie::show OnSprite): they carry no picture of their own and are not a fault.
  std::vector<uint8_t> hosts(scene.sprite_count, 0);
  for (size_t m = 0; m < scene.movie_count; ++m)
    if (scene.movies[m].show == uint8_t(MovieShow::OnSprite) && scene.movies[m].target < scene.sprite_count) hosts[scene.movies[m].target] = 1;

  struct Laid {
    float x = 0, y = 0, w = 0, h = 0;
  };
  std::vector<Laid> laid(scene.sprite_count);

  // ---- sprites
  for (size_t i = 0; i < scene.sprite_count; ++i) {
    const Sprite& sp = scene.sprites[i];
    const std::string who = "sprite " + std::to_string(i);
    float x, y, w, h;
    if (Anchor(sp.anchor) == Anchor::Stretch) {
      x = sp.rect[0] / kRootW * float(width), y = sp.rect[1] / kRootH * float(height);
      w = sp.rect[2] / kRootW * float(width), h = sp.rect[3] / kRootH * float(height);
    } else {
      x = frame.OriginX(sp.anchor) + sp.rect[0] * frame.s, y = frame.OriginY(sp.anchor) + sp.rect[1] * frame.s;
      w = sp.rect[2] * frame.s, h = sp.rect[3] * frame.s;
    }
    laid[i] = {x, y, w, h};

    // where the picture comes from: a surface the scene renderer made (by role), or a file texture; never a file texture standing in for a surface
    const Pass2dTexture* tex = nullptr;
    Pass2dBox::Source source = Pass2dBox::kFile;
    uint32_t source_index = 0;
    if (sp.target != kNone) {
      if (!scene.targets || sp.target >= scene.target_count) {
        Problem(report, who + ": target " + std::to_string(sp.target) + " is not in the record's targets");
        continue;
      }
      const Target& tg = scene.targets[sp.target];
      if (!DrawableRole(tg.role)) {
        Problem(report, who + ": target " + std::to_string(sp.target) + " has the role '" + rdr_adapter::SurfaceRoleName(tg.role) + "', which a sprite cannot show");
        continue;
      }
      tex = resources.Target(sp.target);
      if (!tex || tex->image == rdr::kNoImage) {
        Problem(report, who + ": target " + std::to_string(sp.target) + " ('" + rdr_adapter::SurfaceRoleName(tg.role) + "') is not available");
        continue;
      }
      source = Pass2dBox::kTarget, source_index = sp.target;
    } else if (sp.texture != kNone) {
      tex = resources.Texture(sp.texture);
      if (!tex || tex->image == rdr::kNoImage) {
        Problem(report, who + ": texture asset " + std::to_string(sp.texture) + " is not available");
        continue;
      }
    } else if (hosts[i]) {
      report.boxes.push_back({Pass2dBox::kSprite, uint32_t(i), 0, 0, x, y, w, h, Pass2dBox::kFile, 0});  // (a movie's place: it draws below)
      continue;
    } else {
      Problem(report, who + ": no texture and no target");
      continue;
    }
    if (tex && target != rdr::kNoImage && tex->image == target) {
      Problem(report, who + ": it shows the image this pass draws into (a Ui / SceneColor surface cannot be sampled by its own pass: undefined reads)");
      continue;
    }
    if (sp.blend > 3) {
      Problem(report, who + ": blend mode " + std::to_string(sp.blend) + " is not one of 0 alpha / 1 additive / 2 opaque / 3 premultiplied");
      continue;
    }
    report.boxes.push_back({Pass2dBox::kSprite, uint32_t(i), 0, 0, x, y, w, h, source, source_index});
    DrawItem item;
    item.order = sp.order, item.kind_rank = 1, item.seq = uint32_t(i), item.texture = tex, item.blend = sp.blend;
    item.mode = sp.blend == 3 ? Mode::Premultiplied : Mode::Sprite;
    PushRect(item.verts, x, y, w, h, sp.uv, sp.color, sp.rotation);
    items.push_back(std::move(item));
  }

  // ---- movies
  for (size_t m = 0; m < scene.movie_count; ++m) {
    const Movie& mv = scene.movies[m];
    const std::string who = "movie " + std::to_string(m);
    if (mv.show != uint8_t(MovieShow::FullScreen) && mv.show != uint8_t(MovieShow::OnSprite)) {
      Problem(report, who + ": show mode " + std::to_string(mv.show) + " (a movie on a scene target) belongs to the tron path, not the 2D pass");
      continue;
    }
    Pass2dMoviePlanes planes;
    if (!resources.MoviePlanes(uint32_t(m), planes) || planes.y.image == rdr::kNoImage || planes.cr.image == rdr::kNoImage || planes.cb.image == rdr::kNoImage ||
        planes.y.width == 0 || planes.y.height == 0) {
      Problem(report, who + ": its Y / cR / cB planes are not available");
      continue;
    }
    if (mv.has_alpha && planes.alpha.image == rdr::kNoImage) {
      Problem(report, who + ": it has alpha but no alpha plane is available");
      continue;
    }
    if (target != rdr::kNoImage && (planes.y.image == target || planes.cr.image == target || planes.cb.image == target || (mv.has_alpha && planes.alpha.image == target))) {
      Problem(report, who + ": a plane is the image this pass draws into");
      continue;
    }
    static const float kWhite[4] = {1, 1, 1, 1}, kFullUv[4] = {0, 0, 1, 1};
    const float* color = kWhite;
    const float* uv = kFullUv;
    float x, y, w, h, rotation = 0.0f;
    DrawItem item;
    item.mode = mv.has_alpha ? Mode::YuvAlpha : Mode::Yuv;
    item.planes = planes;
    item.seq = uint32_t(m);
    // per plane: the valid part of the image as a fraction of it, and the uv range linear taps stay inside (half a texel in from each valid edge)
    auto plane_const = [&](const Pass2dTexture& t, const uint32_t valid[2], float* out) {
      const float vw = valid[0] ? float(std::min(valid[0], t.width)) : float(t.width), vh = valid[1] ? float(std::min(valid[1], t.height)) : float(t.height);
      out[0] = vw / float(t.width), out[1] = vh / float(t.height);
      out[2] = 0.5f / float(t.width), out[3] = 0.5f / float(t.height);
    };
    plane_const(planes.y, planes.valid_y, item.plane_const + 0);
    plane_const(planes.cb, planes.valid_cb, item.plane_const + 4);   // S101 lane 131 (review F12): slot 1 is Cb, slot 2 is Cr, as the game binds them (MOVIE_OURS.md)
    plane_const(planes.cr, planes.valid_cr, item.plane_const + 8);
    plane_const(mv.has_alpha ? planes.alpha : planes.y, mv.has_alpha ? planes.valid_alpha : planes.valid_y, item.plane_const + 12);
    const float pic_w = planes.valid_y[0] ? float(std::min(planes.valid_y[0], planes.y.width)) : float(planes.y.width);
    const float pic_h = planes.valid_y[1] ? float(std::min(planes.valid_y[1], planes.y.height)) : float(planes.y.height);
    if (mv.show == uint8_t(MovieShow::FullScreen)) {
      // the whole picture, as large as fits, centred: bars (the clear colour) on the sides of a wider screen, above and below a taller one
      const float fit = std::min(float(width) / pic_w, float(height) / pic_h);
      w = pic_w * fit, h = pic_h * fit;
      x = (float(width) - w) * 0.5f, y = (float(height) - h) * 0.5f;
      item.order = 0, item.kind_rank = 0;
    } else {
      if (mv.target >= scene.sprite_count) {
        Problem(report, who + ": its sprite " + std::to_string(mv.target) + " is not in the record");
        continue;
      }
      const Sprite& sp = scene.sprites[mv.target];
      x = laid[mv.target].x, y = laid[mv.target].y, w = laid[mv.target].w, h = laid[mv.target].h;
      color = sp.color, uv = sp.uv, rotation = sp.rotation;
      item.order = sp.order, item.kind_rank = 1;
      item.seq = mv.target;  // the host's place in the record order (a sprite listed after it is drawn over the movie)
    }
    report.boxes.push_back({Pass2dBox::kMovie, uint32_t(m), 0, 0, x, y, w, h, Pass2dBox::kFile, 0});
    PushRect(item.verts, x, y, w, h, uv, color, rotation);
    items.push_back(std::move(item));
  }

  // ---- text
  const bool extra_ok = scene.text_extra && scene.text_extra_count == scene.text_count;
  if ((scene.text_extra || scene.text_extra_count) && !extra_ok)
    Problem(report, "text_extra has " + std::to_string(scene.text_extra_count) + " entries for " + std::to_string(scene.text_count) + " texts: the table is not used (no wrapping, no clip boxes)");
  for (size_t i = 0; i < scene.text_count; ++i) {
    const Text& t = scene.texts[i];
    const Pass2dTextExtra* extra = extra_ok ? &scene.text_extra[i] : nullptr;
    const std::string who = "text " + std::to_string(i) + (t.string_id ? " (string id " + std::to_string(t.string_id) + ")" : "");
    std::string utf8;
    if (t.utf8) {
      const char* s = StringAt(scene, t.utf8);
      if (!s) {
        Problem(report, who + ": its utf8 field points outside the record's strings");
        continue;
      }
      utf8 = s;
    } else if (t.string_id) {
      const std::string* s = resources.StringById(t.string_id);
      if (!s) {
        Problem(report, who + ": the string id is unknown");
        continue;
      }
      utf8 = *s;
    } else {
      Problem(report, who + ": no text and no string id");
      continue;
    }
    const Pass2dFont* font = resources.Font(t.font);
    if (!font || font->sheet.image == rdr::kNoImage || font->cell <= 0) {
      Problem(report, who + ": font asset " + std::to_string(t.font) + " is not available");
      continue;
    }
    const float size_px = t.size * frame.s, scale = size_px / font->cell;
    const float ox = frame.OriginX(t.anchor), oy = frame.OriginY(t.anchor);
    const float ax = ox + t.pos[0] * frame.s, ay = oy + t.pos[1] * frame.s;
    Pass2dTextInfo info{uint32_t(i), t.string_id, utf8, size_px, scale, ax, ay, t.align};

    // logical lines, then wrapped lines
    std::vector<Line> lines;
    std::vector<Cp> logical;
    std::set<uint32_t> reported_here;
    float space_adv = font->cell * 0.5f;
    if (auto sp = font->glyphs.find(' '); sp != font->glyphs.end()) space_adv = float(sp->second.w);
    const float wrap_px = extra && extra->wrap_width > 0 ? extra->wrap_width * frame.s : 0.0f;
    info.wrap_px = wrap_px;
    // The pen (UI_PROMOTION_MAP.md section 6, measured; the game's run function sub_82BD06B8): a glyph advances (w - advance_trim - kern(this, next)) x
    // scale, the kern read from the .yfb pair list; the run's units below 0x20 and 0x7F are skipped (no glyph, no advance), U+2019 is drawn as U+0027,
    // and a code the font lacks is drawn as '*' (nothing if '*' is missing too). The report stays loud: the missing code is still listed (the game's
    // fallback is not a reason to hide a hole). Icon codes (>= 0xE000) have no glyph here (they come from the icon sheet at a size from the text
    // object, not the run's): a space-wide gap, reported.
    auto end_logical = [&]() {
      for (size_t q = 0; q < logical.size(); ++q) {
        const auto cur = font->glyphs.find(logical[q].cp);
        const float w = cur != font->glyphs.end() ? float(cur->second.w) : space_adv;  // (a gap has the space's width)
        int32_t kern = 0;
        if (cur != font->glyphs.end() && q + 1 < logical.size()) kern = font->Kern(logical[q].cp, logical[q + 1].cp);
        logical[q].adv = std::max(0.0f, w - font->advance_trim - float(kern)) * scale;
      }
      WrapLine(logical, wrap_px, lines);
      logical.clear();
    };
    for (size_t k = 0; k < utf8.size();) {
      uint32_t cp = 0;
      if (!NextCodepoint(utf8, k, cp)) Problem(report, who + ": invalid UTF-8 in the string");
      if (cp == '\n') { end_logical(); continue; }
      if (cp < 0x20 || cp == 0x7F) continue;  // skipped by the game (0x82BD0974): no glyph, no advance
      if (cp == 0x2019) cp = 0x27;           // the right single quote is drawn as an apostrophe (0x82BD098C)
      if (font->glyphs.find(cp) == font->glyphs.end()) {
        report.missing_glyphs.insert(cp);
        if (reported_here.insert(cp).second) Problem(report, who + ": the font has no glyph for " + Hex(cp));
        if (cp < 0xE000) {
          if (font->glyphs.find('*') == font->glyphs.end()) continue;  // '*' missing too: the game draws nothing
          cp = '*';
        }  // else an icon code: stays as it is, drawn as a gap
      }
      logical.push_back({cp, 0.0f});
    }
    end_logical();
    info.lines = uint32_t(lines.size());

    DrawItem item;
    item.order = t.order, item.kind_rank = 2, item.seq = uint32_t(i), item.texture = &font->sheet, item.mode = Mode::Glyph, item.blend = 0;
    if (extra && extra->has_clip) {
      // the scissor: the clip box in this text's anchor frame, pixel edges rounded outwards, kept inside the display
      const float cx0 = ox + extra->clip[0] * frame.s, cy0 = oy + extra->clip[1] * frame.s;
      const float cx1 = cx0 + extra->clip[2] * frame.s, cy1 = cy0 + extra->clip[3] * frame.s;
      if (std::isnan(cx0) || std::isnan(cy0) || std::isnan(cx1) || std::isnan(cy1)) {
        Problem(report, who + ": its clip box is not a number (the text is not drawn)");
        continue;
      }
      // clamp in float BEFORE the integer cast (a huge box would be undefined behaviour as an int)
      auto clampf = [](float v, float hi) { return std::min(std::max(v, 0.0f), hi); };
      const int32_t x0 = int32_t(std::floor(clampf(cx0, float(width)))), y0 = int32_t(std::floor(clampf(cy0, float(height))));
      const int32_t x1 = int32_t(std::ceil(clampf(cx1, float(width)))), y1 = int32_t(std::ceil(clampf(cy1, float(height))));
      item.has_clip = true;
      item.clip[0] = x0, item.clip[1] = y0, item.clip[2] = std::max(0, x1 - x0), item.clip[3] = std::max(0, y1 - y0);
      info.has_clip = true;
      std::memcpy(info.clip_px, item.clip, sizeof(item.clip));
    }
    report.texts.push_back(info);
    for (size_t li = 0; li < lines.size(); ++li) {
      float x = ax - (t.align == 1 ? lines[li].width * 0.5f : t.align == 2 ? lines[li].width : 0.0f);
      const float y = ay + float(li) * size_px;
      for (const Cp& c : lines[li].cps) {
        auto g = font->glyphs.find(c.cp);
        if (g == font->glyphs.end()) {
          x += c.adv;
          continue;
        }
        const UiGlyph& gl = g->second;
        const float w = float(gl.w) * scale, h = float(gl.h) * scale;  // (the quad is w x scale wide; the pen moves by c.adv)
        report.boxes.push_back({Pass2dBox::kGlyph, uint32_t(i), c.cp, uint32_t(li), x, y, w, h, Pass2dBox::kFile, 0});
        if (gl.w && gl.h) {
          const float p[4][2] = {{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}};
          const float sw = float(font->sheet.width), sh = float(font->sheet.height);
          const float uv[4] = {gl.x / sw, gl.y / sh, (gl.x + gl.w) / sw, (gl.y + gl.h) / sh};
          PushQuad(item.verts, p, uv, t.color);
        }
        x += c.adv;
      }
    }
    if (!item.verts.empty()) items.push_back(std::move(item));
  }

  std::stable_sort(items.begin(), items.end(), [](const DrawItem& a, const DrawItem& b) {
    if (a.order != b.order) return a.order < b.order;
    if (a.kind_rank != b.kind_rank) return a.kind_rank < b.kind_rank;
    return a.seq < b.seq;  // (stable: a movie on a sprite keeps its place after the host's own picture)
  });
  if (!builder) return report.ok();

  // ---- record
  rdr::TargetsOp targets;
  targets.color[0].image = target;
  targets.color_count = 1;
  builder->SetTargets(targets);
  if (clear) {
    rdr::ClearOp op;
    op.flags = rdr::kClearColor;
    op.full = 1;
    std::memcpy(op.color, clear, sizeof(op.color));
    builder->Clear(op);
  }
  rdr::SamplerDesc sampler{};
  sampler.mag_filter = sampler.min_filter = sampler.mip_mode = 1;  // linear
  sampler.address_u = sampler.address_v = sampler.address_w = 2;    // clamp to edge
  sampler.max_anisotropy = 1;
  sampler.max_lod = 1000.0f;
  const uint32_t sampler_index = builder->Sampler(sampler);
  const float screen[4] = {2.0f / float(width), 2.0f / float(height), 0, 0};
  const uint32_t vs = builder->PushConstants(screen, sizeof(screen));
  const float modes[5][4] = {{0, 0, 0, 0}, {1, 0, 0, 0}, {2, 0, 0, 0}, {0, 0, 0, 0}, {1, 0, 0, 0}};  // indexed by Mode
  uint32_t fs[5];
  for (int m = 0; m < 5; ++m) fs[m] = builder->PushConstants(modes[m], sizeof(modes[m]));
  for (const DrawItem& item : items) {
    rdr::DynamicState dynamic{};
    dynamic.viewport[2] = float(width), dynamic.viewport[3] = float(height), dynamic.viewport[5] = 1.0f;
    if (item.has_clip) {
      dynamic.scissor.x = item.clip[0], dynamic.scissor.y = item.clip[1];
      dynamic.scissor.width = uint32_t(item.clip[2]), dynamic.scissor.height = uint32_t(item.clip[3]);
    } else {
      dynamic.scissor.width = width, dynamic.scissor.height = height;
    }
    rdr::DrawState state{};
    state.program = item.mode == Mode::Yuv || item.mode == Mode::YuvAlpha ? yuv_program_ : program_;
    state.vertex_layout = layout_;
    state.topology = kVkTriangleList;
    state.depth_compare = kVkCompareAlways;
    rdr::BlendState& b = state.blend[0];
    b.write_mask = 0xF;
    switch (item.blend) {
      case 1:  // additive: the sprite's colour times its alpha, added
        b.enable = 1, b.src_color = kVkSrcAlpha, b.dst_color = kVkOne, b.color_op = 0;
        b.src_alpha = kVkOne, b.dst_alpha = kVkOne, b.alpha_op = 0;
        break;
      case 2:  // opaque
        b.src_color = b.src_alpha = kVkOne, b.dst_color = b.dst_alpha = kVkZero;
        break;
      case 3:  // premultiplied: the shader already carries alpha in the colour
        b.enable = 1, b.src_color = kVkOne, b.dst_color = kVkOneMinusSrcAlpha, b.color_op = 0;
        b.src_alpha = kVkOne, b.dst_alpha = kVkOneMinusSrcAlpha, b.alpha_op = 0;
        break;
      default:  // straight alpha
        b.enable = 1, b.src_color = kVkSrcAlpha, b.dst_color = kVkOneMinusSrcAlpha, b.color_op = 0;
        b.src_alpha = kVkOne, b.dst_alpha = kVkOneMinusSrcAlpha, b.alpha_op = 0;
        break;
    }
    rdr::TextureSet textures;
    if (item.mode == Mode::Yuv || item.mode == Mode::YuvAlpha) {
      textures.slots[0].image = item.planes.y.image;
      textures.slots[1].image = item.planes.cb.image;   // S101 lane 131 (review F12): the game's order (slot 1 Cb, slot 2 Cr)
      textures.slots[2].image = item.planes.cr.image;
      textures.slots[3].image = item.mode == Mode::YuvAlpha ? item.planes.alpha.image : item.planes.y.image;
      for (int s = 0; s < 4; ++s) textures.slots[s].sampler = sampler_index;
    } else {
      textures.slots[0].image = item.texture->image;
      textures.slots[0].sampler = sampler_index;
    }
    rdr::DrawOp draw{};
    draw.state = builder->State(state);
    draw.dynamic = builder->Dynamic(dynamic);
    draw.textures = builder->Textures(textures);
    draw.vs_constants = vs;
    if (item.mode == Mode::Yuv || item.mode == Mode::YuvAlpha) {
      float c[20] = {item.mode == Mode::YuvAlpha ? 1.0f : 0.0f, 0, 0, 0};
      std::memcpy(c + 4, item.plane_const, sizeof(item.plane_const));
      draw.fs_constants = builder->PushConstants(c, sizeof(c));
    } else {
      draw.fs_constants = fs[int(item.mode)];
    }
    draw.vertex[0] = {rdr::kFrameGeometry, builder->PushGeometry(item.verts.data(), uint32_t(item.verts.size() * sizeof(Vertex)))};
    draw.count = uint32_t(item.verts.size());
    draw.instances = 1;
    builder->Draw(draw);
    ++report.draws;
  }
  return report.ok();
}

}  // namespace rdr_scene
