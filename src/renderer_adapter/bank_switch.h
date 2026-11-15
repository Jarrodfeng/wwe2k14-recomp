// S102 T1: the lazy-banks switch and its counters (capture_reader.h's BankView reads them; reader.h sets the switch per frame from
// --wwe2k14_rdr_lazy_banks; rplay from --lazy-banks). No includes beyond <atomic>.
#pragma once
#include <atomic>
#include <cstdint>

namespace rdr_adapter {
inline std::atomic<bool> g_lazy_banks{false};     // read at every ParseFrame
inline std::atomic<bool> g_bank_blocks{false};   // S102 T1b (wwe2k14_rdr_bank_blocks): a 64-entry block map per def; implies lazy; read at every ParseFrame
inline std::atomic<uint64_t> g_bank_map_frames{0};   // S102 T1b: frames parsed with block maps
inline std::atomic<int> g_bank_plant{0};          // planted fault for the gate: 1 = a resolved def skips its patches (must break picture equality); T1b: 4 = a block map skips its patches, 5 = a block map's base off by one
inline std::atomic<uint64_t> g_bank_bytes{0};     // bytes zeroed + copied building banks (both modes), summed
inline std::atomic<uint64_t> g_bank_frames{0};    // frames parsed with bank defs
inline std::atomic<uint64_t> g_bank_resolved{0};  // banks built (lazy: resolved on use; eager: expanded)
}  // namespace rdr_adapter
