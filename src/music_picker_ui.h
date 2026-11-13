// wwe2k14 - USER PLAYLIST music picker, VIEW side, public API (lane 292). The logic side (lane 291) includes THIS header only; no ImGui
// or SDK type appears in it. The boundary types are in music_picker_view.h (PickerModel, PickerInput, PickerView).
//
// How the logic uses it (UI thread unless said otherwise):
//   auto dialog = music_picker::OpenPickerDialog(drawer, host, [&](music_picker::PickerView& view, float dt) {
//     view.Draw(model, frame);                        // once per UI frame; this callback runs inside an ImGui frame
//     for (auto in : view.Poll()) { ... change the model ... }
//   });
//   dialog->Close();                                   // or let the callback return false to close
// A PickerDialog is an ImGuiDialog (the same layer as the performance overlay / the check banner): on the new renderer
// (--wwe2k14_rdr_owner) src\renderer_adapter\overlay_drawer.h records its draw lists and the renderer draws them over every picture;
// with the SDK presenter the SDK's own ImGui drawer does. Both carry textures, so thumbnails and the game's menu font work in both
// when the host's `upload` is set (MakeOwnerDrawerUploader below for the owner mode).
//
// Controller: the game reads the pad through XamInputGetState, not through ImGui. The host calls FeedPad() with the real pad's
// state from the pad filter (the way photo_mode.h's PadFilter gets it: the game's thread, user 1) and, while PickerIsOpen(), zeroes
// the pad it hands to the game so the menu underneath does not also react. Feed the pad BEFORE keyboard_player merges its keys into
// it: the view reads the keyboard from ImGui itself, so a key fed twice would fire twice.
//
// Android: TouchOverlay.java (the on-screen buttons) would swallow every touch; while PickerIsOpen() it hides and lets the touches
// through to the window (native method nativePickerOpen, polled by the Java side four times a second).
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "music_picker_layout.h"
#include "music_picker_view.h"

namespace rex::ui {
class ImGuiDrawer;
}

namespace wwe2k14::music_picker {

// A texture the overlay layer can draw: `id` is what ImGui's AddImage takes (an ImTextureID, 64-bit); `keep` owns the texture and is
// held for as long as the view uses it.
struct GpuImage {
  uint64_t id = 0;
  std::shared_ptr<void> keep;
};

struct ViewHost {
  // RGBA (straight alpha) -> a texture; null = no thumbnails (every row shows the record) and the fallback font. UI thread.
  std::function<GpuImage(int w, int h, const uint8_t* rgba)> upload;
  // The first 0x40000 bytes of pac\menu\fontHD.pac from the player's game files (game_files::Read("game:\\pac\\menu\\fontHD.pac"));
  // null / empty result = the fallback font. Called once.
  std::function<std::vector<uint8_t>()> read_font_pac;
  // The display's unusable edges in surface pixels; null = DefaultSafeArea (rounded corners / title safe). The game's Java side
  // does not report cutouts to native code, so the default is deliberately generous on phones.
  std::function<SafeArea(float width, float height)> safe_area;
};

std::unique_ptr<PickerView> MakeImGuiPickerView(ViewHost host);

// ---- controller feed / game-side state --------------------------------------------------------------------------------------
inline std::atomic<uint32_t> g_pad_buttons{0};  // XInput bits of the real pad (user 1), the latest state
inline std::atomic<uint32_t> g_pad_edges{0};    // buttons pressed since the view last drew (a press shorter than a frame still counts)
inline std::atomic<int> g_pad_stick_y{0};       // left thumb Y, XInput's range (up = positive)
inline std::atomic<bool> g_pad_primed{false};   // false after a picker opened until the first state arrives (that one is the baseline)
inline std::atomic<bool> g_open{false};         // a picker is on the screen (set by the dialog)
// The release latch (SF2): the buttons held when the picker closed. PadGate hides them from XamInputGetState until released; the keystroke
// gate (input_script.h) hides the same ones from XamInputGetKeystrokeEx (the game steps its menus on KEYDOWN / REPEAT keystrokes too).
inline std::atomic<uint16_t> g_latch_buttons{0};
// VK_PAD_* -> the XInput button bit it stands for (0 = not a pad button).
inline uint16_t PadBitOfVk(uint16_t vk) {
  switch (vk) {
    case 0x5800: return 0x1000;  // A
    case 0x5801: return 0x2000;  // B
    case 0x5802: return 0x4000;  // X
    case 0x5803: return 0x8000;  // Y
    case 0x5804: return 0x0200;  // right shoulder
    case 0x5805: return 0x0100;  // left shoulder
    case 0x5810: case 0x5820: case 0x5830: return 0x0001;  // d-pad up (and the left stick's up / right stick's up reuse the bit: held-back too)
    case 0x5811: case 0x5821: case 0x5831: return 0x0002;
    case 0x5812: case 0x5822: case 0x5832: return 0x0004;
    case 0x5813: case 0x5823: case 0x5833: return 0x0008;
    case 0x5814: return 0x0010;  // start
    case 0x5815: return 0x0020;  // back
    case 0x5816: return 0x0040;  // left thumb press
    case 0x5817: return 0x0080;
    default: return 0;
  }
}
// After a close: is this keystroke (flags: KEYDOWN 1, KEYUP 2, REPEAT 4) the closing press still going on? REPEATs of anything are dropped while
// a latch is set; a key whose pad bit is latched is dropped too.
inline bool LatchBlocksKeystroke(uint16_t flags, uint16_t vk) {
  const uint16_t latch = g_latch_buttons.load(std::memory_order_relaxed);
  if (!latch) return false;
  if (flags & 0x4) return true;
  return (PadBitOfVk(vk) & latch) != 0;
}
inline std::atomic<uint32_t> g_keystrokes_while_open{0};  // XamInputGetKeystrokeEx reads the game made while it was open (logged at close)

// GAME thread, every pad read while the picker is open (src\music_picker.h's pad gate calls it through PickerView::OnPadState).
inline void FeedPad(uint16_t buttons, int16_t left_stick_y) {
  static std::atomic<uint32_t> prev{0};
  if (!g_pad_primed.load(std::memory_order_acquire)) {
    // The first state after open: whatever is held (the A that opened the picker) is the baseline, no press. The view reads nothing
    // from the pad until this has happened (g_pad_primed, set last).
    prev.store(buttons, std::memory_order_relaxed);
    g_pad_buttons.store(buttons, std::memory_order_relaxed);
    g_pad_stick_y.store(left_stick_y, std::memory_order_relaxed);
    g_pad_primed.store(true, std::memory_order_release);
    return;
  }
  const uint32_t before = prev.exchange(buttons, std::memory_order_relaxed);
  g_pad_edges.fetch_or(buttons & ~before, std::memory_order_relaxed);
  g_pad_buttons.store(buttons, std::memory_order_relaxed);
  g_pad_stick_y.store(left_stick_y, std::memory_order_relaxed);
}
// UI thread, when a picker opens: forget the last pad state.
inline void ResetPadFeed() {
  g_pad_primed.store(false), g_pad_edges.store(0), g_pad_buttons.store(0), g_pad_stick_y.store(0);
}
inline bool PickerIsOpen() { return g_open.load(std::memory_order_relaxed); }

// ---- the dialog ---------------------------------------------------------------------------------------------------------------
class PickerDialog {
 public:
  virtual ~PickerDialog() = default;
  // Closes after the current frame (the dialog deletes itself; this handle is dead afterwards: take no further calls).
  virtual void Close() = 0;
};

// `per_frame(view, dt)` runs inside every ImGui frame while the dialog is open; return false to close it. The returned handle is valid
// until the dialog has closed (see OnClosed). `on_closed` runs after the dialog was deleted.
PickerDialog* OpenPickerDialog(rex::ui::ImGuiDrawer* drawer, ViewHost host, std::function<bool(PickerView&, float dt)> per_frame,
                               std::function<void()> on_closed = nullptr);
// The same with a view the caller owns (it made it, called OnOpen / SetArt on it, and ends it): the dialog only calls per_frame with it.
PickerDialog* OpenPickerDialogWithView(rex::ui::ImGuiDrawer* drawer, PickerView* view, std::function<bool(PickerView&, float dt)> per_frame,
                                       std::function<void()> on_closed = nullptr);

// The uploader for --wwe2k14_rdr_owner (src\renderer_adapter\overlay_drawer.h's Drawer); empty when no owner drawer is up (the SDK
// presenter's drawer does not expose texture creation to the app: thumbnails off there, the record and the fallback font are used).
std::function<GpuImage(int, int, const uint8_t*)> MakeOwnerDrawerUploader();

}  // namespace wwe2k14::music_picker
