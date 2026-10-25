// wwe2k14 - guest value probe (debug harness, Session 8 task 7 menu/UI labeller B).
//
// --wwe2k14_peek=NAME=ADDR[>OFF...],NAME2=...   (default empty = off: one string compare per frame)
// Once per guest frame (VdSwap) each probe is read and logged when its value changes:
//   [wwe2k14][peek] frame=N NAME=0xVALUE (was 0xOLD)
// ADDR is a guest address (hex). With no '>' the probe is the big-endian word at ADDR. Each '>OFF' (hex with 0x,
// else decimal) treats the current value as a guest pointer and reads the word at value + OFF, e.g.
// seq_id=0x839400F8>0>48 reads p = [0x839400F8], q = [p + 0], value = [q + 48]. A null pointer or an address the
// host cannot read gives "unreadable" (logged once per change). Only for finding/confirming state addresses:
// probes are read on the render thread without synchronisation with the guest threads that write them.

#pragma once

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "platform/compat.h"  // <windows.h> on Windows (Session 63)
#endif

REXCVAR_DEFINE_STRING(wwe2k14_peek, "", "wwe2k14",
                      "Debug: log guest words when they change, once per frame: NAME=0xADDR[>OFF...] comma-separated "
                      "(each >OFF dereferences the value and adds OFF). Empty = off");

namespace wwe2k14 {

class GuestPeek {
 public:
  void OnFrame(uint64_t frame) {
    const std::string& spec = REXCVAR_GET(wwe2k14_peek);
    if (spec.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (spec != parsed_spec_) Parse(spec);
    for (Probe& p : probes_) {
      uint32_t value = 0;
      const bool ok = Evaluate(p, &value);
      if (p.seen && ok == p.ok && (!ok || value == p.value)) continue;
      if (ok) {
        if (p.seen && p.ok) {
          REXLOG_INFO("[wwe2k14][peek] frame={} {}=0x{:08X} ({}) (was 0x{:08X})", frame, p.name, value,
                      int32_t(value), p.value);
        } else {
          REXLOG_INFO("[wwe2k14][peek] frame={} {}=0x{:08X} ({})", frame, p.name, value, int32_t(value));
        }
      } else {
        REXLOG_INFO("[wwe2k14][peek] frame={} {}=unreadable", frame, p.name);
      }
      p.seen = true;
      p.ok = ok;
      p.value = value;
    }
  }

 private:
  struct Probe {
    std::string name;
    uint32_t address = 0;
    std::vector<uint32_t> offsets;
    bool seen = false;
    bool ok = false;
    uint32_t value = 0;
  };

  static bool ReadWord(uint32_t guest, uint32_t* out) {
    if (guest == 0 || (guest & 3) != 0) return false;
    auto* host = REX_KERNEL_MEMORY()->TranslateVirtual<const uint8_t*>(guest);
    if (!host) return false;
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(host, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
      return false;
    }
#endif
    *out = (uint32_t(host[0]) << 24) | (uint32_t(host[1]) << 16) | (uint32_t(host[2]) << 8) | uint32_t(host[3]);
    return true;
  }

  static bool Evaluate(const Probe& p, uint32_t* out) {
    uint32_t v = 0;
    if (!ReadWord(p.address, &v)) return false;
    for (uint32_t off : p.offsets) {
      if (!ReadWord(v + off, &v)) return false;
    }
    *out = v;
    return true;
  }

  static bool ParseNumber(const std::string& s, uint32_t* out) {
    if (s.empty()) return false;
    char* end = nullptr;
    const unsigned long v = std::strtoul(s.c_str(), &end, (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) ? 16 : 10);
    if (!end || *end != '\0') return false;
    *out = uint32_t(v);
    return true;
  }

  void Parse(const std::string& spec) {
    parsed_spec_ = spec;
    probes_.clear();
    std::stringstream items(spec);
    std::string item;
    while (std::getline(items, item, ',')) {
      const size_t eq = item.find('=');
      if (eq == std::string::npos || eq == 0) {
        REXLOG_WARN("[wwe2k14][peek] ignoring '{}' (want NAME=ADDR[>OFF...])", item);
        continue;
      }
      Probe p;
      p.name = item.substr(0, eq);
      std::stringstream parts(item.substr(eq + 1));
      std::string part;
      bool first = true, good = true;
      while (std::getline(parts, part, '>')) {
        uint32_t v = 0;
        if (first) {
          // The address is always hex, with or without 0x.
          const std::string hex = (part.size() > 2 && part[0] == '0' && (part[1] == 'x' || part[1] == 'X'))
                                      ? part
                                      : "0x" + part;
          good = ParseNumber(hex, &v);
          p.address = v;
          first = false;
        } else {
          good = good && ParseNumber(part, &v);
          p.offsets.push_back(v);
        }
      }
      if (!good || first) {
        REXLOG_WARN("[wwe2k14][peek] ignoring '{}' (bad number)", item);
        continue;
      }
      REXLOG_INFO("[wwe2k14][peek] probe {} = 0x{:08X} with {} dereference(s)", p.name, p.address, p.offsets.size());
      probes_.push_back(std::move(p));
    }
  }

  std::mutex mutex_;
  std::string parsed_spec_;
  std::vector<Probe> probes_;
};

inline GuestPeek g_guest_peek;

inline void GuestPeekOnFrame(uint64_t frame) {
  if (REXCVAR_GET(wwe2k14_peek).empty()) return;
  g_guest_peek.OnFrame(frame);
}

}  // namespace wwe2k14
