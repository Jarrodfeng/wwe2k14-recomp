// Lane 292: renders the music picker's view OFFSCREEN (no window, no GPU, no game) into PPM files with Dear ImGui's own draw lists and a
// small software rasteriser, so the pictures are made by the very code the game runs (src\music_picker_imgui.h).
//   build + run + PNG conversion:  pwsh -NoProfile -File tools\music\build_picker_shots.ps1 [-GameRoot <game_data\extracted>] [-Out out\l292]
// The menu face is read from <GameRoot>\pac\menu\fontHD.pac when given (the player's own files; the pictures stay under out\, never committed).
#include <imgui.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <string>
#include <vector>

#include "music_picker_imgui.h"

using namespace wwe2k14::music_picker;

namespace {

struct SwTex {
  int w = 0, h = 0;
  std::vector<uint8_t> px;
};
std::deque<SwTex> g_tex;

GpuImage Upload(int w, int h, const uint8_t* rgba) {
  g_tex.emplace_back();
  SwTex& t = g_tex.back();
  t.w = w, t.h = h, t.px.assign(rgba, rgba + size_t(w) * size_t(h) * 4);
  GpuImage g;
  g.id = uint64_t(reinterpret_cast<uintptr_t>(&t));
  return g;
}

struct Image {
  int w = 0, h = 0;
  std::vector<uint8_t> px;  // RGB
};

void Sample(const SwTex* t, float u, float v, float out[4]) {
  const float x = u * float(t->w) - 0.5f, y = v * float(t->h) - 0.5f;
  const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
  const float fx = x - float(x0), fy = y - float(y0);
  float acc[4] = {0, 0, 0, 0};
  for (int j = 0; j < 2; ++j)
    for (int i = 0; i < 2; ++i) {
      const int xx = std::clamp(x0 + i, 0, t->w - 1), yy = std::clamp(y0 + j, 0, t->h - 1);
      const float w = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
      const uint8_t* p = &t->px[(size_t(yy) * size_t(t->w) + size_t(xx)) * 4];
      for (int c = 0; c < 4; ++c) acc[c] += w * float(p[c]) / 255.0f;
    }
  for (int c = 0; c < 4; ++c) out[c] = acc[c];
}

// Renders ImDrawData into img (RGB). Straight-alpha blend, the same maths as the overlay shader.
void Raster(const ImDrawData* dd, Image& img) {
  for (int n = 0; n < dd->CmdListsCount; ++n) {
    const ImDrawList* dl = dd->CmdLists[n];
    for (const ImDrawCmd& cmd : dl->CmdBuffer) {
      if (cmd.UserCallback) continue;
      const SwTex* tex = reinterpret_cast<const SwTex*>(cmd.GetTexID());
      const int cx0 = std::max(0, int(cmd.ClipRect.x)), cy0 = std::max(0, int(cmd.ClipRect.y));
      const int cx1 = std::min(img.w, int(std::ceil(cmd.ClipRect.z))), cy1 = std::min(img.h, int(std::ceil(cmd.ClipRect.w)));
      for (unsigned i = 0; i + 2 < cmd.ElemCount; i += 3) {
        const ImDrawIdx* ix = &dl->IdxBuffer[int(cmd.IdxOffset + i)];
        const ImDrawVert* v[3] = {&dl->VtxBuffer[int(cmd.VtxOffset + ix[0])], &dl->VtxBuffer[int(cmd.VtxOffset + ix[1])],
                                  &dl->VtxBuffer[int(cmd.VtxOffset + ix[2])]};
        const float area = (v[1]->pos.x - v[0]->pos.x) * (v[2]->pos.y - v[0]->pos.y) - (v[1]->pos.y - v[0]->pos.y) * (v[2]->pos.x - v[0]->pos.x);
        if (std::fabs(area) < 1e-6f) continue;
        const float minx = std::min({v[0]->pos.x, v[1]->pos.x, v[2]->pos.x}), maxx = std::max({v[0]->pos.x, v[1]->pos.x, v[2]->pos.x});
        const float miny = std::min({v[0]->pos.y, v[1]->pos.y, v[2]->pos.y}), maxy = std::max({v[0]->pos.y, v[1]->pos.y, v[2]->pos.y});
        const int x0 = std::max(cx0, int(std::floor(minx))), x1 = std::min(cx1, int(std::ceil(maxx)));
        const int y0 = std::max(cy0, int(std::floor(miny))), y1 = std::min(cy1, int(std::ceil(maxy)));
        for (int y = y0; y < y1; ++y)
          for (int x = x0; x < x1; ++x) {
            const float px = float(x) + 0.5f, py = float(y) + 0.5f;
            float w[3];
            for (int k = 0; k < 3; ++k) {
              const ImVec2 &a = v[(k + 1) % 3]->pos, &b = v[(k + 2) % 3]->pos;
              w[k] = ((b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x)) / area;
            }
            const float eps = -1e-5f;
            if (w[0] < eps || w[1] < eps || w[2] < eps) continue;
            // Top-left-ish rule: a pixel exactly on a shared edge belongs to the triangle where the weight is > 0 for the first zero edge.
            float u = 0, vv = 0, col[4] = {0, 0, 0, 0};
            for (int k = 0; k < 3; ++k) {
              u += w[k] * v[k]->uv.x, vv += w[k] * v[k]->uv.y;
              for (int c = 0; c < 4; ++c) col[c] += w[k] * float((v[k]->col >> (8 * c)) & 255) / 255.0f;
            }
            float t[4] = {1, 1, 1, 1};
            if (tex) Sample(tex, u, vv, t);
            const float a = col[3] * t[3];
            if (a <= 0) continue;
            uint8_t* d = &img.px[(size_t(y) * size_t(img.w) + size_t(x)) * 3];
            for (int c = 0; c < 3; ++c) {
              const float s = col[c] * t[c];
              d[c] = uint8_t(std::clamp(s * a * 255.0f + float(d[c]) * (1 - a) + 0.5f, 0.0f, 255.0f));
            }
          }
      }
    }
  }
}

// ---- a PNG encoder (stored deflate) for the sample album art ------------------------------------------------------------
uint32_t Crc(const uint8_t* p, size_t n, uint32_t c = 0) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t r = i;
      for (int k = 0; k < 8; ++k) r = r & 1 ? 0xEDB88320u ^ (r >> 1) : r >> 1;
      table[i] = r;
    }
    init = true;
  }
  c = ~c;
  for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 255] ^ (c >> 8);
  return ~c;
}
void Be32(std::vector<uint8_t>& o, uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) o.push_back(uint8_t(v >> s));
}
void Chunk(std::vector<uint8_t>& o, const char* type, const std::vector<uint8_t>& data) {
  Be32(o, uint32_t(data.size()));
  std::vector<uint8_t> td(type, type + 4);
  td.insert(td.end(), data.begin(), data.end());
  o.insert(o.end(), td.begin(), td.end());
  Be32(o, Crc(td.data(), td.size()));
}
std::vector<uint8_t> EncodePng(const Image& im) {
  std::vector<uint8_t> raw;
  for (int y = 0; y < im.h; ++y) {
    raw.push_back(0);
    raw.insert(raw.end(), im.px.begin() + size_t(y) * size_t(im.w) * 3, im.px.begin() + size_t(y + 1) * size_t(im.w) * 3);
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  uint32_t a = 1, b = 0;
  for (uint8_t c : raw) a = (a + c) % 65521, b = (b + a) % 65521;
  for (size_t off = 0; off < raw.size(); off += 65535) {
    const size_t n = std::min<size_t>(65535, raw.size() - off);
    z.push_back(off + n >= raw.size() ? 1 : 0);
    z.push_back(uint8_t(n)), z.push_back(uint8_t(n >> 8)), z.push_back(uint8_t(~n)), z.push_back(uint8_t((~n) >> 8));
    z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
  }
  Be32(z, (b << 16) | a);
  std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A}, ihdr;
  Be32(ihdr, uint32_t(im.w)), Be32(ihdr, uint32_t(im.h));
  ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
  Chunk(out, "IHDR", ihdr), Chunk(out, "IDAT", z), Chunk(out, "IEND", {});
  return out;
}

// A made-up "album cover": a colour gradient, a big circle and a stripe, different per seed (non-square on purpose: the view crops).
std::vector<uint8_t> MakeArt(int seed) {
  Image im;
  im.w = 300 + (seed % 3) * 60, im.h = 300;
  im.px.resize(size_t(im.w) * size_t(im.h) * 3);
  const float hue = float(seed) * 0.37f;
  for (int y = 0; y < im.h; ++y)
    for (int x = 0; x < im.w; ++x) {
      const float fx = float(x) / float(im.w), fy = float(y) / float(im.h);
      float r = 0.5f + 0.5f * std::sin(hue + fx * 3.0f), g = 0.5f + 0.5f * std::sin(hue + 2.0f + fy * 3.0f), b = 0.5f + 0.5f * std::sin(hue + 4.0f + (fx + fy) * 2.0f);
      const float dx = fx - 0.5f, dy = fy - 0.55f, d = std::sqrt(dx * dx + dy * dy);
      if (d < 0.28f) r = 1 - r * 0.4f, g = 1 - g * 0.4f, b = 1 - b * 0.4f;
      if (fy > 0.82f) r *= 0.25f, g *= 0.25f, b *= 0.25f;
      uint8_t* p = &im.px[(size_t(y) * size_t(im.w) + size_t(x)) * 3];
      p[0] = uint8_t(r * 255), p[1] = uint8_t(g * 255), p[2] = uint8_t(b * 255);
    }
  return EncodePng(im);
}

std::vector<uint8_t> ReadFilePrefix(const std::string& path, size_t n) {
  std::ifstream f(path, std::ios::binary);
  std::vector<uint8_t> v(n);
  f.read(reinterpret_cast<char*>(v.data()), std::streamsize(n));
  v.resize(size_t(f.gcount()));
  return v;
}

struct Scenario {
  const char* name;
  int W, H;
  bool touch;
  bool controller;
  bool game_font;
  int songs;        // 0 = empty library
  int cursor, selected, playing;
  int art_every;    // every Nth song has art (0 = none)
  bool long_names;
  bool scrolled;    // cursor far down (the view scrolls to it)
  float hover_row;  // PC mouse: hover this row's thumbnail (-1 = none)
  const char* message;
};

}  // namespace

int main(int argc, char** argv) {
  std::string out_dir = argc > 1 ? argv[1] : "out/l292";
  const std::string game_root = argc > 2 ? argv[2] : "";
  std::vector<uint8_t> pac;
  if (!game_root.empty()) pac = ReadFilePrefix(game_root + "/pac/menu/fontHD.pac", 0x40000);
  std::printf("menu font pac: %zu bytes\n", pac.size());

  const Scenario scenarios[] = {
      {"phone_2400x1080_8songs", 2400, 1080, true, false, true, 8, 0, 1, 2, 2, false, false, -1, ""},
      {"phone_2400x1080_40songs_scrolled", 2400, 1080, true, false, true, 40, 22, 5, -1, 3, false, true, -1, ""},
      {"phone_2400x1080_empty", 2400, 1080, true, false, true, 0, 0, -1, -1, 0, false, false, -1, ""},
      {"phone_2400x1080_longnames", 2400, 1080, true, false, true, 6, 0, 0, 1, 2, true, false, -1, "That song is gone"},
      {"phone_1920x1080_noart_playing", 1920, 1080, true, false, true, 5, 0, 2, 2, 0, false, false, -1, ""},
      {"pc_1920x1080_pad_8songs", 1920, 1080, false, true, true, 8, 2, 1, 2, 2, false, false, -1, ""},
      {"pc_1920x1080_keyboard_hover", 1920, 1080, false, false, true, 12, 3, 1, -1, 2, false, false, 5, ""},
      {"pc_2560x1440_40songs", 2560, 1440, false, true, true, 40, 9, 7, 9, 3, false, true, -1, ""},
      {"pc_2560x1080_ultrawide", 2560, 1080, false, true, true, 8, 0, 3, 4, 2, false, false, -1, ""},
      {"pc_1920x1080_empty", 1920, 1080, false, false, true, 0, 0, -1, -1, 0, false, false, -1, ""},
      {"pc_1920x1080_fallback_font", 1920, 1080, false, false, false, 8, 1, 1, 2, 2, false, false, -1, ""},
      {"phone_2400x1080_fallback_font", 2400, 1080, true, false, false, 8, 0, 1, 2, 2, false, false, -1, ""},
  };
  static const char* kNames[] = {"Kingdom", "Whatever", "Superstar", "Hell In A Cell", "I Am The Game", "No Chance In Hell", "Rise Above", "Burn In My Light"};
  static const char* kArtists[] = {"Silent Sonic", "Mark & Co", "", "The Moat", "Dog Years", "", "Field Notes", "Marcus L."};
  static const char* kLong[] = {"A Very Long Song Name That Does Not Fit On One Line Of The Picker At All.mp3",
                                "Superstar Entrance Theme (Extended Remastered Edition 2013) - Live",
                                "short", "Whatever",
                                "Entrance of the Year featuring Somebody With A Rather Long Artist Name",
                                "Kingdom"};

  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  unsigned char* atlas = nullptr;
  int aw = 0, ah = 0;
  io.Fonts->GetTexDataAsRGBA32(&atlas, &aw, &ah);  // the legacy path, as the SDK's ImGuiDrawer does
  g_tex.emplace_back();
  g_tex.back().w = aw, g_tex.back().h = ah, g_tex.back().px.assign(atlas, atlas + size_t(aw) * size_t(ah) * 4);
  io.Fonts->SetTexID(ImTextureID(reinterpret_cast<uintptr_t>(&g_tex.back())));

  int failures = 0;
  for (const Scenario& sc : scenarios) {
    // The font cache is per process: a run with the game's pac draws the game_font scenarios, a run without it the fallback ones.
    if (sc.game_font == pac.empty()) continue;
    ViewHost host;
    host.upload = Upload;
    if (sc.game_font && !pac.empty()) host.read_font_pac = [&pac] { return pac; };
    auto view = std::make_unique<ImGuiPickerView>(host);
    view->OnOpen();
    PickerModel m;
    m.title = "MUSIC LIBRARY";
    m.subtitle = "Choose the song for Brock Lesnar";
    m.empty = sc.songs == 0;
    if (m.empty) m.message = "No songs yet. Add MP3 files in the launcher: Mods > Music.";
    else if (sc.message && *sc.message) m.message = sc.message;
    for (int i = 0; i < sc.songs; ++i) {
      Song s;
      s.name = sc.long_names ? kLong[i % 6] : (i < 8 ? std::string(kNames[i]) : "Track " + std::to_string(i + 1));
      s.artist = i < 8 ? kArtists[i] : (i % 3 ? "Artist " + std::to_string(i) : "");
      if (sc.art_every && i % sc.art_every == 0) {
        s.art_id = uint32_t(i + 1);
        view->SetArt(s.art_id, MakeArt(i));
      }
      m.songs.push_back(s);
    }
    m.cursor = sc.cursor, m.selected = sc.selected, m.playing = sc.playing;
    FrameInfo f;
    f.width = float(sc.W), f.height = float(sc.H), f.touch = sc.touch, f.controller = sc.controller;
    io.DisplaySize = ImVec2(float(sc.W), float(sc.H));
    io.DeltaTime = 1.0f / 60.0f;
    Image img;
    img.w = sc.W, img.h = sc.H;
    img.px.assign(size_t(img.w) * size_t(img.h) * 3, 0);
    // The "menu underneath": a dark red-grey gradient so the dim layer and the panel edge are visible.
    for (int y = 0; y < img.h; ++y)
      for (int x = 0; x < img.w; ++x) {
        uint8_t* p = &img.px[(size_t(y) * size_t(img.w) + size_t(x)) * 3];
        p[0] = uint8_t(90 + 60 * y / img.h), p[1] = uint8_t(60 + 40 * x / img.w), p[2] = uint8_t(70);
      }
    const SafeArea sa = DefaultSafeArea(float(sc.W), float(sc.H), sc.touch);
    const Layout L = ComputeLayout(float(sc.W), float(sc.H), sc.touch, sa, sc.songs);
    for (int frame = 0; frame < 4; ++frame) {
      if (sc.hover_row >= 0) {
        const Rect t = L.ThumbRect(int(sc.hover_row), 0);
        io.AddMousePosEvent((t.x0 + t.x1) * 0.5f, (t.y0 + t.y1) * 0.5f);
      }
      ImGui::NewFrame();
      view->Draw(m, f);
      ImGui::Render();
      if (frame == 3) Raster(ImGui::GetDrawData(), img);
    }
    for (auto in : view->Poll()) (void)in;
    std::ofstream o(out_dir + "/" + sc.name + ".ppm", std::ios::binary);
    if (!o) {
      std::printf("cannot write %s/%s.ppm\n", out_dir.c_str(), sc.name);
      ++failures;
      continue;
    }
    o << "P6\n" << img.w << " " << img.h << "\n255\n";
    o.write(reinterpret_cast<const char*>(img.px.data()), std::streamsize(img.px.size()));
    std::printf("%s %dx%d rows=%d visible=%d u=%.2f ok\n", sc.name, sc.W, sc.H, sc.songs, L.rows_visible, L.u);
  }
  return failures;
}
