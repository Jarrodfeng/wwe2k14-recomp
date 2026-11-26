// S111 RAMPCAM: the unit test of src/ramp_camera_core.h (the ramp camera's side hold and its Royal Rumble pose), of the replay carry of the
// switch (src/replay_carried.h) and of the GAME's stick rule, modelled from the listings, that the controls rest on (we change no pad code:
// the test shows the game's rule turns the stick with the camera's side). No game, no SDK. Built by tools\camera\build_ramp_camera_test.ps1.
//   ramp_camera_test.exe                               every case; exit 0 = all pass
//   ramp_camera_test.exe --plant side_ignored          the hold leaves the field as it is: the side cases must FAIL
//   ramp_camera_test.exe --plant stick_not_rotated     the modelled pad ignores the heading: the stick cases must FAIL
//   ramp_camera_test.exe --plant rumble_gated          the old gate (no hold in a Royal Rumble): the Rumble cases must FAIL
//   ramp_camera_test.exe --plant no_forget             the camera's ctor does not end an earlier hold at its address: the ctor case must FAIL
//   ramp_camera_test.exe --plant old_not_off           a recording without the line keeps the player's value: the replay cases must FAIL
//   ramp_camera_test.exe --plant command_line_wins     the player's given value beats the recording's: the replay cases must FAIL
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "../../src/ramp_camera_core.h"
#include "../../src/replay_carried.h"

using namespace wwe2k14::ramp_camera;

namespace {

bool g_plant_side = false, g_plant_stick = false, g_plant_rumble = false, g_plant_forget = false, g_plant_old = false,
     g_plant_cmd = false;
int g_checks = 0, g_failed = 0;

void Check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAIL %s\n", what);
  }
}

uint32_t DoHold(SideHold& s, uint32_t cam, uint32_t field, bool want) { return g_plant_side ? field : Hold(s, cam, field, want); }
uint32_t DoLend(SideHold& s, uint32_t cam, uint32_t field) { return g_plant_side ? field : GiveBack(s, cam, field); }

constexpr uint32_t kCamA = 0x40001000u, kCamB = 0x40002000u;

void SideCases() {
  {  // switch off: the field is the game's
    SideHold s;
    Check(DoHold(s, kCamA, 0, false) == 0 && !s.held, "off: side 0 stays 0");
  }
  {  // switch on over the hard cam
    SideHold s;
    Check(DoHold(s, kCamA, 0, true) == kRampSide, "on: side 0 becomes the ramp side");
    Check(s.game_side == 0, "on: the game's own side 0 remembered");
    // a director hand-back forces 0 between two updates: the next update holds the ramp side again
    Check(DoHold(s, kCamA, 0, true) == kRampSide, "hand-back: the forced 0 becomes the ramp side again");
    // a motion activation: the game reads its own side, advances to 1; we hold again after it
    Check(DoLend(s, kCamA, kRampSide) == 0, "motion activation: the game gets its own side 0 for the call");
    Check(DoHold(s, kCamA, 1, true) == kRampSide && s.game_side == 1, "motion activation: the game's next side 1 remembered, ramp held");
    // switching off: the game's own side comes back, not the ramp side
    Check(DoHold(s, kCamA, kRampSide, false) == 1 && !s.held, "off: the game's own side 1 comes back");
  }
  {  // switching off when the game wrote a side since our last hold: the game's write stands
    SideHold s;
    DoHold(s, kCamA, 0, true);
    Check(DoHold(s, kCamA, 3, false) == 3, "off after a game write: the game's side 3 stands");
  }
  {  // HIAC keep path: the re-pick writes nothing and the game's own side stays what it was
    SideHold s;
    DoHold(s, kCamA, 1, true);
    Check(DoLend(s, kCamA, kRampSide) == 1, "HIAC re-pick: the game's own side 1 for the call");
    Check(DoHold(s, kCamA, 1, true) == kRampSide && s.game_side == 1, "HIAC re-pick wrote nothing: side 1 kept as the game's");
  }
  {  // a new camera object (a new match) while held: its own field is the game's
    SideHold s;
    DoHold(s, kCamA, 0, true);
    Check(DoHold(s, kCamB, 0, true) == kRampSide && s.cam == kCamB && s.game_side == 0, "new camera: held on the new object");
    Check(DoHold(s, kCamB, kRampSide, false) == 0, "new camera, off: its own side 0 back");
  }
  {  // a new camera object at the SAME address (the heap gave the next match's camera the old one's block; its ctor ran): with the switch off
     // (or no ring) its own side 2 (the preset's no-ring side, sub_82579A20 +0x3F8) stays; the old match's side 0 is not written over it
    SideHold s;
    DoHold(s, kCamA, 0, true);
    if (!g_plant_forget) Forget(s, kCamA);
    Check(DoHold(s, kCamA, kRampSide, false) == kRampSide && !s.held, "ctor at the held address: the new object's own side 2 stays");
  }
}

// Royal Rumble (Jarrod 2026-11-26 21:5x: the ramp camera applies there too). The game's side table 0x83621694 (q words 0x83621694 4 --as f32).
const float kTable[4] = {1.5707964f, 3.1415927f, 0.0f, 4.712389f};

struct Cam {
  uint32_t side;     // +1124
  float yaw;         // +416 (sub_8257ADD8 sets it and +1748 to the posed yaw)
  uint32_t no_side;  // +420
  uint32_t outside;  // +1864
};

// One Apply as the glue runs it (src/ramp_camera.h): Decide on what the camera holds, store the side, pose when asked. The rumble_gated plant
// is the old glue: Wanted said no in a Royal Rumble (the cvar read as off there).
Applied Step(SideHold& s, uint32_t cam, Cam& c, bool on, bool ring, bool rumble) {
  const bool seen_on = (g_plant_rumble && rumble) ? false : on;
  const CameraRead r{c.side, c.yaw, c.no_side, c.outside};
  const Applied a = Decide(s, cam, r, seen_on, ring, rumble, kTable);
  c.side = a.side;
  if (a.pose) c.yaw = a.pose_yaw;
  return a;
}

void RumbleCases() {
  {  // a Rumble record at the bell: side 0, the preset's yaw pi/2 (sub_82579A20 +0x404), the switch on
    SideHold s;
    Cam c{0, kTable[0], 0, 0};
    Applied a = Step(s, kCamA, c, true, true, true);
    Check(c.side == kRampSide && a.pose && c.yaw == kTable[kRampSide], "Rumble: the record ends on side 2, posed at its yaw");
    a = Step(s, kCamA, c, true, true, true);
    Check(c.side == kRampSide && !a.pose, "Rumble: no pose on a frame whose yaw already is the side's (not per frame)");
    // a director hand-back: the re-pick forces side 0 and poses at table[0] (sub_8257BD88 +0x130, +0x13C..+0x1D4); the next update re-poses
    c.side = 0;
    c.yaw = kTable[0];
    a = Step(s, kCamA, c, true, true, true);
    Check(c.side == kRampSide && a.pose && c.yaw == kTable[kRampSide], "Rumble hand-back: back on side 2, posed again");
    // the outside flag set: the game's side step would wait (sub_8257AE98 +0xD4), so do we; it clears: the pose comes
    c.yaw = kTable[0];
    c.outside = 1;
    a = Step(s, kCamA, c, true, true, true);
    Check(c.side == kRampSide && !a.pose && c.yaw == kTable[0], "Rumble outside: held, no pose while +1864 is set");
    c.outside = 0;
    a = Step(s, kCamA, c, true, true, true);
    Check(a.pose && c.yaw == kTable[kRampSide], "Rumble back in: posed on side 2");
    // +420 set (the replay copy's flag, sub_8257BF88): the game's side step is off, so is our pose
    c.yaw = kTable[0];
    c.no_side = 1;
    a = Step(s, kCamA, c, true, true, true);
    Check(!a.pose, "Rumble +420 set: no pose (the game's own gate)");
    c.no_side = 0;
    // switching off: the game's own side 0 comes back, posed at its yaw the same way
    c.yaw = kTable[kRampSide];
    a = Step(s, kCamA, c, false, true, true);
    Check(c.side == 0 && a.pose && c.yaw == kTable[0] && a.ended && !s.held, "Rumble off: side 0 back, posed at its yaw pi/2");
    a = Step(s, kCamA, c, false, true, true);
    Check(!a.pose && c.side == 0, "Rumble off: nothing more after the switch-off frame");
  }
  {  // not a Rumble: the hold only; the game's own side step poses
    SideHold s;
    Cam c{0, kTable[0], 0, 0};
    const Applied a = Step(s, kCamA, c, true, true, false);
    Check(c.side == kRampSide && !a.pose && a.started, "normal match: side 2 held, no pose of ours");
  }
  {  // no ring: nothing
    SideHold s;
    Cam c{2, 0.0f, 0, 0};
    const Applied a = Step(s, kCamA, c, true, false, false);
    Check(!s.held && !a.pose && c.side == 2, "no ring: nothing held, nothing posed");
  }
}

// The replay carry (src/replay_carried.h; what src/replay.h OpenRecord writes and PreSetup / Want apply).
std::vector<std::pair<std::string, std::string>> Parse(const std::string& header) {
  std::vector<std::pair<std::string, std::string>> flags;
  size_t at = 0;
  while (at < header.size()) {
    const size_t e = header.find('\n', at);
    std::pair<std::string, std::string> kv;
    if (wwe2k14::replay::ParseFlagLine(header.substr(at, e == std::string::npos ? std::string::npos : e - at), kv)) flags.push_back(kv);
    at = e == std::string::npos ? header.size() : e + 1;
  }
  return flags;
}

// The switch's value during playback of `header` for a player whose own value is `player` (`given` = on the command line / by the settings file).
std::string Played(const std::string& header, const std::string& player, bool given) {
  using namespace wwe2k14::replay;
  auto flags = Parse(header);
  if (!g_plant_old) AddOldRecordingDefaults(flags);
  std::string v = player;
  for (auto& kv : flags) {
    if (kv.first != kRampCamera) continue;
    const bool sets = g_plant_cmd ? !given : ReplaySets(kv.first, given);
    if (sets) v = kv.second;
  }
  return v;
}

void ReplayCases() {
  using namespace wwe2k14::replay;
  bool listed = false;
  for (const char* n : kAlwaysWritten) listed = listed || std::string(n) == kRampCamera;
  Check(listed && HeaderLine(kRampCamera, "false", false) == "flag wwe2k14_ramp_camera false\n",
        "replay: the switch is always written, a flag line even at its default");
  const std::string head = "# wwe2k14 replay v2\nname t\nseed 5\ndlc none\nflag det_frames true\n";
  const std::string on = head + HeaderLine(kRampCamera, "true", false), off = head + HeaderLine(kRampCamera, "false", false);
  Check(Played(head, "true", true) == "false", "replay: an old recording (no line) plays OFF with the switch given on");
  Check(Played(head, "false", false) == "false", "replay: an old recording plays OFF at the default");
  Check(Played(on, "false", false) == "true", "replay: a recording made ON plays ON");
  Check(Played(on, "false", true) == "true", "replay: a recording made ON plays ON over a given off");
  Check(Played(off, "true", true) == "false", "replay: a recording made OFF plays OFF over a given on");
  Check(!ReplaySets("det_frames", true) && ReplaySets("det_frames", false), "replay: other carried settings keep the player's given value");
}

// The game's stick rule, modelled from the listings: heading = atan2(view m[2], view m[10]) of the key-0 camera (sub_823C7FB0 +0x50..+0x94,
// view words +24 / +56 = the forward's x and z); the forward of a camera at yaw y is (sin y, 0, cos y) (sub_82C32DE0 row 2, row vectors);
// octant = bits 9..11 of int((wrap(code * pi/4 + heading + pi/8) + 2 pi) * 651.8986) (sub_82610680 +0x38..+0xB0).
constexpr double kPi = 3.14159265358979323846;

double Yaw(uint32_t side) {
  const double t[4] = {kPi / 2, kPi, 0.0, 3 * kPi / 2};  // 0x83621694
  return t[side & 3];
}

double Heading(uint32_t side) { return std::atan2(std::sin(Yaw(side)), std::cos(Yaw(side))); }

int Octant(int code, double heading) {
  double a = code * 8192 * 9.58738e-05 + (g_plant_stick ? 0.0 : heading) + 0.3926991;
  while (a > kPi) a -= 2 * kPi;
  while (a < -kPi) a += 2 * kPi;
  const long v = static_cast<long>((a + 6.2831855) * 651.8986);
  return static_cast<int>((v >> 9) & 7);
}

void StickCases() {
  // Side 2's heading is 90 deg below side 0's: every stick octant turns by two (6 mod 8), so a push keeps its direction on screen.
  bool all = true;
  for (int code = -4; code <= 3; ++code) all = all && Octant(code, Heading(kRampSide)) == ((Octant(code, Heading(0)) + 6) & 7);
  Check(all, "stick: the ramp side turns every octant by 90 deg against the hard cam");
  bool moved = false;
  for (int code = -4; code <= 3; ++code) moved = moved || Octant(code, Heading(kRampSide)) != Octant(code, Heading(0));
  Check(moved, "stick: the same push is a different world direction on the ramp side");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 2 && std::strcmp(argv[1], "--plant") == 0) {
    const char* p = argv[2];
    g_plant_side = std::strcmp(p, "side_ignored") == 0;
    g_plant_stick = std::strcmp(p, "stick_not_rotated") == 0;
    g_plant_rumble = std::strcmp(p, "rumble_gated") == 0;
    g_plant_forget = std::strcmp(p, "no_forget") == 0;
    g_plant_old = std::strcmp(p, "old_not_off") == 0;
    g_plant_cmd = std::strcmp(p, "command_line_wins") == 0;
    if (!g_plant_side && !g_plant_stick && !g_plant_rumble && !g_plant_forget && !g_plant_old && !g_plant_cmd) {
      std::printf("unknown plant %s (side_ignored | stick_not_rotated | rumble_gated | no_forget | old_not_off | command_line_wins)\n", p);
      return 2;
    }
  }
  SideCases();
  RumbleCases();
  ReplayCases();
  StickCases();
  std::printf("ramp_camera_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
