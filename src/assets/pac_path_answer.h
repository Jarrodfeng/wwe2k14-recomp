// S111 PACANSWER (docs\research\s111\PACANSWER.md): the game's own pac-list answer for an entrance prop path, so the live loader reads the file the GAME read instead of re-deriving the
// game's list order. The game finds an entrance prop and a prop's recolour mask by PATH: "/BOBJ/%04d" and "/SOBJ/%04d" (the prop reads sub_825AE720 and sub_83391F18, the existence tests
// sub_825AE5D8 / sub_825AE680), "/SPMT/%04d" (sub_825AE6D0). Every one goes through the pac-list walk sub_82C51D20, which returns 1 with [out + 32] = the list node that holds the path; the
// node's first 48 bytes are its pac's path. src\pac_answer_hook.h (the post-hook on that walk, the same one that serves `/EMD/` to scene_live\model_file_note.h) hands these three kinds to
// Note. assets::PropPachEvt / assets::PropMaskPachEvt (asset_load.h) ask PackOf first (assets::PropPachAnswered) and take our derived order (assets::PropPachInPacks, then the disc's
// directory) only when the game has not answered the path: the viewer and every tool (no game runs there: the table stays empty), or a path the game has not asked yet.
// A path's answer is the pac list's, a probe's or a read's alike (one manager, [0x839400C0], for every caller of the walk) and a path walks the same nodes until the list changes; the
// evtobj pacs are registered at boot (the disc's at the tail, the packs' at the head by the DLC pass). The last answer of a path wins.
// S111 READANSWER (docs\research\s111\READANSWER.md): WHICH path the game read is another question. The entrance prop loader sub_8258C590 probes /BOBJ/<id> for every prop, then reads
// a recoloured prop through sub_83391F18, /SOBJ/<id> only, even when the BOBJ probe resolved. So Note also takes `read` (the hit came inside a pac request sub_82C37E48: the reader
// scope of scene_live\model_file_note.h, routed by src\pac_lookup_route.h NotePropHit) and keeps per prop id the section of its last /BOBJ/ or /SOBJ/ READ; ReadSection hands it to
// assets::PropPachEvt, which takes the read section's answer before the probes' BOBJ-then-SOBJ order. /SPMT/ has one path per id: nothing to choose, not kept.
// The node's pac -> our file: `...\EVTOBJ.PAC` = the disc (pack 0), `...\EVTOBJ_DLC<pp>.PAC` = pack pp (any case). Another complete `.PAC` name (a pack whose catalog names its evtobj
// otherwise, or any node under `UPDATE:`, the title-update overlay) is kept as pack -1 with its path: the loader says so once and takes the derived order. A name that does not end in `.PAC` is not taken: the game links a node before it copies
// the node's path and the walk takes no lock, so a lookup on another thread can see a prefix (review S110 PACANS, pac_answer.h ParseNodeFileId).
// Header-only, std only (the hook and the unit both include it). Threads: Note runs on the game's threads, PackOf on the loader thread: a mutex, the atomic count read first. At most
// 3 x 10000 keys (three sections, a 4-digit id): no bound needed.
#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

namespace assets::pac_path_answer {

enum class Section : uint8_t { kBobj = 0, kSobj = 1, kSpmt = 2 };
inline const char* SectionName(Section s) { return s == Section::kBobj ? "BOBJ" : s == Section::kSobj ? "SOBJ" : "SPMT"; }

struct Answer {
  int pack = -1;           // 0 = the disc's evtobj.pac, pp = pack pp's evtobj_dlcPP.pac, -1 = a pac name we do not map
  std::string node_path;   // the node's pac path, as the game holds it
};

namespace detail {
struct State {
  std::mutex mu;
  std::map<uint32_t, Answer> by_path;   // Key(section, id) -> the last answer
  std::map<uint32_t, Section> read_section;   // S111 READANSWER: prop id -> the section of its last /BOBJ/ or /SOBJ/ READ (each entry is written beside a by_path entry)
  std::atomic<size_t> count{0};         // by_path.size(), the fast path (0 = read_section is empty too)
};
inline State& S() {
  static State s;
  return s;
}
inline uint32_t Key(Section s, uint32_t id) { return uint32_t(s) * 10000u + id; }
inline char Up(char c) { return c >= 'a' && c <= 'z' ? char(c - 32) : c; }
inline bool Digit(char c) { return c >= '0' && c <= '9'; }
}  // namespace detail

// The hook's first-bytes test (after the path's leading '/'): "BOBJ/", "SOBJ/" or "SPMT/". Every compare stops at the first byte that differs (a NUL differs), so no byte past the ones the
// game's walk read is touched (the hook reads the path unchecked: pac_answer_hook.h OnLookup).
inline bool IsPropDir(const unsigned char* p) {
  if (p[0] == 'B') return p[1] == 'O' && p[2] == 'B' && p[3] == 'J' && p[4] == '/';
  if (p[0] != 'S') return false;
  if (p[1] == 'O') return p[2] == 'B' && p[3] == 'J' && p[4] == '/';
  return p[1] == 'P' && p[2] == 'M' && p[3] == 'T' && p[4] == '/';
}

// `/BOBJ/2004` (or without the leading '/'): the section and the id; exactly 4 digits (the game's %04d). False for anything else.
inline bool ParseAsked(std::string_view asked, Section& s, uint32_t& id) {
  if (!asked.empty() && asked[0] == '/') asked.remove_prefix(1);
  if (asked.size() != 9 || asked[4] != '/') return false;
  const std::string_view dir = asked.substr(0, 4);
  if (dir == "BOBJ") s = Section::kBobj;
  else if (dir == "SOBJ") s = Section::kSobj;
  else if (dir == "SPMT") s = Section::kSpmt;
  else return false;
  uint32_t v = 0;
  for (size_t i = 5; i < 9; ++i) {
    if (!detail::Digit(asked[i])) return false;
    v = v * 10 + uint32_t(asked[i] - '0');
  }
  id = v;
  return true;
}

// The node's pac path (a 48-byte field: ends at the first NUL) -> the pack: true with `pack` 0 (EVTOBJ.PAC), pp (EVTOBJ_DLC<pp>.PAC) or -1 (another complete .PAC name); false = not a
// complete pac name (a prefix seen mid-copy, or no `.PAC`): nothing is stored.
inline bool ParseNodePack(std::string_view node_path48, int& pack) {
  size_t n = 0;
  while (n < node_path48.size() && node_path48[n] != '\0') ++n;
  const std::string_view p = node_path48.substr(0, n);
  const size_t cut = p.find_last_of("\\/:");
  const std::string_view c = cut == std::string_view::npos ? p : p.substr(cut + 1);
  auto lit = [&](size_t at, const char* s) {   // c[at..] starts with the upper-case literal s
    for (size_t k = 0; s[k]; ++k)
      if (at + k >= c.size() || detail::Up(c[at + k]) != s[k]) return false;
    return true;
  };
  if (c.size() < 5 || !lit(c.size() - 4, ".PAC")) return false;
  pack = -1;
  // PACANSWER review: the title-update pass sub_82C52818 names its nodes "UPDATE:\" + the disc's def name (0x8207A11C), so an UPDATE: evtobj ends in EVTOBJ.PAC too, but our loader
  // reads the game data's pac\evt\evtobj.pac (launcher::GameFiles has no update layer): unmapped (said once, the derived order). S111 TUOVERLAY: unreachable in this build, kept as a
  // guard: sub_82807128 makes UPDATE: nodes only when UPDATE:\plist360.arc opens, and nothing mounts update: (the SDK's update_data_root cvar is never set by Wwe2k14App::OnConfigurePaths;
  // the importer's overlay, superstars_update.h, went in S110 SEAT-B); the player's files carry no title update. A lane that mounts update: again gives GameFiles an update-first layer and maps this node.
  static constexpr char kUpdate[] = "UPDATE:";
  bool update = p.size() >= 7;
  for (size_t k = 0; update && k < 7; ++k) update = detail::Up(p[k]) == kUpdate[k];
  if (update) return true;
  if (!lit(0, "EVTOBJ")) return true;
  if (c.size() == 10) return pack = 0, true;   // EVTOBJ.PAC
  if (!lit(6, "_DLC")) return true;
  size_t i = 10;
  int v = 0;
  for (; i < c.size() && detail::Digit(c[i]) && v < 100000; ++i) v = v * 10 + (c[i] - '0');
  if (i == 10 || i != c.size() - 4 || v <= 0) return true;
  pack = v;
  return true;
}

// A hit of the game's walk for `asked` (the path) whose node holds `node_path48`. True when it was an entrance prop / mask path with a complete pac name (stored).
// `read` = the hit came inside a pac request (the request's own walk, not a probe: src\pac_lookup_route.h): a /BOBJ/ or /SOBJ/ read also becomes the id's read section.
inline bool Note(std::string_view asked, std::string_view node_path48, bool read = false) {
  Section s;
  uint32_t id = 0;
  int pack = -1;
  if (!ParseAsked(asked, s, id) || !ParseNodePack(node_path48, pack)) return false;
  size_t n = 0;
  while (n < node_path48.size() && node_path48[n] != '\0') ++n;
  auto& st = detail::S();
  std::lock_guard<std::mutex> l(st.mu);
  Answer& a = st.by_path[detail::Key(s, id)];
  if (a.pack != pack || a.node_path != node_path48.substr(0, n)) a.pack = pack, a.node_path = std::string(node_path48.substr(0, n));
  if (read && s != Section::kSpmt) st.read_section[id] = s;   // S111 READANSWER: the last read of the id names its section
  st.count.store(st.by_path.size(), std::memory_order_release);
  return true;
}

// The game's last answer for `/<section>/<id>`; false = the game has not answered it (the usual case off the game: one atomic load).
inline bool PackOf(Section s, uint32_t id, Answer& out) {
  auto& st = detail::S();
  if (st.count.load(std::memory_order_acquire) == 0 || id > 9999) return false;
  std::lock_guard<std::mutex> l(st.mu);
  const auto it = st.by_path.find(detail::Key(s, id));
  if (it == st.by_path.end()) return false;
  out = it->second;
  return true;
}

// S111 READANSWER: the section of the game's last READ of prop `id` (/BOBJ/ or /SOBJ/, a hit inside a pac request); false = no read seen (a probe-only id, the viewer, every tool).
// That section's answer is PackOf(section, id): the read and any probe of the same path walk the same nodes.
inline bool ReadSection(uint32_t id, Section& out) {
  auto& st = detail::S();
  if (st.count.load(std::memory_order_acquire) == 0) return false;
  std::lock_guard<std::mutex> l(st.mu);
  const auto it = st.read_section.find(id);
  if (it == st.read_section.end()) return false;
  out = it->second;
  return true;
}

inline void Clear() {   // tests
  auto& st = detail::S();
  std::lock_guard<std::mutex> l(st.mu);
  st.by_path.clear();
  st.read_section.clear();
  st.count.store(0, std::memory_order_release);
}

}  // namespace assets::pac_path_answer
