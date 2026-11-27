
  // ---------------- Session 67: KEYBOARD CONTROLS (Jarrod: "a little button next to the keyboard as controller ... to
  // actually map the buttons ... a little diagram of a 360 controller"). An Xbox 360 pad drawn in the middle, each control
  // joined by a line to its keys on the left or right (the game's own controls screens). A key chip: click = press the
  // new key (Esc cancels; a mouse button clicked on the chip binds that button), right-click = remove it, "+" = add an
  // alternative. Clicking a control on the pad changes its first key. Each change is saved to <save folder>\keybinds.toml
  // at once (keyboard_binds.h), which the game reads when it starts.
  keys::BindFile keys_;
  fs::path keys_file_;
  int key_hot_ = -1, key_hot_force_ = -1;  // the control lit on the pad (a hovered row or control; --keys-hot)
  int cap_bind_ = -1, cap_slot_ = 0, cap_frame_ = 0;
  ImVec2 cap_min_{}, cap_max_{};  // the chip waiting for a key
  std::string keys_status_;
  bool keys_status_warn_ = false, want_keys_popup_ = false;

  // The callout groups: a heading with its controls' rows, joined to the control at `at` (pad units, below).
  struct KeyGroup {
    const char* title;
    int first, count;
    ImVec2 at;
    float r;  // the line stops this far short of `at` (the control's edge: it does not cross the button)
  };
  // (`at` and `r` in the pad picture's units: the SVG's, kPadBox below)
  static constexpr KeyGroup kLeftGroups[] = {{"LT", 4, 1, {665, 170}, 0},       {"LB", 6, 1, {620, 272}, 0},
                                             {"BACK", 22, 1, {875, 477}, 36},    {"LEFT STICK", 8, 5, {650, 520}, 80},
                                             {"D-PAD", 18, 4, {824, 650}, 114}};
  static constexpr KeyGroup kRightGroups[] = {{"RT", 5, 1, {1335, 170}, 0},     {"RB", 7, 1, {1380, 272}, 0},
                                              {"GUIDE", 24, 1, {1002, 476}, 72}, {"Y", 3, 1, {1360, 394}, 44},
                                              {"START", 23, 1, {1131, 477}, 36}, {"B", 1, 1, {1448, 462}, 44},
                                              {"X", 2, 1, {1258, 472}, 44},      {"A", 0, 1, {1353, 541}, 46},
                                              {"RIGHT STICK", 13, 5, {1163, 686}, 80}};

  // "LEFT STICK UP", "A", ... (for the messages).
  static std::string BindTitle(int b) {
    for (const KeyGroup& g : kLeftGroups)
      if (b >= g.first && b < g.first + g.count) return g.count > 1 ? std::string(g.title) + " " + keys::kBinds[b].label : g.title;
    for (const KeyGroup& g : kRightGroups)
      if (b >= g.first && b < g.first + g.count) return g.count > 1 ? std::string(g.title) + " " + keys::kBinds[b].label : g.title;
    return keys::kBinds[b].label;
  }

  void LoadKeys() {
    keys_file_ = paths_.user_data / "keybinds.toml";
    keys_ = keys::LoadBindFile(keys_file_);
  }
  void SaveKeys(const std::string& what) {
    if (keys::SaveBindFile(keys_file_, keys_)) {
      if (!what.empty()) keys_status_ = what, keys_status_warn_ = false;
    } else {
      keys_status_ = "Could not write " + PathStr(keys_file_), keys_status_warn_ = true;
    }
  }

  void BeginCapture(int bind, int slot) {
    if (keys_file_.empty()) LoadKeys();
    cap_bind_ = bind, cap_slot_ = slot, cap_frame_ = ImGui::GetFrameCount();
    g_key_capture = {};
    g_key_capture.active = true;
    keys_status_ = "Press the key for " + BindTitle(bind) + " (Esc: cancel; or click the box with a mouse button).";
    keys_status_warn_ = false;
  }
  void EndCapture() {
    cap_bind_ = -1;
    g_key_capture = {};
  }
  // The captured key (an SDK name with its modifiers) into the waiting slot.
  void ApplyKey(const std::string& token) {
    const int b = cap_bind_;
    EndCapture();
    if (b < 0) return;
    std::vector<std::string> alts = keys::Split(keys::Value(keys_, b));
    const size_t placed = std::min(size_t(std::max(cap_slot_, 0)), alts.size());  // that chip, or a new last one
    if (placed < alts.size()) alts[placed] = token;
    else alts.push_back(token);
    for (size_t i = alts.size(); i-- > 0;)  // the same key once per control (the one just placed stays)
      if (i != placed && alts[i] == token) alts.erase(alts.begin() + std::ptrdiff_t(i));
    keys::SetBind(keys_, b, keys::Join(alts));
    std::string also;
    for (int i = 0; i < keys::kBindCount; ++i)
      if (i != b)
        for (const auto& a : keys::Split(keys::Value(keys_, i)))
          if (a == token) also += (also.empty() ? "" : ", ") + BindTitle(i);
    SaveKeys(BindTitle(b) + " = " + keys::Display(token) + ". Saved.");
    if (!also.empty()) {
      keys_status_ = keys::Display(token) + " is also " + also + ": it presses both.", keys_status_warn_ = true;
    } else if (const std::string warn = keys::ChordWarning(keys_, token, b, [](int i) { return BindTitle(i); });
               !warn.empty()) {
      // A bare Shift / Control / Alt is a button now; chords that use it keep working (the default D-pad is Shift +
      // the arrow keys) and so do the plain binds on their keys.
      keys_status_ = warn, keys_status_warn_ = true;
    } else if (token.size() >= 2 && token[0] == 'F' && std::isdigit(static_cast<unsigned char>(token[1]))) {
      keys_status_ += " (The game's own tools use some F-keys too: F4 opens its settings.)";
    }
  }

  // The pad picture (Jarrod's 360_controller.svg: tools\launcher\make_pad_png.py) covers kPadBox of the SVG's units;
  // positions below are in those units, scaled by `k` from the picture's top-left `o`.
  static constexpr float kPadBox[4] = {415, 100, 1170, 725};  // x, y, w, h (make_pad_png.py VIEW)
  static ImVec2 PadPt(ImVec2 o, float k, float x, float y) {
    return ImVec2(o.x + (x - kPadBox[0]) * k, o.y + (y - kPadBox[1]) * k);
  }

  // The pad part a bind belongs to (its colour in the parts picture, make_pad_png.py PARTS): 0-3 A B X Y, 4 / 5 LT / RT,
  // 6 / 7 LB / RB, 8 left stick, 9 right stick, 10 D-pad, 11 BACK, 12 START, 13 GUIDE.
  static int PartOf(int b) {
    if (b <= 7) return b;
    if (b <= 12) return 8;
    if (b <= 17) return 9;
    if (b <= 21) return 10;
    return b - 22 + 11;
  }
  static constexpr int kPadParts = 14;
  // The sticks' and the D-pad's centres and radii (for the direction arrows and which direction a click is).
  static constexpr float kLeftStick[3] = {650, 520, 76}, kRightStick[3] = {1163, 686, 76}, kDpad[3] = {824, 650, 112};

  // Resources 102 (the pad, 2x) and 103 (the parts picture, 1x), decoded once; textures per drawn width.
  bool pad_tried_ = false;
  std::vector<uint8_t> pad_px_, parts_px_;
  int pad_w_ = 0, pad_h_ = 0, parts_w_ = 0, parts_h_ = 0;
  ID3D11ShaderResourceView* pad_srv_ = nullptr;
  int pad_srv_w_ = 0;
  std::map<std::pair<int, int>, ID3D11ShaderResourceView*> part_srv_;  // (part, drawn width) -> its tint mask

  static std::vector<uint8_t> LoadPngResource(int id, int& w, int& h) {
    const HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    const HGLOBAL g = r ? LoadResource(nullptr, r) : nullptr;
    const auto* data = static_cast<const stbi_uc*>(g ? LockResource(g) : nullptr);
    int n = 0;
    std::vector<uint8_t> out;
    if (data)
      if (stbi_uc* px = stbi_load_from_memory(data, int(SizeofResource(nullptr, r)), &w, &h, &n, 4)) {
        out.assign(px, px + size_t(w) * size_t(h) * 4);
        stbi_image_free(px);
      }
    return out;
  }

  // The part at a pixel of the parts picture (-1 = none): R = 10 + 17 * part, G 200, B 100 (make_pad_png.py).
  int PartAtPixel(int x, int y) const {
    if (x < 0 || y < 0 || x >= parts_w_ || y >= parts_h_) return -1;
    const uint8_t* p = &parts_px_[(size_t(y) * size_t(parts_w_) + size_t(x)) * 4];
    if (p[3] != 255 || p[1] != 200 || p[2] != 100 || p[0] < 10 || (p[0] - 10) % 17) return -1;
    const int part = (p[0] - 10) / 17;
    return part < kPadParts ? part : -1;
  }

  void DrawPad(ImDrawList* dl, ImVec2 o, float k, int hot) {
    if (!pad_tried_) {
      pad_tried_ = true;
      pad_px_ = LoadPngResource(102, pad_w_, pad_h_);
      parts_px_ = LoadPngResource(103, parts_w_, parts_h_);
    }
    const float d = Dpi();
    const int w = std::clamp(int(std::lround(kPadBox[2] * k)), 16, 4096);  // (KeysPopup keeps it to steps of 8 px)
    const int h = std::max(1, int(std::lround(double(w) * kPadBox[3] / kPadBox[2])));
    const ImVec2 hi(o.x + float(w), o.y + float(h));
    if (pad_px_.empty()) {
      dl->AddRect(o, hi, IM_COL32(120, 120, 128, 255));
      dl->AddText(ImVec2(o.x + 12 * d, o.y + 12 * d), IM_COL32(170, 170, 178, 255), "(the controller picture is missing)");
      return;
    }
    if (!pad_srv_ || pad_srv_w_ != w) {
      ReleaseLater(pad_srv_);
      for (auto& [key, srv] : part_srv_) ReleaseLater(srv);  // (the masks of the old size)
      part_srv_.clear();
      pad_srv_ = MakeTexture(theme::Resample(pad_px_.data(), pad_w_, pad_h_, w, h), w, h);
      pad_srv_w_ = w;
    }
    dl->AddImage(ImTextureRef(reinterpret_cast<ImTextureID>(pad_srv_)), o, hi);
    if (hot < 0 || parts_px_.empty()) return;
    // The lit control: its own shape tinted red (a white mask of its pixels, scaled to the drawn size).
    const int part = PartOf(hot);
    ID3D11ShaderResourceView*& mask = part_srv_[{part, w}];
    if (!mask) {
      std::vector<uint8_t> m(size_t(parts_w_) * size_t(parts_h_) * 4, 255);
      for (int y = 0; y < parts_h_; ++y)
        for (int x = 0; x < parts_w_; ++x) m[(size_t(y) * size_t(parts_w_) + size_t(x)) * 4 + 3] = PartAtPixel(x, y) == part ? 255 : 0;
      mask = MakeTexture(theme::Resample(m.data(), parts_w_, parts_h_, w, h), w, h);
    }
    const bool stick = part == 8 || part == 9, dpad = part == 10;
    const int first = part == 8 ? 8 : part == 9 ? 13 : 18;
    const bool direction = (stick || dpad) && hot - first < 4;
    dl->AddImage(ImTextureRef(reinterpret_cast<ImTextureID>(mask)), o, hi, ImVec2(0, 0), ImVec2(1, 1),
                 direction ? IM_COL32(214, 22, 30, 110) : IM_COL32(214, 22, 30, 170));
    if (direction) {  // a red arrow outside the stick or the D-pad, pointing the lit way
      const float* c = part == 8 ? kLeftStick : part == 9 ? kRightStick : kDpad;
      const int dir = hot - first;  // 0 up, 1 down, 2 left, 3 right
      const ImVec2 v = dir == 0 ? ImVec2(0, -1) : dir == 1 ? ImVec2(0, 1) : dir == 2 ? ImVec2(-1, 0) : ImVec2(1, 0);
      const ImVec2 n(-v.y, v.x), at = PadPt(o, k, c[0], c[1]);
      const float r0 = (c[2] + 8) * k, r1 = (c[2] + 62) * k, half = 30 * k;
      const ImVec2 tip(at.x + v.x * r1, at.y + v.y * r1), base(at.x + v.x * r0, at.y + v.y * r0);
      dl->AddTriangleFilled(tip, ImVec2(base.x + n.x * half, base.y + n.y * half),
                            ImVec2(base.x - n.x * half, base.y - n.y * half), kMenuRed);
      dl->AddTriangle(tip, ImVec2(base.x + n.x * half, base.y + n.y * half),
                      ImVec2(base.x - n.x * half, base.y - n.y * half), IM_COL32(0, 0, 0, 200), std::max(1.f, d));
    }
  }

  // The bind under the mouse on the pad, or -1: the control from the parts picture; a stick's or the D-pad's direction
  // by where on it (a stick's middle = PRESS).
  int PadHit(ImVec2 m, ImVec2 o, float k) const {
    if (parts_px_.empty() || k <= 0) return -1;
    const float sx = float(parts_w_) / (kPadBox[2] * k), sy = float(parts_h_) / (kPadBox[3] * k);
    const int part = PartAtPixel(int((m.x - o.x) * sx), int((m.y - o.y) * sy));
    if (part < 0) return -1;
    if (part <= 7) return part;
    if (part >= 11) return 22 + part - 11;
    const float* c = part == 8 ? kLeftStick : part == 9 ? kRightStick : kDpad;
    const int first = part == 8 ? 8 : part == 9 ? 13 : 18;
    const ImVec2 at = PadPt(o, k, c[0], c[1]);
    const float dx = m.x - at.x, dy = m.y - at.y;
    if (part != 10 && dx * dx + dy * dy < (c[2] * 0.45f * k) * (c[2] * 0.45f * k)) return first + 4;
    if (std::fabs(dy) >= std::fabs(dx)) return dy < 0 ? first : first + 1;
    return dx < 0 ? first + 2 : first + 3;
  }

  // One key chip at `pos`: a small key cap with the key's label. Returns 1 when clicked, 2 when right-clicked.
  int KeyChip(const char* id, const std::string& text, ImVec2 pos, float h, bool waiting, bool faint) {
    const float d = Dpi(), pad = 8 * d;
    const std::string shown = waiting ? "PRESS A KEY" : text;
    const ImVec2 ts = ImGui::CalcTextSize(shown.c_str());
    const ImVec2 size(std::max(ts.x + pad * 2, h * 1.15f), h);
    ImGui::SetCursorScreenPos(pos);
    int r = 0;
    bool hover = false;
    if (waiting) {
      ImGui::Dummy(size);  // no button: a click on it is the mouse button to bind (KeysPopup)
      cap_min_ = pos, cap_max_ = ImVec2(pos.x + size.x, pos.y + size.y);
    } else {
      ImGui::PushID(id);
      if (ImGui::InvisibleButton("##chip", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                                                     ImGuiButtonFlags_PressedOnClick))
        r = ImGui::IsMouseClicked(ImGuiMouseButton_Right) ? 2 : 1;
      hover = ImGui::IsItemHovered();
      if (hover && !faint) ImGui::SetItemTooltip("Click to change, right-click to remove");
      ImGui::PopID();
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 hi(pos.x + size.x, pos.y + size.y);
    const float pulse = waiting ? 0.6f + 0.4f * std::sin(float(ImGui::GetTime()) * 6.f) : 1.f;
    const ImU32 fill = waiting ? IM_COL32(int(150 * pulse), 18, 26, 255) : faint ? IM_COL32(40, 41, 47, 150)
                                                                                 : IM_COL32(48, 49, 56, 255);
    dl->AddRectFilled(pos, hi, fill, 3 * d);
    dl->AddRect(pos, hi, waiting || hover ? kMenuRed : faint ? IM_COL32(110, 110, 118, 150) : IM_COL32(150, 150, 158, 255),
                3 * d, 0, std::max(1.f, (hover ? 1.6f : 1.f) * d));
    dl->AddText(ImVec2(pos.x + (size.x - ts.x) / 2, pos.y + (h - ts.y) / 2),
                faint ? IM_COL32(170, 170, 178, 255) : IM_COL32_WHITE, shown.c_str());
    ImGui::SetCursorScreenPos(ImVec2(hi.x + 5 * d, pos.y));
    return r;
  }

  // One column of callouts at `x0` (`width` wide). Returns where each group's line starts (x, y) and bends (z = x)
  // through `anchors`: on the left after the heading row's text or keys, then out to the column's edge.
  void KeyColumn(const KeyGroup* groups, int n, float x0, float y0, float width, float rh, float gap, bool right,
                 std::vector<ImVec4>& anchors, int& hovered) {
    const float d = Dpi(), chips_x = x0 + 78 * d, ch = rh - 5 * d;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float y = y0;
    for (int gi = 0; gi < n; ++gi) {
      const KeyGroup& g = groups[gi];
      const bool group_hot = key_hot_ >= g.first && key_hot_ < g.first + g.count;
      float head_end = x0 + 4 * d + MenuTextSize(17 * d, g.title).x;  // (moved past a one-row control's keys below)
      const int rows = g.count > 1 ? g.count + 1 : 1;
      for (int r = 0; r < rows; ++r) {
        const int b = g.count > 1 ? (r == 0 ? -1 : g.first + r - 1) : g.first;
        const ImVec2 lo(x0, y), hi(x0 + width, y + rh);
        const bool row_hover = b >= 0 && ImGui::IsMouseHoveringRect(lo, hi) && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
        if (row_hover) hovered = b;
        if (b >= 0 && (key_hot_ == b || cap_bind_ == b)) {
          const float sk = 6 * d;
          const ImVec2 q[4] = {{lo.x + sk, lo.y}, {hi.x, lo.y}, {hi.x - sk, hi.y}, {lo.x, hi.y}};
          dl->AddConvexPolyFilled(q, 4, IM_COL32(0, 0, 0, 130));
        }
        if (r == 0) {  // the heading: the control's name (and its keys for a one-row control)
          const float px = 17 * d;
          MenuText(dl, ImVec2(x0 + 4 * d, y + (rh - px) / 2), px, group_hot ? kMenuRed : IM_COL32_WHITE, g.title);
        } else {
          const char* label = keys::kBinds[b].label;
          dl->AddText(ImVec2(x0 + 18 * d, y + (rh - ImGui::GetTextLineHeight()) / 2),
                      key_hot_ == b ? IM_COL32(240, 120, 120, 255) : IM_COL32(160, 161, 168, 255), label);
        }
        if (b >= 0) {
          const std::vector<std::string> alts = keys::Split(keys::Value(keys_, b));
          ImVec2 at(chips_x, y + 2.5f * d);
          for (int a = 0; a < int(alts.size()); ++a) {
            const std::string id = std::to_string(b) + "_" + std::to_string(a);
            const int click = KeyChip(id.c_str(), keys::Display(alts[size_t(a)]), at, ch, cap_bind_ == b && cap_slot_ == a, false);
            at = ImGui::GetCursorScreenPos();
            if (click == 1) BeginCapture(b, a);
            if (click == 2) {
              std::vector<std::string> left = alts;
              left.erase(left.begin() + a);
              keys::SetBind(keys_, b, keys::Join(left));
              EndCapture();
              SaveKeys(BindTitle(b) + ": " + keys::Display(alts[size_t(a)]) + " removed. Saved.");
            }
          }
          if (r == 0 && !alts.empty()) head_end = at.x - 5 * d;  // (not past the "+" a hover shows: the line stays put)
          const bool add_waiting = cap_bind_ == b && cap_slot_ >= int(alts.size());
          if (int(alts.size()) < keys::kMaxAlternatives && (row_hover || alts.empty() || add_waiting)) {
            const std::string id = std::to_string(b) + "_add";
            if (KeyChip(id.c_str(), alts.empty() ? "+ ADD" : "+", at, ch, add_waiting, true) == 1)
              BeginCapture(b, int(alts.size()));
            if (r == 0 && alts.empty()) head_end = ImGui::GetCursorScreenPos().x - 5 * d;
          }
        }
        if (r == 0)
          anchors.push_back(right ? ImVec4(x0 - 6 * d, y + rh / 2, x0 - 18 * d, 0)
                                  : ImVec4(head_end + 6 * d, y + rh / 2, std::max(head_end + 6 * d, x0 + width + 6 * d), 0));
        y += rh;
      }
      y += gap;
    }
  }

  static float KeyColumnHeight(const KeyGroup* groups, int n, float rh, float gap) {
    float h = 0;
    for (int i = 0; i < n; ++i) h += (groups[i].count > 1 ? groups[i].count + 1 : 1) * rh + gap;
    return h - gap;
  }

  void KeysPopup() {
    if (want_keys_popup_) {
      want_keys_popup_ = false;
      LoadKeys();
      keys_status_ = keys_.exists ? "" : "No keys changed yet: these are the defaults.", keys_status_warn_ = false;
      ImGui::OpenPopup("keys");
    }
    const float d = Dpi();
    if (!BeginGameModal("keys", "KEYBOARD CONTROLS", 1060 * d)) {
      if (cap_bind_ >= 0) EndCapture();
      return;
    }
    // A key pressed while a chip waits (WndProc took it).
    if (cap_bind_ >= 0 && g_key_capture.got) {
      const KeyCapture c = g_key_capture;
      if (c.vk == VK_ESCAPE && !c.shift && !c.ctrl && !c.alt) {
        EndCapture();
        keys_status_ = "Cancelled.", keys_status_warn_ = false;
      } else if (const std::string name = keys::NameFromKeyDown(c.vk, c.lp); name.empty()) {
        EndCapture();
        keys_status_ = "That key can't be used by the game: try another.", keys_status_warn_ = true;
      } else {
        ApplyKey(keys::WithModifiers(name, c.shift, c.ctrl, c.alt));
      }
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float width = ImGui::GetContentRegionAvail().x, gap = 8 * d, col_w = 235 * d;
    // Row height: 25 px, less when the window is short (the rest of the window needs ~300 px).
    const float rows_room = vp->WorkSize.y - 330 * d;
    const float rh = std::clamp(rows_room / 15.f, 19 * d, 25 * d);
    const float col_h = std::max(KeyColumnHeight(kLeftGroups, int(std::size(kLeftGroups)), rh, gap),
                                 KeyColumnHeight(kRightGroups, int(std::size(kRightGroups)), rh, gap));
    const float pad_w = width - 2 * col_w - 40 * d;
    // The pad's scale: as large as fits, its drawn width in steps of 8 px (one picture per step while resizing).
    const float fit = std::max(0.05f, std::min(pad_w / kPadBox[2], col_h / kPadBox[3]));
    const float k = float(std::max(16, int(kPadBox[2] * fit) / 8 * 8)) / kPadBox[2];
    const float pw = kPadBox[2] * k, ph = kPadBox[3] * k;
    const ImVec2 top = ImGui::GetCursorScreenPos();
    const float canvas_h = std::max(col_h, ph);
    const ImVec2 pad_o(std::floor(top.x + col_w + 20 * d + (pad_w - pw) / 2), std::floor(top.y + (canvas_h - ph) / 2));
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // The pad first (clicks on a control: change its first key), then the two columns over the lines.
    ImGui::SetCursorScreenPos(ImVec2(pad_o.x, pad_o.y));
    ImGui::InvisibleButton("##pad", ImVec2(pw, ph), ImGuiButtonFlags_PressedOnClick);
    const bool pad_clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    const int pad_hover = ImGui::IsItemHovered() ? PadHit(ImGui::GetIO().MousePos, pad_o, k) : -1;
    if (pad_clicked && pad_hover >= 0) BeginCapture(pad_hover, 0);
    if (pad_hover >= 0) ImGui::SetTooltip("%s: click to change its key", BindTitle(pad_hover).c_str());
    DrawPad(dl, pad_o, k, key_hot_);
    int row_hover = -1;
    std::vector<ImVec4> left_anchor, right_anchor;
    const float ly = top.y + (canvas_h - KeyColumnHeight(kLeftGroups, int(std::size(kLeftGroups)), rh, gap)) / 2;
    const float ry = top.y + (canvas_h - KeyColumnHeight(kRightGroups, int(std::size(kRightGroups)), rh, gap)) / 2;
    KeyColumn(kLeftGroups, int(std::size(kLeftGroups)), top.x, ly, col_w, rh, gap, false, left_anchor, row_hover);
    KeyColumn(kRightGroups, int(std::size(kRightGroups)), top.x + width - col_w, ry, col_w, rh, gap, true, right_anchor,
              row_hover);
    // The lines: from each heading out to its control (red for the lit one).
    auto lines = [&](const KeyGroup* groups, const std::vector<ImVec4>& anchors) {
      for (size_t i = 0; i < anchors.size(); ++i) {
        const KeyGroup& g = groups[i];
        const bool hot = key_hot_ >= g.first && key_hot_ < g.first + g.count;
        const ImU32 col = hot ? kMenuRed : IM_COL32(150, 151, 158, 255);
        const ImVec2 a(anchors[i].x, anchors[i].y), elbow(anchors[i].z, anchors[i].y), c = PadPt(pad_o, k, g.at.x, g.at.y);
        const float dx = elbow.x - c.x, dy = elbow.y - c.y, len = std::max(1.f, std::sqrt(dx * dx + dy * dy));
        const ImVec2 t(c.x + dx / len * g.r * k, c.y + dy / len * g.r * k);
        const float lw = hot ? 2.f * d : 1.2f * d;
        dl->AddLine(a, elbow, col, lw);
        dl->AddLine(elbow, t, IM_COL32(0, 0, 0, 150), lw + 2 * d);  // a dark edge: the line shows on the white pad too
        dl->AddLine(elbow, t, col, lw);
        dl->AddCircleFilled(t, (hot ? 4.f : 3.f) * d, IM_COL32(0, 0, 0, 150));
        dl->AddCircleFilled(t, (hot ? 3.f : 2.f) * d, col);
      }
    };
    lines(kLeftGroups, left_anchor);
    lines(kRightGroups, right_anchor);
    key_hot_ = key_hot_force_ >= 0 ? key_hot_force_ : cap_bind_ >= 0 ? cap_bind_ : row_hover >= 0 ? row_hover : pad_hover;
    ImGui::SetCursorScreenPos(ImVec2(top.x, top.y + canvas_h + 10 * d));

    // A mouse click while a chip waits: on the chip = that mouse button, elsewhere = cancel.
    if (cap_bind_ >= 0 && cap_frame_ != ImGui::GetFrameCount()) {
      static constexpr const char* kMouse[3] = {"LMB", "RMB", "MMB"};
      for (int bt = 0; bt < 3 && cap_bind_ >= 0; ++bt) {
        if (!ImGui::IsMouseClicked(bt)) continue;
        if (ImGui::IsMouseHoveringRect(cap_min_, cap_max_)) {
          const ImGuiIO& io = ImGui::GetIO();
          ApplyKey(keys::WithModifiers(kMouse[bt], io.KeyShift, io.KeyCtrl, io.KeyAlt));
        } else {
          EndCapture();
          keys_status_ = "Cancelled.", keys_status_warn_ = false;
        }
      }
    }

    // The switch itself and the mouse.
    const float half = (width - 16 * d) / 2;
    OptionToggle("KEYBOARD CONTROLS", s.keyboard, true, nullptr, half);
    ImGui::SameLine(0, 16 * d);
    bool mouse = keys::MouseOn(keys_);
    if (OptionToggle("MOUSE AS RIGHT STICK", mouse, true, nullptr, half)) {
      keys::SetMouse(keys_, mouse);
      SaveKeys(mouse ? "The mouse moves the right stick. Saved." : "The mouse is off. Saved.");
    }
    // Session 76 (a player: "add Keyboard as a Separate controller"; Jarrod: on by default).
    OptionToggle("SEPARATE PLAYER", s.keyboard_own_player, s.keyboard, nullptr, half);
    ImGui::SameLine(0, 16 * d);
    {
      const ImVec2 at = ImGui::GetCursorScreenPos();
      ImGui::SetCursorScreenPos(ImVec2(at.x + 6 * d, at.y + 2 * d));
      ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + half - 10 * d);
      ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
      ImGui::TextWrapped("%s", s.keyboard_own_player
                                   ? "The keyboard is the next player after the controllers: player 2 with one "
                                     "controller, player 1 with none."
                                   : "The keyboard and controller 1 are the same player.");
      ImGui::PopStyleColor();
      ImGui::PopTextWrapPos();
    }
    static constexpr double kSpeeds[] = {0.25, 0.5, 0.75, 1.0, 1.5, 2.0, 3.0, 4.0};
    static constexpr const char* kSpeedNames[] = {"0.25x", "0.5x", "0.75x", "1x", "1.5x", "2x", "3x", "4x"};
    int si = 0;
    const double sens = keys::Sensitivity(keys_);
    for (int i = 0; i < int(std::size(kSpeeds)); ++i)
      if (std::fabs(kSpeeds[i] - sens) < std::fabs(kSpeeds[si] - sens)) si = i;
    if (OptionRow("MOUSE SPEED", si, kSpeedNames, int(std::size(kSpeedNames)), mouse, nullptr, half)) {
      keys::SetSensitivity(keys_, kSpeeds[si]);
      SaveKeys(std::string("Mouse speed ") + kSpeedNames[si] + ". Saved.");
    }
    ImGui::SameLine(0, 16 * d);
    {  // beside MOUSE SPEED: what the window says now
      const ImVec2 at = ImGui::GetCursorScreenPos();
      ImGui::SetCursorScreenPos(ImVec2(at.x + 6 * d, at.y + 2 * d));
      ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + half - 10 * d);
      std::string note = keys_status_;
      if (note.empty()) note = "Click a key to change it, or a button on the pad. Saved to " + PathStr(keys_file_) + ".";
      if (game_proc_) note += " The game reads the keys when it starts: restart it to use changes.";
      if (!s.keyboard) note += " KEYBOARD CONTROLS is OFF: the game uses only controllers.";
      ImGui::PushStyleColor(ImGuiCol_Text, keys_status_warn_ ? kWarn : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
      ImGui::TextWrapped("%s", note.c_str());
      ImGui::PopStyleColor();
      ImGui::PopTextWrapPos();
    }
    ImGui::Dummy(ImVec2(0, 6 * d));
    if (ModalMainButton("DONE")) EndCapture(), ImGui::CloseCurrentPopup();
    if (ModalButton("Reset to defaults")) {
      EndCapture();
      for (int i = 0; i < keys::kBindCount; ++i) keys::SetBind(keys_, i, keys::kBinds[i].def);
      keys::SetMouse(keys_, false);
      keys::SetSensitivity(keys_, 1.0);
      SaveKeys("Back to the default keys. Saved.");
    }
    ImGui::NewLine();
    ImGui::EndPopup();
  }
