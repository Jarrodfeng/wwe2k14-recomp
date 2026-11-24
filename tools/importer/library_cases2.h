// S110 SEAT-A: the second half of the library cases (included by test_core.cpp after library_cases.h): the "why imports fail" cases of
// Session 101 on the library + frame (FitCasesLib), the retired seats (RetireCasesLib) and the default one-build flow (BuildFlow).
#pragma once

// ---------------------------------------------------------------- the library's cards: a card's `inst` set / read by the test (no game files)
static void SetInst(const std::string& mods, const std::vector<int>& seats) {
  const fs::path lib = Root(mods) / "library";
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(lib, ec)) {
    const fs::path f = e.path() / "card.txt";
    sc::Card c;
    std::string err;
    if (!e.is_directory(ec) || !sc::ReadCard(Slash(f), c, err)) continue;
    if (std::find(seats.begin(), seats.end(), c.seat) == seats.end()) continue;
    // (S110 LIBSIZE-2 review) as the importer makes every card: for the reference identity, whatever seat he holds
    c.inst.valid = true, c.inst.seat = sc::kRefSeat, c.inst.bank = sc::kRefBank, c.inst.frame_sig = 1, c.inst.src_bank = 100, c.inst.music = 100;
    sc::SetFileForm(c, sc::kFileForm);
    sc::WriteCard(Slash(f), c, &err);
  }
}

static void RetireCasesLib(const std::string& mods) {
  std::printf("retired seats (the library's retired.txt):\n");
  const fs::path root = Root(mods);
  std::error_code ec;
  fs::remove_all(root, ec);
  auto star = [](int slot) {
    importer::Superstar s;
    s.slot = slot, s.name = "T" + std::to_string(slot);
    return s;
  };
  auto text = [&] { return TextOf(root / "library" / "retired.txt"); };
  std::string e;
  importer::PackState st;
  for (int s : {397, 398, 399}) st.stars.push_back(star(s));
  Say(importer::SavePack(mods, st, e) && importer::RetiredSlots(mods).empty(), "three imports saved: nothing retired");
  importer::PackState back;  // a launcher's REMOVE: LoadPack, drop one, SavePack
  Say(importer::LoadPack(mods, back, e) && back.stars.size() == 3 && back.retired.empty() && sc::ValidKey(back.stars[0].key), "LoadPack: 3 cards with keys, retired empty");
  {  // an unbuilt removal: no card was ever instantiated, so no save can hold the seat
    importer::PackState nb = back;
    nb.stars.erase(nb.stars.begin() + 1);
    Say(importer::SavePack(mods, nb, e) && importer::RetiredSlots(mods).empty(), "an unbuilt removal of 398 is not retired");
    Say(importer::SavePack(mods, back, e), "the library back to 3 cards");
  }
  SetInst(mods, {397, 398, 399});  // the three were made
  {  // (S110 LIBSIZE-2 review) the importer UI's REMOVE / seat hand-out ask BuiltPackSlots for the seats a build made: the CARDS' seats, never their inst seat (497)
    const std::vector<int> bs = importer::BuiltPackSlots(mods);
    std::string got;
    for (int s : bs) got += " " + std::to_string(s);
    Say(bs == std::vector<int>{397, 398, 399}, "BuiltPackSlots names the three made cards' seats (got" + got + ")");
  }
  back.stars.erase(back.stars.begin() + 1);  // 398 removed
  Say(importer::SavePack(mods, back, e), "SavePack after REMOVE of 398");
  Say(importer::RetiredSlots(mods) == std::vector<int>{398} && text().find("398") != std::string::npos, "398 is retired (read by RetiredSlots and in retired.txt)");
  importer::PackState again;
  Say(importer::LoadPack(mods, again, e) && again.retired == std::vector<int>{398} && again.stars.size() == 2, "LoadPack gives retired [398]");
  again.stars.push_back(star(400));  // a new import (the UI hands out 400, not 398)
  Say(importer::SavePack(mods, again, e) && importer::RetiredSlots(mods) == std::vector<int>{398}, "adding 400 keeps 398 retired");
  SetInst(mods, {397, 399, 400});  // 400 was made too
  importer::PackState fresh;
  importer::LoadPack(mods, fresh, e);
  fresh.retired.clear();  // a caller that kept no retired list (an older launcher's PackState): the disk's list is not lost
  fresh.stars.pop_back();  // 400 removed too
  Say(importer::SavePack(mods, fresh, e) && importer::RetiredSlots(mods) == std::vector<int>{398, 400}, "a caller with an empty list loses nothing; 400 retired too");
  fresh.stars.push_back(star(398));  // last resort: a retired seat handed out again leaves the list
  Say(importer::SavePack(mods, fresh, e) && importer::RetiredSlots(mods) == std::vector<int>{400}, "398 held again leaves the list");
  {  // T1: an old pack.json (no "retired" key) reads as an empty list
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    std::ofstream(root / "pack.json", std::ios::binary | std::ios::trunc) << "{\"version\": 1, \"enabled\": true, \"needs_build\": false, \"pack_id\": 6, \"stars\": []}\n";
    importer::PackState old;
    Say(importer::LoadPack(mods, old, e) && old.retired.empty() && importer::RetiredSlots(mods).empty(), "a pack.json without the retired key reads as empty");
  }
  {  // T2: an old pack.json WITH retired seats: they are read (and carried into the library by the first save)
    std::ofstream(root / "pack.json", std::ios::binary | std::ios::trunc)
        << "{\"version\": 1, \"enabled\": true, \"needs_build\": false, \"pack_id\": 6, \"retired\": [401, 402], \"stars\": []}\n";
    importer::PackState old;
    const bool rd = importer::LoadPack(mods, old, e) && old.retired == std::vector<int>{401, 402} && importer::RetiredSlots(mods) == std::vector<int>{401, 402};
    Say(rd && importer::SavePack(mods, old, e) && importer::RetiredSlots(mods) == std::vector<int>{401, 402}, "an old pack.json's retired seats [401 402] are read, and kept by the first save");
  }
  // (S110 SEAT-A review) an OLD pack not migrated yet (pack.json + pack6\ + superstars.txt `star` lines; no library, no frame)
  auto old_pack = [&](bool with_json) {
    fs::remove_all(root, ec);
    fs::create_directories(root / "pack6" / "info", ec);
    std::ofstream(root / "pack6" / "info" / "catalog.dlc", std::ios::binary | std::ios::trunc) << "x";
    std::ofstream(root / "superstars.txt", std::ios::binary | std::ios::trunc) << "pack 6\nbuilt_on 5\ntop 5 238A188F\nstar 397 music 100\nstar 398 music 100\n";
    if (with_json) {
      std::string j = "{\"version\": 1, \"enabled\": true, \"needs_build\": false, \"pack_id\": 6, \"retired\": [], \"stars\": [";
      for (int s : {397, 398})
        j += std::string(s == 397 ? "" : ",") + "{\"slot\": " + std::to_string(s) + ", \"name\": \"Old " + std::to_string(s) + "\", \"short_name\": \"OLD\", \"base_id\": 100, " +
             "\"entrance_from\": -1, \"copy_ratings\": true, \"source_pac\": \"\", \"renders\": [[0, 0], [0, 0], [0, 0]]}";
      std::ofstream(root / "pack.json", std::ios::binary | std::ios::trunc) << j << "]}\n";
    }
  };
  {  // a launcher's REMOVE (LoadPack, erase, SavePack) on the old pack: the removed character's slot is retired (a save may hold his row),
     // as the old WriteManifest did from superstars.txt; and the UI's BuiltPackSlots still names the old pack's slots once the library exists
    old_pack(true);
    importer::PackState ol;
    const bool rd = importer::LoadPack(mods, ol, e) && ol.stars.size() == 2;
    if (rd) ol.stars.erase(ol.stars.begin() + 1);
    Say(rd && importer::SavePack(mods, ol, e) && importer::RetiredSlots(mods) == std::vector<int>{398},
        "old pack, not migrated: a REMOVE of 398 retires 398 (retired: " + [&] { std::string s; for (int x : importer::RetiredSlots(mods)) s += " " + std::to_string(x); return s; }() + ")");
    std::vector<int> bs = importer::BuiltPackSlots(mods);
    std::sort(bs.begin(), bs.end());
    Say(bs == std::vector<int>{397, 398}, "old pack, library made by a save: BuiltPackSlots still gives the old pack's slots 397 398");
  }
  {  // review S6: an old pack6\ with NO pack.json (copied in by hand, or pack.json lost) is never taken out by the check before PLAY
    old_pack(false);
    const importer::PlayCheck c = importer::CheckBeforePlay(mods, "", "");
    Say(c.stars == 0 && !c.build, "old pack6\\ without pack.json: CheckBeforePlay says nothing to do (never removes it)" + (c.why.empty() ? std::string() : ": '" + c.why + "'"));
    old_pack(true);
    importer::PackState none;
    importer::SavePack(mods, none, e);  // every superstar removed in the UI: pack.json said so (the library says so now)
    Say(importer::CheckBeforePlay(mods, "", "").build, "old pack6\\ with pack.json and every superstar removed: CheckBeforePlay says build (it comes out)");
  }
  fs::remove_all(root, ec);
}

// ---------------------------------------------------------------- the Session 101 cases on the library + frame
// (S110 LIBSIZE-2 review) a card as the game binds it to his seat: every card is made once for the reference identity (497 / 9997) and the game re-targets it with the
// frame's bank and rank for that seat (library_cases.h SeatForm: RetargetCard, the model names, the EVP names and blobs)
static Lib BoundLib(const std::string& mods, const std::string& key, int slot) {
  Lib l = ReadLib(mods, key);
  const auto fr = wwe2k14::superstars_fit::ReadBuilt(Root(mods) / "frame" / "superstars.txt");
  if (!l.ok || !fr.bank.count(slot) || !fr.rank.count(slot)) return l.ok = false, l;
  return SeatForm(l, slot, fr.bank.at(slot), fr.rank.at(slot), fr.pack_id);
}
static void FitCasesLib(const Env& env, const std::string& stfs_dir) {
  std::printf("fit cases on the library + frame:\n");
  const std::string& dlc = env.dlc;
  bool same = true;
  for (int i = 0; i < 5; ++i) same = same && wwe2k14::dlc::kGateWords[i] == io::kGateTable[i];
  Say(same, "the game's gate table is the same in dlc_catalog.h (the game, the launchers) and pack_io.h (the builder)");
  std::error_code ec;
  auto make = [&](const std::string& cs, int n, std::vector<int> slots = {}) {
    std::string pac;
    const std::string m = FreshMods(env, "fit_" + cs, &pac);
    importer::PackState st;
    const std::vector<int> free = importer::FreeSlots(env.gf, dlc);
    for (int k = 0; k < n; ++k) {
      importer::Superstar s = MakeStar(pac, k < int(slots.size()) ? slots[size_t(k)] : free[size_t(k)], "Fit Test " + std::to_string(k + 1), 100, -1);
      s.short_name = "FIT " + std::to_string(k + 1);
      st.stars.push_back(s);
    }
    return std::make_pair(m, st);
  };
  auto problem = [&](const std::string& m, const std::string& d, const std::string& off) { return importer::PackProblem(m, d, off); };
  auto ftxt = [&](const std::string& m) { return TextOf(Root(m) / "frame" / "superstars.txt"); };
  auto fwrite = [&](const std::string& m, const std::string& t) { std::ofstream(Root(m) / "frame" / "superstars.txt", std::ios::binary | std::ios::trunc) << t; };

  // A. three characters on the whole DLC
  auto [ma, sa] = make("multi", 3);
  if (Say(Build(env, ma, sa, dlc, ""), "A: BuildPack, 3 characters, all DLC on")) {
    const fs::path out = Root(ma) / "frame";
    io::CatInfo info;
    std::vector<uint32_t> gate(std::begin(io::kGateTable), std::end(io::kGateTable));
    gate[0] = 6u << 16 | 1;
    const std::string why = io::ParseCatalogLikeGame(Read(out / "info" / "catalog.dlc"), gate, info);
    std::set<int> brows;
    for (const auto& bd : info.bundles)
      for (const auto& r : bd.rows) brows.insert(r.ch);
    const std::vector<int> seats = importer::FreeSlots(env.gf, dlc);
    Say(why.empty() && info.entries.size() == 1 && info.entries[0].type == 21 && info.entries[0].id == 13 && info.entries[0].char_id == -1 && info.entries[0].attr1 == -1 &&
            brows == std::set<int>(seats.begin(), seats.end()),
        "A: catalog: one type-21 entry id " + std::to_string(info.entries.empty() ? -1 : info.entries[0].id) + " with a bundle row for each of the " + std::to_string(seats.size()) + " seats");
    bool every = true;
    for (int s : seats) every = every && importer::pack_entry::EntryIdFor(info, s, 0) == 13;
    Say(every, "A: EntryIdFor answers the entry's id for every seat");
    int rows = 0;
    std::set<int> ranks;
    importer::PackState back;
    std::string e;
    importer::LoadPack(ma, back, e);
    for (const auto& s : back.stars) {
      const Lib l = BoundLib(ma, s.key, s.slot);
      if (l.ok && l.rows.dat[4 + 16] == 1 && l.rows.dat[4 + 17] == 6 && io::L16(&l.rows.dat[4 + 6]) == 60000 + s.slot) ++rows, ranks.insert(io::L16(&l.rows.dat[4 + 2]));
    }
    Say(rows == 3 && ranks.size() == 3, "A: rows.bin: 3 active DAT rows, pack code 6, own name texts, 3 different ranks");
    Say(std::find_if(sa.stars.begin(), sa.stars.end(), [](auto& s) { return s.slot == 399; }) == sa.stars.end(), "A: seat 399 never given out");
    const std::string top = LineOf(ftxt(ma), "top");
    Say(top.rfind("top 5 ", 0) == 0 && top.size() == 14, "A: the frame's superstars.txt records the top pack and its copy: '" + top + "'");
    Say(problem(ma, dlc, "").empty(), "A: fits all DLC on");
    Say(problem(ma, dlc, "2").empty(), "A: fits with the catalog-only pack 2 switched off");
    Say(problem(ma, dlc, "3").empty(), "A: fits with pack 3 switched off (not built on it; the top is still 5)");
    const std::string p5 = problem(ma, dlc, "5");
    Say(!p5.empty(), "A: pack 5 switched off -> left out: '" + p5 + "'");
    const std::string nd = problem(ma, "", "");
    Say(!nd.empty(), "A: USE MY DLC off -> left out: '" + nd + "'");
    const importer::PlayCheck c0 = importer::CheckBeforePlay(ma, dlc, ""), c5 = importer::CheckBeforePlay(ma, dlc, "5");
    Say(c0.stars == 3 && !c0.build && c5.build, "A: CheckBeforePlay: nothing to do all on; build with pack 5 off ('" + c5.why + "')");
    std::ofstream(Root(ma) / "game_status.txt") << "out test reason\n";
    Say(!importer::LastGameProblem(ma).empty(), "A: the game's 'out' verdict reaches the launcher: '" + importer::LastGameProblem(ma) + "'");
    {  // D. an older frame / pack file (no top line): still judged by built_on
      const std::string old = ftxt(ma);
      std::string kept;
      std::istringstream in(old);
      for (std::string l; std::getline(in, l);)
        if (l.rfind("top ", 0) != 0) kept += l + "\n";
      fwrite(ma, kept);
      Say(problem(ma, dlc, "").empty() && !problem(ma, dlc, "5").empty() && !problem(ma, "", "").empty() && problem(ma, dlc, "3").empty(),
          "D: a superstars.txt with no top line: fits all on and with pack 3 off, left out with pack 5 off or no DLC");
      fwrite(ma, "pack 6\nbuilt_on 4\nstar 397 music 100\n");
      Say(!problem(ma, dlc, "").empty() && problem(ma, dlc, "5").empty(), "D: a file built on pack 4: out with pack 5 on ('" + problem(ma, dlc, "") + "'), fits with pack 5 off");
      fwrite(ma, kept + "top 5 12345678\n");
      const std::string cp = problem(ma, dlc, "");
      Say(!cp.empty() && cp.find("copy") != std::string::npos, "E: a different copy of pack 5 -> left out: '" + cp + "'");
      fwrite(ma, old);
    }
    Say(wwe2k14::superstars_fit::GamePacks(Root(ma), "").empty(), "I: our own pack (id 6) is never one of the game's real packs (the gate), wherever it sits");
    // a changed frame digest (a pack added since) makes every card instantiate again, and a rebuild clears the stale verdict
    Say(Build(env, ma, sa, dlc, "") && importer::LastGameProblem(ma).empty(), "A: a rebuild removes the old game verdict");
  }
  // H. (S110 SEAT-A review: ported) the top pack as an Xbox 360 package (--stfs-dir DIR: a folder holding ONE package made from the top pack,
  //    tools\dlc\make_test_stfs.py). Before Session 101 packages were skipped, so the pack was built on the disc and the game, which mounts
  //    packages, left it out. Same three characters as A: the frame's cumulative copies, its top line and each card's rows must match A's.
  if (!stfs_dir.empty()) {
    std::vector<int> slots;
    for (auto& s : sa.stars) slots.push_back(s.slot);
    auto [mh, sh] = make("stfs", 3, slots);
    const bool ok = Build(env, mh, sh, stfs_dir, "");
    Say(ok, "H: BuildPack on a DLC folder holding the top pack as an Xbox 360 package");
    if (ok) {
      const std::string ta = LineOf(ftxt(ma), "top"), th = LineOf(ftxt(mh), "top");
      Say(!th.empty() && th == ta, "H: the frame is built on the package: '" + th + "' (the folder build: '" + ta + "')");
      for (const char* f : {"pac/ch/chEtc_dlc06.pac", "pac/string_dlc06.pac", "pac/DLC_dlc06.pac", "pac/sound_dlc06.pac", "pac/audience_dlc06.pac", "pac/ExclusiveAI_dlc06.pac"}) {
        const Bytes x = Read(Root(ma) / "frame" / f), y = Read(Root(mh) / "frame" / f);
        Say(!x.empty() && x == y, std::string("H: frame\\") + f + " identical to the folder build's");
      }
      importer::PackState la, lh;
      std::string e;
      importer::LoadPack(ma, la, e), importer::LoadPack(mh, lh, e);
      bool rows_same = la.stars.size() == 3 && lh.stars.size() == 3;
      for (size_t i = 0; rows_same && i < 3; ++i) {
        Lib a = ReadLib(ma, la.stars[i].key), h = ReadLib(mh, lh.stars[i].key);
        rows_same = a.ok && h.ok && a.rows.dsbd == h.rows.dsbd && a.rows.pro == h.rows.pro && a.rows.bin5 == h.rows.bin5 && a.rows.names[0] == h.rows.names[0];
        Bytes da(a.rows.dat.begin(), a.rows.dat.end()), dh(h.rows.dat.begin(), h.rows.dat.end());
        da[6] = da[7] = dh[6] = dh[7] = 0;  // (the rank follows the frame's seat list, which is the DLC folder's)
        rows_same = rows_same && da == dh;
      }
      Say(rows_same, "H: the three cards' rows (DAT but the rank, DSBD, PRO, BIN5, names) equal the folder build's");
      Say(problem(mh, stfs_dir, "").empty(), "H: fits the package");
    }
  } else {
    std::printf("  (H not run: no --stfs-dir)\n");
  }
  // B. built while pack 5 is switched off
  auto [mb, sb] = make("off5", 1);
  if (Say(Build(env, mb, sb, dlc, "5"), "B: BuildPack with pack 5 switched off")) {
    const std::string t = ftxt(mb);
    Say(LineOf(t, "top").rfind("top 4 ", 0) == 0 && LineOf(t, "built_on").find('5') == std::string::npos,
        "B: built on pack 4 (the top the game gets): '" + LineOf(t, "built_on") + "' / '" + LineOf(t, "top") + "'");
    Say(problem(mb, dlc, "5").empty(), "B: fits with pack 5 off");
    Say(!problem(mb, dlc, "").empty(), "B: pack 5 switched back on -> rebuild: '" + problem(mb, dlc, "") + "'");
    // the frame is rebuilt for the new top, and the card is made again for it (the old frame's digest no longer fits)
    importer::PackState back;
    std::string e;
    importer::LoadPack(mb, back, e);
    const std::string key = back.stars[0].key;
    Say(Build(env, mb, back, dlc, "") && LineOf(ftxt(mb), "top").rfind("top 5 ", 0) == 0 && ReadLib(mb, key).ok && problem(mb, dlc, "").empty(),
        "B: built again with pack 5 on: the frame follows (top 5) and the card was made again for it");
  }
  // C. no DLC at all (USE MY DLC off): built on the disc
  auto [mc, sc2] = make("nodlc", 1);
  if (Say(Build(env, mc, sc2, "", ""), "C: BuildPack with USE MY DLC off")) {
    Say(LineOf(ftxt(mc), "top") == "top 0 00000000" && LineOf(ftxt(mc), "built_on") == "built_on", "C: built on the disc: top 0, built_on empty");
    Say(problem(mc, "", "").empty(), "C: fits with no DLC");
    Say(importer::CheckBeforePlay(mc, dlc, "").build, "C: DLC switched on later -> CheckBeforePlay says build: '" + importer::CheckBeforePlay(mc, dlc, "").why + "'");
  }
  // F. a character on a seat that is not one (399, pack 4's; or a pack added since) is moved, not refused; over the seats: stored without one
  {
    const std::vector<int> nodlc = importer::FreeSlots(env.gf, "");
    Say(std::find(nodlc.begin(), nodlc.end(), 399) == nodlc.end(), "F: FreeSlots leaves 399 out even with no DLC");
    auto [mf, sf] = make("slot399", 2, {399, 397});
    const bool ok = Build(env, mf, sf, dlc, "");
    importer::PackState back;
    std::string e;
    importer::LoadPack(mf, back, e);
    bool moved = false;
    for (const auto& l : importer::LastBuildLog()) moved = moved || l.find("moved to") != std::string::npos;
    Say(ok && back.stars.size() == 2 && back.stars[0].slot != 399 && back.stars[0].slot >= 397 && back.stars[0].slot != back.stars[1].slot && moved,
        "F: seat 399 moved to " + std::to_string(back.stars.empty() ? -1 : back.stars[0].slot) + "; the card follows");
    const std::vector<std::string> said = importer::LastBuildMoves();
    Say(said.size() == 1 && said[0].find("moved from slot 399") != std::string::npos && said[0].find("saves") != std::string::npos, "F: the move is said to the player: '" + (said.empty() ? std::string() : said[0]) + "'");
    {  // (S110 SEAT-A review) no pack in the DLC folder uses 399: a character already on it STAYS (as the old build kept him); a second one on
       // 399 moves to another seat, never 399
      auto [m9, s9] = make("slot399_kept", 3, {399, 397, 399});
      const bool ok9 = Build(env, m9, s9, "", "");
      importer::PackState b9;
      importer::LoadPack(m9, b9, e);
      int on399 = 0;
      std::set<int> got9;
      for (const auto& s : b9.stars) on399 += s.slot == 399, got9.insert(s.slot);
      const auto fr9 = wwe2k14::superstars_fit::ReadBuilt(Root(m9) / "frame" / "superstars.txt");
      Say(ok9 && on399 == 1 && got9.size() == 3 && !got9.count(-1) && std::find(fr9.seats.begin(), fr9.seats.end(), 399) != fr9.seats.end() && !b9.needs_build,
          "F (review): no pack uses 399: the character on it stays (seat 399 in the frame), the second one on 399 moves to another seat");
      const std::vector<std::string> said9 = importer::LastBuildMoves();
      Say(said9.size() == 1 && said9[0].find("moved from slot 399") != std::string::npos, "F (review): only the second one is moved and told");
    }
    auto [mf3, sf3] = make("slot_twice", 3, {397, 397, 398});
    const bool ok3 = Build(env, mf3, sf3, dlc, "");
    importer::PackState back3;
    importer::LoadPack(mf3, back3, e);
    std::set<int> got3;
    for (const auto& s : back3.stars) got3.insert(s.slot);
    Say(ok3 && back3.stars.size() == 3 && got3.size() == 3 && got3.count(397) && got3.count(398) && !got3.count(399), "F: a seat used twice: the second character moves to a seat nobody keeps");
    // over the seats: seats + 3 characters (no files per card needed: they share one source)
    const std::vector<int> seats = importer::FreeSlots(env.gf, dlc);
    std::string pac;
    const std::string mo = FreshMods(env, "fit_over", &pac);
    importer::PackState over;
    for (size_t k = 0; k < seats.size() + 3; ++k) over.stars.push_back(MakeStar(pac, k < seats.size() ? int(seats[k]) : -1, "Over " + std::to_string(k + 1), 100, -1));
    const bool oko = Build(env, mo, over, dlc, "");
    importer::PackState bo;
    importer::LoadPack(mo, bo, e);
    int none = 0;
    for (const auto& s : bo.stars) none += s.slot < 0;
    Say(oko && bo.stars.size() == seats.size() + 3 && none == 3 && importer::LastBuildMoves().empty(), "F: " + std::to_string(seats.size() + 3) + " characters on " + std::to_string(seats.size()) + " seats: built, 3 stored without a seat, no error");
    {  // (S110 SEAT-A review, K) an UNSEATED card whose base wrestler comes with DLC the game does not get never blocks a build
      for (auto& s : bo.stars)
        if (s.slot < 0) {
          s.base_id = 385;  // Fandango: pack 4
          break;
        }
      std::string w;
      const bool ok_u = importer::SavePack(mo, bo, e) && importer::BuildPack(mo, env.gf, dlc, bo, nullptr, w, "4");
      Say(ok_u, "K (review): an unseated card with a base from switched-off DLC does not block the build" + (w.empty() ? std::string() : ": '" + w + "'"));
    }
    fs::remove_all(fs::u8path(mo), ec);
  }
  // L. one build at a time across processes
  {
    auto [ml, sl] = make("lock", 1);
    const fs::path lock = Root(ml) / "build.lock";
    fs::create_directories(lock.parent_path(), ec);
    std::ofstream(lock) << "another builder\n";
    std::string e;
    const bool busy = importer::BuildPack(ml, env.gf, dlc, sl, nullptr, e, "");
    Say(!busy && e.find("another build") != std::string::npos && fs::exists(lock), "L: a running build's lock refuses a second one: '" + e + "'");
    fs::last_write_time(lock, fs::file_time_type::clock::now() - std::chrono::minutes(30), ec);
    Say(Build(env, ml, sl, dlc, "") && !fs::exists(lock), "L: a stale lock (30 min) is taken over; the lock is gone after the build");
  }
  // M. a mods folder with no list (nothing saved, no pack): nothing to do, nothing removed
  {
    auto [mm, sm] = make("nolist", 1);
    Build(env, mm, sm, dlc, "");
    fs::remove_all(Root(mm) / "library", ec);  // (the cards are gone, the frame stays: a frame with no list is not removed by the check before PLAY? it is: see J)
    const importer::PlayCheck c = importer::CheckBeforePlay(mm, dlc, "");
    Say(c.stars == 0 && c.build, "M: the library gone, a frame left: the check says build (the frame comes out): '" + c.why + "'");
    const std::string empty = FreshMods(env, "fit_nothing", nullptr);
    const importer::PlayCheck c2 = importer::CheckBeforePlay(empty, dlc, "");
    Say(c2.stars == 0 && !c2.build, "M: a mods folder with no library and no pack: nothing to do");
  }
  // G. the mods folder moved: the card's recorded source path is gone, the copy in superstars\sources is used
  {
    auto [mg, sg] = make("moved", 1);
    importer::PackState st = sg;
    const std::string name = fs::u8path(st.stars[0].source_pac).filename().string();
    st.stars[0].source_pac = "C:\\gone\\old release\\mods\\superstars\\sources\\" + name;
    std::string e;
    importer::SavePack(mg, st, e);
    importer::PackState back;
    importer::LoadPack(mg, back, e);
    Say(!back.stars.empty() && fs::exists(fs::u8path(back.stars[0].source_pac)), "G: LoadPack finds the source in this mods folder: " + (back.stars.empty() ? std::string() : back.stars[0].source_pac));
    Say(Build(env, mg, st, dlc, ""), "G: BuildPack from the stale path works");
  }
  // J. every superstar removed: the check says build, the build takes the frame out
  {
    auto [mj, sj] = make("empty", 1);
    Build(env, mj, sj, dlc, "");
    importer::PackState none;
    std::string e;
    importer::SavePack(mj, none, e);
    const importer::PlayCheck c = importer::CheckBeforePlay(mj, dlc, "");
    Say(c.build, "J: every superstar removed, frame still there -> CheckBeforePlay says build: '" + c.why + "'");
    const bool ok = Build(env, mj, none, dlc, "");
    Say(ok && !fs::exists(Root(mj) / "frame") && !importer::CheckBeforePlay(mj, dlc, "").build, "J: building the empty list takes the frame out (nothing left to do)");
  }
  // K. a base wrestler from DLC the game does not get
  {
    auto [mk, sk] = make("dlcbase", 2);
    sk.stars[0].base_id = 385;  // Fandango: pack 4 ("DLC Pack 3")
    std::string e;
    // (S110 SEAT-A review) a SEATED character whose base comes with switched-off DLC is not made (held, his seat kept) and said; the others are made
    auto card_of = [&](const std::string& m, int i, sc::Card& c) {
      importer::PackState x;
      std::string ce;
      importer::LoadPack(m, x, ce);
      return i < int(x.stars.size()) && sc::ReadCard(Slash(Root(m) / "library" / x.stars[size_t(i)].key / "card.txt"), c, ce);
    };
    const bool off4 = importer::BuildPack(mk, env.gf, dlc, sk, nullptr, e, "4");
    {
      sc::Card c0, c1;
      const bool r = card_of(mk, 0, c0) && card_of(mk, 1, c1);
      const std::vector<std::string> said = importer::LastBuildMoves();
      Say(off4 && r && !c0.inst.valid && c0.seat >= 397 && c1.inst.valid && said.size() == 1 && said[0].find("comes with DLC") != std::string::npos,
          "K (review): base 385 with pack 4 switched off: the build goes on, he keeps seat " + std::to_string(c0.seat) + " not made ('" + (said.empty() ? e : said[0]) +
              "'), the other is made");
    }
    Say(Build(env, mk, sk, dlc, ""), "K: base 385 with every pack on builds");
    // a launcher check: the card records the pack its base came from; switching it off asks for a build
    const importer::PlayCheck c = importer::CheckBeforePlay(mk, dlc, "4");
    Say(c.build, "K: pack 4 switched off after the build -> CheckBeforePlay says build: '" + c.why + "'");
    {  // ... and that build withdraws his instance (the game holds him rather than seat him without pack 4); pack 4 back on makes him again
      importer::PackState x;
      importer::LoadPack(mk, x, e);
      sc::Card c0;
      const bool b4 = importer::BuildPack(mk, env.gf, dlc, x, nullptr, e, "4") && card_of(mk, 0, c0) && !c0.inst.valid;
      Say(b4 && Build(env, mk, x, dlc, "") && card_of(mk, 0, c0) && c0.inst.valid && !importer::CheckBeforePlay(mk, dlc, "").build,
          "K (review): built with pack 4 off: his instance is withdrawn (held); pack 4 back on: made again, nothing left to do");
    }
    sk.stars[0].base_id = 254;
    e.clear();
    const bool warrior = importer::BuildPack(mk, env.gf, dlc, sk, nullptr, e, "");
    Say(warrior || e.find("comes with DLC") == std::string::npos, "K: base 254 (pack 2's catalog entry) is not refused as missing DLC" + (warrior ? std::string(" (built)") : " (other error: " + e + ")"));
  }
  // O. the tron choice
  {
    auto refusal = [&](const std::string& cs, int tron_from, int tron_slot) {
      auto [m, s] = make(cs, 1, {397});
      s.stars[0].tron_from = tron_from, s.stars[0].tron_slot = tron_slot;
      std::string e;
      const bool ok = importer::BuildPack(m, env.gf, dlc, s, nullptr, e, "");
      return ok ? std::string() : e.empty() ? std::string("(refused, no message)") : e;
    };
    for (int bad : {822, 844, 900, 909, 950, 396, 845 + 200}) {
      const std::string why = refusal("tronbad" + std::to_string(bad), 0, bad);
      Say(why.find("custom tron " + std::to_string(bad)) != std::string::npos, "O: twin: custom tron " + std::to_string(bad) + " (outside the band) is refused: '" + why + "'");
    }
    for (int bad : {99, 397, 500}) {
      const std::string why = refusal("tronfrom" + std::to_string(bad), bad, 0);
      Say(why.find("whose tron he uses") != std::string::npos, "O: twin: tron wrestler " + std::to_string(bad) + " (not a roster wrestler) is refused: '" + why + "'");
    }
    auto word = [&](const std::string& m, int slot, size_t off) {
      importer::PackState b;
      std::string e;
      importer::LoadPack(m, b, e);
      for (const auto& s : b.stars)
        if (s.slot == slot) {
          const Lib l = ReadLib(m, s.key);
          return l.ok ? int(io::L16(&l.rows.pro[off])) : -1;
        }
      return -1;
    };
    {  // "none" is any value <= 0
      auto [mn1, sn1] = make("tronnone", 1, {397});
      sn1.stars[0].tron_from = -1, sn1.stars[0].tron_slot = -1;
      const bool ok = Build(env, mn1, sn1, dlc, "");
      const Bytes ce = Read(Root(mn1) / "frame" / "pac" / "ch" / "chEtc_dlc06.pac");
      Say(ok && word(mn1, 397, 578) == ProWord(ce, 100, 578) && word(mn1, 397, 594) == ProWord(ce, 100, 594), "O: tron_from -1 / tron_slot -1 (= none) build the default: PRO +578 / +594 = his base 100's");
    }
    auto [mt, st1] = make("tronwas", 1, {397});
    const fs::path chetc = Root(mt) / "frame" / "pac" / "ch" / "chEtc_dlc06.pac";
    st1.stars[0].tron_slot = 845;
    if (Say(Build(env, mt, st1, dlc, ""), "O: BuildPack, custom tron 845")) {
      importer::PackState b1;
      std::string e;
      importer::LoadPack(mt, b1, e);
      sc::Card c1;
      sc::ReadCard(Slash(Root(mt) / "library" / b1.stars[0].key / "card.txt"), c1, e);
      Say(c1.inst.movie == 845 && c1.tron_slot == 845 && word(mt, 397, 578) == 845 && word(mt, 397, 594) == 845, "O: the card says `inst .. movie 845` and `tron from 0 slot 845`; PRO +578 / +594 = 845");
      const uint64_t hf = HashTree(Root(mt) / "frame");
      st1.stars[0].tron_slot = 846;
      st1.stars[0].key = b1.stars[0].key;
      Say(importer::SavePack(mt, st1, e) && importer::LoadPack(mt, b1, e) && b1.needs_build, "O: a changed tron choice makes the card 'not instantiated' (nothing built yet: needs_build)");
      if (Say(Build(env, mt, st1, dlc, ""), "O: rebuild with custom tron 846")) {
        sc::ReadCard(Slash(Root(mt) / "library" / b1.stars[0].key / "card.txt"), c1, e);
        Say(c1.inst.movie == 846 && word(mt, 397, 578) == 846 && HashTree(Root(mt) / "frame") == hf, "O: the rebuild writes `movie 846` into his card and rows only; the frame is byte-identical");
        st1.stars[0].tron_slot = 0, st1.stars[0].tron_from = 107;
        if (Say(Build(env, mt, st1, dlc, ""), "O: rebuild with the tron of wrestler 107")) {
          const int w107 = ProWord(Read(chetc), 107, 578);
          importer::PackState back;
          Say(word(mt, 397, 578) == w107 && importer::LoadPack(mt, back, e) && back.stars.size() == 1 && back.stars[0].tron_from == 107 && back.stars[0].tron_slot == 0,
              "O: the tron of 107 (movie word " + std::to_string(w107) + ") is in his rows; the card round trip keeps tron_from 107 / tron_slot 0");
        }
      }
      // a tron wrestler with no PRO row: the first id of the roster range the chEtc has no row for
      int gap = -1;
      {
        io::Pac pc;
        std::string er;
        std::vector<io::Leaf> pro;
        io::Entry* pe = nullptr;
        std::set<int> have;
        if (io::ReadPac(Read(chetc), pc, er) && (pe = pc.Find("CHAR", "PRO ")) && io::ReadPach(pe->blob, pro))
          for (const io::Leaf& l : pro) have.insert(int(l.id));
        for (int id = importer::kMinBase; id <= importer::kMaxBase && gap < 0; ++id)
          if (!have.empty() && !have.count(id)) gap = id;
      }
      if (gap > 0) {
        st1.stars[0].tron_slot = 0, st1.stars[0].tron_from = gap;
        std::string e2;
        const bool ok = importer::BuildPack(mt, env.gf, dlc, st1, nullptr, e2, "");
        Say(!ok && e2.find("no profile row") != std::string::npos, "O: twin: tron wrestler " + std::to_string(gap) + " (no PRO row) is refused: '" + e2 + "'");
      } else {
        Say(true, "O: (every roster id 100..396 has a PRO row here: the no-row twin is not applicable)");
      }
    }
  }
  // R. a REBUILD of the frame keeps a seat's recorded bank (the frame is stale: its top copy changed)
  {
    auto [mr, sr] = make("rebank", 2, {397, 398});
    if (Say(Build(env, mr, sr, dlc, ""), "R: BuildPack, 2 characters (397, 398)")) {
      const auto b1 = wwe2k14::superstars_fit::ReadBuilt(Root(mr) / "frame" / "superstars.txt");
      const std::vector<int> fl = importer::asset_ids::Config().free_numbers;
      const int y = fl.size() > 200 ? fl[200] : -1;
      std::string t = ftxt(mr);
      const std::string from = "seat 398 bank " + std::to_string(b1.bank.count(398) ? b1.bank.at(398) : -1);
      const size_t at = t.find(from);
      if (at != std::string::npos && y > 0) t.replace(at, from.size(), "seat 398 bank " + std::to_string(y));
      const size_t tp = t.find("top 5 ");
      if (tp != std::string::npos) t.replace(tp, 6 + 8, "top 5 00000001");  // (a copy of the top the file was not built on: the frame is stale)
      fwrite(mr, t);
      importer::PackState back;
      std::string e;
      importer::LoadPack(mr, back, e);
      const bool rebuilt = at != std::string::npos && y > 0 && Build(env, mr, back, dlc, "");
      const auto b2 = wwe2k14::superstars_fit::ReadBuilt(Root(mr) / "frame" / "superstars.txt");
      importer::LoadPack(mr, back, e);
      bool has_y = false;
      for (const auto& s : back.stars)
        if (s.slot == 398) {
          const Lib l = BoundLib(mr, s.key, s.slot);
          const io::Dir* d = l.ok ? DirOf(l.pac, "EVP ") : nullptr;
          if (d)
            for (const io::Entry& x : d->ents) has_y = has_y || io::Trim(x.name) == std::to_string(y);
        }
      Say(rebuilt && b2.bank.count(398) && b2.bank.at(398) == y && b2.bank.count(397) && b2.bank.at(397) == b1.bank.at(397) && has_y && !b2.was.count(398),
          "R: a rebuilt frame keeps the recorded banks (398 keeps " + std::to_string(y) + ", 397 keeps " + std::to_string(b1.bank.count(397) ? b1.bank.at(397) : -1) + "; his pac names " + std::to_string(y) + ")");
    }
  }
  // N. entrance cases: the base / entrance / seats the old N case ran (rows against the oracle are LibraryCases'; here each builds and its rows hold its bank)
  struct NCase {
    int base;
    std::vector<int> slots;
    int ent;
  };
  const std::vector<NCase> ncases = {{100, {397}, -1}, {385, {397}, -1}, {100, {397, 398}, -1}, {107, {397}, -1}, {100, {397}, 107}, {346, {397}, -1}, {100, {410, 413, 452, 499}, -1}};
  for (const auto& [nbase, nslots, nent] : ncases) {
    std::string who;
    for (int s : nslots) who += " " + std::to_string(s);
    auto [mn, sn] = make("n" + std::to_string(nbase) + "_" + std::to_string(nslots.size()) + "_" + std::to_string(nent) + "_" + std::to_string(nslots[0]), int(nslots.size()), nslots);
    for (auto& s : sn.stars) s.base_id = nbase, s.entrance_from = nent;
    if (!Say(Build(env, mn, sn, dlc, ""), "N: base " + std::to_string(nbase) + " entrance " + std::to_string(nent) + ", seats" + who + " build")) continue;
    const auto fr = wwe2k14::superstars_fit::ReadBuilt(Root(mn) / "frame" / "superstars.txt");
    importer::PackState back;
    std::string e;
    importer::LoadPack(mn, back, e);
    bool ok = back.stars.size() == nslots.size();
    std::string why;
    for (const auto& s : back.stars) {
      const Lib l = BoundLib(mn, s.key, s.slot);
      const int bank = fr.bank.count(s.slot) ? fr.bank.at(s.slot) : -1;
      if (!l.ok || bank <= 0 || io::L16(&l.rows.pro[580]) != bank || io::L16(&l.rows.pro[596]) != bank || int(io::L16(&l.rows.bin5[0])) != s.slot) {
        ok = false, why += " " + std::to_string(s.slot) + ":rows";
        continue;
      }
      const io::Dir* d = DirOf(l.pac, "EVP ");
      bool has = false;
      for (const io::Entry& x : d ? d->ents : std::vector<io::Entry>()) {
        const int n = std::atoi(io::Trim(x.name).c_str());
        has = has || n == bank;
        if (n == s.slot && s.slot != bank && n != 6000 + s.slot) ok = false, why += " " + std::to_string(s.slot) + ":an EVP is named by the slot";
        const Bytes ev = io::Decode(x.blob);
        if (n < 6000 && (ev.size() < 0x44 || io::B16(&ev[0x20]) != n)) ok = false, why += " " + std::to_string(s.slot) + ":header";
      }
      if (!has) ok = false, why += " " + std::to_string(s.slot) + ":no bank pac entry";
    }
    Say(ok, "N: base " + std::to_string(nbase) + " entrance " + std::to_string(nent) + ", seats" + who + ": each card's PRO / BIN5 rows and EVP names carry his bank, none a slot number" + why);
  }
  std::printf("fit cases done\n");
}

// ---------------------------------------------------------------- the default flow: the source into one or more characters, then read back
static void BuildFlow(const Env& env, const std::string& name, const std::string& short_name, int slot, int base, int ent, int count, int tron_from, int tron_slot, bool render) {
  const importer::SourceInfo one = importer::InspectSource(env.source);
  Say(one.ok, "InspectSource: claimed " + std::to_string(one.claimed_id));
  std::string e;
  const std::string pac = importer::ExtractSource(env.source, one.entry_name, env.mods_root, &e);
  Say(!pac.empty(), "ExtractSource -> " + pac + e);
  const auto free = importer::FreeSlots(env.gf, env.dlc);
  Say(!free.empty() && free.front() == 397 && std::find(free.begin(), free.end(), 399) == free.end(), "FreeSlots: " + std::to_string(free.size()) + " free, first " + std::to_string(free.empty() ? -1 : free.front()) + ", 399 excluded");
  importer::PackState st;
  importer::Superstar s = MakeStar(pac, slot, name, base, ent);
  s.short_name = short_name, s.tron_slot = tron_slot, s.tron_from = tron_from;
  if (render) {  // test pictures: SSFA red, SSFB green, SSFC blue
    for (int k = 0; k < 3; ++k) {
      s.render_w[k] = s.render_h[k] = 300;
      s.render_rgba[k].resize(300 * 300 * 4);
      for (int y = 0; y < 300; ++y)
        for (int x = 0; x < 300; ++x) {
          uint8_t* p = &s.render_rgba[k][(size_t(y) * 300 + x) * 4];
          const bool in = (x - 150) * (x - 150) + (y - 120) * (y - 120) < 90 * 90;
          const uint8_t g = uint8_t(60 + y * 195 / 300);
          p[0] = k == 0 ? g : 30, p[1] = k == 1 ? g : 30, p[2] = k == 2 ? g : 30, p[3] = in ? 255 : 0;
        }
    }
  }
  st.stars.push_back(s);
  for (int k = 1; k < count && k < int(free.size()); ++k) {  // bulk: the same model on the next free seats
    importer::Superstar c = s;
    c.slot = free[size_t(k)] == slot ? free[0] : free[size_t(k)];
    c.name = name + " " + std::to_string(k + 1);
    st.stars.push_back(c);
  }
  std::printf("BuildPack:\n");
  const auto t0 = Clock::now();
  const bool built = importer::BuildPack(env.mods_root, env.gf, env.dlc, st, [](float f, const char* w) { std::printf("    %3d%% %s\n", int(f * 100), w); }, e);
  for (const auto& l : importer::LastBuildLog()) std::printf("  | %s\n", l.c_str());
  Say(built, "BuildPack (" + std::to_string(Secs(t0)).substr(0, 5) + " s)" + (built ? std::string() : ": " + e));
  if (!built) return;
  importer::PackState back;
  Say(importer::LoadPack(env.mods_root, back, e) && back.stars.size() == st.stars.size() && back.stars[0].name == name && back.stars[0].slot == slot && !back.needs_build &&
          back.stars[0].render_rgba[0] == st.stars[0].render_rgba[0] && back.stars[0].tron_slot == tron_slot && back.stars[0].tron_from == tron_from,
      "LoadPack: " + std::to_string(back.stars.size()) + " card(s), name, seat, portraits (PNG round trip exact), nothing left to build");
  for (const auto& b : back.stars) {
    const Lib l = ReadLib(env.mods_root, b.key);
    std::printf("  card %s: seat %d bank %d src_bank %d movie %d, rows %s\n", b.key.c_str(), b.slot, l.card.inst.bank, l.card.inst.src_bank, l.card.inst.movie, l.ok ? "read back" : l.err.c_str());
  }
}
