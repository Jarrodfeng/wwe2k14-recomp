// S111 M100-COMP, Q5.3 (docs\research\s111\MENUS100_INV.md): the flow table lookup sub_83424808(T, menu, item) vs OUR code, called directly:
// src\menu_goto_core.h ReadTableWith / MarkSeen (the table read through a word reader, the "seen" flag = "the (menu, item) lookup returns this
// record"). Included by equiv_tests.cpp (menus100_tests.h) inside namespace equiv; not a header of its own.
//
// The rule it must meet (menu_goto_core.h's comment on MarkSeen, MENUTREE): with an index and 0 <= menu < index count the lookup scans only
// [first, first + count) of that menu's entry (an empty range = not found, no fallback to the whole array); else the whole array; either way the
// FIRST record in array order with that (menu, item) is returned; not found = the table's inline default record (T+8).
// Ours = the production functions on the SAME guest bytes the game reads (the reader below reads the test's scratch memory): the answer is the
// record ReadTableWith marked `seen` with that (menu, item), else "default".
// Layout built: holder H ([H+20] = R), T = R+4: T+0 count, T+4 the 76-byte record array, T+84 index count (= [R+88]), T+88 the 12-byte index
// entries {first u32, count u16 at +4}. Preconditions held by the generator: the index ranges stay inside the array (the game would read past the
// array otherwise: not a rule of ours), at least 1 record and 1 index entry (ReadTableWith refuses a table without them on purpose).
namespace lookup {

constexpr int kRecs = 5, kIdx = 4;

inline void W16b(uint32_t a, uint16_t v) { *G(a) = uint8_t(v >> 8), *G(a + 1) = uint8_t(v); }

std::vector<Field> Fields() {
  std::vector<Field> f;
  f.push_back(IntField("n", 1, kRecs, 4));
  for (int k = 0; k < kRecs; ++k) {
    f.push_back(IntField("r" + std::to_string(k) + ".menu", -1, 3, k < 2 ? 1 : 2, {0, 1, 2}));
    f.push_back(IntField("r" + std::to_string(k) + ".item", 0, 3, k % 2, {0, 1}));
  }
  f.push_back(IntField("idx.present", 0, 1, 1, {0}));
  f.push_back(IntField("idx.n", 1, kIdx, 3));
  const int firsts[kIdx] = {0, 0, 2, 4}, counts[kIdx] = {0, 2, 2, 0};
  for (int m = 0; m < kIdx; ++m) {
    f.push_back(IntField("idx" + std::to_string(m) + ".first", 0, kRecs, firsts[m]));
    f.push_back(IntField("idx" + std::to_string(m) + ".count", 0, kRecs, counts[m]));
  }
  f.push_back(IntField("q.menu", -1, 4, 1, {0, 1, 2, 3}));
  f.push_back(IntField("q.item", 0, 3, 0, {0, 1}));
  return f;
}

struct Built {
  uint32_t holder = 0, table = 0, arr = 0;
  int n = 0;
};

// The table in guest memory (the same bytes for the game and for ours).
Built Build(const Test& t, const Case& c) {
  Built b;
  b.n = int(t.I(c, "n"));
  b.holder = H->scratch(32, 16);
  const uint32_t r = H->scratch(128, 16);   // R: T = R + 4
  b.table = r + 4;
  b.arr = H->scratch(uint32_t(76 * kRecs), 16);
  std::memset(G(b.holder), 0, 32), std::memset(G(r), 0, 128), std::memset(G(b.arr), 0, size_t(76 * kRecs));
  W32(b.holder + 20, r);
  for (int k = 0; k < b.n; ++k) {
    const uint32_t a = b.arr + 76 * uint32_t(k);
    W32(a + 0, uint32_t(100 + k)), W32(a + 8, uint32_t(int32_t(t.I(c, "r" + std::to_string(k) + ".menu"))));
    W16b(a + 12, uint16_t(t.I(c, "r" + std::to_string(k) + ".item")));
  }
  const int idx_n = int(t.I(c, "idx.n"));
  W32(b.table + 0, uint32_t(b.n)), W32(b.table + 4, b.arr), W32(b.table + 84, uint32_t(idx_n));
  if (t.I(c, "idx.present")) {
    const uint32_t idx = H->scratch(uint32_t(12 * kIdx), 16);
    std::memset(G(idx), 0, size_t(12 * kIdx));
    for (int m = 0; m < idx_n; ++m) {
      int first = int(t.I(c, "idx" + std::to_string(m) + ".first")), cnt = int(t.I(c, "idx" + std::to_string(m) + ".count"));
      if (first > b.n) first = b.n;
      if (first + cnt > b.n) cnt = b.n - first;   // the range stays inside the array
      W32(idx + 12 * uint32_t(m), uint32_t(first)), W16b(idx + 12 * uint32_t(m) + 4, uint16_t(cnt));
    }
    W32(b.table + 88, idx);
  }
  return b;
}

int Game(const Test& t, const Case& c, Out& out, std::string&) {
  const Built b = Build(t, c);
  H->set_gpr(3, b.table), H->set_gpr(4, uint64_t(uint32_t(int32_t(t.I(c, "q.menu"))))), H->set_gpr(5, uint64_t(t.I(c, "q.item")));
  const int rc = H->call(t.target);
  if (rc != 1) return rc;
  const uint32_t ret = uint32_t(H->get_gpr(3));
  if (ret == b.table + 8) out["record"] = "default";
  else if (ret >= b.arr && ret < b.arr + 76 * uint32_t(b.n) && (ret - b.arr) % 76 == 0) out["record"] = "rec " + std::to_string((ret - b.arr) / 76);
  else out["record"] = HexText(ret);
  return 1;
}

std::string OursWith(const Test& t, const Case& c, bool use_index) {
  const Built b = Build(t, c);
  std::vector<wwe2k14::menu_goto::Rec> recs;
  uint32_t menus = 0;
  auto rd = [](uint32_t a) { return (a >= 0x10000 && a < 0xC0000000u) ? R32(a) : 0u; };
  if (!wwe2k14::menu_goto::ReadTableWith(rd, b.holder, recs, menus)) return "default";
  if (!use_index) wwe2k14::menu_goto::MarkSeen(recs, {});   // planted twin: ignores the menu's index range
  const uint32_t qm = uint32_t(int32_t(t.I(c, "q.menu"))), qi = uint32_t(t.I(c, "q.item"));
  for (size_t i = 0; i < recs.size(); ++i)
    if (recs[i].seen && recs[i].menu == qm && recs[i].item == qi) return "rec " + std::to_string(i);
  return "default";
}
void Ours(const Test& t, const Case& c, Out& out) { out["record"] = OursWith(t, c, true); }
void OursTwinNoIndex(const Test& t, const Case& c, Out& out) { out["record"] = OursWith(t, c, false); }

}  // namespace lookup
