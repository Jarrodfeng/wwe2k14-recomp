// S111 OWNNODEZ unit (docs\research\s111\OWNNODEZ.md): an entrance prop's own-node model takes the node's state (sub_8272D378: blend code 0, alpha test GEQUAL 1/255, z write 1) and the CWDA model
// (entry 6, sub_8272D0F0: the same with ref 128/255) through the object flags rf_props.cpp sets (splat_state.h OwnNodeObjectFlags) and the function FrontEnd::Record calls (ApplyOwnNode). 0 game runs, no files.
//   pwsh -NoProfile -File tools\scene\build_ownnode_unit.ps1
// --plant = the rule before (g_plant_own_node_material: every mesh keeps its material's draw at any MdlAlpha): it must FAIL.
#include "../../src/renderer_scene/splat_state.h"

#include <cstdio>
#include <cstring>

namespace {

int g_fail = 0;
void Check(bool ok, const char* what) {
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++g_fail;
}

namespace ss = rdr_scene::splat_state;

// FrontEnd::Record's own call (splat_state.h ApplyOwnNode): the item's blend (0 = the material's unblended draw) and whether it is drawn.
struct Out { bool drawn; uint8_t blend; };
Out Item(uint32_t flags, float alpha, uint8_t material_blend = 0, bool has_material = true) {
  Out o{true, material_blend};
  o.drawn = ss::ApplyOwnNode(flags, alpha, has_material, o.blend);
  return o;
}

}  // namespace

int main(int argc, char** argv) {
  const bool plant = argc > 1 && std::strcmp(argv[1], "--plant") == 0;
  ss::g_plant_own_node_material = plant;
  const uint32_t own = ss::OwnNodeObjectFlags() | uint32_t(rdr_scene::kObjVisible);
  const uint32_t cwda = ss::OwnNodeObjectFlags(true) | uint32_t(rdr_scene::kObjVisible);
  Check((own & rdr_scene::kObjNodeOwn) && !(own & rdr_scene::kObjNoDepthWrite) && !(own & rdr_scene::kObjNodeRef128), "the own node's flags: kObjNodeOwn, ref 1/255, and z write kept (no kObjNoDepthWrite: the node inherits z write 1)");
  Check((cwda & rdr_scene::kObjNodeOwn) && (cwda & rdr_scene::kObjNodeRef128) && !(cwda & rdr_scene::kObjNoDepthWrite), "the CWDA node's flags: kObjNodeOwn + kObjNodeRef128, z write kept");
  Check(((ss::NodeObjectFlags(false) | ss::NodeObjectFlags(true)) & (rdr_scene::kObjNodeOwn | rdr_scene::kObjNodeRef128)) == 0, "the category-33 node flags never carry kObjNodeOwn / kObjNodeRef128");
  Check(ss::DepthWriteOf(own, false, false) && ss::DepthWriteOf(cwda, false, false), "an opaque own-node / CWDA mesh keeps its depth write (the node's z write 1)");
  {
    const Out o = Item(own, 1.f), d = Item(own, 1.f, 0, false);
    Check(o.drawn && o.blend == 1 && d.drawn && d.blend == 0, "MdlAlpha 1: every mesh takes the node's blend (code 0; where its output alpha is 1 it shows no change); a mesh with no material stays unblended");
  }
  {
    const Out o = Item(own, 0.5f);
    Check(o.drawn && o.blend == 1, "MdlAlpha 0.5: an untested mesh is blended (code 0) like the game's own node");
  }
  {
    const Out o = Item(own, 0.5f, 2);
    Check(o.drawn && o.blend == 2, "MdlAlpha 0.5: a bind that already blends keeps its blend");
  }
  {
    const Out o = Item(own, 0.f);
    Check(!o.drawn, "MdlAlpha 0: nothing drawn (the GEQUAL 1/255 test drops every fragment)");
  }
  {
    const Out lo = Item(own, 0.5f / 255.f), at = Item(own, 1.f / 255.f);
    Check(!lo.drawn && at.drawn && at.blend == 1, "the 1/255 edge: below it nothing, at it blended");
  }
  {
    const Out one = Item(cwda, 1.f), mid = Item(cwda, 0.6f), lo = Item(cwda, 127.f / 255.f), at = Item(cwda, 128.f / 255.f);
    Check(one.drawn && one.blend == 1 && mid.drawn && mid.blend == 1 && !lo.drawn && at.drawn && at.blend == 1, "CWDA: blended at and above 128/255, nothing below (its GEQUAL 128/255 test drops every fragment: output alpha <= MdlAlpha)");
  }
  {
    const Out o = Item(uint32_t(rdr_scene::kObjVisible), 0.f), n = Item(ss::NodeObjectFlags(false), 0.f, 1), r = Item(uint32_t(rdr_scene::kObjNodeRef128), 0.f);
    Check(o.drawn && o.blend == 0 && n.drawn && n.blend == 1 && r.drawn && r.blend == 0, "objects without kObjNodeOwn are unchanged at any alpha");
  }
  if (g_fail) std::printf("FAIL  %d check(s) failed%s\n", g_fail, plant ? " (planted twin: the material's draw at any MdlAlpha)" : "");
  else std::printf("PASS  ownnode_unit\n");
  return g_fail ? 1 : 0;
}
