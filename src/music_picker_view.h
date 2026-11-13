// wwe2k14 - USER PLAYLIST music picker: the boundary between the picker's LOGIC (lane 291: src\music_picker_core.h, the
// hooks, preview, choices.txt) and its VIEW (lane 292: drawing, launcher-style visuals, layout at phone and PC sizes,
// touch / controller / keyboard mapping). No game, SDK, ImGui or Windows includes: both sides and the host tests build
// this header alone.
//
// Flow: the logic owns a PickerModel and calls view->Draw(model, frame) once per UI frame while the picker is open, and
// view->Poll() once per UI frame to collect what the player did since the last call. The logic turns the returned
// inputs into model changes (cursor, preview, select, cancel); the view never changes the model and never touches the
// game, the files or the player. The view draws only what the model says, so a removed file, an empty library or a
// "now playing" row are model fields, not view decisions.
//
// Preview (Jarrod, 2026-11-13: a cursor hover must NOT play): each row has a PLAY / STOP button drawn over the song's
// thumbnail on the left (album art, or a generic record vector when art_id == 0). Moving the cursor never previews.
// The logic plays / stops on kTogglePlay and keeps a preview playing while the cursor moves on; choosing a song or
// cancelling stops it.
//
// Input mapping is the view's job (it knows the device): controller D-pad / stick up-down -> kUp / kDown, A -> kSelect,
// B -> kCancel, X -> kTogglePlay (cursor row), bumpers -> kPageUp / kPageDown; keyboard arrows / Enter / Esc / PageUp /
// PageDown the same, P or Space -> kTogglePlay; touch: a tap on a row's play button -> kTogglePlay with `row`, a tap on
// the rest of a row -> kTapRow with `row` (= choose it), a tap on the back control -> kCancel, a drag -> kScroll with
// `scroll_rows` (positive = later rows). Album art is handed over separately: PickerView::SetArt.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wwe2k14::music_picker {

struct Song {
  std::string name;                   // display name (file name without the extension)
  std::string artist;                 // from the tag; may be empty
  uint32_t art_id = 0;                // 0 = no album art: the view draws the generic record vector; else the id of a SetArt call
};

// What the logic tells the view to show.
struct PickerModel {
  std::string title;                  // e.g. "MUSIC LIBRARY"
  std::string subtitle;               // e.g. "Choose the song for <wrestler>" (may be empty)
  std::vector<Song> songs;            // in list order
  int cursor = 0;                     // highlighted row (0 when there are no songs)
  int selected = -1;                  // the row that is the saved choice for this wrestler (-1 = none / the default)
  int playing = -1;                   // the row whose preview is playing now (-1 = none): its button shows STOP
  std::string message;                // a line shown instead of / under the list: the empty-library text, "That song is gone", ...
  bool empty = false;                 // no songs: the view shows `message` (and only a back control)
  uint32_t revision = 0;              // bumped by the logic on every change (the view may skip redrawing when unchanged)
};

// Per-frame facts the view needs to lay itself out (filled by the logic's host).
struct FrameInfo {
  float width = 1280.0f;              // overlay surface size in pixels
  float height = 720.0f;
  float dt = 1.0f / 60.0f;            // seconds since the last Draw
  bool touch = false;                 // the last input came from touch (phones: bigger rows)
  bool controller = false;            // the last input came from a controller (show the A / B / X hints)
};

enum class InputKind : uint8_t {
  kUp,          // cursor one row up
  kDown,        // cursor one row down
  kPageUp,      // cursor a page up
  kPageDown,    // cursor a page down
  kSelect,      // choose the highlighted row (save, stop any preview, close)
  kCancel,      // leave without changing the choice (stops any preview)
  kTapRow,      // a tap / click on the body of `row`: moves the cursor there AND chooses it (as kSelect)
  kTogglePlay,  // start the preview of `row` (the cursor row when row < 0); stop it when that row is already playing
  kScroll,      // the list was dragged: `scroll_rows` rows (+ = later); the cursor follows the view
};

struct PickerInput {
  InputKind kind = InputKind::kUp;
  int row = -1;
  int scroll_rows = 0;
};

// The controller as the game's input read saw it (XInput layout), handed to the view on the GAME thread while the picker is
// open; the game itself gets an idle pad meanwhile (the picker owns the input). Buttons use the XInput bits (A 0x1000, B 0x2000,
// X 0x4000, Y 0x8000, D-pad 0x0001 up / 2 down / 4 left / 8 right, LB 0x0100, RB 0x0200, Start 0x0010, Back 0x0020).
struct PadState {
  uint16_t buttons = 0;
  int16_t lx = 0, ly = 0, rx = 0, ry = 0;  // sticks, +ly = up
  uint8_t lt = 0, rt = 0;
};

class PickerView {
 public:
  virtual ~PickerView() = default;
  // GAME thread, every input read of player 1 while the picker is open (several times a frame). Only record it (a
  // mutex-guarded queue / last state); map it to PickerInput in Poll(). The first state after OnOpen is the baseline: buttons
  // already held then (the A that opened the picker) produce no press until they are released and pressed again.
  virtual void OnPadState(const PadState&) {}
  // Once per UI frame while the picker is open. Draws `model`; must not keep a pointer to it past the call.
  virtual void Draw(const PickerModel& model, const FrameInfo& frame) = 0;
  // Once per UI frame while the picker is open: the inputs since the last call, in order (may be empty).
  virtual std::vector<PickerInput> Poll() = 0;
  // Album art of a song, once per art_id the logic extracted (the file's embedded cover): the ENCODED image (JPEG or PNG
  // bytes as stored in the tag); the view decodes it into the thumbnail and may drop it at OnClose. Default: ignored.
  virtual void SetArt(uint32_t art_id, const std::vector<uint8_t>& encoded) { (void)art_id, (void)encoded; }
  // The same cover already decoded to a square thumbnail (straight alpha RGBA, w x h) by a worker thread: the UI thread only uploads it.
  virtual void SetArtRgba(uint32_t art_id, int w, int h, const std::vector<uint8_t>& rgba) { (void)art_id, (void)w, (void)h, (void)rgba; }
  // The picker opened / closed (the view may reset scrolling, animations, held-key state). Default: nothing.
  virtual void OnOpen() {}
  virtual void OnClose() {}
};

}  // namespace wwe2k14::music_picker
