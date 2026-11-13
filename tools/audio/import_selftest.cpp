// Offline proof for the song importer (lane 283). No game, no game files. Build + run: tools\audio\import_selftest.ps1
//   import_selftest.exe [SAMPLES_DIR]       SAMPLES_DIR (optional): the files import_samples.py wrote (mp3 / ogg / flac / m4a /
//                                           opus / wma / wav / garbage): each must import or be refused as listed in manifest.txt
// Checks: the meter against reference signals (EBU Tech 3341 style: a 997 Hz stereo sine at -23 dBFS reads -23.0 LUFS), the gates,
// the true-peak meter, the normaliser hitting the target within 0.5 LU on loud and quiet material, the limiter, and the cache
// (hit, rebuilt on a changed source / target, source never modified, garbage and silence refused).
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>
#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#endif
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "../../src/audio_import/audio_import.h"
#include "../../src/audio_import/normalise.h"
#include "../../src/audio_import/tags.h"
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "../../third_party/stb/stb_image.h"
#include "../../third_party/stb/stb_image_write.h"

namespace fs = std::filesystem;
using namespace audio_import;

static int g_fail = 0, g_pass = 0;
static void Check(bool ok, const char* what, double got = 0, double want = 0) {
  if (ok) ++g_pass;
  else ++g_fail;
  std::printf("  %s  %s", ok ? "ok  " : "FAIL", what);
  if (!ok || got != 0 || want != 0) std::printf("   (got %.3f, want %.3f)", got, want);
  std::printf("\n");
}
static bool Near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

static std::vector<float> Sine(int rate, double seconds, double db_peak, double hz, int channels, double phase = 0) {
  const size_t n = size_t(rate * seconds);
  std::vector<float> v(n * size_t(channels));
  const double a = std::pow(10.0, db_peak / 20.0);
  for (size_t i = 0; i < n; ++i) {
    const float s = float(a * std::sin(2 * 3.14159265358979323846 * hz * double(i) / rate + phase));
    for (int c = 0; c < channels; ++c) v[i * size_t(channels) + size_t(c)] = s;
  }
  return v;
}

static std::vector<float> Noise(int rate, double seconds, double rms_db, unsigned seed, double burst = 0) {
  std::mt19937 g(seed);
  std::normal_distribution<float> d(0.f, 1.f);
  const size_t n = size_t(rate * seconds);
  std::vector<float> v(n * 2);
  const float a = float(std::pow(10.0, rms_db / 20.0));
  float lp = 0;
  for (size_t i = 0; i < n; ++i) {
    for (int c = 0; c < 2; ++c) {
      float x = d(g);
      lp = lp * 0.9f + x * 0.1f;  // some low-pass colour
      v[i * 2 + size_t(c)] = (x * 0.3f + lp * 2.0f) * a;
    }
    if (burst > 0 && i % size_t(rate / 2) == 0) v[i * 2] += float(burst), v[i * 2 + 1] -= float(burst);  // sparse loud clicks
  }
  return v;
}

static void WriteFile(const fs::path& p, const std::vector<uint8_t>& b) {
  std::ofstream o(p, std::ios::binary | std::ios::trunc);
  o.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}
static std::vector<uint8_t> ReadFile(const fs::path& p) {
  std::ifstream i(p, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(i)), std::istreambuf_iterator<char>());
}

int main(int argc, char** argv) {
  const bool mem = argc > 1 && !std::strcmp(argv[1], "--mem");
  const char* samples = argc > 1 && !mem ? argv[1] : nullptr;
  std::printf("== loudness meter against reference signals\n");
  for (int rate : {32000, 44100, 48000}) {
    char name[96];
    auto s = Sine(rate, 20, -23.0, 997.0, 2);
    Loudness l = Measure(s.data(), s.size() / 2, 2, rate);
    std::snprintf(name, sizeof name, "stereo 997 Hz -23 dBFS reads -23.0 LUFS @%d", rate);
    Check(Near(l.integrated, -23.0, 0.1), name, l.integrated, -23.0);
    auto s33 = Sine(rate, 20, -33.0, 997.0, 2);
    l = Measure(s33.data(), s33.size() / 2, 2, rate);
    std::snprintf(name, sizeof name, "stereo 997 Hz -33 dBFS reads -33.0 LUFS @%d", rate);
    Check(Near(l.integrated, -33.0, 0.1), name, l.integrated, -33.0);
  }
  {
    auto m = Sine(48000, 20, 0.0, 997.0, 1);
    Loudness l = Measure(m.data(), m.size(), 1, 48000);
    Check(Near(l.integrated, -3.01, 0.1), "mono 997 Hz 0 dBFS reads -3.01 LUFS (BS.1770 definition)", l.integrated, -3.01);
  }
  {  // relative gate: 10 s at -23 LUFS then 10 s 17 LU lower: the quiet half is gated out
    auto a = Sine(48000, 10, -23.0, 997.0, 2), b = Sine(48000, 10, -40.0, 997.0, 2);
    a.insert(a.end(), b.begin(), b.end());
    Loudness l = Measure(a.data(), a.size() / 2, 2, 48000);
    Check(Near(l.integrated, -23.0, 0.1), "relative gate (-10 LU): the quiet half does not count", l.integrated, -23.0);
    // 5 LU lower is above the relative gate: counts
    auto c = Sine(48000, 10, -23.0, 997.0, 2), d = Sine(48000, 10, -28.0, 997.0, 2);
    c.insert(c.end(), d.begin(), d.end());
    l = Measure(c.data(), c.size() / 2, 2, 48000);
    const double want = 10.0 * std::log10((std::pow(10.0, -2.3) + std::pow(10.0, -2.8)) / 2.0);
    Check(Near(l.integrated, want, 0.1), "a part 5 LU lower still counts (energy mean)", l.integrated, want);
    // absolute gate: 6 s at -23 + 30 s of digital silence
    auto e = Sine(48000, 6, -23.0, 997.0, 2);
    e.resize(e.size() + 30 * 48000 * 2, 0.f);
    l = Measure(e.data(), e.size() / 2, 2, 48000);
    Check(Near(l.integrated, -23.0, 0.3), "absolute gate (-70 LUFS): silence does not count (+-0.3: blocks straddling the edge)", l.integrated, -23.0);
    std::vector<float> z(48000 * 2 * 5, 0.f);
    l = Measure(z.data(), z.size() / 2, 2, 48000);
    Check(!(l.integrated > -69.0), "digital silence has no loudness");
  }
  {  // true peak: fs/4 sine at 45 degrees: every sample is +-0.7071, the real waveform peaks at 1.0
    auto s = Sine(48000, 2, 0.0, 12000.0, 1, 3.14159265358979323846 / 4);
    Loudness l = Measure(s.data(), s.size(), 1, 48000);
    Check(Near(l.sample_peak_db, -3.01, 0.05), "sample peak of the fs/4 sine is -3.01 dBFS", l.sample_peak_db, -3.01);
    Check(Near(l.true_peak_db, 0.0, 0.3), "true peak of the same sine is 0 dBFS (+-0.3)", l.true_peak_db, 0.0);
    auto q = Sine(48000, 2, -6.0, 1000.0, 1);
    l = Measure(q.data(), q.size(), 1, 48000);
    Check(Near(l.true_peak_db, -6.0, 0.1), "true peak of a 1 kHz sine at -6 dBFS is -6 dBFS", l.true_peak_db, -6.0);
  }

  std::printf("== normaliser: target %.1f LUFS, true-peak ceiling %.1f dBFS\n", kTargetLufs, kTruePeakCeilingDb);
  {
    struct Case { const char* name; double rms_db; };
    for (Case c : {Case{"loud (-6 dB rms)", -6.0}, Case{"quiet (-45 dB rms)", -45.0}, Case{"medium (-24 dB rms)", -24.0}}) {
      auto n = Noise(44100, 20, c.rms_db, 7);
      for (float& v : n) v = std::clamp(v, -4.f, 4.f);  // pre-clipped sources are fine: only the loudness matters here
      NormResult r = Normalise(n, 44100, kTargetLufs, kTruePeakCeilingDb);
      char name[128];
      std::snprintf(name, sizeof name, "%s: ends at the target (+-0.5 LU), true peak <= ceiling", c.name);
      Loudness l = Measure(n.data(), n.size() / 2, 2, 44100);
      Check(r.ok && Near(l.integrated, kTargetLufs, 0.5) && l.true_peak_db <= kTruePeakCeilingDb + 0.1, name, l.integrated, kTargetLufs);
      std::printf("        src %.1f LUFS / %.1f dBTP -> gain %+.1f dB%s -> %.1f LUFS / %.1f dBTP\n", r.src_lufs,
                  r.src_true_peak_db, r.gain_db, r.limited ? " + limiter" : "", l.integrated, l.true_peak_db);
    }
    // peaky + quiet: the gain would clip the clicks: the limiter keeps the true peak under the ceiling
    auto n = Noise(44100, 20, -40.0, 11, 0.5);
    NormResult r = Normalise(n, 44100, kTargetLufs, kTruePeakCeilingDb);
    Loudness l = Measure(n.data(), n.size() / 2, 2, 44100);
    Check(r.ok && r.limited, "peaky quiet material needs the limiter");
    Check(l.true_peak_db <= kTruePeakCeilingDb + 0.1, "limited: true peak <= ceiling (+0.1)", l.true_peak_db, kTruePeakCeilingDb);
    Check(Near(l.integrated, kTargetLufs, 2.0), "limited: loudness within 2 LU of the target (a 50 dB crest click train)", l.integrated, kTargetLufs);
    std::vector<float> z(44100 * 2 * 3, 0.f);
    Check(!Normalise(z, 44100, kTargetLufs).ok, "a silent song is refused");
    std::vector<float> sh(44100 / 4 * 2, 0.1f);
    Check(!Normalise(sh, 44100, kTargetLufs).ok, "a song shorter than half a second is refused");
  }

  std::printf("== tags + cover art, one synthetic file per container\n");
  {
    using Bytes = std::vector<uint8_t>;
    // a 90x60 gradient PNG as the "album art" (non-square: the thumbnail must be a centred square)
    Bytes art;
    {
      std::vector<uint8_t> px(90 * 60 * 3);
      for (int y = 0; y < 60; ++y)
        for (int x = 0; x < 90; ++x) px[size_t(y * 90 + x) * 3] = uint8_t(x * 2), px[size_t(y * 90 + x) * 3 + 1] = uint8_t(y * 4), px[size_t(y * 90 + x) * 3 + 2] = 99;
      stbi_write_png_to_func([](void* c, void* d, int n) { auto* v = static_cast<Bytes*>(c); v->insert(v->end(), (uint8_t*)d, (uint8_t*)d + n); }, &art, 90, 60, 3, px.data(), 90 * 3);
    }
    auto be32 = [](Bytes& b, uint32_t v) { for (int s = 24; s >= 0; s -= 8) b.push_back(uint8_t(v >> s)); };
    auto le32 = [](Bytes& b, uint32_t v) { for (int s = 0; s < 32; s += 8) b.push_back(uint8_t(v >> s)); };
    auto str = [](Bytes& b, const std::string& s) { b.insert(b.end(), s.begin(), s.end()); };
    auto check = [&](const char* what, const Bytes& file, bool want_art) {
      const Tags t = ReadTags(file);
      char name[160];
      std::snprintf(name, sizeof name, "%s: title 'Kingdom', artist 'Bob', %s", what, want_art ? "art found" : "no art -> none");
      const bool text = t.title == "Kingdom" && t.artist == "Bob";
      Check(text && (want_art ? t.picture == art : t.picture.empty()), name);
      if (want_art) {
        const Bytes png = MakeThumbnailPng(t.picture.data(), t.picture.size());
        int w = 0, h = 0, n = 0;
        const bool dims = !png.empty() && stbi_info_from_memory(png.data(), int(png.size()), &w, &h, &n) && w == 256 && h == 256;
        Check(dims, "   the thumbnail is a 256x256 PNG", w, 256);
      }
    };
    auto vorbis_comment = [&](bool with_pic) {
      Bytes c;
      le32(c, 4), str(c, "test");
      const std::string pic_b64 = [&] {
        Bytes p;
        be32(p, 3), be32(p, 9), str(p, "image/png"), be32(p, 0), be32(p, 90), be32(p, 60), be32(p, 24), be32(p, 0), be32(p, uint32_t(art.size()));
        p.insert(p.end(), art.begin(), art.end());
        const char* k = tags_detail::kB64;
        std::string o;
        for (size_t i = 0; i < p.size(); i += 3) {
          uint32_t v = p[i] << 16 | (i + 1 < p.size() ? p[i + 1] << 8 : 0) | (i + 2 < p.size() ? p[i + 2] : 0);
          o += k[v >> 18 & 63], o += k[v >> 12 & 63], o += i + 1 < p.size() ? k[v >> 6 & 63] : '=', o += i + 2 < p.size() ? k[v & 63] : '=';
        }
        return o;
      }();
      std::vector<std::string> items = {"title=Kingdom", "ARTIST=Bob"};
      if (with_pic) items.push_back("METADATA_BLOCK_PICTURE=" + pic_b64);
      le32(c, uint32_t(items.size()));
      for (auto& i : items) le32(c, uint32_t(i.size())), str(c, i);
      return c;
    };
    // ID3v2.3 (mp3): TIT2 + TPE1 (UTF-16 with BOM) + APIC
    for (bool with : {true, false}) {
      Bytes frames;
      auto frame = [&](const char* id, const Bytes& body) { str(frames, id); be32(frames, uint32_t(body.size())); frames.push_back(0), frames.push_back(0); frames.insert(frames.end(), body.begin(), body.end()); };
      Bytes t1 = {0}; str(t1, "Kingdom");  // Latin-1
      Bytes t2 = {1, 0xFF, 0xFE};          // UTF-16 LE with BOM
      for (char c : std::string("Bob")) t2.push_back(uint8_t(c)), t2.push_back(0);
      frame("TIT2", t1), frame("TPE1", t2);
      if (with) { Bytes a = {0}; str(a, "image/png"); a.push_back(0), a.push_back(3), a.push_back(0); a.insert(a.end(), art.begin(), art.end()); frame("APIC", a); }
      Bytes f = {'I', 'D', '3', 3, 0, 0};
      const size_t n = frames.size();
      f.push_back(uint8_t(n >> 21 & 0x7F)), f.push_back(uint8_t(n >> 14 & 0x7F)), f.push_back(uint8_t(n >> 7 & 0x7F)), f.push_back(uint8_t(n & 0x7F));
      f.insert(f.end(), frames.begin(), frames.end());
      f.resize(f.size() + 64, 0xFF);
      check(with ? "mp3 ID3v2.3 APIC" : "mp3 ID3v2.3 no APIC", f, with);
    }
    // FLAC: STREAMINFO placeholder, VORBIS_COMMENT, PICTURE (last)
    for (bool with : {true, false}) {
      Bytes f = {'f', 'L', 'a', 'C'};
      f.push_back(0), f.push_back(0), f.push_back(0), f.push_back(34), f.resize(f.size() + 34, 0);
      Bytes vc = vorbis_comment(false);
      f.push_back(with ? 4 : 0x84), f.push_back(uint8_t(vc.size() >> 16)), f.push_back(uint8_t(vc.size() >> 8)), f.push_back(uint8_t(vc.size())), f.insert(f.end(), vc.begin(), vc.end());
      if (with) {
        Bytes p;
        be32(p, 3), be32(p, 9), str(p, "image/png"), be32(p, 0), be32(p, 90), be32(p, 60), be32(p, 24), be32(p, 0), be32(p, uint32_t(art.size())), p.insert(p.end(), art.begin(), art.end());
        f.push_back(0x86), f.push_back(uint8_t(p.size() >> 16)), f.push_back(uint8_t(p.size() >> 8)), f.push_back(uint8_t(p.size())), f.insert(f.end(), p.begin(), p.end());
      }
      check(with ? "FLAC PICTURE" : "FLAC no picture", f, with);
    }
    // Ogg (Vorbis and Opus): the comment packet is split over several 255-byte segments and two pages
    for (bool opus : {false, true}) {
      for (bool with : {true, false}) {
        Bytes pkt;
        if (opus) str(pkt, "OpusTags"); else pkt.push_back(3), str(pkt, "vorbis");
        const Bytes vc = vorbis_comment(with);
        pkt.insert(pkt.end(), vc.begin(), vc.end());
        if (!opus) pkt.push_back(1);  // framing bit
        auto page = [&](const Bytes& payload, bool cont, std::vector<uint8_t> segs) {
          Bytes g = {'O', 'g', 'g', 'S', 0, uint8_t(cont ? 1 : 0)};
          g.resize(26, 0);  // granule 8, serial 4, sequence 4, crc 4 (the reader does not check the crc)
          g.push_back(uint8_t(segs.size()));
          g.insert(g.end(), segs.begin(), segs.end()), g.insert(g.end(), payload.begin(), payload.end());
          return g;
        };
        Bytes ident = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, 2};
        if (!opus) ident = {1, 'v', 'o', 'r', 'b', 'i', 's', 0, 0, 0, 0};
        Bytes file = page(ident, false, {uint8_t(ident.size())});
        // comment packet: first page carries up to 200 segments (full 255s), the rest on a continuation page
        std::vector<uint8_t> s1, s2;
        Bytes p1, p2;
        {  // 255-byte segments; the first two go on page 1 (packet continues), the rest on page 2
          size_t at = 0, idx = 0;
          for (;;) {
            const size_t seg = std::min<size_t>(pkt.size() - at, 255);
            auto& S = idx < 2 ? s1 : s2;
            auto& P = idx < 2 ? p1 : p2;
            S.push_back(uint8_t(seg)), P.insert(P.end(), pkt.begin() + long(at), pkt.begin() + long(at + seg));
            at += seg, ++idx;
            if (seg < 255) break;
            if (at == pkt.size()) { (idx <= 2 ? s1 : s2).push_back(0); break; }
          }
        }
        Bytes pg1 = page(p1, false, s1);
        file.insert(file.end(), pg1.begin(), pg1.end());
        if (!s2.empty()) { Bytes pg2 = page(p2, true, s2); file.insert(file.end(), pg2.begin(), pg2.end()); }
        char what[80];
        std::snprintf(what, sizeof what, "Ogg %s%s", opus ? "Opus" : "Vorbis", with ? " METADATA_BLOCK_PICTURE (2 pages)" : "");
        check(what, file, with);
      }
    }
    // MP4 / M4A: ftyp, moov.udta.meta.ilst with (c)nam, (c)ART, covr
    for (bool with : {true, false}) {
      auto atom = [&](const std::string& type, const Bytes& body) { Bytes a; be32(a, uint32_t(8 + body.size())); str(a, type); a.insert(a.end(), body.begin(), body.end()); return a; };
      auto data = [&](const std::string& s, uint32_t flags) { Bytes d; be32(d, flags); be32(d, 0); str(d, s); return atom("data", d); };
      Bytes ilst;
      auto add = [&](const Bytes& x) { ilst.insert(ilst.end(), x.begin(), x.end()); };
      add(atom("\xA9nam", data("Kingdom", 1))), add(atom("\xA9" "ART", data("Bob", 1)));
      if (with) add(atom("covr", data(std::string(art.begin(), art.end()), 14)));
      Bytes meta = {0, 0, 0, 0};
      const Bytes il = atom("ilst", ilst);
      meta.insert(meta.end(), il.begin(), il.end());
      const Bytes udta = atom("udta", atom("meta", meta)), moov = atom("moov", udta);
      Bytes f = atom("ftyp", Bytes{'M', '4', 'A', ' ', 0, 0, 0, 0});
      f.insert(f.end(), moov.begin(), moov.end());
      check(with ? "M4A covr" : "M4A no covr", f, with);
    }
    // WMA / ASF: content description (title, author) + extended content descriptor WM/Picture
    for (bool with : {true, false}) {
      auto u16 = [&](const std::string& s) { Bytes o; for (char c : s) o.push_back(uint8_t(c)), o.push_back(0); o.push_back(0), o.push_back(0); return o; };
      auto le16 = [](Bytes& b, uint32_t v) { b.push_back(uint8_t(v)), b.push_back(uint8_t(v >> 8)); };
      auto obj = [&](const uint8_t* guid, const Bytes& body) { Bytes o(guid, guid + 16); uint64_t sz = 24 + body.size(); le32(o, uint32_t(sz)), le32(o, 0); o.insert(o.end(), body.begin(), body.end()); return o; };
      const uint8_t gh[16] = {0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11, 0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C};
      const uint8_t gc[16] = {0x33, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11, 0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C};
      const uint8_t ge[16] = {0x40, 0xA4, 0xD0, 0xD2, 0x07, 0xE3, 0xD2, 0x11, 0x97, 0xF0, 0x00, 0xA0, 0xC9, 0x5E, 0xA8, 0x50};
      Bytes cd;
      const Bytes ti = u16("Kingdom"), ar = u16("Bob");
      le16(cd, uint32_t(ti.size())), le16(cd, uint32_t(ar.size())), le16(cd, 0), le16(cd, 0), le16(cd, 0);
      cd.insert(cd.end(), ti.begin(), ti.end()), cd.insert(cd.end(), ar.begin(), ar.end());
      Bytes objs = obj(gc, cd);
      if (with) {
        Bytes ec;
        le16(ec, 1);
        const Bytes nm = u16("WM/Picture");
        le16(ec, uint32_t(nm.size())), ec.insert(ec.end(), nm.begin(), nm.end());
        Bytes val;
        val.push_back(3), le32(val, uint32_t(art.size()));
        const Bytes mime = u16("image/png"), desc = u16("");
        val.insert(val.end(), mime.begin(), mime.end()), val.insert(val.end(), desc.begin(), desc.end()), val.insert(val.end(), art.begin(), art.end());
        le16(ec, 1), le16(ec, uint32_t(val.size())), ec.insert(ec.end(), val.begin(), val.end());
        const Bytes eo = obj(ge, ec);
        objs.insert(objs.end(), eo.begin(), eo.end());
      }
      Bytes hbody;
      le32(hbody, 2), hbody.push_back(1), hbody.push_back(2), hbody.insert(hbody.end(), objs.begin(), objs.end());
      Bytes f = obj(gh, hbody);
      check(with ? "WMA WM/Picture" : "WMA no picture", f, with);
    }
    // damaged tags never crash
    Bytes bad = {'I', 'D', '3', 3, 0, 0, 0, 0, 0x7F, 0x7F, 'A', 'P', 'I', 'C', 0xFF, 0xFF, 0xFF, 0xFF, 0, 0};
    (void)ReadTags(bad);
    Bytes junk(5000, 0x41);
    (void)ReadTags(junk);
    Check(true, "damaged / garbage tags do not crash");
    Check(MakeThumbnailPng(junk.data(), junk.size()).empty(), "bytes that are not an image give no thumbnail");
  }

  std::printf("== import + cache\n");
  const fs::path tmp = fs::temp_directory_path() / "wwe2k14_audio_import_selftest";
  fs::remove_all(tmp);
  fs::create_directories(tmp);
  const fs::path cache = tmp / "cache";
  {
    auto n = Noise(44100, 12, -20.0, 3);
    const fs::path a = tmp / "a.wav", b = tmp / "b_same_content_other_name.wav", c = tmp / "c_changed.wav";
    WriteFile(a, WritePcm16Wav(n, 44100));
    fs::copy_file(a, b);
    for (float& v : n) v *= 0.5f;
    n[1000] += 0.01f;
    WriteFile(c, WritePcm16Wav(n, 44100));
    const auto before = ReadFile(a);
    Imported r1 = Import(a, cache), r2 = Import(a, cache), r3 = Import(b, cache), r4 = Import(c, cache);
    Check(r1.ok && !r1.from_cache, "first import converts", r1.ok, 1);
    Check(r2.ok && r2.from_cache && r2.cache == r1.cache, "second import is a cache hit (same file)");
    Check(r3.ok && r3.from_cache && r3.cache == r1.cache, "the same content under another name is a hit");
    Check(r4.ok && !r4.from_cache && r4.cache != r1.cache, "a changed source is rebuilt under a new key");
    Imported r5 = Import(a, cache, kTargetLufs - 3.0);
    Check(r5.ok && !r5.from_cache && r5.cache != r1.cache, "a changed target is rebuilt under a new key");
    Check(ReadFile(a) == before, "the source file is never modified");
    // the cached file really is at the target
    auto w = ReadFile(r1.cache);
    std::vector<float> pcm((w.size() - 44) / 2);
    for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = float(int16_t(w[44 + 2 * i] | (w[45 + 2 * i] << 8))) / 32768.f;
    Loudness l = Measure(pcm.data(), pcm.size() / 2, 2, 44100);
    Check(Near(l.integrated, kTargetLufs, 0.5), "the cache file measures at the target (+-0.5 LU)", l.integrated, kTargetLufs);
    Check(Near(r1.out_lufs, l.integrated, 0.3), "the stored numbers match the file", r1.out_lufs, l.integrated);
    // a clean cache: a half-written entry (wav without its txt) is not a hit
    fs::remove(fs::path(r1.cache).replace_extension(".txt"));
    Imported r6 = Import(a, cache);
    Check(r6.ok && !r6.from_cache, "a cache entry missing its numbers file is rebuilt");
    // refusals
    std::mt19937 g(5);
    std::vector<uint8_t> junk(50000);
    for (auto& x : junk) x = uint8_t(g());
    WriteFile(tmp / "junk.mp3", junk);
    Imported j = Import(tmp / "junk.mp3", cache);
    Check(!j.ok && !j.err.empty(), "garbage named .mp3 is refused with a message");
    if (!j.err.empty()) std::printf("        message: %s\n", j.err.c_str());
    WriteFile(tmp / "empty.wav", {});
    Check(!Import(tmp / "empty.wav", cache).ok, "an empty file is refused");
    std::vector<float> sil(44100 * 2 * 3, 0.f);
    WriteFile(tmp / "silent.wav", WritePcm16Wav(sil, 44100));
    Imported s = Import(tmp / "silent.wav", cache);
    Check(s.ok && !s.normalised, "a silent song imports un-normalised (not refused)");
    if (!s.note.empty()) std::printf("        note: %s\n", s.note.c_str());
    Check(!Import(tmp / "nope.mp3", cache).ok, "a missing file is refused");
    // truncated real wav: decodes what is there or refuses, never crashes
    auto full = ReadFile(a);
    full.resize(full.size() / 3);
    WriteFile(tmp / "trunc.wav", full);
    (void)Import(tmp / "trunc.wav", cache);
    Check(true, "a truncated WAV does not crash");
    // mono and 8 kHz/22 kHz sources become stereo at their own rate
    std::vector<float> mono(22050 * 4);
    for (size_t i = 0; i < mono.size(); ++i) mono[i] = 0.3f * std::sin(float(i) * 0.05f);
    std::vector<float> st = ToStereo(mono.data(), mono.size(), 1);
    WriteFile(tmp / "mono22.wav", WritePcm16Wav(st, 22050));
    Imported m = Import(tmp / "mono22.wav", cache);
    Check(m.ok && m.rate == 22050, "a 22.05 kHz source keeps its rate");
  }

  std::printf("== the game never converts: PlayableNow / Lookup are a stat and the cache index\n");
  {
    const fs::path gcache = tmp / "gcache";
    const fs::path song = tmp / "game_song.wav";
    WriteFile(song, WritePcm16Wav(Noise(44100, 20, -22.0, 9), 44100));
    const uint64_t imp0 = Stats().imports.load(), dec0 = Stats().decodes.load();
    Imported info;
    const auto t0 = std::chrono::steady_clock::now();
    const fs::path p1 = PlayableNow(song, gcache, kTargetLufs, &info);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    Check(p1 == song && !info.ok && info.err.empty(), "an unconverted song: PlayableNow returns the source itself, 'not converted yet'");
    Check(ms < 50.0, "  in under 50 ms", ms, 50.0);
    Check(Stats().imports.load() == imp0 && Stats().decodes.load() == dec0, "  no Import call and no decode happened");
    // the launcher converts (with progress); then the game's lookup finds the cache through index.txt without hashing the file
    int calls = 0;
    float last = -1;
    bool monotone = true, saw[5] = {false, false, false, false, false};
    Imported conv = Import(song, gcache, kTargetLufs, [&](const Progress& p) {
      ++calls;
      if (p.overall + 1e-4f < last) monotone = false;
      last = p.overall;
      saw[int(p.stage)] = true;
      return true;
    });
    Check(conv.ok && conv.normalised, "the launcher's Import converts the song");
    Check(calls > 10 && monotone && last > 0.9f, "  progress is reported, never goes backwards, and ends near 1", last, 1.0);
    Check(saw[0] && saw[1] && saw[2] && saw[4], "  the stages Reading / Decoding / Measuring / Writing all reported");
    Check(fs::is_regular_file(gcache / "index.txt"), "  <cache>\\index.txt was written");
    const uint64_t imp1 = Stats().imports.load();
    Imported info2;
    const fs::path p2 = PlayableNow(song, gcache, kTargetLufs, &info2);
    Check(p2 == conv.cache && info2.ok && info2.title == conv.title, "after that PlayableNow returns the cached normalised copy (from the index)");
    Check(Stats().imports.load() == imp1, "  without any Import call");
    // a new process has no memo: simulate with a changed-then-restored mtime? the index alone must answer: remove the memo effect by a copy
    const fs::path song2 = tmp / "game_song_copy.wav";
    fs::copy_file(song, song2);
    Check(PlayableNow(song2, gcache) == song2, "a copy at another path is 'not converted yet' for the game (the index is per path)");
    // a refused file
    std::mt19937 jg(11);
    std::vector<uint8_t> junk2(30000);
    for (auto& x : junk2) x = uint8_t(jg());
    WriteFile(tmp / "refused.mp3", junk2);
    Imported rj = Import(tmp / "refused.mp3", gcache);
    Imported why;
    const fs::path p3 = PlayableNow(tmp / "refused.mp3", gcache, kTargetLufs, &why);
    Check(!rj.ok && p3.empty() && !why.ok && !why.err.empty(), "a refused song gives an empty path and the reason (the game's own theme stays)");
    // cancel: nothing is written, nothing is indexed
    const fs::path cs = tmp / "cancel_me.wav";
    WriteFile(cs, WritePcm16Wav(Noise(44100, 30, -30.0, 12), 44100));
    int n = 0;
    Imported cr = Import(cs, gcache, kTargetLufs, [&](const Progress&) { return ++n < 5; });
    int files_before = 0;
    for (const auto& e : fs::directory_iterator(gcache)) files_before += e.is_regular_file();
    Check(!cr.ok && cr.cancelled, "cancelling from the progress callback stops the import (cancelled = true)");
    Imported after = Lookup(cs, gcache);
    Check(!after.ok && after.err.empty(), "  and leaves it 'not converted yet' (no index entry)");
    bool part = false;
    for (const auto& e : fs::directory_iterator(gcache)) part |= e.path().string().find(".part") != std::string::npos;
    Check(!part, "  no .part files are left behind");
    (void)files_before;
    // the cache hit path reports too and the later import after a cancel works
    Imported again = Import(cs, gcache);
    Check(again.ok, "  importing it again afterwards works");
  }

  if (const char* lc = std::getenv("LOOKUP_CASE")) {
    // A mods folder with a CJK name holding CJK-named songs, for lookup_only_test.exe: a binary that links lookup.cpp ONLY (no
    // decoder, no Import), proving the game's path never converts and nothing throws.
    const fs::path mods = fs::path(lc) / fs::path(u8"モッズ★唱");
    fs::remove_all(fs::path(lc));
    fs::create_directories(mods / "music" / "new");
    const fs::path cdir = mods / "music" / ".cache";
    WriteFile(mods / "music" / "new" / fs::path(u8"唱★converted.wav"), WritePcm16Wav(Noise(44100, 6, -25.0, 21), 44100));
    WriteFile(mods / "music" / "new" / fs::path(u8"notyet★.wav"), WritePcm16Wav(Noise(44100, 6, -25.0, 22), 44100));
    std::mt19937 jg(5);
    std::vector<uint8_t> junk(20000);
    for (auto& x : junk) x = uint8_t(jg());
    WriteFile(mods / "music" / "new" / fs::path(u8"壊れた.mp3"), junk);
    Imported a = Import(mods / "music" / "new" / fs::path(u8"唱★converted.wav"), cdir);
    Imported b = Import(mods / "music" / "new" / fs::path(u8"壊れた.mp3"), cdir);
    Check(a.ok && !b.ok, "lookup case prepared in a CJK-named mods folder (one converted, one refused, one not converted)");
  }

  std::printf("== names that do not fit the ANSI code page, errors, odd inputs\n");
  {
    const fs::path odd = tmp / fs::path(u8"唱★ソング.wav");  // CJK + a star: not in any single Western code page
    WriteFile(odd, WritePcm16Wav(Noise(44100, 6, -25.0, 2), 44100));
    bool threw = false;
    Imported ro;
    Song so;
    fs::path po;
    try {
      ro = Import(odd, tmp / "ocache");
      Imported rr = Resolve(odd, tmp / "ocache");
      so = Library(odd, tmp / "ocache");
      po = PlayableNow(odd, tmp / "ocache");
    } catch (...) {
      threw = true;
    }
    Check(!threw, "Import / Resolve / Library / PlayableNow on a CJK file name do not throw");
    Check(ro.ok, "  and the song imports");
    const auto want = fs::path(u8"唱★ソング").u8string();
    Check(so.ok && so.title == std::string(want.begin(), want.end()), "  the title falls back to the file name, as UTF-8");
    Check(!po.empty() && po != odd, "  PlayableNow returns the cache copy");
  }

  std::printf("== the length is checked before anything is decoded\n");
  {
    // 8 kHz mono 8-bit WAV of 21 minutes (10 MB of silence-level samples): over the 20-minute limit
    const uint32_t rate = 8000, secs = 21 * 60, data = rate * secs;
    std::vector<uint8_t> w(44 + data, 128);
    auto p32 = [&](size_t at, uint32_t v) { std::memcpy(&w[at], &v, 4); };
    auto p16 = [&](size_t at, uint16_t v) { std::memcpy(&w[at], &v, 2); };
    std::memcpy(&w[0], "RIFF", 4), p32(4, 36 + data), std::memcpy(&w[8], "WAVEfmt ", 8);
    p32(16, 16), p16(20, 1), p16(22, 1), p32(24, rate), p32(28, rate), p16(32, 1), p16(34, 8), std::memcpy(&w[36], "data", 4), p32(40, data);
    WriteFile(tmp / "long.wav", w);
    const uint64_t d0 = Stats().decodes.load();
    Imported rl = Import(tmp / "long.wav", tmp / "lcache");
    Check(!rl.ok && rl.err.find("20 minutes") != std::string::npos, "a 21-minute WAV is refused with the reason");
    Check(Stats().decodes.load() == d0, "  before any sample was decoded (decode counter unchanged)");
    // an Opus-free check for the others: the same rule for FLAC / MP3 / Vorbis uses the same helper (LengthOk) at the header
    // a source over 1 GB is refused before it is read: simulate with the limit via a sparse file
    {
      const fs::path big = tmp / "big.wav";
      {
        std::ofstream f(big, std::ios::binary);
        f.seekp((1ll << 30) + 1024);
        f.put('x');
      }
      Imported rb = Import(big, tmp / "lcache");
      Check(!rb.ok && rb.err.find("1 GB") != std::string::npos, "a file over 1 GB is refused before it is read");
      fs::remove(big);
    }
  }

  std::printf("== silent / short songs play as they are; cache write failures fall back to the source\n");
  {
    Imported s2 = Import(tmp / "silent.wav", cache);
    Check(s2.ok && !s2.normalised && s2.cache == tmp / "silent.wav" && !s2.note.empty(), "a silent song is not refused: it plays as it is (not normalised)");
    WriteFile(tmp / "short.wav", WritePcm16Wav(std::vector<float>(2 * 4000, 0.2f), 44100));
    Imported s3 = Import(tmp / "short.wav", cache);
    Check(s3.ok && !s3.normalised, "a song under 0.4 s is not refused either");
    // the cache folder cannot be created (a regular file sits where it should be): fall back to the source, do not remember it
    WriteFile(tmp / "blocker", {1, 2, 3});
    const uint64_t c0 = Stats().imports.load();
    Imported io1 = Resolve(tmp / "a.wav", tmp / "blocker" / "sub");
    Imported io2 = Resolve(tmp / "a.wav", tmp / "blocker" / "sub");
    Check(!io1.ok && io1.io_failed && !io1.cancelled, "a cache that cannot be written is found out BEFORE decoding: an I/O failure (a retry), not a refusal");
    Check(Stats().imports.load() - c0 == 1, "  an I/O failure is remembered for 60 s only: the second look does not re-convert in a loop", double(Stats().imports.load() - c0), 1);
    (void)io2;
    // several threads importing one new song into one cache: all succeed, no .part files are left
    WriteFile(tmp / "race.wav", WritePcm16Wav(Noise(44100, 8, -28.0, 6), 44100));
    std::vector<std::thread> th;
    std::atomic<int> okc{0};
    for (int i = 0; i < 4; ++i) th.emplace_back([&] { okc += Import(tmp / "race.wav", tmp / "rcache").ok ? 1 : 0; });
    for (auto& t : th) t.join();
    int parts = 0;
    for (const auto& e : fs::directory_iterator(tmp / "rcache")) parts += e.path().string().find(".part") != std::string::npos;
    Check(okc == 4 && parts == 0, "four threads importing one song: all ok, no .part files left", okc, 4);
  }

  std::printf("== case-insensitive keys, extension order, unknown lengths, stale files\n");
  {
#if defined(_WIN32)
    const fs::path up = tmp / "GAME_SONG.WAV";  // game_song.wav was converted above under its lower-case name
    Check(PlayableNow(up, tmp / "gcache") != up, "343.MP3 and 343.mp3 are one file: the cache found under another letter case");
#endif
    Check(std::string(kExts[0]) == ".mp3" && std::string(kExts[1]) == ".m4a" && std::string(kExts[2]) == ".wav", "one id with several files: the old priority (.mp3 .m4a .wav ...)");
    Check(!GamePlayerCanOpen("x.opus") && !GamePlayerCanOpen("x.ogg") && GamePlayerCanOpen("x.mp3"), "the game's player: .opus / .ogg cannot be opened as they are (the game's own theme plays instead)");
    // a streamed WAV that claims no length (data size 0) decodes under a running cap
    std::vector<uint8_t> w = WritePcm16Wav(Noise(44100, 3, -25.0, 8), 44100);
    uint32_t zero = 0;
    std::memcpy(&w[40], &zero, 4);
    std::memcpy(&w[4], &zero, 4);
    WriteFile(tmp / "stream0.wav", w);
    Imported r0 = Import(tmp / "stream0.wav", tmp / "scache");
    Check(r0.ok || !r0.err.empty(), "a WAV claiming no length is decoded or refused with a reason, never a crash");
    // stale .part files (older than 10 minutes) are removed by the launcher's pass; fresh ones are kept
    fs::create_directories(tmp / "stale");
    WriteFile(tmp / "stale" / "old.wav.part123", {1, 2, 3});
    WriteFile(tmp / "stale" / "new.wav.part456", {1, 2, 3});
    fs::last_write_time(tmp / "stale" / "old.wav.part123", fs::file_time_type::clock::now() - std::chrono::minutes(30));
    CleanStale(tmp / "stale");
    Check(!fs::exists(tmp / "stale" / "old.wav.part123") && fs::exists(tmp / "stale" / "new.wav.part456"), "CleanStale removes .part files older than 10 minutes and keeps fresh ones");
  }

  std::printf("== the index: keys relative to the music folder, cache version\n");
  {
    // a file outside the music folder (the cache folder's parent) is converted but never indexed: the game never reads it
    fs::create_directories(tmp / "mus" / "elsewhere");
    fs::create_directories(tmp / "mus" / "new");
    const fs::path ext = tmp / "elsewhere_song.wav", inside = tmp / "mus" / "new" / "In.wav";
    WriteFile(ext, WritePcm16Wav(Noise(44100, 5, -28.0, 31), 44100));
    WriteFile(inside, WritePcm16Wav(Noise(44100, 5, -28.0, 32), 44100));
    const fs::path ic = tmp / "mus" / ".cache";
    Imported e1 = Import(ext, ic), e2 = Import(inside, ic);
    Check(e1.ok && e2.ok, "both import");
    Imported l1 = Lookup(ext, ic), l2 = Lookup(inside, ic);
    Check(!l1.ok && l1.err.empty(), "a file outside the music folder has no index entry (not converted, for the game)");
    {
      const std::vector<uint8_t> idx = ReadFile(ic / "index.txt");
      const std::string t(idx.begin(), idx.end());
      Check(t.find("new/In.wav") != std::string::npos, "index.txt stores the path in its ORIGINAL case (only the lookup key is folded)");
    }
    Check(l2.ok && l2.normalised && Lookup(tmp / "mus" / "NEW" / "in.WAV", ic).ok, "a file inside has one, found whatever the letter case, by its path relative to the music folder");
    // pruning: a deleted song's entry goes at the next CleanStale (once per pass), the others stay
    WriteFile(tmp / "mus" / "new" / "Gone.wav", WritePcm16Wav(Noise(44100, 5, -28.0, 34), 44100));
    Check(Import(tmp / "mus" / "new" / "Gone.wav", ic).ok, "a song to delete imports");
    fs::remove(tmp / "mus" / "new" / "Gone.wav");
    CleanStale(ic);
    {
      const std::vector<uint8_t> idx = ReadFile(ic / "index.txt");
      const std::string t(idx.begin(), idx.end());
      Check(t.find("Gone.wav") == std::string::npos && t.find("new/In.wav") != std::string::npos, "CleanStale drops the entry of a deleted song and keeps the others");
    }
    // another cache version: every entry is stale (a song nobody has looked up yet, so no memo hides the index)
    WriteFile(tmp / "mus" / "new" / "Three.wav", WritePcm16Wav(Noise(44100, 5, -28.0, 33), 44100));
    Check(Import(tmp / "mus" / "new" / "Three.wav", ic).ok, "a third song imports");
    {
      std::vector<uint8_t> idx = ReadFile(ic / "index.txt");
      std::string t(idx.begin(), idx.end());
      const size_t at = t.find(" v" + std::to_string(kCacheVersion));
      Check(at != std::string::npos, "index.txt carries the cache version on its first line");
      if (at != std::string::npos) {
        t.replace(at, 2 + std::to_string(kCacheVersion).size(), " v9");
        std::ofstream o(ic / "index.txt", std::ios::binary | std::ios::trunc);
        o << t;
        o.close();
        fs::last_write_time(ic / "index.txt", fs::file_time_type::clock::now() + std::chrono::seconds(5));
        fs::remove(ic.parent_path() / "x");
        // the memo of the in-process Resolve is not involved: Import does not fill it
        Imported l3 = Lookup(tmp / "mus" / "new" / "Three.wav", ic);
        Check(!l3.ok && l3.err.empty(), "an index written by another cache version = 'not converted yet' for every song");
      }
    }
  }

  std::printf("== channel order, front cover\n");
  {
    // Ogg Vorbis 5.1 is L C R Ls Rs LFE: a signal only on the centre must fold equally to both sides
    std::vector<float> six(6 * 100, 0.f);
    for (int n = 0; n < 100; ++n) six[size_t(n) * 6 + 1] = 1.0f;
    auto v = ToStereo(std::vector<float>(six), 6, true);
    Check(std::fabs(v[0] - v[1]) < 1e-6f && v[0] > 0.3f, "Vorbis 5.1 order: the centre channel folds to both sides");
    auto w = ToStereo(std::vector<float>(six), 6, false);  // the WAV order reads that same data as R = 1
    Check(w[0] == 0.f && w[1] > 0.3f, "WAV 5.1 order reads index 1 as the right channel");
    // two APIC frames, the first "other" (type 0), the second the front cover (type 3): the front cover wins
    using Bytes = std::vector<uint8_t>;
    Bytes frames;
    auto frame = [&](const Bytes& body) {
      const char* id = "APIC";
      frames.insert(frames.end(), id, id + 4);
      for (int s = 24; s >= 0; s -= 8) frames.push_back(uint8_t(body.size() >> s));
      frames.push_back(0), frames.push_back(0);
      frames.insert(frames.end(), body.begin(), body.end());
    };
    auto apic = [&](int type, const Bytes& img) {
      Bytes b = {0};
      const std::string mime = "image/png";
      b.insert(b.end(), mime.begin(), mime.end());
      b.push_back(0), b.push_back(uint8_t(type)), b.push_back(0);
      b.insert(b.end(), img.begin(), img.end());
      return b;
    };
    const Bytes imgA(40, 0xAA), imgB(50, 0xBB);
    frame(apic(0, imgA)), frame(apic(3, imgB));
    Bytes f = {'I', 'D', '3', 3, 0, 0};
    const size_t n = frames.size();
    f.push_back(uint8_t(n >> 21 & 0x7F)), f.push_back(uint8_t(n >> 14 & 0x7F)), f.push_back(uint8_t(n >> 7 & 0x7F)), f.push_back(uint8_t(n & 0x7F));
    f.insert(f.end(), frames.begin(), frames.end());
    f.resize(f.size() + 16, 0);
    Check(ReadTags(f).picture == imgB, "ID3 with two pictures: the front cover (type 3) is chosen, not the first");
  }

  if (mem) {
    std::printf("== memory: a 20-minute 48 kHz stereo 16-bit WAV (peak working set of this process)\n");
    const fs::path big = tmp / "twenty_minutes.wav";
    {
      const uint32_t rate = 48000, secs = 20 * 60 - 1, data = rate * secs * 4;
      std::ofstream f(big, std::ios::binary);
      std::vector<uint8_t> h(44);
      auto p32 = [&](size_t at, uint32_t v) { std::memcpy(&h[at], &v, 4); };
      auto p16 = [&](size_t at, uint16_t v) { std::memcpy(&h[at], &v, 2); };
      std::memcpy(&h[0], "RIFF", 4), p32(4, 36 + data), std::memcpy(&h[8], "WAVEfmt ", 8);
      p32(16, 16), p16(20, 1), p16(22, 2), p32(24, rate), p32(28, rate * 4), p16(32, 4), p16(34, 16), std::memcpy(&h[36], "data", 4), p32(40, data);
      f.write(reinterpret_cast<const char*>(h.data()), 44);
      std::mt19937 g(3);
      std::vector<int16_t> chunk(48000 * 2);
      for (uint32_t s = 0; s < secs; ++s) {
        for (size_t i = 0; i < chunk.size(); ++i) chunk[i] = int16_t(int(g() % 6000) - 3000 + int(2000.0 * std::sin(double(s * 48000 + i / 2) * 0.03)));
        f.write(reinterpret_cast<const char*>(chunk.data()), std::streamsize(chunk.size() * 2));
      }
    }
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc);
    const double base = double(pmc.PeakWorkingSetSize) / 1e9;
    const auto t0 = std::chrono::steady_clock::now();
    Imported rm = Import(big, tmp / "mcache");
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc);
    std::printf("        file %.0f MB, import %.1f s, peak working set %.2f GB (before the import %.2f GB), ok=%d %.1f LUFS limited=%d\n",
                double(fs::file_size(big)) / 1e6, sec, double(pmc.PeakWorkingSetSize) / 1e9, base, rm.ok, rm.out_lufs, rm.limited);
    Check(rm.ok && double(pmc.PeakWorkingSetSize) / 1e9 < 0.8, "a 20-minute 48 kHz song imports in under 0.8 GB peak", double(pmc.PeakWorkingSetSize) / 1e9, 0.8);
#endif
    fs::remove_all(tmp);
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
  }

  if (samples) {
    std::printf("== sample files in %s\n", samples);
    std::ifstream man(fs::path(samples) / "manifest.txt");
    std::string name, expect;
    while (man >> name >> expect) {
      Imported r = Import(fs::path(samples) / name, cache);
      const bool want_ok = expect == "ok" || expect == "plays";
      char what[160];
      std::snprintf(what, sizeof what, "%s: %s", name.c_str(), want_ok ? "imports" : "refused");
      Check(r.ok == want_ok, what);
      if (r.ok && expect == "plays") {
        Check(!r.normalised && r.cache == fs::path(samples) / name, "  it plays as it is (not normalised)");
      } else if (r.ok) {
        std::printf("        %s: %.1f LUFS %.1f dBTP -> gain %+.1f dB%s -> %.1f LUFS %.1f dBTP, %.1f s @ %d\n", name.c_str(), r.src_lufs,
                    r.src_true_peak_db, r.gain_db, r.limited ? " (limited)" : "", r.out_lufs, r.out_true_peak_db, r.seconds, r.rate);
        Check(Near(r.out_lufs, kTargetLufs, 0.5) || r.limited, "  lands at the target", r.out_lufs, kTargetLufs);
        const bool want_art = name.find("_art") != std::string::npos;
        std::printf("        tags: title '%s' artist '%s', thumbnail %s\n", r.title.c_str(), r.artist.c_str(), r.thumb.empty() ? "none" : r.thumb.filename().string().c_str());
        Check(want_art ? (!r.thumb.empty() && fs::file_size(r.thumb) > 100) : r.thumb.empty(), want_art ? "  the embedded cover became a thumbnail" : "  no cover -> no thumbnail file");
        const bool tagged = name != "song.wav" && name != "song.aac";
        if (tagged && name != "song.wma") Check(r.title == "Sample Song" && r.artist == "Sample Artist", "  title and artist read from the tags");
        {  // a cache HIT (nothing converted) must return the same tags and thumbnail as the conversion did
          Imported h = Import(fs::path(samples) / name, cache);
          Imported pk = Peek(fs::path(samples) / name, cache);
          Check(h.from_cache && h.title == r.title && h.artist == r.artist && h.album == r.album && h.thumb == r.thumb,
                "  a cache hit keeps title / artist / album / thumbnail");
          Check(pk.ok && pk.title == r.title && pk.artist == r.artist && pk.album == r.album, "  Peek (cache only) returns the same tags");
        }
      } else {
        std::printf("        refused: %s\n", r.err.c_str());
      }
    }
  }
  fs::remove_all(tmp);
  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
