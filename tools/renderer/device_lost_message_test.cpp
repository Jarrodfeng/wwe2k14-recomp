// S105 A9: the player's word on a lost GPU device (src/device_lost_message.h) and the switch behind it (rdr_switches.h's table), on a fake settings store. Host clang, 0 game runs:
//   clang++ -std=gnu++20 -I src -I src/renderer_adapter tools/renderer/device_lost_message_test.cpp -o out/device_lost_message_test.exe
// Argument (the planted twins; each must exit 1):
//   "plant_switch_off"  the table default is 0 again (the v0.4.0 state)
//   "plant_text"        the PC text loses its report sentence
//   "plant_unknown_off" a build that does not define the setting shows no box (what the player build was)
// With no argument: PASS needs the box ON when the setting is unknown to the build (the stream-free build before its own copy existed must still tell the player), ON when the setting is
// true, OFF only when it is false; the text is the approved wording; the black-box line carries the prefix the phone's launcher reads.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

#include "device_lost_message.h"
#include "rdr_switches.h"

static std::map<std::string, std::string> store;
static bool ReadStore(const char* name, std::string& text, bool) {
  auto it = store.find(name);
  if (it == store.end()) return false;
  text = it->second;
  return true;
}
static void LogNone(const char*) {}

static int fails = 0;
static void Check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++fails;
}

// The switch table is one process-wide object with no reset (like the game's), so each scenario runs in its own process: the test starts itself with "scenario NAME [plant]".
static int RunScenario(const std::string& name, const std::string& plant) {
  namespace dlm = wwe2k14::device_lost_message;
  store.clear();
  if (name == "true") store["wwe2k14_rdr_device_lost_message"] = "true";
  if (name == "false") store["wwe2k14_rdr_device_lost_message"] = "false";
  rdr_sw::Refresh(1, ReadStore, LogNone, 1);
  const bool shown = dlm::ShouldShow(rdr_sw::Bool(rdr_sw::kDeviceLostMessage));
  if (name == "unknown") Check(plant == "plant_unknown_off" ? false : shown, "setting unknown to the build: the box is shown (default on)");
  if (name == "true") Check(shown, "setting true: shown");
  if (name == "false") Check(!shown, "setting false: not shown (a test run that must not put a box on a screen)");
  return fails ? 1 : 0;
}

int main(int argc, char** argv) {
  if (argc > 2 && std::string(argv[1]) == "scenario") return RunScenario(argv[2], argc > 3 ? argv[3] : "");
  const std::string plant = argc > 1 ? argv[1] : "";
  namespace dlm = wwe2k14::device_lost_message;

  // 1. the switch table's default, read the way OnSwap reads it (rdr_sw::Bool). plant_switch_off: the default is 0 again (the v0.4.0 state) -> must FAIL.
  const bool table_default = rdr_sw::Infos()[rdr_sw::kDeviceLostMessage].def != 0;
  Check(plant == "plant_switch_off" ? false : table_default, "the table default of wwe2k14_rdr_device_lost_message is ON");

  // 2/3. a build that does not know the setting must still show the box; true shows; false does not.
  for (const char* name : {"unknown", "true", "false"}) {
    const std::string cmd = std::string("\"") + argv[0] + "\" scenario " + name + " " + plant;
    Check(std::system(cmd.c_str()) == 0, (std::string("scenario ") + name).c_str());
  }

  // 4. the wording and the lines.
  std::string pc = dlm::kTextPc;
  if (plant == "plant_text") pc = pc.substr(0, pc.find(", then send a report"));
  Check(pc == "The graphics driver stopped. Please restart the game. If this keeps happening, update your graphics driver, then send a report (launcher: Report a problem).",
        "the PC text is the approved wording");
  Check(std::string(dlm::kTextPhone) == "The graphics driver stopped. Please restart the game. If this keeps happening, send a report from the launcher.", "the phone text is the approved wording");
  Check(dlm::LogLine(true, false) == "[wwe2k14][frames] message for the player: " + std::string(dlm::kTextPc), "the log line with the box shown");
  Check(dlm::LogLine(false, false).find("(not shown: wwe2k14_rdr_device_lost_message is off)") != std::string::npos, "the log line says when the box was not shown");
  Check(dlm::BlackboxLine(118220) == "GPU LOST: the graphics driver stopped (Vulkan device lost) at guest frame 118220", "the black-box line the phone's launcher reads");

  std::printf(fails ? "device_lost_message_test: FAIL (%d)\n" : "device_lost_message_test: PASS\n", fails);
  return fails ? 1 : 0;
}
