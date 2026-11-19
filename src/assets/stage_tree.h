// S103 V1: the arena's visibility tree (pac entry 50020, text "NODE MY_NAME = name [PARENT_NAME = parent] { x0 y0 z0 x1 y1 z1 } nkids") -> one box per stage slot.
// Layout, join and evidence: docs\formats\stage_visibility.md. Header only (std), read from the player's own files at load; nothing here is shipped.
#pragma once
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "asset_model.h"

namespace assets::stage_tree {

struct Node {
  std::string parent;
  float lo[3] = {}, hi[3] = {};   // the file's 3ds-max space (x, y, z up)
  int kids = 0;
};

// Parses the text. A malformed node ends the parse (what was read stays): the caller treats a short tree as fewer tested slots, never as a guess.
inline std::map<std::string, Node> Parse(std::string_view t) {
  std::map<std::string, Node> out;
  size_t pos = 0;
  const auto word = [&](size_t& p) {
    while (p < t.size() && std::isspace((unsigned char)t[p])) ++p;
    const size_t s = p;
    while (p < t.size() && !std::isspace((unsigned char)t[p])) ++p;
    return t.substr(s, p - s);
  };
  while ((pos = t.find("NODE MY_NAME =", pos)) != std::string_view::npos) {
    size_t p = pos + 14;
    const std::string name(word(p));
    Node n;
    size_t q = p;
    std::string_view w = word(q);
    if (w == "PARENT_NAME") {
      word(q);   // '='
      n.parent = std::string(word(q));
      p = q;
    }
    w = word(p);
    if (w != "{") break;
    float v[6];
    bool ok = true;
    for (float& f : v) {
      const std::string s(word(p));
      char* end = nullptr;
      f = std::strtof(s.c_str(), &end);
      ok = ok && end && end != s.c_str() && std::isfinite(f);
    }
    if (!ok || word(p) != "}") break;
    const std::string k(word(p));
    n.kids = std::atoi(k.c_str());
    for (int i = 0; i < 3; ++i) n.lo[i] = v[i], n.hi[i] = v[3 + i];
    out.emplace(name, n);
    pos = p;
  }
  return out;
}

// The tree's box in the VIEWER's space: the file's models are (x, -z, y) of the tree's (x, y, z), the loader negates Y once more -> (x, z, y).
inline void ToViewer(const Node& n, float lo[3], float hi[3]) {
  lo[0] = n.lo[0], hi[0] = n.hi[0];
  lo[1] = n.lo[2], hi[1] = n.hi[2];
  lo[2] = n.lo[1], hi[2] = n.hi[1];
}

// Fills m.stage_boxes: a stage slot gets a box when (a) its model's first node name is a tree node, (b) the slot is not moved by the stage key (m(N): the node box is local),
// (c) the node's box equals the box of the slot's own meshes within `tol` (the join is by NAME and arenas reuse names, so the geometry confirms every join; the entrance pieces the
// rig places and the slots that share a name fail it). A slot with no box is never tested: it stays drawn ("asked, never dropped").
inline void Fill(Model& m, std::string_view text, const std::set<int>& moving, const std::set<int>& mirrored, float tol = 2.0f) {
  m.stage_boxes.clear();
  const std::map<std::string, Node> tree = Parse(text);
  if (tree.empty()) return;
  struct Acc { float lo[3] = {}, hi[3] = {}; std::string node; bool any = false, bad = false; };
  std::map<int, Acc> slots;
  for (const Mesh& me : m.meshes) {
    int k = 0;
    if (std::sscanf(me.part.c_str(), "slot %d", &k) != 1 || me.part != "slot " + std::to_string(k) || k < 0 || k >= 400) continue;
    Acc& a = slots[k];
    if (me.placement >= 0 && size_t(me.placement) < m.placements.size()) {   // S103 V1 review 1: a placed mesh (the fence corners 390-397, a motion) is drawn elsewhere than its stored box: only the twin placements are the base box
      const std::string& lb = m.placements[size_t(me.placement)].label;
      const auto ends = [&](const char* t) { const size_t n = std::strlen(t); return lb.size() >= n && lb.compare(lb.size() - n, n, t) == 0; };
      if (!ends(" mirrored") && !ends(" turned")) { a.bad = true; continue; }
    }
    if (me.node.empty() || (!a.node.empty() && a.node != me.node)) { a.bad = true; continue; }   // (no node name, or two models under one slot: not joined)
    if (a.node.empty()) a.node = me.node;
    for (const Vertex& v : me.vertices) {
      if (!std::isfinite(v.pos[0] + v.pos[1] + v.pos[2])) continue;
      for (int i = 0; i < 3; ++i) {
        if (!a.any) a.lo[i] = a.hi[i] = v.pos[i];
        a.lo[i] = std::fmin(a.lo[i], v.pos[i]), a.hi[i] = std::fmax(a.hi[i], v.pos[i]);
      }
      a.any = true;
    }
  }
  for (const auto& kv : slots) {
    const Acc& a = kv.second;
    if (!a.any || a.bad || a.node.empty() || moving.count(kv.first)) continue;
    const auto it = tree.find(a.node);
    if (it == tree.end()) continue;
    StageBox b;
    b.slot = kv.first;
    b.twin = mirrored.count(kv.first) != 0;
    ToViewer(it->second, b.lo, b.hi);
    float worst = 0.f;
    for (int i = 0; i < 3; ++i) worst = std::fmax(worst, std::fmax(std::fabs(b.lo[i] - a.lo[i]), std::fabs(b.hi[i] - a.hi[i])));
    if (!(worst <= tol)) continue;
    for (int i = 0; i < 3; ++i) b.lo[i] = std::fmin(b.lo[i], a.lo[i]), b.hi[i] = std::fmax(b.hi[i], a.hi[i]);   // S103 V1 review 2: the UNION of the node box and the mesh box (they differ by up to tol): geometry at the edge never loses a sliver
    m.stage_boxes.push_back(b);
  }
}

}  // namespace assets::stage_tree
