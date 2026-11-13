// wwe2k14 - custom songs in the Create-an-Entrance MUSIC preview (lane 281, 2026-11-13). Included after
// entrance_music.h (kernel_overrides.h). Decision logic: src\music_preview_core.h (unit test
// tools\audio\music_preview_unit.cpp); the player: src\entrance_music.h (PlayPreview / Stop).
//
// Why the entrance hook (sub_82ACFDE0) never sees the preview: the editor plays the preview through its own preview
// object M (global 0x839BFC3C -> object, constructed by sub_83358560 -> sub_8334C050), whose one setter is
// SetSong(M, music id, playlist name) = sub_8334C138: id 9999 (NONE) stops, 9998 (USER PLAYLIST) starts the Xbox
// library, any other id is stored at M+20 and the tick (sub_8334C018 -> sub_8334BDC0) plays it through the menu-music
// player (sub_824F73E8 -> sub_82ACE130), not through the Wwise event name. Docs: docs\systems\music.md "Preview".
//
// So: hook SetSong. For an id with a file of ours (a new song 6001+, or a replaced wrestler's theme under the same rules
// as at entrances) our file plays and the game is told NONE (its own preview stops, the game's own NONE path); any other
// id stops ours and the game does its own. Stop (sub_8334C1E0), Shutdown (sub_8334C1F8) and the destructor
// (sub_8334C0C8) of M stop ours; the frame check stops it when M is gone or a match starts. The preview ignores
// SetSong exactly when the game does (M+112 != 0).
#pragma once

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include <cstdint>
#include <cstring>
#include <mutex>

#include "entrance_music.h"
#include "music_preview_core.h"
#include "music_preview_seen.h"  // lane 303: the script command press_until_preview reads the last SetSong id

REX_EXTERN(__imp__sub_8334C138);
REX_EXTERN(__imp__sub_8334C1E0);
REX_EXTERN(__imp__sub_8334C1F8);
REX_EXTERN(__imp__sub_8334C0C8);

namespace wwe2k14::music_preview {

constexpr uint32_t kGlobalM = 0x839BFC3C;  // pointer to the preview object (sub_83358560 stores it; 0 when not built)
constexpr uint32_t kOffLocked = 112;       // M+112: set by Shutdown; SetSong ignores calls then (sub_8334C138 0x8334C14C)

inline std::mutex g_mutex;  // SetSong / Stop come from the game's threads; the state is tiny
inline Preview g_state;

inline bool ReadWord(uint8_t* base, uint32_t a, uint32_t* out) {
  if (a < 0x1000 || (a & 3) || !GuestWordReadableFast(a)) return false;
  uint32_t v;
  std::memcpy(&v, rex::memory::GuestPtr(base, a), 4);
  *out = _byteswap_ulong(v);
  return true;
}

// The same rules as at an entrance (src\entrance_music.h sub_82ACFDE0 hook): a new song's id always; a stock id only when
// the replacement switch is on and <mods>\music\ID.* exists.
inline bool HasFile(int id, std::filesystem::path* file) {
  using namespace wwe2k14::entrance_music;
  const bool on = REXCVAR_GET(wwe2k14_entrance_music), slots = wwe2k14::music_slots::Active();
  if (!on && !slots) return false;
  if (!on && !wwe2k14::music_slots::IsSlotId(id)) return false;
  auto f = FindMusic(id);
  if (f.empty()) return false;
  if (file) *file = std::move(f);
  return true;
}

inline void Apply(const Step& s, const char* why, int id, const std::filesystem::path& file) {
  using namespace wwe2k14::entrance_music;
  if (s.stop && !s.play) {
    REXLOG_INFO("[wwe2k14][music] preview: ours stops ({})", why);
    Stop();
  }
  if (s.play) {
    REXLOG_INFO("[wwe2k14][music] preview: song {} -> {} instead of the game's", id, wwe2k14::mods::U8(file.filename()));
    PlayPreview(s.play_id, file);  // the worker replaces the song that plays
  }
}

// The entrance path (or a stop from elsewhere) took the player since our preview started: our note is stale, and a later
// stop of ours must not end that theme.
inline void DropStale() {
  if (g_state.playing() >= 0 && !wwe2k14::entrance_music::PreviewIsCurrent()) g_state.Forget();  // lane 288: with the mix-in player the player knows the owner
}

inline void OnSet(uint8_t* base, uint32_t self, int id, uint32_t& r4) {
  std::filesystem::path file;
  uint32_t locked = 0;
  const bool ok = self && ReadWord(base, self + kOffLocked, &locked);
  std::lock_guard lock(g_mutex);
  DropStale();
  const Step s = g_state.OnSet(id, ok && locked != 0, [&](int i) { return HasFile(i, i == id ? &file : nullptr); });
  if (s.game_id != id) r4 = uint32_t(s.game_id);
  Apply(s, "another song", id, file);
}

inline void OnStop(const char* why) {
  std::lock_guard lock(g_mutex);
  DropStale();
  Apply(g_state.OnStop(), why, -1, {});
}

}  // namespace wwe2k14::music_preview

// The frame check of a playing preview (called by entrance_music.h OnFrame): M gone, or a match / in-game sequence.
inline void wwe2k14::entrance_music::OnPreviewFrame(uint8_t* base) {
  using namespace wwe2k14::music_preview;
  uint32_t m = 0, a = 0, b = 0, seq = 0;
  const bool m_known = ReadWord(base, kGlobalM, &m);
  const bool m_gone = m_known && m == 0;
  // S101 2026-11-13 (Jarrod, hands-on run 7212: "If I hit the preview, I just hear crowd noise"): Create-an-Entrance's X preview
  // plays the WHOLE entrance in the in-game sequence (seq 9) while the preview object M is still alive, so the old "seq == 9 ->
  // a match started" rule stopped our song 1 ms after it started (7210 / 7212 logs). While M is alive we are in the editor's
  // preview: only M going away ends it. The sequence rule stays as the fallback when M can't be read.
  const bool in_game = !m_known && ReadWord(base, 0x839400F8, &a) && ReadWord(base, a, &b) && ReadWord(base, b + 48, &seq) && seq == 9;
  if (m_gone || in_game) OnStop(m_gone ? "the preview object is gone" : "a match started");
}

REX_HOOK_RAW(sub_8334C138) {
  uint32_t r4 = ctx.r4.u32;
  wwe2k14::music_seen::Note(int32_t(r4));
  wwe2k14::music_picker::LogSetSong(base, ctx.r3.u32, int32_t(r4), ctx.r5.u32);  // lane 291: the playlist-name argument (KEY trace)
  wwe2k14::music_preview::OnSet(base, ctx.r3.u32, int32_t(r4), r4);
  ctx.r4.u64 = r4;
  __imp__sub_8334C138(ctx, base);
}
REX_HOOK_RAW(sub_8334C1E0) {
  wwe2k14::music_preview::OnStop("the game stopped its preview");
  __imp__sub_8334C1E0(ctx, base);
}
REX_HOOK_RAW(sub_8334C1F8) {
  wwe2k14::music_preview::OnStop("the preview was shut down");
  __imp__sub_8334C1F8(ctx, base);
}
REX_HOOK_RAW(sub_8334C0C8) {
  wwe2k14::music_preview::OnStop("the preview object was destroyed");
  __imp__sub_8334C0C8(ctx, base);
}
