
 private:
  // ================================================================================================================
  // Session 25: the launcher for players. A sidebar (Play Game, Mods, Settings; Report a problem and the version at
  // the bottom), the first-run wizard, crash detection, and the Logos page (src\launcher\logo_save.h).

  static constexpr ImVec4 kGood{0.45f, 0.85f, 0.45f, 1}, kBad{1, 0.55f, 0.4f, 1}, kWarn{1, 0.8f, 0.35f, 1};

  float Dpi() const { return ImGui::GetStyle().FontScaleDpi; }

  // A heading in the menu face (an ImGui item, so the layout flows around it).
  void Heading(const char* text, float scale = 1.35f) {
    const float px = ImGui::GetStyle().FontSizeBase * scale * Dpi();
    const ImVec2 pos = ImGui::GetCursorScreenPos(), sz = MenuTextSize(px, text);
    MenuText(ImGui::GetWindowDrawList(), pos, px, IM_COL32_WHITE, text);
    ImGui::Dummy(sz);
  }

  void Note(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
  }

  // A big coloured button (Play, Import).
  bool BigButton(const char* label, ImVec2 size, ImVec4 col = ImVec4(0.55f, 0.11f, 0.13f, 1)) {  // S31: red, was green
    ImGui::PushStyleColor(ImGuiCol_Button, col);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(col.x * 1.25f, col.y * 1.25f, col.z * 1.25f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(col.x * 1.4f, col.y * 1.4f, col.z * 1.4f, 1));
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.4f);
    const bool r = ImGui::Button(label, size);
    ImGui::PopFont();
    ImGui::PopStyleColor(3);
    return r;
  }

  // ---------------- Session 31: the game's menu look (launcher_theme.h)
  static constexpr ImU32 kMenuRed = IM_COL32(214, 22, 30, 255);
  // The page list, top to bottom, and each item's s.page (0 Play, 1 Mods, 2 Settings, 3 About: the stored numbers
  // stay as before Session 31).
  static constexpr int kNavPages[4] = {0, 2, 1, 3};
  static constexpr const char* kNavNames[4] = {"PLAY", "SETTINGS", "MODS", "ABOUT"};
  static constexpr float kNavPad = 18, kNavRow = 52;

  void GoPage(int page) {
    s.page = page;
    if (page == 0) play_view_ = 0;  // PLAY always opens its home screen
  }

  // The red band (Session 31, Jarrod: full height, "definitely most finished looking", over the floating panel): from
  // the window's top to its bottom on the left edge, the right side leaning like the game's panels.
  theme::Panel NavGeometry(ImVec2 origin, ImVec2 size) const {
    const float d = Dpi();
    theme::Panel p;
    p.x0 = origin.x, p.y0 = origin.y, p.w = 310 * d, p.h = std::max(size.y, 1.f), p.straight_left = true;
    p.slant = std::min(p.h * 0.16f, p.w - 210 * d);  // the foot stays wide enough for the footer
    return p;
  }

  // One picture for the whole window (dots + the red panel), remade only when the window size changes.
  void DrawBackground(ImVec2 pos, ImVec2 size, const theme::Panel& p) {
    const int w = int(size.x), h = int(size.y);
    if (w <= 0 || h <= 0) return;
    if (!bg_srv_ || w != bg_w_ || h != bg_h_) {
      theme::Panel local = p;
      local.x0 -= pos.x, local.y0 -= pos.y;
      ReleaseLater(bg_srv_);
      bg_srv_ = MakeTexture(theme::Background(w, h, Dpi(), local), w, h);
      bg_w_ = w, bg_h_ = h;
    }
    if (bg_srv_)
      ImGui::GetWindowDrawList()->AddImage(ImTextureRef(reinterpret_cast<ImTextureID>(bg_srv_)), pos,
                                           ImVec2(pos.x + float(w), pos.y + float(h)));
  }

  // The band: the logo at the top, the pages under it (stepping down the slant, the selected one a black bar with red
  // text: the game's PLAY row; a hovered one a faint bar), the footer at the foot (game running, Report a problem,
  // the version).
  void NavPanel(const theme::Panel& p) {
    const float d = Dpi(), margin = 20 * d;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 logo = DrawLogo(dl, ImVec2(p.x0 + margin, p.y0 + 18 * d), p.w - 3 * margin, 2);
    const float top = p.y0 + 18 * d + (logo.y > 0 ? logo.y + 24 * d : 0);
    for (int i = 0; i < 4; ++i) {
      const float y0 = top + kNavRow * d * float(i), y1 = y0 + kNavRow * d, yc = (y0 + y1) / 2;
      ImGui::SetCursorScreenPos(ImVec2(p.Left(y0), y0));
      ImGui::PushID(i);
      if (ImGui::InvisibleButton("##nav", ImVec2(p.Right(y0) - p.Left(y0), y1 - y0))) GoPage(kNavPages[i]);
      ImGui::PopID();
      const bool on = s.page == kNavPages[i], hover = ImGui::IsItemHovered();
      if (on || hover) {
        const ImVec2 bar[4] = {{p.Left(y0), y0}, {p.Right(y0), y0}, {p.Right(y1), y1}, {p.Left(y1), y1}};
        dl->AddConvexPolyFilled(bar, 4, on ? IM_COL32(0, 0, 0, 255) : IM_COL32(0, 0, 0, 80));
      }
      // Each row starts a little further left, following the right edge's slant (the game's staircase).
      const float px = 34 * d, tx = p.x0 + 2.4f * margin - (p.Right(top) - p.Right(yc));
      const ImVec2 at(tx, yc - MenuTextSize(px, kNavNames[i]).y * 0.5f);
      if (on) MenuTextRaw(dl, at, px, kMenuRed, kNavNames[i]);
      else MenuText(dl, at, px, IM_COL32_WHITE, kNavNames[i]);
    }
    dl->AddLine(ImVec2(p.x0 + p.w, p.y0), ImVec2(p.x0 + p.w - p.slant, p.y0 + p.h), IM_COL32(255, 255, 255, 170),
                1.6f * d);  // the game's light rim on the right edge

    // Footer, bottom up, inside the band's foot.
    const float fw = p.Right(p.y0 + p.h) - p.x0 - 2 * margin, line = ImGui::GetTextLineHeight(), gap = 6 * d;
    float y = p.y0 + p.h - 16 * d - line;
    ImGui::SetCursorScreenPos(ImVec2(p.x0 + margin, y));
    ImGui::TextColored(ImVec4(1, 1, 1, 0.7f), "%s", Version());
    y -= ImGui::GetFrameHeight() + gap;
    ImGui::SetCursorScreenPos(ImVec2(p.x0 + margin, y));
    if (ImGui::Button("Report a problem", ImVec2(fw, 0)) || want_report_popup_)
      ImGui::OpenPopup("report"), want_report_popup_ = false;
    ReportPopup();
    {  // Session 31: the update notice, when there is one
      // Session 81 (Jarrod): the button's bottom edge lines up with the Report a problem box (the slant's top corner
      // pokes out by sk), and it sits a touch higher.
      const float uh = 46 * d, sk = uh * 0.22f, lift = 4 * d;
      if (UpdateNotice(ImVec2(p.x0 + margin, y - gap - lift - uh), fw + sk) > 0) y -= uh + gap + lift;
    }
    if (game_proc_) {
      const float px = 17 * d;
      MenuText(dl, ImVec2(p.x0 + margin, y - px - gap), px, IM_COL32_WHITE, "GAME RUNNING");
    }
  }

  // A slanted button (the PLAY GAME / QUICK MATCH tiles): red, white rim, the label in the menu font, shrunk to fit.
  bool SlantButton(const char* label, ImVec2 pos, ImVec2 size, bool enabled, float font_px) {
    const float d = Dpi(), sk = size.y * 0.22f;
    ImGui::SetCursorScreenPos(pos);
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::InvisibleButton(label, size);
    ImGui::EndDisabled();
    const bool hover = enabled && ImGui::IsItemHovered(), held = enabled && ImGui::IsItemActive();
    ImU32 top = IM_COL32(160, 20, 28, 255), bottom = IM_COL32(82, 8, 12, 255);
    if (!enabled) top = IM_COL32(66, 66, 72, 255), bottom = IM_COL32(40, 40, 45, 255);
    else if (held) top = IM_COL32(112, 12, 18, 255), bottom = IM_COL32(64, 6, 10, 255);
    else if (hover) top = IM_COL32(205, 30, 38, 255), bottom = IM_COL32(118, 12, 18, 255);
    const ImVec2 q[4] = {{pos.x + sk, pos.y}, {pos.x + size.x, pos.y}, {pos.x + size.x - sk, pos.y + size.y},
                         {pos.x, pos.y + size.y}};
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 uv = dl->_Data->TexUvWhitePixel;
    const auto i0 = ImDrawIdx(dl->_VtxCurrentIdx);
    dl->PrimReserve(6, 4);  // a top-to-bottom gradient quad
    dl->PrimWriteIdx(i0), dl->PrimWriteIdx(ImDrawIdx(i0 + 1)), dl->PrimWriteIdx(ImDrawIdx(i0 + 2));
    dl->PrimWriteIdx(i0), dl->PrimWriteIdx(ImDrawIdx(i0 + 2)), dl->PrimWriteIdx(ImDrawIdx(i0 + 3));
    dl->PrimWriteVtx(q[0], uv, top), dl->PrimWriteVtx(q[1], uv, top);
    dl->PrimWriteVtx(q[2], uv, bottom), dl->PrimWriteVtx(q[3], uv, bottom);
    dl->AddPolyline(q, 4, enabled ? IM_COL32_WHITE : IM_COL32(150, 150, 156, 255), ImDrawFlags_Closed, 3.5f * d);
    const std::string text(label, ImGui::FindRenderedTextEnd(label));
    ImVec2 ts = MenuTextSize(font_px, text.c_str());
    const float room = size.x - 2 * sk - 16 * d;
    if (ts.x > room) font_px *= room / ts.x, ts = MenuTextSize(font_px, text.c_str());
    MenuText(dl, ImVec2(pos.x + (size.x - ts.x) / 2, pos.y + (size.y - ts.y) / 2), font_px,
             enabled ? IM_COL32_WHITE : IM_COL32(170, 170, 176, 255), text.c_str());
    return pressed;
  }

  // The logo (resource 101), scaled once per drawn width (slot 0 the Play screen, 1 About, 2 the band). Returns the drawn size;
  // (0, 0) when the exe has no logo.
  ImVec2 DrawLogo(ImDrawList* dl, ImVec2 pos, float width, int slot) {
    if (!logo_tried_) {
      logo_tried_ = true;
      const HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(101), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
      const HGLOBAL g = r ? LoadResource(nullptr, r) : nullptr;
      const auto* data = static_cast<const stbi_uc*>(g ? LockResource(g) : nullptr);
      int n = 0;
      if (data)
        if (stbi_uc* px = stbi_load_from_memory(data, int(SizeofResource(nullptr, r)), &logo_w_, &logo_h_, &n, 4)) {
          logo_px_.assign(px, px + size_t(logo_w_) * size_t(logo_h_) * 4);
          stbi_image_free(px);
        }
    }
    if (logo_px_.empty()) return ImVec2(0, 0);
    const int w = std::clamp(int(width) / 8 * 8, 16, logo_w_);  // steps of 8 px while the window is resized
    const int h = std::max(1, int(std::lround(double(logo_h_) * w / logo_w_)));
    auto& srv = logo_srv_[slot];
    if (!srv || logo_srv_w_[slot] != w) {
      ReleaseLater(srv);
      srv = MakeTexture(w == logo_w_ ? logo_px_ : theme::Resample(logo_px_.data(), logo_w_, logo_h_, w, h), w, h);
      logo_srv_w_[slot] = w;
    }
    dl->AddImage(ImTextureRef(reinterpret_cast<ImTextureID>(srv)), pos, ImVec2(pos.x + float(w), pos.y + float(h)));
    return ImVec2(float(w), float(h));
  }

  // Menu text: the game's own face when the game files have it (LoadMenuFont), else Arial Black. px = the line
  // height (the game face's 40-pixel cell, scaled).
  static uint32_t NextCodepoint(const char*& p) {
    const auto c = static_cast<unsigned char>(*p++);
    if (c < 0x80) return c;
    const int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    uint32_t cp = c & (0x3Fu >> extra);
    for (int i = 0; i < extra && (static_cast<unsigned char>(*p) & 0xC0) == 0x80; ++i) cp = cp << 6 | (*p++ & 0x3F);
    return cp;
  }
  ImVec2 MenuTextSize(float px, const char* text) {
    if (gfont_srv_) {
      const float k = px / gfont_.cell;
      float w = 0;
      for (const char* p = text; *p;) {
        const auto it = gfont_.glyphs.find(NextCodepoint(p));
        w += (it != gfont_.glyphs.end() ? it->second.w : gfont_.cell * 0.33f) * k;
      }
      return ImVec2(w, px);
    }
    ImGui::PushFont(g_font_menu, px / Dpi());  // ImGui scales font sizes by the DPI itself
    const ImVec2 sz = ImGui::CalcTextSize(text);
    ImGui::PopFont();
    return sz;
  }
  void MenuTextRaw(ImDrawList* dl, ImVec2 pos, float px, ImU32 col, const char* text) {
    if (!gfont_srv_) {
      ImGui::PushFont(g_font_menu, px / Dpi());
      dl->AddText(pos, col, text);
      ImGui::PopFont();
      return;
    }
    const float k = px / gfont_.cell, iw = 1.f / float(gfont_.sheet_w), ih = 1.f / float(gfont_.sheet_h);
    const ImTextureRef tex(reinterpret_cast<ImTextureID>(gfont_srv_));
    float x = std::floor(pos.x);
    const float y = std::floor(pos.y);
    for (const char* p = text; *p;) {
      const auto it = gfont_.glyphs.find(NextCodepoint(p));
      if (it == gfont_.glyphs.end()) {
        x += gfont_.cell * 0.33f * k;
        continue;
      }
      const GameFont::Glyph& g = it->second;
      dl->AddImage(tex, ImVec2(x, y), ImVec2(x + g.w * k, y + g.h * k), ImVec2(g.x * iw, g.y * ih),
                   ImVec2((g.x + g.w) * iw, (g.y + g.h) * ih), col);
      x += g.w * k;
    }
  }
  // With the game's drop shadow.
  void MenuText(ImDrawList* dl, ImVec2 pos, float px, ImU32 col, const char* text) {
    const float o = std::max(1.f, px * 0.06f);
    MenuTextRaw(dl, ImVec2(pos.x + o, pos.y + o), px, IM_COL32(0, 0, 0, 190), text);
    MenuTextRaw(dl, pos, px, col, text);
  }

  // "version 0.1.4" for the big screens (the footer keeps the full string).
  static std::string ShortVersion() {
    std::string v = Version();
    if (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v.erase(0, 1);
    return "version " + v.substr(0, v.find_first_of("-+"));
  }

  // A page title in the menu font, with "< BACK" before it on the Play sub-screens.
  bool TitleWithBack(const char* title) {
    const float d = Dpi();
    bool back = false;
    ImGui::PushFont(g_font_menu, 15);
    back = ImGui::Button("< BACK");
    ImGui::PopFont();
    ImGui::SameLine(0, 14 * d);
    Heading(title, 1.5f);
    return back;
  }
