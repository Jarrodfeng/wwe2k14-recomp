// wwe2k14 - startup diagnostic: find code reached only through pointer tables in the game's data.
//
// Static analysis can't see functions that are only ever called through a pointer stored in data
// (static-initialiser tables, vtables without RTTI, callback tables). At runtime they crash with
// "[FATAL] Call to invalid or unregistered function at guest address 0x...", one per run.
//
// This scans the loaded image (outside the code range) for big-endian words that point into the code
// range but aren't registered functions, and logs them in two groups: "candidate" (an adjacent word in
// the same table IS a registered function, e.g. initialiser tables, vtables) and "lone" (mostly switch
// jump-table labels). Neither group is trusted as-is: each target is classified offline against the
// generated code (uncovered gap vs. inside a function) before it goes into the manifest (see PLAYBOOK.md).

#pragma once

#include <rex/image_info.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>

#include <cstdint>
#include <map>

namespace wwe2k14 {

inline void ScanForUnregisteredFunctionPointers(rex::Runtime* runtime, const rex::PPCImageInfo& image,
                                                size_t max_logged = 400) {
  auto* memory = runtime->memory();
  auto* dispatcher = runtime->function_dispatcher();
  const uint32_t code_begin = image.code_base;
  const uint32_t code_end = image.code_base + image.code_size;

  auto read_be32 = [&](uint32_t guest) {
    const uint8_t* p = memory->TranslateVirtual<const uint8_t*>(guest);
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
  };
  auto in_code = [&](uint32_t v) { return v >= code_begin && v < code_end && (v & 3) == 0; };
  auto registered = [&](uint32_t v) { return in_code(v) && dispatcher->GetFunction(v) != nullptr; };

  // target -> first data address it was seen at (sorted by target for easy reading)
  std::map<uint32_t, uint32_t> table_hits, lone_hits;
  size_t scanned = 0;
  const uint32_t image_end = image.image_base + image.image_size;
  for (uint32_t addr = image.image_base; addr + 4 <= image_end; addr += 4) {
    if (addr >= code_begin && addr < code_end) {
      addr = code_end - 4;  // skip the code itself; loop increment lands on code_end
      continue;
    }
    ++scanned;
    uint32_t v = read_be32(addr);
    if (!in_code(v) || dispatcher->GetFunction(v) != nullptr) continue;
    bool prev_ok = addr >= image.image_base + 4 && registered(read_be32(addr - 4));
    bool next_ok = addr + 8 <= image_end && registered(read_be32(addr + 4));
    (prev_ok || next_ok ? table_hits : lone_hits).emplace(v, addr);
  }

  REXLOG_INFO("[wwe2k14][scan] scanned {} data words in 0x{:08X}-0x{:08X} (code 0x{:08X}-0x{:08X} skipped)",
              scanned, image.image_base, image_end, code_begin, code_end);
  REXLOG_INFO("[wwe2k14][scan] unregistered code pointers: {} next to a registered fn, {} lone "
              "(lone ones are mostly jump-table labels; classify against generated code before use)",
              table_hits.size(), lone_hits.size());
  // Lone pointers are listed too: the offline classifier (gap vs inside-function) is the real filter.
  size_t n = 0;
  for (const auto* hits : {&table_hits, &lone_hits}) {
    const char* kind = hits == &table_hits ? "candidate" : "lone";
    for (const auto& [target, where] : *hits) {
      if (n++ >= max_logged) break;
      REXLOG_INFO("[wwe2k14][scan] {} 0x{:08X} (pointer at 0x{:08X})", kind, target, where);
    }
  }
  if (n > max_logged) REXLOG_INFO("[wwe2k14][scan] ... {} more not logged", n - max_logged);
}

}  // namespace wwe2k14
