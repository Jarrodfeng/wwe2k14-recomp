// S110 TRON-B: host unit for src\tron_slots_ui_core.h (the custom-tron screens' decisions: the list model, add / rename / remove, the problem
// lines, the importer's TRON choice). No game files; writes only under the folder you give it (keep it under out\).
//   out\tools\tron_slots_ui_unit.exe --dir OUT_DIR        (built by tools\importer\build_test.ps1)
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "../../src/tron_slots_ui_core.h"

namespace fs = std::filesystem;
namespace tc = wwe2k14::tron_slots_core;
namespace tu = wwe2k14::tron_slots_ui;

static int g_fail = 0;
static bool Say(bool ok, const std::string& m) {
  std::printf("  %s %s\n", ok ? "PASS" : "FAIL", m.c_str());
  if (!ok) ++g_fail;
  return ok;
}
static void Put(const fs::path& p, const std::string& text) { std::ofstream(p, std::ios::binary | std::ios::trunc) << text; }

// The rule the list must keep: a listed slot has a line AND a video on disk (an empty slot is never shown).
static bool NoEmptySlot(const fs::path& dir, const tu::Model& m) {
  for (const tc::Slot& s : m.filled)
    if (tc::FileFor(dir, s.id).empty()) return false;
  return true;
}
// A planted twin of BuildModel that lists every line, video or not (the old "show every slot" shape): the check above must refuse it.
static tu::Model BrokenTwin(const tc::Library& lib) {
  tu::Model m;
  m.filled = lib.Lines();
  return m;
}

int main(int argc, char** argv) {
  fs::path root;
  for (int i = 1; i + 1 < argc; ++i)
    if (std::string(argv[i]) == "--dir") root = fs::u8path(argv[i + 1]);
  if (root.empty()) {
    std::printf("usage: tron_slots_ui_unit --dir OUT_DIR\n");
    return 2;
  }
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root, ec);
  const fs::path d = root / "titantron";
  fs::create_directories(d, ec);
  std::printf("tron_slots_ui_core:\n");

  // the list model: filled / line only / file only
  Put(d / "823.mp4", "x");
  Put(d / "824.bik2", "x");   // a video with no line
  Put(d / "custom.txt", "823\tBig Show\n825\tGone\n");  // 825: a line with no video
  {
    const tc::Library lib = tc::ReadSlots(d);
    const tu::Model m = tu::BuildModel(d, lib);
    Say(m.filled.size() == 1 && m.filled[0].id == 823 && m.filled[0].name == "Big Show", "the list holds only the filled slot (823 Big Show)");
    Say(NoEmptySlot(d, m), "no listed slot is empty");
    Say(!NoEmptySlot(d, BrokenTwin(lib)), "planted twin that lists a line without a video FAILS the empty-slot check");
    bool a = false, b = false;
    for (const tu::Problem& p : m.problems) {
      if (p.kind == tu::Problem::kVideoMissing && p.id == 825 && p.text.find("Add the video again") != std::string::npos) a = true;
      if (p.kind == tu::Problem::kNameMissing && p.id == 824 && p.text.find("824.bik2") != std::string::npos &&
          p.text.find("Give it a name") != std::string::npos)
        b = true;
    }
    Say(a && b && m.problems.size() == 2, "a line with no video and a video with no line are PROBLEM rows with a one-line fix, not slots");
  }

  // add: the lowest free number, never a number whose line exists, never a video with no line
  {
    int id = 0;
    const std::string err = tu::AddSlot(d, "New one", [&](int n) { Put(d / (std::to_string(n) + ".mp4"), "x"); return std::string(); }, &id);
    // 823 filled, 824 file only, 825 line only -> 826
    Say(err.empty() && id == 826, "add takes 826 (823 filled, 824 has a video, 825 keeps its line): got " + std::to_string(id));
    const tc::Library lib = tc::ReadSlots(d);
    bool named = false;
    for (const tc::Slot& s : lib.listed)
      if (s.id == 826 && s.name == "New one") named = true;
    Say(named, "the new tron is listed under its name");
    Say(tu::AddSlot(d, "x", [](int) { return std::string("copy failed: nope"); }) == "copy failed: nope" && tc::ReadSlots(d).listed.size() == 2,
        "a failed copy adds no line");
    int id2 = 0;
    tu::AddSlot(d, "   ", [&](int n) { Put(d / (std::to_string(n) + ".mov"), "x"); return std::string(); }, &id2);
    bool dflt = false;
    for (const tc::Slot& s : tc::ReadSlots(d).listed)
      if (s.id == id2 && s.name == "Custom tron " + std::to_string(id2)) dflt = true;
    Say(id2 == 827 && dflt, "a blank name becomes 'Custom tron 827'");
  }

  // rename: filled, and the fix for a video with no name
  {
    Say(tu::RenameSlot(d, 823, "The Big Show") == "", "rename 823");
    Say(tu::RenameSlot(d, 824, "Named now") == "" && tc::ReadSlots(d).file_only.empty(), "naming the video 824 makes it a slot (no file_only left)");
    Say(tu::RenameSlot(d, 830, "Nothing here") != "", "rename of a number with neither line nor video is refused");
    Say(tu::RenameSlot(d, 107, "x") != "" && tu::RenameSlot(d, 823, "  ") != "", "rename outside the band / with no name is refused");
    bool ok = false;
    for (const tc::Slot& s : tc::ReadSlots(d).listed)
      if (s.id == 823 && s.name == "The Big Show") ok = true;
    Say(ok, "the new name is read back");
  }

  // refill: the fix for a line with no video keeps the number
  {
    Say(tu::RefillSlot(d, 825, [&](int n) { Put(d / (std::to_string(n) + ".mp4"), "x"); return std::string(); }) == "" &&
            tc::ReadSlots(d).line_only.empty(),
        "adding the video again fills 825 under its old name");
    Say(tu::RefillSlot(d, 823, [](int) { return std::string(); }) != "", "refill of a filled slot is refused");
  }

  // remove: every video and banner file go, the line is RETIRED (kept with its old name); the number is not handed out again
  {
    Put(d / "823.banner.txt", "none");
    Put(d / "823.mkv", "x");  // a second extension of the same number (hand-copied): goes too
    Say(tu::RemovedFiles(d, 823).size() == 3, "RemovedFiles: 823.mp4, 823.mkv and 823.banner.txt");
    Say(tu::RemoveSlot(d, 823) == "" && tc::FileFor(d, 823).empty() && !fs::exists(d / "823.banner.txt"),
        "remove deletes every video of the number and its banner file");
    const tc::Library lib = tc::ReadSlots(d);
    const tu::Model m = tu::BuildModel(d, lib);
    bool listed = false, problem = false;
    for (const tc::Slot& s : lib.listed) listed |= s.id == 823;
    for (const tu::Problem& p : m.problems) problem |= p.id == 823;
    Say(lib.retired.size() == 1 && lib.retired[0].id == 823 && lib.retired[0].name == "The Big Show" && !listed && !problem,
        "the line is retired with its old name: not listed, not a problem");
    const std::string text = [&] { std::ifstream f(d / "custom.txt", std::ios::binary); return std::string(std::istreambuf_iterator<char>(f), {}); }();
    Say(text.find("#retired\t823\tThe Big Show\n") != std::string::npos, "the retired line's form: #retired<TAB>823<TAB>The Big Show");
    // remove then add gives a NEW number; the planted twin (the line dropped: reuse at once) gives 823 and fails the same check
    std::vector<tc::Slot> dropped;
    for (const tc::Slot& s : lib.Lines())
      if (!s.retired) dropped.push_back(s);
    const auto new_number = [](int id) { return id != 823 && id > 0; };
    Say(!new_number(tc::NextFreeId(dropped, lib.file_only)), "planted twin (the removed line dropped, 823 reused at once) FAILS the new-number check");
    int id = 0;
    Say(tu::AddSlot(d, "After", [&](int n) { Put(d / (std::to_string(n) + ".mp4"), "x"); return std::string(); }, &id) == "" && new_number(id) && id == 828,
        "remove then add gives a NEW number (828), not the removed 823: got " + std::to_string(id));
    // the retired line survives a rename of another slot and a WriteSlots round trip; a video put back by hand stays hidden
    Say(tu::RenameSlot(d, 824, "Renamed again") == "" && tc::ReadSlots(d).retired.size() == 1, "a rename of another slot keeps the retired line");
    Say(tc::WriteSlots(d, tc::ReadSlots(d).Lines()) && tc::ReadSlots(d).retired.size() == 1 && tc::ReadSlots(d).retired[0].name == "The Big Show",
        "WriteSlots(ReadSlots().Lines()) round trip keeps the retired line and its name");
    Put(d / "823.mp4", "x");
    const tc::Library back = tc::ReadSlots(d);
    bool shown = false;
    for (const tc::Slot& s : back.listed) shown |= s.id == 823;
    for (int n : back.file_only) shown |= n == 823;
    Say(!shown && back.retired.size() == 1, "a video under a retired number is neither listed nor a nameless video");
    fs::remove(d / "823.mp4", ec);
    Say(tu::RenameSlot(d, 823, "x") != "" && tu::RefillSlot(d, 823, [](int) { return std::string(); }) != "",
        "rename / refill of a retired number are refused (only an explicit restore)");
    Say(tu::RestoreSlot(d, 824, [](int) { return std::string(); }) != "", "restore of a number that is not retired is refused");
    Say(tu::RestoreSlot(d, 823, [&](int n) { Put(d / (std::to_string(n) + ".mp4"), "x"); return std::string(); }) == "" && tc::ReadSlots(d).retired.empty() &&
            tc::ReadSlots(d).listed.front().id == 823 && tc::ReadSlots(d).listed.front().name == "The Big Show",
        "an explicit restore gives 823 its video again under its old name");
    Say(tu::RemoveSlot(d, 823) == "" && tc::ReadSlots(d).retired.size() == 1, "removed again: retired again");
  }

  // exhaustion: no never-used number left -> ADD takes the LOWEST retired number
  {
    const fs::path d3 = root / "full";
    fs::create_directories(d3, ec);
    std::vector<tc::Slot> lines;
    for (int id : tc::BandIds()) {
      tc::Slot s;
      s.id = id, s.name = "T" + std::to_string(id), s.retired = id == 840 || id == 830;
      lines.push_back(s);
    }
    tc::WriteSlots(d3, lines);
    int id = 0;
    Say(tu::AddSlot(d3, "Reuse", [&](int n) { Put(d3 / (std::to_string(n) + ".mp4"), "x"); return std::string(); }, &id) == "" && id == 830,
        "all 116 hold a line: ADD takes the lowest retired number (830): got " + std::to_string(id));
    const tc::Library l3 = tc::ReadSlots(d3);
    bool named = false;
    for (const tc::Slot& s : l3.listed) named |= s.id == 830 && s.name == "Reuse";
    Say(named && l3.retired.size() == 1 && l3.retired[0].id == 840, "830 is now the new tron; 840 stays retired");
    tu::AddSlot(d3, "Last", [&](int n) { Put(d3 / (std::to_string(n) + ".mp4"), "x"); return std::string(); });
    Say(tu::AddSlot(d3, "None", [](int) { return std::string(); }) != "" , "no live-free and no retired number left: ADD is refused");
  }

  // the sheet warning
  Say(tu::SheetWarning(".bik2", 640, 360).find("640x360") != std::string::npos && tu::SheetWarning(".bik2", 320, 320).empty() &&
          tu::SheetWarning(".mp4", 1920, 1080).empty(),
      "a .bik2 that is not 320x320 gets the warning; a 320x320 .bik2 and an MP4 of any size do not");

  // the importer's choice
  {
    int from = 7, slot = 9;
    tu::Apply(tu::ChoiceOf(0, 0), from, slot);
    Say(from == 0 && slot == 0 && tu::ChoiceOf(-1, -1).kind == tu::Choice::kEntrance, "default: entrance wrestler's (0, 0)");
    tu::Apply({tu::Choice::kWrestler, 343}, from, slot);
    Say(from == 343 && slot == 0 && tu::ChoiceOf(from, slot).kind == tu::Choice::kWrestler, "another wrestler's: tron_from");
    tu::Apply({tu::Choice::kSlot, 826}, from, slot);
    Say(from == 0 && slot == 826 && tu::ChoiceOf(343, 826).kind == tu::Choice::kSlot, "a custom slot: tron_slot (wins over tron_from)");
    const tc::Library lib = tc::ReadSlots(d);
    const auto nm = [](int id) { return std::string("W") + std::to_string(id); };
    Say(tu::ChoiceLabel({tu::Choice::kSlot, 826}, lib.listed, &lib, nm) == "New one", "a filled slot shows its name");
    Say(tu::ChoiceLabel({tu::Choice::kSlot, 888}, lib.listed, &lib, nm) == "MISSING: 888", "a saved slot that is not filled shows MISSING: number");
    tu::RemoveSlot(d, 826);
    const tc::Library lr = tc::ReadSlots(d);
    Say(tu::ChoiceLabel({tu::Choice::kSlot, 826}, lr.listed, &lr, nm) == "MISSING: New one" && tu::IsRetired(lr, 826),
        "a saved slot that was removed (retired) shows MISSING: its old name");
    Put(d / "custom.txt", "826\tOld name\n");
    const tc::Library l2 = tc::ReadSlots(d);
    Say(tu::ChoiceLabel({tu::Choice::kSlot, 826}, l2.listed, &l2, nm) == "MISSING: Old name", "a saved slot whose video is gone shows MISSING: its name");
    Say(tu::ChoiceLabel({tu::Choice::kEntrance, 0}, {}, nullptr, nm) == "ENTRANCE WRESTLER'S" &&
            tu::ChoiceLabel({tu::Choice::kWrestler, 343}, {}, nullptr, nm) == "ANOTHER WRESTLER'S: W343",
        "the other two labels");
  }

  // (review) no mods folder: every writer refuses an empty folder. fs::path() / "custom.txt" is the WORKING folder's custom.txt (the viewer's
  // TronDir() is empty with no --mods): nothing may be written or deleted there.
  {
    const fs::path cwd = fs::current_path(ec), here = fs::absolute(root / "cwd", ec);
    fs::create_directories(here, ec);
    fs::current_path(here, ec);
    Put("823.mp4", "x");  // a file in the working folder named like a slot video: must survive
    const auto ok_copy = [](int) { return std::string(); };
    const bool refused = !tu::RenameSlot({}, 823, "x").empty() && !tu::AddSlot({}, "x", ok_copy).empty() &&
                         !tu::RefillSlot({}, 823, ok_copy).empty() && !tu::RetireLine({}, 823).empty() && !tu::RemoveSlot({}, 823).empty();
    const bool untouched = !fs::exists(here / "custom.txt") && !fs::exists(here / "custom.txt.part") && fs::exists(here / "823.mp4");
    fs::current_path(cwd, ec);
    Say(refused && untouched && tu::RemovedFiles({}, 823).empty() && !tu::RestoreSlot({}, 823, ok_copy).empty(),
        "with no mods folder (an empty path) add / rename / refill / remove / retire / restore refuse and touch nothing in the working folder");
  }

  std::printf("%s (%d failed)\n", g_fail ? "FAIL" : "PASS", g_fail);
  return g_fail ? 1 : 0;
}
