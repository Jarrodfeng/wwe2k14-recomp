// S105 IMP unit (no game files, no SDK): the two pure pieces of "imports reach an old save" with planted twins.
//   * src\superstars_mask.h: the hook's whole effect on the saved DLC mask (mask in -> mask out, gate id, big-endian word).
//   * src\importer\slot_retire.h: the retired-slot rule (a new import never inherits a removed import's slot while a never-used one exists).
// Build + run: tools\importer\build_imp_test.ps1 (-> out\tools\imp_test.exe). Exit code = failures.
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "../../src/importer/slot_retire.h"
#include "../../src/superstars_mask.h"

namespace sm = wwe2k14::superstars_mask;
namespace sl = importer::slots;
static int g_fail = 0;
static void Say(bool ok, const std::string& m) {
  std::printf("  %s %s\n", ok ? "PASS" : "FAIL", m.c_str());
  if (!ok) ++g_fail;
}
// A twin is a deliberately wrong implementation: the checker must say FAIL for it (the unsafe direction: it must not pass).
static void Twin(const char* name, bool caught) {
  std::printf("  %s twin \"%s\"\n", caught ? "caught" : "NOT CAUGHT", name);
  if (!caught) ++g_fail;
}

using MaskFn = std::function<uint32_t(uint32_t, int)>;
// The mask rules, checked against an implementation `f`; returns true when every case holds.
static bool MaskCases(const MaskFn& f) {
  bool ok = true;
  ok = ok && f(0x7C, 6) == 0x3C;            // packs 2-6 offered, saved mask has pack 6: bit 6 cleared, packs 2-5 stay
  ok = ok && f(0x3C, 6) == 0x3C;            // already clear (a mask-0-era save): unchanged
  ok = ok && f(0, 6) == 0;                  // mask 0 (the frozen save): nothing changes
  ok = ok && f(0xFFFFFFFF, 6) == 0xFFFFFFBF;  // only bit 6
  ok = ok && f(0xFFFFFFFF, 31) == 0x7FFFFFFF;  // the top valid bit
  ok = ok && f(0x7C, 5) == 0x7C;            // a real pack's id (1..5) is never ours: unchanged
  ok = ok && f(0x7C, 0) == 0x7C && f(0x7C, 32) == 0x7C && f(0x7C, -1) == 0x7C;  // out of range: unchanged
  return ok;
}

using RetireFn = std::function<std::vector<int>(const std::vector<int>&, const std::vector<int>&, const std::vector<int>&)>;
using OrderFn = std::function<std::vector<int>(const std::vector<int>&, const std::vector<int>&)>;

static bool RetireCases(const RetireFn& f) {
  bool ok = true;
  ok = ok && f({}, {397, 398}, {397, 398}) == std::vector<int>{};            // nothing removed
  ok = ok && f({}, {397, 398, 399}, {397, 399}) == std::vector<int>{398};    // 398 removed: retired
  ok = ok && f({398}, {397}, {397, 400}) == std::vector<int>{398};           // an old retired slot stays
  ok = ok && f({398, 398}, {}, {}) == std::vector<int>{398};                 // no duplicates
  ok = ok && f({410}, {397}, {397, 410}) == std::vector<int>{};              // a slot held NOW is not retired (re-use as a last resort)
  ok = ok && f({}, {397}, {500 - 100}) == std::vector<int>{397};            // moved to another slot: the old one is retired
  ok = ok && f({420, 398}, {}, {}) == std::vector<int>{398, 420};            // sorted
  return ok;
}
// S105 IMP review M1: what WriteManifest does: retire from the slots of the last BUILT pack (not from pack.json's own list).
// f(old_retired, pack_slots_before, built, new_slots)
using BuiltRetireFn = std::function<std::vector<int>(const std::vector<int>&, const std::vector<int>&, const std::vector<int>&, const std::vector<int>&)>;
static bool BuiltRetireCases(const BuiltRetireFn& f) {
  bool ok = true;
  ok = ok && f({}, {397, 398, 399}, {397, 399}, {397, 399}) == std::vector<int>{};                 // 398 was never built: removing it retires nothing
  ok = ok && f({}, {397, 398, 399}, {397, 398, 399}, {397, 399}) == std::vector<int>{398};         // 398 was built: retired
  ok = ok && f({}, {397}, {397, 400}, {397}) == std::vector<int>{400};                              // built but not in pack.json (an old importer's removal): seeded
  ok = ok && f({}, {397, 398}, {}, {397}) == std::vector<int>{};                                    // never built at all: nothing retired
  ok = ok && f({420}, {397}, {397}, {397}) == std::vector<int>{420};                                // the old list stays
  return ok;
}
static bool BuiltOrderCases(const std::function<std::vector<int>(const std::vector<int>&, const std::vector<int>&, const std::vector<int>&)>& f) {
  bool ok = true;
  ok = ok && f({397, 398, 399, 400}, {397, 398}, {397}) == std::vector<int>{399, 400, 398, 397};  // never-used, unbuilt-retired, built-retired
  ok = ok && f({397, 398}, {397, 398}, {}) == std::vector<int>{397, 398};                           // no built list: retired keep their order
  return ok;
}
static bool OrderCases(const OrderFn& f) {
  bool ok = true;
  ok = ok && f({397, 398, 399}, {}) == std::vector<int>{397, 398, 399};              // nothing retired: unchanged
  ok = ok && f({397, 398, 399}, {397}) == std::vector<int>{398, 399, 397};           // a retired one goes last
  ok = ok && f({397, 398, 399}, {397, 398}) == std::vector<int>{399, 397, 398};      // never-used first, retired keep their order
  ok = ok && f({397, 398}, {397, 398}) == std::vector<int>{397, 398};                // all retired: still handed out (last resort, never refused)
  ok = ok && f({397, 398, 399}, {400}).size() == 3;                                  // a retired id that is not free changes nothing
  return ok;
}

int main() {
  std::printf("S105 IMP unit\n");
  // entry: wwe2k14::superstars_mask::ClearPackBit
  Say(MaskCases([](uint32_t m, int id) { return sm::ClearPackBit(m, id); }), "ClearPackBit: the mask rules");
  // entry: wwe2k14::superstars_mask::ClearInGuestWord  (the hook's body in guest memory: big-endian word, 0x839129E0)
  {
    uint8_t w[4] = {0x00, 0x00, 0x00, 0x7C};
    uint32_t o = 0, n = 0;
    const bool changed = sm::ClearInGuestWord(w, 6, &o, &n);
    Say(changed && o == 0x7C && n == 0x3C && w[0] == 0 && w[1] == 0 && w[2] == 0 && w[3] == 0x3C, "ClearInGuestWord: 00 00 00 7C -> 00 00 00 3C (big-endian)");
    uint8_t z[4] = {0, 0, 0, 0};
    Say(!sm::ClearInGuestWord(z, 6) && z[3] == 0, "ClearInGuestWord: mask 0 untouched (frozen save: replay / hash references do not move)");
    uint8_t hi[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    Say(sm::ClearInGuestWord(hi, 6) && hi[0] == 0xFF && hi[1] == 0xFF && hi[2] == 0xFF && hi[3] == 0xBF, "ClearInGuestWord: only bit 6 of a full word, other bytes untouched");
    Say(sm::kSavedMaskAddr == 0x839129E0 && sm::kSavedMaskAddr == 0x83891878u + 0x81168u, "mask address = block B 0x83891878 + 0x81168");
  }
  Twin("mask: clears bit id-1", !MaskCases([](uint32_t m, int id) { return sm::ValidGateId(id) ? (m & ~(1u << (id - 1))) : m; }));
  Twin("mask: sets the bit instead of clearing it", !MaskCases([](uint32_t m, int id) { return sm::ValidGateId(id) ? (m | (1u << id)) : m; }));
  Twin("mask: clears the whole mask", !MaskCases([](uint32_t m, int id) { return sm::ValidGateId(id) ? 0u : m; }));
  Twin("mask: accepts a real pack's id (1..5)", !MaskCases([](uint32_t m, int id) { return (id >= 1 && id < 32) ? (m & ~(1u << id)) : m; }));
  Twin("mask: a hook that treats the guest word as little-endian", [] {
    uint8_t w[4] = {0x00, 0x00, 0x00, 0x7C};  // guest memory is big-endian: this twin decodes / encodes it the host way
    uint32_t v = uint32_t(w[0]) | uint32_t(w[1]) << 8 | uint32_t(w[2]) << 16 | uint32_t(w[3]) << 24;
    v = sm::ClearPackBit(v, 6);
    w[0] = uint8_t(v), w[1] = uint8_t(v >> 8), w[2] = uint8_t(v >> 16), w[3] = uint8_t(v >> 24);
    return !(w[3] == 0x3C);  // the right result is 00 00 00 3C: the twin leaves 7C, so the check must say it is wrong
  }());

  // entry: importer::slots::MergeRetired
  Say(RetireCases([](auto& a, auto& b, auto& c) { return sl::MergeRetired(a, b, c); }), "MergeRetired: the retire rules");
  // entry: importer::slots::NeverUsedFirst
  Say(OrderCases([](auto& a, auto& b) { return sl::NeverUsedFirst(a, b); }), "NeverUsedFirst: never-used slots first, retired last, none refused");
  Say(BuiltRetireCases([](auto& r, auto&, auto& b, auto& n) { return sl::MergeRetired(r, b, n); }), "retire from the BUILT slots: an unbuilt removal retires nothing");
  Twin("retire: an unbuilt removal is retired", !BuiltRetireCases([](auto& r, auto& p, auto&, auto& n) { return sl::MergeRetired(r, p, n); }));
  Twin("retire: a built slot missing from pack.json is not seeded", !BuiltRetireCases([](auto& r, auto& p, auto& b, auto& n) {
         std::vector<int> only;
         for (int s : b)
           if (std::find(p.begin(), p.end(), s) != p.end()) only.push_back(s);
         return sl::MergeRetired(r, only, n);
       }));
  Say(BuiltOrderCases([](auto& a, auto& r, auto& b) { return sl::NeverUsedFirst(a, r, b); }), "NeverUsedFirst: never-used, unbuilt-retired, built-retired");
  Twin("order: built-retired before unbuilt-retired", !BuiltOrderCases([](auto& a, auto& r, auto&) { return sl::NeverUsedFirst(a, r); }));
  Twin("retire: a removed slot is forgotten", !RetireCases([](auto& a, auto&, auto&) { return a; }));
  Twin("retire: the slot held now stays retired", !RetireCases([](auto& a, auto& b, auto& c) {
         std::vector<int> r = sl::MergeRetired(a, b, c);
         r.insert(r.end(), c.begin(), c.end());
         std::sort(r.begin(), r.end());
         r.erase(std::unique(r.begin(), r.end()), r.end());
         return r;
       }));
  Twin("retire: the old list is dropped", !RetireCases([](auto&, auto& b, auto& c) { return sl::MergeRetired({}, b, c); }));
  Twin("order: no reordering (retired first-come)", !OrderCases([](auto& a, auto&) { return a; }));
  Twin("order: retired slots dropped (an import could not be placed)", !OrderCases([](auto& a, auto& r) {
         std::vector<int> v;
         for (int s : a)
           if (std::find(r.begin(), r.end(), s) == r.end()) v.push_back(s);
         return v;
       }));
  Twin("order: retired slots first", !OrderCases([](auto& a, auto& r) {
         std::vector<int> v = sl::NeverUsedFirst(a, r);
         std::reverse(v.begin(), v.end());
         return v;
       }));
  std::printf("imp_test: %s (%d failure(s))\n", g_fail ? "FAIL" : "PASS", g_fail);
  return g_fail ? 1 : 0;
}
