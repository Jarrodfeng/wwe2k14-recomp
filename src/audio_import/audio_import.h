// wwe2k14 - lane 283 (2026-11-13): song import. Any common audio file in, ONE cached, loudness-normalised file out.
//
// Jarrod, 2026-11-13: "We should also automatically convert and normalize the audio to match the loudness of the other songs
// in the game" and "we should also be able to take in mp3, wav, ogg, etc. since we'll have the launcher do a conversion to
// maintain the loudness and such for consistency".
//
// What it does (implementation audio_import.cpp; the meter / normaliser / limiter are header-only: loudness.h, normalise.h):
//   Import(src, cache_dir)  decodes the file (mp3, wav, flac, ogg-vorbis built in; m4a / aac / wma / opus through the
//                           platform decoder: Media Foundation on Windows, MediaCodec on Android), makes it stereo,
//                           measures its integrated loudness (BS.1770-4 / EBU R128), applies the gain that brings it to
//                           kTargetLufs (a true-peak limiter if the gain would clip) and writes
//                           <cache_dir>\<hash>_<target>_v<N>.wav (16-bit PCM stereo, the one format the player plays) plus a
//                           small .txt next to it with the numbers the launcher shows.
//   The SOURCE FILE IS NEVER MODIFIED. The cache is keyed by the source's content hash + the target + the format version, so a
//   changed song, a changed target or a new version rebuilds it; a renamed song hits the same entry.
//   A file that cannot be decoded, is silent or too short is REFUSED here (ok = false, err = a sentence for the launcher), so a
//   bad song never reaches a match. The player (src\entrance_music.h) only ever plays the cache file.
//
// Also at import: the source's own tags and embedded cover art (ID3v2 APIC, MP4 covr, FLAC PICTURE, Ogg / Opus
// METADATA_BLOCK_PICTURE, WMA WM/Picture; tags.h) become <key>.png (256x256, only when there is art) and title / artist / album
// lines in <key>.txt. Library() is the one call the launcher's MUSIC tab and the in-game picker make per song.
//
// Where the numbers come from: docs\systems\music.md "Loudness of the game's themes" (kTargetLufs).
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <filesystem>
#include <string>
#include <vector>

namespace audio_import {

inline constexpr int kCacheVersion = 2;  // v2: true-peak ceiling -4 dBTP (was -1)
// The loudness the game's own entrance themes reach as heard (docs\systems\music.md: median -17.6 LUFS of 108 themes, Wwise gain
// chain applied) rounded to 0.5 LU.
inline constexpr double kTargetLufs = -17.5;
inline constexpr double kTruePeakCeilingDb = -4.0;  // the arena chain adds up to +3.3 dB peak around 400-700 Hz; the stock themes peak at -8.1 dBTP median, -6.8 max

// (the priority order for one id with several files: the original six first)
inline constexpr const char* kExts[] = {".mp3", ".m4a", ".wav", ".flac", ".wma", ".aac", ".ogg", ".oga", ".opus", ".mp4"};
// By extension, case-insensitive. Inline: also used by builds that do not compile audio_import.cpp (the phone).
inline bool IsSupportedExt(const std::filesystem::path& p) {
  const auto u = p.extension().u8string();
  std::string e;
  for (auto c : u) e += char(c >= 'A' && c <= 'Z' ? c + 32 : c);
  for (const char* x : kExts) {
    if (e == x) return true;
  }
  return false;
}

// Progress of one Import (the launcher's bar). `overall` is 0..1 across the whole job. Return false from the callback to cancel
// (Import then returns cancelled = true, ok = false, and writes nothing). Called from the importing thread, about every 65k samples.
enum class Stage { Reading, Decoding, Measuring, Limiting, Writing };
inline const char* StageName(Stage s) {
  static const char* n[] = {"Reading", "Decoding", "Measuring loudness", "Limiting peaks", "Writing"};
  return n[int(s)];
}
struct Progress {
  Stage stage = Stage::Reading;
  float stage_fraction = 0, overall = 0;
};
using ProgressFn = std::function<bool(const Progress&)>;

struct Imported {
  bool ok = false;
  // ok with normalised = false: the song cannot be normalised (silent / too short) or the cache could not be written; `cache` is
  // then the SOURCE file itself and it plays as it is. `note` says why. io_failed: a write error (not remembered by Resolve).
  bool normalised = true, io_failed = false, cancelled = false;
  std::string note;
  std::string err;             // when !ok: a sentence for the player ("this file could not be read as audio: ...")
  std::filesystem::path cache;  // the normalised file (ok)
  std::string key;
  bool from_cache = false;     // true: nothing was converted this call
  double src_lufs = 0, src_true_peak_db = 0, gain_db = 0, out_lufs = 0, out_true_peak_db = 0;
  bool limited = false;
  double seconds = 0;
  int rate = 0;
  // tags read from the source file (empty = the file had none) and the cover art as a 256x256 PNG in the cache (empty path =
  // the file had no art, or art that could not be read: draw a generic record)
  std::string title, artist, album;
  std::filesystem::path thumb;
};

// The picture bytes (JPEG / PNG / BMP / GIF) -> a dim x dim PNG (centre-cropped to a square); empty on a picture that cannot be read.
std::vector<uint8_t> MakeThumbnailPng(const uint8_t* data, size_t size, int dim = 256);

// Decodes, measures, normalises and writes (or finds) the cache entry for `src`. Safe to call from any thread.
Imported Import(const std::filesystem::path& src, const std::filesystem::path& cache_dir, double target_lufs = kTargetLufs,
                const ProgressFn& progress = {});

// Same, remembering the answer per (path, size, mtime) so a player that asks every frame does not hash the file every time.
Imported Resolve(const std::filesystem::path& src, const std::filesystem::path& cache_dir, double target_lufs = kTargetLufs);

// The cache entry's numbers without converting (ok = false when there is none yet).
Imported Peek(const std::filesystem::path& src, const std::filesystem::path& cache_dir, double target_lufs = kTargetLufs);

// One library song: the normalised audio path, thumbnail path or none, title, artist, duration, loudness. Same as Resolve (memoised,
// converts on first sight) with the title falling back to the file's name; !ok = refused (err says why).
struct Song {
  bool ok = false;
  std::string err;
  std::filesystem::path audio, thumb;  // thumb empty = none
  std::string title, artist, album;
  double seconds = 0, src_lufs = 0, gain_db = 0, out_lufs = 0, out_true_peak_db = 0;
  bool limited = false;
  bool normalised = true;  // false: plays as it is (silent / too short / cache not writable); note says why
  std::string note;
};
Song Library(const std::filesystem::path& src, const std::filesystem::path& cache_dir, double target_lufs = kTargetLufs);
// Same from the cache only (Lookup): never converts; ok = false with an empty err = not converted yet.
Song LibraryCached(const std::filesystem::path& src, const std::filesystem::path& cache_dir, double target_lufs = kTargetLufs);

// ---- lookups: stat + small files only, NO decoding and NO Import. This is all the game process links (lookup.cpp) ----
// The song is converted by the LAUNCHER (Add a song / Assign music / the scan when it opens), with a progress bar; every Import writes
// <cache>\index.txt (path, size, mtime, target -> cache key / "plain" / "refused: why"), which is how the game finds the cache without
// hashing the file.
// What the cache knows about `src` right now: ok = cached (normalised copy in `cache`, or `cache` = src with normalised = false for a
// silent / unmeasurable song); !ok with a non-empty err = refused; !ok with an empty err = not converted yet.
Imported Lookup(const std::filesystem::path& src, const std::filesystem::path& cache_dir, double target_lufs = kTargetLufs);
// The path to play for `src` (the game thread; a stat and a map lookup): the cached normalised copy, an empty path for a refused file
// (the game's own theme stays), otherwise `src` itself (not converted yet: it plays as it is). `info` (optional) = the Lookup answer.
std::filesystem::path PlayableNow(const std::filesystem::path& src, const std::filesystem::path& cache_dir,
                                  double target_lufs = kTargetLufs, Imported* info = nullptr);

// Test hooks: decodes started and Import calls made in this process (the game process must show 0 of each).
// Can the game's own player (Media Foundation on Windows; WAV only elsewhere) open this file AS IT IS? Used when a song has not been
// converted yet: if it cannot, the game keeps its own theme rather than playing silence.
inline bool GamePlayerCanOpen(const std::filesystem::path& p) {
  const auto u = p.extension().u8string();
  std::string e;
  for (auto c : u) e += char(c >= 'A' && c <= 'Z' ? c + 32 : c);
#if defined(_WIN32)
  return e == ".mp3" || e == ".wav" || e == ".m4a" || e == ".aac" || e == ".wma" || e == ".flac" || e == ".mp4";
#else
  return e == ".wav";
#endif
}

// Deletes stale half-written cache files (*.part*, older than 10 minutes). The launcher calls it when a conversion pass starts.
void CleanStale(const std::filesystem::path& cache_dir);  // also drops index entries of songs that are gone (once per pass)
// Records `src` as refused in the index (the launcher does this after a song failed twice with an I/O error, so it stops being retried).
void MarkRefused(const std::filesystem::path& src, const std::filesystem::path& cache_dir, const std::string& why, double target_lufs = kTargetLufs);

struct Counters {
  std::atomic<uint64_t> decodes{0}, imports{0};
};
Counters& Stats();

std::string ContentKey(const std::filesystem::path& src, double target_lufs);  // "" when unreadable

}  // namespace audio_import
