// S111 M100-COMP, Q11.1 (docs\research\s111\MENUS100_INV.md): GetString sub_822563F8 (r3 = the text manager, +76 the string list, +80 the key
// table; r4 = the id): which key (bank) of the table the id's lookup uses. Included by equiv_tests.cpp (menus100_tests.h) inside namespace
// equiv; not a header of its own.
//
// Ours = a MODEL of the note's rule, not of our code (nothing of ours implements it): MENUS100_INV Q11.1 + q decomp's db line "ids 40000 to
// 79999 pick slot 0, 1 or 2 by thresholds 60000, 50000, 45000": 60000..79999 key slot 0, 50000..59999 slot 1, 45000..49999 slot 2, 40000..44999
// slot 0; any other id tries the table's keys 3..13 (those that are not -1) in order, then slot 0. A lookup that fails everywhere reports an error
// (sub_82C34EF8) and returns the shared fallback string object [0x836F3A3C]. The list lookup sub_82C38648 (r3 list, r4 key, r5 id) is STUBBED
// (coverage tier): it answers 1 for the key `hit` and 0 for every other, and logs each call's key, so the output is "which keys were asked, in
// which order, and what came back". Our own hook on the function (src\gfx_menu.h) passes every id through to the game when its cvars are off.
namespace textbank {

constexpr int kExtra = 11;   // the loop's table entries +12 .. +52

std::vector<Field> Fields() {
  std::vector<Field> f;
  f.push_back(IntField("in_r4", 0, 100000, 60000, {39999, 40000, 44999, 45000, 49999, 50000, 59999, 60000, 79999, 80000}));
  f.push_back(IntField("key0", -1, 300, 100, {-1, 100, 101, 102}));
  f.push_back(IntField("key1", -1, 300, 101, {-1, 100, 101, 102}));
  f.push_back(IntField("key2", -1, 300, 102, {-1, 100, 101, 102}));
  for (int i = 0; i < kExtra; ++i) f.push_back(IntField("ext" + std::to_string(i), -1, 300, -1, {-1, 100, 101, 102}));
  f.push_back(IntField("hit", -2, 300, 100, {-2, 100, 101, 102}));
  return f;
}

std::vector<std::string> g_calls;
bool g_err = false;
int StubLookup(const Wwe2k14EquivHost* h) {
  const int32_t key = int32_t(uint32_t(h->get_gpr(4)));
  g_calls.push_back(std::to_string(key));
  h->set_gpr(3, key == int32_t(g_test->I(*g_case, "hit")) ? 1 : 0);
  return 1;
}
int StubError(const Wwe2k14EquivHost*) {
  g_err = true;
  return 1;
}

int Game(const Test& t, const Case& c, Out& out, std::string&) {
  g_calls.clear(), g_err = false;
  const uint32_t m = H->scratch(128, 16), tab = H->scratch(64, 16);
  std::memset(G(m), 0, 128), std::memset(G(tab), 0, 64);
  W32(m + 76, 0x1234), W32(m + 80, tab);
  W32(tab + 0, uint32_t(int32_t(t.I(c, "key0")))), W32(tab + 4, uint32_t(int32_t(t.I(c, "key1")))), W32(tab + 8, uint32_t(int32_t(t.I(c, "key2"))));
  for (int i = 0; i < kExtra; ++i) W32(tab + 12 + 4 * uint32_t(i), uint32_t(int32_t(t.I(c, "ext" + std::to_string(i)))));
  H->set_gpr(3, m), H->set_gpr(4, uint64_t(t.I(c, "in_r4")));
  const int rc = H->call(t.target);
  if (rc != 1) return rc;
  std::string s;
  for (size_t i = 0; i < g_calls.size(); ++i) s += (i ? "," : "") + g_calls[i];
  out["keys_asked"] = s;
  out["error_report"] = g_err ? "yes" : "no";
  const uint32_t r = uint32_t(H->get_gpr(3));
  out["result"] = r == 0x836F3A3C ? "fallback string" : IntText(r);
  return 1;
}

// swap = the planted twin: the 50000 and 45000 banks read from each other's slot.
void Model(const Test& t, const Case& c, Out& out, bool swap) {
  const uint32_t id = uint32_t(t.I(c, "in_r4"));
  const int32_t hit = int32_t(t.I(c, "hit"));
  int32_t slot[3] = {int32_t(t.I(c, "key0")), int32_t(t.I(c, "key1")), int32_t(t.I(c, "key2"))};
  std::vector<std::string> asked;
  bool found = false;
  auto ask = [&](int32_t key) {
    asked.push_back(std::to_string(key));
    return key == hit;
  };
  int pick = -1;
  if (id >= 60000 && id < 80000) pick = 0;
  else if (id >= 50000 && id < 80000) pick = swap ? 2 : 1;
  else if (id >= 45000 && id < 80000) pick = swap ? 1 : 2;
  else if (id >= 40000 && id < 80000) pick = 0;
  if (pick >= 0) {
    found = ask(slot[pick]);
  } else {
    for (int i = 0; i < kExtra && !found; ++i) {
      const int32_t key = int32_t(t.I(c, "ext" + std::to_string(i)));
      if (key != -1) found = ask(key);
    }
    if (!found) found = ask(slot[0]);
  }
  std::string s;
  for (size_t i = 0; i < asked.size(); ++i) s += (i ? "," : "") + asked[i];
  out["keys_asked"] = s;
  out["error_report"] = found ? "no" : "yes";
  out["result"] = found ? "1" : "fallback string";
}
void Ours(const Test& t, const Case& c, Out& out) { Model(t, c, out, false); }
void OursTwinSwap(const Test& t, const Case& c, Out& out) { Model(t, c, out, true); }

std::vector<std::pair<uint32_t, StubFn>> Stubs() { return {{0x82C38648, StubLookup}, {0x82C34EF8, StubError}}; }

}  // namespace textbank
