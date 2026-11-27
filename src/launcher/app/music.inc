
  // ---------------- Song info (lane 283): what the importer learned about each song (the same numbers the in-game picker shows)
  // audio_import::Library() converts on first sight, so it runs on a worker thread; the UI shows "reading..." until it is back.
  // ONE worker thread does every import for this tab (songs are converted one at a time anyway: audio_import holds a lock). Its state is
  // shared with the thread by shared_ptr, so the launcher closing mid-conversion leaves nothing dangling; results come back to the
  // UI thread through a queue (PumpMusic), the UI thread never imports and never touches the disk per frame.
  // The converter (Jarrod S101: "converted from the moment it is added with a progress bar ... have the converted song done already"):
  // the LAUNCHER converts, the game never does. ONE worker thread; Add a song / Assign music / the convert pass jump ahead of the
  // cheap cache reads. Its state is shared with the thread by shared_ptr, so nothing dangles when the window closes.
  struct MusicJob {
    int kind = 0;  // 0 = read a song's numbers from the cache, 1 = Add a song, 2 = Assign music, 3 = convert pass over the music folders
    fs::path file, cache;
    std::string key;
    int char_id = -1, gen = 0;
    std::vector<fs::path> dirs;  // kind 3
  };
  struct MusicResult {
    int kind = 0;
    fs::path file;
    int char_id = -1;
    audio_import::Imported imp;
  };
  struct MusicState {
    std::mutex m;
    std::condition_variable cv;
    std::deque<MusicJob> q_import, q_meta;
    std::deque<MusicResult> results;
    std::map<std::string, std::shared_ptr<const audio_import::Song>> meta;
    std::vector<std::shared_ptr<const audio_import::Song>> retired;  // dropped entries live until the next frame (pointers handed out)
    std::set<std::string> busy;
    std::map<std::string, std::string> failed;  // path -> why it could not be converted (shown on the row)
    std::map<std::string, int> fails;  // "path|size|mtime" -> I/O failures so far (a second one makes the song refused)
    std::map<std::string, std::chrono::steady_clock::time_point> retry;  // "path|size|mtime" of an I/O failure -> not tried again before this
    int gen = 0;
    std::string cur_name, cur_stage;
    float cur_frac = 0;
    int batch_total = 0, batch_done = 0;
    bool working = false, started = false, quit = false;
    std::atomic<bool> cancel_cur{false}, cancel_all{false};
    std::chrono::steady_clock::time_point last_scan{};
  };
  std::shared_ptr<MusicState> music_ = std::make_shared<MusicState>();
  struct SongStamp {
    std::string key;
    std::chrono::steady_clock::time_point checked{};
  };
  std::map<std::string, SongStamp> song_stamp_;  // path -> its (size, mtime) key, looked at on disk at most every 3 s

  // One conversion with progress, on the worker. Returns the result; updates the bar's state.
  static audio_import::Imported MusicConvert(MusicState& st, const fs::path& file, const fs::path& cache) {
    {
      std::lock_guard lock(st.m);
      st.cur_name = PathStr(file.filename()), st.cur_stage = "Reading", st.cur_frac = 0;
    }
    st.cancel_cur = false;
    return audio_import::Import(file, cache, audio_import::kTargetLufs, [&st](const audio_import::Progress& p) {
      {
        std::lock_guard lock(st.m);
        st.cur_frac = p.overall, st.cur_stage = audio_import::StageName(p.stage);
      }
      return !st.cancel_cur.load() && !st.cancel_all.load();
    });
  }
  static void MusicNote(MusicState& st, const fs::path& file, const fs::path& cache, const audio_import::Imported& imp) {  // st.m held
    const std::string path = PathStr(file);
    if (imp.cancelled) return;
    if (imp.io_failed) {
      const std::string fk = MusicFileKey(file);
      st.retry[fk] = std::chrono::steady_clock::now() + std::chrono::seconds(60);  // not re-tried in a loop
      if (++st.fails[fk] >= 2) {  // a song that fails every time (bad_alloc on a huge file ...) is indexed as refused after two tries
        audio_import::MarkRefused(file, cache, "this file could not be converted on this machine (" + imp.err + ")");
      }
    }
    if (!imp.ok) st.failed[path] = imp.err;
    else if (imp.io_failed) st.failed[path] = imp.note;
    else st.failed.erase(path);
  }
  static std::string MusicFileKey(const fs::path& file) {
    std::error_code e1, e2;
    const uintmax_t size = fs::file_size(file, e1);
    const auto mt = fs::last_write_time(file, e2).time_since_epoch().count();
    return PathStr(file) + "|" + std::to_string(size) + "|" + std::to_string((long long)mt);
  }
  static void MusicWorker(std::shared_ptr<MusicState> sp) {
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);  // a conversion never competes with a running game for the CPU / disk
    MusicState& st = *sp;
    for (;;) {
      MusicJob j;
      {
        std::unique_lock lock(st.m);
        st.cv.wait(lock, [&] { return st.quit || !st.q_import.empty() || !st.q_meta.empty(); });
        if (st.quit) return;
        auto& q = !st.q_import.empty() ? st.q_import : st.q_meta;  // conversions first, cache reads after
        j = std::move(q.front());
        q.pop_front();
      }
      if (j.kind == 0) {
        auto song = std::make_shared<const audio_import::Song>(audio_import::LibraryCached(j.file, j.cache));
        std::lock_guard lock(st.m);
        if (j.gen == st.gen) st.meta[j.key] = song;
        st.busy.erase(j.key);
      } else if (j.kind == 3) {
        audio_import::CleanStale(j.cache);
        std::vector<fs::path> todo;
        for (const fs::path& d : j.dirs) {
          std::error_code ec;
          for (fs::directory_iterator it(d, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code e2;
            if (!it->is_regular_file(e2) || !audio_import::IsSupportedExt(it->path())) continue;
            const audio_import::Imported l = audio_import::Lookup(it->path(), j.cache);
            if (l.ok || !l.err.empty()) continue;
            {
              std::lock_guard lock(st.m);
              auto r = st.retry.find(MusicFileKey(it->path()));
              if (r != st.retry.end() && r->second > std::chrono::steady_clock::now()) continue;  // an I/O failure a moment ago
            }
            todo.push_back(it->path());  // not converted yet (a new file, an old cache version, a dropped-in file)
          }
        }
        {
          std::lock_guard lock(st.m);
          st.batch_total += int(todo.size());
        }
        for (const fs::path& f : todo) {
          if (st.cancel_all.load()) break;
          const audio_import::Imported imp = MusicConvert(st, f, j.cache);
          std::lock_guard lock(st.m);
          ++st.batch_done;
          MusicNote(st, f, j.cache, imp);
          MusicResult r;
          r.kind = 3, r.file = f, r.imp = imp;
          st.results.push_back(std::move(r));
        }
      } else {
        MusicResult r;
        r.kind = j.kind, r.file = j.file, r.char_id = j.char_id;
        if (st.cancel_all.load()) {
          r.imp.cancelled = true, r.imp.err = "cancelled";
        } else {
          r.imp = MusicConvert(st, j.file, j.cache);
        }
        std::lock_guard lock(st.m);
        ++st.batch_done;
        MusicNote(st, j.file, j.cache, r.imp);
        st.results.push_back(std::move(r));
      }
      std::lock_guard lock(st.m);
      if (st.q_import.empty()) {
        st.working = false, st.cancel_all = false;
        st.batch_total = st.batch_done = 0;
        st.cur_name.clear();
      }
    }
  }
  void MusicEnqueue(MusicJob j, bool is_import) {
    std::lock_guard lock(music_->m);
    if (!music_->started) music_->started = true, std::thread([m = music_] { wwe2k14::NameThread(L"lnch_music"); MusicWorker(m); }).detach();
    if (is_import) {
      if (!music_->working) music_->batch_total = music_->batch_done = 0, music_->cancel_all = false;
      music_->working = true;
      if (j.kind != 3) ++music_->batch_total;
      music_->q_import.push_back(std::move(j));
    } else {
      music_->q_meta.push_back(std::move(j));
    }
    music_->cv.notify_one();
  }
  bool MusicBusy() {
    std::lock_guard lock(music_->m);
    return music_->working;
  }
  const audio_import::Song* SongMeta(const fs::path& file) {
    const auto now = std::chrono::steady_clock::now();
    const std::string path = PathStr(file);
    SongStamp& sp = song_stamp_[path];
    if (sp.key.empty() || now - sp.checked > std::chrono::seconds(3)) {
      std::error_code ec;
      const uintmax_t size = fs::file_size(file, ec);
      const auto mt = fs::last_write_time(file, ec).time_since_epoch().count();
      sp.key = path + "|" + std::to_string(size) + "|" + std::to_string(mt);
      sp.checked = now;
    }
    int gen;
    {
      std::lock_guard lock(music_->m);
      auto it = music_->meta.find(sp.key);
      if (it != music_->meta.end()) return it->second.get();
      if (!music_->busy.insert(sp.key).second) return nullptr;
      gen = music_->gen;
    }
    MusicJob j;
    j.kind = 0, j.file = file, j.cache = MusicCacheDir(paths_), j.key = sp.key, j.gen = gen;
    MusicEnqueue(std::move(j), false);
    return nullptr;
  }
  std::optional<std::string> SongFailure(const fs::path& file) {  // the reason a conversion failed, shown on the row (a copy)
    std::lock_guard lock(music_->m);
    auto it = music_->failed.find(PathStr(file));
    return it == music_->failed.end() ? std::nullopt : std::optional<std::string>(it->second);
  }
  // Add a song... / Assign music...: converted right then, with the progress bar; the song is added / assigned when it is done.
  void StartMusicImport(int kind, const fs::path& file, int char_id) {
    MusicJob j;
    j.kind = kind, j.file = file, j.cache = MusicCacheDir(paths_), j.char_id = char_id;
    MusicEnqueue(std::move(j), true);
  }
  // The convert pass: every song in the mods music folders without a current cache (a new file, a cache version bump, a file dropped in
  // by hand), at start and when the MUSIC tab opens.
  // Automation, a running game, or USE MY MUSIC off: nothing is converted or waited for (an automated test's start is never deferred).
  bool MusicAutomation() const {
    return automation_ || !s.mods || s.harness || perf_step_ >= 0 || check_running_ || diag_running_ || importing_;
  }
  // Starts the pass when none runs. true = a pass is running / queued now.
  bool ScanMusic(bool force) {
    if (MusicAutomation() || game_proc_) return false;  // never while the game runs: the hidden launcher must not convert during a match
    const auto now = std::chrono::steady_clock::now();
    {
      std::lock_guard lock(music_->m);
      if (music_->working) return true;
      if (!force && music_->last_scan != std::chrono::steady_clock::time_point{} && now - music_->last_scan < std::chrono::seconds(20)) return false;
      music_->last_scan = now;
    }
    MusicJob j;
    j.kind = 3, j.cache = MusicCacheDir(paths_);
    j.dirs = {paths_.mods / "music", NewSongsDir(paths_), paths_.mods / "music" / "library"};
    MusicEnqueue(std::move(j), true);
    return true;
  }
  // Is there a song the game would read that has no current cache? Lookups only (stats + the index): milliseconds, on the UI thread.
  bool MusicTodo() {
    const fs::path cache = MusicCacheDir(paths_);
    const auto now = std::chrono::steady_clock::now();
    for (const fs::path& d : {paths_.mods / "music", NewSongsDir(paths_), paths_.mods / "music" / "library"}) {
      std::error_code ec;
      for (fs::directory_iterator it(d, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (!it->is_regular_file(e2) || !audio_import::IsSupportedExt(it->path())) continue;
        const audio_import::Imported l = audio_import::Lookup(it->path(), cache);
        if (l.ok || !l.err.empty()) continue;
        std::lock_guard lock(music_->m);
        auto r = music_->retry.find(MusicFileKey(it->path()));
        if (r != music_->retry.end() && r->second > now) continue;
        return true;
      }
    }
    return false;
  }
  // PLAY: wait for the converter? True when songs are converting or some still need it (the pass is started on the spot).
  bool MusicWaitWanted() {
    if (MusicAutomation()) return false;
    if (MusicBusy()) return true;
    return MusicTodo() && ScanMusic(true);
  }
  // The copy the game will read has the same content as the file that was just converted, so this is a cache hit (a hash + the index entry),
  // with the bar still up: PLAY cannot slip in between the copy and its index entry.
  void IndexCopy(const fs::path& copy) {
    if (copy.empty()) return;
    MusicJob j;
    j.kind = 4, j.file = copy, j.cache = MusicCacheDir(paths_);
    MusicEnqueue(std::move(j), true);
  }
  // Once per frame (any page): finished conversions are applied, the busy flag for the window's close button is kept, and a PLAY that
  // waited for the converter goes ahead when it is done.
  void MusicTick() {
    if (!music_scan_started_) music_scan_started_ = true, ScanMusic(true);
    std::deque<MusicResult> done;
    {
      std::lock_guard lock(music_->m);
      done.swap(music_->results);
      music_->retired.clear();
      if (!done.empty()) {  // the cache changed: what the rows know is stale
        for (auto& kv : music_->meta) music_->retired.push_back(kv.second);
        music_->meta.clear();
        ++music_->gen;
      }
    }
    for (const MusicResult& r : done) {
      if (r.kind >= 3 || r.imp.cancelled) continue;  // (the pass's and the index jobs' results show on the rows; a cancel says nothing)
      const std::string name = PathStr(r.file.filename());
      if (!r.imp.ok) {
        Status((r.kind == 1 ? "Song not added: " : "Music not assigned: ") + r.imp.err);
      } else if (r.kind == 1) {
        const std::string err = AddNewSong(paths_, r.file);  // copy first ...
        Status(err.empty() ? "New song added: " + name + SongLoudnessNote(r.imp) : "Song not added: " + err);
        songs_ = ReadNewSongs(paths_), songs_checked_ = std::chrono::steady_clock::now();
        if (err.empty()) IndexCopy(NewSongsDir(paths_) / r.file.filename());  // ... then index the COPY, the path the game reads
      } else {
        const std::string err = AssignMusic(paths_, r.char_id, r.file);
        Status(err.empty() ? "Entrance music assigned: " + name + SongLoudnessNote(r.imp) : "Music not assigned: " + err);
        if (err.empty()) IndexCopy(MusicFor(paths_, r.char_id));
      }
    }
    const bool busy = MusicBusy();
    g_music_busy = busy;
    if (!busy && music_start_pending_ && !close_after_music_) {  // PLAY was waiting for the converter
      music_start_pending_ = false;
      music_skip_ = true;  // one pass only: if a song is still unconverted after it (an index that cannot be updated), the game starts anyway
      StartGame(music_pending_cmd_, music_pending_msg_);
      if (direct_after_precompile_ && game_proc_) {
        direct_after_precompile_ = false;
        exit_with_game_ = true;
        ShowWindow(hwnd_, SW_HIDE);
        Log("direct start: the game started after the songs were converted");
      }
    }
    if (!busy && close_after_music_) DestroyWindow(hwnd_);
  }
  bool automation_ = false;  // launcher steps were given on the command line
  bool music_scan_started_ = false, music_start_pending_ = false, want_music_popup_ = false, music_skip_ = false, close_after_music_ = false;
  Command music_pending_cmd_;
  std::string music_pending_msg_;

  // The bar: what is converting now, "song 2 of 5", the stage, Cancel. Returns true when drawn.
  bool DrawMusicProgress(float width) {
    std::string name, stage;
    float frac;
    int total, done;
    {
      std::lock_guard lock(music_->m);
      if (!music_->working) return false;
      name = music_->cur_name, stage = music_->cur_stage, frac = music_->cur_frac, total = music_->batch_total, done = music_->batch_done;
    }
    if (name.empty()) {
      ImGui::TextDisabled("Checking your songs...");
      return true;
    }
    if (total > 1) ImGui::Text("Converting %s  (song %d of %d)", name.c_str(), std::min(done + 1, total), total);
    else ImGui::Text("Converting %s", name.c_str());
    char lab[96];
    std::snprintf(lab, sizeof lab, "%s  %d%%", stage.c_str(), int(frac * 100.f));
    ImGui::ProgressBar(frac, ImVec2(width, 0), lab);
    return true;
  }
  void MusicCancelAll() {
    music_->cancel_all = true, music_->cancel_cur = true;
  }
  // PLAY while songs convert (waits with the bar; PLAY WITHOUT WAITING lets the game start now with the unconverted originals) and the
  // window's close button while one converts (wait / cancel and close).
  void MusicPopup() {
    if (want_music_popup_) ImGui::OpenPopup("music_conv"), want_music_popup_ = false;
    if (g_close_asked.load() && !close_after_music_) ImGui::OpenPopup("music_close");
    if (BeginGameModal("music_conv", "CONVERTING YOUR SONGS", 640 * Dpi())) {
      if (!music_start_pending_ || !MusicBusy()) {
        ImGui::CloseCurrentPopup();
      } else {
        ImGui::TextWrapped("Your songs are being converted to the game's loudness. The game starts when that is done.");
        ImGui::Dummy(ImVec2(0, 8 * Dpi()));
        DrawMusicProgress(-1);
        ImGui::Dummy(ImVec2(0, 12 * Dpi()));
        if (ModalMainButton("PLAY WITHOUT WAITING")) {  // the unconverted originals play as they are (or the game's own theme where it cannot)
          music_start_pending_ = false;
          music_skip_ = true;  // stays set until the game has started (a start diverted to the precompile / superstars comes back here)
          MusicCancelAll();    // no conversion runs during the game: the rest is converted the next time the launcher opens
          ImGui::CloseCurrentPopup();
          StartGame(music_pending_cmd_, music_pending_msg_);
          if (direct_after_precompile_ && game_proc_) {
            direct_after_precompile_ = false;
            exit_with_game_ = true;
            ShowWindow(hwnd_, SW_HIDE);
          }
        } else if (ModalButton("CANCEL")) {
          music_start_pending_ = false;
          music_skip_ = false;
          direct_after_precompile_ = false;
          ImGui::CloseCurrentPopup();
        }
        ImGui::NewLine();
      }
      ImGui::EndPopup();
    }
    if (BeginGameModal("music_close", "A SONG IS CONVERTING", 600 * Dpi())) {
      g_close_asked = false;
      ImGui::TextWrapped("Closing now would stop the conversion; the song would be converted the next time the launcher opens.");
      ImGui::Dummy(ImVec2(0, 8 * Dpi()));
      DrawMusicProgress(-1);
      ImGui::Dummy(ImVec2(0, 12 * Dpi()));
      if (!MusicBusy()) {  // it finished by itself while this was up: the close the player asked for goes ahead
        close_after_music_ = true;
        ImGui::CloseCurrentPopup();
      } else if (ModalMainButton("WAIT AND CLOSE")) {
        close_after_music_ = true;
        ImGui::CloseCurrentPopup();
      } else if (ModalButton("CANCEL AND CLOSE")) {
        MusicCancelAll();
        close_after_music_ = true;
        music_start_pending_ = false;  // a PLAY that was waiting must not start the game on the way out
        direct_after_precompile_ = false;
        ImGui::CloseCurrentPopup();
      } else if (ModalButton("KEEP OPEN")) {
        if (music_start_pending_) want_music_popup_ = true;  // the PLAY that was waiting gets its popup back
        ImGui::CloseCurrentPopup();
      }
      ImGui::NewLine();
      ImGui::EndPopup();
    }
  }
  static std::string SongStem(const std::string& file) { return PathStr(fs::path(file).stem()); }
  static std::string SongTime(double sec) {
    const int t = int(sec + 0.5);
    char b[24];
    std::snprintf(b, sizeof b, "%d:%02d", t / 60, t % 60);
    return b;
  }
  // The album art as a square thumbnail, or a generic record (black disc, grooves, coloured label) when the file had none.
  void DrawSongArt(ImDrawList* dl, const audio_import::Song* m, ImVec2 a, float size) {
    if (m && m->ok && !m->thumb.empty()) {
      if (ID3D11ShaderResourceView* srv = PngTexture(m->thumb)) {
        dl->AddImage(ImTextureRef(reinterpret_cast<ImTextureID>(srv)), a, ImVec2(a.x + size, a.y + size));
        return;
      }
    }
    const ImVec2 c(a.x + size / 2, a.y + size / 2);
    const float r = size / 2;
    dl->AddRectFilled(a, ImVec2(a.x + size, a.y + size), IM_COL32(36, 36, 44, 255), 4.0f);
    dl->AddCircleFilled(c, r * 0.92f, IM_COL32(14, 14, 18, 255), 40);
    for (float g = 0.45f; g < 0.9f; g += 0.12f) dl->AddCircle(c, r * g, IM_COL32(48, 48, 58, 255), 40, 1.0f);
    dl->AddCircleFilled(c, r * 0.30f, IM_COL32(176, 52, 52, 255), 24);
    dl->AddCircleFilled(c, r * 0.06f, IM_COL32(20, 20, 24, 255), 12);
  }
  void DrawSongInfo(const audio_import::Song* m, float dpi) {
    if (!m) {
      ImGui::TextDisabled("Reading the song...");
      return;
    }
    if (!m->ok) {
      if (m->err.empty()) ImGui::TextDisabled("Waiting to be converted to the game's loudness.");
      else ImGui::TextColored(ImVec4(0.9f, 0.45f, 0.35f, 1), "This song cannot be played: %s", m->err.c_str());
      return;
    }
    const float art = 120 * dpi;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    DrawSongArt(ImGui::GetWindowDrawList(), m, p0, art);
    ImGui::SetCursorScreenPos(ImVec2(p0.x + art + 12 * dpi, p0.y));
    ImGui::BeginGroup();
    ImGui::TextWrapped("%s", m->title.c_str());
    if (!m->artist.empty()) ImGui::TextDisabled("%s", m->artist.c_str());
    if (!m->album.empty()) ImGui::TextDisabled("%s", m->album.c_str());
    ImGui::Text("%s", SongTime(m->seconds).c_str());
    ImGui::Spacing();
    if (m->normalised) {
      ImGui::Text("Loudness %.1f LUFS", m->out_lufs);
      ImGui::TextDisabled("was %.1f LUFS, gain %+.1f dB%s", m->src_lufs, m->gain_db, m->limited ? ", peak limiter on" : "");
      ImGui::TextDisabled("the game's own themes: about %.1f LUFS", audio_import::kTargetLufs);
    } else {
      ImGui::TextColored(ImVec4(0.9f, 0.75f, 0.35f, 1), "Not normalised");
      ImGui::TextWrapped("%s", m->note.c_str());
    }
    ImGui::EndGroup();
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + art + 6 * dpi));
  }
  static std::string SongLoudnessNote(const audio_import::Imported& i) {
    char b[96];
    if (!i.normalised) return " (not normalised: " + i.note + ")";
    std::snprintf(b, sizeof b, " (%.1f LUFS -> %.1f, %+.1f dB%s)", i.src_lufs, i.out_lufs, i.gain_db, i.limited ? ", limited" : "");
    return b;
  }

  // ---------------- Music (Session 75, Jarrod: "music be its own tab")
  // Left: NEW SONGS (extra entries in the game's MUSIC list, src\music_slots.h) and WRESTLERS' THEMES (a replacement
  // for one wrestler's own theme, Session 33, src\entrance_music.h). Right: the selected song or wrestler.
  void MusicTab() {
    const float dpi = Dpi(), right_w = 430 * dpi;
    const auto now = std::chrono::steady_clock::now();
    if (now - songs_checked_ > std::chrono::seconds(2)) songs_ = ReadNewSongs(paths_), songs_checked_ = now;
    const float list_h = -ImGui::GetFrameHeightWithSpacing() * 1.4f;
    ImGui::BeginChild("music_lists", ImVec2(-right_w - 8, list_h), ImGuiChildFlags_Borders);
    Heading("NEW SONGS", 1.15f);
    ImGui::TextDisabled("Extra entries at the end of the game's MUSIC list; any wrestler can have one.");
    const float songs_h = std::max(90 * dpi, ImGui::GetContentRegionAvail().y * 0.38f);
    if (ImGui::BeginTable("songs", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
                          ImVec2(0, songs_h))) {
      ImGui::TableSetupScrollFreeze(0, 1);
      ImGui::TableSetupColumn("Name in the game", ImGuiTableColumnFlags_WidthStretch, 3.0f);
      ImGui::TableSetupColumn("Loudness", ImGuiTableColumnFlags_WidthStretch, 1.8f);
      ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthStretch, 1.6f);
      ImGui::TableSetupColumn("Id", ImGuiTableColumnFlags_WidthStretch, 0.5f);
      ImGui::TableHeadersRow();
      int shown = 0;
      for (int i = 0; i < int(songs_.size()); ++i) {
        const NewSong& sg = songs_[size_t(i)];
        if (!sg.present) continue;
        ++shown;
        const float row_h = 46 * dpi;
        ImGui::TableNextRow(ImGuiTableRowFlags_None, row_h);
        ImGui::TableNextColumn();
        const bool sel = music_kind_ == 1 && music_sel_ == i;
        const audio_import::Song* meta = SongMeta(NewSongsDir(paths_) / sg.file);
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable(("##song" + std::to_string(sg.id)).c_str(), sel,
                              ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap, ImVec2(0, row_h - 2 * dpi))) {
          music_kind_ = 1, music_sel_ = i, song_name_edit_ = sg.name;
        }
        const float th = row_h - 6 * dpi;
        DrawSongArt(ImGui::GetWindowDrawList(), meta, ImVec2(p0.x + 2 * dpi, p0.y + 2 * dpi), th);
        ImGui::SetCursorScreenPos(ImVec2(p0.x + th + 10 * dpi, p0.y + 2 * dpi));
        ImGui::BeginGroup();
        ImGui::TextUnformatted(sg.name.c_str());
        if (meta && meta->ok) {
          std::string sub = meta->title != SongStem(sg.file) ? meta->title : std::string();
          if (!meta->artist.empty()) sub += (sub.empty() ? "" : " - ") + meta->artist;
          sub += (sub.empty() ? "" : "   ") + SongTime(meta->seconds);
          ImGui::TextDisabled("%s", sub.c_str());
        } else if (const auto why = SongFailure(NewSongsDir(paths_) / sg.file)) {
          ImGui::TextColored(ImVec4(0.9f, 0.45f, 0.35f, 1), "could not be converted: %s", why->c_str());
        } else if (meta && !meta->err.empty()) {
          ImGui::TextColored(ImVec4(0.9f, 0.45f, 0.35f, 1), "cannot be played: %s", meta->err.c_str());
        } else if (meta) {
          ImGui::TextDisabled("waiting to be converted");
        } else {
          ImGui::TextDisabled("reading...");
        }
        ImGui::EndGroup();
        ImGui::TableNextColumn();
        if (meta && meta->ok && meta->normalised) {
          ImGui::Text("%.1f LUFS", meta->out_lufs);
          ImGui::TextDisabled("%+.1f dB%s", meta->gain_db, meta->limited ? ", limited" : "");
        } else if (meta && meta->ok) {
          ImGui::TextDisabled("not normalised");
        }
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(sg.file.c_str());
        ImGui::TableNextColumn();
        ImGui::Text("%d", sg.id);
      }
      if (!shown) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("No new songs yet: Add a song...");
      }
      ImGui::EndTable();
    }
    ScanMusic(false);  // anything without a current cache is converted now, with the bar below
    if (ImGui::Button("Add a song...")) {
      for (const fs::path& f : PickAudios(hwnd_)) StartMusicImport(1, f, -1);  // converted right then; each song appears when it is done
    }
    ImGui::SameLine();
    if (ImGui::Button("Open the songs folder")) {
      std::error_code ec;
      fs::create_directories(NewSongsDir(paths_), ec);
      ShellExecuteW(hwnd_, L"explore", NewSongsDir(paths_).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", PathStr(NewSongsDir(paths_)).c_str());
    if (DrawMusicProgress(-90 * dpi)) {
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) MusicCancelAll();
    }
    ImGui::Spacing();
    ImGui::Separator();
    Heading("WRESTLERS' THEMES", 1.15f);
    ImGui::TextDisabled("Replace one wrestler's own theme: yours plays at his entrance instead.");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##music_search", "Search name or id", &music_search_);
    if (ImGui::BeginTable("themes", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupScrollFreeze(0, 1);
      ImGui::TableSetupColumn("Character", ImGuiTableColumnFlags_WidthStretch, 3.0f);
      ImGui::TableSetupColumn("Id", ImGuiTableColumnFlags_WidthStretch, 0.6f);
      ImGui::TableSetupColumn("Theme", ImGuiTableColumnFlags_WidthStretch, 2.0f);
      ImGui::TableHeadersRow();
      const std::string q = Lower(music_search_);
      for (int i = 0; i < int(chars_.size()); ++i) {
        const Character& c = chars_[size_t(i)];
        if (!q.empty() && Lower(c.name).find(q) == std::string::npos && std::to_string(c.id).find(q) == std::string::npos)
          continue;
        const fs::path mu = MusicFor(paths_, c.id);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        const bool sel = music_kind_ == 2 && music_sel_ == i;
        if (ImGui::Selectable((c.name + "##theme" + std::to_string(i)).c_str(), sel, ImGuiSelectableFlags_SpanAllColumns))
          music_kind_ = 2, music_sel_ = i;
        ImGui::TableNextColumn();
        ImGui::Text("%d", c.id);
        ImGui::TableNextColumn();
        if (mu.empty()) ImGui::TextDisabled("original");
        else ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1), "%s", PathStr(mu.filename()).c_str());
      }
      ImGui::EndTable();
    }
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("music_detail", ImVec2(right_w, list_h), ImGuiChildFlags_Borders);
    if (music_kind_ == 1 && music_sel_ >= 0 && music_sel_ < int(songs_.size()) && songs_[size_t(music_sel_)].present) {
      const NewSong sg = songs_[size_t(music_sel_)];
      Heading(sg.name.c_str(), 1.25f);
      ImGui::Text("Music id %d", sg.id);
      ImGui::TextDisabled("File: %s", sg.file.c_str());
      DrawSongInfo(SongMeta(NewSongsDir(paths_) / sg.file), dpi);
      ImGui::Separator();
      ImGui::TextUnformatted("Name in the game's MUSIC list:");
      ImGui::SetNextItemWidth(-1);
      ImGui::InputText("##song_name", &song_name_edit_);
      if (ImGui::Button("Rename", ImVec2(-1, 0))) {
        const std::string err = RenameNewSong(paths_, sg.id, song_name_edit_);
        songs_ = ReadNewSongs(paths_), songs_checked_ = now;
        for (const auto& x : songs_)
          if (x.id == sg.id) song_name_edit_ = x.name;
        Status(err.empty() ? "Renamed: " + song_name_edit_ : "Not renamed: " + err);
      }
      ImGui::Spacing();
      if (ImGui::Button("Remove this song...", ImVec2(-1, 0))) ImGui::OpenPopup("remove_song");
      if (ImGui::BeginPopupModal("remove_song", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Remove %s from the songs folder?", sg.file.c_str());
        ImGui::TextDisabled("A wrestler who has it will have no entrance music until you pick another.");
        if (ImGui::Button("Remove")) {
          const std::string err = RemoveNewSong(paths_, sg.file);
          songs_ = ReadNewSongs(paths_), songs_checked_ = now, music_kind_ = 0, music_sel_ = -1;
          Status(err.empty() ? "Removed: " + sg.file : "Not removed: " + err);
          ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
      }
      ImGui::Spacing();
      ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
      ImGui::TextWrapped("In the game: CREATIONS > ENTRANCE > pick a superstar > MUSIC: this song is at the end of the "
                         "list. Give it to the wrestler there and save; it plays at his entrance. It keeps its id, so "
                         "adding or removing other songs never changes whose song is whose. Needs USE MY MUSIC on.");
      ImGui::PopStyleColor();
    } else if (music_kind_ == 2 && music_sel_ >= 0 && music_sel_ < int(chars_.size())) {
      const Character& c = chars_[size_t(music_sel_)];
      Heading(Upper(c.name).c_str(), 1.25f);
      ImGui::Text("Character id %d", c.id);
      ImGui::Separator();
      const fs::path mu = MusicFor(paths_, c.id);
      if (mu.empty()) ImGui::TextUnformatted("Entrance music: the original");
      else ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1), "Entrance music: %s", PathStr(mu.filename()).c_str());
      if (!mu.empty()) DrawSongInfo(SongMeta(mu), dpi);
      if (ImGui::Button("Assign music...", ImVec2(-1, 0))) {
        if (auto f = PickAudio(hwnd_)) StartMusicImport(2, *f, c.id);
      }
      ImGui::BeginDisabled(mu.empty());
      if (ImGui::Button("Original music", ImVec2(-1, 0))) RevertMusic(paths_, c.id), Status("Original entrance music");
      ImGui::EndDisabled();
      ImGui::Spacing();
      ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
      ImGui::TextWrapped("MP3 / WAV / OGG / OPUS / FLAC / M4A / AAC / WMA, converted to the loudness of the game's own themes. Plays in normal play (not in replays); copied into the "
                         "mods folder as music\\%d. To give a wrestler a song without losing anyone's theme, add it "
                         "as a new song instead. Needs USE MY MUSIC on.", c.id);
      ImGui::PopStyleColor();
    } else {
      Heading("MUSIC", 1.25f);
      ImGui::TextWrapped("New songs are extra entries in the game's own MUSIC list (CREATIONS > ENTRANCE > pick a "
                         "superstar > MUSIC), named after their files; nothing is replaced. Wrestlers' themes replace "
                         "one wrestler's own theme.");
      ImGui::Spacing();
      ImGui::TextDisabled("Pick a song or a wrestler on the left.");
    }
    ImGui::EndChild();
    ImGui::TextDisabled("%zu new song(s); MP3 / WAV / OGG / OPUS / FLAC / M4A / AAC / WMA", std::count_if(songs_.begin(), songs_.end(),
                                                                                         [](const NewSong& x) { return x.present; }));
  }
