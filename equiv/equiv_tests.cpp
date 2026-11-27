// S111 EQUIV piece B (docs\research\s111\EQUIV0.md): the equivalence tester's test DLL. Each test calls the game's own recompiled function
// through the runner's host table (the C interface of EQUIV0's "Build plan"; src\equiv\equiv_api.h, piece A) and our port of the same rule
// (a header-only core of ours, #included from src) on the same inputs, and compares their outputs exactly (integers by value, floats by bits).
// Built by tools\equiv\build_equiv.ps1 (the CMake target wwe2k14_equiv_tests next to the exe; -Fake: beside tools\equiv\fake_host.cpp, the host
// stand-in that runs before piece A lands). Driven by tools\equiv\equiv.py. Contains nothing from the game. Copyright (c) the wwe2k14-recomp
// authors. BSD-3-Clause.
//
// One test = a target address, its input fields (named by q decomp's operand text when the field is one: [in_r3+24], [0x8392DC74],
// ret#sub_X = that stubbed callee's return), the callees it stubs (coverage tier only: host->stub returns 0 elsewhere, and the test is then
// reported not isolable), the game side (inputs into guest memory and registers, the call, outputs read back) and ours. A planted twin
// (twin_of) shares the game side and runs a model that is wrong on purpose: the driver's selftest needs it REFUTED.
// Cases: case 0 = every field at its default, then seeded random cases (defaults, the branch constants and their neighbours, the range
// ends, uniform). WWE2K14_EQUIV_PINS="name=value;..." pins fields in every case (the driver's branch steering); WWE2K14_EQUIV_SEED overrides
// the seed. On the first difference the case is shrunk (fields back to their defaults while it still differs, then each left field to its
// smallest failing value) and reported.
//
// Report lines (tools\equiv\equiv.py reads them):
//   equiv <test> target 0x<addr> seed <s> cases <n>
//   result EQUAL | DIFFER | NOT-ISOLABLE
//   equal <n>               (cases run equal)
//   why <text>              (NOT-ISOLABLE)
//   case <i>                (DIFFER: the first differing case's index)
//   shrunk <f>=<v> ...      (DIFFER: the fields off their defaults in the shrunk case; "(all defaults)" when none)
//   out <name> game=<v> ours=<v>   (DIFFER: each differing output of the shrunk case)
//   pin-unknown <name>      (a pin that names no field of the test)
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "equiv/equiv_api.h"   // the host table (piece A's header, the one C interface)

#include <stb_image.h>   // the declarations pac.h decodes with (the implementation: equiv_stb.cpp)

#include "menus100_pre.h"   // M100-COMP: our menu cores (system headers must come before namespace equiv)

#include "assets/arena_levels.h"       // arena_levels::Rule::Of (ARENALEVELS / LEVELSBYTE: the MipmapData row's byte 31, sign-extended)
#include "assets/arena_props_table.h"  // arena_props::ReadBlock / PlacementIds (ARENAFIX: the 36-byte placement records)

#define EQUIV_EXPORT extern "C" __declspec(dllexport)

namespace equiv {

constexpr int kApiVersion = 1;
const Wwe2k14EquivHost* H = nullptr;

// ---- guest memory: big-endian words at membase + guest address ----
inline uint8_t* G(uint32_t a) { return H->membase + a; }
inline uint32_t R32(uint32_t a) { uint32_t v; std::memcpy(&v, G(a), 4); return __builtin_bswap32(v); }
inline uint16_t R16(uint32_t a) { uint16_t v; std::memcpy(&v, G(a), 2); return __builtin_bswap16(v); }
inline uint8_t R8(uint32_t a) { return *G(a); }
inline void W32(uint32_t a, uint32_t v) { v = __builtin_bswap32(v); std::memcpy(G(a), &v, 4); }
inline void W8(uint32_t a, uint8_t v) { *G(a) = v; }
inline uint32_t Bits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
inline float RF(uint32_t a) { const uint32_t u = R32(a); float f; std::memcpy(&f, &u, 4); return f; }
inline void WF(uint32_t a, float f) { W32(a, Bits(f)); }

// ---- fields, cases, outputs ----
enum class Kind { Int, F32 };
struct Field {
  std::string name;
  Kind kind = Kind::Int;
  double lo = 0, hi = 0, def = 0;
  std::vector<double> hints;   // the compare constants of the function's branches: the generator favours them and their neighbours
};
using Case = std::vector<double>;
using Out = std::map<std::string, std::string>;   // each output as exact text (a float with its bits)

inline std::string F32Text(float f) {
  char b[64];
  std::snprintf(b, sizeof b, "%.9g/0x%08X", double(f), Bits(f));
  return b;
}
inline std::string F64Text(double d) {
  uint64_t u;
  std::memcpy(&u, &d, 8);
  char b[64];
  std::snprintf(b, sizeof b, "%.17g/0x%016llX", d, (unsigned long long)u);
  return b;
}
inline std::string IntText(long long v) { return std::to_string(v); }
inline std::string HexText(uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof b, "0x%08X", v);
  return b;
}

struct Rng {
  uint64_t s;
  uint64_t Next() {
    uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  double Unit() { return double(Next() >> 11) * 0x1.0p-53; }
};

struct Test;
// The game side: inputs in, the call, outputs out. Returns the host call's code (1 returned, 0 guest fault, -1 left the allowlist) or -2 =
// not isolable (`why` says so).
using GameFn = int (*)(const Test&, const Case&, Out&, std::string& why);
using OursFn = void (*)(const Test&, const Case&, Out&);
using StubFn = int (*)(const Wwe2k14EquivHost*);

struct Test {
  std::string name;
  uint32_t target = 0;
  std::string what;
  std::vector<Field> fields;
  std::vector<std::pair<uint32_t, StubFn>> stubs;
  GameFn game = nullptr;
  OursFn ours = nullptr;
  std::string twin_of;   // non-empty: a planted twin of that test (the same game side, a model wrong on purpose)

  int Index(const std::string& n) const {
    for (size_t i = 0; i < fields.size(); ++i)
      if (fields[i].name == n) return int(i);
    return -1;
  }
  double V(const Case& c, const std::string& n) const {
    const int i = Index(n);
    return i < 0 ? 0.0 : c[size_t(i)];
  }
  long long I(const Case& c, const std::string& n) const { return (long long)V(c, n); }
  float F(const Case& c, const std::string& n) const { return float(V(c, n)); }
};

// The case being run, for the stubs (a stub function gets only the host table).
const Test* g_test = nullptr;
const Case* g_case = nullptr;

Field IntField(const std::string& n, double lo, double hi, double def, std::vector<double> hints = {}) {
  Field f;
  f.name = n, f.kind = Kind::Int, f.lo = lo, f.hi = hi, f.def = def, f.hints = std::move(hints);
  return f;
}
Field FloatField(const std::string& n, double lo, double hi, double def, std::vector<double> hints = {}) {
  Field f;
  f.name = n, f.kind = Kind::F32, f.lo = lo, f.hi = hi, f.def = double(float(def)), f.hints = std::move(hints);
  return f;
}

double Clamp(const Field& f, double v) { return v < f.lo ? f.lo : v > f.hi ? f.hi : v; }

double Gen(const Field& f, Rng& r) {
  if (f.lo == f.hi) return f.lo;
  const double u = r.Unit();
  if (u < 0.2) return f.def;
  if (u < 0.45 && !f.hints.empty()) {
    const double h = f.hints[size_t(r.Next() % f.hints.size())];
    const int k = int(r.Next() % 3);
    if (f.kind == Kind::F32) {
      const float hf = float(h);
      return double(k == 0 ? hf : k == 1 ? std::nextafterf(hf, INFINITY) : std::nextafterf(hf, -INFINITY));
    }
    return Clamp(f, h + double(k - 1));
  }
  if (u < 0.55) return (r.Next() & 1) ? f.lo : f.hi;
  if (f.kind == Kind::Int) return f.lo + double(r.Next() % uint64_t(f.hi - f.lo + 1));
  return double(float(f.lo + (f.hi - f.lo) * r.Unit()));
}

std::string ValueText(const Field& f, double v) { return f.kind == Kind::Int ? IntText((long long)v) : F32Text(float(v)); }

// ---- pins (WWE2K14_EQUIV_PINS) ----
std::vector<std::pair<std::string, double>> Pins() {
  std::vector<std::pair<std::string, double>> out;
  const char* e = std::getenv("WWE2K14_EQUIV_PINS");
  if (!e) return out;
  std::string s = e;
  size_t p = 0;
  while (p < s.size()) {
    size_t q = s.find(';', p);
    if (q == std::string::npos) q = s.size();
    const std::string item = s.substr(p, q - p);
    const size_t eq = item.rfind('=');
    if (eq != std::string::npos && eq > 0) out.push_back({item.substr(0, eq), std::strtod(item.c_str() + eq + 1, nullptr)});
    p = q + 1;
  }
  return out;
}

// ---- one comparison ----
// 1 = equal, 0 = differ, -1 = the game side did not return (code in `rc`, reason in `why`).
int Compare(const Test& t, const Case& c, Out& g, Out& o, int& rc, std::string& why) {
  g.clear(), o.clear();
  g_test = &t, g_case = &c;
  H->scratch_reset();
  rc = t.game(t, c, g, why);
  if (rc != 1) return -1;
  t.ours(t, c, o);
  return g == o ? 1 : 0;
}

Case Shrink(const Test& t, Case c, const std::vector<bool>& pinned) {
  auto fails = [&](const Case& k) {
    Out g, o;
    int rc = 0;
    std::string why;
    return Compare(t, k, g, o, rc, why) == 0;
  };
  for (bool changed = true; changed;) {
    changed = false;
    for (size_t i = 0; i < c.size(); ++i) {
      if (pinned[i] || c[i] == t.fields[i].def) continue;
      Case k = c;
      k[i] = t.fields[i].def;
      if (fails(k)) c = k, changed = true;
    }
  }
  for (size_t i = 0; i < c.size(); ++i) {
    const Field& f = t.fields[i];
    if (pinned[i] || c[i] == f.def) continue;
    if (f.kind == Kind::Int) {
      int tries = 0;
      for (double v = f.lo; v < c[i] && tries < 300; v += 1, ++tries) {
        Case k = c;
        k[i] = v;
        if (fails(k)) {
          c = k;
          break;
        }
      }
    } else {
      for (double v : {0.0, 1.0, -1.0}) {
        if (v < f.lo || v > f.hi || std::fabs(v) >= std::fabs(c[i])) continue;
        Case k = c;
        k[i] = v;
        if (fails(k)) {
          c = k;
          break;
        }
      }
    }
  }
  return c;
}

int Run(const Test& t, uint64_t seed, int cases, std::string& rep) {
  if (const char* e = std::getenv("WWE2K14_EQUIV_SEED")) seed = std::strtoull(e, nullptr, 0);
  if (cases <= 0) cases = 200;
  char line[256];
  std::snprintf(line, sizeof line, "equiv %s target 0x%08X seed %llu cases %d\n", t.name.c_str(), t.target, (unsigned long long)seed, cases);
  rep += line;
  std::vector<bool> pinned(t.fields.size(), false);
  Case pins(t.fields.size(), 0.0);
  for (const auto& [n, v] : Pins()) {
    const int i = t.Index(n);
    if (i < 0) {
      rep += "pin-unknown " + n + "\n";
      continue;
    }
    pinned[size_t(i)] = true, pins[size_t(i)] = v;
  }
  for (const auto& [addr, fn] : t.stubs) {
    if (!H->stub || H->stub(addr, fn) == 0) {
      std::snprintf(line, sizeof line, "result NOT-ISOLABLE\nwhy the stub of sub_%08X needs the coverage tier (host stub returned 0)\n", addr);
      rep += line;
      return 3;
    }
  }
  Rng rng{seed};
  int equal = 0;
  for (int i = 0; i < cases; ++i) {
    Case c(t.fields.size());
    for (size_t k = 0; k < c.size(); ++k) c[k] = pinned[k] ? pins[k] : i == 0 ? t.fields[k].def : Gen(t.fields[k], rng);
    Out g, o;
    int rc = 0;
    std::string why;
    const int r = Compare(t, c, g, o, rc, why);
    if (r < 0) {
      rep += "result NOT-ISOLABLE\n";
      rep += "why " + (rc == 0 ? std::string("guest fault in case ") + std::to_string(i)
                      : rc == -1 ? std::string("left the allowlist in case ") + std::to_string(i)
                      : why) + "\n";
      rep += "equal " + std::to_string(equal) + "\n";
      return 3;
    }
    if (r == 1) {
      ++equal;
      continue;
    }
    const Case s = Shrink(t, c, pinned);
    Compare(t, s, g, o, rc, why);
    rep += "result DIFFER\nequal " + std::to_string(equal) + "\ncase " + std::to_string(i) + "\nshrunk";
    bool any = false;
    for (size_t k = 0; k < s.size(); ++k)
      if (s[k] != t.fields[k].def) rep += " " + t.fields[k].name + "=" + ValueText(t.fields[k], s[k]), any = true;
    rep += any ? "\n" : " (all defaults)\n";
    std::map<std::string, bool> keys;
    for (const auto& kv : g) keys[kv.first] = true;
    for (const auto& kv : o) keys[kv.first] = true;
    for (const auto& kv : keys) {
      const auto gi = g.find(kv.first), oi = o.find(kv.first);
      const std::string gv = gi == g.end() ? "(none)" : gi->second, ov = oi == o.end() ? "(none)" : oi->second;
      if (gv != ov) rep += "out " + kv.first + " game=" + gv + " ours=" + ov + "\n";
    }
    return 1;
  }
  rep += "result EQUAL\nequal " + std::to_string(equal) + "\n";
  return 0;
}

// =====================================================================================================================================
// Test 1: placement_table. The game's placement table loader sub_8258F698 (r3 = the table object, r4 = the file bytes, r5 = their length)
// vs ours arena_props::PlacementIds / ReadBlock (ARENAFIX). The loader reads a SubH block into the object's +4 / +8 / +12, skips an 'ASH '
// block, reads the arena SubH block into +28 / +32 / +36; each 'Data' record is 36 bytes and its file byte 31 lands at +23 of the 24-byte
// record (sub_8258CD40). Inputs = the bytes of a table in the loader's layout to its last byte (the generator below) and the allocator's path
// choice (ret#sub_82C2E1E0). The CYMem / per-thread heap family is stubbed (a bump arena on host scratch): its heap table [0x8392FFE4] exists
// only after the game's boot (EQUIV0 proof 2). sub_82252758 (alloc: sub_82C2E1E0 ? sub_82C2DCD8(n) : sub_82C2F5A8((n + 15) >> 4)),
// sub_82CEBA60 (memset 0) and sub_82C2F810 (memset) run as the game's own: they reach only the stubs and memset.
// Ours refusing the bytes (PlacementIds false: the caller keeps its old rule) is compared as "no table", the same as a loader that built none.
// =====================================================================================================================================
namespace placement {

constexpr int kB1 = 3, kB1Recs = 2, kAsh = 2, kB3 = 4, kB3Recs = 3;
std::string P(const std::string& a, int i, const std::string& b) { return a + std::to_string(i) + b; }

std::vector<Field> Fields() {
  std::vector<Field> f;
  f.push_back(IntField("b1.byte0", 0, 2, 1, {1}));
  // S111 EQUIVTIER: the counts are signed halfwords in the file; -1 (0xFFFF) reaches the loader's size-overflow arms
  f.push_back(IntField("b1.count", -1, kB1, 1));
  for (int c = 0; c < kB1; ++c) {
    f.push_back(IntField(P("b1.c", c, ".key"), -1, 30, 12));
    f.push_back(IntField(P("b1.c", c, ".n"), -1, kB1Recs, 0));
    f.push_back(IntField(P("b1.c", c, ".ref"), -1, 1005, 1001, {-1, 1000, 1001}));
    for (int j = 0; j < kB1Recs; ++j) f.push_back(IntField(P("b1.c", c, ".r") + std::to_string(j) + ".id", 0, 255, 6, {6, 7, 32}));
  }
  f.push_back(IntField("ash.count", -1, kAsh, 1));
  for (int h = 0; h < kAsh; ++h) f.push_back(IntField(P("ash.h", h, ".n"), 0, 2, 1));
  f.push_back(IntField("b3.count", 0, kB3, 2));
  const int keys[kB3] = {21, 1001, 1000, 5}, refs[kB3] = {1001, -1, -1, 1000};
  for (int c = 0; c < kB3; ++c) {
    f.push_back(IntField(P("b3.c", c, ".key"), -1, 1005, keys[c], {0, 1, 21, 200, 1000, 1001}));
    f.push_back(IntField(P("b3.c", c, ".n"), -1, kB3Recs, 0));
    f.push_back(IntField(P("b3.c", c, ".ref"), -1, 1005, refs[c], {-1, 1000, 1001}));
    for (int j = 0; j < kB3Recs; ++j) f.push_back(IntField(P("b3.c", c, ".r") + std::to_string(j) + ".id", 0, 255, 32, {6, 7, 29, 32, 35, 105, 106, 107}));
  }
  f.push_back(IntField("ret#sub_82C2E1E0", 0, 1, 1));
  return f;
}

void Put16(std::vector<uint8_t>& b, int v) { b.push_back(uint8_t(v & 0xFF)), b.push_back(uint8_t((v >> 8) & 0xFF)); }
void PutTag(std::vector<uint8_t>& b, const char* t) { b.insert(b.end(), t, t + 4); }

// The file bytes of a case: little-endian heads as the player's bgEtc p10/45010 (ARENAFIX: u8 1, u8 0xCC, u16 count; 12-byte SubH heads
// i16 key, u16 n, i16 ref, u16 pad; 36-byte Data records, size 36 at +4, the id at byte 31; 8-byte 'ASH ' heads u16 id, u16 n, n 8-byte entries).
std::vector<uint8_t> Bytes(const Test& t, const Case& c) {
  std::vector<uint8_t> b;
  auto subh = [&](const char* blk, int max_chunks, int max_recs, bool first) {
    b.push_back(uint8_t(first ? t.I(c, "b1.byte0") : 1)), b.push_back(0xCC);
    const int count = int(t.I(c, std::string(blk) + ".count"));
    Put16(b, count);
    for (int k = 0; k < count && k < max_chunks; ++k) {
      const std::string p = P(std::string(blk) + ".c", k, "");
      const int n = int(t.I(c, p + ".n"));
      PutTag(b, "SubH"), Put16(b, int(t.I(c, p + ".key"))), Put16(b, n), Put16(b, int(t.I(c, p + ".ref"))), Put16(b, 0);
      for (int j = 0; j < n && j < max_recs; ++j) {
        PutTag(b, "Data");
        b.push_back(36), b.push_back(0), b.push_back(0), b.push_back(0);
        for (int z = 8; z < 31; ++z) b.push_back(0);
        b.push_back(uint8_t(t.I(c, p + ".r" + std::to_string(j) + ".id")));
        for (int z = 32; z < 36; ++z) b.push_back(0);
      }
    }
  };
  subh("b1", kB1, kB1Recs, true);
  b.push_back(1), b.push_back(0xCC);
  const int ash = int(t.I(c, "ash.count"));
  Put16(b, ash);
  for (int h = 0; h < ash; ++h) {
    const int n = int(t.I(c, P("ash.h", h, ".n")));
    PutTag(b, "ASH "), Put16(b, h), Put16(b, n);
    for (int z = 0; z < 8 * n; ++z) b.push_back(0);
  }
  subh("b3", kB3, kB3Recs, false);
  return b;
}

// Stubs (coverage tier): the per-thread heap family answers without the boot's tables.
int StubZero(const Wwe2k14EquivHost* h) { h->set_gpr(3, 0); return 1; }
int StubKeep(const Wwe2k14EquivHost*) { return 1; }
int StubPath(const Wwe2k14EquivHost* h) { h->set_gpr(3, uint64_t(g_test->I(*g_case, "ret#sub_82C2E1E0"))); return 1; }
uint32_t Bump(uint64_t bytes) {
  if (bytes == 0 || bytes >= 0x10000000ull) return 0;   // the loader's overflow sizes (-1) fail, as an allocator refusing them
  const uint32_t a = H->scratch(uint32_t(bytes), 16);
  if (a) std::memset(G(a), 0, size_t(bytes));
  return a;
}
int StubAlloc(const Wwe2k14EquivHost* h) { h->set_gpr(3, Bump(uint32_t(h->get_gpr(3)))); return 1; }          // sub_82C2DCD8(bytes, tag, 0)
int StubAlloc16(const Wwe2k14EquivHost* h) { h->set_gpr(3, Bump(uint64_t(uint32_t(h->get_gpr(3))) * 16)); return 1; }   // sub_82C2F5A8(16-byte units)
// sub_82C79000 (the game's forward memcpy: r3 = destination, r4 = source, r5 = bytes, returns r3) is hooked by us (src\nf\memcpy_fwd.h, the
// native function factory), so the static closure refuses it unstubbed: the stub copies the bytes (the loader's ranges are disjoint).
int StubMemcpy(const Wwe2k14EquivHost* h) {
  std::memmove(G(uint32_t(h->get_gpr(3))), G(uint32_t(h->get_gpr(4))), size_t(uint32_t(h->get_gpr(5))));
  return 1;
}

std::string Block(uint32_t obj, uint32_t off) {
  const int count = int16_t(R16(obj + off));
  const uint32_t heads = R32(obj + off + 4), recs = R32(obj + off + 8);
  std::string s = std::to_string(count < 0 ? 0 : count) + ":";
  for (int c = 0; c < count && heads; ++c) {
    s += " " + std::to_string(int16_t(R16(heads + 8 * c))) + "/" + std::to_string(int16_t(R16(heads + 8 * c + 4))) + "[";
    const int n = recs ? int(R32(recs + 8 * c)) : 0;
    const uint32_t list = recs ? R32(recs + 8 * c + 4) : 0;
    for (int j = 0; j < n && list; ++j) s += (j ? "," : "") + std::to_string(R8(list + 24 * j + 23));
    s += "]";
  }
  return s;
}

int Game(const Test& t, const Case& c, Out& out, std::string&) {
  const std::vector<uint8_t> b = Bytes(t, c);
  const uint32_t obj = H->scratch(64, 16), buf = H->scratch(uint32_t(b.size()), 16);
  std::memset(G(obj), 0, 64);
  std::memcpy(G(buf), b.data(), b.size());
  H->set_gpr(3, obj), H->set_gpr(4, buf), H->set_gpr(5, b.size());
  const int rc = H->call(t.target);
  if (rc != 1) return rc;
  if (R32(obj + 8) == 0) {   // no SubH block array: the loader built no table (byte 0 not 1, or a block-1 count <= 0)
    out["table"] = "none";
    return 1;
  }
  out["table"] = "built";
  out["match_block"] = Block(obj, 4);
  out["arena_block"] = Block(obj, 28);
  return 1;
}

std::string Chunks(const std::vector<assets::arena_props::Chunk>& v) {
  std::string s = std::to_string(v.size()) + ":";
  for (const auto& ch : v) {
    s += " " + std::to_string(ch.key) + "/" + std::to_string(ch.ref) + "[";
    for (size_t j = 0; j < ch.ids.size(); ++j) s += (j ? "," : "") + std::to_string(ch.ids[j]);
    s += "]";
  }
  return s;
}

// ours: the loader's block-1 gate (PlacementGateRefuses: no table) and PlacementIds' layout check (the three blocks in the loader's order, to
// the last byte), then the chunk lists ReadBlock gives.
void Ours(const Test& t, const Case& c, Out& out) {
  namespace ap = assets::arena_props;
  const std::vector<uint8_t> b = Bytes(t, c);
  std::vector<int> ids;
  bool found = false;
  if (ap::PlacementGateRefuses(b) || !ap::PlacementIds(b, 0, ids, found)) {
    out["table"] = "none";
    return;
  }
  // PLACECOUNT0's gate (PlacementIds answers true without reading): block 1's signed count <= 0 or its byte 0 not 1 = the game builds no table.
  if (b.size() >= 4 && (int16_t(ap::Rd16(b, 2)) <= 0 || b[0] != 0x01)) {
    out["table"] = "none";
    return;
  }
  std::vector<ap::Chunk> m, skip, a;
  size_t p = 0;
  ap::ReadBlock(b, p, true, m), ap::ReadBlock(b, p, false, skip), ap::ReadBlock(b, p, true, a);
  out["table"] = "built";
  out["match_block"] = Chunks(m);
  out["arena_block"] = Chunks(a);
}

// Planted twin: ReadBlock stepping 32 bytes per record (the ARENAFIX unit's fixture bug, as a parser).
bool ReadBlock32(const std::vector<uint8_t>& b, size_t& p, bool subh, std::vector<assets::arena_props::Chunk>& out) {
  namespace ap = assets::arena_props;
  if (p + 4 > b.size() || b[p] != 0x01 || b[p + 1] != 0xCC) return false;
  const int count = ap::Rd16(b, p + 2);
  size_t q = p + 4;
  for (int c = 0; c < count; ++c) {
    if (q + (subh ? 12 : 8) > b.size() || std::memcmp(b.data() + q, subh ? "SubH" : "ASH ", 4) != 0) return false;
    if (!subh) {
      q += 8 + 8 * size_t(ap::Rd16(b, q + 6));
      continue;
    }
    ap::Chunk ch;
    ch.key = int16_t(ap::Rd16(b, q + 4)), ch.ref = int16_t(ap::Rd16(b, q + 8));
    const int n = ap::Rd16(b, q + 6);
    q += 12;
    for (int j = 0; j < n; ++j) {
      if (q + 32 > b.size() || std::memcmp(b.data() + q, "Data", 4) != 0) return false;
      ch.ids.push_back(b[q + 31]);
      q += 32;
    }
    out.push_back(std::move(ch));
  }
  p = q;
  return true;
}
void OursTwin32(const Test& t, const Case& c, Out& out) {
  const std::vector<uint8_t> b = Bytes(t, c);
  std::vector<assets::arena_props::Chunk> m, skip, a;
  size_t p = 0;
  if (!ReadBlock32(b, p, true, m) || !ReadBlock32(b, p, false, skip) || !ReadBlock32(b, p, true, a) || p != b.size()) {
    out["table"] = "none";
    return;
  }
  out["table"] = "built";
  out["match_block"] = Chunks(m);
  out["arena_block"] = Chunks(a);
}

std::vector<std::pair<uint32_t, StubFn>> Stubs() {
  return {{0x82C2D8C0, StubZero}, {0x82C2DE28, StubKeep}, {0x82C2D808, StubZero},  {0x82C2D940, StubZero},
          {0x82C2E1E0, StubPath}, {0x82C2DCD8, StubAlloc}, {0x82C2F5A8, StubAlloc16}, {0x82C2EA58, StubZero},
          {0x82C79000, StubMemcpy}};
}

}  // namespace placement

#include "menus100_tests.h"   // M100-COMP: the menu questions (Q5.3 ...), one header each; AddMenus100 is called by Tests()

// =====================================================================================================================================
// Test 2: mip_row. The arena MipmapData row reader sub_827FDB78 (r3 = the table: +4 the row count, +8 the 32-byte rows; r4 = the name):
// the first row whose name equals (sub_827FDB10) sets the loader's level word [0x83629CA8] to the row's byte 31, sign-extended
// (sub_82BD42E0); r3 = the word (sub_82BD42F0). Ours: arena_levels::Rule::Of (ARENALEVELS / LEVELSBYTE) with the rows, base = the word's value
// before the call. Compared as Texture::game_levels (ours' output): the game's r3 <= 0 is the whole chain (level_tag::kWholeChainLevels).
// The query is a texture directory record's name: never empty here (ours skips empty row names; the game would match an empty query to an
// empty row: not in the files [L], so not generated).
// =====================================================================================================================================
namespace miprow {

constexpr int kRows = 5;
const char* const kNames[] = {"", "ar_dm", "slo_02", "raw_slope1", "fleet_sh2", "design_01", "ar_dm_b", "abcdefghijklmnopqrstuvwxyz0123"};
constexpr int kNameCount = int(sizeof kNames / sizeof kNames[0]);

std::vector<Field> Fields() {
  std::vector<Field> f;
  f.push_back(IntField("[in_r3+4]", -1, kRows, 3, {0}));
  const int defs[kRows] = {1, 2, 3, 4, 5}, lv[kRows] = {1, 0, 2, 1, 3};
  for (int k = 0; k < kRows; ++k) {
    f.push_back(IntField("row" + std::to_string(k) + ".name", 0, kNameCount - 1, defs[k], {1}));
    f.push_back(IntField("row" + std::to_string(k) + ".byte31", 0, 255, lv[k], {0, 1, 127, 128, 255}));
  }
  f.push_back(IntField("query", 1, kNameCount - 1, 1));
  f.push_back(IntField("[0x83629CA8]", -1, 10, 3, {0, 1, 3}));
  return f;
}

uint32_t GameLevels(int32_t g) { return g <= 0 ? assets::level_tag::kWholeChainLevels : uint32_t(g); }

int Game(const Test& t, const Case& c, Out& out, std::string&) {
  const uint32_t tbl = H->scratch(16, 16), rows = H->scratch(32 * kRows, 16), q = H->scratch(32, 16);
  std::memset(G(rows), 0, 32 * kRows);
  std::memset(G(q), 0, 32);
  W32(tbl + 4, uint32_t(int32_t(t.I(c, "[in_r3+4]")))), W32(tbl + 8, rows);
  for (int k = 0; k < kRows; ++k) {
    const char* nm = kNames[t.I(c, "row" + std::to_string(k) + ".name")];
    std::memcpy(G(rows + 32 * k), nm, std::strlen(nm));
    W8(rows + 32 * k + 31, uint8_t(t.I(c, "row" + std::to_string(k) + ".byte31")));
  }
  const char* qn = kNames[t.I(c, "query")];
  std::memcpy(G(q), qn, std::strlen(qn));
  W32(0x83629CA8, uint32_t(int32_t(t.I(c, "[0x83629CA8]"))));
  H->set_gpr(3, tbl), H->set_gpr(4, q);
  const int rc = H->call(t.target);
  if (rc != 1) return rc;
  out["levels"] = IntText(GameLevels(int32_t(uint32_t(H->get_gpr(3)))));
  return 1;
}

assets::arena_levels::Rule MakeRule(const Test& t, const Case& c) {
  assets::arena_levels::Rule r;
  r.base = uint32_t(int32_t(t.I(c, "[0x83629CA8]")));
  const int n = int(t.I(c, "[in_r3+4]"));
  for (int k = 0; k < n && k < kRows; ++k)
    r.rows.push_back({kNames[t.I(c, "row" + std::to_string(k) + ".name")], uint8_t(t.I(c, "row" + std::to_string(k) + ".byte31"))});
  return r;
}
void Ours(const Test& t, const Case& c, Out& out) { out["levels"] = IntText(MakeRule(t, c).Of(kNames[t.I(c, "query")])); }

// Planted twin: byte 31 read unsigned.
void OursTwinUnsigned(const Test& t, const Case& c, Out& out) {
  const assets::arena_levels::Rule r = MakeRule(t, c);
  const std::string raw = kNames[t.I(c, "query")];
  int v = int(r.base);
  for (const auto& w : r.rows)
    if (!w.name.empty() && w.name == raw) {
      v = int(w.level);
      break;
    }
  out["levels"] = IntText(v <= 0 ? assets::level_tag::kWholeChainLevels : uint32_t(v));
}

}  // namespace miprow

// =====================================================================================================================================
// Test 3: splat_life. The floor splat's life step sub_82697EB8 (r3 = the particle) vs a model from MATSIZE (the scale +56: state 1 sets it
// to counter x dt x 0.0023333 + f1 (+60) and leaves the state at f2 (+52)) and BLOODMOD / BLOODMOD2 (state 3 lowers the alpha +64 by
// 0.003333 x dt and frees the splat below [0x8218B8CC]; state 5 frees; the state is written only as 4 or as state + 1) and MATBLOOD (state 0
// falls by 0.8 x dt per step and lands above -12; later states sit 0.08 / 0.05 above the floor height sub_826088A8 gives). The arithmetic
// is single precision as the PowerPC does it (fmadds / fnmsubs: one rounding of the exact a x b + c). Closure: sub_826088A8 only, no stub.
// =====================================================================================================================================
namespace splat {

constexpr uint32_t kDt = 0x8392DC74, kPool = 0x837689D4, kTable = 0x836F4C74, kFloorWord = 0x83767CF0, kFloor = 0x83622470, kFreeBelow = 0x8218B8CC;
// The image's constants (q words): 0.8 - 1 ulp, 0.0023333333, 0.024, 0.003333, -12, 0.08, 0.05, 0.45.
inline float C(uint32_t bits) { float f; std::memcpy(&f, &bits, 4); return f; }
const float kFall = C(0x3F4CCCCC), kGrow = C(0x3B18EAD6), kSettle = C(0x3CC49BA6), kFade = C(0x3B5A6E76), kLand = C(0xC1400000),
            kAboveHi = C(0x3DA3D70A), kAboveLo = C(0x3D4CCCCD), kSettled = C(0x3EE66666);

std::vector<Field> Fields() {
  return {IntField("[in_r3+16]", 0, 3, 0),
          IntField("[in_r3+20]", 0, 400, 10, {0}),
          IntField("[in_r3+24]", 0, 7, 1, {0, 1, 2, 3, 4, 5}),
          FloatField("[in_r3+36]", -200, 50, -119.92),
          FloatField("[in_r3+48]", -20, 5, -1),
          FloatField("[in_r3+52]", 0.4, 1.6, 1.0),
          FloatField("[in_r3+56]", 0, 1.6, 0.1),
          FloatField("[in_r3+60]", 0.2, 0.7, 0.3),
          FloatField("[in_r3+64]", -0.1, 1, 0.5, {0}),
          FloatField("[in_r3+68]", 0.3, 1, 0.6, {0.45}),
          FloatField("[0x8392DC74]", 0, 3, 1),
          IntField("[0x837689D4]", 0, 1, 1),   // 0 = no pool, else the pool on scratch
          IntField("[[0x837689D4]+3896]", 0, 50, 5),
          IntField("[[0x836F4C74]+376]", 0, 7, 0, {0}),
          IntField("[[0x836F4C74]+368]", -2, 2, 1, {0}),
          IntField("[[0x836F4C74]+372]", -2, 2, 1, {0}),
          IntField("[0x83767CF0]", 0, 1, 0, {0}),
          FloatField("[0x83622470]", -130, -110, -120),
          FloatField("[0x83622474]", -130, -110, -120),
          FloatField("in_f1", -10, 10, 7.5)};
}

struct In {
  uint32_t p16, p20, st;
  float y, vy, f2, s, f1, a, p68, dt;
  bool pool;
  uint32_t count, t376;
  int32_t t368, t372;
  uint32_t floor_word;
  float floor0, floor1;
  double in_f1;
};
In Read(const Test& t, const Case& c) {
  return {uint32_t(t.I(c, "[in_r3+16]")), uint32_t(t.I(c, "[in_r3+20]")), uint32_t(t.I(c, "[in_r3+24]")), t.F(c, "[in_r3+36]"),
          t.F(c, "[in_r3+48]"), t.F(c, "[in_r3+52]"), t.F(c, "[in_r3+56]"), t.F(c, "[in_r3+60]"), t.F(c, "[in_r3+64]"), t.F(c, "[in_r3+68]"),
          t.F(c, "[0x8392DC74]"), t.I(c, "[0x837689D4]") != 0, uint32_t(t.I(c, "[[0x837689D4]+3896]")), uint32_t(t.I(c, "[[0x836F4C74]+376]")),
          int32_t(t.I(c, "[[0x836F4C74]+368]")), int32_t(t.I(c, "[[0x836F4C74]+372]")), uint32_t(t.I(c, "[0x83767CF0]")),
          t.F(c, "[0x83622470]"), t.F(c, "[0x83622474]"), t.V(c, "in_f1")};
}
void Emit(const In& p, double f1, Out& out) {
  out["[in_r3+16]"] = HexText(p.p16), out["[in_r3+20]"] = IntText(p.p20), out["[in_r3+24]"] = IntText(p.st);
  out["[in_r3+36]"] = F32Text(p.y), out["[in_r3+48]"] = F32Text(p.vy), out["[in_r3+52]"] = F32Text(p.f2), out["[in_r3+56]"] = F32Text(p.s);
  out["[in_r3+60]"] = F32Text(p.f1), out["[in_r3+64]"] = F32Text(p.a), out["[in_r3+68]"] = F32Text(p.p68), out["f1"] = F64Text(f1);
  if (p.pool) out["[[0x837689D4]+3896]"] = IntText(p.count);
}

int Game(const Test& t, const Case& c, Out& out, std::string&) {
  const In p = Read(t, c);
  const uint32_t q = H->scratch(80, 16), pool = H->scratch(4096, 16), tbl = H->scratch(512, 16);
  std::memset(G(q), 0, 80);
  W32(q + 16, p.p16), W32(q + 20, p.p20), W32(q + 24, p.st), WF(q + 36, p.y), WF(q + 48, p.vy), WF(q + 52, p.f2), WF(q + 56, p.s);
  WF(q + 60, p.f1), WF(q + 64, p.a), WF(q + 68, p.p68);
  W32(kPool, p.pool ? pool : 0);
  if (p.pool) W32(pool + 3896, p.count);
  W32(kTable, tbl), W32(tbl + 376, p.t376), W32(tbl + 368, uint32_t(p.t368)), W32(tbl + 372, uint32_t(p.t372));
  W32(kFloorWord, p.floor_word), WF(kFloor, p.floor0), WF(kFloor + 4, p.floor1), WF(kDt, p.dt);
  H->set_gpr(3, q), H->set_fpr(1, p.in_f1);
  const int rc = H->call(t.target);
  if (rc != 1) return rc;
  In o = p;
  o.p16 = R32(q + 16), o.p20 = R32(q + 20), o.st = R32(q + 24), o.y = RF(q + 36), o.vy = RF(q + 48), o.f2 = RF(q + 52), o.s = RF(q + 56);
  o.f1 = RF(q + 60), o.a = RF(q + 64), o.p68 = RF(q + 68), o.count = p.pool ? R32(pool + 3896) : 0;
  Emit(o, H->get_fpr(1), out);
  out["r3"] = (uint32_t(H->get_gpr(3)) == q) ? "the particle" : HexText(uint32_t(H->get_gpr(3)));
  return 1;
}

inline float Madd(float a, float b, float c) { return float(std::fma(double(a), double(b), double(c))); }    // fmadds
inline float Nmsub(float a, float b, float c) { return float(-std::fma(double(a), double(b), -double(c))); }  // fnmsubs: c - a x b

// The model. `fade` = the state-3 alpha step per dt (the twin's one wrong constant).
void Model(const Test& t, const Case& c, Out& out, float fade) {
  In p = Read(t, c);
  double f1 = p.in_f1;
  auto free_splat = [&] {
    p.p16 = 0xFFFFFFFFu, p.st = 4;
    if (p.pool) p.count -= 1;
  };
  if (p.st == 5) {
    free_splat();   // a splat marked for release: free (BLOODMOD2)
  } else if (p.st != 4) {
    p.p20 += 1;
    if (p.st == 0) {   // falling (MATBLOOD)
      p.vy = Madd(p.dt, kFall, p.vy);
      p.y = float(double(p.y) + double(p.vy));
      if (p.y > kLand) p.st += 1;
      p.p20 = 0;
    } else {
      const float floor = p.floor_word == 0 ? p.floor1 : p.floor0;   // sub_826088A8
      const int32_t sel = p.t376 == 0 ? p.t372 : p.t368;
      p.y = float(double(floor) - double(sel > 0 ? kAboveHi : kAboveLo));
      f1 = floor;
      if (p.st == 1) {   // growing (MATSIZE): scale = counter x dt x 0.0023333 + f1, until f2
        p.s = Madd(float(double(float(double(int32_t(p.p20)))) * double(p.dt)), kGrow, p.f1);
        if (!(p.s < p.f2)) p.p20 = 0, p.st += 1;
      } else if (p.st == 2) {   // settling: +68 down by 0.024 x dt to 0.45
        p.p68 = Nmsub(p.dt, kSettle, p.p68);
        if (p.p68 < kSettled) p.p68 = kSettled, p.st += 1;
      } else {   // fading (BLOODMOD): alpha down by 0.003333 x dt, freed below the image word
        p.a = Nmsub(p.dt, fade, p.a);
        if (p.a < RF(kFreeBelow)) free_splat();
      }
    }
  }
  Emit(p, f1, out);
  out["r3"] = "the particle";
}
void Ours(const Test& t, const Case& c, Out& out) { Model(t, c, out, kFade); }
// Planted twin: wrong on the fade branch only (0.0033 per dt instead of the image's 0.003333).
void OursTwinFade(const Test& t, const Case& c, Out& out) { Model(t, c, out, 0.0033f); }

}  // namespace splat

#include "flow_build_test.h"   // Test 4: flow_build (M100-Q52): sub_834256E8 vs flow_table.md's rule

}  // namespace equiv

#include "lsweep_tests.h"   // the S111 LSWEEP pilot's [L]-claim models (tools\equiv\lsweep.py)

namespace equiv {

const std::vector<Test>& Tests() {
  static const std::vector<Test> tests = [] {
    std::vector<Test> v;
    AddMenus100(v);
    Test a;
    a.name = "placement_table", a.target = 0x8258F698, a.fields = placement::Fields(), a.stubs = placement::Stubs();
    a.what = "ARENAFIX: the placement table loader (36-byte Data records) vs arena_props::PlacementIds / ReadBlock";
    a.game = placement::Game, a.ours = placement::Ours;
    v.push_back(a);
    Test a2 = a;
    a2.name = "placement_table.twin32", a2.what = "planted twin: ReadBlock stepping 32 bytes per record", a2.ours = placement::OursTwin32,
    a2.twin_of = "placement_table";
    v.push_back(a2);
    Test b;
    b.name = "mip_row", b.target = 0x827FDB78, b.fields = miprow::Fields();
    b.what = "ARENALEVELS / LEVELSBYTE: the MipmapData row reader (byte 31 sign-extended) vs arena_levels::Rule::Of";
    b.game = miprow::Game, b.ours = miprow::Ours;
    v.push_back(b);
    Test b2 = b;
    b2.name = "mip_row.twin_unsigned", b2.what = "planted twin: byte 31 read unsigned", b2.ours = miprow::OursTwinUnsigned, b2.twin_of = "mip_row";
    v.push_back(b2);
    Test c;
    c.name = "splat_life", c.target = 0x82697EB8, c.fields = splat::Fields();
    c.what = "MATSIZE / BLOODMOD: the floor splat's life step vs the notes' model";
    c.game = splat::Game, c.ours = splat::Ours;
    v.push_back(c);
    Test c2 = c;
    c2.name = "splat_life.twin_fade", c2.what = "planted twin: the fade branch's rate 0.0033", c2.ours = splat::OursTwinFade, c2.twin_of = "splat_life";
    v.push_back(c2);
    lsweep::Add(v);
    Test d;
    d.name = "flow_build", d.target = 0x834256E8, d.fields = flowbuild::Fields(), d.stubs = flowbuild::Stubs();
    d.what = "M100-Q52: the flow table build rule (menu ids, 76-byte records, index) vs the flow_table.md model";
    d.game = flowbuild::Game, d.ours = flowbuild::Ours;
    v.push_back(d);
    Test d2 = d;
    d2.name = "flow_build.twin_nodepth", d2.what = "planted twin: the menu ids by page order, every page at depth 0", d2.ours = flowbuild::OursTwinNoDepth,
    d2.twin_of = "flow_build";
    v.push_back(d2);
    return v;
  }();
  return tests;
}

int CopyOut(const std::string& s, char* out, int cap) {
  if (!out || cap <= 0) return int(s.size());
  const size_t n = s.size() < size_t(cap - 1) ? s.size() : size_t(cap - 1);
  std::memcpy(out, s.data(), n);
  out[n] = 0;
  return int(s.size());
}

}  // namespace equiv

EQUIV_EXPORT int wwe2k14_equiv_init(const Wwe2k14EquivHost* host) {
  equiv::H = host;
  return equiv::kApiVersion;
}

EQUIV_EXPORT int wwe2k14_equiv_run(const char* test, uint64_t seed, int cases, char* report, int cap) {
  std::string rep;
  if (!equiv::H || !test) return 2;
  for (const equiv::Test& t : equiv::Tests())
    if (t.name == test) {
      const int rc = equiv::Run(t, seed, cases, rep);
      equiv::CopyOut(rep, report, cap);
      return rc;
    }
  equiv::CopyOut(std::string("equiv ") + test + "\nresult UNKNOWN\n", report, cap);
  return 2;
}

// Not part of the runner's interface: the test table for tools\equiv\equiv.py (ctypes, no host needed). One line per test, tab-separated:
// name, target, twin_of (- when none), stubbed addresses (comma list), fields (name=kind,lo,hi,def;...), what. Returns the full length.
EQUIV_EXPORT int wwe2k14_equiv_list(char* out, int cap) {
  std::string s;
  char b[64];
  for (const equiv::Test& t : equiv::Tests()) {
    std::snprintf(b, sizeof b, "\t0x%08X\t", t.target);
    s += t.name + b + (t.twin_of.empty() ? "-" : t.twin_of) + "\t";
    for (size_t i = 0; i < t.stubs.size(); ++i) {
      std::snprintf(b, sizeof b, "%s0x%08X", i ? "," : "", t.stubs[i].first);
      s += b;
    }
    s += "\t";
    for (size_t i = 0; i < t.fields.size(); ++i) {
      const equiv::Field& f = t.fields[i];
      std::snprintf(b, sizeof b, "=%s,%g,%g,%g", f.kind == equiv::Kind::Int ? "int" : "f32", f.lo, f.hi, f.def);
      s += (i ? ";" : "") + f.name + b;
    }
    s += "\t" + t.what + "\n";
  }
  return equiv::CopyOut(s, out, cap);
}
