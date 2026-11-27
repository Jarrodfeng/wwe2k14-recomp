
  // ---------------- Session 95: DIAGNOSTIC RUN (src\diag_plan.h, src\diag.h, tools\native\diag_read.py).
  // Jarrod's ask (2026-11-06): one button, hands-free, about 3 minutes, one zip with as much as a run can capture: the device
  // (every Vulkan limit / feature / extension / format), the game's logs, pictures of fixed scenes and their hashes, frame
  // timing, and in-run A/Bs; then relaunch arms with one setting flipped. The run has a save folder of its own and a copy of
  // the settings file in its folder (nothing of the player's is read or written); the game's watchdog ends a stuck arm, this
  // launcher ends one that outlives its deadline, and a run that died is zipped at the next launcher start.
  bool diag_running_ = false, want_diag_popup_ = false, diag_long_ = false;
  fs::path diag_run_dir_, diag_zip_, diag_recover_dir_;
  std::vector<wwe2k14::diag_plan::Arm> diag_arms_;
  std::vector<std::string> diag_skipped_, diag_base_, diag_arm_log_;
  bool diag_failed_ = false;  // an arm's route stopped before "done" (the summary and the result screen say so)
  size_t diag_idx_ = 0;
  std::chrono::steady_clock::time_point diag_t0_, diag_arm_t0_;
  std::string diag_progress_;
  static constexpr int kDiagBudgetS = 180, kDiagLongBudgetS = 300;
  int DiagBudget() const { return diag_long_ ? kDiagLongBudgetS : kDiagBudgetS; }
  int DiagElapsed() const {
    return int(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - diag_t0_).count());
  }
  bool DiagAvailable() const { return gd_.ok && !game_proc_ && !precompile_proc_; }
  static std::string DiagJoin(const std::vector<std::string>& args) {
    std::string out;
    for (const auto& a : args) {
      const size_t eq = a.find('=');
      std::string one;
      if (a.rfind("--", 0) == 0 && eq != std::string::npos) one = a.substr(0, eq + 1) + Quote(a.substr(eq + 1));
      else one = Quote(a);
      out += (out.empty() ? "" : " ") + one;
    }
    return out;
  }
  static void DiagWriteText(const fs::path& p, const std::string& text) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << text;
  }
  void DiagState(const std::string& stage, bool finished = false) {
    std::string arms;
    for (const auto& a : diag_arms_) arms += (arms.empty() ? "\"" : ",\"") + a.id + "\"";
    std::string skipped;
    for (const auto& a : diag_skipped_) skipped += (skipped.empty() ? "\"" : ",\"") + a + "\"";
    DiagWriteText(diag_run_dir_ / "state_launcher.json",
                  "{\"finished\":" + std::string(finished ? "true" : "false") + ",\"stage\":\"" + stage +
                      "\",\"arm_index\":" + std::to_string(diag_idx_) + ",\"arms\":[" + arms + "],\"skipped\":[" +
                      skipped + "],\"elapsed_s\":" + std::to_string(DiagElapsed()) + ",\"launcher\":\"" + Version() +
                      "\"}\n");
  }
  void StartDiag() {
    if (game_proc_) return Status("Close the game first");
    namespace dp = wwe2k14::diag_plan;
    const fs::path root = LocalDataDir() / "diag";
    diag_run_dir_ = root / ("run_" + NowStamp("%Y%m%d_%H%M%S"));
    std::error_code ec;
    fs::create_directories(diag_run_dir_ / "save", ec);
    if (!ec) fs::create_directories(root / "cache", ec);
    if (ec) return Status("Could not make " + PathStr(diag_run_dir_) + ": " + ec.message());
    // The player's own flags (no scenario), then a settings copy of its own for the game to read.
    const Command plain = BuildCommand(PlainSettings(), paths_);
    diag_base_ = dp::SplitArgs(Narrow(plain.args));
    std::error_code cec;
    fs::copy_file(settings_path, diag_run_dir_ / "settings_copy.json", fs::copy_options::overwrite_existing, cec);
    diag_skipped_.clear();
    diag_arms_ = dp::PlanArms(diag_base_, &diag_skipped_);
    DiagWriteText(diag_run_dir_ / "route_full.txt", dp::Route(true));
    DiagWriteText(diag_run_dir_ / "route_entrance.txt", dp::Route(false));
    DiagWriteText(diag_run_dir_ / "route_vertex.txt", dp::Route(false, false, true));
    DiagWriteText(diag_run_dir_ / "system.txt",
                  std::string("WWE 2K14 recomp diagnostic run (Session 95)\nVersion: ") + Version() + "\nStarted: " +
                      NowStamp("%Y-%m-%d %H:%M:%S") + " (local time)\n\n" + HardwareText(hw_) + PcDetailsText() +
                      "\nGame files: " + (gd_.ok ? "found" : "missing") + "\n");
    DiagWriteText(diag_run_dir_ / "base_flags.txt", DiagJoin(diag_base_) + "\n");
    diag_idx_ = 0;
    diag_t0_ = std::chrono::steady_clock::now();
    diag_running_ = true;
    diag_zip_.clear();
    diag_arm_log_.clear();
    diag_failed_ = false;
    want_diag_popup_ = true;
    DiagState("start");
    DiagStartArm();
  }
  void DiagStartArm() {
    namespace dp = wwe2k14::diag_plan;
    const dp::Arm arm = diag_arms_[diag_idx_];
    const fs::path adir = diag_run_dir_ / ("arm" + arm.id);
    std::error_code ec;
    fs::create_directories(adir, ec);
    std::vector<std::string> args = dp::ArmArgs(arm, diag_base_);
    // (indexes 1 and 2 always run in full: the storage arm and the vertex truth are not cut by arm 0's time - review F9)
    const int arm_s = diag_idx_ <= 2 ? arm.est_s * 2 + 30 : std::max(30, std::min(arm.est_s * 2 + 30, DiagBudget() + 60 - DiagElapsed()));
    const std::string route = PathStr(diag_run_dir_ / dp::RouteFile(arm));
    for (const std::string& a : dp::DiagArgs(PathStr(diag_run_dir_), arm.id, route, arm_s, PathStr(adir / "device.json"))) {
      const size_t eq = a.find('=');
      dp::SetFlag(args, a.substr(2, eq - 2), a.substr(eq + 1));
    }
    dp::SetFlag(args, "user_data_root", PathStr(diag_run_dir_ / "save"));  // an empty save of its own: the route answers NO
    dp::SetFlag(args, "wwe2k14_settings_file", PathStr(diag_run_dir_ / "settings_copy.json"));
    if (arm.id != "0") dp::SetFlag(args, "cache_root", PathStr(LocalDataDir() / "diag" / "cache"));
    dp::RemoveFlag(args, "wwe2k14_wm30");
    dp::RemoveFlag(args, "wwe2k14_scenario_capture");
    Command c;
    c.exe = paths_.game_exe.wstring();
    c.workdir = paths_.launcher_dir.wstring();
    const std::string line = DiagJoin(args);
    c.args = Widen(line);
    c.display = Quote(PathStr(paths_.game_exe)) + " " + line;
    DiagWriteText(adir / "command.txt", Redact(c.display) + "\n");
    diag_progress_ = "Diagnostic run: " + arm.title + " (step " + std::to_string(diag_idx_ + 1) + " of " +
                     std::to_string(diag_arms_.size()) + ")";
    diag_arm_t0_ = std::chrono::steady_clock::now();
    DiagState("arm " + arm.id + " running");
    Log("diagnostic run: arm " + arm.id + " (" + arm.title + ") " + c.display);
    StartGame(c, diag_progress_);
    if (!game_proc_) {  // could not start
      diag_arm_log_.push_back("arm " + arm.id + ": could not start"), diag_failed_ = true;
      DiagNextArm();
    }
  }
  void DiagNextArm() {
    ++diag_idx_;
    while (diag_idx_ < diag_arms_.size()) {
      const auto& arm = diag_arms_[diag_idx_];
      // (the first two relaunch arms, indexes 1 and 2, always run: the storage arm and the vertex truth are never cut by arm 0's time - review F9)
      if (diag_idx_ > 2 && DiagElapsed() + arm.est_s > DiagBudget()) {
        diag_skipped_.push_back(arm.id + ": skipped, the time budget (" + std::to_string(DiagBudget()) + " s) would be passed");
        ++diag_idx_;
        continue;
      }
      return DiagStartArm();
    }
    DiagFinish();
  }
  // From PollGame, when an arm's game has closed.
  void DiagArmEnded(DWORD code) {
    namespace dp = wwe2k14::diag_plan;
    const auto arm = diag_arms_[diag_idx_];
    const fs::path adir = diag_run_dir_ / ("arm" + arm.id);
    std::error_code ec;
    // The arm's own game log and CSVs. Only its own: the newest run number above every number already collected in this run
    // folder (the next launch touches the older log, so the time window alone also took the arm before: zip 59.7 MB, run diag3).
    // The log is cut (src/diag_logcut.h: repeated line shapes folded) so one log costs well under a MB, not 12.
    namespace lc = wwe2k14::diag_logcut;
    const auto ran = std::chrono::steady_clock::now() - diag_arm_t0_;
    const auto start_ft = fs::file_time_type::clock::now() -
                          std::chrono::duration_cast<fs::file_time_type::duration>(ran + std::chrono::seconds(5));
    long have = -1;  // the highest number already in the run folder
    for (const auto& e : fs::recursive_directory_iterator(diag_run_dir_, ec))
      if (e.is_regular_file()) have = std::max(have, lc::NumberOf(PathStr(e.path().filename())));
    long mine = -1;  // this arm's number: the newest wwe2k14_N.log written during the arm
    for (const auto& e : fs::directory_iterator(paths_.logs, ec)) {
      const std::string n = PathStr(e.path().filename());
      const long num = lc::NumberOf(n);
      if (n.rfind("wwe2k14_", 0) == 0 && e.is_regular_file() && num > have && e.last_write_time(ec) >= start_ft) mine = std::max(mine, num);
    }
    for (const auto& e : fs::directory_iterator(paths_.logs, ec)) {
      const std::string n = PathStr(e.path().filename());
      const long num = lc::NumberOf(n);
      const bool numbered = (n.rfind("wwe2k14_", 0) == 0 && e.path().extension() == ".log") || n.rfind("frames_", 0) == 0;
      const bool other = n.rfind("timeline_", 0) == 0 || n == "startup.txt" || n == "last_game_exit.txt";
      if (!e.is_regular_file() || !(numbered || other)) continue;
      if (numbered ? num != mine : e.last_write_time(ec) < start_ft) continue;
      std::string data = Redact(ReadTail(e.path(), 64u << 20));
      if (e.path().extension() == ".log") data = lc::Cut(data);
      DiagWriteText(adir / n, data);
    }
    const std::string exit = ExitCodeText(code);
    // Did the route finish? The game's stages.log ends on "done"; run 6629's arms all stopped at "boot" in 3 s with exit 0x0.
    const long secs = long(std::chrono::duration_cast<std::chrono::seconds>(ran).count());
    const bool cancelled = code == 6;  // the player's Cancel (TerminateProcess code 6)
    const std::string failure = cancelled ? std::string() : dp::RouteFailure(ReadTail(adir / "stages.log", 1u << 20), secs);
    const std::string verdict = cancelled ? "cancelled by the player" : failure.empty() ? "route reached done" : failure;
    DiagWriteText(adir / "exit.txt", exit + "\n" + verdict + "\nelapsed_s " + std::to_string(secs) + "\n");
    if (!failure.empty()) diag_failed_ = true;
    diag_arm_log_.push_back("arm " + arm.id + " (" + arm.title + "): " + exit + ", " + verdict);
    Log("diagnostic run: arm " + arm.id + " ended: " + exit + ", " + verdict);
    DiagNextArm();
  }
  // Once per frame: a stuck arm is ended here too (the game's own watchdog normally does it first).
  void DiagTick() {
    static bool looked = false;
    if (!looked) {  // once, at the first frame: a run that never finished is offered as a zip
      looked = true;
      DiagLookForUnfinished();
      if (!diag_recover_dir_.empty() && steps.empty()) want_diag_popup_ = true;
    }
    if (!diag_running_ || !game_proc_ || diag_idx_ >= diag_arms_.size()) return;
    const int arm_s = std::max(30, diag_arms_[diag_idx_].est_s * 2 + 30);
    const int ran = int(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - diag_arm_t0_).count());
    if (ran > arm_s + 40 || DiagElapsed() > kDiagLongBudgetS + 120) {
      Log("diagnostic run: ending the stuck game (" + std::to_string(ran) + " s)");
      TerminateProcess(game_proc_, 5);
    }
  }
  void DiagFinish() {
    diag_running_ = false;
    std::string t;
    if (diag_failed_) t += "RESULT: THE ROUTE FAILED in at least one arm (the lines below name the stage it stopped at). The pictures and numbers are incomplete.\n";
    for (const auto& l : diag_arm_log_) t += l + "\n";
    for (const auto& l : diag_skipped_) t += "skipped " + l + "\n";
    DiagWriteText(diag_run_dir_ / "arm_summary.txt", t);
    DiagWriteText(diag_run_dir_ / "system_end.txt", "Ended: " + NowStamp("%Y-%m-%d %H:%M:%S") + "\nelapsed_s " +
                                                      std::to_string(DiagElapsed()) + "\n");
    DiagState("finished", true);
    diag_zip_ = DiagMakeZip(diag_run_dir_);
    want_diag_popup_ = true;
    if (!diag_zip_.empty()) Status("Diagnostic run saved: " + PathStr(diag_zip_));
  }
  // The run folder into one zip in reports (pictures and text; the save and the cache stay out).
  fs::path DiagMakeZip(const fs::path& run) {
    fs::path dir = paths_.launcher_dir / "reports";
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) dir = LocalDataDir() / "reports", fs::create_directories(dir, ec);
    const fs::path zip = dir / ("diagnostic_run_" + NowStamp("%Y%m%d_%H%M") + ".zip");
    ZipWriter z(zip);
    if (!z.ok()) {
      Status("Could not write " + PathStr(zip));
      return {};
    }
    uint64_t total = 0;
    for (const auto& e : fs::recursive_directory_iterator(run, ec)) {
      if (!e.is_regular_file()) continue;
      const std::string name = fs::relative(e.path(), run, ec).generic_string();
      if (name.rfind("save/", 0) == 0 || name.rfind("cache/", 0) == 0) continue;
      const std::string ext = Lower(PathStr(e.path().extension()));
      if (ext == ".rgba" || ext == ".part") continue;  // (a sheet the game never turned into a JPEG: the run was cut there)
      std::string data = ReadTail(e.path(), 16u << 20);
      if (ext == ".txt" || ext == ".json" || ext == ".jsonl" || ext == ".csv" || ext == ".log" || ext == ".tsv")
        data = Redact(data);
      total += data.size();
      z.Add(name, data);
    }
    z.Add("about.txt", Redact(std::string("WWE 2K14 recomp diagnostic run (Session 95)\nVersion: ") + Version() + "\nMade: " +
                              NowStamp("%Y-%m-%d %H:%M") + " (local time)\n\nRead with: py -I tools\\native\\diag_read.py THIS.zip\n"
                              "The pictures in shots/ are small pictures of the game, made on this PC; they are in this zip only.\n"));
    if (!z.Close()) {
      Status("Could not finish " + PathStr(zip));
      return {};
    }
    Log("diagnostic run: zip " + PathStr(zip) + " (" + std::to_string(total) + " bytes of content)");
    fs::remove_all(run, ec);  // done: the folder (its empty save and the copies now in the zip)
    return zip;
  }
  // At the launcher's start: a run folder that never finished (the game or the launcher died) is offered as a zip.
  void DiagLookForUnfinished() {
    std::error_code ec;
    const fs::path root = LocalDataDir() / "diag";
    if (!fs::is_directory(root, ec)) return;
    // Review F21: the newest unfinished run is offered (the folder names sort by time); older unfinished ones are removed.
    for (const auto& e : fs::directory_iterator(root, ec)) {
      if (!e.is_directory() || PathStr(e.path().filename()).rfind("run_", 0) != 0) continue;
      const std::string st = ReadTail(e.path() / "state_launcher.json", 4096);
      if (st.find("\"finished\":true") != std::string::npos) {
        fs::remove_all(e.path(), ec);
      } else if (diag_recover_dir_.empty() || e.path().filename() > diag_recover_dir_.filename()) {
        if (!diag_recover_dir_.empty()) fs::remove_all(diag_recover_dir_, ec);
        diag_recover_dir_ = e.path();
      } else {
        fs::remove_all(e.path(), ec);
      }
    }
  }
  void DiagPopup() {
    if (want_diag_popup_) ImGui::OpenPopup("diag_run"), want_diag_popup_ = false;
    if (!BeginGameModal("diag_run", "DIAGNOSTIC RUN", 700 * Dpi())) return;
    if (diag_running_) {
      ImGui::Text("%s", diag_progress_.c_str());
      ImGui::Spacing();
      ImGui::TextWrapped("%d s of about %d s. The game plays a short Quick Match by itself and closes by itself between "
                         "steps. Please don't touch the controller, mouse or keyboard.", DiagElapsed(), DiagBudget());
      if (ModalButton("Cancel and keep what was captured")) {
        diag_arms_.resize(std::min(diag_arms_.size(), diag_idx_ + 1));  // this arm is the last; its end builds the zip
        if (game_proc_) TerminateProcess(game_proc_, 6);
      }
    } else if (!diag_zip_.empty()) {
      if (diag_failed_) {
        ImGui::TextColored(kBad, "The scripted run did NOT complete. It is still worth sending, but it holds little.");
        ImGui::TextWrapped("Please send this file:");
      } else {
        ImGui::TextWrapped("Done, thank you! Please send this file:");
      }
      ImGui::TextColored(kGood, "%s", PathStr(diag_zip_).c_str());
      for (const auto& l : diag_arm_log_) ImGui::TextWrapped("%s", l.c_str());
      ImGui::Dummy(ImVec2(0, 12 * Dpi()));
      if (ModalMainButton("OPEN THE FOLDER")) {
        const std::wstring arg = L"/select,\"" + diag_zip_.wstring() + L"\"";
        ShellExecuteW(hwnd_, nullptr, L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
      }
      if (ModalButton("Another run")) diag_zip_.clear();  // review F20: the START branch is reachable again
      if (ModalButton("Close")) ImGui::CloseCurrentPopup();
    } else {
      ImGui::TextColored(kGood, "The game will look strange during the run (flat grey or black scenes): that is on purpose.");
      ImGui::TextWrapped("Plays a short scripted Quick Match by itself (about 3 minutes) and saves everything it can learn "
                         "about this PC and how the game draws: the graphics card's limits, the game's logs, frame timing "
                         "and small pictures of a few scenes (the menu, an entrance, the match, the pause menu), then "
                         "repeats the entrance with one setting changed. It saves one .zip for you to send. Please don't "
                         "touch anything while it runs. Your saves and your settings are not touched. The zip holds small "
                         "pictures of the game.");
      ImGui::Dummy(ImVec2(0, 8 * Dpi()));
      ImGui::Checkbox("LONG RUN (about 5 minutes, every comparison)", &diag_long_);
      ImGui::Dummy(ImVec2(0, 8 * Dpi()));
      if (ModalMainButton("START", DiagAvailable())) StartDiag();
      if (!diag_recover_dir_.empty()) {
        ImGui::TextWrapped("An earlier diagnostic run did not finish.");
        if (ModalButton("MAKE A ZIP OF WHAT IT CAPTURED")) {
          DiagWriteText(diag_recover_dir_ / "recovered.txt",
                        "This run did not finish (the game or the launcher died): its arms' game logs were never collected "
                        "here, only what the game wrote into the run folder itself.\n");
          diag_zip_ = DiagMakeZip(diag_recover_dir_);
          diag_recover_dir_.clear();
        }
      }
      if (ModalButton("Not now")) ImGui::CloseCurrentPopup();
      if (!gd_.ok) ImGui::TextColored(kBad, "Set up your game files first (Settings).");
    }
    ImGui::NewLine();
    ImGui::EndPopup();
  }

  void PerfTestPopup() {
    // Offered once when the launcher opens (not to automation runs, which start it with --perf-test if they want it).
    if (PerfTestAvailable() && !perf_offered_) perf_offered_ = true, want_perf_popup_ = steps.empty();
    if (want_perf_popup_) ImGui::OpenPopup("perf_test"), want_perf_popup_ = false;
    if (!BeginGameModal("perf_test", "PERFORMANCE TEST", 680 * Dpi())) return;
    if (perf_step_ >= 0) {
      ImGui::Text("The test is running.");
      ImGui::Spacing();
      ImGui::TextWrapped("The game starts, plays two entrances and the start of a match by itself, and closes; "
                         "then it does the same once more with another renderer (15 to 25 minutes in all on a "
                         "laptop). Please leave the PC alone until then; don't touch the controller or keyboard "
                         "while the game runs.");
      Note("To stop early: close the game (Alt+F4); what ran so far is still saved.");
    } else if (!perf_zip_.empty()) {
      ImGui::TextWrapped("Done, thank you! Please send this file:");
      ImGui::TextColored(kGood, "%s", PathStr(perf_zip_).c_str());
      ImGui::Dummy(ImVec2(0, 12 * Dpi()));
      if (ModalMainButton("OPEN THE FOLDER")) {
        const std::wstring arg = L"/select,\"" + perf_zip_.wstring() + L"\"";
        ShellExecuteW(hwnd_, nullptr, L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
      }
      if (ModalButton("Close")) ImGui::CloseCurrentPopup();
    } else {
      ImGui::TextWrapped("This test build measures where your graphics card spends its time. It plays the same "
                         "Quick Match twice by itself, with two renderers (15 to 25 minutes in all on a laptop), and "
                         "then saves one .zip for you to send back. Your settings stay as they are.");
      ImGui::Spacing();
      Note("Before you start: plug the laptop in, close other programs, and leave it alone while it runs.");
      ImGui::Dummy(ImVec2(0, 12 * Dpi()));
      if (ModalMainButton("START THE TEST", gd_.ok && !game_proc_)) StartPerfTest();
      if (ModalButton("Not now")) ImGui::CloseCurrentPopup();
      if (!gd_.ok) ImGui::TextColored(kBad, "Set up your game files first (Settings).");
    }
    ImGui::NewLine();
    ImGui::EndPopup();
  }
