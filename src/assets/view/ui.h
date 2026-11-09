// Session 98 (plan step 9): the Arena Viewer's UI. A launcher-styled Dear ImGui layer drawn over the finished scene
// (Renderer::SetOverlay). Windowed viewer only: --hidden never creates it, and shots (Renderer::Shot) never contain it.
//   top bar    ARENA VIEWER, two slanted tabs RUNTIME / ASSETS, a status line (arena, look, camera)
//   RUNTIME    the scene (full window, under a semi-transparent sidebar) + a scene bar (arena / wrestler / attire / look pickers)
//              + a right sidebar: INSPECT and MODS panels (placeholders, filled by later builders)
//   ASSETS     categories (wrestler textures, arena textures, videos), a thumbnail grid, a preview pane
// The UI changes the scene only through the viewer's own JSON commands (the Exec callback), never by touching the loaders.
// This file and ui.cpp hold no Win32 / D3D: the platform is a ui::Backend (ui_backend.h).
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "pick.h"
#include "ui_backend.h"

namespace view {
struct Camera;
struct FreeCamCtl;
struct PathUi;
namespace ui {
struct ImporterHost;  // ui_importer.h

// Session 98 (entrance): what the ENTRANCE panel shows (the viewer fills it every frame); its buttons send entrance_time commands.
struct EntranceView {
  bool active = false, playing = false, cam_entrance = false;
  std::vector<std::string> names;
  std::vector<double> starts, lens;  // motion frames (60 per second)
  double t = 0, total = 0, speed = 1;
  int cue = 0;
  // Session 98 (timeline bar, ui_timeline.cpp): per cue, what happens in it (assets::entrance::Segments), the cue number, a tooltip
  // (zones and root positions the label came from); the options the ENTRANCE panel keeps.
  std::vector<std::string> labels, tips;
  std::vector<int> cue_ids;
  double intro_hold = 8.5;
  bool lights = true, show_cue0 = false;
  // Session 98 (gen check 5): the champion options (entrance_title): "none" | "hold" | "waist" | "auto", the belt (evtobj PACH), the
  // entrance bank played and whether that bank is a champion bank (Entrance06 600-699)
  std::string title = "none";
  int belt = 0, bank = 0;
};

class Ui {
 public:
  using Exec = std::function<std::string(const std::string&)>;  // Viewer::Safe: one JSON command in, the reply out
  Ui();
  ~Ui();
  // `game` = the --game path (folder or disc image): the menu font is read from it, like the launcher does.
  bool Init(std::unique_ptr<Backend> be, const std::string& game, Exec exec, std::string& err);
  bool Ready() const;
  // Window messages first: true = the UI used it (out = the result); see Backend::Message.
  bool Message(void* hwnd, unsigned msg, uintptr_t wp, intptr_t lp, bool camera_dragging, intptr_t& out);
  // The overlay body (Renderer::SetOverlay): runs queued actions, builds and draws the UI, takes a queued shot.
  // Runs the actions queued by clicks (scene loads). The main loop calls this BEFORE Renderer::DrawFrame, never from the overlay.
  // Returns true when an action blocked for a while (the phone's file picker covered the app): the caller pumps the platform's
  // events (pause / resume, the surface) before it draws again.
  bool Pump();
  void Draw();
  void SetCameraMode(const char* name);  // "ORBIT" / "FREE" / "GAME", for the status line
  void SetFreeCam(Camera* cam, FreeCamCtl* ctl);  // Session 98 (freecam): the FREECAM panel edits these (same thread as the UI)
  void SetPathUi(PathUi* p);                     // Session 98 (camera path): the CAMERA PATH panel's state (ui_path.cpp)
  bool WantTextInput() const;                   // a text field has the keyboard: the free camera ignores WASD
  void ModsSelect(int tron);  // Session 98: the MODS panel's selected tron (the mods_select command)
  void SetEntrance(const EntranceView* v);  // the pointer must stay valid (the viewer keeps it)
  // Session 98 (timeline bar): a key press (Win32 VK code) while an entrance is active and no text field has the keyboard. True = used.
  // Space play / pause, Left / Right one frame back / forward (Shift = 10), , and . previous / next segment, - and = slower / faster.
  bool TimelineKey(unsigned vk, bool shift);
  // Session 98 (step 16, click to inspect): the selected part the INSPECT panel shows (valid = false: the hint), and whether the mouse is
  // over the UI (then the viewer neither hovers nor picks the scene).
  void SetInspect(const PartInfo& info);
  bool MouseOverUi() const;
  // Session 98 phone port (stage C, touch): is this point (UI coordinates = ImGui's) on one of the UI's windows as last drawn? A touch that
  // starts there belongs to the UI; elsewhere it drives the camera (platform_sdl.cpp's gestures).
  bool HitTest(float x, float y) const;
  // ui_shot: capture the back buffer WITH the UI after a few settled frames. tab "runtime" | "assets" ("" keeps), category / select
  // pick the Assets category and entry (-1 keeps).
  void QueueShot(const std::string& path, const std::string& tab, int category, int select);
  bool ShotDone() const;
  const std::string& ShotError() const;  // empty = written
  // Session 98 (phone fixes, test hooks for the ui_touch / ui_state commands): a test finger at (x, y) in UI coordinates, given to ImGui
  // as a touch-screen pointer every frame AFTER the platform backend's own mouse (the real cursor cannot move it); active = false
  // gives the pointer back. TestState: what the UI shows (LOOK, scene ids, the scene bar's message, the entrance timeline, the tabs).
  void TestPointer(bool active, float x, float y, bool down);
  std::string TestState() const;
  // Session 98 (Superstar Importer, ui_importer.h): the importer window replaces the RUNTIME / ASSETS layout (the viewer's --importer).
  // `host` must stay valid. ImporterCommand: the {"cmd":"importer",...} test hooks (outside the frame).
  void SetImporter(ImporterHost* host);
  std::string ImporterCommand(const std::string& json_line);

 private:
  struct Impl;
  std::unique_ptr<Impl> p_;
};

}  // namespace ui
}  // namespace view
