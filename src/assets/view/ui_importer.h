// Session 98 (Superstar Importer): the SUPERSTAR IMPORTER window of the Arena Viewer (wwe2k14_assets.exe --importer; on the phone
// libviewer.so with the same flag). Pure Dear ImGui + std; drawn by ui.cpp in place of the RUNTIME / ASSETS layout when the viewer runs
// in importer mode. It reuses the viewer's renderer for the 3D preview (a hole in the UI where the scene shows, the camera framed into
// it by the viewer: viewer_main.cpp ImporterFrame) and for the roster portraits (ImporterHost::portrait). The pack itself is the
// core's (src\importer\importer_core.h: LoadPack / SavePack / BuildPack / FreeSlots / InspectSource(s) / ExtractSource).
//
//   header  SUPERSTAR IMPORTER, "MOD PACK: N OF UP TO 103 SLOTS USED"
//   left    the 3D preview (drag to turn, wheel / pinch to zoom), CHOOSE .ZIP OR .PAC, RING / ENTRANCE attire, the roster portrait
//           (RE-TAKE from the model, UPLOAD IMAGE with a crop step: SSFA 512 head-to-belt, SSFB 512 head-to-trunks, SSFC 256
//           head-to-hips), IN THE MOD PACK (slot, name, base; NEW / EDIT / REMOVE, IMPORT MANY)
//   right   NAME, SHORT NAME, BASED ON, ENTRANCE, RATINGS, SLOT; ADD TO PACK / SAVE CHANGES (or the crop step, or the bulk list)
//   footer  SUPERSTARS ON / OFF (the launcher's own setting, mirrored through --settings-file) and BUILD INTO GAME with progress
// Mods are additive (CLAUDE.md): everything is written under the mods folder by the core; game files are only read.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "imgui.h"

namespace jl {
struct Value;
}

namespace view {
namespace ui {

// The viewer's side, owned by viewer_main.cpp (same thread as the UI).
struct ImporterHost {
  std::string mods_dir;       // the player's real mods folder (the pack lives in <mods>\superstars)
  std::string dlc_dir;        // the DLC folder the launcher gave (BuildPack / FreeSlots)
  std::string game;           // --game (a folder or a disc image)
  std::string settings_file;  // the launcher's settings file: its "superstars" key is the ON / OFF switch ("" = not linked)
  int edit_slot = -1;         // --importer-edit SLOT: open that pack entry
  bool bulk_at_start = false; // --importer-bulk: open the bulk picker at once
  bool phone = false;
  std::string picked_dir;     // the phone's picker copies (<files>/importer/picked): cleared once a source is copied into the pack
  // The preview rectangle in window pixels (x0, y0, x1, y1), written by the UI every frame; the viewer frames the model into it.
  float view[4] = {0, 0, 0, 0};
  // A roster render of the previewed model: kind 0 SSFA 512 head-to-belt, 1 SSFB 512 head-to-trunks, 2 SSFC 256 head-to-hips.
  // Straight RGBA8 with a cut-out alpha. Outside the frame only (Pump / queued actions / commands).
  std::function<bool(int kind, std::vector<uint8_t>& rgba, int& w, int& h, std::string& err)> portrait;
};

// What ui.cpp lends the panel (its fonts, textures, touch scrolling, the command channel, the action queue).
struct ImporterCtx {
  float d = 1;
  std::function<void(ImDrawList*, ImVec2, float, ImU32, const char*)> text;      // the game's menu face with a shadow
  std::function<void(ImDrawList*, ImVec2, float, ImU32, const char*)> text_raw;  // without
  std::function<ImVec2(float, const char*)> size;
  std::function<ImTextureID(const uint8_t* rgba, int w, int h)> make_tex;
  std::function<void(ImTextureID)> free_tex;
  std::function<void()> touch_scroll;
  std::function<std::string(const std::string&)> exec;
  std::vector<std::function<void()>>* queued = nullptr;
};

class ImporterPanel {
 public:
  ImporterPanel();
  ~ImporterPanel();
  void SetHost(ImporterHost* h);
  bool Active() const;
  void Draw(const ImporterCtx& cx);  // the whole window (inside the ImGui frame)
  void Pump(const ImporterCtx& cx);  // outside the frame: the first load, bulk steps, the build's end, texture frees
  // Test hooks ({"cmd":"importer",...}, viewer_main.cpp): drive the window the way a click would. Outside the frame.
  std::string Command(const jl::Value& cmd, const ImporterCtx& cx);
  std::string State() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> p_;
};

// The launcher settings file's "superstars" key (a plain text edit that keeps every other key as written). Read: false when absent.
bool ReadSuperstarsSetting(const std::string& settings_file, bool& on);
bool WriteSuperstarsSetting(const std::string& settings_file, bool on, std::string& err);
// Any true / false key of that file (the Arena Viewer's LIGHT tab: "lighting" = USE MY LIGHTING). Same rules as above.
bool ReadBoolSetting(const std::string& settings_file, const std::string& key, bool& on);
bool WriteBoolSetting(const std::string& settings_file, const std::string& key, bool on, std::string& err);

}  // namespace ui
}  // namespace view
