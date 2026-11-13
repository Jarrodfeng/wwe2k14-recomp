// Lane 286 (custom music on the phone): src\music_core.h (music id, stop rules, WAV reader, PlayableFor), src\launcher_mobile\music_mods.h
// (the song / theme file rules) and the music arguments in mobile_settings.h GameArgs, on planted folders under out\android\music_test.
// Host clang, no game, no phone.   pwsh -File tools\android\cpu_semantics\run_music_test.ps1
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "../../../src/music_core.h"
#include "../../../src/launcher_mobile/music_mods.h"
#include "../../../src/launcher_mobile/mobile_settings.h"

namespace fs = std::filesystem;
namespace mc = wwe2k14::music_core;
namespace mm = mlauncher::music;
static int g_fail = 0;
static void Check(const char* name, bool ok, const std::string& got = std::string()) {
  std::printf("%s %-64s %s\n", ok ? "PASS" : "FAIL", name, got.c_str());
  if (!ok) ++g_fail;
}

static void Le(std::string& s, uint32_t v, int n) {
  for (int i = 0; i < n; ++i) s += char((v >> (8 * i)) & 255);
}
// A WAV: tag 1 = int PCM, 3 = float, 0xFFFE = extensible (sub-format tag `sub`); `extra` = a chunk before "data"; data_size < 0 = true size.
static std::string Wav(int tag, int ch, int rate, int bits, const std::string& data, int sub = 1, bool extra = false, int64_t data_size = -1) {
  std::string fmt;
  Le(fmt, uint16_t(tag), 2), Le(fmt, uint32_t(ch), 2), Le(fmt, uint32_t(rate), 4), Le(fmt, uint32_t(rate * ch * bits / 8), 4);
  Le(fmt, uint32_t(ch * bits / 8), 2), Le(fmt, uint32_t(bits), 2);
  if (tag == 0xFFFE) {
    Le(fmt, 22, 2), Le(fmt, uint32_t(bits), 2), Le(fmt, 3, 4);
    Le(fmt, uint32_t(sub), 2), fmt += std::string("\x00\x00\x00\x00\x10\x00\x80\x00\x00\xAA\x00\x38\x9B\x71", 14);
  }
  std::string body = "WAVE";
  body += "fmt ";
  Le(body, uint32_t(fmt.size()), 4);
  body += fmt;
  if (extra) body += "LIST", Le(body, 3, 4), body += std::string("abc\0", 4);  // odd size: padded
  body += "data";
  Le(body, data_size < 0 ? uint32_t(data.size()) : uint32_t(data_size), 4);
  body += data;
  std::string out = "RIFF";
  Le(out, uint32_t(body.size()), 4);
  return out + body;
}
static void Put(const fs::path& p, const std::string& bytes) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary) << bytes;
}
static std::string S16(std::initializer_list<int> v) {
  std::string d;
  for (int x : v) Le(d, uint16_t(int16_t(x)), 2);
  return d;
}

int main() {
  const fs::path root = "out/android/music_test";
  fs::remove_all(root);
  fs::create_directories(root);

  // ---- the shared rules
  Check("MusicIdOf Play_MUS_0343_0_0", mc::MusicIdOf("Play_MUS_0343_0_0") == 343);
  Check("MusicIdOf Play_MUS_6001_0_0", mc::MusicIdOf("Play_MUS_6001_0_0") == 6001);
  Check("MusicIdOf other event", mc::MusicIdOf("Play_SFX_0001") == -1);
  Check("MusicIdOf no digits", mc::MusicIdOf("Play_MUS_") == -1);
  Check("next theme ends ours", mc::NextThemeEndsOurs(230, 6001, true));
  Check("same theme again does not", !mc::NextThemeEndsOurs(6001, 6001, true));
  Check("a poll (not a play) does not", !mc::NextThemeEndsOurs(230, 6001, false));
  Check("nothing playing: nothing to end", !mc::NextThemeEndsOurs(230, -1, true));
  Check("stop: sequence ended", mc::ShouldStopAtFrame(false, true, 0));
  Check("stop: match clock running", mc::ShouldStopAtFrame(true, true, 5));
  Check("go on: in game, clock 0", !mc::ShouldStopAtFrame(true, true, 0));
  Check("go on: clock unreadable", !mc::ShouldStopAtFrame(true, false, 0));

  // ---- the WAV reader
  {
    mc::WavReader r;
    std::string err;
    Put(root / "a.wav", Wav(1, 2, 44100, 16, S16({0, 100, -100, 32767, -32768, 5})));
    Check("16-bit stereo opens", r.Open((root / "a.wav").string(), err), err);
    Check("  3 frames, 44100 Hz, 2 ch", r.Info().Frames() == 3 && r.Info().rate == 44100 && r.Info().channels == 2);
    std::vector<int16_t> o;
    Check("  reads 3 frames", r.ReadFrames(o, 100) == 3 && o.size() == 6 && o[1] == 100 && o[2] == -100 && o[4] == -32768);
    Check("  then the end", r.ReadFrames(o, 100) == 0 && r.AtEnd());
  }
  {
    mc::WavReader r;
    std::string err;
    Put(root / "b.wav", Wav(1, 1, 22050, 8, std::string("\x80\xFF\x00", 3)));
    Check("8-bit mono opens", r.Open((root / "b.wav").string(), err), err);
    std::vector<int16_t> o;
    r.ReadFrames(o, 10);
    Check("  8-bit unsigned -> signed 16", o.size() == 3 && o[0] == 0 && o[1] == 127 * 256 && o[2] == -32768);
  }
  {
    mc::WavReader r;
    std::string err;
    std::string d;  // 24-bit: 0x7FFFFF and 0x800000 (the top two bytes become the sample)
    d += std::string("\xFF\xFF\x7F", 3) + std::string("\x00\x00\x80", 3);
    Put(root / "c.wav", Wav(1, 1, 48000, 24, d));
    Check("24-bit opens", r.Open((root / "c.wav").string(), err), err);
    std::vector<int16_t> o;
    r.ReadFrames(o, 10);
    Check("  24-bit -> top 16", o.size() == 2 && o[0] == 32767 && o[1] == -32768);
  }
  {
    mc::WavReader r;
    std::string err;
    float f[3] = {1.0f, -2.0f, 0.5f};
    Put(root / "d.wav", Wav(3, 1, 48000, 32, std::string(reinterpret_cast<char*>(f), 12)));
    Check("float32 opens", r.Open((root / "d.wav").string(), err), err);
    std::vector<int16_t> o;
    r.ReadFrames(o, 10);
    Check("  float -> 16, clamped", o.size() == 3 && o[0] == 32767 && o[1] == -32767 && o[2] == 16383);
  }
  {
    mc::WavReader r;
    std::string err;
    Put(root / "e.wav", Wav(0xFFFE, 2, 44100, 16, S16({1, 2, 3, 4}), 1, true));
    Check("extensible + a padded LIST chunk before data", r.Open((root / "e.wav").string(), err), err);
    std::vector<int16_t> o;
    r.ReadFrames(o, 1);
    Check("  chunked read (1 frame at a time)", o.size() == 2 && o[0] == 1 && o[1] == 2);
    r.ReadFrames(o, 1);
    Check("  second frame", o.size() == 2 && o[0] == 3 && r.AtEnd());
  }
  {
    mc::WavReader r;
    std::string err;
    Put(root / "f.wav", Wav(1, 2, 44100, 16, S16({1, 2, 3, 4, 5, 6}), 1, false, 0xFFFFFFFF));
    Check("a streamed WAV (size 0xFFFFFFFF) takes what is there", r.Open((root / "f.wav").string(), err) && r.Info().Frames() == 3, err);
  }
  {
    mc::WavReader r;
    std::string err;
    Put(root / "g.wav", Wav(1, 2, 44100, 16, S16({1, 2, 3, 4, 5, 6}), 1, false, 100000));
    Check("a size larger than the file is cut to the file", r.Open((root / "g.wav").string(), err) && r.Info().Frames() == 3, err);
    Put(root / "h.wav", Wav(2, 2, 44100, 4, S16({1, 2})));
    Check("ADPCM refused with a reason", !r.Open((root / "h.wav").string(), err) && err.find("PCM") != std::string::npos, err);
    Put(root / "i.mp3", "ID3....");
    Check("a non-WAV refused", !r.Open((root / "i.mp3").string(), err) && err == "not a WAV file", err);
    Check("a missing file refused", !r.Open((root / "nope.wav").string(), err), err);
    Put(root / "j.wav", Wav(1, 2, 44100, 16, ""));
    Check("an empty WAV refused", !r.Open((root / "j.wav").string(), err), err);
  }

  // ---- PlayableFor (the seam to the conversion lane)
  {
    const fs::path mods = root / "mods";
    Put(mods / "music" / "new" / "Kingdom.wav", Wav(1, 2, 44100, 16, S16({1, 2, 3, 4})));
    Put(mods / "music" / "new" / "Remix.mp3", "ID3fake");
    Put(mods / "music" / "new" / "Bad.wav", "not a wav");
    Check("a good .wav plays as it is", mc::PlayableForUncached(mods / "music" / "new" / "Kingdom.wav", mods) == mods / "music" / "new" / "Kingdom.wav");
    Check("an mp3 with no cached copy: not playable", mc::PlayableForUncached(mods / "music" / "new" / "Remix.mp3", mods).empty());
    Put(mods / "music" / "cache" / "Remix.mp3.wav", Wav(1, 2, 44100, 16, S16({1, 2, 3, 4})));
    Check("  ... with music/cache/Remix.mp3.wav: plays that", mc::PlayableForUncached(mods / "music" / "new" / "Remix.mp3", mods) == mods / "music" / "cache" / "Remix.mp3.wav");
    Check("a broken .wav with no cache: not playable", mc::PlayableForUncached(mods / "music" / "new" / "Bad.wav", mods).empty());
    bool miss = false;
    mc::PlayableFor(mods / "music" / "new" / "Bad.wav", mods, &miss);
    Check("first miss reported once", miss);
    mc::PlayableFor(mods / "music" / "new" / "Bad.wav", mods, &miss);
    Check("  second ask (memo) not reported again", !miss);
  }

  // ---- the launcher's file rules
  {
    const fs::path mods = root / "lmods";
    Put(mods / "music" / "new" / "Kingdom.mp3", "x");
    Put(mods / "music" / "new" / "alpha_song.ogg", "x");
    Put(mods / "music" / "new" / "Notes.txt", "x");
    auto v = mm::ReadNewSongs(mods);
    Check("two songs found, ids from 6001 in name order", v.size() == 2 && v[0].file == "alpha_song.ogg" && v[0].id == 6001 && v[1].id == 6002 && v[1].file == "Kingdom.mp3");
    Check("  shown names in capitals", v[0].name == "ALPHA SONG" && v[1].name == "KINGDOM");
    Put(mods / "music" / "new" / "Aardvark.opus", "x");
    v = mm::ReadNewSongs(mods);
    Check("a new file gets the next free id, old ids stay", v.size() == 3 && v[2].file == "Aardvark.opus" && v[2].id == 6003 && v[0].id == 6001);
    Check("rename keeps the id", mm::RenameNewSong(mods, 6002, "my theme").empty() && mm::ReadNewSongs(mods)[1].name == "MY THEME");
    Check("remove deletes the file, keeps the line", mm::RemoveNewSong(mods, "Kingdom.mp3").empty() && !fs::exists(mods / "music" / "new" / "Kingdom.mp3"));
    v = mm::ReadNewSongs(mods);
    Check("  the id stays with a gone file (present = false)", v.size() == 3 && !v[1].present && v[1].id == 6002 && mm::CountPresent(v) == 2);
    Check("remove refuses a path", !mm::RemoveNewSong(mods, "../x").empty());
    Check("no theme yet", !mm::HasAnyTheme(mods) && mm::ThemeFor(mods, 343).empty());
    Put(mods / "music" / "cache" / "alpha_song.ogg.wav", "wavdata");
    Check("give a song to wrestler 343", mm::AssignTheme(mods, 343, mods / "music" / "new" / "alpha_song.ogg").empty());
    Check("  343.ogg exists, a theme is found", fs::exists(mods / "music" / "343.ogg") && mm::HasAnyTheme(mods) && !mm::ThemeFor(mods, 343).empty());
    // (lane 297: the converted copies live in <mods>/music/.cache, keyed by content by the importer: the launcher no longer copies them)
    Put(mods / "music" / "new" / "z.wav", "x");
    Check("a different song replaces it (old extension gone)", mm::AssignTheme(mods, 343, mods / "music" / "new" / "z.wav").empty() && !fs::exists(mods / "music" / "343.ogg") && fs::exists(mods / "music" / "343.wav"));
    mm::RevertTheme(mods, 343);
    Check("original music: theme gone", !mm::HasAnyTheme(mods));
    Check("a non-song is refused", !mm::AssignTheme(mods, 343, mods / "music" / "new" / "Notes.txt").empty());
    // the same slots.txt is the game's: it reads "6001<TAB>file[<TAB>name]" (src\music_slots.h); check the line form
    std::ifstream in(mods / "music" / "new" / "slots.txt");
    std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Check("slots.txt holds '6002<TAB>Kingdom.mp3<TAB>MY THEME'", all.find("6002\tKingdom.mp3\tMY THEME\n") != std::string::npos);
  }

  // ---- the game's arguments
  {
    mlauncher::Settings s;
    mlauncher::LaunchContext c;
    auto has = [](const std::vector<std::string>& a, const std::string& x) { return std::find(a.begin(), a.end(), x) != a.end(); };
    auto count = [](const std::vector<std::string>& a, const std::string& pre) {
      return int(std::count_if(a.begin(), a.end(), [&](const std::string& x) { return x.rfind(pre, 0) == 0; }));
    };
    auto a0 = mlauncher::GameArgs(s, c);
    Check("music off: no mods flags", !has(a0, "--wwe2k14_mods=true") && count(a0, "--wwe2k14_mods_dir=") == 0);
    s.music = true;
    c.music_dir = "/files/mods";
    auto a1 = mlauncher::GameArgs(s, c);
    Check("music on + songs: mods on, the folder once, no theme switch", has(a1, "--wwe2k14_mods=true") && has(a1, "--wwe2k14_mods_dir=/files/mods") && !has(a1, "--wwe2k14_entrance_music=true"));
    c.music_themes = true;
    auto a2 = mlauncher::GameArgs(s, c);
    Check("  with a wrestler theme: --wwe2k14_entrance_music=true", has(a2, "--wwe2k14_entrance_music=true"));
    s.superstars = true;
    c.mods_dir = "/files/mods";
    auto a3 = mlauncher::GameArgs(s, c);
    Check("  with superstars too: the folder flag only once", count(a3, "--wwe2k14_mods_dir=") == 1 && has(a3, "--wwe2k14_mods_superstars=true"));
    s.music = false;
    auto a4 = mlauncher::GameArgs(s, c);
    Check("music off, superstars on: no mods device", !has(a4, "--wwe2k14_mods=true") && !has(a4, "--wwe2k14_entrance_music=true"));
  }

  std::printf("%s (%d failed)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail);
  return g_fail ? 1 : 0;
}
