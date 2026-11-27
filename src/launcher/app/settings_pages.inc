
  void SettingsGame() {
    SubHeading("GAME FILES");
    GameDataSection(false);
    ImGui::Dummy(ImVec2(0, 8 * Dpi()));
    // Session 53 (a playtester's request; Jarrod: "a toggle in the game settings").
    SubHeading("STARTING THE GAME");
    OptionToggle("QUICK START", s.quick_start, true,
                 "Opening the launcher starts the game straight away with these settings (3 seconds to press a key or "
                 "click to stay; hold Shift while opening the launcher to skip it). The launcher closes with the game.");
    if (ImGui::Button("Make a desktop shortcut")) CreateDesktopShortcut();
    ImGui::SameLine();
    ImGui::TextDisabled("It starts the game directly with these settings (so does wwe2k14.exe itself).");
    ImGui::Dummy(ImVec2(0, 8 * Dpi()));
    SubHeading("DOWNLOADABLE CONTENT");
    DlcSection();
    ImGui::Dummy(ImVec2(0, 8 * Dpi()));
    int li = 0;
    for (int i = 0; i < int(std::size(kLanguageIds)); ++i)
      if (kLanguageIds[i] == s.language) li = i;
    const int auto_id = AutoLanguage();
    std::string note = "The game's menus and text. Automatic follows Windows (now: ";
    for (int i = 1; i < int(std::size(kLanguageIds)); ++i)
      if (kLanguageIds[i] == auto_id) note += kLanguageNames[i];
    note += "). The game has these five.";
    if (OptionRow("LANGUAGE", li, kLanguageNames, int(std::size(kLanguageNames)), true, note.c_str()))
      s.language = kLanguageIds[li];
    ImGui::Dummy(ImVec2(0, 8 * Dpi()));
    SubHeading("IN THE GAME");
    OptionToggle("CINECAM", s.cinecam, true, "Free camera: F8 in a match.");
    OptionToggle("RUN-INS", s.runin, true, "F9 in a 1-on-1 match: another wrestler interferes (experimental).");
    OptionToggle("PHOTO MODE", s.photo_mode, true,
                 "F10 (or both sticks clicked, held) in a match: the scene nearly stops, a free camera, no HUD, depth "
                 "of field; A / Enter saves a PNG to Pictures\\WWE 2K14.");
    OptionToggle("RAMP CAMERA", s.ramp_camera, true,
                 "The match camera from the side facing the stage. In a match: pause, VISUAL PRESENTATION, RAMP CAMERA.");
    OptionToggle("PERMANENT BLOOD", s.permanent_blood, true,
                 "A mod: the blood that drips on the mat and the floor stays for the whole match instead of fading after "
                 "about 10 seconds. Needs the game's blood option on.");
    // Session 33: Discord status (the Discord app on this PC; only the wrestlers, the match type and the time).
    OptionToggle("DISCORD STATUS", s.discord, true,
                 "Your Discord status shows the match: the wrestlers, the match type and the time (\"In the menus\" "
                 "otherwise). Needs the Discord app running on this PC. Nothing else is sent.");
    if (s.discord && !*kDiscordAppId) {
      ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 14 * Dpi());
      ImGui::SetNextItemWidth(320 * Dpi());
      ImGui::InputTextWithHint("##discord_app", "Discord application id", &s.discord_app_id);
      if (s.discord_app_id.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(kWarn, "No application id yet: nothing is sent.");
      }
    }
    bool pr = s.perf_report < 0 ? g_perf_report_default : s.perf_report == 1;
    if (OptionToggle("PERFORMANCE REPORT", pr, true,
                     "Frame and load times per scene when the game closes; it goes into Report a problem."))
      s.perf_report = pr ? 1 : 0;
    // Session 70: src\precompile.h, PrecompileKey above.
    // S105 B3: back in the stream-free build: the boot builds the player's own stored pipeline list of the new renderer (src\precompile.h owner mode, "prepared N of M"),
    // so no pipeline is made cold at the first match frames. (A1b had hidden it while that boot made 0 pipelines: the catalog was never registered.)
    OptionToggle("PREPARE SHADERS", s.precompile, true,
                 "Before playing, build the graphics for this PC once (after an update, a new driver or a graphics "
                 "change), so entrances don't stutter. Takes a minute or less; SKIP is always there.");
    if (ImGui::Button("Prepare again on the next Play")) {
      std::error_code ec;
      fs::remove(PrecompileStampPath(), ec);
      Status("The shaders will be prepared again on the next Play");
    }
    ImGui::Dummy(ImVec2(0, 8 * Dpi()));
    SubHeading("UPDATES");
    OptionToggle("CHECK FOR UPDATES", s.check_updates, true,
                 "When the launcher starts it asks GitHub whether a newer full release is out (nothing else is sent).");
    if (ImGui::Button("Check now")) StartUpdateCheck(true);
    ImGui::SameLine();
    const UpdateInfo u = Update();
    switch (u.state) {
      case UpdateInfo::kChecking: ImGui::TextDisabled("Checking..."); break;
      case UpdateInfo::kLatest: ImGui::TextColored(kGood, "You have the latest version (%s).", MyVersion().c_str()); break;
      case UpdateInfo::kAvailable:
        ImGui::TextColored(kWarn, "%s is out.", u.tag.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Details")) want_update_popup_ = true, update_dismissed_ = false;
        break;
      case UpdateInfo::kFailed: ImGui::TextColored(kBad, "Couldn't check: %s.", u.error.c_str()); break;
      default: ImGui::TextDisabled("Not checked yet (%s).", MyVersion().c_str()); break;
    }
  }

  void SettingsDisplay() {
    const float d = Dpi();
    // Graphics card: automatic (the game picks the dedicated GPU: src\gpu_pick.h) or one by name.
    if (gpus_.empty()) gpus_ = ListGpus();
    std::vector<std::string> names{"AUTOMATIC"};
    for (const Gpu& g : gpus_) names.push_back(g.name);
    int gi = 0;
    for (size_t i = 0; i < gpus_.size(); ++i)
      if (gpus_[i].name == s.gpu) gi = int(i) + 1;
    std::vector<const char*> ptrs;
    for (const auto& n : names) ptrs.push_back(n.c_str());
    const std::string gnote = gpus_.empty() ? std::string("No graphics card found.")
                                            : "Automatic: the dedicated graphics card with the most memory (here: " +
                                                  gpus_.front().name + ").";
    if (OptionRow("GRAPHICS CARD", gi, ptrs.data(), int(ptrs.size()), true, gnote.c_str()))
      s.gpu = gi == 0 ? std::string() : gpus_[size_t(gi - 1)].name;

    static constexpr const char* kMode[] = {"FULLSCREEN", "WINDOWED"};
    int mode = s.windowed ? 1 : 0;
    if (OptionRow("DISPLAY", mode, kMode, 2)) s.windowed = mode == 1;

    // Session 33: ultrawide. AUTO = the monitor's shape: wider than 16:9 shows more of the scene at the sides; the
    // HUD and menus stay a centred 16:9.
    static constexpr const char* kAspect[] = {"16:9", "AUTO"};
    const double mon = hw_.screen_h ? double(hw_.screen_w) / double(hw_.screen_h) : 16.0 / 9.0;
    char anote[200];
    std::snprintf(anote, sizeof(anote), "%s Your monitor: %dx%d (%.2f:1).",
                  s.aspect == 1 ? "The monitor's shape: more of the arena at the sides on an ultrawide; HUD and menus "
                                  "stay 16:9."
                                : "The game's own 16:9.",
                  hw_.screen_w, hw_.screen_h, mon);
    s.aspect = std::clamp(s.aspect, 0, 1);
    OptionRow("ASPECT", s.aspect, kAspect, 2, true, anote);

    // Session 93 (Jarrod: the low-end tier is chosen automatically from the GPU class, with a launcher override).
    {
      static constexpr const char* kTier[] = {"AUTO", "LOW", "STANDARD"};
      const wwe2k14::vkdev::Device tdev = ChosenDevice(s.gpu);
      std::string why;
      const int tier = wwe2k14::gfxs::ResolveQualityTier(s.quality_tier, false, tdev.integrated(), tdev.name, &why);
      s.quality_tier = std::clamp(s.quality_tier, 0, 2);
      // (LOW does nothing with the original look or a LIGHT / ACCURATE override: say STANDARD then.)
      const int rend = wwe2k14::gfxs::RendererFromFlags(s.extra);
      if (tier == 1 && s.original_look) why = "STANDARD (original look is on)";
      else if (tier == 1 && rend >= 0 && rend != 2) why = "STANDARD (not the NATIVE renderer)";
      std::string tnote =
          "AUTO picks LOW on integrated graphics and phones. Choosing LOW sets RESOLUTION to 720p and CROWD DENSITY to 70% "
          "once (if they were at 1440p / 100%); after that your own settings always win. LOW also sharpens a picture "
          "drawn below your screen's size (SGSR). STANDARD is the normal look. Now: " + why + ".";
      const int tier_before = s.quality_tier;
      if (OptionRow("QUALITY", s.quality_tier, kTier, 3, true, tnote.c_str())) {
        if (s.quality_tier == 1 && tier_before != 1 && !s.original_look && (rend < 0 || rend == 2)) {
          // Session 95 (Jarrod: "they start with our safe low defaults"): choosing LOW writes its starting values into the sliders, once,
          // visibly; the values they had are kept to give back if LOW is left with the sliders still showing what it wrote.
          low_prev_ = {s.resolution, s.draw_height, s.crowd_density};
          std::string what;
          low_wrote_ = wwe2k14::gfxs::LowStartValues(s.resolution, s.draw_height, s.upscale, s.crowd_density, &what);
          low_after_ = {s.resolution, s.draw_height, s.crowd_density};
          Log("settings: LOW chosen: wrote the starting values:" + what);
        } else if (tier_before == 1 && s.quality_tier != 1 && low_wrote_) {
          if (std::array<int, 3>{s.resolution, s.draw_height, s.crowd_density} == low_after_) {
            s.resolution = low_prev_[0], s.draw_height = low_prev_[1], s.crowd_density = low_prev_[2];
            Log("settings: LOW left: the values LOW wrote are given back");
          }
          low_wrote_ = false;
        }
      }
    }

    const bool dlss = !kStreamFreeTarget && ((s.upscale >= 2 && s.upscale <= 4) || s.upscale == 6 || s.upscale == 7);  // S33: or FSR Q / P (S105 A1b: not sent in the stream-free build)
    static constexpr const char* kRes[] = {"720P", "1440P", "4K"};
    const int sug = SuggestResolution(hw_);
    char rnote[200];
    std::snprintf(rnote, sizeof(rnote), "%sYour monitor: %dx%d at %d Hz; the default is %s, raise it if your PC has room.",
                  s.original_look ? "Original look is on (Advanced): 720p. "
                  : dlss          ? "The upscaler picks the size itself (Graphics). "
                                  : "",
                  hw_.screen_w, hw_.screen_h, hw_.refresh_hz, kRes[sug]);
    ResolutionRow(s, !dlss && !s.original_look, rnote);

    FrameRateRow(s);
    // Session 100 settings parity S13 (Jarrod 2026-11-11 08:1x: "vsync should get an explicit toggle"): how pictures reach the screen,
    // never the game's speed (--wwe2k14_vsync; gfx_settings.h). The new renderer reads it; the current one keeps its own present modes.
    OptionToggle("V-SYNC", s.vsync, true,
                 "On: every picture waits for the screen, no tearing. Off: pictures go out as soon as they are ready (less delay, "
                 "some tearing). The game's speed is the same either way. Applies with the new renderer.");
    ImGui::Dummy(ImVec2(0, 10 * d));
    ImGui::TextDisabled("In the game: F3 shows the frame rate.");
  }

  // Session 54 (Jarrod, 2026-10-23: "360p, 480, 540p, 720p, 1080p, 1440p, and 4k as resolution options"): 360p - 540p
  // and 1080p are drawn by NATIVE only (its passes at that size, its copies at 720p / 1440p: gfx_settings.h
  // kResolutions), so the other renderers and the upscalers offer the whole sizes, with a note.
  void ResolutionRow(Settings& t, bool enabled, const char* extra_note) {
    const bool native = DrawHeightApplies(t);
    std::vector<int> entries;  // kResolutions indices offered
    for (int i = 0; i < wwe2k14::gfxs::kResolutionCount; ++i)
      if (native || !wwe2k14::gfxs::kResolutions[i].native_only) entries.push_back(i);
    std::vector<const char*> names;
    for (int i : entries) names.push_back(wwe2k14::gfxs::kResolutions[i].name);
    const int current = wwe2k14::gfxs::ResolutionIndex(t.resolution, t.draw_height, native);
    int vi = 0;
    for (size_t k = 0; k < entries.size(); ++k)
      if (entries[k] == current) vi = int(k);
    const auto& e = wwe2k14::gfxs::kResolutions[entries[size_t(vi)]];
    std::string note = extra_note ? extra_note : "";
    if (enabled && native && e.draw_height) {
      char drawn[160];
      std::snprintf(drawn, sizeof(drawn), " Drawn at %dx%d and scaled to your screen%s.", (e.draw_height * 1280 + 719) / 720,
                    e.draw_height, e.draw_height < 720 ? ": less work for the graphics card, softer picture" : "");
      note += drawn;
    }
    if (enabled && !native)
      note += t.renderer != 2 ? " 360p to 648p and 1080p need the NATIVE renderer."
                              : " 360p to 648p and 1080p are not available with upscaling.";
    if (OptionRow("RESOLUTION", vi, names.data(), int(names.size()), enabled, note.c_str())) {
      const auto& n = wwe2k14::gfxs::kResolutions[entries[size_t(std::clamp(vi, 0, int(entries.size()) - 1))]];
      t.resolution = n.resolution, t.draw_height = n.draw_height;
    }
  }

  // Session 31 (Jarrod): FRAME RATE = 60 / 120 / UNCAPPED, 60 fps entrances by default. Stored in the fields the game's
  // own GRAPHICS menu shares (gfx_settings.h): 60 = frame_rate 1; 120 = high refresh (2) at 120 steps; UNCAPPED = high
  // refresh at the monitor's rate (refresh_hz 0). The game's speed is tied to its steps, so "uncapped" is as fast as
  // the monitor shows: a real uncapped rate fast-forwards the game (--vsync=false). CLASSIC (0: 30 fps entrances)
  // appears only when it was picked in the game's menu.
  void FrameRateRow(Settings& t) {
    // Session 37: "30" (frame_rate 3) for weak PCs, after UNCAPPED; CLASSIC (0) last when it was picked in the game.
    // Session 83 (Jarrod: re-label it): 30 runs matches in a mode the game only used for entrances (#63: no Rumble bell /
    // intro); EXPERIMENTAL until matches keep 60 updates and only draw every other frame (PLAYBOOK ideas).
    std::vector<const char*> names{"60", "120", "UNCAPPED", "30 (EXPERIMENTAL)"};
    int fi = t.frame_rate == 2 ? (t.refresh_hz == 120 ? 1 : 2) : t.frame_rate == 3 ? 3 : 0;
    if (t.frame_rate == 0) names.push_back("CLASSIC"), fi = 4;
    const int hz = hw_.refresh_hz;
    std::string note = fi == 0   ? "Entrances and matches at 60."
                       : fi == 1 ? "Entrances and matches at 120."
                       : fi == 2 ? "Matches at your monitor's rate (" + std::to_string(hz) + " Hz), entrances at up to 120."
                       : fi == 3 ? "Experimental, for weaker PCs: entrances and matches at 30. The game was made for 60 "
                                   "in matches, so some match timings can be off (the match timer counts at half rate, "
                                   "get-ups take longer). Use 60 if anything looks or sounds wrong."
                                 : "Entrances at 30, matches at 60 (picked in the game's own menu).";
    if ((fi == 1 || fi == 2) && hz <= 60) note += " Your monitor runs at " + std::to_string(hz) + " Hz: this needs a faster one.";
    if (OptionRow("FRAME RATE", fi, names.data(), int(names.size()), true, note.c_str())) {
      if (fi == 0) t.frame_rate = 1, t.refresh_hz = 0;
      else if (fi == 1) t.frame_rate = 2, t.refresh_hz = 120;
      else if (fi == 2) t.frame_rate = 2, t.refresh_hz = 0;
      else if (fi == 3) t.frame_rate = 3, t.refresh_hz = 0;
    }
  }

  // Session 37: ACCURATE (the Xbox 360's EDRAM in every pixel shader: the reference look, heavy on weak GPUs) or LIGHT
  // (the graphics card's own render targets with the Session 37 fixes: same picture in our checks; src\light_renderer.h).
  // A card without interlock (older AMD drivers / graphics; Session 54) cannot run ACCURATE: the SDK falls back to LIGHT's path, and
  // --wwe2k14_fbo_fallback_fix (default on, Jarrod 2026-10-20) applies LIGHT's fixes there.
  // Session 41 (low-spec step 2): NATIVE (2) = ACCURATE with the native renderer drawing the scene (the game's own
  // shaders on the card's own render targets, --gpu_native=2): on the playtester's UHD 620 +10-28% fps over ACCURATE.
  // It needs interlock like ACCURATE (the SDK turns it off by itself otherwise).
  // Session 51 (Jarrod, 2026-10-22: "native should just replace low ... it may be easy for playtesters to use a wrong
  // setting"; "NATIVE for everyone"): NATIVE is the default on every PC, ACCURATE and LIGHT are fallbacks, and the LOW
  // preset button is gone (resolution and frame rate are their own rows; integrated graphics still start at 720p / 30).
  // Session 61 (Jarrod, 2026-10-25: "remove the renderer option entirely, with light and accurate as advanced flags
  // only"): no RENDERER row - NATIVE always; --wwe2k14_renderer=light|accurate in Advanced -> Extra flags for
  // troubleshooting (launcher_core.h BuildFlags, gfx_settings.h RendererFromFlags).

  // S105 A1b: the UPSCALING row (DLAA / DLSS / FSR) and its NVIDIA files notes. Not in the stream-free build: the scene host has no upscaler
  // path (start_settings.cpp RefuseSetting "DLSS / DLAA" / "FSR"), so the row would choose a picture the game then refuses.
  void SettingsUpscaling() {
    if (kStreamFreeTarget) return;  // S105 A1b: hidden in the stream-free build (settings_reader_lint.py checks this guard)
    const float d = Dpi();
    // Shown in Jarrod's order (Session 31): DLAA, then DLSS from the fastest to the sharpest. Session 33: then AMD FSR 3
    // (native, quality, performance) on every card (AMD's runtime ships next to the game). kUpValue = the stored
    // s.upscale (0 off, 1 DLAA, 2 DLSS performance, 3 quality, 4 ultra performance, 5 FSR native, 6 FSR quality, 7 FSR
    // performance; launcher_core.h).
    static constexpr int kUpValue[] = {0, 1, 4, 2, 3, 5, 6, 7};
    static constexpr const char* kUp[] = {"OFF",          "DLAA",        "DLSS ULTRA PERFORMANCE", "DLSS PERFORMANCE",
                                          "DLSS QUALITY", "FSR NATIVE",  "FSR QUALITY",            "FSR PERFORMANCE"};
    static constexpr const char* kUpNote[] = {"No upscaling.", "NVIDIA anti-aliasing at your resolution.",
                                              "720p drawn, shown as 4K.", "720p drawn, shown as 1440p.",
                                              "1440p drawn, shown as 4K.", "AMD anti-aliasing at your resolution.",
                                              "AMD FSR 3: 1440p drawn, shown as 4K.",
                                              "AMD FSR 3: 720p drawn, shown as 1440p."};
    const bool rtx = hw_.rtx || force_rtx_;
    const bool fsr = wwe2k14::gfxs::HasFsrRuntime();
    // Session 37: the card the game will use (Display -> GRAPHICS CARD).
    const wwe2k14::vkdev::Device dev = ChosenDevice(s.gpu);
    // Session 37 (FSR never breaks a game; only where it works): no FSR on integrated graphics (AMD's FSR 3.1 list
    // starts at Radeon RX 5000 / GeForce RTX 20, no integrated GPUs; a playtester's UHD 620 shook and hung with it).
    // Every FSR mode on any monitor otherwise (Jarrod, 2026-10-20: a bigger output than the screen is a choice, like a
    // render scale above 100%). A saved choice stays visible.
    const bool fsr_card = fsr && !dev.integrated();
    std::vector<int> idx{0};
    if (rtx) idx.insert(idx.end(), {1, 2, 3, 4});
    if (fsr_card) idx.insert(idx.end(), {5, 6, 7});
    else if (fsr && wwe2k14::gfxs::IsFsr(s.upscale)) idx.push_back(s.upscale == 5 ? 5 : s.upscale == 6 ? 6 : 7);
    std::vector<const char*> names;
    int ui = 0;
    for (size_t i = 0; i < idx.size(); ++i) {
      names.push_back(kUp[idx[i]]);
      if (kUpValue[idx[i]] == s.upscale) ui = int(i);
    }
    if (idx.size() > 1) {
      std::string note = kUpNote[idx[size_t(ui)]];
      if (!rtx && ui == 0)
        note += " DLSS needs an NVIDIA RTX card (found: " + (hw_.gpu.empty() ? std::string("unknown") : hw_.gpu) + ").";
      if (wwe2k14::gfxs::IsFsr(s.upscale) && dev.integrated())
        note += " Not offered on integrated graphics (" + dev.name + "): FSR does not support it.";
      if (OptionRow("UPSCALING", ui, names.data(), int(names.size()), true, note.c_str())) {
        s.upscale = kUpValue[idx[size_t(ui)]];
        s.fsr_note.clear();  // a new choice: the old note no longer applies
      }
    } else {
      int off = 0;
      const std::string why = "DLSS needs an NVIDIA RTX card (found: " + (hw_.gpu.empty() ? std::string("unknown") : hw_.gpu) +
                              (!fsr ? "); FSR needs amd_fidelityfx_vk.dll next to the game (missing)."
                                    : "); FSR is not offered on integrated graphics (" + dev.name + ").");
      OptionRow("UPSCALING", off, names.data(), 1, false, why.c_str());
    }
    if (!s.fsr_note.empty()) {  // Session 37: the game turned FSR off on this PC (src\upscale_guard.h)
      ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 14 * d);
      ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x);
      ImGui::TextColored(kWarn, "Last time FSR could not run on this graphics card, so the game turned it off: %s",
                         s.fsr_note.c_str());
      ImGui::PopTextWrapPos();
    }
    if (rtx && wwe2k14::gfxs::IsNvidia(s.upscale)) {
      // Session 31 (Jarrod: the bundled files should just work): with NVIDIA's files next to the launcher (a release)
      // or in the project's Streamline SDK (a development build), nothing to set up; another folder is optional.
      const bool own = HasStreamline(s.dlss_dir), included = !paths_.nvidia.empty();
      ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 14 * d);
      if (included && !own && !nv_other_) {
        if (paths_.nvidia_bundled) ImGui::TextColored(kGood, "Using the NVIDIA DLSS files included with this build.");
        else ImGui::TextColored(kGood, "Using NVIDIA's DLSS files from the project's Streamline SDK (development build).");
        ImGui::SameLine();
        if (ImGui::SmallButton("Use other files...")) nv_other_ = true;
        if (!s.dlss_dir.empty())
          ImGui::TextDisabled("    Your folder %s has no NVIDIA files, so these are used.", s.dlss_dir.c_str());
      } else {
        ImGui::SetNextItemWidth(-110 * d);
        ImGui::InputTextWithHint("##sl", "folder with sl.interposer.dll (NVIDIA's Streamline runtime)", &s.dlss_dir);
        ImGui::SameLine();
        if (ImGui::Button("Browse...##sl", ImVec2(-1, 0)))
          if (auto f = PickFolder(hwnd_, L"Pick the folder with sl.interposer.dll")) s.dlss_dir = PathStr(*f);
        if (HasStreamline(s.dlss_dir)) ImGui::TextColored(kGood, "Using the NVIDIA files in this folder.");
        else if (included) ImGui::TextColored(kWarn, "No NVIDIA files in this folder: the included ones are used.");
        else {
          ImGui::TextColored(kWarn, "NVIDIA runtime folder not set.");
          Note("Where to get these files: DLSS needs NVIDIA's Streamline runtime, a free download from NVIDIA's "
               "developer site (the Streamline SDK; use its bin\\x64 folder).");
        }
        if (included && ImGui::SmallButton("Back to the included files")) s.dlss_dir.clear(), nv_other_ = false;
      }
      ImGui::Dummy(ImVec2(0, 6 * d));
    }
  }

  void SettingsGraphics() {
    SettingsUpscaling();  // S105 A1b (the stream build only)
    // Session 85 (Jarrod's order: UPSCALING, SHADOWS, CROWD DENSITY, DEPTH OF FIELD, POST EFFECTS, TEXTURE FILTERING).
    // The three are NATIVE-only flags (gfx_settings.h GraphicsFlags); crowd density is read when the game starts.
    OptionToggle("SHADOWS", s.shadows, true,
                 "Dynamic shadows from wrestlers, ropes and props. Turning them off is faster on weaker graphics cards; "
                 "the arena's baked lighting stays.");
    static constexpr const char* kCrowd[] = {"0%", "10%", "20%", "30%", "40%", "50%",
                                             "60%", "70%", "80%", "90%", "100%"};
    int crowd = std::clamp(wwe2k14::gfxs::RoundCrowdDensity(s.crowd_density) / 10, 0, 10);
    if (OptionRow("CROWD DENSITY", crowd, kCrowd, 11, true,
                  "How much of the crowd is drawn, spread evenly over the arena. 100% is the normal look; 0% is an empty "
                  "arena. Takes effect when the game starts."))
      s.crowd_density = crowd * 10;
    OptionToggle("DEPTH OF FIELD", s.depth_of_field, true,
                 "The blur on the background behind the wrestlers. Off also removes it from Photo mode.");
    // Session 72's POST EFFECTS row (ORIGINAL / ENHANCED) removed in Session 100 (Jarrod 2026-11-11 08:1x: "we should simply remove those").
    // Session 100 settings parity S3 (Jarrod 08:1x, a player's request), widened in S103 VTG2 (Jarrod 2026-11-18: one toggle for all): the retro
    // WrestleMania arenas' old-footage look (sepia + darkening + film grain + dark edges), --wwe2k14_film_grain (the key keeps its name; old files work).
    OptionToggle("VINTAGE LOOK (RETRO ARENAS)", s.film_grain, true,
                 "The old-footage look of the retro WrestleMania arenas (30 Years of WrestleMania and any match in those "
                 "arenas): sepia colour, darkening, film grain and dark edges. Off shows the arena clean.");
    // Session 33: anisotropic filtering (the game always had 4x forced by the runtime; 8x / 16x sharpen floors,
    // ramps and mats seen at an angle). Changes at once from the game's own GRAPHICS screen too. Session 80 (Jarrod):
    // STANDARD is the game's own filtering, no longer the forced 4x. Session 85: after POST EFFECTS (Session 100: FILM GRAIN).
    static constexpr const char* kTf[] = {"STANDARD", "8X", "16X"};
    static constexpr const char* kTfNote[] = {"The game's own texture filtering.",
                                              "8x anisotropic filtering: sharper floors and ramps at an angle.",
                                              "16x anisotropic filtering: the sharpest floors and ramps at an angle."};
    s.texture_filter = std::clamp(s.texture_filter, 0, 2);
    OptionRow("TEXTURE FILTERING", s.texture_filter, kTf, 3, true, kTfNote[s.texture_filter]);
    // (ORIGINAL LOOK moved to Advanced, Session 85.)
  }

  // ---------------- About (Session 31: its own page; was a section of Settings). The project, its links, credits
  // (the NVIDIA DLSS licence asks for its credit here) and the licences file.
  static constexpr const char* kProjectUrl = "https://github.com/Jarrodfeng/wwe2k14-recomp";
  struct Credit {
    const char* name;
    const char* what;
    const char* url;
  };
  static constexpr Credit kCredits[] = {
      {"ReXGlue SDK", "the recompiler and runtime this is built on (Tom Clay)", "https://github.com/rexglue/rexglue-sdk"},
      {"Xenia", "the Xbox 360 research ReXGlue's runtime grew from (Ben Vanik and contributors)", "https://xenia.jp"},
      {"360tools", "stub templates and the recompilation workflow (sp00nznet)", "https://github.com/sp00nznet/360tools"},
      {"Dear ImGui", "this launcher's interface (Omar Cornut)", "https://github.com/ocornut/imgui"},
      {"SDL", "controllers, sound and the game window", "https://libsdl.org"},
      {"FFmpeg", "audio decoding", "https://ffmpeg.org"},
      {"stb_image", "pictures (Sean Barrett)", "https://github.com/nothings/stb"},
      {"nlohmann/json", "settings files (Niels Lohmann)", "https://github.com/nlohmann/json"},
      {"NVIDIA DLSS", "upscaling (NVIDIA Streamline)", "https://developer.nvidia.com/rtx/streamline"},
      {"AMD FidelityFX", "FSR 3 upscaling (AMD FidelityFX SDK)", "https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK"},
      {"SteamGridDB", "the fan-made icon art", "https://www.steamgriddb.com"},
      // Session 67: CC BY-SA 3.0 asks for the credit, the licence's link and the changes made.
      {"360 controller.svg", "the KEYBOARD CONTROLS pad, by Alphathon (Wikimedia Commons), CC BY-SA 3.0; labels removed",
       "https://commons.wikimedia.org/wiki/File:360_controller.svg"},
  };

  void AboutPage() {
    const float d = Dpi();
    Heading("ABOUT", 1.6f);
    ImGui::Spacing();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const ImVec2 logo = DrawLogo(ImGui::GetWindowDrawList(), at, 300 * d, 1);
    if (logo.x > 0) {
      ImGui::Dummy(logo);
      ImGui::SameLine(0, 24 * d);
    }
    ImGui::BeginGroup();
    ImGui::Dummy(ImVec2(0, logo.y * 0.18f));
    {  // the heading with the author's byline to its right, in the sub-heading style
      const float px = ImGui::GetStyle().FontSizeBase * 1.15f * d, by_px = px * 0.7f;
      const ImVec2 at = ImGui::GetCursorScreenPos(), sz = MenuTextSize(px, "NATIVE PC RECOMPILATION");
      const ImVec2 bs = MenuTextSize(by_px, " - BY JARRODFENG");
      MenuText(ImGui::GetWindowDrawList(), ImVec2(at.x + sz.x, at.y + sz.y - bs.y - 1 * d), by_px,
               IM_COL32(255, 255, 255, 150), " - BY JARRODFENG");
    }
    Heading("NATIVE PC RECOMPILATION", 1.15f);
    ImGui::Text("%s", Version());
    ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x);
    Note("WWE 2K14 (Xbox 360, 2013; Yuke's and Visual Concepts, published by 2K Sports) recompiled into a native "
         "Windows program. A non-commercial fan preservation project, not affiliated with WWE, 2K, Take-Two, Yuke's "
         "or Visual Concepts. No game files are included: it runs from your own copy of the disc.");
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();

    ImGui::Dummy(ImVec2(0, 10 * d));
    Heading("LINKS", 1.15f);
    ImGui::TextLinkOpenURL("The project on GitHub", kProjectUrl);
    ImGui::SameLine(0, 24 * d);
    if (ImGui::TextLink("Third-party licences")) OpenNotices();
    ImGui::SameLine(0, 24 * d);
    if (ImGui::TextLink("Report a problem")) ImGui::OpenPopup("report");
    ReportPopup();
    ImGui::SameLine(0, 24 * d);
    if (ImGui::TextLink("Determinism check")) want_check_popup_ = true;  // Session 77 (CheckPopup)
    ImGui::SameLine(0, 24 * d);
    if (ImGui::TextLink("Diagnostic run")) want_diag_popup_ = true;  // Session 95 (DiagPopup)

    ImGui::Dummy(ImVec2(0, 10 * d));
    Heading("CREDITS", 1.15f);
    if (ImGui::BeginTable("credits", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, 150 * d);
      ImGui::TableSetupColumn("what", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("link", ImGuiTableColumnFlags_WidthFixed, 70 * d);
      for (const Credit& c : kCredits) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(c.name);
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", c.what);
        ImGui::TableNextColumn();
        ImGui::PushID(c.name);
        ImGui::TextLinkOpenURL("website", c.url);
        ImGui::PopID();
      }
      ImGui::EndTable();
    }
    ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x);
    Note("NVIDIA, the NVIDIA logo, DLSS and RTX are trademarks and/or registered trademarks of NVIDIA Corporation. "
         "AMD, FidelityFX and FSR are trademarks of Advanced Micro Devices, Inc. "
         "The full list of libraries and their licences is in THIRD_PARTY_NOTICES.md next to the launcher.");
    ImGui::PopTextWrapPos();
  }

  // THIRD_PARTY_NOTICES.md next to the launcher (a release), else the repo's copy (a development build).
  void OpenNotices() {
    std::error_code ec;
    fs::path f = paths_.launcher_dir / "THIRD_PARTY_NOTICES.md";
    if (!fs::exists(f, ec) && paths_.repo_found) f = paths_.repo / "docs" / "release" / "THIRD_PARTY_NOTICES.md";
    if (!fs::exists(f, ec)) return Status("THIRD_PARTY_NOTICES.md not found next to the launcher");
    ShellExecuteW(hwnd_, L"open", f.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  }

  void AdvancedSettings() {
    const float dpi = Dpi();
    {  // Session 67 (Jarrod): renamed from KEYBOARD AS CONTROLLER, with MAP KEYS beside it (the KEYBOARD CONTROLS window)
      const float bw = 150 * dpi, row_h = 38 * dpi;
      const ImVec2 row = ImGui::GetCursorScreenPos();
      OptionToggle("KEYBOARD CONTROLS", s.keyboard, true, "Play with the keyboard; MAP KEYS sets the key for each button.",
                   ImGui::GetContentRegionAvail().x - bw - 10 * dpi);
      const ImVec2 after = ImGui::GetCursorScreenPos();
      const float bx = row.x + ImGui::GetContentRegionAvail().x - bw;
      if (SlantButton("MAP KEYS", ImVec2(bx, row.y + 2 * dpi), ImVec2(bw, row_h - 4 * dpi), true, 17 * dpi))
        want_keys_popup_ = true;
      ImGui::SetCursorScreenPos(after);
    }
    // Session 27: the logo import's Session 25 route (counted presses, no screen checks), a fallback.
    OptionToggle("LOGO IMPORT: OLD ROUTE", s.logo_legacy_route, true, "Counted presses, no screen checks (a fallback).");
    // Session 85 (Jarrod: "hide Original look ... put it in advanced"): moved here from the Graphics page.
    OptionToggle("ORIGINAL LOOK", s.original_look, true,
                 "The Xbox 360 look: the picture drawn at 720p and entrances at 30 fps (no anti-aliasing, since the new renderer).");  // S105 A1b: the note said "4x MSAA", gone since Session 95
    // Session 87 (Jarrod: "an advanced launcher toggle, called true native"): --wwe2k14_d3d_owned. NATIVE only.
    // S101 (Jarrod, 2026-11-13: "yes hide true native"): hidden. Owner mode (default on) draws everything with our renderer and, with
    // bundle 2, the old SDK draw stack this toggled is not even created; the setting stays on (launcher_core.h forces it on load).
    // Session 95 (warm 60 route #7): for integrated graphics. Fullscreen only; the display goes back on close, alt-tab, crash.
    OptionToggle("720P DISPLAY", s.display_720p, true,
                 "Off by default. For integrated graphics: while the game is in front, switch the screen itself to "
                 "1280x720 so the picture is not scaled up by the game (the screen does it; the LOW look's sharpening upscale is off while this is on). Best with Resolution at 720p or the default. The screen goes back to normal "
                 "when you close the game, alt-tab or if it crashes. Fullscreen only; needs 1280x720 in your display's "
                 "mode list (the game log names the list if it is missing).");
    ImGui::Dummy(ImVec2(0, 6 * dpi));
    ImGui::SetNextItemWidth(-200 * dpi);
    ImGui::InputTextWithHint("Extra flags", "--name=value ...", &s.extra);
    if (ImGui::IsItemDeactivatedAfterEdit()) RepickGameExe(paths_, s);  // S102 W1: --wwe2k14_build=avx2|base takes effect without a reload
    bool changed = false;
    ImGui::SetNextItemWidth(-200 * dpi);
    changed |= ImGui::InputTextWithHint("Save folder", PathStr(DefaultUserRoot()).c_str(), &s.user_data_dir);
    ImGui::SetNextItemWidth(-200 * dpi);
    changed |= ImGui::InputTextWithHint("Mods folder", PathStr(paths_.mods).c_str(), &s.mods_dir);
    ImGui::SetNextItemWidth(-200 * dpi);
    changed |= ImGui::InputTextWithHint("Game exe", PathStr(paths_.launcher_dir / "wwe2k14.exe").c_str(), &s.game_exe);
    if (changed) Reload();
    if (ImGui::Button("Show the first-run wizard again")) wizard_step_ = 0, want_wizard_ = true;
    ImGui::SameLine();
    if (ImGui::Button("Reload everything")) Reload();
    // (Session 98: the asset viewer's button moved to the MODS page as ARENA VIEWER.)
    ImGui::Spacing();
    ImGui::TextDisabled("Settings file: %s", PathStr(settings_path).c_str());
    auto row = [](const char* what, const fs::path& p) {
      std::error_code ec;
      ImGui::TextDisabled("%-12s %s %s", what, PathStr(p).c_str(), fs::exists(p, ec) ? "" : "(missing)");
    };
    row("Game data", paths_.game_data);
    row("Saves", paths_.user_data);
    row("Mods", paths_.mods);
    row("Logs", paths_.logs);
    row("Scenarios", paths_.scenarios);
    if (paths_.repo_found) row("Repo", paths_.repo);
    ImGui::TextDisabled("%zu characters, %zu arenas, %zu saved matches", chars_.size(), arenas_.size(), scenarios_.size());
  }
