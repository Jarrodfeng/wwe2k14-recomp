// S111 M100-Q52: the flow build rule's equivalence test (included by equiv_tests.cpp inside namespace equiv; not a header of its own).
//
// The game's rule sub_834256E8 (r3 = the holder's data object: +4 the flow table T, [data+208] the heap tag; r4 = the UIPR layout: +40 the page
// count, +120 the page array of 1064-byte pages) vs a MODEL written from docs\formats\flow_table.md sections 2-4 ([C] rule: the menu ids by
// depth then page order, a record per visible item of a list page whose flow data D is set and D+20 bit 0 clear, the 76-byte record words)
// and the index sentence of section 1 (T+84 the entry count, 12-byte entries by menu id: first record, record count). The offline
// derivation that rebuilt the 271 records (MENUTREE) was a one-off script that is not in the tree, so the model is this port of the note's
// rule, never of the listing. The generated layout: up to 4 pages of up to 3 items each (page words +580 root, +584 type, +600 / +604 parent
// page and item, +612 / +764 item count, +760 items; item words +28 target page, +32 hidden bit, +48 D; D words as section 2). The allocator
// family is stubbed as in placement_table (placement::Stubs), plus the plain memcpy sub_82C2F7B8.
namespace flowbuild {

constexpr int kPages = 4, kItems = 3;

inline void W16(uint32_t a, uint16_t v) { *G(a) = uint8_t(v >> 8), *G(a + 1) = uint8_t(v); }
std::string PN(int k, const char* n) { return "p" + std::to_string(k) + "." + n; }
std::string IN(int k, int j, const char* n) { return "p" + std::to_string(k) + ".i" + std::to_string(j) + "." + n; }

std::vector<Field> Fields() {
  std::vector<Field> f;
  f.push_back(IntField("layout.pages", 0, kPages, kPages));
  for (int k = 0; k < kPages; ++k) {
    f.push_back(IntField(PN(k, "root"), 0, 1, 0));
    f.push_back(IntField(PN(k, "type"), 0, 5, 4, {2, 4}));
    f.push_back(IntField(PN(k, "parent"), -1, k == 0 ? -1 : k - 1, k == 0 ? -1 : k - 1));   // earlier pages only: no parent cycle
    f.push_back(IntField(PN(k, "pitem"), -1, kItems, 0));
    f.push_back(IntField(PN(k, "count"), 0, kItems, kItems));
    for (int j = 0; j < kItems; ++j) {
      f.push_back(IntField(IN(k, j, "hid"), 0, 3, 0, {1}));
      f.push_back(IntField(IN(k, j, "target"), -1, kPages, j == 0 && k + 1 < kPages ? k + 1 : -1));
      f.push_back(IntField(IN(k, j, "hasD"), 0, 1, 1, {0}));
      f.push_back(IntField(IN(k, j, "d.id"), 0, 100000, k * 10 + j));
      f.push_back(IntField(IN(k, j, "d.label"), 0, 100000, 500 + k * 10 + j));
      f.push_back(IntField(IN(k, j, "d.w8"), -1, 1000, -1));
      f.push_back(IntField(IN(k, j, "d.unlock"), -1, 300, -1, {-1, 163}));
      f.push_back(IntField(IN(k, j, "d.w16"), -1, 1000, -1));
      f.push_back(IntField(IN(k, j, "d.drop"), 0, 3, 0, {1}));
      f.push_back(IntField(IN(k, j, "d.f22"), 0, 3, 0, {1}));
      f.push_back(IntField(IN(k, j, "d.f24"), 0, 3, 0, {1}));
      f.push_back(IntField(IN(k, j, "d.f26"), 0, 3, 0, {1}));
      f.push_back(IntField(IN(k, j, "d.f28"), 0, 3, 0, {1}));
      f.push_back(IntField(IN(k, j, "d.f30"), 0, 3, 0, {1}));
      f.push_back(IntField(IN(k, j, "d.f32"), 0, 3, 0, {1}));
      f.push_back(IntField(IN(k, j, "d.s34"), -32768, 32767, 0, {0, -1, 32767, -32768}));
      f.push_back(IntField(IN(k, j, "d.code"), -2, 3500, -1, {-1, 0, 1001, 2000, 3000}));
      f.push_back(IntField(IN(k, j, "d.type"), -2, 100, -1, {-1, 0}));
      f.push_back(IntField(IN(k, j, "d.w44"), 0, 1000, 0));
    }
  }
  return f;
}

// The case as plain data (the model's input).
struct D {
  bool has;
  uint32_t id, label, w8, unlock, w16, drop, f22, f24, f26, f28, f30, f32, w44;
  int32_t code, type;
  int16_t s34;
  int hid, target;
};
struct Pg {
  int root, type, parent, pitem, count;
  D it[kItems];
};
struct Lay {
  int pages;
  Pg pg[kPages];
};
Lay ReadCase(const Test& t, const Case& c) {
  Lay l{};
  l.pages = int(t.I(c, "layout.pages"));
  for (int k = 0; k < kPages; ++k) {
    Pg& p = l.pg[k];
    p.root = int(t.I(c, PN(k, "root"))), p.type = int(t.I(c, PN(k, "type"))), p.parent = int(t.I(c, PN(k, "parent")));
    p.pitem = int(t.I(c, PN(k, "pitem"))), p.count = int(t.I(c, PN(k, "count")));
    for (int j = 0; j < kItems; ++j) {
      D& d = p.it[j];
      auto u = [&](const char* n) { return uint32_t(t.I(c, IN(k, j, n))); };
      d.hid = int(t.I(c, IN(k, j, "hid"))), d.target = int(t.I(c, IN(k, j, "target"))), d.has = t.I(c, IN(k, j, "hasD")) != 0;
      d.id = u("d.id"), d.label = u("d.label"), d.w8 = u("d.w8"), d.unlock = u("d.unlock"), d.w16 = u("d.w16"), d.drop = u("d.drop");
      d.f22 = u("d.f22"), d.f24 = u("d.f24"), d.f26 = u("d.f26"), d.f28 = u("d.f28"), d.f30 = u("d.f30"), d.f32 = u("d.f32"), d.w44 = u("d.w44");
      d.code = int32_t(t.I(c, IN(k, j, "d.code"))), d.type = int32_t(t.I(c, IN(k, j, "d.type"))), d.s34 = int16_t(t.I(c, IN(k, j, "d.s34")));
    }
  }
  return l;
}

// ---- the game side ----
int Game(const Test& t, const Case& c, Out& out, std::string&) {
  const Lay l = ReadCase(t, c);
  const uint32_t data = H->scratch(256, 16), lay = H->scratch(256, 16), pages = H->scratch(1064 * kPages, 16);
  std::memset(G(data), 0, 256), std::memset(G(lay), 0, 256), std::memset(G(pages), 0, 1064 * kPages);
  W32(lay + 40, uint32_t(l.pages)), W32(lay + 120, pages);
  for (int k = 0; k < kPages; ++k) {
    const Pg& p = l.pg[k];
    const uint32_t pa = pages + 1064 * uint32_t(k), items = H->scratch(80 * kItems, 16);
    std::memset(G(items), 0, 80 * kItems);
    W32(pa + 580, uint32_t(p.root)), W32(pa + 584, uint32_t(p.type)), W32(pa + 600, uint32_t(p.parent)), W32(pa + 604, uint32_t(p.pitem));
    W32(pa + 612, uint32_t(p.count)), W32(pa + 764, uint32_t(p.count)), W32(pa + 760, items);
    for (int j = 0; j < kItems; ++j) {
      const D& d = p.it[j];
      const uint32_t e = items + 80 * uint32_t(j);
      W32(e + 28, uint32_t(d.target)), W16(e + 32, uint16_t(d.hid));
      if (!d.has) continue;
      const uint32_t da = H->scratch(48, 16);
      std::memset(G(da), 0, 48);
      W32(e + 48, da);
      W32(da + 0, d.id), W32(da + 4, d.label), W32(da + 8, d.w8), W32(da + 12, d.unlock), W32(da + 16, d.w16), W16(da + 20, uint16_t(d.drop));
      W16(da + 22, uint16_t(d.f22)), W16(da + 24, uint16_t(d.f24)), W16(da + 26, uint16_t(d.f26)), W16(da + 28, uint16_t(d.f28));
      W16(da + 30, uint16_t(d.f30)), W16(da + 32, uint16_t(d.f32)), W16(da + 34, uint16_t(d.s34));
      W32(da + 36, uint32_t(d.code)), W32(da + 40, uint32_t(d.type)), W32(da + 44, d.w44);
    }
  }
  H->set_gpr(3, data), H->set_gpr(4, lay);
  const int rc = H->call(t.target);
  if (rc != 1) return rc;
  out["ret"] = IntText(int32_t(uint32_t(H->get_gpr(3))));
  const uint32_t T = data + 4, n = R32(T), recs = R32(T + 4), n84 = R32(T + 84), idx = R32(T + 88);
  out["T.count"] = IntText(n), out["T.index_count"] = IntText(n84);
  if (n > 64 || n84 > 64) return 1;   // a runaway count: the two counts above already differ from any model
  for (uint32_t i = 0; i < n && recs; ++i) {
    std::string s;
    for (uint32_t w = 0; w < 19; ++w) s += (w ? " " : "") + HexText(R32(recs + 76 * i + 4 * w));
    out["rec" + std::to_string(i)] = s;
  }
  for (uint32_t m = 0; m < n84 && idx; ++m) out["index" + std::to_string(m)] = IntText(R32(idx + 12 * m)) + "/" + IntText(R16(idx + 12 * m + 4));
  return 1;
}

// ---- the model (flow_table.md sections 1-4) ----
struct Model {
  const Lay& l;
  bool Page(int k) const { return k >= 0 && k < l.pages; }
  bool List(int k) const { return l.pg[k].root == 0 && l.pg[k].type == 4; }
  // a page's n-th item: element index, or -1. Skips hidden elements (bit 0 of +32) except on a plain item array (+584 == 2).
  int Nth(int k, int n) const {
    const Pg& p = l.pg[k];
    if (n < 0 || n >= p.count) return -1;
    if (p.type == 2) return n;
    int seen = 0;
    for (int j = 0; j < p.count; ++j)
      if ((p.it[j].hid & 1) == 0) {
        if (seen == n) return j;
        ++seen;
      }
    return -1;
  }
  int Depth(int k) const {   // the parent pages up to a root (a root page, or none, ends the walk and is not counted)
    int d = 0;
    for (int cur = k;;) {
      const int q = l.pg[cur].parent;
      if (!Page(q) || l.pg[q].root == 1) return d;
      ++d, cur = q;
    }
  }
  uint32_t IdOf(int pg, int item) const {   // D+0 of a page's n-th item; -1 when the page, the item or its D is missing
    if (!Page(pg)) return 0xFFFFFFFFu;
    const int e = Nth(pg, item);
    return e < 0 || !l.pg[pg].it[e].has ? 0xFFFFFFFFu : l.pg[pg].it[e].id;
  }
};

void Ours(const Test& t, const Case& c, Out& out) {
  const Lay l = ReadCase(t, c);
  const Model m{l};
  auto qualifies = [&](int k, int i, const D*& d, int& e) {
    e = m.Nth(k, i);
    d = e < 0 ? nullptr : &l.pg[k].it[e];
    return d && d->has && (d->drop & 1) == 0;
  };
  int total = 0;
  for (int k = 0; k < l.pages; ++k)
    if (m.List(k))
      for (int i = 0; i < l.pg[k].count; ++i) {
        const D* d;
        int e;
        total += qualifies(k, i, d, e);
      }
  if (total <= 0) {
    out["ret"] = "0", out["T.count"] = "0", out["T.index_count"] = "0";
    return;
  }
  std::vector<std::array<uint32_t, 19>> recs;
  uint32_t menu = 0;
  for (int depth = 0; depth < 128; ++depth)
    for (int k = 0; k < l.pages; ++k) {
      if (m.Depth(k) != depth) continue;
      int yielded = 0;
      if (m.List(k))
        for (int i = 0; i < l.pg[k].count; ++i) {
          const D* d;
          int e;
          if (!qualifies(k, i, d, e)) continue;
          const Pg& p = l.pg[k];
          const int tp = d->target;
          std::array<uint32_t, 19> r{};
          r[0] = d->id;
          r[1] = m.IdOf(p.parent, p.pitem);
          r[2] = menu;
          r[3] = (uint32_t(yielded) << 16) | (uint32_t(depth) << 8) | 1;
          r[4] = d->unlock;
          r[5] = m.IdOf(tp, 0);
          r[6] = d->label, r[7] = d->w8, r[8] = 0, r[9] = d->w16;
          const bool leaf = !(m.Page(tp) && m.List(tp));
          r[10] = (d->f24 & 1u ? 0x01u : 0) | (d->f22 & 1u ? 0x04u : 0) | (leaf ? 0x10u : 0) | (d->f28 & 1u ? 0x40u : 0) |
                  (d->f30 & 1u ? 0x80u : 0) | (d->f32 & 1u ? 0x100u : 0) | (d->f26 & 1u ? 0x400u : 0);
          r[11] = d->code >= 0 ? uint32_t(d->code) : 3000u, r[12] = 2;
          r[13] = d->type >= 0 ? uint32_t(d->type) : 0u, r[14] = d->type >= 0 ? 1u : 0u;
          r[15] = uint32_t(int32_t(d->s34)), r[16] = 0, r[17] = d->w44, r[18] = 0;
          recs.push_back(r);
          ++yielded;
        }
      if (yielded) ++menu;
    }
  out["ret"] = "1", out["T.count"] = IntText((long long)recs.size()), out["T.index_count"] = IntText((long long)menu);
  if (recs.size() > 64) return;
  for (size_t i = 0; i < recs.size(); ++i) {
    std::string s;
    for (size_t w = 0; w < 19; ++w) s += (w ? " " : "") + HexText(recs[i][w]);
    out["rec" + std::to_string(i)] = s;
  }
  for (uint32_t id = 0; id < menu; ++id) {
    long long first = 0, n = 0;
    for (size_t i = 0; i < recs.size(); ++i)
      if (recs[i][2] == id) {
        first = (long long)i;
        for (size_t j = i; j < recs.size() && recs[j][2] == id; ++j) ++n;
        break;
      }
    out["index" + std::to_string(id)] = IntText(first) + "/" + IntText(n);
  }
}

// Planted twin: the menu ids run by page order only (no depth passes).
void OursTwinNoDepth(const Test& t, const Case& c, Out& out) {
  Case flat = c;   // every page is read as depth 0 by clearing the parents: the records then differ in menu id / parent / depth
  for (int k = 0; k < kPages; ++k) flat[size_t(t.Index(PN(k, "parent")))] = -1;
  Ours(t, flat, out);
}

std::vector<std::pair<uint32_t, StubFn>> Stubs() {
  auto s = placement::Stubs();
  s.push_back({0x82C2F7B8, placement::StubMemcpy});   // sub_82C2F7B8: the record copy into the table (r3 dest, r4 src, r5 bytes)
  return s;
}

}  // namespace flowbuild
