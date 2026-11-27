// S105 C3: unit of the family cull rule (src\renderer_scene\family_cull.h) with planted twins (the old rules): the rules are pure functions, no GPU, no game files.
//   clang++ -std=c++20 -I src tools\scene\family_cull_unit.cpp -o out\tools\family_cull_unit.exe   (tools\scene\build_family_cull_unit.ps1)
// Each case states the game's answer (sub_82C04DF0 0x82C0A880 cull block, docs/research/s105/C3.md) and each planted twin is a copy of the OLD rule that must be told apart from it.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include "renderer_scene/family_cull.h"

namespace fc = rdr_scene::family_cull;

static int g_fail = 0;
static void Check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_fail;
}

// planted twins: the rules before C3
static int OldCull(const assets::Material* mp, bool stage_cull_on) {   // only the stage slots followed the bit; every other family two-sided (jboy.h gate)
  return (mp && mp->cull_back && (!mp->cull_from_record || stage_cull_on)) ? 2 : 0;
}
static bool OldTwoPass(bool sorted, bool hair, uint32_t) { return sorted && hair; }   // the hair effect name alone, whatever the bit

int main() {
  assets::Material ring;   // a ring / prop / weapon / ladder-light mesh with the bit set
  ring.cull_word = 0x6, ring.cull_from_record = true, ring.cull_back = true, ring.stage_slot = false;
  assets::Material gear;   // a bit-clear gear mesh (cull word 4)
  gear.cull_word = 0x4, gear.cull_from_record = false, gear.cull_back = false;
  assets::Material stage;  // a stage slot, bit set
  stage.cull_word = 0x6, stage.cull_from_record = true, stage.cull_back = true, stage.stage_slot = true;
  assets::Material screen; // a screen / crowd / flash: its own rule (cull_from_record cleared)
  screen.cull_word = 0x6, screen.cull_from_record = false, screen.cull_back = true;
  assets::Material flash;  // the flash sprite: stays two-sided
  flash.cull_word = 0x6, flash.cull_from_record = false, flash.cull_back = false;

  Check(fc::Cull(&ring, true, true) == 2, "a bit-set ring / prop / character mesh is culled with the family switch on");
  Check(fc::Cull(&ring, true, false) == 0, "... two-sided with the family switch off");
  Check(fc::Cull(&ring, false, true) == 2, "... the stage switch does not move a non-stage family");
  Check(fc::Cull(&gear, true, true) == 0, "a bit-clear mesh is two-sided");
  Check(fc::Cull(&stage, true, false) == 2 && fc::Cull(&stage, false, true) == 0, "a stage slot follows the STAGE switch only");
  Check(fc::Cull(&screen, false, false) == 2, "the screens / crowd keep their own cull under both switches off");
  assets::Material screen_old = screen;   // S105 C3 F3 PLANT: the screen as stage_media.h left it before the fix (non-stage, bit still read from the record)
  screen_old.cull_from_record = true, screen_old.stage_slot = false;
  Check(fc::Cull(&screen_old, false, false) == 0 && fc::Cull(&screen, false, false) == 2, "PLANT (F3): a non-stage screen that keeps cull_from_record goes two-sided with the switch off; stage_media.h now clears it");
  Check(fc::Cull(&flash, true, true) == 0, "the flash sprite stays two-sided");
  Check(fc::Cull(nullptr, true, true) == 0, "no material: two-sided");
  assets::Material old_ring = ring;   // the old loader: a non-stage mesh had cull_back false
  old_ring.cull_back = false, old_ring.cull_from_record = false;
  Check(OldCull(&old_ring, true) == 0 && fc::Cull(&ring, true, true) == 2, "PLANT: the old loader's non-stage mesh (cull_back false) is two-sided: told apart from the fix");

  Check(fc::FrontFace(&stage, 2, true, false, false) == 0, "a stage slot's mirrored copy takes the swapped front face");
  Check(fc::FrontFace(&stage, 2, false, false, false) == 1, "a stage slot's normal copy: clockwise");
  Check(fc::FrontFace(&ring, 2, true, false, false) == 1, "a non-stage family's mirrored world gets NO swap (CUL_ADV D)");
  Check(fc::FrontFace(&ring, 2, true, false, true) == 0, "PLANT: the swap applied to a non-stage family is seen (the C3 plant)");
  Check(fc::FrontFace(&stage, 2, true, true, false) == 1, "the SCU plant (no flip) still reaches the stage slot");
  Check(fc::FrontFace(&ring, 0, true, false, true) == 1, "two-sided: front face untouched");

  Check(!fc::CharTwoPass(true, true, 0x6, true), "a translucent hair mesh with the bit SET is one draw (the 223 meshes lose their extra back layer)");
  Check(fc::CharTwoPass(true, true, 0x4, true), "a translucent hair mesh with the bit CLEAR is drawn twice");
  Check(!fc::CharTwoPass(false, true, 0x4, true), "an unsorted mesh is never drawn twice");
  Check(!fc::CharTwoPass(true, false, 0x4, true), "a non-hair translucent mesh stays one draw (U1 open)");
  Check(fc::CharTwoPass(true, true, 0x6, false), "family switch off: the hair names alone, whatever the bit (the draw before)");
  Check(OldTwoPass(true, true, 0x6) && !fc::CharTwoPass(true, true, 0x6, true), "PLANT: the old hair-name rule draws the bit-set hair twice: told apart from the fix");
  Check(OldTwoPass(true, true, 0x4) == fc::CharTwoPass(true, true, 0x4, true), "bit-clear hair: the old rule and the fix agree (same order)");
  {  // S105 C3 review F2: the fixture of OUR derived counts (tools\scene\fixtures\family_cull_counts.txt, from q asset cull; no game bytes)
    std::string path = __FILE__;
    path = path.substr(0, path.find_last_of("/\\") + 1) + "fixtures/family_cull_counts.txt";
    std::ifstream f(path);
    Check(bool(f), "the count fixture opens");
    std::string line;
    unsigned ladder_ok = 0, ladder_n = 0, set_total = 0, clear_total = 0, two_off = 0, two_on = 0;
    while (std::getline(f, line)) {
      std::istringstream is(line);
      std::string kind;
      if (!(is >> kind) || kind[0] == '#') continue;
      if (kind == "ladder") {
        std::string pach, key, word;
        unsigned n = 0;
        is >> pach >> key >> word >> n;
        assets::Material m;   // the ladder rig's piece as the loader builds it: the word's bit 0x2 -> cull_back from the record, a non-stage family
        m.cull_word = uint32_t(std::strtoul(word.c_str(), nullptr, 0)), m.cull_from_record = true, m.cull_back = (m.cull_word & 0x2u) != 0, m.stage_slot = false;
        const bool want = key != "1/1";   // the rope 1/0, disc 1/2 and column 1/3 are culled; the briefcase 1/1 is not
        ++ladder_n;
        if ((fc::Cull(&m, true, true) == 2) == want && fc::Cull(&m, true, false) == 0 && n >= 1) ++ladder_ok;
      } else if (kind == "hair") {
        std::string fam, eff;
        unsigned set = 0, clear = 0;
        is >> fam >> eff >> set >> clear;
        set_total += set, clear_total += clear;
        for (unsigned i = 0; i < set + clear; ++i) {   // one sorted hair-effect mesh each, the bit set for the first `set`
          const uint32_t word = i < set ? 0x6u : 0x4u;
          two_off += fc::CharTwoPass(true, true, word, false) ? 1u : 0u;
          two_on += fc::CharTwoPass(true, true, word, true) ? 1u : 0u;
        }
      }
    }
    Check(ladder_n == 4 && ladder_ok == 4, "ladder rig (bgEtc PACH 4): rope, disc 1/2 and column 1/3 culled, briefcase 1/1 two-sided, one-sided only with the family switch on");
    Check(set_total == 223 && two_off - two_on == set_total && two_on == clear_total, "census: two-pass hair draws drop by exactly the bit-set count (223 hair meshes) with the family switch on");
  }
  std::printf("family_cull_unit: %d failure(s)\n", g_fail);
  return g_fail ? 1 : 0;
}
