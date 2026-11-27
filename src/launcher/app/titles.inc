
  // ---------------- Titles (Session 18): the title reveal videos (Bink 1 with alpha, 880x720)
  struct Title {
    int id = 0;
    std::string name;
  };
  std::vector<Title> titles_;
  bool titles_loaded_ = false;
  int title_sel_ = -1;
  bool title_alpha_ = true;  // assign: the video is stacked alpha
  BannerChoice banner_;
  int banner_tron_ = -1;
  std::chrono::steady_clock::time_point banner_checked_;

  void LoadTitles() {
    titles_loaded_ = true;
    titles_.clear();
    static const std::map<int, const char*> known = {
        {500, "WWE Championship"}, {950, "WWE Championship (older design)"}, {970, "United States Championship"}};
    // Session 26: through GameFiles (a folder or a disc image).
    for (const std::string& name : GameFilesAt(paths_.game_data).List("movies\\titantron")) {
      const fs::path file(name);
      const std::string stem = PathStr(file.stem());
      if (Lower(PathStr(file.extension())) != ".bik2" || stem.empty() ||
          !std::all_of(stem.begin(), stem.end(), ::isdigit))
        continue;
      const int id = std::stoi(stem);
      const VideoInfo vi = OriginalTronInfo(paths_, id);
      if (!vi.ok || !vi.alpha) continue;  // only the files that are transparent in the game itself
      auto k = known.find(id);
      titles_.push_back({id, k != known.end() ? k->second : "Title reveal " + stem});
    }
    std::sort(titles_.begin(), titles_.end(), [](const Title& x, const Title& y) { return x.id < y.id; });
  }

  // A checkerboard behind transparent pictures.
  static void Checker(ImDrawList* dl, ImVec2 a, ImVec2 b, float cell) {
    dl->AddRectFilled(a, b, IM_COL32(70, 72, 78, 255));
    for (float y = a.y; y < b.y; y += cell)
      for (float x = a.x + (int((y - a.y) / cell) % 2 ? cell : 0); x < b.x; x += 2 * cell)
        dl->AddRectFilled(ImVec2(x, y), ImVec2(std::min(x + cell, b.x), std::min(y + cell, b.y)),
                          IM_COL32(110, 112, 120, 255));
  }

  // Session 31 (Jarrod): the titles as a grid too: tiles in the videos' shape (880 x 720) over a checkerboard (their
  // transparency), name and file below, at least 2 columns; hover plays the preview, a click selects.
  void TitlesGrid() {
    const float dpi = Dpi(), avail = ImGui::GetContentRegionAvail().x, gap = 10 * dpi;
    const int cols = std::max(2, int((avail + gap) / (200 * dpi + gap)));
    const float tw = (avail - gap * float(cols - 1)) / float(cols), th = tw * 720.0f / 880.0f;
    const float text_h = ImGui::GetTextLineHeightWithSpacing() * 2;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (size_t n = 0; n < titles_.size(); ++n) {
      const Title& t = titles_[n];
      if (n % size_t(cols)) ImGui::SameLine(0, gap);
      ImGui::PushID(t.id);
      const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + tw, a.y + th);
      if (ImGui::InvisibleButton("tile", ImVec2(tw, th + text_h))) title_sel_ = t.id;
      const bool hov = ImGui::IsItemHovered() || force_hover_ == t.id;
      Checker(dl, a, b, 10 * dpi);
      if (!DrawMedia(dl, t.id, a, b, hov, 0.0f)) {
        const std::string s_id = std::to_string(t.id);
        const ImVec2 ts = ImGui::CalcTextSize(s_id.c_str());
        dl->AddText(ImVec2(a.x + (tw - ts.x) / 2, a.y + (th - ts.y) / 2), IM_COL32(230, 230, 235, 255), s_id.c_str());
      }
      const bool sel = t.id == title_sel_;
      dl->AddRect(a, b, sel ? IM_COL32(230, 70, 60, 255) : hov ? IM_COL32(140, 145, 160, 255) : IM_COL32(60, 64, 76, 255),
                  0, 0, sel ? 3 * dpi : 1.5f * dpi);
      if (overrides_.count(t.id)) {
        const char* badge = "custom";
        const ImVec2 bs = ImGui::CalcTextSize(badge), b0(b.x - bs.x - 14 * dpi, a.y + 6 * dpi);
        dl->AddRectFilled(b0, ImVec2(b.x - 6 * dpi, b0.y + bs.y + 4 * dpi), IM_COL32(40, 120, 60, 230), 4 * dpi);
        dl->AddText(ImVec2(b0.x + 4 * dpi, b0.y + 2 * dpi), IM_COL32_WHITE, badge);
      }
      const std::string sub = "file " + std::to_string(t.id);
      ImGui::PushClipRect(ImVec2(a.x, b.y), ImVec2(b.x, b.y + text_h), true);
      dl->AddText(ImVec2(a.x + 2, b.y + 3 * dpi), ImGui::GetColorU32(ImGuiCol_Text), t.name.c_str());
      dl->AddText(ImVec2(a.x + 2, b.y + 3 * dpi + ImGui::GetTextLineHeight()), ImGui::GetColorU32(ImGuiCol_TextDisabled),
                  sub.c_str());
      ImGui::PopClipRect();
      ImGui::PopID();
    }
  }

  void TitlesTab() {
    if (!titles_loaded_) LoadTitles();
    const float dpi = ImGui::GetStyle().FontScaleDpi, right_w = 430 * dpi;
    ImGui::TextDisabled("%zu title reveal(s)", titles_.size());
    ViewButtons(s.titles_view);
    ImGui::TextWrapped("Title reveal videos: the belt animations shown with a champion's entrance. They are "
                       "transparent in the game. A replacement keeps its transparency when it is a stacked-alpha "
                       "video (colour on top, alpha as grey below; make one with tools\\trons\\make_stacked_alpha.py).");
    ImGui::BeginChild("tlist", ImVec2(-right_w - 8, -ImGui::GetFrameHeightWithSpacing() * 1.4f), ImGuiChildFlags_Borders);
    if (s.titles_view == 1) {
      TitlesGrid();
    } else if (ImGui::BeginTable("titles", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                                  ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupScrollFreeze(0, 1);
      ImGui::TableSetupColumn("Title", ImGuiTableColumnFlags_WidthStretch, 3.0f);
      ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthStretch, 0.7f);
      ImGui::TableSetupColumn("Override", ImGuiTableColumnFlags_WidthStretch, 1.4f);
      ImGui::TableHeadersRow();
      for (const auto& t : titles_) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (ImGui::Selectable((t.name + "##t" + std::to_string(t.id)).c_str(), t.id == title_sel_,
                              ImGuiSelectableFlags_SpanAllColumns))
          title_sel_ = t.id;
        ImGui::TableNextColumn();
        ImGui::Text("%d", t.id);
        ImGui::TableNextColumn();
        auto ov = overrides_.find(t.id);
        if (ov != overrides_.end())
          ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1), "%s", TronExt(ov->second).c_str() + 1);
      }
      ImGui::EndTable();
    }
    if (titles_.empty()) ImGui::TextDisabled("No title reveals found in %s", PathStr(paths_.game_data).c_str());
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("tdetail", ImVec2(right_w, -ImGui::GetFrameHeightWithSpacing() * 1.4f), ImGuiChildFlags_Borders);
    const Title* t = nullptr;
    for (const auto& x : titles_)
      if (x.id == title_sel_) t = &x;
    if (!t) {
      ImGui::TextWrapped("Pick a title on the left. 500 = WWE Championship, 970 = United States Championship; the "
                         "950s are probably the retro titles of 30 Years of WrestleMania.");
    } else {
      ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.2f);
      ImGui::TextUnformatted(t->name.c_str());
      ImGui::PopFont();
      const VideoInfo vi = OriginalTronInfo(paths_, t->id);
      ImGui::TextDisabled("File %d.bik2: %s, %dx%d, %.2f fps, %.1f s", t->id, vi.format.c_str(), vi.w, vi.h, vi.fps,
                          vi.seconds);
      const fs::path ov = OverrideFor(paths_, t->id);
      if (ov.empty()) ImGui::TextUnformatted("Plays: the original");
      else ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1), "Plays: %s%s", PathStr(ov.filename()).c_str(),
                              IsStackedAlpha(ov) ? " (transparent)" : " (no transparency: solid)");
      const float w = ImGui::GetContentRegionAvail().x, hgt = w * 720.0f / 880.0f;
      const ImVec2 a = ImGui::GetCursorScreenPos();
      ImGui::Dummy(ImVec2(w, hgt));
      const bool hov = ImGui::IsItemHovered() || force_hover_ == t->id;
      ImDrawList* dl = ImGui::GetWindowDrawList();
      Checker(dl, a, ImVec2(a.x + w, a.y + hgt), 12 * dpi);
      if (!DrawMedia(dl, t->id, a, ImVec2(a.x + w, a.y + hgt), hov, 0.0f)) {
        const char* m = "No preview yet: Build previews (Trons)";
        const ImVec2 ts = ImGui::CalcTextSize(m);
        dl->AddText(ImVec2(a.x + (w - ts.x) / 2, a.y + (hgt - ts.y) / 2), IM_COL32(230, 230, 235, 255), m);
      } else if (!hov) {
        ImGui::TextDisabled("Hover the picture to play it (checkerboard = transparent)");
      }
      ImGui::Spacing();
      ImGui::Checkbox("My video is stacked alpha (keeps transparency)", &title_alpha_);
      if (ImGui::Button("Assign a video...", ImVec2(-1, 0))) {
        if (auto f = PickFile(hwnd_)) DoAssign(t->id, *f, title_alpha_);
      }
      ImGui::BeginDisabled(ov.empty());
      if (ImGui::Button("Revert to the original", ImVec2(-1, 0))) DoRevert(t->id);
      ImGui::EndDisabled();
    }
    ImGui::EndChild();
  }
