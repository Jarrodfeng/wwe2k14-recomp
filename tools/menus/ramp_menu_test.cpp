// S111 RAMPMENU: host unit for src/ramp_menu_core.h (the RAMP CAMERA row of the pause menu's VISUAL PRESENTATION page, the question's
// answer, the settings key). A planted pause-menu object (guest bytes, big-endian) stands in for the game's; synthetic values only.
// Prints "ramp_menu_test: ok N checks" and returns 0, else the failures and 1. tools/menus/ramp_menu_selftest.py builds it and its
// planted twins (a row written over a stock row, a count not raised, a swapped answer, a value not persisted), which must FAIL.
#include <cstdio>
#include <cstring>
#include <regex>
#include <string>
#include <vector>

#include "ramp_menu_core.h"

using namespace wwe2k14::ramp_menu;

static int g_n = 0, g_fail = 0;
static void Check(bool ok, const char* what) {
  ++g_n;
  if (!ok) ++g_fail, std::printf("FAIL: %s\n", what);
}

constexpr uint32_t kOurText = 0x7C000061u;  // gfx_menu.h kTextRamp (any id outside the stock seven would do)

// The page-3 menu as sub_82385340 leaves it before sub_82391928: vtable, 7 rows, action i, the stock texts; other words filled with a
// marker so a stray write is seen.
static std::vector<uint8_t> StockPage() {
  std::vector<uint8_t> m(688, 0xA5);
  PutBe32(m.data(), kPauseMenuVtable);
  PutBe32(m.data() + kMenuCount, kStockRows);
  for (uint32_t i = 0; i < kStockRows; ++i) {
    PutBe32(m.data() + kMenuActions + 4 * i, i);
    uint8_t* row = m.data() + kMenuRows + kRowSize * i;
    for (uint32_t k = 0; k < 16; k += 4) PutBe32(row + k, 0x100 * i + k);
    PutBe32(row + kRowText, kStockTexts[i]);
  }
  return m;
}

static bool ReadBool(const std::string& json, const char* key, bool& out) {  // an independent reader (gfx_settings.h GetBool's pattern)
  const std::regex re(std::string("\"") + key + "\"\\s*:\\s*(true|false)");
  std::smatch mt;
  if (!std::regex_search(json, mt, re)) return false;
  out = mt[1].str() == "true";
  return true;
}

int main() {
  // 1. the row goes after the seven; the seven, their actions and every other word stay as they were
  {
    std::vector<uint8_t> m = StockPage(), before = m;
    Check(StockVpWhy(m.data()).empty(), "the stock page is recognised");
    Check(AddVpRow(m.data(), kOurText), "the row is added to the stock page");
    Check(Be32(m.data() + kMenuCount) == 8, "the count is 8");
    Check(RowAction(m.data(), 7) == kRampAction && kRampAction == 7, "row 7 has action 7");
    Check(RowText(m.data(), 7) == kOurText, "row 7 has our text");
    for (uint32_t k = 0; k < 16; k += 4) Check(Be32(m.data() + kMenuRows + kRowSize * 7 + k) == 0, "row 7's other words are 0");
    Check(std::memcmp(m.data() + kMenuRows, before.data() + kMenuRows, kRowSize * kStockRows) == 0, "the seven stock rows are unchanged");
    Check(std::memcmp(m.data() + kMenuActions, before.data() + kMenuActions, 4 * kStockRows) == 0, "the seven stock actions are unchanged");
    for (uint32_t i = 0; i < kStockRows; ++i) Check(RowText(m.data(), i) == kStockTexts[i], "a stock row keeps its text at its index");
    size_t changed_outside = 0;
    for (size_t b = 0; b < m.size(); ++b) {
      const bool ours = (b >= kMenuCount && b < kMenuCount + 4) || (b >= kMenuActions + 28 && b < kMenuActions + 32) ||
                        (b >= kMenuRows + kRowSize * 7 && b < kMenuRows + kRowSize * 8);
      if (!ours && m[b] != before[b]) ++changed_outside;
    }
    Check(changed_outside == 0, "nothing outside the count, action 7 and row 7 is written");
    Check(kMenuRows + kRowSize * kMaxRows <= 388 && kMenuActions + 4 * kMaxRows <= kMenuRows, "row 7 fits in the 8-row arrays");
    Check(HasRampRow(m.data(), kOurText), "the page holds our row");
    Check(!StockVpWhy(m.data()).empty() && !AddVpRow(m.data(), kOurText), "a page that already has 8 rows is not added to again");
    Check(Be32(m.data() + kMenuCount) == 8, "the count stays 8 on the second call");
  }
  // 2. anything that is not the stock page is left alone
  {
    std::vector<uint8_t> m = StockPage();
    PutBe32(m.data(), 0x8200DD40);  // the ON / OFF question's vtable
    std::vector<uint8_t> before = m;
    Check(!AddVpRow(m.data(), kOurText) && m == before, "another class: refused, nothing written");
    m = StockPage();
    PutBe32(m.data() + kMenuCount, 4);  // MATCH INFORMATION's four rows
    before = m;
    Check(!AddVpRow(m.data(), kOurText) && m == before, "another row count: refused, nothing written");
    m = StockPage();
    PutBe32(m.data() + kMenuRows + kRowSize * 6 + kRowText, 2379);  // the main list's QUIT MATCH in the last place
    before = m;
    Check(!AddVpRow(m.data(), kOurText) && m == before, "another page's texts: refused, nothing written");
    m = StockPage();
    PutBe32(m.data() + kMenuActions + 4 * 3, 4);
    before = m;
    Check(!AddVpRow(m.data(), kOurText) && m == before, "an action that is not its index: refused");
    m = StockPage();
    Check(!HasRampRow(m.data(), kOurText), "the stock page does not hold our row");
  }
  // 3. the answer: 0 = ON, 1 = OFF, anything else keeps the value; the question opens on the current value
  Check(AnswerToValue(0, false) == true, "answer 0 = ON");
  Check(AnswerToValue(1, true) == false, "answer 1 = OFF");
  Check(AnswerToValue(0, true) == true && AnswerToValue(1, false) == false, "the same answer twice keeps it");
  Check(AnswerToValue(2, true) == true && AnswerToValue(2, false) == false, "answer 2 keeps the value");
  Check(AnswerToValue(0xFFFFFFFFu, true) == true && AnswerToValue(0xFFFFFFFFu, false) == false, "answer -1 keeps the value");
  // S111 RAMPMENU review F1: our question's answer reaches the game's flow state 4 as kAnswerNoWrite, a value it writes nothing for
  // (sub_8235CBE0 +34..+50 writes CAMERA CUTS' byte for 0 and 1 only), so CAMERA CUTS' setting is never written, not even for a moment.
  Check(kAnswerNoWrite != 0 && kAnswerNoWrite != 1, "the answer handed to the game is one it writes nothing for (not 0, not 1)");
  Check(AnswerToValue(kAnswerNoWrite, true) == true && AnswerToValue(kAnswerNoWrite, false) == false,
        "the no-write answer, read as ours, keeps the value");
  Check(QuestionByte(true) == 1 && QuestionByte(false) == 0, "the question opens on ON when ON (byte 1), else on OFF");
  // 4. the settings key: set or added, the rest of the file kept, read back by an independent reader
  {
    bool v = false;
    std::string j = SetBoolKey("", "ramp_camera", true);
    Check(ReadBool(j, "ramp_camera", v) && v, "no file: the key is written ON");
    j = SetBoolKey("{\n}\n", "ramp_camera", true);
    Check(ReadBool(j, "ramp_camera", v) && v && j.find(",") == std::string::npos, "an empty object: the key added, no stray comma");
    const std::string file = "{\n  \"upscale\": 0,\n  \"photo_mode\": true,\n  \"resolution\": 1\n}\n";
    j = SetBoolKey(file, "ramp_camera", true);
    Check(ReadBool(j, "ramp_camera", v) && v, "an existing file: the key added ON");
    bool p = false;
    Check(ReadBool(j, "photo_mode", p) && p && j.find("\"upscale\": 0,") != std::string::npos &&
              j.find("\"resolution\": 1") != std::string::npos,
          "the other keys are kept");
    j = SetBoolKey(j, "ramp_camera", false);
    Check(ReadBool(j, "ramp_camera", v) && !v, "ON then OFF: the key reads OFF");
    j = SetBoolKey(j, "ramp_camera", true);
    Check(ReadBool(j, "ramp_camera", v) && v, "OFF then ON: the key reads ON");
    size_t count = 0;
    for (size_t at = j.find("\"ramp_camera\""); at != std::string::npos; at = j.find("\"ramp_camera\"", at + 1)) ++count;
    Check(count == 1, "the key appears once after three writes");
    Check(ReadBool(j, "photo_mode", p) && p, "photo_mode untouched by our key");
  }
  // 5. S111 RAMPMENU review F3 / F4 (main session decisions): the switch's start value, in one rule: (1) replay playback: the recording
  // decides, the file is not read; (2) a command-line value wins; (3) else the file, only where the safety rule allows (a release build,
  // or --wwe2k14_settings_file given, or use_launcher_settings on: a +dev build never applies the player's file by itself).
  {
    const std::string on_file = "{\n  \"photo_mode\": false,\n  \"ramp_camera\": true\n}\n";
    const std::string off_file = "{\n  \"ramp_camera\": false\n}\n";
    const std::string no_key = "{\n  \"photo_mode\": true\n}\n";
    RampStart release;  // a release exe, nothing given
    release.dev_build = false;
    RampStart dev;  // a +dev exe, no settings file, use_launcher_settings off
    dev.dev_build = true;
    bool on = false;
    Check(StartDecision(release, on_file, on) == StartFrom::kFile && on, "a release build, no command-line value + key true: ON");
    on = true;
    Check(StartDecision(release, off_file, on) == StartFrom::kFile && !on, "a release build + key false: OFF");
    on = false;
    RampStart rp = release;
    rp.replay_playback = true;
    Check(StartDecision(rp, on_file, on) == StartFrom::kRecording && !on, "replay playback + key true: not applied by us (the recording decides)");
    RampStart cl = release;
    cl.command_line = true;
    Check(StartDecision(cl, on_file, on) == StartFrom::kCommandLine && !on, "command-line false + key true: the command line wins (OFF)");
    Check(StartDecision(dev, on_file, on) == StartFrom::kNotAllowed && !on, "a +dev build, no settings file + key true: the file is not read (OFF)");
    RampStart dev_file = dev;
    dev_file.settings_file_given = true;
    Check(StartDecision(dev_file, on_file, on) == StartFrom::kFile && on, "a +dev build with --wwe2k14_settings_file + key true: ON");
    on = false;
    RampStart dev_uls = dev;
    dev_uls.use_launcher_settings = true;
    Check(StartDecision(dev_uls, on_file, on) == StartFrom::kFile && on, "a +dev build with use_launcher_settings on + key true: ON");
    on = false;
    Check(StartDecision(release, no_key, on) == StartFrom::kNoKey && !on, "no key in the file: the switch keeps its default");
    Check(StartDecision(release, "", on) == StartFrom::kNoKey && !on, "no file: the switch keeps its default");
    RampStart rp_cl = cl;
    rp_cl.replay_playback = true;
    Check(StartDecision(rp_cl, on_file, on) == StartFrom::kRecording, "replay playback comes first (before the command line)");
    bool opened = false;
    StartDecisionRead(dev, [&] { opened = true; return on_file; }, on);
    Check(!opened, "a +dev build without a settings file never opens the player's file");
    opened = false;
    StartDecisionRead(rp, [&] { opened = true; return on_file; }, on);
    Check(!opened, "replay playback never opens the file");
  }
  if (g_fail) {
    std::printf("ramp_menu_test: %d of %d checks FAILED\n", g_fail, g_n);
    return 1;
  }
  std::printf("ramp_menu_test: ok %d checks\n", g_n);
  return 0;
}
