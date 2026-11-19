// Session 103 (KBM): bare Shift / Control / Alt binds. Host clang, 0 game runs.
//   pwsh -NoProfile -File tools\input\run_kbm_bind_test.ps1
// Runs (1) the SDK's bind_match.h (extracted from patches\rexglue-sdk\full\118-*.diff by the run script) on a key-state x
// bind-string table, with TokenPressedOld (the pre-patch matcher, copied verbatim from mnk_input_driver.cpp) as the
// PAST-BUG re-find: "Shift" never fires and W dies while Shift is held; (2) a sweep proving the default layout (and any
// layout with no bare modifier) is bit-for-bit the old behaviour; (3) the launcher's keyboard_binds.h: NameFromKeyDown,
// Display, the capture state machine (AltGr, focus loss), ChordUses / ChordWarning, save / load of "Shift,Q" and "Control".
#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>

#include <rex/input/mnk/bind_match.h>

#include "../../src/launcher/keyboard_binds.h"

using namespace rex::input::mnk;
using rex::ui::VirtualKey;
namespace fs = std::filesystem;

// ---- the stand-in for rex::ui::ParseVirtualKey (src\ui\keybinds.cpp kKeyNames): the names the tests use
namespace rex::ui {
VirtualKey ParseVirtualKey(std::string_view n) {
  struct E { const char* name; VirtualKey vk; };
  static const E kNames[] = {
      {"Shift", VirtualKey::kShift}, {"Control", VirtualKey::kControl}, {"Alt", VirtualKey::kMenu},
      {"Up", VirtualKey::kUp},       {"Down", VirtualKey::kDown},       {"Left", VirtualKey::kLeft},
      {"Right", VirtualKey::kRight}, {"Space", VirtualKey::kSpace},     {"Tab", VirtualKey::kTab},
      {"Return", VirtualKey::kReturn}, {"Backspace", VirtualKey::kBack}, {"F4", VirtualKey::kF4},
  };
  for (const E& e : kNames)
    if (n == e.name) return e.vk;
  if (n.size() == 1 && ((n[0] >= 'A' && n[0] <= 'Z') || (n[0] >= '0' && n[0] <= '9'))) return VirtualKey(n[0]);
  if (n == "Semicolon") return VirtualKey(0xBA);
  if (n == "Quote") return VirtualKey(0xDE);
  return VirtualKey::kNone;
}
}  // namespace rex::ui

static int g_fail = 0, g_checks = 0;
#define CHECK(cond, ...)                                  \
  do {                                                    \
    ++g_checks;                                           \
    if (!(cond)) {                                        \
      ++g_fail;                                           \
      std::printf("FAIL line %d: ", __LINE__);            \
      std::printf(__VA_ARGS__);                           \
      std::printf("\n");                                  \
    }                                                     \
  } while (0)

// ---- the pre-patch matcher, verbatim (mnk_input_driver.cpp before patch 118; names kept for the reader)
static bool TokenPressedOld(const bool (&key_down)[256], std::string_view token, uint8_t live_mods) {
  uint8_t want = TakeModifiers(token);
  if (want != live_mods) {
    return false;
  }
  rex::ui::VirtualKey vk = rex::ui::ParseVirtualKey(token);
  if (vk == rex::ui::VirtualKey::kNone) {
    return false;
  }
  uint16_t idx = static_cast<uint16_t>(vk);
  return idx < 256 && key_down[idx];
}
static bool IsBindPressedOld(const bool (&key_down)[256], std::string_view cvar_val) {
  const uint8_t live_mods = LiveModifiers(key_down);
  std::string_view rest(cvar_val);
  while (!rest.empty()) {
    size_t comma = rest.find(',');
    std::string_view token = rest.substr(0, comma);
    if (comma == std::string_view::npos) {
      rest = std::string_view();
    } else {
      rest.remove_prefix(comma + 1);
    }
    token = TrimSpaces(token);
    if (!token.empty() && TokenPressedOld(key_down, token, live_mods)) return true;
  }
  return false;
}

struct Down {
  bool k[256] = {};
  Down(std::initializer_list<const char*> names) {
    for (const char* n : names) k[static_cast<uint16_t>(rex::ui::ParseVirtualKey(n))] = true;
  }
};
// bare mask of a whole layout (every control's value), as the driver computes it
static uint8_t BareOf(const std::vector<std::string>& layout) {
  uint8_t bare = 0;
  for (const std::string& v : layout) bare |= BareModifierMask({std::string_view(v)});
  return bare;
}
static bool Fires(const std::vector<std::string>& layout, const std::string& bind, const Down& d) {
  return IsBindPressed(d.k, BareOf(layout), bind);
}

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("section 1\n");
  // ---- 1. the table: the default layout (bare_mods 0), old == new
  const std::vector<std::string> kDefault = [] {
    std::vector<std::string> v;
    for (const auto& b : launcher::keys::kBinds) v.push_back(b.def);
    return v;
  }();
  CHECK(BareOf(kDefault) == 0, "the default layout has no bare modifier");
  CHECK(Fires(kDefault, "Up", Down{"Up"}), "Up fires on Up");
  CHECK(!Fires(kDefault, "Up", Down{"Shift", "Up"}), "Up is silent with Shift held (the D-pad shares the arrows)");
  CHECK(Fires(kDefault, "Shift+Up", Down{"Shift", "Up"}), "Shift+Up fires on Shift+Up");
  CHECK(!Fires(kDefault, "Shift+Up", Down{"Up"}), "Shift+Up is silent without Shift");
  CHECK(Fires(kDefault, "W", Down{"W"}) && !Fires(kDefault, "W", Down{"Shift", "W"}), "W today: dies with Shift held (old rule)");

  // ---- 2. LT = "Shift": fires on Shift alone; the rest keep working
  const std::vector<std::string> kLtShift = {"Shift", "W", "Shift+Up", "Up"};
  CHECK(BareOf(kLtShift) == kModShift, "bare mask = Shift");
  CHECK(Fires(kLtShift, "Shift", Down{"Shift"}), "bare Shift fires on Shift");
  CHECK(!Fires(kLtShift, "Shift", Down{}), "bare Shift is silent with nothing down");
  CHECK(Fires(kLtShift, "W", Down{"Shift", "W"}), "W still fires while the bound Shift is held");
  CHECK(Fires(kLtShift, "Shift+Up", Down{"Shift", "Up"}), "Shift+Up fires on Shift+Up");
  CHECK(!Fires(kLtShift, "Shift+Up", Down{"Up"}), "Shift+Up still needs Shift held");
  CHECK(Fires(kLtShift, "Up", Down{"Shift", "Up"}), "Up also fires on Shift+Up (documented overlap)");
  CHECK(Fires(kLtShift, "Up", Down{"Up"}), "Up fires on Up");
  CHECK(Fires(kLtShift, "Ctrl+Q", Down{"Shift", "Control", "Q"}), "a bound Shift does not spoil Ctrl+Q");
  CHECK(Fires(kLtShift, "Ctrl+Q", Down{"Control", "Q"}), "Ctrl+Q fires with Ctrl only");
  CHECK(!Fires(kLtShift, "Ctrl+Q", Down{"Control", "Alt", "Q"}), "an unbound Alt still spoils Ctrl+Q");

  // ---- 3. Control and Alt the same
  for (const char* mod : {"Control", "Alt"}) {
    const std::string m = mod, chord = (m == "Control" ? "Ctrl+" : "Alt+") + std::string("Up");
    const std::vector<std::string> lay = {m, "W", chord};
    CHECK(Fires(lay, m, Down{mod}), "bare %s fires", mod);
    CHECK(Fires(lay, "W", Down{mod, "W"}), "W fires while %s held", mod);
    CHECK(Fires(lay, chord, Down{mod, "Up"}), "%s fires on its chord", chord.c_str());
    CHECK(!Fires(lay, chord, Down{"Up"}), "%s needs the modifier", chord.c_str());
  }
  {  // "Ctrl+Q" with Alt bare-bound fires on Ctrl+Alt+Q; without Alt bound it is silent there (exact rule kept)
    const std::vector<std::string> alt_bound = {"Alt", "Ctrl+Q"};
    const std::vector<std::string> alt_unbound = {"Ctrl+Q"};
    CHECK(Fires(alt_bound, "Ctrl+Q", Down{"Control", "Alt", "Q"}), "Ctrl+Q fires on Ctrl+Alt+Q when Alt is a button");
    CHECK(!Fires(alt_unbound, "Ctrl+Q", Down{"Control", "Alt", "Q"}), "Ctrl+Q silent on Ctrl+Alt+Q when Alt is not bound");
    CHECK(Fires(alt_bound, "Ctrl+Q", Down{"Control", "Q"}), "Ctrl+Q still fires on Ctrl+Q");
  }
  {  // bare in any alternative / with spaces / in another control's list
    CHECK(BareModifierMask({"Q, Shift", "E"}) == kModShift, "a bare token after a comma and a space counts");
    CHECK(BareModifierMask({"Shift+Q", "Ctrl+Alt+E"}) == 0, "prefixes are not bare tokens");
    CHECK(BareModifierMask({"Shift", "Control", "Alt"}) == (kModShift | kModCtrl | kModAlt), "all three");
    CHECK(BareModifierMask({"", "Nonsense"}) == 0, "empty / unknown names are ignored");
  }
  {  // the tap mark: a quick tap of Shift between polls counts (the driver ORs tapped keys into the state it matches)
    Down d{"Shift"};
    CHECK(Fires(kLtShift, "Shift", d), "a tapped Shift counts like a held one");
  }

  // ---- 4. PAST BUG re-find: the old matcher reproduces the report (fails on old, passes on new)
  {
    const Down shift{"Shift"}, shift_w{"Shift", "W"};
    CHECK(!IsBindPressedOld(shift.k, "Shift"), "OLD: bare Shift never fires (the reported bug, reproduced)");
    CHECK(!IsBindPressedOld(shift_w.k, "W"), "OLD: W dies while Shift is held (the reported bug, reproduced)");
    CHECK(!IsBindPressedOld(Down{"Control"}.k, "Control") && !IsBindPressedOld(Down{"Alt"}.k, "Alt"),
          "OLD: bare Control / Alt never fire");
    // the same two cases on the new code with the layout that binds Shift
    CHECK(Fires(kLtShift, "Shift", shift), "NEW: bare Shift fires");
    CHECK(Fires(kLtShift, "W", shift_w), "NEW: W lives while Shift is held");
  }

  // ---- 5. no bare modifier bound: every layout behaves bit for bit as before (sweep: all modifier subsets x many keys)
  {
    const std::vector<std::vector<std::string>> layouts = {
        kDefault,
        {"Ctrl+Q", "Alt+W", "Shift+Ctrl+E", "Q,I", "Up", "Shift+Space", "Control+Tab", "F4", "Return,Backspace"},
    };
    const char* keys_to_try[] = {"Up", "Down", "Left", "Right", "W", "A", "S", "D", "Q", "E", "I", "Space", "Tab",
                                 "Return", "F4", "Semicolon", "Quote", "1", "3", "Backspace"};
    long compared = 0, mismatched = 0;
    for (const auto& layout : layouts) {
      CHECK(BareOf(layout) == 0, "layout has no bare modifier");
      for (int m = 0; m < 8; ++m)
        for (const char* key : keys_to_try)
          for (const char* key2 : {"", "W", "Up"}) {
            Down d{};
            if (m & 1) d.k[0x10] = true;
            if (m & 2) d.k[0x11] = true;
            if (m & 4) d.k[0x12] = true;
            d.k[static_cast<uint16_t>(rex::ui::ParseVirtualKey(key))] = true;
            if (*key2) d.k[static_cast<uint16_t>(rex::ui::ParseVirtualKey(key2))] = true;
            for (const std::string& bind : layout) {
              ++compared;
              if (IsBindPressed(d.k, 0, bind) != IsBindPressedOld(d.k, bind)) ++mismatched;
            }
          }
    }
    CHECK(mismatched == 0, "new == old on %ld comparisons with no bare modifier bound (%ld differ)", compared, mismatched);
    std::printf("sweep: %ld comparisons, %ld differ\n", compared, mismatched);
  }

  // ---- 6. launcher: key names, Display
  namespace K = launcher::keys;
  CHECK(K::NameFromKeyDown(VK_SHIFT, LPARAM(0x2A) << 16) == "Shift", "NameFromKeyDown LShift");
  CHECK(K::NameFromKeyDown(VK_SHIFT, LPARAM(0x36) << 16) == "Shift", "NameFromKeyDown RShift");
  CHECK(K::NameFromKeyDown(VK_CONTROL, LPARAM(0x1D) << 16) == "Control", "NameFromKeyDown LCtrl");
  CHECK(K::NameFromKeyDown(VK_CONTROL, (LPARAM(0x1D) << 16) | (LPARAM(1) << 24)) == "Control", "NameFromKeyDown RCtrl (ext)");
  CHECK(K::NameFromKeyDown(VK_MENU, LPARAM(0x38) << 16) == "Alt", "NameFromKeyDown LAlt");
  CHECK(K::NameFromKeyDown(VK_MENU, (LPARAM(0x38) << 16) | (LPARAM(1) << 24)) == "Alt", "NameFromKeyDown RAlt (ext)");
  CHECK(K::NameFromKeyDown(VK_LWIN, LPARAM(0x5B) << 16 | (LPARAM(1) << 24)).empty(), "the Windows key stays unbindable");
  CHECK(K::NameFromKeyDown(VK_RWIN, LPARAM(0x5C) << 16 | (LPARAM(1) << 24)).empty(), "the right Windows key stays unbindable");
  CHECK(K::NameFromKeyDown('Q', LPARAM(0x10) << 16) == "Q", "NameFromKeyDown Q unchanged");
  CHECK(K::NameFromKeyDown(VK_UP, (LPARAM(0x48) << 16) | (LPARAM(1) << 24)) == "Up", "NameFromKeyDown Up unchanged");
  CHECK(K::Display("Shift") == "SHIFT" && K::Display("Control") == "CTRL" && K::Display("Alt") == "ALT", "Display words");
  CHECK(K::Display("Shift+Up") == "SHIFT + UP", "Display chord");
  CHECK(K::WithModifiers("Shift", false, false, false) == "Shift", "WithModifiers bare");

  // ---- 7. the capture state machine
  auto run = [](std::initializer_list<std::pair<bool, WPARAM>> events, bool shift_held_at_key = false) {
    K::CaptureState c;
    std::string name;
    for (const auto& [down, vk] : events) {
      if (down) K::CaptureKeyDown(c, vk, 0, shift_held_at_key, false, false);
      else K::CaptureKeyUp(c, vk);
    }
    if (!c.got) return std::string("<nothing>");
    return K::WithModifiers(c.vk == VK_SHIFT ? "Shift" : c.vk == VK_CONTROL ? "Control" : c.vk == VK_MENU ? "Alt"
                                : c.vk == VK_UP ? "Up" : c.vk == VK_F4 ? "F4" : "?", c.shift, c.ctrl, c.alt);
  };
  CHECK(run({{true, VK_SHIFT}, {false, VK_SHIFT}}) == "Shift", "Shift tapped alone -> Shift");
  CHECK(run({{true, VK_CONTROL}, {false, VK_CONTROL}}) == "Control", "Ctrl tapped alone -> Control");
  CHECK(run({{true, VK_MENU}, {false, VK_MENU}}) == "Alt", "Alt tapped alone -> Alt");
  CHECK(run({{true, VK_SHIFT}, {true, VK_UP}}, true) == "Shift+Up", "Shift then Up -> Shift+Up");
  CHECK(run({{true, VK_SHIFT}, {true, VK_UP}, {false, VK_UP}, {false, VK_SHIFT}}, true) == "Shift+Up", "...and the ups change nothing");
  CHECK(run({{true, VK_SHIFT}}) == "<nothing>", "Shift held, nothing yet: keeps waiting");
  CHECK(run({{true, VK_LWIN}, {false, VK_LWIN}}) == "<nothing>", "Windows key alone captures nothing");
  CHECK(run({{true, VK_SHIFT}, {true, VK_CONTROL}, {false, VK_CONTROL}}) == "<nothing>", "a second modifier let go is not the first one");
  CHECK(run({{true, VK_SHIFT}, {true, VK_CONTROL}, {false, VK_CONTROL}, {false, VK_SHIFT}}) == "Shift", "first modifier let go last -> Shift");
  CHECK(run({{true, VK_UP}, {true, VK_SHIFT}, {false, VK_SHIFT}}) == "Up", "Up then Shift: Up was captured, the later Shift does not replace it");
  CHECK(run({{false, VK_SHIFT}}) == "<nothing>", "a stray key-up of a modifier (the one that opened the window) captures nothing");
  CHECK(run({{true, VK_LSHIFT}, {false, VK_SHIFT}}) == "Shift", "left / right Shift codes count as Shift");

  // ---- 8. ChordUsers: the warning when a bare modifier meets the default D-pad
  {
    K::BindFile f;  // defaults
    const auto users = K::ChordUses(f, "Shift", 4);
    CHECK(users.size() == 4 && users[0].bind == 18 && users[3].bind == 21,
          "Shift: the four D-pad controls use it as a prefix (got %zu)", users.size());
    // the arrows are also plain binds of the right stick (13..16): the warning names them
    CHECK(users.size() == 4 && users[0].plain_binds == std::vector<int>({13}) && users[1].plain_binds == std::vector<int>({14}) &&
              users[2].plain_binds == std::vector<int>({15}) && users[3].plain_binds == std::vector<int>({16}),
          "Shift+Up's key Up is also plain on RIGHT STICK UP (and Down / Left / Right likewise)");
    CHECK(K::ChordUses(f, "Control", 4).empty() && K::ChordUses(f, "Alt", 4).empty(), "Control / Alt: no default user");
    CHECK(K::ChordUses(f, "Q", 4).empty(), "a non-modifier token has no chord users");
    K::SetBind(f, 18, "Ctrl+Up");
    CHECK(K::ChordUses(f, "Control", 4).size() == 1 && K::ChordUses(f, "Control", 4)[0].bind == 18, "Ctrl+ prefix found for Control");
    K::SetBind(f, 18, "Control+Up");
    CHECK(K::ChordUses(f, "Control", 4).size() == 1, "Control+ prefix found too");
    K::SetBind(f, 18, "Shift+Ctrl+Q");  // every prefix counts, not only the first
    CHECK(K::ChordUses(f, "Control", 4).size() == 1 && K::ChordUses(f, "Shift", 4).size() == 4 && K::ChordUses(f, "Alt", 4).empty(),
          "Shift+Ctrl+Q is a chord user of Control and of Shift, not of Alt");
    K::SetBind(f, 18, "Alt+Shift+Q");
    CHECK(K::ChordUses(f, "Alt", 4).size() == 1, "Alt found behind another prefix");
    K::BindFile d;
    const std::string w = K::ChordWarning(d, "Shift", 4, [](int i) { return std::string(K::kBinds[i].label) + "#" + std::to_string(i); });
    CHECK(w.find("SHIFT + UP (UP#18)") != std::string::npos && w.find("UP (UP#13)") != std::string::npos, "warning text names the chord and the plain bind: %s", w.c_str());
    CHECK(K::ChordWarning(d, "Control", 4, [](int) { return std::string("x"); }).empty(), "no warning without a user");
  }

  // ---- 8b. AltGr, focus loss
  {
    // AltGr: a fake non-extended Control, then the extended Alt, same message time (SDL's SkipAltGrLeftControl rule)
    const LPARAM ext = LPARAM(1) << 24;
    K::CaptureState c;
    K::CaptureKeyDown(c, VK_CONTROL, 0, false, true, false, 1000);
    K::CaptureKeyDown(c, VK_MENU, ext, false, true, true, 1000);
    K::CaptureKeyUp(c, VK_CONTROL);
    K::CaptureKeyUp(c, VK_MENU);
    CHECK(c.got && c.vk == VK_MENU && !c.ctrl, "AltGr tapped alone -> Alt (the fake Control is dropped)");
    K::CaptureState d;
    K::CaptureKeyDown(d, VK_CONTROL, 0, false, true, false, 2000);
    K::CaptureKeyDown(d, VK_MENU, ext, false, true, true, 2000);
    K::CaptureKeyDown(d, VK_UP, 0, false, true, true, 2001);
    CHECK(d.got && d.vk == VK_UP && K::WithModifiers("Up", d.shift, d.ctrl, d.alt) == "Alt+Up", "AltGr+Up -> Alt+Up, not Ctrl+Alt+Up");
    K::CaptureState e;  // a real Ctrl then a real Alt at different times: both stay
    K::CaptureKeyDown(e, VK_CONTROL, 0, false, true, false, 3000);
    K::CaptureKeyDown(e, VK_MENU, 0, false, true, true, 3200);
    K::CaptureKeyDown(e, VK_UP, 0, false, true, true, 3300);
    CHECK(K::WithModifiers("Up", e.shift, e.ctrl, e.alt) == "Ctrl+Alt+Up", "Ctrl then Alt (not extended, later) -> Ctrl+Alt+Up");
    K::CaptureState f;  // extended Alt at another time than the Control: not AltGr
    K::CaptureKeyDown(f, VK_CONTROL, 0, false, true, false, 4000);
    K::CaptureKeyDown(f, VK_MENU, ext, false, true, true, 4100);
    K::CaptureKeyUp(f, VK_CONTROL);
    CHECK(f.got && f.vk == VK_CONTROL, "a Control that is not followed by an Alt at the same time stays a Control");
    // focus lost with a modifier waiting (Alt+Tab): nothing binds, the chip keeps waiting
    K::CaptureState g;
    K::CaptureKeyDown(g, VK_MENU, 0, false, false, true, 5000);
    K::CaptureFocusLost(g);
    K::CaptureKeyUp(g, VK_MENU);
    CHECK(!g.got && !g.pending, "Alt+Tab: the Alt does not bind, nothing is stuck");
    K::CaptureKeyDown(g, VK_SHIFT, 0, true, false, false, 6000);
    K::CaptureKeyUp(g, VK_SHIFT);
    CHECK(g.got && g.vk == VK_SHIFT, "after the focus came back a Shift tap still captures");
    K::CaptureState h;  // focus lost after a capture does not undo it
    K::CaptureKeyDown(h, VK_UP, 0, false, false, false, 7000);
    K::CaptureFocusLost(h);
    CHECK(h.got && h.vk == VK_UP, "focus loss does not undo a finished capture");
  }

  // ---- 9. save / load round trip
  {
    const fs::path dir = fs::temp_directory_path() / "kbm_bind_test";
    fs::create_directories(dir);
    const fs::path file = dir / "keybinds.toml";
    K::BindFile f;
    K::SetBind(f, 4, "Shift,Q");   // keybind_left_trigger
    K::SetBind(f, 2, "Control");   // keybind_x
    K::SetBind(f, 3, "Alt,Shift+P");
    CHECK(K::SaveBindFile(file, f), "save");
    const K::BindFile g = K::LoadBindFile(file);
    CHECK(g.exists && K::Value(g, 4) == "Shift,Q" && K::Value(g, 2) == "Control" && K::Value(g, 3) == "Alt,Shift+P",
          "round trip: '%s' '%s' '%s'", K::Value(g, 4).c_str(), K::Value(g, 2).c_str(), K::Value(g, 3).c_str());
    CHECK(g.values == f.values, "the whole map is equal");
    std::string text;
    {
      std::ifstream in(file, std::ios::binary);
      text.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    CHECK(text.find("keybind_left_trigger = \"Shift,Q\"") != std::string::npos &&
              text.find("keybind_x = \"Control\"") != std::string::npos,
          "file text has the lines the game's keybinds_save.h Load reads (name = \"value\")");
    std::error_code ec;
    fs::remove_all(dir, ec);
  }

  std::printf("%s: %d checks, %d failed\n", g_fail ? "FAIL" : "PASS", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
