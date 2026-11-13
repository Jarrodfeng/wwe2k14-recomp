// arena_world_oracle: lane 130's capture-side oracle (Session 101; docs\renderer\ARENA_OURS.md "D3 gaps"). 0 game runs, no GPU, no game.
//   arena_world_oracle --game DIR --arena N --in DRAWS.txt --out SOURCED.txt [--plant NAME]
// DRAWS.txt is written by tools\skip\arena_world_oracle.py from a capture's stream draws (ledger_io readers): per capture frame its guest frame, per placed / moving /
// animated arena draw its engine object, family, VS c76..c78 (12 float bits) and c186 (float bits). This program decides, with the SHIPPED pure functions
// (src\scene_live\arena_world.h: PlacedRows, RowsFromMatrix, ClockAt, FindEntry) and the player's own motion file (pac entry 30001 through assets::stage_key::EvalMotion),
// which draws the new record field WOULD source: the value the field would hold equals the stream draw's register within float tolerance.
//   placed   : the draw's rows are PlacedRows(arena, s) of some slot s in 390..397                    -> regs 76,77,78
//   animated : c186 equals the stage clock (frame - origin; x2 for the 2x group, the beam cones)         -> reg 186
//   moving   : the act m(N) whose first key position equals the draw's translation, evaluated at t = clock - OFF -> regs 76,77,78
// origin and OFF are constants of the capture's route that the oracle FITS from the first frames (the live writer gets the origin from the CStage::MotionSetup hook and OFF
// from nothing: [U]); every later frame is then a prediction. The entries the writer would hold are put in a table stamped with the guest frame and joined back through FindEntry.
// Plants: placed_identity (the record's identity kept), moving_static (frame-0 transform held), clock_frozen, stamp_off_by_one (joined at the previous frame).
// Output: `S fi i regs` per sourced draw, then `SUMMARY family draws sourced`, `FIT origin=.. off=..`. Exit 0 ok, 2 bad input.
#include "stb_image.h"  // declarations only: the loader seam defines the implementation

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "../../src/assets/pac.h"
#include "../../src/assets/stage_key.h"
#include "../../src/assets/view/loader_seam.h"
#include "../../src/renderer_scene/scene_record.h"
#include "../../src/scene_live/arena_world.h"

using namespace rdr_scene;
using namespace rdr_scene::live::arenaworld;

struct Draw {
  int fi = 0, i = 0;
  uint64_t guest = 0;
  uint32_t object = 0;
  std::string family;
  float rows[12] = {};
  float c186 = 0;
};

static float BitsF(uint32_t b) {
  float f;
  std::memcpy(&f, &b, 4);
  return f;
}

int main(int argc, char** argv) {
  std::string game, in, out, plant;
  int arena = 4;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--game" && i + 1 < argc) game = argv[++i];
    else if (a == "--in" && i + 1 < argc) in = argv[++i];
    else if (a == "--out" && i + 1 < argc) out = argv[++i];
    else if (a == "--plant" && i + 1 < argc) plant = argv[++i];
    else if (a == "--arena" && i + 1 < argc) arena = std::atoi(argv[++i]);
  }
  if (game.empty() || in.empty() || out.empty()) { std::fprintf(stderr, "usage: arena_world_oracle --game DIR --arena N --in DRAWS.txt --out SOURCED.txt [--plant NAME]\n"); return 2; }
  std::vector<Draw> draws;
  {
    std::ifstream f(in);
    if (!f) { std::fprintf(stderr, "arena_world_oracle: FAIL cannot read %s\n", in.c_str()); return 2; }
    std::string line;
    int fi = 0;
    uint64_t guest = 0;
    while (std::getline(f, line)) {
      std::istringstream ss(line);
      std::string tag;
      ss >> tag;
      if (tag == "F") ss >> fi >> guest;
      else if (tag == "D") {
        Draw d;
        d.fi = fi, d.guest = guest;
        std::string obj, fam;
        ss >> d.i >> obj >> fam;
        d.object = uint32_t(std::strtoul(obj.c_str(), nullptr, 16));
        d.family = fam;
        for (int k = 0; k < 12; ++k) { std::string h; ss >> h; d.rows[k] = BitsF(uint32_t(std::strtoul(h.c_str(), nullptr, 16))); }
        std::string h;
        ss >> h;
        d.c186 = BitsF(uint32_t(std::strtoul(h.c_str(), nullptr, 16)));
        if (!ss) { std::fprintf(stderr, "arena_world_oracle: FAIL bad draw line: %s\n", line.c_str()); return 2; }
        draws.push_back(d);
      }
    }
  }
  if (draws.empty()) { std::fprintf(stderr, "arena_world_oracle: FAIL no draws in %s\n", in.c_str()); return 2; }

  // the motion file
  std::string err;
  ViewerInit(game);
  std::vector<uint8_t> raw;
  char rel[64];
  std::snprintf(rel, sizeof rel, "pac\\bg\\bg%02d.pac", arena);
  if (!ViewerReadFile(rel, raw, err)) { std::fprintf(stderr, "arena_world_oracle: FAIL read %s: %s\n", rel, err.c_str()); return 2; }
  assets::pac::Pac pc;
  if (!assets::pac::OpenBytes(std::move(raw), rel, pc, err)) { std::fprintf(stderr, "arena_world_oracle: FAIL open %s: %s\n", rel, err.c_str()); return 2; }
  const assets::Bytes keys = assets::pac::Get(pc.bytes, pc.pachs[0], 30001);
  assets::stage_key::Index ix;
  if (keys.empty() || !ix.Parse(keys)) { std::fprintf(stderr, "arena_world_oracle: FAIL %s has no stage motion keys (entry 30001)\n", rel); return 2; }
  std::map<int, std::array<float, 3>> act_pos;  // act -> the motion's translation (constant over t in these arenas; the first key's)
  for (int act = 0; act < 400; ++act) {
    assets::Matrix m;
    if (assets::stage_key::EvalMotion(ix, act, 0, m)) act_pos[act] = {m[12], m[13], m[14]};
  }

  // fit 1: the stage clock's origin from the animated draws of the first frame that has any: the most common (guest - c186)
  std::map<long, int> votes;
  int first_anim_fi = -1;
  for (const Draw& d : draws)
    if (d.family == "animated") {
      if (first_anim_fi < 0) first_anim_fi = d.fi;
      if (d.fi != first_anim_fi) break;
      {  // the 1x group reads clock, the 2x group (the beam cones) 2 x clock: a vote for the origin each way, the true origin gets both groups' votes
        const long c = std::lround(d.c186);
        votes[long(d.guest) - c]++;
        if (c % 2 == 0) votes[long(d.guest) - c / 2]++;
      }
    }
  long origin = 0;
  bool have_origin = false;
  {
    int best = 0;
    for (auto& kv : votes)
      if (kv.second > best) best = kv.second, origin = kv.first, have_origin = true;
  }
  uint64_t first_guest = draws.front().guest;
  auto clock_of = [&](uint64_t guest) {
    float c = 0.f;
    const uint64_t g = (plant == "clock_frozen") ? first_guest : guest;  // the frozen clock never advances
    ClockAt(uint32_t(g), uint32_t(origin), have_origin, c);
    return c;
  };

  // the entries the writer would hold, in frame order, stamped with the guest frame
  std::vector<ArenaWorld> table;
  std::map<uint64_t, size_t> first_of_frame;
  // fit 2: OFF of the moving pieces over 200 moving draws spread over the capture
  auto act_for = [&](const Draw& d) {
    for (auto& kv : act_pos)
      if (std::fabs(kv.second[0] - d.rows[3]) < 0.01f && std::fabs(kv.second[1] - d.rows[7]) < 0.01f && std::fabs(kv.second[2] - d.rows[11]) < 0.01f) return kv.first;
    return -1;
  };
  auto motion_rows = [&](int act, double t, float rows[12]) {
    assets::Matrix m;
    if (!assets::stage_key::EvalMotion(ix, act, t, m)) return false;
    RowsFromMatrix(m.data(), rows);
    return true;
  };
  // The motion's player state is per slot: a direction (a motion is played forward or reversed) and a phase. Both are fitted per act over up to 80 of the act's draws spread
  // over the capture (exact = within 2e-3); an act with no exact fit gets none and its draws are left unsourced (the beam cones have an extra node rotation the file
  // does not hold [C]). The live writer reads the matrix instead (kAwObject), so this fit only shows what the file + one clock would give.
  struct Player { int sign = 1; long off = 0; int fit = 0, of = 0; bool ok = false; };
  std::map<uint32_t, Player> players;  // by engine object (a slot): the player state is per slot
  {
    std::map<uint32_t, std::vector<const Draw*>> by_act;  // keyed by object here
    for (const Draw& d : draws)
      if (d.family == "moving" && act_for(d) >= 0) by_act[d.object].push_back(&d);
    for (auto& kv : by_act) {
      std::vector<const Draw*> pick;
      for (size_t k = 0; k < kv.second.size(); k += std::max<size_t>(1, kv.second.size() / 80)) pick.push_back(kv.second[k]);
      Player best;
      best.fit = -1;
      for (int sign = 1; sign >= -1; sign -= 2)
        for (long cand = 0; cand < 3000; ++cand) {
          int n = 0;
          for (const Draw* d : pick) {
            float r[12];
            const double t = sign * double(clock_of(d->guest)) - double(cand);
            if (motion_rows(act_for(*d), t, r) && RowsEqual(r, d->rows, 2e-3f)) ++n;
          }
          if (n > best.fit) best.fit = n, best.sign = sign, best.off = cand;
        }
      best.of = int(pick.size());
      best.ok = best.fit * 2 >= best.of && best.fit > 0;  // at least half of the sampled draws are exact
      players[kv.first] = best;
    }
  }
  const long off = players.empty() ? 0 : players.begin()->second.off;
  std::map<uint32_t, double> t_first;  // moving_static: the t each object first had
  std::set<std::pair<int, int>> sourced_draw;
  std::map<std::string, std::pair<int, int>> sum;  // family -> draws, sourced
  std::vector<std::string> lines;
  std::map<int, std::pair<int, int>> act_stats;  // moving: act -> draws, sourced (-1 = no act's position fits the draw)
  // pass A: build the table (what the writer holds)
  for (const Draw& d : draws) {
    ArenaWorld w;
    w.kind = kAwObject;
    w.object = d.object;
    w.frame = uint32_t(d.guest);
    bool have = false;
    if (d.family == "placed") {
      for (uint32_t s = kFirstPlaced; s <= kLastPlaced && !have; ++s) {
        float r[12];
        if (PlacedRows(arena, s, r) && RowsEqual(r, d.rows)) {
          std::memcpy(w.world, r, sizeof r);
          have = true;
        }
      }
      if (plant == "placed_identity") {  // the record's identity kept
        const float id[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        std::memcpy(w.world, id, sizeof id);
        have = true;
      }
    } else if (d.family == "moving") {
      const int act = act_for(d);
      if (act >= 0) {
        const Player& pl = players[d.object];
        if (!pl.ok) continue;  // no exact fit for this act: nothing the file + a clock can say
        double t = pl.sign * double(clock_of(d.guest)) - double(pl.off);
        if (plant == "moving_static") {
          auto it = t_first.find(d.object);
          if (it == t_first.end()) t_first[d.object] = t;
          else t = it->second;
        }
        float r[12];
        if (motion_rows(act, t, r)) std::memcpy(w.world, r, sizeof r), have = true;
      }
    } else if (d.family == "animated") {
      w.kind = kAwClock;
      float c = clock_of(d.guest);
      w.clock = c;
      w.origin = uint32_t(origin);
      have = have_origin;
      w.object = d.object;
    }
    if (!have) continue;
    if (first_of_frame.find(d.guest) == first_of_frame.end()) first_of_frame[d.guest] = table.size();
    table.push_back(w);
  }
  std::map<uint64_t, uint64_t> prev_guest;  // frame -> the frame before it in the input
  for (size_t k = 1; k < draws.size(); ++k)
    if (draws[k].guest != draws[k - 1].guest) prev_guest[draws[k].guest] = draws[k - 1].guest;
  // pass B: every draw joined back to the table by the hook's frame stamp (or, planted, by position) and compared with the stream's register
  for (const Draw& d : draws) {
    auto& s = sum[d.family];
    ++s.first;
    uint8_t kind = d.family == "animated" ? kAwClock : kAwObject;
    const ArenaWorld* e = nullptr;
    {
      // an entry per (frame stamp, object); the clock entry is per frame, joined by the stamp alone. stamp_off_by_one: the draw is joined at the PREVIOUS frame of the
      // input (g - 1 on a full capture): what joining one record off does.
      uint64_t stamp = d.guest;
      if (plant == "stamp_off_by_one") {
        auto it = prev_guest.find(d.guest);
        if (it == prev_guest.end()) continue;
        stamp = it->second;
      }
      e = FindEntry(table.data(), table.size(), uint32_t(stamp), d.object, kind);
    }
    if (!e) continue;
    bool ok = false;
    std::string regs;
    if (kind == kAwClock) {
      ok = std::fabs(d.c186 - e->clock) < 0.5f || std::fabs(d.c186 - 2.f * e->clock) < 0.5f;
      regs = "186";
    } else {
      ok = RowsEqual(e->world, d.rows, 2e-3f);
      regs = "76,77,78";
    }
    if (d.family == "moving") {
      auto& as = act_stats[act_for(d)];
      ++as.first;
      if (ok) ++as.second;
    }
    if (ok) {
      ++s.second;
      lines.push_back("S " + std::to_string(d.fi) + " " + std::to_string(d.i) + " " + regs);
    }
  }
  std::ofstream o(out);
  o << "FIT origin=" << (have_origin ? std::to_string(origin) : "none") << " off=" << off << " arena=" << arena << " plant=" << (plant.empty() ? "none" : plant) << "\n";
  for (auto& kv : sum) o << "SUMMARY " << kv.first << " " << kv.second.first << " " << kv.second.second << "\n";
  for (const std::string& l : lines) o << l << "\n";
  for (auto& kv : sum) std::printf("[oracle] %-9s draws %6d  the field would source %6d (%.2f%%)\n", kv.first.c_str(), kv.second.first, kv.second.second, kv.second.first ? 100.0 * kv.second.second / kv.second.first : 0.0);
  for (auto& kv : act_stats) std::printf("[oracle] moving act %3d: draws %5d sourced %5d\n", kv.first, kv.second.first, kv.second.second);
  for (auto& kv : players) std::printf("[oracle] player of object 0x%08X: %s sign %+d phase %ld, %d of %d sampled draws exact\n", kv.first, kv.second.ok ? "fit" : "no fit", kv.second.sign, kv.second.off, kv.second.fit, kv.second.of);
  std::printf("[oracle] fit: clock origin %s%ld (the moving pieces' phases are per slot, above), plant %s\n", have_origin ? "" : "(none) ", origin, plant.empty() ? "none" : plant.c_str());
  return 0;
}
