
  // ---------------- Settings
  void CheckGameDataNow() {
    gd_ = CheckGameData(paths_.game_data);
    gd_path_ = paths_.game_data;
  }

  void GameDataSection(bool wizard) {
    const float dpi = Dpi();
    ImGui::TextWrapped("Your game files: the folder you copied from your own disc (it has default.xex in it), or a "
                       "disc image (.iso) of your disc.");
    std::string shown = s.game_data_dir.empty() ? PathStr(paths_.game_data) : s.game_data_dir;
    ImGui::SetNextItemWidth(-230 * dpi);
    if (ImGui::InputTextWithHint("##gd", "e.g. D:\\Games\\WWE2K14 or D:\\Games\\WWE2K14.iso", &shown,
                                 ImGuiInputTextFlags_EnterReturnsTrue))
      s.game_data_dir = shown, Reload();
    ImGui::SameLine();
    if (ImGui::Button("Folder...", ImVec2(110 * dpi, 0)))
      if (auto d = PickFolder(hwnd_, L"Pick the folder with default.xex")) s.game_data_dir = PathStr(*d), Reload();
    ImGui::SameLine();
    if (ImGui::Button("Disc image...", ImVec2(-1, 0)))
      if (auto f = PickDiscImage(hwnd_)) s.game_data_dir = PathStr(*f), Reload();
    ImGui::TextColored(gd_.ok ? kGood : kBad, "%s", gd_.message.c_str());
    if (!wizard && gd_.ok) {
      RefreshLogos(false);
      if (logo_.save_exists) ImGui::TextColored(kGood, "Save found (%d of 20 logo slots used).", kLogoSlots - logo_.Free());
      else ImGui::TextColored(kWarn, "No save yet: the game makes one the first time you play.");
    }
  }

  // Session 31: Settings like the game's OPTIONS: the sections as a list on the left, the section's rows beside it.
  static constexpr const char* kSettingsSections[] = {"GAME", "DISPLAY", "GRAPHICS", "ADVANCED"};
  int settings_section_ = 0;

  void SettingsPage() {
    const float d = Dpi(), list_w = 190 * d;
    Heading("SETTINGS", 1.6f);
    ImGui::Spacing();
    ImGui::BeginGroup();
    for (int i = 0; i < int(std::size(kSettingsSections)); ++i)
      if (ListItem(kSettingsSections[i], i == settings_section_, 21 * d, list_w)) settings_section_ = i;
    ImGui::EndGroup();
    ImGui::SameLine(0, 16 * d);
    ImGui::BeginChild("section", ImVec2(std::min(ImGui::GetContentRegionAvail().x, 720 * d), -4 * d));
    switch (settings_section_) {
      case 0: SettingsGame(); break;
      case 1: SettingsDisplay(); break;
      case 2: SettingsGraphics(); break;
      default: AdvancedSettings(); break;
    }
    ImGui::EndChild();
  }

  // Session 31: the game's own languages (runtime user_language ids, launcher_core.h AutoLanguage).
  static constexpr int kLanguageIds[] = {0, 1, 3, 4, 5, 6};
  static constexpr const char* kLanguageNames[] = {"AUTOMATIC", "ENGLISH",  "DEUTSCH", "FRAN\xC3\x87" "AIS",
                                                   "ESPA\xC3\x91" "OL", "ITALIANO"};

  // ---------------- DLC (Session 32; Jarrod: in Settings -> GAME, beside the game files)
  // The packs in the DLC folder (dlc_catalog.h Scan), refreshed at most every `max_age_s` seconds or when the folder
  // changes. Scanning reads each pack's catalog and checks its listed files: a few hundred file checks.
  std::vector<wwe2k14::dlc::Pack> dlc_packs_;
  fs::path dlc_scanned_;
  std::chrono::steady_clock::time_point dlc_checked_{};
  bool dlc_folder_exists_ = false;

  void RefreshDlc(int max_age_s) {
    const auto now = std::chrono::steady_clock::now();
    if (dlc_scanned_ == paths_.dlc && dlc_checked_.time_since_epoch().count() &&
        now - dlc_checked_ < std::chrono::seconds(max_age_s))
      return;
    dlc_checked_ = now, dlc_scanned_ = paths_.dlc;
    std::error_code ec;
    dlc_folder_exists_ = fs::is_directory(paths_.dlc, ec);
    try {
      dlc_packs_ = dlc_folder_exists_ ? wwe2k14::dlc::Scan(paths_.dlc) : std::vector<wwe2k14::dlc::Pack>{};
    } catch (...) {  // review: never let an odd folder take the launcher down
      dlc_packs_.clear();
    }
    RebuildDlcWrestlers();
  }

  // Session 32 (Jarrod: DLC wrestlers in Quick Match): every ready pack's wrestlers (its catalog's character files),
  // named from the packs' own string files (dlc_names.h; read once per pack and kept). Whether a pack is on is checked
  // where the list is used (Wrestlers()), so a switch takes effect at once.
  std::vector<Character> dlc_chars_;
  std::map<std::string, std::map<int, std::string>> dlc_names_;  // pack root | string file -> names

  void RebuildDlcWrestlers() {
    dlc_chars_.clear();
    std::map<int, std::string> names;  // the string files are cumulative: any pack's list will do
    for (const auto& p : dlc_packs_) {
      if (p.status != wwe2k14::dlc::Status::kReady || p.string_pack.empty()) continue;
      const std::string key = wwe2k14::dlc::PathUtf8(p.root) + "|" + p.string_pack;
      auto it = dlc_names_.find(key);
      if (it == dlc_names_.end()) {
        std::map<int, std::string> got;
        try {
          got = wwe2k14::dlc::NamesFromPack(p);
        } catch (...) {
        }
        it = dlc_names_.emplace(key, std::move(got)).first;
      }
      for (const auto& [id, n] : it->second) names.emplace(id, n);
    }
    for (const auto& p : dlc_packs_) {
      if (p.status != wwe2k14::dlc::Status::kReady) continue;
      for (int id : p.char_ids) {
        const bool known = std::any_of(chars_.begin(), chars_.end(), [&](const Character& c) { return c.id == id; }) ||
                           std::any_of(dlc_chars_.begin(), dlc_chars_.end(), [&](const Character& c) { return c.id == id; });
        if (known) continue;
        Character c;
        c.id = id, c.kind = "wrestler", c.dlc_pack = p.pack_id;
        const auto n = names.find(id);
        c.name = n != names.end() && !n->second.empty() ? wwe2k14::dlc::ReadableName(n->second)
                                                         : "DLC wrestler #" + std::to_string(id);
        c.note = "DLC pack " + std::to_string(p.pack_id);
        dlc_chars_.push_back(std::move(c));
      }
    }
    std::sort(dlc_chars_.begin(), dlc_chars_.end(),
              [](const Character& a, const Character& b) { return Lower(a.name) < Lower(b.name); });
  }
  // A DLC wrestler the game has right now (USE MY DLC on, the pack not switched off).
  bool DlcWrestlerOn(const Character& c) const { return s.dlc && c.dlc_pack >= 0 && !DlcIsOff(c.dlc_pack); }
  bool DlcIsOff(int id) const {
    const auto off = wwe2k14::dlc::ParseIdList(s.dlc_off);
    return std::find(off.begin(), off.end(), id) != off.end();
  }
  void SetDlcOff(int id, bool off) {
    auto ids = wwe2k14::dlc::ParseIdList(s.dlc_off);
    ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
    if (off) ids.push_back(id);
    std::sort(ids.begin(), ids.end());
    s.dlc_off.clear();
    for (int v : ids) s.dlc_off += (s.dlc_off.empty() ? "" : ",") + std::to_string(v);
  }
  // Ready packs the game gets (USE MY DLC on, not switched off).
  int DlcPacksOn() const {
    if (!s.dlc) return 0;
    int n = 0;
    for (const auto& p : dlc_packs_) n += p.status == wwe2k14::dlc::Status::kReady && !DlcIsOff(p.pack_id);
    return n;
  }
  static std::string DlcStatusText(const wwe2k14::dlc::Pack& p) {
    using wwe2k14::dlc::Status;
    switch (p.status) {
      case Status::kReady: return "Ready";
      case Status::kIncomplete: return "Incomplete: " + p.reason;
      case Status::kDamaged: return "Damaged: " + p.reason;
      case Status::kDuplicate: return "Duplicate: " + p.reason;
      case Status::kOtherContent: return "Not WWE 2K14 DLC: " + p.reason;
    }
    return "?";
  }
  static std::string SizeText(uint64_t bytes) {
    char b[32];
    if (bytes >= (1u << 20)) std::snprintf(b, sizeof(b), "%.1f MB", double(bytes) / (1024.0 * 1024.0));
    else if (bytes >= 1024) std::snprintf(b, sizeof(b), "%.0f KB", double(bytes) / 1024.0);
    else std::snprintf(b, sizeof(b), "%u bytes", unsigned(bytes));
    return b;
  }
  // The text for Report a problem: pack names and states only (never files).
  std::string DlcReportText() {
    RefreshDlc(0);
    std::string t = std::string("DLC: ") + (s.dlc ? "on" : "off") + ", folder " + PathStr(paths_.dlc) +
                    (dlc_folder_exists_ ? "" : " (not there)") + ", " + std::to_string(dlc_packs_.size()) + " pack(s)\n";
    for (const auto& p : dlc_packs_)
      t += "  pack " + std::to_string(p.pack_id) + " '" + p.folder + "' (" + p.AllNames() + "): " + DlcStatusText(p) +
           (p.status == wwe2k14::dlc::Status::kReady ? (DlcIsOff(p.pack_id) ? ", switched off" : ", on") : "") + "\n";
    return t;
  }

  void DlcSection() {
    const float d = Dpi();
    RefreshDlc(10);  // and at once on Look again / a new folder
    OptionToggle("USE MY DLC", s.dlc, true,
                 "Your own downloadable content, read straight from your files (nothing is copied or changed): the "
                 "packages as copied from your console's drive, or packs you extracted (one folder each, with "
                 "info\\catalog.dlc in it).");
    std::string shown = s.dlc_dir.empty() ? PathStr(paths_.dlc) : s.dlc_dir;
    ImGui::SetNextItemWidth(-230 * d);
    if (ImGui::InputTextWithHint("##dlc", "e.g. D:\\Games\\WWE2K14\\dlc", &shown, ImGuiInputTextFlags_EnterReturnsTrue))
      s.dlc_dir = shown, Reload(), RefreshDlc(0);
    ImGui::SameLine();
    if (ImGui::Button("Folder...##dlc", ImVec2(110 * d, 0)))
      if (auto f = PickFolder(hwnd_, L"Pick the folder with your DLC packs")) s.dlc_dir = PathStr(*f), Reload(), RefreshDlc(0);
    ImGui::SameLine();
    if (ImGui::Button("Look again", ImVec2(-1, 0))) RefreshDlc(0);
    if (!s.dlc_dir.empty()) {
      if (ImGui::SmallButton("Use the usual folder")) s.dlc_dir.clear(), Reload(), RefreshDlc(0);
      ImGui::SameLine();
      ImGui::TextDisabled("(dlc next to your game files)");
    }
    if (!dlc_folder_exists_) {
      ImGui::TextColored(kWarn, "No DLC folder here. To add DLC, make it and copy your packs in, or pick another folder.");
      return;
    }
    int ready = 0;
    for (const auto& p : dlc_packs_) ready += p.status == wwe2k14::dlc::Status::kReady;
    if (dlc_packs_.empty()) {
      ImGui::TextColored(kWarn, "No packs in this folder (a pack is a package from the console's drive, or a folder "
                                "with info\\catalog.dlc in it).");
      return;
    }
    ImGui::TextColored(ready ? kGood : kWarn, "%d pack(s) found, %d on. Changes apply the next time the game starts.",
                       int(dlc_packs_.size()), DlcPacksOn());
    // Session 32 (runs 2443-2477, copies of a save): a save made with packs on keeps needing them; with one off the
    // game says the save can't be used, but the unlocks stayed, even after saving, and came back with the packs on.
    Note("Your save remembers the packs it was made with: with one of them off, the game warns that the save needs "
         "it. Switch the pack back on and everything is there again.");
    // One row per pack: its own name (the pack's main entry), ON / OFF; the grey line has every item, size, state.
    const float w = ImGui::GetContentRegionAvail().x, label_room = w - std::min(300 * d, w * 0.5f) - 30 * d;
    for (const auto& p : dlc_packs_) {
      std::string label = Upper(p.ShortName());
      while (label.size() > 4 && MenuTextSize(20 * d, label.c_str()).x > label_room) label.resize(label.size() - 4), label += "...";
      const std::string names = p.entries.size() > 1 || p.package ? p.AllNames() : "";
      std::string note = (names.empty() ? "" : names + ". ") + SizeText(p.bytes) + (p.package ? " package. " : ". ") +
                         DlcStatusText(p) + ".";
      if (p.folder != ".") note += "  (" + p.folder + ")";
      ImGui::PushID(p.folder.c_str());
      if (p.status == wwe2k14::dlc::Status::kReady) {
        bool on = !DlcIsOff(p.pack_id);
        if (OptionToggle(label.c_str(), on, s.dlc, note.c_str())) SetDlcOff(p.pack_id, !on);
      } else {
        static constexpr const char* kNotReady[] = {"NOT USED"};
        int zero = 0;
        OptionRow(label.c_str(), zero, kNotReady, 1, false, note.c_str());
      }
      ImGui::PopID();
    }
  }
