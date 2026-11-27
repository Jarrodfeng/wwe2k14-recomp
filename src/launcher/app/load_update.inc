  void Load() {
    if (settings_path.empty()) settings_path = SettingsPath();
    s = LoadSettings(settings_path);
    if (!override_game_data.empty()) s.game_data_dir = override_game_data;
    if (!override_user_data.empty()) s.user_data_dir = override_user_data;
    hw_ = DetectHardware(hwnd_);
    if (hw_.refresh_hz > 0) g_monitor_hz = hw_.refresh_hz;
    // Session 93: the quality tier's AUTO asks which card the game will use.
    g_gpu_probe = [this](const std::string& gpu, std::string* name) {
      const wwe2k14::vkdev::Device d = ChosenDevice(gpu);
      if (name) *name = d.name;
      return d.integrated();
    };
    if (s.first_run_done && s.loaded_version < 3 && MigrateForDevice(s) && save_settings) SaveSettings(settings_path, s);
    // Session 95 (Jarrod: individual settings win, the tier only supplies starting values): once per file (low_start_done), where
    // the tier resolves LOW, the sliders take LOW's safe values, so an existing player's picture does not change (what ran before
    // is what the sliders now show) and one click raises them. A first run does the same in the setup (SuggestForDevice).
    if (!s.low_start_done) {
      s.low_start_done = true;
      const wwe2k14::vkdev::Device dev = ChosenDevice(s.gpu);
      std::string why;
      const int rend = wwe2k14::gfxs::RendererFromFlags(s.extra);
      if (wwe2k14::gfxs::ResolveQualityTier(s.quality_tier, false, dev.integrated(), dev.name, &why) == 1 && !s.original_look &&
          (rend < 0 || rend == 2)) {
        std::string what;
        wwe2k14::gfxs::LowStartValues(s.resolution, s.draw_height, s.upscale, s.crowd_density, &what);
        Log("settings: " + why + ": starting values applied once to the sliders:" + what);
      }
      if (save_settings) SaveSettingsSynced();
    }
    g_perf_report_default = IsPlaytest();
    Reload();
    StartUpdateCheck(update_force_);
    UpdateStartup();  // Session 81
  }

  // ---------------- Session 31: update check (update_check.h). In the background at start, unless switched off in
  // Settings; development builds (+dev) skip it unless --update-check. --update-file JSON reads a release from a file
  // instead of GitHub and --update-version VER pretends this version (tests).
  struct UpdateState {
    std::mutex m;
    UpdateInfo info;
  };
  std::shared_ptr<UpdateState> update_ = std::make_shared<UpdateState>();
  bool update_force_ = false, update_dismissed_ = false, want_update_popup_ = false;
  std::string update_file_, update_version_;

  std::string MyVersion() const { return update_version_.empty() ? std::string(Version()) : update_version_; }
  UpdateInfo Update() {
    std::lock_guard<std::mutex> lock(update_->m);
    return update_->info;
  }
  void StartUpdateCheck(bool force) {
    const bool dev = MyVersion().find("+dev") != std::string::npos;
    if (!force && (!s.check_updates || dev)) return;
    {
      std::lock_guard<std::mutex> lock(update_->m);
      if (update_->info.state == UpdateInfo::kChecking) return;
      update_->info = {}, update_->info.state = UpdateInfo::kChecking;
    }
    std::thread([st = update_, ver = MyVersion(), file = update_file_] { wwe2k14::NameThread(L"lnch_update");
      UpdateInfo u;
      if (file.empty()) {
        u = CheckForUpdate(ver);
      } else {
        std::ifstream f(fs::path(Widen(file)), std::ios::binary);
        u = ReadRelease(std::string((std::istreambuf_iterator<char>(f)), {}), ver);
      }
      {
        std::lock_guard<std::mutex> lock(st->m);
        st->info = u;
      }
      WakeUi();
    }).detach();  // owns its state (shared_ptr): never joined, so closing the launcher never waits for GitHub
  }
  void LogUpdateOnce() {
    static UpdateInfo::State logged = UpdateInfo::kNone;
    const UpdateInfo u = Update();
    if (u.state == logged || u.state == UpdateInfo::kChecking) return;
    logged = u.state;
    Log("update check: " + std::string(u.state == UpdateInfo::kAvailable ? "available " + u.tag + " " + u.url
                                       : u.state == UpdateInfo::kLatest  ? "up to date (" + MyVersion() + ")"
                                       : u.state == UpdateInfo::kFailed  ? "failed: " + u.error
                                                                         : "not run"));
  }

  // The notice at the band's foot (above Report a problem) when a newer release exists; a click opens the details.
  float UpdateNotice(ImVec2 pos, float width) {
    const UpdateInfo u = Update();
    if (u.state != UpdateInfo::kAvailable || update_dismissed_) return 0;
    const float d = Dpi(), h = 46 * d;
    const std::string label = "UPDATE " + u.tag.substr(0, u.tag.find('-'));
    if (SlantButton(label.c_str(), pos, ImVec2(width, h), true, 17 * d)) want_update_popup_ = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("A newer version is out: see what's new");
    return h;
  }
  // ---------------- Session 81: the launcher installs its own updates (update_install.h has the steps and the
  // reasoning). The dialog has four faces: the offer, the download (checking and unpacking included), "ready" (restart
  // to finish) and, after any failure, UPDATE FAILED with the reason, REPORT A PROBLEM and the release page.
  struct InstallJob {
    upd::Progress prog;
    std::atomic<int> stage{0};  // 1 downloading, 2 checking, 3 unpacking, 4 ready, 5 failed
    std::mutex m;
    std::string error;
    fs::path root;
  };
  std::shared_ptr<InstallJob> job_;
  std::string up_note_, up_fail_msg_;
  bool want_update_fail_ = false, verify_pending_ = false, update_test_files_ = false;
  int verify_frames_ = 0;

  void BadWrapped(const std::string& t) {
    ImGui::PushStyleColor(ImGuiCol_Text, kBad);
    ImGui::TextWrapped("%s", t.c_str());
    ImGui::PopStyleColor();
  }
  static std::string MB(unsigned long long b) { return std::to_string((b + (1u << 20) - 1) >> 20) + " MB"; }

  void OpenReleasePage(const UpdateInfo& u) {
    if (u.url.rfind("https://github.com/", 0) == 0) ShellExecuteW(hwnd_, L"open", Widen(u.url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  }
  void OpenLink(const std::string& url) {
    if (url.rfind("https://", 0) == 0) ShellExecuteW(hwnd_, L"open", Widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  }

  void FailUpdate(const std::string& why) {
    up_fail_msg_ = why, want_update_fail_ = true;
    Log("update failed: " + why);
  }

  // UPDATE NOW: the checks first (each one a plain sentence for the dialog), then the download on its own thread.
  void StartInstall(const UpdateInfo& u) {
    up_note_.clear();
    if (game_proc_) return void(up_note_ = "Close the game first.");
    if (paths_.repo_found && !update_test_files_) return void(up_note_ = "A development build can't update itself.");
    if (u.asset_url.empty() || u.asset_sha256.empty())
      return void(up_note_ = "This release has no installable file. Use View on GitHub.");
    const fs::path install = paths_.launcher_dir;
    if (!upd::Writable(install))
      return void(up_note_ = "This folder can't be written to (" + PathStr(install) +
                             "). Move the game to a normal folder such as Documents, or install from the release page.");
    std::error_code ec;
    const auto sp = fs::space(install, ec);
    const unsigned long long need = std::max<unsigned long long>(u.asset_size, 1u << 20) * 3;
    if (!ec && sp.available < need)
      return void(up_note_ = "Not enough free space: about " + MB(need) + " are needed here and " + MB(sp.available) + " are free.");
    job_ = std::make_shared<InstallJob>();
    job_->stage = 1;
    Log("update: downloading " + u.asset_name + " (" + MB(u.asset_size) + ")");
    std::thread([job = job_, install, u, test = update_test_files_, ver = MyVersion()] { wwe2k14::NameThread(L"lnch_install");
      auto fail = [&](const std::string& why) {
        std::lock_guard<std::mutex> lock(job->m);
        job->error = why, job->stage = 5;
        std::error_code e;
        fs::remove_all(upd::TmpDir(install), e);
      };
      const fs::path zip = upd::TmpDir(install) / L"update.zip";
      std::string err = upd::Download(u.asset_url, zip, job->prog, test, ver);
      if (!err.empty()) return fail(err == "cancelled" ? "cancelled" : "the download failed: " + err);
      job->stage = 2;
      const std::string sha = upd::Sha256File(zip);
      auto lower = [](std::string s) {
        for (char& c : s) c = char(std::tolower((unsigned char)c));
        return s;
      };
      if (sha.empty() || lower(sha) != lower(u.asset_sha256))
        return fail("the download did not match GitHub's checksum (got " + sha.substr(0, 12) + ", expected " +
                    u.asset_sha256.substr(0, 12) + "); nothing was changed");
      job->stage = 3;
      const fs::path root = upd::Unpack(zip, upd::TmpDir(install) / L"stage", err);
      if (root.empty()) return fail(err);
      std::error_code e;
      fs::remove(zip, e);
      {
        std::lock_guard<std::mutex> lock(job->m);
        job->root = root;
      }
      job->stage = 4;
    }).detach();
  }

  // RESTART AND FINISH: the helper does the swap once this launcher has closed.
  void FinishInstall(const UpdateInfo& u) {
    if (game_proc_) return void(up_note_ = "Close the game first.");
    fs::path root;
    {
      std::lock_guard<std::mutex> lock(job_->m);
      root = job_->root;
    }
    const std::string err = upd::StartHelper(paths_.launcher_dir, root, u.tag);
    if (!err.empty()) return job_.reset(), FailUpdate(err);
    Log("update: helper started for " + u.tag + "; closing for the swap");
    PostQuitMessage(0);
  }

  void UpdatePopup() {
    const float d = Dpi();
    if (want_update_popup_) ImGui::OpenPopup("update"), want_update_popup_ = false;
    const int st = job_ ? job_->stage.load() : 0;
    const char* title = st == 0 ? "UPDATE AVAILABLE" : st == 4 ? "READY TO INSTALL" : "UPDATING";
    if (!BeginGameModal("update", title, 660 * d)) return;
    const UpdateInfo u = Update();
    const std::string what = u.name.empty() ? u.tag : u.name;
    if (st == 5) {  // failed: this dialog gives way to UPDATE FAILED (cancel is not a failure)
      std::string why;
      {
        std::lock_guard<std::mutex> lock(job_->m);
        why = job_->error;
      }
      job_.reset();
      ImGui::CloseCurrentPopup();
      if (why != "cancelled") FailUpdate(why);
      ImGui::EndPopup();
      return;
    }
    if (st == 1 || st == 2 || st == 3) {
      const unsigned long long done = job_->prog.done, total = std::max<unsigned long long>(job_->prog.total, u.asset_size);
      if (st == 1) ImGui::Text("Downloading %s... %s of %s", what.c_str(), MB(done).c_str(), MB(total).c_str());
      else ImGui::Text(st == 2 ? "Checking the download..." : "Unpacking...");
      ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.78f, 0.20f, 0.23f, 1));
      ImGui::ProgressBar(st == 1 ? (total ? float(double(done) / double(total)) : 0.f) : 1.f, ImVec2(-1, 12 * d), "");
      ImGui::PopStyleColor();
      ImGui::TextDisabled("The download is checked against GitHub's checksum before anything is changed.");
      ImGui::Dummy(ImVec2(0, 8 * d));
      if (st == 1 && ImGui::Button("Cancel")) job_->prog.cancel = true;
      ImGui::EndPopup();
      return;
    }
    if (st == 4) {
      ImGui::TextWrapped("%s is downloaded and checked. The launcher will close, put the new files in place and open again on %s.",
                         what.c_str(), u.tag.c_str());
      Note("Your old files are kept until the new version has started properly. Close the game first if it is open.");
      if (!up_note_.empty()) BadWrapped(up_note_);
      ImGui::Dummy(ImVec2(0, 10 * d));
      if (ModalMainButton("RESTART AND FINISH")) FinishInstall(u);
      if (ModalButton("Later")) ImGui::CloseCurrentPopup();
      ImGui::NewLine();
      ImGui::EndPopup();
      return;
    }
    const bool can = !u.asset_url.empty();
    ImGui::TextWrapped("%s is out (you have %s).", what.c_str(), MyVersion().c_str());
    if (can)
      ImGui::TextDisabled("Updating replaces the launcher and game program (%s). Your saves, settings and mods stay where they are.",
                          MB(u.asset_size).c_str());
    else
      ImGui::TextDisabled("Download it from its GitHub page; your saves, settings and mods stay where they are.");
    if (!u.notes.empty()) {
      ImGui::Spacing();
      ImGui::BeginChild("notes", ImVec2(0, 240 * d), ImGuiChildFlags_Borders);
      RenderMarkdown(u.notes, ImGui::GetContentRegionAvail().x - 6 * d, d, [this](const std::string& l) { OpenLink(l); });
      ImGui::EndChild();
    }
    if (!up_note_.empty()) BadWrapped(up_note_);
    ImGui::Dummy(ImVec2(0, 10 * d));
    if (can) {
      if (ModalMainButton("UPDATE NOW")) StartInstall(u);
      if (ModalButton("Later")) update_dismissed_ = true, up_note_.clear(), ImGui::CloseCurrentPopup();
      if (ModalButton("View on GitHub")) OpenReleasePage(u);
    } else {
      if (ModalMainButton("OPEN THE RELEASE PAGE")) OpenReleasePage(u), ImGui::CloseCurrentPopup();
      if (ModalButton("Later")) update_dismissed_ = true, ImGui::CloseCurrentPopup();
    }
    ImGui::NewLine();
    ImGui::EndPopup();
  }

  void UpdateFailPopup() {
    if (want_update_fail_) ImGui::OpenPopup("update_fail"), want_update_fail_ = false;
    if (!BeginGameModal("update_fail", "UPDATE FAILED", 640 * Dpi())) return;
    ImGui::TextWrapped("The update could not be installed.");
    ImGui::Spacing();
    BadWrapped(up_fail_msg_);
    ImGui::Spacing();
    Note("Your previous version is still in place and works as before. A report helps fix this; or install the update by hand from its release page.");
    ImGui::Dummy(ImVec2(0, 10 * Dpi()));
    if (ModalMainButton("REPORT A PROBLEM")) {
      ImGui::CloseCurrentPopup();
      MakeReport();
    }
    if (ModalButton("Open the release page")) OpenReleasePage(Update());
    if (ModalButton("Close")) ImGui::CloseCurrentPopup();
    ImGui::NewLine();
    ImGui::EndPopup();
  }

  // At start: what the helper left (a failure to show), then the "did the new version start properly" check. The
  // backup goes only after this launcher has drawn its window for a while (UpdateVerifyTick); three starts that never
  // got there put the old files back.
  void UpdateStartup() {
    const fs::path install = paths_.launcher_dir;
    std::error_code ec;
    const nlohmann::json res = upd::ReadJson(upd::ResultPath(install));
    if (res.is_object()) {
      if (res.value("ok", false)) Log("update: installed " + res.value("version", std::string()));
      else FailUpdate(res.value("error", std::string("unknown")));
      fs::remove(upd::ResultPath(install), ec);
    }
    const nlohmann::json j = upd::ReadJson(upd::JournalPath(install));
    if (!j.is_object() || j.value("phase", std::string()) != "verify") return;
    const int attempts = j.value("attempts", 0) + 1;
    if (attempts >= 3) {
      Log("update: the new version did not start properly " + std::to_string(attempts) + " times; putting the old files back");
      const std::string err = upd::StartRestoreHelper(install);
      if (err.empty()) return void(PostQuitMessage(0));
      return FailUpdate(err);
    }
    nlohmann::json w = j;
    w["attempts"] = attempts;
    upd::WriteJson(upd::JournalPath(install), w);
    verify_pending_ = true;
  }
  void UpdateVerifyTick() {
    if (!verify_pending_ || ++verify_frames_ < 90) return;
    verify_pending_ = false;
    const fs::path install = paths_.launcher_dir;
    std::error_code ec;
    fs::remove_all(upd::BackupDir(install), ec);
    fs::remove_all(upd::TmpDir(install), ec);
    fs::remove(upd::JournalPath(install), ec);
    Log("update: the new version has started properly; the old files were removed");
    upd::LogLine(install, "verified: the new launcher started; backup removed");
  }
