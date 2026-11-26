// S111 SHOTDIET unit (offline, no game): (1) the lossless WebP writer: a synthetic 2560x1440 picture encoded by src\webp_shot.h and decoded by libwebp comes back byte-equal; (2) the cut trigger's
// state machine (src\cut_shot.h): synthetic (guest frame, cut) sequences give exactly the before / cut / +N frames, repeated guest frames skipped.
// Usage: shot_diet_unit.exe            all cases, exit 0 = all pass
//        shot_diet_unit.exe --plant N  one planted fault (webp: 1 = lossy encode; cut: 11 = repeats not skipped, 12 = no before, 13 = +N in guest frames, 14 = an unread tick's cut dropped;
//                                      policy (src\shot_policy.h): 21 = the phone writes always, 22 = no cap on writes in flight, 23 = a named shot drops at a full gate): the run must FAIL (exit 1)
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "../../src/cut_shot.h"
#include "../../src/shot_policy.h"
#include "../../src/webp_shot.h"
#include "../../third_party/libwebp/src/webp/decode.h"

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../../third_party/stb/stb_image_write.h"

using namespace wwe2k14;
static int g_fails = 0;
#define CHECK(c, ...)                                  \
  do {                                                 \
    if (!(c)) {                                        \
      ++g_fails;                                       \
      std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
      std::printf(__VA_ARGS__);                        \
      std::printf("\n");                               \
    }                                                  \
  } while (0)

static double Ms(std::chrono::steady_clock::time_point a) { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count(); }

// A game-like picture: smooth gradients, hard-edged panels, a stripe pattern and a little sensor-style noise (so it is neither trivially flat nor random).
static std::vector<uint8_t> Synthetic(uint32_t w, uint32_t h) {
  std::vector<uint8_t> px(size_t(w) * h * 4);
  uint32_t s = 12345;
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x) {
      uint8_t* p = &px[(size_t(y) * w + x) * 4];
      int r = int(x * 255 / w), g = int(y * 255 / h), b = int(((x + y) * 255) / (w + h));
      if ((x / 160 + y / 90) % 7 == 0) r = 255 - r, b = (x ^ y) & 255;   // panels with hard edges
      if (y % 48 < 3) g = (x / 4) & 255;                                   // stripes
      s = s * 1664525u + 1013904223u;
      const int n = int((s >> 24) % 5) - 2;                                // +-2 noise
      p[0] = uint8_t(std::max(0, std::min(255, r + n))), p[1] = uint8_t(std::max(0, std::min(255, g + n))), p[2] = uint8_t(std::max(0, std::min(255, b + n)));
      p[3] = 255;   // X8
    }
  return px;
}

static void WebpCases(bool print_timing) {
  const uint32_t w = 2560, h = 1440;
  const std::vector<uint8_t> px = Synthetic(w, h);
  std::vector<uint8_t> file;
  auto t0 = std::chrono::steady_clock::now();
  const bool ok = webp_shot::EncodeLosslessRgbx(px.data(), w, h, size_t(w) * 4, file);
  const double enc_ms = Ms(t0);
  CHECK(ok && !file.empty(), "webp encode failed");
  if (!ok) return;
  CHECK(file.size() > 12 && std::memcmp(file.data(), "RIFF", 4) == 0 && std::memcmp(file.data() + 8, "WEBP", 4) == 0, "not a RIFF WEBP file");
  int dw = 0, dh = 0;
  t0 = std::chrono::steady_clock::now();
  uint8_t* dec = WebPDecodeRGBA(file.data(), file.size(), &dw, &dh);
  const double dec_ms = Ms(t0);
  CHECK(dec && dw == int(w) && dh == int(h), "decode failed or wrong size (%d x %d)", dw, dh);
  if (!dec) return;
  size_t diff = 0, first = size_t(-1);
  for (size_t i = 0; i < px.size(); ++i)
    if (px[i] != dec[i]) {
      ++diff;
      if (first == size_t(-1)) first = i;
    }
  CHECK(diff == 0, "decoded pixels differ from the input: %zu bytes (first at byte %zu: %u vs %u)", diff, first, first == size_t(-1) ? 0u : px[first], first == size_t(-1) ? 0u : dec[first]);
  WebPFree(dec);
  if (print_timing) {
    // the stb PNG writer (the portable one) as the size / time reference; the game's Windows path is WIC (about the same class of effort).
    std::vector<uint8_t> rgb(size_t(w) * h * 3);
    for (size_t i = 0, n = size_t(w) * h; i < n; ++i) std::memcpy(&rgb[i * 3], &px[i * 4], 3);
    int len = 0;
    t0 = std::chrono::steady_clock::now();
    unsigned char* png = stbi_write_png_to_mem(rgb.data(), int(w) * 3, int(w), int(h), 3, &len);
    const double png_ms = Ms(t0);
    std::printf("timing 2560x1440 synthetic: webp effort %d: encode %.0f ms, decode %.0f ms, %zu bytes (raw %zu); stb png: encode %.0f ms, %d bytes\n", webp_shot::kEffort, enc_ms, dec_ms, file.size(), px.size(), png_ms, len);
    STBIW_FREE(png);
    for (int e : {0, 10, 30, 60, 100}) {
      std::vector<uint8_t> f2;
      t0 = std::chrono::steady_clock::now();
      webp_shot::EncodeLosslessRgbx(px.data(), w, h, size_t(w) * 4, f2, e);
      std::printf("  effort %3d: encode %.0f ms, %zu bytes\n", e, Ms(t0), f2.size());
    }
  }
}

// "B13 A14 +17": before / at / after frames of one feed sequence, repeats shown as "r".
struct Step {
  uint64_t frame;
  bool cut;
};
static std::string Run(const std::vector<Step>& seq, uint32_t after, uint32_t* cuts = nullptr) {
  cut_shot::Machine m(after);
  std::string s;
  for (const Step& st : seq) {
    std::vector<cut_shot::Out> out;
    const cut_shot::Fed f = m.Feed(st.frame, st.cut, out);
    if (f.repeat) s += "r ";
    for (const cut_shot::Out& o : out) s += (o.kind == cut_shot::Out::kBefore ? "B" : o.kind == cut_shot::Out::kAt ? "A" : "+") + std::to_string((unsigned long long)o.frame) + " ";
  }
  if (cuts) *cuts = uint32_t(m.Cuts());
  if (!s.empty()) s.pop_back();
  return s;
}

// The host's order (scene_host.cpp QueueCutShot / PollShot): per drawn tick the camera test, then either the picture is read back (Read: the machine gets it with the latch's cut flag) or the tick
// is unread because a copy is still in flight (Unread: the latch keeps its cut).
struct Tick {
  uint64_t frame;
  bool cut;
  bool read;
};
static std::string HostRun(const std::vector<Tick>& ticks, uint32_t after) {
  cut_shot::Machine m(after);
  cut_shot::CutLatch latch;
  std::string s;
  for (const Tick& t : ticks) {
    if (!t.read) {
      latch.Unread(t.cut);
      continue;
    }
    std::vector<cut_shot::Out> out;
    m.Feed(t.frame, latch.Read(t.cut), out);
    for (const cut_shot::Out& o : out) s += (o.kind == cut_shot::Out::kBefore ? "B" : o.kind == cut_shot::Out::kAt ? "A" : "+") + std::to_string((unsigned long long)o.frame) + " ";
  }
  if (!s.empty()) s.pop_back();
  return s;
}

static void CutCases() {
  // 1. one cut, steady frames: before 13, the cut 14, +3 distinct = 17. Nothing else is written.
  {
    uint32_t cuts = 0;
    const std::string s = Run({{10, 0}, {11, 0}, {12, 0}, {13, 0}, {14, 1}, {15, 0}, {16, 0}, {17, 0}, {18, 0}, {19, 0}}, 3, &cuts);
    CHECK(s == "B13 A14 +17" && cuts == 1, "one cut: '%s' (cuts %u)", s.c_str(), cuts);
  }
  // 2. a repeated guest frame is skipped: it neither counts toward +N nor is written; a repeat that carries the cut flag is not a second cut.
  {
    const std::string s = Run({{10, 0}, {11, 0}, {11, 0}, {12, 0}, {12, 1}, {13, 1}, {13, 0}, {14, 0}, {15, 0}, {16, 0}}, 3);
    CHECK(s == "r r B12 A13 r +16", "repeats: '%s'", s.c_str());
  }
  // 3. the +N frame counts DISTINCT frames, not guest frame numbers (the guest numbers skip: 14 -> 20 -> 30 -> 41).
  {
    const std::string s = Run({{10, 0}, {12, 0}, {14, 1}, {20, 0}, {30, 0}, {41, 0}, {50, 0}}, 3);
    CHECK(s == "B12 A14 +41", "gapped guest frames: '%s'", s.c_str());
  }
  // 4. a cut on the very first frame has no "before" and is not a cut (nothing to compare with).
  {
    uint32_t cuts = 9;
    const std::string s = Run({{5, 1}, {6, 0}, {7, 0}, {8, 0}, {9, 0}}, 3, &cuts);
    CHECK(s.empty() && cuts == 0, "first-frame cut: '%s' cuts %u", s.c_str(), cuts);
  }
  // 5. two cuts close together: a frame is written once. cut 14 (+17), cut 17: its "before" 16 is new, its cut frame 17 is the first cut's +3 (one file "A17"), its +3 = 20.
  {
    uint32_t cuts = 0;
    const std::string s = Run({{12, 0}, {13, 0}, {14, 1}, {15, 0}, {16, 0}, {17, 1}, {18, 0}, {19, 0}, {20, 0}, {21, 0}}, 3, &cuts);
    CHECK(s == "B13 A14 B16 A17 +20" && cuts == 2, "two cuts: '%s' (cuts %u)", s.c_str(), cuts);
  }
  // 6. the +N frame that is the next cut's "before": one file.
  {
    const std::string s = Run({{1, 0}, {2, 1}, {3, 0}, {4, 0}, {5, 0}, {6, 1}, {7, 0}, {8, 0}, {9, 0}, {10, 0}}, 3);
    // cut 2: B1 A2 (+5 pending); cut at 6: B5 is the first cut's +3 frame, which is written at 5 as "+5": no second B5.
    CHECK(s == "B1 A2 +5 A6 +9", "after = next before: '%s'", s.c_str());
  }
  // 7. N = 0 writes before + cut only; N = 1 the next frame.
  {
    CHECK(Run({{1, 0}, {2, 1}, {3, 0}, {4, 0}}, 0) == "B1 A2", "N=0");
    CHECK(Run({{1, 0}, {2, 1}, {3, 0}, {4, 0}}, 1) == "B1 A2 +3", "N=1");
  }
  // 8. no cut at all: nothing is written (the run shoots only around cuts).
  CHECK(Run({{1, 0}, {2, 0}, {3, 0}, {4, 0}, {5, 0}}, 3).empty(), "no cut, nothing written");
  // 9. (review) the host reads every other tick (a copy still in flight): the cut on the unread tick 13 is carried to the next read tick 14. Before 12, at 14, +3 read distinct frames = 20.
  {
    const std::string s = HostRun({{10, 0, 1}, {11, 0, 0}, {12, 0, 1}, {13, 1, 0}, {14, 0, 1}, {15, 0, 0}, {16, 0, 1}, {17, 0, 0}, {18, 0, 1}, {19, 0, 0}, {20, 0, 1}}, 3);
    CHECK(s == "B12 A14 +20", "cut on an unread tick: '%s'", s.c_str());
  }
  // 10. (review) a cut on a read tick is not carried again: one cut, one "at".
  CHECK(HostRun({{1, 0, 1}, {2, 1, 1}, {3, 0, 0}, {4, 0, 1}, {5, 0, 1}, {6, 0, 1}}, 0) == "B1 A2", "a read cut is not carried");
}

// N1 / N2 (main session, review): the phone writes only with diagnostics on; at most 2 writes in flight, the rest dropped and counted.
static void PolicyCases() {
  CHECK(!shot_policy::MayWrite(true, false), "the phone wrote a screenshot with diagnostics off");
  CHECK(shot_policy::MayWrite(true, true), "the phone did not write with diagnostics on");
  CHECK(shot_policy::MayWrite(false, false), "the PC did not write");
  // A burst of cuts: every frame a cut (a fast zoom the camera test calls a cut each tick), N = 3. The machine asks for a write per picture; none finishes during the burst
  // (a 1440p encode is ~0.5 s). The gate lets 2 run and drops the rest, counted; the host's watch sees the gate full and stops queueing read-backs.
  shot_policy::WriteGate gate;
  cut_shot::Machine m(3);
  int asked = 0, peak = 0;
  for (uint64_t f = 1; f <= 12; ++f) {
    std::vector<cut_shot::Out> out;
    m.Feed(f, f > 1, out);
    for (size_t i = 0; i < out.size(); ++i) {
      ++asked;
      if (gate.TryEnter()) peak = std::max(peak, gate.in_flight.load());
    }
  }
  CHECK(asked > 2 && peak <= shot_policy::kMaxWrites, "a burst of %d pictures had %d writes in flight (cap %d)", asked, peak, shot_policy::kMaxWrites);
  CHECK(gate.dropped.load() == uint64_t(asked - std::min(asked, shot_policy::kMaxWrites)), "dropped %llu of %d asked", (unsigned long long)gate.dropped.load(), asked);
  CHECK(gate.Full(), "the gate is not full with 2 writes in flight (the host would keep queueing read-backs)");
  gate.Leave();
  CHECK(!gate.Full() && gate.TryEnter(), "a write that ended does not free a slot");
  // A NAMED shot never drops (main session: every run is fast-forwarded; regress / shot_refs need every named shot): at a full gate it holds until a write ends (another thread leaves after 50 ms).
  {
    shot_policy::WriteGate g;
    g.TryEnter(), g.TryEnter();
    std::thread t([&] { std::this_thread::sleep_for(std::chrono::milliseconds(50)), g.Leave(); });
    const shot_policy::WriteGate::Entered e = g.EnterWait(std::chrono::milliseconds(5000));
    t.join();
    CHECK(e == shot_policy::WriteGate::Entered::Free && g.dropped.load() == 0 && g.in_flight.load() == 2, "a named shot at a full gate: entered %d, dropped %llu, in flight %d (want held, then Free)",
          int(e), (unsigned long long)g.dropped.load(), g.in_flight.load());
  }
  // ... bounded: nothing frees within the bound: it is written anyway past the cap (Forced, counted), still never dropped.
  {
    shot_policy::WriteGate g;
    g.TryEnter(), g.TryEnter();
    const shot_policy::WriteGate::Entered e = g.EnterWait(std::chrono::milliseconds(100));
    CHECK(e == shot_policy::WriteGate::Entered::Forced && g.dropped.load() == 0 && g.forced.load() == 1 && g.in_flight.load() == 3, "a named shot past the bound: entered %d, dropped %llu, forced %llu",
          int(e), (unsigned long long)g.dropped.load(), (unsigned long long)g.forced.load());
  }
}

int main(int argc, char** argv) {
  int plant = 0;
  for (int i = 1; i < argc; ++i)
    if (!std::strcmp(argv[i], "--plant") && i + 1 < argc) plant = std::atoi(argv[++i]);
  if (plant == 1) webp_shot::g_plant = 1;
  if (plant >= 11 && plant <= 14) cut_shot::g_plant = plant - 10;
  if (plant >= 21 && plant <= 23) shot_policy::g_plant = plant - 20;
  if (plant != 0 && plant != 1 && !(plant >= 11 && plant <= 14) && !(plant >= 21 && plant <= 23)) {
    std::printf("unknown plant %d\n", plant);
    return 2;
  }
  if (plant == 0 || plant == 1) WebpCases(plant == 0);
  if (plant == 0 || (plant >= 11 && plant <= 14)) CutCases();
  if (plant == 0 || plant >= 21) PolicyCases();
  std::printf("%s (%d failures)%s\n", g_fails ? "FAIL" : "PASS", g_fails, plant ? " [planted]" : "");
  return g_fails ? 1 : 0;
}
