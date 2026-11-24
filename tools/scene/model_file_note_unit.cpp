// S110 SEATREL: the unit of src/scene_live/model_file_note.h (the file a slot descriptor's model was BUILT from). Host only, no game, no SDK.
//   pwsh -NoProfile -File tools\scene\build_model_file_note_unit.ps1 ; out\tools\model_file_note_unit.exe
// The planted twin is the OLD behaviour (a table keyed by the asked name, fed by every lookup); the "probe lookup changes nothing" case must FAIL on it.
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "../../src/scene_live/model_file_note.h"

using namespace rdr_scene::live;

static int g_fail = 0;
static void Check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_fail;
}

static std::vector<std::string> g_lines;
static void Sink(const std::string& s) { g_lines.push_back(s); }

// The planted twin: the old name-keyed table, last lookup wins, no reader window, no descriptor.
struct OldNameTable {
  std::map<uint32_t, uint32_t> file_of_key;
  void Lookup(const std::string& asked, const std::string& node) {
    uint32_t key = 0, fid = 0;
    std::string name;
    if (pac_answer::ParseAsked(asked, key, name) && pac_answer::ParseNodeFileId(node, fid)) file_of_key[key] = fid;
  }
  uint32_t FileIdOf(uint32_t key) const {
    auto it = file_of_key.find(key);
    return it == file_of_key.end() ? key / 100 : it->second;
  }
};

static OldNameTable g_old;

constexpr uint32_t kRead1 = 0x70001000, kPre9 = 0x70002000, kDesc0 = 0x71000000, kDesc9 = 0x71000000 + 192 * 9;
static const char* const kCh397 = "DLC4:\\PAC\\CH\\CH397_DLC05.PAC";
static const char* const kCh398 = "DLC4:\\PAC\\CH\\CH398_DLC06.PAC";

int main() {
  model_file_note::SetLog(&Sink);
  pac_answer::Answer a;

  {  // the reader window
    Check(model_file_note::CurrentReader() == 0, "no reader: a lookup now is a probe");
    {
      model_file_note::ReaderScope s1(kRead1);
      Check(model_file_note::CurrentReader() == kRead1, "inside a read: the current reader is the request's reader");
      {
        model_file_note::ReaderScope s2(kPre9);
        Check(model_file_note::CurrentReader() == kPre9, "a nested read: its own reader");
      }
      Check(model_file_note::CurrentReader() == kRead1, "the nested read ends: the outer reader again");
    }
    Check(model_file_note::CurrentReader() == 0, "the read ends: the window is closed");
  }

  {  // a read, a build, a probe after the build
    model_file_note::Clear();
    g_lines.clear();
    Check(model_file_note::NoteRead(kRead1, "/EMD/00039702", kCh398), "the read's lookup (a seated slot: CH398's file) is noted per reader");
    Check(!model_file_note::NoteRead(kRead1, "/SSF/00039702", kCh398) && !model_file_note::NoteRead(0, "/EMD/00039702", kCh398), "a non-EMD path and a lookup with no reader note nothing");
    Check(!model_file_note::FileOfDesc(kDesc0, 39702, a), "before the build the descriptor has no note");
    Check(model_file_note::NoteBuild(kDesc0, 39702, kRead1), "the build gives the reader's note to the descriptor");
    Check(model_file_note::FileOfDesc(kDesc0, 39702, a) && a.file_id == 398 && a.file_key == 39802, "descriptor 0: key 39702 -> file 398, file key 39802");
    Check(g_lines.size() == 1 && g_lines[0].find("CH398_DLC06.PAC") != std::string::npos && g_lines[0].find("39802") != std::string::npos, "one log line, with the node and the file key");
    // PROBE LOOKUP CHANGES NOTHING: in the game a probe (no reader window) never reaches NoteRead; the hook's guard is CurrentReader() == 0.
    // The unit drives the hook's own rule: note only inside a window. -DPLANT_OLD_RULE swaps in the old behaviour (a name-keyed table fed by every
    // lookup, window or not): the probe cases below must then FAIL (seen once, in the hand-back).
    const auto hook = [](const char* asked, const char* node) {
#ifdef PLANT_OLD_RULE
      static OldNameTable& t = g_old;
      t.Lookup(asked, node);
#else
      if (const uint32_t r = model_file_note::CurrentReader()) model_file_note::NoteRead(r, asked, node);   // exactly the post-hook's test
#endif
    };
    const auto file_now = [&]() -> uint32_t {
#ifdef PLANT_OLD_RULE
      return g_old.FileIdOf(39702);
#else
      return model_file_note::FileOfDesc(kDesc0, 39702, a) ? a.file_id : 397u;
#endif
    };
#ifdef PLANT_OLD_RULE
    g_old.Lookup("/EMD/00039702", kCh398);   // the read of the old rule (also a lookup)
#endif
    hook("/EMD/00039702", kCh397);   // a menu probe after the build: outside every window
    Check(file_now() == 398, "a probe lookup after the build changes nothing (still CH398)");
    {
      model_file_note::ReaderScope s(kRead1);
      hook("/EMD/00039702", kCh397);   // a REAL read of the same name that took CH397 again ...
    }
#ifndef PLANT_OLD_RULE
    Check(file_now() == 398, "... still changes nothing until the game BUILDS from it");
    Check(model_file_note::NoteBuild(kDesc0, 39702, kRead1) && file_now() == 397, "a rebuild from that read flips the note to CH397");
    Check(g_lines.size() == 2, "one log line per change");
    Check(!model_file_note::NoteBuild(kDesc0, 39702, kRead1) && g_lines.size() == 2, "the same build again: no change, no second line");
#endif
    // the planted twin itself: the old name-keyed table flips on the probe (the behaviour the cases above forbid)
    OldNameTable old;
    old.Lookup("/EMD/00039702", kCh398);   // the read
    old.Lookup("/EMD/00039702", kCh397);   // a probe after the build
    Check(old.FileIdOf(39702) != 398, "PLANTED TWIN: the old name-keyed table answers CH397 after a probe: the very answer the game's kept model contradicts");
  }

  {  // the key guard, another descriptor, a kept descriptor
    model_file_note::Clear();
    model_file_note::NoteRead(kRead1, "/EMD/00039702", kCh398);
    model_file_note::NoteBuild(kDesc0, 39702, kRead1);
    Check(!model_file_note::FileOfDesc(kDesc0, 39712, a), "the note answers only for the key it was built for (398's own other attire: the id rule)");
    Check(!model_file_note::FileOfDesc(kDesc0 + 192, 39702, a), "another descriptor with the same key has no note");
    Check(model_file_note::FileIdOf(kDesc0, 39702) == 398 && model_file_note::FileIdOf(kDesc0, 39712) == 397 && model_file_note::FileIdOf(0, 39702) == 397, "FileIdOf: the note, else key / 100");
    // the reader asked another key than the descriptor holds: not taken, the old note dropped (the build replaced the model)
    model_file_note::NoteRead(kRead1, "/EMD/00039802", kCh398);
    Check(model_file_note::NoteBuild(kDesc0, 39702, kRead1) && !model_file_note::FileOfDesc(kDesc0, 39702, a), "a build whose reader asked another key drops the old note: the id rule");
    Check(!model_file_note::NoteBuild(0, 39702, kRead1), "no descriptor: nothing");
  }

  {  // an entrance hand-over: descriptors 9..11 are built from the bytes the entrance pre-reader read: the pre-reader's note, not the building reader's
    model_file_note::Clear();
    model_file_note::NoteRead(kPre9, "/EMD/00039704", kCh398);     // the pre-reader's read took CH398
    model_file_note::NoteRead(kRead1, "/EMD/00039704", kCh397);    // the building reader's own lookup (a different answer)
    Check(model_file_note::NoteBuild(kDesc9, 39704, kPre9) && model_file_note::FileOfDesc(kDesc9, 39704, a) && a.file_id == 398 && a.file_key == 39804,
          "entrance hand-over: the descriptor takes the pre-reader's note (CH398), key 39704 -> file key 39804");
    Check(model_file_note::NoteBuild(kDesc9, 39704, kRead1) && model_file_note::FileOfDesc(kDesc9, 39704, a) && a.file_id == 397, "built from the building reader's own note instead: CH397 (the source parameter decides)");
  }

  {  // S110 LIBSIZE: every library file is CH497.PAC: two characters at one seat, one after the other: the SECOND path wins (the id alone is equal)
    model_file_note::Clear();
    g_lines.clear();
    const char* const kLibA = "SSLIB:\\1A2B3C4D\\PAC\\CH\\CH497.PAC";
    const char* const kLibB = "SSLIB:\\5E6F7A8B\\PAC\\CH\\CH497.PAC";
    model_file_note::NoteRead(kRead1, "/EMD/00049702", kLibA);
    Check(model_file_note::NoteBuild(kDesc0, 49702, kRead1) && model_file_note::FileOfDesc(kDesc0, 49702, a) && a.lib_rel == "1A2B3C4D\\pac\\ch\\ch497.pac" && a.file_key == 49702,
          "library character A: lib_rel under A's KEY folder, file key 497 x 100 + 2");
    model_file_note::NoteRead(kRead1, "/EMD/00049702", kLibB);
    Check(model_file_note::NoteBuild(kDesc0, 49702, kRead1), "library character B at the same seat (same id 497, same key): the build is a change");
    Check(model_file_note::FileOfDesc(kDesc0, 49702, a) && a.lib_rel == "5E6F7A8B\\pac\\ch\\ch497.pac", "the second character's path wins");
    Check(g_lines.size() == 2 && g_lines[1].find("5E6F7A8B") != std::string::npos, "two log lines, the second names B's node");
    Check(!model_file_note::NoteBuild(kDesc0, 49702, kRead1) && g_lines.size() == 2, "the same character again: no change");
    // the planted twin is the OLD header (`same` on key and file id only: B equals A, the descriptor keeps A's folder): the cases above FAIL on it (seen in the hand-back)
    model_file_note::NoteRead(kRead1, "/EMD/00039702", kCh398);
    model_file_note::NoteBuild(kDesc9, 39702, kRead1);
    Check(model_file_note::FileOfDesc(kDesc9, 39702, a) && a.lib_rel.empty(), "a disc / DLC node: lib_rel empty (the id rule's file)");
  }

  model_file_note::Clear();
  Check(!model_file_note::FileOfDesc(kDesc0, 39702, a), "Clear empties the table (fast path)");

  std::printf("%s (%d failed)\n", g_fail ? "model_file_note_unit FAILED" : "model_file_note_unit OK", g_fail);
  return g_fail ? 1 : 0;
}
