
  // ---------------- Report a problem
  void ReportPopup() {
    if (BeginGameModal("report", "REPORT A PROBLEM", 620 * Dpi())) {
      ImGui::TextWrapped("Make a problem report: a .zip you can send us.");
      ImGui::Spacing();
      Note("Inside: the logs of your last 3 games (with which wrestlers, arena and match you played), small "
           "screenshots of each scene, the moments you marked with F11, the performance report, the launcher's "
           "settings, the version and your PC's basics (graphics cards and drivers, CPU, RAM, Windows, monitor). Your "
           "user name is removed from file paths. Nothing is sent: the folder opens and you choose what to do with "
           "it.");
      Note("Tip: press F11 in the game when you see a problem. It marks the moment (a screenshot and what the game "
           "was doing) for this report.");
      ImGui::Spacing();
      ImGui::TextUnformatted("What happened? (optional)");
      ImGui::InputTextMultiline("##report_note", report_note_, sizeof(report_note_),
                                ImVec2(-FLT_MIN, 90 * Dpi()));
      ImGui::Dummy(ImVec2(0, 12 * Dpi()));
      if (ModalMainButton("MAKE THE REPORT")) {
        ImGui::CloseCurrentPopup();
        MakeReport();
      }
      if (ModalButton("Cancel")) ImGui::CloseCurrentPopup();
      ImGui::NewLine();
      ImGui::EndPopup();
    }
  }

  // Replaces the user's profile folder (C:\Users\NAME) in text with %USERPROFILE%.
  static std::string Redact(std::string t) {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH);
    // S108 MOB: the rules are in report_redact.h (shared with the phone launcher): the profile and user-name passes are this function's old body verbatim,
    // plus another user's profile, a UNC path and an email-shaped string. (No USERPROFILE: only those three.)
    return report_redact::Redact(std::move(t), report_redact::PcRules(n && n < MAX_PATH ? Narrow(buf) : std::string()));
  }

  static fs::path Newest(const fs::path& dir, const std::string& prefix, const std::string& ext) {
    fs::path best;
    fs::file_time_type t{};
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
      const std::string n = PathStr(e.path().filename());
      if (!e.is_regular_file() || n.rfind(prefix, 0) != 0 || Lower(PathStr(e.path().extension())) != ext) continue;
      const auto w = e.last_write_time(ec);
      if (best.empty() || w > t) best = e.path(), t = w;
    }
    return best;
  }

  static std::string ReadTail(const fs::path& p, size_t max_bytes) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return {};
    const std::streamoff size = f.tellg();
    const std::streamoff from = size > std::streamoff(max_bytes) ? size - std::streamoff(max_bytes) : 0;
    f.seekg(from);
    std::string s((std::istreambuf_iterator<char>(f)), {});
    if (from) s = "[... the first " + std::to_string(from) + " bytes left out ...]\n" + s;
    return s;
  }

  // Session 58: every display adapter Windows lists, with its driver version and date, and the CPU's threads / cores.
  static std::string PcDetailsText() {
    std::string t = "Graphics drivers:\n";
    const wchar_t* cls = L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}";
    for (int i = 0; i < 16; ++i) {
      wchar_t key[256];
      swprintf(key, 256, L"%s\\%04d", cls, i);
      const std::string desc = RegString(HKEY_LOCAL_MACHINE, key, L"DriverDesc");
      if (desc.empty()) continue;
      const std::string driver_date = RegString(HKEY_LOCAL_MACHINE, key, L"DriverDate");
      t += "  " + desc + ": driver " + RegString(HKEY_LOCAL_MACHINE, key, L"DriverVersion") + " (" + driver_date + ")";
      {  // S105 A9: a driver older than two years is flagged (the registry date is M-D-YYYY); a text that does not parse is left alone
        int m = 0, d = 0, y = 0;
        // (review F6) Windows' own built-in drivers (Microsoft Basic Display / Remote Display) carry a 2006 date: not a card maker's driver, no flag
        const bool windows_own = RegString(HKEY_LOCAL_MACHINE, key, L"ProviderName").find("Microsoft") != std::string::npos;
        if (!windows_own && std::sscanf(driver_date.c_str(), "%d-%d-%d", &m, &d, &y) == 3 && y >= 1990 && m >= 1 && m <= 12 && d >= 1 && d <= 31) {
          std::tm tm{};
          tm.tm_year = y - 1900, tm.tm_mon = m - 1, tm.tm_mday = d, tm.tm_hour = 12;
          const std::time_t then = std::mktime(&tm);
          if (then != std::time_t(-1) && std::difftime(std::time(nullptr), then) > 730.0 * 86400.0)
            t += " - older than two years: an update from the card maker may fix graphics problems";
        }
      }
      t += "\n";
    }
    DWORD len = 0;
    uint32_t cores = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    std::vector<uint8_t> buf(len);
    if (len && GetLogicalProcessorInformationEx(RelationProcessorCore,
                                                reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data()),
                                                &len)) {
      for (DWORD off = 0; off < len;) {
        const auto* p = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data() + off);
        ++cores;
        off += p->Size;
      }
    }
    t += "CPU threads: " + std::to_string(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)) + ", cores: " +
         std::to_string(cores) + "\n";
    {  // S108 MOB: the CPU's vendor and the instruction-set features the game's two builds care about (cpu_report.h)
      int r[4] = {};
      __cpuid(r, 0);
      char vendor[13] = {};
      std::memcpy(vendor, &r[1], 4), std::memcpy(vendor + 4, &r[3], 4), std::memcpy(vendor + 8, &r[2], 4);
      __cpuid(r, 1);
      t += CpuFeaturesText(vendor, (unsigned(r[2]) & (1u << 19)) != 0, ReadCpuBits());
    }
    return t;
  }

  // Session 58: `folder`'s files into a compressed .zip with Windows' own tar.exe (Windows 10 1803 and later).
  static bool TarZip(const fs::path& folder, const fs::path& zip) {
    wchar_t sys[MAX_PATH];
    if (!GetSystemDirectoryW(sys, MAX_PATH)) return false;
    const fs::path tar = fs::path(sys) / L"tar.exe";
    std::error_code ec;
    if (!fs::exists(tar, ec)) return false;
    std::wstring cmd = L"\"" + tar.wstring() + L"\" -a -c -f \"" + zip.wstring() + L"\" -C \"" + folder.wstring() + L"\"";
    for (const auto& e : fs::recursive_directory_iterator(folder, ec)) {
      if (e.is_regular_file()) cmd += L" \"" + fs::relative(e.path(), folder, ec).generic_wstring() + L"\"";
    }
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
      return false;
    }
    const DWORD w = WaitForSingleObject(pi.hProcess, 120000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    if (w != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return w == WAIT_OBJECT_0 && code == 0 && fs::exists(zip, ec) && fs::file_size(zip, ec) > 0;
  }

  // Session 58: the newest `n` files in `dir` named prefix*ext, newest first.
  static std::vector<fs::path> NewestN(const fs::path& dir, const std::string& prefix, const std::string& ext, size_t n) {
    std::vector<std::pair<fs::file_time_type, fs::path>> all;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
      const std::string name = PathStr(e.path().filename());
      if (!e.is_regular_file() || name.rfind(prefix, 0) != 0 || Lower(PathStr(e.path().extension())) != ext) continue;
      all.emplace_back(e.last_write_time(ec), e.path());
    }
    std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<fs::path> out;
    for (size_t i = 0; i < all.size() && i < n; ++i) out.push_back(all[i].second);
    return out;
  }

  // Session 58: a game log for the report, condensed: each distinct message (its time stamp and thread id left out)
  // keeps its first 30 copies and the rest are counted at the end (an HD 520's log had one warning tens of thousands
  // of times); a result still over 24 MB keeps its first 4 and last 20 MB.
  static std::string CondenseLog(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::string text((std::istreambuf_iterator<char>(f)), {});
    std::unordered_map<std::string, uint32_t> seen;
    std::vector<std::string> repeated;
    std::string out;
    out.reserve(text.size() / 2);
    for (size_t pos = 0; pos < text.size();) {
      size_t nl = text.find('\n', pos);
      if (nl == std::string::npos) nl = text.size();
      const std::string_view line(text.data() + pos, nl - pos);
      std::string_view rest = line;
      if (rest.size() > 26 && rest[0] == '[' && rest[1] == '2') rest.remove_prefix(std::min(rest.find("] ") + 2, rest.size()));
      std::string key(rest);
      const size_t t = key.find("] [t");
      if (t != std::string::npos) {
        const size_t e = key.find("] ", t + 3);
        if (e != std::string::npos) key.erase(t + 1, e - t);
      }
      uint32_t& n = seen[key];
      if (++n <= 30) {
        out.append(line).push_back('\n');
      } else if (n == 31) {
        repeated.push_back(key);
      }
      pos = nl + 1;
    }
    if (!repeated.empty()) {
      out += "\n[report] lines left out after their first 30 copies (count left out, message):\n";
      for (const std::string& k : repeated) out += "  " + std::to_string(seen[k] - 30) + "  " + k + "\n";
    }
    constexpr size_t kMax = 24u << 20, kHead = 4u << 20, kTail = 20u << 20;
    if (out.size() > kMax) {
      out = out.substr(0, kHead) + "\n[report] ... " + std::to_string(out.size() - kHead - kTail) +
            " bytes left out ...\n" + out.substr(out.size() - kTail);
    }
    return out;
  }

  // Session 58: "Matches in this log" for about.txt, from its [wwe2k14][match] card lines (ids named from
  // data\ids\characters.json and the DLC packs' names).
  std::string MatchCardsText(const std::string& log) const {
    std::string out;
    std::string last;
    for (size_t at = log.find("[wwe2k14][match] card ("); at != std::string::npos;
         at = log.find("[wwe2k14][match] card (", at + 1)) {
      const size_t nl = log.find('\n', at);
      const std::string line = log.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
      const size_t p = line.find("participants ");
      if (p == std::string::npos) continue;
      std::string who;
      for (size_t i = p + 13; i < line.size();) {
        const size_t colon = line.find(':', i);
        if (colon == std::string::npos) break;
        const int slot = std::atoi(line.c_str() + i), id = std::atoi(line.c_str() + colon + 1);
        if (slot < 40) {  // 40-42: the extra ids (roles unknown)
          std::string name = "#" + std::to_string(id);
          for (const auto* list : {&chars_, &dlc_chars_}) {
            for (const Character& c : *list) {
              if (c.id == id) name = c.name + " (" + std::to_string(id) + ")";
            }
          }
          who += (who.empty() ? "" : ", ") + name;
        }
        const size_t comma = line.find(", ", colon);
        if (comma == std::string::npos || comma > line.find(" frame=", colon)) break;
        i = comma + 2;
      }
      std::string arena;
      if (const size_t a = line.find(" arena "); a != std::string::npos) {
        const int id = std::atoi(line.c_str() + a + 7);
        arena = "arena " + std::to_string(id);
        for (const Arena& ar : arenas_) {
          if (ar.id == id) arena = ar.name + " (" + std::to_string(id) + ")";
        }
      }
      const size_t ty = line.find(" type ");
      const std::string entry = "  " + who + "; " + arena + "; match type " +
                                (ty == std::string::npos ? "?" : std::to_string(std::atoi(line.c_str() + ty + 6))) +
                                (line.find("(match clock started)") != std::string::npos ? " (played)" : "") + "\n";
      if (entry != last) out += entry;
      last = entry;
    }
    return out.empty() ? "  (none recorded)\n" : out;
  }

  void MakeReport() {
    fs::path dir = paths_.launcher_dir / "reports";
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) dir = LocalDataDir() / "reports", fs::create_directories(dir, ec);
    const std::string stamp = "report_" + NowStamp("%Y%m%d_%H%M%S");
    const fs::path zip = dir / (stamp + ".zip");
    // Session 58: the files are put together in a folder first, then zipped (compressed) with Windows' own tar.exe;
    // if that fails, the stored .zip of before.
    std::vector<std::pair<std::string, std::string>> files;  // name in the zip -> contents
    std::string about = std::string("WWE 2K14 recomp problem report\nVersion: ") + Version() + "\nMade: " +
                        NowStamp("%Y-%m-%d %H:%M") + " (local time)\n\n" + HardwareText(hw_) + PcDetailsText() +
                        "\nGame data: " + gd_.message + "\nLast game exit: " +
                        (last_exit_.empty() ? std::string("(none since the launcher started)") : last_exit_) + "\n";
    const std::string note(report_note_);
    about += "Player's note: " + (note.empty() ? std::string("(none)") : std::string("note.txt")) + "\n";
    if (!note.empty()) files.emplace_back("note.txt", note + "\n");
    // The last 3 games: logs (condensed), their scene snapshots and F11 marks.
    const fs::path captures = paths_.game_exe.parent_path() / "captures";
    size_t marks = 0;
    about += "\nGames in this report (newest first):\n";
    std::vector<fs::path> raw_logs;  // S105 C0b: the raw logs findings.txt is read from (before CondenseLog)
    for (const fs::path& log : NewestN(paths_.logs, "wwe2k14_", ".log", 3)) {
      raw_logs.push_back(log);
      const std::string name = PathStr(log.filename());
      const std::string text = Redact(CondenseLog(log));
      files.emplace_back(name, text);
      const std::string number = name.substr(8, name.size() - 8 - 4);  // wwe2k14_NNN.log
      about += "- " + name + "\n" + MatchCardsText(text);
      if (text.find("[rdr][DEVICE_LOST] =====") != std::string::npos) about += "  the graphics driver stopped (device lost) in this game\n";  // S105 A9
      {  // S103: the run's stack dump (loader stall / hang / crash: logs\flight_NNN.txt), when the game wrote one
        const std::string flight = ReadTail(paths_.logs / ("flight_" + number + ".txt"), 4u << 20);
        if (!flight.empty()) {
          files.emplace_back("flight_" + number + ".txt", Redact(flight));
          about += "  stack dump: flight_" + number + ".txt\n";
        }
      }
      for (const auto& e : fs::directory_iterator(captures, ec)) {
        const std::string c = PathStr(e.path().filename());
        if (c.rfind(number + "_scene_", 0) == 0 && Lower(PathStr(e.path().extension())) == ".jpg") {
          files.emplace_back("captures/" + c, ReadTail(e.path(), 4u << 20));
        } else if (c.rfind(number + "_bookmark_", 0) == 0 && marks < 12) {
          const std::string ext = Lower(PathStr(e.path().extension()));
          if (ext == ".png" || ext == ".jpg" || ext == ".txt") {
            files.emplace_back("captures/" + c, ext == ".txt" ? Redact(ReadTail(e.path(), 1u << 20))
                                                              : ReadTail(e.path(), 16u << 20));
            if (ext != ".txt") ++marks;
          }
        }
      }
    }
    about += "F11 marks: " + std::to_string(marks) + "\n";
    // S105 C0b: what the logs say was not drawn, per mode (findings.txt; never fails the report: a log it cannot read becomes a line saying so)
    // "log line N" counts the RAW log on the player's PC; the copy in this zip is condensed (repeated lines left out), so its line numbers differ: say so.
    files.emplace_back("findings.txt", Redact("NOTE: 'log line N' below counts the ORIGINAL wwe2k14_NNN.log on the player's PC. The copy of the log in this zip is condensed\n"
                                              "(repeated lines left out), so its line numbers differ: find a line by its HH:MM:SS time stamp.\n" +
                                              report_findings::Build(raw_logs, "the logs in this report")));
    about += "Not-drawn summary: findings.txt\n";
    const fs::path perf = Newest(paths_.logs, "perf_report_", ".txt");
    about += "Performance report: " + (perf.empty() ? std::string("(none: turn it on in Settings)") : PathStr(perf.filename())) + "\n";
    if (!perf.empty()) files.emplace_back(PathStr(perf.filename()), Redact(ReadTail(perf, 4u << 20)));
    {  // S102 U4: the latest benchmark's results (numbers and hardware only), when the player has run the button
      const std::string bench_text = ReadTail(ReportsDir() / "benchmark_latest.txt", 1u << 20);  // the dir FinishBench wrote to
      if (!bench_text.empty()) files.emplace_back("benchmark_results.txt", Redact(bench_text)), about += "Benchmark: benchmark_results.txt\n";
      const std::string bench_json = ReadTail(ReportsDir() / "benchmark_latest.json", 1u << 20);
      if (!bench_json.empty()) files.emplace_back("benchmark_results.json", Redact(bench_json));
    }
    about += DlcReportText();  // Session 32: pack names and states, never their files
    files.emplace_back("launcher_settings.json", Redact(json(s).dump(2)));
    std::string recent;
    for (const std::string& m : recent_log_) recent += m + "\n";
    files.emplace_back("launcher_messages.txt", Redact(recent));
    {  // Session 81: the self-update helper's notes (why an update failed)
      const std::string ul = ReadTail(upd::LogPath(paths_.launcher_dir), 1u << 20);
      if (!ul.empty()) files.emplace_back("update_log.txt", Redact(ul));
    }
    files.insert(files.begin(), {"about.txt", Redact(about)});

    bool zipped = false;
    const fs::path stage = dir / stamp;
    fs::create_directories(stage / "captures", ec);
    bool staged = !ec;
    for (const auto& [name, data] : files) {
      if (!staged) break;
      std::ofstream out(stage / Widen(name), std::ios::binary);
      out.write(data.data(), std::streamsize(data.size()));
      staged = bool(out);
    }
    if (staged) zipped = TarZip(stage, zip);
    fs::remove_all(stage, ec);
    if (!zipped) {
      ZipWriter z(zip);
      if (!z.ok()) {
        Status("Could not write " + PathStr(zip));
        return;
      }
      for (const auto& [name, data] : files) z.Add(name, data);
      if (!z.Close()) {
        Status("Could not finish " + PathStr(zip));
        return;
      }
    }
    report_note_[0] = 0;
    last_report_ = zip;
    Status("Report saved: " + PathStr(zip));
    if (open_folders_) {
      const std::wstring arg = L"/select,\"" + zip.wstring() + L"\"";
      ShellExecuteW(hwnd_, nullptr, L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
    }
  }
