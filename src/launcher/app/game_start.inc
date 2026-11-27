
  // Starts a command; the game itself is watched for an abnormal exit (PollGame).
  // Session 53: a launcher on another settings file (--settings: a test, a fresh profile) gives the game the same file,
  // so the game's GRAPHICS menu and its FSR note never write the player's own one.
  std::string GameSettingsFlag() const {
    return settings_path == SettingsPath() ? "" : "--wwe2k14_settings_file=" + Quote(PathStr(settings_path));
  }
  // A helper run of the game (Build previews, a logo import): where the game files and the save are, as GameFlags
  // gives them to PLAY GAME, the DLC packs (Session 57, tester bug 2: the save may need them; S110 LOGOIMPORT: with the
  // imported superstars' pack, through DlcFlags) and this launcher's settings file; each ends with a space.
  std::string HelperPathFlags() const {
    std::string a;
    if (!s.game_data_dir.empty()) a += "--game_data_root=" + Quote(s.game_data_dir) + " ";
    if (!s.user_data_dir.empty()) a += "--user_data_root=" + Quote(s.user_data_dir) + " ";
    for (const auto& x : DlcFlags(s, paths_)) a += x + " ";
    if (const std::string f = GameSettingsFlag(); !f.empty()) a += f + " ";
    return a;
  }
  Command WithSettingsFile(Command c) const {
    const std::string f = GameSettingsFlag();
    if (f.empty() || c.exe != paths_.game_exe.wstring() || c.args.find(L"--wwe2k14_settings_file=") != std::wstring::npos)
      return c;  // (TRY AGAIN restarts last_cmd_, which has it already)
    c.args += (c.args.empty() ? L"" : L" ") + Widen(f), c.display += " " + f;
    return c;
  }

  void StartGame(const Command& cmd_in, const std::string& ok_msg) {
    const Command cmd = WithSettingsFile(cmd_in);
    if (game_proc_ || precompile_proc_ || (cmd.exe == paths_.game_exe.wstring() && GameRunning())) {
      Status("The game is already running");
      return;
    }
    if (cmd.exe == paths_.game_exe.wstring() && MusicAutomation() && MusicBusy()) MusicCancelAll();  // an automated start never runs alongside a conversion
    if (cmd.exe == paths_.game_exe.wstring() && !music_skip_ && MusicWaitWanted()) {  // lane 283: songs are converting: PLAY waits (with the bar)
      music_start_pending_ = true, want_music_popup_ = true;
      music_pending_cmd_ = cmd_in, music_pending_msg_ = ok_msg;
      Status("Your songs are being converted: the game starts when that is done");
      return;
    }
    // Session 101 (lane 199): the imported superstars' pack is brought up to date first (S110 HELPERSTART: the step every start that loads
    // the save takes; a build already running makes this start wait, as before)
    if (StarsBeforeStart(cmd, ok_msg)) return;
    // Session 70: the precompile first, when this build / card / driver / pipeline settings have not had one.
    if (cmd.exe == paths_.game_exe.wstring() && s.precompile && !s.harness && perf_step_ < 0 && bench_step_ < 0 /* S102 U4 */ && !check_running_ && !diag_running_ &&
        !importing_ &&
        !precompile_skip_once_ && PrecompileKey() != ReadPrecompileStamp()) {
      StartPrecompile(cmd, ok_msg);
      return;
    }
    precompile_skip_once_ = false;
    HANDLE h = nullptr;
    const std::string err = StartProcess(cmd, &h);
    if (!err.empty()) {
      music_skip_ = false;
      Status("Could not start: " + err);
      return;
    }
    if (cmd.exe == paths_.game_exe.wstring()) {
      game_proc_ = h, last_cmd_ = cmd, stars_ok_for_.clear(), music_skip_ = false;
      stars_in_start_ = cmd.display.find("--wwe2k14_mods_superstars=true") != std::string::npos;  // (review N4)
    }
    else CloseHandle(h);
    Status(ok_msg);
    if (save_settings) SaveSettingsSynced();
    if (cmd.exe == paths_.game_exe.wstring()) {  // what the in-game GRAPHICS screen may change (ReloadGraphicsSettings)
      launch_graphics_.upscale = s.upscale, launch_graphics_.resolution = s.resolution;
      launch_graphics_.draw_height = s.draw_height;  // Session 54 (review of a tester's report: it was dropped)
      launch_graphics_.frame_rate = s.frame_rate, launch_graphics_.original_look = s.original_look;
      launch_graphics_.texture_filter = s.texture_filter;
      launch_ramp_camera_ = s.ramp_camera;  // S111 RAMPMENU: the pause menu's RAMP CAMERA writes the same file
      launch_graphics_set_ = true;
    }
  }

  // Session 70 (Jarrod: "every pipeline built before the game starts"; src\precompile.h): a pipeline the PC has not
  // compiled before stalls the game when an entrance meets it (v0.2.6 / v0.2.7 froze 4.6 s at Brock's entrance with
  // cold compiles). Before a Play the game runs once in its precompile mode with the same graphics settings (a plain
  // boot: every stored pipeline made on all CPU threads, then it quits), with a progress window and SKIP. Again only
  // when this key changes: this build (the exe's size and time), the graphics card and driver, the settings that change
  // pipelines (resolution, draw height, upscaler, original look, post effects) and the Extra flags.
  std::string PrecompileKey() {
    const wwe2k14::vkdev::Device dev = ChosenDevice(s.gpu);
    // (Review: the SDK's DLLs too - a release may change only them - and the renderer.)
    std::string files;
    for (const std::string f : {PathStr(paths_.game_exe.filename()), std::string("rexgpu-xenos.dll"), std::string("rexruntime.dll")}) {  // S102: the exe actually started (wwe2k14.exe or the v3 exe)
      std::error_code ec;
      const fs::path p = paths_.game_exe.parent_path() / f;
      files += std::string(f) + " " + std::to_string(fs::file_size(p, ec)) + " " +
               std::to_string(fs::last_write_time(p, ec).time_since_epoch().count()) + "; ";
    }
    return files + "gpu " + dev.name + " driver " + std::to_string(dev.driver_version) + "; renderer " +
           std::to_string(s.renderer) + " resolution " + std::to_string(s.resolution) + " draw_height " +
           std::to_string(s.draw_height) + " upscale " + std::to_string(s.upscale) + " quality_tier " +
           std::to_string(s.quality_tier) + " original_look " +
           std::to_string(int(s.original_look)) + " post_effects " + std::to_string(s.post_effects) +
           // Session 92 M6: TRUE NATIVE has its own translation store (the precompile fills it from the list's V lines),
           // so turning it on or off must run the precompile again.
           " true_native " + std::to_string(int(s.true_native)) + "; extra " +
           StripBuildChoice(s.extra);  // Session 72: the post effects choice swaps two pipelines
  }
  fs::path PrecompileStampPath() const { return fs::path(settings_path).parent_path() / "precompile_stamp.txt"; }
  std::string ReadPrecompileStamp() const {
    std::ifstream f(PrecompileStampPath());
    std::string line;
    std::getline(f, line);
    return line;
  }
  void StartPrecompile(const Command& next, const std::string& next_msg) {
    precompile_next_ = next, precompile_next_msg_ = next_msg;
    precompile_pass_ = 1;
    precompile_all_t0_ = std::chrono::steady_clock::now();
    if (StartPrecompileRun()) want_precompile_popup_ = true;
  }
  // One run of the game in its precompile mode; false (and the game started) when it cannot start.
  bool StartPrecompileRun() {
    std::error_code ec;
    precompile_progress_ = fs::path(settings_path).parent_path() / "precompile_progress.json";
    fs::remove(precompile_progress_, ec);
    Command c = WithSettingsFile(BuildCommand(PlainSettings(), paths_));
    // (Review: no performance report or Discord status from this run.)
    const std::string f = " --wwe2k14_precompile=" + Quote(PathStr(precompile_progress_)) +
                          " --window_activate_when_shown=false --window_show_behind=true" +
                          " --wwe2k14_perf_report=false --wwe2k14_discord=false";
    c.args += Widen(f), c.display += f;
    HANDLE h = nullptr;
    if (const std::string err = StartProcess(c, &h); !err.empty()) {
      Log("precompile: could not start (" + err + "); starting the game without it");
      precompile_skip_once_ = true;
      StartGame(precompile_next_, precompile_next_msg_);
      precompile_skip_once_ = false;
      return false;
    }
    Log("precompile (pass " + std::to_string(precompile_pass_) + "): " + c.display);
    precompile_proc_ = h;
    precompile_t0_ = std::chrono::steady_clock::now();
    precompile_made_ = precompile_total_ = precompile_optimised_ = 0, precompile_state_ = "starting";
    return true;
  }
  // Once per frame: the precompile's progress file; when the run has ended, the stamp (done only) and the game.
  void ReadPrecompileProgress() {
    std::ifstream f(precompile_progress_);
    std::stringstream ss;
    ss << f.rdbuf();
    try {
      const json j = json::parse(ss.str());
      precompile_state_ = j.value("state", precompile_state_);
      precompile_made_ = j.value("made", 0), precompile_total_ = j.value("total", 0);
      precompile_optimised_ = j.value("optimised", 0);
    } catch (...) {
    }
  }
  void PollPrecompile() {
    if (!precompile_proc_) return;
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - precompile_t0_).count();
    if (WaitForSingleObject(precompile_proc_, 0) != WAIT_OBJECT_0) {
      ReadPrecompileProgress();
      // (Review: our own deadline - the game's timeout counts only once it draws; a boot that never gets there ends here.)
      if (secs > kPrecompileDeadlineS) {
        Log("precompile: no end after " + std::to_string(int(secs)) + " s; stopped");
        TerminateProcess(precompile_proc_, 0);
      }
      return;
    }
    DWORD code = 0;
    GetExitCodeProcess(precompile_proc_, &code);
    CloseHandle(precompile_proc_);
    precompile_proc_ = nullptr;
    ReadPrecompileProgress();  // (review: the last state, written just before the run ended)
    // Session 70 (the shader optimiser): the first pass stored optimised shaders but made its pipelines from the
    // unoptimised code - a second pass makes the pipelines the game will ask for (from the stored code).
    if (code == 0 && precompile_state_ == "done" && precompile_optimised_ > 0 && precompile_pass_ == 1) {
      Log("precompile: " + std::to_string(precompile_optimised_) + " shaders optimised in " + std::to_string(int(secs)) +
          " s; the second pass makes their pipelines");
      precompile_pass_ = 2;
      StartPrecompileRun();
      return;
    }
    const double all = std::chrono::duration<double>(std::chrono::steady_clock::now() - precompile_all_t0_).count();
    // A run that gave up at its own timeout is marked too: what it made is in the driver's cache, and a PC that cannot
    // finish would otherwise wait the whole timeout before every Play.
    if (code == 0 && (precompile_state_ == "done" || precompile_state_ == "timeout")) {
      std::ofstream o(PrecompileStampPath(), std::ios::trunc);
      o << PrecompileKey() << "\n" << precompile_state_ << ": " << precompile_made_ << " of " << precompile_total_
        << " pipelines, " << precompile_pass_ << " pass(es) in " << int(all) << " s, " << NowStamp("%Y-%m-%d %H:%M")
        << "\n";
      Log("precompile: " + precompile_state_ + ", " + std::to_string(precompile_made_) + " of " +
          std::to_string(precompile_total_) + " pipelines, " + std::to_string(precompile_pass_) + " pass(es) in " +
          std::to_string(int(all)) + " s");
    } else {
      Log("precompile: ended (" + precompile_state_ + ", " + ExitCodeText(code) + "): not marked done");
    }
    precompile_skip_once_ = true;
    StartGame(precompile_next_, precompile_next_msg_);
    precompile_skip_once_ = false;  // (review: also when StartGame returned early)
    if (direct_after_precompile_) {  // a direct / quick start: hidden with the game, as without the precompile
      direct_after_precompile_ = false;
      if (game_proc_) {
        exit_with_game_ = true;
        ShowWindow(hwnd_, SW_HIDE);
        Log("direct start: the game started after the precompile");
      }
    }
  }
  static constexpr double kPrecompileDeadlineS = 660;  // the game's own timeout (600 s) + a minute
  void PrecompilePopup() {
    if (want_precompile_popup_) ImGui::OpenPopup("precompile"), want_precompile_popup_ = false;
    if (!BeginGameModal("precompile", "PREPARING SHADERS", 640 * Dpi())) return;
    if (!precompile_proc_) {
      ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
      return;
    }
    ImGui::TextWrapped("Building the graphics for this PC before the game starts, so entrances and matches don't "
                       "stutter. This happens once after an update, a new graphics driver or a graphics setting change.");
    ImGui::Dummy(ImVec2(0, 8 * Dpi()));
    // (Session 70: the second pass - the pipelines from the shaders the first one optimised.)
    if (precompile_pass_ == 2) ImGui::TextDisabled("Step 2 of 2: the optimised shaders");
    const float frac = precompile_total_ ? float(precompile_made_) / float(precompile_total_) : 0.0f;
    const std::string label = precompile_state_ == "starting" || !precompile_total_
                                  ? std::string("Starting...")
                                  : std::to_string(precompile_made_) + " / " + std::to_string(precompile_total_);
    ImGui::ProgressBar(frac, ImVec2(-1, 0), label.c_str());
    ImGui::Dummy(ImVec2(0, 12 * Dpi()));
    if (ModalMainButton("SKIP")) {
      Log("precompile: skipped by the player");
      TerminateProcess(precompile_proc_, 0);  // (PollPrecompile starts the game when it has ended: not marked done)
    }
    ImGui::NewLine();
    ImGui::EndPopup();
  }
  // ---- Session 101 (lane 199, Jarrod: "as simple as possible to add characters"): the imported superstars before every game start.
  // With USE MY SUPERSTARS on, the pack is checked against the DLC THIS start gives the game (USE MY DLC, the DLC folder, the
  // switched-off packs: the same flags the command carries) by the game's own rule (src\importer\pack_fit.h). When the list changed,
  // was never built, or no longer fits (DLC added, removed, switched off, another copy), the pack is rebuilt here from the player's
  // own files first (a few seconds; ~14 s for a full pack), then the game starts. Before, the game left such a pack out and only
  // its log said so. A build that fails says why, with PLAY WITHOUT THEM / CANCEL.
  struct StarsJob {
    std::atomic<int> state{1};  // 1 building, 2 done, 3 failed
    std::atomic<float> frac{0};
    std::mutex m;
    std::string phase, err;
    std::vector<std::string> moves;  // (review S5) "X moved from slot A to B ..." for the player
  };
  std::shared_ptr<StarsJob> stars_job_;
  Command stars_next_;
  std::string stars_next_msg_, stars_ok_for_, stars_why_, stars_fail_;
  bool want_stars_popup_ = false, stars_in_start_ = false;
  stars_start::Then stars_then_ = stars_start::Then::kPlay;  // S110 HELPERSTART: the start the build is for (StartAfterStars asks it again)
  std::string StarsDlcDir() const { return s.dlc ? PathStr(paths_.dlc) : std::string(); }  // (DlcFlags' rule: off = no folder)
  // true = this start waits (a build runs, then the same start again: StartAfterStars; or a build already runs: said); false = start now.
  // S110 HELPERSTART: PLAY and the logo import both come here (`then` = which one), just before their process starts, so every start that
  // loads the save gets the superstars pack in a state the game takes in (the decision: src\launcher\stars_start.h, its unit
  // tools\launcher\s110_tests\run_stars_start.py). Build previews does not (review F-3: the tron export never reaches the save, stars_start.h).
  bool StarsBeforeStart(const Command& cmd, const std::string& ok_msg, stars_start::Then then = stars_start::Then::kPlay) {
    stars_start::Ask a;
    a.build_running = stars_job_ != nullptr;
    a.game_exe = cmd.exe == paths_.game_exe.wstring();
    a.stars_flag = stars_start::HasStarsFlag(cmd.display);
    a.built_for_this = cmd.display == stars_ok_for_;  // (built for this start)
    // (review S2) the precompile's exclusions: a test run (harness, performance test, determinism check, diagnostic run, logo
    // import) never waits on a build - it would abort the test and start an orphan game run later
    a.test_run = s.harness || perf_step_ >= 0 || bench_step_ >= 0 /* S102 U4 */ || check_running_ || diag_running_ || importing_;
    const std::string mods = PathStr(paths_.mods);
    importer::PlayCheck c;
    importer::PackState st;
    const stars_start::Step step = stars_start::Decide(a, [&] {
      c = importer::CheckBeforePlay(mods, StarsDlcDir(), s.dlc_off);
      std::string err;
      return c.build && importer::LoadPack(mods, st, err);
    });
    if (step == stars_start::Step::kWait) {
      if (then == stars_start::Then::kLogoImport) logo_result_ = stars_start::WaitText(then);
      Status(stars_start::WaitText(then));
      return true;
    }
    if (step == stars_start::Step::kStart) return false;
    Log("superstars: " + c.why + " Building the pack before the game starts (" + std::to_string(st.stars.size()) + " superstar(s))");
    stars_next_ = cmd, stars_next_msg_ = ok_msg, stars_why_ = c.why, stars_fail_.clear(), stars_then_ = then;
    auto j = stars_job_ = std::make_shared<StarsJob>();
    const std::string game = s.game_data_dir.empty() ? PathStr(paths_.game_data) : s.game_data_dir, dlc = StarsDlcDir(), off = s.dlc_off;
    std::thread([j, st, mods, game, dlc, off] { wwe2k14::NameThread(L"lnch_import");
      launcher::GameFiles g;
      g.SetRoot(fs::u8path(game));
      std::string e;
      bool ok = false;
      try {
        ok = importer::BuildPack(mods, g, dlc, st, [j](float f, const char* phase) {
          j->frac = f;
          std::lock_guard<std::mutex> lk(j->m);
          j->phase = phase ? phase : "";
        }, e, off);
      } catch (const std::exception& x) {
        e = x.what();
      }
      std::vector<std::string> moves = ok ? importer::LastBuildMoves() : std::vector<std::string>();
      {
        std::lock_guard<std::mutex> lk(j->m);
        j->err = e, j->moves = std::move(moves);
        j->state = ok ? 2 : 3;
      }
      WakeUi();  // (review B1: the game starts at once, not at the next input)
    }).detach();  // (the job is shared: a launcher closed mid-build leaves pack6.building, which the next build removes)
    want_stars_popup_ = true;
    return true;
  }
  void PollStars() {  // once per frame
    if (!stars_job_ || stars_job_->state == 1) return;
    std::string err, moved;
    {
      std::lock_guard<std::mutex> lk(stars_job_->m);
      err = stars_job_->err;
      for (const std::string& m : stars_job_->moves) moved += (moved.empty() ? "" : " ") + m;
    }
    const bool ok = stars_job_->state == 2;
    stars_job_.reset();
    stars_sig_.clear();  // (MODS > SUPERSTARS reads the pack again)
    if (!ok) {
      stars_fail_ = err.empty() ? std::string("the build stopped") : err;
      Log("superstars: the build failed: " + stars_fail_);
      if (stars_then_ == stars_start::Then::kLogoImport)  // S110 HELPERSTART: said on the logo page too (nothing was backed up or written)
        logo_result_ = "Your superstars could not be built into the game: " + stars_fail_ + ". " + stars_start::FailText(stars_then_);
      want_stars_popup_ = true;
      return;
    }
    Log("superstars: the pack is built; starting the game" + (moved.empty() ? std::string() : " (" + moved + ")"));
    stars_ok_for_ = stars_next_.display;
    StartAfterStars();
    if (!moved.empty()) Status(moved);  // (review S5b: said to the player, after StartGame's own line)
  }
  // This one start without --wwe2k14_mods_superstars (PLAY WITHOUT THEM).
  static Command WithoutStars(Command c) {
    // (a whole token: GameFlags may put it first in args, with no space before it)
    auto drop = [](auto& s, const auto& flag) {
      for (size_t k = s.find(flag); k != s.npos; k = s.find(flag, k)) {
        const size_t end = k + flag.size();
        if ((k && s[k - 1] != ' ') || (end < s.size() && s[end] != ' ')) {
          k = end;
          continue;
        }
        s.erase(k ? k - 1 : k, flag.size() + (k ? 1 : (end < s.size() ? 1 : 0)));
        k = k ? k - 1 : 0;
      }
    };
    const std::string f = "--wwe2k14_mods_superstars=true";
    drop(c.display, f);
    drop(c.args, std::wstring(f.begin(), f.end()));
    return c;
  }
  void StartAfterStars() {
    // S110 HELPERSTART: the logo import is asked again from its start (the same command: stars_ok_for_ lets the step through). Its re-ask
    // starts or stops at once (no precompile after it, unlike PLAY's), so the memo ends with it (review): a re-ask that stopped before its
    // start (the game opened meanwhile, the backup failed) must not let a later start of the same command past the check.
    if (stars_then_ == stars_start::Then::kLogoImport) {
      StartLogoImport();
      stars_ok_for_.clear();
      return;
    }
    StartGame(stars_next_, stars_next_msg_);
    if (direct_after_precompile_ && game_proc_) {  // a direct / quick start: hidden with the game, as after the precompile
      direct_after_precompile_ = false;
      exit_with_game_ = true;
      ShowWindow(hwnd_, SW_HIDE);
      Log("direct start: the game started after the superstars build");
    }
  }
  void StarsPopup() {
    if (want_stars_popup_) ImGui::OpenPopup("stars"), want_stars_popup_ = false;
    if (!BeginGameModal("stars", "YOUR SUPERSTARS", 640 * Dpi())) return;
    if (stars_job_) {
      ImGui::TextWrapped("%s Building your superstars into the game from your own files %s...", stars_why_.c_str(),
                         stars_start::BeforeText(stars_then_));
      ImGui::Dummy(ImVec2(0, 8 * Dpi()));
      std::string ph;
      {
        std::lock_guard<std::mutex> lk(stars_job_->m);
        ph = stars_job_->phase;
      }
      ImGui::ProgressBar(stars_job_->frac.load(), ImVec2(-1, 0), ph.empty() ? "Starting..." : ph.c_str());
      ImGui::Dummy(ImVec2(0, 12 * Dpi()));
    } else if (!stars_fail_.empty()) {
      ImGui::TextWrapped("Your superstars could not be built into the game: %s.", stars_fail_.c_str());
      ImGui::TextWrapped("%s", stars_start::FailText(stars_then_));
      ImGui::Dummy(ImVec2(0, 12 * Dpi()));
      if (!stars_start::OffersWithout(stars_then_)) {  // S110 HELPERSTART: a helper run without the pack meets the save's notice: no start
        if (ModalMainButton("OK")) {
          stars_fail_.clear();
          ImGui::CloseCurrentPopup();
        }
      } else if (ModalMainButton("PLAY WITHOUT THEM")) {
        stars_fail_.clear();
        stars_next_ = WithoutStars(stars_next_);  // (review S1: the old pack must not load either)
        ImGui::CloseCurrentPopup();
        StartAfterStars();
      } else if (ModalButton("CANCEL")) {
        stars_fail_.clear();
        if (direct_after_precompile_) direct_after_precompile_ = false;
        ImGui::CloseCurrentPopup();
      }
      ImGui::NewLine();
    } else {
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }

  HANDLE precompile_proc_ = nullptr;
  Command precompile_next_;
  std::string precompile_next_msg_, precompile_state_;
  fs::path precompile_progress_;
  std::chrono::steady_clock::time_point precompile_t0_, precompile_all_t0_;
  int precompile_made_ = 0, precompile_total_ = 0, precompile_optimised_ = 0, precompile_pass_ = 1;
  bool precompile_skip_once_ = false, want_precompile_popup_ = false;

  // The game's own GRAPHICS screen (Options) writes the Upscaling / Resolution / Frame rate keys of this same file:
  // take them over when the game closes, or the next Play (and this window's own exit) would put the old values back.
  // Only the keys the game changed (the file differs from what this window wrote when it started the game) are taken:
  // a choice made in this window while the game was running is not undone.
  void ReloadGraphicsSettings() {
    if (!launch_graphics_set_) return;
    wwe2k14::gfxs::Choice c = launch_graphics_;
    const wwe2k14::gfxs::Present p = wwe2k14::gfxs::Parse(wwe2k14::gfxs::ReadFile(settings_path), c);
    if (p.upscale && c.upscale != launch_graphics_.upscale) s.upscale = c.upscale;
    if (p.resolution && c.resolution != launch_graphics_.resolution) s.resolution = c.resolution;
    // Session 54 (a ROG Ally tester: "whatever set from launcher doesn't get saved to game setting graphics or the
    // other way around"): the in-game 360p - 540p / 1080p choice too - before, the launcher's old value was saved over it.
    if ((p.resolution || p.draw_height) && (c.resolution != launch_graphics_.resolution ||
                                            c.draw_height != launch_graphics_.draw_height)) {
      s.resolution = c.resolution, s.draw_height = c.draw_height;
    }
    if (p.frame_rate && c.frame_rate != launch_graphics_.frame_rate) s.frame_rate = c.frame_rate;
    if (p.original_look && c.original_look != launch_graphics_.original_look) s.original_look = c.original_look;
    if (p.texture_filter && c.texture_filter != launch_graphics_.texture_filter) s.texture_filter = c.texture_filter;
    // S111 RAMPMENU: the pause menu's RAMP CAMERA (src\ramp_menu.h writes the key ramp_camera).
    bool ramp = false;
    if (wwe2k14::gfxs::GetBool(wwe2k14::gfxs::ReadFile(settings_path), "ramp_camera", ramp) && ramp != launch_ramp_camera_)
      s.ramp_camera = ramp;
    // Session 37: the game turned FSR off after a failure (src\upscale_guard.h): take its note too.
    std::string note;
    if (wwe2k14::gfxs::GetString(wwe2k14::gfxs::ReadFile(settings_path), "fsr_note", note) && !note.empty() &&
        note != s.fsr_note)
      s.fsr_note = note, Status("FSR could not run on this graphics card and was turned off: " + note);
    launch_graphics_set_ = false;
  }
  wwe2k14::gfxs::Choice launch_graphics_;  // the graphics choices as the game was started with them
  bool launch_graphics_set_ = false;
  bool launch_ramp_camera_ = false;  // S111 RAMPMENU: RAMP CAMERA as the game was started with it (ReloadGraphicsSettings)

  // Once per frame: the game (or a logo import) finished?
  void PollGame() {
    if (!game_proc_ || WaitForSingleObject(game_proc_, 0) != WAIT_OBJECT_0) return;
    DWORD code = 0;
    GetExitCodeProcess(game_proc_, &code);
    CloseHandle(game_proc_);
    game_proc_ = nullptr;
    if (importing_) {
      FinishLogoImport(code);
      return;
    }
    ReloadGraphicsSettings();
    last_exit_ = ExitCodeText(code) + " at " + NowStamp("%Y-%m-%d %H:%M");  // Session 58: for Report a problem
    if (bench_step_ >= 0) return BenchRunEnded(code);  // S102 U4: a benchmark scene's game closed
    if (perf_step_ >= 0) {  // Session 39: a performance test run (its exit code goes into the test's about.txt)
      if (code) Log("performance test run exited with " + ExitCodeText(code));
      PerfRunEnded(code);
      return;
    }
    if (check_running_) {  // Session 77: the determinism check (its zip)
      check_running_ = false;
      CheckEnded(code);
      return;
    }
    if (diag_running_) {  // Session 95: an arm of the diagnostic run
      DiagArmEnded(code);
      return;
    }
    if (code == kGameReportedErrorExitCode) {  // the game showed its own message first: no crash dialog
      Status("The game could not start (or stopped on an error it reported). Its log has the details.");
      return;
    }
    if (code != 0) {
      crash_code_ = code;
      want_crash_popup_ = true;
      Log("game exited with " + ExitCodeText(code));
    }
    // Session 101 (lane 199): the game's own verdict on the imported superstars (<mods>\superstars\game_status.txt), when it left
    // them out for a reason the check before PLAY could not see. One line; MODS > SUPERSTARS keeps saying it.
    // (review N4) only when this start had the switch on: the game writes the file only then
    if (stars_in_start_ && code == 0)
      if (const std::string why = importer::LastGameProblem(PathStr(paths_.mods)); !why.empty()) Status(why + " (MODS > SUPERSTARS)");
  }

  // Session 31: a game-style modal: a red header band with the title in the menu face (a slanted end and the game's
  // light rim), a dark panel below. Centred, `width` wide, as tall as its content. Pair with ImGui::EndPopup().
  bool BeginGameModal(const char* id, const char* title, float width) {
    const float d = Dpi(), head = 54 * d;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(width, vp->WorkSize.x - 40 * d), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(26 * d, 18 * d));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    const bool open = ImGui::BeginPopupModal(
        id, nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
    ImGui::PopStyleVar(2);
    if (!open) return false;
    const ImVec2 p = ImGui::GetWindowPos();
    const float w = ImGui::GetWindowWidth(), sk = 26 * d;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilledMultiColor(p, ImVec2(p.x + w, p.y + head), IM_COL32(176, 20, 28, 255), IM_COL32(150, 14, 22, 255),
                                IM_COL32(96, 8, 13, 255), IM_COL32(110, 10, 15, 255));
    const ImVec2 cut[3] = {{p.x + w - sk * 2, p.y + head}, {p.x + w, p.y}, {p.x + w, p.y + head}};
    dl->AddTriangleFilled(cut[0], cut[1], cut[2], ImGui::GetColorU32(ImGuiCol_PopupBg));  // the slanted end
    dl->AddLine(ImVec2(p.x + w - sk * 2, p.y + head), ImVec2(p.x + w, p.y), IM_COL32(255, 255, 255, 170), 1.6f * d);
    const float px = 26 * d;
    MenuText(dl, ImVec2(p.x + 26 * d, p.y + (head - px) / 2), px, IM_COL32_WHITE, title);
    ImGui::SetCursorPosY(head + 16 * d);
    return true;
  }

  // The modals' buttons: the main one slanted and red, the others plain, all on one row from the left.
  bool ModalMainButton(const char* label, bool enabled = true) {
    const float d = Dpi();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const bool r = SlantButton(label, at, ImVec2(std::max(200 * d, MenuTextSize(22 * d, label).x + 70 * d), 50 * d),
                               enabled, 22 * d);
    ImGui::SameLine(0, 14 * d);
    modal_button_y_ = ImGui::GetCursorPosY() + (50 * d - ImGui::GetFrameHeight()) / 2;  // centred on the main one
    return r;
  }
  bool ModalButton(const char* label) {
    ImGui::SetCursorPosY(modal_button_y_);
    const bool r = ImGui::Button(label, ImVec2(0, ImGui::GetFrameHeight()));
    ImGui::SameLine(0, 10 * Dpi());
    return r;
  }
  float modal_button_y_ = 0;

  void CrashPopup() {
    if (want_crash_popup_) ImGui::OpenPopup("crash"), want_crash_popup_ = false;
    if (BeginGameModal("crash", "THE GAME CLOSED UNEXPECTEDLY", 640 * Dpi())) {
      ImGui::Text("The game stopped with code %s.", ExitCodeText(crash_code_).c_str());
      ImGui::TextWrapped("Sorry about that. Trying again often works; a report helps us fix it.");
      ImGui::Dummy(ImVec2(0, 12 * Dpi()));
      if (ModalMainButton("TRY AGAIN")) {
        ImGui::CloseCurrentPopup();
        StartGame(last_cmd_, "Started the game again");
      }
      if (ModalButton("Report a problem")) {
        ImGui::CloseCurrentPopup();
        MakeReport();
      }
      if (ModalButton("Close")) ImGui::CloseCurrentPopup();
      ImGui::NewLine();
      ImGui::EndPopup();
    }
  }
