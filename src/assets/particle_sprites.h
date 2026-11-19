// The particle sprite textures of the player's own pac\prtcls.pac, by entry name (Session 103, lane F5; docs\formats\particle_items.md "The texture", prtcls_pac.md).
// Pure over an opened pac (assets::pac::Pac): no game memory, no renderer. Two users: the live namer's NameIndex (every sprite directory, keyed by its mip-0 content: particle_namer.h) and the
// render side's texture lookup (an entry name -> the decoded DDS). Nothing here is written anywhere; the bytes are the player's own and stay in memory.
//
// Entry name: "pBBBB/EEE/1/Name": the first letter names the pac's section (p = PCE6 the entrance banks, m = PCMN the menu / title banks, c = PCCH; CAWH: bank numbers repeat across sections, "0100" is in both PCE6 and
// PCMN, and the editor's and menus' particle pictures live in PCMN), BBBB = the bank ("0107"), EEE = the effect's id in that bank's PACH, 1 = the sprite directory of the effect (N/1 in prtcls_pac.md:
// N/0 = the PRTC, N/1 = the texture directory), Name = the 16-byte directory name ("Light0001"). The same string particle_namer.h stores (AddSpriteDirectory(dir, n, pac, leaf)) and the record's
// Texture AssetRef carries (pac = "pac\\prtcls.pac").
#pragma once
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>

#include "asset_model.h"
#include "dds.h"
#include "pac.h"

namespace assets::particle_sprites {

// Test only (f5_unit --plant pce6only): the old walk, PCE6 banks only. (--plant nopcge: PCE6 / PCMN / PCCH, the PCGE named banks skipped.)
inline bool g_pce6_only = false;
inline bool g_skip_pcge = false;

using DirVisit =std::function<void(const std::string& leaf, const uint8_t* dir, size_t n)>;

// The sections the namer index and the loader read: leaf prefix letter <-> EPAC section name (PCE6 keeps "p" so its names, records and gates are unchanged).
// INN: 'g' = PCGE, whose banks are NAMED ("INFE" the Inferno ring fire, "GAME", "RYAL", "ECW_"): the leaf is "gINFE/110/1", the 4 bank chars verbatim.
inline const char* SectionOfPrefix(char c) { return c == 'p' ? "PCE6" : c == 'm' ? "PCMN" : c == 'c' ? "PCCH" : c == 'g' ? "PCGE" : nullptr; }
inline char PrefixOfSection(const std::string& sec) { return sec == "PCE6" ? 'p' : sec == "PCMN" ? 'm' : sec == "PCCH" ? 'c' : sec == "PCGE" ? 'g' : 0; }

// The records of one section of the pac's EPAC directory (12-byte records from +0x80C: char[4] name, LE32 start in 2 KB sectors from +0x4000, LE32 size). A section marker has a non-numeric name AND size 0; a row with
// size > 0 is a bank, numeric ("0107") or named ("INFE" in PCGE; INN, INF.md of the S103 probes, section 4).
inline size_t SectionOffset(const Bytes& d, const char* section, const char* bank4) {
  if (d.size() < 0x4000 || std::memcmp(d.data(), "EPAC", 4) != 0) return 0;
  std::string sec(reinterpret_cast<const char*>(d.data() + 0x800), 4);
  for (size_t o = 0x80C; o + 12 <= 0x4000; o += 12) {
    const uint8_t* r = d.data() + o;
    const uint32_t st = Le32(d, o + 4);
    const bool digits = std::isdigit(r[0]) && std::isdigit(r[1]) && std::isdigit(r[2]) && std::isdigit(r[3]);
    if (!digits && Le32(d, o + 8) == 0) {   // a section marker (a non-digit row with size > 0 is a named bank)
      if (r[0] == 0 && st == 0) break;
      sec.assign(reinterpret_cast<const char*>(r), 4);
      continue;
    }
    if (sec == section && std::memcmp(r, bank4, 4) == 0) return 0x4000 + size_t(st) * 0x800;
  }
  return 0;
}

// Every sprite directory of every PCE6, PCMN and PCCH bank, in directory order: the callback gets the leaf ("p0107/2/1", "m0100/3/1") and the directory bytes (LE32 count at +0, 32-byte records from +0x10).
// Returns the number of directories visited.
inline uint32_t ForEachDirectory(const pac::Pac& p, const DirVisit& visit) {
  const Bytes& d = p.bytes;
  if (d.size() < 0x4000 || std::memcmp(d.data(), "EPAC", 4) != 0) return 0;
  std::string sec(reinterpret_cast<const char*>(d.data() + 0x800), 4);
  uint32_t n = 0;
  for (size_t o = 0x80C; o + 12 <= 0x4000; o += 12) {
    const uint8_t* r = d.data() + o;
    const uint32_t st = Le32(d, o + 4);
    const bool digits = std::isdigit(r[0]) && std::isdigit(r[1]) && std::isdigit(r[2]) && std::isdigit(r[3]);
    if (!digits && Le32(d, o + 8) == 0) {   // a section marker (a non-digit row with size > 0 is a named bank)
      if (r[0] == 0 && st == 0) break;
      sec.assign(reinterpret_cast<const char*>(r), 4);
      continue;
    }
    const char pre = PrefixOfSection(sec);
    if (!pre || (g_pce6_only && pre != 'p') || (g_skip_pcge && pre == 'g')) continue;   // (CAWH: was PCE6 only: the menu particles of PCMN were never named)
    const size_t at = 0x4000 + size_t(st) * 0x800;
    const pac::Pach* h = nullptr;
    for (const pac::Pach& ph : p.pachs)
      if (ph.base == at) h = &ph;
    if (!h) continue;
    char bank[5] = {char(r[0]), char(r[1]), char(r[2]), char(r[3]), 0};
    for (const pac::Ent& en : h->ents) {
      if (en.id == 16000) continue;   // the alias table
      Bytes nb;
      pac::Pach np;
      if (!pac::GetNested(p.bytes, *h, en.id, nb, np)) continue;
      const Bytes tex = pac::Get(nb, np, 1);
      if (tex.size() < 0x30) continue;
      char leaf[48];
      std::snprintf(leaf, sizeof leaf, "%c%s/%u/1", pre, bank, en.id);
      visit(leaf, tex.data(), tex.size());
      ++n;
    }
  }
  return n;
}

// "gBBBB/EEE/1/Name": the name is the rest of the string verbatim (PCGE has "test (1)", with a space and brackets, which %s would cut at the space).
inline bool GScan(const std::string& entry, unsigned& eid, unsigned& one, char (&name)[40]) {
  int used = 0;
  if (std::sscanf(entry.c_str() + 5, "/%u/%u/%n", &eid, &one, &used) < 2 || used <= 0) return false;
  const std::string nm = entry.substr(5 + size_t(used));
  if (nm.empty() || nm.size() > 16) return false;
  std::memcpy(name, nm.c_str(), nm.size() + 1);
  return true;
}

// "pBBBB/EEE/1/Name" -> the decoded DDS. False (with `why`) when the entry is malformed, absent or not a DDS we read.
inline bool LoadSprite(const pac::Pac& p, const std::string& entry, assets::Texture& out, std::string& why) {
  unsigned bank = 0, eid = 0, one = 0;
  char name[40] = {};
  const char* section = entry.empty() ? nullptr : SectionOfPrefix(entry[0]);
  // 'g' (PCGE): the bank is 4 letters ("INFE"): "%4u" cannot read it, so the parse starts after them; the others keep the numeric parse
  const bool parsed = section && entry.size() >= 5 && (entry[0] == 'g' ? GScan(entry, eid, one, name) : std::sscanf(entry.c_str() + 1, "%4u/%u/%u/%39s", &bank, &eid, &one, name) == 4);
  if (!parsed || one != 1) return why = "not a sprite entry: " + entry, false;
  const std::string bank4 = entry.substr(1, 4);
  const Bytes& d = p.bytes;
  const size_t at = SectionOffset(d, section, bank4.c_str());
  const pac::Pach* h = nullptr;
  for (const pac::Pach& ph : p.pachs)
    if (at && ph.base == at) h = &ph;
  if (!h) return why = std::string("no ") + section + " bank " + bank4, false;
  Bytes nb;
  pac::Pach np;
  if (!pac::GetNested(p.bytes, *h, eid, nb, np)) return why = "no effect " + std::to_string(eid) + " in bank " + bank4, false;
  const Bytes tex = pac::Get(nb, np, 1);
  if (tex.size() < 0x30) return why = "the effect has no sprite directory", false;
  const uint32_t count = Le32(tex, 0);
  for (uint32_t i = 0; i < count && 0x10 + size_t(i + 1) * 32 <= tex.size(); ++i) {
    const size_t r = 0x10 + size_t(i) * 32;
    const std::string nm(reinterpret_cast<const char*>(tex.data() + r), strnlen(reinterpret_cast<const char*>(tex.data() + r), 16));
    if (nm != name) continue;
    const uint32_t size = Le32(tex, r + 20), off = Le32(tex, r + 24);
    if (off > tex.size() || size > tex.size() - off) return why = "directory record past the end", false;
    std::string w;
    if (!ParseDds(tex.data() + off, size, out, w)) return why = "not a readable DDS: " + w, false;
    out.name = nm;
    return true;
  }
  return why = "no texture " + std::string(name) + " in " + entry.substr(0, entry.rfind('/')), false;
}

}  // namespace assets::particle_sprites
