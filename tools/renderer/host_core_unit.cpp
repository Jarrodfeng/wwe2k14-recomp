// S103 landing 2, piece P7 H1: the offline proof of src\renderer_host\host_core.h (the scene host's mailbox, thread loop and frame decisions) and of the present size the frame_tick Tick now carries.
// 0 game runs, no GPU, no SDK. Built and run by tools\renderer\host_core_unit.py.
//   host_core_unit            all arms (exit 0 when every check passed)
//   host_core_unit plant N    the same with planted fault N (it must exit 1 with a FAIL line); the plants are in host_core.h (WWE2K14_HOST_PLANTS):
//     1 draw_fault_as_miss   a draw: fault is treated as a plain miss: the repeat reuses a half-drawn `out` (the live.cpp 1884 rule, a past bug)
//     2 no_record_black      a frame with no record presents black instead of F
//     3 poster_draws         Post() runs the handler on the posting (game) thread
//     4 oldest_wins          the mailbox keeps the OLDEST message
//     5 curve_lost           the display curve of a dropped message is lost
//     6 hold_without_last3d  a hold with no valid last 3D picture copies `out` anyway
//     7 take_after_stop      Take hands out a waiting message after Stop (a frame on a closing renderer)
//     8 idle_never           the idle poll never runs (the quit flush's flag is never seen)
//     9 repeat_ignores_holds the R9 movie repeat shows a pruned set's destroyed images (PlaneImages::Holds ignored)
//    10 no_row_cache         the movie row cache is not used (one serve per slot)
//    24 repeat_raw_key       N2: the R9 repeat compares the raw plane key, not the set it names (the other display buffer's key breaks it);  25 key_wiring_dropped  the row's plane key never reaches the store (PlaneKeyOf)
//    16 menu_hook_is_3d    S103 FLAT: the old stamp rule (scene_stamp_rule.h plant 1): a menu backdrop particle call asks the scene decision, so the menu frame is stamped 3D, is never flat and holds the last arena picture
//    18 rate_draws_all       the 30 fps mode draws every tick;  19 rate_debt_unpaid  the vblank debt is never paid;  20 rate_carry_lost  a dropped tick's vblanks are lost (RateArm)
//    21 rate_debt_unbounded  22 rate_contentless_pays  23 feed_counts_calls (RateArm / FeedArm)
//    27 stage_word_ignored  S110 ENTRBLACK: the scene-fact reader ignores the stage's show word (cam_gate.h plant; SceneFactArm);  28 chars_ignored: the reader ignores the characters (SceneFactArm)
//    26 hidden_drawn        S110 ENTRBLACK: Decide ignores Facts::game_draws_no_scene (SceneFactArm);  37 no_scene_sticky: the black outlives the fact by one frame, a hold (SceneFactArm)
//    (29, 30, 33, 34 retired with the S105 A3 camera gate)
//    39-48 S110 ENTRWAIT2 (StartWaitArm, scene_start_wait.h plants 1-10): 39 start_no_wait, 40 start_old_build, 41 start_stall_holds, 42 start_script_waits, 43 start_match_in_entrance,
//          44 start_arm_kept, 45 start_story_class_only, 46 start_card_down_holds, 47 start_stale_host_shut, 48 start_card_pending_only (review: the landed G+8-only card test)
//    49-51 S110 ENTRBODIES (StartWaitArm, scene_start_wait.h plants 11-13): 49 start_bodies_no_restamp, 50 start_bodies_card_down, 51 start_bodies_kept
//    15 draw_unfed          S103 HN: SceneFeed holds nothing: a required input never stamped in this scene is drawn anyway (the old ONF rule, run 003's half-built first entrance frames)
//    52 cause_dropped       S111 BLACKFRAME: Decide drops the hold's cause when it falls to black (BlackFrameArm: the run 328 replay prints one run, no reason)
#define WWE2K14_FRAME_TICK_CORE_ONLY 1
#define WWE2K14_HOST_PLANTS 1
#define WWE2K14_STAMP_PLANTS 1
#include "../../src/renderer_host/host_core.h"
#include "../../src/renderer_host/black_spans.h"   // S111 BLACKFRAME
#include "../../src/renderer_scene/rows_quad2d.h"
#include "../../src/renderer_host/host_planes.h"
#include "../../src/renderer_scene/cam_gate.h"   // S105 A3
#include "../../src/renderer_scene/game_load_follow.h"   // S110 ENTRBLACK round 5
#include "../../src/renderer_scene/scene_start_wait.h"   // S110 ENTRWAIT2
#include "../../src/renderer_scene/start_bodies.h"   // S110 ENTRBODIES
#include "../../src/renderer_game/scene_kind.h"
#include "../../src/renderer_game/scene_stamp_rule.h"
#include "../../src/renderer_game/frame_tick.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace sh = wwe2k14::scene_host;
namespace ft = wwe2k14::frame_tick;

// Stand-ins for the scene writer (frame_tick.h's inline functions name them).
namespace rdr_scene::live {
bool Enabled() { return true; }
void EndFrame(uint64_t, uint32_t) {}
void LatchFrame(uint64_t, uint32_t, bool, Latched&) {}
void EmitLatched(Latched&) {}
}  // namespace rdr_scene::live

static int g_fail = 0, g_checks = 0;
static void Check(bool ok, const char* name, const std::string& why = "") {
  ++g_checks;
  std::printf("%s  %-52s %s\n", ok ? "PASS" : "FAIL", name, why.c_str());
  if (!ok) ++g_fail;
}

using Msg = sh::HostMsg<int>;
static Msg Make(uint64_t j, bool curve = false, uint8_t curve_fill = 0) {
  Msg m;
  m.guest_frame = j;
  m.width = 1280, m.height = 720;
  m.record = std::make_shared<const int>(int(j));
  if (curve) m.curve_changed = true, m.curve.fill(curve_fill);
  return m;
}

// ---- mailbox: newest wins, drops counted, the curve of a dropped message reaches the drawn one -----------------------------------------------------
static void MailboxArm() {
  sh::Counters c;
  sh::Mailbox<int> box(c);
  box.Post(Make(1, true, 77));
  box.Post(Make(2));
  box.Post(Make(3));
  Msg m;
  const bool got = box.Take(m);
  Check(got && m.guest_frame == 3, "mailbox: 3 queued ticks -> the newest is drawn", "got frame " + std::to_string((unsigned long long)m.guest_frame));
  Check(c.ticks_dropped.load() == 2 && c.ticks_in.load() == 3, "mailbox: the 2 older ticks are counted dropped", "dropped " + std::to_string((unsigned long long)c.ticks_dropped.load()));
  Check(m.curve_changed && m.curve[0] == 77, "mailbox: the curve of a dropped tick reaches the drawn one");
  box.Post(Make(4, true, 11));
  box.Post(Make(5, true, 22));
  box.Take(m);
  Check(m.guest_frame == 5 && m.curve[0] == 22, "mailbox: a newer curve wins over a dropped older one");
  box.Stop();
  Check(!box.Take(m), "mailbox: stopped and empty = Take returns false");
  // review H1 B4: a Stop wins over a message that is waiting (no frame is started on a renderer being shut down)
  sh::Counters c2;
  sh::Mailbox<int> box2(c2);
  box2.Post(Make(9));
  box2.Stop();
  Check(!box2.Take(m), "mailbox: Stop with a message waiting: Take returns false (stop.wins)");
}

// ---- the decoupled 30 (VblankDebt: one picture per two vblanks; the stream path's vblank_debt, live.cpp 916-931) ----------------------------------------------------
static void RateArm() {
  sh::VblankDebt d;
  std::string pattern;   // S = skipped, D = drawn
  for (int i = 0; i < 8; ++i) pattern += d.Skip(2, 1, false) ? 'S' : 'D';
  Check(pattern == "SDSDSDSD", "rate: 30 fps (draw_vblanks 2) with one vblank per tick draws every second tick", pattern);
  sh::VblankDebt off;
  bool any = false;
  for (int i = 0; i < 8; ++i) any |= off.Skip(0, 1, false) | off.Skip(1, 1, false);
  Check(!any, "rate: draw_vblanks 0 / 1 (the mode is off) never skips");
  sh::VblankDebt d2;
  Check(d2.Skip(2, 1, false) && !d2.Skip(2, 1, true) && !d2.Skip(2, 1, false), "rate: a tick that must draw (a screenshot) neither adds to the debt nor clears it");
  sh::VblankDebt d3;
  Check(!d3.Skip(2, 2, false) && !d3.Skip(2, 2, false), "rate: a tick of two vblanks (a late present) pays the whole debt: it always draws");
  // the mailbox carries a dropped tick's vblanks to the newer one, so the pacing sees every vblank
  sh::Counters c;
  sh::Mailbox<int> box(c);
  box.Post(Make(1)), box.Post(Make(2)), box.Post(Make(3));
  Msg m;
  box.Take(m);
  Check(m.guest_frame == 3 && m.carried_vblanks == 2, "rate: two dropped ticks carry their 2 vblanks to the drawn one", "carried " + std::to_string(m.carried_vblanks));
  // review fix 1: a tick with no content (no record / no picture) adds its vblanks but never pays the debt: the next tick with content draws
  sh::VblankDebt d5;
  const bool a = d5.Skip(2, 1, false, true);    // debt 1: skipped
  const bool b = d5.Skip(2, 1, false, false);   // debt 2, no content: handled but the debt stays
  const bool c2 = d5.Skip(2, 1, false, true);   // debt 3, content: draws and pays
  const bool d6 = d5.Skip(2, 1, false, true);   // debt 1: skipped again
  Check(a && !b && !c2 && d6, "rate: a content-less tick never clears the debt (the next real tick draws), then the rhythm goes on");
  // review fix 3: one tick's vblanks are clamped (a stall of 100 vblanks banks no long run of draws)
  sh::VblankDebt d7;
  d7.Skip(2, 100, false);
  Check(d7.debt == 0 && d7.Skip(2, 1, false), "rate: a stall's vblanks are clamped per tick (the debt is paid, the next tick skips)");
  sh::VblankDebt d9;
  for (int i = 0; i < 10; ++i) d9.Skip(2, 8, false, false);
  Check(d9.debt <= 2, "rate: the debt of content-less ticks is bounded by draw_vblanks (a stall banks no long run of draws)", "debt " + std::to_string(d9.debt));
  sh::VblankDebt d8;
  d8.Skip(4, 100, false);
  Check(d8.debt == 0, "rate: the clamp is per tick: 100 vblanks count as kMaxTickVblanks");
  sh::VblankDebt d4;
  Check(!d4.Skip(2, m.carried_vblanks + 1, false), "rate: a tick that carries the vblanks of dropped ones draws (the stream's saturated forced_skip)");
}

// ---- the idle poll (the quit flush): the host thread looks at the request flag while no frame comes ----------------------------------------------------
static void IdleArm() {
  sh::HostCore<int> core;
  std::atomic<int> idles{0};
  core.Start([](Msg&) {}, {}, [&] { idles.fetch_add(1); });
  std::this_thread::sleep_for(std::chrono::milliseconds(450));
  Check(idles.load() >= 2, "host: the idle poll runs on the host thread while no tick comes (the quit flush)", "idle calls " + std::to_string(idles.load()));
  core.Stop();
}

// ---- the movie planes of a row (host_planes.h): the row cache, R9, Holds, the served set -------------------------------------------------------------
struct FakeSink {
  uint32_t* next;
  std::vector<uint32_t>* destroyed;
  uint32_t Create(uint32_t, uint32_t) const { return (*next)++; }
  void Update(uint32_t, const uint8_t*, size_t) const {}
  void Destroy(uint32_t id) const { destroyed->push_back(id); }
};
static rdr_scene::live::screens::Frame MakeFrame(uint32_t set, uint64_t id) {
  rdr_scene::live::screens::Frame f;
  f.set = set, f.w = 8, f.h = 4, f.cw = 4, f.ch = 2, f.id = id, f.has_alpha = true;
  f.y.assign(32, 1), f.cb.assign(8, 2), f.cr.assign(8, 3), f.a.assign(32, 4);
  return f;
}
static void PlanesArm() {
  namespace mp = rdr_scene::live::movieplanes;
  uint32_t next = 100;
  std::vector<uint32_t> destroyed;
  mp::PlaneImages<FakeSink> images(FakeSink{&next, &destroyed});
  uint64_t repeats = 0;
  int serves = 0;
  uint32_t frame_set = 5;   // the set the store answers with
  bool have_frame = true;
  sh::HostPlanes<FakeSink> planes(images, [&](const mp::DrawHint&, uint64_t, mp::GuestSlot gs[4], uint32_t& served_set) {
    ++serves;
    if (!have_frame) return mp::Serve::NoFrame;
    mp::PlaneImages<FakeSink>::Ids ids;
    if (!images.Get(frame_set, MakeFrame(frame_set, uint64_t(serves)), ids)) return mp::Serve::Refused;
    for (int s = 0; s < 4; ++s) gs[s].image = ids.id[s];
    served_set = frame_set;
    return mp::Serve::Ok;
  }, repeats);
  mp::GuestSlot out;
  bool ok = true;
  for (int s = 0; s < 4; ++s) ok = planes.Slot(10, 0, 0, false, 77, s, out) && ok;
  Check(ok && serves == 1, "planes: the four slots of one row ask the store once (the row cache)", "serves " + std::to_string(serves));
  const uint32_t y0 = out.image;
  planes.Slot(10, 0, 0, false, 78, 0, out);
  Check(serves == 2, "planes: the same row on another record frame asks again");
  // R9: the store has no frame for this draw: the last frame again, counted, while its images are ours. The row has no set hint (plane_set 0): Holds must ask the SERVED frame's set (5), never the hint (0)
  have_frame = false;
  const bool rep = planes.Slot(11, 0, 0, false, 79, 0, out);
  Check(rep && repeats == 1 && out.image != 0, "planes R9: no frame for the draw -> the last set again, counted movie_repeat (a row with no set hint too)", "repeats " + std::to_string((unsigned long long)repeats));
  (void)y0;
  // another movie (a different hint) never shows this one's frame
  const bool other = planes.Slot(12, 9, 3, true, 80, 0, out);
  Check(!other && repeats == 1, "planes R9: a row of another movie (other set hint) is not given this movie's last frame");
  // the set was pruned: its images are destroyed; the repeat must be refused, never shown
  have_frame = true;
  planes.Slot(13, 0, 0, false, 81, 0, out);   // a fresh serve: last = set 5 again
  have_frame = false;
  for (uint32_t set = 20; set < 20 + 9; ++set) {   // more sets than kMaxSets: the least recently used (set 5) goes, its images are destroyed
    mp::PlaneImages<FakeSink>::Ids ids;
    images.Get(set, MakeFrame(set, 1000 + set), ids);
  }
  const uint64_t before = repeats;
  const bool pruned = planes.Slot(14, 0, 0, false, 82, 0, out);
  Check(!pruned && repeats == before, "planes R9: a pruned set's images are never shown again (Holds)", "destroyed " + std::to_string(destroyed.size()));
  {   // N2 F2: a movie's two display buffers carry two keys; the repeat is by the SET a key names, so the second serve failing (key of the other buffer) still repeats
    uint32_t nx = 500;
    std::vector<uint32_t> dd;
    mp::PlaneImages<FakeSink> im2(FakeSink{&nx, &dd});
    uint64_t rep2 = 0, n2 = 0;
    bool have2 = true;
    sh::HostPlanes<FakeSink> p2(im2, [&](const mp::DrawHint&, uint64_t, mp::GuestSlot gs[4], uint32_t& served_set) {
      ++n2;
      if (!have2) return mp::Serve::NoFrame;
      mp::PlaneImages<FakeSink>::Ids ids;
      if (!im2.Get(7, MakeFrame(7, n2), ids)) return mp::Serve::Refused;
      for (int s = 0; s < 4; ++s) gs[s].image = ids.id[s];
      served_set = 7;
      return mp::Serve::Ok;
    }, rep2, [](uint32_t k) { return (k == 0x71 || k == 0x72) ? 7u : 0u; });
    mp::GuestSlot o2;
    p2.Slot(30, 9, 1, true, 90, 0, o2, 0x71);   // served from set 7 with buffer 0's key
    have2 = false;
    const bool again = p2.Slot(31, 9, 1, true, 91, 0, o2, 0x72);   // the next frame: buffer 1's key, no frame
    Check(again && rep2 == 1, "planes R9 (N2): the repeat follows the SET the key names, so the other display buffer's key still repeats", "repeats " + std::to_string((unsigned long long)rep2));
    const bool other_movie = p2.Slot(32, 9, 1, true, 92, 0, o2, 0x99);   // a key of no set: another movie
    Check(!other_movie && rep2 == 1, "planes R9 (N2): a key that names another set does not repeat this movie's frame");
  }
  {   // N2: the row's key reaches the store through the wiring SceneHost::PlaneOf uses (PlaneKeyOf -> Slot -> the serve's DrawHint)
    uint32_t nx = 700;
    std::vector<uint32_t> dd;
    mp::PlaneImages<FakeSink> im3(FakeSink{&nx, &dd});
    uint32_t seen_key = 0xFFFFFFFFu;
    uint64_t rep3 = 0;
    sh::HostPlanes<FakeSink> p3(im3, [&](const mp::DrawHint& h, uint64_t, mp::GuestSlot gs[4], uint32_t& served_set) {
      seen_key = h.key;
      mp::PlaneImages<FakeSink>::Ids ids;
      if (!im3.Get(3, MakeFrame(3, 1), ids)) return mp::Serve::Refused;
      for (int s = 0; s < 4; ++s) gs[s].image = ids.id[s];
      served_set = 3;
      return mp::Serve::Ok;
    }, rep3, [](uint32_t) { return 0u; });
    rdr_scene::Quad2D row{};
    row.have = rdr_scene::kQhPlanes | rdr_scene::kQhFlush;
    row.tex_key[0] = 0xABCD;
    mp::GuestSlot o3;
    p3.Slot(40, 5, 1, true, 95, 0, o3, sh::PlaneKeyOf(row));
    Check(seen_key == 0xABCD, "planes wiring (N2): a movie row's tex_key[0] reaches the store's draw hint", "key " + std::to_string(seen_key));
    row.have = rdr_scene::kQhFlush;   // no plane keys on this row
    p3.Slot(41, 5, 1, true, 96, 0, o3, sh::PlaneKeyOf(row));
    Check(seen_key == 0, "planes wiring (N2): a row with no plane keys hands key 0 (the old rule)");
  }
}

// ---- the host thread: the poster never draws -------------------------------------------------------------------------------------------------------
static void ThreadArm() {
  sh::HostCore<int> core;
  std::thread::id handler_thread;
  std::atomic<int> calls{0};
  core.Start([&](Msg&) { handler_thread = std::this_thread::get_id(), calls.fetch_add(1); });
  core.Post(Make(1));
  const bool ok = core.WaitHandled(1, std::chrono::milliseconds(3000));
  Check(ok && calls.load() == 1, "host: one tick posted -> one frame handled");
  Check(handler_thread != std::this_thread::get_id(), "host: the handler runs on the host thread, not the poster's", ok ? "" : "timeout");
  Check(handler_thread == core.host_thread(), "host: the handler's thread is the host thread");
  // many ticks: handled + dropped == in (every tick is accounted for)
  for (int i = 2; i < 50; ++i) core.Post(Make(uint64_t(i)));
  core.Post(Make(50));
  for (int spin = 0; spin < 300 && core.counters().handled.load() + core.counters().ticks_dropped.load() < core.counters().ticks_in.load(); ++spin) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  const uint64_t in = core.counters().ticks_in.load(), done = core.counters().handled.load(), dropped = core.counters().ticks_dropped.load();
  Check(in == 50 && done + dropped == in && done >= 1, "host: every tick is handled or counted dropped",
        "in " + std::to_string((unsigned long long)in) + " handled " + std::to_string((unsigned long long)done) + " dropped " + std::to_string((unsigned long long)dropped));
  core.Stop();
  Check(!core.Running(), "host: Stop joins the thread");
}

// ---- the decisions ---------------------------------------------------------------------------------------------------------------------------------
static sh::Facts F(bool ours, sh::DrawResult d, bool loading = false, bool flat = false) {
  sh::Facts f;
  f.have_record = true, f.size_known = true, f.width = 1280, f.height = 720;
  f.ours = ours, f.draw = d, f.loading = loading, f.flat = flat;
  return f;
}

// S110 ENTRBLACK (entry: rdr_scene::ReadSceneFact / SceneFactBits / GameDrawsNoScene, sh::Decide): an OURS frame is black exactly when the game submits no scene geometry for it. Production order: the
// latch's read (cam_gate.h over a fake guest) -> the writer's header bits (SceneFactBits, as Emit stamps them) -> the host's fact (GameDrawsNoScene of the header flags, as SceneHost::Handle sets it on an
// OURS frame; Handle skips the 3D draw then: draw = Miss) -> Decide. Planted 26 (Decide ignores the fact), 27 (the reader ignores the stage word), 28 (the reader ignores the characters), 37 (a one-frame
// hold) must each fail it.
namespace {
struct FakeGuest {   // a tiny guest memory for the reader
  std::vector<std::pair<uint32_t, uint32_t>> w;
  void Put(uint32_t a, uint32_t v) { for (auto& p : w) if (p.first == a) { p.second = v; return; } w.emplace_back(a, v); }
  bool Read(uint32_t a, uint32_t& v) const { for (const auto& p : w) if (p.first == a) { v = p.second; return true; } return false; }
};
constexpr uint32_t kStage = 0x40004000u, kChar0 = 0x40008000u;
// the guest of one frame: the stage's show word, and character 0's three predicate words (all 1 = shown, all 0 = hidden); slots 1..8 empty
static FakeGuest Guest(uint32_t stage_on, uint32_t char_on) {
  FakeGuest g;
  g.Put(0x83763544u, kStage), g.Put(kStage + 1176, stage_on);
  g.Put(0x836F38D0u, kChar0), g.Put(kChar0 + 1208, char_on), g.Put(kChar0 + 224, char_on), g.Put(kChar0 + 232, char_on);
  for (uint32_t i = 1; i < 9; ++i) g.Put(0x836F38D0u + 4 * i, 0);
  return g;
}
// what the latch stamps for this guest, then what Handle sets on the Facts of an OURS frame (draw = Miss when the game draws nothing), then Decide
static sh::Plan Frame(const FakeGuest& g, sh::State& s, sh::Counters& c) {
  bool read = false, drawn = false;
  rdr_scene::ReadSceneFact([&g](uint32_t a, uint32_t& v) { return g.Read(a, v); }, read, drawn);
  const uint32_t flags = rdr_scene::SceneFactBits(read, drawn);   // the record's header flags
  sh::Facts f = F(true, sh::DrawResult::Ok);
  f.game_draws_no_scene = rdr_scene::GameDrawsNoScene(flags);
  if (f.game_draws_no_scene) f.draw = sh::DrawResult::Miss;   // Handle: no DrawLive on such a frame
  return sh::Decide(s, f, c);
}
}  // namespace
// S110 ENTRBLACK round 5 (entry: rdr_scene::loadfollow::ArenaOfGamePath / NoteGameOpen / TakeGameArena): the arena pac the game opens is handed to the host once.
// Planted 38 (the parser accepts any .pac) must fail it.
static void LoadFollowArm() {
  namespace lf = rdr_scene::loadfollow;
  lf::g_plant = sh::g_plant == 38 ? 1 : 0;
  Check(lf::ArenaOfGamePath("D:\\PAC\\BG\\BG04.PAC") == 4 && lf::ArenaOfGamePath("game:/pac/bg/bg57.pac") == 57, "ENTRBLACK r5: the arena pac the game opens names its arena");
  Check(lf::ArenaOfGamePath("D:\\PAC\\BG\\BGETC.PAC") == -1 && lf::ArenaOfGamePath("D:\\PAC\\CH\\CH153.PAC") == -1 && lf::ArenaOfGamePath("bg04.pac") == -1, "ENTRBLACK r5: other pacs name no arena");
  lf::TakeGameArena();
  lf::NoteGameOpen("D:\\PAC\\CH\\CH139.PAC");
  Check(lf::TakeGameArena() == -1, "ENTRBLACK r5: a character pac hands no arena");
  lf::NoteGameOpen("D:\\PAC\\BG\\BG04.PAC");
  lf::NoteGameOpen("D:\\PAC\\BG\\BG12.PAC");
  Check(lf::TakeGameArena() == 12 && lf::TakeGameArena() == -1, "ENTRBLACK r5: the newest open is handed once, nothing kept after the take");
  lf::g_plant = 0;
}
// S110 ENTRWAIT2 (entry: rdr_scene::startwait::Poll; WaitOn / PartStartPoint / MatchStartPoint / LoadFirstStep / CardUp / HostIdle / MayStart / Arm / NoteHostBuild / NoteHostNoBuild, applied
// by src\scene_start_wait_hooks.h at sub_8232E1E8, sub_82331BC0, sub_8273D0C0 and sub_82331DD8): an armed start behind the loading card waits until a host frame from after its first poll
// found every load done; both points of one start pass together; a start with no card up, and every later start, never waits. Planted 39-48 must each fail.
static void StartWaitArm() {
  namespace sw = rdr_scene::startwait;
  sw::g_plant = sh::g_plant >= 39 && sh::g_plant <= 51 ? sh::g_plant - 38 : 0;
  Check(sw::WaitOn(-1, false, false) && !sw::WaitOn(-1, true, false) && sw::WaitOn(1, true, true) && !sw::WaitOn(0, false, false), "ENTRWAIT2: normal play waits; det / replay does not unless =1; =0 never");
  Check(!sw::WaitOn(-1, false, true) && sw::WaitOn(1, false, true), "ENTRWAIT2: a scripted run does not wait unless =1 (exact guest frames)");
  Check(sw::PartStartPoint(0x8200A4BCu, 0) && sw::PartStartPoint(0x8200A3C0u, 0) && sw::PartStartPoint(0x8200A410u, 0) && !sw::PartStartPoint(0x8200A4BCu, 1),
        "ENTRWAIT2: every part class's first update is a start point (an exhibition part is the base class 0x8200A4BC, Season 0x8200A3C0)");
  Check(sw::MatchStartPoint(0, 3, 1) && !sw::MatchStartPoint(1, 3, 1) && !sw::MatchStartPoint(0, 2, 1), "ENTRWAIT2: step 3 at delay 0 is the no-entrance start");
  Check(!sw::MatchStartPoint(0, 3, 4), "ENTRWAIT2: step 3 with the entrance word is no start (the FADE hold)");
  Check(sw::LoadFirstStep(0, 0) && !sw::LoadFirstStep(1, 0) && !sw::LoadFirstStep(0, 3), "ENTRWAIT2: the match load's first step (each load, a rematch too) arms");
  // review: the requester G takes a posted request (G+8 = 1) on its next update and clears +8 while the screen is SHOWN (sub_827E67C0 +88..+94: build, +8 = 0, +4 = 1); G+84 holds the
  // screen until the close finishes (+240..+2A8). So a shown card is +4 != 0 / +84 != 0 with +8 == 0 (the CASREADY rule, cas_goto_core.h:206-213).
  Check(sw::CardUp(0x40001000u, 0, 1, 0) && sw::CardUp(0x40001000u, 1, 0, 0x40002000u) && sw::CardUp(0x40001000u, 2, 0, 0x40002000u) && !sw::CardUp(0x40001000u, 0, 0, 0) &&
            !sw::CardUp(0, 1, 1, 0x40002000u),
        "ENTRWAIT2: the card is up while it is posted, shown or closing (G+8, G+4 or G+84 non-zero), down when the requester is idle or absent");
  Check(sw::HostIdle(false, false, 0, false, false) && !sw::HostIdle(true, false, 0, false, false) && !sw::HostIdle(false, true, 0, false, false) && !sw::HostIdle(false, false, 1, false, false) &&
            !sw::HostIdle(false, false, 0, true, false) && !sw::HostIdle(false, false, 0, false, true),
        "ENTRWAIT2: idle = nothing asked, the game's arena handed over, nothing running / undelivered / queued");
  sw::Facts f;
  f.host_on = true, f.asked_at = 101, f.host_frame = 120, f.host_idle = false;
  Check(!sw::MayStart(f), "ENTRWAIT2: a load still queued / running at the newest host frame: wait");
  f.host_frame = 90, f.host_idle = true;
  Check(!sw::MayStart(f), "ENTRWAIT2: an idle frame from before the ask does not count");
  f.host_frame = 102;
  Check(sw::MayStart(f), "ENTRWAIT2: the first idle frame after the ask starts the scene");
  sw::Facts s = f;
  s.host_idle = false, s.host_on = false;
  const bool no_drawer = sw::MayStart(s);
  s.host_on = true, s.host_frame = 0;
  const bool never_built = sw::MayStart(s);
  s.host_frame = 120, s.device_lost = true;
  const bool lost = sw::MayStart(s);
  Check(no_drawer && never_built && lost, "ENTRWAIT2: a stalled host opens the wait by state (no drawer / nothing handled / device lost)");
  {   // an exhibition entrance: armed by the match load; each frame the part (task group 1) then the card item (group 33) poll with the card up; the host frames: idle but old, busy, busy, idle
    sw::g_host_frame.store(0), sw::g_host_idle.store(false);
    sw::Arm();
    int part_holds = 0, card_holds = 0;
    bool part_started = false, card_dropped = false;
    const struct { uint64_t f; bool idle; } builds[] = {{99, true}, {101, false}, {130, false}, {131, true}};
    uint64_t now = 100;
    for (const auto& b : builds) {
      sw::NoteHostBuild(b.f, b.idle);
      const bool part_hold = sw::PartStartPoint(0x8200A4BCu, 0) && sw::Poll(now, true, false, true);
      const bool card_hold = sw::Poll(now, true, false, true);
      part_holds += part_hold ? 1 : 0, card_holds += card_hold ? 1 : 0;
      if (!part_hold) {
        part_started = true, card_dropped = !card_hold;
        break;
      }
      now += 30;
    }
    Check(part_started && card_dropped && part_holds == 3 && card_holds == 3, "ENTRWAIT2: an exhibition's part and card wait together for the first idle frame after the ask and pass in the same frame",
          "part holds " + std::to_string(part_holds) + ", card holds " + std::to_string(card_holds));
    Check(!sw::g_armed.load() && sw::g_asked_at.load() == 0, "ENTRWAIT2: the pass spends the arm and forgets the ask");
    sw::NoteHostBuild(now + 5, false);
    Check(!sw::Poll(now + 10, true, false, true), "ENTRWAIT2: a later part (the next wrestler, a mid-match entrant) is never held, even with our host busy and a card up");
  }
  {   // a start reached with the card down (a mid-match entrant, a start whose card already went): no wait, and the start is spent
    sw::Arm();
    sw::NoteHostBuild(500, false);
    const bool held = sw::Poll(600, true, false, false);
    Check(!held && !sw::g_armed.load(), "ENTRWAIT2: no loading card up: no wait, the arm is spent");
    sw::g_armed.store(false);
  }
  {   // the stale host: its frames build nothing (stale OURS inputs, no record); once its loader is empty such a frame opens the wait by state
    sw::g_host_frame.store(0), sw::g_host_idle.store(false);
    sw::Arm();
    sw::NoteHostBuild(700, false);   // the last build asked the arena
    const bool held = sw::Poll(710, true, false, true);
    sw::NoteHostNoBuild(720, sw::HostIdle(false, false, 0, false, false));   // the loader delivered it; the host's frames now build nothing
    const bool opened = !sw::Poll(730, true, false, true);
    Check(held && opened, "ENTRWAIT2: a host whose frames build nothing opens the wait by state (its loader empty)");
    sw::g_armed.store(false);
  }
  {   // an abandoned load: a stale ask from a start that never passed is forgotten by the next arm
    sw::g_asked_at.store(55);
    sw::Arm();
    Check(sw::g_asked_at.load() == 0 && sw::g_armed.load(), "ENTRWAIT2: a new arm forgets a stale ask");
    sw::g_armed.store(false);
  }
  {   // S110 ENTRBODIES: the bodies of the game's characters, asked while the start is held (src\scene_start_bodies.h -> start_bodies.h -> LiveSceneDraw::AskStartBodies): a body asked after the
      // start's first poll holds it until a host frame from after that ask finds the loader settled; a failed load settles too (ModelCache::MissFailed: never queued again)
    namespace sb = rdr_scene::startbodies;
    sw::g_host_frame.store(0), sw::g_host_idle.store(false);
    sw::Arm();
    sb::TheBoard().Reset();
    sw::NoteHostBuild(99, true);
    const bool held0 = sw::Poll(100, true, false, true);   // the first poll stamps the ask (101)
    const bool grew = sb::TheBoard().Add(std::vector<sb::Body>{sb::Body{"pac\\ch\\ch187.pac", "model_key:18700", 0}});
    if (grew && sw::AskBodiesNow(sw::g_armed.load(), true)) sw::NoteAsked(105);
    sw::NoteHostBuild(104, true);   // an idle host frame from before the body was asked
    const bool held1 = sw::Poll(106, true, false, true);
    sw::NoteHostBuild(110, false);   // the body is loading
    const bool held2 = sw::Poll(111, true, false, true);
    sw::NoteHostBuild(140, sw::HostIdle(false, false, 0, false, false));   // adopted, or failed: nothing queued, running or undelivered
    const bool opened = !sw::Poll(141, true, false, true);
    Check(held0 && grew && held1 && held2 && opened, "ENTRBODIES: a body of the game's characters asked during the hold keeps the start until a host frame after its ask finds it settled (loaded or failed)",
          std::string("held ") + (held0 ? "1" : "0") + (held1 ? "1" : "0") + (held2 ? "1" : "0") + " opened " + (opened ? "1" : "0"));
    Check(sw::g_bodies_at.load() == 0 && sw::g_asked_at.load() == 0, "ENTRBODIES: the pass forgets the body ask");
    sw::g_armed.store(false);
    Check(!sw::StartPending(), "ENTRBODIES: no start pending after the pass (the crowd's classes are asked only while one is)");
  }
  Check(!sw::AskBodiesNow(true, false) && !sw::AskBodiesNow(false, true) && sw::AskBodiesNow(true, true),
        "ENTRBODIES: the bodies are asked only while a start is armed AND the loading card is up (a mid-match entrant asks nothing and is never held)");
  {   // the board: one set per start, grown once per new body, taken once per change; a new start asks again (a body an arena change retired is asked again)
    namespace sb = rdr_scene::startbodies;
    sb::Board& b = sb::TheBoard();
    b.Reset();
    const sb::Body a{"pac\\ch\\ch150.pac", "model_key:15006", 0}, c{"pac\\ch\\ch153.pac", "model_key:15306", 0};
    uint64_t seen = 0;
    std::vector<sb::Body> got;
    b.TakeIfNew(seen, got);
    const bool g1 = b.Add(std::vector<sb::Body>{a}), g2 = b.Add(std::vector<sb::Body>{a}), g3 = b.Add(std::vector<sb::Body>{a, c});
    const bool t1 = b.TakeIfNew(seen, got) && got.size() == 2 && b.Size() == 2, t2 = !b.TakeIfNew(seen, got);
    b.Reset();
    const bool g4 = b.Add(std::vector<sb::Body>{a});
    Check(g1 && !g2 && g3 && t1 && t2 && g4, "ENTRBODIES: a start's body set grows once per new body, is taken once per change, and a new start asks its bodies again");
    b.Reset();
  }
  sw::g_host_frame.store(0), sw::g_host_idle.store(false), sw::g_asked_at.store(0), sw::g_bodies_at.store(0);
  sw::g_plant = 0;
}
static void SceneFactArm() {
  rdr_scene::g_scene_fact_plant = sh::g_plant == 27 ? 1 : sh::g_plant == 28 ? 2 : 0;
  {   // an entrance start: the stage and the entrants hidden while the game loads (no geometry), then shown by the entrance's start: black until that frame, drawn FROM that frame
    sh::State s; sh::Counters c;
    bool black = true;
    for (int i = 0; i < 40; ++i) black = black && Frame(Guest(0, 0), s, c).source == sh::Source::Black;
    Check(black && c.game_no_scene_frames == 40 && c.ours == 0 && c.strikes == 0, "ENTRBLACK entrance: black (no strike) while the game draws no geometry", std::to_string(c.game_no_scene_frames));
    const sh::Plan p = Frame(Guest(1, 1), s, c);
    Check(p.source == sh::Source::Draw3D && c.ours == 1, "ENTRBLACK entrance: the first frame the game shows its stage is drawn (its 8-frame FADE then runs over a drawn scene)");
    bool drawn = true;
    for (int i = 0; i < 8; ++i) drawn = drawn && Frame(Guest(1, 1), s, c).source == sh::Source::Draw3D;
    Check(drawn && c.ours == 9, "ENTRBLACK entrance: every frame of the fade is drawn");
  }
  {   // a match with no entrance: the stage is shown from the scene's first frame: drawn from the first frame (the game's 30-frame fade from black runs over it)
    sh::State s; sh::Counters c;
    Check(Frame(Guest(1, 1), s, c).source == sh::Source::Draw3D && c.game_no_scene_frames == 0, "ENTRBLACK no-entrance start: drawn from the first frame");
  }
  {   // no camera input at all: a scene whose motion camera never animates is drawn on every frame it is shown, right after a black one (no hold, no cap)
    sh::State s; sh::Counters c;
    Frame(Guest(0, 0), s, c);
    uint32_t drawn = 0;
    for (int i = 0; i < 300; ++i) drawn += Frame(Guest(1, 0), s, c).source == sh::Source::Draw3D ? 1u : 0u;
    Check(drawn == 300, "ENTRBLACK: after a black frame, every shown frame is drawn at once (no hold)", std::to_string(drawn));
  }
  {   // the characters: the stage hidden but a character shown (an editor's preview): the game draws geometry, so it is drawn
    sh::State s; sh::Counters c;
    Check(Frame(Guest(0, 1), s, c).source == sh::Source::Draw3D, "ENTRBLACK: stage hidden, a character shown: drawn");
  }
  {   // the reader: no stage object, a wild pointer, an unreadable word: nothing read, never black
    bool r = true, d = true;
    FakeGuest none;
    none.Put(0x83763544u, 0);
    rdr_scene::ReadSceneFact([&none](uint32_t a, uint32_t& v) { return none.Read(a, v); }, r, d);
    Check(!r && !rdr_scene::GameDrawsNoScene(rdr_scene::SceneFactBits(r, d)), "ENTRBLACK reader: no stage object: not read, never black");
    FakeGuest wild = Guest(0, 0);
    wild.Put(0x83763544u, 0xFFFFFFF0u);
    rdr_scene::ReadSceneFact([&wild](uint32_t a, uint32_t& v) { return wild.Read(a, v); }, r, d);
    Check(!r, "ENTRBLACK reader: a wild stage pointer: not read");
    FakeGuest hole = Guest(0, 0);
    hole.w.erase(hole.w.begin() + 3);   // character 0's +1208 unreadable
    rdr_scene::ReadSceneFact([&hole](uint32_t a, uint32_t& v) { return hole.Read(a, v); }, r, d);
    Check(r && d, "ENTRBLACK reader: an unreadable character word counts as drawn (never black on a failed read)");
    sh::Counters c0;
    Check(sh::CensusLine(c0).find("shown0") == std::string::npos && sh::CensusLine(c0).find("game_no_scene") == std::string::npos, "ENTRBLACK: no camera-gate counts in the [host] line, no scene count when none happened");
  }
  Check(rdr_scene::SceneFactBits(true, true) == (rdr_scene::kSceneFactRead | rdr_scene::kSceneDrawn) && rdr_scene::SceneFactBits(false, false) == 0 && !rdr_scene::GameDrawsNoScene(0) && !rdr_scene::GameDrawsNoScene(1u),
        "ENTRBLACK: the header bits (bit 0 = the stage push is not touched, an unread fact never hides)");
}
// S103 HN: the never-fed rule (host_core.h SceneFeed + Decide's NeverFed branch).
static void FeedArm() {
  constexpr uint32_t kRing = 1u << 6, kCrowd = 1u << 10, kScreens = 1u << 14, kPalette = 1u << 3;
  const uint32_t req = kRing | kCrowd | kScreens | kPalette;
  sh::SceneFeed fd;
  fd.NewScene(100);
  Check(fd.Note(req, kPalette) == (kRing | kScreens), "S103 HN never_fed: the STRUCTURAL bits not stamped since the scene began hold the frame (crowd bit 10 is not structural)");
  Check(fd.Note(req, kPalette | kRing) == kScreens, "S103 HN never_fed: a bit stamped once is no longer held");
  Check(fd.Note(req, kPalette | kRing | kScreens) == 0, "S103 HN never_fed: all structural fed = drawn (bit 10 never fed does not hold)");
  {   // B1/B2 (Opus review): bits 10 / 11 are never in the rule; the mask is exactly the structural list; a bit nothing ever stamped is left out and named
    constexpr uint32_t kParticles = 1u << 11;
    sh::SceneFeed h;
    h.NewScene(1);
    Check((sh::kFeedStructural & (kCrowd | kParticles)) == 0 && sh::kFeedStructural == ((1u << 3) | (1u << 5) | (1u << 6) | (1u << 12) | (1u << 14) | (1u << 15) | (1u << 16)), "S103 HN never_fed: bits 10 / 11 are not in the structural mask, the mask is 3 5 6 12 14 15 16");
    Check(h.Note(kCrowd | kParticles, 0) == 0, "S103 HN never_fed: required crowd + particles, never stamped: NOT held (the OFF host once held every scene's first 180 frames on them)");
    const uint32_t held_bits = h.Note(kRing | kScreens, 0, kRing);   // 14 has no stamp anywhere this session
    Check(held_bits == kRing && h.last_unstamped == kScreens, "S103 HN never_fed: a structural bit nothing ever stamped is left out and reported (never_fed_skipped_unstamped)");
    sh::SceneFeed n;   // a whole scene whose structural inputs never arrive: held at most kMaxHold frames, then drawn
    n.NewScene(1);
    uint32_t frames_held = 0;
    for (uint32_t i = 0; i < 1000; ++i)
      if (n.Note(sh::kFeedStructural, 0)) ++frames_held;
    Check(frames_held <= 60 && frames_held == sh::SceneFeed::kMaxHold && n.gave_up, "S103 HN never_fed: a scene whose structural inputs never come holds at most 60 frames, then draws", std::to_string(frames_held));
  }
  Check(fd.Note(req, 0) == 0, "S103 HN never_fed: a bit that was fed and is now stale is only counted (ONF), never held again");
  fd.NewScene(500);
  Check(fd.Note(req, 0) == (kPalette | kRing | kScreens), "S103 HN never_fed: a new scene starts unfed (the old scene stamps do not count)");
  {   // the cap: an input that never exists in a scene must not hold the picture for good
    sh::SceneFeed g;
    g.NewScene(1);
    uint32_t last = 1;
    for (uint32_t i = 0; i < sh::SceneFeed::kMaxHold + 5; ++i) last = g.Note(kRing, 0);
    Check(last == 0 && g.gave_up, "S103 HN never_fed: the hold is bounded (kMaxHold), then the frame is drawn and the give-up is flagged");
  }
  {   // review fix 2: kMaxHold is guest frames, not calls: at 30 fps the host handles every second frame and must still give up after ~60 frames
    sh::SceneFeed g;
    g.NewScene(1000);
    uint64_t frame = 1000;
    uint32_t calls = 0;
    bool gave = false;
    for (; calls < 200 && !gave; ++calls, frame += 2) { g.Note(kRing, 0, 0xFFFFFFFFu, frame); gave = g.gave_up; }
    Check(gave && frame - 1000 <= sh::SceneFeed::kMaxHold + 2 && calls <= sh::SceneFeed::kMaxHold / 2 + 2, "S103 never_fed: the hold gives up after kMaxHold GUEST frames (every second frame handled), not after kMaxHold calls", "calls " + std::to_string(calls));
  }
  {   // Decide: no draw, the last 3D picture held (or black with none), counted per bit, no strike
    sh::State s;
    sh::Counters c;
    sh::Facts f = F(true, sh::DrawResult::Miss);
    f.never_fed = kRing | kCrowd;
    sh::Plan p = sh::Decide(s, f, c);
    Check(p.source == sh::Source::Black && p.reason == sh::Reason::NoLast3D && !p.strike && c.never_fed_frames == 1 && c.never_fed_bit[6] == 1 && c.never_fed_bit[10] == 1 && c.black_no_3d == 1, "S103 HN never_fed: the very first frame is black_no_3d, counted per bit, no strike");
    Check(sh::CensusLine(c).find("never_fed 1 [bit 6:1 bit 10:1]") != std::string::npos, "S103 HN never_fed: the [host] line names the bits");
    sh::Decide(s, F(true, sh::DrawResult::Ok), c);
    p = sh::Decide(s, f, c);
    Check(p.source == sh::Source::Hold3D && p.reason == sh::Reason::NeverFed && !p.strike, "S103 HN never_fed: with a last 3D picture the frame holds it");
  }
}

static void DecideArm() {
  sh::State st;
  sh::Counters c;
  sh::Plan p = sh::Decide(st, F(false, sh::DrawResult::NotTried), c);
  Check(p.source == sh::Source::Black && p.remake_f && p.reason == sh::Reason::NoLast3D && c.black_no_3d == 1, "R3 before any 3D picture: black, F made, never `out`");
  Check(c.black_mid_match == 0, "S103 OFFT: the black of the first frame is not mid-match");
  p = sh::Decide(st, F(true, sh::DrawResult::Ok), c);
  Check(p.source == sh::Source::Draw3D && !p.remake_f && c.ours == 1, "ours + DrawLive ok: `out` into F");
  {   // S103 OFFT: a black with no last picture AFTER a 3D picture of this scene is mid-match; after a flat frame the next one is a first frame again
    sh::State s2;
    sh::Counters c2;
    sh::Decide(s2, F(true, sh::DrawResult::Ok), c2);                    // a 3D picture
    sh::Decide(s2, F(true, sh::DrawResult::Fault), c2);                  // draw: fault with F holding it: F as it was (no black)
    s2.f_is_3d = false;                                                  // (F no longer holds a 3D picture: the fault repeat cannot apply)
    sh::Decide(s2, F(true, sh::DrawResult::Miss), c2);                   // a miss with no valid last picture: black, mid-match
    Check(c2.black_no_3d == 1 && c2.black_mid_match == 1, "S103 OFFT: black with no last picture after a 3D picture is counted mid-match", std::to_string(c2.black_mid_match));
    sh::Decide(s2, F(false, sh::DrawResult::NotTried, false, /*flat=*/true), c2);   // a flat frame: a new scene follows
    sh::Decide(s2, F(false, sh::DrawResult::NotTried), c2);              // black again, but this is the first frame of the next scene
    Check(c2.black_no_3d == 2 && c2.black_mid_match == 1, "S103 OFFT: after a flat frame the next black is a first frame, not mid-match");
    Check(sh::CensusLine(c2).find("(mid_match 1)") != std::string::npos, "S103 OFFT: the [host] line carries the split");
  }
  p = sh::Decide(st, F(false, sh::DrawResult::NotTried), c);
  Check(p.source == sh::Source::Hold3D && p.reason == sh::Reason::NotOurs3D && c.repeat_3d == 2, "R3 (the game walked): the last 3D picture is held");
  p = sh::Decide(st, F(true, sh::DrawResult::Miss, /*loading=*/true), c);
  Check(p.source == sh::Source::Hold3D && c.repeat_loading == 1 && !p.strike, "R5 loading miss: last 3D held, counted loading, no strike");
  p = sh::Decide(st, F(true, sh::DrawResult::Miss), c);
  Check(p.source == sh::Source::Hold3D && p.strike && c.strikes == 1, "R5 real miss: last 3D held, one strike line");
  // the past rule: a draw: fault leaves `out` half drawn; the repeat must be F unchanged, and `out` must not come back until a draw succeeds
  const sh::DrawResult why = sh::ClassifyWhy("draw: record failed");
  Check(why == sh::DrawResult::Fault, "classify: a \"draw:\" failure is a Fault");
  Check(sh::ClassifyWhy("not resident: 2 asset(s) loading") == sh::DrawResult::Miss, "classify: any other refusal is a Miss");
  p = sh::Decide(st, F(true, why), c);
  Check(p.source == sh::Source::HoldF && p.reason == sh::Reason::DrawFault && p.strike && c.repeat_full == 1, "R5b: a draw: fault presents F unchanged (never `out`)");
  p = sh::Decide(st, F(true, sh::DrawResult::Miss, true), c);
  Check(p.source != sh::Source::Hold3D, "after a fault the next miss still does not copy the half-drawn `out`", sh::SourceName(p.source));
  p = sh::Decide(st, F(false, sh::DrawResult::NotTried), c);
  Check(p.source != sh::Source::Hold3D, "after a fault a walked frame does not copy `out` either", sh::SourceName(p.source));
  p = sh::Decide(st, F(true, sh::DrawResult::Ok), c);
  Check(p.source == sh::Source::Draw3D, "a good draw ends the fault: `out` is a picture again");
  p = sh::Decide(st, F(false, sh::DrawResult::NotTried), c);
  Check(p.source == sh::Source::Hold3D, "...and holds again");
  // R6: no record / no picture / no size: F unchanged, never black, never the stream
  sh::Facts f = F(false, sh::DrawResult::NotTried);
  f.have_record = false;
  p = sh::Decide(st, f, c);
  Check(p.source == sh::Source::HoldF && p.reason == sh::Reason::NoRecord && c.no_record == 1, "R6 no record: F presented unchanged, counted");
  f = F(false, sh::DrawResult::NotTried), f.no_picture = true;
  p = sh::Decide(st, f, c);
  Check(p.source == sh::Source::HoldF && c.no_picture == 1, "R6 r4 == 0: F presented unchanged, counted");
  f = F(false, sh::DrawResult::NotTried), f.size_known = false, f.width = f.height = 0;
  p = sh::Decide(st, f, c);
  Check(p.source == sh::Source::HoldF && c.no_size == 1, "R6 unknown size: F presented unchanged, counted");
  // R2: a flat frame is black (the rows come over it in H2), and the old arena picture does not come back over a menu afterwards
  p = sh::Decide(st, F(false, sh::DrawResult::NotTried, false, /*flat=*/true), c);
  Check(p.source == sh::Source::Black && p.reason == sh::Reason::TwoDOnly && c.frame_2d_only == 1, "R2 flat frame: F black (a 2D frame), not a miss");
  p = sh::Decide(st, F(false, sh::DrawResult::NotTried), c);
  Check(p.source == sh::Source::Black && p.reason == sh::Reason::NoLast3D, "after a menu the old 3D picture is not held over the next scene (black, counted)");
  // resize: F remade, black one frame (the old `out` is the wrong size), then ours draws at the new size
  p = sh::Decide(st, F(true, sh::DrawResult::Ok), c);
  sh::Facts big = F(false, sh::DrawResult::NotTried);
  big.width = 1920, big.height = 1080;
  p = sh::Decide(st, big, c);
  Check(p.remake_f && p.source == sh::Source::Black && p.reason == sh::Reason::Resize && c.resize_black == 1 && p.width == 1920, "resize with no picture at the new size: F remade, black one frame");
  big.ours = true, big.draw = sh::DrawResult::Ok;
  p = sh::Decide(st, big, c);
  Check(p.source == sh::Source::Draw3D && !p.remake_f, "after the resize a good draw shows at the new size");
  // nothing yet and nothing known: no present at all (never a guessed black)
  sh::State st2;
  sh::Counters c2;
  f = F(false, sh::DrawResult::NotTried), f.have_record = false, f.size_known = false, f.width = f.height = 0;
  p = sh::Decide(st2, f, c2);
  Check(!p.present, "no F and no size: no present this tick");
}

// ---- the present size the Tick carries --------------------------------------------------------------------------------------------------------------
static std::vector<ft::Tick> g_ticks;
static void Poster(const ft::Tick& t) { g_ticks.push_back(t); }
static void SizeArm() {
  // fetch words as the 360 writes them: dword 2 = (height - 1) << 13 | (width - 1), dword 5 bits 9-10 = the dimension (1 = 2D)
  const uint32_t w720[6] = {0, 0, (719u << 13) | 1279u, 0, 0, 1u << 9};
  uint32_t w = 0, h = 0;
  Check(ft::SizeOfFetch(w720, w, h) && w == 1280 && h == 720, "present size: 1280x720 from fetch dword 2", std::to_string(w) + "x" + std::to_string(h));
  const uint32_t w1080[6] = {0, 0, (1079u << 13) | 1919u, 0, 0, 1u << 9};
  Check(ft::SizeOfFetch(w1080, w, h) && w == 1920 && h == 1080, "present size: 1920x1080");
  const uint32_t border[6] = {0, 0, (717u << 13) | 1277u, 1u << 31, 0, 1u << 9};   // a border adds 3, not 1 (reader.h ShapeOf)
  Check(ft::SizeOfFetch(border, w, h) && w == 1280 && h == 720, "present size: the border bit adds 3");
  const uint32_t cube[6] = {0, 0, (719u << 13) | 1279u, 0, 0, 3u << 9};
  Check(!ft::SizeOfFetch(cube, w, h), "present size: a cube / volume is not a plain 2D picture (unknown)");
  // the tick carries it, and r4 == 0 sets no_picture
  g_ticks.clear();
  ft::g_post.store(&Poster);
  ft::PresentPicture pic;
  pic.fetch = w720;
  ft::StreamFreePresentCore(10, 1, pic);
  ft::PresentPicture none;
  none.has_texture = false;
  ft::StreamFreePresentCore(11, 1, none);
  ft::StreamFreePresentCore(12, 1);   // the old two-argument call: size unknown, a picture present
  ft::g_post.store(nullptr);
  Check(g_ticks.size() == 3 && g_ticks[0].width == 1280 && g_ticks[0].height == 720 && !g_ticks[0].no_picture, "tick: width / height from the present texture");
  Check(g_ticks.size() == 3 && g_ticks[1].no_picture && g_ticks[1].width == 0, "tick: r4 == 0 is no_picture, no size");
  Check(g_ticks.size() == 3 && !g_ticks[2].no_picture && g_ticks[2].width == 0, "tick: no words read = size unknown (the host then repeats F)");
}

// ---- the tween's gate and camera-cut test (S103 P7 tween lane): host_core.h TweenGate / CameraJumped / Mailbox::Pending ----------------------------------------------------------------------------
static sh::TweenFacts GoFacts() {
  sh::TweenFacts t;
  t.wanted = t.prev_valid = t.presenting = true;   // the plain case: everything is there, nothing waits, no cut
  return t;
}
static sh::TweenCam MakeCam(float ex, float ey, float ez, float yaw_deg, float scale = 1.0f, bool transposed = false) {
  sh::TweenCam c;
  c.have = true, c.eye[0] = ex, c.eye[1] = ey, c.eye[2] = ez;
  const float a = yaw_deg * 3.14159265f / 180.f, cs = std::cos(a) * scale, sn = std::sin(a) * scale;
  const float r[16] = {cs, 0, -sn, 0, 0, scale, 0, 0, sn, 0, cs, 0, -ex, -ey, -ez, 1};   // a yaw about y, scaled (a view-projection looks like this), translation in the last row
  for (int i = 0; i < 16; ++i) c.rot[i] = r[i];
  if (transposed) for (int i = 0; i < 4; ++i) for (int j = i + 1; j < 4; ++j) std::swap(c.rot[i * 4 + j], c.rot[j * 4 + i]);   // the other matrix layout
  return c;
}
static void TweenArm() {
  sh::TweenFacts t = GoFacts();
  Check(sh::TweenGate(t) == sh::TweenVerdict::Go, "tween gate: wanted + previous tick + presenting + nothing waiting = go");
  t = GoFacts(), t.wanted = false;
  Check(sh::TweenGate(t) == sh::TweenVerdict::Off, "tween gate: not asked for = off");
  t = GoFacts(), t.presenting = false;
  Check(sh::TweenGate(t) == sh::TweenVerdict::Off, "tween gate: a tick that presents no drawn picture = off");
  t = GoFacts(), t.prev_valid = false;
  Check(sh::TweenGate(t) == sh::TweenVerdict::Cold, "tween gate: no previous tick = cold (nothing to blend from)");
  t = GoFacts(), t.newer_waiting = true;
  Check(sh::TweenGate(t) == sh::TweenVerdict::Waiting, "tween gate: a newer tick already waiting drops the in-between");
  t = GoFacts(), t.newer_waiting = true, t.picture_requested = true;
  Check(sh::TweenGate(t) == sh::TweenVerdict::Go, "tween gate: a requested picture still gets its halfway frame");
  t = GoFacts(), t.camera_cut = true;
  Check(sh::TweenGate(t) == sh::TweenVerdict::Cut, "tween gate: a camera cut = no blend across it");
  t = GoFacts(), t.scene_changed = true;
  Check(sh::TweenGate(t) == sh::TweenVerdict::Cut, "tween gate: a scene / size change = no blend across it");
  // the camera
  const sh::TweenCam a = MakeCam(0, 150, -400, 10);
  Check(!sh::CameraJumped(a, MakeCam(2, 150, -402, 11)), "camera: a small move + 1 degree is not a cut");
  Check(!sh::CameraJumped(MakeCam(0, 150, -400, 10, 1.8f), MakeCam(2, 150, -402, 11, 1.8f)), "camera: a scaled (view-projection) matrix reads the same");
  Check(!sh::CameraJumped(MakeCam(0, 150, -400, 10, 1.0f, true), MakeCam(2, 150, -402, 11, 1.0f, true)), "camera: the transposed layout reads the same");
  Check(sh::CameraJumped(a, MakeCam(900, 150, -400, 10)), "camera: the eye jumped 900 units = a cut");
  Check(sh::CameraJumped(a, MakeCam(0, 150, -400, 100)), "camera: turned 90 degrees in one tick = a cut");
  // review F5: the thresholds are DLSS's reset numbers (src\dlaa.h: 15 degrees / 200 units) and a zoom is a cut
  Check(sh::CameraJumped(a, MakeCam(0, 150, -400, 26)), "camera: turned 16 degrees in one tick = a cut (gpu_dlaa_reset_degrees 15)");
  Check(!sh::CameraJumped(a, MakeCam(0, 150, -400, 23)), "camera: turned 13 degrees in one tick is not a cut");
  Check(sh::CameraJumped(a, MakeCam(210, 150, -400, 10)), "camera: the eye moved 210 units = a cut (gpu_dlaa_reset_distance 200)");
  Check(!sh::CameraJumped(a, MakeCam(190, 150, -400, 10)), "camera: the eye moved 190 units is not a cut");
  sh::TweenCam za = MakeCam(0, 150, -400, 10), zb = MakeCam(0, 150, -400, 10);
  za.fov_y_deg = 42.0f, zb.fov_y_deg = 50.0f;
  Check(sh::CameraJumped(za, zb), "camera: a pure zoom (fov 42 -> 50 degrees, nothing else moved) is a cut");
  zb.fov_y_deg = 43.5f;
  Check(!sh::CameraJumped(za, zb), "camera: fov 42 -> 43.5 is a normal zoom step");
  za.fov_y_deg = zb.fov_y_deg = 0, za.proj_x = 1.0f, zb.proj_x = 1.3f;
  Check(sh::CameraJumped(za, zb), "camera: a projection x scale that changed by 30 % is a cut");
  sh::TweenCam flagged = MakeCam(1, 150, -400, 10);
  flagged.cut = 1;
  Check(sh::CameraJumped(a, flagged), "camera: the record's own cut flag = a cut");
  sh::TweenCam none;
  Check(!sh::CameraJumped(none, a) && !sh::CameraJumped(a, none), "camera: a record with no camera is never called a cut (the keys decide)");
  // the mailbox's waiting flag (the gate's input)
  sh::Counters c;
  sh::Mailbox<int> box(c);
  Check(!box.Pending(), "mailbox: Pending is false when nothing waits");
  box.Post(Make(1));
  Check(box.Pending(), "mailbox: Pending is true while a newer tick waits");
  Msg m;
  box.Take(m);
  Check(!box.Pending(), "mailbox: Pending is false again once it was taken");
}

// S103 FLAT: a frame with no 3D scene of its own (menu, loading, results) is flat, never the last arena picture. The game-side half is scene_stamp_rule.h: which particle-node call may take the Scene3D decision
// (the node's camera: the 2D screen camera = a menu backdrop; a call that draws nothing is no evidence either). The render side then reads the stamp (live_scene_draw.cpp FlatFacts3D -> scene_kind::IsFlatFrame).
static sh::Facts FromStamp(bool stamped, bool ours = false, sh::DrawResult d = sh::DrawResult::NotTried) {
  scene_kind::FlatFacts ff;   // a record with no stage and no 3D object (a menu): only the stamp differs
  ff.has_stage = false, ff.objects3d = 0, ff.scene3d_stamp = stamped;
  sh::Facts f = F(ours, d);
  f.flat = !ours && scene_kind::IsFlatFrame(ff);
  return f;
}
static bool g_f1_plant = false;
static void FlatArm() {
  namespace sr = scene_stamp_rule;
  Check(!sr::ParticleCallAsksScene(true, true, false), "stamp rule: a menu backdrop call (screen camera) does not ask the scene decision");
  Check(!sr::ParticleCallAsksScene(true, false, true) && !sr::ParticleCallAsksScene(false, false, true), "stamp rule: a call that draws nothing (zero word / null camera) does not ask");
  Check(sr::ParticleCallAsksScene(true, false, false), "stamp rule: a call that draws with the scene camera asks (a match frame is stamped as before)");
  Check(sr::ParticleCallAsksScene(false, false, false), "stamp rule: an unreadable camera is no proof of a menu: it asks as before");
  sh::State st;
  sh::Counters c;
  sh::Plan p = sh::Decide(st, F(true, sh::DrawResult::Ok), c);   // an arena frame
  Check(p.source == sh::Source::Draw3D && st.last3d_valid, "flat: an arena frame is drawn and kept as the last 3D picture");
  // the menu frame: its only scene-hook ask is the backdrop particle node, screen camera
  const bool stamped = sr::ParticleCallAsksScene(true, true, false);
  sh::Facts menu = FromStamp(stamped);
  Check(menu.flat, "flat: a menu frame whose only hook is the menu particles is a FLAT frame (no Scene3D stamp)");
  p = sh::Decide(st, menu, c);
  Check(p.source == sh::Source::Black && p.reason == sh::Reason::TwoDOnly && c.frame_2d_only == 1, "flat: the menu frame is black under its 2D rows (the flat kind), not Hold3D");
  Check(!st.last3d_valid, "flat: the 3D frame then the menu frame clears last3d_valid (no stale arena under the menu)");
  p = sh::Decide(st, F(false, sh::DrawResult::NotTried), c);   // a later frame the host cannot call flat
  Check(p.source != sh::Source::Hold3D, "flat: nothing holds the old arena picture after the menu");
  // F1: post / plane / prelight / shadow / rim also run on non-3D pages of game sequence 9 (in-game load, results): their stamp on a stageless record is NOT READY, which is no 3D walk.
  {
    namespace sk = scene_kind;
    const uint8_t kOurs = uint8_t(rdr_own::Decision::Ours), kWalks = uint8_t(rdr_own::Decision::GameWalks), kNotReady = uint8_t(rdr_own::WhyNot::NotReady);
    const bool old_rule = g_f1_plant;   // plant 17 any_stamp_is_3d: the old rule
    Check(!sk::StampMakes3D(kWalks, kNotReady, old_rule), "F1: a NOT READY stamp (post / plane on a stageless frame) is not a 3D frame");
    Check(sk::StampMakes3D(kOurs, uint8_t(rdr_own::WhyNot::None)) && sk::StampMakes3D(kWalks, uint8_t(rdr_own::WhyNot::Hold)), "F1: an OURS stamp, or a refusal other than NOT READY, still makes a 3D frame");
    sh::State s2;
    sh::Counters c2;
    sh::Decide(s2, F(true, sh::DrawResult::Ok), c2);   // an arena picture, then a sequence-9 page whose hooks (post / plane) stamped NOT READY
    sk::FlatFacts ff;
    ff.has_stage = false, ff.objects3d = 0, ff.scene3d_stamp = sk::StampMakes3D(kWalks, kNotReady, old_rule);
    sh::Facts pg = F(false, sh::DrawResult::NotTried);
    pg.flat = sk::IsFlatFrame(ff);
    Check(pg.flat, "F1: a stageless, object-less frame stamped NOT READY by post / plane is FLAT");
    sh::Plan q = sh::Decide(s2, pg, c2);
    Check(q.source == sh::Source::Black && !s2.last3d_valid, "F1: so a sequence-9 page after an arena is black under its rows, no stale arena");
    ff.has_stage = true, ff.objects3d = 251;   // fail closed: a stage in the record is never flat, whatever the stamp says ... while a model-class hook walked (S103 WB: the scene-walk bit, scene_kind.h)
    sk::FlatFacts walked = ff, idle = ff;
    sk::ApplyWalk(true, walked);
    Check(!sk::IsFlatFrame(walked), "F1: a frame with a stage and a scene walk stays a 3D frame");
    sk::ApplyWalk(false, idle);   // the post-match menu: the sticky arena (stage + 251 objects) in the record, no Model / Crowd / Ring ask
    Check(sk::IsFlatFrame(idle) || g_f1_plant, "WB: a stage in the record with NO scene walk is a flat frame (the sticky arena is not a picture)");
    {
      sh::State s3;
      sh::Counters c3;
      sh::Decide(s3, F(true, sh::DrawResult::Ok), c3);   // the last match frame: an arena picture
      sh::Facts menu2 = F(false, sh::DrawResult::NotTried);
      menu2.flat = sk::IsFlatFrame(idle);
      const sh::Plan z = sh::Decide(s3, menu2, c3);
      Check((z.source == sh::Source::Black && !s3.last3d_valid) || g_f1_plant, "WB: so the post-match menu frame is black under its rows, never the held arena");
    }
  }
}

// S105 A4 (review F1 / F2): the menu after Create a Finisher, end to end through the production order of Handle: WalkGate::Demote per record, the demoted record's flat facts (sticky stage + an OURS stamp + no walk bit)
// through ApplyWalk + DropStampWhenDemoted + IsFlatFrame, then the real Decide. Plants 24 (the stamp is kept as 3D evidence: the first build of the piece, Hold3D / black_no_3d) and 25 (K = 1: a single record demotes).
// entry: scene_kind::WalkGate::Demote, scene_kind::DropStampWhenDemoted, scene_host::Decide (scene_host.cpp Handle's stamp loop and not-ours branch)
static bool g_a4_stamp_old = false, g_a4_k1 = false;
static void A4Arm() {
  namespace sk = scene_kind;
  sh::State st;
  sh::Counters c;
  sk::WalkGate gate;
  sh::Decide(st, F(true, sh::DrawResult::Ok), c);   // the Create a Finisher frames: drawn 3D (the last picture is kept)
  const uint64_t black0 = c.black_no_3d;
  sh::Plan p;
  int held = 0, black = 0, demoted_frames = 0;
  for (int rec = 0; rec < 12; ++rec) {   // the menu: READY by the sticky stage (ours), stamp OURS, no walk bit
    const bool dem = gate.Demote(false, false, g_a4_k1 ? 1 : sk::WalkGate::kRecords);
    sh::Facts f = F(!dem, sh::DrawResult::Miss, /*loading=*/true);   // an OURS miss while loading: hold3d (the inputs of a half-built scene)
    if (dem) {
      ++demoted_frames;
      sk::FlatFacts ff;
      ff.has_stage = true, ff.objects3d = 251;   // FlatFacts3D: the record's leftover arena
      ff.scene3d_stamp = sk::StampMakes3D(uint8_t(rdr_own::Decision::Ours), uint8_t(rdr_own::WhyNot::None));
      sk::ApplyWalk(false, ff);
      sk::DropStampWhenDemoted(ff, true, g_a4_stamp_old);
      f.draw = sh::DrawResult::NotTried, f.loading = false;
      f.flat = sk::IsFlatFrame(ff);
    }
    p = sh::Decide(st, f, c);
    if (p.source == sh::Source::Hold3D) ++held;
    if (p.source == sh::Source::Black) ++black;
  }
  Check(held == 2 && demoted_frames == 10, "A4 F2: the first two no-walk records of a menu still hold the arena picture; the next ten are demoted", "held " + std::to_string(held) + " demoted " + std::to_string(demoted_frames));
  Check(p.source == sh::Source::Black && p.reason == sh::Reason::TwoDOnly && black == 10, "A4 F1: a demoted menu frame is FLAT: black under its rows (TwoDOnly), every one of them", "black " + std::to_string(black));
  Check(c.black_no_3d == black0 && c.frame_2d_only == 10 && !st.last3d_valid, "A4 F1: and it is not counted black_no_3d (a flat frame is a different scene, not a missing picture)", "black_no_3d " + std::to_string(c.black_no_3d));
  sk::WalkGate g2;   // a walk bit mid-match resets the run: no demotion
  Check(!g2.Demote(false, false) && !g2.Demote(true, false) && !g2.Demote(false, false) && !g2.Demote(false, false), "A4 F2: one record missing its walk bit between walks never demotes");
}

// S105 A4B (entry: sh::Decide, sh::SceneRowsOnF): the flat frame (Create a Title: the game drew no 3D) is the only frame whose scene-colour rows are drawn on F. Production order: the host's Decide of the frame's facts -> the
// plan's reason -> SceneRowsOnF (SceneHost::Handle passes it to RowsMayDraw and DrawRows). A Hold3D frame (the game walked, the picture is held) keeps the drop. Planted 35 (a Hold3D frame draws them too) must fail.
static void SceneOnFArm() {
  sh::State st;
  sh::Counters c;
  sh::Plan p = sh::Decide(st, F(true, sh::DrawResult::Ok), c);    // a 3D picture first (so a hold has something to hold)
  p = sh::Decide(st, F(false, sh::DrawResult::NotTried, false, /*flat=*/true), c);
  Check(p.reason == sh::Reason::TwoDOnly && sh::SceneRowsOnF(p.reason), "A4B: a flat (TwoDOnly) frame draws its scene-colour rows on F");
  sh::State s2;
  sh::Counters c2;
  sh::Decide(s2, F(true, sh::DrawResult::Ok), c2);
  p = sh::Decide(s2, F(false, sh::DrawResult::NotTried), c2);     // the game walked, no READY draw: the last picture is held
  Check(p.source == sh::Source::Hold3D && !sh::SceneRowsOnF(p.reason), "A4B: a Hold3D frame keeps the scene-colour drop (a held picture may already hold the row)");
  p = sh::Decide(s2, F(true, sh::DrawResult::Ok), c2);
  Check(!sh::SceneRowsOnF(p.reason), "A4B: a drawn 3D frame never draws scene-colour rows on F (the scene hook drew them in the scene pass)");
  c.scene2d_early = 3, c.scene_rows_on_f = 5;
  Check(sh::CensusLine(c).find("scene2d_early 3, scene_rows_on_f 5") != std::string::npos, "A4B: the [host] frames line carries scene2d_early and scene_rows_on_f");
}

// S111 BLACKFRAME (entry: sh::Decide -> sh::BlackSpans::Note / Close / SummaryLine, as SceneHost::Handle and LogFinal call them; the shown kind from the plan as Handle's present switch sets
// it). Run 328 (S110) showed 0.9 s of black at an entrance start and its log said only "black_no_3d 59 / 86". The replay: the scene's OURS frames arrive before anything the host can draw:
// first required inputs never fed (arena worlds bit 5, ring rows bit 6), a frame with no record between (F unchanged: still black), then the draw refused while the arena loads, then
// the first drawn frame; later a flat frame, and a miss with nothing to hold after a picture of this scene (mid-match). Each run of black is ONE line naming the branch (no_last3d)
// and the input that asked for the hold. Planted 52 (Decide drops the hold's cause: the S110 state) must fail it.
static void BlackFrameArm() {
  sh::State st;
  sh::Counters c;
  sh::BlackSpans bs;
  std::vector<std::string> lines;
  const auto frame = [&](uint64_t j, const sh::Facts& f, const std::string& why) {
    const sh::Plan p = sh::Decide(st, f, c);
    const sh::ShownKind shown = !p.present ? sh::ShownKind::Unchanged : p.source == sh::Source::Black ? sh::ShownKind::Black : p.source == sh::Source::HoldF ? sh::ShownKind::Unchanged : sh::ShownKind::Picture;
    if (shown == sh::ShownKind::Black || bs.Open()) {
      const std::string l = bs.Note(j, shown, p, f, why);
      if (!l.empty()) lines.push_back(l);
    }
    return p;
  };
  uint64_t j = 5000;
  for (int i = 0; i < 30; ++i, ++j) {
    sh::Facts f = F(true, sh::DrawResult::Miss);
    f.never_fed = (1u << 5) | (1u << 6);
    frame(j, f, "never_fed: required record input(s) not stamped since this scene began (first: bit 5)");
  }
  {
    sh::Facts f = F(false, sh::DrawResult::NotTried);
    f.have_record = false;
    frame(j++, f, "");
  }
  for (int i = 0; i < 56; ++i, ++j) frame(j, F(true, sh::DrawResult::Miss, /*loading=*/true), "not resident: 2 asset(s) loading");
  const sh::Plan p = frame(j++, F(true, sh::DrawResult::Ok), "");
  const std::string want0 = "[host] black frames 5000-5030 (n 30, held 1): no_last3d <- never_fed | inputs: ours 1 flat 0 no_scene 0 loading 0 never_fed [5 6] | why: never_fed: required record input(s) not stamped since this scene began (first: bit 5)";
  const std::string want1 = "[host] black frames 5031-5086 (n 56, held 0): no_last3d <- miss_loading | inputs: ours 1 flat 0 no_scene 0 loading 56 never_fed - | why: not resident: 2 asset(s) loading";
  Check(p.source == sh::Source::Draw3D && c.black_no_3d == 86 && lines.size() == 2, "BLACKFRAME run 328: 86 black frames before the first draw are TWO runs (never_fed, then the loading arena)",
        std::to_string(lines.size()) + " lines, black_no_3d " + std::to_string(c.black_no_3d));
  Check(lines.size() >= 1 && lines[0] == want0, "BLACKFRAME run 328: the first run names no_last3d <- never_fed, its bits and why", lines.empty() ? "" : lines[0]);
  Check(lines.size() >= 2 && lines[1] == want1, "BLACKFRAME run 328: the second run names no_last3d <- miss_loading", lines.size() < 2 ? "" : lines[1]);
  frame(j++, F(false, sh::DrawResult::NotTried, false, /*flat=*/true), "flat frame");   // 5088: a menu frame (2d_only)
  frame(j++, F(true, sh::DrawResult::Ok), "");                                         // 5089: drawn: ends the 2d_only run
  st.last3d_valid = false, st.f_is_3d = false;                                            // (no picture left to hold, as after a spoiled `out`)
  frame(j++, F(true, sh::DrawResult::Miss), "the record has no camera");                // 5090: black, mid-match
  const std::string last = bs.Close();
  Check(lines.size() == 3 && lines[2] == "[host] black frames 5088-5088 (n 1, held 0): 2d_only | inputs: ours 0 flat 1 no_scene 0 loading 0 never_fed - | why: flat frame",
        "BLACKFRAME: a flat frame is its own 2d_only run", lines.size() < 3 ? "" : lines[2]);
  Check(last == "[host] black frames 5090-5090 (n 1, held 0): no_last3d <- miss, mid_match | inputs: ours 1 flat 0 no_scene 0 loading 0 never_fed - | why: the record has no camera",
        "BLACKFRAME: the run open at the exit is closed by Close(), mid-match named", last);
  Check(bs.SummaryLine() == "[host] black spans 4 (frames black 88, held 1), lines printed 4", "BLACKFRAME: the exit summary counts every run", bs.SummaryLine());
  Check(bs.Close().empty() && !bs.Open(), "BLACKFRAME: nothing open after Close");
  {   // the line cap: past kMaxLines runs are only counted
    sh::BlackSpans cap;
    sh::Plan pb;
    pb.source = sh::Source::Black, pb.reason = sh::Reason::TwoDOnly;
    const sh::Facts fb = F(false, sh::DrawResult::NotTried, false, true);
    uint64_t printed = 0;
    for (uint64_t k = 0; k < sh::BlackSpans::kMaxLines + 5; ++k) {
      cap.Note(2 * k, sh::ShownKind::Black, pb, fb, "flat frame");
      if (!cap.Note(2 * k + 1, sh::ShownKind::Picture, pb, fb, "").empty()) ++printed;
    }
    Check(printed == sh::BlackSpans::kMaxLines && cap.SummaryLine() == "[host] black spans 405 (frames black 405, held 0), lines printed 400", "BLACKFRAME: past kMaxLines runs are counted, not printed",
          cap.SummaryLine());
  }
}

int main(int argc, char** argv) {
  if (argc >= 3 && std::string(argv[1]) == "plant") {
    sh::g_plant = std::atoi(argv[2]);
    if (sh::g_plant == 31) g_a4_stamp_old = true, sh::g_plant = 0;   // a4_stamp_kept: the first build of the piece (F1)
    if (sh::g_plant == 32) g_a4_k1 = true, sh::g_plant = 0;          // a4_no_k_guard: one record demotes (F2)
    if (sh::g_plant == 16) scene_stamp_rule::g_plant = 1, sh::g_plant = 0;   // menu_hook_is_3d: the old rule
    if (sh::g_plant == 17) g_f1_plant = true, sh::g_plant = 0;   // any_stamp_is_3d: F1 old rule
  }
  MailboxArm();
  RateArm();
  IdleArm();
  PlanesArm();
  ThreadArm();
  DecideArm();
  FeedArm();
  SceneFactArm();   // S110 ENTRBLACK (was S105 A3's NoCamArm)
  LoadFollowArm();   // S110 ENTRBLACK round 5
  StartWaitArm();   // S110 ENTRWAIT2
  SizeArm();
  TweenArm();
  FlatArm();
  A4Arm();
  SceneOnFArm();   // S105 A4B
  BlackFrameArm();   // S111 BLACKFRAME
  std::printf("%d checks, %d failed%s\n", g_checks, g_fail, (sh::g_plant || scene_stamp_rule::g_plant || g_f1_plant || g_a4_stamp_old || g_a4_k1) ? " (plant)" : "");
  return g_fail ? 1 : 0;
}
