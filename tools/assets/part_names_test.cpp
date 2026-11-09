// Session 98 step 16: unit test of src\assets\part_names.h (not part of the viewer build).
//   clang++ -std=c++17 -Wall -Wextra -I src\assets tools\assets\part_names_test.cpp -o out\part_names_test.exe
//   out\part_names_test.exe [crosscheck.tsv]      (the TSV is made by `part_names.py --crosscheck FILE` and lives under out\)
// Hand-written cases plus, when a TSV is given, every distinct (kind, key, node, texture, shader, palette, additive) of the
// scanned arenas / wrestlers against the Python answer.
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "part_names.h"

using namespace assets;

static int g_fail = 0, g_ok = 0;

static void Expect(const char* what, const PartKey& k, const char* friendly, const char* category) {
  const PartName n = FriendlyPartName(k);
  if (n.friendly == friendly && n.category == category) {
    ++g_ok;
  } else {
    ++g_fail;
    std::printf("FAIL %s: got '%s' / '%s', want '%s' / '%s'\n", what, n.friendly.c_str(), n.category.c_str(), friendly, category);
  }
}

static PartKey Arena(const char* pac, int key, const char* node, const char* tex, bool add = false) {
  PartKey k;
  k.kind = PartKind::kArena, k.pac = pac, k.key = key, k.node = node, k.texture = tex, k.additive = add;
  return k;
}

static PartKind KindOf(const std::string& s) {
  return s == "wrestler" ? PartKind::kWrestler : s == "crowd" ? PartKind::kCrowd : s == "prop" ? PartKind::kProp : PartKind::kArena;
}

int main(int argc, char** argv) {
  Expect("barricade", Arena("bg05.pac", 35, "ar_fence01_a", "ar_fence"), "Barricade", "Barricade");
  Expect("barricade corner", Arena("bg05.pac", 392, "root", "ar_fence"), "Barricade Corner", "Barricade Corner");
  Expect("floor mat", Arena("bg05.pac", 25, "ar_dwnmt01", "ar_dm"), "Floor Mat", "Floor Mat");
  Expect("ring base", Arena("bg05.pac", 15, "ar_ring_sita", "stand_ground0"), "Ring Base (under the ring)", "Ring");
  Expect("ring canvas", Arena("bg05.pac", 953, "ar_ring", "rin_mat00"), "Ring Canvas", "Ring");
  Expect("ropes", Arena("bg05.pac", 960, "ar_rope10", "ar_rop01"), "Ring Ropes", "Ring Ropes");
  Expect("post", Arena("bg05.pac", 951, "ar_post", "rin_pos00"), "Ring Post", "Ring Post");
  Expect("billboard not VIP", Arena("bg05.pac", 116, "Vip_Billboard_1", "ad_1"), "Advertising Billboard", "Signage / Banner");
  Expect("vip", Arena("bg05.pac", 107, "Vip_BoxSeat_1", "fleet_vip1"), "VIP Box Seats", "Seats");
  Expect("beam", Arena("bg05.pac", 220, "nwo_glow02", "beam_glow01", true), "Light Beam", "Light Cone / Glare");
  Expect("titantron screen", Arena("bg05.pac", 800, "nwo_800", "titantron_movie"), "Titantron Screen", "Titantron");
  Expect("mini-tron", Arena("bg05.pac", 805, "nwo_805", "mini_tron"), "Mini-Tron", "Mini-Tron");
  Expect("truss", Arena("bg05.pac", 208, "nwo_truss00", "truss_00"), "Lighting Truss", "Lighting Truss");
  Expect("unknown", Arena("bg05.pac", 77, "zzz", "qqq"), "Unknown part (slot 77)", "Unknown");

  PartKey c;
  c.kind = PartKind::kCrowd, c.pac = "bg05.pac", c.key = 20003, c.node = "root", c.shader = "yDefault";
  Expect("crowd", c, "Crowd Member", "Crowd");

  PartKey p;
  p.kind = PartKind::kProp, p.pac = "bgEtc.pac", p.key = 32, p.node = "root", p.texture = "ar_stair01";
  Expect("steps", p, "Ring Steps", "Ring Steps");
  p.key = 6, p.node = "vector", p.texture = "ji_desk";
  Expect("desk", p, "Announce Desk", "Announce Desk");

  PartKey w;
  w.kind = PartKind::kWrestler, w.pac = "ch107.pac", w.key = 10000, w.node = "null", w.texture = "color", w.shader = "yCh_Aniso_trans";
  w.palette = "Bip Head;kami_b09_00";
  Expect("hair", w, "Hair", "Hair");
  w.key = 0, w.node = "vector", w.shader = "yCh_Reflect", w.palette = "l_eye;r_eye";
  Expect("eyes", w, "Eyes", "Head");
  w.shader = "yCh_gear", w.palette = "Bip L UpperArm;Bip L Forearm;l_Elbow_j";
  Expect("arm pads", w, "Arm Pads and Sleeves", "Pads");
  w.shader = "yCh_skin", w.palette = "";
  Expect("skin fallback", w, "Body Skin", "Body");
  {
    const PartName n = FriendlyPartName(Arena("bg05.pac", 392, "root", "ar_fence"));
    if (n.literal != "ar_fence" || n.source != "bg05.pac slot 392") {
      ++g_fail;
      std::printf("FAIL literal/source: '%s' / '%s'\n", n.literal.c_str(), n.source.c_str());
    } else {
      ++g_ok;
    }
    const PartName m = FriendlyPartName(Arena("bg05.pac", 35, "ar_fence01_a", "ar_fence"));
    if (m.literal != "ar_fence01_a") ++g_fail, std::printf("FAIL literal %s\n", m.literal.c_str());
    else ++g_ok;
  }

  int checked = 0;
  if (argc > 1) {
    std::ifstream f(argv[1]);
    std::string line;
    while (std::getline(f, line)) {
      std::vector<std::string> t;
      std::stringstream ss(line);
      std::string cell;
      while (std::getline(ss, cell, '\t')) t.push_back(cell);
      while (t.size() < 9) t.push_back("");  // (empty trailing cells are dropped by getline)
      PartKey k;
      k.kind = KindOf(t[0]), k.key = std::stoi(t[1]), k.node = t[2], k.texture = t[3], k.shader = t[4], k.palette = t[5], k.additive = t[6] == "1";
      const PartName n = FriendlyPartName(k);
      ++checked;
      if (n.friendly == t[7] && n.category == t[8]) {
        ++g_ok;
      } else if (++g_fail < 20) {
        std::printf("MISMATCH %s %d '%s' '%s': C++ '%s' / '%s', Python '%s' / '%s'\n", t[0].c_str(), k.key, k.node.c_str(), k.texture.c_str(),
                    n.friendly.c_str(), n.category.c_str(), t[7].c_str(), t[8].c_str());
      }
    }
    std::printf("crosscheck: %d lines\n", checked);
  }
  std::printf("%d passed, %d failed\n", g_ok, g_fail);
  return g_fail ? 1 : 0;
}
