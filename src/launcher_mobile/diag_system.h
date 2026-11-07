#pragma once
// Session 95 (the diagnostic run): system.txt for the phone launcher - what the device is and how hot it is, read from
// the places Android lets an app read (getprop, /proc/meminfo, /sys). Called at the start and the end of a run (thermal
// zones and battery temperature move). Plain text, no names or accounts. Unreadable items are simply left out.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#if defined(__ANDROID__)
#include <sys/system_properties.h>
#include <unistd.h>
#endif

namespace mlauncher::diag {

inline std::string ReadSmall(const std::filesystem::path& p, size_t max = 4096) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return {};
  std::string s(max, '\0');
  f.read(s.data(), std::streamsize(max));
  s.resize(size_t(f.gcount()));
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
  return s;
}

inline std::string SystemText(const char* when) {
  std::ostringstream o;
  o << "[" << when << "]\n";
#if defined(__ANDROID__)
  static const char* const kProps[] = {"ro.product.manufacturer", "ro.product.model", "ro.product.device", "ro.product.board",
                                       "ro.board.platform", "ro.soc.manufacturer", "ro.soc.model", "ro.hardware",
                                       "ro.hardware.vulkan", "ro.hardware.egl", "ro.opengles.version",
                                       "ro.build.version.release", "ro.build.version.sdk", "ro.build.version.security_patch",
                                       "ro.build.fingerprint", "ro.product.cpu.abi", "ro.build.display.id",
                                       "persist.sys.dalvik.vm.lib.2", "ro.surface_flinger.max_frame_buffer_acquired_buffers"};
  o << "properties:\n";
  for (const char* k : kProps) {
    char v[PROP_VALUE_MAX] = {};
    if (__system_property_get(k, v) > 0) o << "  " << k << " = " << v << "\n";
  }
  o << "page_size: " << sysconf(_SC_PAGESIZE) << "\n";
  o << "cpu_cores_online: " << sysconf(_SC_NPROCESSORS_ONLN) << "\n";
  {
    std::istringstream mem(ReadSmall("/proc/meminfo", 2048));
    o << "meminfo:\n";
    int n = 0;
    for (std::string line; std::getline(mem, line) && n < 8; ++n) o << "  " << line << "\n";
  }
  o << "cpufreq (cpu: governor cur/max kHz):\n";
  for (int i = 0; i < 16; ++i) {
    const std::string base = "/sys/devices/system/cpu/cpu" + std::to_string(i) + "/cpufreq/";
    const std::string cur = ReadSmall(base + "scaling_cur_freq", 32);
    if (cur.empty()) continue;
    o << "  cpu" << i << ": " << ReadSmall(base + "scaling_governor", 32) << " " << cur << "/"
      << ReadSmall(base + "cpuinfo_max_freq", 32) << "\n";
  }
  o << "thermal zones (type: temp, millidegrees as the kernel reports):\n";
  for (int i = 0; i < 64; ++i) {
    const std::string base = "/sys/class/thermal/thermal_zone" + std::to_string(i) + "/";
    const std::string type = ReadSmall(base + "type", 64);
    if (type.empty()) continue;
    o << "  " << i << " " << type << ": " << ReadSmall(base + "temp", 32) << "\n";
  }
  o << "battery: temp=" << ReadSmall("/sys/class/power_supply/battery/temp", 32)
    << " capacity=" << ReadSmall("/sys/class/power_supply/battery/capacity", 32)
    << " status=" << ReadSmall("/sys/class/power_supply/battery/status", 32) << "\n";
#else
  o << "(desktop test build: no phone properties)\n";
#endif
  return o.str();
}

}  // namespace mlauncher::diag
