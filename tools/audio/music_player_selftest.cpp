// Offline proof of the music player + the SDK inject hook (lane 288). No game, no game files, no speakers.
// Build + run: pwsh -File tools\audio\music_player_selftest.ps1 [-Sdk <SDK worktree with patch 115>]
//
// It feeds a synthetic guest audio frame stream (6 big-endian planar channels x 256 samples, exactly what the SDK's
// SubmitFrame sees) through the REAL SDK hook code (include/rex/perf/audio_inject.h + src/core/perf/audio_inject.cpp and
// audio_tap.cpp, compiled in) and the real src\music_player.h, in the same order as sdl_audio_driver.cpp SubmitFrame:
// copy, Inject, Tap, then (SDL callback) mute / fold. Checks: a song's samples appear in FL / FR at the right gain (the
// other planes untouched), stereo balance, resampling (44.1 kHz file), stop (fade then exact silence), switch, natural end
// (event + silence), the one-voice exclusivity of Entrance / Preview / Playlist, pause (no Mix calls = the song waits and
// resumes sample-exact), the chain hook (gain + reset), mute zeroes it after the tap saw it, the SDK driver calls
// Inject before Tap before the mute, and NO heap allocation on the audio thread (a counting global operator new).
#include <rex/perf/audio_inject.h>
#include <rex/perf/audio_tap.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <new>
#include <sstream>
#include <string>
#include <thread>

#include "../../src/music_arena_fx.h"
#include "../../src/music_player.h"
#if defined(_WIN32)
#include "../../src/music_player_mf.h"
#endif

// ---- counting allocator: any new / delete while t_audio is set is a failure
static thread_local bool t_audio = false;
static std::atomic<long> g_audio_allocs{0};
void* operator new(size_t n) {
  if (t_audio) g_audio_allocs++;
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](size_t n) {
  if (t_audio) g_audio_allocs++;
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept {
  if (t_audio) g_audio_allocs++;
  std::free(p);
}
void operator delete[](void* p) noexcept {
  if (t_audio) g_audio_allocs++;
  std::free(p);
}
void operator delete(void* p, size_t) noexcept {
  if (t_audio) g_audio_allocs++;
  std::free(p);
}
void operator delete[](void* p, size_t) noexcept {
  if (t_audio) g_audio_allocs++;
  std::free(p);
}

using namespace wwe2k14::music;
namespace fs = std::filesystem;

static int g_fail = 0, g_checks = 0;
#define CHECK(cond, ...)                                              \
  do {                                                                \
    ++g_checks;                                                       \
    if (!(cond)) {                                                    \
      ++g_fail;                                                       \
      std::printf("FAIL %s:%d  %s  ", __FILE__, __LINE__, #cond);     \
      std::printf(__VA_ARGS__);                                       \
      std::printf("\n");                                              \
    }                                                                 \
  } while (0)

constexpr uint32_t N = 256, CH = 6;
static const double kPi = 3.14159265358979323846;

static float Swap(float v) {
  uint32_t u;
  std::memcpy(&u, &v, 4);
  u = __builtin_bswap32(u);
  std::memcpy(&v, &u, 4);
  return v;
}

// ---- synthetic songs
static void WriteWav(const fs::path& p, uint32_t rate, double seconds, double freq, double amp, bool ramp = false) {
  const uint32_t frames = uint32_t(seconds * rate);
  std::ofstream f(p, std::ios::binary);
  auto w32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
  auto w16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
  f.write("RIFF", 4), w32(36 + frames * 4), f.write("WAVE", 4), f.write("fmt ", 4), w32(16), w16(1), w16(2), w32(rate), w32(rate * 4),
      w16(4), w16(16), f.write("data", 4), w32(frames * 4);
  for (uint32_t i = 0; i < frames; ++i) {
    const double t = double(i) / rate;
    double l = amp * std::sin(2 * kPi * freq * t), r = 0.5 * l;
    if (ramp) l = amp * (double(i) / frames), r = -l;  // a ramp: every sample unique (continuity checks)
    w16(uint16_t(int16_t(std::lround(l * 32767.0)))), w16(uint16_t(int16_t(std::lround(r * 32767.0))));
  }
}

// ---- the simulated SDK SubmitFrame + SDL callback
struct Sim {
  // what the tap saw (decoded FL / FR of the latest frame) and what the "device" got after the fold
  float tap_l[N] = {}, tap_r[N] = {}, tap_c[N] = {};
  float out_l[N] = {}, out_r[N] = {};
  float guest_l = 0.0f, guest_r = 0.0f, guest_c = 0.0f;  // constant guest content (to separate it from the song)
  bool mute = false;
  int frames = 0;
  static void TapFn(void* user, const void* frame_be, uint32_t samples, uint32_t, uint32_t) {
    auto* s = static_cast<Sim*>(user);
    const float* f = static_cast<const float*>(frame_be);
    for (uint32_t i = 0; i < samples; ++i) s->tap_l[i] = Swap(f[i]), s->tap_r[i] = Swap(f[samples + i]), s->tap_c[i] = Swap(f[2 * samples + i]);
  }
  void Frame(bool measure_allocs = true) {
    float guest[CH * N], host[CH * N];
    for (uint32_t i = 0; i < N; ++i) {
      guest[i] = Swap(guest_l), guest[N + i] = Swap(guest_r), guest[2 * N + i] = Swap(guest_c);
      for (uint32_t c = 3; c < CH; ++c) guest[c * N + i] = Swap(0.0f);
    }
    std::memcpy(host, guest, sizeof host);  // SubmitFrame: memcpy to the host frame
    if (measure_allocs) t_audio = true;
    rex::perf::audio::Inject(host, N, CH, 48000);  // then the hook,
    rex::perf::audio::Tap(host, N, CH, 48000);     // then the tap,
    t_audio = false;
    for (uint32_t i = 0; i < N; ++i) {  // then the SDL callback: mute / fold
      out_l[i] = mute ? 0.0f : std::fmax(-1.0f, std::fmin(1.0f, Swap(host[i]) + 0.707f * Swap(host[2 * N + i])));
      out_r[i] = mute ? 0.0f : std::fmax(-1.0f, std::fmin(1.0f, Swap(host[N + i]) + 0.707f * Swap(host[2 * N + i])));
    }
    // planes the song must never touch
    for (uint32_t c = 2; c < CH; ++c)
      for (uint32_t i = 0; i < N; ++i) CHECK(host[c * N + i] == guest[c * N + i], "plane %u touched", c);
    ++frames;
  }
  // the song's contribution = the tapped value minus the guest's own
  float AddedL(uint32_t i) const { return tap_l[i] - guest_l; }
  float AddedR(uint32_t i) const { return tap_r[i] - guest_r; }
};

static bool WaitState(MusicPlayer& p, Kind k, State want, Sim& sim, int ms = 3000) {
  for (int t = 0; t < ms; ++t) {
    sim.Frame();
    if (p.GetState(k) == want) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}
static void Settle() { std::this_thread::sleep_for(std::chrono::milliseconds(150)); }  // let the decoder fill the ring

static double Rms(const float* x, size_t n) {
  double s = 0;
  for (size_t i = 0; i < n; ++i) s += double(x[i]) * x[i];
  return std::sqrt(s / n);
}
static int Crossings(const std::vector<float>& x) {
  int c = 0;
  for (size_t i = 1; i < x.size(); ++i)
    if (x[i - 1] < 0 && x[i] >= 0) ++c;
  return c;
}
static bool NextEvent(MusicPlayer& p, EventType t, Event& out, int ms = 2000) {
  for (int i = 0; i < ms; ++i) {
    Event e;
    while (p.PollEvent(e))
      if (e.type == t) return out = e, true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}
static void Drain(MusicPlayer& p) {
  Event e;
  while (p.PollEvent(e)) {}
}

// ---- tests
static void TestHookAlone() {
  std::printf("[hook] the SDK hook alone\n");
  float host[CH * N];
  for (auto& v : host) v = Swap(0.1f);
  CHECK(!rex::perf::audio::Inject(host, N, CH, 48000), "no injector registered must do nothing");
  CHECK(Swap(host[0]) == 0.1f, "frame changed without an injector");
  struct Fake {
    static bool Fn(void* user, float* l, float* r, uint32_t n, uint32_t rate) {
      CHECK(rate == 48000 && n == N, "rate/n");
      for (uint32_t i = 0; i < n; ++i) CHECK(l[i] == 0.0f && r[i] == 0.0f, "scratch not zeroed");
      for (uint32_t i = 0; i < n; ++i) l[i] += 0.25f, r[i] += *static_cast<float*>(user);
      return true;
    }
  };
  float rv = 0.5f, other = 0;
  rex::perf::audio::SetInject(&Fake::Fn, &rv);
  CHECK(rex::perf::audio::Inject(host, N, CH, 48000), "injector must run");
  CHECK(std::fabs(Swap(host[0]) - 0.35f) < 1e-6 && std::fabs(Swap(host[N]) - 0.6f) < 1e-6, "FL/FR add: %f %f", Swap(host[0]), Swap(host[N]));
  for (uint32_t c = 2; c < CH; ++c) CHECK(Swap(host[c * N]) == 0.1f, "plane %u", c);
  rex::perf::audio::SetInject(nullptr, &other);  // wrong owner: ignored
  CHECK(rex::perf::audio::Inject(host, N, CH, 48000), "unregister by a stranger must be ignored");
  rex::perf::audio::SetInject(nullptr, &rv);
  CHECK(!rex::perf::audio::Inject(host, N, CH, 48000), "unregistered");
}

static void TestDriverOrder(const fs::path& driver) {
  std::printf("[order] SDK driver source order: %s\n", driver.string().c_str());
  std::ifstream f(driver);
  std::stringstream ss;
  ss << f.rdbuf();
  const std::string s = ss.str();
  const auto a = s.find("perf::audio::Inject("), b = s.find("perf::audio::Tap("), c = s.find("REXCVAR_GET(audio_mute)");
  CHECK(a != std::string::npos && b != std::string::npos && c != std::string::npos, "markers missing");
  CHECK(a < b && b < c, "Inject must come before Tap before audio_mute (%zu %zu %zu)", a, b, c);
  CHECK(s.find("perf::audio::Tap(output_frame") != std::string::npos, "the tap must read the injected host frame");
}

static void TestTone(const fs::path& dir) {
  std::printf("[tone] song at the right gain in FL / FR, others untouched\n");
  const fs::path w = dir / "tone48.wav";
  WriteWav(w, 48000, 4.0, 1000.0, 0.5);
  MusicPlayer p;
  Sim sim;
  rex::perf::audio::SetTap(&Sim::TapFn, &sim);
  rex::perf::audio::SetInject(&MusicPlayer::InjectThunk, &p);
  sim.guest_l = 0.01f, sim.guest_r = -0.02f, sim.guest_c = 0.05f;
  p.Play(w, Kind::Entrance, 0.5f, 343);
  CHECK(WaitState(p, Kind::Entrance, State::Playing, sim), "never reached Playing");
  Settle();
  Event ev;
  CHECK(NextEvent(p, EventType::Started, ev) && ev.tag == 343 && ev.kind == Kind::Entrance, "Started event");
  for (int i = 0; i < 8; ++i) sim.Frame();  // past the 20 ms fade-in
  std::vector<float> addl, addr;
  for (int f = 0; f < 60; ++f) {
    sim.Frame();
    for (uint32_t i = 0; i < N; ++i) addl.push_back(sim.AddedL(i)), addr.push_back(sim.AddedR(i));
  }
  const double rl = Rms(addl.data(), addl.size()), rr = Rms(addr.data(), addr.size());
  const double expect = 0.5 * 0.5 / std::sqrt(2.0);  // amp 0.5 * volume 0.5 / sqrt 2
  CHECK(std::fabs(rl - expect) < 0.01 * expect, "L rms %.5f expected %.5f", rl, expect);
  CHECK(std::fabs(rr - expect * 0.5) < 0.01 * expect, "R rms %.5f expected %.5f (half level)", rr, expect * 0.5);
  CHECK(Crossings(addl) >= 0, "");
  const double hz = Crossings(addl) / (addl.size() / 48000.0);
  CHECK(std::fabs(hz - 1000.0) < 10.0, "frequency %.1f", hz);
  // the guest's own audio is still there
  CHECK(std::fabs(Rms(sim.tap_c, N) - 0.05) < 1e-6, "centre plane changed");
  // volume changes take effect (ramped)
  p.SetVolume(0.25f);
  for (int i = 0; i < 4; ++i) sim.Frame();
  std::vector<float> a2;
  for (int f = 0; f < 20; ++f) {
    sim.Frame();
    for (uint32_t i = 0; i < N; ++i) a2.push_back(sim.AddedL(i));
  }
  CHECK(std::fabs(Rms(a2.data(), a2.size()) - expect * 0.5) < 0.02 * expect, "volume 0.25 rms %.5f", Rms(a2.data(), a2.size()));

  std::printf("[mute] the tap sees the music, the device does not\n");
  sim.mute = true;
  sim.Frame();
  CHECK(Rms(sim.tap_l, N) > 0.05, "tap must still see the music under mute");
  CHECK(Rms(sim.out_l, N) == 0.0 && Rms(sim.out_r, N) == 0.0, "mute must zero the output");
  sim.mute = false;

  std::printf("[stop] fade, then exact silence, Stopped event\n");
  Drain(p);
  CHECK(p.Stop(Kind::Preview) == false, "Stop(Preview) must not stop an Entrance song");
  CHECK(p.Stop(Kind::Entrance), "Stop(Entrance)");
  float prev_peak = 9;
  for (int f = 0; f < 6; ++f) {
    sim.Frame();
    if (f >= 4)
      for (uint32_t i = 0; i < N; ++i) CHECK(sim.AddedL(i) == 0.0f && sim.AddedR(i) == 0.0f, "frame %d not silent after the fade", f);
    float peak = 0;
    for (uint32_t i = 0; i < N; ++i) peak = std::fmax(peak, std::fabs(sim.AddedL(i)));
    if (f >= 1 && f <= 3) CHECK(peak <= prev_peak + 1e-6, "fade-out not decaying");
    prev_peak = peak;
  }
  CHECK(NextEvent(p, EventType::Stopped, ev) && ev.tag == 343, "Stopped event");
  CHECK(p.GetState(Kind::Any) == State::Idle, "state after stop");
  rex::perf::audio::SetTap(nullptr, &sim);
  rex::perf::audio::SetInject(nullptr, &p);
}

static void TestResample(const fs::path& dir) {
  std::printf("[rate] a 44.1 kHz cache file plays at the right pitch\n");
  const fs::path w = dir / "tone441.wav";
  WriteWav(w, 44100, 3.0, 1000.0, 0.5);
  MusicPlayer p;
  Sim sim;
  rex::perf::audio::SetTap(&Sim::TapFn, &sim);
  rex::perf::audio::SetInject(&MusicPlayer::InjectThunk, &p);
  p.Play(w, Kind::Playlist, 1.0f, 7);
  CHECK(WaitState(p, Kind::Playlist, State::Playing, sim), "never reached Playing");
  Settle();
  for (int i = 0; i < 8; ++i) sim.Frame();
  std::vector<float> a;
  for (int f = 0; f < 80; ++f) {
    sim.Frame();
    for (uint32_t i = 0; i < N; ++i) a.push_back(sim.AddedL(i));
  }
  const double hz = Crossings(a) / (a.size() / 48000.0);
  CHECK(std::fabs(hz - 1000.0) < 8.0, "resampled frequency %.1f", hz);
  CHECK(std::fabs(Rms(a.data(), a.size()) - 0.5 / std::sqrt(2.0)) < 0.01, "level %.4f", Rms(a.data(), a.size()));
  p.Stop();
  for (int i = 0; i < 6; ++i) sim.Frame();
  rex::perf::audio::SetTap(nullptr, &sim);
  rex::perf::audio::SetInject(nullptr, &p);
}

static void TestSwitchEndExclusive(const fs::path& dir) {
  std::printf("[switch/end/exclusive]\n");
  const fs::path a = dir / "a1k.wav", b = dir / "b2k.wav", shortw = dir / "short.wav";
  WriteWav(a, 48000, 5.0, 1000.0, 0.4);
  WriteWav(b, 48000, 5.0, 2000.0, 0.4);
  WriteWav(shortw, 48000, 0.3, 1000.0, 0.4);
  MusicPlayer p;
  Sim sim;
  rex::perf::audio::SetTap(&Sim::TapFn, &sim);
  rex::perf::audio::SetInject(&MusicPlayer::InjectThunk, &p);
  auto freq_now = [&](int frames) {
    std::vector<float> v;
    for (int f = 0; f < frames; ++f) {
      sim.Frame();
      for (uint32_t i = 0; i < N; ++i) v.push_back(sim.AddedL(i));
    }
    return Crossings(v) / (v.size() / 48000.0);
  };
  Event ev;
  // entrance A, then entrance B replaces it
  p.Play(a, Kind::Entrance, 1.0f, 1);
  CHECK(WaitState(p, Kind::Entrance, State::Playing, sim), "A");
  Settle();
  for (int i = 0; i < 8; ++i) sim.Frame();
  CHECK(std::fabs(freq_now(40) - 1000.0) < 15.0, "A pitch");
  Drain(p);
  p.Play(b, Kind::Entrance, 1.0f, 2);
  CHECK(NextEvent(p, EventType::Stopped, ev) && ev.tag == 1, "A must be reported stopped when B replaces it");
  CHECK(WaitState(p, Kind::Entrance, State::Playing, sim), "B");
  Settle();
  for (int i = 0; i < 12; ++i) sim.Frame();
  CHECK(std::fabs(freq_now(40) - 2000.0) < 25.0, "B pitch after the switch");
  CHECK(p.CurrentTag() == 2, "current tag");
  // preview is exclusive: it replaces the entrance song; the entrance Stop then does nothing
  Drain(p);
  p.Play(a, Kind::Preview, 1.0f, 99);
  CHECK(NextEvent(p, EventType::Stopped, ev) && ev.kind == Kind::Entrance && ev.tag == 2, "preview must stop the entrance song");
  CHECK(p.CurrentKind() == Kind::Preview, "preview is current");
  CHECK(!p.Stop(Kind::Entrance), "Stop(Entrance) must not touch the preview");
  CHECK(p.GetState(Kind::Entrance) == State::Idle, "entrance state while preview plays");
  CHECK(WaitState(p, Kind::Preview, State::Playing, sim), "preview");
  Drain(p);
  p.Play(b, Kind::Playlist, 1.0f, 5);
  CHECK(NextEvent(p, EventType::Stopped, ev) && ev.kind == Kind::Preview, "a playlist song replaces the preview");
  CHECK(p.CurrentKind() == Kind::Playlist, "playlist current");
  Drain(p);
  // natural end
  p.Play(shortw, Kind::Entrance, 1.0f, 3);
  CHECK(WaitState(p, Kind::Entrance, State::Playing, sim), "short");
  bool ended = false;
  for (int f = 0; f < 200 && !ended; ++f) {
    sim.Frame();
    Event e;
    while (p.PollEvent(e))
      if (e.type == EventType::Ended && e.tag == 3) ended = true;
  }
  CHECK(ended, "Ended event after the song's last sample");
  for (int f = 0; f < 4; ++f) {
    sim.Frame();
    for (uint32_t i = 0; i < N; ++i) CHECK(sim.AddedL(i) == 0.0f, "audio after the end");
  }
  CHECK(p.GetState(Kind::Any) == State::Idle, "idle after the end");
  // a file that cannot be decoded
  Drain(p);
  p.Play(dir / "missing.wav", Kind::Entrance, 1.0f, 4);
  bool failed = false;
  for (int f = 0; f < 400 && !failed; ++f) {  // the audio thread keeps running (it acks the request), as in the game
    sim.Frame();
    Event e;
    while (p.PollEvent(e))
      if (e.type == EventType::Failed && e.tag == 4) failed = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  CHECK(failed, "Failed event for a missing file");
  CHECK(p.GetState(Kind::Entrance) == State::Idle, "idle after a failure");
  rex::perf::audio::SetTap(nullptr, &sim);
  rex::perf::audio::SetInject(nullptr, &p);
}

static void TestPauseContinuity(const fs::path& dir) {
  std::printf("[pause] no Mix calls = the song waits and resumes sample-exact\n");
  const fs::path w = dir / "ramp.wav";
  WriteWav(w, 48000, 6.0, 0, 0.8, true);
  MusicPlayer p;
  Sim sim;
  rex::perf::audio::SetTap(&Sim::TapFn, &sim);
  rex::perf::audio::SetInject(&MusicPlayer::InjectThunk, &p);
  p.Play(w, Kind::Entrance, 1.0f, 8);
  CHECK(WaitState(p, Kind::Entrance, State::Playing, sim), "ramp");
  Settle();
  // reference: the whole file as the player's decoder reads it
  std::vector<float> ref;
  {
    To48k d(std::make_unique<WavPcmSource>());
  }
  auto src = std::make_unique<WavPcmSource>();
  std::string err;
  CHECK(src->Open(w, err), "open ref %s", err.c_str());
  To48k dec(std::move(src));
  std::vector<float> buf(4096 * 2);
  for (size_t n; (n = dec.Read(buf.data(), 4096)) > 0;)
    for (size_t i = 0; i < n; ++i) ref.push_back(buf[i * 2]);
  std::vector<float> got;
  const uint64_t fp0 = p.FramesPlayed();  // frames consumed before the first collected one = its index in the source
  for (int f = 0; f < 30; ++f) {
    sim.Frame();
    for (uint32_t i = 0; i < N; ++i) got.push_back(sim.AddedL(i));
  }
  const uint64_t before = p.FramesPlayed();
  std::this_thread::sleep_for(std::chrono::milliseconds(120));  // the audio system is paused: nobody calls Mix
  CHECK(p.FramesPlayed() == before, "the song advanced while paused");
  for (int f = 0; f < 30; ++f) {
    sim.Frame();
    for (uint32_t i = 0; i < N; ++i) got.push_back(sim.AddedL(i));
  }
  const size_t off = size_t(fp0);
  size_t bad = 0;
  for (size_t k = 2000; k < got.size() && off + k < ref.size(); ++k)  // past the 20 ms fade-in
    if (std::fabs(double(ref[off + k]) - got[k]) > 1e-6) ++bad;
  CHECK(bad == 0, "%zu samples differ from the source across the pause (offset %zu)", bad, off);
  p.Stop();
  for (int i = 0; i < 6; ++i) sim.Frame();
  rex::perf::audio::SetTap(nullptr, &sim);
  rex::perf::audio::SetInject(nullptr, &p);
}

static void TestChain(const fs::path& dir) {
  std::printf("[chain] pass-through by default, a chain gets the block and a reset per song\n");
  const fs::path w = dir / "tone48.wav";
  MusicPlayer p;
  Sim sim;
  rex::perf::audio::SetTap(&Sim::TapFn, &sim);
  rex::perf::audio::SetInject(&MusicPlayer::InjectThunk, &p);
  struct St {
    int resets = 0;
    long blocks = 0;
  } st;
  Chain half;
  half.user = &st;
  half.process = [](void* u, float* l, float* r, uint32_t n) {
    static_cast<St*>(u)->blocks++;
    for (uint32_t i = 0; i < n; ++i) l[i] *= 0.5f, r[i] *= 0.5f;
  };
  half.reset = [](void* u) { static_cast<St*>(u)->resets++; };
  p.SetChain(&half);
  p.Play(w, Kind::Entrance, 1.0f, 1);
  CHECK(WaitState(p, Kind::Entrance, State::Playing, sim), "chain song");
  Settle();
  for (int i = 0; i < 8; ++i) sim.Frame();
  std::vector<float> v;
  for (int f = 0; f < 40; ++f) {
    sim.Frame();
    for (uint32_t i = 0; i < N; ++i) v.push_back(sim.AddedL(i));
  }
  CHECK(std::fabs(Rms(v.data(), v.size()) - 0.5 * 0.5 / std::sqrt(2.0)) < 0.004, "chain halved rms %.4f", Rms(v.data(), v.size()));
  CHECK(st.blocks > 40 && st.resets >= 1, "chain blocks=%ld resets=%d", st.blocks, st.resets);
  p.Stop();
  for (int i = 0; i < 6; ++i) sim.Frame();
  p.SetChain(nullptr);
  rex::perf::audio::SetTap(nullptr, &sim);
  rex::perf::audio::SetInject(nullptr, &p);
}

static void TestPauseApi(const fs::path& dir) {
  std::printf("[Pause(bool)/Start(token)] lane 282's PlayerIf shape; coalescing\n");
  const fs::path w = dir / "ramp.wav";
  MusicPlayer p;
  Sim sim;
  rex::perf::audio::SetTap(&Sim::TapFn, &sim);
  rex::perf::audio::SetInject(&MusicPlayer::InjectThunk, &p);
  // coalescing: five Starts in a row, only the last is opened (the first four are superseded before they open)
  for (uint32_t tok = 1; tok <= 4; ++tok) p.Start(Kind::Playlist, w, tok, 1.0f);
  p.Start(Kind::Playlist, dir / "tone48.wav", 5, 1.0f);
  CHECK(WaitState(p, Kind::Playlist, State::Playing, sim), "last Start must play");
  CHECK(p.CurrentTag() == 5, "token of the last Start");
  Settle();
  for (int i = 0; i < 12; ++i) sim.Frame();
  const uint64_t a = p.FramesPlayed();
  p.Pause(true);
  for (int i = 0; i < 8; ++i) sim.Frame();  // fade consumes 960 frames (< 8 blocks)
  const uint64_t b = p.FramesPlayed();
  CHECK(b > a && b - a <= 1100, "pause fade consumed %llu frames", (unsigned long long)(b - a));
  for (int i = 0; i < 20; ++i) {
    sim.Frame();
    for (uint32_t k = 0; k < N; ++k) CHECK(sim.AddedL(k) == 0.0f, "audio while paused");
  }
  CHECK(p.FramesPlayed() == b, "the song advanced while paused");
  p.Pause(false);
  float peak = 0;
  for (int i = 0; i < 6; ++i) {
    sim.Frame();
    for (uint32_t k = 0; k < N; ++k) peak = std::fmax(peak, std::fabs(sim.AddedL(k)));
  }
  CHECK(peak > 0.1f, "resume must play again (peak %.3f)", peak);
  p.Stop();
  for (int i = 0; i < 6; ++i) sim.Frame();
  rex::perf::audio::SetTap(nullptr, &sim);
  rex::perf::audio::SetInject(nullptr, &p);
}

static void TestStopRule() {
  std::printf("[stop rule] cvar OFF is exactly the old rule; ON stops by the start sequence\n");
  // OFF (mix_on = false): seq != 9 stops, even when the song started in seq 5; start_seq is ignored
  CHECK(EntranceShouldStop(false, true, 5, 5, true, 0), "OFF: seq 5 (start 5) must stop (old rule: seq != 9)");
  CHECK(!EntranceShouldStop(false, true, 9, 5, true, 0), "OFF: seq 9 keeps playing");
  CHECK(EntranceShouldStop(false, true, 9, 9, true, 7), "OFF: the match clock running stops");
  CHECK(EntranceShouldStop(false, false, 0, 9, false, 0), "OFF: unreadable sequence stops (old rule)");
  // ON
  CHECK(!EntranceShouldStop(true, true, 5, 5, true, 12345), "ON: started in 5, still 5: plays (clock ignored outside seq 9)");
  CHECK(EntranceShouldStop(true, true, 6, 5, true, 0), "ON: left the start sequence");
  CHECK(!EntranceShouldStop(true, true, 9, 9, true, 0), "ON: entrance in 9 plays");
  CHECK(EntranceShouldStop(true, true, 9, 9, true, 3), "ON: match clock stops a seq-9 song");
  CHECK(EntranceShouldStop(true, true, 9, -1, true, 3) && !EntranceShouldStop(true, true, 9, -1, true, 0), "ON, start unknown: old rule");
  CHECK(EntranceShouldStop(true, true, 5, -1, true, 0), "ON, start unknown, seq 5: old rule stops");
}

static void TestHighRates(const fs::path& dir) {
  std::printf("[rates] 96 / 176.4 / 192 / 352.8 / 384 kHz sources: no crash, right pitch and length\n");
  for (uint32_t rate : {96000u, 176400u, 192000u, 352800u, 384000u}) {
    const fs::path w = dir / ("hi" + std::to_string(rate) + ".wav");
    WriteWav(w, rate, 1.0, 1000.0, 0.5);
    auto src = std::make_unique<WavPcmSource>();
    std::string err;
    CHECK(src->Open(w, err), "open %u", rate);
    To48k d(std::move(src));
    std::vector<float> all, buf(4096 * 2);
    for (size_t n; (n = d.Read(buf.data(), 4096)) > 0;)
      for (size_t i = 0; i < n; ++i) all.push_back(buf[i * 2]);
    CHECK(all.size() > 47000 && all.size() < 48100, "rate %u: %zu output frames (48000 expected)", rate, all.size());
    if (all.size() > 30000) {
      std::vector<float> mid(all.begin() + 2000, all.begin() + 2000 + 24000);
      const double hz = Crossings(mid) / 0.5;
      CHECK(std::fabs(hz - 1000.0) < 8.0, "rate %u pitch %.1f", rate, hz);
      CHECK(std::fabs(Rms(mid.data(), mid.size()) - 0.5 / std::sqrt(2.0)) < 0.02, "rate %u level %.3f", rate, Rms(mid.data(), mid.size()));
    }
  }
}

static void TestFadeFixes(const fs::path& dir) {
  std::printf("[fades] stop/start while paused, tail kept across successive switches, pause while loading\n");
  const fs::path a = dir / "tone48.wav", b = dir / "b2k.wav";
  MusicPlayer p;
  Sim sim;
  rex::perf::audio::SetTap(&Sim::TapFn, &sim);
  rex::perf::audio::SetInject(&MusicPlayer::InjectThunk, &p);
  auto peak_of_frame = [&] {
    sim.Frame();
    float pk = 0;
    for (uint32_t i = 0; i < N; ++i) pk = std::fmax(pk, std::fabs(sim.AddedL(i)));
    return pk;
  };
  // (a) Stop while paused must not burst
  p.Start(Kind::Entrance, a, 1, 1.0f);
  CHECK(WaitState(p, Kind::Entrance, State::Playing, sim), "A playing");
  Settle();
  for (int i = 0; i < 12; ++i) sim.Frame();
  p.Pause(true);
  for (int i = 0; i < 8; ++i) sim.Frame();  // fade done, held
  CHECK(peak_of_frame() == 0.0f, "held");
  p.Stop(Kind::Entrance);
  float burst = 0;
  for (int i = 0; i < 6; ++i) burst = std::fmax(burst, peak_of_frame());
  CHECK(burst == 0.0f, "Stop while paused burst %.3f", burst);
  // (a2) Start while paused: the paused old voice adds no tail
  p.Start(Kind::Entrance, a, 2, 1.0f);
  CHECK(WaitState(p, Kind::Entrance, State::Playing, sim), "A2 playing");
  Settle();
  for (int i = 0; i < 12; ++i) sim.Frame();
  p.Pause(true);
  for (int i = 0; i < 8; ++i) sim.Frame();
  p.Start(Kind::Entrance, b, 3, 1.0f);
  const float first = peak_of_frame();  // B is still loading (or fading in): nothing from the paused old song
  CHECK(first < 0.15f, "Start while paused: old voice burst %.3f", first);
  CHECK(WaitState(p, Kind::Entrance, State::Playing, sim), "B playing");
  p.Stop();
  for (int i = 0; i < 6; ++i) sim.Frame();
  // (b) Stop then Start in successive blocks: the old song's unplayed tail continues
  p.Start(Kind::Entrance, a, 4, 1.0f);
  CHECK(WaitState(p, Kind::Entrance, State::Playing, sim), "A4 playing");
  Settle();
  for (int i = 0; i < 12; ++i) sim.Frame();
  p.Stop();
  sim.Frame();  // 256 of the 960 tail frames
  p.Start(Kind::Entrance, b, 5, 1.0f);
  const float tail = peak_of_frame();  // the next 256: the rest of A's fade (>= 0.4 * 0.52 ~ 0.2), B is loading
  CHECK(tail > 0.12f, "the old tail was dropped by the next Start (peak %.3f)", tail);
  p.Stop();
  for (int i = 0; i < 8; ++i) sim.Frame();
  // (c) Pause(true) while Loading holds the start
  Drain(p);
  p.Start(Kind::Entrance, a, 6, 1.0f);
  p.Pause(true);
  float leak = 0;
  for (int i = 0; i < 300; ++i) {
    leak = std::fmax(leak, peak_of_frame());
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  const uint64_t fp = p.FramesPlayed();
  CHECK(leak == 0.0f, "Pause while Loading leaked %.3f", leak);
  p.Pause(false);
  float after = 0;
  for (int i = 0; i < 12; ++i) after = std::fmax(after, peak_of_frame());
  CHECK(after > 0.1f && p.FramesPlayed() > fp, "resume after a held start must play (peak %.3f)", after);
  p.Stop();
  for (int i = 0; i < 8; ++i) sim.Frame();
  rex::perf::audio::SetTap(nullptr, &sim);
  rex::perf::audio::SetInject(nullptr, &p);
}

// ---- lane 298: arena effects (src\music_arena_fx.h)
// Stereo test songs, normalised to the cache's target loudness (-17.5 LUFS), interleaved.
static std::vector<float> NormalisedTo(std::vector<float> x, double lufs) {
  KMeter m;
  std::vector<float> l(x.size() / 2), r(x.size() / 2);
  for (size_t i = 0; i < l.size(); ++i) l[i] = x[2 * i], r[i] = x[2 * i + 1];
  m.Add(l.data(), r.data(), l.size());
  const float g = float(std::pow(10.0, (lufs - m.Lufs()) / 20.0));
  for (auto& v : x) v *= g;
  return x;
}
// lp_hz < 0: plain pink noise (flat per octave); else programme-shaped pink noise (the calibration reference's kind)
static std::vector<float> MakeNoiseSong(double sec, uint32_t seed, double lp_hz) {
  std::vector<float> x(size_t(sec * kOutRate) * 2);
  if (lp_hz < 0) {
    PinkNoise pn(seed);
    for (size_t i = 0; i < x.size() / 2; ++i) x[2 * i] = pn.Next(0), x[2 * i + 1] = pn.Next(1);
  } else {
    ProgramNoise pn(seed, lp_hz);
    for (size_t i = 0; i < x.size() / 2; ++i) x[2 * i] = pn.Next(0), x[2 * i + 1] = pn.Next(1);
  }
  return NormalisedTo(x, -17.5);
}
// Music-like: bass, saw chords, a lead, a "vocal" (harmonics under two formants), kick, snare, hats and a bright shimmer,
// with the dynamics of a song. Full-band like a mix (the first version, bass + chords only, was darker than any real mix).
static std::vector<float> MakeMusicSong(double sec, float bright = 1.0f, float bass_gain = 1.0f) {
  const size_t n = size_t(sec * kOutRate);
  std::vector<float> x(n * 2, 0.0f);
  uint32_t s = 12345;
  auto white = [&] {
    s = s * 1664525u + 1013904223u;
    return double(int32_t(s)) / 2147483648.0;
  };
  const double bass[4] = {55.0, 65.41, 73.42, 49.0}, lead[4] = {440.0, 523.25, 587.33, 392.0};
  double hat_lp = 0, ph_b = 0, ph_l = 0, ph_k = 0, c1 = 0, c2 = 0, c3 = 0, ph_v = 0, sn_lp = 0, sh_lp = 0;
  auto saw = [](double ph) { return float(2.0 * (ph - std::floor(ph)) - 1.0); };
  for (size_t i = 0; i < n; ++i) {
    const double t = double(i) / kOutRate;
    const int beat = int(t * 2.0), note = (beat / 2) % 4;
    const double bt = std::fmod(t * 2.0, 1.0);  // 120 bpm: phase within the beat
    const double f0 = 220.0 * (1 + note * 0.1);
    ph_b += bass[note] / kOutRate, ph_l += lead[note] / kOutRate, ph_v += f0 / kOutRate;
    c1 += f0 / kOutRate, c2 += 1.26 * f0 / kOutRate, c3 += 1.5 * f0 / kOutRate;
    const float b = 0.50f * bass_gain * saw(ph_b) * float(std::exp(-bt * 2.0));
    const float pad = 0.10f * (saw(c1) + saw(c2) + saw(c3));
    const float ld = 0.12f * saw(ph_l) * float(std::exp(-bt * 3.0));
    ph_k += (60.0 + 120.0 * std::exp(-bt * 20.0)) / kOutRate;
    const float kick = 0.8f * bass_gain * float(std::sin(2 * kPi * ph_k) * std::exp(-bt * 8.0));
    // vocal: harmonics of f0 weighted by two formants (700 Hz, 1800 Hz) and a 1/h tilt
    double voc = 0;
    for (int h = 1; h <= 24; ++h) {
      const double fh = f0 * h;
      const double wt = std::exp(-0.5 * std::pow((fh - 700.0) / 250.0, 2)) + 0.6 * std::exp(-0.5 * std::pow((fh - 1800.0) / 400.0, 2)) + 0.15 / h;
      voc += wt * std::sin(2 * kPi * ph_v * h) / h;
    }
    const float vocal = 0.35f * float(voc);
    const double w = white();
    hat_lp += 0.4 * (w - hat_lp);
    const float hat = bright * 0.14f * float(w - hat_lp) * float(std::exp(-std::fmod(bt * 2, 1.0) * 12.0));
    sn_lp += 0.3 * (w - sn_lp);
    const float snare = (beat & 1) ? 0.45f * float(sn_lp) * float(std::exp(-bt * 9.0)) : 0.0f;
    sh_lp += 0.5 * (w - sh_lp);
    const float shim = bright * 0.03f * float(w - sh_lp);
    x[2 * i] = b + pad + ld + vocal + kick + hat + snare + shim;
    x[2 * i + 1] = b + 0.8f * pad + 1.1f * ld + vocal + kick - hat + snare - shim;
  }
  return NormalisedTo(x, -17.5);
}

// Run `x` (interleaved) through the player's chain (blocks of 256, planar, like the audio thread) and return the
// K-weighted loudness of the result after `skip_s` seconds.
static double ChainLufs(ArenaFx& fx, const std::vector<float>& x, double skip_s) {
  const Chain* c = fx.AsChain();
  c->reset(c->user);
  KMeter m;
  float l[N], r[N];
  const size_t blocks = x.size() / 2 / N, skip = size_t(skip_s * kOutRate / N);
  for (size_t b = 0; b < blocks; ++b) {
    for (uint32_t i = 0; i < N; ++i) l[i] = x[(b * N + i) * 2], r[i] = x[(b * N + i) * 2 + 1];
    c->process(c->user, l, r, N);
    if (b >= skip) m.Add(l, r, N);
  }
  return m.Lufs();
}
static double DryLufs(const std::vector<float>& x, double skip_s) {
  KMeter m;
  std::vector<float> l(x.size() / 2), r(x.size() / 2);
  for (size_t i = 0; i < l.size(); ++i) l[i] = x[2 * i], r[i] = x[2 * i + 1];
  const size_t skip = size_t(skip_s * kOutRate);
  m.Add(l.data() + skip, r.data() + skip, l.size() - skip);
  return m.Lufs();
}
// The bar (S3 of the lane 298 review): a cached song through OUR chain must come out exactly as a stock theme comes out of
// the game's chain. A cached song C = S - 11 dB (S: what a stock theme's stream would be; -11 dB = bus -9 + master -2), so
// our chain on C must equal MusicChain at 0 dB output gain on S = C raised by 11 dB, for every spectrum.
constexpr double kStockTolLu = 0.01, kStockTolRel = 2e-3;

static std::vector<float> RunChain(const Chain* c, const std::vector<float>& x) {
  c->reset(c->user);
  std::vector<float> out(x.size());
  float l[N], r[N];
  const size_t blocks = x.size() / 2 / N;
  for (size_t b = 0; b < blocks; ++b) {
    for (uint32_t i = 0; i < N; ++i) l[i] = x[(b * N + i) * 2], r[i] = x[(b * N + i) * 2 + 1];
    c->process(c->user, l, r, N);
    for (uint32_t i = 0; i < N; ++i) out[(b * N + i) * 2] = l[i], out[(b * N + i) * 2 + 1] = r[i];
  }
  return out;
}
struct StockMatch {
  double lu = 0, rel = 0;  // loudness difference (ours - stock transform), max sample error relative to the stock peak
};
static StockMatch CompareToStock(ArenaFx& fx, wwe::musicchain::MusicChain& stock, double undo_db, const std::vector<float>& cached) {
  std::vector<float> s_in(cached.size());
  const float up = float(std::pow(10.0, undo_db / 20.0));
  for (size_t i = 0; i < cached.size(); ++i) s_in[i] = cached[i] * up;
  stock.reset();
  std::vector<float> ref = s_in;
  stock.process(ref.data(), int(ref.size() / 2));
  const std::vector<float> ours = RunChain(fx.AsChain(), cached);
  const size_t skip = size_t(1.0 * kOutRate) * 2;  // our 20 ms gain ramp (after a gain change) is long over
  double peak = 1e-9, err = 0;
  KMeter a, b;
  std::vector<float> al, ar, bl, br;
  for (size_t i = skip; i < ref.size(); i += 2) {
    peak = std::max<double>(peak, std::fabs(ref[i]));
    err = std::max<double>(err, std::fabs(double(ref[i]) - double(ours[i])));
    al.push_back(ours[i]), ar.push_back(ours[i + 1]), bl.push_back(ref[i]), br.push_back(ref[i + 1]);
  }
  a.Add(al.data(), ar.data(), al.size());
  b.Add(bl.data(), br.data(), bl.size());
  return {a.Lufs() - b.Lufs(), err / peak};
}

static void TestArenaFxLoudness(ArenaFx& fx) {
  const auto& rp = fx.report();
  std::printf("[arena fx] a cached (-17.5 LUFS) song through our chain == a stock theme through the game's chain\n");
  std::printf("  staging: banks=%d  chain net dry %.2f dB  undo bus+master %+.2f dB  trim %+.1f  => user gain %+.2f dB   prepare %.1f ms  tail %u frames (%.2f s)\n",
              rp.from_banks, rp.net_dry_db, rp.undo_staging_db, rp.trim_db, rp.user_gain_db, rp.prepare_ms, rp.tail_frames, rp.tail_frames / 48000.0);
  CHECK(std::fabs(rp.user_gain_db - (rp.undo_staging_db + rp.trim_db)) < 1e-6, "gain must be undo staging + trim only");
  static wwe::musicchain::MusicChain stock;  // the game's chain as lane 287 models it, at 0 dB output gain (no user gain)
  stock.set_output_gain_db(0.0f);
  stock.prepare(fx.chain().params(), double(kOutRate));
  struct Sig {
    const char* name;
    std::vector<float> x;
  } sigs[] = {
      {"programme noise (other seed)", MakeNoiseSong(8.0, 777, kProgramLpHz)},
      {"music-like (bass, chords, kick, hats)", MakeMusicSong(8.0)},
      {"plain pink noise", MakeNoiseSong(8.0, 5, -1.0)},
      {"bright music-like", MakeMusicSong(8.0, 2.5f, 0.6f)},
  };
  for (auto& s : sigs) {
    const double dry = DryLufs(s.x, 2.0), wet = ChainLufs(fx, s.x, 2.0);
    const StockMatch m = CompareToStock(fx, stock, rp.undo_staging_db, s.x);
    std::printf("  %-40s dry %.2f -> ours %.2f LUFS (%+.2f LU, the stock room's level on this spectrum);  vs stock transform: %+.4f LU, max err %.1e of peak\n",
                s.name, dry, wet, wet - dry, m.lu, m.rel);
    CHECK(std::fabs(dry + 17.5) < 0.15, "test signal not near -17.5 (%.2f)", dry);
    CHECK(std::fabs(m.lu) <= kStockTolLu, "%s: %.4f LU from the stock transform", s.name, m.lu);
    CHECK(m.rel <= kStockTolRel, "%s: sample error %.1e of peak", s.name, m.rel);
  }
  // The known bug: the chain's own bus -9 / master -2 staging applied a SECOND time on a song that already carries it =
  // "just run MusicChain" at 0 dB. It must FAIL the bar by a mile.
  const float good = rp.user_gain_db;
  fx.chain().set_output_gain_db(0.0f);
  for (auto& s : sigs) {
    const StockMatch m = CompareToStock(fx, stock, rp.undo_staging_db, s.x);
    std::printf("  KNOWN BUG (double staging) %-40s %+.2f LU from the stock transform, max err %.2f of peak\n", s.name, m.lu, m.rel);
    CHECK(std::fabs(m.lu) > kStockTolLu, "the double-gain mistake must FAIL the bar (%.4f LU)", m.lu);
    CHECK(std::fabs(m.lu) > 5.0 && m.rel > kStockTolRel, "the double-gain mistake should be several LU off (%.2f)", m.lu);
  }
  fx.chain().set_output_gain_db(good);
  const std::vector<float> settle(size_t(kOutRate) * 2, 0.0f);  // let the 20 ms gain ramp settle before the next test
  RunChain(fx.AsChain(), settle);
  // Information only (no bar): MeasureDeltaDb is the loudness change of the chain on the programme-noise reference. A custom song
  // is NOT loudness-compensated against it any more (it lands where a stock theme of its spectrum lands).
  std::printf("  info: chain loudness change on the programme-noise reference %+.2f LU; sensitivity (noise low-pass Hz):", fx.MeasureDeltaDb());
  for (double lp : {-1.0, 800.0, 1500.0, 3000.0, 6000.0, 12000.0}) {
    const auto n = MakeNoiseSong(8.0, 99, lp);
    std::printf("  %s %+.2f", lp < 0 ? "pink" : std::to_string(int(lp)).c_str(), ChainLufs(fx, n, 2.0) - DryLufs(n, 2.0));
  }
  std::printf(" LU\n");
}

static void TestArenaFxPlayer(const fs::path& dir, ArenaFx& fx) {
  std::printf("[arena fx] in the player: wiring, ring-out tail after Stop, exact silence after it, 0 allocations, CPU\n");
  const fs::path w = dir / "tone48.wav";
  MusicPlayer p;
  Sim sim;
  rex::perf::audio::SetTap(&Sim::TapFn, &sim);
  rex::perf::audio::SetInject(&MusicPlayer::InjectThunk, &p);
  p.SetChain(fx.AsChain());
  p.Play(w, Kind::Entrance, 1.0f, 7);
  CHECK(WaitState(p, Kind::Entrance, State::Playing, sim), "arena song");
  Settle();
  for (int i = 0; i < 40; ++i) sim.Frame();  // let the fade-in and the reverb build up
  // steady-state rms vs the same chain run directly on the same tone (a linear system: they must agree)
  std::vector<float> got;
  for (int f = 0; f < 60; ++f) {
    sim.Frame();
    for (uint32_t i = 0; i < N; ++i) got.push_back(sim.AddedL(i));
  }
  {
    std::vector<float> tone(size_t(4.0 * kOutRate) * 2);
    for (size_t i = 0; i < tone.size() / 2; ++i) {
      const float v = float(std::sin(2 * kPi * 1000.0 * double(i) / kOutRate));
      tone[2 * i] = 0.5f * v, tone[2 * i + 1] = 0.25f * v;
    }
    const Chain* c = fx.AsChain();
    c->reset(c->user);
    std::vector<float> ref;
    float l[N], r[N];
    for (size_t b = 0; b < tone.size() / 2 / N; ++b) {
      for (uint32_t i = 0; i < N; ++i) l[i] = tone[(b * N + i) * 2], r[i] = tone[(b * N + i) * 2 + 1];
      c->process(c->user, l, r, N);
      if (b >= 100) ref.insert(ref.end(), l, l + N);
    }
    const double rg = Rms(got.data(), got.size()), rr = Rms(ref.data(), ref.size());
    std::printf("  1 kHz tone: player+chain rms %.5f, chain alone %.5f (ratio %.3f)\n", rg, rr, rg / rr);
    CHECK(std::fabs(rg / rr - 1.0) < 0.06, "player output differs from the chain on the same tone (%.3f)", rg / rr);
    c->reset(c->user);
  }
  // Stop: 20 ms fade, then the reverb rings out (energy decays ~RT60) and ends in exact silence
  p.Stop(Kind::Entrance);
  std::vector<float> after;
  const int frames_total = int((fx.report().tail_frames + 6000) / N) + 8;
  for (int f = 0; f < frames_total; ++f) {
    sim.Frame();
    for (uint32_t i = 0; i < N; ++i) after.push_back(sim.AddedL(i));
  }
  auto win = [&](double t0, double t1) {
    const size_t a = size_t(t0 * kOutRate), b = std::min(after.size(), size_t(t1 * kOutRate));
    return 20.0 * std::log10(Rms(after.data() + a, b - a) + 1e-12);
  };
  const double e_a = win(0.10, 0.20), e_b = win(0.60, 0.70), e_c = win(1.10, 1.20), e_d = win(1.60, 1.70);
  const double slope = (e_d - e_a) / 1.5, rt60 = -60.0 / slope;
  std::printf("  tail after Stop (dBFS rms): 0.10-0.20s %.1f  0.60-0.70s %.1f  1.10-1.20s %.1f  1.60-1.70s %.1f  (slope %.1f dB/s, RT60 %.2f s)\n",
              e_a, e_b, e_c, e_d, slope, rt60);
  CHECK(e_a > -70.0, "the reverb tail is missing after Stop (%.1f dBFS)", e_a);
  CHECK(e_a > e_b && e_b > e_c && e_c > e_d, "the tail does not decay monotonically");
  CHECK(rt60 > 1.4 && rt60 < 3.2, "tail RT60 %.2f s (bank decay 2.3 s)", rt60);
  float late = 0;
  for (size_t i = size_t(fx.report().tail_frames) + 6 * N; i < after.size(); ++i) late = std::fmax(late, std::fabs(after[i]));
  CHECK(late == 0.0f, "audio after the tail window (%g)", late);
  // Owners: the Playlist (menu / user soundtrack) stays dry, Preview gets the room like Entrance
  {
    auto steady_rms = [&](Kind k, uint32_t tag) {
      p.Play(w, k, 1.0f, tag);
      WaitState(p, k, State::Playing, sim);
      Settle();
      for (int i = 0; i < 50; ++i) sim.Frame();
      std::vector<float> v;
      for (int f = 0; f < 40; ++f) {
        sim.Frame();
        for (uint32_t i = 0; i < N; ++i) v.push_back(sim.AddedL(i));
      }
      const double r = Rms(v.data(), v.size());
      p.Stop();
      return r;
    };
    const double dry = 0.5 / std::sqrt(2.0);
    const double r_pl = steady_rms(Kind::Playlist, 20);
    CHECK(std::fabs(r_pl - dry) < 0.01 * dry, "Playlist must stay dry: rms %.5f (dry %.5f)", r_pl, dry);
    for (int f = 0; f < 6; ++f) sim.Frame();
    float after_pl = 0;
    for (int f = 0; f < 6; ++f) {
      sim.Frame();
      for (uint32_t i = 0; i < N; ++i) after_pl = std::fmax(after_pl, std::fabs(sim.AddedL(i)));
    }
    CHECK(after_pl == 0.0f, "a dry Playlist song must leave no reverb tail (%g)", after_pl);
    const double r_en = steady_rms(Kind::Entrance, 21), r_pr = steady_rms(Kind::Preview, 22);
    std::printf("  owners at 1 kHz: playlist rms %.4f (dry %.4f), entrance %.4f, preview %.4f\n", r_pl, dry, r_en, r_pr);
    CHECK(std::fabs(r_en - dry) > 0.02 * dry, "Entrance must go through the chain (rms %.5f)", r_en);
    CHECK(std::fabs(r_pr - dry) > 0.02 * dry, "Preview must go through the chain (rms %.5f)", r_pr);  // (the reverb is still building, so no exact match with Entrance)
    for (int f = 0; f < int(fx.report().tail_frames / N) + 40; ++f) sim.Frame();  // let the tail end
  }
  // (1) A Start while a tail rings must not cut it: the chain is reset at a cold start, never during a tail
  {
    struct Cnt {
      ArenaFx* fx;
      int resets = 0;
    } cnt{&fx};
    Chain cw = *fx.AsChain();
    cw.user = &cnt;
    cw.process = [](void* u, float* l, float* r, uint32_t n) {
      auto* c = static_cast<Cnt*>(u);
      c->fx->AsChain()->process(c->fx->AsChain()->user, l, r, n);
    };
    cw.reset = [](void* u) {
      auto* c = static_cast<Cnt*>(u);
      ++c->resets;
      c->fx->AsChain()->reset(c->fx->AsChain()->user);
    };
    p.SetChain(&cw);
    for (int i = 0; i < 4; ++i) sim.Frame();  // the new chain pointer starts cold (one reset)
    const int base = cnt.resets;
    p.Play(w, Kind::Entrance, 1.0f, 8);
    CHECK(WaitState(p, Kind::Entrance, State::Playing, sim), "arena song 2");
    Settle();
    for (int i = 0; i < 20; ++i) sim.Frame();
    CHECK(cnt.resets == base + 1, "a cold song start resets the chain once (%d -> %d)", base, cnt.resets);
    p.Stop();
    for (int i = 0; i < 6; ++i) sim.Frame();  // the tail rings
    const int at_stop = cnt.resets;
    p.Play(w, Kind::Entrance, 1.0f, 9);
    CHECK(WaitState(p, Kind::Entrance, State::Playing, sim), "arena song 3");
    CHECK(cnt.resets == at_stop, "Start while a tail rings must not reset the chain (%d -> %d)", at_stop, cnt.resets);
    p.Stop();
    for (int f = 0; f < int(fx.report().tail_frames / N) + 40; ++f) sim.Frame();
    p.SetChain(fx.AsChain());
    for (int f = 0; f < 4; ++f) sim.Frame();
  }
  // (2) wet -> dry: Entrance replaced by a Playlist song keeps the Entrance's tail ringing (S2 of the review); the dry Playlist
  // song itself is dry
  {
    const fs::path silent = dir / "silent48.wav";
    WriteWav(silent, 48000, 4.0, 1000.0, 0.0);
    p.Play(w, Kind::Entrance, 1.0f, 30);
    CHECK(WaitState(p, Kind::Entrance, State::Playing, sim), "entrance for the switch");
    Settle();
    for (int i = 0; i < 120; ++i) sim.Frame();
    p.Play(silent, Kind::Playlist, 1.0f, 31);
    std::vector<float> after;
    const int frames_total = int((fx.report().tail_frames + 6000) / N) + 8;
    for (int f = 0; f < frames_total; ++f) {
      sim.Frame();
      for (uint32_t i = 0; i < N; ++i) after.push_back(sim.AddedL(i));
    }
    auto win = [&](double t0, double t1) {
      const size_t a0 = size_t(t0 * kOutRate), b0 = std::min(after.size(), size_t(t1 * kOutRate));
      return 20.0 * std::log10(Rms(after.data() + a0, b0 - a0) + 1e-12);
    };
    const double e_a = win(0.10, 0.20), e_c = win(1.10, 1.20);
    std::printf("  Entrance -> Playlist(silent): tail %.1f dBFS at 0.15 s, %.1f at 1.15 s\n", e_a, e_c);
    CHECK(e_a > -70.0 && e_a > e_c, "the Entrance's tail was cut by the dry Playlist song (%.1f, %.1f dBFS)", e_a, e_c);
    float late = 0;
    for (size_t i = size_t(fx.report().tail_frames) + 6 * N; i < after.size(); ++i) late = std::fmax(late, std::fabs(after[i]));
    CHECK(late == 0.0f, "audio after the tail window (%g)", late);
    p.Stop();
    for (int f = 0; f < 8; ++f) sim.Frame();
  }
  // (3) dry -> wet: a Playlist song is dry; the Entrance that replaces it is wet from its first sample
  {
    p.Play(w, Kind::Playlist, 1.0f, 40);
    CHECK(WaitState(p, Kind::Playlist, State::Playing, sim), "playlist for the switch");
    Settle();
    for (int i = 0; i < 40; ++i) sim.Frame();
    std::vector<float> v;
    for (int f = 0; f < 30; ++f) {
      sim.Frame();
      for (uint32_t i = 0; i < N; ++i) v.push_back(sim.AddedL(i));
    }
    const double dry = 0.5 / std::sqrt(2.0), r0 = Rms(v.data(), v.size());
    CHECK(std::fabs(r0 - dry) < 0.01 * dry, "Playlist before the switch must be dry (%.5f)", r0);
    p.Play(w, Kind::Entrance, 1.0f, 41);
    CHECK(WaitState(p, Kind::Entrance, State::Playing, sim), "entrance after the playlist");
    Settle();
    for (int i = 0; i < 60; ++i) sim.Frame();
    v.clear();
    for (int f = 0; f < 30; ++f) {
      sim.Frame();
      for (uint32_t i = 0; i < N; ++i) v.push_back(sim.AddedL(i));
    }
    const double r1 = Rms(v.data(), v.size());
    std::printf("  Playlist -> Entrance: rms %.4f (dry) -> %.4f (wet)\n", r0, r1);
    CHECK(std::fabs(r1 - dry) > 0.02 * dry, "Entrance after a Playlist song must be wet (%.5f)", r1);
    p.Stop();
    for (int f = 0; f < int(fx.report().tail_frames / N) + 40; ++f) sim.Frame();
  }
  // (4) the chain switched off keeps ringing until its tail ends, then the player is plain pass-through
  {
    p.Play(w, Kind::Entrance, 1.0f, 50);
    CHECK(WaitState(p, Kind::Entrance, State::Playing, sim), "entrance for the switch-off");
    Settle();
    for (int i = 0; i < 60; ++i) sim.Frame();
    p.Stop();
    sim.Frame();
    p.SetChain(nullptr);  // --wwe2k14_music_arena_fx flipped off during a tail
    float tailpk = 0;
    for (int f = 0; f < 10; ++f) {
      sim.Frame();
      for (uint32_t i = 0; i < N; ++i) tailpk = std::fmax(tailpk, std::fabs(sim.AddedL(i)));
    }
    CHECK(tailpk > 0.0f, "switching the chain off must let the ringing tail finish");
    for (int f = 0; f < int(fx.report().tail_frames / N) + 40; ++f) sim.Frame();
    p.Play(w, Kind::Entrance, 1.0f, 51);
    CHECK(WaitState(p, Kind::Entrance, State::Playing, sim), "dry entrance");
    Settle();
    for (int i = 0; i < 40; ++i) sim.Frame();
    std::vector<float> v;
    for (int f = 0; f < 30; ++f) {
      sim.Frame();
      for (uint32_t i = 0; i < N; ++i) v.push_back(sim.AddedL(i));
    }
    const double dry = 0.5 / std::sqrt(2.0);
    CHECK(std::fabs(Rms(v.data(), v.size()) - dry) < 0.01 * dry, "chain off: songs must be dry");
    p.Stop();
    for (int f = 0; f < 8; ++f) sim.Frame();
  }
  p.SetChain(nullptr);
  rex::perf::audio::SetTap(nullptr, &sim);
  rex::perf::audio::SetInject(nullptr, &p);
}

static void TestArenaFxWarmup(const fs::path& dir, ArenaFx& fx) {
  std::printf("[arena fx] warm-up: slow set-up runs on the decode thread, the first song waits for it and never starts dry\n");
  struct W {
    std::atomic<bool> done{false};
    std::thread::id tid;
    ArenaFx* fx;
  } st;
  st.fx = &fx;
  const auto t0 = std::chrono::steady_clock::now();
  MusicPlayer p([&st](MusicPlayer& pl) {
    st.tid = std::this_thread::get_id();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));  // stands in for the pck read + chain prepare
    pl.SetChain(st.fx->AsChain());
    st.done = true;
  });
  const double ctor_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  CHECK(ctor_ms < 60.0, "the constructor must not run the warm-up (%.1f ms)", ctor_ms);
  Sim sim;
  rex::perf::audio::SetTap(&Sim::TapFn, &sim);
  rex::perf::audio::SetInject(&MusicPlayer::InjectThunk, &p);
  p.Play(dir / "tone48.wav", Kind::Entrance, 1.0f, 60);
  float dry_leak = 0;
  bool saw_dry = false;
  for (int i = 0; i < 4000; ++i) {
    const bool done_before = st.done.load();
    sim.Frame();
    float pk = 0;
    for (uint32_t k = 0; k < N; ++k) pk = std::fmax(pk, std::fabs(sim.AddedL(k)));
    if (!done_before && pk > 0) saw_dry = true, dry_leak = pk;
    if (p.GetState(Kind::Entrance) == State::Playing) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  CHECK(st.done.load(), "the warm-up never ran");
  CHECK(st.tid != std::this_thread::get_id(), "the warm-up ran on the calling thread");
  CHECK(!saw_dry, "the first song played before the chain was ready (peak %.3f)", dry_leak);
  p.Stop();
  for (int i = 0; i < 8; ++i) sim.Frame();
  p.SetChain(nullptr);
  for (int f = 0; f < int(fx.report().tail_frames / N) + 8; ++f) sim.Frame();
  rex::perf::audio::SetTap(nullptr, &sim);
  rex::perf::audio::SetInject(nullptr, &p);
}

static void TestArenaFxCpu(ArenaFx& fx) {
  // CPU of the chain on the audio thread: 256-frame blocks of music, planar in / out, the way Mix calls it
  const std::vector<float> x = MakeMusicSong(6.0);
  const Chain* c = fx.AsChain();
  c->reset(c->user);
  std::vector<double> us;
  float l[N], r[N];
  const size_t blocks = x.size() / 2 / N;
  for (size_t b = 0; b < blocks; ++b) {
    for (uint32_t i = 0; i < N; ++i) l[i] = x[(b * N + i) * 2], r[i] = x[(b * N + i) * 2 + 1];
    const auto t0 = std::chrono::steady_clock::now();
    c->process(c->user, l, r, N);
    us.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count());
  }
  std::sort(us.begin(), us.end());
  double mean = 0;
  for (double v : us) mean += v;
  mean /= double(us.size());
  const double budget = double(N) / kOutRate * 1e6;
  std::printf("[arena fx] CPU per %u-frame block (chain only): mean %.1f us, median %.1f us, p99 %.1f us, max %.1f us = %.2f %% of the %.0f us block\n",
              N, mean, us[us.size() / 2], us[us.size() * 99 / 100], us.back(), 100.0 * mean / budget, budget);
  CHECK(mean < 0.05 * budget, "chain too expensive: %.1f us per block", mean);
}

#if defined(_WIN32)
static void TestMf(const fs::path& dir) {
  std::printf("[mf] Media Foundation decodes a file through the same player (a WAV here: no encoder needed)\n");
  mf::Install();
  const fs::path w = dir / "mf.wav";
  WriteWav(w, 44100, 2.0, 1000.0, 0.5);
  auto s = mf::Make(w);
  std::string err;
  std::vector<float> all;
  std::thread t([&] {  // the decode thread's apartment rules: do it off the main thread, like the player
    if (!s->Open(w, err)) return;
    To48k d(std::move(s));
    std::vector<float> buf(4096 * 2);
    for (size_t n; (n = d.Read(buf.data(), 4096)) > 0;)
      for (size_t i = 0; i < n; ++i) all.push_back(buf[i * 2]);
  });
  t.join();
  CHECK(all.size() > 90000 && all.size() < 100000, "MF decoded %zu frames (2 s expected)", all.size());
  if (all.size() > 20000) {
    std::vector<float> mid(all.begin() + 5000, all.begin() + 5000 + 48000);
    CHECK(std::fabs(Crossings(mid) - 1000.0) < 10.0, "MF pitch %d", Crossings(mid));
    CHECK(std::fabs(Rms(mid.data(), mid.size()) - 0.5 / std::sqrt(2.0)) < 0.02, "MF level %.3f", Rms(mid.data(), mid.size()));
  }
  // the player with the MF path: a non-WAV extension would go there; force it with a copy named .mp3 (MF sniffs content)
}
#endif

int main(int argc, char** argv) {
  const fs::path dir = argc > 1 ? fs::path(argv[1]) : fs::path("music_player_test");
  const fs::path driver = argc > 2 ? fs::path(argv[2]) : fs::path();
  std::error_code ec;
  fs::create_directories(dir, ec);
  TestHookAlone();
  if (!driver.empty()) TestDriverOrder(driver);
  WriteWav(dir / "tone48.wav", 48000, 4.0, 1000.0, 0.5);
  TestTone(dir);
  TestResample(dir);
  TestSwitchEndExclusive(dir);
  TestPauseContinuity(dir);
  TestChain(dir);
  TestPauseApi(dir);
  TestStopRule();
  TestHighRates(dir);
  TestFadeFixes(dir);
  {  // lane 298: arena effects. The bank is read when the player's own pck is given (argv[3]), else the built-in defaults.
    wwe::musicchain::Params params = wwe::musicchain::defaults();
    bool banks = false;
    if (argc > 3) {
      std::string why;
      banks = wwe::musicchain::load_from_pck(argv[3], params, &why);
      if (!banks) std::printf("[arena fx] pck not read (%s): defaults\n", why.c_str());
    }
    static ArenaFx fx;  // big (reverb arena): not on the stack
    fx.Prepare(params, banks, 0.0f);
    TestArenaFxLoudness(fx);
    TestArenaFxPlayer(dir, fx);
    TestArenaFxWarmup(dir, fx);
    TestArenaFxCpu(fx);
  }
#if defined(_WIN32)
  TestMf(dir);
#endif
  std::printf("\n[alloc] heap allocations / frees on the audio thread (Inject + Mix): %ld\n", g_audio_allocs.load());
  CHECK(g_audio_allocs.load() == 0, "the audio thread allocated");
  std::printf("%s: %d checks, %d failed\n", g_fail ? "FAIL" : "PASS", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
