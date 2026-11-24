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

#include <map>
#include <set>

#include "../../src/importer/asset_ids.h"
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

// S110 SEAT-A: the bank numbers are assigned over ALL seats (the frame's seat list), the recorded numbers first; the seat order puts retired last.
namespace ai = importer::asset_ids;
using AssignFn = std::function<std::string(const std::vector<int>&, const std::vector<int>&, const std::map<int, int>&, std::map<int, int>&, std::map<int, int>&)>;
static std::vector<int> AllSeats() {
  std::vector<int> v;
  for (int s = 397; s <= 499; ++s)
    if (s != 399) v.push_back(s);
  return v;
}
static bool AssignCases(const AssignFn& f) {
  bool ok = true;
  const std::vector<int> seats = AllSeats();  // 102 seats
  std::vector<int> free_numbers;
  for (int n = 1; n <= 599; ++n)
    if (n != 103 && n != 410) free_numbers.push_back(n);
  auto distinct = [](const std::map<int, int>& a) {
    std::set<int> s;
    for (auto& [k, v] : a) s.insert(v);
    return s.size() == a.size();
  };
  {  // nothing recorded: every seat has a number, all different, the ordinal's: seat 397 -> free[0], 400 -> free[3] (the ordinal counts from 397, 399 included)
    std::map<int, int> as, mv;
    const std::string why = f(seats, free_numbers, {}, as, mv);
    ok = ok && why.empty() && as.size() == 102 && distinct(as) && as[397] == free_numbers[0] && as[400] == free_numbers[3] && as[499] == free_numbers[102] && mv.empty();
  }
  {  // a recorded number that is still free stays, whatever the ordinal says
    std::map<int, int> as, mv;
    const std::string why = f(seats, free_numbers, {{400, 77}, {450, 12}}, as, mv);
    ok = ok && why.empty() && as[400] == 77 && as[450] == 12 && distinct(as) && as.size() == 102;
  }
  {  // a recorded number that is no longer free (a code constant, 103) moves: the seat gets another, and `moved` remembers 103
    std::map<int, int> as, mv;
    const std::string why = f(seats, free_numbers, {{400, 103}, {401, 410}}, as, mv);
    ok = ok && why.empty() && as[400] != 103 && as[401] != 410 && mv[400] == 103 && mv[401] == 410 && distinct(as);
  }
  {  // two seats recorded on one number: the first keeps it, the other moves
    std::map<int, int> as, mv;
    const std::string why = f(seats, free_numbers, {{400, 50}, {401, 50}}, as, mv);
    ok = ok && why.empty() && as[400] == 50 && as[401] != 50 && distinct(as);
  }
  {  // a number recorded for a seat that has left the list is kept clear while there is a choice
    std::map<int, int> as, mv;
    const std::string why = f(seats, free_numbers, {{600, 5}}, as, mv);
    bool has5 = false;
    for (auto& [k, v] : as) has5 = has5 || v == 5;
    ok = ok && why.empty() && !has5 && distinct(as);
  }
  {  // exactly as many free numbers as seats: all get one; one fewer: refused with a reason
    std::vector<int> exact(free_numbers.begin(), free_numbers.begin() + 102), less(free_numbers.begin(), free_numbers.begin() + 101);
    std::map<int, int> as, mv, as2, mv2;
    ok = ok && f(seats, exact, {}, as, mv).empty() && as.size() == 102 && distinct(as);
    ok = ok && !f(seats, less, {}, as2, mv2).empty();
  }
  return ok;
}
static const AssignFn kRealAssign = [](const std::vector<int>& seats, const std::vector<int>& fr, const std::map<int, int>& rec, std::map<int, int>& as, std::map<int, int>& mv) {
  ai::SetFreeList(fr);
  const std::string why = ai::Assign(seats, rec, &mv);
  as = ai::Config().assigned;
  return why;
};
using SeatOrderFn = std::function<std::vector<int>(const std::vector<int>&, const std::vector<int>&, const std::vector<int>&)>;
static bool SeatOrderCases(const SeatOrderFn& f) {
  const std::vector<int> seats = AllSeats();
  const std::vector<int> o = f(seats, {397, 398, 450}, {397});  // 398 and 450 were never built, 397 was
  if (o.size() != seats.size()) return false;
  // never-used seats first in their own order, then the retired ones no build held (398, 450), then the built retired (397) last
  std::vector<int> want;
  for (int s : seats)
    if (s != 397 && s != 398 && s != 450) want.push_back(s);
  want.insert(want.end(), {398, 450, 397});
  return o == want;
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
  // S110 SEAT-A: Assign over all seats (entry: importer::asset_ids::Assign) and the retired order over all seats
  Say(AssignCases(kRealAssign), "Assign over all 102 seats: ordinal numbers, recorded first, moved remembered, duplicates and left seats avoided, exact fit / one short");
  Twin("assign: the recorded numbers ignored", !AssignCases([](auto& seats, auto& fr, auto&, auto& as, auto& mv) { return kRealAssign(seats, fr, {}, as, mv); }));
  Twin("assign: every seat takes the first free number (not distinct)", !AssignCases([](auto& seats, auto& fr, auto&, auto& as, auto&) {
         for (int s : seats) as[s] = fr[0];
         return std::string();
       }));
  Twin("assign: a recorded number kept although it is no longer free", !AssignCases([](auto& seats, auto& fr, auto& rec, auto& as, auto& mv) {
         const std::string w = kRealAssign(seats, fr, rec, as, mv);
         for (auto& [s, b] : rec) as[s] = b;
         return w;
       }));
  Twin("assign: one number short is not refused", !AssignCases([](auto& seats, auto& fr, auto& rec, auto& as, auto& mv) {
         const std::string w = kRealAssign(seats, fr, rec, as, mv);
         return as.size() == seats.size() ? w : std::string();
       }));
  Twin("assign: only the first 103 seats' worth of the old cap (a seat past it has none)", !AssignCases([](auto& seats, auto& fr, auto& rec, auto& as, auto& mv) {
         std::vector<int> few(seats.begin(), seats.begin() + 50);
         return kRealAssign(few, fr, rec, as, mv);
       }));
  Say(SeatOrderCases([](auto& a, auto& r, auto& b) { return sl::NeverUsedFirst(a, r, b); }), "seat order over all 102 seats: never-used, unbuilt-retired, built-retired (the retired rule is unchanged)");
  Twin("seat order: the seats in list order", !SeatOrderCases([](auto& a, auto&, auto&) { return a; }));
  Twin("seat order: built-retired before unbuilt-retired", !SeatOrderCases([](auto& a, auto& r, auto&) { return sl::NeverUsedFirst(a, r); }));
  Twin("seat order: retired seats dropped", !SeatOrderCases([](auto& a, auto& r, auto&) {
         std::vector<int> v;
         for (int s : a)
           if (std::find(r.begin(), r.end(), s) == r.end()) v.push_back(s);
         return v;
       }));
  std::printf("imp_test: %s (%d failure(s))\n", g_fail ? "FAIL" : "PASS", g_fail);
  return g_fail ? 1 : 0;
}
