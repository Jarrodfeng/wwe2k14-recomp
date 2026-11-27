
  // Session 25 state.
  Hardware hw_;
  GameDataCheck gd_;
  fs::path gd_path_;
  HANDLE game_proc_ = nullptr;
  Command last_cmd_;
  DWORD crash_code_ = 0;
  bool want_crash_popup_ = false, want_wizard_ = false, advanced_open_ = false, force_rtx_ = false;
  bool want_report_popup_ = false;  // automation: --show-report
  bool open_folders_ = true;  // automation: --no-open
  int wizard_step_ = 0, want_mods_tab_ = -1;
  std::string capture_name_ = "my_match";
  fs::path last_report_;
  // Session 58 (Report a problem): the launcher's last messages, the last game exit, the player's note.
  std::vector<std::string> recent_log_;
  std::string last_exit_;
  char report_note_[4096] = {};
  LogoSave logo_, logo_before_;
  bool logo_loaded_ = false, importing_ = false;
  fs::file_time_type logo_stamp_{};
  std::chrono::steady_clock::time_point logo_checked_;
  std::array<ID3D11ShaderResourceView*, kLogoSlots> logo_tex_{};
  struct PngTex {
    ID3D11ShaderResourceView* srv = nullptr;
    int w = 0, h = 0;
  };
  std::map<fs::path, PngTex> png_tex_;
  std::vector<fs::path> logo_picks_;
  int logo_pick_sel_ = 0, backup_sel_ = -1;
  std::vector<LogoJob> logo_jobs_;
  std::map<fs::path, int> logo_forced_;  // automation only
  std::string logo_result_;
  fs::path logo_result_file_;  // Session 27: --wwe2k14_input_result of the logo import
  std::vector<Backup> backups_;


  // ---------------- Trons
  void Select(int tron) {
    for (size_t i = 0; i < chars_.size(); ++i)
      if (chars_[i].tron == tron || (chars_[i].tron < 0 && chars_[i].id == tron)) {
        selected_ = int(i);
        scroll_to_selected_ = true;
        return;
      }
    selected_ = -1, selected_tron_ = tron;  // a tron number with no character in the table
  }

  int SelectedTron() const {
    if (selected_ >= 0 && selected_ < int(chars_.size()))
      return chars_[size_t(selected_)].tron >= 0 ? chars_[size_t(selected_)].tron : chars_[size_t(selected_)].id;
    return selected_tron_;
  }

  void DoAssign(int tron, const fs::path& file, bool stacked_alpha = false) {
    const std::string err = AssignTron(paths_, tron, file, stacked_alpha);
    RefreshOverrides();
    ForgetMedia();
    thumb_.reset();
    Status(err.empty() ? "Tron " + std::to_string(tron) + " now plays " + PathStr(file.filename()) +
                             (s.mods ? "" : "  (switch USE MY VIDEOS on)")
                       : "Could not assign tron " + std::to_string(tron) + ": " + err);
  }

  void DoRevert(int tron) {
    const int n = RevertTron(paths_, tron);
    RefreshOverrides();
    ForgetMedia();
    thumb_.reset();
    Status(n ? "Tron " + std::to_string(tron) + " back to the original" : "Tron " + std::to_string(tron) +
                                                                            " had no override");
  }

  // "original" | "video" | "none" | a video file (copied as NNN.banner.<ext>).
  void DoBanner(int tron, const std::string& choice) {
    const std::string l = Lower(choice);
    std::string err;
    if (l == "original" || l.empty()) err = SetBanner(paths_, tron, BannerMode::kOriginal);
    else if (l == "video") err = SetBanner(paths_, tron, BannerMode::kVideo);
    else if (l == "none") err = SetBanner(paths_, tron, BannerMode::kNone);
    else err = SetBanner(paths_, tron, BannerMode::kFile, fs::path(Widen(choice)));
    banner_tron_ = -1;
    Status(err.empty() ? "Tron " + std::to_string(tron) + " banner ring: " + (l.empty() ? "original" : choice)
                       : "Banner ring not set: " + err);
  }

  void UpdateThumb(const fs::path& f) {
    std::error_code ec;
    if (f.empty()) {
      thumb_.reset();
      return;
    }
    const auto stamp = fs::last_write_time(f, ec);
    if (thumb_ && thumb_->file == f && thumb_->stamp == stamp) return;
    thumb_ = std::make_unique<Thumb>();
    thumb_->file = f, thumb_->stamp = stamp;
    std::vector<uint8_t> rgba;
    thumb_->info = ProbeVideo(f, &rgba);
    if (!rgba.empty() && IsStackedAlpha(f)) UnstackRgba(rgba, thumb_->info.w, thumb_->info.h);
    if (!rgba.empty()) {
      thumb_->srv = MakeTexture(rgba, thumb_->info.w, thumb_->info.h);
      thumb_->w = thumb_->info.w, thumb_->h = thumb_->info.h;
    }
  }

  // ---------------- Tron pictures: still (tile / panel) and hover playback
  // Sources per tron: an assigned MP4 / MOV / MKV (its first frame; hover plays the file itself), else the game-made
  // previews in TronCacheDir() (NNN.png / NNN.mp4: "Build previews"). A 320x320 original is a sheet: the big-screen
  // picture is its top 320x224; the strips below feed the banner screens circling the top of the arena (Jarrod), so
  // only the top part is shown.
  // Disk is looked at once per tron every 2 s (cached), never per frame per tile.
  struct Media {
    fs::path still, video;
    bool sheet_crop = false;  // a game original: crop 320x320 sheets
    std::chrono::steady_clock::time_point checked;
  };
  std::map<int, Media> media_;

  const Media& MediaFor(int tron) {
    const auto now = std::chrono::steady_clock::now();
    auto it = media_.find(tron);
    if (it != media_.end() && now - it->second.checked < std::chrono::seconds(2)) return it->second;
    Media m;
    m.checked = now;
    std::error_code ec;
    const fs::path ov = OverrideFor(paths_, tron);
    if (!ov.empty() && Lower(PathStr(ov.extension())) != ".bik2") {
      m.still = ov, m.video = ov;
    } else if (ov.empty()) {  // the original plays (an assigned .bik2 has no preview here)
      const fs::path png = TronCacheDir() / (std::to_string(tron) + ".png");
      const fs::path mp4 = TronCacheDir() / (std::to_string(tron) + ".mp4");
      const fs::path amp4 = TronCacheDir() / (std::to_string(tron) + ".alpha.mp4");  // a reveal: stacked alpha
      if (fs::exists(png, ec)) m.still = png;
      if (fs::exists(amp4, ec)) m.video = amp4;
      else if (fs::exists(mp4, ec)) m.video = mp4;
      m.sheet_crop = true;
    }
    return media_[tron] = m;
  }
  void ForgetMedia() { media_.clear(); }  // after assign / revert / refresh

  // Draws tron `tron` fitted into [a, b] (letterboxed). Returns false if there is nothing to show.
  bool DrawMedia(ImDrawList* dl, int tron, ImVec2 a, ImVec2 b, bool hovered, float rounding) {
    const Media& m = MediaFor(tron);
    ID3D11ShaderResourceView* srv = nullptr;
    int w = 0, h = 0;
    if (hovered && !m.video.empty() && (hover_video_.empty() || hover_video_ == m.video)) {
      hover_video_ = m.video;
      if (player_.File() != m.video) player_.Open(m.video);
      srv = player_.Tick();
      w = player_.W(), h = player_.H();
    }
    if (!srv && !m.still.empty()) srv = StillImage(tron, m.still, &w, &h);
    if (!srv || !w || !h) return false;
    float v1 = 1.0f, aspect = float(w) / float(h);
    // Rows 0-223 are the big-screen picture (the strips start at row 224: brightness step 223/224 in 343, 100, 139);
    // stop 2 rows short so filtering and H.264's half-size colour don't bleed the strip in.
    if (m.sheet_crop && w == 320 && h == 320) v1 = 222.0f / 320.0f, aspect = 320.0f / 222.0f;
    const float bw = b.x - a.x, bh = b.y - a.y;
    const float iw = aspect >= bw / bh ? bw : bh * aspect, ih = aspect >= bw / bh ? bw / aspect : bh;
    const ImVec2 i0(a.x + (bw - iw) / 2, a.y + (bh - ih) / 2);
    dl->AddImageRounded(ImTextureRef(reinterpret_cast<ImTextureID>(srv)), i0, ImVec2(i0.x + iw, i0.y + ih),
                        ImVec2(0, 0), ImVec2(1, v1), IM_COL32_WHITE, rounding);
    return true;
  }

  // Still pictures: decoded by the ThumbLoader thread, turned into textures here; re-checked on disk every 2 s.
  struct StillTex {
    fs::path file;
    fs::file_time_type stamp;
    ID3D11ShaderResourceView* srv = nullptr;
    int w = 0, h = 0;
    std::chrono::steady_clock::time_point checked;
    bool requested = false;
    ~StillTex() { ReleaseLater(srv); }
  };
  std::map<int, std::unique_ptr<StillTex>> stills_;
  ThumbLoader thumbs_;

  // Once per frame: textures for the pictures the loader finished.
  void CollectThumbs() {
    for (auto& d : thumbs_.TakeDone()) {
      auto& t = stills_[d.tron];
      if (!t || t->file != d.file) continue;  // asked for something else meanwhile
      ReleaseLater(t->srv), t->srv = nullptr;
      if (!d.rgba.empty()) t->srv = MakeTexture(d.rgba, d.w, d.h), t->w = d.w, t->h = d.h;
      t->stamp = d.stamp, t->requested = false;
    }
  }

  ID3D11ShaderResourceView* StillImage(int tron, const fs::path& f, int* w, int* h) {
    auto& t = stills_[tron];
    const auto now = std::chrono::steady_clock::now();
    if (!t || t->file != f) {
      t = std::make_unique<StillTex>();
      t->file = f;
    } else if (now - t->checked < std::chrono::seconds(2) || t->requested) {
      *w = t->w, *h = t->h;
      return t->srv;
    }
    t->checked = now;
    std::error_code ec;
    const auto stamp = fs::last_write_time(f, ec);
    if (t->srv && t->stamp == stamp) {
      *w = t->w, *h = t->h;
      return t->srv;
    }
    t->requested = true;
    thumbs_.Request(tron, f, stamp);  // the old picture stays up until the new one is ready
    *w = t->w, *h = t->h;
    return t->srv;
  }

  // "Build previews": one run of the game decodes every original tron with its own Bink (src\tron_export.h).
  void BuildPreviews() {
    if (phone_export_ && !phone_export_->done) return Status("Wait for Export for phone to finish, then build the previews");
    std::error_code ec;
    fs::create_directories(TronCacheDir(), ec);
    // Session 53 item 2.2 (a playtester: "--game_data_root was not provided" from Build previews): the game files and
    // save folders as PLAY GAME gives them (the run had none, so a release found no game files).
    const std::string flag = HelperPathFlags() + "--wwe2k14_tron_export=" + Quote(PathStr(TronCacheDir())) +
                             " --wwe2k14_tron_export_media_mp4=true --wwe2k14_use_launcher_settings=false";  // a helper run keeps the default look; Session 98: the stage movies also as MP4 (the viewer plays them)
    Command c;
    if (s.harness) {
      c.exe = L"pwsh.exe";
      const std::string a = "-NoProfile -File " + Quote(PathStr(paths_.repo / "tools" / "bench" / "run_list.ps1")) +
                            " -Scripts " + Quote(PathStr(paths_.routes / "manual.txt")) + " -Extra " + Quote(flag);
      c.args = Widen(a), c.display = "pwsh " + a, c.console = true, c.workdir = paths_.repo.wstring();
    } else {
      c.exe = paths_.game_exe.wstring(), c.args = Widen(flag), c.workdir = paths_.launcher_dir.wstring();
      c.display = Quote(PathStr(paths_.game_exe)) + " " + flag;
    }
    const std::string err = RunCommand(c);
    Status(err.empty() ? "Building previews: the game opens, decodes every tron (about 2 minutes) and closes itself"
                       : "Could not start the game: " + err);
  }

  int CachedPreviews() {
    const auto now = std::chrono::steady_clock::now();
    if (now - previews_checked_ > std::chrono::seconds(2)) {
      previews_checked_ = now;
      cached_previews_ = 0;
      std::error_code ec;
      for (const auto& e : fs::directory_iterator(TronCacheDir(), ec))
        cached_previews_ += Lower(PathStr(e.path().extension())) == ".mp4";
    }
    return cached_previews_;
  }

  // ---- Session 98 (Jarrod: "could we have it export a zip that the phone launcher takes in?"): EXPORT FOR PHONE. The tron cache
  // (Build previews) as one zip with a manifest (src\tron_zip.h), for the phone launcher's IMPORT TRON PREVIEWS. Made from the
  // player's own game files: it only moves between his own devices.
  struct PhoneExport {
    tron_zip::Progress prog;
    std::atomic<bool> done{false};
    std::string err;  // (written by the thread before `done`)
    tron_zip::Counts counts;
    fs::path zip;
  };
  std::shared_ptr<PhoneExport> phone_export_;
  bool want_phone_export_popup_ = false;
  void ExportForPhone(const fs::path& to_or_empty) {
    if (phone_export_ && !phone_export_->done) return;
    if (GameRunning()) return Status("Close the game first (it may still be building the previews), then export them");
    fs::path to = to_or_empty;
    if (to.empty()) {
      const auto pick = PickSaveZip(hwnd_, L"Save the tron previews for your phone", Widen(tron_zip::kDefaultName).c_str());
      if (!pick) return;
      to = *pick;
    }
    auto job = std::make_shared<PhoneExport>();
    job->zip = to;
    phone_export_ = job;
    Log("export for phone: " + PathStr(to));
    std::thread([job, cache = TronCacheDir(), ver = MyVersion()] { wwe2k14::NameThread(L"lnch_trons");
      try {
        job->err = tron_zip::Export(cache, job->zip, "pc", "wwe2k14 launcher " + ver, job->counts, &job->prog);
      } catch (const std::exception& e) {
        job->err = std::string("the export stopped: ") + e.what();
      } catch (...) {
        job->err = "the export stopped";
      }
      job->done = true;
    }).detach();
  }
  // At the end of the Trons tab's bottom row while it runs (inline); its end opens the result window once.
  void PhoneExportProgress(bool draw = true) {
    auto job = phone_export_;
    if (!job) return;
    if (!job->done) {
      if (!draw) return;
      const uint64_t t = job->prog.total, d = job->prog.done;
      char label[96];
      std::snprintf(label, sizeof label, "Exporting %.0f / %.0f MB", d / 1048576.0, t / 1048576.0);
      ImGui::SameLine();
      ImGui::ProgressBar(t ? float(double(d) / double(t)) : 0.f, ImVec2(-1, ImGui::GetFrameHeight() * 0.8f), label);
      return;
    }
    if (!phone_export_seen_ || phone_export_seen_ != job.get()) {
      phone_export_seen_ = job.get();
      if (job->err.empty()) {
        char mb[32];
        std::snprintf(mb, sizeof mb, "%.0f MB", job->counts.bytes / 1048576.0);
        phone_export_note_ = std::to_string(job->counts.videos) + " videos, " + mb +
                             " - copy this file to your phone and use IMPORT TRON PREVIEWS (Arena Viewer page of the phone launcher).";
        Status("Exported for phone: " + PathStr(job->zip) + " (" + phone_export_note_ + ")");
      } else {
        phone_export_note_.clear();
        Status("Export for phone failed: " + job->err);
      }
      want_phone_export_popup_ = job->err.empty();
    }
  }
  void PhoneExportPopup() {
    if (want_phone_export_popup_) ImGui::OpenPopup("phone_export"), want_phone_export_popup_ = false;
    if (!BeginGameModal("phone_export", "EXPORTED FOR PHONE", 680 * Dpi())) return;
    auto job = phone_export_;
    ImGui::TextColored(kGood, "%s", job ? PathStr(job->zip).c_str() : "");
    ImGui::TextWrapped("%s", phone_export_note_.c_str());
    ImGui::TextDisabled("The previews are made from your own game files: keep them to your own devices.");
    ImGui::Dummy(ImVec2(0, 12 * Dpi()));
    if (ModalMainButton("OK")) ImGui::CloseCurrentPopup();
    if (job && ModalButton("Show in folder")) {
      const std::wstring arg = L"/select,\"" + job->zip.wstring() + L"\"";
      ShellExecuteW(hwnd_, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
    }
    ImGui::NewLine();
    ImGui::EndPopup();
  }
  const void* phone_export_seen_ = nullptr;
  std::string phone_export_note_;
