// S110 SEAT-C: the screens' sentences and the small importer calls they use (src\importer\screen_text.h, MigrationState, SavePack filling the
// list's keys, PutInList, the library's problem sentences).
// Included by test_core.cpp; runs with every mode (no game files). Scratch under <mods>\screen_text_cases (made and removed here).
#pragma once

#include <fstream>

#include "../../src/importer/screen_text.h"

static void ScreenTextCases(const std::string& mods) {
  namespace sx = importer::screen;
  std::printf("screen text (SEAT-C):\n");
  Say(sx::CountLine(5, 3, 102) == "5 imported, 3 of 102 seated", "count line: " + sx::CountLine(5, 3, 102));
  Say(sx::NotSeatedWhy(102, 102) == "All 102 seats are taken. Kept in your library, and can be seated when a seat is free.",
      "not seated, seats full: " + sx::NotSeatedWhy(102, 102));
  Say(sx::NotSeatedWhy(10, 102).find("BUILD INTO GAME") != std::string::npos && sx::NotSeatedWhy(10, 102).find("taken") == std::string::npos,
      "not seated, a seat is free: " + sx::NotSeatedWhy(10, 102));
  {
    std::vector<importer::Superstar> v(5);
    v[0].slot = -1, v[0].name = "Zed";
    v[1].slot = 420, v[1].name = "B";
    v[2].slot = -1, v[2].name = "alan";
    v[3].slot = 401, v[3].name = "C";
    v[4].slot = -1, v[4].name = "Mid";
    const std::vector<size_t> o = sx::DisplayOrder(v);
    Say(o == std::vector<size_t>({3, 1, 2, 4, 0}), "list order: seated by seat, then not seated by name");
    Say(sx::SeatedCount(v) == 2 && sx::CountLine(v, 102) == "5 imported, 2 of 102 seated", "count of a list: " + sx::CountLine(v, 102));
  }
  // the game's status line: every form the game writes (src\dlc.h WriteStatus calls, src\superstars.h Load), as written there
  {
    const std::string a = sx::GameStatusSentence("in 12");
    const std::string b = sx::GameStatusSentence("in 12, 2 imported superstar(s) need a rebuild: open the importer");
    const std::string b1 = sx::GameStatusSentence("in 12, 1 imported superstar(s) need a rebuild: open the importer");
    const std::string k = sx::GameStatusSentence("in 12, 3 card(s) left out (see the game's log)");
    const std::string c = sx::GameStatusSentence("out 3 imported superstar(s) need converting: start the game from the launcher once");
    const std::string c1 = sx::GameStatusSentence("out 1 imported superstar(s) need converting: start the game from the launcher once");
    const std::string o = sx::GameStatusSentence("out an old pack was found: the launcher migrates it (start the game from the launcher once)");
    const std::string d = sx::GameStatusSentence("out the pack is not built yet (the importer's BUILD INTO GAME, or PLAY from the launcher, builds it)");
    const std::string e = sx::GameStatusSentence("out pack 7 is not offered");
    const std::string u_in = sx::GameStatusSentence("in 12, something a newer game says");  // (unknown forms: nothing untrue)
    const std::string u = sx::GameStatusSentence("maybe 3");
    // (S110 SEAT2 review) the hand-over's clause, exactly as src\slot_handover_core.h WithUnseenClause appends it (kUnseenClause), alone and after dlc.h's
    const std::string un = sx::GameStatusSentence("in 12, 2 imported superstar(s) may show another superstar's saved edits (this save was not seen before)");
    const std::string un1 = sx::GameStatusSentence(
        "in 12, 2 imported superstar(s) need a rebuild: open the importer, 1 imported superstar(s) may show another superstar's saved edits (this save was not "
        "seen before)");
    Say(un.rfind("In your last game 2 imported superstars may have shown the saved move set, entrance or Threads of another superstar", 0) == 0 &&
            un.find("Check the move set and entrance") != std::string::npos,
        "status in+unseen save: " + un);
    Say(un1.rfind("In your last game 2 imported superstars were left out and need a rebuild", 0) == 0 &&
            un1.find(" In your last game 1 imported superstar may have shown") != std::string::npos,
        "status in+rebuild+unseen: both said, in order: " + un1);
    Say(a.empty(), "status 'in N' says nothing");
    Say(b == "In your last game 2 imported superstars were left out and need a rebuild: open the Superstar Importer and BUILD INTO GAME.", "status in+rebuild: " + b);
    Say(b1 == "In your last game 1 imported superstar was left out and needs a rebuild: open the Superstar Importer and BUILD INTO GAME.", "status in+rebuild, one: " + b1);
    Say(k.rfind("In your last game 3 superstars were left out (the game's log says why).", 0) == 0, "status in+left out: " + k);
    Say(c == "In your last game 3 imported superstars were left out: the old pack still needs converting to the new library. Start the game from the "
             "launcher (PLAY) once: the launcher converts the old pack.", "status out+converting: " + c);
    Say(c1.rfind("In your last game 1 imported superstar was left out: the old pack still needs converting", 0) == 0, "status out+converting, one: " + c1);
    Say(o.find("old pack still needs converting") != std::string::npos && o.find("PLAY") != std::string::npos, "status out+old pack: " + o);
    Say(d.find("not built yet") != std::string::npos && d.find("Send a report") == std::string::npos, "status out+not built: " + d);
    Say(e == "In your last game your superstars were left out: pack 7 is not offered. Send a report if it happens again.", "status out+other: " + e);
    Say(u_in.empty() && u.empty(), "status: an unknown 'in' form or an unknown first word says nothing");
    // (SEAT-C review) no jargon and no pronoun for a person (imports are women too): every sentence the screens show from screen_text.h
    auto words = [](const std::string& s) {
      std::vector<std::string> w(1);
      for (char ch : s)
        if (std::isalpha(static_cast<unsigned char>(ch))) w.back() += char(std::tolower(static_cast<unsigned char>(ch)));
        else if (!w.back().empty()) w.emplace_back();
      return w;
    };
    bool clean = true;
    std::string bad;
    for (const std::string& s : {b, b1, k, c, c1, o, d, e, un, un1, sx::NotSeatedWhy(102, 102), sx::NotSeatedWhy(10, 102), sx::MigrationLine(importer::Migration::Pending),
                                 sx::MigrationLine(importer::Migration::Kept)}) {
      for (const char* w : {"card", "frame", "inst", "seat none", "(s)", "it still needs", "converts it", "converts them"})
        if (s.find(w) != std::string::npos) clean = false, bad = s;
      for (const std::string& w : words(s))
        if (w == "he" || w == "him" || w == "his" || w == "she" || w == "her" || w == "hers" || w == "himself" || w == "herself") clean = false, bad = s;
    }
    Say(clean, "screen sentences: no card / frame / inst / seat none / (s), no he / him / his / she / her" + (bad.empty() ? std::string() : ": " + bad));
  }
  Say(sx::ProblemLine("the folder library\\X cannot be read (y)") == "The folder library\\X cannot be read (y).", "problem line: first letter up, one full stop");
  Say(sx::ProblemLine("The folder library\\X was not made by the importer. It was left out; nothing in it was changed.") ==
          "The folder library\\X was not made by the importer. It was left out; nothing in it was changed.",
      "problem line: a sentence that ends itself is kept as it is");
  // MigrationState: read from disk (an old pack in place / legacy.old kept / neither)
  {
    namespace fsx = std::filesystem;
    auto PathU8 = [](const fsx::path& p) { const auto u = p.u8string(); return std::string(u.begin(), u.end()); };
    std::error_code ec;
    const fsx::path m = fsx::u8path(mods) / "screen_text_cases";
    fsx::remove_all(m, ec);
    fsx::create_directories(m / "superstars", ec);
    Say(importer::MigrationState(PathU8(m)) == importer::Migration::None && sx::MigrationLine(importer::Migration::None).empty(), "migration: nothing on disk = None, no line");
    Say(importer::SeatCount(PathU8(m)) == 102, "seat count before a frame is built: 102");
    { std::ofstream(m / "superstars" / "pack.json") << "{}"; }
    Say(importer::MigrationState(PathU8(m)) == importer::Migration::Pending && !sx::MigrationLine(importer::Migration::Pending).empty(), "migration: pack.json = Pending");
    // (SEAT-C review) an interrupted conversion: the old pack AND a library, no frame yet: still Pending (the next build continues it); the
    // library's cards are what LoadPack lists, the build converts the rest: "converted when the pack is built" stays true
    fsx::create_directories(m / "superstars" / "library", ec);
    fsx::create_directories(m / "superstars" / "legacy.old", ec);
    Say(importer::MigrationState(PathU8(m)) == importer::Migration::Pending, "migration: an old pack + a library (+ legacy.old), no frame = Pending");
    fsx::create_directories(m / "superstars" / "frame", ec);  // (a frame in place: migrate::Legacy is false whatever else is there)
    Say(importer::MigrationState(PathU8(m)) == importer::Migration::Kept, "migration: a frame + pack.json + legacy.old = Kept (the old pack is not pending)");
    fsx::remove_all(m / "superstars" / "frame", ec);
    fsx::remove_all(m / "superstars" / "library", ec);
    fsx::remove(m / "superstars" / "pack.json", ec);
    Say(importer::MigrationState(PathU8(m)) == importer::Migration::Kept, "migration: legacy.old and no old pack = Kept");
    Say(sx::MigrationLine(importer::Migration::Kept) ==
            "Your imported superstars were converted to the new library. Nothing was lost; a copy of the old pack is kept until the game has loaded "
            "the new library once (the next build then removes it).",
        "migration line (after): " + sx::MigrationLine(importer::Migration::Kept));
    fsx::remove_all(m / "superstars" / "legacy.old", ec);
    Say(importer::MigrationState(PathU8(m)) == importer::Migration::None, "migration: legacy.old removed (RemoveHolds) = None: the line goes");
    // (SEAT-C review) a character with no seat is found again only by his key (SaveCards' step 3 matches a keyless entry by its seat): SavePack
    // writes each key into the caller's list itself, so a list saved twice keeps the same folders (no caller has to copy keys back)
    fsx::remove_all(m / "superstars" / "legacy.old", ec);
    importer::PackState st;
    for (const char* n : {"Aa", "Bb"}) {
      importer::Superstar s;
      s.name = s.short_name = n, s.slot = -1, s.base_id = 100, s.source_pac = PathU8(m / "none.pac");
      st.stars.push_back(s);
    }
    auto folders = [&] {
      std::vector<std::string> v;
      for (const auto& e : fsx::directory_iterator(m / "superstars" / "library", ec))
        if (e.is_directory(ec)) v.push_back(PathU8(e.path().filename()));
      std::sort(v.begin(), v.end());
      return v;
    };
    std::string err;
    const bool ok1 = importer::SavePack(PathU8(m), st, err);
    const std::vector<std::string> f1 = folders();
    const bool ok2 = ok1 && importer::SavePack(PathU8(m), st, err);
    const std::vector<std::string> f2 = folders();
    Say(ok2 && f1.size() == 2 && f2 == f1 && st.stars[0].key == f1[0] && st.stars[1].key == f1[1],
        "two characters with no seat, the same list saved twice: the same two folders (" + std::to_string(f1.size()) + " then " + std::to_string(f2.size()) +
            (f2 == f1 ? ", unchanged" : ", REPLACED") + ")" + (ok2 ? std::string() : ": " + err));
    importer::PackState back;
    const bool rd = importer::LoadPack(PathU8(m), back, err);
    Say(rd && back.stars.size() == 2 && back.stars[0].slot == -1 && back.stars[1].slot == -1 && sx::SeatedCount(back.stars) == 0 && back.problems.empty(),
        "LoadPack: both back, not seated, no problems");
    // (SEAT-C review) the importer screen's draft saved twice: the draft was copied before the first save gave the entry its key. PutInList keeps
    // the entry's key (same folder, his card rewritten); the old assignment (list[i] = draft) is the planted twin: it hands SavePack a keyless
    // entry with no seat, whose folder is deleted and made again under a new key.
    if (f1.size() == 2) {
      importer::Superstar draft = st.stars[0];
      draft.key.clear(), draft.name = "Aa Renamed";
      importer::PackState put = st;
      const int at = importer::PutInList(put, 0, draft);
      const bool okp = importer::SavePack(PathU8(m), put, err);
      Say(okp && at == 0 && folders() == f1 && put.stars[0].key == f1[0] && put.stars[0].name == "Aa Renamed",
          "a draft saved again (PutInList): the same folder " + f1[0] + ", renamed");
      importer::PackState twin = st;
      twin.stars[0] = draft;
      const bool okt = importer::SavePack(PathU8(m), twin, err);
      Twin("a keyless draft assigned over the entry (the builder's SaveDraft): his folder replaced", okt && !fsx::exists(m / "superstars" / "library" / f1[0], ec));
      importer::PackState add;
      importer::Superstar fresh = draft;
      fresh.key = "0000FFFF";  // (a stale key on a draft appended as a new character is dropped: a new character gets a new key)
      Say(importer::PutInList(add, -1, fresh) == 0 && add.stars[0].key.empty(), "PutInList appends a new character with no key");
    }
    // (SEAT-C review) LIBRARY PROBLEMS: three folders that are not readable cards are listed in plain words, and a save never touches them
    fsx::remove_all(m, ec);
    fsx::create_directories(m / "superstars" / "library", ec);
    const fsx::path lib = m / "superstars" / "library";
    fsx::create_directories(lib / "Copy of 00000001", ec);
    { std::ofstream(lib / "Copy of 00000001" / "card.txt") << "card 1\n"; }
    fsx::create_directories(lib / "0000000A", ec);
    { std::ofstream(lib / "0000000A" / "card.txt") << "garbage\n"; }
    {
      wwe2k14::seat_card::Card other;
      other.key = "0000000C", other.name = other.short_name = "Other";
      fsx::create_directories(lib / "0000000B", ec);
      std::ofstream(lib / "0000000B" / "card.txt") << wwe2k14::seat_card::CardText(other);
    }
    importer::PackState pr;
    const bool rdp = importer::LoadPack(PathU8(m), pr, err);
    std::string all;
    bool plain = rdp && pr.problems.size() == 3;
    for (const std::string& p : pr.problems) {
      const std::string line = sx::ProblemLine(p);
      all += "\n      " + line;
      plain = plain && line.rfind("The folder library\\", 0) == 0 && line.size() > 2 && line.compare(line.size() - 2, 2, "..") != 0 &&
              line.find("It was left out; nothing in it was changed.") != std::string::npos;
      std::string no_reason = line;  // (the reader's own reason, in brackets, is seat_card.h's: it names card.txt, the file in that folder)
      if (const size_t lb = no_reason.find(" ("), rb = no_reason.find(")."); lb != std::string::npos && rb != std::string::npos && rb > lb) no_reason.erase(lb, rb - lb + 1);
      for (const char* w : {"card", "frame", "inst", "seat none"}) plain = plain && no_reason.find(w) == std::string::npos;
    }
    Say(plain, "library problems: 3 folders, each in plain words (no card / frame / inst):" + all);
    importer::PackState keep = pr;
    const bool svp = importer::SavePack(PathU8(m), keep, err);
    Say(svp && fsx::exists(lib / "Copy of 00000001" / "card.txt", ec) && fsx::exists(lib / "0000000A" / "card.txt", ec) && fsx::exists(lib / "0000000B" / "card.txt", ec),
        "a save of the list leaves the three problem folders as they are");
    fsx::remove_all(m, ec);
  }
}
