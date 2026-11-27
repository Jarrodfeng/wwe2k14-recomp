
  // ---------------- Mods
  // Session 31: game-style tabs: the selected one a slanted black bar with red text (like the selected page on the
  // red panel), the others grey. Returns true when the selection changed.
  // Session 101 (lane 285): when the headers do not fit in `max_w` (< 0: the rest of the row) a left and a right arrow frame the ones that
  // do (tab_strip.h); the selected header stays visible, the arrows move the window one header (they are ordinary items, so keyboard
  // navigation reaches them and, through them, every header).
  struct TabStripState {
    int first = 0, prev_sel = -1;
  };
  bool GameTabs(const char* const* names, int count, int& sel, TabStripState& st, float max_w = -1) {
    const float d = Dpi(), px = 21 * d, h = 34 * d, pad = 18 * d, sk = 9 * d, gap = 2 * d, arrow_w = 26 * d;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const int before = sel;
    std::vector<float> w(count);
    for (int i = 0; i < count; ++i) w[i] = MenuTextSize(px, names[i]).x + pad * 2;
    if (max_w < 0) max_w = ImGui::GetContentRegionMax().x - ImGui::GetCursorPosX();
    max_w = std::max(max_w, 4 * arrow_w);
    const launcher::tabs::Layout L = launcher::tabs::Fit(w.data(), count, gap, max_w, arrow_w, sel, st.first, sel != st.prev_sel);
    st.prev_sel = sel;
    auto arrow = [&](const char* id, bool right, bool enabled) {
      const ImVec2 pos = ImGui::GetCursorScreenPos();
      ImGui::PushID(id);
      const bool hit = ImGui::InvisibleButton("##arrow", ImVec2(arrow_w, h)) && enabled;
      ImGui::PopID();
      const bool hover = ImGui::IsItemHovered() && enabled;
      if (hover) dl->AddRectFilled(pos, ImVec2(pos.x + arrow_w, pos.y + h), IM_COL32(0, 0, 0, 90));
      const float cx = pos.x + arrow_w / 2, cy = pos.y + h / 2, a = 7 * d, k = right ? 1.f : -1.f;
      const ImU32 col = !enabled ? IM_COL32(70, 71, 75, 255) : hover ? IM_COL32(205, 206, 211, 255) : IM_COL32(141, 142, 147, 255);
      dl->AddTriangleFilled(ImVec2(cx + k * a * 0.6f, cy), ImVec2(cx - k * a * 0.6f, cy - a), ImVec2(cx - k * a * 0.6f, cy + a), col);
      return hit;
    };
    if (L.arrows) {
      if (arrow("tabs_left", false, L.first > 0)) st.first = std::max(st.first - 1, 0);
      ImGui::SameLine(0, gap);
    }
    for (int i = L.first; i < L.last; ++i) {
      if (i > L.first) ImGui::SameLine(0, gap);
      const ImVec2 ts = MenuTextSize(px, names[i]), pos = ImGui::GetCursorScreenPos();
      const ImVec2 size(L.lone_w > 0 ? L.lone_w : ts.x + pad * 2, h);
      ImGui::PushID(i);
      if (ImGui::InvisibleButton(names[i], size)) sel = i;
      ImGui::PopID();
      const bool on = sel == i, hover = ImGui::IsItemHovered();
      if (L.lone_w > 0) dl->PushClipRect(pos, ImVec2(pos.x + size.x, pos.y + h), true);
      if (on || hover) {
        const ImVec2 q[4] = {{pos.x + sk, pos.y}, {pos.x + size.x, pos.y}, {pos.x + size.x - sk, pos.y + h},
                             {pos.x, pos.y + h}};
        dl->AddConvexPolyFilled(q, 4, on ? IM_COL32(0, 0, 0, 255) : IM_COL32(0, 0, 0, 90));
      }
      const ImVec2 tp(pos.x + pad, pos.y + (h - ts.y) / 2);
      if (on) MenuTextRaw(dl, tp, px, kMenuRed, names[i]);
      else MenuText(dl, tp, px, hover ? IM_COL32(205, 206, 211, 255) : IM_COL32(141, 142, 147, 255), names[i]);
      if (L.lone_w > 0) dl->PopClipRect();
    }
    if (L.arrows) {
      ImGui::SameLine(0, gap);
      if (arrow("tabs_right", true, L.last < count)) st.first = std::min(st.first + 1, count - 1);
    }
    return sel != before;
  }
  TabStripState mods_strip_, era_strip_;

  static constexpr const char* kModsTabs[] = {"TRONS", "TITLES", "LOGOS", "MUSIC", "SUPERSTARS"};  // Session 75: MUSIC (Jarrod); Session 98: SUPERSTARS

  // Session 96 (Jarrod: "just a button in advanced for now") -> Session 98: the Arena Viewer (src\assets\view\,
  // wwe2k14_assets.exe beside the launcher) loads arenas, wrestlers and the player's mods straight from the game files,
  // without starting the game. Its button sits at the right of the MODS tab row.
  void LaunchArenaViewer() {
    const fs::path exe = paths_.launcher_dir / "wwe2k14_assets.exe";
    const std::string game = s.game_data_dir.empty() ? PathStr(paths_.game_data) : s.game_data_dir;
    std::error_code ec;
    if (!fs::exists(exe, ec)) {
      asset_viewer_note_ = "wwe2k14_assets.exe is not next to the launcher.";
      return;
    }
    std::string a = "--game " + Quote(game) + " --mods " + Quote(PathStr(paths_.mods));  // the mod workbench reads it
    if (save_settings) {  // Session 98: the LIGHT tab's USE MY LIGHTING is this file's "lighting" key (never handed over by a no-save test run)
      SaveSettingsSynced();
      a += " --settings-file " + Quote(PathStr(settings_path));
    }
    // Session 98: the viewer reads the DLC packs the game gets (USE MY DLC, the folder, the packs switched off: GameDlcFlags' rule)
    if (!s.dlc) a += " --no-dlc";
    else {
      a += " --dlc " + Quote(PathStr(paths_.dlc));
      std::string off;
      for (int id : wwe2k14::dlc::ParseIdList(s.dlc_off)) off += (off.empty() ? "" : ",") + std::to_string(id);
      if (!off.empty()) a += " --dlc-off " + off;
    }
    const std::wstring args = Widen(a);
    ShellExecuteW(hwnd_, L"open", exe.wstring().c_str(), args.c_str(), paths_.launcher_dir.wstring().c_str(), SW_SHOWNORMAL);
    asset_viewer_note_.clear();
  }

  // Session 98 (Superstar Importer): the same program as the Arena Viewer in its importer mode (--importer: src\assets\view\ui_importer.h).
  // It gets the launcher's settings file: its footer switch IS USE MY SUPERSTARS (the "superstars" key, the only on / off). Only when
  // this launcher saves its settings (a test run with no saving never hands its file over: CLAUDE.md Safety). `more` = --importer-edit
  // SLOT / --importer-bulk.
  // S110 SEAT-C review: --importer-edit names his library KEY (one with no seat has no slot); an old pack's entry (no key yet) its slot
  static std::string EditArgs(const importer::Superstar& x) {
    return !x.key.empty() ? "--importer-edit " + x.key : x.slot >= 0 ? "--importer-edit " + std::to_string(x.slot) : std::string();
  }
  void LaunchImporter(const std::string& more = "") {
    const fs::path exe = paths_.launcher_dir / "wwe2k14_assets.exe";
    const std::string game = s.game_data_dir.empty() ? PathStr(paths_.game_data) : s.game_data_dir;
    std::error_code ec;
    if (!fs::exists(exe, ec)) {
      asset_viewer_note_ = "wwe2k14_assets.exe is not next to the launcher.";
      return;
    }
    std::string a = "--importer --game " + Quote(game) + " --mods " + Quote(PathStr(paths_.mods));
    if (save_settings) {
      SaveSettingsSynced();  // (the importer reads the switch from the file)
      a += " --settings-file " + Quote(PathStr(settings_path));
    }
    if (!s.dlc) a += " --no-dlc";
    else {
      a += " --dlc " + Quote(PathStr(paths_.dlc));
      std::string off;
      for (int id : wwe2k14::dlc::ParseIdList(s.dlc_off)) off += (off.empty() ? "" : ",") + std::to_string(id);
      if (!off.empty()) a += " --dlc-off " + off;
    }
    if (!more.empty()) a += " " + more;
    const std::wstring args = Widen(a);
    ShellExecuteW(hwnd_, L"open", exe.wstring().c_str(), args.c_str(), paths_.launcher_dir.wstring().c_str(), SW_SHOWNORMAL);
    asset_viewer_note_.clear();
  }
  // Session 98 (importer review): the importer edits the "superstars" key of this launcher's settings file. Every frame the file's
  // time is checked (a stat) and the key read again when it changed; `force` reads it now (before every save and every game launch),
  // so the launcher never writes or launches with a stale switch. A file mid-write or damaged keeps the current value.
  fs::file_time_type stars_key_time_{};
  void SyncSuperstarsKey(bool force = false) {
    if (settings_path.empty()) return;
    std::error_code ec;
    const fs::file_time_type t = fs::last_write_time(settings_path, ec);
    if (ec || (!force && t == stars_key_time_)) return;
    stars_key_time_ = t;
    try {
      std::ifstream f(settings_path, std::ios::binary);
      if (!f) return;
      const json j = json::parse(f);
      if (j.is_object() && j.contains("superstars") && j["superstars"].is_boolean()) s.superstars = j["superstars"].get<bool>();
      if (j.is_object() && j.contains("lighting") && j["lighting"].is_boolean()) s.lighting = j["lighting"].get<bool>();  // (the Arena Viewer's LIGHT tab)
    } catch (...) {
    }
  }
  void SaveSettingsSynced() {
    SyncSuperstarsKey(true);
    SaveSettings(settings_path, s);
  }
  // Session 101 (lane 285): places the USE MY ... switch (tab_strip.h ToggleSlot) and returns its width: on the tab row, right-aligned
  // against the buttons, when it has the room; else on its own row under the buttons. Never beside or under them.
  float ToggleRow(float room, float right, float row_y, float buttons_h, float d) {
    const launcher::tabs::Toggle t = launcher::tabs::ToggleSlot(room, ImGui::GetContentRegionMax().x, d);
    if (t.same_line) {
      ImGui::SameLine(right - t.w);
    } else {
      ImGui::NewLine();
      ImVec2 cur = ImGui::GetCursorScreenPos();
      cur.y = std::max(cur.y, row_y + buttons_h + 4 * d);
      ImGui::SetCursorScreenPos(cur);
    }
    return t.w;
  }
  void ModsPage() {
    const float d = Dpi();
    if (want_mods_tab_ >= 0) s.mods_tab = want_mods_tab_, want_mods_tab_ = -1;
    s.mods_tab = std::clamp(s.mods_tab, 0, 4);
    const ImVec2 row0 = ImGui::GetCursorScreenPos();
    const float viewer_w = 210 * d, viewer_h = 40 * d;  // the ARENA VIEWER button, right of the tab row
    const float imp_w = 260 * d;                         // Session 98: SUPERSTAR IMPORTER, left of it
    Heading("MODS", 1.6f);
    ImGui::SameLine(0, 28 * d);
    // Session 101 (lane 285): the headers get the row up to the two buttons at its right end (never under them); more headers than fit
    // -> arrows (GameTabs).
    GameTabs(kModsTabs, 5, s.mods_tab, mods_strip_,
             ImGui::GetContentRegionMax().x - ImGui::GetCursorPosX() - viewer_w - imp_w - 10 * d - 16 * d);
    if (s.mods_tab == 4) {  // Session 98 (Jarrod: "as simple as an on and off switch, similar to our custom music and Trons")
      ImGui::SameLine(0, 24 * d);
      const float right = ImGui::GetContentRegionMax().x - viewer_w - imp_w - 26 * d;
      const float x = ImGui::GetCursorPosX(), room = right - x;
      const float w = ToggleRow(room, right, row0.y, viewer_h, d);
      if (OptionToggle("USE MY SUPERSTARS", s.superstars, true, nullptr, w) && save_settings)
        SaveSettings(settings_path, s);  // (at once: an open importer shows the same switch)
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip(s.superstars ? "On: the superstars you built into the pack are in the game (DOWNLOADED SUPERSTARS)."
                                       : "Off: the game's own roster only; your pack stays in the mods folder.");
    } else if (s.mods_tab != 2) {
      // Trons, titles and music are all read from the mods folder by --wwe2k14_mods: one switch, on each of their
      // tabs (named for what the tab holds).
      // Session 75: right-aligned after the four tabs, narrower when the window is (it covered MUSIC at 1264 wide).
      ImGui::SameLine(0, 24 * d);
      // Session 98: leaves room for the ARENA VIEWER button at the right end of the row.
      const float right = ImGui::GetContentRegionMax().x - viewer_w - imp_w - 26 * d;
      const float x = ImGui::GetCursorPosX(), room = right - x;
      const float w = ToggleRow(room, right, row0.y, viewer_h, d);  // (too narrow beside the tabs: its own row, below the buttons)
      OptionToggle(s.mods_tab == 3 ? "USE MY MUSIC" : "USE MY VIDEOS", s.mods, true, nullptr, w);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip(s.mods ? "On: your trons, title reveals and music play in the game (one switch for all)."
                                 : "Off: every original plays; your files stay in the mods folder.");
    }
    {
      const ImVec2 back = ImGui::GetCursorScreenPos();
      const float bx = ImGui::GetWindowPos().x + ImGui::GetContentRegionMax().x - viewer_w;
      if (SlantButton("ARENA VIEWER##mods", ImVec2(bx, row0.y), ImVec2(viewer_w, viewer_h), true, 22 * d))
        LaunchArenaViewer();
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Preview arenas, wrestlers and your mods in 3D, without starting the game.");
      if (SlantButton("SUPERSTAR IMPORTER##mods", ImVec2(bx - imp_w - 10 * d, row0.y), ImVec2(imp_w, viewer_h), true, 20 * d))
        LaunchImporter();
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add community characters (.pac / .zip) to the game's roster as new superstars.");
      ImGui::SetCursorScreenPos(back);
    }
    if (!asset_viewer_note_.empty()) ImGui::TextUnformatted(asset_viewer_note_.c_str());
    {  // Session 98: USE MY LIGHTING lives in the Arena Viewer's LIGHT tab (Jarrod: it stood out among the mods, whose switches stay
       // on their own tabs). The viewer edits this file's "lighting" key (read back by SyncSuperstarsKey). Keep the space under the tab row.
      ImVec2 cur = ImGui::GetCursorScreenPos();
      cur.y = std::max(cur.y, row0.y + viewer_h + 6 * d);
      ImGui::SetCursorScreenPos(cur);
    }
    ImGui::Spacing();
    if (s.mods_tab == 0) TronsTab();
    else if (s.mods_tab == 1) TitlesTab();
    else if (s.mods_tab == 2) LogosTab();
    else if (s.mods_tab == 3) MusicTab();
    else SuperstarsTab();
  }

  // ---------------- Superstars (Session 98, Jarrod: "a table that lists the superstars added, similar to how the music one is now")
  // Left: the pack's superstars (<mods>\superstars, read with the importer core's LoadPack): slot, name, based on, the C render as a
  // thumbnail. Right: the selected one (EDIT IN IMPORTER / REMOVE) or the overview (OPEN THE IMPORTER / IMPORT MANY). REMOVE takes him out
  // of the pack list at once and marks the pack to rebuild (the importer's BUILD INTO GAME puts the change in the game).
  importer::PackState stars_;
  std::string stars_sig_, stars_err_;
  std::chrono::steady_clock::time_point stars_checked_{};
  std::vector<ID3D11ShaderResourceView*> stars_tex_;
  int stars_sel_ = -1;
  std::string StarsSig() const {
    std::string sig;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(paths_.mods / "superstars", ec)) {
      std::error_code e2;
      sig += PathStr(e.path().filename()) + ":" + std::to_string(e.is_regular_file(e2) ? fs::file_size(e.path(), e2) : 0) + ":" +
             std::to_string(static_cast<long long>(fs::last_write_time(e.path(), e2).time_since_epoch().count())) + ";";
    }
    return sig;
  }
  void RefreshStars(bool force) {
    const auto now = std::chrono::steady_clock::now();
    if (!force && now - stars_checked_ < std::chrono::seconds(2)) return;
    stars_checked_ = now;
    const std::string sig = StarsSig();
    if (!force && sig == stars_sig_) return;
    stars_sig_ = sig;
    for (auto*& t : stars_tex_) ReleaseLater(t), t = nullptr;
    stars_tex_.clear();
    stars_err_.clear();
    if (!importer::LoadPack(PathStr(paths_.mods), stars_, stars_err_)) stars_ = importer::PackState();
    for (const importer::Superstar& x : stars_.stars) {
      const bool has = !x.render_rgba[2].empty() && x.render_w[2] > 0 && x.render_rgba[2].size() == size_t(x.render_w[2]) * x.render_h[2] * 4;
      stars_tex_.push_back(has ? MakeTexture(x.render_rgba[2], x.render_w[2], x.render_h[2]) : nullptr);
    }
    if (stars_sel_ >= int(stars_.stars.size())) stars_sel_ = -1;
  }
  void SuperstarsTab() {
    RefreshStars(false);
    const float dpi = Dpi(), right_w = 430 * dpi, thumb = 44 * dpi;
    const float list_h = -ImGui::GetFrameHeightWithSpacing() * 1.4f;
    ImGui::BeginChild("stars_list", ImVec2(-right_w - 8, list_h), ImGuiChildFlags_Borders);
    Heading("MY SUPERSTARS", 1.15f);
    ImGui::TextDisabled("Community characters added to the roster as new superstars (the game's DOWNLOADED SUPERSTARS).");
    if (ImGui::BeginTable("stars", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
                          ImVec2(0, std::max(120 * dpi, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 1.3f)))) {
      ImGui::TableSetupScrollFreeze(0, 1);
      ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, thumb);
      ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthStretch, 0.9f);
      ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 2.4f);
      ImGui::TableSetupColumn("Based on", ImGuiTableColumnFlags_WidthStretch, 2.0f);
      ImGui::TableHeadersRow();
      for (const size_t oi : importer::screen::DisplayOrder(stars_.stars)) {  // (S110 SEAT-C: seated first, in seat order; then not seated, by name)
        const int i = int(oi);
        const importer::Superstar& x = stars_.stars[oi];
        ImGui::TableNextRow(0, thumb);
        ImGui::TableNextColumn();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (i < int(stars_tex_.size()) && stars_tex_[size_t(i)])
          DrawPicture(ImGui::GetWindowDrawList(), stars_tex_[size_t(i)], x.render_w[2], x.render_h[2], p, ImVec2(p.x + thumb, p.y + thumb));
        else Checker(ImGui::GetWindowDrawList(), p, ImVec2(p.x + thumb, p.y + thumb), 6 * dpi);
        ImGui::TableNextColumn();
        if (ImGui::Selectable(((x.slot >= 0 ? std::to_string(x.slot) : std::string("NOT SEATED")) + "##star" + std::to_string(i)).c_str(), stars_sel_ == i,
                              ImGuiSelectableFlags_SpanAllColumns, ImVec2(0, thumb)))
          stars_sel_ = i;
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) LaunchImporter(EditArgs(x));
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(x.name.c_str());
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", CharName(x.base_id).c_str());
      }
      if (stars_.stars.empty()) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TableNextColumn();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("No superstars yet: Open the importer...");
      }
      ImGui::EndTable();
    }
    if (ImGui::Button("Open the importer...")) LaunchImporter();
    ImGui::SameLine();
    if (ImGui::Button("Import many...")) LaunchImporter("--importer-bulk");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("A zip full of pac files, or several files at once");
    ImGui::SameLine();
    if (ImGui::Button("Open the superstars folder")) {
      std::error_code ec;
      fs::create_directories(paths_.mods / "superstars", ec);
      ShellExecuteW(hwnd_, L"explore", (paths_.mods / "superstars").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("stars_detail", ImVec2(right_w, list_h), ImGuiChildFlags_Borders);
    if (stars_sel_ >= 0 && stars_sel_ < int(stars_.stars.size())) {
      const importer::Superstar x = stars_.stars[size_t(stars_sel_)];
      Heading(Upper(x.name).c_str(), 1.25f);
      if (x.slot >= 0) ImGui::Text("Slot %d (Downloadable Content)", x.slot);
      else {
        ImGui::Text("Not seated");
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", importer::screen::NotSeatedWhy(importer::screen::SeatedCount(stars_.stars), stars_seats_).c_str());
        ImGui::PopStyleColor();
      }
      ImGui::TextDisabled("Short name: %s", x.short_name.c_str());
      ImGui::Separator();
      ImGui::Text("Based on: %s", CharName(x.base_id).c_str());
      ImGui::Text("Entrance: %s", x.entrance_from < 0 ? "same as base" : CharName(x.entrance_from).c_str());
      ImGui::Text("Ratings: %s", x.copy_ratings ? "copied from base" : "edit later in the game");
      ImGui::TextDisabled("Model: %s", PathStr(fs::path(Widen(x.source_pac)).filename()).c_str());
      if (stars_sel_ < int(stars_tex_.size()) && stars_tex_[size_t(stars_sel_)]) {
        const ImVec2 a = ImGui::GetCursorScreenPos();
        const float sz = std::min(160 * dpi, ImGui::GetContentRegionAvail().x);
        DrawPicture(ImGui::GetWindowDrawList(), stars_tex_[size_t(stars_sel_)], x.render_w[2], x.render_h[2], a, ImVec2(a.x + sz, a.y + sz));
        ImGui::Dummy(ImVec2(sz, sz));
      }
      ImGui::Spacing();
      if (ImGui::Button("Edit in the importer", ImVec2(-1, 0))) LaunchImporter(EditArgs(x));
      if (ImGui::Button("Remove from the pack...", ImVec2(-1, 0))) ImGui::OpenPopup("remove_star");
      if (ImGui::BeginPopupModal("remove_star", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Remove %s from the pack?", x.name.c_str());
        ImGui::TextDisabled("This deletes the superstar's library folder (the model made for the game and the menu pictures). The source file you imported stays in the mods folder.");
        if (ImGui::Button("Remove")) {
          importer::PackState st;
          std::string err;
          // (importer review: by slot, read fresh - the importer may have changed the list since this table was drawn; S110 SEAT-C: by his key, a character with no seat has no slot)
          const bool read = importer::LoadPack(PathStr(paths_.mods), st, err);
          const auto it = std::find_if(st.stars.begin(), st.stars.end(), [&](const importer::Superstar& y) { return !x.key.empty() ? y.key == x.key : x.slot >= 0 && y.slot == x.slot; });
          if (read && it != st.stars.end()) {
            st.stars.erase(it);
            st.needs_build = true;
            if (!importer::SavePack(PathStr(paths_.mods), st, err)) Status("Not removed: " + err);
            else Status("Removed from the pack: " + x.name + " (build the pack again in the importer)");
          } else Status("Not removed: " + (err.empty() ? std::string("he is no longer in the pack") : err));
          stars_sel_ = -1;
          RefreshStars(true);
          ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
      }
      ImGui::Spacing();
      ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
      ImGui::TextWrapped("In the game: he is in the roster's DOWNLOADED SUPERSTARS with the moves, entrance and music of the wrestler he is "
                         "based on. Needs USE MY SUPERSTARS on.");
      ImGui::PopStyleColor();
    } else {
      Heading("SUPERSTARS", 1.25f);
      ImGui::TextWrapped("The Superstar Importer turns a community character (.pac, or a .zip that holds one) into a new superstar: a name, "
                         "a wrestler he is based on (moves, entrance, music, tron), a free roster slot and a roster portrait rendered from "
                         "his model. BUILD INTO GAME puts the pack in the game; nothing of the game's own files is changed.");
      ImGui::Spacing();
      ImGui::TextDisabled("Pick a superstar on the left, or open the importer.");
    }
    ImGui::EndChild();
    // Session 101 (lane 199): one sentence on whether they are in the game, and the one thing to do when not.
    RefreshStarsFit();
    const ImVec4 warn(1, 0.8f, 0.35f, 1);
    if (!stars_.stars.empty() && !s.superstars) ImGui::TextColored(warn, "USE MY SUPERSTARS is off: switch it on (top right) to put them in the game.");
    else if (!stars_.stars.empty() && stars_fit_.build) ImGui::TextColored(warn, "%s PLAY builds them into the game first (a few seconds).", stars_fit_.why.c_str());
    else if (!stars_.stars.empty() && !stars_last_.empty()) ImGui::TextColored(warn, "%s", stars_last_.c_str());  // (S110 SEAT-C: the sentence says what to do)
    else if (!stars_fit_.why.empty()) ImGui::TextColored(warn, "%s%s", stars_fit_.why.c_str(), stars_fit_.build && s.superstars ? " PLAY takes it out first." : "");
    else ImGui::TextDisabled("%s%s", importer::screen::CountLine(stars_.stars, stars_seats_).c_str(), stars_err_.empty() ? "" : ("; " + stars_err_).c_str());
    if (stars_mig_ == importer::Migration::Kept) ImGui::TextDisabled("%s", importer::screen::MigrationLine(stars_mig_).c_str());  // (while it is pending, the check above says it)
  }
  int stars_seats_ = 102;  // S110 SEAT-C: SeatCount, and where the old pack's conversion stands (read with the check before PLAY, below)
  importer::Migration stars_mig_ = importer::Migration::None;
  // Session 101: the check before PLAY, as this tab shows it: recomputed when the pack folder or the DLC settings changed (it scans the
  // DLC folder), at most every 2 s.
  importer::PlayCheck stars_fit_;
  std::string stars_fit_key_, stars_last_;
  std::chrono::steady_clock::time_point stars_fit_at_{};
  void RefreshStarsFit() {
    const auto now = std::chrono::steady_clock::now();
    if (now - stars_fit_at_ < std::chrono::seconds(2)) return;
    stars_fit_at_ = now;
    const std::string key = stars_sig_ + "|" + StarsDlcDir() + "|" + s.dlc_off;
    if (key == stars_fit_key_) return;
    stars_fit_key_ = key;
    stars_seats_ = importer::SeatCount(PathStr(paths_.mods)), stars_mig_ = importer::MigrationState(PathStr(paths_.mods));
    stars_fit_ = importer::CheckBeforePlay(PathStr(paths_.mods), StarsDlcDir(), s.dlc_off);
    stars_last_ = stars_fit_.build ? std::string() : importer::LastGameProblem(PathStr(paths_.mods));
  }
