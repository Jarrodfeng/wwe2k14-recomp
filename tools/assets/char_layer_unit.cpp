// S111 LAYERS unit (docs\research\s111\LAYERS.md): the character draw split (src\assets\char_draw_layer.h) and the +2 shadow term it drives (front_end.cpp's char_class bit 3 -> technique_pick.h
// CharPassOf). No game, no GPU, no files. Run: out\tools\char_layer_unit.exe [--plant]; --plant = the rule before LAYERS (blend by the effect NAME `*trans*`, no +2 for the ych_trans /
// ych_aniso_trans names): it must FAIL. Build + both runs: pwsh -NoProfile -File tools\assets\build_char_layer_unit.ps1
#include <cstdio>
#include <cstring>
#include <string>

#include "assets/char_draw_layer.h"
#include "renderer_scene/technique_pick.h"

namespace cl = assets::char_layer;
namespace tp = rdr_scene::technique_pick;

static int g_fail = 0;
static void Check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++g_fail;
}

struct Verdict {
  bool blend;
  int pass;
};
// One part as the file loader + front end build it: blend = Material::alpha_blend (jboy.h), pass = CharPassOf with a shadow map, skinned, mode 0, a spec-power record, shadow_ok = !bit 3 (front_end.cpp).
static Verdict Of(bool plant, const char* effect, const std::string& pac, int entry, uint32_t class_key) {
  std::string lower(effect);
  for (char& c : lower) c = char(c >= 'A' && c <= 'Z' ? c + 32 : c);
  tp::CharClass c = tp::CharClassOf(lower);
  const bool blend = plant ? lower.find("trans") != std::string::npos : cl::FileTranslucent(pac, entry, class_key);
  if (!plant) c.hair = blend;   // front_end.cpp: char_class bit 3 = Material::alpha_blend (the plant keeps the NAME class)
  return {blend, tp::CharPassOf(c, true, !c.hair, 0, true)};
}

int main(int argc, char** argv) {
  const bool plant = argc > 1 && std::strcmp(argv[1], "--plant") == 0;
  Check(cl::TagOfKey(14) == 2 && cl::TagOfKey(13) == 6 && cl::TagOfKey(9) == 7 && cl::TagOfKey(109) == 7 && cl::TagOfKey(152) == 5 && cl::TagOfKey(53) == 0 && cl::TagOfKey(99) == 0 && cl::TagOfKey(0) == 3,
        "the tag of a class key is the byte of 0x82007F60 (keys 0..52 and 100..152; any other key 0)");
  Check(cl::Translucent(false, true, 0) && !cl::Translucent(false, false, 7) && !cl::Translucent(true, true, 4) && cl::Translucent(true, false, 5),
        "stock: piece = translucent, body = opaque, whatever the tag; created: tag 5..7 = translucent, piece or body");
  Check(cl::CreatedPartsPac("ed04.pac") && cl::CreatedPartsPac("ED49.PAC") && cl::CreatedPartsPac("ed7.pac") && !cl::CreatedPartsPac("Edge.pac") && !cl::CreatedPartsPac("eddie_guerrero.pac") &&
            !cl::CreatedPartsPac("edit.pac") && !cl::CreatedPartsPac("ed123.pac") && !cl::CreatedPartsPac("ch103.pac"),
        "the created-parts pacs are the game's pac\\edit\\edNN.pac only: a community character named Edge.pac / eddie_guerrero.pac is a stock wrestler's pac (review: the old `ed` prefix test took them)");
  Check(cl::DrawRank(true, false, 5) == 1 && cl::DrawRank(true, true, 6) == 2 && cl::DrawRank(true, false, 7) == 3 && cl::DrawRank(true, false, 2) == 0 && cl::DrawRank(false, true, 0) == 0 && cl::DrawRank(false, false, 7) == 0,
        "the draw order inside one model (CAWPASS folded in): opaque and stock parts 0, a created character's layers 5 / 6 / 7 = 1 / 2 / 3");
  const Verdict a = Of(plant, "yCh_trans", "ed04.pac", 10000, 14);
  Check(!a.blend && a.pass == 3, "created yCh_trans of class key 14 (layer 2): drawn OPAQUE with the +2 shadow term (pass 3)");
  const Verdict b = Of(plant, "yCh_skin", "ed04.pac", 10000, 13);
  Check(b.blend && b.pass == 1, "created yCh_skin of class key 13 (layer 6): BLENDED with no +2 (pass 1)");
  const Verdict c = Of(plant, "yCh_Aniso_trans", "ed04.pac", 10000, 9);
  Check(c.blend && c.pass == 1, "created hair of class key 9 (layer 7): blended, no +2 (as before)");
  const Verdict d = Of(plant, "yCh_trans", "ch103.pac", 0, 14);
  Check(!d.blend && d.pass == 3, "stock body yCh_trans (ch103 entry 0): drawn opaque by the character node, with the +2 term");
  const Verdict e = Of(plant, "yCh_cloth", "ch103.pac", 10500, 0);
  Check(e.blend && e.pass == 1, "stock piece yCh_cloth (ch103 entry 10500): drawn in the translucent draw, blended, no +2");
  const Verdict f = Of(plant, "yCh_skin", "ch103.pac", 0, 13);
  Check(!f.blend && f.pass == 3, "stock body yCh_skin: opaque with the +2 term (unchanged)");
  std::printf("%s: %d failure(s)%s\n", g_fail ? "FAILED" : "all checks passed", g_fail, plant ? " (planted)" : "");
  return g_fail ? 1 : 0;
}
