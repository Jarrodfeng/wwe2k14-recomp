
  // ---------------- Logos (Session 25 item 1)
  void RefreshLogos(bool force) {
    const auto now = std::chrono::steady_clock::now();
    if (!force && now - logo_checked_ < std::chrono::seconds(2)) return;
    logo_checked_ = now;
    std::error_code ec;
    const fs::path title = FindTitleDir(paths_.user_data);
    const fs::path file = title.empty() ? fs::path() : title / "00000001" / "00PaintTool.pt" / "SaveData.Dat";
    const auto stamp = file.empty() ? fs::file_time_type{} : fs::last_write_time(file, ec);
    if (!force && logo_loaded_ && title == logo_.title_dir && stamp == logo_stamp_) return;
    logo_ = ReadLogoSave(paths_.user_data);
    logo_stamp_ = stamp, logo_loaded_ = true;
    for (auto*& t : logo_tex_) ReleaseLater(t), t = nullptr;
    for (int i = 0; i < kLogoSlots; ++i) {
      const auto& sl = logo_.slots[size_t(i)];
      if (sl.used && !sl.rgba.empty()) logo_tex_[size_t(i)] = MakeTexture(sl.rgba, sl.w, sl.h);
    }
    backups_ = ListBackups(logo_.title_dir);
  }

  ID3D11ShaderResourceView* PngTexture(const fs::path& p, int* w = nullptr, int* h = nullptr) {
    auto it = png_tex_.find(p);
    if (it == png_tex_.end()) {
      PngTex t;
      FILE* fp = _wfopen(p.c_str(), L"rb");
      int n = 0;
      if (fp) {
        if (unsigned char* px = stbi_load_from_file(fp, &t.w, &t.h, &n, 4)) {
          t.srv = MakeTexture(std::vector<uint8_t>(px, px + size_t(t.w) * t.h * 4), t.w, t.h);
          stbi_image_free(px);
        }
        fclose(fp);
      }
      it = png_tex_.emplace(p, t).first;
    }
    if (w) *w = it->second.w;
    if (h) *h = it->second.h;
    return it->second.srv;
  }

  // A picture fitted into [a, b] over a checkerboard (transparency shows).
  void DrawPicture(ImDrawList* dl, ID3D11ShaderResourceView* srv, int w, int h, ImVec2 a, ImVec2 b) {
    Checker(dl, a, b, 8 * Dpi());
    if (!srv || w <= 0 || h <= 0) return;
    const float bw = b.x - a.x, bh = b.y - a.y, aspect = float(w) / float(h);
    const float iw = aspect >= bw / bh ? bw : bh * aspect, ih = aspect >= bw / bh ? bw / aspect : bh;
    const ImVec2 i0(a.x + (bw - iw) / 2, a.y + (bh - ih) / 2);
    dl->AddImage(ImTextureRef(reinterpret_cast<ImTextureID>(srv)), i0, ImVec2(i0.x + iw, i0.y + ih));
  }

  void LogosTab() {
    RefreshLogos(false);
    const float dpi = Dpi(), right_w = 430 * dpi;
    if (!logo_.save_exists) {
      ImGui::Dummy(ImVec2(0, 20 * dpi));
      Heading("No save found", 1.2f);
      ImGui::TextWrapped("Start the game once to create your save, then come back. (Play, go past the title screen; "
                         "the game makes its save the first time.)");
      ImGui::TextDisabled("Looked in: %s", PathStr(paths_.user_data).c_str());
      if (ImGui::Button("Look again")) RefreshLogos(true);
      return;
    }
    Note("Your own pictures as real Create-a-Logo logos: the game saves them like logos you paint yourself, so you "
         "can use them on attires, signs and the rest.");
    // Left: the 20 slots as the game shows them (2 pages of 2 x 5).
    ImGui::BeginChild("slots", ImVec2(-right_w - 8, -ImGui::GetFrameHeightWithSpacing() * 1.2f), ImGuiChildFlags_Borders);
    if (!logo_.error.empty()) ImGui::TextColored(kBad, "%s", logo_.error.c_str());
    const float avail = ImGui::GetContentRegionAvail().x, gap = 8 * dpi;
    const float tile = std::min(120 * dpi, (avail - gap * 4) / 5);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int page = 0; page < 2; ++page) {
      ImGui::TextDisabled("Page %d", page + 1);
      for (int k = 0; k < 10; ++k) {
        const int i = page * 10 + k;
        if (k % 5) ImGui::SameLine(0, gap);
        const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + tile, a.y + tile);
        ImGui::Dummy(ImVec2(tile, tile + ImGui::GetTextLineHeight()));
        const auto& sl = logo_.slots[size_t(i)];
        if (sl.used) {
          DrawPicture(dl, logo_tex_[size_t(i)], sl.w, sl.h, a, b);
          if (ImGui::IsItemHovered()) ImGui::SetTooltip("Slot %d: %dx%d, saved %s UTC", i + 1, sl.w, sl.h, sl.date.c_str());
        } else {
          dl->AddRectFilled(a, b, IM_COL32(24, 26, 32, 255), 4 * dpi);
          const char* t = "free";
          const ImVec2 ts = ImGui::CalcTextSize(t);
          dl->AddText(ImVec2(a.x + (tile - ts.x) / 2, a.y + (tile - ts.y) / 2), IM_COL32(90, 95, 110, 255), t);
        }
        dl->AddRect(a, b, sl.used ? IM_COL32(140, 145, 160, 255) : IM_COL32(60, 64, 76, 255), 4 * dpi);
        char lab[32];
        std::snprintf(lab, sizeof(lab), sl.used ? "%d  %dx%d" : "%d", i + 1, sl.w, sl.h);
        dl->AddText(ImVec2(a.x + 2, b.y + 1), ImGui::GetColorU32(ImGuiCol_TextDisabled), lab);
      }
      ImGui::Spacing();
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // Right: import and backups.
    ImGui::BeginChild("logoside", ImVec2(right_w, -ImGui::GetFrameHeightWithSpacing() * 1.2f), ImGuiChildFlags_Borders);
    ImGui::SeparatorText("Import pictures");
    if (importing_) {
      ImGui::TextColored(kWarn, "Importing %zu logo(s)...", logo_jobs_.size());
      ImGui::TextWrapped("The game is going through the menus for you. Please don't touch the controller or the "
                         "game window; it closes itself when it's done (about %d seconds).",
                         20 + 12 * int(logo_jobs_.size()));
    } else {
      if (ImGui::Button("Add PNG pictures...", ImVec2(-1, 0)))
        for (auto& f : PickPngs(hwnd_)) AddLogoPick(f);
      // Session 53 (a playtester's request): the logo's size, the game's own SELECT CANVAS SIZE choice.
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted("Logo size:");
      ImGui::SameLine();
      ImGui::RadioButton("256x256", &s.logo_size, 256);
      ImGui::SameLine();
      ImGui::RadioButton("128x128", &s.logo_size, 128);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("A smaller logo: the game's 128x128 canvas (the size its own editor offers first).");
      if (logo_picks_.empty()) {
        Note("Any PNG works; it is scaled to the logo size. Square pictures look best.");
      } else {
        // The selected picture, as the game will get it (stretched to the square canvas).
        if (logo_pick_sel_ >= int(logo_picks_.size())) logo_pick_sel_ = 0;
        const fs::path& cur = logo_picks_[size_t(logo_pick_sel_)];
        int w = 0, h = 0;
        ID3D11ShaderResourceView* srv = PngTexture(cur, &w, &h);
        const float side = std::min(256 * dpi, ImGui::GetContentRegionAvail().x);
        const ImVec2 a = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(side, side));
        // The game's canvas is square: the picture is stretched to it (as src\logo_import.h scales it).
        Checker(ImGui::GetWindowDrawList(), a, ImVec2(a.x + side, a.y + side), 8 * dpi);
        if (srv)
          ImGui::GetWindowDrawList()->AddImage(ImTextureRef(reinterpret_cast<ImTextureID>(srv)), a,
                                               ImVec2(a.x + side, a.y + side));
        if (srv && w != h) ImGui::TextColored(kWarn, "%dx%d: not square, it will be stretched", w, h);
        else if (!srv) ImGui::TextColored(kBad, "Can't read this PNG");
        for (size_t i = 0; i < logo_picks_.size(); ++i) {
          ImGui::PushID(int(i));
          if (ImGui::SmallButton("x")) {
            logo_picks_.erase(logo_picks_.begin() + std::ptrdiff_t(i));
            ImGui::PopID();
            break;
          }
          ImGui::SameLine();
          if (ImGui::Selectable(PathStr(logo_picks_[i].filename()).c_str(), int(i) == logo_pick_sel_)) logo_pick_sel_ = int(i);
          ImGui::PopID();
        }
      }
      const int free = logo_.Free();
      ImGui::Text("%d free slot(s).", free);
      ImGui::BeginDisabled(logo_picks_.empty() || int(logo_picks_.size()) > free || !gd_.ok || game_proc_ != nullptr);
      char lab[64];
      std::snprintf(lab, sizeof(lab), "Import %zu logo(s)", logo_picks_.size());
      if (BigButton(lab, ImVec2(-1, 40 * dpi))) StartLogoImport();
      ImGui::EndDisabled();
      if (!gd_.ok) ImGui::TextColored(kBad, "Set up your game files first (Settings).");
      Note("Your save is backed up first. The game starts, makes each logo in a free slot and saves it, then "
           "closes. Transparency is kept as on / off (the game stores hard edges). The game allows at most 2 "
           "different 256x256 logos on one custom superstar.");
    }
    if (!logo_result_.empty()) {
      ImGui::Spacing();
      ImGui::TextWrapped("%s", logo_result_.c_str());
    }

    ImGui::SeparatorText("Backups");
    Note("Before every import the launcher copies your whole save (logos and custom superstars). The last 10 are kept.");
    if (backups_.empty()) ImGui::TextDisabled("No backups yet.");
    ImGui::BeginChild("bk", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 5.5f), ImGuiChildFlags_Borders);
    for (size_t i = 0; i < backups_.size(); ++i)
      if (ImGui::Selectable(backups_[i].label.c_str(), int(i) == backup_sel_)) backup_sel_ = int(i);
    ImGui::EndChild();
    ImGui::BeginDisabled(backup_sel_ < 0 || backup_sel_ >= int(backups_.size()) || game_proc_ != nullptr);
    if (ImGui::Button("Restore...")) ImGui::OpenPopup("restore");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(game_proc_ != nullptr);
    if (ImGui::Button("Back up now")) DoBackup("by hand");
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Open folder")) {
      std::error_code ec;
      fs::create_directories(BackupRoot(), ec);
      ShellExecuteW(hwnd_, L"explore", BackupRoot().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    if (ImGui::BeginPopupModal("restore", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
      if (backup_sel_ >= 0 && backup_sel_ < int(backups_.size())) {
        ImGui::Text("Put this save back?\n  %s", backups_[size_t(backup_sel_)].label.c_str());
        ImGui::TextDisabled("Logos AND custom superstars go back to that moment. Your current save is backed up first.");
        if (ImGui::Button("Restore")) DoRestore(backup_sel_), ImGui::CloseCurrentPopup();
        ImGui::SameLine();
      }
      if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }
    ImGui::EndChild();
    ImGui::TextDisabled("Save: %s", logo_.title_dir.empty() ? "-" : PathStr(logo_.title_dir).c_str());
  }

  void AddLogoPick(const fs::path& f) {
    if (std::find(logo_picks_.begin(), logo_picks_.end(), f) == logo_picks_.end()) logo_picks_.push_back(f);
  }

  void DoBackup(const std::string& why) {
    RefreshLogos(true);
    const std::string err = MakeBackup(logo_.title_dir, why);
    backups_ = ListBackups(logo_.title_dir);
    Status(err.empty() ? "Save backed up" : "Backup: " + err);
  }

  void DoRestore(int index) {
    if (GameRunning()) {
      Status("Close the game first, then restore");
      return;
    }
    RefreshLogos(true);
    if (index < 0 || index >= int(backups_.size())) return;
    const std::string err = RestoreBackup(backups_[size_t(index)], logo_.title_dir);
    Status(err.empty() ? "Restored the save from " + backups_[size_t(index)].label + " (checked: identical)"
                       : "Restore: " + err);
    Log("restore: " + (err.empty() ? std::string("ok, byte-for-byte identical") : err));
    RefreshLogos(true);
  }

  void StartLogoImport() {
    if (GameRunning()) {
      logo_result_ = "Close the game first: the import runs it by itself.";
      return;
    }
    RefreshLogos(true);
    if (!logo_.save_exists) return;
    if (!logo_.error.empty()) {
      logo_result_ = "Can't read your logo save (" + logo_.error + "); nothing was changed.";
      return;
    }
    std::string err;
    logo_jobs_ = PlanLogoJobs(logo_, logo_picks_, err, s.logo_size);
    if (!logo_forced_.empty() && !logo_jobs_.empty()) {  // automation: the slots given with --logo-add PNG@SLOT
      for (auto& j : logo_jobs_)
        if (auto it = logo_forced_.find(j.png); it != logo_forced_.end()) j.slot = it->second;
      std::sort(logo_jobs_.begin(), logo_jobs_.end(), [](const LogoJob& a, const LogoJob& b) {
        return SlotX(a.slot) != SlotX(b.slot) ? SlotX(a.slot) < SlotX(b.slot) : SlotRow(a.slot) < SlotRow(b.slot);
      });
      std::error_code ec2;
      for (const auto& j : logo_jobs_)
        if (j.slot < 0 || j.slot >= kLogoSlots || logo_.slots[size_t(j.slot)].used ||
            (j.slot == 0 && !fs::exists(logo_.file, ec2)))  // Session 27: a new logo save puts the default logo there
          logo_jobs_.clear(), err = "a forced slot is not free";
      if (logo_jobs_.empty()) {
        logo_result_ = err;
        return;
      }
    }
    if (logo_jobs_.empty()) {
      logo_result_ = err.empty() ? "Nothing to import." : err;
      return;
    }
    // Review fix (Session 27): the old counted route can't answer the "create the Custom Logo save" prompt.
    if (std::error_code lec; s.logo_legacy_route && !fs::exists(logo_.file, lec)) {
      logo_result_ = "The old logo route (Settings -> Advanced) can't create your first logo save: turn it off, or "
                     "open CREATIONS -> LOGO in the game once.";
      return;
    }
    // S110 HELPERSTART: the command first (it names the script and the result file, written below), then the imported superstars' pack
    // brought up to date for it as PLAY does (StarsBeforeStart: a build runs first and this import is asked again when it is done). Nothing
    // is backed up or written before that.
    const fs::path script = LocalDataDir() / "logo_import_script.txt";
    // Session 27: the script writes where it ended (ok / failed + the stage) here; read in FinishLogoImport.
    logo_result_file_ = LocalDataDir() / "logo_import_result.txt";
    Command c;
    c.exe = paths_.game_exe.wstring(), c.workdir = paths_.launcher_dir.wstring();
    std::string a = HelperPathFlags();
    a += "--wwe2k14_input_script=" + Quote(PathStr(script)) + " --vsync=false";  // fast-forwarded menus
    a += " --wwe2k14_use_launcher_settings=false";  // a helper run keeps the default look, whatever the Play settings are
    a += " --wwe2k14_input_result=" + Quote(PathStr(logo_result_file_));
    c.args = Widen(a), c.display = Quote(PathStr(paths_.game_exe)) + " " + a;
    if (StarsBeforeStart(c, std::string(), stars_start::Then::kLogoImport)) return;
    if (std::string e = MakeBackup(logo_.title_dir, "before logo import"); !e.empty()) {
      logo_result_ = "Not imported: the backup failed (" + e + ").";
      return;
    }
    logo_before_ = logo_;
    std::error_code ec;
    fs::create_directories(LocalDataDir(), ec);
    std::ofstream(script, std::ios::trunc) << LogoImportScript(logo_jobs_, s.logo_legacy_route, fs::exists(logo_.file, ec));
    fs::remove(logo_result_file_, ec);
    HANDLE h = nullptr;
    if (std::string e = StartProcess(c, &h); !e.empty()) {
      logo_result_ = "Could not start the game: " + e;
      return;
    }
    game_proc_ = h, last_cmd_ = c, importing_ = true, stars_ok_for_.clear();  // (the memo is for one start, as StartGame clears it)
    logo_result_.clear();
    Log("logo import: " + std::to_string(logo_jobs_.size()) + " job(s), " + c.display);
    backups_ = ListBackups(logo_.title_dir);
  }

  void FinishLogoImport(DWORD code) {
    importing_ = false;
    RefreshLogos(true);
    const LogoResult r = CheckLogoJobs(logo_jobs_, logo_before_, logo_);
    logo_result_ = std::to_string(r.ok) + " of " + std::to_string(logo_jobs_.size()) + " logo(s) imported.\n" + r.detail;
    // Session 27: the route stops (and the game closes without saving) when a screen is not the expected one.
    std::string line;
    if (std::ifstream rf(logo_result_file_); rf && std::getline(rf, line) && line.rfind("failed", 0) == 0) {
      const size_t t = line.rfind('\t');
      std::string stage = t == std::string::npos ? line : line.substr(t + 1);
      if (stage.rfind("logo_import: ", 0) == 0) stage = stage.substr(13);
      logo_result_ += "The import stopped at an unexpected screen (" + stage +
                      "); the game closed without saving there. Nothing else was changed.\n";
      // Session 57 (tester bug 2): before the main menu, the likely screen is the game's notice that the save needs
      // DLC packs it does not see (a pack switched off, or the DLC folder moved). S110 LOGOIMPORT review: the imported
      // superstars' pack is one of them (USE MY SUPERSTARS); S110 HELPERSTART: the import brings it up to date itself.
      if (stage == "start" || stage == "waiting for the main menu")
        logo_result_ += "If the game said your save needs downloadable content: turn on in Settings -> GAME everything the "
                        "save was made with (USE MY DLC and its packs, USE MY SUPERSTARS), then import again.\n";
    } else if (line.empty() && r.failed) {
      logo_result_ += "The game closed before the import finished.\n";
    }
    if (r.failed) logo_result_ += "Your backup from just before the import is in the list below.";
    if (code != 0) logo_result_ += "\nThe game stopped with " + ExitCodeText(code) + ".";
    if (r.failed == 0) logo_picks_.clear();
    Log("logo import result: " + logo_result_);
    Status(r.failed ? "Logo import: some logos were not saved" : "Logo import done");
  }
