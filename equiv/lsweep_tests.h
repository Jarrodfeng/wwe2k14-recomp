// S111 LSWEEP pilot (tools\equiv\lsweep.py): five [L] claims of db\claims written as models and run against the game's own function by the
// equivalence tester. Each model is the CLAIM's computation (plus the [C] rows of the same note that it rests on, named at each test), not a
// transcription of the listing: a model that agrees on every case with every branch run makes the claim [C]; a difference is the claim's
// error, shrunk. Fields are named as `q decomp FUNC --fields` names them (a loop's per-record words by their concrete offset). Included by
// equiv\equiv_tests.cpp after its helpers (namespace equiv: H, G, R32, W32, R8, W8, IntField, Test, Out, IntText, HexText). Contains
// nothing from the game. Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
#pragma once

namespace equiv {
namespace lsweep {

// ---------------------------------------------------------------------------------------------------------------------------------------
// 1. ENTRBODIES (s110_ENTRBODIES.json): sub_8251D950 "Answers 1 when none of the nine 24-byte records at +392 has a first word in 1..3".
//    The first word is read signed (the claim says "in 1..3"; a word above 3 or at most 0 is outside).
// ---------------------------------------------------------------------------------------------------------------------------------------
namespace loading {
constexpr int kRecs = 9;
std::string Name(int k) { return "[in_r3+" + std::to_string(392 + 24 * k) + "]"; }
std::vector<Field> Fields() {
  std::vector<Field> f;
  for (int k = 0; k < kRecs; ++k) f.push_back(IntField(Name(k), -2, 5, 0, {0, 1, 3, 4}));
  return f;
}
int Game(const Test& t, const Case& c, Out& out, std::string&) {
  const uint32_t r = H->scratch(392 + 24 * kRecs + 16, 16);
  for (int k = 0; k < kRecs; ++k) W32(r + 392 + 24 * k, uint32_t(int32_t(t.I(c, Name(k)))));
  H->set_gpr(3, r);
  const int rc = H->call(t.target);
  if (rc != 1) return rc;
  out["r3"] = IntText(int32_t(uint32_t(H->get_gpr(3))));
  return 1;
}
void Ours(const Test& t, const Case& c, Out& out) {
  bool any = false;
  for (int k = 0; k < kRecs; ++k) {
    const long long w = t.I(c, Name(k));
    any = any || (w >= 1 && w <= 3);
  }
  out["r3"] = IntText(any ? 0 : 1);
}
}  // namespace loading

// ---------------------------------------------------------------------------------------------------------------------------------------
// 2. RUMBLEEND (s111_RUMBLEEND.json): sub_822C6A20(M, p) "the result is the entrant's time in the match in minutes (frame-rate word 60)".
//    The claim's minutes: (exit - entry) frames / 60 per second / 60 per minute, with the note's [C] row for the rest: a u16, at least 1, 0
//    when the word [0x8392DC40] is 0. The claim names the word's value, so the word is 0 or 60 here (field "[0x8392DC40]=60": 0 / 1); the
//    entrant p is a slot of the two 40-word arrays (entry M + 4 x (47221 + p), exit M + 4 x (47261 + p)): 0..39.
// ---------------------------------------------------------------------------------------------------------------------------------------
namespace minutes {
constexpr uint32_t kWord = 0x8392DC40;
const char* const kEntry = "[((in_r4+47221 << 2) + in_r3)]";
const char* const kExit = "[((in_r4+47261 << 2) + in_r3)]";
std::vector<Field> Fields() {
  return {IntField("in_r4", 0, 39, 3, {0, 39}), IntField(kEntry, 0, 400000, 1000, {0, 3600}), IntField(kExit, 0, 400000, 30000, {3599, 3600, 7200}),
          IntField("[0x8392DC40]=60", 0, 1, 1)};
}
int Game(const Test& t, const Case& c, Out& out, std::string&) {
  const uint32_t m = H->scratch(4 * (47261 + 40) + 16, 16);
  const int p = int(t.I(c, "in_r4"));
  W32(m + 4 * (47221 + p), uint32_t(int32_t(t.I(c, kEntry)))), W32(m + 4 * (47261 + p), uint32_t(int32_t(t.I(c, kExit))));
  W32(kWord, t.I(c, "[0x8392DC40]=60") ? 60u : 0u);
  H->set_gpr(3, m), H->set_gpr(4, uint64_t(int64_t(p)));
  const int rc = H->call(t.target);
  if (rc != 1) return rc;
  out["r3"] = IntText(int64_t(H->get_gpr(3) & 0xFFFFFFFFull));
  return 1;
}
void Ours(const Test& t, const Case& c, Out& out) {
  if (!t.I(c, "[0x8392DC40]=60")) {
    out["r3"] = "0";
    return;
  }
  const int32_t frames = int32_t(t.I(c, kExit) - t.I(c, kEntry));
  const int32_t mins = frames / 60 / 60;
  out["r3"] = IntText(mins == 0 ? 1 : (mins & 0xFFFF));
}
}  // namespace minutes

// ---------------------------------------------------------------------------------------------------------------------------------------
// 3. THREADSDATA (s111_THREADSDATA.json): sub_8339F790(R, i, a, ch) "for a channel >= 16 it reads gear byte +252 + 4a + ch (not ch - 16)".
//    The note's layout ([C] rows of the SSTL read-end): R+48 -> 1644-byte superstar entries, R+56 -> 452-byte gear entries, entry i of
//    each; a channel below 16 reads the superstar entry's byte +844 + 16a + c (the note's table, [C] chain). Each table byte is a hash of its
//    offset and the case's salt, so a wrong offset reads a different byte.
// ---------------------------------------------------------------------------------------------------------------------------------------
namespace preset {
constexpr int kEntries = 4, kSstl = 1644, kGear = 452;
uint8_t Byte(uint32_t table, uint32_t off, uint32_t salt) { return uint8_t(((off + 1) * 2654435761u ^ (salt * 40503u + table)) >> 13); }
std::vector<Field> Fields() {
  return {IntField("in_r4", 0, kEntries - 1, 1), IntField("in_r5", 0, 9, 2, {0, 9}), IntField("in_r6", 0, 31, 3, {15, 16, 19}),
          IntField("salt", 0, 1000, 7)};
}
int Game(const Test& t, const Case& c, Out& out, std::string&) {
  const uint32_t salt = uint32_t(t.I(c, "salt"));
  const uint32_t r = H->scratch(64, 16), s = H->scratch(kSstl * kEntries, 16), g = H->scratch(kGear * kEntries, 16);
  for (uint32_t k = 0; k < uint32_t(kSstl * kEntries); ++k) W8(s + k, Byte(1, k, salt));
  for (uint32_t k = 0; k < uint32_t(kGear * kEntries); ++k) W8(g + k, Byte(2, k, salt));
  W32(r + 48, s), W32(r + 52, kEntries), W32(r + 56, g), W32(r + 60, kEntries);
  H->set_gpr(3, r), H->set_gpr(4, uint64_t(t.I(c, "in_r4"))), H->set_gpr(5, uint64_t(t.I(c, "in_r5"))), H->set_gpr(6, uint64_t(t.I(c, "in_r6")));
  const int rc = H->call(t.target);
  if (rc != 1) return rc;
  out["r3"] = IntText(int64_t(H->get_gpr(3) & 0xFFFFFFFFull));
  return 1;
}
void Ours(const Test& t, const Case& c, Out& out) {
  const uint32_t salt = uint32_t(t.I(c, "salt")), i = uint32_t(t.I(c, "in_r4")), a = uint32_t(t.I(c, "in_r5")), ch = uint32_t(t.I(c, "in_r6"));
  out["r3"] = IntText(ch >= 16 ? Byte(2, kGear * i + 252 + 4 * a + ch, salt) : Byte(1, kSstl * i + 844 + 16 * a + ch, salt));
}
}  // namespace preset

// ---------------------------------------------------------------------------------------------------------------------------------------
// 4. UNIVSAVE (s111_UNIVSAVE.json): sub_82B6F5E8(view) "the view holds pointers to 14 of the 19 records at U + 261568". U = [0x8376F3C0] +
//    486564 (sub_827D9020); the records are 52 bytes (the note's U table, [C]); the view's pointers to them sit at +456 + 4k (the note's view
//    row, [C]). Output: every view word (0..1020) that points into the 19 records, as "record k (+off)": the claim = records 0..13, each once,
//    at +456 + 4k, and none into records 14..18. With U null (the word's value -486564) the view holds no record pointer.
// ---------------------------------------------------------------------------------------------------------------------------------------
namespace univview {
constexpr uint32_t kBaseWord = 0x8376F3C0, kUOff = 486564, kRecs = 261568, kRecSize = 52, kRecCount = 19, kView = 1024;
std::vector<Field> Fields() { return {IntField("ret#sub_827D9020=0", 0, 1, 0)}; }
std::string Pointers(uint32_t view, uint32_t u) {
  std::string s;
  for (uint32_t off = 0; off < kView; off += 4) {
    const uint32_t w = R32(view + off);
    if (!u || w < u + kRecs || w >= u + kRecs + kRecSize * kRecCount) continue;
    const uint32_t d = w - (u + kRecs);
    s += " +" + std::to_string(off) + "=rec" + std::to_string(d / kRecSize) + (d % kRecSize ? "+" + std::to_string(d % kRecSize) : "");
  }
  return s.empty() ? "none" : s;
}
int Game(const Test& t, const Case& c, Out& out, std::string&) {
  const bool null_u = t.I(c, "ret#sub_827D9020=0") != 0;
  const uint32_t view = H->scratch(kView, 16), blk = null_u ? 0 : H->scratch(637824 + 64, 16);
  W32(kBaseWord, null_u ? uint32_t(0u - kUOff) : blk - kUOff);
  H->set_gpr(3, view);
  const int rc = H->call(t.target);
  if (rc != 1) return rc;
  out["records"] = Pointers(view, null_u ? 0 : blk);
  return 1;
}
void Ours(const Test& t, const Case& c, Out& out) {
  if (t.I(c, "ret#sub_827D9020=0")) {
    out["records"] = "none";
    return;
  }
  std::string s;
  for (int k = 0; k < 14; ++k) s += " +" + std::to_string(456 + 4 * k) + "=rec" + std::to_string(k);
  out["records"] = s;
}
}  // namespace univview

// ---------------------------------------------------------------------------------------------------------------------------------------
// 5. PINCORE (s111_PINCORE.json): sub_825B0AB8(J, slot) "the judge's rope flag for a slot (J+152, or J+156 + 4 x slot in special-referee
//    types)". Special referee = rule byte G+45 non-zero (RULESGATE's [C] row of sub_822BA738; PINCORE: "under a special referee (G+45)"),
//    G = 0x8376AAF4 (sub_827D7578). The slot is one of the nine judge slots 0..8. G's byte 0 (the match type) and byte 37 vary too: the
//    claim says they do not matter.
// ---------------------------------------------------------------------------------------------------------------------------------------
namespace ropeflag {
constexpr uint32_t kG = 0x8376AAF4;
std::string Slot(int k) { return "[in_r3+" + std::to_string(156 + 4 * k) + "]"; }
std::vector<Field> Fields() {
  std::vector<Field> f = {IntField("in_r4", 0, 8, 0), IntField("[in_r3+152]", 0, 3, 1), IntField("[0x8376AAF4]", 0, 120, 1, {34, 35, 36, 37, 96, 112, 117, 118}),
                          IntField("[0x8376AAF4+37]", 0, 2, 0, {0}), IntField("[0x8376AAF4+45]", 0, 2, 0, {0})};
  for (int k = 0; k < 9; ++k) f.push_back(IntField(Slot(k), 0, 3, 2));
  return f;
}
int Game(const Test& t, const Case& c, Out& out, std::string&) {
  const uint32_t j = H->scratch(256, 16);
  W32(j + 152, uint32_t(t.I(c, "[in_r3+152]")));
  for (int k = 0; k < 9; ++k) W32(j + 156 + 4 * k, uint32_t(t.I(c, Slot(k))));
  W8(kG, uint8_t(t.I(c, "[0x8376AAF4]"))), W8(kG + 37, uint8_t(t.I(c, "[0x8376AAF4+37]"))), W8(kG + 45, uint8_t(t.I(c, "[0x8376AAF4+45]")));
  H->set_gpr(3, j), H->set_gpr(4, uint64_t(t.I(c, "in_r4")));
  const int rc = H->call(t.target);
  if (rc != 1) return rc;
  out["r3"] = IntText(int64_t(H->get_gpr(3) & 0xFFFFFFFFull));
  return 1;
}
void Ours(const Test& t, const Case& c, Out& out) {
  const bool special = t.I(c, "[0x8376AAF4+45]") != 0;
  out["r3"] = IntText(special ? t.I(c, Slot(int(t.I(c, "in_r4")))) : t.I(c, "[in_r3+152]"));
}
// The rule the run REFUTED the claim into (shrunk [0x8376AAF4+45]=1: game J+152, the claim J+156): the per-slot word needs G+45 AND (G+37
// non-zero OR the match type byte G+0 in {34, 35, 96, 112, 117}: MATCHLIST's set of sub_822BB558 less the over-the-top set of
// sub_822BB5C8, both [C] rows of their own notes).
void OursFixed(const Test& t, const Case& c, Out& out) {
  const long long type = t.I(c, "[0x8376AAF4]");
  const bool kind = type == 34 || type == 35 || type == 96 || type == 112 || type == 117;
  const bool per_slot = t.I(c, "[0x8376AAF4+45]") != 0 && (t.I(c, "[0x8376AAF4+37]") != 0 || kind);
  out["r3"] = IntText(per_slot ? t.I(c, Slot(int(t.I(c, "in_r4")))) : t.I(c, "[in_r3+152]"));
}
}  // namespace ropeflag

// ---------------------------------------------------------------------------------------------------------------------------------------
// 6. CAARING (s111_CAARING.json, the [L] row at 0x822A27F8): sub_822A2760(M) state 1 "writes the stage's ring-side word through
//    [0x83763544] with no null test, so it runs with the stage built". The claim's computation, with the note's [C] rows (state 1 waits
//    sub_822AB370 == 1 and [M+32] == 1, skips on [[M+120]+34696] == 1, the work record W = [[M+120]+34656]): the work byte +140 = 0 / 1
//    stores 0 / 1 at [stage+21180] (other values leave it), [stage+21196] = [W+152], state [M+72] = 2 after the tick. The claim = that holds
//    when [0x83763544] is 0 too (the stores land at 21180 / 21196). sub_822AB370's virtual call (the sub-object at R+16, slot 2) is given a
//    vtable whose slot 2 is sub_824E6130 (a stage setter: it returns its r3, non-zero, and writes nothing on a zeroed object).
// ---------------------------------------------------------------------------------------------------------------------------------------
namespace ringload {
constexpr uint32_t kStageWord = 0x83763544, kSent = 0xDEADBEEF;
std::vector<Field> Fields() {
  return {IntField("stage_null", 0, 1, 0), IntField("[[[in_r3+120]+34656]+140]", 0, 2, 0, {0, 1, 2}),
          IntField("[[[in_r3+120]+34656]+152]", 0, 16383, 0, {0, 1, 255, 16383})};
}
int Game(const Test& t, const Case& c, Out& out, std::string&) {
  const bool null_s = t.I(c, "stage_null") != 0;
  const uint32_t m = H->scratch(512, 16), r = H->scratch(34800, 16), w = H->scratch(490000, 16), b = H->scratch(65536, 16);
  const uint32_t s = null_s ? 0 : H->scratch(24000, 16), vt = H->scratch(64, 16);
  const uint32_t buf204 = H->scratch(8192, 16), buf208 = H->scratch(8192, 16), buf148 = H->scratch(8192, 16);
  W32(m + 72, 1), W32(m + 32, 1), W32(m + 120, r), W32(m + 56, b);
  W32(b + 204, buf204), W32(b + 208, buf208), W32(b + 148, buf148);
  // the category reader sub_822B1620 writes its view through [b+212] and reads the tables at +152 / +244 (all zeroed blocks here)
  W32(b + 212, H->scratch(8192, 16)), W32(b + 152, H->scratch(8192, 16)), W32(b + 244, H->scratch(16384, 16));
  W32(0x836F4C74, 0), W32(0x836F2C08, 0);
  W32(r + 34656, w), W32(r + 34648, 15), W32(r + 16, vt), W32(vt + 8, 0x824E6130);
  W8(w + 140, uint8_t(t.I(c, "[[[in_r3+120]+34656]+140]"))), W32(w + 152, uint32_t(t.I(c, "[[[in_r3+120]+34656]+152]")));
  W32(kStageWord, s);
  if (s) W32(s + 21180, kSent), W32(s + 21196, kSent);   // low guest memory is not writable: with a null stage the store at +0x9C / +0xB4 faults
  H->set_gpr(3, m);
  const int rc = H->call(t.target);
  if (null_s) {   // the claim: no null test, so the store through the null word is reached = a guest fault (rc 0), not a skip
    out["null_stage"] = rc == 0 ? "faults" : rc == 1 ? "returns" : "left the allowlist";
    return 1;
  }
  if (rc != 1) return rc;
  out["state"] = IntText(R32(m + 72));
  out["word21180"] = IntText(R32(s + 21180));
  out["word21196"] = IntText(R32(s + 21196));
  return 1;
}
void Ours(const Test& t, const Case& c, Out& out) {
  if (t.I(c, "stage_null")) {
    out["null_stage"] = "faults";
    return;
  }
  const long long flag = t.I(c, "[[[in_r3+120]+34656]+140]");
  out["state"] = "2";
  out["word21180"] = IntText(flag == 0 || flag == 1 ? uint32_t(flag) : kSent);
  out["word21196"] = IntText(uint32_t(t.I(c, "[[[in_r3+120]+34656]+152]")));
}
}  // namespace ringload

inline void Add(std::vector<Test>& v) {
  struct Row {
    const char* name;
    uint32_t target;
    std::vector<Field> (*fields)();
    GameFn game;
    OursFn ours;
    const char* what;
  };
  const Row rows[] = {
      {"l_entrbodies_loading", 0x8251D950, loading::Fields, loading::Game, loading::Ours,
       "LSWEEP ENTRBODIES [L]: answers 1 when none of the nine 24-byte records at +392 has a first word in 1..3"},
      {"l_rumbleend_minutes", 0x822C6A20, minutes::Fields, minutes::Game, minutes::Ours,
       "LSWEEP RUMBLEEND [L]: the entrant's time in the match in minutes (frame-rate word 60)"},
      {"l_threads_preset", 0x8339F790, preset::Fields, preset::Game, preset::Ours,
       "LSWEEP THREADSDATA [L]: a channel >= 16 reads gear byte +252 + 4a + ch (below 16: SSTL +844 + 16a + c)"},
      {"l_univsave_view", 0x82B6F5E8, univview::Fields, univview::Game, univview::Ours,
       "LSWEEP UNIVSAVE [L]: the view holds pointers to 14 of the 19 records at U + 261568"},
      {"l_pincore_ropeflag", 0x825B0AB8, ropeflag::Fields, ropeflag::Game, ropeflag::Ours,
       "LSWEEP PINCORE [L]: the judge's rope flag for a slot (J+152, or J+156 + 4 x slot in special-referee types)"},
      {"l_pincore_ropeflag_fixed", 0x825B0AB8, ropeflag::Fields, ropeflag::Game, ropeflag::OursFixed,
       "LSWEEP PINCORE, the refuted row's correction: J+156 + 4 x slot when G+45 and (G+37 or the match type in 34, 35, 96, 112, 117), else J+152"},
      {"l_caaring_stage", 0x822A2760, ringload::Fields, ringload::Game, ringload::Ours,
       "LSWEEP CAARING [L]: state 1 writes the stage's ring-side word through [0x83763544] with no null test"},
  };
  for (const Row& r : rows) {
    Test x;
    x.name = r.name, x.target = r.target, x.fields = r.fields(), x.game = r.game, x.ours = r.ours, x.what = r.what;
    v.push_back(x);
  }
}

}  // namespace lsweep
}  // namespace equiv
