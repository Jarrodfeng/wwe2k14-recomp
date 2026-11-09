// wwe2k14_assets.exe: the asset viewer. Window (optional), renderer (D3D11 or Vulkan), orbit camera, shots, command pipe.
// Args: --game PATH, --pipe NAME, --script FILE, --exit | --exit-after-script, --hidden, --size WxH, --parent PID, --guest_image PATH
// (exit when that process ends), --backend d3d11|vk (Session 98 phone port: vk draws with the Vulkan renderer in an SDL3 window).
// Windowed with no --script: browse the game's arenas / wrestlers with the keyboard.
// Pipe commands: see Execute(); Session 97 added {"cmd":"crowd","act":14005,"frame":N,"pose":"game"|"stored"} (crowd pose from m2.pac).
// Session 98 phone port: no window-system code here (platform.h); what is left of Win32 (the command line, the parent watch, the
// named pipe in pipe_server.h) is behind _WIN32.
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <map>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#define STBI_ONLY_PNG
#include "stb_image.h"  // (declarations only: the seam holds the implementation; src\assets\pac.h needs the zlib decoder)

#include "../belt_pose.h"
#include "../evt_props.h"
#include "../stance_table.h"
#include "../motion.h"
#include "../particles.h"
#include "../part_names.h"
#include "../spots.h"
#include "../stage_pools.h"
#include "../stage_media.h"
#include "camera.h"
#include "freecam.h"
#include "json_lite.h"
#include "light_cmds.h"
#include "loader_seam.h"
#include "pick.h"
#include "pipe_server.h"
#include "campath.h"
#include "render_job.h"
#include "renderer.h"
#include "scene.h"
#include "ui.h"
#include "ui_mods.h"
#include "ui_importer.h"  // Session 98 (Superstar Importer)
#include "ui_light.h"     // Session 98: LauncherSettingsFile (the LIGHT tab's USE MY LIGHTING)
#include "shot.h"
#include "../mods_lookup.h"
#include "../char_mods.h"
#include "tron_video.h"
#include "platform.h"
#ifdef WWE_VIEWER_VK
#include "renderer_vk.h"  // Session 98 phone port (stage C): SurfaceGone / SurfaceBack (Android background / foreground)
#endif
#ifdef __ANDROID__
#include <SDL3/SDL_main.h>  // Session 98 phone port (stage C): main() becomes SDL_main, which ViewerActivity (libviewer.so) calls
#endif

using namespace view;

static double NowMs() {
  using namespace std::chrono;
  return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}
static std::string Num(double v) {
  char b[64];
  snprintf(b, sizeof b, "%.4g", v);
  return b;
}
static std::string Fix(double v, int d = 3) {
  char b[64];
  snprintf(b, sizeof b, "%.*f", d, v);
  return b;
}
static std::string Err(const std::string& m) { return "{\"ok\":false,\"error\":\"" + jl::Esc(m) + "\"}"; }
#ifdef _WIN32
static std::string Utf8(const wchar_t* w) {
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return "";
  std::string s((size_t)n - 1, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
  return s;
}
#endif
static void SleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
// A JSON number to int without undefined behaviour on huge values.
static int IntArg(double v) { return (int)std::max(-2e9, std::min(2e9, v)); }

struct Viewer : InputSink {
  explicit Viewer(std::unique_ptr<Renderer> renderer) : rp(std::move(renderer)), r(*rp) {}
  std::unique_ptr<Renderer> rp;  // Session 98 phone port: the backend (--backend d3d11 | vk)
  Renderer& r;
  std::unique_ptr<Platform> plat;  // the window (null = --hidden)
  ui::Ui ui;  // Session 98: the Arena Viewer UI (windowed only; never created with --hidden)
  Camera cam;
  std::string game, pipe_name = "wwe2k14_assets";
  bool hidden = false, quit = false;
  int exit_code = 0;
  // in-window browsing (windowed, no --script)
  bool browse = false, browse_arena = true;
  std::vector<int> browse_ids;
  size_t browse_idx = 0;
  int browse_attire = 0;
  std::string status, shown_title;
  int w = 1280, h = 720;
  // scene totals
  std::string label;
  size_t meshes = 0, vertices = 0, triangles = 0, textures = 0;
  std::vector<std::string> warnings;
  int last_arena = -1;  // the arena of the last plain arena load (the crowd command re-loads it)
  std::string last_media;  // the stage command's media folder (the mods commands re-stage with it)
  int stage_wrestler = -1;  // the wrestler whose entrance look (tron + MVSW / MVSB) the stage shows (-1 = the match look)
  // Session 98 (tron video, tron_video.h): the stage's tron slots play the cache's MP4s. Clock = the entrance clock, else tron.clock.
  TronVideo tron;
  int tron_wr = -2;
  double tron_last_ms = 0, tron_rend_base = 0, tron_rend_ent0 = 0, tron_rend_t0 = 0;
  void TronNow(bool sync, double t_override = -1) {
    if (stage_wrestler != tron_wr) tron_wr = stage_wrestler, tron.clock = 0;
    if (!tron.on) return;
    tron.entrance_clock = ent.active;  // (renders pass the entrance clock as t_override too)
    tron.Update(r, stage_wrestler, last_arena, last_media, t_override >= 0 ? t_override : ent.active ? tron.EntranceVideoTime(ent.t) : tron.clock, sync);
  }
  void TronTick() {  // windowed main loop: the preview clock runs in real time; the worker decodes toward it
    const double now = NowMs();
    const double dt = tron_last_ms > 0 ? std::min(100.0, now - tron_last_ms) : 0;
    tron_last_ms = now;
    if (!ent.active && tron.play) tron.clock += dt / 1000.0;
    if (!job.Active()) TronNow(false);
  }
  std::string DoTron(const std::string& c, const jl::Value& cmd) {  // {"cmd":"tron_time","t":s} / {"cmd":"tron_play","play":bool,"t":s}
    if (stage_wrestler != tron_wr) tron_wr = stage_wrestler, tron.clock = 0;  // (a new stage look starts its video at 0; then t applies)
    if (c == "tron_offset") tron.offset = cmd.num("offset", 0);  // seconds of entrance clock at which the video is at 0
    if (cmd.has("t")) tron.clock = std::max(0.0, cmd.num("t", 0));
    if (c == "tron_play") {
      const jl::Value* pv = cmd.get("play");
      tron.play = pv ? (pv->t == jl::Value::Bool ? pv->b : pv->n != 0) : true;
    }
    TronNow(true);
    return "{\"ok\":true,\"clock\":" + Fix(tron.clock, 3) + ",\"play\":" + (tron.play ? "true" : "false") + ",\"entrance_clock\":" + (ent.active ? "true" : "false") +
           ",\"t\":" + Fix(ent.active ? tron.EntranceVideoTime(ent.t) : tron.clock, 3) + ",\"video\":" + (tron.on ? "true" : "false") + ",\"layers\":" + tron.Info() + "}";
  }
  // timing
  double frame_ms = 0, last_shot_ms = 0;
  // Session 97 (motions): the skeletons of the shown models (parallel to the renderer's models), the motion packs read so
  // far, the chosen motion and its pose settings.
  std::vector<std::vector<assets::Bone>> model_bones;
  std::map<std::string, std::unique_ptr<assets::motion::Pack>> packs;
  assets::motion::Motion cur_motion;
  bool have_motion = false, pose_motion = false, playing = false;
  double frame = 0, last_tick_ms = 0;
  std::string motion_label;
  assets::motion::PoseOpts popt;
  // Session 98 (scenes): the actors placed in the loaded arena, the arena's spots (rectangles of entry 50003, flat floor pieces,
  // zone light sets), and the spot the Tab key last framed.
  assets::spots::ArenaSpots sp;
  assets::spots::Ground ground;  // Session 98 (generalisation): the stage slots' walkable surfaces (entrance grounding)
  std::vector<std::unique_ptr<Actor>> actors;
  LightCtl lt;  // Session 98 (plan step 13): the lighting editor (light_cmds.h)
  int next_actor = 1, spot_cycle = -1;
  EntranceState ent;  // Session 98 (entrance): the playing entrance, if any
  // Session 98 (cap / particles): the entrance bank's particle effects (assets\particles.h) and each emitter's sprite texture slot
  assets::particles::Bank pbank;
  std::map<std::pair<int, size_t>, int> ptex;  // (effect, emitter) -> renderer sprite texture slot
  std::map<uint32_t, assets::particles::SimState> psim;  // Session 99: each placed copy's running simulation (0 = the cue rule)
  bool particles_on = true;
  // Session 99 (scene objects): the entrance bank's GMEV screen flashes + soft-particle switches (particles.h SceneLights), the film
  // filter the entrance lights last asked for (the flash is composed onto it every frame: SetFilmComposed) and the last flash shown.
  assets::particles::SceneLights scene_lights;
  bool scene_lights_on = true;
  float film_base[4] = {0, 0, 0, 0}, flash_now[4] = {0, 0, 0, 0};
  bool film_base_set = false;
  uint32_t particle_seed = 0;  // Session 99 (smoke density): the entrance command's "particle_seed" (0 = default)
  // Session 99 (smoke density): the last frame's particle sprites and which placed effect made them (effect, effect frame, first,
  // end), for {"cmd":"particles_dump","path":...} (the per-sprite comparison with a game capture, tools\assets\smoke_compare.py)
  struct PfxRange { int effect; double frame; size_t first, end; };
  std::vector<assets::particles::Sprite> last_psprites;
  std::vector<PfxRange> last_pfx;
  std::vector<assets::LightSet> arena_orig;  // the loaded arena's light sets before any entrance rewrote them
  // The entrance ended (or its lights are off): the arena's and the wrestler's light sets go back to what the loads made.
  // Session 98 (arena fix 2, assets\stage_pools.h): the entrance's light pools on the stage light map. The rig's slot 2 tracks 52-59
  // (position + RGB) of the latest cue that has them, at (entrance frames since that cue began) mod the motion's length [C: run 6816
  // tints at 8341-12241 and run 6817's dumped page]. `on` false = the stored map (the match, no rig).
  int pools_shown = -1;
  bool pools_on = true;  // {"cmd":"look","pools":false}: the stored map only (A/B)
  std::map<int, std::vector<assets::LightSet>> prop_lights_orig;  // Session 98 (sweep look): belt / prop models' lights as loaded (model -> sets)
  bool follow_on = true;  // {"cmd":"look","follow_spot":false}: no follow spot under the entrant (A/B; Session 98 spot / beard pass)
  bool film_on = true;   // {"cmd":"look","film":false}: no entrance film filter (A/B; Session 98 integration)
  bool match_lm_on = true;  // {"cmd":"look","match_lightmap":false}: the match keeps the entrance page (fade_lightmap00; A/B)
  // entr_page: the entrance page without pools while an entrance is active (its lights off / no rig key), not the match page.
  void ApplyStagePools(bool on, bool entr_page = false) {
    // Session 98 (match look, stage_pools.h): an entrance draws fade_lightmap00 + its pools; otherwise the page is the match's
    // (fade_lightmap01, where its fade settles).
    const bool entr = (on || entr_page) && ent.active && ent.show;
    on = on && pools_on;
    using namespace assets;
    const stage_pools::Cache& pc = stage_pools::TheCache();
    if (!pc.ok) return;
    std::vector<stage_pools::Pool> pools;
    if (on && ent.show && ent.show->rig.ok && ent.cue < ent.show->cues.size() && ent.cue < ent.tl.start.size()) {
      const entrance::Show& sh = *ent.show;
      for (size_t i = ent.cue + 1; i-- > 0 && pools.empty();) {
        const int sub = 10 * sh.cues[i].sub + 13;
        // Session 98 (spot / beard pass, stage_pools.h step 3): the follow spot = track 50's RGB key, on the entrant's root x / z
        if (follow_on)
          if (auto it = sh.rig.m.find({sub, 50}); it != sh.rig.m.end()) {
            double t = ent.t - ent.tl.start[i];
            if (it->second.length > 0) t = std::fmod(std::max(0.0, t), double(it->second.length));
            for (const motion::Track& k : it->second.tracks) {
              double v[3];
              if (k.flags != 0x0100 || !motion::SamplePos(k, t, v)) continue;
              if (v[0] + v[1] + v[2] <= 1e-4) break;  // (black: cue 0)
              stage_pools::Pool p;
              p.disk = true, p.half = stage_pools::kFollowHalf, p.x = float(ent.root[0]), p.z = float(ent.root[2]);
              for (int a = 0; a < 3; ++a) p.rgb[a] = float(v[a]);
              pools.push_back(p);
              break;
            }
          }
        for (int tr = 52; tr <= 59; ++tr) {
          auto it = sh.rig.m.find({sub, tr});
          if (it == sh.rig.m.end()) continue;
          const motion::Motion& mo = it->second;
          double t = ent.t - ent.tl.start[i];
          if (mo.length > 0) t = std::fmod(std::max(0.0, t), double(mo.length));
          stage_pools::Pool p;
          bool have_pos = false, have_col = false;
          for (const motion::Track& k : mo.tracks) {
            double v[3];
            if (!motion::SamplePos(k, t, v)) continue;
            if (k.flags == 0x0100) for (int a = 0; a < 3; ++a) p.rgb[a] = float(v[a]), have_col = true;
            else p.x = float(v[0]), p.z = float(v[2]), have_pos = true;
          }
          if (have_pos && have_col) pools.push_back(p);
        }
      }
    }
    // 0 = the match page, 2 = the entrance page without pools, 1 = with pools (recomposed every call: they move)
    const int sig = !entr ? (match_lm_on ? 0 : 2) : pools.empty() ? 2 : 1;
    if (sig != 1 && sig == pools_shown) return;  // (that page is already up)
    std::vector<uint8_t> page;
    stage_pools::Compose(pools, page, sig == 0);
    r.UpdateLiveTexture(stage_pools::LiveName(), pc.w, pc.h, page.data());
    pools_shown = sig;
  }
  void RestoreEntranceLights(bool entr_page = false) {
    ApplyStagePools(false, entr_page);
    if (auto* l = r.MutableLights(ent.arena_model)) if (l->size() == arena_orig.size()) *l = arena_orig;
    if (Actor* a = FindActor(ent.actor)) if (auto* l = r.MutableLights(a->model)) if (l->size() == ent.base_actor.size()) *l = ent.base_actor;
    match_cam_sig[0] = -1e30f;  // Session 98 (match oracle review): a match-lit actor gets match_base back (its falloff; base_actor has none)
    MatchLightsFollowCamera();
  }
  // mouse
  bool dragging = false;
  int last_x = 0, last_y = 0;
  // Session 98 (step 16, click to inspect): the selected part, its info, the click being decided (press + release under 4 px), the hover pace.
  PickHit sel;
  PartInfo sel_info;
  bool click_cand = false;
  int down_x = 0, down_y = 0;
  double last_hover_ms = 0, last_info_ms = 0;
  int last_hover_x = -1, last_hover_y = -1;
  Camera last_hover_cam;
  // Session 98 (freecam): the controller state (tuning, input, easing), the first game pad, the right-button look (the platform hides
  // the cursor and pins it; its moves arrive as OnLookDelta).
  FreeCamCtl fc;
  std::chrono::steady_clock::time_point fc_last = std::chrono::steady_clock::now();

  void BeginLook() {
    if (fc.looking || !plat || !cam.free) return;
    fc.looking = true;
    plat->SetMouseLook(true);
  }
  void EndLook() {
    if (!fc.looking) return;
    fc.looking = false;  // (first: releasing the capture comes back here through OnCaptureLost)
    if (plat) plat->SetMouseLook(false);
  }
  // Once per main-loop frame (windowed): poll keys / pad, advance the free camera by the host clock.
  void FreeTickFrame() {
    const auto now = std::chrono::steady_clock::now();
    const float dt = std::chrono::duration<float>(now - fc_last).count();
    fc_last = now;
    if (!cam.free || cam.use_vp) {  // (a game view-projection is shown: the free camera takes no input)
      EndLook();
      fc.keys = FreeKeys();
      fc.pad_move_x = fc.pad_move_y = fc.pad_look_x = fc.pad_look_y = fc.pad_up = fc.pad_zoom = 0;
    } else {
      const bool focus = plat && plat->Focused() && !ui.WantTextInput();
      if (plat) plat->PollFreeKeys(fc.keys, focus), plat->PollPad(fc, focus);
      if (fc.looking && !focus) EndLook();
    }
    FreeTick(cam, fc, dt);
  }
  // The free pose as JSON (in orbit mode: the pose the orbit camera looks from, so a shot can be repeated with cam_set).
  std::string PoseJson() const {
    float p[3], yaw, pitch, roll = 0;
    Camera tmp = cam;
    if (cam.free && !cam.use_vp) std::memcpy(p, cam.fpos, sizeof p), yaw = cam.fyaw, pitch = cam.fpitch, roll = cam.froll;
    else FreeFromOrbit(tmp, p, yaw, pitch, &roll);
    return std::string("\"mode\":\"") + (cam.free ? "free" : "orbit") + "\",\"pos\":[" + Fix(p[0], 4) + "," + Fix(p[1], 4) + "," +
           Fix(p[2], 4) + "],\"yaw\":" + Fix(yaw, 4) + ",\"pitch\":" + Fix(pitch, 4) + ",\"roll\":" + Fix(roll, 4) +
           ",\"fov\":" + Fix(cam.fov, 4) + ",\"orbit\":{\"target\":[" + Fix(cam.target[0], 2) + "," + Fix(cam.target[1], 2) + "," +
           Fix(cam.target[2], 2) + "],\"dist\":" + Fix(cam.dist, 2) + ",\"yaw\":" + Fix(cam.yaw, 2) + ",\"pitch\":" + Fix(cam.pitch, 2) + "}";
  }

  // The GPU device is gone: reply "device lost" and end the process (the census restarts it).
  bool Lost() {
    if (!r.DeviceLost()) return false;
    quit = true;
    exit_code = 4;
    return true;
  }
  // Execute, but a bad_alloc from a corrupt pac gives an error line instead of killing the process.
  std::string Safe(const std::string& line) {
    try { return Execute(line); }
    catch (const std::exception& e) { return Err(std::string("exception: ") + e.what()); }
    catch (...) { return Err("unknown exception"); }
  }

  void Preset(const std::string& name, bool* ok = nullptr) {
    float mn[3] = {-100, 0, -100}, mx[3] = {100, 200, 100};
    if (r.HasModel()) r.Bounds(mn, mx);
    bool k = ApplyPreset(cam, name, mn, mx);
    if (k) cam.free = false;  // (Session 98: a preset is an orbit view)
    if (ok) *ok = k;
  }
  std::string CamJson() const {
    return "\"camera\":{\"yaw\":" + Fix(cam.yaw, 2) + ",\"pitch\":" + Fix(cam.pitch, 2) + ",\"dist\":" + Fix(cam.dist, 2) +
           ",\"fov\":" + Fix(cam.fov, 1) + ",\"target\":[" + Fix(cam.target[0], 2) + "," + Fix(cam.target[1], 2) + "," +
           Fix(cam.target[2], 2) + "]}";
  }
  std::string WarnJson(const std::vector<std::string>& w) const {
    std::string s = "[";
    for (size_t i = 0; i < w.size() && i < 50; i++) { if (i) s += ","; s += "\"" + jl::Esc(w[i]) + "\""; }
    if (w.size() > 50) s += ",\"... " + std::to_string(w.size() - 50) + " more\"";
    return s + "]";
  }

  // ---- Session 98 (step 16): click to inspect ------------------------------------------------------------------------------
  void DropSelection() {
    sel = PickHit();
    sel_info = PartInfo();
    r.SetHighlight(PickHit(), PickHit());
    r.ClearHover();
    if (ui.Ready()) ui.SetInspect(sel_info);
  }
  void SetSelection(const PickHit& h) {
    sel = h;
    sel_info = MakeInfo(h);
    if (!sel_info.valid) sel = PickHit();
    r.SetHighlight(PickHit(), sel);
    if (ui.Ready()) ui.SetInspect(sel_info);
  }
  const Actor* ActorOfModel(int model) const {
    for (const auto& a : actors) if ((int)a->model == model) return a.get();
    return nullptr;
  }
  PartInfo MakeInfo(const PickHit& h) const {
    PartInfo p;
    const auto& models = r.Models();
    if (!h.valid() || h.model >= (int)models.size() || h.mesh >= (int)models[(size_t)h.model].meshes.size()) return p;
    const GpuModel& gm = models[(size_t)h.model];
    if (h.mesh >= (int)gm.parts.size()) return p;
    const GpuMesh& me = gm.meshes[(size_t)h.mesh];
    const PartMeta& pm = gm.parts[(size_t)h.mesh];
    p.valid = true;
    p.hit = h;
    p.model_label = gm.label;
    // names
    std::string diffuse = pm.diffuse_name;
    for (const auto& t : pm.tex)
      if (t.first == "Diffuse" && t.second >= 0 && t.second < (int)gm.tex_info.size()) diffuse = gm.tex_info[(size_t)t.second].name;
    assets::PartKey k;
    k.kind = pm.kind >= 1 && pm.kind <= 4 ? (assets::PartKind)pm.kind : assets::PartKind::kArena;
    k.pac = pm.pac, k.key = pm.key, k.node = pm.node, k.texture = diffuse, k.shader = pm.shader, k.palette = pm.palette, k.additive = pm.additive;
    const assets::PartName n = assets::FriendlyPartName(k);
    p.friendly = n.friendly, p.literal = n.literal, p.category = n.category, p.source = n.source;
    p.kind = pm.kind == 2 ? "wrestler" : pm.kind == 3 ? "crowd" : pm.kind == 4 ? "prop" : "arena";
    p.shader = pm.shader, p.node = pm.node;
    if (pm.kind == 2) p.palette = pm.palette;  // (only a wrestler's skin palette says something)
    for (size_t i = 0; i < p.palette.size(); ++i) if (p.palette[i] == ';') p.palette[i] = ',';
    {  // "mesh 2 of 4 in this piece"
      int idx = 0, total = 0;
      for (size_t i = 0; i < gm.parts.size(); ++i)
        if (gm.parts[i].kind == pm.kind && gm.parts[i].pac == pm.pac && gm.parts[i].key == pm.key) {
          if ((int)i == h.mesh) idx = total;
          ++total;
        }
      p.mesh_note = "mesh " + std::to_string(idx + 1) + " of " + std::to_string(total) + " (" + pm.part + ")";
    }
    // textures + movies
    for (const auto& t : pm.tex) {
      PartTexRef tr;
      tr.role = t.first, tr.model = h.model, tr.slot = t.second;
      if (t.second >= 0 && t.second < (int)gm.tex_info.size()) {
        const GpuTexInfo& ti = gm.tex_info[(size_t)t.second];
        tr.name = ti.name, tr.w = ti.width, tr.h = ti.height;
      }
      p.textures.push_back(std::move(tr));
    }
    if (pm.kind == 1 && pm.key >= 800 && pm.key <= 809 && !pm.diffuse_name.empty()) p.movies.push_back(pm.diffuse_name);
    // light, motion, placement
    const Actor* ac = ActorOfModel(h.model);
    p.light = ac ? ac->light : pm.light_id >= 0 ? "arena light set " + std::to_string(pm.light_id) : "the viewer's default rig";
    const float kRad = 57.29578f;
    const float model_turn = std::atan2(gm.rot_sin, gm.rot_cos) * kRad;
    float c[3] = {me.center[0], me.center[1], me.center[2]};
    float inst_turn = 0;
    if (pm.placement >= 0 && pm.placement < (int)gm.pmeta.size()) {
      const PlacementMeta& pl = gm.pmeta[(size_t)pm.placement];
      p.instances = (int)pl.count();
      if (h.instance >= 0 && h.instance < p.instances) {
        const float* M = &pl.mats[(size_t)h.instance * 16];
        const float x = c[0] * M[0] + c[1] * M[4] + c[2] * M[8] + M[12], y = c[0] * M[1] + c[1] * M[5] + c[2] * M[9] + M[13],
                    z = c[0] * M[2] + c[1] * M[6] + c[2] * M[10] + M[14];
        c[0] = x, c[1] = y, c[2] = z;
        inst_turn = std::atan2(-M[2], M[0]) * kRad;
      }
      p.placement = pl.label + ", copy " + std::to_string(h.instance + 1) + " of " + std::to_string(p.instances);
      if (pm.kind == 3) p.placement = pl.label + ", member " + std::to_string(h.instance + 1) + " of " + std::to_string(p.instances);
      const size_t mp = pl.label.find(" m(");
      if (mp != std::string::npos) p.motion = "stage key motion" + pl.label.substr(mp);
    } else {
      p.placement = "drawn once, as stored";
    }
    const float x = c[0] * gm.rot_cos + c[2] * gm.rot_sin + gm.offset[0], z = -c[0] * gm.rot_sin + c[2] * gm.rot_cos + gm.offset[2];
    p.pos[0] = x, p.pos[1] = c[1] + gm.offset[1], p.pos[2] = z;
    p.turn_deg = model_turn + inst_turn;
    if (ac) {
      p.pos[0] = ac->pos[0], p.pos[1] = ac->pos[1], p.pos[2] = ac->pos[2];
      p.turn_deg = ac->yaw;
      p.placement = "actor '" + ac->id + "' at " + (ac->spot.empty() ? std::string("a free position") : "spot " + ac->spot);
      p.motion = ac->have_motion ? ac->motion_label + ", frame " + Fix(ac->frame, 1) : "none (bind pose)";
    }
    return p;
  }
  std::string DoPick(const jl::Value& cmd) {
    if (!r.HasModel()) return Err("nothing is loaded");
    const int pw = plat ? r.Width() : IntArg(cmd.num("w", w)), ph = plat ? r.Height() : IntArg(cmd.num("h", h));
    const int px = IntArg(cmd.num("x", -1)), py = IntArg(cmd.num("y", -1));
    std::string perr;
    const PickHit hit = r.PickAt(cam, pw, ph, px, py, perr);
    if (!perr.empty()) return Err(perr);
    PartInfo info = MakeInfo(hit);
    std::string s = PartInfoJson(info);
    s.insert(s.size() - 1, ",\"x\":" + std::to_string(px) + ",\"y\":" + std::to_string(py) + ",\"w\":" + std::to_string(pw) + ",\"h\":" + std::to_string(ph));
    if (const jl::Value* sv = cmd.get("select"); sv && (sv->t == jl::Value::Bool ? sv->b : sv->n != 0)) SetSelection(hit);
    return s;
  }
  std::string DoSelect(const jl::Value& cmd) {
    if (const jl::Value* cv = cmd.get("clear"); cv && (cv->t == jl::Value::Bool ? cv->b : cv->n != 0)) {
      DropSelection();
      return "{\"ok\":true,\"valid\":false}";
    }
    if (!cmd.has("mesh")) return Err("select needs \"mesh\" (and optionally \"model\", \"instance\"), or \"clear\":true");
    PickHit hit;
    hit.model = IntArg(cmd.num("model", 0)), hit.mesh = IntArg(cmd.num("mesh", 0)), hit.instance = IntArg(cmd.num("instance", 0));
    const PartInfo info = MakeInfo(hit);
    if (!info.valid) return Err("no such mesh (model " + std::to_string(hit.model) + ", mesh " + std::to_string(hit.mesh) + ")");
    SetSelection(hit);
    return PartInfoJson(sel_info);
  }
  // The mod workbench is not built yet: these only say what they would do.
  std::string DoModStub(const jl::Value& cmd) {
    const std::string act = cmd.str("action");
    if (act != "swap_texture" && act != "show_in_mods") return Err("mod action must be swap_texture or show_in_mods");
    if (!sel_info.valid) return Err("nothing is selected");
    std::string tex;
    for (const PartTexRef& t : sel_info.textures) if (t.role == "Diffuse") tex = t.name;
    const std::string would = act == "swap_texture"
        ? "would open the mod workbench on texture '" + tex + "' of '" + sel_info.friendly + "' (" + sel_info.source + ") to swap it"
        : "would open the mod workbench's list for '" + sel_info.friendly + "' (" + sel_info.category + ", " + sel_info.source + ")";
    fprintf(stderr, "mod stub: %s %s\n", act.c_str(), would.c_str());
    return "{\"ok\":true,\"stub\":true,\"action\":\"" + act + "\",\"would\":\"" + jl::Esc(would) + "\"}";
  }
  // Hover (windowed): the id under the cursor, asked for at most every ~70 ms, and the highlight state.
  void PickTick() {
    if (!plat || plat->Minimized() || importer) return;  // (Session 98: the importer's preview has no parts to pick)
    if (job.Active()) {  // a render job owns the scene: no hover, no clicks
      r.ClearHover();
      r.SetHighlight(PickHit(), sel);
      return;
    }
    int px = -1, py = -1;
    const bool inside = plat->CursorInClient(px, py);
    const bool blocked = !inside || (ui.Ready() && ui.MouseOverUi()) || dragging || fc.looking || !r.HasModel();
    if (blocked) {
      r.ClearHover();
    } else {
      // ask again only when the cursor or the camera moved, or (for animation) every 0.15 s while playing / 0.6 s otherwise
      const double now = NowMs();
      const bool changed = px != last_hover_x || py != last_hover_y || std::memcmp(&cam, &last_hover_cam, sizeof cam) != 0;
      if (now - last_hover_ms > 70 && (changed || now - last_hover_ms > (playing ? 150 : 600))) {
        r.RequestHover(px, py);
        last_hover_ms = now, last_hover_x = px, last_hover_y = py, last_hover_cam = cam;
      }
    }
    if (sel.valid() && ActorOfModel(sel.model) && NowMs() - last_info_ms > 200) {  // a selected actor moves / plays: keep the panel's numbers current
      last_info_ms = NowMs();
      sel_info = MakeInfo(sel);
      if (ui.Ready()) ui.SetInspect(sel_info);
    }
    r.SetHighlight(blocked ? PickHit() : r.HoverHit(), sel);
  }
  void ClickSelect(int x, int y) {
    if (importer || !r.HasModel() || job.Active() || (ui.Ready() && ui.MouseOverUi())) return;
    std::string perr;
    const PickHit hit = r.PickAt(cam, r.Width(), r.Height(), x, y, perr);
    if (!perr.empty()) { fprintf(stderr, "pick: %s\n", perr.c_str()); return; }
    if (hit.valid()) SetSelection(hit);
    else DropSelection();  // a click on nothing clears the selection
  }

  std::string DoLoad(const jl::Value& cmd, bool add) {
    std::string kind = cmd.str("kind");
    if (kind != "wrestler" && kind != "arena" && kind != "prop") return Err("kind must be 'wrestler', 'arena' or 'prop'");
    if (!cmd.has("id")) return Err("missing id");
    int id = IntArg(cmd.num("id", 0)), attire = IntArg(cmd.num("attire", 0));
    float at[3] = {0, 0, 0};
    if (auto* a = cmd.get("at")) {
      if (a->t != jl::Value::Arr || a->a.size() != 3) return Err("'at' must be [x,y,z]");
      for (int i = 0; i < 3; i++) at[i] = (float)a->a[i].n;
    }
    assets::Model m;
    std::string err;
    double t0 = NowMs();
    if (!ViewerLoad(kind, id, attire, m, err)) {
      if (err.empty()) err = "load failed";
      // the seam's own "attire N does not exist (wrestler ...": the end of this wrestler's attires, not a failure of the loader
      if (kind == "wrestler" && err.rfind("attire ", 0) == 0 && err.find(" does not exist (wrestler ") != std::string::npos)
        return "{\"ok\":false,\"no_attire\":true,\"error\":\"" + jl::Esc(err) + "\"}";
      return Err(err);
    }
    double t1 = NowMs();
    DropSelection();  // Session 98 (step 16)
    if (!add) { r.Clear(); model_bones.clear(); actors.clear(); ent.active = false; meshes = vertices = triangles = textures = 0; warnings.clear(); label.clear(); }
    // Session 98 (the UI plays entrances: an arena change during one): r.Clear() took the entrance's own models (belt, props, mic);
    // their indices would point at the new arena's models (a later entrance start hid them "out of sight"). The announcer pointer
    // goes too (ReloadArena puts it back when it re-installs him).
    // The companions' ids too (review: a later entrance removed "comp" actors by id - a user's own actor that took the id since);
    // ReloadArena puts both back for the actors it re-installs.
    if (!add) ent.belt_model = -1, ent.props.clear(), ent.ann_mic_model = -1, ent.ann_ptr = nullptr, prop_lights_orig.clear(), ent.comps.clear(), ent.comp = false;
    std::vector<std::string> up_warn = m.warnings;
    if (!add) {  // Session 98: a new arena = new spots (the rectangles are read at the first use)
      sp = assets::spots::ArenaSpots();
      if (kind == "arena") sp.flat.Collect(m, at);
      ground = assets::spots::Ground();
      if (kind == "arena") ground.Collect(m, at);
    }
    if (!r.AddModel(m, at, up_warn, err)) return Lost() ? Err("device lost") : Err(err);
    double t2 = NowMs();
    if (!add && kind == "arena") {  // the arena's own light sets as loaded: what an entrance's lights revert to
      if (auto* l = r.MutableLights(0)) arena_orig = *l;
      else arena_orig.clear();
      // Session 98 (match look): the load put up the match page (asset_load.h); forget what was shown before, then apply the A/B
      pools_shown = assets::stage_pools::TheCache().match.empty() ? 2 : 0;
      if (!match_lm_on) ApplyStagePools(false);
    }
    model_bones.resize(r.ModelCount() - 1);
    model_bones.push_back(m.bones);
    if (!add) model_wid.clear();
    model_wid[r.ModelCount() - 1] = kind == "wrestler" ? id : -1;  // (Session 98 stance: a loaded wrestler stands in his own stance)
    if (kind == "wrestler") load_wid = id;
    ApplyPose();
    size_t nv = 0, nt = 0;
    for (auto& me : m.meshes) { nv += me.vertices.size(); nt += me.indices.size() / 3; }
    meshes += m.meshes.size(); vertices += nv; triangles += nt; textures += m.textures.size();
    for (auto& s : up_warn) warnings.push_back(s);
    label = add && !label.empty() ? label + " + " + m.label : m.label;
    if (!add) Preset(kind == "arena" ? "ring" : "three_quarter");
    if (!add) last_arena = kind == "arena" ? id : -1;
    lt.OnLoad(r, kind == "arena", id, add, m.light_sets, stage_wrestler);  // Session 98: lighting editor (re-applies its edits)
    return "{\"ok\":true,\"label\":\"" + jl::Esc(m.label) + "\",\"meshes\":" + std::to_string(m.meshes.size()) +
           ",\"vertices\":" + std::to_string(nv) + ",\"triangles\":" + std::to_string(nt) +
           ",\"textures\":" + std::to_string(m.textures.size()) + ",\"warnings\":" + WarnJson(up_warn) +
           ",\"load_ms\":" + Fix(m.load_ms > 0 ? m.load_ms : t1 - t0, 2) + ",\"upload_ms\":" + Fix(t2 - t1, 2) + "}";
  }

  // ---- in-window browsing (runs on the render thread, like pipe loads) ----
  void BrowseLoad(int attire_try) {
    if (browse_ids.empty()) { status = "no " + std::string(browse_arena ? "arenas" : "wrestlers") + " found (is --game right?)"; return; }
    std::string line = std::string("{\"cmd\":\"load\",\"kind\":\"") + (browse_arena ? "arena" : "wrestler") + "\",\"id\":" +
                       std::to_string(browse_ids[browse_idx]) + ",\"attire\":" + std::to_string(attire_try) + "}";
    std::string reply = Safe(line);
    if (reply.find("\"ok\":true") != std::string::npos) { browse_attire = attire_try; status.clear(); }
    else if (reply.find("no_attire") != std::string::npos) status = "no more attires";
    else status = "load failed: " + reply;
  }
  void BrowseKind(bool arena) {
    browse_arena = arena;
    browse_ids = ViewerList(arena ? "arena" : "wrestler");
    browse_idx = 0;
    BrowseLoad(0);
  }
  void BrowseKey(unsigned wp) {
    if (!browse) return;
    if (wp == key::kPrior || wp == key::kNext || wp == key::kOem4 || wp == key::kOem6) {  // tron preview: previous / next wrestler with media
      if (!browse_arena) return;
      const std::string reply = Safe(std::string("{\"cmd\":\"stage\",\"step\":") + ((wp == key::kNext || wp == key::kOem6) ? "1" : "-1") + "}");
      if (reply.find("\"ok\":true") == std::string::npos) status = "tron preview: " + reply;
      else status.clear();
    } else if (wp == key::kTab) BrowseKind(!browse_arena);
    else if (wp == key::kLeft || wp == key::kRight) {
      if (browse_ids.empty()) return;
      size_t n = browse_ids.size();
      browse_idx = wp == key::kRight ? (browse_idx + 1) % n : (browse_idx + n - 1) % n;
      BrowseLoad(0);
    } else if ((wp == key::kUp || wp == key::kDown) && !browse_arena) {
      int a = browse_attire + (wp == key::kUp ? 1 : -1);
      if (a >= 0) BrowseLoad(a);
    }
  }
  void UpdateTitle() {
    if (!plat || importer) return;  // (Session 98: the importer keeps its window title)
    std::string t = (status.empty() ? (label.empty() ? std::string("(nothing loaded)") : label) : status) +
                    "  -  Tab, arrows: browse (with actors placed: Tab / Shift+Tab = camera to the next spot); 1-8 views; drag/wheel: camera; F: free camera" +
                    (browse_arena ? std::string("; PgUp/PgDn or [ ]: tron preview (") + (stage_wrestler >= 0 ? "wrestler " + std::to_string(stage_wrestler) : std::string("match look")) + ")" : std::string());
    if (t != shown_title) { shown_title = t; plat->SetTitle(t); }
  }

  // ---- Session 97: motions ----
  assets::motion::Pack* GetPack(const std::string& file, std::string& err) {
    auto it = packs.find(file);
    if (it != packs.end()) return it->second.get();
    auto p = std::make_unique<assets::motion::Pack>();
    if (!ViewerReadFile("pac\\" + file + ".pac", p->d, err)) return nullptr;
    if (!p->Parse(err)) return err = file + ": " + err, nullptr;
    return (packs[file] = std::move(p)).get();
  }
  // Pose every skeleton (models with a `Bip` bone) from the chosen motion at `frame`, or back to the bind pose. A model that
  // belongs to an actor takes the actor's own motion and frame (Session 98); the others take the global one.
  static bool HasBip(const std::vector<assets::Bone>& bones) {
    for (const auto& b : bones) if (b.name == "Bip") return true;
    return false;
  }
  Actor* ActorOf(size_t model) {
    for (auto& a : actors) if (a->model == model) return a.get();
    return nullptr;
  }
  // Session 98 (stance): a wrestler with no motion of his own stands in his in-ring STANCE (frame 175, below) instead of the bind A-pose (Jarrod);
  // an explicit "bind" keeps the bind pose. The stance = the motion the game plays for a wrestler waiting in the ring at the match start:
  // mot00 CA01 top 30, one of the acts 29000-29022 (motion "00000000", 200-270 frames, quiet, Bip y 0-5 = a root delta on the default pose).
  // The game picks it per character (sub_8273AE20, called from sub_8273C1C0, ran in_ring): a table in the executable at guest 0x82029A58,
  // rows {u16 character id, act, act, act} ended by id 0xFFFF; it shuffles the row's acts and takes the first non-zero one no other slot
  // holds; a character with no row gets 29000 + a shuffled 0..17. Evidence (out\s98\stance\notes.md, 0 game runs): run 6816 probe frame
  // 12841 (19 frames after the bell), the posed upper bodies from the skin palettes: Cena = act 29008 frame ~174 (7.5 deg mean over 9
  // joints; Orton's 29015 fits him at 13.8), Orton = act 29015 frame ~178 (6.4; Cena's 29008 at 11.4); 29008 is in Cena's row, 29015 in
  // Orton's. The viewer takes the row's FIRST act (the game's pick is random among the three). The table is read at run time from the
  // player's own files (assets\stance_table.h): --guest_image PATH (a dumped guest image) if given, else the game folder's default.xex
  // (decrypted / unpacked in memory; nothing from it is in the repo). Without either (or for an id with no row): act 29009 (a quiet
  // upright member of the game's fallback pool). The old default (mot00 CA01 top 40 act 23101, a knees-bent crouch: a two-person act set
  // by sub_829D8D68 with a partner, not the stance) stays reachable as the motion name "crouch".
  static constexpr int kStanceFallback = 29009;
  std::map<size_t, int> model_wid;  // model index -> the wrestler id it was loaded as ("load" / "add"; -1 = not a wrestler)
  int load_wid = -1;                // the last wrestler loaded with "load" / "add" (the global motion's stance)
  int WidOf(size_t model) const {
    auto it = model_wid.find(model);
    return it != model_wid.end() ? it->second : -1;
  }
  // The stance's default frame: where both wrestlers of run 6816 stood at the bell (Cena 174, Orton 178 of 227 / 222; the motions start
  // with a gesture, e.g. 29008's arms crossed at frame 0). [U] whether the stance always has run this long when the bell rings.
  static constexpr double kStanceFrame = 175;
  static double StanceFrame(const assets::motion::Motion& mo) { return std::min(kStanceFrame, std::max(0.0, mo.length - 1.0)); }
  std::string guest_image;  // --guest_image PATH (a dumped guest image; empty = read the game folder's default.xex)
  std::map<int, std::vector<int>> stance_rows;
  int stance_state = 0;       // 0 not read, 1 read, -1 no table
  std::string stance_source;  // where the table came from (the "actors" / log note)
  int StanceAct(int wrestler) {
    if (stance_state == 0) {
      stance_state = -1;
      std::string why;
      std::vector<uint8_t> bytes;
      if (!guest_image.empty()) {
        std::ifstream f(std::filesystem::u8path(guest_image), std::ios::binary);
        constexpr uint32_t kOff = assets::stance_table::kTableAddr - assets::stance_table::kImageBase;
        bytes.resize(assets::stance_table::kMaxRows * 8);
        if (f && f.seekg(kOff) && f.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size())) &&
            assets::stance_table::ParseRows(bytes.data(), bytes.size(), stance_rows, why))
          stance_state = 1, stance_source = "guest image " + guest_image;
        else if (why.empty()) why = "cannot read " + guest_image;
      } else {
        std::vector<uint8_t> xex;
        std::string rerr;
        if (ViewerReadFile("default.xex", xex, rerr) && assets::stance_table::FromXex(xex, bytes, why) &&
            assets::stance_table::ParseRows(bytes.data(), bytes.size(), stance_rows, why))
          stance_state = 1, stance_source = "default.xex";
        else if (why.empty()) why = rerr.empty() ? "no default.xex" : rerr;
      }
      if (stance_state < 0) stance_rows.clear(), fprintf(stderr, "stance: no stance table (%s): every wrestler takes act %d\n", why.c_str(), kStanceFallback);
      else fprintf(stderr, "stance: %zu rows from %s\n", stance_rows.size(), stance_source.c_str());
    }
    auto it = stance_rows.find(wrestler);
    return it != stance_rows.end() && !it->second.empty() ? it->second[0] : kStanceFallback;
  }
  std::map<int, assets::motion::Motion> stance_cache;  // act -> motion ("length" < 0 = failed to load)
  const assets::motion::Motion* StanceMotion(int wrestler) {
    const int act = StanceAct(wrestler);
    auto it = stance_cache.find(act);
    if (it == stance_cache.end()) {
      jl::Value none;
      none.t = jl::Value::Obj;
      std::string lab, err;
      assets::motion::Motion mo;
      if (!LoadMotion(none, "stance", mo, lab, err, wrestler)) fprintf(stderr, "stance %d: %s\n", act, err.c_str()), mo.length = -1;
      it = stance_cache.emplace(act, std::move(mo)).first;
    }
    return it->second.length >= 0 ? &it->second : nullptr;
  }
  void PoseActor(Actor& a) {
    const auto& bones = model_bones[a.model];
    std::string e;
    const assets::motion::Motion* idle = (!a.have_motion && !a.want_bind && HasBip(bones)) ? StanceMotion(a.wrestler) : nullptr;
    if (HasBip(bones) && a.have_motion && a.pose_motion) a.skin = assets::motion::BuildSkin(bones, a.motion, a.frame, a.has_opt ? a.opt : popt);
    else if (idle) a.skin = assets::motion::BuildSkin(bones, *idle, StanceFrame(*idle), a.has_opt ? a.opt : popt);
    else a.skin.clear();
    r.SetSkin(a.model, a.skin, e);
    a.feet_y = PosedFeet(a.feet, a.skin, a.feet_cxz);
    if (!a.feet.empty()) r.SetModelFeet(a.model, a.feet_cxz[0], a.feet_cxz[1], a.feet_y);  // Session 98: the shadows stand on the posed feet
  }
  // Move / turn the actor's model; with `reground` the feet go to the surface of the spot (the lowest posed foot vertex).
  // With `anchor` the model is also shifted so that the centroid of the posed feet stands on the spot (the pose's root is not
  // between the feet: the idle's feet sit ~47 units forward of the model origin).
  void PlaceXform(Actor& a, bool reground, bool refresh_anchor = true) {
    if (reground) a.pos[1] = a.ground ? a.floor_y - a.feet_y : a.floor_y;
    if (refresh_anchor) a.anchor_dx = a.feet_cxz[0], a.anchor_dz = a.feet_cxz[1];  // (frozen while a motion plays)
    const float turn = TurnFor(a.yaw), sn = std::sin(turn), cs = std::cos(turn);
    a.origin[0] = a.pos[0], a.origin[1] = a.pos[1], a.origin[2] = a.pos[2];
    if (a.anchor) {
      const float dx = a.anchor_dx, dz = a.anchor_dz;
      a.origin[0] -= dx * cs + dz * sn;
      a.origin[2] -= -dx * sn + dz * cs;
    }
    r.SetTransform(a.model, a.origin, turn);
  }
  void ApplyPose() {
    for (size_t i = 0; i < model_bones.size(); ++i) {
      if (Actor* a = ActorOf(i)) { PoseActor(*a); continue; }
      const auto& bones = model_bones[i];
      if (!HasBip(bones)) continue;
      std::string e;
      if (have_motion && pose_motion) r.SetSkin(i, assets::motion::BuildSkin(bones, cur_motion, frame, popt), e);
      else if (const assets::motion::Motion* idle = !have_motion ? StanceMotion(WidOf(i)) : nullptr) r.SetSkin(i, assets::motion::BuildSkin(bones, *idle, StanceFrame(*idle), popt), e);  // (Session 98: the loaded wrestler's stance, not the A-pose)
      else r.SetSkin(i, {}, e);
    }
  }
  static std::string IdArg(const jl::Value& cmd, const char* key) {
    const jl::Value* v = cmd.get(key);
    if (!v) return "";
    if (v->t == jl::Value::Str) return v->s;
    if (v->t == jl::Value::Num) return "a" + std::to_string(IntArg(v->n));
    return "";
  }
  Actor* FindActor(const std::string& id) {
    for (auto& a : actors) if (a->id == id) return a.get();
    return nullptr;
  }
  // Who a motion / frame / pose command addresses: "actor":ID = that actor only; without it every actor AND the global model(s)
  // (a wrestler from `load` / `add`). use_actors = at least one actor is addressed.
  bool Targets(const jl::Value& cmd, std::vector<Actor*>& out, bool& use_actors, std::string& err) {
    out.clear();
    use_actors = false;
    if (cmd.has("actor")) {
      Actor* a = FindActor(IdArg(cmd, "actor"));
      if (!a) return err = "no actor '" + IdArg(cmd, "actor") + "' (send {\"cmd\":\"actors\"})", false;
      if (a->entrance) return err = "actor '" + a->id + "' is driven by the entrance timeline (use entrance_time)", false;
      out.push_back(a), use_actors = true;
      return true;
    }
    for (auto& a : actors) if (!a->entrance) out.push_back(a.get());  // (the entrance actor only follows entrance_time)
    use_actors = !out.empty();
    return true;
  }
  std::string DoMotions(const jl::Value& cmd) {
    using namespace assets::motion;
    std::string file = cmd.str("file"), bank = cmd.str("bank"), err;
    const int off = std::max(0, IntArg(cmd.num("offset", 0))), lim = std::max(1, std::min(1000, IntArg(cmd.num("limit", 100))));
    if (file.empty()) {
      std::string s = "{\"ok\":true,\"files\":[";
      bool first = true;
      std::vector<std::string> files = {"mot00", "mot01", "mot02", "mot03", "mot07", "mot08"};
      for (const std::string& f : ViewerDlcMotionFiles()) files.push_back(f);  // Session 98: + the DLC packs' dlc_motPP
      for (const std::string& f : files) {
        Pack* p = GetPack(f, err);
        if (!p) continue;
        if (!first) s += ",";
        first = false;
        s += "{\"file\":\"" + std::string(f) + "\",\"banks\":[";
        for (size_t i = 0; i < p->banks.size(); ++i) s += (i ? ",\"" : "\"") + jl::Esc(p->banks[i].name) + "\"";
        s += "]}";
      }
      return s + "]}";
    }
    Pack* p = GetPack(file, err);
    if (!p) return Err(err);
    if (bank.empty()) {
      std::string s = "{\"ok\":true,\"file\":\"" + jl::Esc(file) + "\",\"banks\":[";
      for (size_t i = 0; i < p->banks.size(); ++i)
        s += (i ? "," : "") + std::string("{\"bank\":\"") + jl::Esc(p->banks[i].name) + "\",\"kb\":" + std::to_string(p->banks[i].size / 1024) + "}";
      return s + "]}";
    }
    const Pack::Bank* bk = p->FindBank(bank);
    if (!bk) return Err("no bank '" + bank + "' in " + file);
    auto acts = p->Acts(*bk);
    if (!cmd.has("act")) {  // (top, act) pairs
      std::map<std::pair<uint32_t, uint32_t>, int> seen;  // bit 1 zlib, bit 2 bpe
      for (auto& a : acts) seen[{a.top, a.act}] |= a.zlib ? 1 : 2;
      std::string s = "{\"ok\":true,\"file\":\"" + jl::Esc(file) + "\",\"bank\":\"" + jl::Esc(bank) + "\",\"total\":" +
                      std::to_string(seen.size()) + ",\"offset\":" + std::to_string(off) + ",\"acts\":[";
      int n = 0, k = 0;
      for (auto& kv : seen) {
        if (k++ < off) continue;
        if (n >= lim) break;
        s += (n++ ? "," : "") + std::string("{\"top\":") + std::to_string(kv.first.first) + ",\"act\":" + std::to_string(kv.first.second) +
             ",\"zlib\":" + ((kv.second & 1) ? "true" : "false") + "}";
      }
      return s + "]}";
    }
    const uint32_t top = (uint32_t)IntArg(cmd.num("top", -1)), act = (uint32_t)IntArg(cmd.num("act", -1));
    for (auto& a : acts) {
      if (a.top != top || a.act != act || !a.zlib) continue;
      Act ac;
      if (!p->LoadAct(a, ac, err)) return Err(err);
      std::string s = "{\"ok\":true,\"file\":\"" + jl::Esc(file) + "\",\"bank\":\"" + jl::Esc(bank) + "\",\"top\":" + std::to_string(top) +
                      ",\"act\":" + std::to_string(act) + ",\"total\":" + std::to_string(ac.yanm.size()) + ",\"motions\":[";
      int n = 0;
      for (size_t i = (size_t)off; i < ac.yanm.size() && n < lim; ++i, ++n) {
        Motion mo = ParseMotion(ac, i);
        s += (n ? "," : "") + std::string("{\"name\":\"") + jl::Esc(mo.name) + "\",\"frames\":" + std::to_string(mo.length) + ",\"tracks\":" +
             std::to_string(mo.tracks.size()) + (mo.unsupported.empty() ? "" : ",\"unsupported\":\"" + jl::Esc(mo.unsupported) + "\"") + "}";
      }
      return s + "]}";
    }
    return Err("no ZLIB act " + std::to_string(act) + " in top " + std::to_string(top) + " of " + file + " " + bank);
  }
  // Find and parse the motion `name` of the act named by cmd (file, bank, top, act, index). Session 98 (stance): "idle" / "stance" =
  // the wrestler's in-ring stance (StanceAct; `wrestler` -1 = the fallback act), "crouch" = the old default idle (mot00 CA01 top 40
  // act 23101 "00000000"); explicit file / bank / top / act in cmd win.
  bool LoadMotion(const jl::Value& cmd, std::string name, assets::motion::Motion& out, std::string& label, std::string& err, int wrestler = -1) {
    using namespace assets::motion;
    std::string file = cmd.str("file", "mot00"), bank = cmd.str("bank", "CA01");
    // (review: "idle" with an explicit top or act keeps loading that act's "00000000", as before the stance)
    const bool stance = (name == "idle" || name == "stance") && !cmd.has("top") && !cmd.has("act");
    uint32_t top = (uint32_t)IntArg(cmd.num("top", stance ? 30 : 40)), act = (uint32_t)IntArg(cmd.num("act", stance ? StanceAct(wrestler) : 23101));
    if (name == "idle" || name == "stance" || name == "crouch") name = "00000000";
    Pack* p = GetPack(file, err);
    if (!p) return false;
    const Pack::Bank* bk = p->FindBank(bank);
    if (!bk) return err = "no bank '" + bank + "' in " + file, false;
    for (auto& a : p->Acts(*bk)) {
      if (a.top != top || a.act != act || !a.zlib) continue;
      Act ac;
      if (!p->LoadAct(a, ac, err)) return false;
      size_t idx = SIZE_MAX;  // (S97 review: "not found" must not alias a real motion index)
      for (size_t i = 0; i < ac.motion_names.size(); ++i) if (ac.motion_names[i] == name) { idx = i; break; }
      if (cmd.has("index")) idx = (size_t)std::max(0, IntArg(cmd.num("index", 0)));
      if (idx >= ac.yanm.size()) return err = "no motion '" + name + "' in this act (" + std::to_string(ac.yanm.size()) + " motions)", false;
      Motion mo = ParseMotion(ac, idx);
      if (!mo.unsupported.empty()) return err = "motion not readable: " + mo.unsupported, false;
      out = std::move(mo);
      if (file == "mot00" && bank == "CA01" && top == 30 && act >= 29000 && act <= 29022) out.root_delta = 1;  // a stance (see StanceAct)
      label = file + " " + bank + " " + std::to_string(top) + " " + std::to_string(act) + " " + out.name;
      return true;
    }
    return err = "no ZLIB act " + std::to_string(act) + " in top " + std::to_string(top) + " of " + file + " " + bank, false;
  }
  std::string DoMotion(const jl::Value& cmd) {
    using namespace assets::motion;
    std::string err, label;
    std::vector<Actor*> tg;
    bool ua = false;
    if (!Targets(cmd, tg, ua, err)) return Err(err);
    Motion mo;
    const std::string mname = cmd.str("name", "idle");
    if (!LoadMotion(cmd, mname, mo, label, err, load_wid)) return Err(err);  // (the global motion: the loaded wrestler's stance)
    // (Session 98 stance: "idle" / "stance" without an explicit act = each actor's own stance act)
    const bool own_stance = (mname == "idle" || mname == "stance") && !cmd.has("act") && !cmd.has("top");
    std::string notes;
    int bit0 = 0;
    for (auto& t : mo.tracks) {
      if (t.bit0) ++bit0;
      if (!t.unsupported.empty()) notes += (notes.empty() ? "" : "; ") + t.unsupported;
    }
    std::string who;
    for (Actor* a : tg) {
      Motion own;
      std::string own_label, own_err;
      if (own_stance && LoadMotion(cmd, mname, own, own_label, own_err, a->wrestler)) a->motion = std::move(own), a->motion_label = own_label;
      else a->motion = mo, a->motion_label = label;
      a->have_motion = a->pose_motion = true, a->frame = a->motion.root_delta == 1 ? StanceFrame(a->motion) : 0;  // (a stance: its bell frame)
      PoseActor(*a);
      PlaceXform(*a, true);
      who += (who.empty() ? "\"" : ",\"") + a->id + "\"";
    }
    if (!cmd.has("actor")) {
      cur_motion = mo;
      have_motion = true, pose_motion = true;
      frame = mo.root_delta == 1 ? StanceFrame(mo) : 0;  // (a stance: its bell frame)
      motion_label = label;
      ApplyPose();
    }
    return "{\"ok\":true,\"motion\":\"" + jl::Esc(label) + "\",\"frames\":" + std::to_string(mo.length) + ",\"tracks\":" +
           std::to_string(mo.tracks.size()) + ",\"calf_tracks_left_at_bind\":" + std::to_string(bit0) + ",\"track_notes\":\"" + jl::Esc(notes) + "\"" +
           (ua ? ",\"actors\":[" + who + "]" : "") + "}";
  }
  std::string DoPose(const jl::Value& cmd) {
    std::vector<Actor*> tg;
    bool ua = false;
    std::string terr;
    if (!Targets(cmd, tg, ua, terr)) return Err(terr);
    if (cmd.has("mode")) {
      const std::string m = cmd.str("mode");
      if (m == "bind") { for (Actor* a : tg) a->pose_motion = false; if (!cmd.has("actor")) pose_motion = false; }
      else if (m == "motion") {
        int n = 0;  // actors without a motion are skipped
        for (Actor* a : tg) if (a->have_motion) a->pose_motion = true, ++n;
        if (!cmd.has("actor") && have_motion) pose_motion = true, ++n;
        if (!n) return Err("no motion chosen (send 'motion' first)");
      }
      else return Err("mode must be 'bind' or 'motion'");
    }
    // the unconfirmed conventions, switchable for the picture check
    if (cmd.has("rot")) popt.rot_mode = std::max(0, std::min(2, IntArg(cmd.num("rot", 0))));
    if (const jl::Value* v = cmd.get("conj")) popt.conj = v->t == jl::Value::Bool ? v->b : v->n != 0;
    if (cmd.has("flip")) popt.flip = IntArg(cmd.num("flip", 2)) & 7;
    if (cmd.has("tscale")) popt.tscale = cmd.num("tscale", 1.0);  // (Session 98 match look: 1 is the default; -1 = the old auto scale)
    if (const jl::Value* v = cmd.get("trans")) popt.trans = v->t == jl::Value::Bool ? v->b : v->n != 0;
    if (const jl::Value* v = cmd.get("ik")) popt.leg_ik = v->t == jl::Value::Bool ? v->b : v->n != 0;
    if (cmd.has("legsolver")) popt.leg_solver = std::max(1, std::min(2, IntArg(cmd.num("legsolver", 2))));  // 2 = the game's solver (port), 1 = the earlier approximation
    if (cmd.has("posrule")) popt.pos_rule = std::max(0, std::min(1, IntArg(cmd.num("posrule", 1))));  // 1 = the game's (delta on the bind), 0 = the earlier name rule
    if (cmd.has("legaxis")) popt.leg_axis = std::max(1, std::min(2, IntArg(cmd.num("legaxis", 1))));
    if (const jl::Value* v = cmd.get("notracks")) popt.no_tracks = v->t == jl::Value::Bool ? v->b : v->n != 0;
    if (const jl::Value* v = cmd.get("vector")) popt.vector_track = v->t == jl::Value::Bool ? v->b : v->n != 0;
    ApplyPose();
    for (Actor* a : tg) PlaceXform(*a, true);
    std::string info;
    for (Actor* a : tg) {
      if (HasBip(model_bones[a->model]) && a->have_motion) { assets::motion::BuildSkin(model_bones[a->model], a->motion, a->frame, popt, &info); break; }
    }
    if (info.empty() && !cmd.has("actor"))
      for (auto& bones : model_bones) {
        if (HasBip(bones) && have_motion) { assets::motion::BuildSkin(bones, cur_motion, frame, popt, &info); break; }
      }
    const bool pm = !tg.empty() ? tg[0]->pose_motion : pose_motion;
    return std::string("{\"ok\":true,\"mode\":\"") + (pm ? "motion" : "bind") + "\",\"rot\":" + std::to_string(popt.rot_mode) +
           ",\"conj\":" + (popt.conj ? "true" : "false") + ",\"flip\":" + std::to_string(popt.flip) + ",\"info\":\"" + jl::Esc(info) + "\"}";
  }
  std::string DoBones() {  // debug: the skeleton of the first model that has one
    for (auto& bones : model_bones) {
      if (bones.empty()) continue;
      std::string s = "{\"ok\":true,\"bones\":[";
      for (size_t i = 0; i < bones.size(); ++i) {
        const auto& b = bones[i];
        s += (i ? "," : "") + std::string("{\"i\":") + std::to_string(i) + ",\"n\":\"" + jl::Esc(b.name) + "\",\"p\":" + std::to_string(b.parent) +
             ",\"t\":[" + Fix(b.local_translation[0], 2) + "," + Fix(b.local_translation[1], 2) + "," + Fix(b.local_translation[2], 2) +
             "],\"q\":[" + Fix(b.local_rotation[0], 3) + "," + Fix(b.local_rotation[1], 3) + "," + Fix(b.local_rotation[2], 3) + "," +
             Fix(b.local_rotation[3], 3) + "]}";
      }
      return s + "]}";
    }
    return Err("no skeleton loaded");
  }
  // Session 98 (motion rule, debug): {"cmd":"tracks"[,"actor":ID]} = per bone the bind translation, the track's flags and its sampled
  // position / bit-0 vector at the current frame (the actor's motion, else the global one). Numbers only (no game data kept).
  std::string DoTracks(const jl::Value& cmd) {
    const assets::motion::Motion* mo = have_motion ? &cur_motion : nullptr;
    double f = frame;
    size_t model = SIZE_MAX;
    if (cmd.has("actor")) {
      Actor* a = FindActor(IdArg(cmd, "actor"));
      if (!a || !a->have_motion) return Err("no such actor with a motion");
      mo = &a->motion, f = a->frame, model = a->model;
    }
    if (!mo) return Err("no motion");
    for (size_t i = 0; model == SIZE_MAX && i < model_bones.size(); ++i) if (HasBip(model_bones[i])) model = i;
    if (model == SIZE_MAX) return Err("no skeleton");
    const auto& bones = model_bones[model];
    std::string s = "{\"ok\":true,\"frame\":" + Fix(f, 1) + ",\"bones\":[";
    for (size_t i = 0; i < bones.size(); ++i) {
      const auto& b = bones[i];
      s += (i ? "," : "") + std::string("{\"i\":") + std::to_string(i) + ",\"n\":\"" + jl::Esc(b.name) + "\",\"p\":" + std::to_string(b.parent) + ",\"t\":[" +
           Fix(b.local_translation[0], 2) + "," + Fix(b.local_translation[1], 2) + "," + Fix(b.local_translation[2], 2) + "]";
      if (const assets::motion::Track* t = mo->Find(assets::motion::NodeHash(b.name))) {
        char fl[8];
        std::snprintf(fl, sizeof fl, "%04X", t->flags);
        s += std::string(",\"fl\":\"") + fl + "\"";
        double p[4];
        if (assets::motion::SamplePos(*t, f, p)) s += ",\"pos\":[" + Fix(p[0], 2) + "," + Fix(p[1], 2) + "," + Fix(p[2], 2) + "]";
        if (assets::motion::SampleIk(*t, f, p)) s += ",\"ik\":[" + Fix(p[0], 2) + "," + Fix(p[1], 2) + "," + Fix(p[2], 2) + "," + Fix(p[3], 3) + "]";
      }
      s += "}";
    }
    return s + "]}";
  }
  // window mode: advance a playing motion at 60 frames/s
  void Tick() {
    const double now = NowMs();
    const double dt = last_tick_ms > 0 ? std::min(100.0, now - last_tick_ms) : 0;
    last_tick_ms = now;
    if (playing && have_motion && pose_motion && cur_motion.length > 0) {
      frame += dt * 0.06;
      frame = std::fmod(frame, (double)cur_motion.length);
      ApplyPose();
    }
    if (playing)
      for (auto& a : actors)
        if (!a->entrance && a->have_motion && a->pose_motion && a->motion.length > 0) {
          a->frame = std::fmod(a->frame + dt * 0.06, (double)a->motion.length);
          PoseActor(*a);
          PlaceXform(*a, a->ground_play, false);
        }
    EntranceLightsFollowCamera();
    MatchLightsFollowCamera();
    if (ent.active && ent.playing) {  // Session 98: the entrance timeline (motion frames, 60 per second of scene time)
      ent.t += dt * 0.06 * ent.speed;
      if (ent.t >= ent.tl.total) ent.t = ent.tl.total, ent.playing = false;
      ApplyEntrance();
    }
  }

  // ---- Session 98: scenes (actors on spots) ----
  // The rectangles / zone lights of the loaded arena are read once (a failure is remembered in sp.zones_err: an arena without entry
  // 50003 still has the ring spots). False only when no arena is loaded and `need_arena`.
  bool EnsureSpots(std::string& err, bool need_arena = false) {
    if (last_arena < 0) return need_arena ? (err = "load an arena first ({\"cmd\":\"load\",\"kind\":\"arena\",\"id\":5})", false) : true;
    if (sp.zones_tried) return true;
    sp.zones_tried = true;
    char rel[64];
    snprintf(rel, sizeof rel, "pac\\bg\\bg%02d.pac", last_arena);
    std::vector<uint8_t> raw;
    std::string e;
    if (ViewerReadFile(rel, raw, e) && assets::spots::ReadZones(raw, sp.rects, sp.lights, e)) sp.have_rects = true;
    else sp.zones_err = e;
    return true;
  }
  // Re-load the loaded arena (lights / stage / crowd changes) with the camera kept; the actors are re-placed with their own settings.
  // Entrance combine strengths per wrestler (out\runtime\s98\passes.md 2: c31 / c32 measured Brock (343) 0.4 / 1.6, Bryan (324) 0 / 0;
  // anyone else bloom 0.4, glare 0 [unconfirmed]; -1 = no wrestler known: the same "anyone else" values).
  // Session 98 (arena fix): Orton (161) 0 / 0 [C: probe run 6816, the entrance combine's pixel c31 = c32 = 0 in all six of his
  // frames 8341-12241, out\s98\arena_fix\notes.md]. The star flares of his ring frames are yFlashDefault sprites (crowd camera
  // flashes, drawn over the finished picture), not the glare pass.
  static void EntranceStrengths(int wrestler, float& bloom, float& glare) {
    bloom = wrestler == 324 || wrestler == 161 ? 0.f : 0.4f;
    glare = wrestler == 343 ? 1.6f : 0.f;
  }
  std::string ReloadArena() {
    std::vector<std::unique_ptr<Actor>> saved = std::move(actors);
    actors.clear();
    jl::Value lc;
    std::string perr2;
    jl::Parse("{\"cmd\":\"load\",\"kind\":\"arena\",\"id\":" + std::to_string(last_arena) + "}", lc, perr2);
    const Camera cs = cam;
    const void* const ann = ent.ann_ptr;  // (DoLoad forgets it; the same actor object is re-installed below)
    const auto comps = ent.comps;         // (the same for the companions' ids)
    const bool comp = ent.comp;
    std::string res = DoLoad(lc, false);
    cam = cs;
    if (res.find("\"ok\":true") == std::string::npos) return res;
    std::string lost;
    for (auto& a : saved) {
      const bool is_ann = ann && a.get() == ann;
      std::string e;
      EnsureSpots(e);
      assets::spots::Spot s;
      if (a->spot != "pos" && sp.Resolve(a->spot, a->t, s, e)) {  // the new arena's spot (floor and zone light may differ)
        for (int i = 0; i < 3; i++) a->pos[i] = s.pos[i];
        a->floor_y = s.pos[1];
      }
      if (!Install(std::move(a), e)) lost += (lost.empty() ? "" : "; ") + e;
      else if (is_ann) ent.ann_ptr = ann;
    }
    for (const auto& c : comps)
      if (FindActor(c.first)) ent.comps.push_back(c), ent.comp = comp;
    if (!lost.empty() && res.back() == '}') res.insert(res.size() - 1, ",\"actors_lost\":\"" + jl::Esc(lost) + "\"");
    return res;
  }
  static std::string Vec3(const float* v, int d = 1) { return "[" + Fix(v[0], d) + "," + Fix(v[1], d) + "," + Fix(v[2], d) + "]"; }
  static std::string CustomJson(int wrestler) {
    assets::char_mods::Entry ce;
    return assets::char_mods::IsCustom(wrestler) && assets::char_mods::Find(wrestler, ce) ? ",\"custom\":\"" + jl::Esc(ce.name) + "\"" : std::string();
  }
  std::string ActorJson(const Actor& a) const {
    const float* origin = a.origin;
    const float feet[3] = {a.pos[0], a.floor_y, a.pos[2]};
    return "{\"id\":\"" + jl::Esc(a.id) + "\",\"wrestler\":" + std::to_string(a.wrestler) + CustomJson(a.wrestler) + ",\"attire\":" + std::to_string(a.attire) +
           ",\"spot\":\"" + jl::Esc(a.spot) + "\",\"t\":" + Fix(a.t, 2) + ",\"pos\":" + Vec3(feet) + ",\"origin\":" + Vec3(origin) +
           ",\"yaw\":" + Fix(a.yaw, 1) + ",\"floor\":" + Fix(a.floor_y, 1) + ",\"feet_y\":" + Fix(a.feet_y, 1) + ",\"drift\":[" + Fix(a.feet_cxz[0], 1) + "," + Fix(a.feet_cxz[1], 1) + "]" + ",\"light\":\"" + jl::Esc(a.light) +
           "\",\"motion\":\"" + jl::Esc(a.have_motion ? a.motion_label : std::string("bind")) + "\",\"frame\":" + Fix(a.frame, 1) + "}";
  }
  void RemoveActorAt(size_t idx) {
    if (actors[idx]->entrance) {
      if (ent.active) RestoreEntranceLights();
      ent.active = false;
      { const float z[3] = {0, 0, 0}; r.SetEntranceAim(false, z); r.SetFlashFrame(false, 0); }
    }
    DropSelection();  // Session 98 (step 16): model indices shift
    if (actors[idx].get() == ent.ann_ptr) ent.ann_ptr = nullptr;  // Session 99: no dangling announcer (a new actor may reuse the address)
    const size_t mi = actors[idx]->model;
    actors.erase(actors.begin() + (std::ptrdiff_t)idx);
    RemoveModelIndex(mi);
  }
  // Removes model `mi` from the renderer and shifts every index kept above it (actors, the entrance's belt / props / mic, the
  // prop lights, model_wid). Session 98 phone fixes review: shared by RemoveActorAt and the entrance's own models
  // (ClearEntranceExtras), which before were only moved out of sight and piled up with every UI restart.
  void RemoveModelIndex(size_t mi) {
    DropSelection();
    r.RemoveModel(mi);
    if (mi < model_bones.size()) model_bones.erase(model_bones.begin() + (std::ptrdiff_t)mi);
    for (auto& a : actors) if (a->model > mi) --a->model;
    {
      std::map<size_t, int> mw;
      for (const auto& [k, v] : model_wid)
        if (k != mi) mw[k > mi ? k - 1 : k] = v;
      model_wid.swap(mw);
    }
    // Session 99 (review): the entrance's own non-actor models (belt, props, the announcer's mic) shift with them
    auto shift = [&](int& m) { if (m >= 0 && size_t(m) > mi) --m; else if (m >= 0 && size_t(m) == mi) m = -1; };
    shift(ent.belt_model), shift(ent.ann_mic_model);
    for (auto& p : ent.props) shift(p.model);
    if (!prop_lights_orig.empty()) {
      std::map<int, std::vector<assets::LightSet>> moved;
      for (auto& [m, ls] : prop_lights_orig) {
        int k = m;
        shift(k);
        if (k >= 0) moved[k] = std::move(ls);
      }
      prop_lights_orig.swap(moved);
    }
  }
  std::string DoRemove(const jl::Value& cmd) {
    size_t n = 0;
    if (const jl::Value* all = cmd.get("all"); all && (all->t == jl::Value::Bool ? all->b : all->n != 0)) {
      while (!actors.empty()) RemoveActorAt(actors.size() - 1), ++n;
    } else {
      const std::string id = IdArg(cmd, "id");
      if (id.empty()) return Err("remove needs \"id\" or \"all\":true");
      size_t idx = actors.size();
      for (size_t i = 0; i < actors.size(); ++i) if (actors[i]->id == id) idx = i;
      if (idx == actors.size()) return Err("no actor '" + id + "'");
      RemoveActorAt(idx), n = 1;
    }
    return "{\"ok\":true,\"removed\":" + std::to_string(n) + ",\"left\":" + std::to_string(actors.size()) + "}";
  }
  std::string DoActors() {
    std::string s = "{\"ok\":true,\"arena\":" + std::to_string(last_arena) + ",\"actors\":[";
    for (size_t i = 0; i < actors.size(); ++i) s += (i ? "," : "") + ActorJson(*actors[i]);
    return s + "]}";
  }
  // Put an actor's wrestler into the scene: load it, give it the light of its spot, add the model, pose and ground it. The actor
  // keeps its settings (spot / t / yaw / pos / floor / motion / frame / flags) in `a`; used by place and by the arena reload.
  bool Install(std::unique_ptr<Actor> a, std::string& err) {
    assets::Model m;
    if (!ViewerLoad("wrestler", a->wrestler, a->attire, m, err)) {
      if (err.empty()) err = "load failed";
      return false;
    }
    int want = a->light_mode;
    if (want == -3) {  // auto: the zone set of the spot (stage 200, ramp 201), else the default rig
      want = a->spot == "stage" ? 200 : a->spot == "ramp" ? 201 : -1;
    }
    std::string ldesc = "default rig (set 121 numbers)";
    // Session 98 (shadow / match light): in the MATCH look an "auto" wrestler off the stage / ramp takes the game's match wrestler light
    // [C: probe run 6816, match frames 12841-17641, every skin draw of both wrestlers: c0..c3 = set 10's key colours (0.6275, 0.349,
    // 1.445, 0), c181 / c182 sky / ground 0.137 / 0.333, c4 = 1.949-1.961 (set 10's light 4), c0..c3.w = set 10's lobe weights to 4%;
    // c8..c12 = V^T d of set 10's keyed directions (V = rows c52..c54) with error 0.0000-0.0001 (light 2: 0.011-0.017), i.e. the five
    // lights are CAMERA-ATTACHED as in the entrance]. Before: the default rig (set 121's grey 0.451 world-fixed lights): a dark wrestler
    // from the far orbit view.
    a->match_light = false;
    if (want < 0 && a->light_mode == -3 && stage_wrestler < 0) {
      const assets::LightSet* s10 = nullptr;  // (the loaded arena's own set 10, the stage key's values)
      for (const assets::LightSet& s : arena_orig) if (s.id == 10) s10 = &s;
      float tot = 0;
      if (s10)
        for (int i = 0; i < 4; ++i) for (int k = 0; k < 3; ++k) tot += s10->col[i][k];
      if (s10 && tot > 1e-4f && last_arena >= 0) {
        assets::LightSet s = *s10;
        for (int k = 0; k < 3; ++k) s.hl_col[k] = 1.961f;  // light 4 (the edge highlight): white 1.961, view-space (0, 0.643, -0.766) (run 6816)
        s.hl_dir[0] = 0.f, s.hl_dir[1] = 0.643f, s.hl_dir[2] = -0.766f;
        s.has_pcol = false;
        a->match_light = true, a->match_base = s;
        m.light_sets = {s};
        for (auto& me : m.meshes) me.light_set = 0;
        ldesc = "match light (arena set 10, camera-attached)";
      }
    }
    if (want >= 0) {
      auto it = sp.lights.find(want);
      float tot = 0;
      if (it != sp.lights.end()) {
        for (int i = 0; i < 4; ++i) for (int k = 0; k < 3; ++k) tot += it->second.col[i][k];
        for (int k = 0; k < 3; ++k) tot += it->second.sky[k] + it->second.ground[k];
      }
      if (it != sp.lights.end() && tot > 1e-4f) {
        m.light_sets = {it->second};
        // Session 98 (match oracle review): the cube falloff only on the match light (set 10) for now. Sets 200 / 201 store 0.5 too, but a
        // wrestler on the stage / ramp outside an entrance has no game sample [U]; entrances measure 0. So a zone / numbered set keeps 0.
        std::memset(m.light_sets[0].fall, 0, sizeof m.light_sets[0].fall);
        for (auto& me : m.meshes) me.light_set = 0;
        ldesc = "arena set " + std::to_string(want);
      } else ldesc = std::string("default rig (arena set ") + std::to_string(want) + (it == sp.lights.end() ? " not in this arena)" : " is black)");
    }
    // (the UI's own wrestler keeps the plain label so its scene scan finds it; other actors are marked)
    if (a->id != "ui") m.label = "actor " + a->id + " " + m.label;
    std::vector<std::string> up_warn = m.warnings;
    const float zero[3] = {0, 0, 0};
    if (!r.AddModel(m, zero, up_warn, err)) return err = Lost() ? "device lost" : err, false;
    model_bones.resize(r.ModelCount() - 1);
    model_bones.push_back(m.bones);
    a->light = ldesc, a->model = r.ModelCount() - 1;
    a->feet = CollectFeet(m);
    Actor& ar = *a;
    actors.push_back(std::move(a));
    PoseActor(ar);
    PlaceXform(ar, true);
    match_cam_sig[0] = -1e30f;  // (the new actor's match light is turned to the camera at once)
    MatchLightsFollowCamera();
    return true;
  }
  // {"cmd":"place","wrestler":161,"attire":0,"spot":"stage|ramp|ring|ring_ne|ring_nw|ring_se|ring_sw|ringside","t":0.5,"yaw":deg,
  //  "motion":"idle","frame":0,"id":"name","light":"auto"|"rig"|N,"ground":true,"anchor":true,"ground_play":false}  (or "pos":[x,y,z]
  //  instead of "spot": a free position, y = the surface; both = an error). A "motion" name is looked up in the act given by
  //  file / bank / top / act (defaults: the idle of mot00 CA01); "none" = no motion (stands in the default idle); "bind" = the bind pose.
  //  Session 98 (stance): "idle" (the default) / "stance" = the wrestler's own in-ring stance (StanceAct), "crouch" = the old idle (23101).
  std::string DoPlace(const jl::Value& cmd) {
    using namespace assets::spots;
    if (!cmd.has("wrestler") && !cmd.has("custom")) return Err("missing wrestler");
    if (cmd.has("pos") && cmd.has("spot")) return Err("give either \"spot\" or \"pos\", not both");
    int wid = IntArg(cmd.num("wrestler", 0));
    const int attire = IntArg(cmd.num("attire", 0));
    if (cmd.has("custom")) {  // Session 98: an imported community character by file name ("Mr Kennedy 09"); shown only while the mods toggle is on
      assets::char_mods::Entry ce;
      if (!assets::char_mods::FindName(cmd.str("custom", ""), ce)) return Err("no imported character '" + cmd.str("custom", "") + "' (char_list; the mods toggle must be on)");
      wid = ce.id;
    }
    const double fr = cmd.num("frame", 0);
    if (!(fr >= 0 && fr < 1e7)) return Err("frame must be a frame number >= 0");
    std::string err;
    Spot s;
    if (cmd.has("pos")) {
      const jl::Value* pv = cmd.get("pos");
      if (pv->t != jl::Value::Arr || pv->a.size() != 3) return Err("'pos' must be [x,y,z]");
      s.name = "pos";
      for (int i = 0; i < 3; i++) s.pos[i] = (float)pv->a[i].n;
      s.yaw = 0;
      s.note = "free position";
    } else {
      const std::string spot = cmd.str("spot", "ring");
      if (!EnsureSpots(err, spot == "stage" || spot == "ramp")) return Err(err);
      if (!sp.Resolve(spot, cmd.num("t", 0.5), s, err)) return Err(err);
    }
    if (cmd.has("yaw")) s.yaw = (float)cmd.num("yaw", 0);
    auto a = std::make_unique<Actor>();
    std::string id = IdArg(cmd, "id");
    if (id.empty()) { do id = "a" + std::to_string(next_actor++); while (FindActor(id)); }
    a->id = id, a->wrestler = wid, a->attire = attire, a->spot = s.name, a->t = cmd.num("t", 0.5);
    for (int i = 0; i < 3; i++) a->pos[i] = s.pos[i];
    a->floor_y = s.pos[1], a->yaw = s.yaw;
    if (cmd.has("light") && cmd.get("light")->t == jl::Value::Num) a->light_mode = IntArg(cmd.get("light")->n);
    else if (cmd.str("light", "auto") == "rig") a->light_mode = -1;
    if (const jl::Value* gv = cmd.get("ground")) a->ground = gv->t == jl::Value::Bool ? gv->b : gv->n != 0;
    if (const jl::Value* av = cmd.get("anchor")) a->anchor = av->t == jl::Value::Bool ? av->b : av->n != 0;
    if (const jl::Value* pv = cmd.get("ground_play")) a->ground_play = pv->t == jl::Value::Bool ? pv->b : pv->n != 0;
    std::string merr;
    const std::string mname = cmd.str("motion", "idle");
    a->want_bind = mname == "bind";  // (Session 98: "none" = no motion of its own: the default idle pose; "bind" = the bind pose)
    if (mname != "none" && mname != "bind") {
      if (LoadMotion(cmd, mname, a->motion, a->motion_label, merr, wid)) a->have_motion = a->pose_motion = true;
    }
    a->frame = fr;
    if (a->have_motion && a->motion.root_delta == 1 && !cmd.has("frame")) a->frame = StanceFrame(a->motion);  // (Session 98: a stance stands at its bell frame)
    if (a->have_motion && a->motion.length > 0 && a->frame > a->motion.length) a->frame = std::fmod(a->frame, (double)a->motion.length);
    Actor* old = FindActor(id);  // an actor of the same id is replaced once the new one is in
    Actor* np = a.get();
    if (!Install(std::move(a), err)) {
      if (err.rfind("attire ", 0) == 0 && err.find(" does not exist (wrestler ") != std::string::npos)
        return "{\"ok\":false,\"no_attire\":true,\"error\":\"" + jl::Esc(err) + "\"}";
      return Err(err);
    }
    if (old)
      for (size_t i = 0; i < actors.size(); ++i) if (actors[i].get() == old) { RemoveActorAt(i); break; }
    std::string reply = ActorJson(*np);
    reply.pop_back();
    return "{\"ok\":true," + reply.substr(1) + ",\"note\":\"" + jl::Esc(s.note) + "\"" + (merr.empty() ? "" : ",\"motion_error\":\"" + jl::Esc(merr) + "\"") + "}";
  }
  // Session 98 (custom characters, srcssets\char_mods.h): the CHARACTERS part of the mods folder.
  //   {"cmd":"char_import","path":"x.pac"|"x.zip"}  copy into <mods>\characters\<safe name>.pac (a zip: only its .pac members)
  //   {"cmd":"char_list"}                           the imported characters (+ attires, bytes); {"cmd":"char_remove","name":..,"confirm":true} = Recycle Bin
  std::string DoChars(const std::string& c, const jl::Value& cmd) {
    namespace cm = assets::char_mods;
    namespace ml = assets::mods_lookup;
    if (c == "char_import") {
      if (!cmd.has("path")) return Err("missing path");
      std::vector<std::string> names, warns;
      const std::string why = cm::Import(cmd.str("path", ""), names, [](const std::string& p, std::string& w) { return ViewerCheckCharacterFile(p, w); }, &warns);
      if (!why.empty()) return Err(why);
      std::string a, wa;
      for (const std::string& n : names) a += (a.empty() ? "" : ",") + std::string("\"") + jl::Esc(n) + "\"";
      for (const std::string& n : warns) wa += (wa.empty() ? "" : ",") + std::string("\"") + jl::Esc(n) + "\"";
      return "{\"ok\":true,\"imported\":[" + a + "],\"warnings\":[" + wa + "]," + ModsState() + "}";
    }
    if (c == "char_list") {
      std::string a;
      for (const cm::Entry& e : cm::List(true)) {
        const int at = ViewerCustomAttires(e.id);
        a += (a.empty() ? "" : ",") + std::string("{\"name\":\"") + jl::Esc(e.name) + "\",\"id\":" + std::to_string(e.id) + ",\"attires\":" + std::to_string(at) +
             ",\"bytes\":" + std::to_string(e.bytes) + "}";
      }
      return "{\"ok\":true,\"dir\":\"" + jl::Esc(ml::U8(cm::Dir())) + "\",\"shown\":" + (ml::Cfg().on ? "true" : "false") + ",\"characters\":[" + a + "]}";
    }
    const jl::Value* cv = cmd.get("confirm");
    if (!cv || cv->t != jl::Value::Bool || !cv->b) return Err("char_remove needs \"confirm\":true (the file goes to the Recycle Bin)");
    cm::Entry e;
    if (!cm::FindName(cmd.str("name", ""), e)) return Err("no imported character '" + cmd.str("name", "") + "'");
    for (size_t i = actors.size(); i-- > 0;)  // (an actor built from it leaves the scene first)
      if (actors[i]->wrestler == e.id) RemoveActorAt(i);
    std::string err;
    if (!ml::Plat().recycle) return Err("no Recycle Bin on this platform");
    if (!ml::Plat().recycle(e.file, err)) return Err(err);
    return "{\"ok\":true,\"removed\":\"" + jl::Esc(e.name) + "\"}";
  }
  // ---- Session 98 (Superstar Importer, --importer; ui_importer.h) -------------------------------------------------------------
  // The importer shows ONE character (the actor "imp") at the origin with the game's match wrestler light (arena set 10, camera-attached:
  // Install's evidence), no arena around him: an arena is loaded once only to take its set 10, then its model is dropped (arena_orig and
  // last_arena stay, so later places keep the match light). The pac is copied into a private preview folder (the viewer's mods folder
  // in this mode, never the player's: char_mods.h Import's checks + the seam's test load), one at a time.
  bool importer = false;
  ui::ImporterHost imp;
  bool imp_have = false, imp_lit = false;
  std::string imp_path, imp_name;
  float imp_c[3] = {0, 90, 0}, imp_min_y = 0, imp_max_y = 180, imp_h = 180, imp_zoom = 1;
  // Jarrod (S98): "right click to move the camera preview ... if I zoom in on his body, I can't get to the model's head".
  // The pan moves the framed point in the view plane (right-drag / two fingers); a new character starts centred.
  float imp_pan[3] = {0, 0, 0};
  bool imp_panning = false;
  int imp_px = 0, imp_py = 0;
  int imp_last_w = 0, imp_last_h = 0;
  std::string DoImporterPreview(const jl::Value& cmd) {
    namespace cm = assets::char_mods;
    if (cmd.has("clear")) {
      for (size_t i = actors.size(); i-- > 0;)
        if (actors[i]->id == "imp") RemoveActorAt(i);
      imp_have = false, imp_path.clear();
      return "{\"ok\":true}";
    }
    const std::string path = cmd.str("path", "");
    if (path.empty()) return Err("missing path");
    const bool entr = cmd.str("attire", "ring") == "entrance";
    std::vector<std::string> warns;
    cm::Entry have;
    if (path != imp_path || !cm::FindName(imp_name, have)) {
      for (size_t i = actors.size(); i-- > 0;)
        if (actors[i]->id == "imp") RemoveActorAt(i);
      std::error_code ec;
      if (!cm::Dir().empty()) std::filesystem::remove_all(cm::Dir(), ec);  // (the preview folder holds the shown one only)
      std::vector<std::string> names;
      const std::string why = cm::Import(path, names, [](const std::string& p, std::string& w) { return ViewerCheckCharacterFile(p, w); }, &warns);
      if (!why.empty()) return imp_have = false, Err(why);
      if (names.empty()) return imp_have = false, Err("no character in the file");
      imp_path = path, imp_name = names.front();
    }
    cm::Entry ce;
    if (!cm::FindName(imp_name, ce)) return Err("the preview copy is gone");
    bool arena_in = false;
    if (!imp_lit) {  // the match light: the first arena whose set 10 is lit (one load per importer session)
      for (int aid : ViewerList("arena")) {
        const std::string r2 = Safe("{\"cmd\":\"load\",\"kind\":\"arena\",\"id\":" + std::to_string(aid) + "}");
        if (r2.find("\"ok\":true") == std::string::npos) continue;
        arena_in = true;
        for (const assets::LightSet& s : arena_orig)
          if (s.id == 10) {
            float tot = 0;
            for (int i = 0; i < 4; ++i) for (int k = 0; k < 3; ++k) tot += s.col[i][k];
            imp_lit = tot > 1e-4f;
          }
        if (imp_lit) break;
      }
      if (!imp_lit) fprintf(stderr, "importer: no arena with a lit set 10: the default rig lights the preview\n");
    }
    int attire = 0;
    const int ea = ViewerEntranceAttire(ce.id, 0);
    if (entr) {
      if (ea < 0) return Err("this pac has no entrance attire");
      attire = ea;
    }
    const std::string pr = Safe("{\"cmd\":\"place\",\"id\":\"imp\",\"custom\":\"" + jl::Esc(imp_name) + "\",\"attire\":" + std::to_string(attire) +
                                ",\"pos\":[0,0,0],\"yaw\":0,\"motion\":\"idle\"}");
    if (arena_in) {  // the arena only lent its light: out of the scene (model 0 = the arena of the plain load above)
      for (size_t mi = 0; mi < r.ModelCount(); ++mi)
        if (!ActorOf(mi)) { RemoveModelIndex(mi); break; }
    }
    if (pr.find("\"ok\":true") == std::string::npos) return imp_have = false, pr;
    Actor* a = FindActor("imp");
    float mn[3] = {-50, 0, -50}, mx[3] = {50, 180, 50};
    if (a && a->model < r.Models().size() && r.Models()[a->model].has_wrestler) {
      const GpuModel& g = r.Models()[a->model];
      std::memcpy(mn, g.bmin, sizeof mn), std::memcpy(mx, g.bmax, sizeof mx);
    } else if (r.HasModel()) r.Bounds(mn, mx);
    for (int i = 0; i < 3; ++i) imp_c[i] = (mn[i] + mx[i]) * 0.5f;
    imp_min_y = a ? std::min(mn[1], a->floor_y) : mn[1], imp_max_y = mx[1], imp_h = std::max(20.f, imp_max_y - imp_min_y);
    imp_c[1] = imp_min_y + imp_h * 0.5f;
    imp_pan[0] = imp_pan[1] = imp_pan[2] = 0;
    if (!imp_have) cam.free = false, cam.use_vp = false, cam.yaw = 0, cam.pitch = 4  /* (yaw 0: his front, seen in the first test sheet) */, cam.fov = 30, imp_zoom = 1;
    imp_have = true;
    ImporterFrame(true);
    std::string wa;
    for (const std::string& n : warns) wa += (wa.empty() ? "" : ",") + std::string("\"") + jl::Esc(n) + "\"";
    return "{\"ok\":true,\"name\":\"" + jl::Esc(imp_name) + "\",\"attires\":" + std::to_string(ViewerCustomAttires(ce.id)) + ",\"has_entrance\":" +
           (ea >= 0 ? "true" : "false") + ",\"light\":\"" + (imp_lit ? "match" : "rig") + "\",\"height\":" + Fix(imp_h, 1) + ",\"warnings\":[" + wa + "]}";
  }
  // Every windowed frame: the orbit camera framed so the model's centre sits in the middle of the UI's preview rectangle (an off-centre
  // look: the target is moved in the view plane), whatever the drag turned it to; the wheel / pinch zoom is imp_zoom.
  void ImporterFrame(bool force = false) {
    if (!importer || !imp_have || cam.free || cam.use_vp) return;
    const int W = std::max(1, r.Width()), H = std::max(1, r.Height());
    float x0 = imp.view[0], y0 = imp.view[1], x1 = imp.view[2], y1 = imp.view[3];
    if (x1 - x0 < 8 || y1 - y0 < 8) x0 = 0, y0 = 0, x1 = float(W), y1 = float(H);
    (void)force;
    const float tanv = std::tan(cam.fov * kDeg * 0.5f), rh = (y1 - y0) / float(H);
    cam.dist = std::max(20.f, imp_h * 1.12f / rh * 0.5f / tanv * imp_zoom);
    const float fx = (x0 + x1) / W - 1.f, fy = 1.f - (y0 + y1) / H;
    const float p = cam.pitch * kDeg, y = cam.yaw * kDeg;
    const V3 o = FromUpFrame(std::sin(p), std::cos(p) * std::sin(y), std::cos(p) * std::cos(y));  // target -> eye
    const V3 z = {-o.x, -o.y, -o.z}, rt = Norm(Cross(UpVector(), z)), up = Cross(z, rt);
    const float hh = cam.dist * tanv, hw = hh * float(W) / float(H);
    const float c[3] = {imp_c[0] + imp_pan[0], imp_c[1] + imp_pan[1], imp_c[2] + imp_pan[2]};
    cam.target[0] = c[0] - rt.x * fx * hw - up.x * fy * hh;
    cam.target[1] = c[1] - rt.y * fx * hw - up.y * fy * hh;
    cam.target[2] = c[2] - rt.z * fx * hw - up.z * fy * hh;
  }
  // The pan: a move of (dx, dy) screen pixels moves the picture with the cursor / fingers (the framed point goes the other way
  // in the view plane, at the depth of the model). Kept within ~0.6 of his height of the centre so he cannot be lost.
  void ImporterPan(float dx, float dy) {
    if (!importer || !imp_have || cam.free || cam.use_vp) return;
    const int H = std::max(1, r.Height());
    const float upp = 2.f * cam.dist * std::tan(cam.fov * kDeg * 0.5f) / float(H);  // world units per pixel at the model
    const float p = cam.pitch * kDeg, y = cam.yaw * kDeg;
    const V3 o = FromUpFrame(std::sin(p), std::cos(p) * std::sin(y), std::cos(p) * std::cos(y));
    const V3 z = {-o.x, -o.y, -o.z}, rt = Norm(Cross(UpVector(), z)), up = Cross(z, rt);
    for (int i = 0; i < 3; i++) imp_pan[i] += (-(&rt.x)[i] * dx + (&up.x)[i] * dy) * upp;
    const float len = std::sqrt(imp_pan[0] * imp_pan[0] + imp_pan[1] * imp_pan[1] + imp_pan[2] * imp_pan[2]), lim = imp_h * 0.6f;
    if (len > lim) for (float& v : imp_pan) v *= lim / len;
    ImporterFrame();
  }
  // A roster render (ui_importer.h ImporterHost::portrait): the model from the preview's side angle, front-lit by the camera-attached
  // match light, rendered twice (over black, over white) at 2x; alpha = 1 - (white - black), colour = black / alpha, then 2x2 down.
  // Crops [docs\formats\roster_images.md, pack 4's renders]: A head to belt, B head to the top of the trunks, C head to hips with room.
  bool ImporterPortrait(int kind, std::vector<uint8_t>& rgba, int& w, int& h, std::string& err) {
    if (!imp_have) return err = "load a character first", false;
    if (kind < 0 || kind > 2) return err = "kind 0-2", false;
    const int out = kind == 2 ? 256 : 512, ss = 2, n = out * ss;
    static const float kBottom[3] = {0.555f, 0.47f, 0.43f};  // the crop's lower edge, as a share of his height from the floor
    const float top = imp_max_y + imp_h * (kind == 2 ? 0.06f : 0.025f), bot = imp_min_y + imp_h * kBottom[kind];
    float span = top - bot;
    Camera pc;
    pc.free = false, pc.use_vp = false, pc.fov = 18;
    pc.yaw = cam.yaw, pc.pitch = std::clamp(cam.pitch, -10.f, 12.f);
    pc.target[0] = imp_c[0], pc.target[1] = (top + bot) * 0.5f, pc.target[2] = imp_c[2];
    if (kind == 2) span *= 1.1f, pc.target[1] -= span * 0.02f;
    pc.dist = span * 0.5f / std::tan(pc.fov * kDeg * 0.5f);
    const Camera keep = cam;
    cam = pc;
    match_cam_sig[0] = -1e30f;
    MatchLightsFollowCamera();
    std::vector<uint8_t> blk, wht;
    bool ok = true;
    r.SetClearColour(0, 0, 0);
    ok = r.ShotPixels(pc, n, n, blk, err);
    r.SetClearColour(1, 1, 1);
    if (ok) ok = r.ShotPixels(pc, n, n, wht, err);
    ImporterBackground();
    cam = keep;
    match_cam_sig[0] = -1e30f;
    MatchLightsFollowCamera();
    if (!ok) return false;
    if (blk.size() != size_t(n) * n * 4 || wht.size() != blk.size()) return err = "render size mismatch", false;
    rgba.assign(size_t(out) * out * 4, 0);
    for (int yy = 0; yy < out; ++yy)
      for (int xx = 0; xx < out; ++xx) {
        float acc[4] = {0, 0, 0, 0};
        for (int j = 0; j < ss; ++j)
          for (int i = 0; i < ss; ++i) {
            const size_t q = (size_t(yy * ss + j) * n + size_t(xx * ss + i)) * 4;
            const float d = ((wht[q] - blk[q]) + (wht[q + 1] - blk[q + 1]) + (wht[q + 2] - blk[q + 2])) / (3.f * 255.f);
            const float a = std::clamp(1.f - d, 0.f, 1.f);
            acc[0] += blk[q + 2], acc[1] += blk[q + 1], acc[2] += blk[q];  // BGRA -> RGB, premultiplied (over black)
            acc[3] += a;
          }
        uint8_t* o = &rgba[(size_t(yy) * out + xx) * 4];
        const float a = acc[3] / float(ss * ss);
        if (a > 0.004f)
          for (int c = 0; c < 3; ++c) o[c] = uint8_t(std::clamp(acc[c] / float(ss * ss) / a, 0.f, 255.f));
        o[3] = uint8_t(std::clamp(a * 255.f + 0.5f, 0.f, 255.f));
      }
    w = h = out;
    return true;
  }
  void ImporterBackground() { r.SetClearColour(0.17f, 0.18f, 0.21f); }  // the preview's backdrop: a neutral dark grey

  // {"cmd":"spot_cam","spot":"ramp","t":0.5} or {"actor":"a1"}: a camera framing the spot (an actor standing there is seen from the
  // front, "side" degrees to the side). Optional: dist (320), pitch (8), fov (40), side (25), height (92, the look-at height).
  // Session 98 (plan step 14, the mod workbench; src\assets\mods_lookup.h = the game's own lookup rules):
  //   {"cmd":"mods","dir":"C:\\...\\mods","on":true|false}   set the mods folder (the toggle defaults to on when it exists) and / or the toggle
  //   {"cmd":"mods_list"}                                   the folder's trons + banners and the game's tron numbers
  //   {"cmd":"mods_select","tron":N}                        decode the mod's frame (cached) and show tron N on the stage (entrance look)
  //   {"cmd":"mods_add","tron":N,"file":"x.mp4"}            copy into <mods>\titantron\NNN.<ext> (the launcher's rule), then show it
  //   {"cmd":"mods_remove","tron":N,"confirm":true}         move the mod file to the Recycle Bin
  // The stage re-loads (camera kept) whenever the shown tron's source changes.
  std::string ModsRestage(int tron) {
    if (stage_wrestler < 0 || (tron >= 0 && tron != stage_wrestler)) return "";
    const std::string md = last_media;
    return Safe("{\"cmd\":\"stage\",\"wrestler\":" + std::to_string(stage_wrestler) + (md.empty() ? "" : ",\"media\":\"" + jl::Esc(md) + "\"") + "}");
  }
  std::string ModsState() {
    namespace ml = assets::mods_lookup;
    return "\"mods_dir\":\"" + jl::Esc(ml::U8(ml::Cfg().dir)) + "\",\"mods_on\":" + (ml::Cfg().on ? "true" : "false");
  }
  std::string DoMods(const std::string& c, const jl::Value& cmd) {
    namespace ml = assets::mods_lookup;
    if (c == "mods") {
      if (cmd.has("dir")) {
        const std::string why = ml::SetDir(std::filesystem::u8path(cmd.str("dir")));
        ml::Reindex();
        if (!why.empty()) return Err(why);
      }
      if (const jl::Value* ov = cmd.get("on"); ov && ov->t == jl::Value::Bool) ml::Cfg().on = ov->b && !ml::Cfg().dir.empty();
      const std::string rs = ModsRestage(-1);
      return "{\"ok\":true," + ModsState() + (rs.empty() ? "" : ",\"restaged\":true") + "}";
    }
    if (c == "mods_refresh") {  // the panel: a decode finished, re-stage with the mod
      ModsRestage(-1);
      return "{\"ok\":true}";
    }
    if (c == "mods_list") {
      std::string a, b;
      for (const ml::Entry& e : ml::ScanTrons())
        a += std::string(a.empty() ? "" : ",") + "{\"tron\":" + std::to_string(e.id) + ",\"file\":\"" + jl::Esc(ml::U8(e.file.filename())) +
             "\",\"bytes\":" + std::to_string(e.bytes) + ",\"bink\":" + (e.bink ? "true" : "false") + "}";
      for (const auto& bn : ml::ScanBanners())
        b += std::string(b.empty() ? "" : ",") + "{\"tron\":" + std::to_string(bn.first) + ",\"mode\":\"" + ml::BannerName(bn.second.mode) + "\"}";
      std::vector<int> game;
      std::string used;
      ViewerStageWrestlers("", game, used);
      std::string g;
      for (int id : game) g += (g.empty() ? "" : ",") + std::to_string(id);
      return "{\"ok\":true," + ModsState() + ",\"trons\":[" + a + "],\"banners\":[" + b + "],\"game_trons\":[" + g + "]}";
    }
    const int tron = IntArg(cmd.num("tron", -1));
    if (tron < 0 || tron > ml::kMaxTron) return Err("tron must be 0-9999 (the game reads at most 4 digits)");
    if (c == "mods_add") {
      const std::string e = ml::AssignTron(tron, std::filesystem::u8path(cmd.str("file")), cmd.has("stacked"));
      if (!e.empty()) return Err(e);
      const std::string f = ml::U8(ml::TronFile(tron).filename());
      std::string r = DoMods("mods_select", cmd);
      if (r.find("\"ok\":true") == std::string::npos) return r;
      return r.insert(r.size() - 1, ",\"added\":\"" + jl::Esc(f) + "\""), r;
    }
    if (c == "mods_remove") {
      const jl::Value* cv = cmd.get("confirm");
      if (!cv || cv->t != jl::Value::Bool || !cv->b) return Err("mods_remove needs \"confirm\":true (the file goes to the Recycle Bin)");
      const std::string e = ml::RemoveTron(tron);
      if (!e.empty()) return Err(e);
      ModsRestage(tron);
      return "{\"ok\":true,\"removed\":" + std::to_string(tron) + "}";
    }
    if (c == "mods_sheet") {  // debug: {"cmd":"mods_sheet","tron":N,"path":"x.ppm"}: the 320x320 sheet the stage gets (original strips left black)
      const assets::mods_lookup::Frame s = ml::ModSheet(tron, nullptr);
      if (!s.ok()) return Err("no mod sheet for tron " + std::to_string(tron) + " (no mod, toggle off, or no decodable frame)");
      std::ofstream f(std::filesystem::u8path(cmd.str("path")), std::ios::binary);
      if (!f) return Err("cannot write " + cmd.str("path"));
      f << "P6\n" << s.w << " " << s.h << "\n255\n";
      for (size_t i = 0; i < s.rgba.size(); i += 4) f.write(reinterpret_cast<const char*>(&s.rgba[i]), 3);
      return "{\"ok\":true,\"w\":" + std::to_string(s.w) + ",\"h\":" + std::to_string(s.h) + "}";
    }
    // mods_select
    ui.ModsSelect(tron);
    const std::filesystem::path f = ml::TronFor(tron);
    std::string src = "game", note, derr;
    if (!f.empty()) {
      if (ml::IsBink(f)) src = "mod", note = "Bink 2 mod: no preview here (black sheet)";
      else if (cmd.has("async")) {  // the UI: never block; the worker bumps DecodeGen and the panel asks for mods_refresh
        const int st = ml::StateOf(tron);
        if (st == 0) ml::StartDecode(tron), note = "decoding your video...";
        else if (st == 1) src = "mod";
        else note = ml::DecodeError(tron);
      } else if (ml::EnsureFrames(tron, derr)) src = "mod";
      else note = derr;
    } else if (!ml::TronFile(tron).empty()) note = "the mod is switched off";
    if (stage_wrestler != tron || last_arena < 0) {
      stage_wrestler = tron;
      const std::string md = last_media;
      std::string r = Safe("{\"cmd\":\"stage\",\"wrestler\":" + std::to_string(tron) + (md.empty() ? "" : ",\"media\":\"" + jl::Esc(md) + "\"") + "}");
      if (r.find("\"ok\":true") == std::string::npos) return r;
    } else {
      ModsRestage(tron);
    }
    return "{\"ok\":true,\"tron\":" + std::to_string(tron) + ",\"source\":\"" + src + "\"" +
           (f.empty() ? "" : ",\"file\":\"" + jl::Esc(ml::U8(f.filename())) + "\"") + (note.empty() ? "" : ",\"note\":\"" + jl::Esc(note) + "\"") + "," + ModsState() + "}";
  }

  std::string DoSpotCam(const jl::Value& cmd) {
    float p[3];
    float yaw = 0;
    std::string name;
    if (cmd.has("actor")) {
      Actor* a = FindActor(IdArg(cmd, "actor"));
      if (!a) return Err("no actor '" + IdArg(cmd, "actor") + "'");
      p[0] = a->pos[0], p[1] = a->floor_y, p[2] = a->pos[2], yaw = a->yaw, name = a->spot;
    } else {
      std::string err;
      assets::spots::Spot s;
      const std::string spot = cmd.str("spot", "ring");
      if (!EnsureSpots(err, spot == "stage" || spot == "ramp") || !sp.Resolve(spot, cmd.num("t", 0.5), s, err)) return Err(err);
      for (int i = 0; i < 3; i++) p[i] = s.pos[i];
      yaw = cmd.has("yaw") ? (float)cmd.num("yaw", 0) : s.yaw, name = s.name;
    }
    cam.use_vp = false;
    cam.target[0] = p[0], cam.target[1] = p[1] + (float)cmd.num("height", 92), cam.target[2] = p[2];
    cam.yaw = yaw + (float)cmd.num("side", 25);
    // (ringside: the ring's apron hides the feet from the ring side, so the default view is from above)
    cam.pitch = std::max(-89.5f, std::min(89.5f, (float)cmd.num("pitch", name == "ringside" ? 28 : 8)));
    cam.dist = std::max(1.0f, (float)cmd.num("dist", 320));
    cam.fov = std::max(5.0f, std::min(120.0f, (float)cmd.num("fov", 40)));
    return "{\"ok\":true,\"spot\":\"" + jl::Esc(name) + "\",\"at\":" + Vec3(p) + ",\"facing_yaw\":" + Fix(yaw, 1) + "," + CamJson() + "}";
  }
  // Tab / Shift+Tab: the camera visits the spots one after the other.
  void CycleSpot(int dir) {
    const auto& names = assets::spots::ArenaSpots::Cycle();
    spot_cycle = (spot_cycle + dir + (int)names.size() * 2) % (int)names.size();
    const std::string reply = Safe("{\"cmd\":\"spot_cam\",\"spot\":\"" + names[(size_t)spot_cycle] + "\",\"t\":0.5}");
    status = reply.find("\"ok\":true") != std::string::npos ? "spot: " + names[(size_t)spot_cycle] : "spot_cam: " + reply;
  }

  // ---- Session 98 (plan step 12): camera paths + offline render ----
  PathUi pu;
  RenderJob job;
  std::map<std::string, double> scene_base;  // actor id -> its motion frame at the base time (the scene clock's zero)
  double scene_base_global = 0, scene_t0 = 0;
  bool scene_base_valid = false;
  std::chrono::steady_clock::time_point path_last = std::chrono::steady_clock::now();

  void CaptureSceneBase(double t0) {
    scene_base.clear();
    for (auto& a : actors) scene_base[a->id] = a->frame;
    scene_base_global = frame, scene_t0 = t0, scene_base_valid = true;
    ent.scene_base_t = ent.t;
  }
  static double WrapFrame(double f, double len) {
    f = std::fmod(f, len);
    return f < 0 ? f + len : f;
  }
  // The scene clock at path time t: every playing motion at its base frame + (t - t0) * 60.
  void SceneAt(double t) {
    if (!scene_base_valid) return;
    const double rel = t - scene_t0;
    for (auto& a : actors) {
      auto it = scene_base.find(a->id);
      if (a->entrance || it == scene_base.end() || !a->have_motion || !a->pose_motion || a->motion.length <= 0) continue;
      a->frame = WrapFrame(it->second + rel * 60.0, (double)a->motion.length);
      PoseActor(*a);
      PlaceXform(*a, a->ground_play, false);
    }
    if (ent.active) {  // the entrance runs on its own timeline (cues, camera, lights): base + path time, the path owns the camera
      ent.t = std::min(ent.tl.total, std::max(0.0, ent.scene_base_t + rel * 60.0));
      ApplyEntrance(false);
    }
    if (have_motion && pose_motion && cur_motion.length > 0) {
      frame = WrapFrame(scene_base_global + rel * 60.0, (double)cur_motion.length);
      ApplyPose();
    }
  }
  CamPose CurrentPose() const {
    CamPose p;
    Camera tmp = cam;
    if (cam.free) std::memcpy(p.pos, cam.fpos, sizeof p.pos), p.yaw = cam.fyaw, p.pitch = cam.fpitch, p.roll = cam.froll;
    else FreeFromOrbit(tmp, p.pos, p.yaw, p.pitch);
    p.fov = cam.fov;
    return p;
  }
  void ApplyPathCam(double t) {
    CamPose p;
    if (!pu.path.Eval(pu.path.Wrap(t), p)) return;
    EnterFree(cam, fc);
    std::memcpy(cam.fpos, p.pos, sizeof p.pos);
    cam.fyaw = p.yaw, cam.fpitch = p.pitch, cam.froll = p.roll, cam.fov = p.fov;
    fc.fov_target = fc.last_fov = cam.fov;
    std::memset(fc.vel, 0, sizeof fc.vel);
    fc.roll_rate = fc.look_yaw = fc.look_pitch = 0;
  }
  void PathAt(double t) {  // the camera (and, linked, the scene) at path time t
    pu.time = t;
    ApplyPathCam(t);
    if (pu.link) {
      if (!scene_base_valid) CaptureSceneBase(pu.path.Start());
      SceneAt(t);
    }
  }
  void PathTick() {  // windowed main loop: advance a playing preview by the host clock
    const auto now = std::chrono::steady_clock::now();
    const double dt = std::min(0.1, std::chrono::duration<double>(now - path_last).count());
    path_last = now;
    if (!pu.playing || job.Active()) return;
    if (pu.path.keys.size() < 2) { pu.playing = false; return; }
    double t = pu.time + dt;
    if (t >= pu.path.End()) {
      if (pu.path.loop) t = pu.path.Wrap(t);
      else t = pu.path.End(), pu.playing = false;
    }
    PathAt(t);
  }
  void PathPlay(bool on) {
    pu.playing = on;
    if (on) {
      if (pu.time >= pu.path.End() - 1e-6 && !pu.path.loop) pu.time = pu.path.Start();
      if (pu.link) CaptureSceneBase(pu.time);
      path_last = std::chrono::steady_clock::now();
    }
  }
  void PathKeyAdd() {  // the K key
    double t = pu.time;
    for (const CamKey& k : pu.path.keys) if (std::fabs(k.t - t) < 1e-4) { t = pu.path.End() + 1.0; break; }
    pu.time = t;
    status = "path: " + Safe("{\"cmd\":\"path_add\",\"t\":" + Fix(t, 4) + "}").substr(0, 60);
  }
  static std::string PathsDir() {
    auto s = (std::filesystem::u8path(LocalDataDir()) / "wwe2k14" / "viewer" / "paths").u8string();
    return std::string(s.begin(), s.end());
  }
  // A bare name goes in the default folder (".json" added); anything with a folder part is used as given.
  static std::string ResolvePath(const std::string& p) {
    std::filesystem::path fp = std::filesystem::u8path(p);
    if (!fp.has_parent_path()) {
      if (!fp.has_extension()) fp += ".json";
      { auto s = (std::filesystem::u8path(PathsDir()) / fp).u8string(); return std::string(s.begin(), s.end()); }
    }
    return p;
  }
  std::string PathListJson() const {
    std::string s = "{\"ok\":true,\"count\":" + std::to_string(pu.path.keys.size()) + ",\"duration\":" + Fix(pu.path.Duration(), 4) + ",\"loop\":" +
                    (pu.path.loop ? "true" : "false") + ",\"link\":" + (pu.link ? "true" : "false") + ",\"time\":" + Fix(pu.time, 4) +
                    ",\"playing\":" + (pu.playing ? "true" : "false") + ",\"keys\":[";
    for (size_t i = 0; i < pu.path.keys.size(); i++) s += (i ? "," : "") + CamPath::KeyJson(pu.path.keys[i]);
    return s + "]}";
  }
  static bool BoolArg(const jl::Value& cmd, const char* k, bool d) {
    const jl::Value* v = cmd.get(k);
    return v ? (v->t == jl::Value::Bool ? v->b : v->n != 0) : d;
  }
  std::string RenderStatusJson() { return job.StatusJson(); }
  void SyncRenderUi() {
    pu.rendering = job.Active(), pu.frame = job.Frame(), pu.frames = job.Frames(), pu.render_msg = job.Message(), pu.render_out = job.OutDir();
  }
  // {"cmd":"render","path":"file.json"|"current","out":F,"w","h","fps","ss","motion_blur":0..1,"samples","mp4":bool,"scene":bool,"t0","t1","wait":bool}
  std::string DoRender(const jl::Value& cmd) {
    CamPath use;
    const std::string ps = cmd.str("path", "current");
    if (ps == "current" || ps.empty()) use = pu.path;
    else {
      std::ifstream f(std::filesystem::u8path(ResolvePath(ps)), std::ios::binary);
      if (!f) return Err("cannot open path file " + ResolvePath(ps));
      std::string txt((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()), e;
      if (!use.FromJson(txt, e)) return Err("path file: " + e);
    }
    RenderOpts o;
    o.out = cmd.str("out");
    o.w = IntArg(cmd.num("w", 1920)), o.h = IntArg(cmd.num("h", 1080)), o.fps = IntArg(cmd.num("fps", 30));
    o.ss = IntArg(cmd.num("ss", 1)), o.samples = IntArg(cmd.num("samples", 1)), o.motion_blur = (float)cmd.num("motion_blur", 0);
    o.t0 = cmd.num("t0", use.Start()), o.t1 = cmd.num("t1", use.End());
    o.mp4 = BoolArg(cmd, "mp4", false);
    if (!cmd.str("mp4_name").empty()) o.mp4_name = cmd.str("mp4_name");
    const bool scene = BoolArg(cmd, "scene", true);
    RenderHooks hk;
    hk.begin = [this, scene] {
      pu.playing = false;
      render_saved.clear();
      for (auto& a : actors) render_saved[a->id] = a->frame;
      render_saved_global = frame;
      render_saved_ent_t = ent.t;
      render_saved_valid = scene;
      if (scene) CaptureSceneBase(job_t0);
      else scene_base_valid = false;
      tron_rend_ent0 = ent.t, tron_rend_t0 = job_t0;  // tron video: where its clock stands when the render starts
      tron_rend_base = ent.active ? tron.EntranceVideoTime(ent.t) : tron.clock;
    };
    hk.scene = [this, scene](double t) {
      if (scene) SceneAt(t);
      // the tron video at the frame's scene time (frame-exact decode): follows the entrance clock when something drives it, else
      // starts where the clock stood and runs at the entrance speed (1x in the plain preview)
      const bool driven = ent.active && ent.t != tron_rend_ent0;
      TronNow(true, driven ? tron.EntranceVideoTime(ent.t) : tron_rend_base + (t - tron_rend_t0) * (ent.active ? ent.speed : 1.0));
    };
    hk.end = [this] {
      if (!render_saved_valid) return;
      for (auto& a : actors) {
        auto it = render_saved.find(a->id);
        if (a->entrance || it == render_saved.end() || !a->have_motion) continue;
        a->frame = it->second;
        PoseActor(*a);
        PlaceXform(*a, a->ground_play, false);
      }
      if (ent.active) ent.t = render_saved_ent_t, ApplyEntrance(false);
      if (have_motion) { frame = render_saved_global; ApplyPose(); }
      render_saved_valid = false;
      scene_base_valid = false;
    };
    job_t0 = o.t0;
    std::string err;
    if (!job.Start(&r, use, cam, o, std::move(hk), err)) return Err(err);
    std::string started = "{\"ok\":true,\"started\":true,\"frames\":" + std::to_string(job.Frames()) + ",\"out\":\"" + jl::Esc(o.out) + "\"}";
    SyncRenderUi();  // (the render runs one frame per main-loop pass; poll render_status; a --script run finishes it before the next line)
    return started;
  }
  std::map<std::string, double> render_saved;
  double render_saved_global = 0, render_saved_ent_t = 0, job_t0 = 0;
  bool render_saved_valid = false;

  // Returns true (and fills `out`) when `c` is a path_* / render command.
  bool DoPathCmd(const std::string& c, const jl::Value& cmd, std::string& out) {
    if (c == "render") { out = DoRender(cmd); return true; }
    if (c == "render_status") { out = job.StatusJson(); return true; }
    if (c == "render_cancel") { job.Cancel(); SyncRenderUi(); out = job.StatusJson(); return true; }
    if (c.compare(0, 5, "path_") != 0) return false;
    if (c == "path_add") {
      CamKey k;
      k.t = pu.time;
      k.p = CurrentPose();
      std::string e;
      if (!CamPath::KeyFrom(cmd, k, e)) { out = Err(e); return true; }
      const int i = pu.path.Add(k);
      pu.selected = i;
      out = "{\"ok\":true,\"index\":" + std::to_string(i) + ",\"count\":" + std::to_string(pu.path.keys.size()) + ",\"key\":" + CamPath::KeyJson(pu.path.keys[(size_t)i]) + "}";
      return true;
    }
    if (c == "path_clear") { pu.path.keys.clear(), pu.selected = -1, pu.time = 0, pu.playing = false; out = "{\"ok\":true}"; return true; }
    if (c == "path_list") { out = PathListJson(); return true; }
    if (c == "path_set_key" || c == "path_delete") {
      const int i = IntArg(cmd.num("index", -1));
      if (i < 0 || i >= (int)pu.path.keys.size()) { out = Err("index out of range (0.." + std::to_string((int)pu.path.keys.size() - 1) + ")"); return true; }
      if (c == "path_delete") {
        pu.path.Remove(i);
        pu.selected = std::min(pu.selected, (int)pu.path.keys.size() - 1);
        out = "{\"ok\":true,\"count\":" + std::to_string(pu.path.keys.size()) + "}";
        return true;
      }
      CamKey k = pu.path.keys[(size_t)i];
      if (BoolArg(cmd, "from_camera", false)) k.p = CurrentPose();
      std::string e;
      if (!CamPath::KeyFrom(cmd, k, e)) { out = Err(e); return true; }
      const int ni = pu.path.Set(i, k);
      if (ni == -2) { out = Err("another key is already at that time"); return true; }
      pu.selected = ni;
      out = "{\"ok\":true,\"index\":" + std::to_string(ni) + ",\"key\":" + CamPath::KeyJson(pu.path.keys[(size_t)ni]) + "}";
      return true;
    }
    if (c == "path_set") {
      pu.path.loop = BoolArg(cmd, "loop", pu.path.loop);
      const bool nl = BoolArg(cmd, "link", pu.link);
      if (nl && !pu.link) scene_base_valid = false;  // (re-captured at the next play / scrub)
      pu.link = nl;
      out = PathListJson();
      return true;
    }
    if (c == "path_time") {
      if (cmd.has("t")) {
        if (pu.path.keys.empty()) { out = Err("the path has no keys"); return true; }
        const double t = cmd.num("t", 0);
        if (BoolArg(cmd, "capture", false)) CaptureSceneBase(t);
        PathAt(t);
      }
      CamPose p;
      pu.path.Eval(pu.path.Wrap(pu.time), p);
      out = "{\"ok\":true,\"time\":" + Fix(pu.time, 4) + ",\"pos\":" + Vec3(p.pos, 4) + ",\"yaw\":" + Fix(p.yaw, 4) + ",\"pitch\":" + Fix(p.pitch, 4) +
            ",\"roll\":" + Fix(p.roll, 4) + ",\"fov\":" + Fix(p.fov, 4) + "}";
      return true;
    }
    if (c == "path_play") {
      if (pu.path.keys.size() < 2 && BoolArg(cmd, "on", true)) { out = Err("a path needs 2+ keys to play"); return true; }
      if (cmd.has("from")) pu.time = cmd.num("from", 0);
      PathPlay(BoolArg(cmd, "on", true));
      out = std::string("{\"ok\":true,\"playing\":") + (pu.playing ? "true" : "false") + ",\"time\":" + Fix(pu.time, 4) +
            (hidden ? ",\"note\":\"hidden viewer: time advances only with path_time\"" : "") + "}";
      return true;
    }
    if (c == "path_save") {
      if (pu.path.keys.empty()) { out = Err("the path has no keys"); return true; }
      const std::string p = ResolvePath(cmd.str("path", "path"));
      try {
        std::filesystem::create_directories(std::filesystem::absolute(std::filesystem::u8path(p)).parent_path());
        std::ofstream f(std::filesystem::u8path(p), std::ios::binary);
        if (!f) { out = Err("cannot write " + p); return true; }
        f << pu.path.ToJson();
      } catch (const std::exception& e) { out = Err(std::string("save failed: ") + e.what()); return true; }
      out = "{\"ok\":true,\"path\":\"" + jl::Esc(p) + "\",\"keys\":" + std::to_string(pu.path.keys.size()) + "}";
      return true;
    }
    if (c == "path_load") {
      const std::string p = ResolvePath(cmd.str("path", "path"));
      std::ifstream f(std::filesystem::u8path(p), std::ios::binary);
      if (!f) { out = Err("cannot open " + p); return true; }
      std::string txt((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()), e;
      if (!pu.path.FromJson(txt, e)) { out = Err("path file: " + e); return true; }
      pu.selected = -1, pu.time = pu.path.Start(), pu.playing = false;
      out = "{\"ok\":true,\"path\":\"" + jl::Esc(p) + "\",\"keys\":" + std::to_string(pu.path.keys.size()) + "}";
      return true;
    }
    return false;
  }

  // ---- Session 98 (plan step 6): entrances ----
  static constexpr double kAnnouncerTurn = 1.0;  // Session 99: the EV heading's sign for the announcer's turn (PlaceAt; see the notes)
  static constexpr double kAnnouncerLateAt = 371.0;  // Session 99: frames into the first cue from 15 on where 00200110 starts (run 6818) [U]
  static constexpr double kEntranceOriginY = 120.0;  // Session 98: world height of the entrance motions' y zero = the ring canvas top (entry 953 of every bg pac; the game adds -120.0 file space for every ring arena, out\s98\heights.md); measured 120.2 (replay S5)
  // The surface height under an entrance root at (x, z): the ring canvas inside the ropes, the arena's flat floor (median of the flat
  // pieces around, cached per 40 units) outside, blended over 70 units at the apron edge. [U] heuristic: the real collision (entries 995-999) is not decoded.
  float EntFloorAt(float x, float z) {
    const float over = std::max(std::fabs(x), std::fabs(z)) - assets::spots::kCanvasHalf;
    if (over <= 0) return assets::spots::kCanvasTop;
    const std::pair<int, int> key = {(int)std::lround(x / 40.f), (int)std::lround(z / 40.f)};
    float arena = 0;
    auto it = ent.floor_cache.find(key);
    if (it != ent.floor_cache.end()) arena = it->second;
    else {
      float fy = 0;
      if (!sp.flat.FloorIn(x - 60.f, x + 60.f, z - 60.f, z + 60.f, fy) && !sp.flat.FloorIn(x - 250.f, x + 250.f, z - 250.f, z + 250.f, fy)) fy = 0;
      ent.floor_cache[key] = arena = fy;
    }
    if (over >= 70.f) return arena;
    return assets::spots::kCanvasTop + (arena - assets::spots::kCanvasTop) * (over / 70.f);
  }
  // The entrance rig's colours (and keyed, camera-attached directions) for the cue/frame, into the arena's and the wrestler's light sets.
  void ApplyEntranceLights(const assets::entrance::Cue& c) {
    using namespace assets::entrance;
    const Show& sh = *ent.show;
    if (!sh.rig.ok) { RestoreEntranceLights(true); return; }  // (no key for this wrestler: the arena's lights as loaded)
    EntCam cp;
    if (ent.cam_entrance) cp = assets::entrance::Camera(c, ent.take, ent.local);
    if (!cp.ok || !cp.has_q) cp = ViewAxes();
    ApplyEntranceLightsWith(c, cp);
  }
  // The viewer's own camera (free / orbit / a cutaway): its axes in file space (-Y up), for the camera-attached lights.
  assets::entrance::EntCam ViewAxes() {
    assets::entrance::EntCam cp;
    {
      V3 fw, rt, up;
      if (cam.use_vp) {  // a game view-projection (refcheck): the axes are its columns (clip = (pos,1) * vp: x -> right, y -> up, w -> forward)
        rt = Norm(V3{cam.vp[0], cam.vp[4], cam.vp[8]}), up = Norm(V3{cam.vp[1], cam.vp[5], cam.vp[9]}), fw = Norm(V3{cam.vp[3], cam.vp[7], cam.vp[11]});
      } else if (cam.free) fw = FreeForward(cam.fyaw, cam.fpitch), rt = FreeRight(cam.fyaw, cam.fpitch), up = FreeUp(cam.fyaw, cam.fpitch, cam.froll);
      else {
        const V3 e = Eye(cam);
        fw = Norm(V3{cam.target[0] - e.x, cam.target[1] - e.y, cam.target[2] - e.z});
        rt = Norm(Cross(UpVector(), fw));
        up = Cross(fw, rt);
      }
      cp.has_axes = true, cp.ok = true;
      cp.right[0] = rt.x, cp.right[1] = -rt.y, cp.right[2] = rt.z;  // (viewer Y up -> file -Y up)
      // Session 98 (skin fix): the rig's d.y runs along the game view matrix's second row c53, which points DOWN on the screen (probe
      // run 6816, all four Orton frames: c53 = -normalize(c57), c57 = the projection's screen-up row; world = V^T d reproduces c8-c12
      // exactly). So the axis that d.y scales is minus the camera's up; it was +up (every light came from below in a free / refcheck view).
      cp.up[0] = -up.x, cp.up[1] = up.y, cp.up[2] = -up.z;
      cp.fwd[0] = fw.x, cp.fwd[1] = -fw.y, cp.fwd[2] = fw.z;
    }
    return cp;
  }
  void ApplyEntranceLightsWith(const assets::entrance::Cue& c, const assets::entrance::EntCam& cp) {
    using namespace assets::entrance;
    const Show& sh = *ent.show;
    // The rig of the cue; a cue without a motion for the slot (cue 5 has no slot-0 key, cue 20 none either) holds the previous cue's last key
    // (run 6816: cue 20 holds cue 15's last key; cue 5's values came from cue 0 [U]).
    // Session 98 (arena fix): every slot holds, not only slot 0 [C: probe run 6816, cue 20 frames 10741-12241: the arena draws (slot 1)
    // equal cue 15's last key to 0.03 and the crowd (slot 2) to 0.007 (sum of 15 abs differences); cue 20 has no slot 1 / 2 motion,
    // and the viewer kept the stage key's match set there (x0.43-0.57 dark). Cue 5's props (slot 3) take cue 0's: 0.1-0.4 off, the
    // match set further.]
    // Session 98 (integration): the arena-fix commit (49895e98) deleted the EvalForSet call together with the slot-0-only break, so
    // this returned false for every set: no rig colour reached the entrant (set 10 kept the stage key's dark match set: the face x0.5)
    // nor the arena / crowd / props (the stage key's sets). Every slot holds: the cue's own key at the cue-local frame, else the last
    // key of the newest earlier cue that has one. {"cmd":"entrance_time","rig":false} = the arena's own sets (A/B).
    // Session 99: only motions with a loop (YAT2 start / end events) loop, per track (the Rock's / Bryan's end events sit on some
    // tracks only); the others hold their last key (Kane's cue-5 fade-in, entrance_light.h LoopEnd / LoopTime).
    // Session 98 (generalisation, probe run 6819, entrance_light.h LoopTime): rig motions loop (period = last key - 1). A cue with NO
    // rig motion in any slot (The Rock / Daniel Bryan: cues 10-20) keeps the newest earlier cue's motions RUNNING from that cue's start
    // [C: 12 frames, exact]; a cue that has a rig for other slots holds the missing slots at the earlier cue's last key [C: Orton's cue
    // 20 (slots 3 / 4 / 7 keyed, slot 0 not): the same values in all six probed frames over 1500 frames, run 6816]. "rig_loop":false = before.
    auto eval_rig_at = [&](int id, assets::LightSet& k, double local) {
      if (!ent.rig_on) return false;
      const bool run_on = ent.rig_loop && !assets::entrance_light::CueHasRig(sh.rig, c.sub);
      for (int sub = c.sub; sub >= 0; sub -= 5) {
        double t = 1e9;  // (a hold: the earlier cue's last key)
        bool loop = false;
        if (sub == c.sub) t = local, loop = ent.rig_loop;
        else if (run_on) {
          for (size_t i = 0; i < sh.cues.size() && i < ent.tl.start.size(); ++i)
            if (sh.cues[i].sub == sub && ent.cue < ent.tl.start.size()) t = ent.tl.start[ent.cue] + local - ent.tl.start[i], loop = true;
        }
        if (assets::entrance_light::EvalForSet(sh.rig, id, sub, t, k, loop)) return true;
      }
      return false;
    };
    auto eval_rig = [&](int id, assets::LightSet& k) { return eval_rig_at(id, k, ent.local); };
    // Session 98 (sweep look): the entrant's lobe weights (c0..c3.w; the reflection's light weights) are set 10's stored weights
    // (0.627 / 0.353 / 1.451 / 0.275 in arenas 5 / 0) only while the cue that drives the rig has a SLOT-4 motion; otherwise each is 1.0
    // [C: probe frames, skin draws of the entrant: Cena 139 (rig cues 0 / 5 with slots 4 / 5; every cue set 10's: runs 6820 / 6824),
    // Orton 161 (slots 4-7 at cues 0-20: set 10's, run 6816), Kane 107 (cue 5 with slots 4 / 5: set 10's at 27170; cues 10 / 20 with
    // slot 0 only: 1.0 at 27764 / 29342), Del Rio 322 (cue 0 slot 0 only: 1.0 at 18823 / 19304 / 20436), the Rock 100 and Daniel
    // Bryan 324 (cue 5 slot 0 only: 1.0 in all 6 frames of run 6819); c197.w = their sum in every frame]. Slot 4 and slot 5 always come
    // together in these rigs: which one decides is [U], as is the mechanism (slot 4 remaps set 14). "rig_weights":false = set 10's
    // always (before).
    const int drv = ent.rig_on ? assets::entrance_light::DriverCue(sh.rig, c.sub) : -1;
    const bool w_one = ent.rig_weights && drv >= 0 && !assets::entrance_light::CueHasSlot(sh.rig, drv, 4);
    auto apply = [&](size_t model, const std::vector<assets::LightSet>& base, bool entrant) {
      auto* ls = r.MutableLights(model);
      if (!ls || ls->size() != base.size()) return;
      for (size_t i = 0; i < base.size(); ++i) {
        assets::LightSet k, out = base[i];
        if (entrant && w_one) for (float& w : out.wt) w = 1.f;
        if (!eval_rig(base[i].id, k)) { (*ls)[i] = out; continue; }
        std::memcpy(out.col, k.col, sizeof out.col);
        std::memset(out.fall, 0, sizeof out.fall);  // Session 98 (match oracle): the rig's cube has no falloff (LightSet::fall)
        {  // Session 98 (replay S5): the ambient cube is built from the previous frame's colours (asset_model.h LightSet::pcol)
          assets::LightSet kp;
          out.has_pcol = true;
          std::memcpy(out.pcol, eval_rig_at(base[i].id, kp, std::max(0.0, ent.local - 1.0)) ? kp.col : k.col, sizeof out.pcol);
        }
        std::memcpy(out.sky, k.sky, sizeof out.sky);
        std::memcpy(out.ground, k.ground, sizeof out.ground);
        if (entrant && ent.l4 && base[i].id == 10) {  // (the entrant only) set 10 keeps its light 4 (white 1.961, view-space dir (0, 0.643, -0.766)) (run 6816)
          // Session 98 (skin fix): light 4 is the skin draw's c4 / c12 = the edge HIGHLIGHT light, not a diffuse light: the ambient cube
          // (the game's yPreLight pass) is built from c0-c3 only, and the probe's c3 is black. It used to go into the 4th diffuse slot.
          for (int a = 0; a < 3; ++a) k.hl_col[a] = 1.961f, out.hl_col[a] = 1.961f;
          k.hl_dir[0] = 0.f, k.hl_dir[1] = 0.643f, k.hl_dir[2] = -0.766f;
        }
        // Session 98 (arena fix): the arena / props / crowd sets are camera-attached too [C: probe run 6816, frames 8341-11341: the arena
        // (slot 1) and crowd (slot 2) draws' c8-c10 = V^T d of the rig's keyed directions (V = rows c52..c54) with 0.000 error in 9 of 10
        // frame x group cases (0.05 on one light of 8341); out\s98\arena_fix\notes.md]. Before, the arena kept its stage-key directions.
        if (ent.light_keyed) {
          float mag = 0;
          for (int a = 0; a < 4; ++a) for (int b = 0; b < 3; ++b) mag += std::fabs(k.dir[a][b]);
          // Session 98 (replay S3/S4): the entrant's light 3 (no rotation key in the rig) points along minus the camera's forward axis:
          // c11 = -normalize(c59.xyz) in all 4 entrance frames of probe run 6816 and in capture rp_ringin (run 6818) to 1e-6 [C]; the
          // arena / crowd draws keep c11 = 0. A zero rig direction of the entrant is the view-space (0, 0, -1) (lobe weight c3.w 0.27).
          if (entrant && mag > 1e-3f)
            for (int a = 0; a < 4; ++a)
              if (std::fabs(k.dir[a][0]) + std::fabs(k.dir[a][1]) + std::fabs(k.dir[a][2]) < 1e-6f) k.dir[a][0] = 0.f, k.dir[a][1] = 0.f, k.dir[a][2] = -1.f;
          if (mag > 1e-3f && RotateDirs(k, cp, 0)) {
            std::memcpy(out.dir, k.dir, sizeof out.dir);
            if (entrant) std::memcpy(out.hl_dir, k.hl_dir, sizeof out.hl_dir);
          }
        }
        (*ls)[i] = out;
      }
    };
    apply(ent.arena_model, arena_orig, false);
    if (Actor* a = FindActor(ent.actor)) apply(a->model, ent.base_actor, true);
    // Session 98 (sweep look): the entrant's belt and hand props are lit by the ENTRANT's light block, not the default rig [C: probe run
    // 6824 frame 18823, Del Rio's waist belt (evtobj PACH 459, its five meshes found by vertex count at stride 44, skinned VS 1063A665):
    // c0-c2 = his rig colours (0.392 / 0.784,1.02,1.25 / 1.53,1.76,2.0), c181 / c182 = his sky / ground, c4 / c12 = light 4, c197 = his
    // cube scale, the same as his skin draws; the highlight c200 (1,1,1) x c201.x 2.0, power c202.x 50 / 20 / 5 per mesh = the
    // file material's]. Before: set 121's grey 0.451 world-fixed lights (a dull belt). "prop_light":false (entrance_time) = before.
    if (Actor* a = FindActor(ent.actor))
      if (auto* src = r.MutableLights(a->model); src && !src->empty()) {
        assets::LightSet s10 = src->front();
        for (const assets::LightSet& s : *src) if (s.id == 10) { s10 = s; break; }
        auto give = [&](int model) {
          if (model < 0 || size_t(model) >= r.ModelCount()) return;
          auto* ls = r.MutableLights(size_t(model));
          if (!ls) return;
          auto orig = prop_lights_orig.find(model);
          if (orig == prop_lights_orig.end()) orig = prop_lights_orig.emplace(model, *ls).first;  // (the lights as loaded, for the A/B)
          *ls = orig->second;  // (prop models load with one set, every mesh on index 0)
          if (!ent.prop_light) { r.SetMeshLightsByNormal(size_t(model), 0, 0); return; }
          // the bump-mapped pieces (the belt's yProp_BumpMap meshes: the five the game drew with the entrant's block) take the entrant's
          // set; the others (the belt's 26-vertex yProp_Default name plate: the game's draw for it not identified [U]) keep theirs
          ls->push_back(s10);
          r.SetMeshLightsByNormal(size_t(model), int(ls->size()) - 1, 0);
        };
        give(ent.belt_model);
        // (merge with props general, review) only what the ENTRANT carries takes his lights (the evidence: his own belt, 6824 f18823):
        // title acts, and other hand props when no companion is placed (else the companion carries them) and the act is never world-
        // placed. Companion-carried and world props (torches, the Miz letters) keep their own sets [U: their lights not measured].
        for (const auto& p : ent.props) {
          bool world = false;
          if (p.act < ent.show->props.size())
            for (const auto& [sub, m] : ent.show->props[p.act].mode) world = world || m == assets::entrance::kPropWorld;
          if (p.belt || p.worn || (ent.comps.empty() && !world)) give(p.model);  // (a worn cap: the entrant's head)
        }
      }
    ApplyStagePools(true);
    // Session 98 (arena fix 2): the entrant's film filter (entrance_light.h Rig::film); gen check 6: the cue's own record (Rig::FilmFor)
    SetFilmComposed(film_on ? sh.rig.FilmFor(c.sub) : nullptr);  // (Session 99: + the scene objects' screen flash)
  }
  // Session 99 (scene objects): the film filter as the entrance lights ask for it (null = none), with this frame's GMEV screen flash
  // (particles.h ScreenFlash, flash_now) composed onto it. Both are one full-screen lerp toward a colour, so the pair is one lerp:
  // lerp(lerp(c, F, a), G, b) = lerp(c, (F a (1 - b) + G b) / s, s), s = 1 - (1 - a)(1 - b). [U] the game's order of the two quads.
  void SetFilmComposed(const float* film) {
    film_base_set = film != nullptr;
    for (int k = 0; k < 4; ++k) film_base[k] = film ? film[k] : 0.f;
    ComposeFilm();
  }
  void ComposeFilm() {
    const float b = std::max(0.f, std::min(1.f, flash_now[3]));
    if (b <= 0.f) { r.SetFilm(film_base_set ? film_base : nullptr); return; }
    // (review) the film alpha is NOT clamped: FindFilm stores byte / 128 (up to ~1.99), and the shader's lerp takes it as is, so a flash
    // starting / ending must not change the film's own strength. The composition holds for any a: (1 - a)(1 - b) stays the colour's weight.
    const float a = film_base_set ? std::max(0.f, film_base[3]) : 0.f;
    const float s = 1.f - (1.f - a) * (1.f - b);
    if (std::fabs(s) < 1e-6f) { r.SetFilm(film_base_set ? film_base : nullptr); return; }
    float out[4];
    for (int k = 0; k < 3; ++k) out[k] = (film_base[k] * a * (1.f - b) + flash_now[k] * b) / s;
    out[3] = s;
    r.SetFilm(out);
  }
  // Session 99 (scene objects): this entrance moment's screen flash and soft-particle range from the bank's GMEV objects (cue sub +
  // cue-local frame: the game's sub_823102F0 / sub_823FAF90 clock). "scene_lights":false = no flash, the fixed 30-unit soft fade.
  void UpdateSceneLights(const assets::entrance::Cue& c) {
    if (!scene_lights_on) {
      for (float& v : flash_now) v = 0.f;
      r.SetSoftParticles(30.f);  // (Renderer::kSoftRange: particles 4's fixed fade)
      return;
    }
    assets::particles::FlashNow(scene_lights, c.sub, ent.local, flash_now);
    // [U] the game's switch is one global object (particles.h SoftNow): a later entrance without a switch may inherit the last value
    // (Kane's 30) until it is rebuilt; the viewer starts every entrance with it off.
    r.SetSoftParticles(assets::particles::SoftNow(scene_lights, c.sub, ent.local));
  }
  // The lights are camera-attached: when the viewer's own camera moved (and is not the entrance camera, whose pose changes with ent.t),
  // the rig is evaluated again for the new axes.
  float ent_cam_sig[16] = {0};
  void EntranceLightsFollowCamera() {
    if (!ent.active || !ent.show || !ent.lights_on || ent.cam_entrance) return;
    float s[16] = {cam.use_vp ? 1.f : 0.f, cam.free ? 1.f : 0.f, cam.vp[0], cam.vp[1], cam.vp[3], cam.vp[4], cam.vp[5], cam.vp[7],
                   cam.fyaw, cam.fpitch, cam.froll, cam.yaw, cam.pitch, cam.target[0], cam.target[1], cam.target[2]};
    if (std::memcmp(s, ent_cam_sig, sizeof s) == 0) return;
    std::memcpy(ent_cam_sig, s, sizeof s);
    ApplyEntranceLights(ent.show->cues[std::min(ent.cue, ent.show->cues.size() - 1)]);
  }
  // Session 98 (shadow / match light): the match-lit actors' set 10 turned to the current camera (Install's comment has the evidence).
  float match_cam_sig[16] = {0};
  void MatchLightsFollowCamera() {
    bool any = false;
    for (auto& a : actors) any = any || (a->match_light && !a->entrance);
    if (!any) return;
    float s[16] = {cam.use_vp ? 1.f : 0.f, cam.free ? 1.f : 0.f, cam.vp[0], cam.vp[1], cam.vp[3], cam.vp[4], cam.vp[5], cam.vp[7],
                   cam.fyaw, cam.fpitch, cam.froll, cam.yaw, cam.pitch, cam.target[0], cam.target[1], cam.target[2]};
    if (std::memcmp(s, match_cam_sig, sizeof s) == 0) return;
    std::memcpy(match_cam_sig, s, sizeof s);
    const assets::entrance::EntCam cp = ViewAxes();
    for (auto& a : actors) {
      if (!a->match_light || a->entrance) continue;
      auto* ls = r.MutableLights(a->model);
      if (!ls || ls->empty()) continue;
      assets::LightSet k = a->match_base;
      if (assets::entrance::RotateDirs(k, cp, 0)) (*ls)[0] = k;
    }
  }
  // `camera` false: something else owns the camera this frame (a camera-path render).
  // Session 98 (gen check 4 / 5): the title belt. (1) When the entrance holds a motion for the belt (Show::props: the prop act whose
  // cue motion has the most tracks named after the belt model's nodes, at least 4), the belt plays it on the entrant's clock and model
  // transform, like the body (bank 602 leaves 12 / 13, bank 416 leaf 7) [C: the tracks; U: that its root shares the body's space -
  // judged by the picture]. (2) Otherwise it rides the wearer's bone with the game's measured pose (belt_pose.h): Bip L Hand ("hold",
  // run 6820) or Bip Spine ("waist", run 6821), each belt node at its measured bend.
  static assets::Matrix RowFromFile(const float* m12) {  // file column 3x4 -> viewer row-vector 4x4 (Y mirrored)
    assets::Matrix r = assets::Identity();
    for (int i = 0; i < 3; ++i) {
      for (int j = 0; j < 3; ++j) r[j * 4 + i] = m12[i * 4 + j] * ((i == 1) != (j == 1) ? -1.f : 1.f);
      r[12 + i] = m12[i * 4 + 3] * (i == 1 ? -1.f : 1.f);
    }
    return r;
  }
  const assets::motion::Motion* BeltMotion(const std::vector<assets::Bone>& bb, int sub, uint32_t* prop_id = nullptr) {
    if (!ent.show) return nullptr;
    const assets::motion::Motion* best = nullptr;
    int best_n = 3;
    for (const auto& pa : ent.show->props) {
      auto it = pa.by_sub.find(sub);
      if (it == pa.by_sub.end()) continue;
      int n = 0;
      for (const assets::Bone& b : bb) if (it->second.Find(assets::motion::NodeHash(b.name))) ++n;
      if (n > best_n) {
        best_n = n, best = &it->second;
        if (prop_id) *prop_id = pa.prop_id;
      }
    }
    return best;
  }
  // Session 98 (gen check 6): a prop model posed by its act motion in a hand of `carrier` (entrance.h PropAttach). False: the motion
  // has no base, the prop is parked, or the carrier has no such hand (the caller hides the prop).
  bool PoseHandProp(size_t pm, const Actor& carrier, const assets::motion::Motion& mo, bool right_hand) {
    if (pm >= model_bones.size() || carrier.model >= model_bones.size()) return false;
    float rel[12];
    if (!assets::entrance::PropAttach(mo, ent.local, right_hand, rel)) return false;
    const auto& bones = model_bones[carrier.model];
    const char* bone = right_hand ? "Bip R Hand" : "Bip L Hand";
    int k = -1;
    for (size_t i = 0; i < bones.size(); ++i) if (bones[i].name == bone) { k = int(i); break; }
    if (k < 0 || size_t(k) >= carrier.skin.size()) return false;
    const assets::Matrix frame = assets::Mul(RowFromFile(rel), assets::Mul(bones[size_t(k)].bind_world, carrier.skin[size_t(k)]));
    assets::motion::PoseOpts o = carrier.opt;
    o.leg_ik = false;
    std::vector<assets::Matrix> pal = assets::motion::BuildSkin(model_bones[pm], mo, ent.local, o);
    if (pal.empty()) pal.push_back(assets::Identity());
    for (assets::Matrix& m : pal) m = assets::Mul(m, frame);
    std::string e;
    r.SetSkin(pm, pal, e);
    r.SetTransform(pm, carrier.origin, TurnFor(carrier.yaw));
    return true;
  }
  // Session 98 (cap / particles): a WORN prop (a cap: asset_load.h PropWorn) in a still-zero cue: rigid on the wearer's Bip Head. The
  // cap's vertices are an offset from the head in the character's own axes (PACH 181: x -9..9 across, y 7..23 above the head joint, the
  // brim 20 to one side in z), so each vertex goes where the head's bind position + that offset goes: p * T(head bind) * skin(head)
  // [C: the game's shots 6824_sw_s0_* show the cap seated on his head in all four cues; U: the game's own attach code is not traced].
  bool PoseWornProp(size_t pm, const Actor& wearer, const assets::motion::Motion& mo) {
    if (pm >= model_bones.size() || wearer.model >= model_bones.size()) return false;
    const auto& bones = model_bones[wearer.model];
    int k = -1;
    for (size_t i = 0; i < bones.size(); ++i) if (bones[i].name == "Bip Head") { k = int(i); break; }
    if (k < 0 || size_t(k) >= wearer.skin.size()) return false;
    assets::Matrix at = assets::Identity();
    for (int j = 0; j < 3; ++j) at[size_t(12 + j)] = bones[size_t(k)].bind_world[size_t(12 + j)];
    const assets::Matrix frame = assets::Mul(at, wearer.skin[size_t(k)]);
    (void)mo;  // (a still-zero act: the cap's own bind pose, its base track carries nothing)
    std::vector<assets::Matrix> pal(std::max<size_t>(1, model_bones[pm].size()), frame);
    std::string e;
    r.SetSkin(pm, pal, e);
    r.SetTransform(pm, wearer.origin, TurnFor(wearer.yaw));
    return true;
  }
  // Session 98 (props general): a kPropWorld prop: its root in the entrance's motion space (entrance.h PropWorld), placed where the
  // body's root space goes - the entrant's origin without his own lift, plus the raised-stage lift of the surface under the PROP
  // (the same ground rule as the entrant's, ApplyEntrance) [U: the game's lift for props is not traced].
  bool PoseWorldProp(size_t pm, const Actor& a, const assets::motion::Motion& mo, float* at = nullptr) {
    if (pm >= model_bones.size()) return false;
    float rel[12];
    if (!assets::entrance::PropWorld(mo, ent.local, rel)) return false;
    float org[3] = {a.origin[0], a.origin[1] - ent.ground_dy, a.origin[2]};
    if (ent.root_y == "canvas" && ent.stage_ground) {
      const float top = org[1] - rel[7] + 150.f;  // (viewer height of the prop root + 150: overhead rigs never ground it)
      float h = 0;
      if (ground.Top(rel[3], rel[11], top, h) && std::fabs(h) > 5.f) org[1] += h;
    }
    assets::motion::PoseOpts o = a.opt;
    o.leg_ik = false;
    std::vector<assets::Matrix> pal = assets::motion::BuildSkin(model_bones[pm], mo, ent.local, o);
    if (pal.empty()) pal.push_back(assets::Identity());
    const assets::Matrix frame = RowFromFile(rel);
    for (assets::Matrix& m : pal) m = assets::Mul(m, frame);
    std::string e;
    r.SetSkin(pm, pal, e);
    r.SetTransform(pm, org, TurnFor(a.yaw));
    if (at) at[0] = org[0] + rel[3], at[1] = org[1] - rel[7], at[2] = org[2] + rel[11];
    return true;
  }
  void UpdateProps(const Actor& a, const assets::entrance::Cue& c, bool hidden) {
    // (review fix) the first companion carries only in a cue where he plays a motion; else he was moved away and the entrant carries
    const Actor* comp = nullptr;
    if (!ent.comps.empty() && ent.comps.front().second < ent.show->comps.size() && ent.show->comps[ent.comps.front().second].by_sub.count(c.sub))
      comp = FindActor(ent.comps.front().first);
    // (review) a cap played in hand / world this cue (bank 424 cue 20: 3081 carried and thrown while 2081 is still-zero) = the worn one
    // came off: no second cap on the head [U: the game's hand-over is not traced]
    bool cap_played = false;
    for (const auto& p : ent.props) {
      if (!p.worn) continue;
      const auto& pa = ent.show->props[p.act];
      auto mi = pa.mode.find(c.sub);
      cap_played = cap_played || (pa.by_sub.count(c.sub) && mi != pa.mode.end() && mi->second != assets::entrance::kPropZero);
    }
    for (auto& p : ent.props) {
      if (p.model < 0 || size_t(p.model) >= r.ModelCount()) continue;
      const auto& pa = ent.show->props[p.act];
      auto it = pa.by_sub.find(c.sub);
      auto mi = pa.mode.find(c.sub);
      const int mode = mi == pa.mode.end() ? assets::entrance::kPropZero : mi->second;
      bool shown = false;
      if (!hidden && it != pa.by_sub.end()) {
        if (mode == assets::entrance::kPropWorld) shown = PoseWorldProp(size_t(p.model), a, it->second, p.at);
        else if (mode == assets::entrance::kPropHand) {
          const Actor* carrier = (p.belt || !comp) ? &a : comp;  // the title: the champion; other hand props: the companion [U]
          shown = PoseHandProp(size_t(p.model), *carrier, it->second, assets::entrance::PropAttachKind(pa.prop_id) == 3);
        } else if (p.worn && !cap_played) shown = PoseWornProp(size_t(p.model), a, it->second);
      }
      p.shown = shown;
      if (!shown) {
        const float z[3] = {0, -100000.f, 0};
        r.SetTransform(size_t(p.model), z, 0.f);
      }
    }
  }
  // Session 98 (cap / particles): the cue's particle effect (assets\particles.h: effect id == the cue's sub [U]), emitted around the
  // entrant's feet [U: the game's emitter frame is not found] and simulated from the cue's start (scrub-safe). "particles":false = none.
  // (review) no entrance any more (stopped, actor removed, scene reloaded): no sprites, no particle textures, no bank
  void StopParticles() {
    if (r.SpriteCount() || !ptex.empty()) r.ClearSprites();
    ptex.clear(), psim.clear();
    if (flash_now[3] > 0.f) { for (float& v : flash_now) v = 0.f; ComposeFilm(); }  // Session 99 (scene objects): no flash left on
    if (pbank.ok) pbank = assets::particles::Bank();
  }
  // Session 99 (particles 2): with the bank's GMEV scene objects (assets\particles.h Placement) every placed effect plays at its own
  // place from its own start (cue sub + frame) to its stop, on the entrance clock; without them the old rule above.
  // [U] a floor-relative object (p[9] = 1) counts its y from the floor at its own x / z (Placement::floor, looked up once at load,
  // EntFloorAt); the rest at arena height.
  void UpdateParticles(const Actor& a, const assets::entrance::Cue& c, bool hidden) {
    std::vector<Renderer::Sprite> out;
    std::vector<assets::particles::Sprite>& sp = last_psprites;
    sp.clear(), last_pfx.clear();
    size_t budget = assets::particles::kSlotBudget;  // (review) slots simulated this frame across every copy
    auto tex_of_fx = [&](const assets::particles::Effect& fx) {
      return [&](const assets::particles::Emitter& em) {
        const auto t = ptex.find({fx.id, size_t(&em - fx.emitters.data())});
        return t == ptex.end() ? -1 : t->second;
      };
    };
    if (particles_on && pbank.ok && !pbank.placed.empty() && ent.show) {
      auto cue_start = [&](int sub) -> double {
        for (size_t i = 0; i < ent.show->cues.size() && i < ent.tl.start.size(); ++i)
          if (ent.show->cues[i].sub == sub) return ent.tl.start[i];
        return -1;
      };
      uint32_t copy = 0;
      // Session 99 (scene objects): the factory (sub_827D1408 case 8, 0x827D15E0) makes a type-8 object only when the arena in use
      // (G+64, sub_827D7578) is NOT a Create-an-Arena slot 200..249 (sub_822C3280), not 76 / 77 / 79 (sub_824E6FE8) and not 20 [C].
      // Types 9 / 10 split on byte 32 of the arena's 108-byte record (sub_822C3878; the table is loaded at run time, 0x836F2C34 +
      // 11316): not read, both drawn [U].
      const bool no_type8 = last_arena == 20 || last_arena == 76 || last_arena == 77 || last_arena == 79 || (last_arena >= 200 && last_arena <= 249);
      for (const auto& pl : pbank.placed) {
        ++copy;
        if (pl.type == 8 && no_type8) continue;
        auto it = pbank.effects.find(pl.effect);
        const double s0 = cue_start(pl.start_sub);
        if (it == pbank.effects.end() || s0 < 0) continue;
        const double start = s0 + pl.start_f, fr = ent.t - start;
        if (fr < 0) continue;
        double stop = -1;
        if (pl.end_f != 9999) {
          const double e0 = cue_start(pl.end_sub);
          if (e0 >= 0) stop = e0 + pl.end_f - start;
        }
        if (stop >= 0 && fr > stop + 3600) continue;  // long gone
        const float org[3] = {pl.pos[0], (pl.floor_rel ? pl.floor : 0.f) - pl.pos[1], pl.pos[2]};
        const size_t n0 = sp.size();
        assets::particles::Simulate(it->second, fr, org, tex_of_fx(it->second), sp, pl.rot, stop, copy + particle_seed * 7919u,
                                    &psim[copy], &budget);
        if (sp.size() > n0) last_pfx.push_back({pl.effect, fr, n0, sp.size()});
      }
    } else if (!hidden && particles_on && pbank.ok) {
      auto it = pbank.effects.find(c.sub);
      if (it != pbank.effects.end() && !pbank.Grouped(c.sub)) {
        const float org[3] = {a.origin[0] + float(ent.root[0]), a.origin[1], a.origin[2] + float(ent.root[2])};
        // (review) one kept state per cue (keyed by the cue index, high bit set: never a placed copy's number)
        assets::particles::Simulate(it->second, ent.local, org, tex_of_fx(it->second), sp, nullptr, -1, 0,
                                    &psim[0x80000000u | uint32_t(ent.cue)], &budget);
      }
    }
    {
      out.reserve(sp.size());
      for (const auto& s : sp) {
        Renderer::Sprite q;
        for (int k = 0; k < 3; ++k) q.c[k] = s.c[k], q.ax[k] = s.ax[k], q.ay[k] = s.ay[k];
        for (int k = 0; k < 4; ++k) q.col[k] = s.col[k];
        q.tex = s.tex, q.billboard = s.billboard;
        out.push_back(q);
      }
    }
    r.SetSprites(std::move(out));
  }
  // Session 98 (gen check 5 / props general): every placed companion (Show::comps) plays its leaf's motion of the cue on the entrant's
  // clock, placed like the entrant (the motions carry their own paths; same origin rule) [C: run 6821, Ricardo within 2 units].
  void UpdateCompanion(const Actor& a, const assets::entrance::Cue& c, bool hidden) {
    for (const auto& [id, ci] : ent.comps) {
      Actor* cp = FindActor(id);
      if (!cp || ci >= ent.show->comps.size()) continue;
      const auto& sc = ent.show->comps[ci];
      auto it = sc.by_sub.find(c.sub);
      if (hidden || it == sc.by_sub.end()) {
        const float z[3] = {0, -100000.f, 0};
        r.SetTransform(cp->model, z, 0.f);
        continue;
      }
      if (cp->ent_cue != (int)ent.cue) {
        cp->motion = it->second, cp->ent_cue = (int)ent.cue, cp->have_motion = cp->pose_motion = true;
        cp->motion_label = "entrance companion leaf " + std::to_string(sc.leaf) + " cue " + std::to_string(c.sub);
      }
      cp->frame = ent.local;
      PoseActor(*cp);
      cp->anchor = false, cp->ground = false, cp->yaw = a.yaw, cp->pos[0] = a.pos[0], cp->pos[2] = a.pos[2], cp->floor_y = a.floor_y;
      PlaceXform(*cp, true, false);
    }
  }
  // Session 99 (announcer): the ring announcer plays his motion of the cue (ent.ann_mo: already at his spot in the entrance's motion
  // space) on the entrance clock, placed like the entrant without the raised-stage lift (he stands in the ring). The shared evtmot
  // motions are idle loops (first key = last key) and loop over a longer cue [U: the game's own wrap]; the bank's own leaf-8 motions
  // run once like the entrant's. His microphone rides his hand (entrance.h PropAttach).
  void UpdateAnnouncer(const Actor& a, const assets::entrance::Cue& c) {
    const float z[3] = {0, -100000.f, 0};
    auto mic_away = [&]() { if (ent.ann_mic_model >= 0 && size_t(ent.ann_mic_model) < r.ModelCount()) r.SetTransform(size_t(ent.ann_mic_model), z, 0.f); };
    Actor* an = nullptr;
    for (auto& ac : actors) if (ent.ann_ptr && ac.get() == ent.ann_ptr) an = ac.get();
    if (!an) return mic_away();  // (none, or replaced / removed by the user: his mic goes too)
    // (review) a segment's start is resolved against the CURRENT timeline (entrance_time's intro_hold rebuilds ent.tl)
    auto seg_at = [&](const EntranceState::AnnSeg& s) {
      if (s.cue >= ent.tl.start.size()) return 1e300;
      return ent.tl.start[s.cue] + std::min(s.off, std::max(0.0, ent.tl.len[s.cue] - 1));
    };
    int si = -1;
    for (size_t i = 0; i < ent.ann_seg.size(); ++i)
      if (seg_at(ent.ann_seg[i]) <= ent.t + 1e-9) si = int(i);
    if (si < 0 && !ent.ann_seg.empty()) si = 0;
    const EntranceState::AnnSeg* sg = si >= 0 ? &ent.ann_seg[size_t(si)] : nullptr;
    if (!sg || sg->mo < 0 || size_t(sg->mo) >= ent.ann_mo.size()) {
      r.SetTransform(an->model, z, 0.f);
      return mic_away();
    }
    if (ent.ann_seg_cur != si) {
      const assets::motion::Motion& m = ent.ann_mo[size_t(sg->mo)];
      an->motion = m, an->have_motion = an->pose_motion = true, ent.ann_seg_cur = si;
      an->motion_label = "entrance announcer cue " + std::to_string(c.sub) + " (" + m.name + ")";
    }
    const double len = std::max(1, an->motion.length);
    const double at = std::max(0.0, ent.t - seg_at(*sg));
    an->frame = sg->loop ? std::fmod(at, len) : std::min(at, len);
    PoseActor(*an);
    an->anchor = false, an->ground = false, an->yaw = a.yaw, an->pos[0] = a.pos[0], an->pos[2] = a.pos[2], an->floor_y = a.floor_y - ent.ground_dy;
    PlaceXform(*an, true, false);
    if (ent.ann_mic_model < 0) return;
    const assets::motion::Motion* mm = nullptr;
    auto own = ent.show->ann.mic_own.find(c.sub);
    if (own != ent.show->ann.mic_own.end()) mm = &own->second;
    else if (ent.show->ann.has_mic) mm = &ent.show->ann.mic;  // (review: per cue - a cue with no own leaf-9 motion takes the shared one)
    if (!mm || !PoseHandProp(size_t(ent.ann_mic_model), *an, *mm, ent.ann_mic_right)) mic_away();
  }
  void UpdateBelt(const Actor& a, bool hidden) {
    if (ent.belt_model < 0 || size_t(ent.belt_model) >= model_bones.size() || a.model >= model_bones.size()) return;
    const size_t bm = size_t(ent.belt_model);
    const auto& bb = model_bones[bm];
    const auto& bones = model_bones[a.model];
    std::string e;
    auto away = [&]() { const float z[3] = {0, -100000.f, 0}; r.SetTransform(bm, z, 0.f); };
    if (hidden) return away();
    const assets::entrance::Cue& c = ent.show->cues[std::min(ent.cue, ent.show->cues.size() - 1)];
    // (gen check 6) the belt's own motion, its root in the hand's frame (entrance.h PropAttach: 2xxx left, 3xxx right - bank 602's
    // cue 20 act is 3009). A parked base hides it. The waist belt keeps the measured pose (its act's base is zero: [U] its frame).
    // (props general, review fix) a cue that has a hand title act (item 9): the acts decide - their own models draw it (UpdateProps)
    // or, when every act of the cue is an unused zero slot, nothing is drawn. A cue with no title act keeps the static belt below
    // [U: the game's belt in such a cue (602's cue 6) is not captured].
    if (ent.belt_motion && !ent.belt_waist) {
      bool act = false, all_zero = true;
      for (const auto& pa : ent.show->props) {
        const int kind = assets::entrance::PropAttachKind(pa.prop_id);
        if (pa.prop_id % 1000 != 9 || (kind != 2 && kind != 3) || !pa.by_sub.count(c.sub)) continue;
        act = true;
        auto mi = pa.mode.find(c.sub);
        if (mi != pa.mode.end() && mi->second != assets::entrance::kPropZero) all_zero = false;
      }
      if (act && (all_zero || ent.title_props > 0)) return away();
    }
    uint32_t pid = 0;
    if (ent.belt_motion && !ent.belt_waist)
      if (const assets::motion::Motion* mo = BeltMotion(bb, c.sub, &pid)) {
        if (!PoseHandProp(bm, a, *mo, assets::entrance::PropAttachKind(pid) == 3)) away();
        return;
      }
    const char* bone = ent.belt_waist ? "Bip Spine" : "Bip L Hand";
    int k = -1;
    for (size_t i = 0; i < bones.size(); ++i) if (bones[i].name == bone) { k = int(i); break; }
    if (k < 0 || size_t(k) >= a.skin.size()) return away();
    namespace bp = assets::belt_pose;
    const float* att = ent.belt_waist ? bp::kWaistAttach : bp::kHandAttach;
    const bp::BeltNode* nodes = ent.belt_waist ? bp::kWaistNodes : bp::kHandNodes;
    const size_t nn = ent.belt_waist ? sizeof bp::kWaistNodes / sizeof bp::kWaistNodes[0] : sizeof bp::kHandNodes / sizeof bp::kHandNodes[0];
    const assets::Matrix frame = assets::Mul(RowFromFile(att), assets::Mul(bones[size_t(k)].bind_world, a.skin[size_t(k)]));
    std::vector<assets::Matrix> pal(std::max<size_t>(1, bb.size()), frame);
    for (size_t i = 0; i < bb.size(); ++i)
      for (size_t j = 0; j < nn; ++j)
        if (bb[i].name == nodes[j].name) { pal[i] = assets::Mul(RowFromFile(nodes[j].m), frame); break; }
    r.SetSkin(bm, pal, e);
    r.SetTransform(bm, a.origin, TurnFor(a.yaw));
  }
  void ApplyEntrance(bool camera = true) {
    using namespace assets::entrance;
    static const float kNoAim[3] = {0, 0, 0};
    if (!ent.active || !ent.show) { r.SetEntranceAim(false, kNoAim); r.SetFlashFrame(false, 0); StopParticles(); return; }
    Actor* a = FindActor(ent.actor);
    if (!a) { ent.active = false; ApplyStagePools(false); r.SetEntranceAim(false, kNoAim); r.SetFlashFrame(false, 0); StopParticles(); return; }
    const Show& sh = *ent.show;
    ent.t = std::max(0.0, std::min(ent.t, ent.tl.total));
    Locate(sh, ent.tl, ent.t, ent.cue, ent.local);
    const Cue& c = sh.cues[ent.cue];
    if (a->ent_cue != (int)ent.cue) {  // a hard cut: the game starts each cue with blend 0.0
      a->motion = c.body, a->ent_cue = (int)ent.cue, a->have_motion = a->pose_motion = true;
      a->motion_label = "entrance cue " + std::to_string(c.sub);
      if (ent.cue < ent.off.size()) Shift(a->motion, ent.off[ent.cue]);
    }
    a->frame = ent.local;
    double p[3] = {0, 0, 0};
    Root(c, ent.local, p);
    if (ent.cue < ent.off.size()) for (int i = 0; i < 3; ++i) p[i] += ent.off[ent.cue][(size_t)i];
    for (int i = 0; i < 3; ++i) ent.root[i] = p[i];
    PoseActor(*a);
    a->anchor = false, a->yaw = 180.f, a->pos[0] = a->pos[2] = 0.f;  // (the pose carries the root path itself; yaw 180 = no turn)
    if (ent.root_y == "ground") {
      a->ground = true;
      ent.floor_y = a->floor_y = EntFloorAt((float)p[0], (float)p[2]);
    } else {
      // The Bip height is -Y up with an offset (S98 review): the roll into the ring goes 123 -> 1 while the canvas is 111 higher, the ring
      // walk sits at ~1-4 and the turnbuckle at -68. So the root's HEIGHT = reference - y, and the pose (built with +y up, which keeps the
      // legs' relative numbers) is moved by origin = 96.6 + ref - 2 y ("raw") or the same with the vertical travel x 0.785 ("scale").
      // Standing pelvis height 96.6 = the bind pose's. [U: the reference / offset; the shots decide]
      a->ground = false;
      ent.floor_y = 0;
      const double k = ent.root_y == "scale" ? 0.785 : 1.0;
      // (pose built -Y up: viewer root height = origin - y; the old +Y-up reading: origin + y. With root_delta the pose already carries
      // the bind Bip height (default + sample, motion.h), so the 96.6 is not added again: same root heights as before.)
      const double base = a->opt.root_delta ? 0.0 : 96.6;
      a->floor_y = (a->opt.flip & 2) ? (float)(base + k * ent.y_ref - (1.0 + k) * p[1]) : (float)(base + k * ent.y_ref + (1.0 - k) * p[1]);
      // "canvas" (S98 motion rule): the motion space's y zero sits on the ring canvas. Data: with the bind + sample root the in-ring
      // feet of Orton's cue 20 land within 0.7 of the canvas (ground mode's own placement) at f0 / f300, so the turnbuckle climb keeps
      // its height (ground mode puts the lowest foot on the canvas). [U] the ramp: the feet sit ~7 under the viewer's ramp surface.
      // Session 98 (replay S5, run 6818 captures, 0 runs): the game puts the motion's y zero at world y 120.2 (viewer space) in every
      // place of the entrance - stage, ramp, ring-in, ring: Orton's bone palette (probe c76..) sits 9.3 / 9.8 / 9.2 / 8.9 above this mode
      // with 111, x / z within 1-3; his eyes 125 px (10.5 units) above at rp_ringin with the arena aligned to 1 px; the boots' lowest
      // vertex (post-VS, unprojected) 0.24 on the stage and 120.4 standing in the ring = the motion's own sole heights (0 / 120). So no
      // per-frame grounding: the motion carries the stage / ramp / ring heights itself (and the turnbuckle climb).
      // [U] 120.2 vs the canvas top 111 used for the spots (spots.h; the ropes' live heights were read against 111): the ring mat may
      // sit higher than 111; one arena (5) measured.
      if (ent.root_y == "canvas") a->floor_y = (float)(kEntranceOriginY + (a->opt.root_delta ? 0.0 : 96.55));
      // Session 98 (generalisation, probe run 6819: The Rock / Daniel Bryan in RAW, a RAISED stage): the motions are per wrestler and
      // carry a floor-level stage (world 0 outside the ring), so on a raised stage the game lifts the entrant by the stage surface under
      // his root. [C] the palette bones vs this mode without the lift (out\s98\gen_check\place_before): stage +179 / +178 where the
      // stage slots' deck under the root is 180.0; the ramp (sloping deck, slot 203) +66 / +66 where it is 63.6 / 64.0; ringside and
      // the ring +-2 (surface 0). Arena 5 (floor stage, deck 2.4) measured +0.2 (Orton's boots): surfaces within 5 of the floor count
      // as the floor (the motion's own 0). Ceiling = the un-lifted pelvis + 150 (overhead rigs / screens never ground the actor).
      // [U] the game's own query (collision entries 995-999 are not decoded): the stage meshes stand in for it.
      ent.ground_dy = 0;
      if (ent.root_y == "canvas" && ent.stage_ground) {
        const float pelvis = (float)(kEntranceOriginY - p[1] + 96.6);
        float h = 0;
        if (ground.Top((float)p[0], (float)p[2], pelvis + 150.f, h) && std::fabs(h) > 5.f) ent.ground_dy = h, a->floor_y += h;
      }
    }
    PlaceXform(*a, true, false);  // (anchor off, nothing re-anchored: the root path is exact)
    if (c.sub == 0 && !ent.show_cue0) {  // cue 0 = the hidden pyro pose (Bip y 446): not shown, like the game
      const float away[3] = {0, -100000.f, 0};
      r.SetTransform(a->model, away, 0.f);
    }
    UpdateBelt(*a, c.sub == 0 && !ent.show_cue0);
    UpdateCompanion(*a, c, c.sub == 0 && !ent.show_cue0);
    UpdateAnnouncer(*a, c);  // Session 99: in every cue his row places him (cue 0 included: the game fills its row too)
    UpdateProps(*a, c, c.sub == 0 && !ent.show_cue0);  // (after the companion: the props ride his posed hands)
    UpdateParticles(*a, c, c.sub == 0 && !ent.show_cue0);
    {  // Session 98 (stage fix): the take's aim point drives the far-DoF focus (renderer.cpp Frame), whatever camera is shown
      const EntCam ca = assets::entrance::Camera(c, ent.take, ent.local);
      const float ap[3] = {(float)ca.aim[0], (float)(-ca.aim[1] + ent.cam_dy + ent.ground_dy), (float)ca.aim[2]};
      r.SetEntranceAim(ca.ok && !(c.sub == 0 && !ent.show_cue0), ap);
    }
    // Session 98 (flash fix): the crowd camera flashes follow the entrance clock (one guest frame = one motion frame), so a render repeats
    r.SetFlashFrame(true, (int64_t)std::floor(ent.t));
    if (camera && ent.cam_entrance) {
      const EntCam cp = assets::entrance::Camera(c, ent.take, ent.local);
      if (cp.ok) {
        const float dx = (float)(cp.eye[0] - cp.aim[0]), dy = (float)(-cp.eye[1] + cp.aim[1]), dz = (float)(cp.eye[2] - cp.aim[2]);
        const float d = std::max(1.f, std::sqrt(dx * dx + dy * dy + dz * dz));
        // the free camera pose (camera.h): at the eye, looking at the aim; roll 0 (Camera01's rotation key also carries a roll: unused [U])
        // Session 98 (generalisation): the camera is lifted with the entrant (ground_dy) [C: probe run 6819, The Rock on RAW's 180 deck:
        // eye 323.3 vs the game's 325.2, on the ramp 186.8 vs 186.9, x / z within 1.5; Bryan's two frames sit 14 lower in the game [U]].
        cam.use_vp = false;
        cam.target[0] = (float)cp.aim[0], cam.target[1] = (float)(-cp.aim[1] + ent.cam_dy + ent.ground_dy), cam.target[2] = (float)cp.aim[2];
        cam.dist = d;
        cam.free = true;
        cam.fpos[0] = (float)cp.eye[0], cam.fpos[1] = (float)(-cp.eye[1] + ent.cam_dy + ent.ground_dy), cam.fpos[2] = (float)cp.eye[2];
        cam.fyaw = std::atan2(-dx, -dz) * 57.29578f;
        cam.fpitch = std::max(-89.5f, std::min(89.5f, std::asin(std::max(-1.f, std::min(1.f, -dy / d))) * 57.29578f));
        cam.froll = 0;
        if (cp.fov > 1.0) cam.fov = (float)std::min(120.0, cp.fov);  // the fov track (vertical, radians in the file)
      }
    }
    UpdateSceneLights(c);  // Session 99 (scene objects): before the lights, which compose the flash onto their film filter
    if (ent.lights_on) ApplyEntranceLights(c);
    ComposeFilm();  // (lights off / no rig: the flash still shows over the last film)
  }
  std::string EntranceInfo(bool probe) {
    using namespace assets::entrance;
    if (!ent.show) return Err("no entrance (send {\"cmd\":\"entrance\",\"wrestler\":161} first)");
    const Show& sh = *ent.show;
    std::string s = "{\"ok\":true,\"wrestler\":" + std::to_string(sh.wrestler) + ",\"file\":\"" + jl::Esc(sh.file) + "\",\"active\":" + (ent.active ? "true" : "false") +
                    ",\"playing\":" + (ent.playing ? "true" : "false") + ",\"speed\":" + Fix(ent.speed, 2) + ",\"intro_hold_s\":" + Fix(ent.intro_hold, 2) +
                    ",\"total_frames\":" + Fix(ent.tl.total, 0) + ",\"total_s\":" + Fix(ent.tl.total / 60.0, 2) + ",\"cues\":[";
    for (size_t i = 0; i < sh.cues.size(); ++i) {
      const Cue& c = sh.cues[i];
      std::string takes;
      for (auto& kv : c.cam) takes += (takes.empty() ? "" : ",") + std::to_string(kv.first);
      s += (i ? "," : "") + std::string("{\"i\":") + std::to_string(i) + ",\"cue\":" + std::to_string(c.sub) + ",\"len\":" + std::to_string(c.len) +
           ",\"start\":" + Fix(ent.tl.start[i], 0) + ",\"dur\":" + Fix(ent.tl.len[i], 0) + ",\"takes\":[" + takes + "]}";
    }
    s += "],\"entrance_segments\":[";  // Session 98 (timeline bar): the label per cue and the positions / zones it came from
    {
      const std::vector<assets::entrance::Segment>& segs = EntSegments();
      for (size_t i = 0; i < segs.size() && i < sh.cues.size(); ++i) {
        const assets::entrance::Segment& g = segs[i];
        s += (i ? "," : "") + std::string("{\"i\":") + std::to_string(i) + ",\"cue\":" + std::to_string(sh.cues[i].sub) + ",\"start\":" + Fix(ent.tl.start[i], 0) +
             ",\"dur\":" + Fix(ent.tl.len[i], 0) + ",\"label\":\"" + jl::Esc(g.label) + "\",\"from\":[" + Fix(g.a[0], 0) + "," + Fix(g.a[2], 0) + "],\"to\":[" +
             Fix(g.b[0], 0) + "," + Fix(g.b[2], 0) + "],\"zones\":\"" + SegZones(g) + "\"}";
      }
    }
    s += "],\"zone_rects\":[";
    for (size_t i = 0; i < sp.rects.size(); ++i) {
      const assets::spots::Rect& r = sp.rects[i];
      s += (i ? "," : "") + std::string("{\"light\":") + std::to_string(r.light) + ",\"x\":[" + Fix(r.xmin(), 0) + "," + Fix(r.xmax(), 0) + "],\"z\":[" + Fix(r.zmin(), 0) + "," + Fix(r.zmax(), 0) + "]}";
    }
    s += "]";
    {  // Session 98 (props general): every prop act (leaf, X, the placement mode per cue, the model drawn) and every companion leaf
      static const char* const kMode[] = {"zero", "hand", "world"};
      s += ",\"prop_acts\":[";
      for (size_t i = 0; i < sh.props.size(); ++i) {
        const auto& pa = sh.props[i];
        int pach = -1;
        bool belt = false, shown = false;
        const float* at = nullptr;
        for (const auto& p : ent.props) if (p.act == i) pach = p.pach, belt = p.belt, shown = p.shown, at = p.at;
        std::string md;
        for (const auto& [sub, m] : pa.mode) md += (md.empty() ? "" : ",") + std::string("\"") + std::to_string(sub) + "\":\"" + kMode[m < 0 || m > 2 ? 0 : m] + "\"";
        const auto pl = ent.played.find(i);
        // Session 99 (scene objects): this cue's motion against the prop model: tracks, tracks on a model node, and of those how many
        // move (more than one key in a component), the model's node count and the largest vertex bone index
        std::string trk;
        int model = -1;
        for (const auto& p : ent.props) if (p.act == i) model = p.model;
        const size_t ci = std::min(ent.cue, sh.cues.size() - 1);
        if (auto mi = pa.by_sub.find(sh.cues[ci].sub); mi != pa.by_sub.end() && model >= 0 && size_t(model) < model_bones.size()) {
          int on = 0, moving = 0, roots = 0;
          for (const auto& b : model_bones[size_t(model)]) roots += b.parent < 0 ? 1 : 0;
          for (const auto& t : mi->second.tracks) {
            bool hit = false;
            for (const auto& b : model_bones[size_t(model)]) if (assets::motion::NodeHash(b.name) == t.hash) { hit = true; break; }
            if (!hit) continue;
            ++on;
            // moving = its rotation turns by more than 1 degree, or its position by more than 1 unit, somewhere in the motion
            bool mv = false;
            assets::motion::Quat q0{0, 0, 0, 1};
            double p0[3] = {0, 0, 0};
            const bool hr = assets::motion::SampleRot(t, 0, q0), hp = assets::motion::SamplePos(t, 0, p0);
            for (int k = 1; k <= 32 && !mv; ++k) {
              const double f = mi->second.length * k / 32.0;
              assets::motion::Quat q{0, 0, 0, 1};
              double p[3] = {0, 0, 0};
              if (hr && assets::motion::SampleRot(t, f, q))
                mv = std::fabs(q[0] * q0[0] + q[1] * q0[1] + q[2] * q0[2] + q[3] * q0[3]) < 0.99996;
              if (hp && assets::motion::SamplePos(t, f, p))
                mv = mv || std::fabs(p[0] - p0[0]) + std::fabs(p[1] - p0[1]) + std::fabs(p[2] - p0[2]) > 1.0;
            }
            moving += mv ? 1 : 0;
          }
          trk = ",\"tracks\":" + std::to_string(mi->second.tracks.size()) + ",\"tracks_on_nodes\":" + std::to_string(on) + ",\"moving\":" +
                std::to_string(moving) + ",\"nodes\":" + std::to_string(model_bones[size_t(model)].size()) + ",\"root_nodes\":" + std::to_string(roots) +
                (size_t(model) < r.ModelCount() ? ",\"palette\":" + std::to_string(r.Models()[size_t(model)].model_bones) : std::string());
        }
        s += (i ? "," : "") + std::string("{\"leaf\":") + std::to_string(pa.leaf) + ",\"x\":" + std::to_string(pa.prop_id) + trk +
             (pl != ent.played.end() ? ",\"played\":" + std::to_string(pl->second) : std::string()) + ",\"pach\":" + std::to_string(pach) +
             ",\"belt\":" + (belt ? "true" : "false") + ",\"shown\":" + (shown ? "true" : "false") +
             (at ? ",\"at\":[" + Fix(at[0], 1) + "," + Fix(at[1], 1) + "," + Fix(at[2], 1) + "]" : std::string()) + ",\"mode\":{" + md + "}}";
      }
      // Session 98 (attire props): the keys of the game's prop rewrite and every change (evt_props.h)
      s += "],\"attire_props\":{\"prop_key\":" + std::to_string(ent.prop_key) + ",\"prop_key_src\":\"" + jl::Esc(ent.prop_key_src) +
           "\",\"title_key\":" + std::to_string(ent.title_key) + ",\"title_key_src\":\"" + jl::Esc(ent.title_key_src) + "\",\"tables_err\":\"" +
           jl::Esc(ent.tables_err) + "\",\"rewrites\":[";
      for (size_t i = 0; i < ent.rewrites.size(); ++i)
        s += (i ? "," : "") + std::string("{\"leaf\":") + std::to_string(ent.rewrites[i].leaf) + ",\"from\":" + std::to_string(ent.rewrites[i].from) +
             ",\"to\":" + std::to_string(ent.rewrites[i].to) + ",\"by\":\"" + jl::Esc(ent.rewrites[i].by) + "\"}";
      s += "]},\"companions\":[";
      for (size_t i = 0; i < sh.comps.size(); ++i) {
        std::string placed;
        for (const auto& [id, ci] : ent.comps) if (ci == i) placed = id;
        std::string subs;
        for (const auto& kv : sh.comps[i].by_sub) subs += (subs.empty() ? "" : ",") + std::to_string(kv.first);
        s += (i ? "," : "") + std::string("{\"leaf\":") + std::to_string(sh.comps[i].leaf) + ",\"x\":" + std::to_string(sh.comps[i].x) + ",\"char\":" +
             std::to_string(sh.comps[i].char_id) + ",\"actor\":\"" + placed + "\",\"cues\":[" + subs + "]}";
      }
      s += "]";
      {  // Session 99 (announcer)
        std::string cues;
        for (const auto& [sub, cm] : sh.ann.by_sub)
          cues += (cues.empty() ? "" : ",") + std::string("{\"cue\":") + std::to_string(sub) + ",\"src\":\"" + jl::Esc(cm.src) + "\",\"pos\":[" +
                  Fix(cm.pos[0], 1) + "," + Fix(-cm.pos[1], 1) + "," + Fix(cm.pos[2], 1) + "],\"heading\":" + Fix(cm.heading, 3) + "}";
        s += ",\"announcer\":{\"actor\":\"" + ent.ann_actor + "\",\"char\":" + std::to_string(ent.ann_id) + ",\"leaf_x\":" + std::to_string(sh.ann.x) +
             ",\"mic\":" + std::to_string(sh.ann.mic_id) + ",\"mic_model\":" + std::to_string(ent.ann_mic_model) + ",\"cues\":[" + cues + "]}";
      }
    }
    s += ",\"current\":{\"i\":" + std::to_string(ent.cue) + ",\"cue\":" + std::to_string(sh.cues[std::min(ent.cue, sh.cues.size() - 1)].sub) + ",\"t\":" + Fix(ent.t, 1) +
         ",\"local\":" + Fix(ent.local, 1) + ",\"root\":" + Vec3d(ent.root) + ",\"floor\":" + Fix(ent.floor_y, 1) + ",\"ground_dy\":" + Fix(ent.ground_dy, 1);
    {
      const EntCam cp = assets::entrance::Camera(sh.cues[std::min(ent.cue, sh.cues.size() - 1)], ent.take, ent.local);
      if (cp.ok) {
        const double e[3] = {cp.eye[0], -cp.eye[1], cp.eye[2]}, a2[3] = {cp.aim[0], -cp.aim[1], cp.aim[2]};
        s += ",\"cam_eye\":" + Vec3d(e) + ",\"cam_aim\":" + Vec3d(a2);
      }
    }
    if (Actor* pa = FindActor(ent.actor); pa && pa->have_motion && HasBip(model_bones[pa->model])) {  // the pose in numbers (log first: S98 ring height)
      std::string pinfo;
      assets::motion::BuildSkin(model_bones[pa->model], pa->motion, pa->frame, pa->has_opt ? pa->opt : popt, &pinfo);
      s += ",\"pose\":{\"origin_y\":" + Fix(pa->origin[1], 1) + ",\"feet_y\":" + Fix(pa->feet_y, 1) + ",\"floor\":" + Fix(pa->floor_y, 1) + ",\"ik\":[";
      bool first = true;
      for (const char* nm : {"Bip L Calf", "Bip R Calf"}) {
        const assets::motion::Track* t = pa->motion.Find(assets::motion::NodeHash(nm));
        double v[4];
        if (t && assets::motion::SampleIk(*t, pa->frame, v)) s += (first ? "" : ",") + std::string("[") + Fix(v[0], 1) + "," + Fix(v[1], 1) + "," + Fix(v[2], 1) + "," + Fix(v[3], 2) + "]", first = false;
      }
      s += "],\"skel\":\"" + jl::Esc(pinfo.substr(pinfo.rfind("; Bip Head") == std::string::npos ? 0 : pinfo.rfind("; Bip Head"))) + "\"}";
    }
    s += "}";
    s += ",\"root_y\":\"" + ent.root_y + "\",\"camera\":\"" + (ent.cam_entrance ? "entrance" : "free") + "\",\"take\":" + std::to_string(ent.take) +
         ",\"light_dirs\":\"" + (ent.light_keyed ? "keyed" : "arena") + "\",\"light_variant\":" + std::to_string(ent.light_variant) +
         ",\"rig\":" + (sh.rig.ok ? "true" : "false") + ",\"rope_leaves\":" + std::to_string(sh.ropes) + ",\"particle_effects\":" + std::to_string(pbank.effects.size()) + ",\"particle_objects\":" + std::to_string(pbank.placed.size()) +
         ",\"screen_flashes\":" + std::to_string(scene_lights.flashes.size()) + ",\"soft_switches\":" + std::to_string(scene_lights.soft.size()) +
         ",\"flash_now\":[" + Fix(flash_now[0], 3) + "," + Fix(flash_now[1], 3) + "," + Fix(flash_now[2], 3) + "," + Fix(flash_now[3], 3) + "],\"soft_range\":" + Fix(r.SoftParticles(), 1) +
         ",\"sprite_textures\":" + std::to_string(ptex.size()) + ",\"sprites\":" + std::to_string(r.SpriteCount()) +",\"unreadable\":[";
    for (size_t i = 0; i < sh.notes.size(); ++i) s += (i ? ",\"" : "\"") + jl::Esc(sh.notes[i]) + "\"";
    s += "],\"joins\":[";
    for (size_t i = 0; i + 1 < sh.cues.size(); ++i) {
      double e[3] = {0, 0, 0}, b[3] = {0, 0, 0};
      Root(sh.cues[i], sh.cues[i].len, e), Root(sh.cues[i + 1], 0, b);
      const double dx = b[0] - e[0], dy = b[1] - e[1], dz = b[2] - e[2];
      s += (i ? "," : "") + std::string("{\"from\":") + std::to_string(sh.cues[i].sub) + ",\"to\":" + std::to_string(sh.cues[i + 1].sub) + ",\"end\":" + Vec3d(e) +
           ",\"start\":" + Vec3d(b) + ",\"file_dist_xz\":" + Fix(std::hypot(dx, dz), 2) + ",\"file_dist\":" + Fix(std::sqrt(dx * dx + dy * dy + dz * dz), 2) + ",\"played_gap\":" + Fix(i == 0 ? -1.0 : std::sqrt(std::pow(dx + (i + 1 < ent.off.size() && i >= 1 ? ent.off[i + 1][0] - ent.off[i][0] : 0.0), 2) + std::pow(dy + (i + 1 < ent.off.size() && i >= 1 ? ent.off[i + 1][1] - ent.off[i][1] : 0.0), 2) + std::pow(dz + (i + 1 < ent.off.size() && i >= 1 ? ent.off[i + 1][2] - ent.off[i][2] : 0.0), 2)), 2) + "}";
    }
    s += "]";
    if (probe) {  // light directions vs the game's draw 814 numbers of swap 6728 (cue 0, slot 1, frame 68): every convention
      const double want[3][3] = {{0.1788, 0.6507, 0.738}, {-0.2033, 0.9634, 0.1745}, {-0.1777, 0.6557, -0.7338}};
      s += ",\"light_probe\":{";
      if (!sh.cues.empty() && sh.rig.ok) {
        assets::LightSet k;
        const EntCam cp = assets::entrance::Camera(sh.cues[0], ent.take, 68);
        if (assets::entrance_light::EvalSlot(sh.rig, 0, 1, 68, k) && cp.ok) {
          s += "\"raw\":[";
          for (int i = 0; i < 3; ++i) s += (i ? "," : "") + std::string("[") + Fix(k.dir[i][0], 4) + "," + Fix(k.dir[i][1], 4) + "," + Fix(k.dir[i][2], 4) + "]";
          s += "],\"cam_eye\":" + Vec3d(cp.eye) + ",\"cam_aim\":" + Vec3d(cp.aim) + ",\"cam_q\":[" + Fix(cp.q[0], 4) + "," + Fix(cp.q[1], 4) + "," + Fix(cp.q[2], 4) + "," + Fix(cp.q[3], 4) +
               "],\"variants\":[";
          for (int v = 0; v < 8; ++v) {
            double res = 0;
            std::string dirs;
            bool ok = true;
            for (int i = 0; i < 3; ++i) {
              const double d[3] = {k.dir[i][0], k.dir[i][1], k.dir[i][2]};
              double o[3];
              if (!RotateDir(cp, v, d, o)) { ok = false; break; }
              for (int a = 0; a < 3; ++a) res += std::fabs(o[a] - want[i][a]);
              dirs += (i ? "," : "") + Vec3d(o);
            }
            s += (v ? "," : "") + std::string("{\"v\":") + std::to_string(v) + (ok ? ",\"sum_abs_diff\":" + Fix(res, 4) + ",\"dirs\":[" + dirs + "]" : ",\"error\":\"no rotation key\"") + "}";
          }
          s += "],\"scan\":[";  // which (take, camera frame, convention) reproduces the game's numbers best
          struct Hit { double res; int take, v, cue; double f; };
          std::vector<Hit> hits;
          for (size_t ci = 0; ci < sh.cues.size(); ++ci) for (const auto& tk : sh.cues[ci].cam)
            for (double f = 0; f <= sh.cues[ci].len; f += 4)
              for (int v = 0; v < 8; ++v) {
                const EntCam c2 = assets::entrance::Camera(sh.cues[ci], tk.first, f);
                if (!c2.ok) continue;
                double res = 0;
                bool ok = true;
                for (int i = 0; i < 3 && ok; ++i) {
                  const double d[3] = {k.dir[i][0], k.dir[i][1], k.dir[i][2]};
                  double o[3];
                  ok = RotateDir(c2, v, d, o);
                  for (int a = 0; ok && a < 3; ++a) res += std::fabs(o[a] - want[i][a]);
                }
                if (ok) hits.push_back({res, tk.first, v, (int)sh.cues[ci].sub, f});
              }
          std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.res < b.res; });
          for (size_t i = 0; i < hits.size() && i < 6; ++i)
            s += (i ? "," : "") + std::string("{\"res\":") + Fix(hits[i].res, 4) + ",\"cue\":" + std::to_string(hits[i].cue) + ",\"take\":" + std::to_string(hits[i].take) + ",\"v\":" + std::to_string(hits[i].v) + ",\"f\":" + Fix(hits[i].f, 0) + "}";
          s += "]";
        } else s += "\"error\":\"no slot 1 / camera at cue 0\"";
      } else s += "\"error\":\"no rig\"";
      s += "}";
    }
    return s + "}";
  }
  static std::string Vec3d(const double* v) { return "[" + Fix(v[0], 1) + "," + Fix(v[1], 1) + "," + Fix(v[2], 1) + "]"; }
  // {"cmd":"entrance","wrestler":161,"attire":0,"play":true,"root_y":"ground"|"raw"|"scale","camera":"entrance"|"free","take":0,
  //  "cue":0..20 (start there),"t":seconds,"intro_hold":0 (seconds cue 0 is held),"cue0":"hide"|"show","light_dirs":"keyed"|"arena",
  //  "lights":true,"stage":true (the wrestler's tron / stage look, reloads the arena),"tscale":1.0,"speed":1}
  // Places the wrestler as the actor "ent" at his entrance path and plays act 25000's cues back to back (hard cuts).
  jl::Value last_ent_cmd;  // Session 98 (gen check 5): the last entrance command (entrance_title re-runs it with new champion options)
  // Session 98 (first phone test: the UI plays entrances now): {"cmd":"entrance_stop"} ends the entrance - its extras (belt, props, the
  // announcer's mic out of sight; companions and the announcer removed, as a new entrance does) and the entrant "ent" himself (his lights
  // go back: RemoveActorAt). The UI sends it when LOOK goes back to MATCH. No entrance: nothing happens (ok).
  std::string DoEntranceStop() {
    const bool had = ent.show != nullptr;
    ClearEntranceExtras();
    for (size_t k = 0; k < actors.size(); ++k)
      if (actors[k]->entrance) { RemoveActorAt(k); break; }
    ent = EntranceState();
    last_ent_cmd = jl::Value();
    StopParticles();
    return std::string("{\"ok\":true,\"stopped\":") + (had ? "true" : "false") + "}";
  }
  // A previous entrance's own models out of sight and its extra actors removed (the start of every entrance, and entrance_stop).
  void ClearEntranceExtras() {
    // A previous entrance's own models - the belt (gen check 4), the hand props (gen check 6), the announcer's microphone (Session 99) -
    // removed, highest index first (each removal shifts the ones above it: RemoveModelIndex). Session 98 phone fixes review: before,
    // they were only moved out of sight and piled up with every restart from the UI.
    std::vector<size_t> gone;
    auto take = [&](int m) { if (m > 0 && size_t(m) < r.ModelCount()) gone.push_back(size_t(m)); };  // (never model 0: the arena)
    take(ent.belt_model), take(ent.ann_mic_model);
    for (const auto& p : ent.props) take(p.model);
    ent.belt_model = -1, ent.ann_mic_model = -1;
    ent.props.clear();
    std::sort(gone.begin(), gone.end());
    gone.erase(std::unique(gone.begin(), gone.end()), gone.end());
    for (size_t i = gone.size(); i-- > 0;) {
      prop_lights_orig.erase(int(gone[i]));
      RemoveModelIndex(gone[i]);
    }
    // (gen check 5) a previous entrance's companions: exactly the actors this entrance placed (ent.comps), never a user's "compare"
    for (const auto& [cid, ci] : ent.comps)
      for (size_t k = 0; k < actors.size(); ++k)
        if (actors[k]->id == cid) { RemoveActorAt(k); break; }
    ent.comp = false;
    ent.comps.clear();
    // Session 99 (announcer): a previous entrance's announcer actor, the same way
    // (review) by identity, never a user's actor that took the id since
    if (ent.ann_ptr)
      for (size_t k = 0; k < actors.size(); ++k)
        if (actors[k].get() == ent.ann_ptr) { RemoveActorAt(k); break; }
    ent.ann_actor.clear(), ent.ann_ptr = nullptr;
  }
  std::string DoEntrance(const jl::Value& cmd) {
    if (!cmd.has("wrestler")) return Err("missing wrestler");
    ClearEntranceExtras();
    // Session 98: "custom":"name" = this wrestler's entrance (motions, camera, lights, tron) played by an imported character's model.
    int cid = -1;
    if (cmd.has("custom")) {
      assets::char_mods::Entry ce;
      if (!assets::char_mods::FindName(cmd.str("custom", ""), ce)) return Err("no imported character '" + cmd.str("custom", "") + "' (char_list; the mods toggle must be on)");
      cid = ce.id;
    }
    if (last_arena < 0) return Err("load an arena first ({\"cmd\":\"load\",\"kind\":\"arena\",\"id\":5})");
    const int wid = IntArg(cmd.num("wrestler", 0));
    // attire: "entrance" (default) = the entrance model of game attire "game_attire" (default 0) named by the pac's own directory
    // (asset_load.h EntranceModelIndex: "..A4" else the ring model "..A2"; ch161 -> PACH 1 the T-shirt, ch100 -> PACH 0, ch324 -> PACH 1);
    // a pac whose directory names none: PACH 1 when it exists, else PACH 0 (the old rule); or a number (a PACH index)
    const jl::Value* attv = cmd.get("attire");
    const bool attire_entrance = !attv || (attv->t == jl::Value::Str && attv->s == "entrance");
    int attire = attire_entrance ? 1 : IntArg(cmd.num("attire", 0));
    bool attire_named = false;
    if (attire_entrance) {
      const int ea = ViewerEntranceAttire(cid >= 0 ? cid : wid, IntArg(cmd.num("game_attire", 0)));
      if (ea >= 0) attire = ea, attire_named = true;
    }
    // Session 98 (gen check 4): the CHAMPION entrance. The game plays a separate title entrance for a champion: run 6820 (Cena, the
    // save's WWE Champion) loaded evt\Entrance06.pac, whose sections are banks 600-677 named "<Wrestler>_Champ" (602 John_Cena_Champ,
    // 664 John_Cena_Champ_Black_Shirt, 655 Alberto_Del_Rio_Champ, 660 Daniel Bryan Champ, 609 The_Rock_Champ, ...) plus generic ones
    // (610-613 General_Champ_A/B/C, Cruiser; size variants _Normal / _Bigsize / _Divasize). "bank":N plays entrance bank N with this
    // wrestler's model; "title":"none" (default) | "hold" (the belt in the left hand, "belt" default 352 = the WWE Championship of run
    // 6820) | "waist" (not ported: the waist attach is not known yet) | "auto" (= none: the champion comes from the save, which the
    // viewer cannot read). Which champ bank the game picks for a wrestler (own / general / size class) is not decoded [U].
    std::string title = cmd.str("title", "none");
    if (title != "none" && title != "hold" && title != "waist" && title != "auto") return Err("title must be none, hold, waist or auto");
    int bank = IntArg(cmd.num("bank", wid));
    // (gen check 5) "auto" = the wrestler as champion: his own Entrance06 champion bank with the belt in hand when one is named after
    // him (entrance.h ChampBank), else his own entrance with the belt on the waist. [U] not the game's rule (it reads the save's
    // entrance choice: Del Rio, the champion of run 6821, used bank 416 + the waist although 655 exists).
    if (title == "auto") {
      const int cb = cmd.has("bank") ? -1 : ViewerChampBank(wid);
      if (cb > 0) bank = cb, title = "hold";
      else title = "waist";
    }
    auto show = std::make_shared<assets::entrance::Show>();
    std::string err;
    if (!ViewerEntrance(bank, *show, err)) return Err(err);
    // Session 98 (gen check 6): an entrance from another bank (champion / companion section) is lit by the wrestler's OWN bank's rig
    // (entrance.h LoadRig: probe run 6820, Cena in 602 used 139's rig). "rig_bank":N picks one (A/B: "rig_bank":602 = before).
    const int rig_bank = IntArg(cmd.num("rig_bank", wid));
    if (rig_bank != bank) {
      std::string rerr;
      if (!ViewerEntranceRig(rig_bank, *show, rerr) && cmd.has("rig_bank")) return Err("rig_bank: " + rerr);
    }
    const std::string ry = cmd.str("root_y", "canvas");  // Session 98 (replay S5): the motion's own height (see ApplyEntrance)
    if (ry != "ground" && ry != "raw" && ry != "scale" && ry != "canvas") return Err("root_y must be ground, raw, scale or canvas");
    const std::string cm = cmd.str("camera", "free");
    if (cm != "free" && cm != "entrance") return Err("camera must be free or entrance");
    const std::string ld = cmd.str("light_dirs", "keyed");
    if (ld != "keyed" && ld != "arena") return Err("light_dirs must be keyed or arena");
    const std::string c0 = cmd.str("cue0", "hide");
    if (c0 != "hide" && c0 != "show") return Err("cue0 must be hide or show");
    // Session 98 (gen check 6): "absolute" is the default: the game plays every cue on the motion's own path [C: probe run 6821 (bank 416),
    // the palette Bip of Del Rio / Ricardo vs the cue roots: stage cue 5 f301 Ricardo (94.5, 2613.9) vs (94.2, 2615.7); ramp cue 10 f301
    // Del Rio (-26.8, 2135.9) vs (-26.5, 2135.8), Ricardo (50.1, 2286.7) vs (49.9, 2286.7); ring cue 20 f122 Del Rio (-173.1, 19.9) vs
    // (-172.6, 20.0), Ricardo (71.5, 83.5) vs (71.6, 83.8) - while cue 15 starts 124 units off cue 10's end, which "chain" carried into
    // cue 20 (Del Rio stood at the wrong side of the ring)]. "chain" stays as an A/B.
    const std::string jn = cmd.str("join", "absolute");
    if (jn != "chain" && jn != "absolute") return Err("join must be chain or absolute");
    // (every argument is checked before the arena is reloaded or the actor placed: a bad command leaves no half-made entrance)
    const double intro_hold = std::max(0.0, cmd.num("intro_hold", 0));
    const assets::entrance::Timeline tl0 = assets::entrance::Build(*show, intro_hold);
    double start_t = 0;
    if (cmd.has("cue")) {
      const int want = IntArg(cmd.num("cue", 0));
      bool hit = false;
      for (size_t i = 0; i < show->cues.size(); ++i)
        if (show->cues[i].sub == want) start_t = tl0.start[i] + std::max(0.0, cmd.num("f", 0)), hit = true;
      if (!hit) return Err("this entrance has no cue " + std::to_string(want));
    }
    if (cmd.has("t")) start_t = std::max(0.0, cmd.num("t", 0)) * 60.0;
    std::string note;
    if (BoolArg(cmd, "stage", true) && stage_wrestler != wid) {  // the wrestler's own tron + stage media (reloads the arena: actors are cleared)
      const std::string rep = Execute("{\"cmd\":\"stage\",\"wrestler\":" + std::to_string(wid) + ",\"light\":false}");
      if (rep.find("\"ok\":true") == std::string::npos) note = "stage: " + rep;
    }
    auto place_cmd = [&](int att) {
      return Safe("{\"cmd\":\"place\",\"wrestler\":" + std::to_string(cid >= 0 ? cid : wid) + ",\"attire\":" + std::to_string(att) +
                  ",\"pos\":[0,0,0],\"yaw\":180,\"id\":\"ent\",\"motion\":\"none\",\"light\":\"rig\",\"anchor\":false}");
    };
    std::string place = place_cmd(attire);
    if (attire_entrance && !attire_named && place.find("no_attire") != std::string::npos) attire = 0, place = place_cmd(0);  // no PACH 1: the ring model
    if (place.find("\"ok\":true") == std::string::npos) return place;
    // Session 98 (attire props): table 101's key = the performer's model id (evt_props.h), read here while his pac is the cached one.
    // "prop_key":N sets it (51200 = none: no table-101 lookup, the game's "no performer" value).
    int prop_key = -1;
    std::string prop_key_src;
    if (cmd.has("prop_key")) prop_key = IntArg(cmd.num("prop_key", 51200)), prop_key_src = "arg";
    else if (cid >= 0) prop_key = 51200, prop_key_src = "custom character: none (51200, no table-101 lookup) [U]";
    else if ((prop_key = ViewerModelKey(wid, attire)) >= 0) prop_key_src = "model directory";
    else prop_key = wid * 100 + IntArg(cmd.num("game_attire", 0)) * 10 + 4, prop_key_src = "inferred (wrestler*100 + game_attire*10 + 4)";
    Actor* a = FindActor("ent");
    if (!a) return Err("entrance actor missing");
    a->entrance = true, a->has_opt = true, a->opt = popt, a->opt.tscale = cmd.num("tscale", 1.0), a->anchor = false, a->ent_cue = -1;
    // The entrance numbers are -Y up for the root AND the ankle targets (flip bit 1 off); the Bip sample is a delta on the bind Bip
    // translation like every position track (the game's rule, motion.h PoseOpts::root_delta). "root_delta":false = the sample as is (A/B).
    a->opt.flip = popt.flip & ~2;
    a->opt.root_delta = BoolArg(cmd, "root_delta", true);
    ent = EntranceState();
    ent.show = show;
    ent.actor = "ent";
    ent.intro_hold = intro_hold;
    ent.tl = tl0;
    ent.root_y = ry, ent.cam_entrance = cm == "entrance", ent.show_cue0 = c0 == "show", ent.light_keyed = ld == "keyed";
    ent.lights_on = BoolArg(cmd, "lights", true), ent.take = IntArg(cmd.num("take", 1)), ent.light_variant = IntArg(cmd.num("light_variant", 0)) & 7;
    ent.speed = std::max(0.0, cmd.num("speed", 1.0));
    ent.cam_dy = cmd.num("cam_dy", 120);
    ent.l4 = BoolArg(cmd, "l4", true);
    ent.arena_model = 0;
    ent.chain = jn == "chain";
    ent.off = ent.chain ? assets::entrance::ChainOffsets(*show) : std::vector<std::array<double, 3>>(show->cues.size(), std::array<double, 3>{0, 0, 0});
    // Session 98 (gen check 4): "belt":N (evtobj.pac PACH, e.g. 352 = the WWE Championship Cena carried in runs 6816 / 6820) loads the
    // belt and keeps it in the entrant's left hand (UpdateBelt). The game takes the champion from the save; the viewer cannot read it,
    // so there is no automatic choice: "belt" absent / 0 = none.
    // (gen check 5) the waist belts are their own models, PACHs 451-502 (five meshes, the strap pre-shaped round the waist); 459 = the
    // one Del Rio wore in run 6821. The hand belts are 349-450.
    prop_lights_orig.clear();  // Session 98 (sweep look): new belt / prop models below
    ent.belt = IntArg(cmd.num("belt", title == "hold" ? 352 : title == "waist" ? 459 : 0));
    if (title == "none" || title == "auto") ent.belt = cmd.has("belt") ? ent.belt : 0;
    ent.belt_motion = BoolArg(cmd, "belt_motion", true);
    // Session 98 (props general): every title prop of every bank is item 9 (2009 left / 3009 right / 5009 waist; no bank names a belt,
    // census); held ("hold"), the title acts of the bank are played by the chosen title's model of that hand's run (BeltTwin), each
    // posed by its own act like any prop (UpdateProps). The one static belt of UpdateBelt is always loaded: it is the belt of the cues
    // with no title act at all (602's cue 6), of a bank whose title models could not be loaded (no BeltTwin: 364, 379, 388, 389, 397),
    // and of "belt_motion":false / the waist (review fix: it was dropped whenever the bank had a title act).
    const bool title_acts = title == "hold" && ent.belt > 0 && ent.belt_motion;
    if (ent.belt > 0) {
      jl::Value lc;
      lc.t = jl::Value::Obj;
      jl::Value k, i;
      k.t = jl::Value::Str, k.s = "prop";
      i.t = jl::Value::Num, i.n = ent.belt;
      lc.keys = {"kind", "id"}, lc.vals = {k, i};
      const std::string lr = DoLoad(lc, true);
      if (lr.find("\"ok\":true") == std::string::npos) note += (note.empty() ? "" : "; ") + std::string("belt: ") + lr;
      else ent.belt_model = int(r.ModelCount()) - 1;
    }
    ent.belt_waist = title == "waist";
    // (gen check 6: on by default - the prop motion's `base` track is the belt's root in the hand's frame, entrance.h PropAttach; the
    // waist belt keeps the measured pose. "belt_motion":false = the measured static hand pose of belt_pose.h)
    // Session 98 (gen check 6 / props general): the props of the prop acts: one evtobj model per act with a 2xxx / 3xxx prop id (hand or
    // world, Show::PropAct::mode); the title acts (item 9) only when held (above). "props":false leaves them out. The model is found by
    // the id's record in evtobj.pac's own directory (asset_load.h PropDirIn: every id of the census resolves; 2717's model has no entry
    // 0 and stays skipped); a prop whose motions are all unused (kPropZero) is skipped unless it is worn (a cap, PropWorn); the skipped
    // ones are listed once in the reply's note.
    ent.props_on = BoolArg(cmd, "props", true);
    // Session 98 (attire props): the game rewrites the prop acts' ids before it loads them (evt_props.h): table 103 by the held title,
    // then (records 12+) the fixed cases and table 101 by the performer's model id. Both tables come from the player's evtdata.pac.
    // "game_props":false = the bank's own ids (before); "title_key":N picks table 103's row; "mode40" / "drop_2758" / "swap_2072" set
    // the game state the fixed cases read (defaults: off) [U what they are].
    ent.prop_key = prop_key, ent.prop_key_src = prop_key_src;
    std::vector<std::vector<int>> t101, t103;
    const bool game_props = BoolArg(cmd, "game_props", true);
    bool have101 = false, have103 = false;
    if (game_props) {
      std::string e1, e3;
      have101 = ViewerEvtTable(101, t101, e1);
      have103 = ViewerEvtTable(103, t103, e3);
      if (!have101 || !have103) ent.tables_err = !have101 ? e1 : e3;
    } else {
      ent.tables_err = "game_props off";
    }
    assets::evt_props::Flags eflags;
    eflags.mode40 = BoolArg(cmd, "mode40", false), eflags.drop_2758 = BoolArg(cmd, "drop_2758", false), eflags.swap_2072 = BoolArg(cmd, "swap_2072", false);
    if (have103 && title_acts) {
      // The held title's row: the one whose left-hand id (column 4) is, by evtobj's own directory, the chosen belt's left-hand model
      // [U: the game takes the key from the entrant's setup block (byte +44 + class through sub_8230F7D0); this inverse is ours].
      if (cmd.has("title_key")) ent.title_key = IntArg(cmd.num("title_key", -1)), ent.title_key_src = "arg";
      else {
        const int left = ViewerBeltTwin(ent.belt, 2);
        for (const auto& row : t103)
          if (left >= 0 && row.size() > 4 && row[0] != 255 && row[4] > 0 && ViewerPropPachDir(unsigned(row[4])) == left) {
            ent.title_key = row[0], ent.title_key_src = "belt " + std::to_string(ent.belt) + " = row " + std::to_string(row[0]) + "'s " + std::to_string(row[4]);
            break;
          }
        if (ent.title_key < 0) ent.title_key_src = "belt " + std::to_string(ent.belt) + ": no table-103 row names its model";
      }
    }
    std::string skipped;
    for (size_t i = 0; ent.props_on && i < show->props.size(); ++i) {
      const auto& pa = show->props[i];
      const int kind = assets::entrance::PropAttachKind(pa.prop_id);
      const bool is_title = pa.prop_id % 1000 == 9;
      if (kind != 2 && kind != 3) continue;  // (5xxx = the waist title: UpdateBelt)
      if (is_title && !title_acts) continue;
      uint32_t game_id = pa.prop_id;
      auto log_rw = [&](uint32_t to, const std::string& by) {
        if (to != game_id) ent.rewrites.push_back({pa.leaf, game_id, to, by}), game_id = to;
      };
      // (review) like sub_8230B4C0: only an id with a "/BOBJ/%04d" model (sub_82310000) goes through table 103
      if (have103 && ent.title_key >= 0 && ViewerPropBobj(game_id)) log_rw(assets::evt_props::Rewrite103(t103, ent.title_key, game_id), "table 103 key " + std::to_string(ent.title_key));
      if (have101 && pa.leaf >= 12 && game_id != 0) {
        log_rw(assets::evt_props::Fixed101(game_id, eflags), "fixed");
        if (game_id != 0 && prop_key >= 0 && prop_key != 51200) log_rw(assets::evt_props::Rewrite101(t101, prop_key, game_id), "table 101 key " + std::to_string(prop_key));
      }
      if (game_id == 0) continue;  // (the game's rule removes it)
      ent.played[i] = game_id;
      bool used = false;
      for (const auto& [sub, md] : pa.mode) used = used || md != assets::entrance::kPropZero;
      // Session 98 (cap / particles): a cap / hat model (asset_load.h PropWorn) is used in every cue it has a motion for: a still-zero
      // cue wears it on the head (UpdateProps) [C: run 6824, Cena's 2718 in bank 602]. ("worn":false = before: skipped like any zero act)
      const bool can_wear = !is_title && !pa.mode.empty() && BoolArg(cmd, "worn", true);
      // Session 98 (cap / particles): "prop_swap":{"2718":2720} plays an act's prop as another id (keyed by the bank's id, else by the
      // id after the game's rewrite above), an override over the game's rule. (Run 6824 drew evtobj 2720 for bank 602's act 2718 on
      // Cena in the yellow attire: table 101's rows 13902 / 13904 / 13912 [C: evt_props.h, run 071].)
      if (const jl::Value* sw = cmd.get("prop_swap"); sw && sw->t == jl::Value::Obj) {
        const jl::Value* to = sw->get(std::to_string(pa.prop_id).c_str());
        if (!to) to = sw->get(std::to_string(game_id).c_str());
        if (to && to->t == jl::Value::Num && to->n > 0) log_rw(uint32_t(to->n), "prop_swap");
        ent.played[i] = game_id;
      }
      const uint32_t play_id = game_id;
      // A title act: the model of the rewritten id when table 103 changed it (evtobj's directory: the game's "/SOBJ" then "/BOBJ"
      // lookup), else the chosen belt's twin in that hand's run (BeltTwin).
      int pach = -1;
      if (is_title && play_id != pa.prop_id) pach = ViewerPropPachDir(play_id);
      if (is_title && pach < 0) pach = ViewerBeltTwin(ent.belt, kind);
      if (!is_title) pach = ViewerPropPach(play_id);
      const bool worn =can_wear && pach >= 0 && ViewerPropWorn(pach);
      if (!used && !worn) continue;
      if (pach < 0) {
        skipped += (skipped.empty() ? "" : ", ") + std::to_string(pa.prop_id) + " (leaf " + std::to_string(pa.leaf) + ")";
        std::fprintf(stderr, "entrance %d: prop %u (leaf %d) has no evtobj model: skipped\n", bank, pa.prop_id, pa.leaf);
        continue;
      }
      jl::Value lc;
      lc.t = jl::Value::Obj;
      jl::Value k, iv;
      k.t = jl::Value::Str, k.s = "prop";
      iv.t = jl::Value::Num, iv.n = pach;
      lc.keys = {"kind", "id"}, lc.vals = {k, iv};
      const std::string lr = DoLoad(lc, true);
      if (lr.find("\"ok\":true") == std::string::npos) { note += (note.empty() ? "" : "; ") + std::string("prop ") + std::to_string(pa.prop_id) + ": " + lr; continue; }
      EntranceState::Prop p;
      p.act = i, p.model = int(r.ModelCount()) - 1, p.pach = pach, p.belt = is_title, p.worn = worn;
      ent.props.push_back(p);
      if (is_title) ++ent.title_props;
    }
    if (!skipped.empty()) note += (note.empty() ? "" : "; ") + std::string("props without a model (skipped): ") + skipped;
    // Session 98 (cap / particles): the bank's particle effects (prtcls.pac PCE6) and their sprite textures, uploaded once
    r.ClearSprites(), ptex.clear(), psim.clear(), pbank = assets::particles::Bank();
    particles_on = BoolArg(cmd, "particles", true);
    // Session 99 (scene objects): the bank's GMEV screen flashes / soft-particle switches (particles.h SceneLights); "scene_lights":false = none
    scene_lights = assets::particles::SceneLights();
    scene_lights_on = BoolArg(cmd, "scene_lights", true);
    if (scene_lights_on) ViewerSceneLights(bank, scene_lights);
    // Session 99 (smoke density): "particle_seed":k = another random draw of every placed effect (0 = the default; the game's own
    // generator is shared by every system, so its draws cannot be matched: one frame is ONE sample, k > 0 gives others to compare)
    particle_seed = uint32_t(std::max(0, std::min(1 << 20, IntArg(cmd.num("particle_seed", 0)))));
    if (particles_on) {
      std::string pe;
      if (!ViewerParticles(bank, pbank, pe)) pbank = assets::particles::Bank();
      // (review) a floor-relative object's floor: once, at its own x / z (not the entrant's height, which changes as he walks)
      // The arena floor (EntFloorAt) misses a raised stage deck (Kane object 5 at (0, 2529) in arena 0 read 0, the deck is 180: the
      // smoke sank under the deck), so the stage meshes' surface under it (ground.Top, as the entrant's lift) wins when higher.
      for (auto& pl : pbank.placed)
        if (pl.floor_rel) {
          pl.floor = EntFloorAt(pl.pos[0], pl.pos[2]);
          float h = 0;
          if (ground.Top(pl.pos[0], pl.pos[2], pl.floor + 300.f, h) && h > pl.floor + 5.f) pl.floor = h;
        }
      // (Session 99) only the effects that can play (pbank.Drawn), one upload per texture (the copies of a group share one set)
      std::map<std::pair<int, std::string>, int> once;
      for (const auto& [key, t] : pbank.sprites) {
        if (!pbank.Drawn(key.first)) continue;  // (not drawn: UpdateParticles)
        const auto fx = pbank.effects.find(key.first);
        const std::pair<int, std::string> k2{fx != pbank.effects.end() ? fx->second.tex_from : key.first, t.name};
        if (auto s = once.find(k2); s != once.end()) { ptex[key] = s->second; continue; }
        if (const int slot = r.AddSpriteTexture(t); slot >= 0) ptex[key] = slot, once[k2] = slot;
      }
    }
    ent.title = cmd.str("title", "none"), ent.bank = bank, ent.wrestler = wid;
    last_ent_cmd = cmd;
    // Session 98 (gen check 5 / props general): the entrance companions (Show::comps with a named performer; e.g. bank 416 -> Ricardo
    // 323, bank 423 -> Cameron 354 + Naomi 355), each played with its leaf's motions on the same clock as the entrant, actor ids "comp",
    // "comp2", .. "companion":false leaves them out.
    for (size_t ci = 0; BoolArg(cmd, "companion", true) && ci < show->comps.size(); ++ci) {
      const int who = show->comps[ci].char_id;
      if (who <= 0) continue;
      const std::string id = ent.comps.empty() ? "comp" : "comp" + std::to_string(ent.comps.size() + 1);
      const int ca = ViewerEntranceAttire(who, 0);
      const std::string cr = Safe("{\"cmd\":\"place\",\"wrestler\":" + std::to_string(who) + ",\"attire\":" + std::to_string(ca >= 0 ? ca : 0) +
                                  ",\"pos\":[0,0,0],\"yaw\":180,\"id\":\"" + id + "\",\"motion\":\"none\",\"light\":\"rig\",\"anchor\":false}");
      if (cr.find("\"ok\":true") == std::string::npos) note += (note.empty() ? "" : "; ") + std::string("companion ") + std::to_string(who) + ": " + cr;
      else if (Actor* ca2 = FindActor(id)) {
        ca2->entrance = false, ca2->has_opt = true, ca2->opt = a->opt, ca2->anchor = false, ca2->ent_cue = -1;
        ent.comp = true;
        ent.comps.push_back({id, ci});
      }
    }
    // Session 99 (announcer): the ring announcer (entrance.h Show::Announcer) on the entrance clock, actor id "announcer"; his
    // microphone (leaf 9's prop, evtobj model) in his hand. "announcer":false leaves him out; "announcer_id":N plays him with
    // character N; "announcer_turn" = the heading's sign (A/B; the probe decided the default, out\s98\announcer\notes.md).
    if (BoolArg(cmd, "announcer", true) && !show->ann.by_sub.empty()) {
      const double turn = cmd.num("announcer_turn", kAnnouncerTurn);
      // The segments: each cue starts its own motion at its start (cues 0-10: sub_8230B680's rows; a bank's own leaf-8 motion), except
      // the "late" cues (rows 15-20, act -1 there): the motion before runs on, and sub_82320878's 00200110 starts `announcer_late_at`
      // frames into the first late cue and runs on through the next ones [C: probe run 6818 (Orton): with 00200110 from cue 15 f371, the
      // ring-in (f767) and ring (cue 20 f122) frames both fit the game's bones (10 / 12 units); from the cue's start they do not (29+);
      // U: one entrance measured - the game's trigger (sub_82320878's state machine, audio-timed?) is not decoded].
      // (A/B) "announcer_late":"NAME" plays that evtmot motion instead; "announcer_late_at":N moves its start.
      const std::string late = cmd.str("announcer_late", "");
      const double late_at = std::max(0.0, cmd.num("announcer_late_at", kAnnouncerLateAt));
      bool late_started = false;
      for (size_t i = 0; i < show->cues.size(); ++i) {
        const int sub = show->cues[i].sub;
        auto it = show->ann.by_sub.find(sub);
        EntranceState::AnnSeg sg;
        sg.cue = i, sg.off = 0;
        if (it == show->ann.by_sub.end()) { ent.ann_seg.push_back(sg), late_started = false; continue; }  // (not there in this cue)
        const auto& cm = it->second;
        assets::motion::Motion m = cm.mo;
        if (cm.late && !late.empty()) {
          auto sm = show->ann.shared.find(late);
          if (sm != show->ann.shared.end()) m = sm->second;
          else if (note.find("announcer_late") == std::string::npos) note += (note.empty() ? "" : "; ") + std::string("announcer_late: evtmot has no motion ") + late;
        }
        if (cm.late && late_started) continue;  // (the late motion runs on)
        if (!cm.own) assets::entrance::PlaceAt(m, cm.pos, turn * cm.heading);
        sg.loop = !cm.own;
        if (cm.late) {
          late_started = true;
          sg.off = late_at;  // (clamped to the cue's length where it is resolved: AnnSegAt)
        } else {
          late_started = false;
        }
        ent.ann_mo.push_back(std::move(m));
        sg.mo = int(ent.ann_mo.size()) - 1;
        ent.ann_seg.push_back(sg);
      }
      ent.ann_seg_cur = -2;
      const int who = IntArg(cmd.num("announcer_id", show->ann.char_id > 0 ? show->ann.char_id : assets::entrance::ArenaAnnouncer(last_arena)));
      const int aa = ViewerEntranceAttire(who, 0);
      std::string aid = "announcer";  // (review) a user's own actor of that name is never replaced: the next free id
      for (int n = 2; FindActor(aid); ++n) aid = "announcer" + std::to_string(n);
      const std::string ar = Safe("{\"cmd\":\"place\",\"wrestler\":" + std::to_string(who) + ",\"attire\":" + std::to_string(aa >= 0 ? aa : 0) +
                                  ",\"pos\":[0,0,0],\"yaw\":180,\"id\":\"" + aid + "\",\"motion\":\"none\",\"light\":\"rig\",\"anchor\":false}");
      if (ar.find("\"ok\":true") == std::string::npos) note += (note.empty() ? "" : "; ") + std::string("announcer ") + std::to_string(who) + ": " + ar;
      else if (Actor* an = FindActor(aid)) {
        an->entrance = false, an->has_opt = true, an->opt = a->opt, an->anchor = false, an->ent_cue = -1;
        ent.ann_actor = aid, ent.ann_ptr = an, ent.ann_id = who;
        const uint32_t mic = show->ann.mic_id;
        const int kind = assets::entrance::PropAttachKind(mic);
        const int mp = (ent.props_on && (kind == 2 || kind == 3) && (show->ann.has_mic || !show->ann.mic_own.empty())) ? ViewerPropPach(mic) : -1;
        if (mp >= 0) {
          jl::Value lc;
          lc.t = jl::Value::Obj;
          jl::Value k, iv;
          k.t = jl::Value::Str, k.s = "prop";
          iv.t = jl::Value::Num, iv.n = mp;
          lc.keys = {"kind", "id"}, lc.vals = {k, iv};
          const std::string lr = DoLoad(lc, true);
          if (lr.find("\"ok\":true") == std::string::npos) note += (note.empty() ? "" : "; ") + std::string("announcer mic: ") + lr;
          // the RIGHT hand, also for 2038 (a "2" = left id): the game's shot 6824_sw_s1_ring shows the mic in his right hand [C: the
          // picture; U the mechanism - sub_8233C758 special-cases prop 2038 (+1468 = 13 under conditions, not decoded)]
          else ent.ann_mic_model = int(r.ModelCount()) - 1, ent.ann_mic_right = true;
        }
      }
    }
    if (auto* l = r.MutableLights(a->model)) {
      ent.base_actor = *l;
      const assets::LightSet* s10 = nullptr;  // Session 98 (skin fix): set 10's lobe weights (colour w) are the entrant's c0..c3.w (run 6816)
      for (const assets::LightSet& s : arena_orig) if (s.id == 10) s10 = &s;
      for (assets::LightSet& s : ent.base_actor) {
        s.id = 10;  // the entrant is lit by light set 10 overridden by the rig's slot 0 (run 6816); a waiting wrestler keeps his own set
        if (s10) std::memcpy(s.wt, s10->wt, sizeof s.wt);
        // Session 98 (match oracle review): no cube falloff in entrances [C: every entrance prelight draw of runs 6816 / 6824 / 071], also
        // with the rig off / no rig key / lights off (those use base_actor as it is; a match-lit actor's set 10 holds 0.5)
        std::memset(s.fall, 0, sizeof s.fall);
      }
    }
    for (const assets::entrance::Cue& c : show->cues) {  // the root height of the first shown cue = the -Y-up reference of raw / scale
      double p[3];
      if (c.sub != 0 && assets::entrance::Root(c, 0, p)) { ent.y_ref = p[1]; break; }
    }
    ent.t = start_t;
    ent.playing = BoolArg(cmd, "play", false);
    if (ent.playing && ent.t >= ent.tl.total - 1e-6) ent.t = 0;
    ent.active = true;
    ApplyEntrance();
    std::string info = EntranceInfo(false);
    if (cid >= 0 && a->model < model_bones.size()) {  // Session 98: how well the borrowed entrance motions fit the custom skeleton (tracks are matched by bone-name hash)
      const std::vector<assets::Bone>& bn = model_bones[a->model];
      const assets::motion::Motion* ref = nullptr;
      for (const assets::entrance::Cue& c : show->cues) if (c.sub != 0 && !c.body.tracks.empty()) { ref = &c.body; break; }
      if (ref) {
        int hit = 0, tr_hit = 0;
        std::string miss;
        for (const assets::Bone& b : bn) {
          if (ref->Find(assets::motion::NodeHash(b.name))) ++hit;
          else if (miss.size() < 600) miss += (miss.empty() ? "" : ", ") + b.name;
        }
        for (const assets::motion::Track& t : ref->tracks)
          for (const assets::Bone& b : bn) if (assets::motion::NodeHash(b.name) == t.hash) { ++tr_hit; break; }
        info.insert(info.size() - 1, ",\"bone_match\":{\"bones\":" + std::to_string(bn.size()) + ",\"with_track\":" + std::to_string(hit) + ",\"tracks\":" + std::to_string(ref->tracks.size()) +
                                         ",\"tracks_used\":" + std::to_string(tr_hit) + ",\"bones_without_track\":\"" + jl::Esc(miss) + "\"}");
      }
    }
    if (!note.empty()) info.insert(info.size() - 1, ",\"note\":\"" + jl::Esc(note) + "\"");
    return info;
  }
  // {"cmd":"entrance_time","t":seconds | "cue":N ["f":local frames] | "frame":scene frames,"play":bool,"pause":true,"speed":x,"camera":..,"take":N,
  //  "root_y":..,"light_dirs":..,"intro_hold":s,"lights":bool}: moves / starts / stops the entrance timeline and returns the info.
  std::string DoEntranceTime(const jl::Value& cmd) {
    if (!ent.show || !ent.active) return Err("no entrance playing (send {\"cmd\":\"entrance\",\"wrestler\":161} first)");
    using namespace assets::entrance;
    if (cmd.has("intro_hold")) {
      Locate(*ent.show, ent.tl, ent.t, ent.cue, ent.local);
      const size_t cue = ent.cue;
      const double local = ent.local;
      ent.intro_hold = std::max(0.0, cmd.num("intro_hold", 0));
      ent.tl = Build(*ent.show, ent.intro_hold);
      ent.t = ent.tl.start[cue] + local;
    }
    if (cmd.has("cue")) {
      const int want = IntArg(cmd.num("cue", 0));
      bool hit = false;
      for (size_t i = 0; i < ent.show->cues.size(); ++i)
        if (ent.show->cues[i].sub == want) ent.t = ent.tl.start[i] + std::max(0.0, cmd.num("f", 0)), hit = true;
      if (!hit) return Err("this entrance has no cue " + std::to_string(want));
    } else if (cmd.has("t")) ent.t = std::max(0.0, cmd.num("t", 0)) * 60.0;
    else if (cmd.has("frame")) ent.t = std::max(0.0, cmd.num("frame", 0));
    if (cmd.has("speed")) ent.speed = std::max(0.0, cmd.num("speed", 1.0));
    if (cmd.has("play")) ent.playing = BoolArg(cmd, "play", true);
    if (BoolArg(cmd, "pause", false)) ent.playing = false;
    if (ent.playing && ent.t >= ent.tl.total - 1e-6) ent.t = 0;  // PLAY at the end starts over
    if (cmd.has("camera")) {
      const std::string cm = cmd.str("camera");
      if (cm != "free" && cm != "entrance") return Err("camera must be free or entrance");
      ent.cam_entrance = cm == "entrance";
    }
    if (cmd.has("take")) ent.take = IntArg(cmd.num("take", 1));
    if (cmd.has("root_y")) {
      const std::string ry = cmd.str("root_y");
      if (ry != "ground" && ry != "raw" && ry != "scale" && ry != "canvas") return Err("root_y must be ground, raw, scale or canvas");
      ent.root_y = ry;
    }
    if (cmd.has("light_dirs")) {
      const std::string ld = cmd.str("light_dirs");
      if (ld != "keyed" && ld != "arena") return Err("light_dirs must be keyed or arena");
      ent.light_keyed = ld == "keyed";
    }
    if (cmd.has("light_variant")) ent.light_variant = IntArg(cmd.num("light_variant", 0)) & 7;
    if (cmd.has("lights")) {
      const bool on = BoolArg(cmd, "lights", true);
      if (!on && ent.lights_on) RestoreEntranceLights(true);  // the lights as loaded (the entrance page stays: no pools)
      ent.lights_on = on;
    }
    if (cmd.has("cue0")) ent.show_cue0 = cmd.str("cue0") == "show";
    if (cmd.has("cam_dy")) ent.cam_dy = cmd.num("cam_dy", 120);
    if (cmd.has("l4")) ent.l4 = BoolArg(cmd, "l4", true);
    if (cmd.has("rig")) ent.rig_on = BoolArg(cmd, "rig", true);  // Session 98 (integration): the rig's colours on / off (A/B)
    if (cmd.has("rig_weights")) ent.rig_weights = BoolArg(cmd, "rig_weights", true);  // Session 98 (sweep look): the lobe weight rule (A/B)
    if (cmd.has("prop_light")) ent.prop_light = BoolArg(cmd, "prop_light", true);  // Session 98 (sweep look): props lit by the entrant (A/B)
    if (cmd.has("rig_loop")) ent.rig_loop = BoolArg(cmd, "rig_loop", true);  // Session 98 (generalisation): rig loop rule on / off (A/B)
    if (cmd.has("ground")) ent.stage_ground = BoolArg(cmd, "ground", true);  // Session 98 (generalisation): the raised-stage lift on / off (A/B)
    ApplyEntrance();
    return EntranceInfo(false);
  }
  // The panel's data: scalars every frame, the cue names / lengths only when the entrance (or its cue-0 hold) changed.
  ui::EntranceView ent_view;
  const void* ent_view_show = nullptr;
  double ent_view_total = -1;
  // Session 98 (timeline bar): what each cue does, named from the root path and this arena's stage / ramp rectangles (entrance.h Segments).
  std::vector<assets::entrance::Segment> ent_segs;
  const void* ent_segs_show = nullptr;
  bool ent_segs_rects = false;
  const std::vector<assets::entrance::Segment>& EntSegments() {
    if (!ent.show) { ent_segs.clear(); ent_segs_show = nullptr; return ent_segs; }
    std::string e;
    EnsureSpots(e);
    if (ent_segs_show != ent.show.get() || ent_segs_rects != sp.have_rects) {
      std::vector<assets::entrance::ZoneRect> zr;
      if (sp.have_rects) {
        const assets::spots::Rect* st = sp.StageRect();
        for (const assets::spots::Rect& r : sp.rects) {
          assets::entrance::ZoneRect z;
          z.x0 = r.x0, z.z0 = r.z0, z.x1 = r.x1, z.z1 = r.z1;
          z.kind = (r.light == 200 || &r == st) ? assets::entrance::kZoneStage : assets::entrance::kZoneRamp;
          zr.push_back(z);
        }
      }
      ent_segs = assets::entrance::Segments(*ent.show, ent.off, zr);
      ent_segs_show = ent.show.get(), ent_segs_rects = sp.have_rects;
      ent_view_total = -1;  // (the panel's labels follow)
    }
    return ent_segs;
  }
  static std::string SegZones(const assets::entrance::Segment& g) {
    std::string z;
    for (int k : g.zones) z += (z.empty() ? "" : " / ") + std::string(assets::entrance::ZoneName(k));
    return z;
  }
  const ui::EntranceView* EntranceViewPtr() {
    ui::EntranceView& v = ent_view;
    if (!ent.active || !ent.show) { v.active = false; ent_view_show = nullptr; return &v; }
    const std::vector<assets::entrance::Segment>& segs = EntSegments();
    if (ent_view_show != ent.show.get() || ent_view_total != ent.tl.total) {
      v.names.clear(), v.starts.clear(), v.lens.clear(), v.labels.clear(), v.tips.clear(), v.cue_ids.clear();
      for (size_t i = 0; i < ent.show->cues.size(); ++i) {
        v.names.push_back("CUE " + std::to_string(ent.show->cues[i].sub)), v.starts.push_back(ent.tl.start[i]), v.lens.push_back(ent.tl.len[i]);
        v.cue_ids.push_back(ent.show->cues[i].sub);
        v.labels.push_back(i < segs.size() ? segs[i].label : v.names.back());
        char tip[256] = "";
        if (i < segs.size()) {
          const assets::entrance::Segment& g = segs[i];
          std::snprintf(tip, sizeof tip, "cue %d, %.2f s - %.2f s\nroot (%.0f, %.0f) to (%.0f, %.0f)\nzones: %s", ent.show->cues[i].sub, ent.tl.start[i] / 60.0,
                        (ent.tl.start[i] + ent.tl.len[i]) / 60.0, g.a[0], g.a[2], g.b[0], g.b[2], SegZones(g).c_str());
        }
        v.tips.push_back(tip);
      }
      ent_view_show = ent.show.get(), ent_view_total = ent.tl.total;
    }
    v.active = true, v.playing = ent.playing, v.t = ent.t, v.total = ent.tl.total, v.speed = ent.speed, v.cue = (int)ent.cue, v.cam_entrance = ent.cam_entrance;
    v.intro_hold = ent.intro_hold, v.lights = ent.lights_on, v.show_cue0 = ent.show_cue0;
    v.title = ent.title, v.belt = ent.belt, v.bank = ent.bank;
    return &v;
  }

  std::string Execute(const std::string& line) {
    jl::Value cmd;
    std::string perr;
    if (!jl::Parse(line, cmd, perr)) return Err("bad JSON: " + perr);
    if (cmd.t != jl::Value::Obj) return Err("command must be a JSON object");
    std::string c = cmd.str("cmd");
    if (job.Active()) {  // a running render owns the scene and the clock: only reads, the render controls and path edits get through
      static const char* const ok_cmds[] = {"list", "stats", "cam_get", "motions", "render", "render_status", "render_cancel", "quit",
                                            "path_list", "path_add", "path_set_key", "path_delete", "path_save", "path_load", "path_clear"};
      bool allowed = false;
      for (const char* a : ok_cmds) allowed |= c == a;
      if (!allowed) return Err("a render is running: '" + c + "' is refused until it ends (render_status / render_cancel)");
    }
    if (c == "dlc") return "{\"ok\":true,\"packs\":" + ViewerDlcJson() + "}";  // Session 98: the DLC packs the viewer reads (dlc_files.h)
    if (c == "list") {
      std::string kind = cmd.str("kind");
      auto ids = ViewerList(kind);
      std::string s = "{\"ok\":true,\"kind\":\"" + jl::Esc(kind) + "\",\"ids\":[";
      for (size_t i = 0; i < ids.size(); i++) { if (i) s += ","; s += std::to_string(ids[i]); }
      return s + "]}";
    }
    if (c.rfind("light_", 0) == 0) return lt.Handle(c, cmd, r, last_arena, stage_wrestler);  // Session 98: lighting editor
    if (c == "load") return DoLoad(cmd, false);
    if (c == "add") return DoLoad(cmd, true);
    if (c == "pick") return DoPick(cmd);  // Session 98 (step 16): {"cmd":"pick","x":..,"y":..[,"w":..,"h":..][,"select":true]} -> the part under the pixel
    if (c == "select") return DoSelect(cmd);  // {"cmd":"select","model":0,"mesh":N,"instance":I} | {"cmd":"select","clear":true}
    if (c == "inspect") return PartInfoJson(sel_info);
    if (c == "hover") {  // windowed only (a test hook for the hover path): {"cmd":"hover","x":..,"y":..} asks for the id under the pixel, draws a few frames, shows the faint glow
      if (!plat || !r.HasModel()) return Err("hover needs the windowed viewer with something loaded");
      r.RequestHover(IntArg(cmd.num("x", -1)), IntArg(cmd.num("y", -1)));
      for (int i = 0; i < 8; i++) {
        plat->Pump(*this);
        r.DrawFrame(cam);
      }
      r.SetHighlight(r.HoverHit(), sel);
      return PartInfoJson(MakeInfo(r.HoverHit()));
    }
    if (c == "mod") return DoModStub(cmd);  // stubs: SWAP TEXTURE / SHOW IN MODS print what they would do
    if (c == "clear") {
      DropSelection();
      r.Clear(); model_bones.clear(); actors.clear(); ent.active = false; meshes = vertices = triangles = textures = 0; warnings.clear(); label.clear();
      ent.belt_model = -1, ent.props.clear(), ent.ann_mic_model = -1, ent.ann_ptr = nullptr, prop_lights_orig.clear(), ent.comps.clear(), ent.comp = false;  // (as DoLoad)
      last_arena = -1;
      return "{\"ok\":true}";
    }
    if (c == "camera") {
      // validate everything first: a bad command changes nothing
      float tgt[3] = {0, 0, 0};
      const jl::Value* t = cmd.get("target");
      if (t) {
        if (t->t != jl::Value::Arr || t->a.size() != 3) return Err("'target' must be [x,y,z]");
        for (int i = 0; i < 3; i++) {
          if (t->a[i].t != jl::Value::Num) return Err("'target' must be three numbers");
          tgt[i] = (float)t->a[i].n;
        }
      }
      if (cmd.has("preset")) {
        bool ok = false;
        Preset(cmd.str("preset"), &ok);  // (ApplyPreset changes nothing for an unknown name)
        if (!ok) return Err("unknown preset '" + cmd.str("preset") + "' (front side three_quarter top close ring ring_side ring_back)");
      }
      cam.yaw = (float)cmd.num("yaw", cam.yaw);
      cam.pitch = std::max(-89.5f, std::min(89.5f, (float)cmd.num("pitch", cam.pitch)));
      cam.dist = std::max(1.0f, (float)cmd.num("dist", cam.dist));
      cam.fov = std::max(5.0f, std::min(120.0f, (float)cmd.num("fov", cam.fov)));
      if (t) for (int i = 0; i < 3; i++) cam.target[i] = tgt[i];
      // Session 97: {"cmd":"camera","viewproj":[16 floats = the game's c56..c59 rows, file space],"eye":[x,y,z] (game c72)}. The game's
      // world is the files' own space (-Y up); the viewer negates Y, so the matrix gets its Y column negated and is transposed
      // (clip.i = dot(row_i, (world,1)) becomes the row-vector form clip = (pos,1) * M). "viewproj":null returns to the orbit camera.
      if (const jl::Value* vpv = cmd.get("viewproj")) {
        if (vpv->t == jl::Value::Null) cam.use_vp = false;
        else {
          if (vpv->t != jl::Value::Arr || vpv->a.size() != 16) return Err("'viewproj' must be 16 numbers (c56..c59, rows)");
          float g[16];
          for (int i = 0; i < 16; i++) {
            if (vpv->a[i].t != jl::Value::Num) return Err("'viewproj' must be 16 numbers");
            g[i] = (float)vpv->a[i].n;
          }
          for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++) cam.vp[i * 4 + j] = g[j * 4 + i] * (i == 1 ? -1.f : 1.f);
          cam.use_vp = true;
          const jl::Value* ev = cmd.get("eye");
          if (ev && ev->t == jl::Value::Arr && ev->a.size() >= 3) {
            cam.eye[0] = (float)ev->a[0].n; cam.eye[1] = -(float)ev->a[1].n; cam.eye[2] = (float)ev->a[2].n;
          }
        }
      } else if (cmd.has("preset") || cmd.has("yaw") || cmd.has("pitch") || cmd.has("dist") || cmd.has("fov") || t) cam.use_vp = false;
      if (cmd.has("preset") || cmd.has("yaw") || cmd.has("pitch") || cmd.has("dist") || t) cam.free = false;  // Session 98: the orbit fields mean the orbit camera
      EntranceLightsFollowCamera();
      MatchLightsFollowCamera();
      return "{\"ok\":true," + CamJson() + "}";
    }
    if (c == "cam_get") return "{\"ok\":true," + PoseJson() + "}";  // Session 98: the full pose (both modes)
    if (c == "cam_set") {  // Session 98: {"cmd":"cam_set","pos":[x,y,z],"yaw":..,"pitch":..,"roll":..,"fov":..,"mode":"free"|"orbit"}: a free pose; missing fields keep the current pose
      float np[3];
      const jl::Value* pv = cmd.get("pos");
      auto sane = [](double d) { return std::isfinite(d) && std::isfinite((float)d) && std::fabs(d) <= 1e7; };
      if (pv) {
        if (pv->t != jl::Value::Arr || pv->a.size() != 3) return Err("'pos' must be [x,y,z]");
        for (int i = 0; i < 3; i++) {
          if (pv->a[i].t != jl::Value::Num || !sane(pv->a[i].n)) return Err("'pos' must be three finite numbers (|x| <= 1e7)");
          np[i] = (float)pv->a[i].n;
        }
      }
      float ang[4];  // yaw, pitch, roll, fov: NaN = keep the current value
      const char* const names[4] = {"yaw", "pitch", "roll", "fov"};
      for (int i = 0; i < 4; i++) {
        ang[i] = NAN;
        if (const jl::Value* av = cmd.get(names[i])) {
          if (av->t != jl::Value::Num || !sane(av->n)) return Err(std::string("'") + names[i] + "' must be a finite number (|x| <= 1e7)");
          ang[i] = (float)av->n;
        }
      }
      const std::string mode = cmd.str("mode");
      if (!mode.empty() && mode != "free" && mode != "orbit") return Err("'mode' must be 'free' or 'orbit'");
      EnterFree(cam, fc);  // (from orbit or a game view-projection: the current view; then the fields below)
      cam.use_vp = false;
      if (pv) std::memcpy(cam.fpos, np, sizeof np);
      if (!std::isnan(ang[0])) cam.fyaw = WrapDeg(ang[0]);
      if (!std::isnan(ang[1])) cam.fpitch = std::max(-89.5f, std::min(89.5f, ang[1]));
      if (!std::isnan(ang[2])) cam.froll = WrapDeg(ang[2]);
      if (!std::isnan(ang[3])) cam.fov = std::max(kFreeFovMin, std::min(kFreeFovMax, ang[3]));
      fc.fov_target = fc.last_fov = cam.fov;
      std::memset(fc.vel, 0, sizeof fc.vel);
      fc.roll_rate = fc.look_yaw = fc.look_pitch = 0;
      const std::string pose = PoseJson();  // (the pose as set, before a switch back to orbit)
      if (mode == "orbit") LeaveFree(cam, fc);
      return "{\"ok\":true," + pose + "}";
    }
    if (c == "shot") {
      std::string path = cmd.str("path");
      if (path.empty()) return Err("missing path");
      int sw = IntArg(cmd.num("w", w)), sh = IntArg(cmd.num("h", h));
      Camera saved = cam;
      if (cmd.has("preset")) {
        bool ok = false;
        Preset(cmd.str("preset"), &ok);
        if (!ok) { cam = saved; return Err("unknown preset"); }
      }
      std::string err;
      TronNow(true);  // Session 98: the tron video at the current clock (exact picture)
      MatchLightsFollowCamera();  // (a preset shot's camera)
      double t0 = NowMs();
      bool ok = r.Shot(cam, sw, sh, path, err);
      last_shot_ms = NowMs() - t0;
      if (cmd.has("preset")) cam = saved;
      if (!ok) return Lost() ? Err("device lost") : Err(err);
      return "{\"ok\":true,\"path\":\"" + jl::Esc(path) + "\",\"w\":" + std::to_string(sw) + ",\"h\":" + std::to_string(sh) +
             ",\"ms\":" + Fix(last_shot_ms, 2) + "}";
    }
    if (c == "timing") {  // Session 98: {"cmd":"timing","w":1280,"h":720,"frames":12,"preset":"ring"} = GPU ms per pass, offscreen
      Camera saved = cam;
      if (cmd.has("preset")) {
        bool ok = false;
        Preset(cmd.str("preset"), &ok);
        if (!ok) { cam = saved; return Err("unknown preset"); }
      }
      std::string terr;
      const std::string res = r.Timing(cam, IntArg(cmd.num("w", w)), IntArg(cmd.num("h", h)), IntArg(cmd.num("frames", 12)), terr, cmd.has("pick") && (cmd.get("pick")->t == jl::Value::Bool ? cmd.get("pick")->b : cmd.get("pick")->n != 0));
      if (cmd.has("preset")) cam = saved;
      return res.empty() ? Err(terr) : res;
    }
    // Session 98 (replay S3): {"cmd":"replay","draws":[{"model":M,"mesh":K,"cb":{"ldir":[[x,y,z,w] x4],"lcol":[..x4],"sky":[4],...}}]}
    // overrides those meshes' draw constants (bank_dump field names; tools\assets\replay\replay.py maps the game's registers into them),
    // "clear":true drops every override. Nothing else changes (camera, pose, runtime maps stay the viewer's).
    if (c == "replay") {
      if (BoolArg(cmd, "clear", false)) r.ClearReplay();
      int slots = 0;
      if (const jl::Value* dv = cmd.get("draws"); dv && dv->t == jl::Value::Arr)
        for (const jl::Value& d : dv->a) {
          const int mo = IntArg(d.num("model", -1)), me = IntArg(d.num("mesh", -1));
          const jl::Value* cbv = d.get("cb");
          if (mo < 0 || me < 0 || !cbv || cbv->t != jl::Value::Obj) return Err("replay: each draw needs model, mesh and cb");
          for (size_t k = 0; k < cbv->keys.size(); k++) {
            const jl::Value& v = cbv->vals[k];
            auto vec4 = [&](const jl::Value& a, float out[4]) {
              if (a.t != jl::Value::Arr || a.a.size() != 4) return false;
              for (int i = 0; i < 4; i++) out[i] = a.a[i].t == jl::Value::Num ? (float)a.a[i].n : 0.f;
              return true;
            };
            float f4[4];
            if (cbv->keys[k] == "ldir" || cbv->keys[k] == "lcol" || cbv->keys[k] == "plcol") {
              if (v.t != jl::Value::Arr) return Err("replay: " + cbv->keys[k] + " must be 4 vectors");
              for (size_t i = 0; i < v.a.size() && i < 4; i++)
                if (vec4(v.a[i], f4)) r.SetReplaySlot(mo, me, r.CbSlot(cbv->keys[k], (int)i), f4), ++slots;
            } else {
              const int sl = r.CbSlot(cbv->keys[k], 0);
              if (sl < 0 || !vec4(v, f4)) return Err("replay: unknown field or not 4 numbers: " + cbv->keys[k]);
              r.SetReplaySlot(mo, me, sl, f4), ++slots;
            }
          }
        }
      return "{\"ok\":true,\"meshes\":" + std::to_string(r.ReplayCount()) + ",\"slots\":" + std::to_string(slots) + "}";
    }
    if (c == "bank_dump") {  // Session 98 (replay S0, read-only): {"cmd":"bank_dump","path":"x.json"[,"w":..,"h":..]}: every mesh's draw constants as the viewer feeds them (tools/assets/bank_diff.py)
      const std::string path = cmd.str("path");
      const std::string js = r.BankDump(cam, IntArg(cmd.num("w", w)), IntArg(cmd.num("h", h)));
      if (path.empty()) return js;
      std::ofstream bf(path, std::ios::binary);
      if (!bf) return Err("cannot write " + path);
      bf << js;
      return "{\"ok\":true,\"path\":\"" + jl::Esc(path) + "\",\"bytes\":" + std::to_string(js.size()) + "}";
    }
    if (c == "stats") {
      float mn[3] = {0, 0, 0}, mx[3] = {0, 0, 0};
      if (r.HasModel()) r.Bounds(mn, mx);
      return "{\"ok\":true,\"frame_ms\":" + Fix(frame_ms, 2) + ",\"last_shot_ms\":" + Fix(last_shot_ms, 2) + ",\"adapter\":\"" +
             jl::Esc(r.AdapterName()) + "\",\"hidden\":" + (hidden ? "true" : "false") + ",\"label\":\"" + jl::Esc(label) +
             "\",\"models\":" + std::to_string(r.ModelCount()) + ",\"meshes\":" + std::to_string(meshes) +
             ",\"vertices\":" + std::to_string(vertices) + ",\"triangles\":" + std::to_string(triangles) +
             ",\"textures\":" + std::to_string(textures) + ",\"warnings\":" + WarnJson(warnings) + ",\"bounds_min\":[" +
             Fix(mn[0], 1) + "," + Fix(mn[1], 1) + "," + Fix(mn[2], 1) + "],\"bounds_max\":[" + Fix(mx[0], 1) + "," +
             Fix(mx[1], 1) + "," + Fix(mx[2], 1) + "]," + CamJson() + "}";
    }
    if (c == "look") {  // Session 97: {"cmd":"look","mode":"lit"|"plain","exposure":1.2}
      bool lit = r.LookLit();
      float ex = r.Exposure();
      if (cmd.has("mode")) {
        std::string md = cmd.str("mode");
        if (md == "plain") lit = false;
        else if (md == "lit") lit = true;
        else return Err("mode must be 'lit' or 'plain'");
      }
      if (cmd.has("exposure")) {
        const double e = cmd.num("exposure", 1.0);
        if (!(e > 0.0 && e < 100.0)) return Err("exposure must be in (0, 100)");
        ex = (float)e;
      }
      r.SetLook(lit, ex);
      if (cmd.has("debug")) r.SetDebug(IntArg(cmd.num("debug", 0)));
      // Session 98 (skin fix): {"cmd":"look","skin":"game"|"old","rim_map":0..1}: the wrestler shading (the game's rule by default; "old" =
      // the viewer's earlier recipe, A/B) and the value standing in for the game's rim light map (0 = the edge highlight off).
      if (cmd.has("skin") || cmd.has("rim_map")) {
        bool sg = r.SkinGame();
        if (cmd.has("skin")) {
          const std::string sk = cmd.str("skin");
          if (sk != "game" && sk != "old") return Err("skin must be game or old");
          sg = sk == "game";
        }
        const double rm = cmd.num("rim_map", r.RimMap());
        if (!(rm >= 0.0 && rm <= 4.0)) return Err("rim_map must be in [0, 4]");
        r.SetSkinRule(sg, (float)rm);
      }
      // Session 97: {"cmd":"look","gamma":false} turns off the game's display-gamma curve (on by default)
      if (const jl::Value* gm = cmd.get("gamma")) r.SetGamma(gm->t == jl::Value::Bool ? gm->b : gm->n != 0);
      // Session 98: the frame recipe's switches {"cmd":"look","shadows":true,"dof":true,"fxaa":true,"bloom":true,"glare":true} (+ shadow_props /
      // shadow_wrestler / shadow_plane / shadow_blobs); "entrance":true|false|{"bloom":0.4,"glare":1.6,"threshold":0.7} = the entrance look
      // (missing strengths = the stage wrestler's, EntranceStrengths: glare 1.6 is Brock's only).
      // (flash fix: "flashes" = the crowd camera flashes of the entrance look, "ent_lm" = the entrance's LM shader variants on props / arena)
      for (const char* k : {"shadows", "shadow_props", "shadow_wrestler", "shadow_plane", "shadow_blobs", "dof", "dof_near", "fxaa", "bloom", "glare", "live_screen", "rim_light", "lid_layer", "flashes", "ent_lm", "ent_lm_bg", "ent_own_lm"})
        if (const jl::Value* fv = cmd.get(k)) r.SetEffect(k, fv->t == jl::Value::Bool ? fv->b : fv->n != 0);
      if (const jl::Value* cv = cmd.get("cull")) r.SetCullMode(cv->t == jl::Value::Bool ? (cv->b ? 1 : 0) : (int)cv->n);  // Session 98 (stage fix, A/B)
      // Session 98 (gen check 3): "crowd_cards": false = every stands member a body (A/B); true / 1 = the game's card rule (auto:
      // entrance factor while an entrance plays); 2 = entrance factor; 3 = match factor (renderer.cpp CrowdCardDistance)
      if (const jl::Value* cc = cmd.get("crowd_cards")) r.SetCrowdCards(cc->t == jl::Value::Bool ? (cc->b ? 1 : 0) : std::clamp((int)cc->n, 0, 3));
      if (const jl::Value* pv = cmd.get("pools")) {  // Session 98 (arena fix 2): the entrance light pools on the stage light map (A/B)
        pools_on = pv->t == jl::Value::Bool ? pv->b : pv->n != 0;
        if (!pools_on) ApplyStagePools(false, true);  // (match look: the entrance page without pools; the match page when none plays)
        else if (ent.active && ent.show && ent.lights_on) ApplyEntranceLights(ent.show->cues[std::min(ent.cue, ent.show->cues.size() - 1)]);
      }
      if (const jl::Value* mv = cmd.get("match_lightmap")) {  // Session 98 (match look): the match page = fade_lightmap01 (A/B: false = 00)
        match_lm_on = mv->t == jl::Value::Bool ? mv->b : mv->n != 0;
        if (!ent.active) ApplyStagePools(false);
      }
      if (const jl::Value* cf = cmd.get("cube_falloff"))  // Session 98 (match oracle): the ambient cube's per-light falloff (A/B: false = 0)
        r.SetCubeFall(cf->t == jl::Value::Bool ? cf->b : cf->n != 0);
      if (const jl::Value* fv = cmd.get("follow_spot")) {  // Session 98 (spot / beard pass): the follow spot under the entrant (A/B)
        follow_on = fv->t == jl::Value::Bool ? fv->b : fv->n != 0;
        if (ent.active && ent.show && ent.lights_on) ApplyEntranceLights(ent.show->cues[std::min(ent.cue, ent.show->cues.size() - 1)]);
      }
      if (const jl::Value* fv = cmd.get("film")) {  // Session 98 (integration): the entrance film filter on / off (A/B)
        film_on = fv->t == jl::Value::Bool ? fv->b : fv->n != 0;
        if (ent.active && ent.show && ent.lights_on) ApplyEntranceLights(ent.show->cues[std::min(ent.cue, ent.show->cues.size() - 1)]);
        else if (!film_on) SetFilmComposed(nullptr);
      }
      if (const jl::Value* ev = cmd.get("entrance")) {
        if (ev->t == jl::Value::Obj) {
          float eb = 0.f, eg = 0.f;
          EntranceStrengths(stage_wrestler, eb, eg);
          r.SetEntrance(true, (float)ev->num("bloom", eb), (float)ev->num("glare", eg)), r.SetBloomThreshold((float)ev->num("threshold", 0.45));
        } else {  // (Session 98 brightness fix: the strengths of the stage's wrestler, as the `stage` command sets them; was Brock's 1.6 glare for anyone)
          float eb = 0.f, eg = 0.f;
          EntranceStrengths(stage_wrestler, eb, eg);
          r.SetEntrance(ev->t == jl::Value::Bool ? ev->b : ev->n != 0, eb, eg);
        }
      }
      if (const jl::Value* tv = cmd.get("tron_video")) {  // Session 98: the stage's tron slots play video (default on); off = the stills
        const bool want = tv->t == jl::Value::Bool ? tv->b : tv->n != 0;
        const bool was_applied = tron.Applied();
        tron.on = want;
        if (!want && was_applied && last_arena >= 0) tron.Forget(), ReloadArena();
      }
      if (cmd.has("ssao")) return Err("ssao is not implemented (the game has none)");
      return std::string("{\"ok\":true,\"mode\":\"") + (lit ? "lit" : "plain") + "\",\"exposure\":" + Fix(ex, 3) + "}";
    }
    if (c == "lights") {  // Session 97: {"cmd":"lights","haze":true|false} and/or {"cmd":"lights","override":{"col":[[r,g,b]x4],"dir":[[x,y,z]x4],"sky":[r,g,b],"ground":[r,g,b]}} (A/B: one set for every arena mesh; "override":null clears)
      if (const jl::Value* hz = cmd.get("haze")) r.SetHaze(hz->t == jl::Value::Bool ? hz->b : hz->n != 0);
      if (cmd.has("time") || cmd.has("mode") || cmd.has("key")) {
        // Session 97 (stage key, out\runtime\s97\stage_key.md): {"cmd":"lights","time":T,"mode":"match"|"entrance","key":true|false}
        // sets the game frame T at which the arena's stage motion key (light sets, moving slots) is evaluated (mode picks the
        // default T: match 132, entrance 140); "key":false loads the arena with the plain 30002 sets and unmoved slots. The loaded
        // arena is re-loaded with the camera kept.
        double tm = 132;
        const std::string md = cmd.str("mode", "match");
        if (md == "entrance") tm = 140;
        else if (md != "match") return Err("mode must be 'match' or 'entrance'");
        tm = cmd.num("time", tm);
        bool on = true;
        if (const jl::Value* kv = cmd.get("key")) on = kv->t == jl::Value::Bool ? kv->b : kv->n != 0;
        std::string kerr;
        if (!ViewerStageKey(on, tm, kerr)) return Err(kerr);
        if (last_arena >= 0) {
          const std::string res = ReloadArena();
          if (res.find("\"ok\":true") == std::string::npos) return res;
        }
      }
      if (const jl::Value* ov = cmd.get("override")) {
        if (ov->t == jl::Value::Null) r.SetLightOverride(false, assets::LightSet());
        else {
          assets::LightSet ls;
          auto vec3 = [&](const jl::Value* v, float* o) {
            if (!v || v->t != jl::Value::Arr || v->a.size() < 3) return false;
            for (int k = 0; k < 3; k++) o[k] = (float)v->a[k].n;
            return true;
          };
          const jl::Value* col = ov->get("col");
          const jl::Value* dir = ov->get("dir");
          if (!col || !dir || col->t != jl::Value::Arr || dir->t != jl::Value::Arr || col->a.size() != 4 || dir->a.size() != 4) return Err("override needs col[4] and dir[4]");
          for (int k = 0; k < 4; k++)
            if (!vec3(&col->a[k], ls.col[k]) || !vec3(&dir->a[k], ls.dir[k])) return Err("override col / dir entries must be [x,y,z]");
          if (!vec3(ov->get("sky"), ls.sky) || !vec3(ov->get("ground"), ls.ground)) return Err("override needs sky and ground [r,g,b]");
          r.SetLightOverride(true, ls);
        }
      }
      return R"({"ok":true})";
    }
    if (c == "stage") {  // Session 97: {"cmd":"stage","media":"DIR","tron":N} / {"cmd":"stage","media":false}
      // Movie frames on the entrance stage / video wall (keys 800-809, srcssets\stage_media.h): DIR = the folder the game's
      // --wwe2k14_tron_export made (NNN.png + evtmov\GROUP_Mnnn.png); "" or missing = the launcher's tron cache; false = untextured.
      // `tron` = the titantron number for the big screen / banner ring (default 100). The loaded arena is re-loaded, camera kept.
      // Tron preview: "wrestler":N = the entrance look of wrestler N (his tron + MVSW / MVSB, else the arena's MOV / MVAB; -1 = the
      // match look); "step":+1 / -1 = the next / previous wrestler that has media (wraps); "list":true = only list them.
      const jl::Value* mv = cmd.get("media");
      const bool off = mv && mv->t == jl::Value::Bool && !mv->b;
      const std::string media_dir = off || !mv || mv->t != jl::Value::Str ? "" : mv->s;
      last_media = media_dir;
      std::vector<std::pair<std::string, std::string>> smap;
      if (const jl::Value* mp = cmd.get("map"); mp && mp->t == jl::Value::Obj)
        for (size_t i = 0; i < mp->keys.size(); i++)
          if (mp->vals[i].t == jl::Value::Str) smap.push_back({mp->keys[i], mp->vals[i].s});
      std::string err;
      std::vector<int> wl;
      std::string wdir;
      ViewerStageWrestlers(media_dir, wl, wdir);
      const std::string note = wl.empty() ? "no tron media found" + (wdir.empty() ? std::string() : " in " + wdir) +
                                                " (run the tron export: the launcher's Build previews, or --wwe2k14_tron_export=DIR)"
                                          : "";
      if (cmd.has("list") || cmd.has("step")) {
        if (cmd.has("list") && !cmd.has("step") && !cmd.has("wrestler")) {
          std::string a;
          for (size_t i = 0; i < wl.size(); i++) a += (i ? "," : "") + std::to_string(wl[i]);
          return "{\"ok\":true,\"media_dir\":\"" + jl::Esc(wdir) + "\",\"count\":" + std::to_string(wl.size()) + ",\"wrestlers\":[" + a +
                 "],\"note\":\"" + jl::Esc(note) + "\"}";
        }
      }
      int want = cmd.has("wrestler") ? IntArg(cmd.num("wrestler", -1)) : stage_wrestler;
      if (cmd.has("step")) {
        if (wl.empty()) return Err(note);
        const int dir = IntArg(cmd.num("step", 1)) < 0 ? -1 : 1;
        size_t at = 0;
        if (dir > 0) {  // the first id above `want` (wrap), or below for -1
          at = wl.size();
          for (size_t i = 0; i < wl.size(); i++) if (wl[i] > want) { at = i; break; }
          at %= wl.size();
        } else {
          at = wl.size() - 1;
          for (size_t i = wl.size(); i-- > 0;) if (wl[i] < want) { at = i; break; }
        }
        want = wl[at];
      }
      stage_wrestler = want;
      // Session 98: the entrance look turns the entrance combine on. Measured strengths (passes.md 2): Brock (343) bloom 0.4 / glare 1.6,
      // Bryan (324) 0 / 0; any other wrestler: bloom 0.4, glare 0 [unconfirmed].
      {
        float eb = 0.f, eg = 0.f;
        EntranceStrengths(want, eb, eg);
        r.SetEntrance(want >= 0, eb, eg);
        if (want < 0) SetFilmComposed(nullptr);
      }
      if (want >= 0) smap.push_back({"@wrestler", std::to_string(want)});
      // Entrance light (entrance_light.h): "cue":0/5/10/15/20 and "t":motion frames pick the key (default cue 0, t 68); "light":false keeps the arena lights.
      if (cmd.has("cue")) smap.push_back({"@cue", std::to_string(cmd.num("cue", 0))});
      if (cmd.has("t")) smap.push_back({"@t", std::to_string(cmd.num("t", 68))});
      if (const jl::Value* lv = cmd.get("light"); lv && lv->t == jl::Value::Bool && !lv->b) smap.push_back({"@nolight", "1"});
      if (!ViewerStageMedia(media_dir, IntArg(cmd.num("tron", 100)), off, smap, err)) return Err(err);
      if (last_arena < 0) return "{\"ok\":true,\"reloaded\":false,\"wrestler\":" + std::to_string(want) + "}";
      std::string res = ReloadArena();
      if (!res.empty() && res.back() == '}' && res.find("\"ok\":true") != std::string::npos)
        res.insert(res.size() - 1, ",\"wrestler\":" + std::to_string(want) + ",\"media_dir\":\"" + jl::Esc(wdir) + "\",\"media_wrestlers\":" +
                                       std::to_string(wl.size()) + (note.empty() ? "" : ",\"note\":\"" + jl::Esc(note) + "\""));
      return res;
    }
    if (c == "tron_time" || c == "tron_play" || c == "tron_offset") return DoTron(c, cmd);
    if (c == "flash_info") {  // Session 98 (flash fix): {"cmd":"flash_info","test":0|1} (test 1 = no centre depth test, A/B / debugging)
      if (cmd.has("test")) r.SetFlashTest(IntArg(cmd.num("test", 0)));
      return r.FlashInfo();
    }
    if (c == "crowd") {  // Session 97: {"cmd":"crowd","act":14005,"frame":N,"pose":"game"|"stored"}
      // Pose of the crowd people of arenas loaded from now on, from the player's own pac\m2.pac: `act` 14001-14011 (default
      // 14005), `frame` a game frame of the motion (default -1 = a different frame per person type), `pose` "stored" = the
      // bodies' stored T-pose. The loaded arena (if any) is re-loaded with the camera kept.
      const std::string pose = cmd.str("pose", "game");
      if (pose != "game" && pose != "stored") return Err("pose must be 'game' or 'stored'");
      const int act = IntArg(cmd.num("act", 14005)), fr = IntArg(cmd.num("frame", -1));
      std::string err;
      if (!ViewerCrowd(pose == "stored", act, fr, err)) return Err(err);
      if (last_arena < 0) return "{\"ok\":true,\"reloaded\":false}";
      return ReloadArena();
    }
    if (c == "motions") return DoMotions(cmd);
    if (c == "motion") return DoMotion(cmd);
    if (c == "pose") return DoPose(cmd);
    if (c == "bones") return DoBones();
    if (c == "tracks") return DoTracks(cmd);
    if (c == "frame") {
      std::vector<Actor*> tg;
      bool ua = false;
      std::string terr;
      if (!Targets(cmd, tg, ua, terr)) return Err(terr);
      const double f = cmd.num("f", 0);
      if (!(f >= 0 && f < 1e7)) return Err("f must be a frame number >= 0");
      std::string who;
      for (Actor* a : tg) {  // Session 98: per actor (each wraps at its own motion length); the feet are put back on the floor
        if (!a->have_motion) continue;
        a->frame = a->motion.length > 0 && f > a->motion.length ? std::fmod(f, (double)a->motion.length) : f;
        a->pose_motion = true;
        PoseActor(*a);
        PlaceXform(*a, true);
        who += (who.empty() ? "{" : ",{") + std::string("\"id\":\"") + a->id + "\",\"frame\":" + Fix(a->frame, 2) + ",\"frames\":" + std::to_string(a->motion.length) + "}";
      }
      if (cmd.has("actor") || (ua && !have_motion)) {
        if (who.empty()) return Err("no motion chosen (send 'motion' first)");
        return "{\"ok\":true,\"actors\":[" + who + "]}";
      }
      if (!have_motion) return Err("no motion chosen (send 'motion' first)");
      frame = cur_motion.length > 0 && f > cur_motion.length ? std::fmod(f, (double)cur_motion.length) : f;
      pose_motion = true;
      ApplyPose();
      return "{\"ok\":true,\"frame\":" + Fix(frame, 2) + ",\"frames\":" + std::to_string(cur_motion.length) + "}";
    }
    if (c == "place") return DoPlace(cmd);
    if (c == "remove") return DoRemove(cmd);
    if (c == "actors") return DoActors();
    if (c == "entrance") return DoEntrance(cmd);
    if (c == "entrance_stop") return DoEntranceStop();
    // Session 98 (gen check 5): {"cmd":"entrance_title","title":"none|hold|waist|auto","belt":N,"champ_bank":bool} re-runs the current
    // entrance at its current time with new champion options (the ENTRANCE tab). champ_bank:false = the wrestler's own bank.
    if (c == "entrance_title") {
      if (!ent.show || last_ent_cmd.t != jl::Value::Obj) return Err("no entrance (send {\"cmd\":\"entrance\",...} first)");
      jl::Value nc = last_ent_cmd;
      auto set = [&](const char* k, const jl::Value& v) {
        for (size_t i = 0; i < nc.keys.size(); ++i) if (nc.keys[i] == k) { nc.vals[i] = v; return; }
        nc.keys.push_back(k), nc.vals.push_back(v);
      };
      auto drop = [&](const char* k) {
        for (size_t i = 0; i < nc.keys.size(); ++i) if (nc.keys[i] == k) { nc.keys.erase(nc.keys.begin() + (std::ptrdiff_t)i); nc.vals.erase(nc.vals.begin() + (std::ptrdiff_t)i); return; }
      };
      jl::Value num, str;
      num.t = jl::Value::Num, str.t = jl::Value::Str;
      if (const jl::Value* tv = cmd.get("title")) set("title", *tv);
      if (const jl::Value* bv = cmd.get("belt")) { if (bv->t == jl::Value::Num && bv->n > 0) set("belt", *bv); else drop("belt"); }
      if (const jl::Value* cb = cmd.get("champ_bank")) {
        const bool on = cb->t == jl::Value::Bool ? cb->b : cb->n != 0;
        const int b = on ? ViewerChampBank(ent.wrestler) : -1;
        if (on && b < 0) return Err("this wrestler has no champion bank named after him in Entrance06");
        if (on) num.n = b, set("bank", num); else drop("bank");
      }
      for (const char* k : {"cue", "f"}) drop(k);
      num.n = ent.t / 60.0, set("t", num);
      jl::Value fl;
      fl.t = jl::Value::Bool, fl.b = false;
      set("stage", fl);  // (same wrestler: the stage media stay)
      return DoEntrance(nc);
    }
    if (c == "entrance_time") return DoEntranceTime(cmd);
    if (c == "particles_dump") {  // Session 99 (smoke density): the last frame's sprites as JSON (viewer space, Y up; colour 0-1)
      const std::string path = cmd.str("path");
      if (path.empty()) return Err("missing path");
#if defined(_WIN32)
      FILE* f = _wfopen(std::filesystem::u8path(path).c_str(), L"wb");  // (review) UTF-8 paths, like the other writers
#else
      FILE* f = std::fopen(path.c_str(), "wb");  // (Android / POSIX: paths are UTF-8 already)
#endif
      if (!f) return Err("cannot write " + path);
      std::fprintf(f, "{\"t\":%.3f,\"effects\":[", ent.t);
      for (size_t i = 0; i < last_pfx.size(); ++i)
        std::fprintf(f, "%s{\"effect\":%d,\"frame\":%.3f,\"first\":%zu,\"end\":%zu}", i ? "," : "", last_pfx[i].effect, last_pfx[i].frame,
                     last_pfx[i].first, last_pfx[i].end);
      std::fprintf(f, "],\"sprites\":[");
      for (size_t i = 0; i < last_psprites.size(); ++i) {
        const auto& s = last_psprites[i];
        std::fprintf(f, "%s\n{\"c\":[%.3f,%.3f,%.3f],\"ax\":[%.3f,%.3f,%.3f],\"ay\":[%.3f,%.3f,%.3f],\"col\":[%.4f,%.4f,%.4f,%.4f],"
                     "\"bb\":%d,\"em\":%d,\"age\":%d,\"life\":%d}", i ? "," : "", s.c[0], s.c[1], s.c[2], s.ax[0], s.ax[1], s.ax[2], s.ay[0],
                     s.ay[1], s.ay[2], s.col[0], s.col[1], s.col[2], s.col[3], s.billboard ? 1 : 0, s.emitter, s.age, s.life);
      }
      std::fprintf(f, "]}\n");
      std::fclose(f);
      return "{\"ok\":true,\"sprites\":" + std::to_string(last_psprites.size()) + "}";
    }
    if (c == "entrance_info") return EntranceInfo(BoolArg(cmd, "light_probe", false));
    if (c == "spot_cam") return DoSpotCam(cmd);
    if (c == "play") {
      const jl::Value* v = cmd.get("on");
      playing = !v || (v->t == jl::Value::Bool ? v->b : v->n != 0);
      if (playing) last_tick_ms = 0;
      return std::string("{\"ok\":true,\"playing\":") + (playing ? "true" : "false") +
             (hidden ? ",\"note\":\"hidden viewer: frames advance only with the frame command\"" : "") + "}";
    }
    if (c == "ui_shot") {  // Session 98: {"cmd":"ui_shot","path":P,"tab":"runtime"|"assets","category":0-2,"select":N,"quit":true}: the back buffer WITH the UI
      if (!plat || !ui.Ready()) return Err("ui_shot needs the windowed viewer (not --hidden)");
      if (plat->Minimized()) return Err("ui_shot: the window is minimised");
      const std::string path = cmd.str("path");
      if (path.empty()) return Err("missing path");
      ui.QueueShot(path, cmd.str("tab"), IntArg(cmd.num("category", -1)), IntArg(cmd.num("select", -1)));
      for (int i = 0; i < 40 && !ui.ShotDone(); i++) {
        plat->Pump(*this);
        ui.Pump();
        ImporterFrame();  // (Session 98: the importer's framing follows the UI's preview rectangle)
        MatchLightsFollowCamera();
        r.DrawFrame(cam);
      }
      if (!ui.ShotDone()) return Err("ui_shot: no frame was drawn");
      if (!ui.ShotError().empty()) return Err(ui.ShotError());
      if (const jl::Value* qv = cmd.get("quit"); qv && qv->t == jl::Value::Bool && qv->b) quit = true;
      return "{\"ok\":true,\"path\":\"" + jl::Esc(path) + "\"}";
    }
    // Session 98 (phone fixes, test hooks): drive the UI the way a finger does, through its own widgets.
    //   {"cmd":"ui_touch","x":X,"y":Y,"down":true|false,"frames":N}  the test finger at (X, Y) (UI pixels), pressed or lifted, then N
    //        frames run as the main loop runs them (scene clock, queued UI actions, draw). "release":true gives the pointer back.
    //   {"cmd":"ui_state"}  what the UI shows (Ui::TestState)
    if (c == "ui_touch") {
      if (!plat || !ui.Ready()) return Err("ui_touch needs the windowed viewer (not --hidden)");
      if (BoolArg(cmd, "release", false)) ui.TestPointer(false, 0, 0, false);
      else ui.TestPointer(true, (float)cmd.num("x", 0), (float)cmd.num("y", 0), BoolArg(cmd, "down", false));
      const int n = std::max(1, std::min(600, IntArg(cmd.num("frames", 3))));
      for (int i = 0; i < n && !quit; i++) {
        plat->Pump(*this);
        if (!job.Active()) Tick();
        ui.Pump();
        r.DrawFrame(cam);
        SleepMs(16);
      }
      return "{\"ok\":true,\"ui\":" + ui.TestState() + "}";
    }
    if (c == "ui_state") return "{\"ok\":true,\"ui\":" + (ui.Ready() ? ui.TestState() : std::string("null")) + "}";
    // Session 98 (Superstar Importer): {"cmd":"importer_preview","path":P,"attire":"ring"|"entrance"} / {"clear":true}; {"cmd":"importer",...}
    // = the window's test hooks (ui_importer.cpp Command); {"cmd":"importer_portrait","kind":0-2,"path":"x.png"} = one render as a PNG.
    if (c == "importer_preview") return DoImporterPreview(cmd);
    if (c == "importer") return ui.Ready() ? ui.ImporterCommand(line) : Err("the importer needs the windowed viewer");
    if (c == "importer_portrait") {
      std::vector<uint8_t> px;
      int pw = 0, ph = 0;
      std::string perr2;
      if (!ImporterPortrait(IntArg(cmd.num("kind", 0)), px, pw, ph, perr2)) return Err(perr2);
      for (size_t i = 0; i < px.size(); i += 4) std::swap(px[i], px[i + 2]);
      if (!WritePng(cmd.str("path"), px.data(), pw, ph, perr2)) return Err(perr2);
      return "{\"ok\":true,\"w\":" + std::to_string(pw) + ",\"h\":" + std::to_string(ph) + "}";
    }
    { std::string po; if (DoPathCmd(c, cmd, po)) return po; }  // Session 98: path_* / render
    if (c.compare(0, 4, "mods") == 0 && (c == "mods" || c == "mods_list" || c == "mods_select" || c == "mods_add" || c == "mods_remove" || c == "mods_sheet" || c == "mods_refresh")) return DoMods(c, cmd);
    if (c == "char_import" || c == "char_list" || c == "char_remove") return DoChars(c, cmd);
    if (c == "quit") { quit = true; return "{\"ok\":true}"; }
    return Err("unknown cmd '" + c + "'");
  }
  // ---- InputSink (platform.h): the window's events. Session 98 phone port: the former WndProc's cases, unchanged in what they do. ----
  bool UiFirst(const void* native, intptr_t& out) override {
    if (!ui.Ready()) return false;  // Session 98: the UI sees every message first; it swallows mouse / keys it wants
    if (plat && std::string(plat->Name()) == "win32") {
      const NativeMsg& m = *static_cast<const NativeMsg*>(native);
      return ui.Message(m.hwnd, m.msg, m.wp, m.lp, dragging || fc.looking, out);
    }
    return ui.Message(nullptr, ui::kSdlEventMsg, (uintptr_t)native, 0, dragging || fc.looking, out);
  }
  void OnClose() override { quit = true; }
  void OnResize(int ww, int hh) override { r.Resize(ww, hh); }
  void OnMouseDown(int button, int x, int y) override {
    look_move_x = look_move_y = 0;
    if (button == 0) {
      click_cand = true, down_x = x, down_y = y;  // Session 98 (step 16): a click selects a part
      if (cam.free) return;  // (Session 98: the free camera looks with the right button)
      dragging = true; last_x = x; last_y = y;
      plat->SetCapture(true);
      return;
    }
    if (importer) {  // Session 98 (importer): the left drag turns the model, the right drag moves the picture
      if (imp_have) { imp_panning = true, imp_px = x, imp_py = y; plat->SetCapture(true); }
      return;
    }
    // Session 98 (Jarrod: freecam while his entrance plays): the entrance camera hands over to the free camera at its
    // current shot when the user takes control.
    // Jarrod: "right clicking or f should also initiate Freecam based on the current shot" - from orbit too.
    if (ent.cam_entrance) ent.cam_entrance = false;
    if (!cam.free) EnterFree(cam, fc);  // (clicks the UI wants never reach here: the UI swallows them)
    BeginLook();
  }
  void OnMouseUp(int button, int x, int y) override {
    if (button == 0) {
      if (dragging) { dragging = false; plat->SetCapture(false); }
      if (click_cand) { click_cand = false; ClickSelect(x, y); }
      return;
    }
    if (imp_panning) { imp_panning = false; plat->SetCapture(false); return; }
    EndLook();
  }
  void OnMouseMove(int x, int y) override {
    if (click_cand && (std::abs(x - down_x) >= 4 || std::abs(y - down_y) >= 4)) click_cand = false;
    if (dragging) {
      cam.yaw -= (x - last_x) * 0.4f;
      cam.pitch = std::max(-89.5f, std::min(89.5f, cam.pitch + (y - last_y) * 0.4f));
      if (importer) cam.pitch = std::clamp(cam.pitch, -25.f, 45.f);  // Session 98 (importer): around him, not under his feet
      last_x = x; last_y = y;
    }
    if (imp_panning) { ImporterPan(float(x - imp_px), float(y - imp_py)); imp_px = x, imp_py = y; }
  }
  void OnLookDelta(float dx, float dy) override {
    fc.mouse_dx += dx;
    fc.mouse_dy += dy;
    // (the platform swallows the plain mouse moves while it looks: the 4-px click cancel of OnMouseMove happens here; review of 8dcd6d7b)
    look_move_x += dx, look_move_y += dy;
    if (click_cand && (std::fabs(look_move_x) >= 4.f || std::fabs(look_move_y) >= 4.f)) click_cand = false;
  }
  float look_move_x = 0, look_move_y = 0;  // the look's moves since the last button press
  void OnWheel(int ticks) override {
    if (importer) { imp_zoom = std::clamp(imp_zoom * std::pow(0.9f, (float)ticks), 0.25f, 3.f); return; }  // Session 98 (importer)
    if (cam.free) { fc.wheel += ticks; return; }  // Session 98: the free camera's wheel is the FOV
    cam.dist = std::max(1.0f, cam.dist * std::pow(0.9f, (float)ticks));
  }
  void OnKey(unsigned wp, bool repeat) override {
    if (importer) return;  // Session 98 (importer): no viewer keys (Esc would close the window and lose an unsaved entry)
    const bool shift = plat->KeyHeld(key::kShift), ctrl = plat->KeyHeld(key::kControl);
    if (wp == key::kTab && !actors.empty()) { CycleSpot(shift ? -1 : 1); return; }  // Session 98: with a scene, Tab visits the spots
    // Session 98 (timeline bar): while an entrance is active (and no text field has the keyboard) Space, Left / Right, , . and - = drive it
    if (ent.active && ui.Ready() && !ui.WantTextInput() && !ctrl) {
      if (repeat && wp == key::kSpace) return;  // (no auto-repeat on play / pause)
      if (ui.TimelineKey(wp, shift)) return;
    }
    BrowseKey(wp);
    if (wp >= '1' && wp <= '8') Preset(kPresetNames[wp - '1']);  // (Session 96: 7, 8 = ring_side, ring_back)
    else if (wp == 'F' && !repeat && !ctrl) {
      if (ent.cam_entrance) ent.cam_entrance = false;  // entrance camera -> free camera at the same shot
      else ToggleFree(cam, fc);                         // Session 98: orbit <-> free
    }
    else if (wp == 'K' && !repeat && !ui.WantTextInput()) PathKeyAdd();  // Session 98: a path key from the current camera
    else if (wp == key::kEscape) quit = true;
  }
  void OnFocusLost() override { EndLook(); fc.keys = FreeKeys(); }
  void OnCaptureLost() override { EndLook(); }
  // ---- Session 98 phone port (stage C): touch (platform_sdl.cpp's gestures; moves in dp). The same directions as the mouse: one finger
  // turns like the mouse look (free) / the left-button orbit drag (orbit); two fingers move the camera with the picture (pan) and
  // pinch forward / back. A game pose (the entrance camera) hands over to the free camera at its shot, as a right click does.
  bool TouchOnUi(float x, float y) override { return ui.Ready() && ui.HitTest(x, y); }
  void TouchTakeOver() {
    if (job.Active()) return;
    if (ent.cam_entrance || cam.use_vp) { ent.cam_entrance = false; EnterFree(cam, fc); }
  }
  void OnTouchLook(float dx, float dy) override {
    TouchTakeOver();
    if (cam.free) {
      fc.look_yaw += dx * fc.sensitivity;
      fc.look_pitch -= dy * fc.sensitivity;
    } else {
      cam.yaw -= dx * 0.4f;
      cam.pitch = std::max(-89.5f, std::min(89.5f, cam.pitch + dy * 0.4f));
      if (importer) cam.pitch = std::clamp(cam.pitch, -25.f, 45.f);
    }
  }
  void OnTouchPan(float dx, float dy) override {
    if (importer) { const float k = plat ? plat->PixelsPerDp() : 1.f; ImporterPan(dx * k, dy * k); return; }  // Session 98 (importer): two fingers move the picture
    TouchTakeOver();
    if (cam.free) {  // the camera moves against the fingers (the picture follows them); speed = the FREECAM speed
      const float k = fc.speed * 0.003f;
      const V3 rt = FreeRight(cam.fyaw, cam.fpitch), up = FreeUp(cam.fyaw, cam.fpitch, 0);
      for (int i = 0; i < 3; i++) cam.fpos[i] += (-(&rt.x)[i] * dx + (&up.x)[i] * dy) * k;
    } else {  // the orbit target moves in the view plane, scaled by the distance
      const float k = cam.dist * 0.0015f, fy = WrapDeg(cam.yaw + 180.0f), fp = std::max(-89.5f, std::min(89.5f, -cam.pitch));
      const V3 rt = FreeRight(fy, fp), up = FreeUp(fy, fp, 0);
      for (int i = 0; i < 3; i++) cam.target[i] += (-(&rt.x)[i] * dx + (&up.x)[i] * dy) * k;
    }
  }
  void OnTouchDolly(float d) override {
    if (importer) { imp_zoom = std::clamp(imp_zoom * std::exp(-d * 0.004f), 0.25f, 3.f); return; }  // Session 98 (importer): pinch = zoom
    TouchTakeOver();
    if (cam.free) {
      const V3 fw = FreeForward(cam.fyaw, cam.fpitch);
      for (int i = 0; i < 3; i++) cam.fpos[i] += (&fw.x)[i] * d * fc.speed * 0.006f;
    } else {
      cam.dist = std::max(1.0f, cam.dist * std::exp(-d * 0.004f));
    }
  }
  void OnTap(int x, int y) override { ClickSelect(x, y); }
  void OnSurfaceGone() override {
#ifdef WWE_VIEWER_VK
    if (auto* vk = dynamic_cast<RendererVk*>(&r)) vk->SurfaceGone();
#endif
    fprintf(stderr, "surface gone (background)\n");
  }
  void OnSurfaceBack() override {
    bool ok = false;
#ifdef WWE_VIEWER_VK
    if (auto* vk = dynamic_cast<RendererVk*>(&r)) ok = vk->SurfaceBack();
#endif
    fprintf(stderr, "surface back (foreground): %s\n", ok ? "ok" : "FAILED");
    if (plat) {  // (the new swap chain at the window's size now)
      int cw = 0, ch = 0;
      plat->ClientSize(cw, ch);
      if (cw > 0 && ch > 0) r.Resize(cw, ch);
    }
  }
};

int main(int argc_in, char** argv_in) {
  std::string script;
  bool exit_after = false;
  unsigned long parent_pid = 0;
  std::string media_dir;
  int media_tron = 100;
  std::string mods_arg;
  // Session 98 (DLC): no flag = the usual DLC folder (`dlc` beside the game data), every ready pack (src\assets\dlc_files.h)
  bool dlc_on = true;
  std::string dlc_dir, dlc_off;
  bool pipe_given = false;
  std::string pipe_token;
  std::vector<std::string> args;
#ifdef _WIN32
  // (argv is ANSI: take the command line as UTF-16 and convert, so a non-ASCII path survives)
  {
    int wargc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    for (int i = 0; wargv && i < wargc; i++) args.push_back(Utf8(wargv[i]));
    if (wargv) LocalFree(wargv);
  }
  (void)argc_in, (void)argv_in;
#else
  for (int i = 0; i < argc_in; i++) args.push_back(argv_in[i]);
#endif
#ifdef __ANDROID__
  {  // Session 98 phone port (stage C): stdout / stderr (every reply and message) into <files>/logs/viewer.log (nothing reads them on a phone)
    const std::filesystem::path logs = std::filesystem::u8path(LocalDataDir()) / "logs";
    std::error_code ec;
    std::filesystem::create_directories(logs, ec);
    const std::string lf = (logs / "viewer.log").string();
    if (std::freopen(lf.c_str(), "w", stderr)) setvbuf(stderr, nullptr, _IONBF, 0);
    std::freopen((logs / "viewer_out.log").string().c_str(), "w", stdout);
    std::string line;
    for (const auto& a : args) line += " " + a;
    fprintf(stderr, "wwe2k14 arena viewer (android), files %s, args:%s\n", LocalDataDir().c_str(), line.c_str());
  }
#endif
  const int argc = (int)args.size();
  // Session 98 phone port: the renderer backend first (the Viewer is built around it). d3d11 = the PC default; vk = Vulkan in an SDL3 window.
#ifdef _WIN32
  std::string backend = "d3d11";
#else
  std::string backend = "vk";
#endif
  for (int i = 1; i + 1 < argc; i++)
    if (args[i] == "--backend") backend = args[i + 1];
  std::string err;
  std::unique_ptr<Renderer> rend = MakeRenderer(backend, err);
  if (!rend) { fprintf(stderr, "--backend %s: %s\n", backend.c_str(), err.c_str()); return 2; }
  Viewer v(std::move(rend));
  for (int i = 1; i < argc; i++) {
    std::string a = args[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? args[++i] : std::string(); };
    if (a == "--game") v.game = next();
    else if (a == "--pipe") v.pipe_name = next(), pipe_given = true;
    else if (a == "--pipe-token") pipe_token = next();  // Session 98 stage C review: the phone's TCP command port (pipe_server.h)
    else if (a == "--script") script = next();
    else if (a == "--exit" || a == "--exit-after-script") exit_after = true;
    else if (a == "--hidden") v.hidden = true;
    else if (a == "--guest_image") v.guest_image = next();  // Session 98 (stance): the guest image holding the stance table (StanceAct)
    else if (a == "--media") media_dir = next();  // Session 97: stage movies folder (see the "stage" command)
    else if (a == "--tron") media_tron = atoi(next().c_str());
    else if (a == "--mods") mods_arg = next();  // Session 98: the launcher's mods folder (the MODS panel, the tron preview)
    else if (a == "--dlc") dlc_dir = next(), dlc_on = true;  // Session 98 (DLC): the launcher's DLC folder (USE MY DLC on)
    else if (a == "--dlc-off") dlc_off = next();             //   pack ids switched off, "3,5"
    else if (a == "--no-dlc") dlc_on = false;                //   USE MY DLC off
    else if (a == "--parent") parent_pid = strtoul(next().c_str(), nullptr, 10);
    else if (a == "--importer") v.importer = true;                  // Session 98: the SUPERSTAR IMPORTER window (ui_importer.h)
    else if (a == "--settings-file") v.imp.settings_file = ui::LauncherSettingsFile() = next();  //   the launcher's settings file ("superstars" in the importer, "lighting" in the LIGHT tab)
    else if (a == "--importer-edit") v.imp.edit_slot = atoi(next().c_str());
    else if (a == "--importer-bulk") v.imp.bulk_at_start = true;
    else if (a == "--backend") next();  // (read above)
    else if (a == "--size") {
      std::string s = next();
      int ww = 0, hh = 0;
      if (sscanf(s.c_str(), "%dx%d", &ww, &hh) == 2 && ww > 0 && hh > 0) { v.w = ww; v.h = hh; }
    } else { fprintf(stderr, "unknown argument %s\n", a.c_str()); return 2; }
  }
  ViewerInit(v.game);
  fprintf(stderr, "%s\n", ViewerSetDlc(dlc_on, dlc_dir, dlc_off).c_str());  // before anything is read: every file lookup sees the packs
  ui::RegisterModsPlatform();
  assets::mods_lookup::Cfg().game_root = std::filesystem::u8path(v.game);
  assets::mods_lookup::Cfg().async = !v.hidden;  // the windowed viewer never blocks on a video decode (a worker does it)
#ifdef __ANDROID__
  // Session 98 (phone test: IMPORT CHARACTER greyed out - the phone had no mods folder): the phone's mods folder is <files>/mods
  // (the app's own folder, reachable over USB: Android/data/<app>/files/mods), made here when the launcher passes none.
  if (mods_arg.empty() && !LocalDataDir().empty()) {
    const std::filesystem::path md = std::filesystem::u8path(LocalDataDir()) / "mods";
    std::error_code ec;
    std::filesystem::create_directories(md, ec);
    const auto u = md.u8string();
    mods_arg.assign(u.begin(), u.end());
  }
#endif
  if (v.importer) {
    // Session 98 (Superstar Importer): the pack lives in the player's mods folder (the core writes <mods>\superstars); the viewer's own
    // mods lookups point at a private preview folder instead (the shown pac is copied there, one at a time; nothing of the player's).
    v.imp.mods_dir = mods_arg, v.imp.dlc_dir = dlc_on ? dlc_dir : std::string(), v.imp.game = v.game;
#ifdef __ANDROID__
    v.imp.phone = true;
    {
      const auto u = (std::filesystem::u8path(LocalDataDir()) / "importer" / "picked").u8string();
      v.imp.picked_dir.assign(u.begin(), u.end());  // (mods_android.cpp PickFiles kinds 1 / 2 copy there)
    }
#endif
    const std::filesystem::path pv = std::filesystem::u8path(LocalDataDir()) / "wwe2k14-importer" / "preview";
    std::error_code ec;
    std::filesystem::create_directories(pv, ec);
    const std::string why = assets::mods_lookup::SetDir(pv);
    if (!why.empty()) fprintf(stderr, "importer preview folder refused: %s\n", why.c_str());
    assets::mods_lookup::Cfg().on = true;
    if (mods_arg.empty()) fprintf(stderr, "importer: no --mods folder: the pack cannot be saved\n");
  } else if (!mods_arg.empty()) {
    const std::string why = assets::mods_lookup::SetDir(std::filesystem::u8path(mods_arg));
    if (!why.empty()) fprintf(stderr, "--mods refused: %s\n", why.c_str());
  }
  assets::mods_lookup::Reindex();
  {  // Session 98 phone port: the launcher's tron cache folder comes from the platform (assets\stage_media.h DefaultDir)
#ifdef __ANDROID__
    auto td = (std::filesystem::u8path(LocalDataDir()) / "trons").u8string();  // stage C: <files>/trons (PNG stills: the phone launcher's BUILD TRON PREVIEWS)
#else
    auto td = (std::filesystem::u8path(LocalDataDir()) / "wwe2k14-launcher" / "trons").u8string();
#endif
    assets::stage_media::Cfg().default_dir = std::string(td.begin(), td.end());
  }
  v.last_media = media_dir;
  if (!media_dir.empty() || media_tron != 100) {
    std::string merr;
    if (!ViewerStageMedia(media_dir, media_tron, false, {}, merr)) fprintf(stderr, "--media: %s\n", merr.c_str());
  }

#ifdef _WIN32
  if (parent_pid) {  // exit when the process that started us ends (a crashed census must not leave a viewer on the pipe)
    HANDLE ph = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)parent_pid);
    if (ph) std::thread([ph] { WaitForSingleObject(ph, INFINITE); ExitProcess(0); }).detach();
  }
#else
  (void)parent_pid;
#endif

  if (!v.hidden) {  // the window: Win32 for d3d11, SDL3 for vk
#ifdef WWE_VIEWER_VK
    if (backend == "vk") v.plat = MakeSdlPlatform();
#endif
#ifdef _WIN32
    if (backend == "d3d11") v.plat = MakeWin32Platform();
#endif
    if (!v.plat) { fprintf(stderr, "no window platform for --backend %s\n", backend.c_str()); return 1; }
    if (!v.plat->CreateMainWindow(v.importer ? "WWE 2K14 Superstar Importer" : "WWE 2K14 asset viewer", v.w, v.h, err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
  }
  if (!v.r.Init(v.plat ? v.plat->MakeSurface() : Surface{}, v.w, v.h, err)) { fprintf(stderr, "renderer init failed: %s\n", err.c_str()); return 1; }
  if (v.plat) {  // the swap chain must match the window's real client size
    int cw = 0, ch = 0;
    v.plat->ClientSize(cw, ch);
    v.r.Resize(cw, ch);
  }
  v.Preset("three_quarter");
  if (v.plat) {  // Session 98: the Arena Viewer UI over the scene (a failure leaves the plain viewer)
    std::string uerr;
    std::unique_ptr<ui::Backend> be = v.plat->MakeUiBackend(&v.r, uerr);
    if (be && v.ui.Init(std::move(be), v.game, [&v](const std::string& l) { return v.Safe(l); }, uerr)) {
      v.ui.SetPathUi(&v.pu);  // Session 98 (camera path)
      v.ui.SetFreeCam(&v.cam, &v.fc);  // Session 98: the FREECAM panel edits the camera and the controller directly (same thread)
      if (v.importer) {  // Session 98 (Superstar Importer): the importer window; the portraits come from this viewer's renderer
        v.imp.portrait = [&v](int kind, std::vector<uint8_t>& px, int& pw, int& ph, std::string& e) { return v.ImporterPortrait(kind, px, pw, ph, e); };
        v.ui.SetImporter(&v.imp);
        v.r.SetEffect("dof", false);  // (a sharp preview and sharp portraits)
        v.ImporterBackground();
      }
      v.r.SetOverlay([&v] { v.ui.SetCameraMode(v.cam.use_vp ? "GAME" : v.cam.free ? "FREE" : "ORBIT"); v.ui.SetEntrance(v.EntranceViewPtr()); v.ui.Draw(); });
    }
    else fprintf(stderr, "UI init failed: %s\n", uerr.c_str());
  }

  Mailbox mb;
  PipeServer ps(mb, v.pipe_name);
#ifdef _WIN32
  (void)pipe_given, (void)pipe_token;
  if (!exit_after) {
    if (!ps.Start(err)) { fprintf(stderr, "%s\n", err.c_str()); return 3; }
  }
#else
  // Session 98 stage C review: off Windows (the phone) the command port is a 127.0.0.1 TCP port every app on the phone can reach:
  // open only when asked (--pipe, from tools\android\run.ps1 -Viewer; the launcher never passes it) and only with the run's token.
  // A failure is a log line: the viewer itself works without it.
  ps.SetToken(pipe_token);
  if (!exit_after && pipe_given) {
    if (ps.Start(err)) fprintf(stderr, "command port: 127.0.0.1:%d (first line = the token)\n", ps.Port());
    else fprintf(stderr, "command port not opened: %s\n", err.c_str());
  }
#endif

  if (v.plat && script.empty() && !exit_after && !v.importer) {  // started from the launcher: show something to look at
    v.browse = true;
    v.BrowseKind(true);
  }

  if (!script.empty()) {
    std::ifstream f(std::filesystem::u8path(script));
    if (!f) { fprintf(stderr, "cannot open script %s\n", script.c_str()); return 1; }
    std::string line;
    while (std::getline(f, line) && !v.quit) {
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
      if (line.empty() || line[0] == '#') continue;
      std::string reply = v.Safe(line);
      while (v.job.Active() && !v.quit) { v.job.Step(); if (v.job.Active() && v.r.DeviceLost()) break; if (v.job.StatusJson().find("\"encoding\"") != std::string::npos) SleepMs(50); }  // a script waits for its render
      printf("%s\n", reply.c_str());
      fflush(stdout);
    }
    if (exit_after) v.quit = true;
  }

  while (!v.quit) {
    if (v.plat) {
      v.plat->Pump(v);
      mb.Pump([&](const std::string& l) { return v.Execute(l); });
      if (v.quit) break;
      v.UpdateTitle();
      if (!v.job.Active()) v.Tick();  // (a running render sets the clock itself)
      v.TronTick();       // Session 98: tron video (the render sets its own picture per frame)
      v.FreeTickFrame();  // Session 98
      v.PathTick();       // Session 98: path preview (overrides the free camera while it plays)
      if (v.job.Active()) v.job.Step();
      v.SyncRenderUi();
      v.PickTick();       // Session 98 (step 16): hover
      if (v.plat->Minimized()) { SleepMs(30); continue; }
      double t0 = NowMs();
      // Session 98: queued UI actions (scene loads) run here, outside the frame (not during a render). One that blocked for a while
      // (the phone's file picker covered the app: paused, surface gone) skips this frame's draw: the next pass pumps the platform's
      // events first, so the draw never runs on a surface Android took away meanwhile.
      if (!v.job.Active() && v.ui.Pump()) continue;
      v.ImporterFrame();            // Session 98 (Superstar Importer): the model framed into the preview rectangle
      v.MatchLightsFollowCamera();  // Session 98 (match light): after the path preview moved the camera
      v.r.DrawFrame(v.cam);
      double dt = NowMs() - t0;
      v.frame_ms = v.frame_ms == 0 ? dt : v.frame_ms * 0.9 + dt * 0.1;
    } else {
      mb.Pump([&](const std::string& l) { return v.Execute(l); });
      if (v.job.Active()) { v.job.Step(); v.SyncRenderUi(); }  // Session 98: an offline render runs between commands
      else mb.Wait(20);
    }
  }
  v.job.Cancel();  // (ends a running ffmpeg)
  // let the quit reply reach the client before the process ends
  double t0 = NowMs();
  while (mb.Pending() > 0 && NowMs() - t0 < 1500) SleepMs(5);
  if (v.plat) v.plat->Destroy();
  fflush(stdout);
  // (the detached pipe thread may still touch the Mailbox that main's return would destroy)
#ifdef _WIN32
  ExitProcess((UINT)v.exit_code);
#else
  _Exit(v.exit_code);
#endif
}
