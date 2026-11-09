// Session 98: the Arena Viewer UI (see ui.h). Pure Dear ImGui + std + the launcher's game_font.h; the platform is ui::Backend.
// Look: after src\launcher\launcher_main.cpp (red header bar, slanted tabs, the game's menu face read from the player's own
// game files at run time, kMenuRed). The helpers are copied here (the launcher's are members of its App class).
#define STBI_ONLY_PNG
#include "stb_image.h"  // declarations only (the loader seam holds the implementation); game_font.h needs stbi_zlib_decode_buffer

#include "ui.h"

#include "../id_names.h"
#include "../char_mods.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "imgui_internal.h"  // Session 98 phone port: HitTest (the window list), TouchScroll (ClearActiveID)
#include <filesystem>
#include <fstream>
#include <map>
#include <utility>

#include "freecam.h"
#include "game_font.h"
#include "loader_seam.h"
#include "ui_path.h"
#include "ui_timeline.h"
#include "ui_mods.h"
#include "ui_light.h"
#include "ui_inspect.h"
#include "ui_importer.h"
#include "json_lite.h"

namespace view {
static std::string Upper(std::string t) { for (char& c : t) if (c >= 'a' && c <= 'z') c = char(c - 32); return t; }

namespace ui {
namespace {

namespace fs = std::filesystem;
constexpr ImU32 kMenuRed = IM_COL32(214, 22, 30, 255);
constexpr ImU32 kGrey = IM_COL32(141, 142, 147, 255), kHover = IM_COL32(205, 206, 211, 255);
const char* const kTabNames[] = {"RUNTIME", "ASSETS"};
const char* const kCatNames[] = {"WRESTLER TEXTURES", "ARENA TEXTURES", "VIDEOS"};

struct Video {
  std::string name;
  std::vector<fs::path> frames;  // the exported pictures in time order (first, 2 s, 5 s, 10 s, ..., settled)
};

struct PngTex {
  ImTextureID id = 0;
  int w = 0, h = 0;
};

// Session 98 (entrance from the UI): the UI's wrestler is the actor "ui" (plain label "wrestler N attire M") or, while LOOK =
// ENTRANCE plays his entrance, the entrance actor "ent" (label "actor ent wrestler N attire M": viewer_main.cpp Install marks
// every actor but "ui"). Companions / the announcer ("actor comp ...") stay out.
const char* Bare(const std::string& l) { return l.rfind("actor ent ", 0) == 0 ? l.c_str() + 10 : l.c_str(); }
bool IsWrestler(const std::string& l) { return std::strncmp(Bare(l), "wrestler", 8) == 0; }
bool IsArena(const std::string& l) { return l.rfind("arena", 0) == 0; }

}  // namespace

struct Ui::Impl {
  std::unique_ptr<Backend> be;
  Exec exec;
  std::string game;
  bool ready = false;

  // style / fonts
  float dpi = 0;
  ImFont* menu_fallback = nullptr;
  launcher::GameFont gfont;
  ImTextureID gfont_tex = 0;

  // state
  int tab = 0;      // 0 RUNTIME, 1 ASSETS
  bool sidebar_open = true;
  int stab = 0;            // the sidebar tab: 0 INSPECT, 1 MODS, 2 CAMERA, 3 LIGHT, 4 ENTRANCE (kept for the viewer session)
  bool ent_was_active = false;
  std::string inspect_key;
  std::string camera_mode = "ORBIT";
  const EntranceView* evp = nullptr;  // Session 98 (entrance): the viewer's entrance timeline (a pointer: nothing is copied per frame, Ui::SetEntrance)
  EntranceView ev0;
  float ent_hold = 8.5f;
  bool ent_hold_edit = false;
  TimelineState tls;  // Session 98: the timeline bar (ui_timeline.cpp)
  Camera* fcam = nullptr;      // Session 98: the viewer's camera and free-camera controller (Ui::SetFreeCam)
  FreeCamCtl* fctl = nullptr;
  PathUi* pathui = nullptr;  // Session 98 (camera path)
  std::string msg; // the last problem, shown in the scene bar
  std::vector<std::function<void()>> queued;  // actions from clicks: run by Pump(), which the main loop calls BEFORE DrawFrame
  bool evict_png = false;
  bool want_entrance = false;
  std::vector<int> arena_ids, wrestler_ids;
  bool lists_loaded = false;
  ModsPanel mods;  // Session 98 step 14 (ui_mods.h)
  ImporterPanel importer;  // Session 98 (Superstar Importer, ui_importer.h): active when the viewer runs with --importer
  ImporterCtx ImpCtx() {
    ImporterCtx c;
    c.d = dpi;
    c.text = [this](ImDrawList* dl, ImVec2 p, float px, ImU32 col, const char* t) { Text(dl, p, px, col, t); };
    c.text_raw = [this](ImDrawList* dl, ImVec2 p, float px, ImU32 col, const char* t) { TextRaw(dl, p, px, col, t); };
    c.size = [this](float px, const char* t) { return TextSize(px, t); };
    c.make_tex = [this](const uint8_t* px, int w, int h) { return be->MakeTexture(px, w, h, false); };
    c.free_tex = [this](ImTextureID id) { be->FreeTexture(id); };
    c.exec = exec;
    c.queued = &queued;
    return c;
  }
  PartInfo inspect;  // Session 98 (step 16): the selected part (Ui::SetInspect)

  // assets
  int cat = 0;
  int sel[3] = {-1, -1, -1};
  std::vector<ModelMeta> models;
  std::vector<Video> videos;
  std::string videos_key, videos_dir, videos_note;
  bool videos_dirty = true;
  std::map<std::string, PngTex> png_cache;
  int vid_frame = 0;
  bool vid_play = true;
  double vid_t = 0;
  int vid_sel_seen = -2;

  struct TestPtr {  // Session 98 (phone fixes): the ui_touch test finger (Ui::TestPointer)
    bool active = false, down = false;
    float x = 0, y = 0;
  } test_ptr;

  // shot
  struct Shot {
    bool queued = false, done = false;
    std::string path, err;
    int frames = 0;
  } shot;

  ~Impl() {
    if (be) {
      for (auto& kv : png_cache) be->FreeTexture(kv.second.id);
      if (gfont_tex) be->FreeTexture(gfont_tex);
      be.reset();  // (its Shutdown needs the ImGui context)
    }
    if (ready) ImGui::DestroyContext();
  }

  // ---------------------------------------------------------------- style
  void ApplyStyle(float d) {
    ImGuiStyle& st = ImGui::GetStyle();
    st = ImGuiStyle();
    ImGui::StyleColorsDark();
    st.WindowRounding = 0, st.FrameRounding = 2, st.GrabRounding = 2, st.TabRounding = 0, st.ChildRounding = 0;
    st.PopupRounding = 2, st.FramePadding = ImVec2(9, 6), st.ItemSpacing = ImVec2(9, 7), st.WindowPadding = ImVec2(16, 14);
    ImVec4* c = st.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.075f, 0.08f, 0.095f, 1);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = ImVec4(0.07f, 0.075f, 0.088f, 1);
    c[ImGuiCol_FrameBg] = ImVec4(0.13f, 0.135f, 0.16f, 0.95f);
    c[ImGuiCol_Header] = ImVec4(0.55f, 0.12f, 0.12f, 0.55f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.65f, 0.16f, 0.16f, 0.75f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.72f, 0.18f, 0.18f, 1);
    c[ImGuiCol_Button] = ImVec4(0.20f, 0.205f, 0.24f, 1);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.55f, 0.11f, 0.13f, 1);
    c[ImGuiCol_ButtonActive] = ImVec4(0.40f, 0.07f, 0.09f, 1);
    c[ImGuiCol_CheckMark] = ImVec4(0.95f, 0.35f, 0.3f, 1);
    st.ScaleAllSizes(d);
    st.FontScaleDpi = d;
    st.FontSizeBase = 17.0f;
    dpi = d;
  }

  bool Init(std::unique_ptr<Backend> b, const std::string& g, Exec e, std::string& err) {
    be = std::move(b);
    exec = std::move(e);
    mods.SetExec(exec);
    game = g;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ready = true;
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
#if !defined(WWE_VIEWER_DEBUG) && !defined(_DEBUG)
    // Session 98 (first phone test: an "In window '##assets': Code uses SetCursorPos() ..." box over the VIDEOS panel): a player
    // never sees Dear ImGui's recoverable-error assert or its red error tooltip. ImGui recovers by itself; the error goes to the
    // viewer's log (stderr: logs\viewer.log on the phone), each distinct one once. A debug build (build_viewer.ps1 -DebugUi, or
    // -DWWE_VIEWER_DEBUG) keeps ImGui's defaults (assert + tooltip) so the mistakes are still caught while working on the UI.
    io.ConfigErrorRecoveryEnableAssert = false;
    io.ConfigErrorRecoveryEnableTooltip = false;
    io.ConfigErrorRecoveryEnableDebugLog = false;
    GImGui->ErrorCallback = [](ImGuiContext* ctx, void*, const char* m) {
      static std::map<std::string, int> seen;
      const std::string where = ctx && ctx->CurrentWindow ? ctx->CurrentWindow->Name : "NULL";
      if (seen[where + "|" + m]++ == 0) std::fprintf(stderr, "[ui] imgui error (recovered) in window '%s': %s\n", where.c_str(), m);
    };
#endif
    const std::string seg = be->SystemFont("segoeui.ttf"), blk = be->SystemFont("ariblk.ttf");
    if (!seg.empty()) io.Fonts->AddFontFromFileTTF(seg.c_str(), 17.0f);
    else io.Fonts->AddFontDefault();
    if (!blk.empty()) menu_fallback = io.Fonts->AddFontFromFileTTF(blk.c_str(), 17.0f);
    ApplyStyle(be->Dpi());
    if (!be->Init(err)) return false;
    LoadMenuFont();
    return true;
  }

  // The game's own menu face from the player's files (nothing is shipped): launcher::LoadGameFont.
  void LoadMenuFont() {
    if (game.empty()) return;
    launcher::GameFiles files;
    files.SetRoot(fs::u8path(game));
    if (!launcher::LoadGameFont(files, gfont)) return;
    gfont_tex = be->MakeTexture(gfont.bc3.data(), gfont.sheet_w, gfont.sheet_h, true);
  }

  // ---------------------------------------------------------------- menu text (copied from the launcher)
  static uint32_t NextCodepoint(const char*& p) {
    const auto c = static_cast<unsigned char>(*p++);
    if (c < 0x80) return c;
    const int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    uint32_t cp = c & (0x3Fu >> extra);
    for (int i = 0; i < extra && (static_cast<unsigned char>(*p) & 0xC0) == 0x80; ++i) cp = cp << 6 | (*p++ & 0x3F);
    return cp;
  }
  ImVec2 TextSize(float px, const char* text) {
    if (gfont_tex) {
      const float k = px / gfont.cell;
      float w = 0;
      for (const char* p = text; *p;) {
        const auto it = gfont.glyphs.find(NextCodepoint(p));
        w += (it != gfont.glyphs.end() ? it->second.w : gfont.cell * 0.33f) * k;
      }
      return ImVec2(w, px);
    }
    ImGui::PushFont(menu_fallback, px / dpi);
    const ImVec2 sz = ImGui::CalcTextSize(text);
    ImGui::PopFont();
    return sz;
  }
  void TextRaw(ImDrawList* dl, ImVec2 pos, float px, ImU32 col, const char* text) {
    if (!gfont_tex) {
      ImGui::PushFont(menu_fallback, px / dpi);
      dl->AddText(pos, col, text);
      ImGui::PopFont();
      return;
    }
    const float k = px / gfont.cell, iw = 1.f / float(gfont.sheet_w), ih = 1.f / float(gfont.sheet_h);
    const ImTextureRef tex(gfont_tex);
    float x = std::floor(pos.x);
    const float y = std::floor(pos.y);
    for (const char* p = text; *p;) {
      const auto it = gfont.glyphs.find(NextCodepoint(p));
      if (it == gfont.glyphs.end()) {
        x += gfont.cell * 0.33f * k;
        continue;
      }
      const launcher::GameFont::Glyph& g = it->second;
      dl->AddImage(tex, ImVec2(x, y), ImVec2(x + g.w * k, y + g.h * k), ImVec2(g.x * iw, g.y * ih),
                   ImVec2((g.x + g.w) * iw, (g.y + g.h) * ih), col);
      x += g.w * k;
    }
  }
  void Text(ImDrawList* dl, ImVec2 pos, float px, ImU32 col, const char* text) {
    const float o = std::max(1.f, px * 0.06f);
    TextRaw(dl, ImVec2(pos.x + o, pos.y + o), px, IM_COL32(0, 0, 0, 190), text);
    TextRaw(dl, pos, px, col, text);
  }

  // A slanted tab strip at an explicit position (the launcher's GameTabs: the selected one a black slanted bar with red text).
  bool Tabs(const char* const* names, int count, int& selected, ImVec2 at, float px, float h, float pad_dp = 18, float sk_dp = 9, float gap_dp = 2) {
    const float d = dpi, pad = pad_dp * d, sk = sk_dp * d;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const int before = selected;
    float x = at.x;
    for (int i = 0; i < count; ++i) {
      const ImVec2 ts = TextSize(px, names[i]), pos(x, at.y), size(ts.x + pad * 2, h);
      ImGui::SetCursorScreenPos(pos);
      ImGui::PushID(i);
      if (ImGui::InvisibleButton(names[i], size)) selected = i;
      ImGui::PopID();
      const bool on = selected == i, hover = ImGui::IsItemHovered();
      if (on || hover) {
        const ImVec2 q[4] = {{pos.x + sk, pos.y}, {pos.x + size.x, pos.y}, {pos.x + size.x - sk, pos.y + h}, {pos.x, pos.y + h}};
        dl->AddConvexPolyFilled(q, 4, on ? IM_COL32(0, 0, 0, 255) : IM_COL32(0, 0, 0, 90));
      }
      const ImVec2 tp(pos.x + pad, pos.y + (h - ts.y) / 2);
      if (on) TextRaw(dl, tp, px, kMenuRed, names[i]);
      else Text(dl, tp, px, hover ? kHover : IM_COL32(215, 215, 220, 255), names[i]);
      x += size.x + gap_dp * d;
    }
    return selected != before;
  }

  // A list entry like the game's second menu column: grey, white with the red arrow when selected.
  bool ListItem(const char* label, bool selected, float px, float width) {
    const float d = dpi, h = px * 1.2f, arrow = px * 0.5f;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(label, ImVec2(width, h));
    const bool hover = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (selected) {
      const float cy = pos.y + h / 2;
      dl->AddTriangleFilled(ImVec2(pos.x, cy - arrow / 2), ImVec2(pos.x + arrow * 0.9f, cy), ImVec2(pos.x, cy + arrow / 2), IM_COL32_WHITE);
    }
    const ImU32 col = selected ? IM_COL32_WHITE : hover ? IM_COL32(235, 225, 225, 255) : IM_COL32(215, 170, 172, 255);
    Text(dl, ImVec2(pos.x + arrow + 8 * d, pos.y + (h - px) / 2), px, col, label);
    return pressed;
  }

  // Session 98 (step 16): ASSETS tab entry of texture `slot` of the shown model `model` (same order as AssetsTab's items), or the VIDEOS list.
  void OpenTexture(int model, int slot) {
    if (model < 0 || model >= (int)models.size() || slot < 0) return;
    const bool is_w = IsWrestler(models[(size_t)model].label), is_a = IsArena(models[(size_t)model].label);
    if (!is_w && !is_a) return;
    const int c = is_w ? 0 : 1;
    int idx = 0;
    for (size_t mi = 0; mi < models.size(); mi++) {
      const bool w2 = IsWrestler(models[mi].label), a2 = IsArena(models[mi].label);
      if ((c == 0 && !w2) || (c == 1 && !a2)) continue;
      if ((int)mi == model) {
        if (slot < (int)models[mi].textures.size()) tab = 1, cat = c, sel[c] = idx + slot;
        return;
      }
      idx += (int)models[mi].textures.size();
    }
  }
  void InspectPanel(float d, float w) {
    ImGui::SetCursorPosX(16 * d);
    ImGui::BeginChild("##inspect", ImVec2(w - 32 * d, (inspect.valid ? 470 : 96) * d), false);
    InspectDraw D;
    D.d = d, D.red = kMenuRed;
    D.text = [this](ImDrawList* dl, ImVec2 p, float px, ImU32 col, const char* t) { Text(dl, p, px, col, t); };
    D.width = [this](float px, const char* t) { return TextSize(px, t).x; };
    D.open_texture = [this](int m, int s) { OpenTexture(m, s); };
    D.open_movie = [this](const std::string&) { tab = 1, cat = 2, sel[2] = 0, videos_dirty = true; };
    D.mod = [this](const std::string& a) { exec("{\"cmd\":\"mod\",\"action\":\"" + a + "\"}"); };
    D.deselect = [this] { exec("{\"cmd\":\"select\",\"clear\":true}"); };
    DrawInspect(inspect.valid ? &inspect : nullptr, models, D, w - 32 * d - 14 * d);
    ImGui::EndChild();
  }

  void SectionHead(const char* t) {
    TextRaw(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), 15 * dpi, kMenuRed, t);
    ImGui::Dummy(ImVec2(0, 15 * dpi + 4 * dpi));
  }

  // ---------------------------------------------------------------- scene state (read back from the shown models)
  struct Ids {
    int arena = -1, wrestler = -1, attire = 0;
  };
  Ids Scan() const {
    Ids s;
    bool have_ui = false;
    for (const ModelMeta& m : models) {
      int a = 0, b = 0;
      if (std::sscanf(m.label.c_str(), "arena %d", &a) == 1) s.arena = a;
      else if (std::sscanf(m.label.c_str(), "wrestler %d attire %d", &a, &b) == 2) s.wrestler = a, s.attire = b, have_ui = true;
      else if (!have_ui && std::sscanf(m.label.c_str(), "actor ent wrestler %d attire %d", &a, &b) == 2)  // (entrance)
        s.wrestler = a, s.attire = ent_ui_attire >= 0 ? ent_ui_attire : b;  // (the UI's choice, not the entrance model's PACH)
    }
    return s;
  }

  bool Ok(const std::string& reply) { return reply.find("\"ok\":true") != std::string::npos; }
  std::string ErrOf(const std::string& reply) {
    const size_t p = reply.find("\"error\":\"");
    if (p == std::string::npos) return reply.substr(0, 120);
    const size_t e = reply.find('"', p + 9);
    return reply.substr(p + 9, e == std::string::npos ? std::string::npos : e - p - 9);
  }

  // Session 98 (first phone test: "LOOK ENTRANCE doesn't play any wrestler's entrance"): the entrance the UI plays. Before, LOOK =
  // ENTRANCE only sent the lights / stage look and nothing in the UI ever sent the entrance command. Now it plays the UI's wrestler's
  // entrance as the actor "ent" (viewer_main DoEntrance) in the UI's attire, with the ENTRANCE tab's options kept across wrestler
  // changes (a new entrance command resets them in the viewer). The CHAMPION choices are the ENTRANCE tab's (entrance_title).
  struct EntOpts {
    std::string title = "none";  // none / hold / waist / auto
    int belt = 0;                // 0 = the title's default belt
    bool champ_bank = false;     // the wrestler's Entrance06 champion bank
    bool lights = true, cue0_show = false;
    double intro_hold = 0;
  } ent_opts;
  std::string EntranceCmd(int wrestler, int attire) const {
    char b[96];
    std::snprintf(b, sizeof b, "%.2f", ent_opts.intro_hold);
    // (Session 98 review, Jarrod's goal "look like the game": attire 0 = no attire argument, so the game's entrance model plays -
    // the pac's own entrance entry; another attire choice passes through)
    std::string c = "{\"cmd\":\"entrance\",\"wrestler\":" + std::to_string(wrestler) + (attire > 0 ? ",\"attire\":" + std::to_string(attire) : std::string()) +
                    ",\"play\":true,\"camera\":\"entrance\",\"title\":\"" + ent_opts.title + "\",\"lights\":" + (ent_opts.lights ? "true" : "false") +
                    ",\"cue0\":\"" + (ent_opts.cue0_show ? "show" : "hide") + "\",\"intro_hold\":" + b;
    if (ent_opts.belt > 0) c += ",\"belt\":" + std::to_string(ent_opts.belt);
    return c + "}";
  }
  // Starts (or restarts) the UI wrestler's entrance; false = it could not (msg set, the caller shows him standing instead).
  int ent_ui_attire = -1;  // the UI's attire choice behind the entrant (his model may be the entrance one: Scan reports this)
  bool StartEntrance(int wrestler, int attire) {
    exec("{\"cmd\":\"remove\",\"id\":\"ui\"}");  // (one copy of him: the entrance actor)
    ent_ui_attire = attire;
    std::string r = exec(EntranceCmd(wrestler, attire));
    if (!Ok(r)) {
      msg = r.find("no_attire") != std::string::npos ? "no more attires" : "entrance: " + ErrOf(r);
      exec("{\"cmd\":\"entrance_stop\"}");
      return false;
    }
    if (ent_opts.champ_bank) {  // (a wrestler with no champion bank of his own: his own entrance stays, said in the scene bar)
      r = exec("{\"cmd\":\"entrance_title\",\"champ_bank\":true}");
      if (!Ok(r)) msg = "champion entrance: " + ErrOf(r);
    }
    return true;
  }

  // Rebuild the shown scene: the arena (if any), then the wrestler on top (standing, or playing his entrance under LOOK =
  // ENTRANCE). Returns false (msg set) when a command failed.
  bool Rebuild(int arena, int wrestler, int attire, bool relook) {
    std::string r;
    const Ids cur = Scan();
    if (relook) {
      if (want_entrance) {
        r = exec("{\"cmd\":\"lights\",\"mode\":\"entrance\"}");
        if (!Ok(r)) return msg = "entrance look: " + ErrOf(r), want_entrance = false, false;
      } else {
        exec("{\"cmd\":\"entrance_stop\"}");  // (before the reloads below: the entrance's belt / props / companions go with it)
        exec("{\"cmd\":\"lights\",\"mode\":\"match\"}");
        exec("{\"cmd\":\"stage\",\"wrestler\":-1}");
      }
    }
    // (the lights / stage commands above already re-loaded the same arena and kept the scene's actors: no plain load then)
    if (arena >= 0) {
      if (cur.arena != arena) {
        r = exec("{\"cmd\":\"load\",\"kind\":\"arena\",\"id\":" + std::to_string(arena) + "}");
        if (!Ok(r)) return msg = "arena " + std::to_string(arena) + ": " + ErrOf(r), false;
      }
    } else {
      exec("{\"cmd\":\"clear\"}");
    }
    if (want_entrance) {
      std::string why;
      if (wrestler < 0) {
        exec("{\"cmd\":\"entrance_stop\"}");
        exec("{\"cmd\":\"remove\",\"id\":\"ui\"}");
        if (relook) exec("{\"cmd\":\"stage\",\"step\":1}");  // (no wrestler: the next wrestler's tron look, as before)
        msg = arena >= 0 ? "ENTRANCE: pick a wrestler to play his entrance" : "ENTRANCE: pick an arena and a wrestler";
        return true;
      }
      if (arena < 0) why = "ENTRANCE: pick an arena to play the entrance in";
      else if (assets::char_mods::IsCustom(wrestler)) why = "ENTRANCE: an imported character has no entrance of his own";
      else if (msg.clear(), StartEntrance(wrestler, attire)) return true;  // (msg: empty, or the champion note)
      else why = msg;
      // (no entrance: he stands at the ring in the entrance look, with the reason in the scene bar)
      if (arena >= 0) exec("{\"cmd\":\"stage\",\"wrestler\":" + std::to_string(wrestler) + "}");
      const bool ok = PlaceUi(arena, wrestler, attire);
      if (ok) msg = why;
      return ok;
    }
    if (!PlaceUi(arena, wrestler, attire)) return false;
    msg.clear();
    return true;
  }
  bool PlaceUi(int arena, int wrestler, int attire) {
    std::string r;
    // The UI's wrestler is the actor "ui" (one model path): at the ring in an arena, at the origin without one.
    if (wrestler >= 0) {
      const std::string cmd = std::string("{\"cmd\":\"place\",\"id\":\"ui\",\"wrestler\":") + std::to_string(wrestler) + ",\"attire\":" +
                              std::to_string(attire) + (arena >= 0 ? ",\"spot\":\"ring\"" : ",\"pos\":[0,0,0]") + ",\"motion\":\"idle\"}";  // (Session 98: idle, not the T-pose)
      r = exec(cmd);
      if (!Ok(r)) return msg = r.find("no_attire") != std::string::npos ? "no more attires" : "wrestler " + std::to_string(wrestler) + ": " + ErrOf(r), false;
      if (arena < 0) exec("{\"cmd\":\"camera\",\"preset\":\"three_quarter\"}");
    } else {
      exec("{\"cmd\":\"remove\",\"id\":\"ui\"}");
    }
    msg.clear();
    return true;
  }
  void Request(int arena, int wrestler, int attire, bool relook) {
    const Ids prev = Scan();
    queued.push_back([=, this] {
      if (!Rebuild(arena, wrestler, attire, relook)) Rebuild(prev.arena, prev.wrestler, prev.attire, false);  // put the old scene back
    });
  }

  // ---------------------------------------------------------------- PNG frames (the tron export)
  PngTex Png(const fs::path& p) {
    const auto u8 = p.u8string();
    const std::string key(u8.begin(), u8.end());
    auto it = png_cache.find(key);
    if (it != png_cache.end()) return it->second;
    if (png_cache.size() > 300) evict_png = true;  // freed at the top of the next Draw
    PngTex t;
    std::ifstream f(p, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    int w = 0, h = 0, n = 0;
    if (!bytes.empty())
      if (stbi_uc* px = stbi_load_from_memory(bytes.data(), (int)bytes.size(), &w, &h, &n, 4)) {
        t.id = be->MakeTexture(px, w, h, false);
        t.w = w, t.h = h;
        stbi_image_free(px);
      }
    return png_cache[key] = t;
  }

  void RefreshVideos(const Ids& ids) {
    videos.clear();
    std::vector<int> wl;
    std::string dir;
    ViewerStageWrestlers("", wl, dir);
    videos_dir = dir;
    videos_note.clear();
    if (dir.empty() || !fs::is_directory(fs::u8path(dir))) {
#ifdef __ANDROID__
      videos_note = "No tron previews yet. In the launcher: ARENA VIEWER page, BUILD TRON PREVIEWS (the game runs once and makes them).";
#else
      videos_note = "No tron export found. Run the launcher's Build previews (or --wwe2k14_tron_export=DIR).";
#endif
      return;
    }
    std::vector<std::string> top, evt;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(fs::u8path(dir), ec))
      if (e.path().extension() == ".png") top.push_back(e.path().filename().string());
    for (auto& e : fs::directory_iterator(fs::u8path(dir) / "evtmov", ec))
      if (e.path().extension() == ".png") evt.push_back(e.path().filename().string());
    std::sort(top.begin(), top.end());
    std::sort(evt.begin(), evt.end());
    auto add = [&](const std::string& title, const std::vector<std::string>& names, const fs::path& base, const std::string& stem) {
      Video v;
      v.name = title;
      std::vector<std::string> mid;
      std::string first, end;
      for (const std::string& n : names) {
        if (n == stem + ".png") first = n;
        else if (n == stem + "_end.png") end = n;
        else if (n.rfind(stem + "_t", 0) == 0) mid.push_back(n);
      }
      if (!first.empty()) v.frames.push_back(base / first);
      for (auto& n : mid) v.frames.push_back(base / n);
      if (!end.empty()) v.frames.push_back(base / end);
      if (!v.frames.empty()) videos.push_back(std::move(v));
    };
    const fs::path d = fs::u8path(dir);
    char b[32];
    if (ids.wrestler >= 0) {
      add("TITANTRON " + std::to_string(ids.wrestler), top, d, std::to_string(ids.wrestler));
      for (const char* g : {"MVSW", "MVSB"}) {
        std::snprintf(b, sizeof b, "%s_M%03d", g, ids.wrestler);
        add(std::string(g) + " M" + std::to_string(ids.wrestler), evt, d / "evtmov", b);
      }
    }
    if (ids.arena >= 0)
      for (const char* g : {"MOV", "MOVT", "MVAB", "MOVL", "MOVN"}) {
        std::snprintf(b, sizeof b, "%s_M%03d", g, ids.arena);
        add(std::string(g) + " M" + std::to_string(ids.arena), evt, d / "evtmov", b);
      }
    if (videos.empty()) videos_note = "No exported frames for this arena / wrestler in " + dir;
  }

  // ---------------------------------------------------------------- drawing
  void CheckerRect(ImDrawList* dl, ImVec2 a, ImVec2 b, float sq) {
    dl->AddRectFilled(a, b, IM_COL32(52, 53, 58, 255));
    for (float y = a.y, r = 0; y < b.y; y += sq, r++)
      for (float x = a.x + (std::fmod(r, 2.f) ? 0 : sq), c = 0; x < b.x; x += 2 * sq, c++)
        dl->AddRectFilled(ImVec2(x, y), ImVec2(std::min(x + sq, b.x), std::min(y + sq, b.y)), IM_COL32(74, 75, 80, 255));
  }
  // Fit an image into a box, centred, without upscaling past `max_up`.
  void DrawFit(ImDrawList* dl, ImTextureID id, int iw, int ih, ImVec2 a, ImVec2 b, float max_up) {
    if (!id || iw <= 0 || ih <= 0) {
      dl->AddRect(a, b, IM_COL32(90, 40, 44, 255));
      dl->AddLine(a, b, IM_COL32(90, 40, 44, 255)), dl->AddLine(ImVec2(a.x, b.y), ImVec2(b.x, a.y), IM_COL32(90, 40, 44, 255));
      return;
    }
    const float s = std::min({(b.x - a.x) / iw, (b.y - a.y) / ih, max_up});
    const ImVec2 sz(iw * s, ih * s), o(a.x + ((b.x - a.x) - sz.x) / 2, a.y + ((b.y - a.y) - sz.y) / 2);
    dl->AddImage(ImTextureRef(id), o, ImVec2(o.x + sz.x, o.y + sz.y));
  }

  struct Item {
    std::string name;
    ImTextureID id = 0;
    int w = 0, h = 0;
    int model = -1, slot = -1;  // textures: where it came from
    int video = -1;
  };

  void Cell(const Item& it, bool selected, float cell, float thumb, bool& clicked, int idx) {
    const float d = dpi;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(idx);
    clicked = ImGui::InvisibleButton("##c", ImVec2(cell - 4 * d, cell + 18 * d));
    ImGui::PopID();
    const bool hover = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a = p, b(p.x + cell - 4 * d, p.y + cell + 18 * d);
    dl->AddRectFilled(a, b, selected ? IM_COL32(120, 16, 22, 200) : hover ? IM_COL32(60, 62, 72, 200) : IM_COL32(28, 29, 35, 220));
    if (selected) dl->AddRect(a, b, kMenuRed, 0, 0, 2 * d);
    const float m = 4 * d;
    DrawFit(dl, it.id, it.w, it.h, ImVec2(a.x + m, a.y + m), ImVec2(a.x + thumb + m, a.y + thumb + m), 4.f);
    const ImVec4 clip(a.x + m, a.y, b.x - m, b.y);
    dl->AddText(ImGui::GetFont(), 13 * d, ImVec2(a.x + m, a.y + thumb + m + 2 * d), IM_COL32(225, 226, 230, 255), it.name.c_str(), nullptr, 0.f, &clip);
  }

  float grid_scroll = 0, preview_scroll = 0, preview_scroll_max = 0;  // (the ui_state test hook: the ASSETS panels' scroll)
  void AssetsTab(ImVec2 origin, ImVec2 size) {
    const float d = dpi;
    ImGui::SetNextWindowPos(origin);
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.067f, 0.071f, 0.082f, 1));
    ImGui::Begin("##assets", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float left_w = 300 * d, right_w = std::min(480 * d, size.x * 0.4f);
    // left: the categories on a red slanted panel
    const float sl = 30 * d;
    {
      const ImVec2 q[4] = {{origin.x, origin.y}, {origin.x + left_w, origin.y}, {origin.x + left_w - sl, origin.y + size.y}, {origin.x, origin.y + size.y}};
      dl->AddConvexPolyFilled(q, 4, IM_COL32(150, 18, 26, 255));
      dl->AddRectFilledMultiColor(ImVec2(origin.x, origin.y), ImVec2(origin.x + left_w - sl, origin.y + size.y), IM_COL32(170, 20, 28, 255),
                                  IM_COL32(170, 20, 28, 255), IM_COL32(100, 10, 16, 255), IM_COL32(100, 10, 16, 255));
      dl->AddLine(q[1], q[2], IM_COL32(255, 255, 255, 140), 1.6f * d);
    }
    ImGui::SetCursorScreenPos(ImVec2(origin.x + 24 * d, origin.y + 24 * d));
    ImGui::BeginGroup();
    for (int i = 0; i < 3; i++) {
      if (ListItem(kCatNames[i], cat == i, 22 * d, left_w - 70 * d)) cat = i, videos_dirty = true;
      ImGui::Dummy(ImVec2(0, 6 * d));
    }
    ImGui::EndGroup();

    // items of the chosen category
    std::vector<Item> items;
    const Ids ids = Scan();
    if (cat < 2) {
      for (size_t mi = 0; mi < models.size(); mi++) {
        const bool is_w = IsWrestler(models[mi].label), is_a = IsArena(models[mi].label);
        if ((cat == 0 && !is_w) || (cat == 1 && !is_a)) continue;
        for (size_t t = 0; t < models[mi].textures.size(); t++) {
          const TexMeta& tm = models[mi].textures[t];
          Item it;
          it.name = tm.name, it.id = tm.id, it.w = (int)tm.width, it.h = (int)tm.height, it.model = (int)mi, it.slot = (int)t;
          items.push_back(std::move(it));
        }
      }
    } else {
      std::string key = std::to_string(ids.arena) + "/" + std::to_string(ids.wrestler);
      if (videos_dirty || key != videos_key) RefreshVideos(ids), videos_key = key, videos_dirty = false;
      for (size_t i = 0; i < videos.size(); i++) {
        Item it;
        it.name = videos[i].name, it.video = (int)i;
        const PngTex t = Png(videos[i].frames[0]);
        it.id = t.id, it.w = t.w, it.h = t.h;
        items.push_back(std::move(it));
      }
    }
    int& s = sel[cat];
    if (s >= (int)items.size()) s = -1;
    if (s < 0 && !items.empty()) s = 0;

    // centre: the grid
    const float cx = origin.x + left_w + 16 * d, cw = size.x - left_w - right_w - 32 * d;
    const std::string head = std::string(kCatNames[cat]) + "  (" + std::to_string(items.size()) + ")";
    TextRaw(dl, ImVec2(cx, origin.y + 14 * d), 15 * d, kMenuRed, head.c_str());
    ImGui::SetCursorScreenPos(ImVec2(cx, origin.y + 14 * d + 24 * d));
    ImGui::BeginChild("##grid", ImVec2(cw, size.y - 52 * d), false);
    if (items.empty()) {
      ImGui::PushTextWrapPos(0);
      if (cat == 2) ImGui::TextDisabled("%s", videos_note.c_str());
      else ImGui::TextDisabled("Nothing loaded: pick a %s on the RUNTIME tab.", cat == 0 ? "wrestler" : "arena");
      ImGui::PopTextWrapPos();
    } else {
      const float cell = 128 * d, thumb = cell - 12 * d, rowh = cell + 22 * d;
      const int cols = std::max(1, (int)(ImGui::GetContentRegionAvail().x / cell));
      const int rows = ((int)items.size() + cols - 1) / cols;
      ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
      ImGuiListClipper clip;
      clip.Begin(rows, rowh);
      while (clip.Step())
        for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
          const ImVec2 p0 = ImGui::GetCursorScreenPos();
          ImGui::Dummy(ImVec2(cols * cell, rowh));
          for (int c = 0; c < cols && r * cols + c < (int)items.size(); c++) {
            const int i = r * cols + c;
            ImGui::SetCursorScreenPos(ImVec2(p0.x + c * cell, p0.y));
            bool clicked = false;
            Cell(items[i], s == i, cell, thumb, clicked, i);
            if (clicked) s = i;
          }
          ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + rowh));
        }
      ImGui::PopStyleVar();
    }
    TouchScroll();  // (Session 98 phone fixes: a finger drags the grid; a tap still picks)
    grid_scroll = ImGui::GetScrollY();
    ImGui::EndChild();

    // right: the preview
    const float rx = origin.x + size.x - right_w;
    dl->AddRectFilled(ImVec2(rx, origin.y), ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(20, 21, 26, 255));
    dl->AddRectFilled(ImVec2(rx, origin.y), ImVec2(rx + 3 * d, origin.y + size.y), kMenuRed);
    // (Session 98 phone test: drawn at its position, no SetCursorScreenPos - with nothing selected no item followed it and ImGui
    // reported "SetCursorPos() to extend window/parent boundaries" at End(); every cursor move in this file is followed by an item.)
    TextRaw(dl, ImVec2(rx + 18 * d, origin.y + 14 * d), 15 * d, kMenuRed, "PREVIEW");
    // Session 98 (phone test: "you can't scroll the right panel that shows the details"): the picture and its details live in their
    // own child window, so what does not fit scrolls (the wheel, or a finger drag via TouchScroll). Drawn with the child's draw list
    // at its cursor (the scroll moves it).
    ImGui::SetCursorScreenPos(ImVec2(rx + 3 * d, origin.y + 40 * d));
    ImGui::BeginChild("##preview", ImVec2(right_w - 3 * d, size.y - 40 * d), false);
    if (s >= 0 && s < (int)items.size()) {
      ImDrawList* dl = ImGui::GetWindowDrawList();
      const Item& it = items[s];
      const ImVec2 c0 = ImGui::GetCursorScreenPos();
      const ImVec2 a(rx + 18 * d, c0.y + 4 * d), b(origin.x + size.x - 18 * d, c0.y + 4 * d + (right_w - 36 * d) * 0.72f);
      CheckerRect(dl, a, b, 10 * d);
      ImTextureID id = it.id;
      int w = it.w, h = it.h;
      std::string l1, l2, l3, l4;
      if (it.video >= 0) {
        const Video& v = videos[it.video];
        const int n = (int)v.frames.size();
        if (vid_sel_seen != it.video) vid_sel_seen = it.video, vid_frame = 0, vid_t = 0;
        if (vid_play && n > 1) {
          vid_t += ImGui::GetIO().DeltaTime;
          if (vid_t > 0.9) vid_t = 0, vid_frame = (vid_frame + 1) % n;
        }
        vid_frame = std::clamp(vid_frame, 0, n - 1);
        const PngTex t = Png(v.frames[vid_frame]);
        id = t.id, w = t.w, h = t.h;
        l1 = v.name;
        l2 = "frame " + std::to_string(vid_frame + 1) + " / " + std::to_string(n) + ":  " + v.frames[vid_frame].filename().string();
        l3 = "folder:  " + v.frames[vid_frame].parent_path().string();
        l4 = std::to_string(w) + " x " + std::to_string(h) + "  (exported picture; no video decoding)";
      } else {
        const ModelMeta& mm = models[it.model];
        const TexMeta& tm = mm.textures[it.slot];
        l1 = tm.name;
        l2 = "model:  " + mm.label + "   slot " + std::to_string(it.slot) + " of " + std::to_string(mm.textures.size());
        l3 = std::to_string(tm.width) + " x " + std::to_string(tm.height) + "   " + tm.format + "   " + std::to_string(tm.mips) + " mip" + (tm.mips == 1 ? "" : "s");
        l4 = tm.id ? "" : "upload failed (no picture)";
      }
      DrawFit(dl, id, w, h, a, b, 8.f);
      ImGui::SetCursorScreenPos(ImVec2(a.x, b.y + 12 * d));
      ImGui::BeginGroup();
      float px = 24 * d;
      const float room = b.x - a.x;
      if (const float tw = TextSize(px, l1.c_str()).x; tw > room) px *= room / tw;
      Text(dl, ImGui::GetCursorScreenPos(), px, IM_COL32_WHITE, l1.c_str());
      ImGui::Dummy(ImVec2(room, px + 6 * d));
      ImGui::PushTextWrapPos(b.x - ImGui::GetWindowPos().x);
      ImGui::TextUnformatted(l2.c_str());
      ImGui::TextUnformatted(l3.c_str());
      if (!l4.empty()) ImGui::TextDisabled("%s", l4.c_str());
      ImGui::PopTextWrapPos();
      if (it.video >= 0 && videos[it.video].frames.size() > 1) {
        const int n = (int)videos[it.video].frames.size();
        if (ImGui::Button("< PREV")) vid_frame = (vid_frame + n - 1) % n, vid_play = false;
        ImGui::SameLine();
        if (ImGui::Button(vid_play ? "PAUSE" : "PLAY")) vid_play = !vid_play;
        ImGui::SameLine();
        if (ImGui::Button("NEXT >")) vid_frame = (vid_frame + 1) % n, vid_play = false;
      }
      ImGui::Dummy(ImVec2(0, 24 * d));  // (room under the last line, so it can be scrolled clear of the screen edge)
      ImGui::EndGroup();
    }
    TouchScroll();
    preview_scroll = ImGui::GetScrollY(), preview_scroll_max = ImGui::GetScrollMaxY();
    ImGui::EndChild();
    ImGui::End();
  }

  // Session 98 (custom characters): imported community characters (the MODS toggle on), refreshed about once a second.
  std::vector<std::pair<int, std::string>> custom_list;
  std::chrono::steady_clock::time_point custom_at{};
  void RefreshCustom() {
    const auto now = std::chrono::steady_clock::now();
    if (now - custom_at < std::chrono::seconds(1) && custom_at.time_since_epoch().count() != 0) return;
    custom_at = now;
    custom_list.clear();
    try {
      for (const auto& e : assets::char_mods::List()) custom_list.push_back({e.id, e.name});
    } catch (...) {
    }
  }
  std::string WrestlerLabel(int w) const {
    if (assets::char_mods::IsCustom(w)) {
      for (const auto& c : custom_list)
        if (c.first == w) return c.second + "  (custom)";
      return "Custom character";
    }
    return assets::CharacterLabel(w) + "  (" + std::to_string(w) + ")";
  }

  void SceneBar(float top) {
    const float d = dpi;
    ImGui::SetNextWindowPos(ImVec2(12 * d, top + 10 * d));
    ImGui::SetNextWindowSize(ImVec2(300 * d, 0));
    ImGui::SetNextWindowBgAlpha(0.72f);
    ImGui::Begin("##scenebar", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing);
    if (!lists_loaded) lists_loaded = true, arena_ids = ViewerList("arena"), wrestler_ids = ViewerList("wrestler");
    const Ids ids = Scan();
    ImGui::PushItemWidth(-1);
    const std::string an = ids.arena >= 0 ? assets::ArenaLabel(ids.arena) + "  (" + std::to_string(ids.arena) + ")" : "(no arena)";  // Session 98: names from db\ids
    if (ImGui::BeginCombo("##arena", an.c_str())) {
      if (ImGui::Selectable("(no arena)", ids.arena < 0) && ids.arena >= 0) Request(-1, ids.wrestler, ids.attire, false);
      for (int a : arena_ids)
        if (ImGui::Selectable((assets::ArenaLabel(a) + "  (" + std::to_string(a) + ")##a" + std::to_string(a)).c_str(), a == ids.arena) && a != ids.arena) Request(a, ids.wrestler, ids.attire, false);
      TouchScroll();
      ImGui::EndCombo();
    }
    RefreshCustom();
    const std::string wn = ids.wrestler >= 0 ? WrestlerLabel(ids.wrestler) : "(no wrestler)";
    if (ImGui::BeginCombo("##wrestler", wn.c_str())) {
      if (ImGui::Selectable("(no wrestler)", ids.wrestler < 0) && ids.wrestler >= 0) Request(ids.arena, -1, 0, false);
      for (int w : wrestler_ids)
        if (ImGui::Selectable((assets::CharacterLabel(w) + "  (" + std::to_string(w) + ")##w" + std::to_string(w)).c_str(), w == ids.wrestler) && w != ids.wrestler) Request(ids.arena, w, 0, false);  // (LOOK ENTRANCE: his entrance starts, Rebuild)
      if (!custom_list.empty()) {
        ImGui::SeparatorText("CUSTOM");
        for (const auto& c : custom_list)
          if (ImGui::Selectable((c.second + "##c" + std::to_string(c.first)).c_str(), c.first == ids.wrestler) && c.first != ids.wrestler) Request(ids.arena, c.first, 0, false);
      }
      TouchScroll();  // (phone: a finger drags the long list; a tap still picks)
      ImGui::EndCombo();
    }
    ImGui::PopItemWidth();
    ImGui::BeginDisabled(ids.wrestler < 0);
    ImGui::TextUnformatted("ATTIRE");
    ImGui::SameLine();
    if (ImGui::Button(" - ") && ids.attire > 0) Request(ids.arena, ids.wrestler, ids.attire - 1, false);
    ImGui::SameLine();
    ImGui::Text("%d", ids.attire);
    ImGui::SameLine();
    if (ImGui::Button(" + ")) Request(ids.arena, ids.wrestler, ids.attire + 1, false);
    ImGui::EndDisabled();
    ImGui::TextUnformatted("LOOK");
    ImGui::SameLine();
    if (ImGui::RadioButton("MATCH", !want_entrance) && want_entrance) want_entrance = false, Request(ids.arena, ids.wrestler, ids.attire, true);
    ImGui::SameLine();
    if (ImGui::RadioButton("ENTRANCE", want_entrance) && !want_entrance) want_entrance = true, Request(ids.arena, ids.wrestler, ids.attire, true);
    ImGui::TextUnformatted("LIGHT");
    ImGui::SameLine();
    const bool lit = be->LookLit();
    if (ImGui::RadioButton("LIT", lit) && !lit) exec("{\"cmd\":\"look\",\"mode\":\"lit\"}");
    ImGui::SameLine();
    if (ImGui::RadioButton("PLAIN", !lit) && lit) exec("{\"cmd\":\"look\",\"mode\":\"plain\"}");
    if (!msg.empty()) {
      ImGui::PushTextWrapPos(0);
      ImGui::TextColored(ImVec4(1, 0.55f, 0.4f, 1), "%s", msg.c_str());
      ImGui::PopTextWrapPos();
    }
    ImGui::End();
  }

  // Session 98 (plan step 11): the FREECAM panel (collapsible, in the sidebar). Edits the camera and the controller directly (same thread).
  void FreecamPanel(float d) {
    ImGui::SetCursorPosX(16 * d);
    SectionHead("FREECAM");
    ImGui::SetCursorPosX(16 * d);
    if (!fcam || !fctl) {
      ImGui::TextDisabled("(not available)");
      return;
    }
    if (ImGui::TreeNodeEx("Camera controls", ImGuiTreeNodeFlags_DefaultOpen)) {
      Camera& c = *fcam;
      FreeCamCtl& f = *fctl;
      if (ImGui::Button(c.free ? "BACK TO ORBIT  (F)" : "FREE CAMERA  (F)")) ToggleFree(c, f);
      if (c.free) {
        ImGui::TextDisabled("pos %.0f  %.0f  %.0f", c.fpos[0], c.fpos[1], c.fpos[2]);
        ImGui::TextDisabled("yaw %.1f  pitch %.1f  roll %.1f", c.fyaw, c.fpitch, c.froll);
      } else {
        ImGui::TextDisabled("Orbit: drag = look round, wheel = zoom");
      }
      ImGui::PushItemWidth(-1);
      ImGui::TextUnformatted("SPEED");
      ImGui::SliderFloat("##fc_speed", &f.speed, 25.0f, 3000.0f, "%.0f / s", ImGuiSliderFlags_Logarithmic);
      ImGui::TextUnformatted("LOOK SENSITIVITY");
      ImGui::SliderFloat("##fc_sens", &f.sensitivity, 0.02f, 0.5f, "%.2f deg / px", ImGuiSliderFlags_Logarithmic);
      ImGui::TextUnformatted("SMOOTHING");
      ImGui::SliderFloat("##fc_smooth", &f.smoothing, 0.0f, 1.0f, "%.2f");
      ImGui::TextUnformatted("FOV");
      ImGui::SliderFloat("##fc_fov", &c.fov, kFreeFovMin, kFreeFovMax, "%.1f deg");
      ImGui::BeginDisabled(!c.free);
      ImGui::TextUnformatted("ROLL");
      ImGui::SliderFloat("##fc_roll", &c.froll, -180.0f, 180.0f, "%.1f deg");
      ImGui::EndDisabled();
      ImGui::PopItemWidth();
      if (ImGui::Button("FOV 40")) c.fov = 40.0f;
      ImGui::SameLine();
      ImGui::BeginDisabled(!c.free);
      if (ImGui::Button("ROLL 0")) c.froll = 0.0f, f.roll_rate = 0;
      ImGui::EndDisabled();
      ImGui::SameLine();
      if (ImGui::Button("TUNING")) f.speed = kFreeSpeedDefault, f.sensitivity = kFreeSensDefault, f.smoothing = kFreeSmoothDefault;
      ImGui::PushTextWrapPos(0);
      ImGui::TextDisabled("Free: hold right mouse = look, WASD move, E / Q up / down, Shift x4, Ctrl slow, wheel = FOV, Z / X roll, R roll 0. Pad: sticks, triggers up / down, bumpers zoom.");
      ImGui::PopTextWrapPos();
      ImGui::TreePop();
    }
  }

  // The ENTRANCE tab of the sidebar: the options (lights, intro pose, intro hold). The transport stays in the bottom timeline bar.
  // Every click is an entrance_time command (queued: the main loop runs it outside the frame).
  void EntranceOptions(float d) {
    const EntranceView& ev = evp ? *evp : ev0;
    ImGui::SetCursorPosX(16 * d);
    SectionHead("ENTRANCE OPTIONS");
    if (!ev.active || ev.names.empty()) {
      // Session 98 (first phone test): the way in, by tap - LOOK to ENTRANCE with a wrestler starts his entrance (Rebuild); a
      // reload that ended it (a light / arena change) can start it again here.
      ImGui::PushTextWrapPos(0);
      const Ids ids = Scan();
      if (!want_entrance) {
        ImGui::TextDisabled("(no entrance playing)");
        if (ImGui::Button("LOOK: ENTRANCE")) want_entrance = true, Request(ids.arena, ids.wrestler, ids.attire, true);
        ImGui::TextDisabled("Plays the chosen wrestler's entrance in the chosen arena (the LOOK switch in the scene bar does the same).");
      } else if (ids.arena < 0 || ids.wrestler < 0) {
        ImGui::TextDisabled(ids.arena < 0 ? "(pick an arena and a wrestler in the scene bar)" : "(pick a wrestler in the scene bar)");
      } else {
        ImGui::TextDisabled("(his entrance is not playing)");
        if (ImGui::Button("PLAY HIS ENTRANCE")) Request(ids.arena, ids.wrestler, ids.attire, false);
      }
      ImGui::PopTextWrapPos();
      return;
    }
    auto send = [this](std::string j) { queued.push_back([this, j] { exec(j); }); };
    bool lights = ev.lights, c0 = ev.show_cue0;
    if (ImGui::Checkbox("ENTRANCE LIGHTS", &lights)) ent_opts.lights = lights, send(std::string("{\"cmd\":\"entrance_time\",\"lights\":") + (lights ? "true" : "false") + "}");
    if (ImGui::Checkbox("SHOW INTRO POSE", &c0)) ent_opts.cue0_show = c0, send(std::string("{\"cmd\":\"entrance_time\",\"cue0\":\"") + (c0 ? "show" : "hide") + "\"}");
    if (!ent_hold_edit) ent_hold = (float)ev.intro_hold;
    ImGui::PushItemWidth(-1);
    ImGui::SliderFloat("##hold", &ent_hold, 0.0f, 20.0f, "INTRO HOLD %.1f s");
    ent_hold_edit = ImGui::IsItemActive();
    if (ImGui::IsItemDeactivatedAfterEdit()) ent_opts.intro_hold = ent_hold, send("{\"cmd\":\"entrance_time\",\"intro_hold\":" + std::to_string(ent_hold) + "}");
    ImGui::PopItemWidth();
    if (ImGui::Button("RESTART")) {
      const Ids ids = Scan();
      if (ids.wrestler >= 0) Request(ids.arena, ids.wrestler, ids.attire, false);
    }
    ImGui::SameLine();
    if (ImGui::Button("STOP (LOOK: MATCH)")) {
      const Ids ids = Scan();
      want_entrance = false, Request(ids.arena, ids.wrestler, ids.attire, true);
    }
    // Session 98 (gen check 5): CHAMPION: the title belt (none / in the hand / on the waist / auto) and the champion entrance bank.
    // Each change re-runs the entrance at the current time (entrance_title).
    ImGui::SetCursorPosX(16 * d);
    SectionHead("CHAMPION");
    {
      static const char* const kTitles[4] = {"none", "hold", "waist", "auto"};
      static const char* const kLabels[4] = {"NO TITLE", "BELT IN HAND", "BELT ON WAIST", "AUTO"};
      int cur = 0;
      for (int i = 0; i < 4; ++i) if (ev.title == kTitles[i]) cur = i;
      ImGui::PushItemWidth(-1);
      if (ImGui::Combo("##title", &cur, kLabels, 4)) ent_opts.title = kTitles[cur], send(std::string("{\"cmd\":\"entrance_title\",\"title\":\"") + kTitles[cur] + "\"}");
      int belt = ev.belt;
      if (ImGui::InputInt("##belt", &belt, 1, 10) && belt != ev.belt && belt >= 0)
        ent_opts.belt = belt, send("{\"cmd\":\"entrance_title\",\"belt\":" + std::to_string(belt) + "}");
      ImGui::PopItemWidth();
      bool champ = ev.bank >= 600 && ev.bank < 700;
      if (ImGui::Checkbox("CHAMPION ENTRANCE", &champ)) ent_opts.champ_bank = champ, send(std::string("{\"cmd\":\"entrance_title\",\"champ_bank\":") + (champ ? "true" : "false") + "}");
      ImGui::PushTextWrapPos(0);
      ImGui::TextDisabled("Belt = evtobj model: 349-450 held belts (352 WWE Championship), 451-502 waist belts (459 Del Rio's). Entrance bank %d.", ev.bank);
      ImGui::PopTextWrapPos();
    }
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Transport: the timeline bar below. Space, Left / Right, , . and - = keys.");
    ImGui::PopTextWrapPos();
  }

  // The timeline bar (ui_timeline.cpp): drawn first in the RUNTIME view so the panel above can sit on it.
  void TimelineBar(ImVec2 disp) {
    const EntranceView& ev = evp ? *evp : ev0;
    if (!ev.active || ev.names.empty()) return;
    TimelineCtx cx;
    cx.d = dpi, cx.disp = disp, cx.right_margin = sidebar_open ? 300 * dpi : 30 * dpi, cx.pending = !queued.empty();
    cx.text = [this](ImDrawList* dl, ImVec2 p, float px, ImU32 col, const char* t) { Text(dl, p, px, col, t); };
    cx.size = [this](float px, const char* t) { return TextSize(px, t); };
    cx.send = [this](const std::string& j) { queued.push_back([this, j] { exec(j); }); };
    DrawTimeline(ev, tls, cx);
  }

  // Session 98 phone port (stage C): touch has no wheel and ImGui has no touch scrolling. A mostly vertical finger drag inside the
  // current window scrolls it and cancels the press it began on (the phone launcher's rule, mobile_main.cpp TouchScroll). Touch only:
  // the mouse is unchanged.
  // (Session 98 phone fixes: several windows call this in one frame - the ASSETS grid and its preview - so the drag belongs to the
  // window it started in, by id; before, one drag scrolled every window that called this after it.)
  ImGuiID touch_drag = 0;
  void TouchScroll() {
    ImGuiIO& io = ImGui::GetIO();
    if (io.MouseSource != ImGuiMouseSource_TouchScreen || !ImGui::IsMouseDown(0)) { touch_drag = 0; return; }
    const ImGuiID me = ImGui::GetCurrentWindow()->ID;
    if (!touch_drag && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
      const ImVec2 dd = ImGui::GetMouseDragDelta(0, 0);
      if (std::fabs(dd.y) > 10 * dpi && std::fabs(dd.y) > std::fabs(dd.x) * 1.2f) touch_drag = me;
    }
    if (touch_drag == me) {
      ImGui::SetScrollY(ImGui::GetScrollY() - io.MouseDelta.y);
      ImGui::ClearActiveID();
    }
  }

  void Sidebar(float top, ImVec2 disp) {
    const float d = dpi, w = sidebar_open ? 300 * d : 30 * d;
    ImGui::SetNextWindowPos(ImVec2(disp.x - w, top));
    ImGui::SetNextWindowSize(ImVec2(w, disp.y - top));
    ImGui::SetNextWindowBgAlpha(0.86f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sidebar_open ? 16 * d : 4 * d, 10 * d));
    ImGui::Begin("##sidebar", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetWindowPos();
    dl->AddRectFilled(p, ImVec2(p.x + 4 * d, p.y + disp.y - top), kMenuRed);  // the red left edge
    ImGui::SetCursorPosX(sidebar_open ? ImGui::GetWindowWidth() - 36 * d : 6 * d);
    if (ImGui::Button(sidebar_open ? ">>" : "<<", ImVec2(26 * d, 0))) sidebar_open = !sidebar_open;
    if (sidebar_open) {
      // auto-switch: a new clicked part -> INSPECT; an entrance just started -> ENTRANCE; otherwise the last chosen tab stays
      const EntranceView& ev = evp ? *evp : ev0;
      const bool ent_active = ev.active && !ev.names.empty();
      if (ent_active && !ent_was_active) stab = 4;
      ent_was_active = ent_active;
      if (ui::LightingPanelOpen()) stab = 3, ui::LightingPanelOpen() = false;  // the light_panel command
      static const char* const names[5] = {"INSPECT", "MODS", "CAMERA", "LIGHT", "ENTRANCE"};
      const float ty = p.y + 10 * d + ImGui::GetFrameHeight() + 6 * d, th = 28 * d;
      Tabs(names, 5, stab, ImVec2(p.x + 10 * d, ty), 13 * d, th, 5, 5, 0);
      const float body_y = ty - p.y + th + 6 * d;
      dl->AddLine(ImVec2(p.x + 8 * d, ty + th + 2 * d), ImVec2(p.x + w - 8 * d, ty + th + 2 * d), IM_COL32(255, 255, 255, 50));
      ImGui::SetCursorPos(ImVec2(0, body_y));
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16 * d, 4 * d));
      ImGui::BeginChild("##sbody", ImVec2(w, disp.y - top - body_y), ImGuiChildFlags_AlwaysUseWindowPadding, stab == 3 ? ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse : 0);
      ImGui::PopStyleVar();
      switch (stab) {
        case 0: InspectPanel(d, w); break;  // Session 98 (step 16): the clicked part
        case 1: ImGui::SetCursorPosX(16 * d); mods.Draw(d, queued); break;  // step 14: ui_mods.cpp
        case 2:
          FreecamPanel(d);
          ImGui::Dummy(ImVec2(0, 8 * d));
          ImGui::SetCursorPosX(16 * d);
          SectionHead("CAMERA PATH");
          PathPanel(d, exec, pathui);  // ui_path.cpp
          break;
        case 3: LightingPanel(exec, queued, d, Scan().arena, want_entrance); break;  // ui_light.cpp
        default: EntranceOptions(d); break;
      }
      TouchScroll();  // Session 98 phone port: a finger drags the panel up / down
      ImGui::EndChild();
    }
    ImGui::End();
  }

  void TopBar(ImVec2 disp, const Ids& ids, float head) {
    const float d = dpi;
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(disp.x, head));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##top", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float sk = 26 * d;
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(disp.x, head), IM_COL32(176, 20, 28, 255), IM_COL32(150, 14, 22, 255), IM_COL32(96, 8, 13, 255), IM_COL32(110, 10, 15, 255));
    dl->AddLine(ImVec2(0, head - 1), ImVec2(disp.x, head - 1), IM_COL32(255, 255, 255, 90), 1.5f * d);
    (void)sk;
    const float px = 28 * d;
    const char* title = "ARENA VIEWER";
    Text(dl, ImVec2(22 * d, (head - px) / 2), px, IM_COL32_WHITE, title);
    const float tx = 22 * d + TextSize(px, title).x + 36 * d;
    const int before = tab;
    Tabs(kTabNames, 2, tab, ImVec2(tx, (head - 34 * d) / 2), 21 * d, 34 * d);
    if (tab != before) videos_dirty = true;
    // Session 98 (Jarrod: "the arena, look, and camera text should look more distinct from what they're set to"): each
    // item is a small dim caption with its value under it in white, laid out from the right edge.
    const std::string items[3][2] = {
        {"ARENA", ids.arena >= 0 ? Upper(assets::ArenaLabel(ids.arena)) : std::string("NONE")},
        {"LOOK", want_entrance ? "ENTRANCE" : "MATCH"},
        {"CAMERA", camera_mode}};
    const float cap_px = 11 * d, val_px = 19 * d, gap = 30 * d;
    const float block_h = cap_px + 3 * d + val_px, y0 = (head - block_h) / 2;
    float right = disp.x - 22 * d;
    for (int i = 2; i >= 0; --i) {
      const ImVec2 cs = TextSize(cap_px, items[i][0].c_str()), vs = TextSize(val_px, items[i][1].c_str());
      const float w = std::max(cs.x, vs.x), x = right - w;
      Text(dl, ImVec2(x, y0), cap_px, IM_COL32(255, 190, 192, 170), items[i][0].c_str());
      Text(dl, ImVec2(x, y0 + cap_px + 3 * d), val_px, IM_COL32_WHITE, items[i][1].c_str());
      if (i > 0) dl->AddLine(ImVec2(x - gap / 2, y0 + 2 * d), ImVec2(x - gap / 2, y0 + block_h), IM_COL32(255, 255, 255, 60), 1.f * d);
      right = x - gap;
    }
    ImGui::End();
  }

  // ---------------------------------------------------------------- frame
  void Draw() {
    if (!ready) return;
    if (evict_png) {  // (here, before this frame's draw list exists: nothing points at the cached textures any more)
      for (auto& kv : png_cache) be->FreeTexture(kv.second.id);
      png_cache.clear();
      evict_png = false;
    }
    if (const float nd = be->Dpi(); std::fabs(nd - dpi) > 0.01f) ApplyStyle(nd);
    be->ListModels(models);
    be->NewFrame();
    if (test_ptr.active) {  // (after the backend's own mouse position: this one is processed last and wins)
      ImGuiIO& io = ImGui::GetIO();
      io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
      io.AddMousePosEvent(test_ptr.x, test_ptr.y);
      io.AddMouseButtonEvent(0, test_ptr.down);
    }
    ImGui::NewFrame();
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    const float head = 54 * dpi;
    const Ids ids = Scan();
    if (importer.Active()) importer.Draw(ImpCtx());  // Session 98: the SUPERSTAR IMPORTER window (ui_importer.cpp)
    else {
      if (tab == 1) AssetsTab(ImVec2(0, head), ImVec2(disp.x, disp.y - head));
      else {
        SceneBar(head);
        Sidebar(head, disp);
        TimelineBar(disp);
      }
      TopBar(disp, ids, head);
    }
    ImGui::Render();
    be->RenderDrawData();
    if (shot.queued && !shot.done && ++shot.frames >= 4) {
      std::vector<uint8_t> px;
      int w = 0, h = 0;
      if (!be->GrabBackBuffer(px, w, h, shot.err) || !be->WritePng(shot.path, px.data(), w, h, shot.err)) {
        if (shot.err.empty()) shot.err = "ui_shot failed";
      }
      shot.done = true;
    }
  }
};

Ui::Ui() = default;
Ui::~Ui() = default;

bool Ui::Init(std::unique_ptr<Backend> be, const std::string& game, Exec exec, std::string& err) {
  p_ = std::make_unique<Impl>();
  if (!p_->Init(std::move(be), game, std::move(exec), err)) {
    p_.reset();
    return false;
  }
  return true;
}
bool Ui::Ready() const { return p_ && p_->ready; }
bool Ui::Message(void* hwnd, unsigned msg, uintptr_t wp, intptr_t lp, bool dragging, intptr_t& out) {
  return p_ && p_->be->Message(hwnd, msg, wp, lp, dragging, out);
}
bool Ui::Pump() {
  if (!p_) return false;
  const auto t0 = std::chrono::steady_clock::now();
  for (auto& a : std::exchange(p_->queued, {})) a();
  const bool long_wait = std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(500);
  if (p_->importer.Active()) p_->importer.Pump(p_->ImpCtx());  // Session 98: bulk steps / the build's end (not a reason to skip a draw)
  else p_->mods.Poll();  // Session 98: the MODS panel's ~1 s look at the mods folder
  return long_wait;
}
void Ui::SetImporter(ImporterHost* host) {
  if (p_) p_->importer.SetHost(host);
}
std::string Ui::ImporterCommand(const std::string& line) {
  if (!p_ || !p_->importer.Active()) return "{\"ok\":false,\"error\":\"not in importer mode (--importer)\"}";
  jl::Value v;
  std::string err;
  if (!jl::Parse(line, v, err)) return "{\"ok\":false,\"error\":\"bad json\"}";
  return p_->importer.Command(v, p_->ImpCtx());
}
void Ui::Draw() {
  if (p_) p_->Draw();
}
void Ui::ModsSelect(int tron) {
  if (p_) p_->mods.SetSelected(tron);
}
void Ui::SetCameraMode(const char* name) {
  if (p_) p_->camera_mode = name;
}
void Ui::SetEntrance(const EntranceView* v) {
  if (p_) p_->evp = v;
}
void Ui::SetFreeCam(Camera* cam, FreeCamCtl* ctl) {
  if (p_) p_->fcam = cam, p_->fctl = ctl;
}
void Ui::SetPathUi(PathUi* p) {
  if (p_) p_->pathui = p;
}
void Ui::SetInspect(const PartInfo& info) {
  if (!p_) return;
  if (info.valid) {  // a newly clicked part switches the sidebar to INSPECT
    char k[160];
    std::snprintf(k, sizeof k, "%s|%.2f|%.2f|%.2f", info.literal.c_str(), info.pos[0], info.pos[1], info.pos[2]);
    if (p_->inspect_key != k) p_->inspect_key = k, p_->stab = 0;
  } else p_->inspect_key.clear();
  p_->inspect = info;
}
bool Ui::TimelineKey(unsigned vk, bool shift) {
  if (!p_ || !p_->ready || p_->tab != 0 || WantTextInput()) return false;
  const EntranceView& ev = p_->evp ? *p_->evp : p_->ev0;
  return view::ui::TimelineKey(vk, shift, ev, p_->tls, [this](const std::string& j) { p_->queued.push_back([this, j] { p_->exec(j); }); });
}
bool Ui::MouseOverUi() const { return p_ && p_->ready && ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse; }
bool Ui::HitTest(float x, float y) const {
  if (!p_ || !p_->ready || !ImGui::GetCurrentContext()) return false;
  const ImVec2 pt(x, y);
  for (ImGuiWindow* w : GImGui->Windows)  // (root windows drawn last frame; children sit inside their parents)
    if (w->WasActive && !w->Hidden && !w->ParentWindow && !(w->Flags & ImGuiWindowFlags_NoMouseInputs) &&
        std::strncmp(w->Name, "Debug##", 7) != 0 && w->OuterRectClipped.Contains(pt))
      return true;
  return false;
}
bool Ui::WantTextInput() const { return p_ && p_->ready && ImGui::GetCurrentContext() && ImGui::GetIO().WantTextInput; }
void Ui::QueueShot(const std::string& path, const std::string& tab, int category, int select) {
  if (!p_) return;
  p_->shot = {};
  p_->shot.queued = true, p_->shot.path = path;
  if (tab == "assets") p_->tab = 1, p_->videos_dirty = true;
  else if (tab == "runtime") p_->tab = 0;
  else if (tab == "inspect" || tab == "mods" || tab == "camera" || tab == "light" || tab == "entrance") {  // a sidebar tab (test hook)
    p_->tab = 0;
    p_->stab = tab == "inspect" ? 0 : tab == "mods" ? 1 : tab == "camera" ? 2 : tab == "light" ? 3 : 4;
  }
  if (category >= 0 && category < 3) p_->cat = category, p_->videos_dirty = true;
  if (select >= 0) p_->sel[p_->cat] = select;
}
void Ui::TestPointer(bool active, float x, float y, bool down) {
  if (!p_) return;
  p_->test_ptr.active = active, p_->test_ptr.x = x, p_->test_ptr.y = y, p_->test_ptr.down = active && down;
  if (!active && p_->ready && ImGui::GetCurrentContext()) ImGui::GetIO().AddMouseButtonEvent(0, false);
}
std::string Ui::TestState() const {
  if (!p_ || !p_->ready) return "{\"ready\":false}";
  const Impl& m = *p_;
  const Impl::Ids ids = m.Scan();
  const EntranceView& ev = m.evp ? *m.evp : m.ev0;
  auto esc = [](const std::string& s) {
    std::string o;
    for (char c : s) o += c == '"' || c == '\\' ? std::string("\\") + c : std::string(1, c);
    return o;
  };
  char b[160];
  std::snprintf(b, sizeof b, ",\"t\":%.1f,\"total\":%.1f,\"playing\":%s,\"cue\":%d,\"camera\":\"%s\"", ev.t, ev.total, ev.playing ? "true" : "false", ev.cue,
                ev.cam_entrance ? "entrance" : "free");
  static const char* const kSide[5] = {"inspect", "mods", "camera", "light", "entrance"};
  const ImGuiIO& io = ImGui::GetIO();
  char pb[200];
  std::snprintf(pb, sizeof pb, ",\"pointer\":{\"x\":%.0f,\"y\":%.0f,\"down\":%s,\"touch\":%s,\"popup\":%s,\"active_id\":%u}", io.MousePos.x, io.MousePos.y,
                io.MouseDown[0] ? "true" : "false", io.MouseSource == ImGuiMouseSource_TouchScreen ? "true" : "false",
                ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) ? "true" : "false", (unsigned)GImGui->ActiveId);
  char ab[200];  // (Session 98 phone fixes: the ASSETS panels' finger scroll)
  std::snprintf(ab, sizeof ab, ",\"assets\":{\"cat\":%d,\"sel\":%d,\"grid_scroll\":%.0f,\"preview_scroll\":%.0f,\"preview_scroll_max\":%.0f,\"video_playing\":%s}",
                m.cat, m.sel[std::clamp(m.cat, 0, 2)], m.grid_scroll, m.preview_scroll, m.preview_scroll_max, m.vid_play ? "true" : "false");
  return std::string("{\"ready\":true,\"tab\":\"") + (m.tab ? "assets" : "runtime") + "\",\"sidebar\":\"" + kSide[std::clamp(m.stab, 0, 4)] +
         "\",\"look\":\"" + (m.want_entrance ? "entrance" : "match") + "\",\"arena\":" + std::to_string(ids.arena) + ",\"wrestler\":" +
         std::to_string(ids.wrestler) + ",\"attire\":" + std::to_string(ids.attire) + ",\"msg\":\"" + esc(m.msg) + "\",\"queued\":" +
         std::to_string(m.queued.size()) + ",\"entrance\":{\"active\":" + (ev.active && !ev.names.empty() ? "true" : "false") + b + ",\"title\":\"" +
         esc(ev.title) + "\",\"bank\":" + std::to_string(ev.bank) + "}" + pb + ab + "}";
}
bool Ui::ShotDone() const { return !p_ || p_->shot.done; }
const std::string& Ui::ShotError() const {
  static const std::string none;
  return p_ ? p_->shot.err : none;
}

}  // namespace ui
}  // namespace view
