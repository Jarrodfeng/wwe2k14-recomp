
  // ---------------- Session 39: the performance test (the native renderer on vs off, for a playtester's laptop)
  // Offered only when the test route native_ab.txt is next to the game (only a test build ships it). One game: a
  // Quick Match the route drives by itself (Lesnar vs Bryan, entrances on, both CPU; the native renderer switched on
  // and off every 120 frames through both entrances and the start of the match) on the accurate renderer, which then
  // quits. Its choices are its own flags: the player's saved settings are not changed. Then one .zip of the run's log
  // and frame times (user name removed) to send back; tools\native\laptop_ab.py reads it. (The machinery runs a list
  // of runs; one is enough, Jarrod 2026-10-20.)
  struct PerfRun {
    int renderer;
    const char* label;
    const char* script;  // order A (native on first) or B: alternating windows show different shots, A and B cancel
    std::string log;     // the run's game log (its frames_NNN.csv has the same NNN)
    DWORD code = 0;
  };
  std::vector<PerfRun> perf_runs_;
  int perf_step_ = -1;  // the run going now; -1 = no test running
  bool perf_stop_ = false, want_perf_popup_ = false, perf_offered_ = false;
  fs::path perf_zip_;

  // Session 41 (low-spec step 0, docs\native\LOWSPEC_PLAN.md): the test is now the per-pass breakdown. One button, two
  // games in a row: the LOW preset (NATIVE: the accurate renderer with the native renderer, fast clears; 720p, frame
  // rate 30), then the light renderer with the same settings. The route (perf_test.txt, from
  // tools\input_scripts\short\s41_perf_breakdown.txt) takes frame-anatomy captures with GPU timestamps per pass at the
  // main menu, Bryan's entrance and the match, and switches the fast clears on and off through the entrances. The zip
  // gets the anatomy files too; tools\native\laptop_breakdown.py reads it.
  static constexpr const char* kPerfRoute = "perf_test.txt";
  bool PerfTestAvailable() const {
    std::error_code ec;
    return fs::exists(paths_.game_exe.parent_path() / kPerfRoute, ec);
  }
  void StartPerfTest() {
    if (game_proc_) return Status("Close the game first");
    perf_runs_ = {{2, "LOW preset (native renderer)", kPerfRoute}, {1, "light renderer", kPerfRoute}};
    perf_step_ = 0, perf_stop_ = false, perf_zip_.clear();
    StartPerfRun();
  }
  void StartPerfRun() {
    Settings t = PlainSettings();
    t.renderer = perf_runs_[perf_step_].renderer;
    // The LOW preset's resolution and frame rate for both games (only the renderer differs), no upscaler.
    t.resolution = 0, t.draw_height = 0, t.frame_rate = 3, t.refresh_hz = 0, t.upscale = 0, t.original_look = false;
    t.shadows = t.depth_of_field = true, t.crowd_density = 100;  // Session 85: test results compare like with like
    t.quality_tier = 2;  // Session 93: the test runs STANDARD whatever the card (AUTO would pick LOW on integrated graphics)
    t.true_native = true;  // Session 87: the performance test runs the default renderer (Session 93: TRUE NATIVE)
    t.mods = false, t.cinecam = t.photo_mode = t.runin = false, t.perf_report = 0;
    t.ramp_camera = false;  // S111 RAMPMENU review: the hard cam's own side (the ramp camera draws another view: numbers like with like)
    t.scenario = "@quick", t.quick_type = 0, t.quick_ids = "343,324", t.quick_cpu_only = true, t.quick_arena = -1;
    t.skip_entrances = false;
    // (Session 60: without the match's faster fast-forward vblank - the numbers stay comparable with earlier builds'
    // tests, and its precise timer does not take a core from a weak laptop.)
    t.extra = std::string("--wwe2k14_input_script=") + perf_runs_[perf_step_].script +
              " --wwe2k14_file_trace=true --vsync=false --wwe2k14_ff_vblank_hz=0";
    StartGame(BuildCommand(t, paths_), "Performance test: run " + std::to_string(perf_step_ + 1) + " of " +
                                           std::to_string(perf_runs_.size()) + " started");
    if (!game_proc_) perf_step_ = -1;  // could not start: Status says why
  }
  // From PollGame, when a test run's game has closed.
  void PerfRunEnded(DWORD code) {
    PerfRun& r = perf_runs_[perf_step_];
    r.code = code;
    const fs::path log = Newest(paths_.logs, "wwe2k14_", ".log");
    r.log = log.empty() ? std::string() : PathStr(log.filename());
    if (!perf_stop_ && perf_step_ + 1 < int(perf_runs_.size())) {
      ++perf_step_;
      StartPerfRun();
      return;
    }
    perf_step_ = -1;
    FinishPerfTest();
  }
  void FinishPerfTest() {
    fs::path dir = paths_.launcher_dir / "reports";
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) dir = LocalDataDir() / "reports", fs::create_directories(dir, ec);
    const fs::path zip = dir / ("performance_test_" + NowStamp("%Y%m%d_%H%M") + ".zip");
    ZipWriter z(zip);
    if (!z.ok()) return Status("Could not write " + PathStr(zip));
    std::string about = std::string("WWE 2K14 recomp performance test (Session 41: per-pass breakdown, LOW preset and "
                                    "light renderer)\nVersion: ") +
                        Version() + "\nMade: " + NowStamp("%Y-%m-%d %H:%M") + " (local time)\n\n" + HardwareText(hw_) +
                        "\nTest settings: 720p, frame rate 30 (the LOW preset's), no upscaler; the player's own "
                        "resolution: " +
                        ResolutionName(s) + "\n\n";
    for (size_t i = 0; i < perf_runs_.size(); ++i) {
      const PerfRun& r = perf_runs_[i];
      about += "Run " + std::to_string(i + 1) + ": " + r.label + ", log " + (r.log.empty() ? "(none)" : r.log) +
               ", exit " + (r.log.empty() && !r.code ? std::string("(not run)") : ExitCodeText(r.code)) + "\n";
      if (r.log.empty()) continue;
      z.Add(r.log, Redact(ReadTail(paths_.logs / Widen(r.log), 48u << 20)));
      const std::string number = r.log.substr(8, r.log.size() - 8 - 4);  // wwe2k14_NNN.log
      const std::string frames = "frames_" + number + ".csv";
      if (fs::exists(paths_.logs / Widen(frames), ec)) z.Add(frames, ReadTail(paths_.logs / Widen(frames), 16u << 20));
      // Session 41: the run's frame-anatomy captures (anatomy_NNN_K.csv trimmed to the columns the breakdown reads;
      // its _gpu, _gpu_frames and _frames files whole). Numbers only (shader hashes, registers, times).
      const std::string prefix = "anatomy_" + number + "_";
      for (const auto& e : fs::directory_iterator(paths_.logs, ec)) {
        const std::string name = PathStr(e.path().filename());
        if (name.rfind(prefix, 0) != 0 || e.path().extension() != ".csv") continue;
        const bool main_csv = name.find_first_not_of("0123456789", prefix.size()) == name.size() - 4;
        std::string data = ReadTail(e.path(), 64u << 20);
        if (main_csv) data = AnatomyColumns(data);
        if (name.find("_texloads") == std::string::npos) z.Add(name, data);
      }
    }
    z.Add("about.txt", Redact(about));
    if (!z.Close()) return Status("Could not finish " + PathStr(zip));
    perf_zip_ = zip;
    want_perf_popup_ = true;
    Status("Performance test saved: " + PathStr(zip));
    Log("performance test saved " + PathStr(zip));
  }
  // Session 41: a frame-anatomy CSV with only the columns tools\native\laptop_breakdown.py reads (the zip is stored
  // uncompressed; the full file is ~3 MB per capture). Rows whose field count differs from the header's are left out.
  static std::string AnatomyColumns(const std::string& csv) {
    static const char* kKeep[] = {"frame",          "idx",           "kind",          "cp_us",         "vs_hash",
                                  "ps_hash",        "prim",          "count",         "rb_surface_info",
                                  "rb_color_info0", "rb_color_info1", "rb_color_info2", "rb_color_info3",
                                  "rb_depth_info",  "rb_modecontrol", "rb_depthcontrol", "rb_color_mask", "skipped",
                                  "copy_dest",      "copy_info",     "dispatches",    "barrier_batches",
                                  "fsi_edram_barriers"};
    auto split = [](const std::string& line) {
      std::vector<std::string> f;
      size_t a = 0;
      for (size_t b; (b = line.find(',', a)) != std::string::npos; a = b + 1) f.push_back(line.substr(a, b - a));
      f.push_back(line.substr(a));
      return f;
    };
    std::string out;
    std::vector<size_t> cols;
    size_t header_fields = 0;
    size_t pos = 0;
    while (pos < csv.size()) {
      size_t end = csv.find('\n', pos);
      if (end == std::string::npos) end = csv.size();
      std::string line = csv.substr(pos, end - pos);
      pos = end + 1;
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.empty()) continue;
      const std::vector<std::string> f = split(line);
      if (!header_fields) {
        header_fields = f.size();
        for (const char* k : kKeep)
          for (size_t i = 0; i < f.size(); ++i)
            if (f[i] == k) cols.push_back(i);
      } else if (f.size() != header_fields) {
        continue;
      }
      for (size_t i = 0; i < cols.size(); ++i) out += (i ? "," : "") + f[cols[i]];
      out += '\n';
    }
    return out;
  }

  // ---------------- S102 U4: the benchmark button. Three short games in a row (Brock's entrance, Bryan's, the match; the Surface bench's per-scene
  // routes launched the way the shipped performance test is, "@quick" on Over the Limit; scene list, judging and numbers in bench_button.h) at the player's OWN
  // settings, on a copy of the save and of the settings file (LocalDataDir()\bench). Then benchmark_<stamp>.txt / .json in reports\ (and benchmark_latest.* that the
  // report zip picks up). No popup: Status says where the file is. Offered only when bench\ is shipped next to the exe (bench_button.h RequiredFiles()).
  // A scene that runs past kSceneLimitSeconds is ended (NO RESULT); STOP ends the game and the benchmark; a game closed by the player (or crashed) before the route's
  // `bench: done` ends the benchmark; earlier scenes keep their numbers.
  struct BenchRun {
    launcher::bench::SceneResult result;
    std::string device, tier;
    bool ran = false;
  };
  int bench_step_ = -1;  // the scene going now; -1 = no benchmark running
  std::vector<BenchRun> bench_runs_;
  long bench_prev_log_ = -1;
  bool bench_stop_ = false, bench_timed_out_ = false;
  std::chrono::steady_clock::time_point bench_t0_;
  fs::path ReportsDir() const {  // the one place the benchmark files are written and Report a problem reads them (MakeReport falls back the same way)
    fs::path dir = paths_.launcher_dir / "reports";
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) dir = LocalDataDir() / "reports", fs::create_directories(dir, ec);
    return dir;
  }
  bool BenchAvailable() const { return gd_.ok && launcher::bench::MissingFiles(paths_.game_exe.parent_path()).empty(); }
  void StartBench() {
    if (game_proc_) return Status("Close the game first");
    const fs::path root = LocalDataDir() / "bench";
    std::error_code ec;
    fs::create_directories(root, ec);
    if (!fs::exists(root / "save", ec)) {  // once: a copy of the player's save, so the benchmark never touches the real one
      fs::create_directories(root / "save", ec);
      if (fs::exists(paths_.user_data, ec)) fs::copy(paths_.user_data, root / "save", fs::copy_options::recursive | fs::copy_options::skip_existing, ec);
      if (ec) {
        fs::remove_all(root / "save", ec);
        return Status("Could not copy the save folder for the benchmark: " + ec.message());
      }
    }
    fs::copy_file(settings_path, root / "settings_copy.json", fs::copy_options::overwrite_existing, ec);  // the game reads this copy (a missing original: it runs on defaults)
    bench_runs_.assign(launcher::bench::Scenes().size(), BenchRun{});
    bench_step_ = 0, bench_stop_ = false, bench_timed_out_ = false;
    StartBenchRun();
  }
  void StartBenchRun() {
    namespace bn = launcher::bench;
    const bn::Scene& sc = bn::Scenes()[size_t(bench_step_)];
    const fs::path root = LocalDataDir() / "bench";
    Settings t = PlainSettings();  // the player's own resolution, tier and options: the number is what this player gets
    t.mods = t.lighting = t.superstars = t.discord = t.permanent_blood = false;
    t.cinecam = t.photo_mode = t.runin = false, t.perf_report = 0;
    t.scenario = "@quick", t.quick_type = 0, t.quick_ids = sc.quick_ids, t.quick_cpu_only = true, t.quick_arena = bn::kArenaOverTheLimit, t.skip_entrances = sc.skip_entrances;
    t.user_data_dir = PathStr(root / "save");
    t.extra = s.extra + (s.extra.empty() ? "" : " ") + sc.flags + " --wwe2k14_input_script=" + Quote(PathStr(paths_.game_exe.parent_path() / sc.route)) +
              " --wwe2k14_settings_file=" + Quote(PathStr(root / "settings_copy.json"));
    const fs::path before = Newest(paths_.logs, "wwe2k14_", ".log");
    bench_prev_log_ = before.empty() ? -1 : bn::LogNumber(PathStr(before.filename()));
    bench_timed_out_ = false;
    bench_t0_ = std::chrono::steady_clock::now();
    StartGame(BuildCommand(t, paths_), std::string("Benchmark ") + std::to_string(bench_step_ + 1) + " of " + std::to_string(bench_runs_.size()) + ": " + sc.label);
    if (!game_proc_) {  // could not start: Status says why
      bench_runs_[size_t(bench_step_)].result = bn::SceneResult{sc.id, sc.label, "the game could not be started"};
      bench_runs_[size_t(bench_step_)].ran = true;
      bench_stop_ = true;
      bench_step_ = -1;
      FinishBench();
    }
  }
  void StopBench() {  // the STOP button: ends the game and the benchmark; the scene going gets NO RESULT, earlier ones keep their numbers
    bench_stop_ = true;
    if (game_proc_) TerminateProcess(game_proc_, 6);
  }
  void BenchTick() {  // every frame: the wall-clock limit of a scene
    if (bench_step_ < 0 || !game_proc_ || bench_timed_out_) return;
    const double ran = std::chrono::duration<double>(std::chrono::steady_clock::now() - bench_t0_).count();
    if (launcher::bench::OverLimit(ran)) {
      Log("benchmark: ending a scene that ran " + std::to_string(int(ran)) + " s");
      bench_timed_out_ = true;
      TerminateProcess(game_proc_, 7);
    }
  }
  void BenchRunEnded(DWORD code) {
    namespace bn = launcher::bench;
    const bn::Scene& sc = bn::Scenes()[size_t(bench_step_)];
    BenchRun& r = bench_runs_[size_t(bench_step_)];
    r.ran = true;
    const fs::path log = Newest(paths_.logs, "wwe2k14_", ".log");
    const std::string log_name = log.empty() ? std::string() : PathStr(log.filename());
    const bool is_new = !log_name.empty() && bn::LogIsNew(bench_prev_log_, log_name);
    std::string log_text, csv_text;
    if (is_new) {
      log_text = ReadTail(log, 48u << 20);
      csv_text = ReadTail(paths_.logs / Widen("frames_" + log_name.substr(8, log_name.size() - 8 - 4) + ".csv"), 64u << 20);
    }
    r.result = bn::MeasureRun(sc.id, sc.label, csv_text, log_text, bench_timed_out_, is_new);
    if (!r.result.error.empty() && !bench_timed_out_ && !bench_stop_) r.result.error += " (exit " + ExitCodeText(code) + ")";
    if (bench_stop_) r.result.error = r.result.error.empty() ? std::string("stopped by the player") : "stopped by the player (" + r.result.error + ")";
    r.device = bn::DeviceFromLog(log_text), r.tier = bn::TierFromLog(log_text);
    if (code) Log("benchmark scene exited with " + ExitCodeText(code));
    // The benchmark goes on only after a scene the route finished or our own time limit ended; a game that ended by itself without 'bench: done' (closed by the player,
    // crashed, a route step failed) or a STOP ends it.
    const bool go_on = !bench_stop_ && (bench_timed_out_ || bn::HasDone(log_text)) && size_t(bench_step_) + 1 < bench_runs_.size();
    bench_timed_out_ = false;
    if (go_on) {
      ++bench_step_;
      StartBenchRun();
      return;
    }
    bench_step_ = -1;
    FinishBench();
  }
  void FinishBench() {
    namespace bn = launcher::bench;
    std::vector<bn::SceneResult> results;
    std::string device, tier;
    for (size_t i = 0; i < bench_runs_.size(); ++i) {
      bn::SceneResult res = bench_runs_[i].result;
      if (!bench_runs_[i].ran) res = bn::SceneResult{bn::Scenes()[i].id, bn::Scenes()[i].label, "not run (the benchmark stopped before this scene)"};
      if (device.empty()) device = bench_runs_[i].device;
      if (tier.empty()) tier = bench_runs_[i].tier;
      results.push_back(res);
    }
    bn::Info info;
    info.version = Version(), info.made = NowStamp("%Y-%m-%d %H:%M"), info.gpu = hw_.gpu + " (" + std::to_string(hw_.gpu_vram_mb) + " MB)", info.driver_and_api = device;
    info.cpu = hw_.cpu, info.ram = std::to_string(int(hw_.ram_gb + 0.5)) + " GB", info.windows = hw_.windows, info.resolution = ResolutionName(s);
    static const char* kTiers[] = {"AUTO", "LOW", "STANDARD"};
    info.tier = tier.empty() ? std::string("setting ") + kTiers[std::clamp(s.quality_tier, 0, 2)] + " (the game's applied line was not found)" : tier + " (setting " + kTiers[std::clamp(s.quality_tier, 0, 2)] + ")";
    const fs::path dir = ReportsDir();
    const std::string text = Redact(bn::ResultsText(info, results)), json = Redact(bn::ResultsJson(info, results));
    const std::string stamp = NowStamp("%Y%m%d_%H%M");
    for (const auto& [name, data] : {std::pair<std::string, std::string>{"benchmark_" + stamp + ".txt", text}, {"benchmark_latest.txt", text},
                                     {"benchmark_" + stamp + ".json", json}, {"benchmark_latest.json", json}}) {
      std::ofstream out(dir / Widen(name), std::ios::binary);
      out.write(data.data(), std::streamsize(data.size()));
    }
    Status("Benchmark saved: " + PathStr(dir / Widen("benchmark_" + stamp + ".txt")) + " (Report a problem adds it to the zip)");
    Log("benchmark saved " + PathStr(dir / Widen("benchmark_" + stamp + ".txt")));
    if (open_folders_) {
      const std::wstring arg = L"/select,\"" + (dir / Widen("benchmark_" + stamp + ".txt")).wstring() + L"\"";
      ShellExecuteW(hwnd_, nullptr, L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
    }
  }

  // ---------------- Session 77 (XDET_PLAN.md "P7 testers", step 3): the determinism check. Jarrod's interview
  // (2026-10-31 ~15:30): phone and PC testers, a button with the help links, the match shown, the results in a zip the
  // tester sends, compared at my end against a PC play of the same release; in every build. One game: the recording
  // xdet\xdet_check.rec with the route xdet\check_route.txt (both shipped next to the exe; Cena vs Undertaker, a
  // Quick Match from a fresh save), --wwe2k14_xdet_check (src\xdet_check.h: every input a tester's machine or settings
  // could change, pinned), an empty save folder of its own (the tester's save is never read or written: the route
  // answers NO to creating save data) and a shader cache of its own that later checks reuse. Its own short command
  // line, not GameFlags: the player's settings stay out, and a second --user_data_root would repeat a flag (a
  // repeated flag drops every startup flag after it). Then one .zip in reports: the hash CSV (one hash per memory
  // region per frame: no game data), the scheduler trace and the run's log, the user name removed;
  // tools\determinism\det_compare.py --cross REFERENCE ZIP compares it.
  static constexpr const char* kCheckDir = "xdet";
  bool check_running_ = false, want_check_popup_ = false;
  fs::path check_run_dir_, check_zip_;
  bool CheckAvailable() const {
    std::error_code ec;
    const fs::path dir = paths_.game_exe.parent_path() / kCheckDir;
    return fs::exists(dir / "xdet_check.rec", ec) && fs::exists(dir / "check_route.txt", ec);
  }
  void StartCheck() {
    if (game_proc_) return Status("Close the game first");
    const fs::path root = LocalDataDir() / "xdet_check";
    check_run_dir_ = root / ("run_" + NowStamp("%Y%m%d_%H%M%S"));
    std::error_code ec;
    fs::create_directories(check_run_dir_ / "save", ec);
    if (!ec) fs::create_directories(check_run_dir_ / "out", ec);
    if (!ec) fs::create_directories(root / "cache", ec);
    if (ec) return Status("Could not make " + PathStr(check_run_dir_) + ": " + ec.message());
    const fs::path ship = paths_.game_exe.parent_path() / kCheckDir;
    const std::string out = PathStr(check_run_dir_ / "out");
    std::string a = "--wwe2k14_xdet_check=true";
    if (!s.game_data_dir.empty()) a += " --game_data_root=" + Quote(s.game_data_dir);
    a += " --user_data_root=" + Quote(PathStr(check_run_dir_ / "save"));
    a += " --cache_root=" + Quote(PathStr(root / "cache"));
    a += " --wwe2k14_settings_file=" + Quote(PathStr(check_run_dir_ / "settings.json"));  // none: not the player's
    a += " --wwe2k14_replay_dir=" + Quote(PathStr(ship)) + " --wwe2k14_replay_play=xdet_check";
    a += " --wwe2k14_input_script=" + Quote(PathStr(ship / "check_route.txt"));
    a += " --det_serial_trace=" + Quote(out) + " --wwe2k14_det_hash_dir=" + Quote(out);
    Command c;
    c.exe = paths_.game_exe.wstring();
    c.workdir = paths_.launcher_dir.wstring();
    c.args = Widen(a);
    c.display = Quote(PathStr(paths_.game_exe)) + " " + a;
    check_zip_.clear();
    check_running_ = true;
    StartGame(c, "Determinism check started");
    if (!game_proc_) check_running_ = false;  // could not start: Status says why
  }
  // From PollGame, when the check's game has closed.
  void CheckEnded(DWORD code) {
    fs::path dir = paths_.launcher_dir / "reports";
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) dir = LocalDataDir() / "reports", fs::create_directories(dir, ec);
    const fs::path zip = dir / ("determinism_check_" + NowStamp("%Y%m%d_%H%M") + ".zip");
    ZipWriter z(zip);
    if (!z.ok()) return Status("Could not write " + PathStr(zip));
    // The hash CSV (det_NNN.csv or det_<exe folder>_NNN.csv) and the scheduler trace; NNN = the run's log number.
    std::string number, identity;
    const fs::path out = check_run_dir_ / "out";
    for (const auto& e : fs::directory_iterator(out, ec)) {
      const std::string name = PathStr(e.path().filename());
      if (name.rfind("det_", 0) != 0) continue;
      if (e.path().extension() == ".csv") {
        const std::string data = Redact(ReadTail(e.path(), 64u << 20));
        z.Add(name, data);
        const size_t us = name.rfind('_');
        if (us != std::string::npos) number = name.substr(us + 1, name.size() - us - 1 - 4);
        if (const size_t at = data.find("# xdet identity: "); at != std::string::npos)
          identity = data.substr(at + 17, data.find('\n', at) - at - 17);
      } else if (e.path().extension() == ".txt") {
        z.Add(name, ReadTail(e.path(), 64u << 20));
      }
    }
    // The run's log and its rotated parts (wwe2k14_NNN.log, wwe2k14_NNN.K.log).
    int logs = 0;
    if (!number.empty()) {
      const std::string base = "wwe2k14_" + number + ".";
      for (const auto& e : fs::directory_iterator(paths_.logs, ec)) {
        const std::string name = PathStr(e.path().filename());
        if (name.rfind(base, 0) == 0 && e.path().extension() == ".log")
          z.Add(name, Redact(ReadTail(e.path(), 48u << 20))), ++logs;
      }
    }
    const std::string about =
        std::string("WWE 2K14 recomp determinism check (Session 77)\nVersion: ") + Version() + "\nMade: " +
        NowStamp("%Y-%m-%d %H:%M") + " (local time)\n\n" + HardwareText(hw_) + "\nGame exit: " + ExitCodeText(code) +
        "\nRun log number: " + (number.empty() ? std::string("(no hash file: the check did not start its replay)")
                                                : number) +
        ", log files: " + std::to_string(logs) + "\nIdentity: " + (identity.empty() ? std::string("-") : identity) +
        "\n\nThe .csv holds one hash per memory region per frame (no game data); compared with a reference play "
        "of the same release: tools\\determinism\\det_compare.py --cross REFERENCE THIS.zip\n";
    z.Add("about.txt", Redact(about));
    if (!z.Close()) return Status("Could not finish " + PathStr(zip));
    fs::remove_all(check_run_dir_, ec);  // the empty save and the copies now in the zip
    check_zip_ = zip;
    want_check_popup_ = true;
    Status("Determinism check saved: " + PathStr(zip));
    Log("determinism check saved " + PathStr(zip) + " (exit " + ExitCodeText(code) + ")");
  }
  void CheckPopup() {
    if (want_check_popup_) ImGui::OpenPopup("xdet_check"), want_check_popup_ = false;
    if (!BeginGameModal("xdet_check", "DETERMINISM CHECK", 680 * Dpi())) return;
    if (check_running_) {
      ImGui::Text("The check is running.");
      ImGui::Spacing();
      ImGui::TextWrapped("The game plays a short Cena vs Undertaker match by itself (about 2 minutes) and closes. "
                         "Please don't touch the controller, mouse or keyboard while it runs.");
    } else if (!check_zip_.empty()) {
      ImGui::TextWrapped("Done, thank you! Please send this file:");
      ImGui::TextColored(kGood, "%s", PathStr(check_zip_).c_str());
      ImGui::Dummy(ImVec2(0, 12 * Dpi()));
      if (ModalMainButton("OPEN THE FOLDER")) {
        const std::wstring arg = L"/select,\"" + check_zip_.wstring() + L"\"";
        ShellExecuteW(hwnd_, nullptr, L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
      }
      if (ModalButton("Close")) ImGui::CloseCurrentPopup();
    } else {
      ImGui::TextWrapped("Checks that the game runs on this PC exactly as on ours: it plays a recorded Cena vs "
                         "Undertaker match by itself (about 2 minutes, from a fresh, empty save of its own) and saves "
                         "one .zip for you to send. Your save and your settings are not touched.");
      ImGui::Dummy(ImVec2(0, 12 * Dpi()));
      const bool can = gd_.ok && !game_proc_ && CheckAvailable();
      if (ModalMainButton("START THE CHECK", can)) StartCheck();
      if (ModalButton("Not now")) ImGui::CloseCurrentPopup();
      if (!gd_.ok) ImGui::TextColored(kBad, "Set up your game files first (Settings).");
      else if (!CheckAvailable()) ImGui::TextColored(kBad, "This build has no check files (the xdet folder).");
    }
    ImGui::NewLine();
    ImGui::EndPopup();
  }
