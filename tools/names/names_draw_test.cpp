// Session 101 lane 18 (review fix 1): runs the REAL draw-origin step (names_draw.h TakeDrawOriginInto, InternName, the intern tables) with two 2D
// draws of different widgets in one frame, and checks the frame's string table the way the capture writer serialises it (reader.h FrameString =
// InternName on the frame's strings / string_map). No game, no SDK. Built and run by tools\names\ui_names_selftest.py.
//   names_draw_test.exe            checks; exit 0 = all pass
//   names_draw_test.exe --planted  the old call shape (a per-draw stack buffer handed to the pointer-keyed table); exit 0 only if the bug SHOWS
//   S101 U1 plant: built with -DS101_PLANT_NO_TEXT_PROMOTION (names_draw.h without the sprite -> text line) the main checks must FAIL (exit 1)
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

#include "names_draw.h"

namespace og = rdr_adapter::origin;
namespace nd = rdr_adapter::names_draw;

static constexpr size_t kBig = 600;
static int failures = 0;
static void Check(bool ok, const char* what) {
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++failures;
}

static const char* At(const std::string& strings, uint32_t name) { return name ? strings.c_str() + (name - 1) : ""; }

// A 2D draw as the replay hands it over: a record tag in t_replay_tag, op 19.
static rdr_adapter::DrawOrigin Draw(std::string& strings, std::unordered_map<const char*, uint32_t>& map, uint32_t call_site, uint32_t pass_site,
                                    uint32_t widget_id) {
  og::Key k;
  k.call_site = call_site, k.pass_site = pass_site, k.object = 0x83A00000u, k.part = 7, k.op = 19, k.widget = widget_id;
  og::t_replay_tag = og::Global().Intern(k);
  og::t_replay_op = 19;
  return nd::TakeDrawOriginInto(strings, map);
}

// A glyph quad as UiScope records it: Key.text / Key.run taken from the open text draw (og::RunFor), only for a text-object function.
static rdr_adapter::DrawOrigin Glyph(std::string& strings, std::unordered_map<const char*, uint32_t>& map, uint32_t call_site, uint32_t widget_id,
                                     bool text_object_fn, rdr_adapter::TextRun* run) {
  const og::RunTake rt = og::RunFor(text_object_fn);
  og::Key k;
  k.call_site = call_site, k.pass_site = 0x82BD0600u, k.object = 0x83A00000u, k.part = 7, k.op = 19, k.widget = widget_id, k.text = rt.text, k.run = rt.run;
  og::t_replay_tag = og::Global().Intern(k);
  og::t_replay_op = 19;
  return nd::TakeDrawOriginInto(strings, map, run);
}

static size_t Units(const char16_t* s, uint16_t* out) { size_t n = 0; for (; s[n]; ++n) out[n] = uint16_t(s[n]); out[n] = 0; return n + 1; }

int main(int argc, char** argv) {
  const bool planted = argc > 1 && !std::strcmp(argv[1], "--planted");
  og::g_on = true;
  og::WidgetInfo a, b, c;
  a.seq = 75, a.menu = 2, a.element = 0x83A00010u, a.node = 0x83B00020u, a.text_id = 45004;
  b.seq = 75, b.menu = 2, b.element = 0x83A00110u, b.node = 0x83B00020u, b.text_id = 0;  // element known, text id not
  c.seq = 9;                                                                              // screen only
  const uint32_t ia = og::Widgets().Intern(a), ib = og::Widgets().Intern(b), ic = og::Widgets().Intern(c);
  Check(ia && ib && ic && ia != ib && ib != ic, "three distinct widget ids");
  Check(og::Widgets().Intern(a) == ia, "interning the same widget again gives the same id (thread-local cache)");

  std::string strings;
  std::unordered_map<const char*, uint32_t> map;
  const uint32_t kSpriteFn = 0x82BC4A00u;  // inside sub_82BC4890 (origin table: sprite)
  if (planted) {
    // The first version: format into a per-draw stack buffer and hand that to the pointer-keyed table.
    uint32_t off[2] = {};
    const og::WidgetInfo* w[2] = {&a, &b};
    for (int i = 0; i < 2; ++i) {
      char buf[160];
      std::snprintf(buf, sizeof(buf), "%s", og::FormatWidget(*w[i]).c_str());
      off[i] = nd::InternName(strings, map, buf);
    }
    const bool shows = off[0] == off[1];  // the bug: the second widget gets the first one's string
    std::printf("%s  planted old call shape: two widgets got %s offsets\n", shows ? "ok  " : "FAIL", shows ? "the same" : "different");
    return shows ? 0 : 1;
  }

  const auto d1 = Draw(strings, map, kSpriteFn, 0, ia);
  const auto d2 = Draw(strings, map, kSpriteFn, 0, ib);
  const auto d3 = Draw(strings, map, kSpriteFn, 0, ia);
  const auto d4 = Draw(strings, map, kSpriteFn, 0, ic);
  Check(d1.klass == rdr_adapter::DrawClass::Sprite, "the draw is classed sprite by its call site");
  Check(d1.widget && d2.widget && d1.widget != d2.widget, "two widgets in one frame get two different string offsets");
  Check(std::string(At(strings, d1.widget)) == og::FormatWidget(a), "draw 1's widget text is its own");
  Check(std::string(At(strings, d2.widget)) == og::FormatWidget(b), "draw 2's widget text is its own");
  Check(d3.widget == d1.widget, "the same widget again reuses its string");
  Check(std::string(At(strings, d4.widget)) == "screen seq=9 menu=-1", "a screen-only widget reads the screen alone");
  // Review 5: a 2D scope's pass_site (its caller) inside a scene-pass range must not make the draw a scene pass.
  const auto d5 = Draw(strings, map, kSpriteFn, 0x82BEF750u, ia);
  Check(d5.klass == rdr_adapter::DrawClass::Sprite, "a pass_site inside a kPasses range does not turn a sprite draw into a scene pass");
  // Counters.
  const nd::NamesCount& n = nd::g_names;
  Check(n.ui_draws == 5 && n.ui_with_element == 4 && n.ui_screen_only == 1 && n.ui_element_no_text == 1, "counters: 5 2D draws, 4 with an element (1 without text id), 1 screen only");
  // Review 3: the registry only matches an element whose live words still agree.
  og::WidgetTable::Element e;
  og::Widgets().NoteText(0x83A00500u, 0x82065FF0u, 0x83B00100u, 0x83C00000u, 40001);
  Check(og::Widgets().FindElement(0x83A00500u, 0x82065FF0u, 0x83B00100u, 0x83C00000u, e) && e.text_id == 40001, "registry: unchanged element matches");
  Check(!og::Widgets().FindElement(0x83A00500u, 0x82065FF0u, 0x83B00100u, 0x83C00010u, e), "registry: a recycled address (string word changed) does not match");
  Check(!og::Widgets().FindElement(0x83A00500u, 0x82065FF0u, 0x83B00200u, 0x83C00000u, e), "registry: a recycled address (node changed) does not match");
  Check(!og::Widgets().FindElement(0x83A00500u, 0x82000000u, 0x83B00100u, 0x83C00000u, e), "registry: a recycled address (other class) does not match");
  // The real order (review of 3ddc317f): the element's draw method (vtable slot 15, sub_82B553F8) opens the element scope, the set-string and the
  // text object's draw chain run INSIDE it, and the glyph quads (sub_82BC3518 / sub_82BC8180) are recorded there: the widget id is built at
  // each quad from the open element. After the scope closes a draw is screen only. A second element's scope names the second element.
  const og::OpenElement el1{0x83A00010u, 0x83B00020u, 45004}, el2{0x83A00110u, 0x83B00020u, 45008}, none;
  const uint32_t q1 = og::WidgetIdFor(el1, 75, 2);   // quad 1 recorded inside element 1's draw
  const uint32_t q1b = og::WidgetIdFor(el1, 75, 2);  // its second quad
  const uint32_t q2 = og::WidgetIdFor(el2, 75, 2);   // a quad inside element 2's draw
  const uint32_t q0 = og::WidgetIdFor(none, 75, 2);  // a quad recorded outside any element draw
  const auto g1 = Draw(strings, map, 0x82BC3600u, 0, q1);  // inside sub_82BC3518 (origin table: sprite)
  const auto g1b = Draw(strings, map, 0x82BC3600u, 0, q1b);
  const auto g2 = Draw(strings, map, 0x82BC8200u, 0, q2);  // inside sub_82BC8180
  const auto g0 = Draw(strings, map, 0x82BC3600u, 0, q0);
  Check(std::string(At(strings, g1.widget)) == "screen seq=75 menu=2; element 0x83A00010 node 0x83B00020 text 45004", "a glyph quad inside element 1's draw names element 1 and its text id");
  Check(g1b.widget == g1.widget, "the element's second quad shares the string");
  Check(std::string(At(strings, g2.widget)) == "screen seq=75 menu=2; element 0x83A00110 node 0x83B00020 text 45008", "a glyph quad inside element 2's draw names element 2");
  Check(std::string(At(strings, g0.widget)) == "screen seq=75 menu=2", "a quad outside any element draw is screen only (the scope closed)");
  const nd::NamesCount& n2 = nd::g_names;
  Check(n2.ui_with_element >= 7 && n2.ui_screen_only >= 2, "counters keep counting with-element and screen-only draws");
  // Labels (review of a83e727d): the text a text draw shows, from the UTF-16 string of sub_82BD06B8 (the hook keeps it in t_label for the call).
  auto U = [](const char* t, uint16_t* out) { size_t n = 0; for (; t[n]; ++n) out[n] = uint16_t(uint8_t(t[n])); out[n] = 0; return n + 1; };
  uint16_t u[64];
  char lab[40];
  size_t nu = U("LABEL ONE", u);
  Check(og::FilterLabel(u, nu, lab) && std::string(lab) == "LABEL ONE", "filter: plain text passes");
  u[0] = 0;
  Check(!og::FilterLabel(u, 1, lab), "filter: a string starting with NUL has no label");
  nu = U("AB", u); u[1] = 0x07;
  Check(!og::FilterLabel(u, 3, lab), "filter: a control unit rejects the string");
  nu = U("A\"B", u);
  Check(!og::FilterLabel(u, nu, lab), "filter: a quote rejects the string");
  nu = U("AB", u); u[1] = 0x00E9;
  Check(!og::FilterLabel(u, 3, lab), "filter: a unit >= 0x80 rejects the string");
  nu = U("X", u); u[0] = 0x4E00;
  Check(!og::FilterLabel(u, 2, lab), "filter: a wide (non-ASCII) unit rejects the string");
  for (int i = 0; i < 60; ++i) u[i] = uint16_t('a' + i % 26);
  u[60] = 0;
  Check(og::FilterLabel(u, 61, lab) && std::strlen(lab) == 39 && std::strcmp(lab + 36, "...") == 0, "filter: an over-long string is cut to 36 chars + \"...\" (marked)");
  for (int i = 0; i < 39; ++i) u[i] = 'z';
  u[39] = 0;
  Check(og::FilterLabel(u, 40, lab) && std::strcmp(lab + 36, "...") == 0, "filter: exactly 39 units is already marked truncated");
  // The scope predicate: a label exists only inside a text draw's call, only with labels on, only for the text-object functions.
  Check(og::LabelFor(true, true) == nullptr, "no open text draw: no label");
  {
    nu = U("LABEL ONE", u);
    og::LabelScope outer(u, nu);
    Check(og::LabelFor(true, true) && std::string(og::LabelFor(true, true)) == "LABEL ONE", "inside a text draw: the label");
    Check(og::LabelFor(false, true) == nullptr, "labels off (live mirror only): none");
    Check(og::LabelFor(true, false) == nullptr, "a plain sprite function (not a text-object function): none");
    {
      nu = U("LABEL TWO", u);
      og::LabelScope inner(u, nu);
      Check(std::string(og::LabelFor(true, true)) == "LABEL TWO", "a nested text draw has its own label");
    }
    Check(std::string(og::LabelFor(true, true)) == "LABEL ONE", "after the nested call the outer label is back");
  }
  Check(og::LabelFor(true, true) == nullptr, "after the call: no label again");
  const uint32_t lab1 = og::WidgetIdFor(none, 75, 2, "LABEL ONE"), lab2 = og::WidgetIdFor(none, 75, 2, "LABEL TWO");
  const uint64_t named_before = nd::g_names.ui_named, label_before = nd::g_names.ui_with_label;
  const auto l1 = Draw(strings, map, 0x82BC3600u, 0, lab1);
  const auto l2 = Draw(strings, map, 0x82BC3600u, 0, lab2);
  Check(l1.widget && l2.widget && l1.widget != l2.widget, "two labels give two different strings in one frame");
  Check(std::string(At(strings, l1.widget)) == "screen seq=75 menu=2; label \"LABEL ONE\"", "a label-only widget reads screen + label");
  Check(nd::g_names.ui_named == named_before + 2 && nd::g_names.ui_with_label == label_before + 2, "labelled draws count as with widget");
  // The label cap (review 4): once kMaxLabelWidgets distinct labels exist, further labels fall back to the screen entry and are counted.
  char dyn[40];
  uint32_t last = 0;
  for (size_t i = 0; i < og::WidgetTable::kMaxLabelWidgets + 20; ++i) {
    std::snprintf(dyn, sizeof(dyn), "T%zu", i);
    last = og::WidgetIdFor(none, 75, 2, dyn);
  }
  og::WidgetInfo wl;
  Check(og::Widgets().labels() <= og::WidgetTable::kMaxLabelWidgets && og::Widgets().label_overflow() >= 20, "label cap: the table stops growing and counts the overflow");
  Check(og::Widgets().Get(last, wl) && wl.label[0] == 0 && wl.seq == 75, "label cap: past the cap a draw keeps its screen entry");
  // ---- S101 U1: glyph quads are class text; the whole run string rides beside the label --------------------------------------------------
  {
    uint16_t u2[kBig];
    rdr_adapter::TextRun run;
    // No text draw open: a quad from a text-object function (the plain sprite slot of sub_82BC3518) stays a sprite and has no run.
    const auto s0 = Glyph(strings, map, 0x82BC3600u, ia, true, &run);
    Check(s0.klass == rdr_adapter::DrawClass::Sprite && run.string == 0, "U1: sub_82BC3518 outside a text draw (a plain textured rectangle) stays class sprite, no run");
    size_t nu2 = Units(u"Hello, WORLD", u2);
    {
      og::TextRunScope t1(true, u2, nu2, false);
      Check(og::t_run.open && og::t_run.run != 0, "U1: inside a text draw the scope is open and the run interned");
      rdr_adapter::TextRun r1, r2, r3;
      const auto a1 = Glyph(strings, map, 0x82BC3600u, ia, true, &r1);
      const auto a2 = Glyph(strings, map, 0x82BC8200u, ia, true, &r2);  // sub_82BC8180: a glyph route too
      Check(a1.klass == rdr_adapter::DrawClass::Text && a2.klass == rdr_adapter::DrawClass::Text, "U1: glyph quads from sub_82BC3518 and sub_82BC8180 inside a text draw are class text");
      Check(r1.string && r1.string == r2.string && std::string(At(strings, r1.string)) == "Hello, WORLD" && r1.units == 12 && r1.flags == 0, "U1: the whole run string (not the 40-char label) with its unit count; one string per frame");
      const auto p = Glyph(strings, map, 0x82BC4A00u, ia, false, &r3);  // a sprite function (not a text-object function) inside the same call
      Check(p.klass == rdr_adapter::DrawClass::Sprite && r3.string == 0, "U1: a sprite-function quad inside a text draw stays class sprite (only text-object functions take the scope)");
      // Review D3: the gradient draw sub_82BC6A18 runs inside sub_82BC8180 and keeps its outer (text) scope, so its Key carries the text flag; it is no glyph.
      rdr_adapter::TextRun r4;
      const auto gr = Glyph(strings, map, 0x82BC6B00u, ia, true, &r4);
      Check(gr.klass == rdr_adapter::DrawClass::Sprite && r4.string == 0, "U1: a gradient quad (sub_82BC6A18) inside a text draw's outer scope stays class sprite, no run");
      // A nested text draw has its own run; the outer one is back after it.
      nu2 = Units(u"Inner", u2);
      {
        og::TextRunScope t2(true, u2, nu2, false);
        rdr_adapter::TextRun rn;
        Glyph(strings, map, 0x82BC3600u, ia, true, &rn);
        Check(std::string(At(strings, rn.string)) == "Inner", "U1: a nested text draw has its own run");
      }
      rdr_adapter::TextRun ro;
      Glyph(strings, map, 0x82BC3600u, ia, true, &ro);
      Check(std::string(At(strings, ro.string)) == "Hello, WORLD", "U1: after the nested call the outer run is back");
    }
    Check(!og::t_run.open, "U1: after the text draw the scope is closed");
    // Labels off: the class is still text (a scope, no string).
    {
      og::TextRunScope t3(false, nullptr, 0, false);
      rdr_adapter::TextRun rl;
      const auto a = Glyph(strings, map, 0x82BC3600u, ia, true, &rl);
      Check(a.klass == rdr_adapter::DrawClass::Text && rl.string == 0, "U1: labels off: glyph quads are still class text, with no run string");
    }
    // Non-ASCII survives: icon codes (private use), accented letters, a surrogate pair (one code point), and U+2019.
    nu2 = Units(u"A\xE00C \xE9\xD83D\xDE00\x2019", u2);
    {
      og::TextRunScope t4(true, u2, nu2, false);
      rdr_adapter::TextRun rw;
      Glyph(strings, map, 0x82BC3600u, ia, true, &rw);
      Check(std::string(At(strings, rw.string)) == "A\xEE\x80\x8C \xC3\xA9\xF0\x9F\x98\x80\xE2\x80\x99" && rw.units == 6, "U1: icon codes, accents, a surrogate pair and U+2019 are carried as UTF-8 (units counts code points)");
    }
    // A cut run is marked, never silent; a long run is carried whole up to kRunUnits.
    for (size_t i = 0; i < rdr_adapter::kRunUnits; ++i) u2[i] = uint16_t('a' + i % 26);
    {
      og::TextRunScope t5(true, u2, rdr_adapter::kRunUnits, true);
      rdr_adapter::TextRun rt;
      Glyph(strings, map, 0x82BC3600u, ia, true, &rt);
      Check(rt.units == rdr_adapter::kRunUnits && (rt.flags & rdr_adapter::kRunTruncated) && std::strlen(At(strings, rt.string)) == rdr_adapter::kRunUnits, "U1: a run cut at kRunUnits is marked truncated and counted");
    }
    // The table cap: dynamic text (a clock) must not grow it for the session.
    for (size_t i = 0; i < og::TextRuns::kMaxRuns + 20; ++i) {
      char tmp[24];
      const int len = std::snprintf(tmp, sizeof(tmp), "T%zu", i);
      for (int j = 0; j <= len; ++j) u2[j] = uint16_t(uint8_t(tmp[j]));
      og::TextRunScope td(true, u2, size_t(len) + 1, false);
    }
    Check(og::Runs().size() <= og::TextRuns::kMaxRuns && og::Runs().overflow() >= 20, "U1: the run table stops at its cap and counts the overflow");
    {
      nu2 = Units(u"past the cap", u2);
      og::TextRunScope te(true, u2, nu2, false);
      rdr_adapter::TextRun rf;
      const auto a = Glyph(strings, map, 0x82BC3600u, ia, true, &rf);
      Check(a.klass == rdr_adapter::DrawClass::Text && rf.string == 0, "U1: past the cap a glyph quad keeps its class and loses only the string");
    }
    Check(nd::g_names.ui_runs >= 6 && nd::g_names.ui_runs_truncated == 1, "U1: counters: run strings carried, one truncated");
    // Review D2: a capture start clears the room (the cap and the overflow count) but never reuses an id.
    nu2 = Units(u"Hello, WORLD", u2);
    uint32_t old_id;
    {
      og::TextRunScope tq(true, u2, nu2, false);
      old_id = og::t_run.run;
    }
    og::Runs().Clear();
    Check(og::Runs().size() == 0 && og::Runs().overflow() == 0, "U1: Clear: the table's room and its overflow count start again");
    nu2 = Units(u"after the clear", u2);
    {
      og::TextRunScope tr(true, u2, nu2, false);
      rdr_adapter::TextRun rc;
      Glyph(strings, map, 0x82BC3600u, ia, true, &rc);
      Check(rc.string && std::string(At(strings, rc.string)) == "after the clear" && og::t_run.run > old_id && og::Runs().size() == 1, "U1: Clear: a new string is carried with a new id");
    }
    {
      const og::TextRuns::Run* old = og::Runs().Get(old_id);
      Check(old && old->utf8 == "Hello, WORLD", "U1: Clear: an id handed out before still names its own string (draw-origin keys hold it)");
    }
  }
  std::printf("%s\n", failures ? "FAILED" : "all pass");
  return failures ? 1 : 0;
}
