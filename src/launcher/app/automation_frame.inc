
  // The first-run wizard (unless automation said otherwise: it would cover every screenshot).
  void SetAutomation(bool on) { automation_ = on; }  // lane 283: launcher steps were given (no song conversion is waited for or run)
  void MaybeWizard(bool automation) {
    if (wizard_mode_ == 1 || (wizard_mode_ == 0 && !automation && !s.first_run_done)) want_wizard_ = true;
  }
  int wizard_mode_ = 0;  // --wizard 1, --no-wizard -1
  std::string override_game_data, override_user_data;  // --game-data / --user-data (this run only)
  void OpenAdvanced() { advanced_open_ = true; }
  void ForceRtx() { force_rtx_ = true; }
  void FakeIntegratedGpu() { fake_igpu_ = true; }  // Session 37: --fake-igpu
  void FakeNoInterlock() { fake_no_interlock_ = true; }  // Session 54: --fake-no-interlock
  void NoOpen() { open_folders_ = false; }

  void Reload() {
    paths_ = FindPaths(s);
    if (!paths_.exe_note.empty()) Status(paths_.exe_note);  // S102 W1: an explicit AVX2 exe on a CPU that cannot run it
    std::string err;
    chars_ = LoadCharacters(paths_.ids, err);
    if (!err.empty()) Status("characters: " + err);
    arenas_ = LoadArenas(paths_.ids);
    scenarios_ = LoadScenarios(paths_);
    replays_ = ListFiles(paths_.replays, ".rec", true);
    directors_ = ListFiles(paths_.director, ".txt", true);
    shots_ = ListFiles(paths_.shots, ".txt", false);
    routes_ = ListFiles(paths_.routes, ".txt", false);
    for (const auto& r : ListFiles(paths_.routes / "short", ".txt", false)) routes_.push_back("short\\" + r);
    RefreshOverrides();
    seeds_scenario_.clear();
    CheckGameDataNow();
    logo_loaded_ = false;
    LoadMenuFont();
  }

  // Session 31: the game's menu face from the player's own game files (game_font.h), again when they change.
  void LoadMenuFont() {
    if (gfont_tried_ && gfont_root_ == paths_.game_data) return;
    gfont_tried_ = true, gfont_root_ = paths_.game_data;
    ReleaseLater(gfont_srv_), gfont_srv_ = nullptr;
    if (!LoadGameFont(GameFilesAt(paths_.game_data), gfont_)) {
      Log("menu font: not found in the game files; using the fallback font");
      return;
    }
    D3D11_TEXTURE2D_DESC d{};
    d.Width = UINT(gfont_.sheet_w), d.Height = UINT(gfont_.sheet_h), d.MipLevels = 1, d.ArraySize = 1;
    d.Format = DXGI_FORMAT_BC3_UNORM, d.SampleDesc.Count = 1, d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA init{gfont_.bc3.data(), UINT(gfont_.sheet_w / 4 * 16), 0};
    ID3D11Texture2D* tex = nullptr;
    if (SUCCEEDED(g_device->CreateTexture2D(&d, &init, &tex))) {
      g_device->CreateShaderResourceView(tex, nullptr, &gfont_srv_);
      tex->Release();
    }
    gfont_.bc3 = {};
    Log(gfont_srv_ ? "menu font: the game's font000 (" + std::to_string(gfont_.glyphs.size()) + " glyphs)"
                   : "menu font: texture failed; using the fallback font");
  }

  void RefreshOverrides() { overrides_ = AllOverrides(paths_); }

  void Status(const std::string& msg) {
    status_ = msg;
    status_time_ = std::chrono::steady_clock::now();
    Log(msg);
  }

  void Log(const std::string& msg) {
    // Session 58: the last 300 messages in memory too, for Report a problem (players have no --log file).
    recent_log_.push_back(NowStamp("%Y-%m-%d %H:%M:%S") + "  " + msg);
    if (recent_log_.size() > 300) recent_log_.erase(recent_log_.begin());
    if (log_path.empty()) return;
    std::ofstream f(log_path, std::ios::app);
    f << msg << "\n";
  }

  // Runs one automation step (the same code the buttons use). Returns false when the step must wait a frame.
  bool RunStep(const Step& st) {
    if (st.op == "tab") {  // Sessions 17-24 tab names
      if (st.a == "play") s.page = 0;
      else if (st.a == "settings") s.page = 2;
      else s.page = 1, want_mods_tab_ = st.a == "titles" ? 1 : st.a == "logos" ? 2 : st.a == "music" ? 3 : 0;
    } else if (st.op == "page") {  // Session 31: + about, quick (Play -> Quick Match), advanced (Play -> Advanced)
      GoPage(st.a == "mods" ? 1 : st.a == "settings" ? 2 : st.a == "about" ? 3 : 0);
      play_view_ = st.a == "quick" ? 1 : st.a == "advanced" ? 2 : st.a == "wm30" ? 3 : 0;
    }
    else if (st.op == "mods_tab")
      s.page = 1, want_mods_tab_ = st.a == "titles" ? 1 : st.a == "logos" ? 2 : st.a == "music" ? 3 : 0;
    else if (st.op == "logo_add") {  // PNG or PNG@SLOT (a test: that slot, 1-20, instead of the first free one)
      const size_t at = st.a.rfind('@');
      const bool forced = at != std::string::npos && at + 1 < st.a.size() &&
                          std::all_of(st.a.begin() + std::ptrdiff_t(at) + 1, st.a.end(), ::isdigit);
      const fs::path f = fs::path(Widen(forced ? st.a.substr(0, at) : st.a));
      AddLogoPick(f);
      if (forced) logo_forced_[f] = std::atoi(st.a.c_str() + at + 1) - 1;
    }
    else if (st.op == "logo_import") StartLogoImport();
    else if (st.op == "logo_size") s.logo_size = std::atoi(st.a.c_str()) == 128 ? 128 : 256;
    else if (st.op == "wait_game") return game_proc_ == nullptr;
    else if (st.op == "wait_stars") return !stars_job_;  // Session 101: the superstars build a PLAY started (then the game starts)
    else if (st.op == "perf_test") StartPerfTest();                // Session 39
    else if (st.op == "wait_perf_test") return perf_step_ < 0;     // ... until all its runs are done
    else if (st.op == "show_perf_test") want_perf_popup_ = true;   // its window, for a look
    else if (st.op == "xdet_check") StartCheck();                  // Session 77
    else if (st.op == "wait_xdet_check") return !check_running_ && game_proc_ == nullptr;
    else if (st.op == "show_xdet_check") want_check_popup_ = true;
    else if (st.op == "diag_run") StartDiag();  // Session 95
    else if (st.op == "wait_diag_run") return !diag_running_ && game_proc_ == nullptr;
    else if (st.op == "backup") DoBackup("automation");
    else if (st.op == "restore") RefreshLogos(true), DoRestore(std::atoi(st.a.c_str()));
    else if (st.op == "report") MakeReport();
    else if (st.op == "play") LaunchPlain();
    else if (st.op == "create_shortcut") CreateDesktopShortcut();  // Session 53 (with --shortcut-dir in tests)
    else if (st.op == "build_previews") BuildPreviews();
    else if (st.op == "export_phone") ExportForPhone(fs::path(Widen(st.a)));  // Session 98: EXPORT FOR PHONE to this file
    else if (st.op == "wait_export_phone") {
      if (phone_export_ && !phone_export_->done) return false;
      PhoneExportProgress(false);  // (its result line, also when the Trons tab is not shown)
    }
    else if (st.op == "wizard_step") wizard_step_ = std::clamp(std::atoi(st.a.c_str()), 0, 2);
    else if (st.op == "no_rtx") hw_.rtx = false, force_rtx_ = false;
    else if (st.op == "quick_match") LaunchQuickMatch();
    else if (st.op == "quick_type") s.quick_mode = 0, s.quick_type = std::atoi(st.a.c_str());  // Session 31
    else if (st.op == "wm30_era") wm30_era_ = std::clamp(std::atoi(st.a.c_str()), 0, 4), wm30_sel_ = -1;  // Session 34
    else if (st.op == "wm30_play") {  // Session 34: what PLAY MATCH does for match N (the list read first)
      if (!wm30_.loaded && gd_.ok) wm30_ = wwe2k14::wm30list::Load(GameFilesAt(paths_.game_data));
      const int n = std::atoi(st.a.c_str());
      for (const auto& m : wm30_.matches)
        if (m.index == n) LaunchWm30(m);
    }
    else if (st.op == "settings_section") s.page = 2, settings_section_ = std::atoi(st.a.c_str());
    else if (st.op == "show_crash") crash_code_ = DWORD(std::strtoul(st.a.c_str(), nullptr, 0)), want_crash_popup_ = true;
    else if (st.op == "show_report") want_report_popup_ = true;
    else if (st.op == "show_keys") want_keys_popup_ = true;  // Session 67
    else if (st.op == "keys_capture") BeginCapture(std::clamp(std::atoi(st.a.c_str()), 0, keys::kBindCount - 1), 0);
    else if (st.op == "keys_hot") key_hot_force_ = std::atoi(st.a.c_str());
    else if (st.op == "titles_view") s.titles_view = st.a == "grid" ? 1 : 0;
    else if (st.op == "show_update") want_update_popup_ = Update().state == UpdateInfo::kAvailable;
    else if (st.op == "update_install") StartInstall(Update());  // Session 81 tests: UPDATE NOW
    else if (st.op == "update_finish") { if (job_ && job_->stage == 4) FinishInstall(Update()); }  // RESTART AND FINISH
    else if (st.op == "update_cancel") { if (job_) job_->prog.cancel = true; }
    else if (st.op == "capture") LaunchCapture(st.a);
    else if (st.op == "wait_game_start") return game_proc_ == nullptr || WaitForInputIdle(game_proc_, 0) != WAIT_TIMEOUT;
    else if (st.op == "kill_game") {  // a test of the crash dialog: end the game with this exit code
      if (game_proc_) TerminateProcess(game_proc_, UINT(std::strtoul(st.a.c_str(), nullptr, 0)));
    }
    else if (st.op == "wait") {}
    else if (st.op == "banner") DoBanner(std::atoi(st.a.c_str()), st.b);
    else if (st.op == "title") title_sel_ = std::atoi(st.a.c_str());
    else if (st.op == "assign_alpha") DoAssign(std::atoi(st.a.c_str()), fs::path(Widen(st.b)), true);
    else if (st.op == "select") Select(std::atoi(st.a.c_str()));
    else if (st.op == "assign") DoAssign(std::atoi(st.a.c_str()), fs::path(Widen(st.b)));
    else if (st.op == "revert") DoRevert(std::atoi(st.a.c_str()));
    else if (st.op == "scenario") s.scenario = st.a;
    else if (st.op == "mods") s.mods = st.a != "0";
    else if (st.op == "harness") s.harness = true, s.harness_route = st.a;
    else if (st.op == "preset") {
      for (int i = 0; i < kPresetCount; ++i)
        if (Lower(kPresetNames[i]) == Lower(st.a) || (st.a == "reshoot" && i == kReshoot)) s.preset = i;
    } else if (st.op == "launch") DoLaunch(false);
    else if (st.op == "view") s.trons_view = st.a == "grid" ? 1 : 0;
    else if (st.op == "sort") s.trons_sort = std::abs(std::atoi(st.a.c_str())) % 4, s.trons_sort_desc = st.a[0] == '-';
    else if (st.op == "reset_play") ResetPlay();
    else if (st.op == "hover") force_hover_ = std::atoi(st.a.c_str());
    else if (st.op == "shot") Log(SaveBackbuffer(fs::path(Widen(st.a))) ? "shot " + st.a : "shot FAILED " + st.a);
    return true;
  }

  void Frame() {
    SyncSuperstarsKey();  // Session 98 (importer review): every frame (a stat; the key is read when the file changed)
    CollectThumbs();
    PollGame();
    DiagTick();  // Session 95
    BenchTick();  // S102 U4: a benchmark scene's wall-clock limit
    PollPrecompile();  // Session 70
    PollStars();       // Session 101: the superstars pack built before a start
    DirectStartPoll();  // Session 53
    hover_video_.clear();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##main", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar();
    // Session 31: the background picture (dots, the red panel), the page list on the panel, the page to its right.
    const theme::Panel panel = NavGeometry(vp->WorkPos, vp->WorkSize);
    DrawBackground(vp->WorkPos, vp->WorkSize, panel);
    NavPanel(panel);
    ImGui::SetCursorScreenPos(ImVec2(panel.x0 + panel.w + 24 * Dpi(), vp->WorkPos.y));
    ImGui::BeginChild("page", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    if (s.page == 1) ModsPage();
    else if (s.page == 2) SettingsPage();
    else if (s.page == 3) AboutPage();
    else PlayPage();
    // Status line at the bottom.
    if (!status_.empty() && std::chrono::steady_clock::now() - status_time_ < std::chrono::seconds(12)) {
      ImGui::SetCursorPosY(ImGui::GetWindowHeight() - ImGui::GetFrameHeightWithSpacing() - 4);
      ImGui::Separator();
      ImGui::TextUnformatted(status_.c_str());
    }
    ImGui::EndChild();
    Wizard();
    CrashPopup();
    PrecompilePopup();  // Session 70
    StarsPopup();       // Session 101
    MusicTick();        // lane 283: finished conversions, the PLAY that waited, the close button
    MusicPopup();
    QuickStartPopup();  // Session 53
    PerfTestPopup();  // Session 39
    CheckPopup();     // Session 77
    DiagPopup();      // Session 95
    UpdatePopup();
    UpdateFailPopup();
    UpdateVerifyTick();
    KeysPopup();  // Session 67
    LogUpdateOnce();
    ImGui::End();
    if (hover_video_.empty()) player_.Close();  // nothing hovered: stop playing
  }

  // Something moving: a preview playing or thumbnails loading (the main loop keeps drawing at the screen rate).
  bool Animating() { return player_.Playing() || thumbs_.Busy() || quick_pending_ || cap_bind_ >= 0 || stars_job_ || (MusicBusy() && IsWindowVisible(hwnd_)); }  // (S101: the superstars build's bar)

  // ---------------- Session 53 (items 2.1 / 2.4): the game without the launcher's window. `--start-game` (wwe2k14.exe
  // started directly hands over to it, src\direct_start.h; a desktop shortcut) starts the game at once, as PLAY GAME
  // does, and the window stays hidden; QUICK START (Settings -> GAME) does the same after 3 seconds in which a key or a
  // click keeps the launcher open (Shift held while it opens skips it). Hidden, the launcher closes when the game
  // closes, or shows itself when the game crashed (the crash window) or when something needs the player (no game
  // files yet, the game already running).
  bool start_game_ = false, quick_pending_ = false, exit_with_game_ = false;
  bool direct_after_precompile_ = false;  // Session 70: a direct / quick start waiting for the precompile
  std::chrono::steady_clock::time_point quick_at_{};
  static constexpr int kQuickStartSeconds = 3;

  void BeginDirectStart(bool automation) {
    if (start_game_) {
      if (gd_.ok) {
        want_wizard_ = false;  // the files are there: the player asked for the game, not the wizard
        LaunchPlain();
        if (precompile_proc_ || stars_job_ || music_start_pending_) {  // Session 70 (review): the precompile first - the window shows its SKIP; Session 101: or
                                               // the superstars build (its progress)
          direct_after_precompile_ = true;
          ShowWindow(hwnd_, SW_SHOWDEFAULT);
          Log(precompile_proc_ ? "direct start: preparing shaders first, the game follows"
                               : music_start_pending_ ? "direct start: converting songs first, the game follows"
                                                      : "direct start: building the superstars first, the game follows");
          return;
        }
        if (game_proc_) {
          exit_with_game_ = true;
          Log("direct start: the game started, the window stays hidden");
          return;
        }
      }
      ShowWindow(hwnd_, SW_SHOWDEFAULT);
      if (!gd_.ok) GoPage(2), settings_section_ = 0, Status("Pick your game files here, then press PLAY GAME.");
      Log(std::string("direct start: window shown (") + (gd_.ok ? "the game did not start" : "no game files") + ")");
      return;
    }
    if (s.quick_start && !automation && gd_.ok && !want_wizard_ && !GameRunning()) {
      if (GetAsyncKeyState(VK_SHIFT) & 0x8000) {
        Status("Quick start skipped (Shift held).");
        return;
      }
      quick_pending_ = true;  // the 3 seconds start with the first frame drawn (the window takes ~2 s to come up)
      Log("quick start: counting down");
    }
  }

  // The countdown over the launcher (QUICK START); any key or click cancels it.
  void QuickStartPopup() {
    if (!quick_pending_) return;
    if (quick_at_ == std::chrono::steady_clock::time_point{}) {
      quick_at_ = std::chrono::steady_clock::now() + std::chrono::seconds(kQuickStartSeconds);
      Log("quick start: countdown shown");
    }
    if (!ImGui::IsPopupOpen("quickstart")) ImGui::OpenPopup("quickstart");
    if (!BeginGameModal("quickstart", "STARTING THE GAME", 560 * Dpi())) return;
    const double left = std::chrono::duration<double>(quick_at_ - std::chrono::steady_clock::now()).count();
    ImGui::Text("The game starts in %d...", std::max(1, int(std::ceil(left))));
    ImGui::TextDisabled("Press any key or click to stay in the launcher. QUICK START is in Settings -> GAME.");
    ImGui::Dummy(ImVec2(0, 10 * Dpi()));
    bool stay = ModalMainButton("STAY IN THE LAUNCHER");
    ImGui::NewLine();
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END && !stay; ++k)
      if (ImGui::IsKeyPressed(ImGuiKey(k), false)) stay = true;
    if (stay) {
      quick_pending_ = false;
      ImGui::CloseCurrentPopup();
      Status("Quick start cancelled: you are in the launcher.");
    } else if (left <= 0) {
      quick_pending_ = false;
      ImGui::CloseCurrentPopup();
      LaunchPlain();
      if (precompile_proc_) direct_after_precompile_ = true, Log("quick start: preparing shaders first");  // Session 70
      else if (stars_job_) direct_after_precompile_ = true, Log("quick start: building the superstars first");  // Session 101
      else if (music_start_pending_) direct_after_precompile_ = true, Log("quick start: converting songs first");  // lane 283
      if (game_proc_) exit_with_game_ = true, ShowWindow(hwnd_, SW_HIDE), Log("quick start: the game started");
    }
    ImGui::EndPopup();
  }

  // Each frame: a hidden launcher closes with the game, or comes back for a crash.
  void DirectStartPoll() {
    if (!exit_with_game_ || game_proc_ || precompile_proc_) return;
    exit_with_game_ = false;
    if (want_crash_popup_) {
      ShowWindow(hwnd_, SW_SHOWDEFAULT), SetForegroundWindow(hwnd_);
      Log("direct start: the game crashed, window shown");
      return;
    }
    Log("direct start: the game closed, the launcher closes too");
    PostQuitMessage(0);
  }

  // Settings -> GAME: a shortcut on the desktop (or in --shortcut-dir, tests) that starts the game directly.
  fs::path shortcut_dir_;
  bool CreateDesktopShortcut() {
    fs::path dir = shortcut_dir_;
    if (dir.empty()) {
      PWSTR desk = nullptr;
      if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desk))) dir = desk;
      CoTaskMemFree(desk);
    }
    wchar_t self[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, self, DWORD(std::size(self)));
    IShellLinkW* link = nullptr;
    if (dir.empty() || FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))))
      return false;
    std::wstring args = L"--start-game";
    if (settings_path != SettingsPath()) args += L" --settings \"" + settings_path.wstring() + L"\"";
    link->SetPath(self);
    link->SetArguments(args.c_str());
    link->SetWorkingDirectory(fs::path(self).parent_path().c_str());
    link->SetIconLocation(paths_.game_exe.c_str(), 0);
    link->SetDescription(L"WWE 2K14 (starts the game with your launcher settings)");
    IPersistFile* file = nullptr;
    bool ok = SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file)));
    const fs::path lnk = dir / L"WWE 2K14.lnk";
    if (ok) ok = SUCCEEDED(file->Save(lnk.c_str(), TRUE)), file->Release();
    link->Release();
    Status(ok ? "Made a desktop shortcut: " + PathStr(lnk) : "Couldn't make the shortcut in " + PathStr(dir));
    return ok;
  }

  void Shutdown() {
    {
      std::lock_guard lock(music_->m);  // the music worker (shared state) stops after its current song; nothing it touches is ours
      music_->cancel_all = true, music_->cancel_cur = true;
      music_->quit = true;
      music_->cv.notify_all();
    }
    // Session 101 (review N2): a superstars build still running finishes first (at most 3 minutes), so a closed launcher never
    // leaves a half-written pack (an abandoned one: pack6.building and a stale lock, both cleared by the next build).
    for (int i = 0; stars_job_ && stars_job_->state == 1 && i < 1800; ++i) Sleep(100);
    if (game_proc_) CloseHandle(game_proc_), game_proc_ = nullptr;
    RECT r;
    if (GetWindowRect(hwnd_, &r)) s.window_w = r.right - r.left, s.window_h = r.bottom - r.top;
    if (save_settings) SaveSettingsSynced();
    thumb_.reset();
  }
