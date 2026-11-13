// wwe2k14 - USER PLAYLIST music picker: the guest glue (lane 291, 2026-11-13). Included after user_playlist.h
// (kernel_overrides.h). State machine: src\music_picker_core.h (host test tools\music\music_picker_test.cpp); the boundary to the
// drawing: src\music_picker_view.h.
//
// What it is (Jarrod: "it's just exposing the music playlist 360 mechanic"): on the 360, choosing USER PLAYLIST in the MUSIC
// list of Create-an-Entrance shows a hint (text 50303: "create a Playlist containing only that song through the Xbox
// Dashboard"), and the player picks the song in the 360 Guide, a system overlay. We provide the Guide's part: the hint is
// replaced by OUR picker over the Mods > Music library (<mods>\music\library\, else new\); the choice is saved in
// <library>\choices.txt and the game's XMP "play user music" request (src\user_playlist.h) plays it.
//
// Where it hooks (listings read, not run: docs\systems\music.md "Music picker"):
//   sub_82268C10  the game's "post a notice" routine (r4 = text id, tail-called by sub_82268F58). Called with 50303 by the
//                 MUSIC row's select handler sub_83353F30 (when its music id, sub_8226d870([this+44]), is 9998) and by
//                 sub_8335F530. The notice does not block the menu; the rest of the handler (the row becomes the editor's
//                 music) runs as on the console. Our hook opens the picker INSTEAD of the notice; when the picker cannot open
//                 (no overlay, mods off, already open) the game's own notice shows, with our words (gfx_menu.h text hook).
//   input         while the picker is open the game's pad reads (XamInputGetState, player 1) get an idle pad and the real
//                 state goes to the view (g_picker_pad_filter, input_script.h): the picker owns the input and gives it back on
//                 close. Keyboard / mouse / touch reach the view through the overlay (Dear ImGui, in the game window; the new
//                 renderer draws it as it does the other overlays).
//   preview       a row's PLAY button (never the cursor): the user-playlist machine plays that ONE song (single mode), the same
//                 player the entrance uses. Lane 288's shared player replaces it behind PlayerIf.
// The wrestler KEY (lane 309): the choice is saved under the edited wrestler's character id (user_playlist::CurrentKey: the
// Create-an-Entrance screen's +84, read-only static evidence, to be proven by the `editor key:` trace line) and ALSO under "*" (the
// fallback every wrestler without his own line uses, and what an entrance plays while its wrestler cannot be named yet).
// docs\systems\music.md "Per-wrestler choices (lane 309)".
#pragma once

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>

#include <imgui.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "game_files.h"
#include "guest_read.h"
#include "input_script.h"  // g_picker_pad_filter
#include "music_picker_assets.h"  // DecodeCover (the worker decodes covers; the UI thread uploads)
#include "music_picker_core.h"
#include "music_picker_ui.h"  // lane 292: the launcher-style view, its dialog, texture uploader and pad feed
#include "user_playlist.h"

REXCVAR_DEFINE_BOOL(wwe2k14_music_picker, true, "wwe2k14",
                    "Lane 291: choosing USER PLAYLIST in the entrance MUSIC list opens a song picker over the mods folder's music "
                    "library (<mods>\\music\\library\\) instead of the console's Dashboard hint. Needs --wwe2k14_mods and "
                    "--wwe2k14_user_playlist. See src/music_picker.h");

REXCVAR_DEFINE_BOOL(wwe2k14_music_picker_trace, false, "wwe2k14",
                    "Lane 291 diagnostics: dump the MUSIC row's select handler, its record and the music manager's USER PLAYLIST track (the "
                    "wrestler KEY trace of docs/systems/music.md). Off by default: the hooks run for every player when the game asks.");

REX_EXTERN(__imp__sub_82268C10);
REX_EXTERN(__imp__sub_83353F30);
REX_EXTERN(__imp__sub_82ACDCE8);

namespace wwe2k14::music_picker {

constexpr uint32_t kHintTextId = 50303;  // the Dashboard hint (read from the player's string.pac; not copied here)

// Plays the picker's previews through the user-playlist machine (its list is the picker's list, set at open).
class MachinePreview : public PreviewIf {
 public:
  bool Play(const std::filesystem::path& file) override {
    auto& m = user_playlist::TheMachine();
    const bool ok = m.PlayPath(file, true) == 0;  // by path: a Rescan / Drop between two calls cannot shift it onto another song
    user_playlist::g_play_seq = seq.load();  // the sequence the picker is open in: the frame check keeps it playing there
    return ok;
  }
  void Stop() override { user_playlist::TheMachine().Stop(); }
  bool IsPlaying() override { return user_playlist::g_machine && user_playlist::g_machine->state() != user_playlist::State::Idle; }
  std::atomic<int32_t> seq{-1};
};

using ViewFactory = std::function<std::unique_ptr<PickerView>()>;
// Lane 292's launcher-style ImGui view (src\music_picker_imgui.h) is the view; Install() sets this. A test or a platform without ImGui can
// replace it.
inline ViewFactory g_view_factory;

// What the view needs from the game: textures from the new renderer's overlay drawer (thumbnails, the game's menu font; without it the
// rows show the record and the text uses ImGui's font), the menu face from the player's own game files, the display's safe area (none: the
// view's default). The owner drawer exists only after OnCreateDialogs, so it is looked up on every use.
inline ViewHost MakeViewHost() {
  ViewHost v;
  v.upload = [](int w, int h, const uint8_t* rgba) -> GpuImage {
    const auto up = MakeOwnerDrawerUploader();
    return up ? up(w, h, rgba) : GpuImage{};
  };
  v.read_font_pac = [] {
    std::vector<uint8_t> pac(0x40000);
    pac.resize(game_files::Read("game:\\pac\\menu\\fontHD.pac", std::span<uint8_t>(pac.data(), pac.size()), 0));
    return pac;
  };
  return v;
}

struct Host;
inline Host& H();

struct Host {
  OpenLifecycle life;  // open (the pad gate reads it), gen, pump_frames: see music_picker_core.h
  std::atomic<bool> force_close{false};
  std::atomic<bool> pad_seen{false};  // a controller drove the picker (the view shows A / B hints)
  // The release latch (review S1 / SF2): the buttons held when the picker closed (the A / B that closed it) stay hidden from the game until
  // they are let go (g_latch_buttons in music_picker_ui.h, also read by the keystroke gate), so the closing press does not also reach the
  // menu underneath (re-open loop, backing out of the editor).
  std::atomic<bool> stick_latch{false};
  std::atomic<uint16_t> last_buttons{0};  // the pad as the gate saw it last while open
  std::atomic<bool> last_stick{false};
  std::atomic<int64_t> opened_ms{0};
  std::atomic<int8_t> enabled_cache{-1};
  std::mutex view_m;              // guards `view` for the pad gate (game thread) against open / close (UI thread)
  std::unique_ptr<PickerView> view;
  // UI thread only:
  rex::ui::ImGuiDrawer* drawer = nullptr;
  std::function<void(std::function<void()>)> post_ui;
  PickerDialog* dialog = nullptr;  // lane 292's ImGui dialog (src\music_picker_ui.h); it deletes itself after Close()
  MachinePreview preview;
  std::unique_ptr<Picker> picker;
  struct ArtItem {
    uint32_t id;
    int w, h;
    std::vector<uint8_t> rgba;  // the cover decoded to a thumbnail on the worker
  };
  std::deque<ArtItem> art_queue;  // covers read by the worker, handed to the view one per frame (UI thread)
  std::string key;
  std::filesystem::path library;
  bool installed = false;
  int32_t open_seq = -1;
};
inline Host& H() {
  static Host* h = new Host();  // never destroyed: the pad gate may run at exit
  return *h;
}
inline int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---- the pad gate (game thread, every XamInputGetState of player 1) ----
inline bool StickDeflected(const rex::input::X_INPUT_STATE* s) {
  auto big = [](int16_t v) { return v > 12000 || v < -12000; };
  return big(int16_t(s->gamepad.thumb_lx)) || big(int16_t(s->gamepad.thumb_ly)) || big(int16_t(s->gamepad.thumb_rx)) ||
         big(int16_t(s->gamepad.thumb_ry));
}
inline void PadGate(rex::input::X_INPUT_STATE* s) {
  Host& h = H();
  if (!h.life.open.load(std::memory_order_acquire)) {
    // Just closed: what was held at the close stays hidden until it is released (S1), sticks included.
    uint16_t latch = g_latch_buttons.load(std::memory_order_relaxed);
    if (latch) {
      const uint16_t b = uint16_t(s->gamepad.buttons);
      latch = uint16_t(latch & b);
      g_latch_buttons.store(latch, std::memory_order_relaxed);
      s->gamepad.buttons = uint16_t(b & ~latch);
    }
    if (h.stick_latch.load(std::memory_order_relaxed)) {
      if (StickDeflected(s)) s->gamepad.thumb_lx = s->gamepad.thumb_ly = s->gamepad.thumb_rx = s->gamepad.thumb_ry = 0;
      else h.stick_latch.store(false, std::memory_order_relaxed);
    }
    return;
  }
  PadState p;
  p.buttons = uint16_t(s->gamepad.buttons);
  if (p.buttons) h.pad_seen.store(true, std::memory_order_relaxed);
  p.lx = int16_t(s->gamepad.thumb_lx), p.ly = int16_t(s->gamepad.thumb_ly);
  p.rx = int16_t(s->gamepad.thumb_rx), p.ry = int16_t(s->gamepad.thumb_ry);
  p.lt = uint8_t(s->gamepad.left_trigger), p.rt = uint8_t(s->gamepad.right_trigger);
  h.last_buttons.store(p.buttons, std::memory_order_relaxed);
  h.last_stick.store(StickDeflected(s), std::memory_order_relaxed);
  {
    std::lock_guard lock(h.view_m);
    if (h.view) h.view->OnPadState(p);
  }
  s->gamepad.buttons = 0;
  s->gamepad.left_trigger = 0, s->gamepad.right_trigger = 0;
  s->gamepad.thumb_lx = s->gamepad.thumb_ly = s->gamepad.thumb_rx = s->gamepad.thumb_ry = 0;
}

inline void Finish(const Result& r) {  // UI thread: the picker closed
  Host& h = H();
  const uint32_t keystrokes = g_keystrokes_while_open.exchange(0);
  if (r.outcome == Outcome::Chosen) {
    try {
      const std::string key = h.key.empty() ? std::string(user_playlist::kAnyKey) : h.key;
      const auto file = h.library / "choices.txt";
      const std::string name = user_playlist::U8(r.file.filename());  // UTF-8: path::string() throws on a name the ANSI page cannot hold
      const std::string out_text = user_playlist::UpsertPick(user_playlist::ReadChoices(), key, name);
      bool ok = false;
      if (out_text.size() <= (64u << 10)) {
        // Written beside, then renamed over: a crash or a full disk in the middle never leaves a half choices.txt.
        auto tmp = file;
        tmp += ".tmp";
        {
          std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
          out << out_text;
          out.flush();
          ok = bool(out);
        }
        std::error_code ec;
        if (ok) std::filesystem::rename(tmp, file, ec);
        if (ec || !ok) std::filesystem::remove(tmp, ec), ok = false;
      }
      REXLOG_INFO("[wwe2k14][music_picker] chose {} for key '{}' ({}); keystroke reads while open: {}", name, key, ok ? "saved" : "SAVE FAILED",
                  keystrokes);
      // The game's own USER PLAYLIST song is playing (it started it when the row was chosen): the chosen one replaces it.
      if (ok && user_playlist::g_machine && user_playlist::g_machine->state() != user_playlist::State::Idle)
        user_playlist::TheMachine().PlayPath(r.file, true);
    } catch (...) {
      REXLOG_WARN("[wwe2k14][music_picker] the choice could not be saved (an exception: file name or disk)");
    }
  } else {
    REXLOG_INFO("[wwe2k14][music_picker] closed, nothing changed; keystroke reads while open: {}", keystrokes);
  }
  // The buttons held now (the A / B that closed it) stay hidden from the game until released (S1).
  g_latch_buttons.store(h.last_buttons.load(std::memory_order_relaxed), std::memory_order_relaxed);
  h.stick_latch.store(h.last_stick.load(std::memory_order_relaxed), std::memory_order_relaxed);
  h.life.Release();  // open = false, and every posted piece of this open is void
  {
    std::lock_guard lock(h.view_m);
    if (h.view) h.view->OnClose();
    h.view.reset();
  }
  h.art_queue.clear();
  if (h.dialog) h.dialog->Close();  // runs inside the dialog's frame: the dialog deletes itself after it (on_closed clears `dialog`)
}

// Once per UI frame while the picker is open (the dialog's per_frame): ticks the logic, turns the view's inputs into model changes, draws.
// False = the picker finished: the dialog closes.
inline bool PumpFrame(PickerView& /*the dialog's view: stale after a re-open, see below*/, float dt) {
  Host& h = H();
  // The picker was released under the dialog (the 2-s release, Shutdown): close the dialog, drop the view, give nothing back to the game.
  // The view is read from the host, never from the dialog's argument: a re-open replaces `h.view` while an old dialog may still be alive.
  if (h.life.Stale()) {
    if (h.picker) h.picker->ForceClose();
    {
      std::lock_guard lock(h.view_m);
      h.view.reset();
    }
    h.art_queue.clear();
    return false;
  }
  if (!h.picker || !h.picker->is_open()) return true;
  PickerView* cur = nullptr;
  {
    std::lock_guard lock(h.view_m);
    cur = h.view.get();
  }
  if (!cur) return true;  // (the posted open has not set the view yet)
  PickerView& view = *cur;
  h.life.pump_frames.fetch_add(1);
  if (h.force_close.exchange(false)) {
    h.picker->ForceClose();
    Finish({Outcome::Cancelled, {}});
    return false;
  }
  h.picker->Tick();
  if (!h.art_queue.empty()) {  // one cover per frame: only the upload runs here
    Host::ArtItem a = std::move(h.art_queue.front());
    h.art_queue.pop_front();
    view.SetArtRgba(a.id, a.w, a.h, a.rgba);
  }
  for (const PickerInput& in : view.Poll()) {
    const Result r = h.picker->Handle(in);
    if (r.outcome != Outcome::None) {
      Finish(r);
      return false;
    }
  }
  const ImGuiIO& io = ImGui::GetIO();
  FrameInfo f;
  f.width = io.DisplaySize.x, f.height = io.DisplaySize.y, f.dt = dt;
  f.controller = h.pad_seen.load();
  view.Draw(h.picker->model(), f);
  return true;
}

inline bool Enabled();
inline void Install(rex::ui::ImGuiDrawer* drawer, std::function<void(std::function<void()>)> post_ui) {
  Host& h = H();
  h.drawer = drawer;
  h.post_ui = std::move(post_ui);
  h.installed = drawer != nullptr && h.post_ui != nullptr;
  h.enabled_cache.store(-1);
  if (h.installed) g_picker_pad_filter = PadGate;
  if (!g_view_factory) g_view_factory = [] { return MakeImGuiPickerView(MakeViewHost()); };
  g_music_hint_on = [] { return Enabled(); };
}
inline void Shutdown() {
  Host& h = H();
  h.life.Release();
  g_picker_pad_filter = nullptr;
  g_music_hint_on = nullptr;
  h.installed = false;
  h.drawer = nullptr;
}

// Cached after Install (user_playlist::Enabled looks at the replay cvars by name): the hooks ask on every notice.
inline bool Enabled() {
  Host& h = H();
  if (!h.installed) return false;
  int8_t c = h.enabled_cache.load(std::memory_order_relaxed);
  if (c < 0) {
    c = REXCVAR_GET(wwe2k14_music_picker) && user_playlist::Enabled() ? 1 : 0;
    h.enabled_cache.store(c, std::memory_order_relaxed);
  }
  return c == 1;
}

// The ID3 tags of a library file (title, artist, cover); empty when it has none. Runs on the WORKER, never in a guest hook.
inline Tags ReadTags(const std::filesystem::path& p) {
  Tags t;
  try {
    if (user_playlist::Lower(user_playlist::U8(p.extension())) != ".mp3") return t;
    std::ifstream in(p, std::ios::binary);
    uint8_t head[10];
    if (!in.read(reinterpret_cast<char*>(head), 10)) return t;
    if (head[0] != 'I' || head[1] != 'D' || head[2] != '3') return t;
    const size_t size = std::min<size_t>(10 + id3::Syncsafe(head + 6), 8u << 20);
    std::vector<uint8_t> buf(size);
    std::memcpy(buf.data(), head, 10);
    in.read(reinterpret_cast<char*>(buf.data() + 10), std::streamsize(size - 10));
    buf.resize(size_t(10 + in.gcount()));
    return ParseId3(buf.data(), buf.size());
  } catch (...) {
    return Tags{};
  }
}

// A stable id for a file's cover: FNV-1a of its UTF-8 path, size and mtime, so the view's texture cache (one per process: the overlay layer
// keeps every uploaded texture) finds the same cover again at the next open and a changed file gets a new one. Never 0.
inline uint32_t ArtId(const std::filesystem::path& p) {
  std::error_code ec;
  uint32_t hsh = 2166136261u;
  auto feed = [&hsh](const void* d, size_t n) {
    const auto* b = static_cast<const uint8_t*>(d);
    for (size_t i = 0; i < n; ++i) hsh = (hsh ^ b[i]) * 16777619u;
  };
  const std::string u = user_playlist::U8(p);
  feed(u.data(), u.size());
  const uint64_t size = std::filesystem::file_size(p, ec);
  const int64_t mtime = ec ? 0 : int64_t(std::filesystem::last_write_time(p, ec).time_since_epoch().count());
  feed(&size, sizeof size), feed(&mtime, sizeof mtime);
  return hsh ? hsh : 1u;
}

inline void LogEditorKey(uint8_t* base, const std::string& key);  // lane 309 trace, defined with the other diagnostics below

// Game thread: open the picker. False = it cannot (the caller lets the game's own notice show). Only the folder scan and choices.txt run
// here (review S5); the tags and covers are read by a worker and arrive through the UI thread. Never throws out of a hook.
inline bool TryOpen(uint8_t* base, uint32_t lr) {
  try {
    Host& h = H();
    if (!Enabled() || h.life.open.load()) return false;
    user_playlist::Rescan();
    auto songs = std::make_shared<const std::vector<std::filesystem::path>>(user_playlist::TheMachine().Songs());
    const std::string key = user_playlist::CurrentKey(base);  // lane 309: the edited wrestler's character id ("" = unreadable: the "*" line)
    LogEditorKey(base, key);
    std::string current;  // the saved choice's file name; empty = none saved (no line is not "song 1")
    {
      const auto c = user_playlist::ParseChoices(user_playlist::ReadChoices());
      const size_t i = user_playlist::CurrentChoice(c, key, *songs);
      if (i != size_t(-1)) current = user_playlist::U8((*songs)[i].filename());
    }
    const int32_t seq = user_playlist::ReadSeq(base);
    h.preview.seq = seq;
    h.force_close.store(false);  // a stale close request of the last open must not end this one
    h.opened_ms.store(NowMs());
    const uint32_t gen = h.life.Begin();  // the pad is the picker's from now on
    h.post_ui([songs, key, current, seq, lr, gen] {
      Host& hh = H();
      if (!hh.life.PostedMayRun(gen)) return;  // released meanwhile (a stalled UI thread): nothing of this open may start, a newer open posts its own
      if (!hh.picker) hh.picker.reset(new Picker(hh.preview, [](const std::filesystem::path& p) {
        std::error_code ec;
        return std::filesystem::is_regular_file(p, ec);
      }));
      hh.key = key;
      hh.library = user_playlist::LibraryDir();
      hh.open_seq = seq;
      auto view = g_view_factory();
      hh.picker->Open(*songs, current, "MUSIC LIBRARY", "Choose the song for this entrance",
                      "No songs yet. Put mp3, m4a, wav or flac files in Mods > Music > library.");
      view->OnOpen();
      {
        std::lock_guard lock(hh.view_m);
        hh.view = std::move(view);
      }
      hh.art_queue.clear();
      if (!hh.dialog && hh.drawer)
        hh.dialog = OpenPickerDialogWithView(
            hh.drawer, hh.view.get(), [](PickerView& v, float dt) { return PumpFrame(v, dt); }, [] { H().dialog = nullptr; });
      if (!hh.dialog) {  // no dialog = nothing would ever close it: give the pad back
        hh.life.Release();
        std::lock_guard lock(hh.view_m);
        hh.view.reset();
        REXLOG_WARN("[wwe2k14][music_picker] the dialog could not be opened: the game's own notice");
        return;
      }
      REXLOG_INFO("[wwe2k14][music_picker] opened: {} song(s), key '{}', current '{}', sequence {}, caller lr {:08X}", songs->size(), key,
                  current, seq, lr);
      // The tags and covers: read off every game and UI thread, handed over one by one (the list may shift meanwhile: by path).
      auto post = hh.post_ui;
      std::thread([songs, gen, post] {
        try {  // nothing may escape a worker (std::terminate): the rows just keep their file names and the record
          size_t art_total = 0;
          for (const auto& path : *songs) {
            if (H().life.gen.load() != gen) return;  // closed meanwhile
            Tags t = ReadTags(path);
            if (art_total + t.art.size() > (24u << 20)) t.art.clear();  // a bounded cover budget per open
            art_total += t.art.size();
            const uint32_t id = t.art.empty() ? 0 : ArtId(path);
            Rgba thumb;  // decoded here (<= 3000 px source, 160 px result); the UI thread only uploads it
            const bool has_art = id && DecodeCover(t.art.data(), t.art.size(), 160, thumb);
            t.art.clear(), t.art.shrink_to_fit();
            post([path, title = std::move(t.title), artist = std::move(t.artist), id = has_art ? id : 0u, thumb = std::move(thumb), gen]() mutable {
              Host& h2 = H();
              if (h2.life.gen.load() != gen || !h2.picker || !h2.picker->is_open()) return;
              h2.picker->SetTagsByPath(path, title, artist, id);
              if (id) h2.art_queue.push_back({id, thumb.w, thumb.h, std::move(thumb.px)});
            });
          }
        } catch (...) {
        }
      }).detach();
    });
    return true;
  } catch (...) {
    H().life.Release();
    REXLOG_WARN("[wwe2k14][music_picker] could not open (an exception, probably a file name): the game's own notice shows");
    return false;
  }
}

// Game thread, once per frame (kernel_overrides.h): the game left the sequence the picker opened in -> close it.
inline void OnFrame(uint8_t* base) {
  Host& h = H();
  if (!h.life.open.load(std::memory_order_acquire)) return;
  // The dialog never ran a frame (a lost post, no overlay): the pad must not stay the picker's for ever.
  if (h.life.pump_frames.load() == 0 && NowMs() - h.opened_ms.load() > 2000) {
    h.life.Release();  // (a dialog or posted open that shows up later finds the generation changed and closes / never starts)
    REXLOG_WARN("[wwe2k14][music_picker] the picker never drew a frame in 2 s: the pad goes back to the game");
    return;
  }
  const int32_t s = h.preview.seq.load();
  const int32_t now = user_playlist::ReadSeq(base);
  if (s >= 0 && now >= 0 && now != s) h.force_close.store(true);
}

// ---- diagnostics for the one run that confirms the KEY (docs\systems\music.md "Music picker: the KEY trace") ----
inline bool Rd(uint8_t* base, uint32_t a, uint32_t* out) {
  if (a < 0x1000 || (a & 3) || !GuestWordReadableFast(a)) return false;
  uint32_t v;
  std::memcpy(&v, rex::memory::GuestPtr(base, a), 4);
  *out = ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) | ((v >> 8) & 0xFF00) | (v >> 24);
  return true;
}
// A short printable string at a guest address ("" when it is not one): ASCII, or UTF-16BE ASCII as the 360 wide strings.
inline std::string Peek(uint8_t* base, uint32_t a) {
  if (a < 0x10000 || a >= 0xA0000000u) return {};
  std::string s;
  uint32_t w = 0;
  if (!Rd(base, a & ~3u, &w)) return {};
  const uint8_t b0 = rex::memory::GuestPtr(base, a)[0], b1 = rex::memory::GuestPtr(base, a)[1];
  const bool wide = b0 == 0 && b1 >= 0x20 && b1 < 0x7F;
  for (int i = 0; i < 40; ++i) {
    const uint32_t at = a + uint32_t(i) * (wide ? 2 : 1);
    if (!Rd(base, at & ~3u, &w)) break;
    const uint8_t* p = rex::memory::GuestPtr(base, at);
    const uint8_t c = wide ? (p[0] == 0 ? p[1] : 0) : p[0];
    if (c == 0) break;
    if (c < 0x20 || c >= 0x7F) return {};
    s += char(c);
  }
  return s.size() >= 3 ? s : std::string{};
}
// `len` bytes of the object at `a` as words; a word that points at a printable string shows it.
inline void LogDump(uint8_t* base, const char* what, uint32_t a, uint32_t len) {
  std::string line;
  for (uint32_t o = 0; o < len; o += 4) {
    uint32_t v = 0;
    if (!Rd(base, a + o, &v)) continue;
    if (!v) continue;
    char b[48];
    std::snprintf(b, sizeof b, " +%u=%08X", o, v);
    line += b;
    const std::string t = Peek(base, v);
    if (!t.empty()) line += "('" + t + "')";
    if (line.size() > 1500) {
      REXLOG_INFO("[wwe2k14][music_picker] {} @{:08X}:{}", what, a, line);
      line.clear();
    }
  }
  REXLOG_INFO("[wwe2k14][music_picker] {} @{:08X}:{}", what, a, line);
}
// Lane 309 (trace only): what the picker's KEY came from. The next hands-on run reads this line: for AJ Lee's custom entrance (the first
// superstar) it must say +84=0000012A (298 = her theme id = her character id) and key '298'. If +84 is something else, the dump of the screen
// object (S, first 0x120 bytes) holds the right word (the one equal to her character id) and CurrentKey's kEditorCharOffset moves there.
inline void LogEditorKey(uint8_t* base, const std::string& key) {
  if (!REXCVAR_GET(wwe2k14_music_picker_trace)) return;
  static std::atomic<int> n{0};
  if (n.fetch_add(1) >= 8) return;
  const uint32_t s = user_playlist::g_cae_screen.load();
  uint32_t m = 0, w76 = 0, w80 = 0, w84 = 0, w88 = 0;
  Rd(base, user_playlist::kEditorPreviewGlobal, &m);
  if (s) Rd(base, s + 76, &w76), Rd(base, s + 80, &w80), Rd(base, s + 84, &w84), Rd(base, s + 88, &w88);
  REXLOG_INFO("[wwe2k14][music_picker] editor key: screen={:08X} preview M={:08X} +76={:08X} +80={:08X} +84={:08X} +88={:08X} -> key '{}'", s, m, w76,
              w80, w84, w88, key);
  if (s) LogDump(base, "editor screen", s, 0x120);
}
// Lane 309 (trace only): the ENTRANCE side. The game's current music context at the moment a USER PLAYLIST track starts: X = [0x83929790]
// (sub_82AAC3F0's global, the object sub_82ACFDE0 starts from), Y = [X+52] (the object whose vtable slot 4 returns the current music
// object). Any word here equal to the entering wrestler's character id (compare with the roster: a small number < 500) is the entrance
// key; the vtable word names the class. Nothing is called, only read.
inline void LogEntranceContext(uint8_t* base) {
  uint32_t x = 0, y = 0, vt = 0, slot4 = 0;
  if (!Rd(base, 0x83929790, &x) || !x || !Rd(base, x + 52, &y) || !y) {
    REXLOG_INFO("[wwe2k14][music_picker] entrance context: X={:08X} Y={:08X} (unreadable)", x, y);
    return;
  }
  if (Rd(base, y, &vt) && vt) Rd(base, vt + 16, &slot4);
  REXLOG_INFO("[wwe2k14][music_picker] entrance context: X={:08X} Y={:08X} vtable={:08X} slot4={:08X} (the music-object getter)", x, y, vt, slot4);
  LogDump(base, "entrance context Y", y, 0x100);
}

// The editor's SetSong (sub_8334C138, called from src\music_preview.h's hook): the playlist-name argument.
inline void LogSetSong(uint8_t* base, uint32_t self, int32_t id, uint32_t name_ptr) {
  static std::atomic<int> n{0};
  if (id != 9998 || n.fetch_add(1) >= 12) return;
  REXLOG_INFO("[wwe2k14][music_picker] SetSong(9998): M={:08X} name_ptr={:08X} '{}' seq={}", self, name_ptr, Peek(base, name_ptr),
              user_playlist::ReadSeq(base));
  if (name_ptr) LogDump(base, "SetSong name block", name_ptr, 96);
}

}  // namespace wwe2k14::music_picker

// The notice routine: a 50303 call opens the picker instead.
REX_HOOK_RAW(sub_82268C10) {
  if (ctx.r4.u32 == wwe2k14::music_picker::kHintTextId && wwe2k14::music_picker::TryOpen(base, uint32_t(ctx.lr))) {
    ctx.r3.u64 = 0;
    return;  // the picker is the console's "pick the song in the Guide"
  }
  __imp__sub_82268C10(ctx, base);
}

// S101 2026-11-13 (Jarrod, phone + PC, run 7213 log: "the USER PLAYLIST option remains greyed out and doesn't actually
// select/apply the song when you choose it from the Music Library that pops up"). Listings read (0 runs):
//  - sub_83353A18 (0x83353AE4-0x83353B20) finds the 9998 row (sub_82270400) and sets its "unavailable" bit (entry +4 bit 0,
//    sub_82270560) when sub_8334B280 (the Xbox user-music state at [0x839BFC38]+0x2057C / +0x20554) says no - always on PC.
//  - The row's select handler sub_83353F30: 9998 -> the 50303 notice (= our picker), then sub_8226D850 (the cursor entry's
//    bit 0): set -> nothing more for 9998 (the row is greyed and never applied: 7213, +68 stayed 6002 after the pick); clear ->
//    for 9998 only a list close through (this-28) vtable 16, for every other id the list's apply (vtable 40) = the row becomes
//    the wrestler's music.
// So, while our picker can open: the availability check says yes (the row is no longer greyed), and inside the handler the
// lock read (return address 0x83353F78) says unlocked for a 9998 entry and the id read after it (0x83353FC8) says "not 9998",
// so USER PLAYLIST takes the apply path every song row takes and the wrestler's music becomes 9998 (the entrance's XMP path,
// src\user_playlist.h, then plays the picked song). Every other row is untouched. sub_8334B280's one other caller, sub_833640C0
// (another editor screen, which one [U]; Opus review 2026-11-13), stops greying ITS 9998 row too and then takes its own console
// path (sub_8335F530 -> sub_8335E3E0 applies the row, the 50303 notice opens our picker). The row is applied when it is chosen,
// so cancelling the picker leaves the wrestler on USER PLAYLIST (his entrance plays the '*' choice / the first song).
REX_EXTERN(__imp__sub_8334B280);
REX_EXTERN(__imp__sub_8226D850);
REX_EXTERN(__imp__sub_8226D870);
namespace wwe2k14::music_picker {
// The list widget's cursor entry value (sub_8226D870's own reads: list+292 -> {count +0, entries +8, 24 bytes each}, list+24 = cursor).
inline bool CursorValueIs9998(uint8_t* base, uint32_t list) {
  uint32_t arr = 0, idx = 0, count = 0, ents = 0, v = 0;
  if (!Rd(base, list + 292, &arr) || !arr || !Rd(base, list + 24, &idx) || !Rd(base, arr, &count) || idx >= count ||
      !Rd(base, arr + 8, &ents) || !ents || !Rd(base, ents + 24 * idx, &v)) {
    return false;
  }
  return v == 9998;
}
}  // namespace wwe2k14::music_picker
REX_HOOK_RAW(sub_8334B280) {
  __imp__sub_8334B280(ctx, base);
  if (ctx.r3.u32 == 0 && wwe2k14::music_picker::Enabled()) ctx.r3.u64 = 1;  // "the user's music is available": our library
}
REX_HOOK_RAW(sub_8226D850) {
  const uint32_t lr = uint32_t(ctx.lr), list = ctx.r3.u32;
  __imp__sub_8226D850(ctx, base);
  if (lr == 0x83353F78 && ctx.r3.u32 != 0 && wwe2k14::music_picker::Enabled() &&
      wwe2k14::music_picker::CursorValueIs9998(base, list)) {
    ctx.r3.u64 = 0;  // USER PLAYLIST is not locked while our picker serves it
  }
}
REX_HOOK_RAW(sub_8226D870) {
  const uint32_t lr = uint32_t(ctx.lr);
  __imp__sub_8226D870(ctx, base);
  if (lr == 0x83353FC8 && ctx.r3.u32 == 9998 && wwe2k14::music_picker::Enabled()) {
    static std::atomic<int> n{0};
    if (n.fetch_add(1) < 8) REXLOG_INFO("[wwe2k14][music_picker] USER PLAYLIST row applied as the wrestler's music (the game's apply path)");
    ctx.r3.u64 = 9999;  // only this compare: the handler then applies the cursor row (its value, 9998) like any song
  }
}

// Diagnostics only (calls the original first): the MUSIC row's select handler. When the row is USER PLAYLIST the handler's
// object and the music record are dumped, to find what identifies the created entrance in the editor.
REX_HOOK_RAW(sub_83353F30) {
  using namespace wwe2k14::music_picker;
  if (!REXCVAR_GET(wwe2k14_music_picker_trace)) return __imp__sub_83353F30(ctx, base);  // diagnostics only, off by default
  static std::atomic<int> n{0};
  const uint32_t self = ctx.r3.u32;
  uint32_t rec = 0;
  if (Rd(base, self + 44, &rec) && n.load() < 4 && wwe2k14::user_playlist::Enabled()) {
    n.fetch_add(1);
    REXLOG_INFO("[wwe2k14][music_picker] MUSIC row selected: this={:08X} record={:08X} seq={}", self, rec,
                wwe2k14::user_playlist::ReadSeq(base));
    LogDump(base, "row object", self, 0x60);
    LogDump(base, "music record", rec, 0x60);
  }
  __imp__sub_83353F30(ctx, base);
}

// Diagnostics only: the music manager's per-frame track update (sub_82ACDCE8, r3 = the manager). The first times a track
// holds USER PLAYLIST (+292 == 9998, +288 == 0) it is dumped: at an entrance this is where the entering wrestler can show.
REX_HOOK_RAW(sub_82ACDCE8) {
  using namespace wwe2k14::music_picker;
  if (!REXCVAR_GET(wwe2k14_music_picker_trace)) return __imp__sub_82ACDCE8(ctx, base);  // every frame for every player otherwise
  static std::atomic<int> n{0};
  if (n.load() < 3 && wwe2k14::user_playlist::Enabled()) {
    uint32_t list = 0, head = 0;
    if (Rd(base, ctx.r3.u32 + 8, &list) && Rd(base, list + 4, &head)) {
      for (int guard = 0; head && guard < 64; ++guard) {
        uint32_t obj = 0, next = 0, a = 0, b = 0;
        if (!Rd(base, head + 8, &obj) || !Rd(base, head, &next)) break;
        if (obj && Rd(base, obj + 288, &a) && Rd(base, obj + 292, &b) && a == 0 && b == 9998 && n.fetch_add(1) < 3) {
          REXLOG_INFO("[wwe2k14][music_picker] USER PLAYLIST track: manager={:08X} track={:08X} seq={}", ctx.r3.u32, obj,
                      wwe2k14::user_playlist::ReadSeq(base));
          LogDump(base, "track", obj, 0x1A0);
          LogEntranceContext(base);
          break;
        }
        head = next;
      }
    }
  }
  __imp__sub_82ACDCE8(ctx, base);
}
