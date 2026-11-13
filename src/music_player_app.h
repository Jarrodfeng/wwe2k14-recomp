// wwe2k14 - the music player inside the game: glue between src\music_player.h and the SDK's audio inject hook (lane 288).
//
// --wwe2k14_music_mix_into_game=true   custom music (entrance themes, the editor's PREVIEW, the USER PLAYLIST) is decoded
//                                      by src\music_player.h and added to the guest's audio frames by the SDK hook
//                                      rex::perf::audio::SetInject (SDK patch 115), so it obeys --audio_mute, appears in
//                                      --wwe2k14_audio_capture and freezes with the pause. Default FALSE until the live check
//                                      (docs\systems\audio.md 7.5) has proven it; false = today's Media Foundation MFPlay path.
//
// THE RULES (one player, three owners: Entrance = src\entrance_music.h, Preview = src\music_preview.h (lane 281), Playlist =
// lane 282's Machine in src\user_playlist.h):
//   * a new owner's Start pre-empts the current song, whoever owned it (one voice; the old owner gets a Stopped event);
//     commands are coalesced, only the last Start is opened;
//   * ONE resolver: Resolve(path) / ResolveId(id) -> lane 283's cached normalised file (fallback: the original file);
//   * ONE volume rule: EffectiveVolume() = the game's music volume (SetGameVolume, fed by the XMPSetVolume hook
//     sub_82E1A7C0 that lane 282 already has) x clamp(--wwe2k14_entrance_music_volume);
//   * ONE stop rule (entrance / preview): by the guest sequence seen at Start (entrance_music.h NoteStart), not "seq != 9".
//
// THE ONE-LINE CHANGE for lane 282 / 291 (Playlist owner), in src\user_playlist.h, next to MfPlayer:
//
//     struct MixPlayer : PlayerIf {   // PlayerIf = Start(file, token, volume), Stop, Pause(bool), SetVolume(float)
//       bool Start(const std::filesystem::path& f, uint32_t token, float v) override {
//         wwe2k14::music_app::Start(wwe2k14::music::Kind::Playlist, f, token, v); return true; }
//       void Stop() override { wwe2k14::music_app::Stop(wwe2k14::music::Kind::Playlist); }
//       void Pause(bool p) override { wwe2k14::music_app::Pause(p); }
//       void SetVolume(float v) override { wwe2k14::music_app::SetGameVolume(v); wwe2k14::music_app::SetVolume(wwe2k14::music_app::EffectiveVolume()); }
//     };
//     // choose it when wwe2k14::music_app::Active(), else MfPlayer; the end of a song comes from music_app::Pump:
//     //   Pump([](const music::Event& e) { if (e.kind == Kind::Playlist && (e.type == EventType::Ended || e.type == EventType::Failed))
//     //     g_machine->OnSongEnded(uint32_t(e.tag), e.type == EventType::Ended); });
//   (call Pump once per frame; entrance_music.h OnFrame already does and hands it the Entrance / Preview handler, so lane 291 adds
//   its handler to that one call: Pump takes ONE handler, wrap both in a lambda there.)
#pragma once

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/perf/audio_inject.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <thread>
#include <string>

#include "music_arena_fx.h"
#include "music_player.h"
#include "music_player_mf.h"

REXCVAR_DEFINE_BOOL(wwe2k14_music_mix_into_game, true, "wwe2k14",  // S101 2026-11-13: default ON (Jarrod, after the live hands-on runs 7212 / 7213)
                    "Lane 288: play custom music (entrance themes, preview, playlist) through the SDK's audio inject hook, "
                    "added to the game's own mix: it obeys --audio_mute, shows in --wwe2k14_audio_capture and pauses with "
                    "the game. Default ON since S101 (live runs 7212 / 7213); false = the old Media Foundation player. See src/music_player_app.h");

REXCVAR_DEFINE_BOOL(wwe2k14_music_arena_fx, true, "wwe2k14",
                    "Lane 298: custom songs played through --wwe2k14_music_mix_into_game get the stock themes' arena sound: the "
                    "RoomVerb reverb and the two EQs of the game's own music chain (parameters read from your own "
                    "Game_Sound_E_S.pck), in the level a stock theme gets. Entrance and Preview songs only; the "
                    "playlist stays dry. false = dry. Only active while the player is (mix_into_game). See src/music_arena_fx.h");
REXCVAR_DEFINE_DOUBLE(wwe2k14_music_arena_fx_trim_db, 0.0, "wwe2k14",
                      "Lane 298: level trim of the arena effects in dB (-12..12), for tuning by ear. Read once when the effects are prepared: restart to change");

namespace wwe2k14::music_app {

inline bool Active() { return REXCVAR_GET(wwe2k14_music_mix_into_game); }

// ONE file resolver. Identity: FindMusic / music_slots::FileFor already return lane 283's cached, normalised file (or, while
// it is being made, the source itself), so the player opens exactly what it is given. Every owner resolves through here.
inline std::filesystem::path Resolve(const std::filesystem::path& file) { return file; }
inline std::filesystem::path ResolvePlayable(const std::filesystem::path& file) { return Resolve(file); }

// ONE volume rule: the game's own music volume x the player's cvar. The game's volume is fed by SetGameVolume (the
// XMPSetVolume hook, f1 = the volume; default 1 = not known yet, as before this lane).
inline std::atomic<float> g_game_volume{1.0f};
inline void SetGameVolume(float v) { g_game_volume.store(std::clamp(v, 0.0f, 1.0f)); }
// EffectiveVolume() is defined in src\entrance_music.h (it needs that file's wwe2k14_entrance_music_volume cvar).

// ---- arena effects (lane 298). The game folder is set once at start (SetGameRoot). The chain (pck read + MusicChain prepare,
// about 45-65 ms) is prepared OFF the guest threads: on the music player's decode thread as its warm-up (the first song stays
// Loading until it is done, so it never starts dry), or on a one-off helper thread when the cvar is flipped on in a session.
// UpdateArenaFx (frame thread, every Pump) only publishes an already-prepared chain.
inline std::filesystem::path g_game_root;
inline void SetGameRoot(const std::filesystem::path& root) { g_game_root = root; }

inline std::atomic<bool> g_created{false};  // the player (and its thread, and the SDK hook) exist only once something plays
inline std::atomic<music::ArenaFx*> g_arena_fx{nullptr};  // set once, never freed: the audio thread may use it until exit
inline std::atomic<bool> g_arena_fx_preparing{false};
inline std::atomic<const music::Chain*> g_arena_published{nullptr};  // what SetChain last got
inline std::atomic<bool> g_arena_fx_logged{false};

// Parameters from the player's own sound bank (a folder dump; a disc image or a missing file falls back to the built-in
// numbers, which are the same ones, and says so once). Blocking (tens of ms): never on a guest or frame thread.
inline music::ArenaFx* PrepareArenaFx() {
  static std::mutex mu;
  std::lock_guard<std::mutex> lk(mu);
  if (auto* have = g_arena_fx.load()) return have;
  wwe::musicchain::Params params = wwe::musicchain::defaults();
  std::string why = "no game folder";
  bool banks = false;
  std::error_code ec;
  if (!g_game_root.empty() && std::filesystem::is_directory(g_game_root, ec)) {
    banks = wwe::musicchain::load_from_pck((g_game_root / "sound" / "Game_Sound_E_S.pck").string(), params, &why);
  } else if (!g_game_root.empty()) {
    why = "the game files are a disc image";
  }
  if (!banks) REXLOG_WARN("[wwe2k14][music] arena fx: stock chain not read from the sound bank ({}); using the built-in numbers", why);
  const float trim = float(std::clamp(REXCVAR_GET(wwe2k14_music_arena_fx_trim_db), -12.0, 12.0));
  auto* fx = new music::ArenaFx();
  fx->Prepare(params, banks, trim);
  const auto& r = fx->report();
  REXLOG_INFO("[wwe2k14][music] arena fx ready: chain from {} (RoomVerb decay {:.2f} s, pre-delay {:.0f} ms, dry {:.1f} dB, reverb {:.1f} dB); "
              "gain: undo bus+master {:+.2f} dB, trim {:+.1f} dB => output {:+.2f} dB (the chain's own static net is {:.2f} dB; a song "
              "goes through the same transform as a stock theme); tail {:.2f} s; prepared in {:.0f} ms off the guest threads",
              banks ? "the sound bank" : "built-in numbers", params.rv.decay_s, params.rv.pre_delay_ms, params.rv.dry_db,
              params.rv.reverb_db, r.undo_staging_db, r.trim_db, r.user_gain_db, r.net_dry_db,
              double(r.tail_frames) / music::kOutRate, r.prepare_ms);
  g_arena_fx.store(fx);
  return fx;
}

inline void PublishArenaFx(music::MusicPlayer& player, const music::Chain* desired) {
  if (g_arena_published.exchange(desired) == desired && g_arena_fx_logged.load()) return;
  g_arena_fx_logged.store(true);
  player.SetChain(desired);
  REXLOG_INFO("[wwe2k14][music] arena fx {} (entrance and preview songs get the stock room; the playlist stays dry)",
              desired ? "ON" : "OFF (dry)");
}

// Follow --wwe2k14_music_arena_fx (a cvar read and an atomic load per frame). Turning it on in a session prepares the chain on a
// helper thread once; the chain is published on a later frame. Turning it off keeps the old chain running until its tail ends.
inline void UpdateArenaFx(music::MusicPlayer& player) {
  const bool want = REXCVAR_GET(wwe2k14_music_arena_fx);
  music::ArenaFx* fx = g_arena_fx.load();
  if (want && !fx && !g_arena_fx_preparing.exchange(true)) std::thread([] { PrepareArenaFx(); }).detach();
  if (want && !fx) return;  // still preparing: nothing to publish yet (no false "OFF")
  PublishArenaFx(player, want ? fx->AsChain() : nullptr);
}

// S101 2026-11-13 (Jarrod: custom songs quieter than the game's own; measured in hands-on 7220's capture: the game's audio in the
// editor ~-18 LUFS, our dry library preview -24.5, our editor preview with the arena room -27.4). One loss is the SDK's stereo fold:
// our song is added to the guest's FL / FR BEFORE the 5.1 -> stereo fold (sdl_audio_driver.cpp: inject at line ~130, the fold after
// it), which scales FL / FR by StereoFold::scale = 1 / (1 + 0.707) = 0.586 (-4.6 dB, include\rex\audio\downmix.h). Our song is
// already a finished stereo mix, so the fold must not touch it: it gets 1 / scale back here. Phones always fold; a PC with a 5.1 /
// 7.1 endpoint does not (the SDK then passes the channels through) and gets the song 4.6 dB louder [U: no such endpoint measured].
REXCVAR_DEFINE_BOOL(wwe2k14_music_fold_comp, true, "wwe2k14",
                    "S101: give custom songs back the 4.6 dB the SDK's 5.1 -> stereo fold takes from the guest's FL / FR (our song is "
                    "added before the fold). false = the old level. See src/music_player_app.h");
inline constexpr float kFoldComp = 1.0f + 0.70710678f;  // 1 / StereoFold::scale (the SDK default fold)
inline constexpr uint32_t kInjectBlock = 1024;
inline bool InjectComp(void* user, float* l, float* r, uint32_t n, uint32_t rate) {
  auto* player = static_cast<music::MusicPlayer*>(user);
  if (!REXCVAR_GET(wwe2k14_music_fold_comp)) return music::MusicPlayer::InjectThunk(user, l, r, n, rate);
  thread_local float tl[kInjectBlock], tr[kInjectBlock];
  bool any = false;
  for (uint32_t off = 0; off < n; off += kInjectBlock) {
    const uint32_t m = std::min(kInjectBlock, n - off);
    std::memset(tl, 0, m * sizeof(float));
    std::memset(tr, 0, m * sizeof(float));
    if (!music::MusicPlayer::InjectThunk(player, tl, tr, m, rate)) continue;
    any = true;
    for (uint32_t i = 0; i < m; ++i) l[off + i] += kFoldComp * tl[i], r[off + i] += kFoldComp * tr[i];
  }
  return any;
}

inline music::MusicPlayer& Player() {
  static music::MusicPlayer* p = [] {
    // never destroyed: the SDK's audio thread may call Mix until exit. The warm-up runs on its decode thread.
    auto* player = new music::MusicPlayer([](music::MusicPlayer& pl) {
      if (!REXCVAR_GET(wwe2k14_music_arena_fx)) return;
      g_arena_fx_preparing.store(true);
      PublishArenaFx(pl, PrepareArenaFx()->AsChain());  // the first song is still Loading: it never starts dry
    });
    music::mf::Install();
    player->SetLogSink([](int level, const char* msg) {
      if (level >= 1) REXLOG_WARN("{}", msg);
      else REXLOG_INFO("{}", msg);
    });
    rex::perf::audio::SetInject(&InjectComp, player);  // S101: the fold compensation wraps the player's mix
    g_created = true;  // after SetInject: the pump may now read the player
    REXLOG_INFO("[wwe2k14][music] player registered with the SDK audio inject hook (48 kHz stereo, added to the game's FL / FR)");
    return player;
  }();
  return *p;
}

inline uint64_t Play(const std::filesystem::path& file, music::Kind kind, float volume, int64_t tag = -1) {
  return Player().Play(ResolvePlayable(file), kind, volume, tag);
}
inline uint64_t Start(music::Kind owner, const std::filesystem::path& file, uint32_t token, float volume) {
  return Player().Start(owner, ResolvePlayable(file), token, volume);
}
inline void Pause(bool paused) { Player().Pause(paused); }
inline bool Stop(music::Kind kind = music::Kind::Any) { return Player().Stop(kind); }
inline music::State StateOf(music::Kind kind) { return Player().GetState(kind); }
inline void SetVolume(float v) { Player().SetVolume(v); }

// Drain the end-of-song events. The handler sees every event (all kinds); the entrance player's own handler is in
// src\entrance_music.h. Logs each one.
inline void Pump(const std::function<void(const music::Event&)>& handler) {
  if (!g_created.load()) return;
  UpdateArenaFx(Player());
  music::Event ev;
  while (Player().PollEvent(ev)) {
    const char* what = ev.type == music::EventType::Started ? "started"
                       : ev.type == music::EventType::Ended ? "ended"
                       : ev.type == music::EventType::Stopped ? "stopped" : "failed";
    REXLOG_INFO("[wwe2k14][music] {} song {} {}{}{}", music::KindName(ev.kind), ev.tag, what, ev.text.empty() ? "" : ": ", ev.text);
    if (handler) handler(ev);
  }
}

}  // namespace wwe2k14::music_app
