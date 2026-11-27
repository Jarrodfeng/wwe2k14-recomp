
  // ---------------- Play
  std::string asset_viewer_note_;  // Session 96: why the ASSET VIEWER button could not start it
  int play_view_ = 0;  // Session 31: 0 the home screen (logo, PLAY GAME, QUICK MATCH), 1 Quick Match, 2 Advanced

  void PlayPage() {
    if (play_view_ == 1) {
      if (TitleWithBack("QUICK MATCH")) play_view_ = 0;
      ImGui::Spacing();
      QuickMatch();
      return;
    }
    if (play_view_ == 3) {
      if (TitleWithBack("30 YEARS OF WRESTLEMANIA")) play_view_ = 0;
      Wm30Page();
      return;
    }
    if (play_view_ == 2) {
      if (TitleWithBack("ADVANCED")) play_view_ = 0;
      ImGui::TextDisabled("Replays, re-shoots, seeds, director scripts and the exact command line.");
      ImGui::Spacing();
      AdvancedPlay();
      return;
    }
    PlayHome();
  }

  // The home screen, as in Jarrod's sketch: the logo, "NATIVE PC RECOMPILATION / version", two big slanted buttons.
  void PlayHome() {
    const float d = Dpi();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos(), avail = ImGui::GetContentRegionAvail();
    const float want_w = std::min(avail.x * 0.8f, 640 * d);
    const float logo_y = o.y + std::max(6 * d, avail.y * 0.05f);
    ImVec2 logo = DrawLogo(dl, ImVec2(o.x + (avail.x - want_w) / 2, logo_y), want_w, 0);
    float y = logo_y + logo.y;
    const float sub_px = 22 * d;
    if (logo.x > 0) {
      // Under "2K14", tucked into the logo's empty lower right (the signature runs under the W on the left).
      const float cx = o.x + (avail.x - logo.x) / 2 + logo.x * 0.655f;
      y = logo_y + logo.y * 0.80f;
      const char* sub = "NATIVE PC RECOMPILATION";
      ImVec2 ts = MenuTextSize(sub_px, sub);
      MenuText(dl, ImVec2(cx - ts.x / 2, y), sub_px, IM_COL32_WHITE, sub);
      y += ts.y;
      const std::string ver = ShortVersion();
      ts = MenuTextSize(sub_px * 0.9f, ver.c_str());
      MenuText(dl, ImVec2(cx - ts.x / 2, y), sub_px * 0.9f, IM_COL32_WHITE, ver.c_str());
      y += ts.y;
      const char* by = "BY JARRODFENG";
      ts = MenuTextSize(sub_px * 0.6f, by);
      MenuText(dl, ImVec2(cx - ts.x / 2, y + 2 * d), sub_px * 0.6f, IM_COL32(255, 255, 255, 140), by);
      y += ts.y + 2 * d;
    } else {
      MenuText(dl, ImVec2(o.x, y), 48 * d, IM_COL32_WHITE, "WWE 2K14");
      y += 60 * d;
    }

    y = std::max(y + 34 * d, logo_y + logo.y + 16 * d);
    if (!gd_.ok) {
      ImGui::SetCursorScreenPos(ImVec2(o.x + avail.x * 0.18f, y));
      ImGui::TextColored(kBad, "Your game files aren't set up yet.");
      ImGui::SameLine();
      if (ImGui::SmallButton("Go to Settings")) GoPage(2);
      y += ImGui::GetFrameHeightWithSpacing();
    }
    const float gap = 44 * d, bw = std::min(330 * d, (avail.x - gap - 20 * d) / 2), bh = 116 * d;
    const float bx = o.x + (avail.x - 2 * bw - gap) / 2;
    if (SlantButton("PLAY GAME", ImVec2(bx, y), ImVec2(bw, bh), gd_.ok && !game_proc_, 36 * d)) LaunchPlain();
    if (SlantButton("QUICK MATCH", ImVec2(bx + bw + gap, y), ImVec2(bw, bh), true, 36 * d)) play_view_ = 1;
    const float buttons_bottom = y + bh;  // Session 34: the 30 YEARS button continues these two's outer slanted edges
    y += bh + 8 * d;
    auto caption = [&](float x, const char* text) {
      const ImVec2 ts = ImGui::CalcTextSize(text);
      ImGui::SetCursorScreenPos(ImVec2(x + (bw - ts.x) / 2, y));
      ImGui::TextDisabled("%s", text);
    };
    caption(bx + bw + gap, "pick the match, wrestlers and arena");
    // Session 34: every 30 Years of WrestleMania match, one click away (Play -> 30 YEARS). Its left and right edges
    // lie on the lines of PLAY GAME's left and QUICK MATCH's right edge (SlantButton slants 0.22 x its height, the
    // same slope at any height): shifted left by the slant over the gap below them, narrower by the slant they have
    // and it hasn't (Jarrod: "it otherwise looks uncentered").
    y += ImGui::GetTextLineHeightWithSpacing() + 12 * d;
    const float wh = 64 * d, wx = bx - 0.22f * (y + wh - buttons_bottom), ww = 2 * bw + gap - 0.22f * (bh - wh);
    if (SlantButton("30 YEARS OF WRESTLEMANIA", ImVec2(wx, y), ImVec2(ww, wh), true, 30 * d)) play_view_ = 3;
    y += wh + 8 * d;
    {
      const char* text = "any of the mode's 46 matches, straight from the main menu";
      const ImVec2 ts = ImGui::CalcTextSize(text);
      ImGui::SetCursorScreenPos(ImVec2(wx + (ww - 0.22f * wh - ts.x) / 2, y));
      ImGui::TextDisabled("%s", text);
    }
    if (PerfTestAvailable()) {  // Session 39: a test build's performance test (PerfTestPopup)
      y += ImGui::GetTextLineHeightWithSpacing() + 10 * d;
      ImGui::SetCursorScreenPos(ImVec2(wx + (ww - 0.22f * wh) / 2 - 110 * d, y));
      if (ImGui::Button("PERFORMANCE TEST (test build)", ImVec2(220 * d, 0))) want_perf_popup_ = true;
      y += ImGui::GetFrameHeight();
    }
    if (BenchAvailable()) {  // S102 U4: the benchmark button (three short games, results into the report zip)
      y += ImGui::GetTextLineHeightWithSpacing() + 6 * d;
      ImGui::SetCursorScreenPos(ImVec2(wx + (ww - 0.22f * wh) / 2 - 110 * d, y));
      if (ImGui::Button(bench_step_ >= 0 ? "STOP THE BENCHMARK" : "BENCHMARK (Lesnar vs Bryan)", ImVec2(220 * d, 0))) {
        if (bench_step_ >= 0) StopBench();
        else StartBench();
      }
      y += ImGui::GetFrameHeight();
    }

    // Bottom: what Play uses, the keys, and the Advanced tools.
    const float line = ImGui::GetTextLineHeightWithSpacing();
    float by = o.y + avail.y - line * 2 - ImGui::GetFrameHeight() - 6 * d;
    by = std::max(by, y + line * 2);
    ImGui::SetCursorScreenPos(ImVec2(o.x, by));
    RefreshDlc(30);  // Session 32: "DLC: N packs on"
    const int dlc_on = DlcPacksOn();
    const std::string dlc_text = dlc_on ? ", DLC: " + std::to_string(dlc_on) + (dlc_on == 1 ? " pack on" : " packs on") : "";
    ImGui::TextDisabled("%s%s%s%s.  F8 CineCam (when on in Settings), Alt+F4 quits.",
                        ResolutionName(s).c_str(),
                        s.frame_rate == 2   ? (s.refresh_hz == 120 ? ", 120 fps" : ", uncapped")
                        : s.frame_rate == 0 ? ", classic frame rate"
                        : s.frame_rate == 3 ? ", 30 fps"  // Session 41 (it said 60)
                                            : ", 60 fps",
                        (!kStreamFreeTarget && s.upscale) ? ", upscaling on" : "", dlc_text.c_str());
    if (ImGui::Button("Advanced: replays, re-shoots, seeds, director...")) play_view_ = 2;
  }

  static constexpr const char* kResolutionNames[] = {"720p", "1440p", "4K"};
  // Session 54: the size the game draws at (360p ... 4K): a draw height only with NATIVE and no upscaler.
  static bool DrawHeightApplies(const Settings& t) { return t.renderer == 2 && (kStreamFreeTarget || t.upscale == 0) && !t.original_look; }  // S105 A1b: a saved upscaler is not sent in the stream-free build
  static std::string ResolutionName(const Settings& t) {
    if (t.original_look) return kResolutionNames[0];
    if (DrawHeightApplies(t) && t.draw_height) return std::to_string(t.draw_height) + "p";
    return kResolutionNames[std::clamp(t.resolution, 0, 2)];
  }

  // The match types a Quick Match can build, laid out like the game's PLAY menu (its order, from the Session 30 menu
  // crawl; db\ids\match_types.json). type = the game's match type id (docs\systems\match_setup.md); group = the
  // game's submenu, if any; sides = one team digit per wrestler slot, as the game's apply sets them (slot +13), so its
  // length is the wrestler count. Checked: Session 27 (0, 3, 37, 39, 49), Session 30 (13, 14, 33, 55, 60, 81), Session
  // 31 the other 48 (runs 2357-2404: each loaded and ran 10 s of match clock; the sides from the log; the pictures
  // show the cage, the cell, the chamber wall, ladders, backstage where expected). Not here: the Royal Rumble (its
  // entry order comes from the menus: My saved setups), Special Referee, Title Match, Tournaments.
  struct QuickType {
    int type;
    const char* cat;
    const char* group;
    const char* name;
    const char* sides;
  };
  static constexpr const char* kQuickCats[] = {"ONE ON ONE", "TWO ON TWO", "TRIPLE THREAT", "FATAL 4-WAY",
                                               "6-MAN",      "HANDICAP",   "SPECIALTY"};
  static constexpr QuickType kQuickTypes[] = {
      {0, "ONE ON ONE", "", "NORMAL", "01"},
      {77, "ONE ON ONE", "", "EXTREME RULES", "01"},
      {49, "ONE ON ONE", "", "HELL IN A CELL", "01"},
      {33, "ONE ON ONE", "", "I QUIT", "01"},
      {81, "ONE ON ONE", "", "INFERNO", "01"},
      {54, "ONE ON ONE", "", "IRON MAN", "01"},
      {55, "ONE ON ONE", "", "LADDER", "01"},
      {59, "ONE ON ONE", "", "LAST MAN STANDING", "01"},
      {60, "ONE ON ONE", "", "STEEL CAGE", "01"},
      {67, "ONE ON ONE", "", "SUBMISSION", "01"},
      {68, "ONE ON ONE", "", "TABLE", "01"},
      {72, "ONE ON ONE", "", "TLC", "01"},
      {3, "TWO ON TWO", "", "TAG TEAM", "0101"},
      {9, "TWO ON TWO", "ELIMINATION TAG", "TAG TEAM", "0101"},
      {10, "TWO ON TWO", "ELIMINATION TAG", "TORNADO TAG", "0101"},
      {12, "TWO ON TWO", "ELIMINATION TAG", "TORNADO TAG TABLE", "0101"},
      {7, "TWO ON TWO", "", "MIXED GENDER TAG", "0101"},
      {4, "TWO ON TWO", "TORNADO TAG", "TAG TEAM", "0101"},
      {78, "TWO ON TWO", "TORNADO TAG", "EXTREME RULES", "0101"},
      {50, "TWO ON TWO", "TORNADO TAG", "HELL IN A CELL", "0101"},
      {56, "TWO ON TWO", "TORNADO TAG", "LADDER", "0101"},
      {61, "TWO ON TWO", "TORNADO TAG", "STEEL CAGE", "0101"},
      {69, "TWO ON TWO", "TORNADO TAG", "TABLE", "0101"},
      {73, "TWO ON TWO", "TORNADO TAG", "TLC", "0101"},
      {13, "TRIPLE THREAT", "", "NORMAL", "012"},
      {79, "TRIPLE THREAT", "", "EXTREME RULES", "012"},
      {51, "TRIPLE THREAT", "", "HELL IN A CELL", "012"},
      {57, "TRIPLE THREAT", "", "LADDER", "012"},
      {62, "TRIPLE THREAT", "", "STEEL CAGE", "012"},
      {70, "TRIPLE THREAT", "", "TABLE", "012"},
      {74, "TRIPLE THREAT", "", "TLC", "012"},
      {14, "FATAL 4-WAY", "", "NORMAL", "0123"},
      {34, "FATAL 4-WAY", "", "BATTLE ROYAL", "0123"},
      {80, "FATAL 4-WAY", "", "EXTREME RULES", "0123"},
      {52, "FATAL 4-WAY", "", "HELL IN A CELL", "0123"},
      {58, "FATAL 4-WAY", "", "LADDER", "0123"},
      {63, "FATAL 4-WAY", "", "STEEL CAGE", "0123"},
      {71, "FATAL 4-WAY", "", "TABLE", "0123"},
      {75, "FATAL 4-WAY", "", "TLC", "0123"},
      {35, "6-MAN", "", "BATTLE ROYAL", "012345"},
      {39, "6-MAN", "ELIMINATION CHAMBER", "NORMAL", "012345"},
      {40, "6-MAN", "ELIMINATION CHAMBER", "TORNADO TAG", "010101"},
      {11, "6-MAN", "", "ELIMINATION TAG", "010101"},
      {53, "6-MAN", "", "HELL IN A CELL", "012345"},
      {76, "6-MAN", "", "LADDER", "012345"},
      {8, "6-MAN", "", "TAG TEAM", "010101"},
      {37, "6-MAN", "", "6-PACK CHALLENGE", "012345"},  // not in PLAY (Session 27's six-man type)
      {16, "HANDICAP", "ONE ON TWO", "TAG TEAM", "011"},
      {15, "HANDICAP", "ONE ON TWO", "TORNADO TAG", "011"},
      {18, "HANDICAP", "ONE ON THREE", "TAG TEAM", "0111"},
      {17, "HANDICAP", "ONE ON THREE", "TORNADO TAG", "0111"},
      {19, "HANDICAP", "", "TWO ON THREE", "01011"},
      {82, "HANDICAP", "", "GAUNTLET", "0111"},
      {26, "SPECIALTY", "BACKSTAGE BRAWL", "ONE ON ONE", "01"},
      {29, "SPECIALTY", "BACKSTAGE BRAWL", "TWO ON TWO", "0101"},
      {27, "SPECIALTY", "BACKSTAGE BRAWL", "ONE ON TWO", "011"},
      {28, "SPECIALTY", "BACKSTAGE BRAWL", "ONE ON THREE", "0111"},
      {30, "SPECIALTY", "BACKSTAGE BRAWL", "TRIPLE THREAT", "012"},
      {31, "SPECIALTY", "BACKSTAGE BRAWL", "FATAL 4-WAY", "0123"},
  };
  static int QuickIndex(int type) {
    if (type == 1) type = 13;  // Session 27's triple threat / fatal 4-way ids (not in PLAY): the menu's own ones
    if (type == 2) type = 14;
    for (int i = 0; i < int(std::size(kQuickTypes)); ++i)
      if (kQuickTypes[i].type == type) return i;
    return 0;
  }
  // "TEAM 1" / "TEAM 2" for team matches, "WRESTLER N" when everyone is on their own.
  static std::string SlotLabel(const QuickType& t, size_t slot) {
    const std::string sides = t.sides;
    const bool solo = std::all_of(sides.begin(), sides.end(),
                                  [&](char c) { return std::count(sides.begin(), sides.end(), c) == 1; });
    return solo ? "Wrestler " + std::to_string(slot + 1) : "Team " + std::string(1, char(sides[slot] + 1));
  }

  static std::vector<int> ParseIds(const std::string& text) {
    std::vector<int> out;
    std::stringstream ss(text);
    for (std::string t; std::getline(ss, t, ',');)
      if (!t.empty()) out.push_back(std::atoi(t.c_str()));
    return out;
  }

  std::vector<const Character*> Wrestlers() const {
    std::vector<const Character*> w;
    for (const auto& c : chars_)
      if (c.kind == "wrestler") w.push_back(&c);
    for (const auto& c : dlc_chars_)  // Session 32: the DLC wrestlers whose pack is on
      if (DlcWrestlerOn(c)) w.push_back(&c);
    return w;
  }

  // One wrestler picker: a combo that filters as you type.
  bool WrestlerCombo(const char* id, int& value) {
    bool changed = false;
    const std::string label = value >= 0 ? CharName(value) : std::string("(pick)");
    if (ImGui::BeginCombo(id, label.c_str(), ImGuiComboFlags_HeightLarge)) {
      if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(), pick_search_.clear();
      ImGui::InputTextWithHint("##search", "type to search", &pick_search_);
      const std::string q = Lower(pick_search_);
      for (const Character* c : Wrestlers()) {
        if (!q.empty() && Lower(c->name).find(q) == std::string::npos) continue;
        if (ImGui::Selectable(c->name.c_str(), c->id == value)) value = c->id, changed = true;
      }
      ImGui::EndCombo();
    }
    return changed;
  }

  // A list entry in the style of the game's second menu column: grey, white with the red arrow when selected.
  bool ListItem(const char* label, bool selected, float px, float width) {
    const float d = Dpi(), h = px * 1.2f, arrow = px * 0.5f;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(label, ImVec2(width, h));
    const bool hover = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (selected) {
      const float cy = pos.y + h / 2;
      dl->AddTriangleFilled(ImVec2(pos.x, cy - arrow / 2), ImVec2(pos.x + arrow * 0.9f, cy),
                            ImVec2(pos.x, cy + arrow / 2), kMenuRed);
    }
    const ImU32 col = selected ? IM_COL32_WHITE : hover ? IM_COL32(205, 206, 211, 255) : IM_COL32(141, 142, 147, 255);
    MenuText(dl, ImVec2(pos.x + arrow + 8 * d, pos.y + (h - px) / 2), px, col, label);
    return pressed;
  }

  // A game-style option row, like the in-game GRAPHICS menu (Session 28): LABEL on the left, "< VALUE >" on the
  // right. The arrows step through the values; a click on the label or value steps forward. A faint slanted bar
  // marks the hovered row. `note` (optional) is a grey line under the row. Returns true when the value changed.
  bool OptionRow(const char* label, int& value, const char* const* names, int count, bool enabled = true,
                 const char* note = nullptr, float width = 0) {
    const float d = Dpi(), px = 20 * d, h = 38 * d, w = width > 0 ? width : ImGui::GetContentRegionAvail().x;
    const float val_w = std::min(300 * d, w * 0.5f), arrow = 9 * d;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::PushID(label);
    ImGui::BeginDisabled(!enabled);
    const bool click = ImGui::InvisibleButton("##row", ImVec2(w, h));
    ImGui::EndDisabled();
    const bool hover = enabled && ImGui::IsItemHovered();
    ImGui::PopID();
    value = std::clamp(value, 0, count - 1);
    const int before = value;
    if (click) {
      const float mx = ImGui::GetIO().MousePos.x, vx = pos.x + w - val_w;
      value = mx >= vx && mx < vx + arrow * 3 ? (value + count - 1) % count : (value + 1) % count;
    }
    if (hover) {
      const float sk = 10 * d;
      const ImVec2 q[4] = {{pos.x + sk, pos.y}, {pos.x + w, pos.y}, {pos.x + w - sk, pos.y + h}, {pos.x, pos.y + h}};
      dl->AddConvexPolyFilled(q, 4, IM_COL32(0, 0, 0, 120));
    }
    const ImU32 text = enabled ? IM_COL32_WHITE : IM_COL32(110, 111, 116, 255);
    MenuText(dl, ImVec2(pos.x + 14 * d, pos.y + (h - px) / 2), px, text, label);
    // The value, centred between the two arrows.
    const float vx = pos.x + w - val_w, cy = pos.y + h / 2;
    const ImU32 arrow_col = enabled ? kMenuRed : IM_COL32(90, 30, 32, 255);
    dl->AddTriangleFilled(ImVec2(vx + arrow, cy - arrow), ImVec2(vx + arrow, cy + arrow), ImVec2(vx, cy), arrow_col);
    dl->AddTriangleFilled(ImVec2(vx + val_w - arrow - 6 * d, cy - arrow), ImVec2(vx + val_w - 6 * d, cy),
                          ImVec2(vx + val_w - arrow - 6 * d, cy + arrow), arrow_col);
    const char* shown = names[value];
    float vpx = px;
    ImVec2 ts = MenuTextSize(vpx, shown);
    const float room = val_w - 4 * arrow - 12 * d;
    if (ts.x > room) vpx *= room / ts.x, ts = MenuTextSize(vpx, shown);
    MenuText(dl, ImVec2(vx + (val_w - 6 * d - ts.x) / 2, cy - ts.y / 2), vpx, text, shown);
    if (note && *note) {
      ImGui::SetCursorScreenPos(ImVec2(pos.x + 14 * d, ImGui::GetCursorScreenPos().y - 4 * d));
      ImGui::PushTextWrapPos(pos.x + w - ImGui::GetWindowPos().x);
      ImGui::TextDisabled("%s", note);
      ImGui::PopTextWrapPos();
      ImGui::Dummy(ImVec2(0, 2 * d));
    }
    return value != before;
  }
  bool OptionToggle(const char* label, bool& on, bool enabled = true, const char* note = nullptr, float width = 0) {
    static constexpr const char* kOffOn[] = {"OFF", "ON"};
    int v = on ? 1 : 0;
    const bool changed = OptionRow(label, v, kOffOn, 2, enabled, note, width);
    on = v == 1;
    return changed;
  }
  // A red section heading inside a page (TEAM 1, ARENA, ...).
  void SubHeading(const char* text) {
    const float d = Dpi();
    MenuTextRaw(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), 15 * d, kMenuRed, text);
    ImGui::Dummy(ImVec2(0, 19 * d));
  }

  // Session 31: the game's PLAY menu as two columns (category, then its matches with the game's submenus as
  // headings), the setup beside them (below them in a narrow window).
  void BuildMatch() {
    const float d = Dpi(), px = 21 * d, cat_w = 205 * d, type_w = 270 * d;
    int ti = QuickIndex(s.quick_type);
    s.quick_type = kQuickTypes[ti].type;  // review fix: a stored type not in the list launches as the one shown
    ImGui::BeginGroup();
    for (const char* cat : kQuickCats) {
      const bool on = std::strcmp(cat, kQuickTypes[ti].cat) == 0;
      if (ListItem(cat, on, px, cat_w) && !on)  // like the game: the cursor lands on the category's first match
        for (int i = 0; i < int(std::size(kQuickTypes)); ++i)
          if (std::strcmp(kQuickTypes[i].cat, cat) == 0) {
            ti = i, s.quick_type = kQuickTypes[i].type;
            break;
          }
    }
    ImGui::EndGroup();
    ImGui::SameLine(0, 6 * d);
    ImGui::BeginGroup();
    const char* group = "";
    for (int i = 0; i < int(std::size(kQuickTypes)); ++i) {
      const QuickType& t = kQuickTypes[i];
      if (std::strcmp(t.cat, kQuickTypes[ti].cat) != 0) continue;
      if (*t.group && std::strcmp(group, t.group) != 0) {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        MenuTextRaw(ImGui::GetWindowDrawList(), ImVec2(at.x, at.y + 4 * d), 15 * d, kMenuRed, t.group);
        ImGui::Dummy(ImVec2(type_w, 21 * d));
      }
      group = t.group;
      const float indent = *t.group ? 14 * d : 0;
      ImGui::PushID(t.type);
      if (indent > 0) ImGui::Indent(indent);
      if (ListItem(t.name, i == ti, px * 0.86f, type_w - indent)) ti = i, s.quick_type = t.type;
      if (indent > 0) ImGui::Unindent(indent);
      ImGui::PopID();
    }
    if (std::strcmp(kQuickTypes[ti].cat, "SPECIALTY") == 0) {
      ImGui::Dummy(ImVec2(0, 6 * d));
      ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + type_w);
      ImGui::TextDisabled("Royal Rumble: set it up once in the game's menus with My saved setups (its entry order "
                          "comes from the menus).");
      ImGui::PopTextWrapPos();
    }
    ImGui::EndGroup();
    const float left = ImGui::GetContentRegionAvail().x - cat_w - type_w - 6 * d;
    if (left >= 400 * d) ImGui::SameLine(0, 24 * d);
    else ImGui::Dummy(ImVec2(0, 10 * d));
    ImGui::BeginGroup();
    MatchSetup(kQuickTypes[ti], std::min(left >= 400 * d ? left - 24 * d : ImGui::GetContentRegionAvail().x, 520 * d));
    ImGui::EndGroup();
  }

  // The chosen match: its wrestlers (grouped by team), arena, options, START MATCH.
  void MatchSetup(const QuickType& qt, float width) {
    const float d = Dpi(), w = width - 96 * d;  // room for a full-height Random button
    {
      const std::string path = std::string(qt.cat) + (*qt.group ? std::string("  >  ") + qt.group : "") + "  >  " + qt.name;
      float px = 19 * d;
      if (const float tw = MenuTextSize(px, path.c_str()).x; tw > width) px *= width / tw;  // long ones: smaller
      MenuText(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), px, IM_COL32_WHITE, path.c_str());
      ImGui::Dummy(ImVec2(width, 26 * d));
    }
    const std::string sides = qt.sides;
    std::vector<int> ids = ParseIds(s.quick_ids);
    ids.resize(sides.size(), -1);
    const auto pool = Wrestlers();
    auto random_id = [&](const std::vector<int>& taken) {
      for (int tries = 0; tries < 200 && !pool.empty(); ++tries) {
        const int id = pool[size_t(rng_() % pool.size())]->id;
        if (std::find(taken.begin(), taken.end(), id) == taken.end()) return id;
      }
      return pool.empty() ? -1 : pool.front()->id;
    };
    // The slots in team order (the game decides the sides by slot: qt.sides), each team under its own heading.
    const bool solo = SlotLabel(qt, 0).rfind("Wrestler", 0) == 0;
    for (char team = '0'; team <= '9'; ++team) {
      if (sides.find(team) == std::string::npos) continue;
      if (!solo) {
        const std::string head = "TEAM " + std::string(1, char(team + 1));
        MenuTextRaw(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), 15 * d, kMenuRed, head.c_str());
        ImGui::Dummy(ImVec2(0, 17 * d));
      }
      for (size_t i = 0; i < ids.size(); ++i) {
        if (sides[i] != team) continue;
        ImGui::PushID(int(i));
        ImGui::SetNextItemWidth(w);
        WrestlerCombo(("##w" + std::to_string(i)).c_str(), ids[i]);
        ImGui::SameLine();
        if (ImGui::Button("Random")) {  // full height, like the pickers (Jarrod)
          std::vector<int> others = ids;
          others.erase(others.begin() + std::ptrdiff_t(i));
          ids[i] = random_id(others);
        }
        ImGui::PopID();
      }
    }
    ImGui::Dummy(ImVec2(0, 4 * d));
    std::string arena_label = "The match type's own arena";
    for (const auto& a : arenas_)
      if (a.id == s.quick_arena) arena_label = a.name;
    MenuTextRaw(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), 15 * d, kMenuRed, "ARENA");
    ImGui::Dummy(ImVec2(0, 17 * d));
    ImGui::SetNextItemWidth(w);
    if (ImGui::BeginCombo("##arena", arena_label.c_str(), ImGuiComboFlags_HeightLarge)) {
      if (ImGui::Selectable("The match type's own arena", s.quick_arena < 0)) s.quick_arena = -1;
      for (const auto& a : arenas_)
        if (ImGui::Selectable((a.name + "##" + std::to_string(a.id)).c_str(), a.id == s.quick_arena)) s.quick_arena = a.id;
      ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Random##arena") && !arenas_.empty()) s.quick_arena = arenas_[size_t(rng_() % arenas_.size())].id;
    if (ImGui::Button("Random everything")) {
      std::vector<int> taken;
      for (auto& id : ids) id = random_id(taken), taken.push_back(id);
      if (!arenas_.empty()) s.quick_arena = arenas_[size_t(rng_() % arenas_.size())].id;
    }
    ImGui::Checkbox("Skip entrances", &s.skip_entrances);
    ImGui::SameLine();
    ImGui::Checkbox("CPU vs CPU (watch)", &s.quick_cpu_only);
    if (!s.quick_cpu_only)
      ImGui::TextDisabled("You play %s with controller 1.", solo ? "wrestler 1" : "Team 1's first wrestler");
    std::string text;
    for (int id : ids) text += (text.empty() ? "" : ",") + std::to_string(id);
    s.quick_ids = text;
    const bool complete = std::find(ids.begin(), ids.end(), -1) == ids.end();
    ImGui::Dummy(ImVec2(0, 6 * d));
    const ImVec2 at = ImGui::GetCursorScreenPos();
    if (SlantButton("START MATCH", at, ImVec2(250 * d, 64 * d), complete && gd_.ok && !game_proc_, 28 * d))
      LaunchQuickMatch();
    if (!complete) ImGui::TextDisabled("Pick every wrestler (or press Random).");
  }

  void QuickMatch() {
    const float dpi = Dpi(), label_w = 170 * dpi;
    RefreshDlc(30);  // Session 32: the DLC wrestlers for the pickers
    ImGui::RadioButton("Build a match", &s.quick_mode, 0);
    ImGui::SameLine();
    ImGui::RadioButton("My saved setups", &s.quick_mode, 1);
    if (s.quick_mode == 0) {
      BuildMatch();
      return;
    }
    std::vector<std::string> names;
    for (const auto& sc : scenarios_) names.push_back(sc.name);
    if (names.empty()) {
      Note("No saved matches yet. \"Save a match setup\" starts the game: set a match up in the menus as usual "
           "(wrestlers, arena, match type). When you confirm it, it is saved here, and next time it starts in one "
           "click, straight from the main menu.");
    } else {
      ImGui::SetNextItemWidth(label_w * 2);
      Combo("Match", s.scenario, names, "(pick a saved match)");
      for (const auto& sc : scenarios_) {
        if (sc.name != s.scenario) continue;
        std::string who;
        for (int id : sc.ids) who += (who.empty() ? "" : ", ") + CharName(id);
        ImGui::TextDisabled("%s%s%s", who.c_str(), sc.description.empty() ? "" : "  -  ", sc.description.c_str());
      }
      ImGui::BeginDisabled(s.scenario.empty());
      ImGui::Checkbox("Other arena", &s.arena_override);
      ImGui::SameLine();
      ImGui::BeginDisabled(!s.arena_override);
      ImGui::SetNextItemWidth(label_w * 2);
      std::string arena_label = "#" + std::to_string(s.arena);
      for (const auto& a : arenas_)
        if (a.id == s.arena) arena_label = a.name + " (" + std::to_string(a.id) + ")";
      if (ImGui::BeginCombo("##qarena", arena_label.c_str())) {
        for (const auto& a : arenas_)
          if (ImGui::Selectable((a.name + " (" + std::to_string(a.id) + ")").c_str(), a.id == s.arena)) s.arena = a.id;
        ImGui::EndCombo();
      }
      ImGui::EndDisabled();
      ImGui::SetNextItemWidth(label_w * 2);
      ImGui::InputTextWithHint("Wrestlers", "empty = the saved match's (ids, comma separated)", &s.participants);
      ImGui::SameLine();
      if (ImGui::SmallButton("Add...")) ImGui::OpenPopup("pick_char");
      CharPicker();
      ImGui::Checkbox("Skip entrances", &s.skip_entrances);
      ImGui::EndDisabled();
      ImGui::BeginDisabled(s.scenario.empty() || !gd_.ok || game_proc_ != nullptr);
      if (BigButton("Start match", ImVec2(200 * dpi, 40 * dpi), ImVec4(0.5f, 0.12f, 0.12f, 1))) LaunchQuickMatch();
      ImGui::EndDisabled();
      ImGui::SameLine();
    }
    ImGui::BeginDisabled(!gd_.ok || game_proc_ != nullptr);
    if (ImGui::Button("Save a match setup...", ImVec2(0, 40 * dpi))) ImGui::OpenPopup("capture");
    ImGui::EndDisabled();
    if (ImGui::BeginPopup("capture")) {
      ImGui::TextUnformatted("Name for the match (letters, numbers, _):");
      ImGui::SetNextItemWidth(260 * dpi);
      ImGui::InputText("##capname", &capture_name_);
      std::string clean;
      for (char c : capture_name_)
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') clean += c;
      ImGui::TextDisabled("The game starts. Set the match up in the menus and confirm it: it is saved as \"%s\".",
                          clean.empty() ? "my_match" : clean.c_str());
      if (ImGui::Button("Start the game")) {
        LaunchCapture(clean.empty() ? "my_match" : clean);
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndPopup();
    }
  }

  void CharPicker() {
    if (ImGui::BeginPopup("pick_char")) {
      ImGui::SetNextItemWidth(260);
      ImGui::InputTextWithHint("##pq", "search", &pick_search_);
      ImGui::BeginChild("pk", ImVec2(300, 300));
      const std::string q = Lower(pick_search_);
      for (const auto& c : chars_) {
        if (!q.empty() && Lower(c.name).find(q) == std::string::npos) continue;
        if (ImGui::Selectable((c.name + "  (" + std::to_string(c.id) + ")").c_str())) {
          s.participants += (s.participants.empty() ? "" : ",") + std::to_string(c.id);
          ImGui::CloseCurrentPopup();
        }
      }
      ImGui::EndChild();
      ImGui::EndPopup();
    }
  }

  // The game with the player's settings: no scenario, the Play preset, not through the test harness.
  Settings PlainSettings() const {
    Settings p = s;
    p.scenario.clear(), p.preset = kPlay, p.harness = false;
    return p;
  }
  void LaunchPlain() { StartGame(BuildCommand(PlainSettings(), paths_), "Started the game"); }

  // ---------------- Play -> 30 Years of WrestleMania (Session 34; src\launcher\wm30_list.h, src\wm30_jump.h)
  wwe2k14::wm30list::Catalogue wm30_;
  int wm30_era_ = 0, wm30_sel_ = -1;

  void Wm30Page() {
    const float d = Dpi();
    if (!wm30_.loaded && gd_.ok) wm30_ = wwe2k14::wm30list::Load(GameFilesAt(paths_.game_data));
    if (!gd_.ok) {
      ImGui::TextColored(kBad, "Your game files aren't set up yet.");
      if (ImGui::SmallButton("Go to Settings")) GoPage(2);
      return;
    }
    if (!wm30_.ok) {
      ImGui::TextColored(kBad, "Couldn't read the 30 Years of WrestleMania matches from your game files.");
      ImGui::TextDisabled("%s", wm30_.error.c_str());
      if (ImGui::SmallButton("Try again")) wm30_ = {};
      return;
    }
    ImGui::TextDisabled("The game starts at its main menu and goes to the match by itself (its own videos, story card, "
                        "objectives). Locked matches work too.");
    ImGui::Spacing();
    std::string upper[5];
    const char* names[5];
    for (int e = 0; e < 5; ++e) {
      upper[e] = wm30_.era_names[e];
      for (char& ch : upper[e]) ch = char(std::toupper(static_cast<unsigned char>(ch)));
      // Short tab labels so all five fit: HULKAMANIA, NEW GENERATION, ATTITUDE, RUTHLESS AGGRESSION, UNIVERSE.
      if (upper[e].rfind("THE ", 0) == 0) upper[e].erase(0, 4);
      for (const char* tail : {" RUNS WILD", " ERA"})
        if (upper[e].size() > std::strlen(tail) && upper[e].ends_with(tail)) upper[e].resize(upper[e].size() - std::strlen(tail));
      names[e] = upper[e].c_str();
    }
    if (GameTabs(names, 5, wm30_era_, era_strip_)) wm30_sel_ = -1;
    ImGui::Spacing();
    const float list_w = std::min(ImGui::GetContentRegionAvail().x * 0.48f, 520 * d);
    const float h = ImGui::GetContentRegionAvail().y - 90 * d;
    ImGui::BeginChild("wm30_list", ImVec2(list_w, std::max(h, 120 * d)), ImGuiChildFlags_Borders);
    for (const auto& m : wm30_.matches) {
      if (m.era != wm30_era_) continue;
      if (wm30_sel_ < 0) wm30_sel_ = m.index;
      const std::string label = m.title + "##" + std::to_string(m.index);
      if (ImGui::Selectable(label.c_str(), wm30_sel_ == m.index)) wm30_sel_ = m.index;
      ImGui::TextDisabled("   %s, %s", m.show.c_str(), m.date.c_str());
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("wm30_detail", ImVec2(0, std::max(h, 120 * d)), ImGuiChildFlags_Borders);
    const wwe2k14::wm30list::Match* sel = nullptr;
    for (const auto& m : wm30_.matches)
      if (m.index == wm30_sel_) sel = &m;
    if (sel) {
      SubHeading(sel->title.c_str());
      ImGui::Text("%s, %s, %s", sel->show.c_str(), sel->venue.c_str(), sel->date.c_str());
      if (!sel->championship.empty()) ImGui::Text("Title match: %s", sel->championship.c_str());
      if (!sel->type_name.empty()) ImGui::Text("Match type: %s", sel->type_name.c_str());
      std::string arena;
      for (const auto& a : arenas_)
        if (a.id == sel->arena) arena = a.name;
      if (!arena.empty()) ImGui::Text("Arena: %s", arena.c_str());
      std::string people;
      for (const auto& p : sel->people) people += (people.empty() ? "" : ", ") + p;
      ImGui::TextWrapped("In the match: %s", people.c_str());
      ImGui::Spacing();
      for (const auto& t : sel->stipulations) ImGui::BulletText("%s", t.c_str());
      if (!sel->historical.empty()) ImGui::TextDisabled("Historical objectives:");
      for (const auto& t : sel->historical) ImGui::BulletText("%s", t.c_str());
    }
    ImGui::EndChild();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    if (SlantButton("PLAY MATCH", ImVec2(at.x, at.y + 8 * d), ImVec2(250 * d, 64 * d), sel && !game_proc_, 28 * d))
      LaunchWm30(*sel);
  }

  void LaunchWm30(const wwe2k14::wm30list::Match& m) {
    Settings q = PlainSettings();
    q.scenario.clear();
    q.wm30 = m.index;
    StartGame(BuildCommand(q, paths_), "Started 30 Years of WrestleMania: " + m.title);
  }
  void LaunchQuickMatch() {
    Settings q = s;
    q.preset = kPlay, q.harness = false;
    if (s.quick_mode == 0) {  // Session 27 item 8: built from our numbers
      // Session 32: a DLC wrestler needs the pack on (the game has no such character otherwise), and an id the
      // launcher doesn't know at all (a DLC wrestler from a folder that has gone) is not sent either.
      for (int id : ParseIds(s.quick_ids)) {
        if (id < 0) continue;
        const bool on_disc = std::any_of(chars_.begin(), chars_.end(), [&](const Character& c) { return c.id == id; });
        const auto dlc = std::find_if(dlc_chars_.begin(), dlc_chars_.end(), [&](const Character& c) { return c.id == id; });
        if (dlc != dlc_chars_.end() && !DlcWrestlerOn(*dlc)) {
          Status(dlc->name + " needs DLC pack " + std::to_string(dlc->dlc_pack) +
                 ": switch it on in Settings -> Game -> Downloadable content, or pick another wrestler");
          return;
        }
        if (!on_disc && dlc == dlc_chars_.end()) {
          Status("Wrestler #" + std::to_string(id) + " isn't available (a DLC wrestler whose pack is missing?): pick "
                 "another wrestler");
          return;
        }
      }
      q.scenario = "@quick";
      StartGame(BuildCommand(q, paths_), "Started a Quick Match");
      return;
    }
    StartGame(BuildCommand(q, paths_), "Started: " + s.scenario);
  }
  void LaunchCapture(const std::string& name) {
    Settings c = PlainSettings();
    c.extra += (c.extra.empty() ? "" : " ") + std::string("--wwe2k14_scenario_capture=") + name;
    std::error_code ec;
    fs::create_directories(paths_.scenarios, ec);
    StartGame(BuildCommand(c, paths_), "Set up your match in the menus; it is saved as " + name);
  }
