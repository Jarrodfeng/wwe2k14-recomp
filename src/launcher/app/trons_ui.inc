
  // The characters shown (search / overrides-only filter), in the current sort order (both views use it).
  std::vector<int> Visible() {
    std::vector<int> out;
    const std::string q = Lower(search_);
    for (size_t i = 0; i < chars_.size(); ++i) {
      const auto& c = chars_[i];
      const int tron = c.tron >= 0 ? c.tron : c.id;
      if (only_overrides_ && !overrides_.count(tron)) continue;
      if (!q.empty() && Lower(c.name).find(q) == std::string::npos && std::to_string(c.id).find(q) != 0 &&
          std::to_string(tron).find(q) != 0)
        continue;
      out.push_back(int(i));
    }
    auto last = [&](int i) {  // no value for the sort column: always at the bottom
      const auto& c = chars_[size_t(i)];
      const int tron = c.tron >= 0 ? c.tron : c.id;
      return (s.trons_sort == 2 && c.tron < 0) || (s.trons_sort == 3 && !overrides_.count(tron) && c.note.empty());
    };
    auto key = [&](int i) -> std::tuple<int, std::string, int> {
      const auto& c = chars_[size_t(i)];
      const int tron = c.tron >= 0 ? c.tron : c.id;
      switch (s.trons_sort) {
        case 1: return {c.id, "", 0};
        case 2: return {c.tron >= 0 ? c.tron : 100000, "", c.id};
        case 3: {
          auto ov = overrides_.find(tron);
          return {ov == overrides_.end() ? 1 : 0, ov == overrides_.end() ? c.note : PathStr(ov->second.extension()),
                  0};
        }
        default: return {0, Lower(c.name), c.id};
      }
    };
    std::stable_sort(out.begin(), out.end(), [&](int a, int b) {
      if (last(a) != last(b)) return last(b);
      auto ka = key(a), kb = key(b);
      if (ka == kb) return Lower(chars_[size_t(a)].name) < Lower(chars_[size_t(b)].name);
      return s.trons_sort_desc ? kb < ka : ka < kb;
    });
    return out;
  }

  // View buttons (list / grid), drawn at the right end of the current line.
  // List / Grid, right-aligned on the current line (Session 31: for the titles too).
  void ViewButtons(int& view) {
    const float w = 64 * ImGui::GetStyle().FontScaleDpi;
    ImGui::SameLine(ImGui::GetContentRegionMax().x - 2 * w - ImGui::GetStyle().ItemSpacing.x);
    ImGui::PushID(&view);
    for (int v = 0; v < 2; ++v) {
      if (v) ImGui::SameLine();
      const bool on = view == v;
      if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_TabSelected));
      if (ImGui::Button(v ? "Grid" : "List", ImVec2(w, 0))) view = v;
      if (on) ImGui::PopStyleColor();
    }
    ImGui::PopID();
  }

  void TronsList(const std::vector<int>& rows) {
    if (!ImGui::BeginTable("chars", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                           ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Sortable))
      return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Character", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultSort, 3.0f);
    ImGui::TableSetupColumn("Id", ImGuiTableColumnFlags_WidthStretch, 0.6f);
    ImGui::TableSetupColumn("Tron", ImGuiTableColumnFlags_WidthStretch, 0.7f);
    ImGui::TableSetupColumn("Override", ImGuiTableColumnFlags_WidthStretch, 1.6f);
    ImGui::TableHeadersRow();
    if (ImGuiTableSortSpecs* sp = ImGui::TableGetSortSpecs()) {
      const ImGuiSortDirection want = s.trons_sort_desc ? ImGuiSortDirection_Descending : ImGuiSortDirection_Ascending;
      if (sp->SpecsDirty && sp->SpecsCount > 0 && table_sort_ready_) {  // a header click
        s.trons_sort = sp->Specs[0].ColumnIndex;
        s.trons_sort_desc = sp->Specs[0].SortDirection == ImGuiSortDirection_Descending;
      } else if (sp->SpecsCount == 0 || sp->Specs[0].ColumnIndex != s.trons_sort ||
                 sp->Specs[0].SortDirection != want) {  // changed elsewhere (saved, grid, automation)
        ImGui::TableSetColumnSortDirection(s.trons_sort, want, false);
      }
      sp->SpecsDirty = false;
      table_sort_ready_ = true;
    }
    for (int i : rows) {
      const auto& c = chars_[size_t(i)];
      const int tron = c.tron >= 0 ? c.tron : c.id;
      const auto ov = overrides_.find(tron);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      const bool sel = i == selected_;
      if (ImGui::Selectable((c.name + "##" + std::to_string(i)).c_str(), sel, ImGuiSelectableFlags_SpanAllColumns))
        selected_ = i;
      if (sel && scroll_to_selected_) ImGui::SetScrollHereY(), scroll_to_selected_ = false;
      ImGui::TableNextColumn();
      ImGui::Text("%d", c.id);
      ImGui::TableNextColumn();
      if (c.tron >= 0) ImGui::Text("%d", c.tron);
      else ImGui::TextDisabled("-");
      ImGui::TableNextColumn();
      if (ov != overrides_.end())
        ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1), "%s", PathStr(ov->second.extension()).c_str() + 1);
      else if (!c.note.empty()) ImGui::TextDisabled("%s", c.note.c_str());
    }
    ImGui::EndTable();
  }

  // Grid: one tile per character (thumbnail, name, tron number); at least 2 columns (Jarrod, Session 31: also at the
  // default window size), more as the window grows.
  void TronsGrid(const std::vector<int>& rows) {
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    const float avail = ImGui::GetContentRegionAvail().x;
    const float gap = 10 * dpi;
    const int cols = std::max(2, int((avail + gap) / (200 * dpi + gap)));
    const float tile = (avail - gap * float(cols - 1)) / float(cols);
    const float text_h = ImGui::GetTextLineHeightWithSpacing() * 2;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    hovered_tron_ = -1;
    for (size_t n = 0; n < rows.size(); ++n) {
      const int i = rows[n];
      const auto& c = chars_[size_t(i)];
      const int tron = c.tron >= 0 ? c.tron : c.id;
      if (n % size_t(cols)) ImGui::SameLine(0, gap);
      ImGui::PushID(i);
      const ImVec2 p = ImGui::GetCursorScreenPos();
      const bool clicked = ImGui::InvisibleButton("tile", ImVec2(tile, tile + text_h));
      const bool hov = ImGui::IsItemHovered();
      if (clicked) selected_ = i;
      if (i == selected_ && scroll_to_selected_) ImGui::SetScrollHereY(), scroll_to_selected_ = false;
      const ImVec2 a = p, b = ImVec2(p.x + tile, p.y + tile);
      dl->AddRectFilled(a, b, IM_COL32(24, 26, 32, 255), 6 * dpi);
      if (c.tron >= 0 || overrides_.count(tron)) {
        if (hov) hovered_tron_ = tron;
        if (!DrawMedia(dl, tron, a, b, hov || force_hover_ == tron, 0.0f)) {
          const std::string t = std::to_string(tron);
          ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 2.2f);
          const ImVec2 ts = ImGui::CalcTextSize(t.c_str());
          dl->AddText(ImVec2(a.x + (tile - ts.x) / 2, a.y + (tile - ts.y) / 2), IM_COL32(90, 95, 110, 255), t.c_str());
          ImGui::PopFont();
        }
      } else {
        const char* t = "no tron";
        const ImVec2 ts = ImGui::CalcTextSize(t);
        dl->AddText(ImVec2(a.x + (tile - ts.x) / 2, a.y + (tile - ts.y) / 2), IM_COL32(90, 95, 110, 255), t);
      }
      const ImU32 border = i == selected_ ? IM_COL32(230, 70, 60, 255) : hov ? IM_COL32(140, 145, 160, 255)
                                                                              : IM_COL32(60, 64, 76, 255);
      dl->AddRect(a, b, border, 6 * dpi, 0, i == selected_ ? 3 * dpi : 1.5f * dpi);
      if (overrides_.count(tron)) {
        const char* badge = "custom";
        const ImVec2 bs = ImGui::CalcTextSize(badge);
        const ImVec2 b0(b.x - bs.x - 14 * dpi, a.y + 6 * dpi);
        dl->AddRectFilled(b0, ImVec2(b.x - 6 * dpi, b0.y + bs.y + 4 * dpi), IM_COL32(40, 120, 60, 230), 4 * dpi);
        dl->AddText(ImVec2(b0.x + 4 * dpi, b0.y + 2 * dpi), IM_COL32_WHITE, badge);
      }
      const std::string sub = c.tron >= 0 ? "tron " + std::to_string(c.tron) : std::string("no tron");
      ImGui::PushClipRect(ImVec2(a.x, b.y), ImVec2(b.x, b.y + text_h), true);
      dl->AddText(ImVec2(a.x + 2, b.y + 3 * dpi), ImGui::GetColorU32(ImGuiCol_Text), c.name.c_str());
      dl->AddText(ImVec2(a.x + 2, b.y + 3 * dpi + ImGui::GetTextLineHeight()), ImGui::GetColorU32(ImGuiCol_TextDisabled),
                  sub.c_str());
      ImGui::PopClipRect();
      ImGui::PopID();
    }
  }

  // ---- S110 TRON-B: CUSTOM TRONS (src\tron_slots_ui_core.h decides; this only draws). Only FILLED slots are listed; a name with no video
  // and a video with no name are problems with a one-line fix. The slot's video is assigned through AssignTron like any tron.
  fs::path CustomDir() const { return paths_.mods / "titantron"; }
  void RefreshCustom(bool force = false) {
    const auto now = std::chrono::steady_clock::now();
    if (!force && now - custom_checked_ < std::chrono::seconds(1)) return;
    custom_checked_ = now;
    custom_lib_ = wwe2k14::tron_slots_core::ReadSlots(CustomDir());
    custom_model_ = wwe2k14::tron_slots_ui::BuildModel(CustomDir(), custom_lib_);
  }
  void CustomDone(const std::string& err, const std::string& ok) {
    RefreshOverrides();
    ForgetMedia();
    thumb_.reset();
    RefreshCustom(true);
    Status(err.empty() ? ok : "Not done: " + err);
  }
  void CustomWarnBik2(const fs::path& f) {
    if (Lower(PathStr(f.extension())) != ".bik2") return;
    const VideoInfo vi = BinkInfo(f);
    if (vi.ok)
      if (const std::string w = wwe2k14::tron_slots_ui::SheetWarning(".bik2", vi.w, vi.h); !w.empty())
        ImGui::TextColored(ImVec4(1, 0.75f, 0.3f, 1), "%s", w.c_str());
  }
  void CustomSection() {
    namespace tu = wwe2k14::tron_slots_ui;
    RefreshCustom();
    Heading("CUSTOM TRONS", 1.1f);
    for (const auto& sl : custom_model_.filled) {
      const bool sel = selected_ < 0 && selected_tron_ == sl.id;
      if (ImGui::Selectable((sl.name + "##ct" + std::to_string(sl.id)).c_str(), sel)) Select(sl.id);
    }
    // (review) a copy: "Add the video..." refreshes custom_model_ (CustomDone) inside the loop, which freed the vector being walked; and the
    // name popup is opened OUTSIDE the PushID, where BeginPopupModal("custom_name") below looks it up (OpenPopup hashes the ID stack).
    const std::vector<tu::Problem> problems = custom_model_.problems;
    bool open_name = false;
    for (size_t k = 0; k < problems.size(); ++k) {
      const tu::Problem& p = problems[k];
      ImGui::PushID(int(k));
      ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "Problem: %s", p.text.c_str());
      if (p.kind == tu::Problem::kVideoMissing && ImGui::SmallButton("Add the video...")) {
        if (auto f = PickFile(hwnd_)) {
          const std::string err = tu::RefillSlot(CustomDir(), p.id, [&](int n) { return AssignTron(paths_, n, *f); });
          CustomDone(err, "Custom tron " + std::to_string(p.id) + " has its video again");
        }
      }
      if (p.kind == tu::Problem::kNameMissing && ImGui::SmallButton("Give it a name...")) {
        custom_fix_id_ = p.id, custom_pending_.clear(), custom_name_new_ = "Custom tron " + std::to_string(p.id);
        open_name = true;
      }
      ImGui::PopID();
    }
    if (open_name) ImGui::OpenPopup("custom_name");
    if (ImGui::Button("Add a custom tron...")) {
      if (auto f = PickFile(hwnd_)) {
        custom_pending_ = *f, custom_fix_id_ = -1;
        custom_name_new_ = wwe2k14::tron_slots_core::CleanName(PathStr(f->stem()));
        ImGui::OpenPopup("custom_name");
      }
    }
    if (custom_model_.filled.empty() && custom_model_.problems.empty())
      ImGui::TextDisabled("None yet. A custom tron is a video that you can give to a superstar you imported.");
    if (ImGui::BeginPopupModal("custom_name", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
      if (custom_fix_id_ < 0) ImGui::Text("Add %s", PathStr(custom_pending_.filename()).c_str());
      else ImGui::Text("Name for the video of custom tron %d", custom_fix_id_);
      if (custom_fix_id_ < 0) CustomWarnBik2(custom_pending_);
      ImGui::TextUnformatted("Name (shown in lists):");
      ImGui::SetNextItemWidth(320 * Dpi());
      ImGui::InputText("##cname_new", &custom_name_new_);
      const bool can = !wwe2k14::tron_slots_core::CleanName(custom_name_new_).empty();
      ImGui::BeginDisabled(!can);
      if (ImGui::Button(custom_fix_id_ < 0 ? "Add" : "Save the name")) {
        if (custom_fix_id_ < 0) {
          int id = -1;
          const fs::path f = custom_pending_;
          const std::string err = tu::AddSlot(CustomDir(), custom_name_new_, [&](int n) { return AssignTron(paths_, n, f); }, &id);
          CustomDone(err, "Added " + wwe2k14::tron_slots_core::CleanName(custom_name_new_) + (s.mods ? "" : "  (switch USE MY VIDEOS on)"));
          if (err.empty()) Select(id);
        } else {
          CustomDone(tu::RenameSlot(CustomDir(), custom_fix_id_, custom_name_new_), "Named: " + custom_name_new_);
        }
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndDisabled();
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }
  }
  void CustomDetail(int tron) {
    namespace tu = wwe2k14::tron_slots_ui;
    RefreshCustom();
    const wwe2k14::tron_slots_core::Slot* sl = nullptr;
    for (const auto& x : custom_model_.filled)
      if (x.id == tron) sl = &x;
    if (!sl) {  // removed, or its video went: nothing to show (the section lists the problem)
      ImGui::TextWrapped("This custom tron is not in the list any more.");
      return;
    }
    Heading(Upper(sl->name).c_str(), 1.25f);
    ImGui::TextDisabled("Custom tron, number %d", tron);
    ImGui::Separator();
    const fs::path ov = OverrideFor(paths_, tron);
    UpdateThumb(ov);
    ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1), "Plays: %s", PathStr(ov.filename()).c_str());
    if (thumb_) {
      const auto& vi = thumb_->info;
      if (vi.ok) ImGui::TextDisabled("%s, %dx%d, %.2f fps, %.1f s", vi.format.c_str(), vi.w, vi.h, vi.fps, vi.seconds);
      else ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "Problem: %s", vi.error.c_str());
      if (thumb_->srv) {
        const float w = ImGui::GetContentRegionAvail().x;
        ImGui::Image(ImTextureRef(reinterpret_cast<ImTextureID>(thumb_->srv)),
                     ImVec2(w, w * float(thumb_->h) / float(std::max(1, thumb_->w))));
      } else if (vi.ok) {
        ImGui::TextDisabled("(Bink 2: no preview here; it plays in the game)");
      }
    }
    CustomWarnBik2(ov);
    ImGui::Separator();
    if (custom_name_for_ != tron) custom_name_edit_ = sl->name, custom_name_for_ = tron;
    ImGui::TextUnformatted("Name in lists:");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##cname", &custom_name_edit_);
    if (ImGui::Button("Rename", ImVec2(-1, 0))) {
      CustomDone(tu::RenameSlot(CustomDir(), tron, custom_name_edit_), "Renamed: " + custom_name_edit_);
      custom_name_for_ = -1;
    }
    if (ImGui::Button("Replace the video...", ImVec2(-1, 0))) {
      if (auto f = PickFile(hwnd_)) CustomDone(AssignTron(paths_, tron, *f), "Custom tron " + std::to_string(tron) + " now plays " + PathStr(f->filename()));
    }
    ImGui::Spacing();
    if (ImGui::Button("Remove this custom tron...", ImVec2(-1, 0))) ImGui::OpenPopup("remove_custom");
    if (ImGui::BeginPopupModal("remove_custom", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::Text("Remove %s and delete its video?", sl->name.c_str());
      ImGui::TextDisabled("A superstar or a saved entrance that uses it will show an idle big screen. Its number is given to a new\n"
                          "custom tron only when all 116 are used.");
      if (ImGui::Button("Remove")) {
        const std::string name = sl->name;
        CustomDone(tu::RemoveSlot(CustomDir(), tron), "Removed: " + name);
        selected_tron_ = -1;
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("Give it to a superstar you imported in MODS > SUPERSTARS (TRON). MP4 / MOV / MKV: any size, looped, no sound. "
                       "Only the big screen plays it. Needs USE MY VIDEOS on.");
    ImGui::PopStyleColor();
  }
  wwe2k14::tron_slots_core::Library custom_lib_;
  wwe2k14::tron_slots_ui::Model custom_model_;
  std::chrono::steady_clock::time_point custom_checked_{};
  std::string custom_name_edit_, custom_name_new_;
  int custom_name_for_ = -1, custom_fix_id_ = -1;
  fs::path custom_pending_;

  void TronsTab() {
    const float right_w = 430 * ImGui::GetStyle().FontScaleDpi;
    const ImGuiStyle& st = ImGui::GetStyle();
    float extras = ImGui::CalcTextSize("Overrides only").x + ImGui::GetFrameHeight() + st.ItemInnerSpacing.x +
                   st.ItemSpacing.x * 2;
    if (s.trons_view == 1) extras += 130 * st.FontScaleDpi + ImGui::GetFrameHeight() + st.ItemSpacing.x * 2;
    ImGui::SetNextItemWidth(std::max(120.0f, ImGui::GetContentRegionAvail().x - right_w - 8 - extras));
    ImGui::InputTextWithHint("##search", "Search name, id or tron number", &search_);
    ImGui::SameLine();
    ImGui::Checkbox("Overrides only", &only_overrides_);
    if (s.trons_view == 1) {  // the grid has no column headers: sort here
      ImGui::SameLine();
      ImGui::SetNextItemWidth(130 * ImGui::GetStyle().FontScaleDpi);
      const char* keys[] = {"Name", "Id", "Tron", "Override"};
      ImGui::Combo("##sort", &s.trons_sort, keys, 4);
      ImGui::SameLine();
      if (ImGui::ArrowButton("##dir", s.trons_sort_desc ? ImGuiDir_Down : ImGuiDir_Up)) s.trons_sort_desc = !s.trons_sort_desc;
    }
    ViewButtons(s.trons_view);

    // Left: the characters (list or grid).
    const std::vector<int> rows = Visible();
    ImGui::BeginChild("left", ImVec2(-right_w - 8, -ImGui::GetFrameHeightWithSpacing() * 1.4f), ImGuiChildFlags_None);
    ImGui::BeginChild("list", ImVec2(0, -150 * ImGui::GetStyle().FontScaleDpi), ImGuiChildFlags_Borders);
    if (s.trons_view == 1) TronsGrid(rows);
    else TronsList(rows);
    ImGui::EndChild();
    ImGui::BeginChild("customs", ImVec2(0, 0), ImGuiChildFlags_Borders);  // S110 TRON-B: CUSTOM TRONS
    CustomSection();
    ImGui::EndChild();
    ImGui::EndChild();
    ImGui::SameLine();

    // Right: the selected character.
    ImGui::BeginChild("detail", ImVec2(right_w, -ImGui::GetFrameHeightWithSpacing() * 1.4f), ImGuiChildFlags_Borders);
    const int tron = SelectedTron();
    if (tron < 0) {
      ImGui::TextWrapped("Pick a character on the left.");
      ImGui::Spacing();
      ImGui::TextWrapped("Each wrestler's entrance plays one titantron file, numbered like the wrestler "
                         "(Lesnar 343, Bryan 324). Some share one (The Shield: 403) or use an era version "
                         "(Stone Cold: 658). Assigning a video copies it into the mods folder; the game's own files "
                         "are never changed. Switch USE MY VIDEOS on (above).");
    } else if (selected_ < 0 && wwe2k14::tron_slots_core::IsSlotId(tron)) {  // S110 TRON-B: a custom tron
      CustomDetail(tron);
    } else {
      const Character* c = selected_ >= 0 ? &chars_[size_t(selected_)] : nullptr;
      Heading(c ? Upper(c->name).c_str() : "TRON", 1.25f);
      if (c) ImGui::Text("Character id %d, kind %s", c->id, c->kind.c_str());
      ImGui::Text("Titantron %d%s%s", tron, c && !c->note.empty() ? " - " : "", c ? c->note.c_str() : "");
      if (OriginalTronExists(paths_, tron)) {
        if (orig_tron_ != tron) orig_info_ = OriginalTronInfo(paths_, tron), orig_tron_ = tron;
        ImGui::TextDisabled("Original: %s, %dx%d, %.2f fps, %.1f s", orig_info_.format.c_str(), orig_info_.w,
                            orig_info_.h, orig_info_.fps, orig_info_.seconds);
      } else {
        ImGui::TextDisabled("Original: no movies\\titantron\\%d.bik2 in the game files", tron);
      }
      ImGui::Separator();
      const fs::path ov = OverrideFor(paths_, tron);
      UpdateThumb(ov);
      if (ov.empty()) {
        ImGui::TextUnformatted("Plays: the original tron");
        const float w = ImGui::GetContentRegionAvail().x, hgt = w * 224.0f / 320.0f;
        const ImVec2 a = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(w, hgt));
        const bool hov = ImGui::IsItemHovered() || force_hover_ == tron;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(a, ImVec2(a.x + w, a.y + hgt), IM_COL32(24, 26, 32, 255), 6);
        if (!DrawMedia(dl, tron, a, ImVec2(a.x + w, a.y + hgt), hov, 0.0f)) {
          const char* t = "No preview yet: Build previews (below)";
          const ImVec2 ts = ImGui::CalcTextSize(t);
          dl->AddText(ImVec2(a.x + (w - ts.x) / 2, a.y + (hgt - ts.y) / 2), IM_COL32(110, 115, 130, 255), t);
        } else if (!hov) {
          ImGui::TextDisabled("Hover the picture to play it");
        }
      } else {
        ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1), "Plays: %s", PathStr(ov.filename()).c_str());
        if (thumb_) {
          const auto& vi = thumb_->info;
          if (vi.ok) ImGui::TextDisabled("%s, %dx%d, %.2f fps, %.1f s", vi.format.c_str(), vi.w, vi.h, vi.fps, vi.seconds);
          else ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "Problem: %s", vi.error.c_str());
          if (thumb_->srv) {
            const float w = ImGui::GetContentRegionAvail().x;
            ImGui::Image(ImTextureRef(reinterpret_cast<ImTextureID>(thumb_->srv)),
                         ImVec2(w, w * float(thumb_->h) / float(std::max(1, thumb_->w))));
          } else if (vi.ok) {
            ImGui::TextDisabled("(Bink 2: no preview here; it plays in the game)");
          }
        }
        if (Lower(PathStr(ov.extension())) != ".bik2" && orig_info_.w == 320 && orig_info_.h == 320) BannerPanel(tron);
      }
      ImGui::Spacing();
      if (ImGui::Button("Assign a video...", ImVec2(-1, 0))) {
        if (auto f = PickFile(hwnd_)) DoAssign(tron, *f);
      }
      ImGui::BeginDisabled(ov.empty());
      if (ImGui::Button("Revert to the original", ImVec2(-1, 0))) DoRevert(tron);
      ImGui::EndDisabled();
      ImGui::Spacing();
      ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
      ImGui::TextWrapped("MP4 / MOV / MKV: any size (shown up to 1024 wide), looped, no sound. "
                         ".bik2: played by the game's own Bink decoder.");
      ImGui::PopStyleColor();
      // Session 33's entrance music moved to the MUSIC tab (Session 75, Jarrod: music on its own tab).
      if (c) {
        ImGui::Separator();
        const fs::path mu = MusicFor(paths_, c->id);
        ImGui::TextDisabled("Entrance music: %s (MUSIC tab)", mu.empty() ? "the original" : PathStr(mu.filename()).c_str());
      }
    }
    ImGui::EndChild();

    // Bottom row (Session 31: the folder's path in the button's tooltip, so the row fits the window).
    ImGui::Text("%zu override(s)", overrides_.size());
    ImGui::SameLine();
    if (ImGui::SmallButton("Open mods folder")) {
      std::error_code ec;
      fs::create_directories(paths_.mods / "titantron", ec);
      ShellExecuteW(hwnd_, L"explore", (paths_.mods / "titantron").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", PathStr(paths_.mods / "titantron").c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(overrides_.empty());
    if (ImGui::SmallButton("Revert all...")) ImGui::OpenPopup("revert_all");
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::SmallButton("Refresh")) RefreshOverrides(), thumb_.reset(), stills_.clear(), ForgetMedia();
    ImGui::SameLine();
    {
      const int n = CachedPreviews();
      ImGui::BeginDisabled(phone_export_ && !phone_export_->done);  // Session 98: not while Export for phone reads the cache
      if (ImGui::SmallButton(n ? "Rebuild previews" : "Build previews")) BuildPreviews();
      ImGui::EndDisabled();
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Runs the game once: its own Bink decoder makes a thumbnail and a faithful MP4 copy of every\n"
                          "original tron (every frame, full length, original size; about 2-3 minutes, ~0.5 GB; the\n"
                          "game closes itself). Saved in\n%s",
                          PathStr(TronCacheDir()).c_str());
      ImGui::SameLine();
      ImGui::TextDisabled("%d preview(s)", n);
      // Session 98: EXPORT FOR PHONE (its own block: src\tron_zip.h, ExportForPhone)
      ImGui::SameLine();
      ImGui::BeginDisabled(n == 0 || (phone_export_ && !phone_export_->done));
      if (ImGui::SmallButton("Export for phone...")) ExportForPhone({});
      ImGui::EndDisabled();
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(n == 0 ? "Build previews first: the export is the previews as one .zip for the phone launcher."
                                 : "Saves the previews (pictures and videos) as one .zip. Copy it to your phone and use\n"
                                   "IMPORT TRON PREVIEWS on the phone launcher's Arena Viewer page: no need to build them\n"
                                   "again on the phone.");
      PhoneExportProgress();
      PhoneExportPopup();
    }
    if (ImGui::BeginPopupModal("revert_all", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::Text("Remove all %zu tron override(s) from the mods folder?", overrides_.size());
      if (ImGui::Button("Remove all")) {
        Status("Reverted " + std::to_string(RevertAll(paths_)) + " file(s)");
        RefreshOverrides(), thumb_.reset(), ForgetMedia();
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }
  }

  // ---------------- Banner ring (Session 18): the strips under a 320x320 tron's big screen
  void BannerPanel(int tron) {
    if (banner_tron_ != tron || std::chrono::steady_clock::now() - banner_checked_ > std::chrono::seconds(2))
      banner_ = GetBanner(paths_, tron), banner_tron_ = tron, banner_checked_ = std::chrono::steady_clock::now();
    ImGui::Spacing();
    ImGui::SeparatorText("Banner ring");
    const char* names[] = {"Original (the wrestler's own)", "From my video (a band of it)", "None (black)",
                           "Separate video..."};
    int cur = banner_.mode == BannerMode::kOriginal ? 0 : banner_.mode == BannerMode::kVideo ? 1
              : banner_.mode == BannerMode::kNone ? 2 : 3;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##banner", cur == 3 ? ("Separate: " + PathStr(banner_.file.filename())).c_str() : names[cur])) {
      for (int i = 0; i < 4; ++i)
        if (ImGui::Selectable(names[i], i == cur)) {
          if (i == 3) {
            if (auto f = PickFile(hwnd_)) DoBanner(tron, PathStr(*f));
          } else {
            DoBanner(tron, i == 0 ? "original" : i == 1 ? "video" : "none");
          }
        }
      ImGui::EndCombo();
    }
    // The sheet as the game gets it: the big screen (top 224 rows) and the four 24-row strips.
    const float w = ImGui::GetContentRegionAvail().x, big_h = w * kSheetBigRows / 320.0f;
    const float strip_h = w * kSheetStripRows / 320.0f;
    const ImVec2 a = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(w, big_h + strip_h * kSheetStrips));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, ImVec2(a.x + w, a.y + big_h + strip_h * kSheetStrips), IM_COL32(8, 8, 10, 255));
    if (thumb_ && thumb_->srv)
      dl->AddImage(ImTextureRef(reinterpret_cast<ImTextureID>(thumb_->srv)), a, ImVec2(a.x + w, a.y + big_h));
    const float y0 = a.y + big_h;
    if (banner_.mode == BannerMode::kOriginal) {
      int sw = 0, sh = 0;
      const fs::path png = TronCacheDir() / (std::to_string(tron) + ".png");
      std::error_code ec;
      if (auto* srv = fs::exists(png, ec) ? StillImage(tron + 200000, png, &sw, &sh) : nullptr)
        dl->AddImage(ImTextureRef(reinterpret_cast<ImTextureID>(srv)), ImVec2(a.x, y0),
                     ImVec2(a.x + w, y0 + strip_h * kSheetStrips), ImVec2(0, kSheetBigRows / 320.0f), ImVec2(1, 1));
      else
        dl->AddText(ImVec2(a.x + 6, y0 + 4), IM_COL32(110, 115, 130, 255), "(original strips: Build previews to see)");
    } else if (banner_.mode != BannerMode::kNone) {
      ID3D11ShaderResourceView* srv = nullptr;
      float v0 = 0, v1 = 1;
      bool split = false;  // a wide banner video runs around the ring: one quarter per strip (as the game does)
      if (banner_.mode == BannerMode::kVideo) {
        srv = thumb_ ? thumb_->srv : nullptr;
        const float band = float(kSheetStripRows) / float(kSheetBigRows);
        v0 = 0.5f - band / 2, v1 = 0.5f + band / 2;
      } else {
        int sw = 0, sh = 0;
        srv = StillImage(tron + 100000, banner_.file, &sw, &sh);
        split = sh > 0 && sw * kSheetStripRows > sh * 2 * 320;
      }
      for (int i = 0; srv && i < kSheetStrips; ++i) {
        const float u0 = split ? i / float(kSheetStrips) : 0.0f, u1 = split ? (i + 1) / float(kSheetStrips) : 1.0f;
        dl->AddImage(ImTextureRef(reinterpret_cast<ImTextureID>(srv)), ImVec2(a.x, y0 + strip_h * i),
                     ImVec2(a.x + w, y0 + strip_h * (i + 1)), ImVec2(u0, v0), ImVec2(u1, v1));
      }
    }
    for (int i = 0; i <= kSheetStrips; ++i)
      dl->AddLine(ImVec2(a.x, y0 + strip_h * i), ImVec2(a.x + w, y0 + strip_h * i), IM_COL32(0, 0, 0, 200), 1.0f);
    ImGui::TextDisabled("Top: the big screen. Below: the 4 strips; the banner ring around the arena shows them end to "
                        "end (1, 2, 3, 4, 1, ...). A separate banner video wider than 27:1 runs around the ring.");
  }
