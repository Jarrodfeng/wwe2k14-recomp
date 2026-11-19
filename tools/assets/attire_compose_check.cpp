// S103 CMB: the offline proof's C++ half for src\assets\attire_compose.* (docs\research\s103\CMP.md adversary "Exact checks for the builder"). 0 game runs.
//   attire_compose_check compose DIR OUT.rgba [--fault NAME] [--white keep|paint] [--drop 13]
//        DIR holds base.dds, mask13.dds .. mask16.dds (the PACH's entries 12 .. 16, plain DDS files) and colours.bin (48 bytes), written by
//        tools\assets\attire_compose_dump.py from the player's own pac. Writes level 0 as raw RGBA8 (w, h on stdout). --drop N leaves mask entry N out
//        (a fake PACH without it: the enumerate-shift fault); --zero N replaces mask N by an all-zero mask (what a correct k-by-entry build equals).
//   attire_compose_check unit [--fault NAME]
//        the hook read (ReadComposite) and the tag / registry on a fake guest memory; exit 0 = every check passed.
// Fault names (planted: the check around them must FAIL): swap_rb rotl4 enumerate white_keep_default compose_mode0 compose_mode2.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "../../src/assets/attire_compose.h"

using namespace attire_compose;

static std::vector<uint8_t> ReadAll(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static Fault ParseFault(const std::string& s) {
  if (s == "swap_rb") return Fault::kSwapRB;
  if (s == "rotl4") return Fault::kRotl4;
  if (s == "enumerate") return Fault::kEnumerate;
  if (s == "white_keep_default") return Fault::kWhiteKeepDefault;
  if (s == "compose_mode0") return Fault::kComposeMode0;
  if (s == "compose_mode2") return Fault::kComposeMode2;
  return Fault::kNone;
}

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; std::printf("FAIL line %d: %s  ", __LINE__, #c); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

struct Mem {
  std::map<uint32_t, uint32_t> w;
  bool operator()(uint32_t a, uint32_t& v) const {
    auto it = w.find(a);
    if (it == w.end()) return false;
    v = it->second;
    return true;
  }
};

static void PutColours(Mem& m, uint32_t at, uint8_t seed) {
  for (int i = 0; i < 12; ++i) {
    uint32_t v = 0;
    for (int b = 0; b < 4; ++b) v = (v << 8) | uint8_t(seed + 4 * i + b);
    m.w[at + 4 * uint32_t(i)] = v;
  }
}

// reader 0x1000; desc 0x2000; P 0x30000; block 0x40000. colour set `set` holds bytes seed(set), seed + 1, ... (so a wrong slot offset shows).
static Mem Scene(uint32_t mode, int32_t set, uint32_t chosen = 24912, uint32_t loaded = 24902) {
  Mem m;
  m.w[0x1000 + 96] = 0x2000, m.w[0x1000 + 100] = 0x30000;
  m.w[0x2000 + 28] = loaded, m.w[0x2000 + 44] = mode, m.w[0x2000 + 48] = uint32_t(set), m.w[0x2000 + 92] = chosen;
  m.w[0x30000 + 16] = 0x40000;
  for (int s = 0; s < 16; ++s) PutColours(m, 0x40000 + 132 * uint32_t(s) + 1320, uint8_t(16 * s));
  return m;
}

static int Unit(Fault fault) {
  Link l;
  {  // a composite (mode 1) at set 1: the 48 bytes at block + 132 + 1320, in memory order, the chosen / loaded keys carried
    Mem m = Scene(1, 1);
    CHECK(ReadComposite(m, 0x1000, l, fault) == Read::kComposite, "mode 1 set 1");
    uint8_t want[48];
    for (int i = 0; i < 48; ++i) want[i] = uint8_t(16 + i);
    CHECK(std::memcmp(l.colours, want, 48) == 0, "colour bytes of set 1 (slot offset or byte order)");
    CHECK(l.chosen == 24912 && l.loaded == 24902 && l.mode == 1 && l.set == 1, "carried fields %u %u %u %d", l.chosen, l.loaded, l.mode, l.set);
    CHECK(l.hash == HashColours(want), "hash");
  }
  {  // set 0 differs (slot 0's bytes must not be taken for set 1's: the "slot 0 colours" fault)
    Mem m = Scene(1, 0);
    Link z;
    CHECK(ReadComposite(m, 0x1000, z, fault) == Read::kComposite && z.colours[0] == 0 && z.colours[47] == 47, "set 0 bytes");
    CHECK(std::memcmp(z.colours, l.colours, 48) != 0, "set 0 and set 1 differ");
  }
  {  // mode 0 and mode 2 = the base as is (the collision case: Heenan a0 24902 stays the stock attire)
    Link z;
    CHECK(ReadComposite(Scene(0, 1), 0x1000, z, fault) == Read::kBase, "mode 0 must stay base");
    CHECK(ReadComposite(Scene(2, 1), 0x1000, z, fault) == Read::kBase, "mode 2 must stay base");
    CHECK(ReadComposite(Scene(3, 1), 0x1000, z, fault) == Read::kComposite, "mode 3 composes (any mode but 0 / 2)");
  }
  {  // every refusal, once
    Link z;
    Mem m = Scene(1, 1);
    m.w[0x1000 + 96] = 0;
    CHECK(ReadComposite(m, 0x1000, z, fault) == Read::kNoDesc, "desc 0");
    m = Scene(1, 1), m.w.erase(0x1000 + 96);
    CHECK(ReadComposite(m, 0x1000, z, fault) == Read::kUnreadable, "desc unreadable");
    m = Scene(1, 1), m.w[0x1000 + 100] = 0xFFFF;
    CHECK(ReadComposite(m, 0x1000, z, fault) == Read::kBadP, "P below 0x10000");
    m = Scene(1, 1), m.w[0x30000 + 16] = 0;
    CHECK(ReadComposite(m, 0x1000, z, fault) == Read::kBadBlock, "block null");
    m = Scene(1, -1);
    CHECK(ReadComposite(m, 0x1000, z, fault) == Read::kBadSet, "set -1");
    m = Scene(1, 16);
    CHECK(ReadComposite(m, 0x1000, z, fault) == Read::kBadSet, "set 16");
    m = Scene(1, 1), m.w.erase(0x40000 + 132 + 1320 + 20);
    CHECK(ReadComposite(m, 0x1000, z, fault) == Read::kUnreadable, "a colour word unreadable");
    m = Scene(1, 1), m.w[0x30000 + 16] = 0xFFFFFF00u;
    CHECK(ReadComposite(m, 0x1000, z, fault) == Read::kBadBlock, "block + offset past 4 GB");
    m = Scene(0, -1);   // a base read never inspects the set
    CHECK(ReadComposite(m, 0x1000, z, fault) == Read::kBase, "mode 0 with a junk set is still base");
  }
  {  // the tag
    uint8_t c[48], back[48];
    for (int i = 0; i < 48; ++i) c[i] = uint8_t(255 - 5 * i);
    const std::string e = "model_key:24902" + EntryTag(c);
    int key = 0;
    CHECK(ParseEntryTag(e, back) && std::memcmp(c, back, 48) == 0, "tag round trip");
    CHECK(std::sscanf(e.c_str(), "model_key:%d", &key) == 1 && key == 24902, "sscanf still parses the suffixed entry (%d)", key);
    CHECK(!ParseEntryTag("model_key:24902", back), "no tag, no parse");
    CHECK(!IsRefusalTag(e) && IsRefusalTag("model_key:24902" + std::string(RefusalTag())) && !ParseEntryTag("model_key:24902/r", back), "the refusal tag is its own form");
    // two bodies with the same colours but another chosen key share one entry (chosen is not in the tag: the colours alone decide the picture)
    CHECK(EntryTag(c) == EntryTag(c), "same colours, same tag");
  }
  {  // LinkTag: a composite gives the tag the contributor stores in the body link; a base read gives none; a refusal gives the refusal tag (a hole) and is counted
    std::string line;
    Mem m = Scene(1, 1);
    const std::string t = LinkTag(m, 0x1000, line);
    uint8_t cb[48];
    CHECK(ParseEntryTag(t, cb) && cb[0] == 16 && line.find("composed") != std::string::npos, "tag %s line %s", t.c_str(), line.c_str());
    CHECK(LinkTag(Scene(0, 1), 0x1000, line).empty() && LinkTag(Scene(2, 1), 0x1000, line).empty(), "mode 0 / 2: no tag");
    CHECK(LinkTag(Scene(1, 99), 0x1000, line) == "/r" && Count().n[int(Read::kBadSet)].load() >= 1 && line.find("refused") != std::string::npos, "a bad set gives the refusal tag, is counted and said: %s", line.c_str());
    Mem bp = Scene(1, 1);
    bp.w[0x1000 + 100] = 0xFFFF;
    Mem nd = Scene(1, 1);
    nd.w.erase(0x1000 + 96);
    Mem bb = Scene(1, 1);
    bb.w[0x30000 + 16] = 0;
    CHECK(LinkTag(bp, 0x1000, line) == "/r" && LinkTag(nd, 0x1000, line) == "/r" && LinkTag(bb, 0x1000, line) == "/r", "bad P / unreadable desc / bad block: each the refusal tag, never the plain base");
  }
  {  // S103 ARM (HNN open question): mode 0 and mode 2 are printed apart and counted apart; the diagnosis words and the first build step's re-read are said
    std::string line;
    Mem m0 = Scene(0, 1), m2 = Scene(2, 1);
    const uint64_t b0 = Count().base_mode0.load(), b2 = Count().base_mode2.load();
    LinkTag(m0, 0x1000, line);
    CHECK(line.find("link: base (mode 0)") != std::string::npos && !(line.find("mode 2") != std::string::npos), "a mode 0 base read is printed as mode 0: %s", line.c_str());
    LinkTag(m2, 0x1000, line);
    CHECK(line.find("link: base (mode 2)") != std::string::npos && !(line.find("(mode 0)") != std::string::npos), "a mode 2 base read is printed as mode 2: %s", line.c_str());
    CHECK(Count().base_mode0.load() == b0 + 1 && Count().base_mode2.load() == b2 + 1, "mode 0 and mode 2 base reads are counted apart");
    CHECK(line.find("diag chosen 24912: desc+28 24902 +44 2 +48 1 +92 24912") != std::string::npos, "the diagnosis words (desc+28 / +44 / +48 / +92) ride on the base line of a remapped attire: %s", line.c_str());
    Mem mp = Scene(0, 1);
    mp.w[0x30000 + 40] = 1, mp.w[0x30000 + 44] = 0x01000000u, mp.w[0x30000 + 60] = 0xC800, mp.w[0x30000 + 64] = 24902;
    LinkTag(mp, 0x1000, line);
    CHECK(line.find("P+40 1 P+44 1 P+60 0xC800 P+64 0x6146") != std::string::npos, "the P words (P+40 / +44 s8 / +60 / +64): %s", line.c_str());
    Mem stock = Scene(0, 0, 24902, 24902);
    LinkTag(stock, 0x1000, line);
    CHECK(line.find("diag") == std::string::npos, "a stock attire (loaded == chosen, not 24912) adds no diagnosis line: %s", line.c_str());
    Mem step;
    step.w[0x5000 + 100] = 0x30000, step.w[0x5000 + 140] = 0x22000, step.w[0x22000 + 92] = 24912, step.w[0x22000 + 28] = 24902;
    step.w[0x30000 + 40] = 1, step.w[0x30000 + 44] = 0x01000000u, step.w[0x30000 + 60] = 0xC800, step.w[0x30000 + 64] = 0xC800;
    CHECK(!BuildStepLine(step, 0x5000, 1, line) && BuildStepLine(step, 0x5000, 0, line) && line.find("build step 0: desc+92 24912") != std::string::npos && line.find("P+40 1") != std::string::npos, "the build step's re-read (first step only): %s", line.c_str());
  }
  CHECK(Options{}.white == White::kPaint, "the default is paint (255 = white)");
  std::printf("%s\nunit: %s\n", CountersLine().c_str(), fails ? "FAIL" : "ok");
  return fails ? 1 : 0;
}

int main(int argc, char** argv) {
  if (argc < 2) return 2;
  Fault fault = Fault::kNone;
  Options opt;
  std::string mode = argv[1];
  for (int i = 2; i < argc; ++i)
    if (!std::strcmp(argv[i], "--fault") && i + 1 < argc) fault = ParseFault(argv[i + 1]);
  opt.fault = fault;
  if (mode == "unit") {
    return Unit(fault);
  }
  if (mode != "compose" || argc < 4) return 2;
  const std::string dir = argv[2];
  Input in;
  std::vector<uint8_t> base = ReadAll(dir + "/base.dds"), m[4], cols = ReadAll(dir + "/colours.bin");
  if (base.empty() || cols.size() != 48) { std::printf("missing base.dds / colours.bin in %s\n", dir.c_str()); return 3; }
  in.base = base.data(), in.base_n = base.size();
  std::memcpy(in.colours, cols.data(), 48);
  int drop = -1, zero = -1;
  opt.bc1 = false;   // the proof compares RGBA8; --bc1 composes BC1 and writes its decoded level 0
  bool want_bc1 = false;
  for (int i = 4; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--white") && i + 1 < argc) opt.white = std::strcmp(argv[i + 1], "keep") == 0 ? White::kKeep : White::kPaint;
    if (!std::strcmp(argv[i], "--masks") && i + 1 < argc) {   // TEST ONLY: R,G,B,A hex bit masks, e.g. F000,000F,00F0,0F00
      unsigned a = 0, b = 0, c = 0, d = 0;
      if (std::sscanf(argv[i + 1], "%x,%x,%x,%x", &a, &b, &c, &d) == 4) opt.test_masks[0] = a, opt.test_masks[1] = b, opt.test_masks[2] = c, opt.test_masks[3] = d;
    }
    if (!std::strcmp(argv[i], "--bc1")) opt.bc1 = want_bc1 = true;
    if (!std::strcmp(argv[i], "--drop") && i + 1 < argc) drop = std::atoi(argv[i + 1]);
    if (!std::strcmp(argv[i], "--zero") && i + 1 < argc) zero = std::atoi(argv[i + 1]);
  }
  for (int k = 0; k < 4; ++k) {
    m[k] = ReadAll(dir + "/mask" + std::to_string(13 + k) + ".dds");
    if (m[k].empty() || 13 + k == drop) continue;
    if (13 + k == zero && m[k].size() > 128) std::memset(m[k].data() + 128, 0, m[k].size() - 128);
    in.mask[k] = m[k].data(), in.mask_n[k] = m[k].size();
  }
  Image img;
  std::string why;
  if (!Compose(in, opt, img, why)) { std::printf("compose failed: %s\n", why.c_str()); return 4; }
  std::ofstream o(argv[3], std::ios::binary);
  std::vector<uint8_t> rgba;
  if (want_bc1) {
    if (!img.bc1 || !DecodeBc1Level(img.data.data(), img.data.size(), img.w, img.h, rgba)) { std::printf("bc1 decode failed\n"); return 5; }
  } else rgba = img.data;
  o.write(reinterpret_cast<const char*>(rgba.data()), std::streamsize(size_t(img.w) * img.h * 4));
  std::printf("%u %u %u bytes %zu\n", img.w, img.h, img.mips, img.data.size());
  return 0;
}
