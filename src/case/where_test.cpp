// S101 lane 32: runs the case zip's address rules (src/case/case_where.h, the header src/bookmark.h's FillCaseWhere calls) on synthetic state sets.
// Built and driven by tools/case/selftest.py section 12 (clang++ -std=c++17 -I src/case src/case/where_test.cpp). No game, no SDK.
// One state set a line, whitespace separated KEY=VALUE (no spaces inside a value):
//   seq= scene_word= menu= clock= spec= go=N dry=0|1 wm30= replay= frame= forced=VERB:ID live=type:T,arena:A,pace:P,ids:343/324 ring=BANK:TYPE steps=5/-1/-1
// Output, one line a set: ADDRESS <address or -> | BASIS <text> | REASON <text> | WORDS <text> | NODE <node or -> | MATCH <type> <arena> <ids,...>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>

#include "case_where.h"

static std::vector<int64_t> Nums(const std::string& s, char sep) {
  std::vector<int64_t> v;
  std::stringstream ss(s);
  for (std::string t; std::getline(ss, t, sep);) v.push_back(t.empty() ? 0 : std::atoll(t.c_str()));
  return v;
}

int main() {
  std::string line;
  while (std::getline(std::cin, line)) {
    rdr_case::where::Inputs in;
    std::stringstream ss(line);
    for (std::string kv; ss >> kv;) {
      const size_t eq = kv.find('=');
      if (eq == std::string::npos) continue;
      const std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
      if (k == "seq") in.seq = std::atoll(v.c_str());
      else if (k == "scene_word") in.scene_word = std::atoll(v.c_str());
      else if (k == "menu") in.menu = std::atoll(v.c_str());
      else if (k == "clock") in.match_clock = std::atoll(v.c_str());
      else if (k == "spec") in.match_spec = v;
      else if (k == "go") in.match_go = std::atoll(v.c_str());
      else if (k == "dry") in.match_dry = v == "1";
      else if (k == "wm30") in.wm30 = std::atoll(v.c_str());
      else if (k == "replay") in.replay_play = v;
      else if (k == "frame") in.guest_frame = std::strtoull(v.c_str(), nullptr, 10);
      else if (k == "steps") in.entrance_steps = Nums(v, '/');
      else if (k == "ring") {
        const auto n = Nums(v, ':');
        if (n.size() == 2) in.ring_bank = n[0], in.ring_type = n[1];
      } else if (k == "forced") {
        const size_t c = v.find(':');
        in.forced_verb = v.substr(0, c);
        in.forced_id = c == std::string::npos ? 0 : std::atoll(v.c_str() + c + 1);
      } else if (k == "live") {  // type:T,arena:A,pace:P,ids:343/324
        in.live.have = true;
        std::stringstream ls(v);
        for (std::string part; std::getline(ls, part, ',');) {
          const size_t c = part.find(':');
          const std::string pk = part.substr(0, c), pv = part.substr(c + 1);
          if (pk == "type") in.live.type = std::atoll(pv.c_str());
          else if (pk == "arena") in.live.arena = std::atoll(pv.c_str());
          else if (pk == "pace") in.live.pace = std::atoll(pv.c_str());
          else if (pk == "ids") in.live.ids = Nums(pv, '/');
        }
      }
    }
    rdr_case::Manifest m;
    rdr_case::where::Fill(m, in);
    const auto& w = m.where;
    std::string ids;
    for (size_t i = 0; i < w.wrestlers.size(); ++i) ids += (i ? "," : "") + w.wrestlers[i];
    std::printf("ADDRESS %s | BASIS %s | REASON %s | WORDS %s | NODE %s | MATCH %s %s %s\n", w.address ? w.address->c_str() : "-",
                w.address_basis ? w.address_basis->c_str() : "-", m.IsUnavailable("where.address") ? "set" : "-", w.words.c_str(),
                w.node ? w.node->c_str() : "-", w.match_type ? w.match_type->c_str() : "-", w.arena ? w.arena->c_str() : "-", ids.empty() ? "-" : ids.c_str());
  }
  return 0;
}
