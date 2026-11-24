#pragma once
// wwe2k14 phone launcher - the CPU block of the problem report (S108 lane MOB): the SoC, the core count, each cluster's top clock, the core types, the ABI the
// app runs as and the RAM, read from places Android lets an app read (system properties, /proc, /sys: no permission, no Java). Never a serial or an id:
// only the named keys below are read out of /proc/cpuinfo (its "Serial" line is never touched).
// The same cluster figures the game logs once when the performance hint is on ("[perf_hint] CPU: cores 0-3 1.80 GHz, ..." android_perf_hint.h
// ReadClusters): that line exists only in a game log that ran with the hint, so the launcher reads cpufreq itself and prints the same kind of text.
// The roots are parameters so tools\report\redact_unit.cpp can point them at a temp folder: no Android header is needed here (the real property reader
// is SystemProp, Android only).
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <vector>

#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

namespace mlauncher::cpu {

namespace fs = std::filesystem;

struct Roots {
  fs::path proc = "/proc", sys = "/sys";
  std::function<std::string(const char*)> prop;  // a system property's value, "" when not set
  std::string abi;                               // the ABI the app runs as
};

inline std::string SystemProp(const char* name) {
#if defined(__ANDROID__)
  char v[PROP_VALUE_MAX] = {};
  if (__system_property_get(name, v) > 0) return v;
#else
  (void)name;
#endif
  return {};
}

inline const char* AppAbi() {
#if defined(__aarch64__)
  return "arm64-v8a";
#elif defined(__arm__)
  return "armeabi-v7a";
#elif defined(__x86_64__) || defined(_M_X64)
  return "x86_64";
#elif defined(__i386__) || defined(_M_IX86)
  return "x86";
#else
  return "unknown";
#endif
}

inline std::string ReadAll(const fs::path& p, size_t max = 1 << 18) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return {};
  std::string s(max, '\0');
  f.read(s.data(), std::streamsize(max));
  s.resize(size_t(f.gcount()));
  return s;
}
inline std::string Trim(std::string s) {
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t' || s.back() == '\0')) s.pop_back();
  size_t b = 0;
  while (b < s.size() && (s[b] == ' ' || s[b] == '\t')) ++b;
  return s.substr(b);
}

// 0,1,2,3,6 -> "0-3,6"
inline std::string RangeList(const std::vector<int>& v) {
  std::string out;
  for (size_t i = 0; i < v.size();) {
    size_t j = i;
    while (j + 1 < v.size() && v[j + 1] == v[j] + 1) ++j;
    out += (out.empty() ? "" : ",") + std::to_string(v[i]) + (j > i ? "-" + std::to_string(v[j]) : "");
    i = j + 1;
  }
  return out;
}

// ARM's part numbers (the "CPU part" of /proc/cpuinfo, implementer 0x41); anything else is printed as the raw number.
inline std::string ArmCoreName(const std::string& part) {
  static const std::map<std::string, const char*> k = {
      {"0xd03", "Cortex-A53"}, {"0xd04", "Cortex-A35"}, {"0xd05", "Cortex-A55"}, {"0xd07", "Cortex-A57"}, {"0xd08", "Cortex-A72"},
      {"0xd09", "Cortex-A73"}, {"0xd0a", "Cortex-A75"}, {"0xd0b", "Cortex-A76"}, {"0xd0d", "Cortex-A77"}, {"0xd41", "Cortex-A78"},
      {"0xd44", "Cortex-X1"},  {"0xd46", "Cortex-A510"}, {"0xd47", "Cortex-A710"}, {"0xd48", "Cortex-X2"}, {"0xd4d", "Cortex-A715"},
      {"0xd4e", "Cortex-X3"},  {"0xd80", "Cortex-A520"}, {"0xd81", "Cortex-A720"}, {"0xd82", "Cortex-X4"}};
  const auto it = k.find(part);
  return it == k.end() ? std::string() : it->second;
}

inline std::string Text(const Roots& r) {
  auto prop = [&](const char* name) { return r.prop ? Trim(r.prop(name)) : std::string(); };
  std::error_code ec;
  // /proc/cpuinfo: the Hardware line and, per processor, implementer and part (nothing else is read: its Serial line is never touched).
  std::string hardware;
  std::map<int, std::pair<std::string, std::string>> core_type;  // cpu -> (implementer, part)
  {
    std::string text = ReadAll(r.proc / "cpuinfo"), line;
    int cur = -1;
    for (size_t at = 0; at < text.size();) {
      size_t nl = text.find('\n', at);
      if (nl == std::string::npos) nl = text.size();
      line = text.substr(at, nl - at);
      at = nl + 1;
      const size_t colon = line.find(':');
      if (colon == std::string::npos) continue;
      const std::string key = Trim(line.substr(0, colon)), val = Trim(line.substr(colon + 1));
      if (key == "processor") cur = std::atoi(val.c_str());
      else if (key == "Hardware" && hardware.empty()) hardware = val;
      else if (key == "CPU implementer" && cur >= 0) core_type[cur].first = val;
      else if (key == "CPU part" && cur >= 0) core_type[cur].second = val;
    }
  }
  // /sys/devices/system/cpu/cpuN: the cores, each one's top clock (an unreadable file = 0)
  std::map<int, unsigned> khz;
  const fs::path cpus = r.sys / "devices" / "system" / "cpu";
  for (const auto& e : fs::directory_iterator(cpus, ec)) {
    const std::string n = e.path().filename().string();
    if (n.size() < 4 || n.compare(0, 3, "cpu") != 0 || n.find_first_not_of("0123456789", 3) != std::string::npos) continue;
    const std::string v = Trim(ReadAll(e.path() / "cpufreq" / "cpuinfo_max_freq", 32));
    khz[std::atoi(n.c_str() + 3)] = v.empty() || v.find_first_not_of("0123456789") != std::string::npos ? 0u : unsigned(std::strtoul(v.c_str(), nullptr, 10));
  }

  std::string t;
  // SoC: the named properties first, then the board names, then the Hardware line
  const std::string maker = prop("ro.soc.manufacturer"), model = prop("ro.soc.model"), platform = prop("ro.board.platform"), hw_prop = prop("ro.hardware");
  std::string soc;
  if (!model.empty()) {
    soc = (maker.empty() ? "" : maker + " ") + model;
    std::string more;
    if (!platform.empty()) more += "board platform " + platform;
    if (!hw_prop.empty()) more += (more.empty() ? "" : ", ") + std::string("hardware ") + hw_prop;
    if (!hardware.empty() && hardware != hw_prop) more += (more.empty() ? "" : ", ") + std::string("/proc/cpuinfo Hardware: ") + hardware;
    if (!more.empty()) soc += " (" + more + ")";
  } else if (!platform.empty() || !hw_prop.empty() || !hardware.empty()) {
    soc = "not named (ro.soc.model not set)";
    if (!platform.empty()) soc += "; board platform " + platform;
    if (!hw_prop.empty()) soc += "; hardware " + hw_prop;
    if (!hardware.empty()) soc += "; /proc/cpuinfo Hardware: " + hardware;
  } else {
    soc = "not readable (no ro.soc.model, board platform, hardware or /proc/cpuinfo Hardware)";
  }
  t += "SoC: " + soc + "\n";
  t += "CPU: " + (khz.empty() ? std::string("core count not readable") : std::to_string(khz.size()) + " cores") + ", app ABI " + (r.abi.empty() ? "unknown" : r.abi) + "\n";
  if (!khz.empty()) {
    std::map<unsigned, std::vector<int>> by;
    for (const auto& [cpu, k] : khz) by[k].push_back(cpu);
    std::string clocks;
    for (const auto& [k, list] : by) {
      char ghz[32];
      std::snprintf(ghz, sizeof(ghz), "%.2f GHz", k / 1e6);
      clocks += (clocks.empty() ? "" : ", ") + std::string(list.size() > 1 ? "cores " : "core ") + RangeList(list) + " " + (k ? ghz : "clock not readable");
    }
    t += "CPU top clocks (cpufreq): " + clocks + "\n";
    const std::string online = Trim(ReadAll(cpus / "online", 64));
    if (!online.empty()) t += "CPU cores online: " + online + "\n";
  }
  if (!core_type.empty()) {
    std::map<std::string, std::vector<int>> by;
    for (const auto& [cpu, ip] : core_type) {
      std::string label = ip.second.empty() ? std::string("unknown") : ip.second;
      if (ip.first == "0x41" && !ArmCoreName(ip.second).empty()) label = "ARM " + ArmCoreName(ip.second) + " (" + ip.second + ")";
      else if (!ip.second.empty()) label = "implementer " + (ip.first.empty() ? std::string("?") : ip.first) + " part " + ip.second;
      by[label].push_back(cpu);
    }
    std::string types;
    for (const auto& [label, list] : by) types += (types.empty() ? "" : ", ") + std::string(list.size() > 1 ? "cores " : "core ") + RangeList(list) + " " + label;
    t += "CPU core types (/proc/cpuinfo): " + types + "\n";
  }
  {
    const std::string mem = ReadAll(r.proc / "meminfo", 4096);
    const size_t at = mem.find("MemTotal:");
    unsigned long long kb = at == std::string::npos ? 0 : std::strtoull(mem.c_str() + at + 9, nullptr, 10);
    char ram[64];
    std::snprintf(ram, sizeof(ram), "%.1f GB", double(kb) / (1024.0 * 1024.0));
    t += "RAM: " + (kb ? std::string(ram) + " (MemTotal " + std::to_string(kb) + " kB)" : std::string("not readable")) + "\n";
  }
  return t;
}

// The real phone: /proc and /sys, the system properties, the ABI this build is.
inline std::string PhoneText() {
  Roots r;
  r.prop = SystemProp;
  r.abi = AppAbi();
  return Text(r);
}

}  // namespace mlauncher::cpu
