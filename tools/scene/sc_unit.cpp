// Lane SC unit proof (Session 103, landing 2: our 2D must clip like the game's; 0 game runs). The scissor, the colour target and the viewport of a 2D draw, from the recorders' state (quads2d_core.h FillScissor / FillTarget / OnOp79 /
// OnSetRenderTarget / OnSetViewport), the layout-4 record, and the drawer's pure ScissorOf.
//   [SC TAP]     scissor on (state object +124) + the last op-79 rect -> a row with the box x y w h; off -> scissor_on 0; on with no rect / no state read -> no kQhScissor (a hole, RowProblem refuses)
//   [SC TARGET]  an offscreen page (512x512) reads offscreen, the 1280x720 frame reads frame; a later SetViewport overrides the whole-surface default; no target seen -> a hole
//   [SC RECORD]  layout 5, 324 bytes a row, round trip byte-equal; a layout-4 row is refused; scissor / target limits
//   [SC DRAW]    ScissorOf scales guest pixels to the target, clamps, flips nothing; a zero-area box is empty (Append skips it)
//   [SC RT HOLDER] review F1: SetRenderTarget's r5 is a pointer to the SLOT holding the surface (the recorder stores r5 at record +20, the executor does lwz r5,0(r10)); a null holder at the record is counted and unknown
//   [SC RESET]   review F2: slot-0 SetRenderTarget, op 47 (reset to the default target) and op 48 (SetDepthStencil) rewrite the device scissor (sub_82CABC68): the thread's rect is unknown after them (a scissor-on row is a hole)
//   [SC TARGET]  review F3: Final = the surface IS the default back buffer (*(engine+232)), never "1280x720": a 1280x720 scene-colour target reads Offscreen (the tap and the rig's TargetOfImage)
//   [SC DRAW]    review F4 / F5: ScissorOf scales by the row's target size and intersects with its viewport; Check counts a zero-area scissor row like Append
// Planted twins (--plant NAME, each must be CAUGHT): seed_off (PBS), scissor_not_filled, scissor_enable_ignored, scissor_y_flipped, scissor_ignored (drawer), scissor_y_flipped_draw (drawer), target_not_filled, target_always_frame,
// rt_holder_direct, reset_ignored, final_by_size, scissor_viewport_ignored, scissor_root_fixed, check_empty_ignored, cross_target_ignored.
//   [SC FRAME] [SC SAVE] [SC SCENE]  S103 PFR: the per-frame target rule (frame reset to the default target + unknown scissor, op-46 saves and op-45 restores, op 73 / op 74); plants frame_reset_skipped, save_ignored
//   [SC CROSS]   the dev-only live cross-check (CrossCheckStream): row i vs the i-th row-site stream draw, target class / size and scissor ∩ viewport
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../../src/renderer_scene/quads2d_draw.h"
#include "../../src/renderer_scene/quads2d_core.h"
#include "sc_support.h"
#include "../../src/renderer_scene/record_io.h"
#include "../../src/renderer_scene/quads2d_rig.h"

using namespace rdr_scene;
using namespace rdr_scene::q2d;

static std::string g_failed;
static int g_checks = 0, g_failures = 0;
static void Check(bool ok, const char* tag, const std::string& what) {
  ++g_checks;
  std::printf("%s [%s] %s\n", ok ? "PASS" : "FAIL", tag, what.c_str());
  if (!ok) {
    ++g_failures;
    if (g_failed.find(tag) == std::string::npos) g_failed += std::string(g_failed.empty() ? "" : ", ") + "[" + tag + "]";
  }
}
struct FakeMem {
  std::map<uint32_t, uint32_t> words;
  uint32_t deny = 0;   // an address that reads as unreadable (0 = none)
  bool ReadWords(uint32_t a, uint32_t* out, uint32_t n) const {
    if (deny && a == deny) return false;
    if (a < 0x10000u || uint64_t(a) + 4ull * n > 0xA0000000ull) return false;
    for (uint32_t i = 0; i < n; ++i) {
      auto it = words.find(a + 4 * i);
      out[i] = it == words.end() ? 0u : it->second;
    }
    return true;
  }
};
static const uint32_t kR13 = 0x40000100u, kBlock = 0x40000200u, kObj = 0x40000400u, kVerts = 0x40002000u, kSurf = 0x40005000u, kRect = 0x40006000u, kEngine = 0x40007000u, kHolder = 0x40008000u;
// The default back buffer: *(engine + 232) (kEngineGlobal -> the engine object).
static void PutEngine(FakeMem& m, uint32_t default_surface) { m.words[kEngineGlobal] = kEngine, m.words[kEngine + kEngineDefaultRt] = default_surface; }
// SetRenderTarget through the recorder's own view: r5 = the HOLDER (a slot that holds the surface pointer).
static bool SetRt(TargetState& tg, FakeMem& m, uint32_t slot, uint32_t surface, Counters* c = nullptr) { m.words[kHolder] = surface; return OnSetRenderTarget(tg, m, slot, kHolder, c); }
static void PutState(FakeMem& m, uint32_t scissor_enable) {   // r13 -> block -> (+1036) the state object; +124 the scissor enable cache word
  m.words[kR13] = kBlock, m.words[kBlock + kStateSlot] = kObj, m.words[kObj + kStScissorEnable] = scissor_enable;
}
static void PutSurface(FakeMem& m, uint32_t surf, uint32_t w, uint32_t h) { m.words[surf + 36] = ((w - 1) << 18) | ((h - 1) << 3); }
static void PutRect(FakeMem& m, int l, int t, int r, int b) {
  m.words[kRect] = uint32_t(l), m.words[kRect + 4] = uint32_t(t), m.words[kRect + 8] = uint32_t(r), m.words[kRect + 12] = uint32_t(b);
}
static void PutVerts(FakeMem& m) {
  const float v[4][3] = {{100, 50, 0.1f}, {300, 50, 0.1f}, {300, 150, 0.1f}, {100, 150, 0.1f}};
  for (int i = 0; i < 4; ++i)
    for (int c = 0; c < 3; ++c) {
      uint32_t b;
      std::memcpy(&b, &v[i][c], 4);
      m.words[kVerts + 12u * i + 4u * c] = b;
    }
}
// One rect draw through the core with the given thread state.
static Quad2D Draw(const FakeMem& m, const Scissor* sc, const TargetState* tg) {
  Store store;
  Counters c;
  Scope s;
  OpenScope(s, kQkRect, 0x7000AAAAu);
  OnOp19(s, m, store, 5, kSiteRect, 4, 4, 12, kVerts, c, nullptr, kR13, sc, tg);
  CloseScope(s, store, 5, c);
  std::vector<Quad2D> rows;
  store.Drain(5, rows);
  return rows.size() == 1 ? rows[0] : Quad2D{};
}
static void Tap() {
  FakeMem m;
  PutVerts(m);
  TargetState tg;
  PutEngine(m, kSurf);
  SetRt(tg, m, 0, 0);   // (no surface: stays unknown)
  PutSurface(m, kSurf, 1280, 720);
  SetRt(tg, m, 0, kSurf);
  PutRect(m, 126, 402, 522, 566);   // the menu list of c2d_menu frame 7: 126,402 396x164
  Scissor sc;
  Check(OnOp79(sc, m, kRect) && sc.known && sc.rect[0] == 126 && sc.rect[2] == 522, "SC TAP", "the op-79 recorder's four ints are read (left top right bottom)");
  PutState(m, 1);
  Quad2D q = Draw(m, &sc, &tg);
  Check((q.have & kQhScissor) && q.scissor_on == 1 && q.scissor[0] == 126 && q.scissor[1] == 402 && q.scissor[2] == 396 && q.scissor[3] == 164 && Quad2DRowProblem(q).empty(), "SC TAP", "scissor on + the rect 126,402,522,566 -> a row with the box 126,402 396x164 (the c2d_menu list)");
  PutState(m, 0);
  q = Draw(m, &sc, &tg);
  Check((q.have & kQhScissor) && q.scissor_on == 0 && !q.scissor[2] && Quad2DRowProblem(q).empty(), "SC TAP", "scissor off (state object +124 = 0): scissor_on 0, a valid row");
  PutState(m, 1);
  Scissor none;
  q = Draw(m, &none, &tg);
  Check(!(q.have & kQhScissor) && !Quad2DRowProblem(q).empty(), "SC TAP", "scissor on with no rect ever seen: no kQhScissor, RowProblem refuses (a hole, never unclipped)");
  q = Draw(m, nullptr, &tg);
  Check(!(q.have & kQhScissor), "SC TAP", "no thread scissor state at all: a hole");
  FakeMem no_state = m;
  no_state.words.erase(kObj + kStScissorEnable);
  no_state.words.erase(kR13);
  q = Draw(no_state, &sc, &tg);
  Check(!(q.have & kQhScissor), "SC TAP", "the state object unreadable: a hole");
  PutRect(m, 400, 300, 400, 300);
  OnOp79(sc, m, kRect);
  q = Draw(m, &sc, &tg);
  Check((q.have & kQhScissor) && q.scissor_on == 1 && !q.scissor[2] && !q.scissor[3] && Quad2DRowProblem(q).empty(), "SC TAP", "a zero-area game scissor is a valid row with width and height 0 (the drawer skips it)");
  Check(!OnOp79(sc, m, 0) && !sc.known, "SC TAP", "an unreadable op-79 record makes the thread's rect unknown");
}
static void TargetChecks() {
  FakeMem m;
  PutVerts(m);
  PutState(m, 0);
  PutEngine(m, kSurf + 64);   // the default back buffer: the 1280x720 surface at kSurf + 64
  Scissor sc;
  TargetState tg;
  Counters cc;
  Quad2D q = Draw(m, &sc, &tg);
  Check(!(q.have & kQhTarget) && !Quad2DRowProblem(q).empty(), "SC TARGET", "no SetRenderTarget seen: the row has no target state (a hole)");
  PutSurface(m, kSurf, 512, 512);
  SetRt(tg, m, 0, kSurf, &cc);
  q = Draw(m, &sc, &tg);
  Check((q.have & kQhTarget) && q.pass == kQpOffscreen && q.target_size[0] == 512 && q.target_size[1] == 512 && q.viewport[2] == 512 && q.viewport[3] == 512 && Quad2DRowProblem(q).empty(), "SC TARGET",
        "an offscreen page (the match HUD's 512x512 target) reads OFFSCREEN, viewport 0,0,512,512");
  PutSurface(m, kSurf + 64, 1280, 720);
  SetRt(tg, m, 0, kSurf + 64, &cc);
  q = Draw(m, &sc, &tg);
  Check((q.have & kQhTarget) && q.pass == kQpFinal && q.viewport[2] == 1280, "SC TARGET", "the default back buffer (1280x720, *(engine+232)) reads FRAME");
  PutSurface(m, kSurf + 128, 1280, 720);   // review F3: another 1280x720 surface (a scene-colour target) is NOT the frame
  SetRt(tg, m, 0, kSurf + 128, &cc);
  q = Draw(m, &sc, &tg);
  Check((q.have & kQhTarget) && q.pass == kQpOffscreen && q.target_size[0] == 1280 && q.target_size[1] == 720, "SC TARGET", "F3: a 1280x720 surface that is not the default back buffer reads OFFSCREEN (judged by identity, not size)");
  {   // the rig's classifier (TargetOfImage): the frame-buffer role image is the default; a 1280x720 scene-colour image is not
    rdr_adapter::SurfaceRoleEntry roles[2] = {};
    roles[0].image = 5, roles[0].role = rdr_adapter::SurfaceRole::FrameBuffer;
    roles[1].image = 7, roles[1].role = rdr_adapter::SurfaceRole::SceneColor;
    const q2d::rig::RigTarget fb = q2d::rig::TargetOfImage(5, 1280, 720, roles, 2), sceneC = q2d::rig::TargetOfImage(7, 1280, 720, roles, 2), nothing = q2d::rig::TargetOfImage(0, 1280, 720, roles, 2);
    Check(fb.known && fb.is_default && sceneC.known && !sceneC.is_default && !nothing.known, "SC TARGET", "F3 (rig): the FrameBuffer-role image is the default back buffer; a 1280x720 SceneColor image reads Offscreen; no image = unknown");
  }
  PutSurface(m, kSurf + 64, 1280, 720);
  PutRect(m, 10, 20, 300, 200);
  SetRt(tg, m, 0, kSurf + 64, &cc);
  OnSetViewport(tg, m, kRect);
  q = Draw(m, &sc, &tg);
  Check(q.viewport[0] == 10 && q.viewport[1] == 20 && q.viewport[2] == 300 && q.viewport[3] == 200, "SC TARGET", "a SetViewport record overrides the whole-surface default");
  SetRt(tg, m, 1, kSurf);   // another slot: the 2D target stays
  q = Draw(m, &sc, &tg);
  Check(q.pass == kQpFinal, "SC TARGET", "a SetRenderTarget of another slot does not change the colour target");
  OnResetRtDs(tg, m);   // op 47: back to the default target, whole-surface viewport
  q = Draw(m, &sc, &tg);
  Check((q.have & kQhTarget) && q.pass == kQpFinal && q.viewport[0] == 0 && q.viewport[2] == 1280, "SC RESET", "op 47 (reset to default): the default back buffer again, the viewport the whole surface");
  FakeMem none = m;
  none.words.erase(kEngineGlobal);
  TargetState tg2;
  Check(SetRt(tg2, none, 0, kSurf + 64) && !tg2.known, "SC TARGET", "the default back buffer unreadable: the target class is unknown (a hole), never a guess");
  Counters c2;
  SetRt(tg, m, 0, 0, &c2);   // a null slot-0 surface: unknown again
  q = Draw(m, &sc, &tg);
  Check(!(q.have & kQhTarget) && c2.rt_holder_null == 1, "SC RT HOLDER", "a holder with no surface in it at the record (filled later [U]) makes the target unknown and is counted (rt_holder_null)");
}
// Review F1: r5 of the op-45 recorder is the address of the slot holding the surface; the header is *r5.
static void HolderChecks() {
  FakeMem m;
  PutEngine(m, kSurf);
  PutSurface(m, kSurf, 1280, 720);
  TargetState tg;
  m.words[kHolder] = kSurf;
  const bool ok = OnSetRenderTarget(tg, m, 0, kHolder) && tg.known && tg.is_default && tg.w == 1280 && tg.h == 720;
  Check(ok, "SC RT HOLDER", "slot 0: the surface header is *r5 (the holder's word), its size word at *r5 + 36 gives 1280x720");
  TargetState direct;   // r5 taken as the surface itself would read kHolder + 36 (nothing there)
  OnSetRenderTarget(direct, m, 0, kHolder);
  Check(direct.known && direct.w == 1280, "SC RT HOLDER", "the holder is dereferenced once (not read as the surface header itself)");
}
// Review F2: every recorder whose executor reaches sub_82CABC68 invalidates the thread's scissor.
// PBS: a thread that never ran op 45 / 47 (the boot / title / main-menu screens draw into the default back buffer) is seeded from it at its first op-19, so its rows are not holes.
static void SeedChecks() {
  FakeMem m;
  PutVerts(m);
  PutState(m, 0);
  PutSurface(m, kSurf + 64, 1280, 720);
  PutEngine(m, kSurf + 64);
  Scissor sc;
  TargetState tg;
  Counters c;
  SeedTargetIfUnseen(tg, m, c);
  Quad2D q = Draw(m, &sc, &tg);
  Check((q.have & kQhTarget) && q.pass == kQpFinal && q.target_size[0] == 1280 && q.target_size[1] == 720 && q.viewport[2] == 1280 && q.viewport[3] == 720 && Quad2DRowProblem(q).empty() && c.tg_seeded == 1, "SC SEED",
        "op-19 with no prior op 45 / 47: the target is seeded from the default back buffer: a final 1280x720 row, no problem, tg_seeded 1");
  SeedTargetIfUnseen(tg, m, c);
  Check(c.tg_seeded == 1, "SC SEED", "seeded once: the next op-19 does not seed again");
  TargetState seen;   // a seen op with a null holder stays fail closed (never seeded over)
  Counters c2;
  SetRt(seen, m, 0, 0, &c2);
  SeedTargetIfUnseen(seen, m, c2);
  q = Draw(m, &sc, &seen);
  Check(!(q.have & kQhTarget) && c2.tg_seeded == 0 && c2.rt_holder_null == 1, "SC SEED", "a slot-0 op with a null holder is not seeded over (fail closed, counted rt_holder_null)");
  FakeMem none = m;   // the default surface unreadable: not seeded, counted, retried
  none.words.erase(kEngineGlobal);
  TargetState t3;
  Counters c3;
  SeedTargetIfUnseen(t3, none, c3);
  Check(!t3.known && !t3.seen_op && c3.tg_seeded == 0 && c3.tg_seed_failed == 1, "SC SEED", "the default surface unreadable: no seed, counted tg_seed_failed, left unseen (retried at the next op-19)");
  OnResetRtDs(t3, m);
  Check(t3.seen_op && t3.known && !t3.seeded, "SC SEED", "op 47 marks the thread as having seen a target op and clears the seeded flag");
  q = Draw(m, &sc, &tg);
  FakeMem unused = m;
  (void)unused;
  Check(tg.seeded, "SC SEED", "a seeded target carries the seeded flag");
  Counters c4;
  FillTarget(q, &tg, c4);
  Check(c4.tg_frame == 1 && c4.tg_frame_seeded == 1, "SC SEED", "a frame row under a seed counts in tg_frame_seeded");
  TargetState t5;   // a SetViewport seen before the first op-19 survives the seed
  PutRect(m, 10, 20, 300, 200);
  OnSetViewport(t5, m, kRect);
  Counters c5;
  SeedTargetIfUnseen(t5, m, c5);
  Check(t5.known && t5.vp[0] == 10 && t5.vp[2] == 300 && t5.vp[3] == 200, "SC SEED", "the seed keeps a viewport that was already set");
}
static void ResetChecks() {
  FakeMem m;
  PutVerts(m);
  PutState(m, 1);
  PutEngine(m, kSurf);
  PutSurface(m, kSurf, 1280, 720);
  TargetState tg;
  SetRt(tg, m, 0, kSurf);
  PutRect(m, 126, 402, 522, 566);
  Scissor sc;
  OnOp79(sc, m, kRect);
  Quad2D q = Draw(m, &sc, &tg);
  Check((q.have & kQhScissor) && q.scissor_on == 1, "SC RESET", "(setup) a scissor-on row with the rect known");
  m.words[kScissorTable + 8] = 65535, m.words[kScissorTable + 12] = 65535;
  if (SetRt(tg, m, 0, kSurf)) OnTargetResetTo(sc, m);
  q = Draw(m, &sc, &tg);
  Check((q.have & kQhScissor) && q.scissor_on == 1 && q.scissor[0] == 0 && q.scissor[2] > 1280, "SC RESET", "a slot-0 SetRenderTarget rewrites the device scissor from the default table (review 4: the whole surface, known): the row is not clipped to the old rect");
  OnOp79(sc, m, kRect);
  const bool slot1 = SetRt(tg, m, 1, kSurf);
  Check(!slot1 && sc.known, "SC RESET", "a slot-1 SetRenderTarget leaves the rect alone");
  OnResetRtDs(tg, m), OnTargetResetTo(sc, m);   // op 47
  q = Draw(m, &sc, &tg);
  Check((q.have & kQhScissor) && q.scissor[0] == 0 && q.scissor[2] > 1280, "SC RESET", "op 47 (reset to default): the scissor is the default table's whole surface");
  OnOp79(sc, m, kRect);
  OnTargetResetTo(sc, m);   // op 48 SetDepthStencil
  q = Draw(m, &sc, &tg);
  Check((q.have & kQhScissor) && q.scissor[0] == 0 && q.scissor[2] > 1280, "SC RESET", "op 48 (SetDepthStencil): the scissor is the default table's whole surface");
  FakeMem nt = m;
  nt.deny = kScissorTable;
  OnOp79(sc, m, kRect);
  OnTargetResetTo(sc, nt);
  q = Draw(m, &sc, &tg);
  Check(!sc.known && !(q.have & kQhScissor), "SC RESET", "the default scissor table unreadable: the scissor is unknown after a reset (a scissor-on row is a hole)");
  OnOp79(sc, m, kRect);
  q = Draw(m, &sc, &tg);
  Check((q.have & kQhScissor) && q.scissor_on == 1 && q.scissor[2] == 396, "SC RESET", "a fresh op-79 rect after the reset makes the row clipped again");
}
// S103 PFR (docs\research\s103\PFR.md section "Unit cases"): the game's per-frame target rule. Every replay starts on the default target (a thread's first tap op of a new guest frame resets target + scissor), op 46 saves the thread's CURRENT
// target under its holder and an op-45 restore of that holder takes the save (the guest's *holder is empty or stale at record time), op 73 binds the scene colour surface and op 74 / op 47 go back to the default.
static const uint32_t kPage = 0x40009000u, kHolder2 = 0x40008100u, kScene = 0x4000A000u, kSceneSurf = 0x4000A400u;
// The device's default scissor table (sub_82CABC68): 0, 0, 65535, 65535 = the whole surface.
static void PutScissorTable(FakeMem& m) { m.words[kScissorTable] = 0, m.words[kScissorTable + 4] = 0, m.words[kScissorTable + 8] = 65535, m.words[kScissorTable + 12] = 65535; }
static void FrameChecks() {
  FakeMem m;
  PutVerts(m);
  PutState(m, 0);
  PutEngine(m, kSurf);
  PutScissorTable(m);
  PutSurface(m, kSurf, 1280, 720);      // the default back buffer
  PutSurface(m, kPage, 512, 512);       // an offscreen page
  PutSurface(m, kSceneSurf, 1280, 720); // the scene colour surface (a 1280x720 non-default target)
  m.words[kHolder2] = kPage;            // a page holder as the game fills it for a plain op 45
  Scissor sc;
  TargetState tg;
  Saves sv;
  Counters c;
  uint32_t tf = 0xFFFFFFFFu;
  // 7. a fresh thread whose first tap op is mid-frame: the default target (replaces the once-per-thread seed)
  Check(OnFrameBoundary(tf, 10, tg, sc, m, &c) && c.frame_resets == 1, "SC FRAME", "case 7: a fresh thread's first op of a frame resets it (frame_resets 1)");
  Quad2D q = Draw(m, &sc, &tg);
  Check((q.have & kQhTarget) && q.pass == kQpFinal && q.viewport[2] == 1280 && Quad2DRowProblem(q).empty(), "SC FRAME", "case 7: a fresh thread draws into the default back buffer: kQpFinal, whole viewport (no op 45 / 47 ever seen)");
  Check(!OnFrameBoundary(tf, 10, tg, sc, m, &c) && c.frame_resets == 1, "SC FRAME", "the same frame's next op does not reset again");
  // 1. op45(0, page) in frame N; the first row of N+1 before any target op is the default again
  OnSetRenderTarget(tg, m, 0, kHolder2, &c, &sv, tf);
  q = Draw(m, &sc, &tg);
  Check(q.pass == kQpOffscreen && q.target_size[0] == 512, "SC FRAME", "case 1 (setup): op45(0, page) in frame N: the row is offscreen");
  OnFrameBoundary(tf, 11, tg, sc, m, &c);
  q = Draw(m, &sc, &tg);
  Check((q.have & kQhTarget) && q.pass == kQpFinal && q.viewport[2] == 1280 && q.viewport[3] == 720, "SC FRAME", "case 1: the first row of frame N+1 before any target op is kQpFinal, viewport the whole default surface (not last frame's page)");
  // 2. the op-79 rect of frame N is gone in N+1: the game's frame start rewrites the scissor from the default table (review 4: known, the whole surface)
  PutState(m, 1);
  PutRect(m, 126, 402, 522, 566);
  OnOp79(sc, m, kRect);
  q = Draw(m, &sc, &tg);
  Check((q.have & kQhScissor) && q.scissor_on == 1 && q.scissor[0] == 126, "SC FRAME", "case 2 (setup): a rect in frame N clips a scissor-on row");
  OnFrameBoundary(tf, 12, tg, sc, m, &c);
  q = Draw(m, &sc, &tg);
  Check((q.have & kQhScissor) && q.scissor_on == 1 && q.scissor[0] == 0 && q.scissor[1] == 0 && q.scissor[2] > 1280 && Quad2DRowProblem(q).empty(), "SC FRAME", "case 2 / review 4: a scissor-enabled row of frame N+1 before any op-79 has the default table's whole-surface box (known, never last frame's rect)");
  {
    FakeMem nt = m;
    nt.deny = kScissorTable;
    Scissor s2;
    s2.known = true;
    uint32_t tf2 = 0xFFFFFFFFu;
    TargetState t2;
    OnFrameBoundary(tf2, 1, t2, s2, nt, &c);
    q = Draw(nt, &s2, &t2);
    Check(!s2.known && !(q.have & kQhScissor), "SC FRAME", "review 4: the default scissor table unreadable: the scissor stays unknown (a scissor-on row is a hole)");
  }
  PutState(m, 0);
  q = Draw(m, &sc, &tg);
  Check((q.have & kQhScissor) && q.scissor_on == 0, "SC FRAME", "case 2: a scissor-off row of the new frame needs no rect");
  // 3. save / restore with a null holder (the holder is 0 at record: the game fills it at replay)
  OnFrameBoundary(tf, 20, tg, sc, m, &c);
  Counters c3;
  OnGetRenderTarget(sv, tg, 0, kHolder, &c3, tf);                // op 46 (H) while on the default; *H == 0
  OnSetRenderTarget(tg, m, 0, kHolder2, &c3, &sv, tf);            // op 45(0, page)
  q = Draw(m, &sc, &tg);
  const bool off = q.pass == kQpOffscreen;
  OnSetRenderTarget(tg, m, 0, kHolder, &c3, &sv, tf);             // op 45(0, H): restore
  q = Draw(m, &sc, &tg);
  Check(off && (q.have & kQhTarget) && q.pass == kQpFinal && c3.rt_holder_null == 0 && c3.restore_from_save == 1 && c3.saves_made == 1, "SC SAVE", "case 3: op46(H), op45(page), a row offscreen, op45(0, H) with *H == 0: the row is kQpFinal, rt_holder_null 0, restore_from_save 1");
  // 4 (review 1). *H names a different surface than the save (a stale page): fail closed, counted; *H equal to the save: the save; *H null: the save (case 3)
  OnFrameBoundary(tf, 21, tg, sc, m, &c);
  m.words[kHolder] = kPage;
  Counters c4;
  OnGetRenderTarget(sv, tg, 0, kHolder, &c4, tf);                // saved while on the default
  OnSetRenderTarget(tg, m, 0, kHolder2, &c4, &sv, tf);
  OnSetRenderTarget(tg, m, 0, kHolder, &c4, &sv, tf);
  q = Draw(m, &sc, &tg);
  Counters c4b;
  FillTarget(q, &tg, c4b);
  Check(!(q.have & kQhTarget) && c4.restore_disagree == 1 && c4.restore_from_save == 0 && c4b.tg_unknown_disagree == 1, "SC SAVE", "review 1: *H holds a page, the save is the default: a disagreement is fail closed (unknown, restore_disagree 1, counted as tg_unknown_disagree)");
  m.words[kHolder] = kSurf;   // *H = the default = what the save holds: no disagreement
  OnFrameBoundary(tf, 22, tg, sc, m, &c);
  OnSetRenderTarget(tg, m, 0, kHolder2, &c4, &sv, tf);
  OnSetRenderTarget(tg, m, 0, kHolder, &c4, &sv, tf);
  q = Draw(m, &sc, &tg);
  Check((q.have & kQhTarget) && q.pass == kQpFinal && c4.restore_from_save == 1 && c4.restore_disagree == 1, "SC SAVE", "review 1: *H equal to the saved surface restores the save");
  m.words.erase(kHolder);
  // review 2: the restore puts the viewport back to the whole surface
  OnFrameBoundary(tf, 23, tg, sc, m, &c);
  OnSetRenderTarget(tg, m, 0, kHolder2, &c, &sv, tf);            // on the page
  PutRect(m, 10, 20, 100, 60);
  OnSetViewport(tg, m, kRect);                                    // a smaller viewport
  OnGetRenderTarget(sv, tg, 0, kHolder, &c, tf);
  OnFrameBoundary(tf, 24, tg, sc, m, &c);
  OnSetRenderTarget(tg, m, 0, kHolder, &c, &sv, tf);
  Check(tg.known && tg.vp_known && tg.vp[0] == 0 && tg.vp[1] == 0 && tg.vp[2] == 512 && tg.vp[3] == 512, "SC SAVE", "review 2: a restore takes the whole-surface viewport (SetRT resets it), not the save's smaller one");
  // 5. cross-frame save (the CAS composite saves in frame N and restores in N+2): the frame reset does not clear the saves
  OnFrameBoundary(tf, 30, tg, sc, m, &c);
  OnSetRenderTarget(tg, m, 0, kHolder2, &c, &sv, tf);             // on page P
  OnGetRenderTarget(sv, tg, 0, kHolder, &c, tf);                  // op 46(H) saves P
  OnFrameBoundary(tf, 32, tg, sc, m, &c);                         // two frames later: the default again
  q = Draw(m, &sc, &tg);
  const bool dflt = q.pass == kQpFinal;
  OnSetRenderTarget(tg, m, 0, kHolder, &c, &sv, tf);              // op 45(0, H)
  q = Draw(m, &sc, &tg);
  Check(dflt && q.pass == kQpOffscreen && q.target_size[0] == 512, "SC SAVE", "case 5: op46(H) on page P in frame N, op45(0, H) in N+2: the target is P (the reset does not clear the saves)");
  // review 5: an op 46 of another slot never touches the slot-0 entry; an op-58 release erases it; an old save is ignored
  OnFrameBoundary(tf, 40, tg, sc, m, &c);
  Counters c5;
  OnSetRenderTarget(tg, m, 0, kHolder2, &c5, &sv, tf);
  OnGetRenderTarget(sv, tg, 0, kHolder, &c5, tf);                 // slot 0 saved: page P
  OnFrameBoundary(tf, 41, tg, sc, m, &c);
  OnGetRenderTarget(sv, tg, 1, kHolder, &c5, tf);                 // slot 1 (tg = the default here): ignored
  OnSetRenderTarget(tg, m, 0, kHolder, &c5, &sv, tf);
  Check(tg.known && !tg.is_default && c5.saves_made == 1 && c5.restore_from_save == 1, "SC SAVE", "review 5: op46(slot 1, H) does not touch the slot-0 save under H (still page P)");
  OnReleaseHolder(sv, kHolder, &c5);                              // op 58
  OnSetRenderTarget(tg, m, 0, kHolder, &c5, &sv, tf);             // *H == 0 and no save: null holder, counted
  Check(!tg.known && c5.saves_released == 1 && c5.rt_holder_null == 1 && c5.restore_from_save == 1, "SC SAVE", "review 5: an op-58 release erases the holder's save (the next restore reads the guest's empty holder)");
  OnSetRenderTarget(tg, m, 0, kHolder2, &c5, &sv, tf);
  OnGetRenderTarget(sv, tg, 0, kHolder, &c5, 100);
  OnSetRenderTarget(tg, m, 0, kHolder, &c5, &sv, 100 + Saves::kMaxAge + 1);   // too old: ignored, falls to the (null) memory holder
  Check(!tg.known && c5.saves_expired == 1 && c5.rt_holder_null == 2, "SC SAVE", "review 5: a save older than 300 guest frames is ignored (counted saves_expired)");
  // 6. op 73 / op 74 (through OnSceneBeginOp / OnSceneEndOp, which the tap's hooks call)
  OnFrameBoundary(tf, 50, tg, sc, m, &c);
  m.words[kEngine + 292] = 0, m.words[kEngine + 312] = 1, m.words[kEngine + 316] = kScene, m.words[kScene + 272] = kSceneSurf, m.words[kScene + 280] = 1;
  Counters c6;
  PutRect(m, 126, 402, 522, 566);
  OnOp79(sc, m, kRect);
  OnSceneBeginOp(tg, sc, m, &c6);
  q = Draw(m, &sc, &tg);
  FillTarget(q, &tg, c6);
  Check(tg.is_scene && q.pass == kQpSceneColour && q.target_size[0] == 1280 && c6.tg_scene == 1 && c6.tg_offscreen == 0 && c6.scene_begins == 1 && Quad2DRowProblem(q).empty(), "SC SCENE", "case 6: op 73 with +292 == 0, +312 set, scene +280 set binds the scene colour surface: the row is kQpSceneColour (own census tg_scene 1, not tg_offscreen), a valid row");
  Check(sc.known && sc.rect[0] == 0 && sc.rect[2] == 65535, "SC SCENE", "op 73 rewrites the scissor from the default table");
  OnSceneEndOp(tg, sc, m, &c6);   // op 74 = like op 47
  q = Draw(m, &sc, &tg);
  Check(!tg.is_scene && q.pass == kQpFinal, "SC SCENE", "case 6: op 74 goes back to the default: the next row is kQpFinal");
  {   // op 74 changes nothing with no scene (+312 == 0), or with a custom band layout (+296 > 0 and +300 > 0: sub_82C22650 skips the bind back)
    OnSetRenderTarget(tg, m, 0, kHolder2, &c6, &sv, tf);   // on the page
    OnSceneEndOp(tg, sc, m, &c6);
    Check(tg.is_default, "SC SCENE", "(setup) op 74 with a scene goes back to the default");
    OnSetRenderTarget(tg, m, 0, kHolder2, &c6, &sv, tf);
    m.words[kEngine + 312] = 0;
    OnSceneEndOp(tg, sc, m, &c6);
    Check(!tg.is_default && tg.surface == kPage, "SC SCENE", "review 3: op 74 with [engine+312] == 0 changes nothing");
    m.words[kEngine + 312] = 1, m.words[kScene + 296] = 2, m.words[kScene + 300] = 3;
    OnSceneEndOp(tg, sc, m, &c6);
    Check(!tg.is_default && tg.surface == kPage, "SC SCENE", "review 3: op 74 with scene +296 > 0 and +300 > 0 (custom band layout) skips the bind back");
    m.words[kScene + 300] = 0;
    OnSceneEndOp(tg, sc, m, &c6);
    Check(tg.is_default, "SC SCENE", "review 3: +296 > 0 but +300 == 0 still binds the default");
  }
  m.words[kEngine + 312] = 0;
  const TargetState before = tg;
  Check(!OnSceneBegin(tg, m, &c6) && tg.surface == before.surface && !tg.is_scene, "SC SCENE", "case 6: op 73 with +312 == 0 changes nothing");
  m.words[kEngine + 312] = 1, m.words[kEngine + 292] = 5;
  Check(!OnSceneBegin(tg, m, &c6) && !tg.is_scene, "SC SCENE", "case 6: op 73 with +292 != 0 changes nothing");
  m.words[kEngine + 292] = 0, m.words[kScene + 280] = 0;
  Check(!OnSceneBegin(tg, m, &c6) && !tg.is_scene, "SC SCENE", "review 3: op 73 with scene +280 == 0 changes nothing (sub_82C224A0 binds only when set)");
  {   // the unknown-by-reason labels: default unreadable vs surface unreadable; a frame start that could not read the default
    FakeMem nd = m;
    nd.words.erase(kEngineGlobal);
    TargetState d1;
    OnResetRtDs(d1, nd);
    Check(d1.why == 3 && !d1.known, "SC FRAME", "default surface unreadable is labelled default-unreadable (3), not surface-unreadable");
    Scissor s2;
    uint32_t tf2 = 0xFFFFFFFFu;
    TargetState t2;
    Counters c7;
    OnFrameBoundary(tf2, 5, t2, s2, nd, &c7);
    Check(c7.frame_reset_failed == 1 && c7.frame_resets == 1 && t2.why == 3, "SC FRAME", "a frame start that cannot read the default counts frame_reset_failed and says default-unreadable");
  }
  // the unknown-by-reason split (a thread that never had a target; a null holder)
  TargetState fresh;
  Counters c9;
  Quad2D u = Draw(m, &sc, &fresh);
  FillTarget(u, &fresh, c9);
  OnSetRenderTarget(fresh, m, 0, kHolder + 0x800, &c9);
  FillTarget(u, &fresh, c9);
  Check(c9.tg_unknown == 2 && c9.tg_unknown_never == 1 && c9.tg_unknown_holder == 1, "SC FRAME", "FillTarget splits the holes by reason (never set 1, holder null 1)");
  Quad2D rowp;
  ValidSC(rowp);
  rowp.pass = kQpOther;
  Check(!Quad2DRowProblem(rowp).empty(), "SC SCENE", "a kQpOther row is still refused (only Final / Offscreen / SceneColour are valid)");
}
static void Record() {
  Quad2D q;
  q.kind = kQkRect, q.site = kSiteRect, q.seq = 1, q.frame = 7, q.nverts = 4, q.stride_bytes = 12;
  q.have = kQhVerts | kQhPosScale | kQhColour | kQhProj | kQhBlend | kQhAlphaTest;
  q.blend = kQbAlpha;
  q.pos_scale[0] = 2.0f / 1280, q.pos_scale[1] = 2.0f / 720, q.color[3] = 1.f;
  ValidSC(q);
  q.scissor_on = 1, q.scissor[0] = 126, q.scissor[1] = 402, q.scissor[2] = 396, q.scissor[3] = 164;
  SceneTables t;
  t.has_camera = true;
  Quad2D b = q;
  b.seq = 2, b.pass = kQpOffscreen, b.target_size[0] = b.target_size[1] = 512, b.viewport[2] = b.viewport[3] = 512;
  t.quads2d = {q, b};
  std::vector<uint8_t> by = SerializeRecord(t);
  SceneTables back;
  std::string e;
  const bool ok = !by.empty() && ParseRecord(by.data(), by.size(), back, e) && back.quads2d.size() == 2 && back.quad2d_errors == 0 && std::memcmp(&back.quads2d[0], &q, sizeof(Quad2D)) == 0 && std::memcmp(&back.quads2d[1], &b, sizeof(Quad2D)) == 0;
  Check(sizeof(Quad2D) == 324 && kQuad2DLayout == 5 && ok, "SC RECORD", "layout 5, 324 bytes a row: a scissored frame row and an offscreen row round-trip byte-equal");
  Quad2D old = q;
  old.version = 4;
  Check(!Quad2DRowProblem(old).empty(), "SC RECORD", "a layout-4 row is refused (fail closed)");
  Quad2D a = q;
  a.scissor[2] = -1;
  Check(!Quad2DRowProblem(a).empty(), "SC RECORD", "a negative scissor size is refused");
  a = q, a.scissor_on = 0;
  Check(!Quad2DRowProblem(a).empty(), "SC RECORD", "a scissor rect set with the scissor off is refused");
  a = q, a.scissor_on = 2;
  Check(!Quad2DRowProblem(a).empty(), "SC RECORD", "scissor_on above 1 is refused");
  a = q, a.have &= ~uint32_t(kQhScissor);
  Check(!Quad2DRowProblem(a).empty(), "SC RECORD", "no scissor state: refused");
  a = q, a.have &= ~uint32_t(kQhTarget);
  Check(!Quad2DRowProblem(a).empty(), "SC RECORD", "no target state: refused");
  a = q, a.pass = kQpUnknown;
  Check(!Quad2DRowProblem(a).empty(), "SC RECORD", "a target class of unknown is refused");
}
static void DrawChecks() {
  Quad2D q;
  ValidSC(q);
  rdr::Rect r{};
  Check(!ScissorOf(q, 1280, 720, r) && r.width == 1280 && r.height == 720, "SC DRAW", "scissor off: no clip (the whole target)");
  q.scissor_on = 1, q.scissor[0] = 126, q.scissor[1] = 402, q.scissor[2] = 396, q.scissor[3] = 164;
  Check(ScissorOf(q, 1280, 720, r) && r.x == 126 && r.y == 402 && r.width == 396 && r.height == 164, "SC DRAW", "1x: the box is the row's (126,402 396x164), top-left origin, nothing flipped");
  Check(ScissorOf(q, 1920, 1080, r) && r.x == 189 && r.y == 603 && r.width == 594 && r.height == 246, "SC DRAW", "1.5x: the box scales with the target");
  q.scissor[0] = 1200, q.scissor[2] = 400;
  Check(ScissorOf(q, 1280, 720, r) && r.x == 1200 && r.width == 80, "SC DRAW", "a box past the right edge is clamped to the target");
  q.scissor[0] = 2000;
  Check(ScissorOf(q, 1280, 720, r) && r.width == 0, "SC DRAW", "a box outside the target is empty");
  q.scissor[0] = 10, q.scissor[2] = 0;
  Check(ScissorOf(q, 1280, 720, r) && (r.width == 0 || r.height == 0), "SC DRAW", "a zero-area game scissor gives an empty box: Append SKIPS the row (counted in Report::scissor_empty), never draws it whole");
  {   // review F4: the box is in the row's own target pixels (an offscreen page is not 1280x720) and is clipped to the row's viewport
    Quad2D p;
    ValidSC(p);
    p.pass = kQpOffscreen, p.target_size[0] = p.target_size[1] = 512, p.viewport[2] = p.viewport[3] = 512;
    p.scissor_on = 1, p.scissor[0] = 0, p.scissor[1] = 0, p.scissor[2] = 256, p.scissor[3] = 256;
    Check(ScissorOf(p, 1024, 1024, r) && r.x == 0 && r.y == 0 && r.width == 512 && r.height == 512, "SC DRAW", "F4: a 512x512 page's box 0,0,256,256 drawn into 1024x1024 is 0,0,512,512 (scaled by the row's target size, not by 1280x720)");
    Quad2D v;
    ValidSC(v);
    v.scissor_on = 1, v.scissor[0] = 100, v.scissor[1] = 100, v.scissor[2] = 800, v.scissor[3] = 500;
    v.viewport[0] = 0, v.viewport[1] = 0, v.viewport[2] = 640, v.viewport[3] = 360;
    Check(ScissorOf(v, 1280, 720, r) && r.x == 100 && r.y == 100 && r.width == 540 && r.height == 260, "SC DRAW", "F4: the box is intersected with the row's viewport (the game clips to viewport ∩ rect)");
    Quad2D w;
    ValidSC(w);
    w.viewport[2] = 640, w.viewport[3] = 360;
    Check(ScissorOf(w, 1280, 720, r) && r.x == 0 && r.y == 0 && r.width == 640 && r.height == 360, "SC DRAW", "F4: scissor off + a smaller viewport still clips to the viewport");
  }
  {   // review F5: Check counts the zero-area scissor rows Append skips
    FakeMem m;
    PutVerts(m);
    PutEngine(m, kSurf);
    PutSurface(m, kSurf, 1280, 720);
    PutState(m, 1);
    TargetState tg;
    SetRt(tg, m, 0, kSurf);
    Scissor sc;
    PutRect(m, 400, 300, 400, 300);
    OnOp79(sc, m, kRect);
    Quad2D zero = Draw(m, &sc, &tg);
    PutRect(m, 0, 0, 1280, 720);
    OnOp79(sc, m, kRect);
    Quad2D whole = Draw(m, &sc, &tg);
    for (Quad2D* row : {&zero, &whole}) {
      row->have |= kQhPosScale | kQhColour | kQhProj;
      row->pos_scale[0] = 2.0f / 1280, row->pos_scale[1] = 2.0f / 720, row->color[3] = 1.0f;
    }
    Context ctx;
    ctx.width = 1280, ctx.height = 720;
    const Quad2D rows[2] = {zero, whole};
    const Report rep = q2d::Check(ctx, rows, 2);
    Check(rep.drawn == 1 && rep.scissor_empty == 1 && rep.refused_total() == 0, "SC DRAW", "F5: Check counts the zero-area scissor row as skipped (scissor_empty 1, drawn 1), the same decision Append makes");
  }
}
// The dev cross-check: row i against the i-th row-site stream draw (target class, target size, scissor ∩ viewport).
static void CrossChecks() {
  StreamOp ops[2];
  for (StreamOp& o : ops) o.kind = 0, o.op19 = true, o.site = kSiteRect, o.target = 9;   // two row-site draws into frame image 9
  ops[0].scissor[0] = 126, ops[0].scissor[1] = 402, ops[0].scissor[2] = 396, ops[0].scissor[3] = 164;
  ops[1].scissor[0] = 0, ops[1].scissor[1] = 0, ops[1].scissor[2] = 1280, ops[1].scissor[3] = 720;
  Quad2D a, b;
  ValidSC(a), ValidSC(b);
  a.scissor_on = 1, a.scissor[0] = 126, a.scissor[1] = 402, a.scissor[2] = 396, a.scissor[3] = 164;
  CrossCheck ok = CrossCheckStream(ops, 2, 9, 1280, 720, std::vector<Quad2D>{a, b}.data(), 2);
  Check(ok.pairs == 2 && !ok.target_mismatch && !ok.scissor_mismatch && !ok.size_mismatch, "SC CROSS", "rows that match their stream draws (a clipped list, a whole-target row): no mismatch");
  Quad2D off = b;
  off.pass = kQpOffscreen;   // the row says offscreen, the stream draw rendered into the frame image
  CrossCheck bad = CrossCheckStream(ops, 2, 9, 1280, 720, std::vector<Quad2D>{a, off}.data(), 2);
  Check(bad.target_mismatch == 1, "SC CROSS", "a row classed offscreen whose stream draw renders into the frame image is a target mismatch");
  Quad2D wide = a;
  wide.scissor[2] = 1000;
  CrossCheck sc = CrossCheckStream(ops, 2, 9, 1280, 720, std::vector<Quad2D>{wide, b}.data(), 2);
  Check(sc.scissor_mismatch == 1 && !sc.target_mismatch, "SC CROSS", "a row whose scissor differs from the stream draw's window scissor is a scissor mismatch");
  Quad2D vp = b;
  vp.viewport[2] = 640, vp.viewport[3] = 360;
  ops[1].scissor[2] = 640, ops[1].scissor[3] = 360;
  CrossCheck v = CrossCheckStream(ops, 2, 9, 1280, 720, std::vector<Quad2D>{a, vp}.data(), 2);
  Check(!v.scissor_mismatch, "SC CROSS", "the row's box is its scissor ∩ viewport: a smaller viewport matches the stream's window scissor");
}
static void All() { Tap(), TargetChecks(), HolderChecks(), SeedChecks(), ResetChecks(), FrameChecks(), Record(), DrawChecks(), CrossChecks(); }

int main(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; ++i)
    if (std::string(argv[i]) == "--plant") {
      const std::string p = argv[i + 1];
      const std::map<std::string, int> plants = {{"scissor_not_filled", 38}, {"scissor_enable_ignored", 39}, {"scissor_y_flipped", 40}, {"scissor_ignored", 41}, {"scissor_y_flipped_draw", 42}, {"target_not_filled", 43}, {"target_always_frame", 44}, {"rt_holder_direct", 45}, {"reset_ignored", 46}, {"final_by_size", 47}, {"scissor_viewport_ignored", 48}, {"scissor_root_fixed", 49}, {"check_empty_ignored", 50}, {"cross_target_ignored", 51}, {"seed_off", 52}, {"frame_reset_skipped", 53}, {"save_ignored", 54}};
      const std::map<std::string, std::string> expect = {{"scissor_not_filled", "[SC TAP]"}, {"scissor_enable_ignored", "[SC TAP]"}, {"scissor_y_flipped", "[SC TAP]"}, {"scissor_ignored", "[SC DRAW]"},
                                                         {"scissor_y_flipped_draw", "[SC DRAW]"}, {"target_not_filled", "[SC TARGET]"}, {"target_always_frame", "[SC TARGET]"}, {"rt_holder_direct", "[SC RT HOLDER]"}, {"reset_ignored", "[SC RESET]"},
                                                         {"final_by_size", "[SC TARGET]"}, {"scissor_viewport_ignored", "[SC DRAW]"}, {"scissor_root_fixed", "[SC DRAW]"}, {"check_empty_ignored", "[SC DRAW]"}, {"cross_target_ignored", "[SC CROSS]"}, {"seed_off", "[SC SEED]"}, {"frame_reset_skipped", "[SC FRAME]"}, {"save_ignored", "[SC SAVE]"}};
      auto it = plants.find(p);
      if (it == plants.end()) return std::printf("unknown plant %s\n", p.c_str()), 2;
      q2d::g_plant = it->second;
      All();
      const bool caught = g_failed.find(expect.at(p)) != std::string::npos;
      std::printf("plant %s: %s (failed: %s)\n", p.c_str(), caught ? "CAUGHT" : "NOT CAUGHT", g_failed.empty() ? "-" : g_failed.c_str());
      return caught ? 0 : 1;
    }
  All();
  std::printf("%d checks, %d failed%s\n", g_checks, g_failures, g_failed.empty() ? "" : (" " + g_failed).c_str());
  return g_failures ? 1 : 0;
}
