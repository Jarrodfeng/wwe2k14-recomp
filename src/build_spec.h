// The match builder's spec (--wwe2k14_match, src\scenario.h FireBuild), pure: no guest memory, no game types, so a host unit drives the
// production parser (tools\scene\launch_participant_unit.cpp). Moved out of src\scenario.h in S109 lane ENT (docs\research\s109\ENT_plan.md).
//   type=T;p=ID:PAD[:ATTIRE],...;arena=A;pace=P;o=K:V,...;title=T0/T1;rumble=ID,...;pods=N,...;entrances=0|1
// S109 ENT: a p item may carry a third part, the launcher's ATTIRE grammar (src\launch_participant.h) limited to what the builder can do:
//   N  attire 0-9 (a CAW, id < 100: 0-3): FireBuild writes the seat record +20 = N, +24 = -1, +28 = 512 after sub_829278C8, so finalize makes the
//      launcher's calls for ID:PAD:N (sub_822C89F8 a5 = N, no alternate key, sub_822C6808 with -1; ENT_plan.md section 3)
//   s  the superstar's saved defaults: what the builder's pick already leaves in the record (nothing written)
//   N/S, tS (colour sets) are refused: the empty-set check needs the M slot word finalize writes itself (ENT_plan.md section 3, [U])
// An empty PAD with a third part is a CPU (as on the launcher); a two-part item is parsed exactly as before (strict numbers).
// entrances=0: finalize's team-table build is skipped (the REX_HOOK_RAW(sub_822CBD28) in src\scenario.h), as --wwe2k14_launch_skip_entrances does.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "launch_participant.h"

namespace wwe2k14::scenario {

struct SeatAttire {
  bool write = false;  // FireBuild writes the seat record's attire words (+20 attire, +24 set -1, +28 no alternate key 512)
  int attire = 0;
};

struct BuildSpec {
  int64_t type = -1, arena = -1, pace = -1, title0 = -1, title1 = -1;
  std::vector<std::pair<int64_t, int64_t>> parts;  // (id, pad)
  std::vector<SeatAttire> attire;                  // S109 ENT: one per parts entry
  std::vector<std::pair<int64_t, int64_t>> opts;   // (k, v): option block byte R+8+k
  std::vector<int64_t> rumble;
  std::vector<int64_t> pods;  // Elimination Chamber (types 39 / 40): the pod per seat, 0..5 (-1 = the game's draw)
  bool entrances = true;      // S109 ENT: entrances=0 skips finalize's team-table build (no entrances)
};

namespace build_detail {
inline bool Num(const std::string& s, int64_t& v) {
  char* end = nullptr;
  v = std::strtoll(s.c_str(), &end, 0);
  return !s.empty() && end && *end == 0;
}
inline std::vector<std::string> Split(const std::string& s, char c) {
  std::vector<std::string> v;
  size_t a = 0;
  while (a <= s.size()) {
    size_t b = s.find(c, a);
    if (b == std::string::npos) b = s.size();
    if (b > a) v.push_back(s.substr(a, b - a));
    a = b + 1;
  }
  return v;
}
// One p item: ID:PAD (exactly as before ENT) or ID:PAD:ATTIRE (S109 ENT). False + why = the whole spec is refused (never dropped).
inline bool ParseSeat(const std::string& item, int64_t& id, int64_t& pad, SeatAttire& att, std::string& why) {
  att = SeatAttire{};
  const size_t c = item.find(':');
  if (c == std::string::npos) return why = "'" + item + "': A:B", false;
  const size_t c2 = item.find(':', c + 1);
  if (c2 == std::string::npos) {  // two parts: the pre-ENT parse
    if (!Num(item.substr(0, c), id) || !Num(item.substr(c + 1), pad)) return why = "'" + item + "': A:B", false;
    return true;
  }
  if (item.find(':', c2 + 1) != std::string::npos) return why = "'" + item + "': ID:PAD:ATTIRE has three parts", false;
  const std::string pad_text = item.substr(c + 1, c2 - c - 1), a = item.substr(c2 + 1);
  if (!Num(item.substr(0, c), id)) return why = "'" + item + "': ID:PAD:ATTIRE, ID a number", false;
  if (pad_text.empty()) {
    pad = -1;  // an empty PAD with an attire part = CPU, as on the launcher
  } else if (!Num(pad_text, pad)) {
    return why = "'" + item + "': ID:PAD:ATTIRE, PAD a number or empty", false;
  }
  if (a.find('/') != std::string::npos || (!a.empty() && a[0] == 't'))
    return why = "'" + item + "': colour sets (N/S, tS) are launcher only (--wwe2k14_launch_participants): the builder cannot run the empty-set check before finalize", false;
  wwe2k14::launch::ParticipantToken tok;
  std::string tok_why;
  if (!wwe2k14::launch::ParseParticipantToken(item, tok, tok_why) || !tok.has_attire)
    return why = "'" + item + "': " + (tok_why.empty() ? std::string("ATTIRE = N (0-9, a CAW 0-3) | s") : tok_why), false;
  if (!tok.saved) att.write = true, att.attire = tok.attire;
  return true;
}
}  // namespace build_detail

inline bool ParseBuildSpec(const std::string& spec, BuildSpec& out, std::string& why) {
  using build_detail::Num;
  using build_detail::Split;
  for (const std::string& kv : Split(spec, ';')) {
    const size_t eq = kv.find('=');
    if (eq == std::string::npos) return why = "'" + kv + "': KEY=VALUE", false;
    const std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
    int64_t n = 0, m = 0;
    if (k == "type" || k == "arena" || k == "pace") {
      if (!Num(v, n)) return why = "'" + kv + "': a number", false;
      (k == "type" ? out.type : k == "arena" ? out.arena : out.pace) = n;
    } else if (k == "title") {
      const auto t = Split(v, '/');
      if (t.size() != 2 || !Num(t[0], n) || !Num(t[1], m)) return why = "title=T0/T1", false;
      out.title0 = n, out.title1 = m;
    } else if (k == "p") {
      for (const std::string& item : Split(v, ',')) {
        SeatAttire att;
        if (!build_detail::ParseSeat(item, n, m, att, why)) return false;
        out.parts.push_back({n, m});
        out.attire.push_back(att);
      }
    } else if (k == "o") {
      for (const std::string& item : Split(v, ',')) {
        const size_t c = item.find(':');
        if (c == std::string::npos || !Num(item.substr(0, c), n) || !Num(item.substr(c + 1), m))
          return why = "'" + item + "': A:B", false;
        out.opts.push_back({n, m});
      }
    } else if (k == "rumble" || k == "pods") {
      for (const std::string& item : Split(v, ',')) {
        if (!Num(item, n)) return why = k + ": '" + item + "'", false;
        (k == "rumble" ? out.rumble : out.pods).push_back(n);
      }
    } else if (k == "entrances") {  // S109 ENT
      if (v != "0" && v != "1") return why = "entrances=0|1", false;
      out.entrances = v == "1";
    } else {
      return why = "unknown key '" + k + "'", false;
    }
  }
  if (out.type < 0 || out.type > 118) return why = "type 0..118 is required", false;
  for (auto& [id, pad] : out.parts)
    if (id < 0 || id >= 500 || pad < -1 || pad > 3) return why = "participant id 0..499, pad -1..3", false;
  for (auto& [k, v] : out.opts)
    if (k < 0 || k > 91 || v < 0 || v > 255) return why = "o: K 0..91, V 0..255", false;
  if (out.rumble.size() > 40) return why = "rumble: at most 40", false;
  for (int64_t id : out.rumble)
    if (id < 0 || id >= 500) return why = "rumble ids 0..499", false;
  for (int64_t pod : out.pods)
    if (pod < -1 || pod > 5) return why = "pods -1..5", false;
  if (out.pods.size() > 6) return why = "pods: at most 6", false;
  if (out.pace > 2) return why = "pace 0..2", false;
  return true;
}

}  // namespace wwe2k14::scenario
