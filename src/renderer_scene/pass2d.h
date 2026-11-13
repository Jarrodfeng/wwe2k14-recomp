// Session 101 lane 9 (arc 3.4 P2 / P3, class C1): our own 2D pass. Draws a scene record's 2D layer (scene_record.h: Sprite[] and Text[]) through
// src\renderer into a colour image of the display's own size: sprites are textured quads, text is the game's strings drawn glyph by glyph from the
// game's font (ui_assets.h). Nothing here knows the 360, a draw call of the game or a Vulkan type.
//
// Layout (PASS2D.md): the record's units are the game's 1280 x 720 root. One uniform scale s = display height / 720 (the layout is always
// as tall as the screen); the anchor says what the horizontal / vertical position is measured from, so a wider or narrower screen only adds or
// removes room at the sides:
//   horizontal  Left, TopLeft, BottomLeft   from the screen's left edge          Right, TopRight, BottomRight   from the right edge (x = 1280 is the edge)
//               everything else             from the centred 1280*s wide box
//   vertical    Top, TopLeft, TopRight      from the top edge                    Bottom, BottomLeft, BottomRight   from the bottom edge
//               everything else             from the centred 720*s tall box (on 16:9 and wider that is the whole height)
//   Stretch     the item's rect is a fraction of the root and covers the same fraction of the WHOLE screen (backgrounds, scrims)
// Text: pos is where the line starts (align 0), is centred on (1) or ends at (2); `size` is the glyph cell height; a glyph advances by the .yfb
// record's width times size * s / cell; '\n' starts a new line one `size` lower.
// Order: by `order`, movies before sprites before texts at the same order, record order after that. A full-screen movie is a backdrop (order 0).
// Lane 23: a sprite shows a file texture or, with `target`, a surface the scene made (by role, via Pass2dResources::Target); blend 0 straight alpha,
// 1 additive, 2 opaque, 3 premultiplied; movies (Pass2dMoviePlanes) are YUV quads, full screen (aspect kept, centred) or on a sprite's rect; a text can
// wrap at a width and be clipped to a box (Pass2dTextExtra).
#pragma once
#include <cstdint>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "../renderer/frame_builder.h"
#include "../renderer/renderer.h"
#include "scene_record.h"
#include "ui_assets.h"

namespace rdr_scene {

struct Pass2dTexture {
  rdr::ImageId image = rdr::kNoImage;
  uint32_t width = 0, height = 0;
};

struct Pass2dFont {
  Pass2dTexture sheet;
  float cell = 0;
  std::unordered_map<uint32_t, UiGlyph> glyphs;
  float advance_trim = 2.0f;                  // S101 U1: see UiFont (the game's advance is (w - advance_trim - kern) x scale)
  std::unordered_map<uint32_t, int32_t> kerns;  // key = left << 16 | right
  int32_t Kern(uint32_t left, uint32_t right) const {
    if (left > 0xFFFF || right > 0xFFFF) return 0;  // (see UiFont::Kern)
    const auto it = kerns.find((left << 16) | (right & 0xFFFF));
    return it == kerns.end() ? 0 : it->second;
  }
};

// A movie's decoded planes (S101 lane 23): what the game's Bink decoder keeps decoding, as the caller holds them on the GPU. Y is the full-size luma
// plane (it gives the picture's size), cR / cB are the chroma planes (any size, sampled by uv: half size for 4:2:0), alpha is optional (Movie::has_alpha).
// All are single-channel (R8) images; the colour is BT.601 limited range (docs\renderer\effects\ySprite.md, the game's own YUV sprite).
// Each plane may be PADDED (the game's own planes are: Y 1280 x 280 with chroma 640 x 144 in a capture, effects\ySprite.md): `valid_*` is the part of the
// image that holds picture, in texels; 0 = the whole image. The pass maps the movie's uv onto the valid part only and keeps linear taps inside it. The
// aspect of a movie is its Y plane's valid size.
struct Pass2dMoviePlanes {
  Pass2dTexture y, cr, cb, alpha;
  uint32_t valid_y[2] = {0, 0}, valid_cr[2] = {0, 0}, valid_cb[2] = {0, 0}, valid_alpha[2] = {0, 0};  // width, height
};

// What the pass asks its caller for. An asset index is the record's own (Sprite::texture, Text::font); null = not available (reported, not drawn).
struct Pass2dResources {
  virtual ~Pass2dResources() = default;
  virtual const Pass2dTexture* Texture(uint32_t asset) = 0;
  virtual const Pass2dFont* Font(uint32_t asset) = 0;
  // Text with no utf8 of its own but a string_id: null = unknown id (reported).
  virtual const std::string* StringById(uint32_t id) = 0;
  // S101 lane 23: the surface a Sprite::target names (an index of the record's kScTargets; the Target's role says what it is: the scene renderer made it
  // this frame, before this pass). null = not available (reported, not drawn). Never a file texture: a GPU picture is sampled where it is.
  virtual const Pass2dTexture* Target(uint32_t target_index) { (void)target_index; return nullptr; }
  // S101 lane 23: the decoded planes of the record's Movie[movie_index]; false = not available (reported, not drawn).
  virtual bool MoviePlanes(uint32_t movie_index, Pass2dMoviePlanes& out) { (void)movie_index; (void)out; return false; }
};

// S101 lane 23: text options the record's Text does not carry yet (a side table, parallel to Pass2dScene::texts; lane 1 owns the record). Layout units, in
// the text's own anchor frame (the same frame as Text::pos).
struct Pass2dTextExtra {
  float wrap_width = 0;      // > 0: break the text into lines no wider than this (at spaces; a word wider than a line is cut between glyphs)
  bool has_clip = false;     // true: nothing of this text is drawn outside `clip` (x, y, w, h)
  float clip[4] = {0, 0, 0, 0};
};

// The record's 2D tables (views; nothing is copied).
struct Pass2dScene {
  const char* strings = nullptr;  // the record's kScStrings: a Text's utf8 / widget is a byte offset + 1 into it (0 = none)
  size_t strings_bytes = 0;
  const Sprite* sprites = nullptr;
  size_t sprite_count = 0;
  const Text* texts = nullptr;
  size_t text_count = 0;
  // S101 lane 23
  const Target* targets = nullptr;  // the record's kScTargets: a Sprite::target is an index into it
  size_t target_count = 0;
  const Movie* movies = nullptr;    // the record's kScMovies
  size_t movie_count = 0;
  const Pass2dTextExtra* text_extra = nullptr;  // parallel to `texts` (or null: no wrapping, no clip boxes)
  size_t text_extra_count = 0;                  // must equal text_count when text_extra is set (a mismatch is a problem and the table is not used)
};

// The pass's layout output: where everything landed, in display pixels (top-left origin). The offline tests read this, not the picture.
struct Pass2dBox {
  enum Kind : uint8_t { kSprite = 0, kGlyph = 1, kMovie = 2 };
  enum Source : uint8_t { kFile = 0, kTarget = 1 };  // sprites: where the picture comes from (a file texture / a surface by role)
  Kind kind = kSprite;
  uint32_t item = 0;       // index into Pass2dScene::sprites / texts / movies
  uint32_t codepoint = 0;  // glyphs
  uint32_t line = 0;       // glyphs: the line within the text
  float x = 0, y = 0, w = 0, h = 0;
  Source source = kFile;   // sprites
  uint32_t source_index = 0;  // sprites from a surface: the Target index
};

struct Pass2dTextInfo {
  uint32_t item = 0;
  uint32_t string_id = 0;
  std::string utf8;
  float size_px = 0;       // the cell height in pixels
  float scale = 0;         // size_px / cell
  float anchor_x = 0;      // pos.x in pixels (before the alignment)
  float anchor_y = 0;
  uint8_t align = 0;
  uint32_t lines = 1;      // S101 lane 23: lines drawn (after wrapping)
  float wrap_px = 0;       // the wrap width in pixels (0 = none)
  bool has_clip = false;
  int32_t clip_px[4] = {0, 0, 0, 0};  // the scissor the text's draw uses: x, y, w, h (display pixels)
};

struct Pass2dReport {
  std::vector<Pass2dBox> boxes;
  std::vector<Pass2dTextInfo> texts;
  std::set<uint32_t> missing_glyphs;       // code points the font has no glyph for (drawn as a gap, never silently)
  std::vector<std::string> problems;       // everything that was not drawn as asked
  uint32_t draws = 0;
  float scale = 0;                         // s
  bool ok() const { return problems.empty(); }
};

class Pass2d {
 public:
  explicit Pass2d(rdr::Renderer& renderer);

  // A texture / the font sheet on the GPU (RGBA8 / BGRA8 / BC1-3 from ui_assets). kNoImage image on failure.
  Pass2dTexture Upload(const UiImage& image);
  Pass2dFont Upload(const UiFont& font);

  // Records the pass into `builder`: SetTargets(target) and the draws (and a clear to `clear` first when it is non-null: 4 floats). The
  // target must be a colour image of exactly width x height. Returns report.ok(). Layout only: pass builder = nullptr.
  bool Draw(const Pass2dScene& scene, Pass2dResources& resources, rdr::FrameBuilder* builder, rdr::ImageId target, uint32_t width, uint32_t height,
            const float* clear, Pass2dReport& report);

 private:
  rdr::Renderer& renderer_;
  uint16_t program_ = 0, yuv_program_ = 0, layout_ = 0;
};

}  // namespace rdr_scene
