// S110 LIBSIZE lane 3 (docs\research\s110\LIBSIZE.md section 2b and items 10-13): the game side of the alias rows. A character's file holds his model, his EVP banks
// and his UI renders only; every other name his seat needs (EVZ, GMEV, PCE6, MOVN, the base renders) is a row that points at the game's OWN entry. The pure half is
// src\seat_alias_core.h (the walk's first-match answer and the rows) and src\seat_alias_plan.h (which nodes are ours, which nodes to write, the EV stamp).
//
// Refresh (game thread; the row pass at boot through seats::g_alias_refresh, and a page flip in src\seat_bind.h after all its arrivals and vacates):
//   1. the game's pac list is walked from the head ([[0x839400C0] + 72]; node +0 path, +48 rows, +60 next: the reads and the checks of seats.h AddNodes), each
//      node's rows copied into a NodeView up to and including the first 0xFF-topped word, found as the game's own walk sub_82C51D20 +1FC finds it
//      (al::GameRowsSpan). Node +52 is NOT read: the disc's node maker sub_82C52668 and the title-update pass sub_82C52818 store start - end there (negative),
//      only the DLC registration sub_82816458 stores end - start (S110 ALIASMISS). A node whose rows are not readable to that word gives no rows (the game's
//      own walk would fault on it); every node WE made is flagged by its ADDRESS (the residents'
//      character and EV nodes, the guests' nodes, our alias nodes), so a lookup never resolves through our own rows;
//   2. AliasNodes is asked for the wants of everybody seated now (Occupants(): a resident's and a guest's rows hold the ALIA list of his file), with the frame's
//      bank of each seat: one node per game FILE, carrying the game's own path;
//   3. each node that changed is written with seat_bind::PutNode (made the first time, re-pointed after, under its pac-worker-idle check). A path that has no
//      rows any more (a seat vacated, a re-bind) is re-pointed to an EMPTY node (seat_alias_plan.h EmptyNodeRows): the game's list cannot lose a node, and
//      a stale row would answer first. A write the worker's state refuses is owed (the node's last rows are not updated): Refresh returns false and
//      seat_bind.h runs it again at the next tick its flip gate is open (seat_alias_plan.h RefreshRetry). An AliasNodes refusal (a card / frame the core refuses)
//      is logged once per reason and is not retried (the same inputs refuse again): the nodes are left as they were.
// One line per Refresh at most, only when something was written, refused or is missing. Nothing per frame.
#pragma once

#include <rex/logging.h>

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "guest_call.h"
#include "seat_alias_core.h"
#include "seat_alias_plan.h"
#include "seat_bind.h"
#include "seat_occ.h"
#include "seats.h"

namespace wwe2k14::seats_alias {

namespace st = wwe2k14::seats;
namespace sbd = wwe2k14::seat_bind;
namespace al = wwe2k14::seat_alias;
namespace sc = wwe2k14::seat_card;

constexpr uint32_t kMaxNodeRows = 16u << 20;  // a node of the game's list with more rows than this is not read (its names are not aliased)

inline std::map<std::string, sbd::NodeBuf> g_nodes;                 // game path -> our alias node for it (made the first time it has rows)
inline std::map<std::string, std::vector<uint8_t>> g_last;          // game path -> the rows its node holds now (EmptyNodeRows once emptied)
inline size_t g_missing_logged = 0;
inline std::string g_err_logged;                                     // the last AliasNodes refusal logged (logged once per reason)

// The rows in a node (the directories' row counts, summed).
inline size_t CountRows(const std::vector<uint8_t>& v) {
  size_t at = 0, n = 0;
  while (at + 12 <= v.size() && v[at] != 0xFF) {
    const uint32_t flags = sbd::Be32(&v[at + 4]), words = flags >> 20, kind = (flags >> 12) & 15;
    n += size_t(words) * 4 / (kind ? 16 : 12);
    at += 12 + size_t(words) * 4;
  }
  return n;
}

// The game's node list as NodeViews (head first) with the guest address of each. false = the list is not there yet.
inline bool Walk(uint8_t* base, std::vector<al::NodeView>& game, std::vector<uint32_t>& addrs) {
  game.clear(), addrs.clear();
  const uint32_t mgr = st::U32(base, st::kPacMgrPtr), list = mgr ? st::U32(base, mgr + 72) : 0, head = list ? st::U32(base, list) : 0;
  if (!head) return false;
  int nodes = 0;
  for (uint32_t n = head; n && nodes < int(st::kMaxNodes); n = st::U32(base, n + st::kNodeNext), ++nodes) {
    if (!st::Readable(n, 64)) break;
    const char* path = reinterpret_cast<const char*>(st::P(base, n + st::kNodePath));
    size_t len = 0;
    while (len < st::kPathBytes && path[len]) ++len;
    al::NodeView v;
    v.path.assign(path, len);
    const uint32_t rows = st::U32(base, n + st::kNodeRows);
    size_t span = 0;
    const auto word = [&](size_t off, uint32_t& w) {  // node +52 is not read (its sign depends on the maker): the rows are walked to the 0xFF word
      if (!st::Readable(rows + uint32_t(off), 4)) return false;
      w = st::U32(base, rows + uint32_t(off));
      return true;
    };
    if (rows && al::GameRowsSpan(word, kMaxNodeRows, span) && st::Readable(rows, uint32_t(span))) v.rows.assign(st::P(base, rows), st::P(base, rows) + span);
    game.push_back(std::move(v));
    addrs.push_back(n);
  }
  return true;
}

// Every node address WE made.
inline std::set<uint32_t> OurAddrs() {
  std::set<uint32_t> s;
  for (const st::Resident& r : st::g_roster.res)
    if (r.node_addr) s.insert(r.node_addr);
  for (const auto& kv : st::g_res_ev_node)
    if (kv.second) s.insert(kv.second);
  for (const auto& kv : sbd::g_char_node)
    if (kv.second.node) s.insert(kv.second.node);
  for (const auto& kv : sbd::g_ev_node)
    if (kv.second.node) s.insert(kv.second.node);
  for (const auto& kv : g_nodes)
    if (kv.second.node) s.insert(kv.second.node);
  return s;
}

// false = owed again: the game's list is not there, or a node write was refused (seat_bind.h retries it). An AliasNodes refusal returns true (nothing a retry changes).
inline bool Refresh(scene_director::Caller& call, uint8_t* base) {
  std::vector<al::NodeView> game;
  std::vector<uint32_t> addrs;
  if (!Walk(base, game, addrs)) return false;
  al::MarkOurs(game, addrs, OurAddrs());
  std::map<int, std::vector<sc::Alias>> wants;
  std::map<int, int> bank_of;
  for (const st::Occupant* o : st::Occupants()) {
    if (o->rows.alias.empty()) continue;
    wants[o->seat] = o->rows.alias;
    bank_of[o->seat] = o->bank;
  }
  std::map<std::string, std::vector<uint8_t>> wanted;
  std::string err;
  std::vector<std::string> missing;
  if (!al::AliasNodes(wants, bank_of, game, wanted, err, &missing)) {
    if (err != g_err_logged) REXLOG_WARN("[wwe2k14][alias] {}: the alias nodes are left as they were", err);
    g_err_logged = err;
    return true;
  }
  g_err_logged.clear();
  std::vector<std::string> emptied;
  const auto put = al::PutPlan(wanted, g_last, &emptied);
  int done = 0, failed = 0;
  std::string first_why;
  for (const auto& [path, rows] : put) {
    std::string why;
    if (sbd::PutNode(call, base, g_nodes[path], rows, path, why)) {
      g_last[path] = rows;
      ++done;
    } else if (!failed++) {
      first_why = path + ": " + why;
    }
  }
  size_t nrows = 0;
  for (const auto& kv : wanted) nrows += CountRows(kv.second);
  if (done || failed || (!missing.empty() && missing.size() != g_missing_logged)) {
    std::string names;
    for (size_t i = 0; i < missing.size() && i < 4; ++i) names += (i ? ", " : "") + missing[i];
    REXLOG_INFO("[wwe2k14][alias] {} node(s) written ({} emptied, {} refused{}), now {} node(s) with {} row(s) for {} seat(s); {} name(s) not in the game's list{}{}", done,
                emptied.size(), failed, failed ? ": " + first_why : std::string(), wanted.size(), nrows, wants.size(), missing.size(), missing.empty() ? "" : ": ", names);
  }
  g_missing_logged = missing.size();
  return failed == 0;
}

inline const bool g_registered = (st::g_alias_refresh = &Refresh, true);

}  // namespace wwe2k14::seats_alias
