// S103 DLW (docs\systems\dlc.md "fixed by DLW"): the packs the GAME was offered (src\dlc.h Install: ready, switched on, accepted by its gate,
// plus the Superstar Importer's pack while its toggle is on), published for the live scene loader so the stream-free build reads the same
// files the game reads. Before DLW only the viewer configured its file reader with packs (assets\dlc_files.h): in the player's default build
// a DLC wrestler's pac\ch\ch385.pac failed to open (the disc has no ch377-396) and he was invisible in every state (COV2 GAP 2).
// No game / rex dependency: dlc.h writes it once at boot (Publish), the loader's file readers read it (Get; Generation tells a reader to re-apply).
// Read-only folders and STFS packages; nothing here writes to a pack (CLAUDE.md "Mods are additive": the imported-superstars pack is offered only while its toggle is on, so it is
// here only then). ORDER = the game's: highest pack id first (docs\formats\catalog_dlc.md "The rule the game applies" item 3 [C]); a reader takes the first pack that has a file.
#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace wwe2k14::dlc_overlays {

struct Item {
  std::filesystem::path root;  // the pack's folder
  std::string suffix;          // "_dlc04"
  std::string kind;            // "dlc" (a real pack) or "mod" (the imported-superstars pack)
  int pack_id = -1;
  bool package = false;        // an STFS package (root = the package file), read in place through dlc_catalog.h StfsPackage
};

namespace detail {
struct State {
  std::mutex mu;
  std::vector<Item> items;
  std::atomic<uint64_t> gen{0};
};
inline State& S() {
  static State s;
  return s;
}
}  // namespace detail

inline void Publish(std::vector<Item> items) {   // pack-id order (the game's order: the first pack that has a file wins)
  auto& s = detail::S();
  std::lock_guard<std::mutex> l(s.mu);
  s.items = std::move(items);
  s.gen.fetch_add(1, std::memory_order_release);
}
inline std::vector<Item> Get() {
  auto& s = detail::S();
  std::lock_guard<std::mutex> l(s.mu);
  return s.items;
}
inline uint64_t Generation() { return detail::S().gen.load(std::memory_order_acquire); }

}  // namespace wwe2k14::dlc_overlays
