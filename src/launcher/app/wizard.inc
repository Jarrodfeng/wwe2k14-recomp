
  // ---------------- First run
  // The first-run wizard (Session 25; Session 31: the game's look, and the last step's suggestions as rows you can
  // change, with the language and the graphics card).
  void Wizard() {
    if (want_wizard_) ImGui::OpenPopup("Welcome"), want_wizard_ = false;
    if (!BeginGameModal("Welcome", "WELCOME", 780 * Dpi())) return;
    const float d = Dpi();
    // The steps, like the game's tabs (not clickable: Back / Next move).
    {
      static constexpr const char* kSteps[] = {"1  GAME FILES", "2  YOUR SAVE", "3  SETTINGS"};
      ImDrawList* dl = ImGui::GetWindowDrawList();
      const float px = 18 * d, h = 30 * d, pad = 14 * d, sk = 8 * d;
      for (int i = 0; i < 3; ++i) {
        if (i) ImGui::SameLine(0, 4 * d);
        const ImVec2 ts = MenuTextSize(px, kSteps[i]), pos = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(ts.x + pad * 2, h));
        if (i == wizard_step_) {
          const ImVec2 q[4] = {{pos.x + sk, pos.y}, {pos.x + ts.x + pad * 2, pos.y},
                               {pos.x + ts.x + pad * 2 - sk, pos.y + h}, {pos.x, pos.y + h}};
          dl->AddConvexPolyFilled(q, 4, IM_COL32(0, 0, 0, 255));
          MenuTextRaw(dl, ImVec2(pos.x + pad, pos.y + (h - ts.y) / 2), px, kMenuRed, kSteps[i]);
        } else {
          MenuText(dl, ImVec2(pos.x + pad, pos.y + (h - ts.y) / 2), px, IM_COL32(141, 142, 147, 255), kSteps[i]);
        }
      }
    }
    ImGui::Dummy(ImVec2(0, 10 * d));
    if (wizard_step_ == 0) {
      ImGui::TextWrapped("This plays WWE 2K14 natively on your PC, from your own copy of the Xbox 360 disc. No game "
                         "files come with it: point it at the files you copied from your disc.");
      ImGui::Spacing();
      GameDataSection(true);
    } else if (wizard_step_ == 1) {
      RefreshLogos(true);
      if (logo_.save_exists) {
        ImGui::TextColored(kGood, "Found your save (%d logo slot(s) used).", kLogoSlots - logo_.Free());
        ImGui::TextDisabled("%s", PathStr(logo_.title_dir).c_str());
      } else {
        ImGui::TextWrapped("No save yet. That's fine: the game makes one the first time you play. (Mods -> Logos needs "
                           "it, so start the game once before importing logos.)");
        ImGui::TextDisabled("Saves go to %s", PathStr(paths_.user_data).c_str());
      }
    } else {
      if (!wiz_ready_) {  // the suggestions, once per visit of this step
        wiz_ = s, wiz_.resolution = SuggestResolution(hw_), wiz_.draw_height = 0, wiz_.frame_rate = SuggestFrameRate(hw_), wiz_.upscale = 0;
        SuggestForDevice(wiz_);  // Session 37: integrated graphics -> LIGHT at 720p
        wiz_ready_ = true;
      }
      ImGui::TextDisabled("Suggested for your PC (%s, %dx%d at %d Hz). You can change them later in Settings.",
                          hw_.gpu.c_str(), hw_.screen_w, hw_.screen_h, hw_.refresh_hz);
      ImGui::Dummy(ImVec2(0, 4 * d));
      int li = 0;
      for (int i = 0; i < int(std::size(kLanguageIds)); ++i)
        if (kLanguageIds[i] == wiz_.language) li = i;
      if (OptionRow("LANGUAGE", li, kLanguageNames, int(std::size(kLanguageNames)))) wiz_.language = kLanguageIds[li];
      if (gpus_.empty()) gpus_ = ListGpus();
      std::vector<std::string> names{"AUTOMATIC"};
      for (const Gpu& g : gpus_) names.push_back(g.name);
      std::vector<const char*> ptrs;
      for (const auto& n : names) ptrs.push_back(n.c_str());
      int gi = 0;
      for (size_t i = 0; i < gpus_.size(); ++i)
        if (gpus_[i].name == wiz_.gpu) gi = int(i) + 1;
      if (OptionRow("GRAPHICS CARD", gi, ptrs.data(), int(ptrs.size()), true,
                    gpus_.size() > 1 ? "Automatic: the dedicated card with the most memory." : nullptr)) {
        wiz_.gpu = gi == 0 ? std::string() : gpus_[size_t(gi - 1)].name;
        wiz_.resolution = SuggestResolution(hw_), wiz_.draw_height = 0, wiz_.frame_rate = SuggestFrameRate(hw_), wiz_.renderer = 2;
        SuggestForDevice(wiz_);  // Session 37: the suggestions follow the card
      }
      ResolutionRow(wiz_, true, ChosenDevice(wiz_.gpu).integrated() ? "720p on integrated graphics; 540P in the list is lighter." : nullptr);
      FrameRateRow(wiz_);
      if (wiz_.crowd_density < 100)  // Session 95: LOW's starting values are written into the settings, not applied behind them
        ImGui::TextDisabled("Crowd density starts at %d%% on this PC (Settings -> Graphics).", wiz_.crowd_density);
      ImGui::TextDisabled("%s", hw_.rtx ? "Upscaling (DLSS / DLAA / FSR) is in Settings -> Graphics."
                                        : "Upscaling (AMD FSR) is in Settings -> Graphics.");
    }
    ImGui::Dummy(ImVec2(0, 12 * d));
    if (wizard_step_ == 0) {
      if (ModalMainButton("NEXT", gd_.ok)) wizard_step_ = 1;
      if (ModalButton("Skip for now")) wizard_step_ = 1;
    } else if (wizard_step_ == 1) {
      if (ModalMainButton("NEXT")) wizard_step_ = 2, wiz_ready_ = false;
      if (ModalButton("Back")) wizard_step_ = 0;
    } else {
      if (ModalMainButton("FINISH")) {
        s.language = wiz_.language, s.gpu = wiz_.gpu, s.resolution = wiz_.resolution;
        s.frame_rate = wiz_.frame_rate, s.upscale = wiz_.upscale, s.renderer = wiz_.renderer;
        s.draw_height = wiz_.draw_height, s.refresh_hz = wiz_.refresh_hz;  // Session 95 (the setup's choices were dropped)
        s.crowd_density = wiz_.crowd_density;  // Session 95
        FinishWizard();
        ImGui::CloseCurrentPopup();
      }
      if (ModalButton("Back")) wizard_step_ = 1;
      if (ModalButton("Keep my settings")) FinishWizard(), ImGui::CloseCurrentPopup();
    }
    ImGui::NewLine();
    ImGui::EndPopup();
  }
  std::array<int, 3> low_prev_{}, low_after_{};  // Session 95: resolution, draw height, crowd before / after the QUALITY row's LOW
  bool low_wrote_ = false;
  Settings wiz_;            // the wizard's last step: the suggestions, changed in place until FINISH
  bool wiz_ready_ = false;

  // (History.) Session 37 (the brief: "The first-run wizard should pick it on integrated graphics"): integrated graphics get the
  // light renderer at 720p (a UHD 620 drew 5-10 fps in matches with the accurate renderer at 720p); a card that cannot
  // run the accurate renderer (no interlock: older AMD drivers / graphics) gets the light renderer.
  // Session 41 (low-spec step 2): integrated graphics get the LOW preset (NATIVE, or LIGHT without interlock; 720p,
  // FRAME RATE 30). Session 51 ("NATIVE for everyone"): NATIVE on every card with interlock; integrated graphics still
  // start at 720p / 30 fps without an upscaler (the LOW preset's quality part, no button any more).
  // Session 54 (Jarrod, 2026-10-23: "540 as the default for igpus, as well as native as default for unsupported
  // interlock GPUs ... to make sure AMD people try the right thing"): NATIVE on cards without interlock too (it runs
  // there now: gpu_native_truth_host_rt), integrated graphics at 540p (NATIVE draws at 960x540).
  void SuggestForDevice(Settings& t) {
    const wwe2k14::vkdev::Device dev = ChosenDevice(t.gpu);
    if ((!dev.name.empty() && !dev.interlock) || dev.integrated()) t.renderer = 2;
    // (Session 95, Jarrod: "720p the default": integrated graphics start at true 720p, no longer 540p; 540P is still a hand pick.)
    // S102 S0-1: the values live in gfx_applied.h (the Surface bench's "auto" profile is generated from the same function).
    if (dev.integrated()) wwe2k14::gfxs::SuggestIntegrated(t.resolution, t.draw_height, t.frame_rate, t.refresh_hz, t.upscale, t.original_look);
    // Session 95 (Jarrod: "they start with our safe low defaults"): first-run auto-detect writes the LOW tier's starting values
    // into the settings the player sees (once; the sliders are his after that). Not with the original look.
    t.crowd_density = s.crowd_density;  // (a card change in the wizard starts from the file's value again)
    std::string why;
    const int rend_flag = wwe2k14::gfxs::RendererFromFlags(t.extra);
    if (wwe2k14::gfxs::ResolveQualityTier(t.quality_tier, false, dev.integrated(), dev.name, &why) == 1 && !t.original_look &&
        (rend_flag < 0 || rend_flag == 2)) {
      std::string what;
      wwe2k14::gfxs::LowStartValues(t.resolution, t.draw_height, t.upscale, t.crowd_density, &what);
      Log("setup: auto-detect " + why + " wrote the starting values:" + what + "; draw height " + std::to_string(t.draw_height) +
          ", frame rate " + std::to_string(t.frame_rate));
    }
  }

  // Session 54: once for a file from before settings version 3 (after the first-run setup), the same two choices
  // where the file still has what the setup used to pick: LIGHT on a card without interlock -> NATIVE; NATIVE at 720p
  // on integrated graphics -> 540p. Logged; true when something moved.
  bool MigrateForDevice(Settings& t) {
    const wwe2k14::vkdev::Device dev = ChosenDevice(t.gpu);
    bool moved = false;
    if (!dev.name.empty() && !dev.interlock && t.renderer == 1) {
      t.renderer = 2, moved = true;
      Log("settings: no interlock on " + dev.name + ": renderer LIGHT -> NATIVE (once, Session 54)");
    }
    if (dev.integrated() && t.renderer == 2 && t.resolution == 0 && t.draw_height == 0 && t.upscale == 0) {
      t.draw_height = 540, moved = true;
      Log("settings: integrated graphics: resolution 720p -> 540p (once, Session 54)");
    }
    t.loaded_version = wwe2k14::gfxs::kSettingsVersion;
    return moved;
  }

  void FinishWizard() {
    s.first_run_done = true, s.page = 0, wizard_step_ = 0;
    if (save_settings) SaveSettingsSynced();
  }
