// S108 lane MOB: the shared redaction (src\launcher\report_redact.h), the CPU blocks (src\launcher_mobile\cpu_block.h, src\launcher\cpu_report.h).
// Every text here is made up by this file (no real log, no real user name). Build + run: tools\report\run_redact_unit.py (it also plants faults).
//   clang++ -std=c++20 -I src/launcher -I src/launcher_mobile tools/report/redact_unit.cpp -o redact_unit.exe && redact_unit.exe [tempdir]
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "cpu_block.h"
#include "cpu_report.h"
#include "report_redact.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#endif

namespace fs = std::filesystem;
using report_redact::PcRules;
using report_redact::PhoneRules;
using report_redact::Redact;

static int fails = 0, checks = 0;
#define CHECK(c, ...)                \
  do {                               \
    ++checks;                        \
    if (!(c)) {                      \
      ++fails;                       \
      std::printf("FAIL %s: ", #c);  \
      std::printf(__VA_ARGS__);      \
      std::printf("\n");             \
    }                                \
  } while (0)

static bool Has(const std::string& t, const std::string& s) { return t.find(s) != std::string::npos; }
// redacted text == expected
#define EQ(rules, in, want)                                                                                                    \
  do {                                                                                                                          \
    const std::string got_ = Redact(in, rules);                                                                                 \
    CHECK(got_ == (want), "in [%s] got [%s] want [%s]", std::string(in).c_str(), got_.c_str(), std::string(want).c_str());       \
    CHECK(Redact(got_, rules) == got_, "not idempotent: [%s] -> [%s]", got_.c_str(), Redact(got_, rules).c_str());              \
  } while (0)

// The launcher's OLD inline Redact (launcher_main.cpp before S108), kept verbatim as the reference (the profile is a parameter instead of the environment).
static std::string Lower(std::string s) {
  for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
static std::string OldRedact(std::string t, const std::string& prof) {
  if (prof.empty()) return t;
  for (const std::string& needle : {prof, Lower(prof), [&] {
         std::string x = prof;
         std::replace(x.begin(), x.end(), '\\', '/');
         return x;
       }()}) {
    if (needle.size() < 4) continue;
    for (size_t at = 0; (at = t.find(needle, at)) != std::string::npos;) t.replace(at, needle.size(), "%USERPROFILE%");
  }
  const size_t slash = prof.find_last_of('\\');
  const std::string user = Lower(slash == std::string::npos ? prof : prof.substr(slash + 1));
  if (user.size() >= 3) {
    const std::string low = Lower(t);
    std::string out;
    size_t from = 0;
    auto word = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; };
    for (size_t at = low.find(user); at != std::string::npos; at = low.find(user, at + 1)) {
      const bool left = at == 0 || !word(low[at - 1]), right = at + user.size() >= low.size() || !word(low[at + user.size()]);
      if (!left || !right || at < from) continue;
      out.append(t, from, at - from).append("USER");
      from = at + user.size();
    }
    t = out.append(t, from, std::string::npos);
  }
  return t;
}

static void WriteFile(const fs::path& p, const std::string& s) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary) << s;
}

static void PcTests() {
  const std::string prof = "C:\\Users\\Zedname";
  const auto rules = PcRules(prof);
  // the old behaviour, on a fixture of its old inputs (fragments a PC log / about.txt / settings json holds; none of them has a new shape)
  const std::vector<std::string> frag = {
      "C:\\Users\\Zedname\\AppData\\Local\\wwe\\logs\\wwe2k14_001.log", "c:\\users\\zedname\\documents", "C:/Users/Zedname/Videos/x.mp4",
      "C--Users-Zedname-temp-run", "Zedname", "zedname's laptop", "ZEDNAME", "Zednamer is not the name", "xZedname", "name=Zedname;",
      "D:\\Games\\WWE\\game.exe", "GPU: NVIDIA GeForce RTX 4070 (12282 MB)", "driver 31.0.15.4601 (10-14-2024)", "Windows 11 Pro (build 26100, 24H2)",
      "[wwe2k14][match] card 1: 343 vs 324", "10.0.26100.1", "CPU: AMD Ryzen 7 5800X 8-Core Processor", "", " ", "\n", "\t", "{\"a\": \"C:\\\\Users\\\\Zedname\\\\x\"}",
      "S:\\share\\folder", "version 1.0.0.2", "100% done", "a@b", "http://example.com/x", "C:\\Users\\Zedname", "C:\\Users\\Zedname\\"};
  uint32_t seed = 12345;
  auto next = [&] { return seed = seed * 1664525u + 1013904223u; };
  for (int i = 0; i < 4000; ++i) {
    std::string t;
    const int n = 1 + int(next() >> 24) % 8;
    for (int k = 0; k < n; ++k) t += frag[(next() >> 16) % frag.size()] + (next() & 0x10000 ? " " : (next() & 0x20000 ? "\n" : ""));
    const std::string want = OldRedact(t, prof), got = Redact(t, rules);
    if (want != got) {
      CHECK(false, "old != new on [%s]: old [%s] new [%s]", t.c_str(), want.c_str(), got.c_str());
      break;
    }
    ++checks;
  }
  CHECK(Redact("C:\\Users\\Zedname\\x", rules) == "%USERPROFILE%\\x", "profile");
  // no USERPROFILE: the profile passes are off (the three new patterns still run)
  EQ(PcRules(""), "C:\\Users\\Zedname\\x", "C:\\Users\\OTHERUSER\\x");

  // the new PC patterns
  EQ(rules, "dump at C:\\Users\\Other Person\\AppData\\Local\\Temp\\a.dmp", "dump at C:\\Users\\OTHERUSER\\AppData\\Local\\Temp\\a.dmp");
  EQ(rules, "d:/users/bob/Desktop/x.txt", "d:/users/OTHERUSER/Desktop/x.txt");
  EQ(rules, "{\"p\": \"C:\\\\Users\\\\Bob\\\\x\"}", "{\"p\": \"C:\\\\Users\\\\OTHERUSER\\\\x\"}");
  EQ(rules, "C:\\Users\\Public\\Documents\\x", "C:\\Users\\Public\\Documents\\x");
  EQ(rules, "C:\\Users\\Default\\NTUSER.DAT and C:\\Users\\All Users\\x", "C:\\Users\\Default\\NTUSER.DAT and C:\\Users\\All Users\\x");
  EQ(rules, "C:\\Users\\Bob", "C:\\Users\\OTHERUSER");
  EQ(rules, "C:\\Users\\Bob read it", "C:\\Users\\OTHERUSER read it");
  EQ(rules, "saved to \\\\nas-01\\media\\wwe\\saves\\slot1.bin ok", "saved to \\\\<host>\\<share>\\slot1.bin ok");
  EQ(rules, "\\\\nas-01\\media", "\\\\<host>\\<share>");
  EQ(rules, "\\\\nas-01", "\\\\<host>");
  EQ(rules, "{\"p\":\"\\\\\\\\nas-01\\\\media\\\\a\\\\b.bin\"}", "{\"p\":\"\\\\\\\\<host>\\\\<share>\\\\b.bin\"}");
  EQ(rules, "\\\\?\\C:\\long\\path and \\\\.\\pipe\\x", "\\\\?\\C:\\long\\path and \\\\.\\pipe\\x");
  EQ(rules, "contact: jane.doe+wwe@mail.example.org, thanks", "contact: <email>, thanks");
  EQ(rules, "frame d3d11@nvwgf2umx.dll and v@0615.0 and name@host", "frame d3d11@nvwgf2umx.dll and v@0615.0 and name@host");
  // S108 review: a text that starts with "users" (OtherProfiles read the character before index 0; run_redact_unit.py builds this unit with
  // the checked STL on Windows, _ITERATOR_DEBUG_LEVEL=1, which stops the process at such a read)
  EQ(PcRules(""), "Users\\Bob\\x and C:\\Users\\Bob\\y", "Users\\Bob\\x and C:\\Users\\OTHERUSER\\y");
  EQ(rules, "users: 3 signed in at C:\\Users\\Bob now", "users: 3 signed in at C:\\Users\\OTHERUSER now");
  // hardware lines stay (PC)
  const std::string hw = "GPU: NVIDIA GeForce RTX 4070 (12282 MB)\nMonitor: 3440x1440 at 165 Hz\nCPU: 13th Gen Intel(R) Core(TM) i7-13700K\nRAM: 32 GB\n"
                         "Windows: Windows 11 Pro (build 26100, 24H2)\n  NVIDIA GeForce RTX 4070: driver 31.0.15.4601 (10-14-2024)\n  AMD driver 23.40.27.6\nCPU threads: 24, cores: 16\n";
  EQ(rules, hw, hw);
  // a line that has both the user name outside any path and a second profile
  EQ(rules, "Zedname opened C:\\Users\\Other\\x", "USER opened C:\\Users\\OTHERUSER\\x");
}

static void PhoneTests() {
  const auto r = PhoneRules();
  EQ(r, "files /data/user/0/com.example.wwe/files/logs/wwe2k14_007.log", "files <path>/wwe2k14_007.log");
  EQ(r, "files /data/user/10/com.example.wwe/files", "files <path>/files");
  EQ(r, "/data/user/0/com.example.wwe", "<path>");
  EQ(r, "/data/user/0", "<path>");
  EQ(r, "x=/data/user_de/0/com.example.wwe/cache/a.tmp;", "x=<path>/a.tmp;");
  EQ(r, "/data/data/com.example.wwe/files/logs/", "<path>/logs/");
  // S108 MOB review: a folder path ending in '/' keeps its last folder's name (android_driver.h logs the custom driver's folder this way:
  // "[driver] opening NAME from DIR/", the folder named after the driver the player imported); a bare root with a '/' is still only <path>
  EQ(r, "[driver] opening libvulkan_freedreno.so from /data/user/0/com.example.wwe/files/drivers/v849/",
     "[driver] opening libvulkan_freedreno.so from <path>/v849/");
  EQ(r, "wwe2k14_android_tmp_dir = /data/user/0/com.example.wwe/cache/", "wwe2k14_android_tmp_dir = <path>/cache/");
  EQ(r, "/data/user/0/com.example.wwe/ and /storage/emulated/0/", "<path> and <path>");
  EQ(r, "open(/data/data/com.example.wwe/files/settings.json): failed", "open(<path>/settings.json): failed");
  EQ(r, "at /data/data/com.example.wwe/files/a.log.", "at <path>/a.log.");
  EQ(r, "/storage/emulated/0/Download/WWE 2K14/default.xex is bad", "<path>/default.xex is bad");
  EQ(r, "/storage/emulated/0/Download/default.xex", "<path>/default.xex");
  EQ(r, "/storage/emulated/0", "<path>");
  EQ(r, "/storage/emulated/11/Android/data/com.example.wwe/files/a.png", "<path>/a.png");
  EQ(r, "/storage/1A2B-3C4D/WWE/game.iso", "<path>/game.iso");
  EQ(r, "/storage/1A2B-3C4D", "<path>");
  EQ(r, "/sdcard/Music/John's Mix/song one.mp3 played", "<path>/song one.mp3 played");
  EQ(r, "read /sdcard/Download/x y.iso now", "read <path>/x y.iso now");
  EQ(r, "\"path\": \"/sdcard/Download/My Games/wwe.iso\", \"b\": 1", "\"path\": \"<path>/wwe.iso\", \"b\": 1");
  EQ(r, "/mnt/user/0/emulated/0/Download/a.bin", "<path>/a.bin");
  EQ(r, "/mnt/user/0", "<path>");
  EQ(r, "/mnt/media_rw/ABCD-1234/WWE/x.iso", "<path>/x.iso");
  EQ(r, "file:///sdcard/Download/a.png", "file://<path>/a.png");
  EQ(r, "/data/data/com.example.wwe/files /storage/emulated/0/x.txt", "<path>/files <path>/x.txt");
  // not Android paths
  EQ(r, "/proc/self/fd/57 /sys/devices/system/cpu/cpu0/cpufreq /system/lib64/libc.so /vendor/lib64/egl/libGLES_mali.so /data/app/~~abc==/com.example-xyz==/lib/arm64/libmain.so",
     "/proc/self/fd/57 /sys/devices/system/cpu/cpu0/cpufreq /system/lib64/libc.so /vendor/lib64/egl/libGLES_mali.so /data/app/~~abc==/com.example-xyz==/lib/arm64/libmain.so");
  EQ(r, "x/data/data/y notastorage/sdcard", "x/data/data/y notastorage/sdcard");
  // content:// addresses
  EQ(r, "game files: content://com.android.externalstorage.documents/tree/primary%3AWWE%20Backup/document/primary%3AWWE%20Backup%2FGame%2Fdefault.xex (ok)",
     "game files: content://com.android.externalstorage.documents/<uri>/default.xex (ok)");
  EQ(r, "picked content://media/picker/0/com.android.providers.media.photopicker/media/1234", "picked content://media/<uri>/1234");
  EQ(r, "content://authority", "content://authority");
  // other identifiers
  EQ(r, "mail jane.doe@example.com done", "mail <email> done");
  EQ(r, "wlan0 aa:bb:cc:dd:ee:ff and 0A-1B-2C-3D-4E-5F", "wlan0 <mac> and <mac>");
  EQ(r, "ip 192.168.1.23 and 10.0.0.5:8080 and 8.8.8.8.", "ip <ip> and <ip>:8080 and <ip>.");
  EQ(r, "127.0.0.1:47140 0.0.0.2", "127.0.0.1:47140 0.0.0.2");
  EQ(r, "version 1.2.3 build 5.15.74-android13-8 r38p1 V@0615.0 1.2.3.4.5 a1.2.3.4", "version 1.2.3 build 5.15.74-android13-8 r38p1 V@0615.0 1.2.3.4.5 a1.2.3.4");
  EQ(r, "time 12:34:56:78 guid 123e4567-e89b-12d3-a456-426614174000 id 00:11:22:33:44", "time 12:34:56:78 guid 123e4567-e89b-12d3-a456-426614174000 id 00:11:22:33:44");
  // hardware / build lines stay
  const std::string hw =
      "Phone: samsung SM-S918B, Android 14, SoC Qualcomm SM8550, 4 KB memory pages\nKERNEL: 5.15.74-android13-8-27999366-abS918BXXU3CWL5 aarch64\n"
      "GPU driver loaded: Adreno (TM) 740 Vulkan 1.3.128 driver 512.530.0 V@0615.0 (GIT@a1b2c3d4e5, I0123456789) (Date:03/18/24)\n"
      "SoC: Qualcomm SM8550 (board platform kalama, hardware qcom)\nCPU: 8 cores, app ABI arm64-v8a\nRAM: 11.2 GB\nDisplay 2340x1080 at 120 Hz\n"
      "ro.build.fingerprint = samsung/dm1qxxx/dm1q:14/UP1A.231005.007/S918BXXU3CWL5:user/release-keys\n";
  EQ(r, hw, hw);
  // S108 MOB review: the hardware lines as a real phone report and game log print them (Mali, s102 player report) pass unchanged
  const std::string real =
      "[rdr] device: Mali-G615 MC2, Vulkan 1.3.247, driver 0B001000, unified memory 1, lazy memory 1, BC 0, ASTC 1\n"
      "OK  GPU: Mali-G615 MC2, Vulkan 1.3, driver 44.1.0\nKernel: 6.1.157-android14-11-o-g81f880efbfc5 aarch64\n"
      "Phone: OPPO CPH2763, Android 16, SoC MT6878, 4 KB memory pages\nAndroid's exit reason: UNKNOWN (exit_self_21170_10440), 2026-11-13 18:06:04\n"
      "phone launcher android-v0.3.2 on android, ui scale 2.55\n";
  EQ(r, real, real);
  // the Android user number appears in no output form
  CHECK(!Has(Redact("/data/user/10/pkg/files/a /storage/emulated/10/b /mnt/user/10/emulated/10/c", r), "10"), "user number kept");
}

// ---- S108 review (D1): the binary-safe pass. A protobuf shaped like Android's tombstone (open files, memory maps, a log line, a memory dump),
// built here byte by byte; a text ANR trace; random bytes.
static std::string Vint(uint64_t v) {
  std::string s;
  do {
    unsigned char c = v & 0x7F;
    v >>= 7;
    if (v) c |= 0x80;
    s += char(c);
  } while (v);
  return s;
}
static std::string PbStr(int num, const std::string& s) { return Vint(uint64_t(num) << 3 | 2) + Vint(s.size()) + s; }
static std::string PbInt(int num, uint64_t v) { return Vint(uint64_t(num) << 3) + Vint(v); }
// This test's own decoder: every field of [b, e) well formed; the length-delimited contents of the fields in `nested` decoded as messages too;
// each string's (field, content) collected in order.
static bool Decode(const std::string& s, size_t b, size_t e, const std::vector<int>& nested, std::vector<std::pair<int, std::string>>& strs) {
  size_t p = b;
  auto vint = [&](uint64_t& v) {
    v = 0;
    for (int k = 0; k < 10 && p < e; ++k) {
      const unsigned char c = static_cast<unsigned char>(s[p++]);
      v |= uint64_t(c & 0x7F) << (7 * k);
      if (!(c & 0x80)) return true;
    }
    return false;
  };
  while (p < e) {
    uint64_t key, v;
    if (!vint(key)) return false;
    const int num = int(key >> 3);
    if ((key & 7) == 0) {
      if (!vint(v)) return false;
    } else if ((key & 7) == 2) {
      if (!vint(v) || v > e - p) return false;
      if (std::find(nested.begin(), nested.end(), num) != nested.end()) {
        if (!Decode(s, p, p + size_t(v), nested, strs)) return false;
      } else {
        strs.push_back({num, s.substr(p, size_t(v))});
      }
      p += size_t(v);
    } else {
      return false;
    }
  }
  return true;
}

static void BinaryTests(const fs::path& tmp) {
  using report_redact::RedactBinary;
  const auto r = PhoneRules();
  // the pieces (lengths 40..127: each length byte is a printable character glued to its string; tags ':' 'B' '*' '"' '2' printable too)
  const std::string p1 = "/data/user/0/com.example.wwe/files/logs/x.log";
  const std::string p2 = "/storage/emulated/0/Some Folder/y.iso";
  const std::string build_id = "0123456789abcdef0123456789abcdef01234567";
  const std::string ra = "/data/user/0/com.example.wwe/files/a.log", rb = "/data/user/0/com.example.wwe/files/b.log";
  const std::string logline = "11-21 12:00:01 I wwe: opened /data/user/0/com.example.wwe/files/logs/wwe2k14_007.log for jane@example.com mac aa:bb:cc:dd:ee:ff ip 192.168.1.23";
  const std::string dump = std::string("\x00\x01\xff\x80", 4) + "/storage/emulated/0/Pics/z.png" + std::string("\x00\xfe", 2);
  const std::string mapping = PbInt(1, 0x7000000000ull) + PbStr(7, p1) + PbStr(8, build_id) + PbInt(9, 0);   // mapping_name then build_id
  const std::string fd = PbInt(1, 57) + PbStr(2, p2) + PbStr(3, "owned by the game");
  const std::string pb = PbInt(1, 3) + PbStr(17, mapping) + PbStr(19, fd) + PbStr(5, ra) + PbStr(5, rb) + PbStr(4, logline) + PbStr(6, dump) +
                         PbStr(11, "/apex/com.android.runtime/lib64/bionic/libc.so");
  const std::string m = RedactBinary(pb, r);
  CHECK(m.size() == pb.size(), "binary pass changed the size: %zu -> %zu", pb.size(), m.size());
  std::vector<std::pair<int, std::string>> before, after;
  const std::vector<int> nested = {17, 19};
  CHECK(Decode(pb, 0, pb.size(), nested, before), "the test's own protobuf does not decode");
  const bool dec = Decode(m, 0, m.size(), nested, after);
  CHECK(dec && after.size() == before.size(), "after the binary pass the protobuf no longer decodes (%d, %zu fields)", int(dec), after.size());
  for (size_t k = 0; dec && k < std::min(before.size(), after.size()); ++k)
    CHECK(after[k].first == before[k].first && after[k].second.size() == before[k].second.size(), "field %zu: number or length changed", k);
  // bytes outside the hidden spans are the same: every byte that differs lies inside one of the sensitive strings
  std::vector<std::pair<size_t, size_t>> allowed;
  for (const std::string* x : {&p1, &p2, &ra, &rb, &logline, &dump}) {
    const size_t at = pb.find(*x);
    if (at != std::string::npos) allowed.push_back({at, at + x->size()});
  }
  size_t outside = 0, changed = 0;
  for (size_t k = 0; k < std::min(m.size(), pb.size()); ++k) {
    if (m[k] == pb[k]) continue;
    ++changed;
    bool in = false;
    for (const auto& [a, b] : allowed) in = in || (k >= a && k < b);
    outside += in ? 0 : 1;
  }
  CHECK(changed > 0 && outside == 0, "%zu bytes changed, %zu of them outside the sensitive strings", changed, outside);
  CHECK(!Has(m, "/data/user") && !Has(m, "/storage/emulated") && !Has(m, "Some Folder") && !Has(m, "jane@") && !Has(m, "aa:bb:cc"), "a path / email / MAC left in the binary");
  CHECK(Has(m, "x.log") && Has(m, "y.iso") && Has(m, "/a.log") && Has(m, "/b.log") && Has(m, "/wwe2k14_007.log for <email>") && Has(m, "/z.png"), "a kept name lost");
  CHECK(Has(m, "<path>x") && Has(m, build_id) && Has(m, "owned by the game") && Has(m, "/apex/com.android.runtime/lib64/bionic/libc.so"), "masked form / untouched fields");
  CHECK(Has(m, "ip 192.168.1.23"), "IPv4 is off in the binary pass");
  // the files: the old text pass changes this file's size (the fault D1 named); the kind list sends .bin to the binary pass
  const fs::path bin = tmp / "last_game_exit_trace.bin", t_out = tmp / "trace_text.out", b_out = tmp / "trace_bin.out";
  WriteFile(bin, pb);
  CHECK(report_redact::RedactFile(bin, t_out, r) && fs::file_size(t_out) != pb.size(), "the text pass was expected to change the tombstone's size");
  CHECK(report_redact::KindOf(bin) == report_redact::Kind::kBinary, ".bin is not binary");
  CHECK(report_redact::RedactFileByKind(bin, b_out, r) && fs::file_size(b_out) == pb.size(), "RedactFileByKind changed the tombstone's size");
  {
    std::ifstream f(b_out, std::ios::binary);
    const std::string got((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CHECK(got == m, "RedactFileBinary differs from RedactBinary");
  }
  for (const auto& [name, want] : std::vector<std::pair<const char*, report_redact::Kind>>{
           {"a/wwe2k14_007.log", report_redact::Kind::kText}, {"frames_007.CSV", report_redact::Kind::kText},
           {"launcher_settings.json", report_redact::Kind::kText}, {"shots/a.png", report_redact::Kind::kPicture},
           {"x.JPG", report_redact::Kind::kPicture}, {"noext", report_redact::Kind::kBinary}})
    CHECK(report_redact::KindOf(name) == want, "KindOf(%s) = %d", name, int(report_redact::KindOf(name)));
  // an ANR's text traces under the same name: masked line by line, same size
  const std::string anr =
      "----- pid 12345 at 2026-11-21 12:00:01 -----\nCmd line: com.example.wwe:game\n\"main\" prio=5 tid=1 Native\n"
      "  at java.io.FileInputStream.open(/storage/emulated/0/Android/data/com.example.wwe/files/logs/wwe2k14_007.log)\n"
      "  native: #00 pc 000000000004f0c8  /apex/com.android.runtime/lib64/bionic/libc.so (read+8)\n";
  const std::string ma = RedactBinary(anr, r);
  CHECK(ma.size() == anr.size() && !Has(ma, "/storage/emulated") && Has(ma, "open(<path>x") && Has(ma, "/wwe2k14_007.log)\n") &&
            Has(ma, "/apex/com.android.runtime/lib64/bionic/libc.so (read+8)") && Has(ma, "Cmd line: com.example.wwe:game"),
        "ANR text: [%s]", ma.c_str());
  // random bytes (with a path dropped in): the size kept; every changed byte was a text byte (printable ASCII or part of UTF-8)
  {
    uint32_t seed = 777;
    std::string rnd;
    for (int k = 0; k < (1 << 20); ++k) rnd += char((seed = seed * 1664525u + 1013904223u) >> 24);
    rnd.insert(5000, std::string("\x01") + "/storage/emulated/0/Folder/q.bin" + "\x02");
    const std::string mr = RedactBinary(rnd, r);
    size_t bad = 0;
    for (size_t k = 0; k < rnd.size() && k < mr.size(); ++k) {
      const unsigned char c = static_cast<unsigned char>(rnd[k]);
      if (mr[k] != rnd[k] && !((c >= 0x20 && c < 0x7F) || c == '\t' || c >= 0x80)) ++bad;
    }
    CHECK(mr.size() == rnd.size() && bad == 0, "random: size %zu -> %zu, %zu non-text bytes changed", rnd.size(), mr.size(), bad);
    CHECK(!Has(mr, "/storage/emulated/0/Folder") && Has(mr, "/q.bin"), "random: the dropped-in path");
  }
  fs::remove(bin), fs::remove(t_out), fs::remove(b_out);
}

// ---- S108 MOB (review of D1): hostile input to the binary pass. Every output has the input's size; a byte that changed was a text byte of the input
// (printable ASCII, a tab, or part of UTF-8: bytes >= 0x80); nothing crashes or hangs (the runner builds this with the STL's hardening on).
static bool SafeOut(const std::string& in, const std::string& out) {
  if (in.size() != out.size()) return false;
  for (size_t k = 0; k < in.size(); ++k) {
    const unsigned char c = static_cast<unsigned char>(in[k]);
    if (in[k] != out[k] && !((c >= 0x20 && c < 0x7F) || c == '\t' || c >= 0x80)) return false;
  }
  return true;
}
static void HostileTests() {
  using report_redact::RedactBinary;
  const auto r = PhoneRules();
  const std::string path = "/data/user/0/com.example.wwe/files/logs/x.log";
  auto safe = [&](const std::string& name, const std::string& in) {
    const std::string out = RedactBinary(in, r);
    CHECK(SafeOut(in, out), "hostile case '%s': size %zu -> %zu or a non-text byte changed", name.c_str(), in.size(), out.size());
    return out;
  };
  safe("empty", "");
  safe("one byte", "\x0a");
  safe("overlong varint key", std::string(11, '\x80') + "\x01");
  safe("varint at the very end", PbStr(1, "ab") + "\x08\x80");
  safe("key at the very end", PbStr(1, "ab") + "\x12");
  safe("length past the end", "\x0a\x7f" + path);
  safe("length 2^64-1", std::string("\x0a") + std::string(9, '\xff') + "\x01" + path);
  safe("length 2^63", std::string("\x0a") + std::string(9, '\x80') + "\x01" + path);
  safe("length 0", std::string("\x0a\x00", 2) + PbStr(2, path));
  safe("group start", std::string("\x0b") + PbStr(2, path) + "\x0c");
  safe("wire type 6 and 7", std::string("\x0e\x0f") + path);
  safe("field number 0", std::string("\x02\x04", 2) + "abcd" + path);
  safe("truncated fixed64", std::string("\x09") + "abc");
  safe("truncated fixed32", std::string("\x0d") + "ab");
  // the path in a string field of a protobuf must not survive even when the message is cut after it
  {
    const std::string cut = PbInt(1, 1) + PbStr(5, path);
    const std::string o = safe("a path field, whole", cut);
    CHECK(!Has(o, "/data/user"), "path left in a plain string field");
  }
  // a string field followed at once by a field whose tag ('2') and length ('A') bytes are printable: the walk masks each string inside its own bounds,
  // so the neighbour's tag and length survive (runs alone would take them into the path's span)
  {
    const std::string two = PbStr(5, path) + PbStr(6, "aaaa/" + std::string(60, 'q'));
    const std::string o = safe("printable tag after a path", two);
    std::vector<std::pair<int, std::string>> a, b;
    CHECK(Decode(two, 0, two.size(), {}, a) && Decode(o, 0, o.size(), {}, b) && a.size() == b.size() && b.size() == 2 && b[1].second == a[1].second,
          "a neighbouring field's tag / length / content was changed by the path's mask");
  }
  // deep nesting: depth 5000 (recursion is bounded at 32; the rest is masked run by run)
  {
    std::string deep = PbStr(5, path);
    for (int k = 0; k < 5000; ++k) deep = PbStr(1, deep);
    const auto t0 = std::chrono::steady_clock::now();
    const std::string o = safe("nested 5000", deep);
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(s < 5.0, "deep nesting took %.1f s", s);
    CHECK(!Has(o, "/data/user"), "path left in a deeply nested message");
  }
  // text that happens to parse as protobuf: its size is kept and its path is still hidden
  {
    const std::string t = "(" + path + ")";  // '(' = field 5 wire type 0 ...
    const std::string o = safe("text that parses", t);
    CHECK(!Has(o, "/data/user"), "text that parses as protobuf kept its path: [%s]", o.c_str());
    const std::string t2 = "\n" + std::string(1, char(path.size())) + path;  // a message whose string is the path
    CHECK(!Has(safe("message string", t2), "/data/user"), "path left");
  }
  // a fuzz loop over the tombstone-shaped buffer: flips, cuts, splices, inserts
  {
    const std::string mapping = PbInt(1, 0x7000000000ull) + PbStr(7, path) + PbStr(8, "0123456789abcdef0123456789abcdef01234567");
    const std::string base = PbInt(1, 3) + PbStr(17, mapping) + PbStr(19, PbInt(1, 57) + PbStr(2, "/storage/emulated/0/Some Folder/y.iso")) +
                             PbStr(4, "11-21 opened " + path + " for jane@example.com aa:bb:cc:dd:ee:ff") + PbStr(6, std::string("\x00\xff", 2) + path);
    uint32_t seed = 4242;
    auto rnd = [&] { return seed = seed * 1664525u + 1013904223u; };
    int bad = 0;
    for (int it = 0; it < 6000 && bad < 5; ++it) {
      std::string b = base;
      const int muts = 1 + int(rnd() >> 29);
      for (int k = 0; k < muts && !b.empty(); ++k) {
        const size_t at = (rnd() >> 8) % b.size();
        switch ((rnd() >> 20) % 5) {
          case 0: b[at] = char(rnd() >> 24); break;
          case 1: b.resize(at); break;
          case 2: b.insert(at, 1, char(rnd() >> 24)); break;
          case 3: b.erase(at, 1 + (rnd() >> 28) % 4); break;
          default: b.insert(at, base.substr((rnd() >> 8) % base.size(), 1 + (rnd() >> 26))); break;
        }
      }
      const std::string o = RedactBinary(b, r);
      if (!SafeOut(b, o)) {
        ++bad;
        CHECK(false, "fuzz iteration %d: size or a non-text byte changed", it);
      }
    }
    ++checks;
  }
}

static void BigTest(const fs::path& tmp) {
  // a 50 MB synthetic log: ordinary lines, paths, an IP, an email, a long line with no newline
  const fs::path in = tmp / "big.log", out = tmp / "big_out.log";
  const std::string unit = "12:00:01 [wwe2k14][perf] frame 1234 ok 16.6 ms\n12:00:01 opened /data/user/0/com.example.wwe/files/logs/wwe2k14_007.log for 192.168.1.23 jane@example.com\n"
                           "12:00:02 driver Adreno 740 V@0615.0 cores 0-3 1.80 GHz /storage/emulated/0/Download/My Games/wwe.iso\n";
  {
    std::ofstream f(in, std::ios::binary);
    size_t total = 0;
    while (total < (50u << 20)) f << unit, total += unit.size();
    f << std::string(3u << 20, 'x') << "\n/sdcard/end/tail.txt\n";  // a 3 MB line with no break
  }
  const auto t0 = std::chrono::steady_clock::now();
  const bool ok = report_redact::RedactFile(in, out, PhoneRules());
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  CHECK(ok, "RedactFile failed");
  size_t peak_mb = 0;
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS pmc{};
  if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) peak_mb = pmc.PeakWorkingSetSize >> 20;
#endif
  // read back: the first 3 unit lines and the tail
  std::ifstream r(out, std::ios::binary);
  std::string head(unit.size() * 2, '\0');
  r.read(head.data(), std::streamsize(head.size()));
  CHECK(Has(head, "opened <path>/wwe2k14_007.log for <ip> <email>"), "head redacted [%s]", head.substr(0, 200).c_str());
  CHECK(Has(head, "<path>/wwe.iso"), "space folder in the big log");
  CHECK(Has(head, "V@0615.0 cores 0-3 1.80 GHz"), "hardware kept");
  CHECK(!Has(head, "192.168") && !Has(head, "jane@") && !Has(head, "/data/user"), "leak in head");
  r.seekg(-24, std::ios::end);
  std::string tail(24, '\0');
  r.read(tail.data(), 24);
  CHECK(Has(tail, "<path>/tail.txt"), "tail [%s]", tail.c_str());
  r.close();
  CHECK(fs::file_size(out) < fs::file_size(in), "output should be smaller (paths cut)");
  std::printf("redact_unit: 50 MB (+3 MB single line) redacted in %.2f s%s\n", secs, peak_mb ? "" : " (peak memory: not measured here)");
  if (peak_mb) std::printf("redact_unit: peak working set of this whole test process %zu MB (the 50 MB test file is never held whole: pieces of ~1-2 MB)\n", peak_mb);
  CHECK(secs < 30.0, "too slow: %.1f s", secs);
  if (peak_mb) CHECK(peak_mb < 60, "peak working set %zu MB: the file was held whole", peak_mb);
  fs::remove(in), fs::remove(out);
}

static std::string Prop(const std::map<std::string, std::string>& m, const char* k) {
  const auto it = m.find(k);
  return it == m.end() ? std::string() : it->second;
}

static void CpuTests(const fs::path& tmp) {
  using namespace mlauncher::cpu;
  // 1. a big.LITTLE 8-core phone: 4 x A55 1.80 GHz, 3 x A78 2.42, 1 x X1 2.84; the SoC properties set; /proc/cpuinfo with Hardware, a Serial line and per-core parts
  {
    const fs::path root = tmp / "cpu1";
    fs::remove_all(root);
    std::string info;
    const unsigned khz[8] = {1800000, 1800000, 1800000, 1800000, 2420000, 2420000, 2420000, 2840000};
    const char* part[8] = {"0xd05", "0xd05", "0xd05", "0xd05", "0xd41", "0xd41", "0xd41", "0xd44"};
    for (int i = 0; i < 8; ++i) {
      WriteFile(root / "sys/devices/system/cpu" / ("cpu" + std::to_string(i)) / "cpufreq/cpuinfo_max_freq", std::to_string(khz[i]) + "\n");
      info += "processor\t: " + std::to_string(i) + "\nBogoMIPS\t: 38.40\nCPU implementer\t: 0x41\nCPU part\t: " + part[i] + "\n\n";
    }
    WriteFile(root / "sys/devices/system/cpu/cpufreq/policy0/x", "junk");  // a folder that is not a core
    WriteFile(root / "sys/devices/system/cpu/online", "0-7\n");
    WriteFile(root / "proc/cpuinfo", info + "Hardware\t: Qualcomm Technologies, Inc TEST8550\nSerial\t\t: 0123456789ABCDEF\n");
    WriteFile(root / "proc/meminfo", "MemTotal:       11768604 kB\nMemFree:  100 kB\n");
    const std::map<std::string, std::string> props = {{"ro.soc.manufacturer", "QTI"}, {"ro.soc.model", "SM8550"}, {"ro.board.platform", "kalama"}, {"ro.hardware", "qcom"}};
    Roots r{root / "proc", root / "sys", [&](const char* k) { return Prop(props, k); }, "arm64-v8a"};
    const std::string t = Text(r);
    std::printf("---- phone CPU block (fake roots, 8 cores) ----\n%s----\n", t.c_str());
    CHECK(Has(t, "SoC: QTI SM8550 (board platform kalama, hardware qcom, /proc/cpuinfo Hardware: Qualcomm Technologies, Inc TEST8550)"), "soc line");
    CHECK(Has(t, "CPU: 8 cores, app ABI arm64-v8a"), "cores");
    CHECK(Has(t, "CPU top clocks (cpufreq): cores 0-3 1.80 GHz, cores 4-6 2.42 GHz, core 7 2.84 GHz"), "clusters");
    CHECK(Has(t, "CPU core types (/proc/cpuinfo): cores 0-3 ARM Cortex-A55 (0xd05), cores 4-6 ARM Cortex-A78 (0xd41), core 7 ARM Cortex-X1 (0xd44)"), "types");
    CHECK(Has(t, "CPU cores online: 0-7"), "online");
    CHECK(Has(t, "RAM: 11.2 GB (MemTotal 11768604 kB)"), "ram");
    CHECK(!Has(t, "0123456789ABCDEF") && !Has(t, "Serial"), "a serial leaked");
    CHECK(Redact(t, PhoneRules()) == t, "the CPU block must pass the phone redaction unchanged");
  }
  // 2. unreadable cpufreq files (a core whose file is empty, one whose file holds text, one with no cpufreq folder), a missing soc property
  {
    const fs::path root = tmp / "cpu2";
    fs::remove_all(root);
    WriteFile(root / "sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq", "2000000\n");
    WriteFile(root / "sys/devices/system/cpu/cpu1/cpufreq/cpuinfo_max_freq", "");
    WriteFile(root / "sys/devices/system/cpu/cpu2/cpufreq/cpuinfo_max_freq", "denied\n");
    fs::create_directories(root / "sys/devices/system/cpu/cpu3");
    WriteFile(root / "proc/cpuinfo", "processor\t: 0\nHardware\t: ExampleBoard X1\n");
    const std::map<std::string, std::string> props = {{"ro.board.platform", "exb"}};
    Roots r{root / "proc", root / "sys", [&](const char* k) { return Prop(props, k); }, "arm64-v8a"};
    const std::string t = Text(r);
    std::printf("---- phone CPU block (unreadable cpufreq, no ro.soc.model, no meminfo) ----\n%s----\n", t.c_str());
    CHECK(Has(t, "SoC: not named (ro.soc.model not set); board platform exb; /proc/cpuinfo Hardware: ExampleBoard X1"), "soc fallback");
    CHECK(Has(t, "CPU: 4 cores"), "4 cores");
    CHECK(Has(t, "CPU top clocks (cpufreq): cores 1-3 clock not readable, core 0 2.00 GHz"), "unreadable clocks");
    CHECK(Has(t, "RAM: not readable"), "no meminfo");
    CHECK(!Has(t, "online"), "no online file");
  }
  // 3. nothing at all (no /proc, no /sys, no property reader)
  {
    Roots r{tmp / "nothing" / "proc", tmp / "nothing" / "sys", nullptr, ""};
    const std::string t = Text(r);
    CHECK(Has(t, "SoC: not readable") && Has(t, "core count not readable") && Has(t, "app ABI unknown") && Has(t, "RAM: not readable"), "empty roots: [%s]", t.c_str());
  }
  CHECK(RangeList({0, 1, 2, 3, 6}) == "0-3,6" && RangeList({5}) == "5" && RangeList({0, 2, 4}) == "0,2,4", "RangeList");
  // the PC's feature lines
  {
    launcher::CpuBits c;
    c.fma = c.movbe = c.popcnt = c.avx = c.os_ymm = c.bmi1 = c.avx2 = c.bmi2 = c.lzcnt = c.f16c = true;
    const std::string t = launcher::CpuFeaturesText("GenuineIntel", true, c);
    std::printf("---- PC CPU lines (a v3-capable CPU) ----\n%s", t.c_str());
    CHECK(Has(t, "CPU vendor: GenuineIntel") && Has(t, "SSE4.1 yes, AVX yes, AVX2 yes") && Has(t, "wwe2k14_v3.exe): this CPU can run it"), "v3 cpu");
    c.avx2 = false, c.bmi2 = false;
    const std::string u = launcher::CpuFeaturesText("AuthenticAMD", true, c);
    std::printf("---- PC CPU lines (no AVX2) ----\n%s----\n", u.c_str());
    CHECK(Has(u, "AVX2 no") && Has(u, "cannot run it (missing: AVX2, BMI2)"), "no avx2");
    CHECK(Redact(t + u, PcRules("C:\\Users\\Zedname")) == t + u, "the PC CPU lines must pass the PC redaction unchanged");
  }
}

int main(int argc, char** argv) {
  const fs::path tmp = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "redact_unit";
  fs::create_directories(tmp);
  try {
    PcTests();
    PhoneTests();
    CpuTests(tmp);
    BinaryTests(tmp);
    HostileTests();
    if (argc < 3) BigTest(tmp);  // (a second argument skips the 50 MB test: the planted-fault runs that are not about size)
  } catch (const std::exception& e) {
    std::printf("redact_unit: EXCEPTION %s\n", e.what());
    return 2;
  }
  std::printf("redact_unit: %d checks, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}
