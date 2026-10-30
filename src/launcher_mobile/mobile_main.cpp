// wwe2k14 phone launcher (Session 71, ANDROID_PLAN F4; Jarrod 2026-10-28: "Port the PC launcher", both
// orientations as "enterable menus with a top left hamburger menu to get back").
//
// Dear ImGui on SDL3's renderer, in its own Android process (LauncherActivity); PLAY hands the game its command line
// and closes the launcher (the game runs in a fresh process, GameActivity). The look is the PC launcher's (Session 31,
// launcher_theme.h): the dotted dark background, the red band, slanted buttons, the game's menu font read from the
// player's own files (game_font.h) and the logo traced by Jarrod. The same file builds as a Windows test program
// (platform_desktop.cpp) so the pages can be checked in a phone-shaped window without a phone:
//   wwe2k14_launcher_mobile.exe [--window WxH] [--scale S] [--files DIR] [--screen WxH]
//                               [--page N] [--shot FILE.png] [--frames N] [--scroll PX] [--select N]
//                               [--tap X,Y] [--quit-ms N] [--check] [--share-test]
// --shot draws --frames frames (default 4) of page N, saves the window as a PNG and quits (tools\android\
// launcher_shots.ps1 uses it). --tap sends a touch tap at the 3rd frame, --quit-ms quits after N ms (Session 72: the
// frame pacing check; launcher.log shows when the page came and how many frames were drawn). --share-test makes
// SHARE's zip (<files>/logs/share/wwe2k14_report_NNN.zip, its path in launcher.log) without a share sheet and quits.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "imgui_internal.h"  // ClearActiveID (a touch drag that scrolls must not press the button it started on)

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

#include "../dlc_catalog.h"
#include "../launcher/disc_image.h"
#include "../launcher/dlc_names.h"
#include "../launcher/game_font.h"
#include "../launcher/launcher_theme.h"
#include "../launcher/update_check.h"
#include "../launcher/wm30_list.h"
#include "mobile_platform.h"
#include "mobile_settings.h"
#include "phone_check.h"  // Session 73: CHECK THIS PHONE and how the last game run ended
#include "share_report.h"  // Session 73: report.txt in the shared zip
#include "zip_writer.h"

namespace mlauncher {

namespace theme = launcher::theme;

#ifdef WWE2K14_VERSION
constexpr const char* kVersion = WWE2K14_VERSION;  // android\VERSION
#else
constexpr const char* kVersion = "android-dev";
#endif

enum Page { kHome, kGameFiles, kGraphics, kTouch, kDlc, kWm30, kSaves, kHelp, kPageCount };
constexpr const char* kPageNames[kPageCount] = {"HOME",   "GAME FILES", "GRAPHICS", "TOUCH CONTROLS",
                                                "DLC",    "30 YEARS",   "SAVES",    "LOGS AND UPDATES"};

// A disc image by its extension, any case.
inline bool IsIso(const fs::path& p) {
  std::string e = U8(p.extension());
  for (auto& c : e) c = char(std::tolower(static_cast<unsigned char>(c)));
  return e == ".iso";
}

constexpr ImU32 kMenuRed = IM_COL32(214, 22, 30, 255);
constexpr ImVec4 kGood{0.45f, 0.85f, 0.45f, 1}, kBad{1, 0.55f, 0.4f, 1}, kDim{0.62f, 0.63f, 0.68f, 1};

struct Texture {
  SDL_Texture* tex = nullptr;
  int w = 0, h = 0;
  void Reset() {
    if (tex) SDL_DestroyTexture(tex);
    tex = nullptr, w = h = 0;
  }
  ImTextureRef Ref() const { return ImTextureRef(ImTextureID(reinterpret_cast<intptr_t>(tex))); }
};

Texture MakeTexture(SDL_Renderer* r, const std::vector<uint8_t>& rgba, int w, int h,
                    SDL_BlendMode blend = SDL_BLENDMODE_BLEND) {
  Texture t;
  if (w <= 0 || h <= 0 || rgba.size() < size_t(w) * size_t(h) * 4) return t;
  t.tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h);  // bytes R, G, B, A
  if (!t.tex) return t;
  SDL_UpdateTexture(t.tex, nullptr, rgba.data(), w * 4);
  SDL_SetTextureBlendMode(t.tex, blend);
  SDL_SetTextureScaleMode(t.tex, SDL_SCALEMODE_LINEAR);
  t.w = w, t.h = h;
  return t;
}

// New pixels for `t`: into the texture it has when the size matches (Session 72: no new texture per page change).
void SetPixels(SDL_Renderer* r, Texture& t, const std::vector<uint8_t>& rgba, int w, int h, SDL_BlendMode blend) {
  if (t.tex && t.w == w && t.h == h && rgba.size() >= size_t(w) * size_t(h) * 4) {
    SDL_UpdateTexture(t.tex, nullptr, rgba.data(), w * 4);
    return;
  }
  t.Reset();
  t = MakeTexture(r, rgba, w, h, blend);
}

// What the PC test build was asked to do (--page / --shot ...).
struct TestSteps {
  int page = -1, frames = 4, select = -1;
  bool drawer = false;  // --drawer: the sidebar open
  float scroll = 0;
  std::string shot;
  float tap_x = -1, tap_y = -1;  // --tap X,Y: a touch tap there (position, down, up in one batch) at the 3rd frame
  int quit_ms = 0;               // --quit-ms N: quit N ms after start
  bool check = false;            // --check: CHECK THIS PHONE at start (Session 73)
  bool share = false;            // --share-test: SHARE's zip and report, no share sheet, then quit (Session 73)
};

class App {
 public:
  App(SDL_Window* window, SDL_Renderer* renderer, float ui) : window_(window), renderer_(renderer), ui_(ui) {}

  void Start(const TestSteps& test) {
    test_ = test;
    files_ = platform::FilesDir();
    std::error_code ec;
    fs::create_directories(files_ / "logs", ec);
    log_.open(files_ / "logs" / "launcher.log", std::ios::trunc);
    Log("wwe2k14 phone launcher %s on %s, files %s, ui scale %.2f", kVersion, platform::Name(), U8(files_).c_str(), ui_);
    s_ = LoadSettings(SettingsPath());
    ScanDrivers();
    CheckDriver();
    for (const auto& d : kDrivers) driver_bundled_.push_back(DriverBundled(d.file));
    if (!driver_ask_.empty()) page_ = kGraphics;  // Session 73: the question at the top of GRAPHICS
    RefreshGame();
    // Session 73: how the last game run ended (src\blackbox.h; LauncherActivity wrote Android's reason first).
    last_run_ = check::LastRun(files_);
    if (!last_run_.empty()) {
      Log("last game run: %s", last_run_.c_str());
      ShowToast(last_run_, 10000);
    }
    if (s_.check_updates && test_.shot.empty() && !test_.share) CheckUpdate();
    if (test_.check) RunCheck();
    if (test_.share) ShareLogs(false);
    if (test_.page >= 0 && test_.page < kPageCount) page_ = Page(test_.page);
    if (page_ == kSaves) ScanSaves();
    if (test_.select >= 0) wm30_pick_ = test_.select;
    if (test_.drawer) drawer_open_ = true;
  }

  // One frame. Returns false once the PC test's screenshot is saved.
  bool Frame() {
    for (platform::Event e; platform::PollEvent(e);) OnEvent(e);
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 size = io.DisplaySize;
    SDL_Rect safe{0, 0, int(size.x), int(size.y)};
    SDL_GetWindowSafeArea(window_, &safe);  // the camera hole / rounded corners / navigation bar
    safe_min_ = ImVec2(float(safe.x), float(safe.y));
    safe_max_ = ImVec2(float(safe.x + safe.w), float(safe.y + safe.h));
    if (safe.w <= 0 || safe.h <= 0) safe_min_ = ImVec2(0, 0), safe_max_ = size;
    landscape_ = size.x > size.y;

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    if (page_ == kHome) HomePage(size);
    else SectionPage(size);
    Toast(size);
    ImGui::End();
    Drawer(size);
    return true;
  }

  // Android's Back, Escape on the PC: closes the sidebar, else one press from any section to the main page, else
  // leaves the app. A second Back within 250 ms is the same press arriving twice (Android's back callback and SDL's
  // Back key; Jarrod, 2026-10-28: "Should only take one back input").
  void Back() {
    const uint64_t now = SDL_GetTicks();
    if (now - last_back_ < 250) return;
    last_back_ = now;
    if (drawer_open_) drawer_open_ = false;
    else if (page_ != kHome) GoPage(kHome);
    else platform::Quit();
  }

  const TestSteps& test() const { return test_; }
  const char* PageName() const { return kPageNames[page_]; }

  // Session 72: the next frame must come at once: the sidebar is sliding, ImGui still holds input it has not used (it
  // spreads a tap's finger position, down and up over three frames), or a gamepad button is held (the menus repeat).
  bool Busy() const {
    if (!test_.drawer && drawer_t_ != (drawer_open_ ? 1.f : 0.f)) return true;
    if (ImGui::GetCurrentContext()->InputEventsQueue.Size > 0) return true;
    for (int k = ImGuiKey_GamepadStart; k <= ImGuiKey_GamepadRStickDown; ++k)
      if (ImGui::IsKeyDown(ImGuiKey(k))) return true;
    return false;
  }
  // How long the launcher may sleep with no input: until the note at the bottom goes, else until the next event (-1).
  int IdleWaitMs() const {
    const uint64_t now = SDL_GetTicks();
    if (!toast_.empty() && now <= toast_until_) return int(toast_until_ - now) + 1;
    return -1;
  }

  void Log(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    SDL_Log("wwe2k14 launcher: %s", buf);
    char at[24];
    std::snprintf(at, sizeof(at), "%8.3f ", double(SDL_GetTicks()) / 1000.0);  // seconds since the launcher started
    if (log_) log_ << at << buf << "\n", log_.flush();
  }

  // The renderer lost its textures (Android may drop the GL context while the app is in the background).
  void DeviceReset() {
    bg_.Reset(), logo_.Reset(), gfont_tex_.Reset();
    for (Band& b : bands_) b.tex.Reset(), b.made = false;
    font_tried_ = logo_tried_ = false;
  }

 private:
  float D(float v) const { return v * ui_; }  // density-independent pixels -> ImGui's

  fs::path SettingsPath() const { return files_ / "launcher_settings.json"; }
  fs::path DlcDir() const { return files_ / "dlc"; }
  fs::path UsbGameDir() const { return files_ / "game"; }
  // The game's save folder: the SDK's default user folder (HOME/.local/share/wwe2k14 on the phone, where the activity
  // sets HOME to the app folder: SDK windowed_app_main_sdl.cpp, filesystem_posix.cpp GetUserFolder).
  fs::path SaveDir() const { return files_ / ".local" / "share" / "wwe2k14"; }
  void Save() {
    if (!SaveSettings(SettingsPath(), s_)) Log("settings: could not write %s", U8(SettingsPath()).c_str());
  }

  void GoPage(Page p) {
    if (p != page_) Log("page: %s", kPageNames[p]);
    page_ = p;
    drawer_open_ = false;
    scroll_reset_ = true;
    if (p == kDlc && !dlc_scanned_) ScanDlc();
    if (p == kSaves) ScanSaves();
  }

  // ---------------------------------------------------------------- game files
  // The picked file wins; else <files>/game over USB (default.xex there = an extracted copy, else the first .iso).
  void RefreshGame() {
    read_path_.clear(), address_.clear(), game_from_usb_ = false;
    game_error_.clear();
    if (!s_.game_uri.empty()) {
      read_path_ = platform::OpenGameFile(s_.game_uri);
      if (read_path_.empty()) game_error_ = "The chosen file can't be opened (moved, deleted or on a removed card?)";
      else address_ = s_.game_uri;
    }
    if (read_path_.empty()) {
      std::error_code ec;
      fs::path usb;
      if (fs::is_regular_file(UsbGameDir() / "default.xex", ec)) usb = UsbGameDir();
      else
        for (const auto& e : fs::directory_iterator(UsbGameDir(), ec))
          if (e.is_regular_file(ec) && IsIso(e.path())) {
            usb = e.path();
            break;
          }
      if (!usb.empty()) read_path_ = address_ = U8(usb), game_from_usb_ = true;
    }
    files_reader_.SetRoot(fs::u8path(read_path_));
    game_ok_ = !read_path_.empty() && files_reader_.Exists("default.xex");
    if (!read_path_.empty() && !game_ok_)
      game_error_ = files_reader_.image_error().empty() ? "No default.xex in these game files"
                                                        : "Not a readable disc image: " + files_reader_.image_error();
    Log("game files: %s (%s) %s", read_path_.empty() ? "none" : read_path_.c_str(),
        game_from_usb_ ? "USB folder" : "picked", game_ok_ ? "ok" : game_error_.c_str());
    font_tried_ = false, gfont_tex_.Reset();
    wm30_ = {}, wm30_pick_ = -1;
  }

  // ---------------------------------------------------------------- menu font (game_font.h) and pictures
  void LoadMenuFont() {
    if (font_tried_) return;
    font_tried_ = true;
    if (!game_ok_ || !launcher::LoadGameFont(files_reader_, gfont_)) {
      Log("menu font: not in the game files; using the fallback");
      return;
    }
    gfont_tex_ = MakeTexture(renderer_, launcher::DecodeBc3(gfont_.bc3, gfont_.sheet_w, gfont_.sheet_h),
                             gfont_.sheet_w, gfont_.sheet_h);
    gfont_.bc3 = {};
    Log("menu font: the game's font000 (%zu glyphs)", gfont_.glyphs.size());
  }

  static uint32_t NextCodepoint(const char*& p) {
    const auto c = static_cast<unsigned char>(*p++);
    if (c < 0x80) return c;
    const int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    uint32_t cp = c & (0x3Fu >> extra);
    for (int i = 0; i < extra && (static_cast<unsigned char>(*p) & 0xC0) == 0x80; ++i) cp = cp << 6 | (*p++ & 0x3F);
    return cp;
  }
  ImVec2 MenuTextSize(float px, const char* text) {
    LoadMenuFont();
    if (gfont_tex_.tex) {
      const float k = px / gfont_.cell;
      float w = 0;
      for (const char* p = text; *p;) {
        const auto it = gfont_.glyphs.find(NextCodepoint(p));
        w += (it != gfont_.glyphs.end() ? it->second.w : gfont_.cell * 0.33f) * k;
      }
      return ImVec2(w, px);
    }
    ImGui::PushFont(menu_font_, px * 0.8f);
    const ImVec2 sz = ImGui::CalcTextSize(text);
    ImGui::PopFont();
    return ImVec2(sz.x, px);
  }
  void MenuTextRaw(ImDrawList* dl, ImVec2 pos, float px, ImU32 col, const char* text) {
    LoadMenuFont();
    if (!gfont_tex_.tex) {
      ImGui::PushFont(menu_font_, px * 0.8f);
      dl->AddText(ImVec2(pos.x, pos.y + px * 0.08f), col, text);
      ImGui::PopFont();
      return;
    }
    const float k = px / gfont_.cell, iw = 1.f / float(gfont_.sheet_w), ih = 1.f / float(gfont_.sheet_h);
    float x = std::floor(pos.x);
    const float y = std::floor(pos.y);
    for (const char* p = text; *p;) {
      const auto it = gfont_.glyphs.find(NextCodepoint(p));
      if (it == gfont_.glyphs.end()) {
        x += gfont_.cell * 0.33f * k;
        continue;
      }
      const launcher::GameFont::Glyph& g = it->second;
      dl->AddImage(gfont_tex_.Ref(), ImVec2(x, y), ImVec2(x + g.w * k, y + g.h * k), ImVec2(g.x * iw, g.y * ih),
                   ImVec2((g.x + g.w) * iw, (g.y + g.h) * ih), col);
      x += g.w * k;
    }
  }
  void MenuText(ImDrawList* dl, ImVec2 pos, float px, ImU32 col, const char* text) {
    const float o = std::max(1.f, px * 0.06f);
    MenuTextRaw(dl, ImVec2(pos.x + o, pos.y + o), px, IM_COL32(0, 0, 0, 190), text);
    MenuTextRaw(dl, pos, px, col, text);
  }

  // The logo (assets\launcher\wwe2k14_logo.png: in the APK's assets, next to the PC test exe), at most `width` wide.
  ImVec2 DrawLogo(ImDrawList* dl, ImVec2 center_top, float width) {
    if (!logo_tried_) {
      logo_tried_ = true;
      SDL_Surface* s = SDL_LoadPNG("wwe2k14_logo.png");
      if (!s) {
        const char* base = SDL_GetBasePath();
        if (base) s = SDL_LoadPNG((std::string(base) + "wwe2k14_logo.png").c_str());
      }
      if (s) {
        if (SDL_Surface* c = SDL_ConvertSurface(s, SDL_PIXELFORMAT_ABGR8888)) {
          logo_px_.assign(static_cast<uint8_t*>(c->pixels), static_cast<uint8_t*>(c->pixels) + size_t(c->pitch) * c->h);
          logo_w_ = c->w, logo_h_ = c->h;
          if (c->pitch != c->w * 4) logo_px_.clear();  // ABGR8888 rows are packed; anything else is unexpected
          SDL_DestroySurface(c);
        }
        SDL_DestroySurface(s);
      }
      if (logo_px_.empty()) Log("logo: wwe2k14_logo.png not found (%s)", SDL_GetError());
    }
    if (logo_px_.empty()) {
      const float px = std::min(width / 6.f, D(64));
      const ImVec2 ts = MenuTextSize(px, "WWE 2K14");
      MenuText(dl, ImVec2(center_top.x - ts.x / 2, center_top.y), px, IM_COL32_WHITE, "WWE 2K14");
      return ts;
    }
    const int w = std::clamp(int(width) / 8 * 8, 16, logo_w_);
    const int h = std::max(1, int(std::lround(double(logo_h_) * w / logo_w_)));
    if (!logo_.tex || logo_.w != w)
      logo_.Reset(), logo_ = MakeTexture(renderer_, w == logo_w_ ? logo_px_ : theme::Resample(logo_px_.data(), logo_w_, logo_h_, w, h), w, h);
    const ImVec2 p(std::floor(center_top.x - w / 2.f), std::floor(center_top.y));
    dl->AddImage(logo_.Ref(), p, ImVec2(p.x + float(w), p.y + float(h)));
    return ImVec2(float(w), float(h));
  }

  // The dots: one opaque picture for the whole screen, made once per screen size. The red band: a picture of just its
  // box, blended over the dots (theme::PanelPicture), one kept for the main page and one for the sections, so a page
  // change remakes at most a header-sized picture. Session 72: before, every page change remade the whole screen's
  // picture (1440 x 3168 on Jarrod's phone, 18 MB) and a new texture for it.
  enum BandSlot { kBandHome, kBandSection, kBandSlots };
  void DrawBackground(ImVec2 size, const theme::Panel& p, BandSlot slot) {
    const int w = int(size.x), h = int(size.y);
    if (w <= 0 || h <= 0) return;
    const float dpi = ui_ * 0.9f;
    if (!bg_.tex || bg_.w != w || bg_.h != h) {
      const uint64_t t0 = SDL_GetTicksNS();
      SetPixels(renderer_, bg_, theme::Background(w, h, dpi, theme::Panel{}), w, h, SDL_BLENDMODE_NONE);
      Log("background: %d x %d made in %.1f ms", w, h, double(SDL_GetTicksNS() - t0) / 1e6);
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (bg_.tex) dl->AddImage(bg_.Ref(), ImVec2(0, 0), ImVec2(float(w), float(h)));
    Band& b = bands_[slot];
    const std::array<float, 8> key{size.x, size.y, p.x0, p.y0, p.w, p.h, p.slant, p.straight_left ? 1.f : 0.f};
    if (!b.made || key != b.key) {
      int bw = 0, bh = 0;
      const std::vector<uint8_t> px = theme::PanelPicture(w, h, dpi, p, b.x, b.y, bw, bh);
      if (bw > 0 && bh > 0) SetPixels(renderer_, b.tex, px, bw, bh, SDL_BLENDMODE_BLEND);
      else b.tex.Reset();
      b.key = key, b.made = true;
    }
    if (b.tex.tex)
      dl->AddImage(b.tex.Ref(), ImVec2(float(b.x), float(b.y)), ImVec2(float(b.x + b.tex.w), float(b.y + b.tex.h)));
  }

  // A slanted button (the PC's PLAY GAME tile): red, white rim, the label in the menu font, shrunk to fit.
  bool SlantButton(const char* label, ImVec2 pos, ImVec2 size, bool enabled, float font_px) {
    const float sk = size.y * 0.22f;
    ImGui::SetCursorScreenPos(pos);
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::InvisibleButton(label, size);
    ImGui::EndDisabled();
    const bool held = enabled && ImGui::IsItemActive();
    ImU32 top = IM_COL32(160, 20, 28, 255), bottom = IM_COL32(82, 8, 12, 255);
    if (!enabled) top = IM_COL32(66, 66, 72, 255), bottom = IM_COL32(40, 40, 45, 255);
    else if (held) top = IM_COL32(205, 30, 38, 255), bottom = IM_COL32(118, 12, 18, 255);
    const ImVec2 q[4] = {{pos.x + sk, pos.y}, {pos.x + size.x, pos.y}, {pos.x + size.x - sk, pos.y + size.y},
                         {pos.x, pos.y + size.y}};
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 uv = dl->_Data->TexUvWhitePixel;
    const auto i0 = ImDrawIdx(dl->_VtxCurrentIdx);
    dl->PrimReserve(6, 4);  // a top-to-bottom gradient quad
    dl->PrimWriteIdx(i0), dl->PrimWriteIdx(ImDrawIdx(i0 + 1)), dl->PrimWriteIdx(ImDrawIdx(i0 + 2));
    dl->PrimWriteIdx(i0), dl->PrimWriteIdx(ImDrawIdx(i0 + 2)), dl->PrimWriteIdx(ImDrawIdx(i0 + 3));
    dl->PrimWriteVtx(q[0], uv, top), dl->PrimWriteVtx(q[1], uv, top);
    dl->PrimWriteVtx(q[2], uv, bottom), dl->PrimWriteVtx(q[3], uv, bottom);
    dl->AddPolyline(q, 4, enabled ? IM_COL32_WHITE : IM_COL32(150, 150, 156, 255), ImDrawFlags_Closed, D(2.5f));
    const std::string text(label, ImGui::FindRenderedTextEnd(label));
    ImVec2 ts = MenuTextSize(font_px, text.c_str());
    const float room = size.x - 2 * sk - D(12);
    if (ts.x > room) font_px *= room / ts.x, ts = MenuTextSize(font_px, text.c_str());
    MenuText(dl, ImVec2(pos.x + (size.x - ts.x) / 2, pos.y + (size.y - ts.y) / 2), font_px,
             enabled ? IM_COL32_WHITE : IM_COL32(170, 170, 176, 255), text.c_str());
    return pressed;
  }

  // ---------------------------------------------------------------- the main page
  // Portrait: the logo, PLAY and the sections on the red band below. Landscape: the band on the left (the PC
  // launcher's sidebar) with the sections, the logo and PLAY on the right.
  void HomePage(ImVec2 size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    static constexpr Page kItems[] = {kGameFiles, kGraphics, kTouch, kDlc, kWm30, kSaves, kHelp};
    const int n = int(std::size(kItems));
    // Landscape: the rows share the height (a phone on its side is ~410 dp tall).
    const float row = landscape_ ? std::min(D(54), (safe_max_.y - safe_min_.y - D(36)) / float(n)) : D(54);
    const float menu_px = std::min(D(30), row * 0.56f);
    theme::Panel band;
    ImVec2 hero_min, hero_max;  // where the logo and PLAY go
    if (landscape_) {
      band.x0 = 0, band.y0 = 0, band.h = size.y, band.straight_left = true;
      band.w = std::min(size.x * 0.46f, D(430)) + safe_min_.x;
      band.slant = std::min(band.h * 0.16f, band.w * 0.25f);
      hero_min = ImVec2(band.w, safe_min_.y), hero_max = ImVec2(safe_max_.x, safe_max_.y);
    } else {
      const float list_h = row * n + D(28);
      band.x0 = 0, band.straight_left = true, band.h = list_h + (size.y - safe_max_.y) + D(48);
      band.y0 = size.y - band.h, band.w = size.x, band.slant = 0;
      hero_min = safe_min_, hero_max = ImVec2(safe_max_.x, band.y0);
    }
    DrawBackground(size, band, kBandHome);
    // The red band's top edge leans in portrait (the game's panels): a dark wedge over its right end.
    if (!landscape_) {
      const ImVec2 wedge[3] = {{size.x * 0.35f, band.y0 - 1}, {size.x + 1, band.y0 - 1}, {size.x + 1, band.y0 + D(40)}};
      dl->AddTriangleFilled(wedge[0], wedge[1], wedge[2], IM_COL32(17, 18, 21, 255));
      dl->AddLine(ImVec2(0, band.y0 + D(1)), ImVec2(size.x * 0.35f, band.y0 + D(1)), IM_COL32(255, 255, 255, 150), D(1.5f));
      dl->AddLine(ImVec2(size.x * 0.35f, band.y0 + D(1)), ImVec2(size.x, band.y0 + D(41)), IM_COL32(255, 255, 255, 150), D(1.5f));
    } else {
      dl->AddLine(ImVec2(band.x0 + band.w, band.y0), ImVec2(band.x0 + band.w - band.slant, band.y0 + band.h),
                  IM_COL32(255, 255, 255, 170), D(1.6f));
    }

    // The hero: logo, PLAY, the game files' state.
    const float hw = hero_max.x - hero_min.x, hh = hero_max.y - hero_min.y;
    const float play_h = D(76), play_w = std::min(hw - D(48), D(420));
    const float logo_w = std::min({hw - D(56), D(560), (hh - play_h - D(110)) * 1280.f / 617.f});
    const float logo_h = logo_px_.empty() ? D(64) : logo_w * float(logo_h_ > 0 ? logo_h_ : 617) / float(logo_w_ > 0 ? logo_w_ : 1280);
    const float block = logo_h + D(28) + play_h + D(14) + ImGui::GetTextLineHeight();
    float y = hero_min.y + std::max(D(16), (hh - block) / 2);
    DrawLogo(dl, ImVec2(hero_min.x + hw / 2, y), logo_w);
    y += logo_h + D(28);
    if (SlantButton("PLAY", ImVec2(hero_min.x + (hw - play_w) / 2, y), ImVec2(play_w, play_h), true, D(44))) Play();
    y += play_h + D(14);
    {
      const std::string st = game_ok_ ? "Game files: " + GameName() : "Choose your game files to play";
      const ImVec2 ts = ImGui::CalcTextSize(st.c_str());
      ImGui::SetCursorScreenPos(ImVec2(hero_min.x + (hw - std::min(ts.x, hw)) / 2, y));
      ImGui::PushStyleColor(ImGuiCol_Text, game_ok_ ? kDim : kBad);
      ImGui::TextUnformatted(st.c_str());
      ImGui::PopStyleColor();
    }

    // The sections: rows on the band, each a little further left down the slant (the game's staircase).
    const float top = landscape_ ? std::max(safe_min_.y + D(10), (size.y - row * n) / 2) : band.y0 + D(36);
    const float left = landscape_ ? safe_min_.x + D(26) : safe_min_.x + D(26);
    for (int i = 0; i < n; ++i) {
      const float y0 = top + row * float(i), y1 = y0 + row, yc = (y0 + y1) / 2;
      const float x_left = landscape_ ? band.Left(y0) : 0, x_right = landscape_ ? band.Right(y0) : size.x;
      ImGui::SetCursorScreenPos(ImVec2(x_left, y0));
      ImGui::PushID(i);
      const bool pressed = ImGui::InvisibleButton("##nav", ImVec2(x_right - x_left, row));
      ImGui::PopID();
      const bool held = ImGui::IsItemActive() && !dragging_;
      if (held || ImGui::IsItemFocused()) {
        const float r0 = landscape_ ? band.Right(y0) : size.x, r1 = landscape_ ? band.Right(y1) : size.x;
        const ImVec2 bar[4] = {{x_left, y0}, {r0, y0}, {r1, y1}, {x_left, y1}};
        dl->AddConvexPolyFilled(bar, 4, IM_COL32(0, 0, 0, held ? 255 : 120));
      }
      const float step = landscape_ ? band.Right(top) - band.Right(yc) : 0;
      const ImVec2 at(left + D(8) - step * 0.0f, yc - menu_px / 2);
      if (held) MenuTextRaw(dl, at, menu_px, kMenuRed, kPageNames[kItems[i]]);
      else MenuText(dl, at, menu_px, IM_COL32_WHITE, kPageNames[kItems[i]]);
      // A chevron at the row's right end: it opens.
      const float cx = (landscape_ ? band.Right(yc) - band.slant * 0.15f : safe_max_.x) - D(30);
      dl->AddTriangleFilled(ImVec2(cx, yc - D(8)), ImVec2(cx + D(9), yc), ImVec2(cx, yc + D(8)), IM_COL32(255, 255, 255, 200));
      if (pressed && !drag_cancelled_) GoPage(kItems[i]);
    }
    // The version: at the band's foot (portrait), the screen's bottom right (landscape).
    const float vx = landscape_ ? safe_max_.x - ImGui::CalcTextSize(kVersion).x - D(16) : left;
    ImGui::SetCursorScreenPos(ImVec2(vx, safe_max_.y - ImGui::GetTextLineHeight() - D(8)));
    ImGui::TextColored(ImVec4(1, 1, 1, 0.6f), "%s", kVersion);
  }

  // ---------------------------------------------------------------- a section: the header with the hamburger, then
  // the page in a scrolling area (touch drags scroll it).
  void SectionPage(ImVec2 size) {
    const float head = D(64);
    const ImVec2 hb(safe_min_.x + D(6), safe_min_.y + D(6));
    // The red band reaches past the title (Jarrod: "TOUCH CONTROLS" / "LOGS AND UPDATES" ran past it); the title
    // shrinks when the screen is too narrow for it.
    theme::Panel band;
    band.x0 = 0, band.y0 = 0, band.h = safe_min_.y + head, band.straight_left = true;
    band.slant = band.h * 0.5f;
    const float title_x = hb.x + D(64);
    float title_px = D(32), title_w = MenuTextSize(title_px, kPageNames[page_]).x;
    const float room = size.x - title_x - band.slant - D(16);
    if (title_w > room) title_px *= room / title_w, title_w = room;
    band.w = std::clamp(title_x + title_w + D(28) + band.slant, std::min(size.x, D(300)), size.x);
    DrawBackground(size, band, kBandSection);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddLine(ImVec2(band.w, 0), ImVec2(band.w - band.slant, band.h), IM_COL32(255, 255, 255, 170), D(1.6f));

    // The hamburger: the sidebar with HOME and every section (Jarrod, 2026-10-28).
    ImGui::SetCursorScreenPos(hb);
    if (ImGui::InvisibleButton("##menu", ImVec2(D(52), D(52)))) drawer_open_ = true;
    {
      const bool held = ImGui::IsItemActive();
      if (held) dl->AddRectFilled(hb, ImVec2(hb.x + D(52), hb.y + D(52)), IM_COL32(0, 0, 0, 120), D(4));
      HamburgerIcon(dl, hb);
    }
    MenuText(dl, ImVec2(title_x, safe_min_.y + (head - title_px) / 2), title_px, IM_COL32_WHITE, kPageNames[page_]);

    // The page: at most ~760 dp wide, centred (landscape gets wider pages, not a different layout).
    const float pad = D(16), max_w = D(760);
    const float avail = safe_max_.x - safe_min_.x - 2 * pad;
    const float w = std::min(avail, max_w);
    const float x0 = safe_min_.x + pad + (avail - w) / 2;
    float bottom_bar = (page_ == kWm30) ? D(92) : 0;
    ImGui::SetCursorScreenPos(ImVec2(x0, band.h + D(8)));
    ImGui::BeginChild("##page", ImVec2(w, safe_max_.y - band.h - D(8) - bottom_bar), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar);
    if (scroll_reset_) ImGui::SetScrollY(test_.scroll), scroll_reset_ = false;
    TouchScroll();
    switch (page_) {
      case kGameFiles: GameFilesPage(); break;
      case kGraphics: GraphicsPage(); break;
      case kTouch: TouchPage(); break;
      case kDlc: DlcPage(); break;
      case kWm30: Wm30Page(); break;
      case kSaves: SavesPage(); break;
      case kHelp: HelpPage(); break;
      default: break;
    }
    ImGui::Dummy(ImVec2(1, D(24)));
    ImGui::EndChild();
    if (page_ == kWm30) Wm30Bar(ImVec2(x0, safe_max_.y - bottom_bar), w);
  }

  void HamburgerIcon(ImDrawList* dl, ImVec2 at) {
    for (int i = 0; i < 3; ++i) {
      const float ly = at.y + D(17) + D(9) * i;
      dl->AddRectFilled(ImVec2(at.x + D(13), ly), ImVec2(at.x + D(39), ly + D(3)), IM_COL32_WHITE, D(1.5f));
    }
  }

  // The sidebar the hamburger opens (Jarrod, 2026-10-28: "the burger menu actually opening a sidebar that would get you
  // back to the other menus, home button at top"): a red band from the left like the PC launcher's page list, HOME on
  // top, then every section; the open one a black bar with red text. A tap beside it, the hamburger again or Back
  // closes it.
  void Drawer(ImVec2 size) {
    const float dt = std::min(ImGui::GetIO().DeltaTime, 0.05f);
    drawer_t_ = std::clamp(drawer_t_ + (drawer_open_ ? dt : -dt) / 0.16f, 0.f, 1.f);
    if (test_.drawer) drawer_t_ = 1;
    if (drawer_t_ <= 0) return;
    const float ease = 1 - (1 - drawer_t_) * (1 - drawer_t_);
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##drawer", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(0, 0), size, IM_COL32(0, 0, 0, int(160 * ease)));

    static constexpr Page kItems[] = {kHome, kGameFiles, kGraphics, kTouch, kDlc, kWm30, kSaves, kHelp};
    const int n = int(std::size(kItems));
    const float w = std::min(D(330), size.x * 0.84f) + safe_min_.x, off = -(1 - ease) * w;
    theme::Panel p;
    p.x0 = off, p.y0 = 0, p.w = w, p.h = size.y, p.straight_left = true;
    p.slant = std::min(size.y * 0.12f, w * 0.22f);
    {  // the band: the game's red, top to bottom, with the light rim on its leaning edge
      const ImVec2 q[4] = {{p.x0, 0}, {p.x0 + p.w, 0}, {p.x0 + p.w - p.slant, size.y}, {p.x0, size.y}};
      const ImU32 top = IM_COL32(int(theme::kRedTop.r * 255), int(theme::kRedTop.g * 255), int(theme::kRedTop.b * 255), 255);
      const ImU32 bottom =
          IM_COL32(int(theme::kRedBottom.r * 255), int(theme::kRedBottom.g * 255), int(theme::kRedBottom.b * 255), 255);
      const ImVec2 uv = dl->_Data->TexUvWhitePixel;
      const auto i0 = ImDrawIdx(dl->_VtxCurrentIdx);
      dl->PrimReserve(6, 4);
      dl->PrimWriteIdx(i0), dl->PrimWriteIdx(ImDrawIdx(i0 + 1)), dl->PrimWriteIdx(ImDrawIdx(i0 + 2));
      dl->PrimWriteIdx(i0), dl->PrimWriteIdx(ImDrawIdx(i0 + 2)), dl->PrimWriteIdx(ImDrawIdx(i0 + 3));
      dl->PrimWriteVtx(q[0], uv, top), dl->PrimWriteVtx(q[1], uv, top);
      dl->PrimWriteVtx(q[2], uv, bottom), dl->PrimWriteVtx(q[3], uv, bottom);
      dl->AddLine(q[1], q[2], IM_COL32(255, 255, 255, 170), D(1.6f));
    }
    // The hamburger again, in the same place: it closes the sidebar.
    const ImVec2 hb(off + safe_min_.x + D(6), safe_min_.y + D(6));
    ImGui::SetCursorScreenPos(hb);
    if (ImGui::InvisibleButton("##close", ImVec2(D(52), D(52)))) drawer_open_ = false;
    HamburgerIcon(dl, hb);
    const float top = safe_min_.y + D(72);
    const float row = std::min(D(56), (safe_max_.y - top - D(12)) / float(n));
    const float px = std::min(D(28), row * 0.55f);
    for (int i = 0; i < n; ++i) {
      const float y0 = top + row * float(i), y1 = y0 + row, yc = (y0 + y1) / 2;
      ImGui::SetCursorScreenPos(ImVec2(p.x0, y0));
      ImGui::PushID(i);
      const bool pressed = ImGui::InvisibleButton("##item", ImVec2(p.Right(y1) - p.x0, row));
      ImGui::PopID();
      const bool on = page_ == kItems[i], held = ImGui::IsItemActive();
      if (on || held) {
        const ImVec2 bar[4] = {{p.x0, y0}, {p.Right(y0), y0}, {p.Right(y1), y1}, {p.x0, y1}};
        dl->AddConvexPolyFilled(bar, 4, IM_COL32(0, 0, 0, on ? 255 : 120));
      }
      const ImVec2 at(p.x0 + safe_min_.x + D(24), yc - px / 2);
      if (on) MenuTextRaw(dl, at, px, kMenuRed, kPageNames[kItems[i]]);
      else MenuText(dl, at, px, IM_COL32_WHITE, kPageNames[kItems[i]]);
      if (pressed && drawer_t_ >= 1) GoPage(kItems[i]);
    }
    // A tap beside the band closes it.
    if (p.x0 + p.w < size.x) {
      ImGui::SetCursorScreenPos(ImVec2(p.x0 + p.w, 0));
      if (ImGui::InvisibleButton("##outside", ImVec2(size.x - (p.x0 + p.w), size.y))) drawer_open_ = false;
    }
    ImGui::End();
  }

  // Touch: a vertical drag scrolls the page and cancels the press it began on (ImGui has no touch scrolling).
  void TouchScroll() {
    ImGuiIO& io = ImGui::GetIO();
    if (!ImGui::IsMouseDown(0)) {
      dragging_ = false;
      drag_cancelled_ = false;
      return;
    }
    if (!dragging_ && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
      const ImVec2 d = ImGui::GetMouseDragDelta(0, 0);
      if (std::fabs(d.y) > D(10) && std::fabs(d.y) > std::fabs(d.x) * 1.2f && !slider_active_) dragging_ = true;
    }
    if (dragging_) {
      ImGui::SetScrollY(ImGui::GetScrollY() - io.MouseDelta.y);
      ImGui::ClearActiveID();
      drag_cancelled_ = true;
    }
    slider_active_ = false;
  }

  // ---------------------------------------------------------------- widgets (finger-sized)
  void GroupTitle(const char* text) {
    ImGui::Dummy(ImVec2(1, D(10)));
    const float px = D(21);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    MenuText(ImGui::GetWindowDrawList(), p, px, IM_COL32(235, 70, 70, 255), text);
    ImGui::Dummy(ImVec2(1, px + D(6)));
  }
  void Note(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
  }
  // A full-width row; returns true when tapped (not when a drag scrolled over it).
  bool Row(const char* id, float h, bool selected, ImVec2* out_min = nullptr) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h)) && !drag_cancelled_;
    const bool held = ImGui::IsItemActive() && !dragging_;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 bg = held ? IM_COL32(120, 16, 22, 255) : selected ? IM_COL32(32, 8, 10, 235) : IM_COL32(30, 31, 37, 215);
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), bg, D(3));
    if (selected) dl->AddRectFilled(p, ImVec2(p.x + D(5), p.y + h), kMenuRed, D(3), ImDrawFlags_RoundCornersLeft);
    if (ImGui::IsItemFocused()) dl->AddRect(p, ImVec2(p.x + w, p.y + h), IM_COL32(255, 255, 255, 160), D(3), 0, D(1.5f));
    if (out_min) *out_min = p;
    ImGui::Dummy(ImVec2(0, D(4)) );
    return pressed;
  }
  // One choice of a list; returns true when picked.
  bool Choice(const char* label, bool selected) {
    ImVec2 p;
    ImGui::PushID(label);
    const bool r = Row("##c", D(52), selected, &p);
    ImGui::PopID();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float h = D(52);
    dl->AddText(ImVec2(p.x + D(18), p.y + (h - ImGui::GetTextLineHeight()) / 2), IM_COL32_WHITE, label);
    const float w = ImGui::GetContentRegionAvail().x, cx = p.x + w - D(28), cy = p.y + h / 2;
    dl->AddCircle(ImVec2(cx, cy), D(10), IM_COL32(200, 200, 206, 255), 0, D(2));
    if (selected) dl->AddCircleFilled(ImVec2(cx, cy), D(6), kMenuRed);
    return r;
  }
  bool Toggle(const char* label, bool& v, const char* note = nullptr) {
    ImVec2 p;
    ImGui::PushID(label);
    const float h = note ? D(68) : D(52);
    const bool r = Row("##t", h, false, &p);
    ImGui::PopID();
    if (r) v = !v;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float line = ImGui::GetTextLineHeight();
    const float ty = note ? p.y + h / 2 - line - D(1) : p.y + (h - line) / 2;
    const float w = ImGui::GetContentRegionAvail().x, sx = p.x + w - D(66), sy = p.y + h / 2 - D(13);
    ImGui::PushClipRect(p, ImVec2(sx - D(8), p.y + h), true);  // long names / notes stop before the switch
    dl->AddText(ImVec2(p.x + D(18), ty), IM_COL32_WHITE, label);
    if (note) dl->AddText(ImVec2(p.x + D(18), ty + line + D(2)), ImGui::GetColorU32(kDim), note);
    ImGui::PopClipRect();
    dl->AddRectFilled(ImVec2(sx, sy), ImVec2(sx + D(48), sy + D(26)), v ? kMenuRed : IM_COL32(70, 72, 80, 255), D(13));
    dl->AddCircleFilled(ImVec2(v ? sx + D(35) : sx + D(13), sy + D(13)), D(10), IM_COL32_WHITE);
    return r;
  }
  bool Slider(const char* label, int& v, int lo, int hi, const char* fmt) {
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-1);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(D(10), D(14)));
    ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, D(30));
    const bool r = ImGui::SliderInt((std::string("##") + label).c_str(), &v, lo, hi, fmt);
    if (ImGui::IsItemActive()) slider_active_ = true;
    ImGui::PopStyleVar(2);
    ImGui::Dummy(ImVec2(0, D(4)));
    return r;
  }
  bool Button(const char* label, bool enabled = true) {
    ImGui::BeginDisabled(!enabled);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.11f, 0.13f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.55f, 0.11f, 0.13f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.75f, 0.15f, 0.18f, 1));
    const bool r = ImGui::Button(label, ImVec2(-1, D(54))) && !drag_cancelled_;
    ImGui::PopStyleColor(3);
    ImGui::EndDisabled();
    ImGui::Dummy(ImVec2(0, D(2)));
    return r;
  }

  // ---------------------------------------------------------------- the pages
  std::string GameName() const {
    if (!s_.game_name.empty() && !game_from_usb_) return s_.game_name;
    return U8(fs::u8path(read_path_).filename());
  }

  void GameFilesPage() {
    GroupTitle("YOUR GAME");
    if (game_ok_) {
      ImGui::TextColored(kGood, "Ready: %s", GameName().c_str());
      Note(game_from_usb_ ? "From the USB folder (below)." : "The file you chose. The app keeps permission to read it.");
      Note(files_reader_.is_image() ? "Disc image (.iso)" : "Extracted game folder");
    } else {
      ImGui::TextColored(kBad, "%s", game_error_.empty() ? "No game files yet" : game_error_.c_str());
    }
    ImGui::Dummy(ImVec2(0, D(6)));
    Note("Your own WWE 2K14 for Xbox 360, as a disc image (.iso). Nothing from the game comes with this app.");
    ImGui::Dummy(ImVec2(0, D(6)));
    if (Button("CHOOSE GAME FILE (.ISO)")) platform::PickGameFile();
    if (!s_.game_uri.empty() && Button("FORGET THE CHOSEN FILE")) {
      s_.game_uri.clear(), s_.game_name.clear();
      Save();
      RefreshGame();
    }
    GroupTitle("OR COPY IT OVER USB");
    Note("Connect the phone to a PC and copy the .iso (or the extracted game folder's contents) into:");
    ImGui::TextWrapped("Android/data/io.github.jarrodfeng.wwe2k14/files/game");
    Note("A chosen file is used first; without one, the app looks in that folder.");
    if (Button("LOOK AGAIN")) RefreshGame();
  }

  // Session 72 (Android drivers): the chosen driver must still be in the app (an update can drop one), and a trial
  // file left from the last PLAY means the game never got going with it: back to the phone's own driver.
  fs::path DriverTrialPath() const { return files_ / "driver_trial"; }
  // Session 73: the custom driver of the last PLAY, until the player has said whether it looked right.
  fs::path DriverAskPath() const { return files_ / "driver_ask"; }
  std::string driver_ask_;  // asked on GRAPHICS: the game got going with it last time
  bool DriverBundled(const std::string& file) const {
    const fs::path dir = platform::NativeLibDir();
    std::error_code ec;
    return !dir.empty() && fs::is_regular_file(dir / file, ec);
  }
  std::string DriverName(const std::string& id) const {
    for (const auto& d : kDrivers)
      if (id == d.file) return d.name;
    if (const UserDriver* u = FindUserDriver(id)) return u->name;
    return "the custom driver";
  }

  // Part 8 (a player: "upload their own drivers from their storage"): the drivers the player imported, one folder each
  // under platform::DriversDir() with the wwe2k14_driver.json DriverImport.java writes.
  struct UserDriver {
    std::string folder, name, library, description, version, author;
  };
  void ScanDrivers() {
    user_drivers_.clear();
    std::error_code ec;
    const fs::path root = platform::DriversDir();
    if (root.empty()) return;
    for (const auto& e : fs::directory_iterator(root, ec)) {
      const std::string folder = U8(e.path().filename());
      if (!e.is_directory(ec) || folder.empty() || folder[0] == '.') continue;
      UserDriver d;
      d.folder = folder;
      try {
        std::ifstream f(e.path() / "wwe2k14_driver.json", std::ios::binary);
        if (!f) continue;
        const nlohmann::json j = nlohmann::json::parse(f);
        Get(j, "name", d.name), Get(j, "library", d.library), Get(j, "description", d.description);
        Get(j, "version", d.version), Get(j, "author", d.author);
      } catch (...) {
        continue;
      }
      if (d.library.empty() || !fs::is_regular_file(e.path() / fs::u8path(d.library), ec)) continue;
      if (d.name.empty()) d.name = d.folder;
      user_drivers_.push_back(std::move(d));
    }
    std::sort(user_drivers_.begin(), user_drivers_.end(),
              [](const UserDriver& a, const UserDriver& b) { return a.folder < b.folder; });
  }
  const UserDriver* FindUserDriver(const std::string& id) const {
    if (id.rfind(kUserDriverPrefix, 0) != 0) return nullptr;
    const std::string folder = id.substr(std::string(kUserDriverPrefix).size());
    for (const auto& d : user_drivers_)
      if (d.folder == folder) return &d;
    return nullptr;
  }
  void RemoveUserDriver(const UserDriver& d) {
    const std::string name = d.name, id = kUserDriverPrefix + d.folder;
    std::error_code ec;
    fs::remove_all(platform::DriversDir() / fs::u8path(d.folder), ec);
    Log("driver: removed %s (%s)%s", d.folder.c_str(), name.c_str(), ec ? (" - " + ec.message()).c_str() : "");
    if (s_.driver == id) s_.driver.clear(), Save();
    ScanDrivers();
    ShowToast(ec ? "Could not remove " + name : "Removed " + name);
  }
  void CheckDriver() {
    std::error_code ec;
    const bool trial_left = fs::exists(DriverTrialPath(), ec);
    if (fs::exists(DriverAskPath(), ec)) {
      std::string id;
      std::getline(std::ifstream(DriverAskPath()), id);
      fs::remove(DriverAskPath(), ec);
      if (!trial_left && id == s_.driver && !IdListHas(s_.drivers_ok, id) && !IdListHas(s_.drivers_bad, id)) {
        driver_ask_ = id;
        Log("driver: asking whether %s looked right", id.c_str());
      }
    }
    if (trial_left) {
      fs::remove(DriverTrialPath(), ec);
      if (!s_.driver.empty()) {
        Log("driver: %s did not get the game running last time: back to the phone's driver", s_.driver.c_str());
        ShowToast(std::string(DriverName(s_.driver)) + " did not start the game: back to your phone's own driver");
        s_.driver.clear();
        Save();
      }
    }
    const bool user = s_.driver.rfind(kUserDriverPrefix, 0) == 0;
    if (!s_.driver.empty() && (user ? !FindUserDriver(s_.driver) : !DriverBundled(s_.driver))) {
      Log("driver: %s is not in this app: the phone's driver", s_.driver.c_str());
      s_.driver.clear();
      Save();
    }
  }

  void GraphicsPage() {
    if (!driver_ask_.empty()) {  // Session 73 (idea 10)
      GroupTitle("YOUR LAST GAME");
      Note(("Did the game look right with " + DriverName(driver_ask_) +
            "? White or missing textures, wrong colours or broken models mean this driver draws wrongly on this phone.")
               .c_str());
      if (Button("YES, IT LOOKED RIGHT")) {
        IdListAdd(s_.drivers_ok, driver_ask_);
        Log("driver: %s looked right (the player)", driver_ask_.c_str());
        driver_ask_.clear();
        Save();
      }
      if (Button("NO, IT LOOKED BROKEN")) {
        IdListAdd(s_.drivers_bad, driver_ask_);
        Log("driver: %s looked broken (the player): back to the phone's driver", driver_ask_.c_str());
        ShowToast(DriverName(driver_ask_) + " is marked as drawing wrongly here: back to your phone's own driver", 8000);
        if (s_.driver == driver_ask_) s_.driver.clear();
        driver_ask_.clear();
        Save();
      }
    }
    GroupTitle("RESOLUTION");
    for (int i = 0; i < kResolutionCount; ++i) {
      const std::string label = std::string(kResolutions[i].name) + (i == kDefaultResolution ? "  (default)" : "") +
                                (kResolutions[i].resolution == 1 ? "  (heavy)" : "");
      if (Choice(label.c_str(), s_.resolution == i)) s_.resolution = i, Save();
    }
    Note("What the game draws at. Lower is faster and cooler; 720p is what this phone was tested at.");
    if (Toggle("Performance mode", s_.performance, "540p at most, sharp upscaling, less crowd work")) Save();
    Note("For phones that drop frames in matches: the scene is drawn at 540p at most and sharpened up to the screen. "
         "Turn it off for the sharpest picture.");
    if (Toggle("Sharp upscaling (Snapdragon GSR)", s_.sgsr, "A sharper picture on the screen than a plain stretch"))
      Save();
    if (Toggle("Dynamic resolution", s_.dynamic_res, "Sharper when it can, smoother when it must")) Save();
    Note("When the phone cannot keep up (heat, busy entrances) the game draws a little smaller for a while, then "
         "goes back up.");
    GroupTitle("FRAME RATE");
    for (const auto& f : kFrameRates)
      if (Choice(f.name, s_.frame_rate == f.value)) s_.frame_rate = f.value, Save();
    if (Toggle("Big cores (test)", s_.big_cores, "The game's busiest work on the fast CPU cores only")) Save();
    Note("A test: does the game run smoother on your phone with it on?");
    GroupTitle("SCREEN SHAPE");
    if (Choice("16:9 WITH BARS (the game's own)", s_.aspect == 0)) s_.aspect = 0, Save();
    if (Choice("FILL THE SCREEN (more of the arena)", s_.aspect == 1)) s_.aspect = 1, Save();
    Note("Fill the screen shows more of the arena in matches: at the sides on a long phone, above and below on a fold "
         "or tablet. Menus stay 16:9.");
    GroupTitle("TEXTURE FILTERING");
    static constexpr const char* kFilters[] = {"STANDARD", "8X", "16X"};
    for (int i = 0; i < 3; ++i)
      if (Choice(kFilters[i], s_.texture_filter == i)) s_.texture_filter = i, Save();
    GroupTitle("LANGUAGE");
    for (const auto& l : kLanguages)
      if (Choice(l.name, s_.language == l.id)) s_.language = l.id, Save();
    GroupTitle("IF THE PICTURE IS BROKEN");
    if (Toggle("Safe shaders", s_.safe_shaders, "Plain shaders, a little slower")) Save();
    Note("Spiky or stretched models, smeared faces? Try this, then another driver below, and send the game log "
         "(LOGS AND UPDATES) with what you saw.");
    GroupTitle("GRAPHICS DRIVER");
    if (Choice("YOUR PHONE'S OWN DRIVER  (default)", s_.driver.empty())) s_.driver.clear(), Save();
    for (size_t i = 0; i < std::size(kDrivers); ++i) {
      const auto& d = kDrivers[i];
      if (i >= driver_bundled_.size() || !driver_bundled_[i]) continue;
      if (Choice(d.name, s_.driver == d.file)) s_.driver = d.file, Save();
      Note(d.note);
      if (IdListHas(s_.drivers_bad, d.file)) Note("You said it drew wrongly on this phone.");
    }
    // Part 8: the player's own drivers.
    const UserDriver* remove = nullptr;
    for (const auto& d : user_drivers_) {
      const std::string id = kUserDriverPrefix + d.folder;
      ImGui::PushID(d.folder.c_str());
      if (Choice((d.name + "  (yours)").c_str(), s_.driver == id)) s_.driver = id, Save();
      std::string about = d.description;
      if (!d.version.empty()) about += (about.empty() ? "" : " ") + std::string("(") + d.version + ")";
      if (!d.author.empty()) about += (about.empty() ? "By " : ". By ") + d.author;
      Note(((about.empty() ? std::string() : about + ". ") + "You added this one; it is not tested by the project.").c_str());
      if (s_.driver == id && Button("REMOVE THIS DRIVER")) remove = &d;
      ImGui::PopID();
    }
    if (remove) RemoveUserDriver(*remove);
    if (Button("ADD YOUR OWN DRIVER (.ZIP OR .SO)")) platform::ImportDriver(platform::DriversDir());
    Note("A driver package as Android emulators' driver managers use them (a .zip with the driver and its meta.json), "
         "or the driver's .so on its own, for 64-bit ARM. It is copied into the app.");
    Note("Try another driver if the picture is wrong or slow. One that does not suit your phone is skipped "
         "(the game uses your phone's own driver), and if the game does not start with it, the launcher goes back "
         "to your phone's own driver next time.");
  }

  void TouchPage() {
    GroupTitle("SHOW TOUCH CONTROLS");
    static constexpr const char* kModes[] = {"AUTOMATIC (hidden with a controller)", "ALWAYS", "OFF"};
    for (int i = 0; i < 3; ++i)
      if (Choice(kModes[i], s_.touch_mode == i)) s_.touch_mode = i, Save();
    GroupTitle("LOOK AND FEEL");
    if (Slider("Size", s_.touch_size, 60, 150, "%d%%")) Save();
    if (Slider("Opacity", s_.touch_opacity, 20, 100, "%d%%")) Save();
    if (Toggle("Vibrate on press", s_.touch_vibrate, "A short buzz for each button")) Save();
    // Session 74 (players asked for it): drag the controls to new places (TouchEditActivity saves the file; in a game,
    // Back -> MOVE TOUCH CONTROLS does the same).
    GroupTitle("POSITIONS");
    if (Button("MOVE THE CONTROLS")) platform::EditTouchLayout(s_.touch_size, s_.touch_opacity);
    std::error_code ec;
    const fs::path layout = files_ / "touch_layout.txt";
    if (fs::exists(layout, ec) && Button("PUT THE CONTROLS BACK")) fs::remove(layout, ec);
    Note("Drag a control to where your thumb wants it (A B X Y move together), then DONE. In a game, Back opens the "
         "same editor (MOVE TOUCH CONTROLS).");
    Note("An Xbox layout drawn over the game: sticks, d-pad, A B X Y, bumpers, triggers, stick clicks, and Start / "
         "View at the top centre. A connected controller hides it (automatic).");
  }

  void ScanDlc() {
    dlc_scanned_ = true;
    packs_ = wwe2k14::dlc::Scan(DlcDir());
    pack_names_.clear();
    std::map<int, std::string> names;
    for (const auto& p : packs_) {
      if (p.status != wwe2k14::dlc::Status::kReady || p.string_pack.empty()) continue;
      try {
        for (const auto& [id, n] : wwe2k14::dlc::NamesFromPack(p)) names.emplace(id, n);
      } catch (...) {
      }
    }
    for (const auto& p : packs_) {
      std::string list;
      for (int id : p.char_ids) {
        const auto n = names.find(60000 + id);
        const std::string name = n != names.end() && !n->second.empty() ? wwe2k14::dlc::ReadableName(n->second)
                                                                        : "#" + std::to_string(id);
        list += (list.empty() ? "" : ", ") + name;
      }
      pack_names_.push_back(list);
    }
    Log("dlc: %zu packs in %s", packs_.size(), U8(DlcDir()).c_str());
  }

  void DlcPage() {
    GroupTitle("DOWNLOADABLE CONTENT");
    if (Toggle("Use DLC", s_.dlc, "Packs below load with the game")) Save();
    if (Button("ADD DLC FILES")) platform::AddDlc(DlcDir());
    Note("Pick the packs as they sit on the console's drive (copied with FATXplorer, for example). They are copied "
         "into the app. Over USB, put them in Android/data/io.github.jarrodfeng.wwe2k14/files/dlc.");
    if (Button("LOOK AGAIN")) ScanDlc();
    GroupTitle("PACKS");
    if (packs_.empty()) Note("No packs yet.");
    std::vector<int> off = ParseIds(s_.dlc_off);
    for (size_t i = 0; i < packs_.size(); ++i) {
      const auto& p = packs_[i];
      const bool ready = p.status == wwe2k14::dlc::Status::kReady;
      bool on = ready && std::find(off.begin(), off.end(), p.pack_id) == off.end();
      const std::string title = (p.package && !p.package_name.empty() ? p.package_name : p.folder) +
                                (p.pack_id >= 0 ? "  (pack " + std::to_string(p.pack_id) + ")" : "");
      const std::string note = ready ? (pack_names_[i].empty() ? std::string("ready") : pack_names_[i])
                                     : std::string(wwe2k14::dlc::StatusName(p.status)) + ": " + p.reason;
      ImGui::PushID(int(i));
      ImGui::BeginDisabled(!ready || !s_.dlc);
      if (Toggle(title.c_str(), on, note.c_str()) && ready) {
        if (on) off.erase(std::remove(off.begin(), off.end(), p.pack_id), off.end());
        else off.push_back(p.pack_id);
        s_.dlc_off = JoinIds(off);
        Save();
      }
      ImGui::EndDisabled();
      ImGui::PopID();
    }
  }

  void Wm30Page() {
    if (!game_ok_) {
      Note("Choose your game files first: the list comes from them.");
      return;
    }
    if (!wm30_.loaded) {
      wm30_ = wwe2k14::wm30list::Load(files_reader_);
      Log("30 years: %s (%zu matches)", wm30_.ok ? "ok" : wm30_.error.c_str(), wm30_.matches.size());
    }
    if (!wm30_.ok) {
      ImGui::TextColored(kBad, "The list could not be read: %s", wm30_.error.c_str());
      return;
    }
    Note("Pick a match, then PLAY THIS MATCH: the game goes straight to it.");
    int era = -1;
    for (size_t i = 0; i < wm30_.matches.size(); ++i) {
      const auto& m = wm30_.matches[i];
      if (m.era != era) {
        era = m.era;
        GroupTitle(m.era >= 0 && m.era < 5 && !wm30_.era_names[m.era].empty() ? wm30_.era_names[m.era].c_str() : "ERA");
      }
      ImVec2 p;
      ImGui::PushID(int(i));
      const float h = D(70);
      if (Row("##m", h, wm30_pick_ == int(i), &p)) wm30_pick_ = int(i);
      ImGui::PopID();
      ImDrawList* dl = ImGui::GetWindowDrawList();
      const float line = ImGui::GetTextLineHeight();
      std::string people;
      for (const auto& x : m.people) people += (people.empty() ? "" : " vs ") + x;
      const std::string sub = m.show + (m.date.empty() ? "" : "  -  " + m.date);
      ImGui::PushClipRect(p, ImVec2(p.x + ImGui::GetContentRegionAvail().x - D(8), p.y + h), true);
      dl->AddText(ImVec2(p.x + D(18), p.y + h / 2 - line - D(1)), IM_COL32_WHITE, (m.title.empty() ? people : m.title).c_str());
      dl->AddText(ImVec2(p.x + D(18), p.y + h / 2 + D(1)), ImGui::GetColorU32(kDim), sub.c_str());
      ImGui::PopClipRect();
    }
  }
  void Wm30Bar(ImVec2 pos, float w) {
    const bool picked = wm30_pick_ >= 0 && wm30_pick_ < int(wm30_.matches.size());
    if (SlantButton(picked ? "PLAY THIS MATCH" : "PICK A MATCH", ImVec2(pos.x + D(8), pos.y + D(10)),
                    ImVec2(w - D(16), D(70)), picked && game_ok_, D(36)) && picked)
      Play(wm30_.matches[size_t(wm30_pick_)].index);
  }

  // The save folder's size, counted when the page opens and after an import (Session 72: it was counted every frame).
  void ScanSaves() {
    std::error_code ec;
    save_exists_ = fs::is_directory(SaveDir(), ec);
    save_bytes_ = 0, save_files_ = 0;
    if (!save_exists_) return;
    for (const auto& e : fs::recursive_directory_iterator(SaveDir(), ec))
      if (e.is_regular_file(ec) && U8(e.path()).find("/cache") == std::string::npos)
        save_bytes_ += e.file_size(ec), ++save_files_;
  }

  void SavesPage() {
    GroupTitle("YOUR SAVE");
    if (save_exists_) ImGui::Text("%d files, %.1f MB", save_files_, double(save_bytes_) / (1024.0 * 1024.0));
    else Note("No save yet (the game makes one the first time it saves).");
    Note("Saves live in the app's own folder: uninstalling the app deletes them. Export a copy first.");
    ImGui::Dummy(ImVec2(0, D(6)));
    if (Button("EXPORT SAVE (.ZIP)", save_exists_)) platform::ExportSaves(SaveDir());
    if (Button("IMPORT SAVE (.ZIP)")) platform::ImportSaves(SaveDir());
    Note("Import replaces the current save; the old one is kept next to it as a backup.");
  }

  void HelpPage() {
    GroupTitle("PROBLEM REPORTS");
    // Session 73: how the last game run ended, and the phone check.
    if (!last_run_.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.45f, 1.0f));
      ImGui::TextWrapped("%s", last_run_.c_str());
      ImGui::PopStyleColor();
    }
    if (Button("SHARE THE LAST GAME LOG")) ShareLogs();
    Note("Sends one .zip through Android's share menu (Discord, email...): a short report, the newest game log, its "
         "start-up record, the launcher's log and settings and this phone's check.");
    if (Button("CHECK THIS PHONE")) RunCheck();
    for (const auto& line : check_) {
      static const ImVec4 kColors[3] = {ImVec4(0.55f, 0.9f, 0.55f, 1.0f), ImVec4(1.0f, 0.8f, 0.35f, 1.0f),
                                        ImVec4(1.0f, 0.45f, 0.4f, 1.0f)};
      static const char* const kMarks[3] = {"OK", "!", "X"};
      ImGui::PushStyleColor(ImGuiCol_Text, kColors[std::clamp(line.level, 0, 2)]);
      ImGui::TextWrapped("%s  %s", kMarks[std::clamp(line.level, 0, 2)], line.text.c_str());
      ImGui::PopStyleColor();
    }
    if (check_.empty()) {
      Note("Tests what stops the game before its first picture: the game files, storage, memory, the game's memory "
           "block and the phone's Vulkan driver.");
    }
    GroupTitle("UPDATES");
    if (Toggle("Check for updates", s_.check_updates, "Asks GitHub when the app opens")) {
      Save();
      if (s_.check_updates) CheckUpdate();
    }
    switch (update_.state) {
      case launcher::UpdateInfo::kChecking: Note("Checking..."); break;
      case launcher::UpdateInfo::kLatest: ImGui::TextColored(kGood, "No newer release"); break;
      case launcher::UpdateInfo::kFailed: ImGui::TextColored(kBad, "Update check failed: %s", update_.error.c_str()); break;
      case launcher::UpdateInfo::kAvailable:
        ImGui::TextColored(kGood, "New release: %s", (update_.name.empty() ? update_.tag : update_.name).c_str());
        if (Button("OPEN THE RELEASE PAGE")) platform::OpenUrl(update_.url);
        break;
      default: break;
    }
    if (update_.state != launcher::UpdateInfo::kChecking && Button("CHECK NOW")) CheckUpdate();
    GroupTitle("ABOUT");
    ImGui::Text("WWE 2K14 recompiled, %s", kVersion);
    Note("A fan preservation project: the Xbox 360 game, recompiled to run natively. Free and non-commercial. It "
         "needs your own copy of the game; nothing from the game ships with the app. Not affiliated with WWE or 2K.");
  }

  // ---------------------------------------------------------------- actions
  void CheckUpdate() {
    update_ = {};
    update_.state = launcher::UpdateInfo::kChecking;
    platform::CheckForUpdate(kVersion);
  }

  // Session 73: CHECK THIS PHONE.
  void RunCheck() {
    check_ = check::CheckPhone(files_, game_ok_, game_error_, s_.game_name);
    for (const auto& line : check_) Log("phone check %s: %s", line.level == 0 ? "ok" : line.level == 1 ? "warn" : "FAIL",
                                        line.text.c_str());
  }

  // hand_over false (--share-test): makes the zip, no share sheet.
  void ShareLogs(bool hand_over = true) {
    std::vector<fs::path> out;
    std::error_code ec;
    fs::path newest;
    fs::file_time_type t{};
    for (const auto& e : fs::directory_iterator(files_ / "logs", ec)) {
      const std::string n = U8(e.path().filename());
      if (n.rfind("wwe2k14_", 0) != 0 || e.path().extension() != ".log") continue;
      if (U8(e.path().stem()).find('.') != std::string::npos) continue;  // a rotated part (below)
      const auto wt = e.last_write_time(ec);
      if (newest.empty() || wt > t) newest = e.path(), t = wt;
    }
    if (!newest.empty()) out.push_back(newest);
    // Part 8: a long run's log rotates every 5 MB (SDK log_max_file_size_mb): wwe2k14_004.log is its newest part,
    // wwe2k14_004.N.log older ones. The oldest kept (the highest N) has the run's start - the GPU, its driver and
    // features - which a tester's log lacked: that one goes too.
    if (!newest.empty()) {
      const std::string base = U8(newest.stem()) + ".";
      fs::path oldest;
      long most = 0;
      for (const auto& e : fs::directory_iterator(files_ / "logs", ec)) {
        const std::string n = U8(e.path().filename());
        // wwe2k14_004.N.log: the name minus base and ".log" (wwe2k14_004.log itself starts with base too).
        if (n.rfind(base, 0) != 0 || n.size() <= base.size() + 4 || e.path().extension() != ".log") continue;
        const std::string mid = n.substr(base.size(), n.size() - base.size() - 4);
        if (mid.empty() || mid.find_first_not_of("0123456789") != std::string::npos || mid.size() > 6) continue;
        if (const long k = std::stol(mid); k > most) most = k, oldest = e.path();
      }
      if (!oldest.empty()) out.push_back(oldest);
    }
    // Session 73 (Jarrod: testers send ONE file): everything goes into logs/share/wwe2k14_report_NNN.zip, report.txt
    // first (share_report.h). The files one by one, as before, only when the zip cannot be written.
    if (!newest.empty()) Log("share: game log %s", U8(newest.filename()).c_str());
    const fs::path zip = ShareZip(newest, out.size() > 1 ? out[1] : fs::path());
    out.push_back(files_ / "logs" / "launcher.log");
    // Session 73: the last run's start-up record and Android's reason for its end (and its native crash record).
    for (const char* extra : {"startup.txt", "last_game_exit.txt", "last_game_exit_trace.bin"}) {
      if (fs::is_regular_file(files_ / "logs" / extra, ec)) out.push_back(files_ / "logs" / extra);
    }
    if (!zip.empty()) out = {zip};
    if (!hand_over) return;
    if (newest.empty()) ShowToast("No game log yet: sharing the launcher's log");
    platform::Share(out);
  }

  // The zip SHARE sends: report.txt, the game log (and its oldest kept part), frames_NNN.csv, the start-up record,
  // Android's exit record, the launcher's log and settings, phone.txt (CHECK THIS PHONE). Empty when it failed.
  fs::path ShareZip(const fs::path& game_log, const fs::path& oldest_part) {
    std::error_code ec;
    const fs::path logs = files_ / "logs", dir = logs / "share";
    fs::create_directories(dir, ec);
    // NNN: wwe2k14_116.log -> 116.
    std::string num;
    if (!game_log.empty()) num = U8(game_log.stem()).substr(8);
    const fs::path frames = num.empty() ? fs::path() : logs / fs::u8path("frames_" + num + ".csv");
    const fs::path path = dir / fs::u8path("wwe2k14_report_" + (num.empty() ? std::string("none") : num) + ".zip");
    // Only the newest report stays (they are copies of the logs).
    for (const auto& e : fs::directory_iterator(dir, ec)) {
      const std::string n = U8(e.path().filename());
      if (n.rfind("wwe2k14_report_", 0) == 0 && e.path() != path) fs::remove(e.path(), ec);
    }
    if (check_.empty()) RunCheck();
    report::Inputs in;
    in.launcher_version = kVersion;
    in.last_run = last_run_;
    in.logs_dir = logs;
    if (!oldest_part.empty()) in.game_logs.push_back(oldest_part);
    if (!game_log.empty()) in.game_logs.push_back(game_log);
    in.frames_csv = frames;
    const std::string text = report::Build(in);
    std::string phone;
    static const char* const kMarks[3] = {"OK", "!", "X"};
    for (const auto& line : check_) phone += std::string(kMarks[std::clamp(line.level, 0, 2)]) + "  " + line.text + "\n";

    zip::Writer z(path);
    bool ok = z.ok() && z.AddText("report.txt", text);
    auto add = [&](const fs::path& f) {
      if (ok && !f.empty() && fs::is_regular_file(f, ec)) ok = z.AddFile(U8(f.filename()), f);
    };
    add(game_log);
    add(oldest_part);
    add(frames);
    for (const char* extra : {"startup.txt", "last_game_exit.txt", "last_game_exit_trace.bin"}) add(logs / extra);
    Log("share: report zip %s", U8(path).c_str());  // before the copy, so the zip's launcher.log has it
    add(logs / "launcher.log");
    add(SettingsPath());
    if (ok) ok = z.AddText("phone.txt", phone);
    const size_t entries = z.count();
    ok = z.Finish() && ok;
    if (!ok) {
      Log("share: could not write %s: sharing the files one by one", U8(path).c_str());
      fs::remove(path, ec);
      return {};
    }
    Log("share: %s written, %zu files, %llu bytes", U8(path).c_str(), entries,
        static_cast<unsigned long long>(fs::file_size(path, ec)));
    return path;
  }

  void Play(int wm30 = -1) {
    if (!game_ok_) {
      GoPage(kGameFiles);
      ShowToast("Choose your game files first");
      return;
    }
    // Session 73: 16 KB memory pages would break the game's memory (phone_check.h PageSizeProblem): say so instead.
    if (const std::string pages = check::PageSizeProblem(); !pages.empty()) {
      Log("PLAY refused: %s", pages.c_str());
      ShowToast(pages, 10000);
      return;
    }
    LaunchContext c;
    c.game_path = address_.rfind("content://", 0) == 0 ? kGameFdToken : read_path_;
    std::error_code ec;
    if (fs::is_directory(DlcDir(), ec)) c.dlc_dir = U8(DlcDir());
    platform::ScreenSize(c.screen_w, c.screen_h);
    c.wm30 = wm30;
    if (const UserDriver* u = FindUserDriver(s_.driver)) {  // part 8: an imported driver, from its own folder
      c.driver_library = u->library;
      c.driver_dir = U8(platform::DriversDir() / fs::u8path(u->folder)) + "/";
    }
    if (!s_.driver.empty()) {  // Session 72: the game removes it once it has drawn a few hundred frames
      std::ofstream(DriverTrialPath(), std::ios::trunc) << s_.driver << "\n";
      c.driver_trial = U8(DriverTrialPath());
      // Session 73: asked once, after the first game that gets going with it.
      if (!IdListHas(s_.drivers_ok, s_.driver) && !IdListHas(s_.drivers_bad, s_.driver)) {
        std::ofstream(DriverAskPath(), std::ios::trunc) << s_.driver << "\n";
      }
    }
    const std::vector<std::string> args = GameArgs(s_, c);
    Save();
    std::string line;
    for (const auto& a : args) line += " " + a;
    Log("PLAY:%s", line.c_str());
    platform::TouchChoice t;
    t.mode = s_.touch_mode, t.size = s_.touch_size, t.opacity = s_.touch_opacity, t.vibrate = s_.touch_vibrate;
    platform::StartGame(args, address_, t);
  }

  void OnEvent(const platform::Event& e) {
    using E = platform::Event;
    switch (e.kind) {
      case E::kGamePicked:
        if (!e.ok) break;
        s_.game_uri = e.a, s_.game_name = e.b;
        Save();
        RefreshGame();
        ShowToast(game_ok_ ? "Game files ready" : game_error_);
        break;
      case E::kDlcAdded:
        ShowToast(e.a);
        ScanDlc();
        break;
      case E::kDriverImported:
        Log("driver import: %s %s", e.ok ? "ok" : "failed", e.a.c_str());
        ScanDrivers();
        if (e.ok && FindUserDriver(kUserDriverPrefix + e.b)) s_.driver = kUserDriverPrefix + e.b, Save();
        if (!e.a.empty()) ShowToast(e.a);
        break;
      case E::kSavesImported:
        ScanSaves();
        if (!e.a.empty()) ShowToast(e.a);
        break;
      case E::kSavesExported:
      case E::kMessage:
        if (!e.a.empty()) ShowToast(e.a);
        break;
      case E::kUpdate:
        update_ = e.update;
        Log("update check: state %d %s %s", int(update_.state), update_.tag.c_str(), update_.error.c_str());
        if (update_.state == launcher::UpdateInfo::kAvailable) ShowToast("A newer release is out: see Logs and updates");
        break;
      case E::kBack: Back(); break;
    }
  }

  void ShowToast(const std::string& text, uint64_t ms = 4000) {
    toast_ = text;
    toast_until_ = SDL_GetTicks() + ms;
    Log("note: %s", text.c_str());
  }
  void Toast(ImVec2 size) {
    if (toast_.empty() || SDL_GetTicks() > toast_until_) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float wrap = std::min(size.x - D(48), D(520));
    const ImVec2 ts = ImGui::CalcTextSize(toast_.c_str(), nullptr, false, wrap);
    const ImVec2 p((size.x - ts.x) / 2, safe_max_.y - ts.y - D(110));
    dl->AddRectFilled(ImVec2(p.x - D(16), p.y - D(12)), ImVec2(p.x + ts.x + D(16), p.y + ts.y + D(12)),
                      IM_COL32(10, 10, 12, 235), D(6));
    dl->AddRect(ImVec2(p.x - D(16), p.y - D(12)), ImVec2(p.x + ts.x + D(16), p.y + ts.y + D(12)), kMenuRed, D(6), 0, D(1.5f));
    dl->AddText(nullptr, 0, p, IM_COL32_WHITE, toast_.c_str(), nullptr, wrap);
  }

 public:
  ImFont* menu_font_ = nullptr;

 private:
  SDL_Window* window_;
  SDL_Renderer* renderer_;
  float ui_;
  TestSteps test_;
  fs::path files_;
  std::string last_run_;               // Session 73: the last game run's end in plain words, empty when normal
  std::vector<check::Line> check_;     // Session 73: CHECK THIS PHONE's results
  std::ofstream log_;
  Settings s_;
  Page page_ = kHome;
  bool landscape_ = false, scroll_reset_ = true;
  ImVec2 safe_min_, safe_max_;
  bool dragging_ = false, drag_cancelled_ = false, slider_active_ = false;
  bool drawer_open_ = false;
  float drawer_t_ = 0;  // 0 closed .. 1 open (the slide)
  uint64_t last_back_ = 0;

  std::string read_path_, address_, game_error_;
  bool game_ok_ = false, game_from_usb_ = false;
  launcher::GameFiles files_reader_;

  launcher::GameFont gfont_;
  Texture gfont_tex_;
  bool font_tried_ = false;
  std::vector<uint8_t> logo_px_;
  int logo_w_ = 0, logo_h_ = 0;
  bool logo_tried_ = false;
  Texture logo_, bg_;
  struct Band {
    Texture tex;
    int x = 0, y = 0;
    std::array<float, 8> key{};
    bool made = false;
  };
  Band bands_[kBandSlots];

  bool save_exists_ = false;
  uintmax_t save_bytes_ = 0;
  int save_files_ = 0;
  std::vector<bool> driver_bundled_;  // per kDrivers entry, looked up once (Start)
  std::vector<UserDriver> user_drivers_;
  std::vector<wwe2k14::dlc::Pack> packs_;
  std::vector<std::string> pack_names_;
  bool dlc_scanned_ = false;
  wwe2k14::wm30list::Catalogue wm30_;
  int wm30_pick_ = -1;
  launcher::UpdateInfo update_;
  std::string toast_;
  uint64_t toast_until_ = 0;
};

void ApplyStyle(float ui) {
  ImGuiStyle& st = ImGui::GetStyle();
  ImGui::StyleColorsDark();
  st.WindowRounding = 0, st.FrameRounding = 3, st.GrabRounding = 3, st.ChildRounding = 0, st.PopupRounding = 3;
  st.FramePadding = ImVec2(12, 10), st.ItemSpacing = ImVec2(10, 8), st.WindowPadding = ImVec2(16, 14);
  st.ScrollbarSize = 4;
  ImVec4* c = st.Colors;
  c[ImGuiCol_WindowBg] = ImVec4(0.067f, 0.071f, 0.082f, 1);
  c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_FrameBg] = ImVec4(0.13f, 0.135f, 0.16f, 0.95f);
  c[ImGuiCol_FrameBgActive] = ImVec4(0.18f, 0.185f, 0.21f, 1);
  c[ImGuiCol_SliderGrab] = ImVec4(0.84f, 0.09f, 0.12f, 1);
  c[ImGuiCol_SliderGrabActive] = ImVec4(0.95f, 0.25f, 0.25f, 1);
  c[ImGuiCol_Button] = ImVec4(0.20f, 0.205f, 0.24f, 1);
  c[ImGuiCol_ButtonHovered] = ImVec4(0.20f, 0.205f, 0.24f, 1);  // touch: no hover colour
  c[ImGuiCol_ButtonActive] = ImVec4(0.55f, 0.11f, 0.13f, 1);
  c[ImGuiCol_CheckMark] = ImVec4(0.95f, 0.35f, 0.3f, 1);
  c[ImGuiCol_NavCursor] = ImVec4(1, 1, 1, 0.8f);
  st.ScaleAllSizes(ui);
}

// The text face: the phone's Roboto / Windows' Segoe UI; the menu fallback (no game files yet): a heavy face.
void LoadFonts(ImGuiIO& io, float ui, App& app) {
  static constexpr const char* kText[] = {"/system/fonts/Roboto-Regular.ttf", "/system/fonts/NotoSans-Regular.ttf",
                                          "C:/Windows/Fonts/segoeui.ttf"};
  static constexpr const char* kMenu[] = {"/system/fonts/Roboto-Black.ttf", "/system/fonts/Roboto-Bold.ttf",
                                          "C:/Windows/Fonts/ariblk.ttf"};
  std::error_code ec;
  ImFont* text = nullptr;
  for (const char* f : kText)
    if (!text && fs::exists(f, ec)) text = io.Fonts->AddFontFromFileTTF(f, 18.0f * ui);
  if (!text) io.Fonts->AddFontDefault();
  for (const char* f : kMenu)
    if (!app.menu_font_ && fs::exists(f, ec)) app.menu_font_ = io.Fonts->AddFontFromFileTTF(f, 18.0f * ui);
  ImGui::GetStyle().FontSizeBase = 18.0f * ui;
}

TestSteps ParseTest(int argc, char** argv, int& win_w, int& win_h, float& scale) {
  TestSteps t;
  for (int i = 1; i + 1 < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--check") t.check = true;
    else if (a == "--page") t.page = std::atoi(argv[++i]);
    else if (a == "--shot") t.shot = argv[++i];
    else if (a == "--frames") t.frames = std::max(1, std::atoi(argv[++i]));
    else if (a == "--scroll") t.scroll = float(std::atof(argv[++i]));
    else if (a == "--select") t.select = std::atoi(argv[++i]);
    else if (a == "--drawer") t.drawer = std::atoi(argv[++i]) != 0;
    else if (a == "--window") std::sscanf(argv[++i], "%dx%d", &win_w, &win_h);
    else if (a == "--scale") scale = float(std::atof(argv[++i]));
    else if (a == "--tap") std::sscanf(argv[++i], "%f,%f", &t.tap_x, &t.tap_y);
    else if (a == "--quit-ms") t.quit_ms = std::atoi(argv[++i]);
  }
  for (int i = 1; i < argc; ++i) {  // flags without a value (the loop above misses one given last)
    const std::string a = argv[i];
    if (a == "--share-test") t.share = true;
    else if (a == "--check") t.check = true;
  }
  return t;
}

// Session 72: how the launcher draws, in its log (players share it from LOGS AND UPDATES): per ~10 s of drawing, the
// frames, their rate while drawing (the screen's refresh rate when all is well) and a frame's own work before the
// wait for the screen; a frame whose work takes over 50 ms gets a line of its own.
struct DrawStats {
  int frames = 0, slow_lines = 0;
  double work_ms = 0, worst_ms = 0, total_ms = 0;
  uint64_t start_ns = 0;
  void Add(App& app, double work, double total) {
    const uint64_t now = SDL_GetTicksNS();
    if (!frames) start_ns = now;
    ++frames, work_ms += work, total_ms += total, worst_ms = std::max(worst_ms, work);
    if (work > 50 && slow_lines < 40) ++slow_lines, app.Log("slow frame: %.0f ms (%s)", work, app.PageName());
    if (now - start_ns >= 10'000'000'000ull) Flush(app);
  }
  void Flush(App& app) {
    if (!frames) return;
    app.Log("drawing: %d frames in %.1f s, %.0f fps while drawing; a frame's work %.2f ms on average, %.1f ms at most",
            frames, double(SDL_GetTicksNS() - start_ns) / 1e9, total_ms > 0 ? frames * 1000.0 / total_ms : 0.0,
            work_ms / frames, worst_ms);
    frames = 0, work_ms = worst_ms = total_ms = 0;
  }
};

bool SaveShot(SDL_Renderer* r, const std::string& path) {
  SDL_Surface* s = SDL_RenderReadPixels(r, nullptr);
  if (!s) return false;
  const bool ok = SDL_SavePNG(s, path.c_str());
  SDL_DestroySurface(s);
  return ok;
}

}  // namespace mlauncher

int main(int argc, char** argv) {
  using namespace mlauncher;
  SDL_SetHint(SDL_HINT_ORIENTATIONS, "Portrait LandscapeLeft LandscapeRight");  // the launcher turns with the phone
  SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");  // Back = our page back (LauncherActivity also sends it)
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
    SDL_Log("wwe2k14 launcher: SDL_Init failed: %s", SDL_GetError());
    return 1;
  }
  platform::Init(argc, argv);
  int win_w = 720, win_h = 1584;  // PC test: half the Find X8 Ultra's 1440 x 3168
  float scale = 0;
  TestSteps test = ParseTest(argc, argv, win_w, win_h, scale);
#ifdef __ANDROID__
  // Not full screen: the phone's own bars stay (in immersive full screen the first Back swipe only shows them, a
  // likely cause of Jarrod's two-Back report, unconfirmed); the layout keeps to the safe area.
  const SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#else
  const SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | (test.shot.empty() && !test.share ? 0 : SDL_WINDOW_HIDDEN);
#endif
  SDL_Window* window = SDL_CreateWindow("WWE 2K14", win_w, win_h, flags);
  SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
  if (!renderer) {
    SDL_Log("wwe2k14 launcher: no window / renderer: %s", SDL_GetError());
    return 1;
  }
  SDL_SetRenderVSync(renderer, 1);

  // The UI scale in ImGui's units: the screen's density (a 560 dpi phone: 3.5) over what SDL already scales.
  int pw = 0, ph = 0, ww = 0, wh = 0;
  SDL_GetWindowSizeInPixels(window, &pw, &ph);
  SDL_GetWindowSize(window, &ww, &wh);
  float ui = scale > 0 ? scale : SDL_GetWindowDisplayScale(window) / (ww > 0 ? float(pw) / float(ww) : 1.f);
  if (!(ui > 0.5f && ui < 8)) ui = 1;

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
  ApplyStyle(ui);
  App app(window, renderer, ui);
  LoadFonts(io, ui, app);
  ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
  ImGui_ImplSDLRenderer3_Init(renderer);
  app.Start(test);
  app.Log("renderer: %s, %d x %d pixels", SDL_GetRendererName(renderer), pw, ph);

  bool running = !test.share;  // --share-test: Start made the zip; nothing to draw
  int frame = 0;
  // Session 72 (players: "the launcher runs at about 15 fps"): it drew one frame per batch of input, else one every
  // 100 ms, while ImGui spreads a tap over three frames (finger position, down, up: imgui.cpp's input trickling), so
  // a tap's page came 200-300 ms late and the sidebar slid at ~10 fps; and it drew 10 frames a second doing nothing.
  // Now: after input it draws at the screen's rate until ImGui has used it all and the page has settled
  // (kSettleFrames), then sleeps until the next event (SDL wakes on touch, keys, the activity's events; a note at the
  // bottom wakes it when it goes).
  constexpr int kSettleFrames = 6;
  int settle = kSettleFrames;
  DrawStats stats;
  int drawn = 0;
  while (running) {
    SDL_Event e;
    if (test.tap_x >= 0 && drawn == 3) {  // the PC test's tap, as a phone's touch arrives (SDL_TOUCH_MOUSEID)
      SDL_Event m{};
      m.type = SDL_EVENT_MOUSE_MOTION, m.motion.windowID = SDL_GetWindowID(window), m.motion.which = SDL_TOUCH_MOUSEID;
      m.motion.x = test.tap_x, m.motion.y = test.tap_y;
      SDL_PushEvent(&m);
      SDL_Event b{};
      b.type = SDL_EVENT_MOUSE_BUTTON_DOWN, b.button.windowID = m.motion.windowID, b.button.which = SDL_TOUCH_MOUSEID;
      b.button.button = SDL_BUTTON_LEFT, b.button.down = true, b.button.clicks = 1, b.button.x = test.tap_x, b.button.y = test.tap_y;
      SDL_PushEvent(&b);
      b.type = SDL_EVENT_MOUSE_BUTTON_UP, b.button.down = false;
      SDL_PushEvent(&b);
      app.Log("test: tap at %.0f, %.0f", test.tap_x, test.tap_y);
    }
    if (test.quit_ms && SDL_GetTicks() >= uint64_t(test.quit_ms)) break;
    if (app.Busy()) settle = std::max(settle, 2);
    int wait = app.IdleWaitMs();
    if (test.quit_ms) wait = int(std::clamp<int64_t>(int64_t(test.quit_ms) - int64_t(SDL_GetTicks()), 1, wait < 0 ? INT32_MAX : wait));
    const bool got = (!test.shot.empty() || settle > 0) ? SDL_PollEvent(&e) : SDL_WaitEventTimeout(&e, wait);
    if (!got && settle > 0) --settle;
    if (got) {
      settle = kSettleFrames;
      do {
        ImGui_ImplSDL3_ProcessEvent(&e);
        if (e.type == SDL_EVENT_QUIT) running = false;
        else if (e.type == SDL_EVENT_KEY_DOWN && (e.key.key == SDLK_AC_BACK || e.key.key == SDLK_ESCAPE)) app.Back();
        else if (e.type == SDL_EVENT_RENDER_DEVICE_RESET) {
          ImGui_ImplSDLRenderer3_DestroyDeviceObjects();
          app.DeviceReset();
        }
      } while (SDL_PollEvent(&e));
    }
    const uint64_t t0 = SDL_GetTicksNS();
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    app.Frame();
    ImGui::Render();
    SDL_SetRenderDrawColor(renderer, 17, 18, 21, 255);
    SDL_RenderClear(renderer);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
    if (!test.shot.empty() && ++frame >= test.frames) {
      const bool ok = SaveShot(renderer, test.shot);
      app.Log("shot %s: %s", test.shot.c_str(), ok ? "saved" : SDL_GetError());
      running = false;
    }
    const uint64_t t1 = SDL_GetTicksNS();
    SDL_RenderPresent(renderer);
    stats.Add(app, double(t1 - t0) / 1e6, double(SDL_GetTicksNS() - t0) / 1e6);
    ++drawn;
  }
  stats.Flush(app);
  ImGui_ImplSDLRenderer3_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
