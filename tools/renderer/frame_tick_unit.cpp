// S103 landing 2, piece P6: the offline proof of src\renderer_game\frame_tick.h (the scene record's frame boundary, out of the stream reader).
// 0 game runs, no SDK, no game memory. Built and run by tools\renderer\frame_tick_unit.py.
//   frame_tick_unit            all arms (exit 0 when every check passed)
//   frame_tick_unit plant N    the same with planted fault N (it must exit 1 with a FAIL line):
//     1 old_hook      the pre-P6 present hook: with the reader off nothing is called (no EndFrame, no message)
//     2 double_tick   the hook calls the reader's present AND the stream-free one (two boundaries per present)
//     3 stale_frame   the stream-free present passes guest_frame - 1 (the record would be one frame behind the reader's)
//     4 curve_lost    the stream-free tick takes the display table even with no poster (a table set before the renderer starts is lost)
//
// It builds the SHIPPED frame_tick.h (WWE2K14_FRAME_TICK_CORE_ONLY: everything but the guest-memory reads) against stand-ins of the scene writer
// (rdr_scene::live::Enabled / EndFrame / LatchFrame / EmitLatched) that write an event list. The "reader on" driver makes the frame_tick calls at
// the points reader.h OnPresent / TailLatch / TailFinishAt make them (the .py lints that reader.h makes them there and nowhere calls live:: itself);
// the "reader off" driver is the present hook's other branch, StreamFreePresentCore. The event lists are compared.
#define WWE2K14_FRAME_TICK_CORE_ONLY 1
#include "../../src/renderer_game/frame_tick.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace ft = wwe2k14::frame_tick;
namespace tl = rdr_adapter::tail;

// ---- the event list -------------------------------------------------------------------------------------------------------------------------------
struct E {
  std::string what;
  uint64_t frame;
  uint32_t aux;
  bool operator==(const E& o) const { return what == o.what && frame == o.frame && aux == o.aux; }
};
static std::vector<E> g_ev;
static bool g_enabled = true;

// Stand-ins for the scene writer (live_scene.cpp): what the boundary asked for, in order.
namespace rdr_scene::live {
bool Enabled() { return g_enabled; }
void EndFrame(uint64_t guest_frame, uint32_t capture_frame) { g_ev.push_back({"scene.end", guest_frame, capture_frame}); }
void LatchFrame(uint64_t guest_frame, uint32_t capture_frame, bool allow_defer, Latched& out) {
  g_ev.push_back({"scene.latch", guest_frame, capture_frame});
  out.emit_deferred = allow_defer;
  out.info.guest_frame = guest_frame;
}
void EmitLatched(Latched& l) {
  g_ev.push_back({"scene.emit", l.info.guest_frame, 0});
  l.emit_deferred = false;
}
}  // namespace rdr_scene::live

static int g_fail = 0, g_checks = 0, g_plant = 0;
static void Check(bool ok, const char* name, const std::string& why = "") {
  ++g_checks;
  std::printf("%s  %-44s %s\n", ok ? "PASS" : "FAIL", name, why.c_str());
  if (!ok) ++g_fail;
}

// Posts reach the list as "msg" (the render thread's message), whichever side made them.
static void TraceToList(ft::Ev ev, uint64_t gf, uint32_t interval) {
  if (ev == ft::kEvPost) g_ev.push_back({"msg", gf, interval});
  if (ev == ft::kEvNoPost) g_ev.push_back({"msg.none", gf, interval});
}
static std::vector<ft::Tick> g_ticks;
static void Poster(const ft::Tick& t) { g_ticks.push_back(t); }

// ---- the reader-on driver: reader.h's calls, in reader.h's order ------------------------------------------------------------------------------------
// OnPresent: FlushIfPending(kAtPresent) first; then either TailDecide -> TailLatch (SceneLatch; the message at the finish) or SceneEnd + the
// stream message (NoteStreamPost right after g_live.Submit*). The finish (TailFinishAt): SceneEmit, then the message.
struct ReaderModel {
  uint64_t tail_frame = 0;
  uint32_t tail_interval = 0;
  rdr_scene::live::Latched tail_scene;
};
static ReaderModel g_reader;
static void ReaderFinish(tl::Where where) {   // reader.h TailFinishAt's frame_tick part
  tl::State& st = tl::The();
  std::lock_guard<std::mutex> lock(st.FinishMutex());
  if (!st.Pending()) return;
  st.FinishStarted();
  ft::SceneEmit(g_reader.tail_frame, g_reader.tail_scene);
  ft::NoteStreamPost(g_reader.tail_frame, g_reader.tail_interval);
  st.FinishDone(where);
}
static void ReaderPresent(uint64_t gf, uint32_t interval, bool want_defer) {
  tl::The().FlushIfPending(tl::kAtPresent);
  if (tl::The().Decide(want_defer ? tl::kWhyCount : tl::kOff)) {   // TailDecide
    tl::The().SetFinish(&ReaderFinish);                               // TailLatch
    g_reader.tail_frame = gf, g_reader.tail_interval = interval;
    ft::SceneLatch(gf, g_reader.tail_scene);
    tl::The().SetPending(g_reader.tail_scene.emit_deferred);
    return;
  }
  ft::SceneEnd(gf, rdr_scene::live::kNoCaptureFrame);
  ft::NoteStreamPost(gf, interval);
}

// ---- the present hook (half2_hooks.h sub_82CB37C0) ------------------------------------------------------------------------------------------------
static void Hook(bool reader_on, uint64_t gf, uint32_t interval, bool want_defer) {
  if (g_plant == 1) {   // old_hook: the reader's present only, behind ReaderOn()
    if (reader_on) ReaderPresent(gf, interval, want_defer);
    return;
  }
  if (g_plant == 2) {   // double_tick
    if (reader_on) ReaderPresent(gf, interval, want_defer);
    ft::StreamFreePresentCore(gf, interval);
    return;
  }
  if (reader_on) ReaderPresent(gf, interval, want_defer);
  else ft::StreamFreePresentCore(g_plant == 3 ? gf - 1 : gf, interval);
}
// The game's release of the update thread (tail_release_hook.h): the window closes, the original, then the finish.
static void Release() {
  tl::The().OnRelease();
  ft::FinishTailAtRelease();
}

// One run of N presents; the swap loop: arrive (window opens) -> present -> release.
static std::vector<E> Run(bool reader_on, bool defer, uint64_t first, int n) {
  g_ev.clear();
  for (int i = 0; i < n; ++i) {
    const uint64_t gf = first + uint64_t(i);
    const uint32_t interval = 1 + (i % 3 == 2);   // a 30 fps stretch: the interval must travel with its own frame
    tl::The().OnArrive();
    Hook(reader_on, gf, interval, defer);
    Release();
  }
  return g_ev;
}

// The frame-level sequence: per present, the record's frame and the message (latch + emit of frame F == end of F; the order of records and messages kept).
static std::vector<E> Boundaries(const std::vector<E>& ev) {
  std::vector<E> out;
  for (const E& e : ev) {
    if (e.what == "scene.end" || e.what == "scene.emit") out.push_back({"record", e.frame, 0});
    else if (e.what == "msg") out.push_back(e);
  }
  return out;
}
static std::string Show(const std::vector<E>& v, size_t n = 8) {
  std::string s;
  for (size_t i = 0; i < v.size() && i < n; ++i) s += v[i].what + ":" + std::to_string(v[i].frame) + "/" + std::to_string(v[i].aux) + " ";
  return s + (v.size() > n ? "..." : "");
}

int main(int argc, char** argv) {
  if (argc >= 3 && std::string(argv[1]) == "plant") g_plant = std::atoi(argv[2]);
  ft::g_trace.store(&TraceToList);
  const int kN = 240;

  // 1. Reader on, no defer: the old order exactly (EndFrame then the message, per present, the present's own frame and interval).
  const std::vector<E> on = Run(true, false, 1000, kN);
  {
    std::vector<E> want;
    for (int i = 0; i < kN; ++i) {
      want.push_back({"scene.end", 1000u + i, rdr_scene::live::kNoCaptureFrame});
      want.push_back({"msg", 1000u + i, uint32_t(1 + (i % 3 == 2))});
    }
    Check(on == want, "reader_on: EndFrame then the message per present", Show(on));
  }
  // 2. Reader on, the deferred tail: latch at the present, emit + message at the release, no EndFrame.
  const std::vector<E> on_tail = Run(true, true, 1000, kN);
  {
    std::vector<E> want;
    for (int i = 0; i < kN; ++i) {
      want.push_back({"scene.latch", 1000u + i, rdr_scene::live::kNoCaptureFrame});
      want.push_back({"scene.emit", 1000u + i, 0});
      want.push_back({"msg", 1000u + i, uint32_t(1 + (i % 3 == 2))});
    }
    Check(on_tail == want, "reader_on tail: latch, then emit + message at the release", Show(on_tail));
    Check(tl::The().stats().finished[tl::kAtRelease] >= uint64_t(kN), "every deferred finish ran at the release (FinishTailAtRelease)");
  }
  // 3. Reader off, a poster registered: the same frame-level sequence as the reader's (one record + one message per present, same frames, same intervals).
  ft::g_post.store(&Poster);
  g_ticks.clear();
  const std::vector<E> off = Run(false, false, 1000, kN);
  Check(Boundaries(off) == Boundaries(on), "reader_off == reader_on (records + messages)", Show(Boundaries(off)) + "| on: " + Show(Boundaries(on)));
  Check(Boundaries(off) == Boundaries(on_tail), "reader_off == reader_on tail (records + messages)");
  Check(off == on, "reader_off: the exact event list of reader_on (EndFrame, msg)", Show(off));
  {
    bool frames_ok = g_ticks.size() == size_t(kN);
    for (size_t i = 0; frames_ok && i < g_ticks.size(); ++i) frames_ok = g_ticks[i].guest_frame == 1000 + i && g_ticks[i].present_interval == uint32_t(1 + (i % 3 == 2));
    Check(frames_ok, "reader_off: one Tick per present with record j = its guest frame", std::to_string(g_ticks.size()) + " ticks");
  }
  // 4. Reader off with the writer disabled (no sink): no EndFrame, the message still goes (the render thread is told every frame).
  g_enabled = false;
  const std::vector<E> off_nosink = Run(false, false, 2000, 20);
  int ends = 0, msgs = 0;
  for (const E& e : off_nosink) ends += e.what == "scene.end", msgs += e.what == "msg";
  Check(ends == 0 && msgs == 20, "reader_off, writer disabled: no EndFrame, one message per present", Show(off_nosink));
  g_enabled = true;

  // 5. The display table: the parse equals the reader's old one byte for byte; a change is pending once; no poster -> it waits.
  {
    uint8_t tables[1536];
    for (int i = 0; i < 1536; ++i) tables[i] = uint8_t((i * 37 + 11) ^ (i >> 3));
    std::array<uint8_t, 1024> ref{};   // reader.h OnGammaRamp before P6 (db::Host bytes p[0], p[1])
    for (uint32_t i = 0; i < 256; ++i) {
      for (uint32_t c = 0; c < 3; ++c) {
        const uint8_t* p = tables + 512 * c + 2 * i;
        const uint32_t level = ((uint32_t(p[0]) << 8 | p[1]) >> 6) & 0x3FF;
        ref[4 * i + c] = uint8_t((level * 255 + 511) / 1023);
      }
      ref[4 * i + 3] = 255;
    }
    ft::DisplayCurve dc;
    auto be16 = [&](uint32_t off) { return uint32_t(tables[off]) << 8 | tables[off + 1]; };
    const bool changed = dc.Set(be16);
    Check(changed && dc.Current() == ref, "display table: the same 1024 bytes as the reader's old parse");
    Check(!dc.Set(be16), "display table: the same table again is no change");
    std::array<uint8_t, 1024> got{};
    Check(dc.TakePending(got) && got == ref && !dc.TakePending(got), "display table: pending once");
    dc.RepostIfKnown();
    Check(dc.Pending(), "display table: a renderer start sends the known table again");

    // through the stream-free tick: no poster -> it stays pending; the first posted tick carries it
    ft::TheCurve().Set(be16);
    ft::g_post.store(nullptr);
    if (g_plant == 4) { std::array<uint8_t, 1024> drop; ft::TheCurve().TakePending(drop); }
    Run(false, false, 3000, 3);
    Check(ft::TheCurve().Pending(), "display table: kept pending while no poster takes the tick");
    ft::g_post.store(&Poster);
    g_ticks.clear();
    Run(false, false, 3003, 2);
    Check(g_ticks.size() == 2 && g_ticks[0].display_curve_changed && g_ticks[0].display_curve == ref && !g_ticks[1].display_curve_changed,
          "display table: the first posted tick carries it, the next does not");
  }
  // 6. Counts: unposted ticks are counted, never lost silently.
  {
    ft::g_post.store(nullptr);
    const uint64_t before = ft::TheCounts().ticks_unposted.load();
    const std::vector<E> ev = Run(false, false, 4000, 7);
    int none = 0;
    for (const E& e : ev) none += e.what == "msg.none";
    Check(ft::TheCounts().ticks_unposted.load() - before == 7 && none == 7, "no poster: every tick counted unposted");
  }

  std::printf("%d checks, %d failed%s\n", g_checks, g_fail, g_plant ? (" (plant " + std::to_string(g_plant) + ")").c_str() : "");
  return g_fail ? 1 : 0;
}
