// The live mirror (LIVE_ADAPTER.md stage A). Every image's contents are sent again from here on, like at a capture's start.
inline void LiveLog(const char* line) { REXLOG_INFO("{}", line); }
inline void OnHostWrite(uint32_t addr, uint32_t length);  // (with the page marks below)
inline void OnHostWriteBy(uint32_t addr, uint32_t length, wwe2k14::host_write::Writer writer, const char* file, uint32_t source);  // S101 A1c (below)
inline void StartLive() {
  // START-ONLY(ON): the guest's host-write reports feed the stream's page marks; the stream-free build has no stream to mark
  wwe2k14::host_write::g_hook.store(&OnHostWrite, std::memory_order_relaxed);  // strip-360 E2 / E3: the host writers' reports
  // S101 names A1c: who wrote each page: installed only when provenance is wanted (InstallProvenanceAtStart; a capture installs it itself)
  cap::LiveOptions options;
  // S103 SPB: the settings both start paths apply (the scene-input block, PlayerQuality, the renderer-option half) are applied by ONE function (start_settings.cpp) that the stream-free start calls too;
  // this start copies its results into LiveOptions instead of reading the flags a second time.
  const wwe2k14::start::Applied& applied = wwe2k14::start::ApplyStartSettings();
  options.validation = REXCVAR_GET(wwe2k14_rdr_live_validation);
  options.gpu_timestamps = applied.gpu_timestamps;  // lane 269 (S103 SPB: start_settings.cpp)
  options.gpu_name = REXCVAR_GET(wwe2k14_gpu);  // S102 U1
  options.bc_decode = uint32_t(std::clamp(REXCVAR_GET(wwe2k14_rdr_bc_decode), 0, 2));  // S102 U1: read when the renderer starts (a device property, not per frame)
  options.counts = applied.counts;  // S102 M1 (S103 SPB: start_settings.cpp)
  // S103 P4F (dev_cvar_lint): WWE2K14_VERSION is defined for main.cpp and dev_build.cpp only, so the old `#if defined(WWE2K14_VERSION)` here was always false and kDevBuild always true: a RELEASE build honoured the test mask and test_fail_start.
  const bool kDevBuild = wwe2k14::IsDevBuild();
  // Lane 200 review SF2: a release build ignores the test mask (its 0x80000000 bit deliberately sends invalid Vulkan usage).
  if (wwe2k14::IsDevBuild() && !REXCVAR_GET(wwe2k14_rdr_format_caps_mask).empty()) options.format_caps_mask = uint32_t(std::strtoul(REXCVAR_GET(wwe2k14_rdr_format_caps_mask).c_str(), nullptr, 16));  // lane 200 (dev switch)
  options.png_folder = CapturesRoot() / "live";
  {
    std::string list = REXCVAR_GET(wwe2k14_rdr_live_picture_isolates), item;
    for (char ch : list + ",") {
      if (ch == ',') {
        if (!item.empty()) options.picture_isolates.push_back(item);
        item.clear();
      } else if (ch != ' ') {
        item += ch;
      }
    }
  }
  options.png_every = uint32_t(std::max(0, REXCVAR_GET(wwe2k14_rdr_live_png_every)));
  {  // S103 L12: --wwe2k14_dump_stage_page=FRAMES (empty = off: no frame list, no read-back, nothing changes)
    const std::string sp = REXCVAR_GET(wwe2k14_dump_stage_page);
    for (size_t at = 0; at < sp.size();) {
      const size_t comma = sp.find(',', at);
      const std::string item = sp.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
      if (!item.empty()) options.stage_page_frames.push_back(std::strtoull(item.c_str(), nullptr, 10));
      if (comma == std::string::npos) break;
      at = comma + 1;
    }
    options.stage_page_dir = CapturesRoot().parent_path() / "s103" / "stage_page";
  }
  {  // S103 T7: --wwe2k14_dump_targets=FRAMES (empty = off: no frame list, no read-back, nothing changes)
    options.dump_targets_frames = rdr_adapter::stage_mirror::ParseFrames(REXCVAR_GET(wwe2k14_dump_targets));
    options.dump_targets_dir = CapturesRoot().parent_path() / "s103" / "stage_mirror";
  }
  const std::string list = REXCVAR_GET(wwe2k14_rdr_live_png);
  for (size_t at = 0; at < list.size();) {
    const size_t comma = list.find(',', at);
    const std::string item = list.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
    if (!item.empty()) options.png_frames.push_back(std::strtoull(item.c_str(), nullptr, 10));
    if (comma == std::string::npos) break;
    at = comma + 1;
  }
  options.max_queued = uint32_t(std::max(1, REXCVAR_GET(wwe2k14_rdr_max_queued)));
  options.keep_state_copies = REXCVAR_GET(wwe2k14_rdr_skip_keeps_state);
  options.perframe_by_image = REXCVAR_GET(wwe2k14_rdr_perframe_by_image);   // lane 307
  options.prune = REXCVAR_GET(wwe2k14_rdr_skip_prune);   // lane 307
  options.catch_up_state = REXCVAR_GET(wwe2k14_rdr_catch_up_state);
  options.log = LiveLog;
  options.picture_done = [] { wwe2k14::g_live_pictures_in_flight.fetch_sub(1); };
  // START-ONLY(ON): the stage view draws over the stream's frames (a dev tool of the stream path)
  rdr_adapter::stage_view::g_enabled = REXCVAR_GET(wwe2k14_stage_view);  // S101 lane 59: outside the owner block (the guest thread stamps frames whenever the ring or the renderer takes them)
  // START-ONLY(ON): owner mode is the only mode of the stream-free build (owner_cvars_off.cpp pins wwe2k14_rdr_owner true)
  if (REXCVAR_GET(wwe2k14_rdr_owner)) {  // stage B: the game window, handed over by the app (OnPostSetup); presentation is detached (OnPreSetup)
    options.window_instance = g_owner_instance;
    options.window = g_owner_window;
    options.window_source = g_owner_window_source;  // lane 235
    options.window_source_user = g_owner_window_source_user;
    // S100: the display rate follows the game's V-SYNC switch (the SDK's `vsync`, set by the launcher); off: the swapchain presents without
    // waiting for the display (mailbox, else immediate) and our renderer draws as many frames as it can.
    if (rex::cvar::GetFlagInfo("vsync")) options.vsync = rex::cvar::Query<bool>("vsync");
    if (!REXCVAR_GET(wwe2k14_vsync)) options.vsync = false;  // settings parity S13: the player's V-SYNC (present only)
    // Settings parity S1 (Jarrod 08:1x, DECISIONS): the launcher's RESOLUTION (720p / 1440p / 4K = the SDK's resolution_scale 1 / 2 / 3,
    // gfx_settings.h) is the new renderer's render scale: the game's 1280x720 targets drawn at that multiple (images marked kUsageScaled).
    if (rex::cvar::GetFlagInfo("resolution_scale")) {
      const int scale = rex::cvar::Query<int>("resolution_scale");
      if (scale >= 1 && scale <= 4) options.render_scale = float(scale);
    }
    // Settings parity S1b (Jarrod's phone log 017, 11:4x: 1440p with a 1080 draw height drew the full 1440p): the RESOLUTION entries with a
    // smaller draw height (360 / 480 / 540 at 720p, 1080 at 1440p: the SDK's gpu_native_truth_draw_height, gfx_settings.h / mobile_settings.h)
    // are a render scale of draw height / 720 (1080 -> 1.5); the present scales the picture to the screen (SGSR where it is on).
    if (rex::cvar::GetFlagInfo("gpu_native_truth_draw_height")) {
      const int draw_height = std::atoi(rex::cvar::GetFlagByName("gpu_native_truth_draw_height").c_str());
      if (draw_height >= 360 && draw_height <= 2880 && float(draw_height) < options.render_scale * 720.0f) options.render_scale = float(draw_height) / 720.0f;
    }
    // START-ONLY(ON): the stream's sampler path reads the override; the stream-free build's samplers are fixed (ApplyStartSettings refuses it out loud: RefuseSetting)
    if (rex::cvar::GetFlagInfo("anisotropic_override")) g_aniso_override = rex::cvar::Query<int>("anisotropic_override");  // S2
    g_debug_lod0 = REXCVAR_GET(wwe2k14_rdr_debug_lod0);  // S100 diagnosis
    g_debug_no_aniso = REXCVAR_GET(wwe2k14_rdr_debug_no_aniso);
    g_scale_all = REXCVAR_GET(wwe2k14_rdr_scale_all);  // S100 diagnosis
    if (g_scale_all) REXLOG_INFO("[rdr][settings] DIAGNOSIS: every image follows the render scale (--wwe2k14_rdr_scale_all)");
    if (g_debug_no_aniso) REXLOG_INFO("[rdr][settings] DIAGNOSIS: no anisotropic filtering on any sampler (--wwe2k14_rdr_debug_no_aniso)");
    if (g_debug_lod0) REXLOG_INFO("[rdr][settings] DIAGNOSIS: every sampler reads only the first mip of its range (--wwe2k14_rdr_debug_lod0)");
    // START-ONLY-BEGIN(ON): the stream-free build has no in-place replacers: its scene draws everything itself and these switches do not exist there
    options.post_ours = REXCVAR_GET(wwe2k14_rdr_post_ours);  // S101 lane PO (no scene record needed)
    options.crowd_ours = REXCVAR_GET(wwe2k14_rdr_crowd_ours) && REXCVAR_GET(wwe2k14_scene_records);  // S101 lane 35
    options.crowd_floor = options.crowd_ours && REXCVAR_GET(wwe2k14_rdr_crowd_floor);  // S101 lane 82: the seated floor people, only with the crowd promotion
    options.crowd_cards = options.crowd_ours && REXCVAR_GET(wwe2k14_rdr_crowd_cards);  // S101 lane CD: the far cards, only with the crowd promotion
    options.crowd_cards_thin_probe = options.crowd_cards && (REXCVAR_GET(wwe2k14_rdr_crowd_card_thin) >= 1 || REXCVAR_GET(wwe2k14_rdr_card_cut));  // lane 175 (SF2)
    options.crowd_cards_thin = false;  // S102 T2 F6: the start-time copy is gone; the binder's thin branch follows --wwe2k14_rdr_card_cut every cards frame through options.cards_thin_now (card_thin::g_cut_seen, live.cpp). The gate reports flow with the probe
    options.crowd_members_probe = options.crowd_ours && REXCVAR_GET(wwe2k14_rdr_crowd_member_thin) >= 1;  // lane 272: census (the uniformity verify, the stamp probe, the gate's reports)
    options.crowd_members_thin = options.crowd_ours && REXCVAR_GET(wwe2k14_rdr_crowd_member_thin) >= 2 && wwe2k14::member_thin::kSkipBuilt && REXCVAR_GET(wwe2k14_scene_crowd) == 0;  // lane 272: the binder's thin branch exists only while the cut can act
    options.arena_ours = REXCVAR_GET(wwe2k14_rdr_arena_ours) && REXCVAR_GET(wwe2k14_scene_records);  // S101 lane 41
    // (S103 L8: the stage-page switch is stored by ApplyStartSettings, start_settings.cpp, for both builds)
    options.wrestlers_ours = REXCVAR_GET(wwe2k14_rdr_wrestlers_ours) && REXCVAR_GET(wwe2k14_scene_records);
    options.caw_ours = REXCVAR_GET(wwe2k14_rdr_caw_ours);
    options.caw_cache_root = g_caw_cache_root;   // lane 153 (empty = no counts-index cache)   // lane 153 (no scene record needed)
    if (REXCVAR_GET(wwe2k14_rdr_pipeline_store)) options.pipeline_store_dir = g_pipeline_store_root, options.pipeline_prebuild_slice_us = uint32_t(REXCVAR_GET(wwe2k14_rdr_pipeline_prebuild_slice_us));  // lane 276
    options.arena_thin = REXCVAR_GET(wwe2k14_rdr_arena_thin) && options.arena_ours;  // lane 122
    options.wrestlers_thin = REXCVAR_GET(wwe2k14_rdr_wrestlers_thin) && options.wrestlers_ours;  // lane 129 (KC); the ring's is set below, after ring_ours
    if (REXCVAR_GET(wwe2k14_rdr_wrestlers_thin) && !options.wrestlers_ours) REXLOG_WARN("[rdr][thin] --wwe2k14_rdr_wrestlers_thin needs --wwe2k14_rdr_wrestlers_ours (and --wwe2k14_scene_records): off");  // lane 129
    if (REXCVAR_GET(wwe2k14_rdr_arena_thin) && !options.arena_ours) REXLOG_WARN("[rdr][thin] --wwe2k14_rdr_arena_thin needs --wwe2k14_rdr_arena_ours (and --wwe2k14_scene_records): off");  // lane 122
    options.ring_ours = REXCVAR_GET(wwe2k14_rdr_ring_ours) && REXCVAR_GET(wwe2k14_scene_records);  // S101 ring contract
    // START-ONLY-END
    // NOT-READ-YET(OFF): ui_low_ram (SP2 M9): StartPictures indexes bgEtc on a phone too; the stream's UI class skips it for memory (lane 126)
    {   // lane 126: low_ram.h's rule without its RAM probe: "on" forces it; "auto" = a phone (low_ram.h also needs < 6 GB: a bigger phone still skips bgEtc, the safe side)
      // NOT-READ-YET(OFF): ui_low_ram (SP2 M9, see above): the stream-free start does not read the low-RAM switch yet
      const std::string low_ram_mode = REXCVAR_GET(wwe2k14_low_ram);
#if defined(__ANDROID__)
      options.ui_low_ram = low_ram_mode != "off";
#else
      options.ui_low_ram = low_ram_mode == "on";
#endif
    }
    // START-ONLY-BEGIN(ON): the stream-free build has no in-place replacers: its scene draws everything itself and these switches do not exist there
    options.movie_ours = REXCVAR_GET(wwe2k14_rdr_movie_ours);  // S101 lane 131 (no scene record needed)
    options.ring_thin = REXCVAR_GET(wwe2k14_rdr_ring_thin) && options.ring_ours;  // lane 129 (KC)
    options.arena_d3 = REXCVAR_GET(wwe2k14_rdr_arena_d3) && options.arena_thin && REXCVAR_GET(wwe2k14_scene_light_states) && REXCVAR_GET(wwe2k14_rdr_owner);   // lane 152 (S2): a skip removes the object from the picture, so only the owner (our renderer draws everything) may do it  // lane 138 (D3): arena_ours + arena_thin + scene records (arena_thin has them) + light states
    options.arena_d3_verify = options.arena_d3 && REXCVAR_GET(wwe2k14_rdr_arena_d3_verify);   // lane 152
    if (REXCVAR_GET(wwe2k14_rdr_arena_d3) && !options.arena_d3) REXLOG_WARN("[rdr][d3] --wwe2k14_rdr_arena_d3 needs --wwe2k14_rdr_arena_ours, --wwe2k14_rdr_arena_thin, --wwe2k14_scene_records, --wwe2k14_scene_light_states and owner mode: off");  // lane 138
    if (REXCVAR_GET(wwe2k14_rdr_ring_thin) && !options.ring_ours) REXLOG_WARN("[rdr][thin] --wwe2k14_rdr_ring_thin needs --wwe2k14_rdr_ring_ours (and --wwe2k14_scene_records): off");  // lane 129
    options.ui_ours = REXCVAR_GET(wwe2k14_rdr_ui_ours);  // S101 UI contract (no scene record needed)
    options.ui_quads = REXCVAR_GET(wwe2k14_rdr_ui_quads_ours) && options.ui_ours;  // S101 lane 148 (the quads ride on the UI class)
    if (REXCVAR_GET(wwe2k14_rdr_ui_quads_ours) && !options.ui_ours) REXLOG_WARN("[rdr][ui] --wwe2k14_rdr_ui_quads_ours needs --wwe2k14_rdr_ui_ours: off");  // lane 148 (review S7)
    options.ui_text_ours = REXCVAR_GET(wwe2k14_rdr_ui_text_ours);  // S101 lane 89 (UT; no scene record needed)
    options.flash_ours = REXCVAR_GET(wwe2k14_rdr_flash_ours) && REXCVAR_GET(wwe2k14_scene_records);  // S101 flash contract (the arena id from the record)
    options.props_ours = REXCVAR_GET(wwe2k14_rdr_props_ours) && REXCVAR_GET(wwe2k14_scene_records);  // S101 lane 84
    options.screens_ours = REXCVAR_GET(wwe2k14_rdr_screens_ours) && REXCVAR_GET(wwe2k14_scene_records);  // S101 lane 115
    options.crowd_props_ours = REXCVAR_GET(wwe2k14_rdr_crowd_props_ours) && REXCVAR_GET(wwe2k14_scene_records);  // S101 lane 133
    if (REXCVAR_GET(wwe2k14_rdr_crowd_props_ours) && !REXCVAR_GET(wwe2k14_scene_records)) REXLOG_WARN("[rdr][crowd_props] --wwe2k14_rdr_crowd_props_ours needs --wwe2k14_scene_records: off");  // S101 lane 133
    if (REXCVAR_GET(wwe2k14_rdr_screens_ours) && !REXCVAR_GET(wwe2k14_scene_records)) REXLOG_WARN("[rdr][screens] --wwe2k14_rdr_screens_ours needs --wwe2k14_scene_records: off");  // S101 lane 115
    if (REXCVAR_GET(wwe2k14_rdr_props_ours) && !REXCVAR_GET(wwe2k14_scene_records)) REXLOG_WARN("[rdr][props] --wwe2k14_rdr_props_ours needs --wwe2k14_scene_records: off");  // S101 lane 84 (review F8)
    if (REXCVAR_GET(wwe2k14_rdr_flash_ours) && !REXCVAR_GET(wwe2k14_scene_records)) REXLOG_WARN("[rdr][flash] --wwe2k14_rdr_flash_ours needs --wwe2k14_scene_records: off");  // S101 lane 84 (review F8)
    if (REXCVAR_GET(wwe2k14_rdr_shadow_casters_ours) && !REXCVAR_GET(wwe2k14_scene_records)) REXLOG_WARN("[rdr][shadow_casters] --wwe2k14_rdr_shadow_casters_ours needs --wwe2k14_scene_records: off");  // S101 lane 99 (review F8)
    options.shadow_casters_ours = REXCVAR_GET(wwe2k14_rdr_shadow_casters_ours) && REXCVAR_GET(wwe2k14_scene_records);  // S101 lane 99
    if (REXCVAR_GET(wwe2k14_rdr_rim_ours) && !REXCVAR_GET(wwe2k14_scene_records)) REXLOG_WARN("[rdr][rim_map] --wwe2k14_rdr_rim_ours needs --wwe2k14_scene_records: off");  // lane 124
    options.rim_ours = REXCVAR_GET(wwe2k14_rdr_rim_ours) && REXCVAR_GET(wwe2k14_scene_records);  // lane 124
    // lane 228 (Caster D3 live seam): a skipped caster pass removes its draws from the game's stream, so ONLY the owner (our renderer draws everything) with our caster geometry for the maps and the rim map (the marker's
    // mini frame is drawn through those hooks) may do it; the tracker (wwe2k14_app.h) needs the light states too. Android registers none of it (wwe2k14_app.h).
    options.caster_d3 = REXCVAR_GET(wwe2k14_rdr_caster_d3) && options.shadow_casters_ours && options.rim_ours && REXCVAR_GET(wwe2k14_scene_light_states) && REXCVAR_GET(wwe2k14_rdr_owner);
#if defined(__ANDROID__)
    options.caster_d3 = false;   // the tracker is not registered on Android (wwe2k14_app.h: SafeCopy is not fault-safe there): no records, so no skip either
#endif
    options.caster_d3_verify = options.caster_d3 && REXCVAR_GET(wwe2k14_rdr_caster_d3_verify);
    // lane 268 (+ review S1 / N2); lane 299: the skip arm is open (verify run 7199 passed: markers 29768, mismatching 0, unsourced 0, length differs 0); a skipped plane call replays its PlaneDraw entries (d3_caster_hook.h, 17.15). The plane tracker (the c0-c16 tables) is registered by --wwe2k14_scene_plane OR --wwe2k14_rdr_plane_shadow_ours (wwe2k14_app.h)
    options.caster_d3_plane = options.caster_d3 && REXCVAR_GET(wwe2k14_rdr_caster_d3_plane) && (REXCVAR_GET(wwe2k14_scene_plane) || REXCVAR_GET(wwe2k14_rdr_plane_shadow_ours));
    if (REXCVAR_GET(wwe2k14_rdr_caster_d3_plane) && !options.caster_d3_plane) REXLOG_WARN("[rdr][d3] --wwe2k14_rdr_caster_d3_plane needs --wwe2k14_rdr_caster_d3 (with its requirements) and --wwe2k14_scene_plane (or --wwe2k14_rdr_plane_shadow_ours): the plane stays out of the caster cut");
    // lane 246 (Ring D3 live seam, 16.9): a skipped ring call removes its draws from the game's stream, so ONLY the owner (our renderer draws everything) with the ring thin draws (the marker's parts are thin-form draws of a mini frame)
    // may do it. --wwe2k14_rdr_ring_d3 (the R2 record, registered in wwe2k14_app.h) is a separate switch and is REQUIRED here, never implied (16.8 N5): without the ring-rows record there is nothing behind a marker.
    options.ring_d3 = REXCVAR_GET(wwe2k14_rdr_ring_d3_live) && REXCVAR_GET(wwe2k14_rdr_ring_d3) && options.ring_thin && REXCVAR_GET(wwe2k14_scene_light_states) && REXCVAR_GET(wwe2k14_rdr_owner);
#if defined(__ANDROID__)
    options.ring_d3 = false;   // the ring-rows tracker is not registered on Android (wwe2k14_app.h: SafeCopy is not fault-safe there): no records, so no skip either
#endif
    options.ring_d3_verify = options.ring_d3 && REXCVAR_GET(wwe2k14_rdr_ring_d3_live_verify);
    // lane 246 review (A): each chain's run scan counts the OTHER chain's markers by its own arm, so mixed arms (one skips, one verifies) break both
    // chains' ordinals and latch them. With both live, the ring follows the arena's arm.
    if (options.ring_d3 && options.arena_d3 && options.ring_d3_verify != options.arena_d3_verify) {
      // lane 289 review S2: never PROMOTE a verify to a skip. Ring skip + arena verify: the ring follows into verify. Arena skip + ring verify: both go to verify (the arena's skip arm is switched back too).
      REXLOG_WARN("[rdr][d3] arena D3 and ring D3 live are both on in different arms: both run the verify arm");
      options.ring_d3_verify = true;
      options.arena_d3_verify = true;
    }
    // lane 289: the ring SKIP arm is allowed again (still default off: --wwe2k14_rdr_ring_d3_live without the verify switch). Lane 258 refused it until the live thin ordinals counted a skipped call's draws; they do now: the reader (TryD3Marker -> FrameLite::Reserve)
    // and the binder (ThinBeginFrame -> ComputeKeys fill) both add the marker's part triples, published with the mark set, at the marker's position (SKIP_GAME_WORK_MAP 16.9 lane 289).
    if (REXCVAR_GET(wwe2k14_rdr_ring_d3_live) && !options.ring_d3) REXLOG_WARN("[rdr][d3] --wwe2k14_rdr_ring_d3_live needs --wwe2k14_rdr_ring_d3 (the record), --wwe2k14_rdr_ring_ours, --wwe2k14_rdr_ring_thin, --wwe2k14_scene_records, --wwe2k14_scene_light_states and owner mode (not on Android): the ring skip is off");
    // lane 245 (Wrestlers D3 live seam, SKIP_GAME_WORK_MAP.md 18.9): a skipped wrestler colour call removes the wrestler's draws from the game's stream, so ONLY the owner (our renderer draws everything) with the wrestlers drawn by us
    // (their thin draws are the marker's parts: wrestlers_ours + wrestlers_thin) may do it; the records it reads (light states, the caster pass entries, the sphere rows) register with the switch (wwe2k14_app.h). Android registers none.
    options.wrestlers_d3 = REXCVAR_GET(wwe2k14_rdr_wrestlers_d3) && options.wrestlers_ours && options.wrestlers_thin && REXCVAR_GET(wwe2k14_scene_light_states) && REXCVAR_GET(wwe2k14_rdr_owner);
#if defined(__ANDROID__)
    options.wrestlers_d3 = false;   // nothing is registered on Android (wwe2k14_app.h: SafeCopy is not fault-safe there): no records, so no skip either
#endif
    options.wrestlers_d3_verify = options.wrestlers_d3 && REXCVAR_GET(wwe2k14_rdr_wrestlers_d3_verify);
    if (REXCVAR_GET(wwe2k14_rdr_wrestlers_d3) && !options.wrestlers_d3) REXLOG_WARN("[rdr][d3] --wwe2k14_rdr_wrestlers_d3 needs --wwe2k14_rdr_wrestlers_ours, --wwe2k14_rdr_wrestlers_thin, --wwe2k14_scene_records, --wwe2k14_scene_light_states and owner mode: the skip is off (the records may still be taken)");
    if (REXCVAR_GET(wwe2k14_rdr_caster_d3) && !options.caster_d3) REXLOG_WARN("[rdr][d3] --wwe2k14_rdr_caster_d3 needs --wwe2k14_rdr_shadow_casters_ours, --wwe2k14_rdr_rim_ours, --wwe2k14_scene_records, --wwe2k14_scene_light_states and owner mode: the skip is off (the tracker may still record)");
    if (REXCVAR_GET(wwe2k14_rdr_circles_ours) && !REXCVAR_GET(wwe2k14_scene_records)) REXLOG_WARN("[rdr][circles] --wwe2k14_rdr_circles_ours needs --wwe2k14_scene_records: off");  // lane 136
    options.circles_ours = REXCVAR_GET(wwe2k14_rdr_circles_ours) && REXCVAR_GET(wwe2k14_scene_records);  // lane 136
    if (REXCVAR_GET(wwe2k14_rdr_plane_shadow_ours) && !REXCVAR_GET(wwe2k14_scene_records)) REXLOG_WARN("[rdr][plane_shadow] --wwe2k14_rdr_plane_shadow_ours needs --wwe2k14_scene_records: off");  // lane 139
    options.plane_shadow_ours = REXCVAR_GET(wwe2k14_rdr_plane_shadow_ours) && REXCVAR_GET(wwe2k14_scene_records);  // lane 139
    options.small_passes_ours = REXCVAR_GET(wwe2k14_rdr_small_passes_ours);  // lane 135 (no scene record needed)
    options.rim_chain_ours = REXCVAR_GET(wwe2k14_rdr_rim_chain_ours);  // lane 151 (no scene record needed)
    options.particles_ours = REXCVAR_GET(wwe2k14_rdr_particles_ours);  // S101 lane 67 (no scene record needed)
    options.shadows_ours = REXCVAR_GET(wwe2k14_rdr_shadows_ours);  // S101 lane 123 (SH2; no scene record needed)
    options.copy_alias = REXCVAR_GET(wwe2k14_rdr_copy_alias), options.copy_alias_classes = uint32_t(REXCVAR_GET(wwe2k14_rdr_copy_alias_classes));  // S101 lane 190
    if (options.copy_alias >= 2 && (options.copy_alias_classes & 1u) && REXCVAR_GET(wwe2k14_rdr_crowd_bake_skip) >= 2) {  // S1: a skipped bake's cards read the last bake's destination: an elided copy never wrote it
      REXLOG_WARN("[rdr][copy_alias] the crowd bake class is exclusive with --wwe2k14_rdr_crowd_bake_skip=2: the bake class is turned off");
      options.copy_alias_classes &= ~1u;
    }
    options.shadow_map_size = uint32_t(REXCVAR_GET(wwe2k14_rdr_shadow_map_size)), options.shadow_plane_percent = uint32_t(REXCVAR_GET(wwe2k14_rdr_shadow_plane_scale));
    if (REXCVAR_GET(wwe2k14_rdr_ring_ours) && !REXCVAR_GET(wwe2k14_scene_records)) REXLOG_WARN("[rdr][ring] --wwe2k14_rdr_ring_ours needs --wwe2k14_scene_records: off");
    if (REXCVAR_GET(wwe2k14_rdr_arena_ours) && !REXCVAR_GET(wwe2k14_scene_records)) REXLOG_WARN("[rdr][arena] --wwe2k14_rdr_arena_ours needs --wwe2k14_scene_records: off");
    if (REXCVAR_GET(wwe2k14_rdr_wrestlers_ours) && !REXCVAR_GET(wwe2k14_scene_records)) REXLOG_WARN("[rdr][wrestlers] --wwe2k14_rdr_wrestlers_ours needs --wwe2k14_scene_records: off");
    options.crowd_game_root = []() -> std::string { return g_crowd_game_root; };
    options.announced_arena = []() -> int { return REXCVAR_GET(wwe2k14_rdr_announce_arena) ? wwe2k14::g_announced_arena.load(std::memory_order_relaxed) : -1; };   // S101 lane 271
    options.on_d3_latch = [](const char* line) { if (auto f = wwe2k14::g_d3_latch_hook) f(line); };   // lane 301: the case ring takes a zip at a D3 latch (d3_latch_hook.h)
    if (REXCVAR_GET(wwe2k14_rdr_crowd_ours) && !REXCVAR_GET(wwe2k14_scene_records)) REXLOG_WARN("[rdr][crowd] --wwe2k14_rdr_crowd_ours needs --wwe2k14_scene_records: off");
    if (REXCVAR_GET(wwe2k14_rdr_crowd_cards) && !options.crowd_ours) REXLOG_WARN("[rdr][crowd] --wwe2k14_rdr_crowd_cards needs --wwe2k14_rdr_crowd_ours (and --wwe2k14_scene_records): off");  // S101 lane CD
    if (REXCVAR_GET(wwe2k14_rdr_crowd_floor) && !options.crowd_ours) REXLOG_WARN("[rdr][crowd] --wwe2k14_rdr_crowd_floor needs --wwe2k14_rdr_crowd_ours (and --wwe2k14_scene_records): off");
    // START-ONLY-END
    // (settings parity S3: film grain is PlayerQuality::film_grain, read by the binder and the scene draw; not a LiveOptions field any more)
    {  // Settings parity S10b: DLSS / DLAA from the launcher's UPSCALING (src\dlaa.h: --wwe2k14_dlss + _mode, --wwe2k14_dlaa; the runtime
       // folder it found, our own switch in owner mode so the SDK's idle device never loads Streamline).
      const auto flag = [](const char* name) { return rex::cvar::GetFlagInfo(name) ? rex::cvar::GetFlagByName(name) : std::string(); };
      const bool dlss = flag("wwe2k14_dlss") == "true" && flag("wwe2k14_fsr") != "true", dlaa = flag("wwe2k14_dlaa") == "true";
      const std::string dir = flag("wwe2k14_rdr_streamline_dir");
      if ((dlss || dlaa) && !dir.empty()) {
        // START-ONLY(ON): DLSS / DLAA is not in the stream-free build (ApplyStartSettings: RefuseSetting names it)
        const std::string mode = flag("wwe2k14_dlss_mode");
        options.dlss_mode = !dlss ? 0 : mode == "quality" ? 3 : mode == "ultra_performance" ? 4 : mode == "balanced" ? 2 : 1;
        options.streamline_dir = dir;
        g_jitter = JitterState{};
        g_jitter.on = true;
        g_jitter.legacy = REXCVAR_GET(wwe2k14_rdr_dlss_jitter_legacy);  // lane 185
        options.dlss_jitter_legacy = g_jitter.legacy;  // lane 185: the binder's wrestler velocity window follows (live.cpp)
        g_jitter.phases = options.dlss_mode == 0 ? 8 : options.dlss_mode == 4 ? 72 : options.dlss_mode == 3 ? 18 : 32;
        g_jitter.scale = options.render_scale;
        options.dlss_phases = g_jitter.phases;
        // The scene's textures sampled for the output size (NVIDIA's rule, log2(render / output); the old path's gpu_dlss_lod_bias).
        g_jitter.lod_bias = options.dlss_mode == 0 ? 0.0f : options.dlss_mode == 4 ? -1.58f : options.dlss_mode == 3 ? -0.58f : -1.0f;
        REXLOG_INFO("[rdr][settings] DLSS: mode {} (0 DLAA), {} jitter phases, texture bias {}, runtime {}", options.dlss_mode, g_jitter.phases,
                    g_jitter.lod_bias, dir);
      } else if (dlss || dlaa) {
        REXLOG_WARN("[rdr][settings] DLSS / DLAA asked for, but no NVIDIA runtime folder: off");
      }
    }
    // Photo mode's HUD hiding (the SDK's gpu_hide_hud, set live by src\photo_mode.h) in the new renderer (binder.h g_hide_hud).
    if (rex::cvar::GetFlagInfo("gpu_hide_hud")) {
      g_live.SetHideHud(rex::cvar::GetFlagByName("gpu_hide_hud") == "true");
      rex::cvar::RegisterChangeCallback("gpu_hide_hud", [](std::string_view, std::string_view value) { g_live.SetHideHud(value == "true"); });
    }
    // Settings parity S10a: the SDK's present_effect=sgsr (the phone's SGSR UPSCALING, the PC's LOW tier) is our present's SGSR.
    options.present_sgsr = rex::cvar::GetFlagInfo("present_effect") && rex::cvar::GetFlagByName("present_effect") == "sgsr";
    // Settings parity S9: the launchers' 30 fps mode (--wwe2k14_step_hz=30, gfx_settings.h / mobile_settings.h frame_rate 3) ran the game
    // at 30 updates a second, so match timings were off (the timer at half rate). In owner mode it is decoupled: the game keeps its own
    // 60 updates (step_hz back to 0; read live by step_rate.h) and the new renderer draws every other 60-a-second frame.
    if (rex::cvar::GetFlagInfo("wwe2k14_step_hz") && rex::cvar::GetFlagByName("wwe2k14_step_hz") == "30") {
      rex::cvar::SetFlagByName("wwe2k14_step_hz", "0");
      rex::cvar::SetFlagByName("wwe2k14_step_hz_entrance", "0");
      options.draw_vblanks = 2;
      REXLOG_INFO("[rdr][settings] 30 fps decoupled: the game keeps its 60 updates a second, the new renderer draws one picture per two vblanks");
    }
    // Settings parity S9b: the launchers' 120 / high-refresh mode (--wwe2k14_step_hz=N above 60 with --wwe2k14_step_vblank, gfx_settings.h
    // frame_rate 2) ran the game at N updates. In owner mode it is decoupled: the game keeps its 60 and the sink draws each tick twice, halfway
    // from the previous one and then its own (frame_blend.h), so a 120 Hz display gets 120 pictures. Needs V-SYNC (FIFO) to space them.
    if (rex::cvar::GetFlagInfo("wwe2k14_step_hz") && std::atoi(rex::cvar::GetFlagByName("wwe2k14_step_hz").c_str()) > 60) {
      const std::string was = rex::cvar::GetFlagByName("wwe2k14_step_hz");
      rex::cvar::SetFlagByName("wwe2k14_step_hz", "0");
      rex::cvar::SetFlagByName("wwe2k14_step_hz_entrance", "0");
      rex::cvar::SetFlagByName("wwe2k14_step_vblank", "false");
      // lane 168 (GATE_FAULTS row 4): the pump rate follows what the display shows. 120 on a 60 Hz panel pumps at 60 (no halfway pictures,
      // no 120 Hz present spacing); the setting itself is left as the player chose it. rdr_adapter::rate::Decide (display_rate.h).
      const bool follow = REXCVAR_GET(wwe2k14_pump_follow_display);
      const double panel_hz = rdr_adapter::rate::PanelHz(options.window, REXCVAR_GET(wwe2k14_panel_peak_hz));
      auto decision = rdr_adapter::rate::Decide(follow ? panel_hz : 0.0, std::atoi(was.c_str()));
      if (!follow) decision.why = "--wwe2k14_pump_follow_display=false: request kept";
      REXLOG_INFO("[pump] display refresh {:.2f} Hz{}, requested {} fps -> pumping at {} ({})", decision.panel_hz, decision.panel_known ? "" : " (unknown)",
                  was, decision.pump_hz, decision.why);
      if (decision.tween) {
        options.tween = true;
        options.present_hz = 120;  // S100 phone 120 fix 2: the presents spaced 1/120 s, not by the phone's (wrong, 60 Hz) refresh reading
        rex::cvar::SetFlagByName("wwe2k14_rdr_display_hz", "120");  // S9b fix: the phone asks its screen for 120 Hz (android_frame_rate.h)
        REXLOG_INFO("[rdr][settings] {} fps decoupled: the game keeps its 60 updates a second, the new renderer draws a halfway picture between ticks", was);
      } else {
        REXLOG_INFO("[rdr][settings] {} fps asked, the display shows {:.2f} Hz: the game keeps its 60 updates a second, no halfway pictures", was, decision.panel_hz);
      }
    }
    {  // settings parity S6 / S7 / S8: the SDK's switches, which the launchers set (gfx_settings.h GraphicsFlags, mobile_settings.h)
      // S103 SPB: PlayerQuality (start_settings.cpp) read the SDK flags once; the stream-free scene draw reads the same struct
      // (S103 SPB review L7: LiveOptions no longer carries crowd_density / shadows / depth_of_field / shadow_half / film_grain: nothing read them; the binder reads PlayerQuality)
      options.density_track = applied.quality.density_track;  // S101 lane 234
      REXLOG_INFO("[rdr][settings] crowd density {}%, shadows {}{}, depth of field {}", applied.quality.crowd_density, applied.quality.shadows ? "on" : "off",
                  applied.quality.shadow_half ? " (every other frame)" : "", applied.quality.depth_of_field ? "on" : "off");
    }
    {  // S102 lane G1: LOW's levers (low_levers.h), each behind its own switch, default off
      const bool low = applied.quality.low_tier;   // S103 SPB: low_levers::IsLow, resolved once by ApplyStartSettings (which also stored rdr::g_low_tier)
      options.low_tier = low;   // S102 lane G3
      const uint32_t size_was = options.shadow_map_size;
      options.shadow_map_size = rdr_adapter::low_levers::ShadowMapSize(REXCVAR_GET(wwe2k14_rdr_low_shadow_small), low, options.shadows_ours, options.shadow_map_size);
      options.prelight_compute = REXCVAR_GET(wwe2k14_rdr_prelight_compute);   // S102 L1b review F2: the start-time value
      options.prelight_layered = REXCVAR_GET(wwe2k14_rdr_prelight_layered);   // S102 C1 review F2: the start-time value (the device enables multiview only then)
      options.robust_buffers = applied.robust_buffers;   // S102 D1 (StartOnly: the device is made once; S103 SPB: start_settings.cpp)
      options.exclusive_fullscreen = applied.exclusive_fullscreen;  // S102 G2b (StartOnly: the swapchain is built once; Windows only, start_settings.cpp)
      // S102 G2: rdr::g_low_tier (the two output levers are PerFrame, live.cpp reads the S0-4 table, but LOW is the tier resolved at start) is stored by ApplyStartSettings (S103 SPB)
      options.present_noclear = applied.present_noclear;   // low_levers::PresentNoClear(wwe2k14_rdr_low_present_noclear, low), start_settings.cpp
      const rdr_adapter::low_levers::CopyAlias alias = rdr_adapter::low_levers::CopyAliasFor(REXCVAR_GET(wwe2k14_rdr_low_no_fullsize_copies), low, options.copy_alias, options.copy_alias_classes);
      options.copy_alias = alias.mode, options.copy_alias_classes = alias.classes;
      // START-ONLY(ON): the copy-alias class mask (the stream's resolve copies) has no twin: the stream-free build has no resolve copies
      if (options.copy_alias >= 2 && (options.copy_alias_classes & 1u) && REXCVAR_GET(wwe2k14_rdr_crowd_bake_skip) >= 2) options.copy_alias_classes &= ~1u;  // as the plain switch (above)
      if (REXCVAR_GET(wwe2k14_rdr_low_shadow_small) && low && !options.shadows_ours) REXLOG_WARN("[rdr][low] --wwe2k14_rdr_low_shadow_small needs --wwe2k14_rdr_shadows_ours (the maps are the stream's otherwise): off");
      REXLOG_INFO("[rdr][low] LOW {}: shadow maps {} (was {}), present clear {}, copy alias mode {} classes 0x{:X}", low ? "yes" : "no", options.shadow_map_size, size_was,
                  options.present_noclear ? "bars only" : "whole panel", options.copy_alias, options.copy_alias_classes);
    }
    {  // settings parity S5: the screen shape (the SDK's cvars, set by ultrawide.h)
      const auto num = [](const char* name, double fallback) {
        const std::string v = rex::cvar::GetFlagInfo(name) ? rex::cvar::GetFlagByName(name) : std::string();
        return v.empty() ? fallback : std::strtod(v.c_str(), nullptr);
      };
      g_wide = WideState{};
      g_wide.wide = applied.wide;   // S103 SPB: the shape factors are read (and range-checked) once, by ApplyStartSettings
      g_wide.tall = applied.tall;
      // START-ONLY-BEGIN(ON): the stream squeezes the 360's draws by the screen-shape factors (the wide-view classes below); the stream-free scene draw has no such classes (SP2 M6: present_fill is gated, see host_start.cpp kSceneFollowsWide)
      g_wide.min_width = uint32_t(std::max(0.0, num("gpu_wide_min_width", 1200)));
      g_wide.legacy = REXCVAR_GET(wwe2k14_rdr_wide_legacy);  // lane 171
      g_wide.no_full_view = REXCVAR_GET(wwe2k14_rdr_wide_no_fullview);  // lane 177
      if (rex::cvar::GetFlagInfo("gpu_dlss_hud_after_vs"))
        g_wide.hud_after_vs = std::strtoull(rex::cvar::GetFlagByName("gpu_dlss_hud_after_vs").c_str(), nullptr, 16);
      if (rex::cvar::GetFlagInfo("gpu_wide_post_vs")) {
        const std::string list = rex::cvar::GetFlagByName("gpu_wide_post_vs");
        for (size_t at = 0; at < list.size();) {
          const size_t comma = std::min(list.find(',', at), list.size());
          if (comma > at) g_wide.post_vs.push_back(std::strtoull(list.substr(at, comma - at).c_str(), nullptr, 16));  // verbatim (lane 186: the SDK default no longer lists the T_Rect VS 032E8FAC979E01DA; listing it again = the old behaviour, the live A/B)
          at = comma + 1;
        }
      }
      // START-ONLY-END
      options.present_fill = applied.present_fill;   // S103 SPB: start_settings.cpp (== g_wide.on())
      if (g_wide.on()) options.shown_wide = g_wide.wide, options.shown_tall = g_wide.tall;  // lane 201: photo mode saves the picture as shown (stretched back)
      if (g_wide.on())
        REXLOG_INFO("[rdr][settings] screen shape: wide {:.3f} tall {:.3f} (scene draws at least {} wide, HUD after {:016X}, {} post passes left alone)",
                    g_wide.wide, g_wide.tall, g_wide.min_width, g_wide.hud_after_vs, g_wide.post_vs.size());
    }
    REXLOG_INFO("[rdr][settings] render scale {}, texture filtering override {}, film grain {}, v-sync {}, sgsr {}", options.render_scale, g_aniso_override,
                applied.quality.film_grain ? "on" : "off", options.vsync ? "on" : "off", options.present_sgsr ? "on" : "off");
    if (!options.window) REXLOG_ERROR("[rdr][live] owner mode: no game window found; the new renderer runs headless");
  }
  std::string error;
  // S102 U1: --wwe2k14_rdr_test_fail_start makes this start fail with the plain sentence the real failures use (dev builds only; the offline twin is rplay --gpu 99).
  const bool forced_fail = kDevBuild && REXCVAR_GET(wwe2k14_rdr_test_fail_start);
  if (forced_fail) error = "This graphics hardware is below the minimum. Missing: (test: --wwe2k14_rdr_test_fail_start).";
  if (forced_fail || !g_live.Start(options, error)) {
    REXLOG_ERROR("[rdr][live] could not start: {}", error);
    REXLOG_ERROR("[rdr][verdict] FAILED to start: {} (card asked for: '{}')", error, options.gpu_name);  // S102 U1: the one line a report reads first
    wwe2k14::blackbox::Note("FAILED: the new renderer could not start: %s", error.c_str());
    // START-ONLY(ON): the stream build's per-frame mode pointer (the stream-free build's mode is a constant, scene_frame.h)
    scene_frame::g_mode_cvar.store(nullptr, std::memory_order_relaxed);   // S103 SPB review L2: ApplyStartSettings stored it before Start; a failed start leaves no mode pointer (the game side reads "off")
    wwe2k14::start::RearmStartSettings();   // (L3) a later start reads the flags again
    REXCVAR_SET(wwe2k14_rdr_live, false);
    ShowStartFailure(error, options.gpu_name);
    return;
  }
  g_cap.geo.clear();
  g_geo_used.clear();
  g_cap.geo.reserve(kGeoReserve);   // lane 273
  g_geo_used.reserve(kGeoReserve);
  g_geo_expiry.Clear();
  ++g_geo_forget_epoch, ++g_geo_clear_epoch;
  g_live_sent.clear();
  ResetGeoMemoForNewStore();  // S103 IXF: as StartCapture
  wwe2k14::frame_tick::TheCurve().RepostIfKnown();  // a table set before the renderer started (the game sets it at boot); S103 P6: kept in frame_tick
  for (Image& img : g_reg.images) {
    img.content_written = false;
    img.checked_frame = ~0ull;
    img.written_in_capture = false;
  }
}

// ---- targets ------------------------------------------------------------------------------------------------------------------------
inline void EmitTargets(uint8_t* base, uint32_t dev) {
  FrameTables& t = g_frame;
  uint32_t now[5];
  bool same = t.targets_valid;
  for (uint32_t i = 0; i < 5; ++i) {
    now[i] = db::Ld32(base, dev + kDevRt0 + 4 * i);
    same = same && now[i] == t.last_targets[i];
  }
  if (same) return;
  std::memcpy(t.last_targets, now, sizeof(now));
  t.targets_valid = true;
  rdr::TargetsOp op;
  for (uint32_t i = 0; i < 4; ++i) {
    if (!now[i]) continue;
    op.color[i].image = rdr::ImageId(SurfaceOfObject(base, now[i], false));
    op.color_count = i + 1;
    if (i) Note("mrt_bound");
  }
  if (now[4]) op.depth.image = rdr::ImageId(SurfaceOfObject(base, now[4], true));
  t.ops.push_back({cap::OpKind::SetTargets, uint32_t(t.targets.size())});
  t.targets.push_back(op);
}
