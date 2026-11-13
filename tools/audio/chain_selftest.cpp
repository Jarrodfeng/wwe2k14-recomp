// chain_selftest.cpp - offline proof for src\audio\music_chain.h and src\audio\music_chain_params.h (lane 287, B1 + B2).
// Answers: does our chain match the stock numbers (docs\systems\audio.md 7.1/7.4)? No game run, no audio device.
//
// Build + run (from the repo root):  powershell -File tools\audio\build_selftest.ps1 [-Pck PATH]
//   or by hand: clang++ -std=c++20 -O2 -Isrc\audio tools\audio\chain_selftest.cpp -o out\audio\chain_selftest.exe
// Run:        chain_selftest.exe [--pck PATH_TO_Game_Sound_E_S.pck]
// Exit code:  0 = every check passed. Prints one line per check with the measured numbers.
//
// Parts: (1) bank reader on hand-made banks, refusals, defaults == stock numbers, optional real pck;
// (2) EQ response vs the RBJ formula and the bank's gains; (3) RoomVerb RT60 / first arrival / wet-to-dry;
// (4) net gain; (5) bypass; (6) denormals / NaN / silence; (7) allocation count in process(); (8) CPU.
#include <chrono>
#include <cstdarg>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>

#include "music_chain.h"

using namespace wwe::musicchain;

// ---- allocation counter (process() must not allocate)
static bool g_count_allocs = false;
static long g_allocs = 0;
void* operator new(size_t n) {
  if (g_count_allocs) g_allocs++;
  void* p = std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  return p;
}
void* operator new[](size_t n) {
  if (g_count_allocs) g_allocs++;
  void* p = std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }

static int g_fail = 0, g_pass = 0;
static void check(bool ok, const char* name, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
static void check(bool ok, const char* name, const char* fmt, ...) {
  char b[400];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  std::printf("[%s] %-34s %s\n", ok ? "PASS" : "FAIL", name, b);
  (ok ? g_pass : g_fail)++;
}

// ---------------------------------------------------------------- deterministic noise
struct Rng {
  uint64_t s = 0x9E3779B97F4A7C15ull;
  float next() {  // uniform [-1, 1)
    s = s * 6364136223846793005ull + 1442695040888963407ull;
    return float(int32_t(s >> 33) - (1 << 30)) / float(1 << 30);
  }
};

// ---------------------------------------------------------------- hand-made bank builder (mirror of the python test)
using Bytes = std::vector<uint8_t>;
static void be32(Bytes& b, uint32_t v) { for (int s = 24; s >= 0; s -= 8) b.push_back(uint8_t(v >> s)); }
static void bef(Bytes& b, float f) { uint32_t v; std::memcpy(&v, &f, 4); be32(b, v); }
static void app(Bytes& b, const Bytes& o) { b.insert(b.end(), o.begin(), o.end()); }
static Bytes obj(uint8_t t, uint32_t id, const Bytes& body) {
  Bytes o{t};
  be32(o, uint32_t(body.size() + 4));
  be32(o, id);
  app(o, body);
  return o;
}
static Bytes bank(const std::vector<Bytes>& objs, uint32_t ver = 65) {
  Bytes h;
  be32(h, uint32_t(objs.size()));
  for (auto& o : objs) app(h, o);
  Bytes b{'B', 'K', 'H', 'D'};
  be32(b, 12);
  be32(b, ver);
  be32(b, 1);
  be32(b, 0);
  for (char c : {'H', 'I', 'R', 'C'}) b.push_back(uint8_t(c));
  be32(b, uint32_t(h.size()));
  app(b, h);
  return b;
}
static Bytes fxslot(uint8_t slot, uint32_t id, uint8_t share) {
  Bytes b{slot};
  be32(b, id);
  b.push_back(share);
  b.push_back(0);
  return b;
}
struct DuckSpec { uint32_t target; float db; uint32_t down, up; };
static Bytes busbody(uint32_t parent, float vol, std::vector<Bytes> fx = {}, std::vector<DuckSpec> ducks = {}) {
  Bytes b;
  be32(b, parent);
  b.push_back(1);
  b.push_back(0);
  bef(b, vol);
  b.insert(b.end(), 10, 0);
  be32(b, 0);
  bef(b, 0);
  be32(b, uint32_t(ducks.size()));
  for (auto& d : ducks) {  // target, db, down, up
    be32(b, d.target);
    bef(b, d.db);
    be32(b, d.down);
    be32(b, d.up);
    b.push_back(4);
  }
  b.push_back(uint8_t(fx.size()));
  if (!fx.empty()) {
    b.push_back(0);
    for (auto& f : fx) app(b, f);
  }
  b.push_back(0);
  b.push_back(0);  // no RTPC
  be32(b, 0);
  return b;
}
static Bytes roomverb_body() {
  Bytes blob;
  for (float f : {2.3f, 6.45f, 46.f, 167.f, 6.5f, 300.f, 1.f, 0.f, 400.f, 1.f, -24.f, 7500.f, 8.5f, 0.f, -96.f, -96.3f, 0.f, -3.3f, -2.f, -17.f})
    bef(blob, f);
  blob.push_back(0);
  be32(blob, 30);
  for (float f : {36.f, 100.f, 0.f, 28.f, 100.f}) bef(blob, f);
  be32(blob, 8);
  blob.push_back(1);
  for (uint32_t v : {3u, 0u, 0u, 1u, 3u, 1u}) be32(blob, v);
  blob.insert(blob.end(), 8 + 44, 0);
  Bytes b;
  be32(b, 0x760003);
  be32(b, uint32_t(blob.size()));
  app(b, blob);
  b.insert(b.end(), 3, 0);
  return b;
}
static Bytes eq_body(const float (*bands)[4], const int* on, float out) {
  Bytes blob;
  for (int i = 0; i < 3; i++) {
    be32(blob, uint32_t(bands[i][0]));
    bef(blob, bands[i][1]);
    bef(blob, bands[i][2]);
    bef(blob, bands[i][3]);
    blob.push_back(uint8_t(on[i]));
  }
  bef(blob, out);
  blob.push_back(0);
  Bytes b;
  be32(b, 0x690003);
  be32(b, uint32_t(blob.size()));
  app(b, blob);
  b.insert(b.end(), 3, 0);
  return b;
}

static std::vector<Bytes> stock_objects() {
  const float e1[3][4] = {{4, -4.f, 333.f, 1.f}, {4, -3.f, 5665.f, 63.3f}, {5, -8.f, 1646.f, 1.f}};
  const float e2[3][4] = {{4, 0.f, 821.f, 1.f}, {6, -2.f, 1000.f, 1.f}, {5, -1.5f, 763.f, 1.f}};
  const int on1[3] = {1, 0, 1}, on2[3] = {1, 1, 1};
  Bytes mixer{0, 1, 0};
  app(mixer, fxslot(0, 75612494, 1));
  be32(mixer, 0x26160EEA);
  be32(mixer, 0x2BB3807A);
  mixer.insert(mixer.end(), 30, 0);
  return {obj(7, 126675442, mixer), obj(18, 75612494, roomverb_body()), obj(19, 1060198217, eq_body(e1, on1, 3.f)),
          obj(19, 616649844, eq_body(e2, on2, 1.f)), obj(8, 0x26160EEA, busbody(0x2BB3807A, -9.f)),
          obj(8, 0x2BB3807A, busbody(0x26DF8722, 0.f, {fxslot(1, 1060198217, 0), fxslot(2, 616649844, 0)})),
          obj(8, 0x26DF8722, busbody(0, -2.f)), obj(8, 0x213DB458, busbody(0x2BB3807A, -4.f, {}, {{0x26160EEAu, -5.f, 0u, 1000u}}))};
}

static bool near_(float a, float b, float tol = 1e-4f) { return std::fabs(a - b) <= tol; }

static void test_reader(const std::string& pck) {
  Params d = defaults();
  check(near_(net_dry_db(d), -10.3f), "defaults net dry", "%.3f dB (want -10.3)", net_dry_db(d));
  Params p;
  std::string why;
  bool ok = load_from_bank_bytes({bank(stock_objects())}, p, &why);
  check(ok && p.from_banks, "hand-made bank parses", "%s", ok ? "ok" : why.c_str());
  if (ok) {
    check(near_(p.rv.decay_s, 2.3f) && near_(p.rv.pre_delay_ms, 36) && near_(p.rv.reverb_db, -17) && near_(p.rv.dry_db, -3.3f) && near_(p.eq[0].band[2].gain_db, -8) &&
              near_(p.eq[1].band[1].gain_db, -2) && !p.eq[0].band[1].on && near_(p.music_bus_db, -9) && near_(p.master_db, -2) && p.n_ducks == 1 &&
              near_(p.ducks[0].db, -5) && p.ducks[0].fade_up_ms == 1000,
          "hand-made bank values", "decay %.2f pre %.0f eq1 hs %.1f duck %d", p.rv.decay_s, p.rv.pre_delay_ms, p.eq[0].band[2].gain_db, p.n_ducks);
  }
  Params q = defaults();
  q.rv.decay_s = 9.0f;
  { bool r = load_from_bank_bytes({bank(stock_objects(), 62)}, q, &why); check(!r && why.find("v65") != std::string::npos, "non-v65 bank refused", "%s", why.c_str()); }
  Bytes cut = bank(stock_objects());
  cut.resize(cut.size() - 7);
  { bool r = load_from_bank_bytes({cut}, q, &why); check(!r, "truncated bank refused", "%s", why.c_str()); }
  { bool r = load_from_bank_bytes({bank({obj(8, 1, busbody(0, 0))})}, q, &why); check(!r, "bank without the mixer refused", "%s", why.c_str()); }
  Params fb = defaults();
  fb.rv.decay_s = 9.0f;
  { bool r = load_from_pck("C:/definitely/not/here.pck", fb, &why); check(!r && near_(fb.rv.decay_s, 2.3f), "missing pck -> defaults", "%s", why.c_str()); }
  if (!pck.empty()) {
    Params r;
    auto t0 = std::chrono::steady_clock::now();
    bool rk = load_from_pck(pck, r, &why);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    check(rk, "real pck parses", "%s in %.1f ms", rk ? "ok" : why.c_str(), ms);
    if (rk) {
      std::printf("%s", describe(r).c_str());
      Params s = defaults();
      bool same = near_(r.rv.decay_s, s.rv.decay_s) && near_(r.rv.hf_damping, s.rv.hf_damping) && near_(r.rv.diffusion, s.rv.diffusion) && near_(r.rv.stereo_width, s.rv.stereo_width) &&
                  near_(r.rv.pre_delay_ms, s.rv.pre_delay_ms) && near_(r.rv.room_size, s.rv.room_size) && near_(r.rv.density, s.rv.density) && near_(r.rv.dry_db, s.rv.dry_db) &&
                  near_(r.rv.reverb_db, s.rv.reverb_db) && near_(r.music_bus_db, s.music_bus_db) && near_(r.master_db, s.master_db) && r.n_ducks == s.n_ducks;
      for (int e = 0; e < 2; e++) {
        same = same && near_(r.eq[e].out_db, s.eq[e].out_db);
        for (int k = 0; k < 3; k++)
          same = same && r.eq[e].band[k].type == s.eq[e].band[k].type && near_(r.eq[e].band[k].gain_db, s.eq[e].band[k].gain_db) && near_(r.eq[e].band[k].freq, s.eq[e].band[k].freq) &&
                 r.eq[e].band[k].on == s.eq[e].band[k].on;
      }
      for (int i = 0; i < r.n_ducks && i < s.n_ducks; i++) same = same && r.ducks[i].ducker == s.ducks[i].ducker && near_(r.ducks[i].db, s.ducks[i].db) && r.ducks[i].fade_up_ms == s.ducks[i].fade_up_ms;
      check(same, "pck values == built-in defaults", "net dry %.2f dB", net_dry_db(r));
    }
  }
}

// ---------------------------------------------------------------- helpers
static Params flat_params() {  // chain with every gain at 0 dB and every EQ band off
  Params p = defaults();
  p.music_bus_db = p.master_db = 0;
  for (auto& e : p.eq) {
    e.out_db = 0;
    for (auto& b : e.band) b.on = false;
  }
  p.rv.dry_db = 0;
  return p;
}
static std::complex<double> dft(const std::vector<float>& h, double fs, double f, int ch) {
  std::complex<double> a = 0;
  const double w = 2 * kPi * f / fs;
  for (size_t n = 0; n < h.size() / 2; n++) a += double(h[2 * n + ch]) * std::polar(1.0, -w * double(n));
  return a;
}

static void test_eq() {
  const double fs = 48000;
  // (a) individual bands: gain at the right place, within 0.1 dB
  Params d = defaults();
  const struct { int e, k; } bands[] = {{0, 0}, {0, 2}, {1, 0}, {1, 1}, {1, 2}};
  double worst = 0;
  for (auto& b : bands) {
    const EqBand& x = d.eq[b.e].band[b.k];
    BiquadCoef c = design_biquad(x.type, fs, x.freq, x.gain_db, x.q);
    double at;
    if (x.type == kPeaking) at = biquad_response_db(c, fs, x.freq);
    else if (x.type == kLowShelf) at = biquad_response_db(c, fs, 1.0);
    else at = biquad_response_db(c, fs, fs * 0.5);
    worst = std::max(worst, std::fabs(at - x.gain_db));
  }
  check(worst <= 0.1, "EQ band gains (shelf asymptote, peak)", "worst error %.4f dB (limit 0.1)", worst);
  // (b) the whole chain (reverb bypassed) against the sum of the bands' analytic curves + the static gains
  MusicChain ch;
  ch.prepare(d, fs);
  ch.set_reverb_bypass(true);
  std::vector<float> sil(2 * 4800, 0.f);
  ch.process(sil.data(), 4800);  // lets the wet ramp finish and the unit clear
  std::vector<float> imp(2 * 16384, 0.f);
  imp[0] = imp[1] = 1.f;
  ch.process(imp.data(), 16384);
  const double fr[] = {40, 120, 333, 500, 763, 1000, 1646, 3000, 6000, 10000, 16000, 20000};
  double worst2 = 0;
  for (double f : fr) {
    double want = d.rv.dry_db + d.music_bus_db + d.master_db + d.eq[0].out_db + d.eq[1].out_db;
    for (int e = 0; e < 2; e++)
      for (int k = 0; k < 3; k++) {
        const EqBand& x = d.eq[e].band[k];
        if (x.on) want += biquad_response_db(design_biquad(x.type, fs, x.freq, x.gain_db, x.q), fs, f);
      }
    const double got = 20 * std::log10(std::abs(dft(imp, fs, f, 0)));
    worst2 = std::max(worst2, std::fabs(got - want));
  }
  check(worst2 <= 0.1, "chain EQ response vs formula", "worst error over %zu freqs %.4f dB (limit 0.1)", sizeof fr / sizeof fr[0], worst2);
}

struct RevResult { double rt60, onset_ms, first_nonzero_ms, mid_rt60; };
static RevResult measure_reverb(double fs, const RoomVerbParams& rp) {
  RoomVerb rv;
  rv.prepare(fs, rp);
  const size_t n = size_t(8.0 * fs);
  std::vector<double> e(n);
  size_t first = n;
  double peak = 0;
  for (size_t i = 0; i < n; i++) {
    float l, r;
    rv.frame(i == 0 ? 1.f : 0.f, 0.f, l, r);
    e[i] = double(l) * l + double(r) * r;
    if (first == n && e[i] > 0) first = i;
    peak = std::max(peak, e[i]);
  }
  // Schroeder backward integration
  std::vector<double> edc(n);
  double s = 0;
  for (size_t i = n; i-- > 0;) {
    s += e[i];
    edc[i] = s;
  }
  auto db = [&](size_t i) { return 10 * std::log10(edc[i] / edc[0] + 1e-30); };
  size_t i5 = 0, i35 = 0;
  for (size_t i = 0; i < n; i++) {
    if (!i5 && db(i) <= -5) i5 = i;
    if (!i35 && db(i) <= -35) { i35 = i; break; }
  }
  RevResult r{};
  r.rt60 = 2.0 * double(i35 - i5) / fs;  // 30 dB span -> x2 = 60 dB
  r.first_nonzero_ms = 1000.0 * double(first) / fs;
  // onset: first sample within 20 dB of the strongest early reflection (first 100 ms after arrival)
  double pk = 0;
  for (size_t i = first; i < first + size_t(0.1 * fs) && i < n; i++) pk = std::max(pk, e[i]);
  size_t on = first;
  while (on < n && e[on] < pk * 0.01) on++;
  r.onset_ms = 1000.0 * double(on) / fs;
  r.mid_rt60 = 0;
  return r;
}

static double noise_power(MusicChain& ch, double seconds, float amp, float** keep = nullptr) {
  Rng rng;
  const int blocks = int(seconds * 48000 / 256);
  std::vector<float> in(2 * 256), out;
  double s = 0;
  long cnt = 0;
  for (int b = 0; b < blocks; b++) {
    for (auto& x : in) x = amp * rng.next();
    ch.process(in.data(), 256);
    if (b * 256 > 48000)  // skip the first second (settling)
      for (float x : in) { s += double(x) * x; cnt++; }
  }
  (void)keep;
  return s / double(cnt);
}

static void test_reverb() {
  Params d = defaults();
  for (double fs : {48000.0, 44100.0}) {
    RevResult r = measure_reverb(fs, d.rv);
    char nm[64];
    std::snprintf(nm, sizeof nm, "RT60 @ %.0f Hz", fs);
    check(std::fabs(r.rt60 - 2.3) <= 0.23, nm, "%.3f s (want 2.3 +-10%%)", r.rt60);
    std::snprintf(nm, sizeof nm, "first arrival @ %.0f Hz", fs);
    check(std::fabs(r.first_nonzero_ms - 36.0) <= 2.0, nm, "first wet sample %.2f ms (want 36 +-2); strong-reflection onset %.2f ms", r.first_nonzero_ms, r.onset_ms);
  }
  // wet-to-dry with the stock levels, uncorrelated noise on L and R
  Params a = flat_params(), b = flat_params();
  a.rv.dry_db = -120;                                   // wet only
  b.rv.reverb_db = -120;                                // dry only (dry_db stays 0 in flat; use stock below)
  a = defaults(); b = defaults();
  for (Params* p : {&a, &b}) {
    p->music_bus_db = p->master_db = 0;
    for (auto& e : p->eq) { e.out_db = 0; for (auto& x : e.band) x.on = false; }
  }
  a.rv.dry_db = -120;
  b.rv.reverb_db = -120;
  MusicChain ca, cb, cf;
  ca.prepare(a, 48000);
  cb.prepare(b, 48000);
  const double pw = noise_power(ca, 30, 0.3f), pd = noise_power(cb, 30, 0.3f);
  const double wd = 10 * std::log10(pw / pd);
  check(std::fabs(wd - (-13.7)) <= 1.0, "wet-to-dry level", "%.2f dB (want -13.7 +-1)", wd);
  Params f = a;
  f.rv.dry_db = d.rv.dry_db;
  cf.prepare(f, 48000);
  const double pf = noise_power(cf, 30, 0.3f), pin = 0.3 * 0.3 / 3.0;
  check(std::fabs(10 * std::log10(pf / pin) - 10 * std::log10(std::pow(db_to_lin(-3.3f), 2) + std::pow(db_to_lin(-17.f), 2))) <= 0.3, "dry + wet sum", "%.2f dB re input (want %.2f)",
        10 * std::log10(pf / pin), 10 * std::log10(std::pow(db_to_lin(-3.3f), 2) + std::pow(db_to_lin(-17.f), 2)));
}

static void test_net_gain() {
  // static gains only: reverb bypassed, EQ bands off, outputs and buses at the stock values -> -10.3 dB
  Params p = defaults();
  for (auto& e : p.eq) for (auto& x : e.band) x.on = false;
  MusicChain c;
  c.prepare(p, 48000);
  c.set_reverb_bypass(true);
  const double pin = 0.1 * 0.1 / 3.0;
  const double g = 10 * std::log10(noise_power(c, 6, 0.1f) / pin);
  check(std::fabs(g - (-10.3)) <= 0.5, "net static gain (dry path)", "%.3f dB (want -10.3 +-0.5)", g);
  // with the EQ bands: equal-power-per-octave multisine (pink-noise equivalent), -20 dBFS rms
  Params d = defaults();
  MusicChain c2;
  c2.prepare(d, 48000);
  c2.set_reverb_bypass(true);
  std::vector<float> sil(2 * 4800, 0.f);
  c2.process(sil.data(), 4800);
  const int K = 240;
  std::vector<double> fk(K), ph(K);
  Rng rng;
  for (int k = 0; k < K; k++) { fk[k] = 20.0 * std::pow(1000.0, (k + 0.5) / K); ph[k] = (rng.next() + 1) * kPi; }
  const double amp = std::sqrt(2.0 / K) * std::pow(10.0, -20.0 / 20.0);  // rms -20 dBFS in total
  const size_t n = 48000 * 4;
  std::vector<float> x(2 * n);
  for (size_t i = 0; i < n; i++) {
    double v = 0;
    for (int k = 0; k < K; k++) v += amp * std::sin(2 * kPi * fk[k] * double(i) / 48000.0 + ph[k]);
    x[2 * i] = x[2 * i + 1] = float(v);
  }
  std::vector<float> y = x;
  c2.process(y.data(), int(n));
  auto pw = [&](const std::vector<float>& v) { double s = 0; for (size_t i = n / 2; i < n; i++) s += double(v[2 * i]) * v[2 * i]; return s / double(n - n / 2); };
  const double got = 10 * std::log10(pw(y) / pw(x));
  double wsum = 0;
  for (int k = 0; k < K; k++) {
    double db = d.rv.dry_db + d.music_bus_db + d.master_db + d.eq[0].out_db + d.eq[1].out_db;
    for (int e = 0; e < 2; e++)
      for (int b = 0; b < 3; b++) {
        const EqBand& xb = d.eq[e].band[b];
        if (xb.on) db += biquad_response_db(design_biquad(xb.type, 48000, xb.freq, xb.gain_db, xb.q), 48000, fk[k]);
      }
    wsum += std::pow(10.0, db / 10.0);
  }
  const double want = 10 * std::log10(wsum / K);
  check(std::fabs(got - want) <= 0.5, "pink-equivalent level change", "%.3f dB, static -10.3 + EQ weighting predicts %.3f dB", got, want);
}

static void test_bypass() {
  Params p = defaults();
  MusicChain on, off;
  on.prepare(p, 48000);
  off.prepare(p, 48000);
  off.set_reverb_bypass(true);
  Params q = p;
  q.rv.reverb_db = -200;
  MusicChain ref;
  ref.prepare(q, 48000);
  Rng rng;
  std::vector<float> a(2 * 256), b(2 * 256), c(2 * 256);
  double maxd = 0;
  for (int blk = 0; blk < 400; blk++) {
    for (size_t i = 0; i < a.size(); i++) a[i] = 0.3f * rng.next();
    b = a; c = a;
    off.process(a.data(), 256);
    ref.process(b.data(), 256);
    if (blk > 100)  // after the 20 ms ramp has settled (~20 time constants)
      for (size_t i = 0; i < a.size(); i++) maxd = std::max(maxd, double(std::fabs(a[i] - b[i])));
    on.process(c.data(), 256);
  }
  check(maxd < 1e-6, "bypass = dry path only", "max difference to a no-reverb chain %.2e", maxd);
  // impulse into a bypassed chain leaves no tail beyond the EQ ringing
  std::vector<float> imp(2 * 48000, 0.f);
  imp[0] = imp[1] = 1.f;
  off.process(imp.data(), 48000);
  double tail = 0;
  for (size_t i = 2 * 24000; i < imp.size(); i++) tail += double(imp[i]) * imp[i];
  check(tail < 1e-12, "bypass has no tail", "energy after 0.5 s %.2e", tail);
  // un-bypass: reverb comes back clean
  off.set_reverb_bypass(false);
  std::vector<float> imp2(2 * 48000, 0.f);
  imp2[0] = imp2[1] = 1.f;
  off.process(imp2.data(), 48000);
  double e2 = 0;
  for (size_t i = 2 * 12000; i < imp2.size(); i++) e2 += double(imp2[i]) * imp2[i];
  check(e2 > 1e-8, "re-enable brings the reverb back", "tail energy %.2e", e2);
}

static void test_robust() {
  Params p = defaults();
  MusicChain c;
  c.prepare(p, 48000);
  std::vector<float> blk(2 * 256, 0.f);
  blk[0] = blk[1] = 1.f;
  long sub = 0, nonfinite = 0;
  bool exact_zero_end = true;
  for (int b = 0; b < 48000 * 40 / 256; b++) {
    c.process(blk.data(), 256);
    for (float x : blk) {
      if (std::fpclassify(x) == FP_SUBNORMAL) sub++;
      if (!std::isfinite(x)) nonfinite++;
    }
    if (b > 48000 * 25 / 256)
      for (float x : blk) if (x != 0.f) exact_zero_end = false;
    std::fill(blk.begin(), blk.end(), 0.f);
  }
  check(sub == 0 && nonfinite == 0, "silence after impulse: no denormals", "subnormal outputs %ld, non-finite %ld over 40 s", sub, nonfinite);
  check(exact_zero_end, "tail decays to exact zero", "%s", exact_zero_end ? "all-zero from 25 s on" : "NOT zero");
  // NaN / inf poisoning
  std::vector<float> bad(2 * 256, std::nanf(""));
  bad[10] = INFINITY;
  c.process(bad.data(), 256);
  bool fin = true;
  for (float x : bad) fin = fin && std::isfinite(x);
  Rng rng;
  std::vector<float> s(2 * 256);
  double e = 0;
  bool fin2 = true;
  for (int b = 0; b < 200; b++) {
    for (auto& x : s) x = 0.2f * rng.next();
    c.process(s.data(), 256);
    for (float x : s) { fin2 = fin2 && std::isfinite(x); e += double(x) * x; }
  }
  check(fin && fin2 && e > 0, "NaN / inf input is survived", "output finite, then normal signal passes (energy %.3f)", e);
  // pure silence from a cold start is exactly silent
  MusicChain c2;
  c2.prepare(p, 48000);
  std::vector<float> z(2 * 256, 0.f);
  bool zero = true;
  for (int b = 0; b < 500; b++) { c2.process(z.data(), 256); for (float x : z) zero = zero && x == 0.f; }
  check(zero, "silence in = silence out", "%s", zero ? "exact zeros" : "NONZERO");
}

static void test_alloc_cpu() {
  Params p = defaults();
  MusicChain c;
  auto t0 = std::chrono::steady_clock::now();
  c.prepare(p, 48000);
  const double prep_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  Rng rng;
  std::vector<float> in(2 * 256);
  const int secs = 60, blocks = secs * 48000 / 256;
  g_allocs = 0;
  g_count_allocs = true;
  auto t1 = std::chrono::steady_clock::now();
  for (int b = 0; b < blocks; b++) {
    for (auto& x : in) x = 0.2f * rng.next();
    c.process(in.data(), 256);
  }
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
  g_count_allocs = false;
  check(g_allocs == 0, "no allocation in process()", "%ld allocations over %d blocks", g_allocs, blocks);
  std::printf("[INFO] CPU: %.2f ms per second of audio = %.3f %% of one core (60 s of stereo noise, 256-frame blocks, this machine); prepare() %.1f ms\n", ms / secs, 100.0 * ms / secs / 1000.0, prep_ms);
}

int main(int argc, char** argv) {
  std::string pck;
  for (int i = 1; i + 1 < argc; i++)
    if (std::string(argv[i]) == "--pck") pck = argv[i + 1];
  test_reader(pck);
  test_eq();
  test_reverb();
  test_net_gain();
  test_bypass();
  test_robust();
  test_alloc_cpu();
  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
