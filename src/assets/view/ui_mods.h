// Session 98 (plan step 14, the mod workbench): the MODS panel of the Arena Viewer's sidebar. Pure Dear ImGui + std (portable);
// the platform pieces (video frame decode, file picker, Recycle Bin) come from assets::mods_lookup::Plat(), filled on Windows by
// mods_win32.cpp (RegisterModsPlatform). The lookup rules are in src\assets\mods_lookup.h (the same ones the game uses).
#pragma once
#include <chrono>
#include <functional>
#include <string>
#include <vector>

namespace view {
namespace ui {

void RegisterModsPlatform();  // mods_win32.cpp: decode / pick / recycle / open folder / cache folder

class ModsPanel {
 public:
  using Exec = std::function<std::string(const std::string&)>;  // the viewer's JSON commands
  using Queue = std::vector<std::function<void()>>;             // actions run by Ui::Pump, outside the frame
  void SetExec(Exec e) { exec_ = std::move(e); }
  // Inside the sidebar window, below the "MODS" head. `d` = the dpi scale. Uses the rest of the window's height.
  void Draw(float d, Queue& q);
  // From Ui::Pump (outside the frame): the ~1 s poll of the mods folder; runs commands directly.
  void Poll();
  int Selected() const { return sel_; }
  void SetSelected(int id) { sel_ = id; shown_sig_ = SelSig(id); }

 private:
  struct Row {
    int id = 0;
    bool yours = false, game = false, bink = false;
    std::string file;  // "343.mp4"
    unsigned long long bytes = 0;
  };
  void Refresh();
  std::string Cmd(const std::string& json) { return exec_ ? exec_(json) : std::string(); }
  std::string SelSig(int id) const;
  void Select(int id, Queue* q);
  void SetNote(const std::string& reply, const std::string& ok_text);
  Exec exec_;
  bool scanned_ = false;
  std::vector<Row> rows_;
  std::vector<std::pair<int, std::string>> banners_;  // tron id, "video" / "none" / "file: x.mp4" / "original"
  std::string dir_sig_, shown_sig_, note_;
  int sel_ = -1, seen_gen_ = 0;
  bool confirm_remove_ = false, confirm_remove_char_ = false;
  std::string sel_char_;
  std::chrono::steady_clock::time_point last_poll_{};
};

}  // namespace ui
}  // namespace view
