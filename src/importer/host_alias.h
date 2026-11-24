// S110 LIBSIZE-2: the HOST finder the importer's bind cases (tools\importer\bind_cases.h) resolve alias rows with: the same game / pack pacs the importer's old byte copies came from,
// as a list the way the game's own pac list is (the newest DLC pack first, then the disc), each file as plain bytes (an XCTD file unpacked: the game reads it through its
// decompressed view). The bind cases build src\seat_alias_core.h NodeViews from these, call FindFirst / AliasNodes, and read the bytes each row points at.
// Test support only: nothing in the build uses it. UNITY INCLUDE: importer_core.cpp, inside `namespace importer`, after the Builder.
#pragma once

int HostPackOfCharImpl(const std::string& mods_dir, const launcher::GameFiles& game, const std::string& dlc_dir, const std::string& dlc_off, int id) {
  Builder b;
  static std::vector<std::string> sink;
  b.log = &sink;
  b.game.SetRoot(game.root());
  b.packs = RealPacks(dlc_dir, U8Path(mods_dir) / "superstars", dlc_off);
  const RealPack* p = b.PackOfChar(id);
  return p ? p->id : -1;
}

std::vector<HostPac> HostGamePacsImpl(const std::string& mods_dir, const launcher::GameFiles& game, const std::string& dlc_dir, const std::string& dlc_off, bool sd) {
  auto b = std::make_shared<Builder>();
  static std::vector<std::string> sink;
  b->log = &sink;
  b->game.SetRoot(game.root());
  b->packs = RealPacks(dlc_dir, U8Path(mods_dir) / "superstars", dlc_off);
  std::vector<HostPac> out;
  const std::string menu = sd ? "SD" : "HD";
  // the newest pack first (a later DLC pack's node sits nearer the head of the game's list)
  for (auto it = b->packs.rbegin(); it != b->packs.rend(); ++it) {
    const RealPack* rp = &*it;
    for (const std::string& stem : {std::string("pac\\evt\\Entrance"), std::string("pac\\evt\\Winning"), std::string("pac\\gm"), std::string("pac\\prtcls"), std::string("pac\\evt\\evtmov"),
                                    std::string("pac\\evt\\evtmov2_2"), "pac\\DLC_" + menu}) {
      const std::string rel = stem + "_dlc" + rp->Pp() + ".pac";
      if (rp->Has(rel)) out.push_back({"pack " + std::to_string(rp->id) + " " + rel, [b, rp, rel](Bytes& d) { return b->ReadPackFile(*rp, rel, d); }});
    }
  }
  std::vector<std::string> disc;
  for (const std::string& f : b->game.ListRoot("pac\\evt")) {
    const std::string l = Lower(f);
    for (const char* stem : {"entrance", "winning"}) {
      const size_t n = std::strlen(stem);
      if (l.size() == n + 6 && l.compare(0, n, stem) == 0 && l.compare(n + 2, 4, ".pac") == 0 && std::isdigit(static_cast<unsigned char>(l[n]))) disc.push_back("pac\\evt\\" + f);
    }
  }
  std::sort(disc.begin(), disc.end());
  for (const char* f : {"pac\\evt\\evtmov2_2.pac", "pac\\gm.pac", "pac\\prtcls.pac"}) disc.push_back(f);
  disc.push_back("pac\\menu\\menu" + menu + ".pac");
  for (const std::string& rel : disc)
    if (b->game.Exists(rel)) out.push_back({"disc " + rel, [b, rel](Bytes& d) { return b->ReadGame(rel, d); }});
  return out;
}
