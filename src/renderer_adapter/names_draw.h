// The reader's draw-origin step, without the reader (Session 101 lane 18, so tools\names\names_draw_test.cpp can run the REAL code: the
// review found a bug that only the real string-table path shows). No game or SDK includes. Used by reader.h (TakeDrawOrigin, FrameString).
//   InternName      the frame's string table: NUL-separated names, a name field = offset + 1; keyed by POINTER, so a name must live as long as
//                   the frame (a literal, or WidgetTable's stable texts). Never pass a stack buffer.
//   NamesCount      the per-window counters behind the [rdr][names] line
//   TakeDrawOriginInto  turns the tag of the record being replayed (origin::t_replay_tag / t_replay_op) into a DrawOrigin
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>

#include "names.h"
#include "origin_intern.h"
#include "origin_table.h"

namespace rdr_adapter::names_draw {

static_assert(origin::kLabelUnitsRun == kRunUnits, "origin_intern.h reads the same number of units names.h documents");

struct NamesCount {
  uint64_t draws = 0, untagged = 0, unclassified = 0, stale = 0;
  uint64_t by_class[uint8_t(DrawClass::Count)] = {};
  // S101 lane18 (NAMES.md "2D draws"): 2D draws (class sprite / text / movie) by what names their widget.
  uint64_t ui_draws = 0, ui_with_element = 0, ui_screen_only = 0;
  uint64_t ui_named = 0;                             // 2D draws with an element or a label (the "with widget" count)
  uint64_t ui_with_label = 0;                        // 2D draws whose text object's string was read (label)
  uint64_t ui_runs = 0, ui_runs_truncated = 0;      // S101 U1: glyph draws that carried their whole run string / of them cut at kRunUnits
  uint64_t ui_element_no_text = 0;                   // element found, its text id unknown (labels set before recording began, recycled address)
  std::map<uint32_t, uint64_t> ui_no_element_by_fn;  // submit function start -> 2D draws without an element
};
inline NamesCount g_names;

inline uint32_t InternName(std::string& strings, std::unordered_map<const char*, uint32_t>& map, const char* s) {
  if (!s || !*s) return 0;
  if (auto it = map.find(s); it != map.end()) return it->second;
  const uint32_t at = uint32_t(strings.size()) + 1;
  strings.append(s);
  strings.push_back('\0');
  map.emplace(s, at);
  return at;
}

// `run` (may be null): filled for a glyph draw whose whole run string was read (draw is left 0: the caller knows the draw index).
inline DrawOrigin TakeDrawOriginInto(std::string& strings, std::unordered_map<const char*, uint32_t>& map, TextRun* run = nullptr) {
  namespace og = rdr_adapter::origin;
  DrawOrigin o;
  ++g_names.draws;
  og::Key k;
  if (!og::Global().Resolve(og::t_replay_tag, k)) {
    ++g_names.untagged;
    ++g_names.by_class[0];
    return o;  // unnamed: no tag reached this draw (a record we do not hook, or a draw outside the replay)
  }
  if (k.op != og::t_replay_op) {  // a real tag from another opcode's record left in a reused list slot (S101 review 3): never a wrong name
    ++g_names.untagged;
    ++g_names.stale;
    ++g_names.by_class[0];
    return o;
  }
  o.flags = kOriginFromSubmit;
  o.op = uint8_t(k.op);
  o.call_site = k.call_site;
  o.pass_site = k.pass_site;
  o.object = k.object;
  o.part = k.part;
  const og::FnRow* sub = og::FindRow(og::kSubmitters, k.call_site);
  // S101 lane18 (review 5): a 2D scope's pass_site is the 2D function's caller, not a scene pass; never look it up as one.
  const bool ui_submitter = sub && (sub->klass == DrawClass::Sprite || sub->klass == DrawClass::Movie || sub->klass == DrawClass::Text);
  const og::FnRow* pass = (k.pass_site && !ui_submitter) ? og::FindRow(og::kPasses, k.pass_site) : nullptr;
  const og::FnRow* row = pass ? pass : sub;
  if (row) {
    o.klass = row->klass;
    o.technique = InternName(strings, map, row->technique);
  } else {
    ++g_names.unclassified;  // named by its call site; the origin table has no row for that function yet (a hole to lodge)
  }
  // S101 U1: a glyph quad (recorded inside a text draw by a text-object function) is class text, whatever its submitter's row says.
  // Only when the submitter is a text-object function: the gradient draw sub_82BC6A18 runs INSIDE sub_82BC8180 and keeps its outer scope (so its
  // Key carries the text flag too), but its quads are gradients, not glyphs (review D3).
#ifndef S101_PLANT_NO_TEXT_PROMOTION  // planted fault (tools\names\ui_names_selftest.py): the promotion line removed; the main checks must then fail
  if (k.text && o.klass == DrawClass::Sprite && sub && (sub->start == 0x82BC3518u || sub->start == 0x82BC8180u)) o.klass = DrawClass::Text;
#endif
  ++g_names.by_class[uint8_t(o.klass)];
  if (k.run && o.klass == DrawClass::Text && run) {
    thread_local std::unordered_map<uint32_t, const og::TextRuns::Run*> run_cache;   // S101 lane 89 (perf): ids are never reused and the deque never moves a string: no mutex per glyph draw
    const og::TextRuns::Run* cached = nullptr;
    if (const auto it = run_cache.find(k.run); it != run_cache.end()) cached = it->second;
    else if ((cached = og::Runs().Get(k.run)) != nullptr && run_cache.size() < 65536) run_cache.emplace(k.run, cached);
    if (const og::TextRuns::Run* r = cached) {  // the run's string lives in Runs() (a deque): a stable pointer for the pointer-keyed table
      run->string = InternName(strings, map, r->utf8.c_str());
      run->units = r->units;
      run->flags = r->truncated ? uint32_t(kRunTruncated) : 0;
      ++g_names.ui_runs;
      if (r->truncated) ++g_names.ui_runs_truncated;
    }
  }
  // S101 lane18: a 2D draw's widget text, built once per widget by origin::WidgetTable (stable pointer; review 1).
  if (o.klass == DrawClass::Sprite || o.klass == DrawClass::Text || o.klass == DrawClass::Movie) {
    ++g_names.ui_draws;
    og::WidgetInfo w;
    const char* text = k.widget ? og::Widgets().Get(k.widget, w) : nullptr;
    if (text) {
      o.widget = InternName(strings, map, text);
      if (w.label[0]) ++g_names.ui_with_label;
      if (w.element) {
        ++g_names.ui_with_element;
        if (!w.text_id) ++g_names.ui_element_no_text;
      }
      if (w.element || w.label[0]) ++g_names.ui_named;
      else ++g_names.ui_screen_only;
    }
    if (!text || !(w.element || w.label[0])) ++g_names.ui_no_element_by_fn[row ? row->start : k.call_site];
  }
  return o;
}

}  // namespace rdr_adapter::names_draw
