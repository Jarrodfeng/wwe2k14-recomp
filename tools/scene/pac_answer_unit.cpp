// S110 PACANS: the unit of src/scene_live/pac_answer.h (the two parsers of the game's own pac-list answer). Host only, no game, no SDK.
// S110 SEATREL: the name-keyed table that lived here is gone (its replacement, the per-descriptor note, is tools/scene/model_file_note_unit.cpp).
//   pwsh -NoProfile -File tools\scene\build_pac_answer_unit.ps1 ; out\tools\pac_answer_unit.exe
#include <cstdio>
#include <string>

#include "../../src/scene_live/pac_answer.h"

using namespace rdr_scene::live;

static int g_fail = 0;
static void Check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_fail;
}

int main() {
  {  // parsing, both node forms
    uint32_t id = 0, key = 0;
    std::string name;
    Check(pac_answer::ParseNodeFileId("DLC4:\\PAC\\CH\\CH398_DLC06.PAC", id) && id == 398, "node DLC form -> 398");
    Check(pac_answer::ParseNodeFileId("DISC:\\PAC\\CH\\CH012.PAC", id) && id == 12, "node disc form CH012.PAC -> 12");
    Check(pac_answer::ParseNodeFileId("x\\ch398_dlc06.pac", id) && id == 398, "node lower case -> 398");
    // S110 SEAT-B: a resident's node is made by src\seats.h from his library pac: SSLIB:\<KEY>\PAC\CH\CH<S>_DLC<PP>.PAC (the same file-name form)
    Check(pac_answer::ParseNodeFileId("SSLIB:\\1A2B3C4D\\PAC\\CH\\CH397_DLC06.PAC", id) && id == 397, "library node SSLIB:\\1A2B3C4D\\PAC\\CH\\CH397_DLC06.PAC -> 397");
    Check(!pac_answer::ParseNodeFileId("SSLIB:\\1A2B3C4D\\PAC\\CH\\CH397_DLC06.PA", id), "a torn library path is refused");
    Check(!pac_answer::ParseNodeFileId("DLC4:\\PAC\\CH\\CHXYZ.PAC", id), "node with no digits is refused");
    Check(!pac_answer::ParseNodeFileId("DLC4:\\PAC\\BG\\BG002.PAC", id), "a non-CH node is refused");
    Check(!pac_answer::ParseNodeFileId("DLC4:\\PAC\\CH\\CH398X.PAC", id), "CH<digits> followed by another letter is refused");
    // Review S110 PACANS: the game inserts a node into the list BEFORE it copies the node's path (sub_82C51B98 then sub_82C51B20; the disc
    // pass sub_82C52668 +E0..+138 the same), and the lookup takes no lock: another thread's lookup can see the path half written (a prefix,
    // the node zeroed behind it). Only the whole file name may answer.
    Check(!pac_answer::ParseNodeFileId(std::string("DLC4:\\PAC\\CH\\CH39\0\0\0\0", 20), id), "a torn path (a prefix ending in the id's first digits) is refused");
    Check(!pac_answer::ParseNodeFileId("DLC4:\\PAC\\CH\\CH398_DL", id), "a torn path (cut inside _DLC) is refused");
    Check(!pac_answer::ParseNodeFileId("DLC4:\\PAC\\CH\\CH398_DLC06.PA", id), "a torn path (cut inside .PAC) is refused");
    Check(!pac_answer::ParseNodeFileId("DLC4:\\PAC\\CH\\CH398", id), "no file extension is refused");
    Check(pac_answer::ParseNodeFileId("D:\\PAC\\CH\\CH100.PAC", id) && id == 100, "the disc node form D:\\PAC\\CH\\CH100.PAC -> 100");
    Check(pac_answer::ParseNodeFileId(std::string("DLC4:\\PAC\\CH\\CH398_DLC06.PAC\0\0\0", 31), id) && id == 398, "the 48-byte field, NUL padded -> 398");
    Check(!pac_answer::ParseNodeFileId("DLC4:\\PAC\\CH\\CHETC_DLC06.PAC", id), "chEtc is refused");
    Check(pac_answer::ParseAsked("/EMD/00039702", key, name) && key == 39702 && name == "00039702", "asked /EMD/00039702 -> key 39702 (5 digits)");
    Check(pac_answer::ParseAsked("/EMD/00123402", key, name) && key == 123402, "asked a 6-digit key");
    Check(!pac_answer::ParseAsked("/SSF/00039702", key, name), "a non-EMD path is refused");
    Check(!pac_answer::ParseAsked("/EMD/SSFA", key, name), "asked a name that is not 8 digits is refused");
    Check(!pac_answer::ParseAsked("/EMD/0003970A", key, name), "asked a name with a non-digit is refused");
    Check(!pac_answer::ParseAsked("/EMD/00000007", key, name), "asked a key under 100 is refused");
  }

  {  // S110 LIBSIZE: the library node's folder
    uint32_t id = 0;
    Check(pac_answer::ParseNodeFileId("SSLIB:\\1A2B3C4D\\PAC\\CH\\CH497.PAC", id) && id == 497, "library node CH497.PAC -> 497");
    Check(pac_answer::LibRel("SSLIB:\\1A2B3C4D\\PAC\\CH\\CH497.PAC") == "1A2B3C4D\\pac\\ch\\ch497.pac", "lib_rel: KEY folder keeps its case, the rest lower-cased");
    Check(pac_answer::LibRel("SSLIB:\\1a2b3c4d\\PAC\\CH\\CH497.PAC") == "1a2b3c4d\\pac\\ch\\ch497.pac", "a lower-case KEY stays lower-case");
    Check(pac_answer::LibRel(std::string("SSLIB:\\1A2B3C4D\\PAC\\CH\\CH497.PAC\0\0\0", 36)) == "1A2B3C4D\\pac\\ch\\ch497.pac", "the 48-byte field, NUL padded");
    Check(pac_answer::LibRel("DLC4:\\PAC\\CH\\CH398_DLC06.PAC").empty() && pac_answer::LibRel("DISC:\\PAC\\CH\\CH012.PAC").empty(), "a non-SSLIB path leaves lib_rel empty");
    Check(pac_answer::LibRel("SSLIB:\\CH497.PAC").empty() && pac_answer::LibRel("SSLIB:\\\\PAC\\CH\\CH497.PAC").empty() && pac_answer::LibRel("SSLIB:\\1A2B3C4D").empty() &&
              pac_answer::LibRel("SSLIB:\\1A2B3C4D\\").empty() && pac_answer::LibRel("SSLIB:\\").empty(),
          "a path with no KEY folder (or nothing below it) is left empty");
    Check(pac_answer::LibRel("OTHER:\\1A2B3C4D\\PAC\\CH\\CH497.PAC").empty(), "another device prefix is left empty");
  }

  std::printf("%s (%d failed)\n", g_fail ? "pac_answer_unit FAILED" : "pac_answer_unit OK", g_fail);
  return g_fail ? 1 : 0;
}
