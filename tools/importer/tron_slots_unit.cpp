// S110 TRONSLOT: host unit for src\tron_slots_core.h (the custom tron slot rule: band, custom.txt, the files). No game files; writes only
// under the folder you give it (keep it under out\).
//   out\tools\tron_slots_unit.exe --dir OUT_DIR        (built by tools\importer\build_test.ps1)
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "../../src/tron_slots_core.h"

namespace fs = std::filesystem;
namespace tc = wwe2k14::tron_slots_core;

static int g_fail = 0;
static bool Say(bool ok, const std::string& m) {
  std::printf("  %s %s\n", ok ? "PASS" : "FAIL", m.c_str());
  if (!ok) ++g_fail;
  return ok;
}
static void Put(const fs::path& p, const std::string& text) {
  std::ofstream(p, std::ios::binary | std::ios::trunc) << text;
}
static std::string Get(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

int main(int argc, char** argv) {
  fs::path root;
  for (int i = 1; i + 1 < argc; ++i)
    if (std::string(argv[i]) == "--dir") root = fs::u8path(argv[i + 1]);
  if (root.empty()) {
    std::printf("usage: tron_slots_unit --dir OUT_DIR\n");
    return 2;
  }
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root, ec);
  std::printf("tron_slots_core:\n");

  // the band
  int n = 0, runs = 0;
  bool prev = false;
  for (int i = 0; i < 1000; ++i) {
    const bool in = tc::IsSlotId(i);
    n += in, runs += in && !prev, prev = in;
  }
  Say(n == 116 && runs == 3 && tc::IsSlotId(823) && tc::IsSlotId(843) && !tc::IsSlotId(844) && tc::IsSlotId(845) && tc::IsSlotId(899) &&
          !tc::IsSlotId(900) && !tc::IsSlotId(909) && tc::IsSlotId(910) && tc::IsSlotId(949) && !tc::IsSlotId(950) && !tc::IsSlotId(822) && !tc::IsSlotId(-1),
      "the band: 116 numbers in 3 runs (823-843, 845-899, 910-949); 844, 900-909, 950 and 822 are out");

  // ReadSlots: line + file, line only, file only, a file outside the band, a duplicate number
  const fs::path d = root / "titantron";
  fs::create_directories(d, ec);
  Put(d / "845.mp4", "x");                 // line + file
  Put(d / "846.mov", "x");                 // file only (no line)
  Put(d / "850.mkv", "x");                 // line + file, second one
  Put(d / "844.mp4", "x");                 // a file outside the band (never listed, never reported)
  Put(d / "107.bik2", "x");                // a stock tron replacement: not ours
  Put(d / "845.alpha.mp4", "x");           // a second video for the same number: the redirect's order (kExts) picks .mp4
  Put(d / "custom.txt",
      "# a comment\n"
      "845\tTest Tron\n"
      "847\tGone Tron\r\n"                 // line only
      "850\t  Padded   \n"
      "845\tDuplicate\n"                   // a duplicate number: the first line is kept
      "844\tOutside\n"                     // outside the band: reported, not listed
      "abc\tNo number\n"
      "851\n"                              // no name: gets a default, but no file: line only
      "852\tCaf\xC3\xA9 \x01Ring\n");      // non-printable / non-ASCII dropped
  Put(d / "852.mp4", "x");
  tc::Library lib = tc::ReadSlots(d);
  bool ids_ok = lib.listed.size() == 3 && lib.listed[0].id == 845 && lib.listed[1].id == 850 && lib.listed[2].id == 852;
  Say(ids_ok, "ReadSlots lists only a number with BOTH a line and a file (845, 850, 852), sorted");
  Say(ids_ok && lib.listed[0].name == "Test Tron" && lib.listed[0].file.filename() == "845.mp4" && lib.listed[1].name == "Padded" &&
          lib.listed[2].name == "Caf Ring",
      "names are trimmed printable ASCII; the first line of a duplicate wins; the file is the first of kExts (.mp4 before .alpha.mp4)");
  Say(lib.line_only.size() == 2 && lib.line_only[0].id == 847 && lib.line_only[1].id == 851 && !lib.line_only[0].present && lib.line_only[1].name == "Custom tron 851",
      "a line without a file is reported (847, 851), not listed; a nameless line gets a default name");
  Say(lib.file_only.size() == 1 && lib.file_only[0] == 846, "a file in the band without a line is reported (846), not listed; 844.mp4 (outside) and 107.bik2 are neither");
  Say(lib.problems.size() == 3, "three bad lines are reported (a duplicate, outside the band, no number): " + std::to_string(lib.problems.size()));
  Say(lib.Lines().size() == 5, "Lines() = listed + line only (5)");

  // NextFreeId
  Say(tc::NextFreeId(lib.Lines()) == 823, "NextFreeId: the lowest band number with no line (823)");
  Say(tc::NextFreeId(lib.Lines(), {823, 824}) == 825, "NextFreeId: a number with a file but no line is avoided (the new video must not overwrite it)");
  std::vector<tc::Slot> all;
  for (int id : tc::BandIds()) all.push_back({id, "x", false, {}});
  Say(tc::NextFreeId(all) == -1, "NextFreeId: all 116 taken = -1");
  all.erase(all.begin() + 21);  // 845 gone (index 21 = the first of 845-899)
  Say(tc::NextFreeId(all) == 845, "NextFreeId: the gap (845) after 823-843 are taken");
  std::vector<tc::Slot> few = {{823, "a", false, {}}, {825, "b", false, {}}};
  Say(tc::NextFreeId(few) == 824, "NextFreeId: a number whose line exists is never reused, a hole below the highest is taken first (824)");

  // WriteSlots round trip
  const fs::path d2 = root / "titantron2";
  std::vector<tc::Slot> w = {{850, "Second", false, {}}, {845, "First One", false, {}}};
  Say(tc::WriteSlots(d2, w), "WriteSlots creates the folder and the file");
  const tc::Library back = tc::ReadSlots(d2);
  Say(back.listed.empty() && back.line_only.size() == 2 && back.line_only[0].id == 845 && back.line_only[0].name == "First One" && back.line_only[1].id == 850 &&
          back.problems.empty() && back.has_file,
      "WriteSlots -> ReadSlots round trip (sorted by number; no video there, so both are line only)");
  Put(d2 / "845.mp4", "x");
  Say(tc::ReadSlots(d2).listed.size() == 1 && tc::ReadSlots(d2).listed[0].id == 845, "after the video is put there the slot is listed");
  Say(!fs::exists(d2 / "custom.txt.part"), "no .part file is left behind");
  Say(!tc::WriteSlots(d2, {{844, "Out", false, {}}}) && !tc::WriteSlots(d2, {{845, "A", false, {}}, {845, "B", false, {}}}) && !tc::WriteSlots(d2, {{846, "   ", false, {}}}),
      "WriteSlots refuses a number outside the band, a duplicate and an empty name");
  Say(tc::ReadSlots(d2).line_only.size() == 1 || tc::ReadSlots(d2).listed.size() == 1, "a refused write leaves the old file as it was");
  const std::string text = Get(d2 / "custom.txt");
  Say(text.find("845\tFirst One\n") != std::string::npos && text.rfind("#", 0) == 0, "the file text: a comment header, then `N<TAB>name` lines");
  // a long name is cut at the list's limit (the same 40 as music_slots) and a BOM / empty folder reads
  Say(tc::CleanName(std::string(60, 'A')).size() == tc::kMaxName && tc::kMaxName == 40, "a name is cut at 40 characters (music_slots' limit)");
  Put(d2 / "custom.txt", "\xEF\xBB\xBF" "846\tBom Line\n");
  Put(d2 / "846.mkv", "x");
  Say(tc::ReadSlots(d2).listed.size() == 1 && tc::ReadSlots(d2).listed[0].name == "Bom Line", "a UTF-8 BOM (Notepad) at the top does not hide the first line");
  // (review) the game's list hook reads with find_unlisted = false: the 116 x 7 file checks for unlisted videos are the UIs' business, not a
  // load's; a line written with a space instead of the TAB keeps its name
  Put(d2 / "custom.txt", "846 Space Name\n847\tTab Name\n");
  Put(d2 / "847.mp4", "x");
  Put(d2 / "848.mp4", "x");  // a video with no line
  const tc::Library quick = tc::ReadSlots(d2, false), full = tc::ReadSlots(d2);
  Say(quick.listed.size() == 2 && quick.file_only.empty() && full.file_only.size() == 2 && full.file_only[0] == 845 && full.file_only[1] == 848,
      "ReadSlots(dir, false) lists the same slots and skips the unlisted-video scan (full scan: 845, 848)");
  Say(full.listed.size() == 2 && full.listed[0].name == "Space Name" && full.listed[1].name == "Tab Name", "a space after the number works as the TAB does");
  const tc::Library none = tc::ReadSlots(root / "no such folder");
  Say(none.listed.empty() && none.line_only.empty() && none.file_only.empty() && !none.has_file, "a missing folder = an empty library");

  // S110 TRON-B: the retired line "#retired<TAB>N<TAB>name" (a removed tron's kept number)
  const fs::path d4 = root / "retired";
  fs::create_directories(d4, ec);
  Put(d4 / "860.mp4", "x");  // a video under a retired number (put back by hand): still not listed
  Put(d4 / "861.mp4", "x");
  Put(d4 / "custom.txt",
      "#retired\t860\tOld Name\n"
      "#retired\t861\tWas Retired\n"
      "861\tLive Again\n"            // a live line for the same number wins (a hand edit)
      "#retired\tabc\tBad\n"         // a bad retired line is a comment: no problem, no slot
      "#retired\t844\tOutside\n"     // outside the band: a comment too
      "#retired\t862\n");            // no name: a default
  const tc::Library r = tc::ReadSlots(d4);
  Say(r.listed.size() == 1 && r.listed[0].id == 861 && r.listed[0].name == "Live Again" && r.line_only.empty() && r.problems.empty() && r.file_only.empty(),
      "retired lines are never listed, never problems, and a video under a retired number is not a nameless video; a live line wins");
  Say(r.retired.size() == 2 && r.retired[0].id == 860 && r.retired[0].name == "Old Name" && r.retired[0].retired && r.retired[1].id == 862 &&
          r.retired[1].name == "Custom tron 862",
      "Library::retired holds 860 (its old name) and 862 (a default name)");
  Say(r.Lines().size() == 3, "Lines() = listed + line only + retired (3)");
  Say(tc::WriteSlots(d4, r.Lines()) && Get(d4 / "custom.txt").find("#retired\t860\tOld Name\n") != std::string::npos &&
          Get(d4 / "custom.txt").find("\n861\tLive Again\n") != std::string::npos && tc::ReadSlots(d4).retired.size() == 2,
      "WriteSlots writes a retired slot as #retired<TAB>N<TAB>name and it reads back retired");
  // NextFreeId: a retired number counts as taken; only with no never-used number left is the lowest retired one given
  std::vector<tc::Slot> band;
  for (int id : tc::BandIds()) {
    tc::Slot s;
    s.id = id, s.name = "x", s.retired = id == 900 || id == 920 || id == 912;
    band.push_back(s);
  }
  Say(tc::NextFreeId(band) == 912, "all 116 hold a line: the lowest RETIRED number (912)");
  Say(tc::NextFreeId(band, {912}) == 920, "a retired number with a video on disk (avoid) is skipped (920)");
  std::vector<tc::Slot> some = {{823, "a", false, {}, true}};
  Say(tc::NextFreeId(some) == 824, "a retired 823 is not reused while 824 is never-used");

  std::printf("failures: %d\n", g_fail);
  return g_fail;
}
