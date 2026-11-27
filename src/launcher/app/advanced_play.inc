
  // ---------------- Play
  bool Combo(const char* label, std::string& value, const std::vector<std::string>& items, const char* none) {
    bool changed = false;
    if (ImGui::BeginCombo(label, value.empty() ? none : value.c_str())) {
      if (none && ImGui::Selectable(none, value.empty())) value.clear(), changed = true;
      for (const auto& it : items)
        if (ImGui::Selectable(it.c_str(), it == value)) value = it, changed = true;
      ImGui::EndCombo();
    }
    return changed;
  }

  std::string CharName(int id) const {
    for (const auto& c : chars_)
      if (c.id == id) return c.name;
    for (const auto& c : dlc_chars_)  // Session 32
      if (c.id == id) return c.name + (DlcWrestlerOn(c) ? "" : " (DLC pack off)");
    return "#" + std::to_string(id);
  }

  // Back to a clean Play tab: every match / preset / option setting to its default. Paths, the window size and the
  // Trons tab's view stay.
  void ResetPlay() {
    Settings d;
    d.game_exe = s.game_exe, d.mods_dir = s.mods_dir, d.window_w = s.window_w, d.window_h = s.window_h;
    d.trons_view = s.trons_view, d.trons_sort = s.trons_sort, d.trons_sort_desc = s.trons_sort_desc;
    s = d;
    rec_mode_ = 0;
    seeds_scenario_ = "\x01";
    if (save_settings) SaveSettingsSynced();
    Status("Play settings reset to the defaults");
  }

  // Session 25: the Sessions 17-24 Play tab, now Play Game -> Advanced (the match itself is Quick Match's).
  void AdvancedPlay() {
    const float label_w = 170 * ImGui::GetStyle().FontScaleDpi;
    if (ImGui::Button("Reset these settings")) ImGui::OpenPopup("reset_play");
    if (ImGui::BeginPopupModal("reset_play", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::TextUnformatted("Reset the match, preset and option settings to their defaults?");
      ImGui::TextDisabled("Paths, your game files, trons, logos and Settings stay.");
      if (ImGui::Button("Reset")) ResetPlay(), ImGui::CloseCurrentPopup();
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }
    ImGui::SeparatorText("Preset");
    for (int i = 0; i < kPresetCount; ++i) {
      if (i) ImGui::SameLine();
      ImGui::RadioButton(kPresetNames[i], &s.preset, i);
    }
    switch (s.preset) {
      case kPlay:
        ImGui::TextDisabled("The game with your settings (and the Quick Match above, if one is picked).");
        break;
      case kReplay:
        ImGui::RadioButton("Record a new match", &rec_mode_, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Play a recording", &rec_mode_, 1);
        s.replay_record = rec_mode_ == 0;
        ImGui::SetNextItemWidth(label_w * 2);
        if (s.replay_record) ImGui::InputText("Recording name", &s.replay_name);
        else Combo("Recording", s.replay_play, replays_, nullptr);
        ImGui::TextDisabled("Replay mode: the run is exactly repeatable (out\\replays\\NAME.rec).");
        break;
      case kReshoot: {
        ImGui::SetNextItemWidth(label_w * 2);
        Combo("Recording", s.replay_play, replays_, nullptr);
        ImGui::SetNextItemWidth(label_w * 2);
        ImGui::InputTextWithHint("Shots", "a.txt,b.txt (CineCam paths)", &s.reshoot_shots);
        ImGui::SameLine();
        if (ImGui::SmallButton("Add shot...")) ImGui::OpenPopup("pick_shot");
        if (ImGui::BeginPopup("pick_shot")) {
          for (const auto& sh : shots_)
            if (ImGui::Selectable(sh.c_str())) s.reshoot_shots += (s.reshoot_shots.empty() ? "" : ",") + sh;
          if (shots_.empty()) ImGui::TextDisabled("No paths in %s", PathStr(paths_.shots).c_str());
          ImGui::EndPopup();
        }
        ImGui::SetNextItemWidth(label_w);
        ImGui::InputText("Anchor", &s.reshoot_anchor);
        ImGui::TextDisabled("One pass per shot, fast-forwarded -> out\\reshoots\\NAME\\shotNN.mp4");
        break;
      }
      case kSeed:
        SeedPanel(label_w);
        break;
      case kDirector:
        ImGui::SetNextItemWidth(label_w * 2);
        Combo("Director script", s.director, directors_, nullptr);
        ImGui::SameLine();
        if (ImGui::SmallButton("Open folder")) {
          std::error_code ec;
          fs::create_directories(paths_.director, ec);
          ShellExecuteW(hwnd_, L"explore", paths_.director.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        ImGui::TextDisabled("out\\director\\NAME.txt: scenario, seed and timed events; runs in replay mode.");
        ImGui::Checkbox("Use this seed instead of the script's", &s.director_seed);
        SeedPanel(label_w);
        break;
    }
    ImGui::SeparatorText("Options");
    ImGui::Checkbox("Mods (custom trons and titles)", &s.mods);
    ImGui::SameLine();
    ImGui::Checkbox("CineCam (F8)", &s.cinecam);
    if (paths_.repo_found) {
      ImGui::Checkbox("Test harness (frozen save, run lock)", &s.harness);
      if (s.harness) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(label_w * 2);
        Combo("Route", s.harness_route, routes_, nullptr);
      }
    }
    ImGui::SeparatorText("Command");
    SyncSuperstarsKey(true);  // Session 98 (importer review): the switch as the importer may have just set it
    const Command cmd = BuildCommand(s, paths_);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
    ImGui::BeginChild("cmd", ImVec2(-1, ImGui::GetTextLineHeightWithSpacing() * 2.8f), ImGuiChildFlags_None);
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(cmd.display.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    ImGui::PopStyleColor();
    if (ImGui::Button("Copy")) ImGui::SetClipboardText(cmd.display.c_str()), Status("Command copied");
    ImGui::SameLine();
    if (!cmd.error.empty()) ImGui::TextColored(ImVec4(1, 0.55f, 0.4f, 1), "%s", cmd.error.c_str()), ImGui::SameLine();
    ImGui::BeginDisabled(!cmd.error.empty() || game_proc_ != nullptr);
    if (ImGui::Button("Launch with this preset")) DoLaunch(false);
    ImGui::EndDisabled();
  }

  void SeedPanel(float label_w) {
    ImGui::SetNextItemWidth(label_w);
    ImGui::InputInt("Seed", &s.seed);
    ImGui::SameLine();
    ImGui::TextDisabled("plays the scenario in replay mode with this seed");
    const std::string key = s.scenario + (s.preset == kDirector && !s.director.empty() ? "+" + s.director : "");
    if (seeds_scenario_ != key) seeds_ = LoadSeeds(paths_, key), seeds_scenario_ = key;
    ImGui::SetNextItemWidth(label_w * 2);
    ImGui::InputTextWithHint("Filter", "winner=0 finish=1 max_clock=300 min_near_falls=2", &s.seed_filter);
    ImGui::SameLine();
    if (ImGui::SmallButton("Reload")) seeds_scenario_.clear();
    if (ImGui::BeginTable("seeds", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Borders,
                          ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 7))) {
      ImGui::TableSetupScrollFreeze(0, 1);
      for (const char* h : {"seed", "finish", "winner", "clock s", "pins", "near falls", "run"}) ImGui::TableSetupColumn(h);
      ImGui::TableHeadersRow();
      int shown = 0;
      for (const auto& r : seeds_) {
        if (!SeedMatches(r, s.seed_filter)) continue;
        ++shown;
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        const int seed = std::atoi(r.get("seed").c_str());
        if (ImGui::Selectable((r.get("seed") + "##s").c_str(), seed == s.seed, ImGuiSelectableFlags_SpanAllColumns))
          s.seed = seed;
        for (const char* k : {"finish", "winner", "clock_s", "pins", "near_falls", "run"}) {
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(r.get(k).c_str());
        }
      }
      ImGui::EndTable();
      if (seeds_.empty()) ImGui::TextDisabled("No sweep for this scenario yet (out\\seeds\\%s.csv).", s.scenario.c_str());
      else ImGui::TextDisabled("%d of %zu seeds match.", shown, seeds_.size());
    }
    ImGui::SetNextItemWidth(label_w * 0.7f);
    ImGui::InputInt("from", &s.sweep_from, 0);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(label_w * 0.7f);
    ImGui::InputInt("to", &s.sweep_to, 0);
    ImGui::SameLine();
    if (ImGui::Button("Run a sweep")) DoLaunch(true);
    ImGui::SameLine();
    ImGui::TextDisabled("one fast-forwarded CPU match per seed (tools\\seeds\\sweep.py)");
  }

  void DoLaunch(bool sweep) {
    SyncSuperstarsKey(true);  // Session 98 (importer review)
    const Command cmd = BuildCommand(s, paths_, sweep);
    StartGame(cmd, "Started: " + cmd.display);
  }

  HWND hwnd_;
  Paths paths_;
  std::vector<Character> chars_;
  std::vector<Arena> arenas_;
  std::vector<Scenario> scenarios_;
  std::vector<std::string> replays_, directors_, shots_, routes_;
  std::map<int, fs::path> overrides_;
  // Session 75: the MUSIC tab (new songs in <mods>\music\new, wrestlers' replacement themes).
  std::vector<NewSong> songs_;
  std::chrono::steady_clock::time_point songs_checked_{};
  int music_kind_ = 0, music_sel_ = -1;  // selection: 1 = songs_[music_sel_], 2 = chars_[music_sel_]
  std::string music_search_, song_name_edit_;
  std::vector<SeedRow> seeds_;
  std::string seeds_scenario_ = "\x01";
  std::string search_, pick_search_, status_;
  std::mt19937 rng_{std::random_device{}()};  // Session 27: Quick Match "Random"
  std::chrono::steady_clock::time_point status_time_;
  bool only_overrides_ = false, scroll_to_selected_ = false;
  int selected_ = -1, selected_tron_ = -1, want_tab_ = -1, orig_tron_ = -1;
  int rec_mode_ = 0;
  int hovered_tron_ = -1;  // grid tile under the mouse (its preview plays)
  int force_hover_ = -1;   // automation: --hover TRON (screenshots of playback)
  Player player_;
  fs::path hover_video_;   // the video asked for this frame (the player stops when nothing is hovered)
  std::chrono::steady_clock::time_point previews_checked_;
  int cached_previews_ = 0;
  bool table_sort_ready_ = false;  // the table's first sort specs are its defaults, not a click
  VideoInfo orig_info_;
  std::unique_ptr<Thumb> thumb_;
  // Session 31: the look (background picture, logo, the game's menu face).
  ID3D11ShaderResourceView* bg_srv_ = nullptr;
  int bg_w_ = 0, bg_h_ = 0;
  std::vector<uint8_t> logo_px_;
  int logo_w_ = 0, logo_h_ = 0;
  bool logo_tried_ = false;
  ID3D11ShaderResourceView* logo_srv_[3] = {};  // slots: Play screen, About, the band
  int logo_srv_w_[3] = {};
  GameFont gfont_;
  std::vector<Gpu> gpus_;  // Settings -> Display: the graphics cards (ListGpus)
  // Session 37: the Vulkan devices (vk_devices.h), listed once; the one the game will use for a GRAPHICS CARD choice.
  std::vector<wwe2k14::vkdev::Device> vk_devs_;
  bool vk_listed_ = false;
  bool fake_igpu_ = false;  // automation: --fake-igpu (the integrated-graphics choices on any PC, for screenshots)
  bool fake_no_interlock_ = false;  // automation (Session 54): --fake-no-interlock (a card without interlock)
  wwe2k14::vkdev::Device ChosenDevice(const std::string& gpu) {
    if (!vk_listed_) vk_devs_ = wwe2k14::vkdev::List(), vk_listed_ = true;
    const int i = wwe2k14::vkdev::Pick(vk_devs_, gpu);
    wwe2k14::vkdev::Device d = i >= 0 ? vk_devs_[size_t(i)] : wwe2k14::vkdev::Device{};
    if (fake_igpu_) d.type = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU, d.name = "(test) integrated GPU";
    if (fake_no_interlock_) d.interlock = false, d.name = "(test) card without interlock";
    return d;
  }
  bool nv_other_ = false;  // Settings -> Graphics: the player opened "Use other files..." (Session 31)
  ID3D11ShaderResourceView* gfont_srv_ = nullptr;
  fs::path gfont_root_;
  bool gfont_tried_ = false;
