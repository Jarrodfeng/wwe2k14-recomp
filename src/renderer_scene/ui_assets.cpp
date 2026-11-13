// Session 101 lane 9: see ui_assets.h. Formats: docs\formats\ui_font.md (the font), docs\formats\string_pac.md (the strings), src\assets\pac.h
// (the containers), src\assets\dds.h (the texture files).
#include "ui_assets.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <stb_image.h>  // declarations only: see ui_assets.h

#include "../assets/dds.h"
#include "../importer/pack_io.h"
#include "../xctd_file.h"

namespace rdr_scene {
namespace {

using assets::Bytes;
namespace pac = assets::pac;

bool ToFormat(assets::TexFormat f, rdr::Format& out) {
  switch (f) {
    case assets::TexFormat::kBC1: return out = rdr::Format::BC1, true;
    case assets::TexFormat::kBC2: return out = rdr::Format::BC2, true;
    case assets::TexFormat::kBC3: return out = rdr::Format::BC3, true;
    case assets::TexFormat::kB8G8R8A8: return out = rdr::Format::BGRA8, true;
    case assets::TexFormat::kR8G8B8A8: return out = rdr::Format::RGBA8, true;
    // S101 lane 23: A4R4G4B4 (916 textures in the atlas, all in model directories, none under pac\menu) -> rdr::Format::RGBA4 by a repack at load.
    case assets::TexFormat::kB4G4R4A4: return out = rdr::Format::RGBA4, true;
    default: return false;
  }
}

// The DDS A4R4G4B4 word (the file's little-endian u16: A = bits 15..12, R = 11..8, G = 7..4, B = 3..0; the DDS header's masks are A 0xF000, R 0x0F00,
// G 0x00F0, B 0x000F, src\assets\dds.h) becomes the renderer's RGBA4 word (B = bits 15..12, G = 11..8, R = 7..4, A = 3..0: Vulkan's
// B4G4R4A4_UNORM_PACK16, src\renderer\renderer.cpp ToVk). The two are different 16-bit layouts (the viewer's TexFormat name "B4G4R4A4" follows DXGI,
// which lists the fields low bits first): uploading the file's words as they are turns A into B, R into A, G into R and B into G.
void Repack4444(std::vector<uint8_t>& data) {
  for (size_t i = 0; i + 1 < data.size(); i += 2) {
    const uint16_t w = uint16_t(data[i] | (data[i + 1] << 8));
    const uint16_t a = w >> 12, r = (w >> 8) & 0xF, g = (w >> 4) & 0xF, b = w & 0xF;
    const uint16_t out = uint16_t((b << 12) | (g << 8) | (r << 4) | a);
    data[i] = uint8_t(out), data[i + 1] = uint8_t(out >> 8);
  }
}

std::string Num(unsigned long v) { return std::to_string(v); }

bool SplitEntry(const std::string& entry, std::vector<uint32_t>& path, std::string& name, std::string& err) {
  path.clear(), name.clear();
  std::string head = entry;
  if (const size_t colon = entry.find(':'); colon != std::string::npos) head = entry.substr(0, colon), name = entry.substr(colon + 1);
  if (head.size() < 2 || head[0] != 'p') return err = "entry path '" + entry + "' does not start with p<N>", false;
  size_t pos = 1;
  while (pos <= head.size()) {
    const size_t slash = head.find('/', pos);
    const std::string part = head.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
    if (part.empty() || part.find_first_not_of("0123456789") != std::string::npos) return err = "entry path '" + entry + "' has a bad part '" + part + "'", false;
    path.push_back(uint32_t(std::strtoul(part.c_str(), nullptr, 10)));
    if (slash == std::string::npos) break;
    pos = slash + 1;
  }
  if (path.size() < 2) return err = "entry path '" + entry + "' names no entry (p<N>/<id>)", false;
  return true;
}

}  // namespace

struct UiAssets::Impl {
  launcher::GameFiles files;
  std::map<std::string, std::unique_ptr<pac::Pac>> pacs;
  std::map<std::string, UiImage> textures;
  bool strings_loaded = false;
  std::string strings_error;
  std::unordered_map<uint32_t, std::string> strings;

  const pac::Pac* OpenPac(const std::string& rel, std::string& err) {
    auto it = pacs.find(rel);
    if (it != pacs.end()) return it->second.get();
    auto p = std::make_unique<pac::Pac>();
    if (!pac::Open(files, rel, *p, err)) return nullptr;
    return (pacs[rel] = std::move(p)).get();
  }

  // The leaf the entry path names, decoded. `name` is set when the path ended with ":name".
  bool Leaf(const std::string& rel, const std::string& entry, Bytes& blob, std::string& name, std::string& err) {
    std::vector<uint32_t> path;
    if (!SplitEntry(entry, path, name, err)) return false;
    const pac::Pac* pc = OpenPac(rel, err);
    if (!pc) return false;
    if (path[0] >= pc->pachs.size()) return err = rel + ": no top-level PACH " + Num(path[0]) + " (it has " + Num(pc->pachs.size()) + ")", false;
    const Bytes* buf = &pc->bytes;
    const pac::Pach* pach = &pc->pachs[path[0]];
    Bytes cur;
    pac::Pach cur_pach;
    for (size_t i = 1; i < path.size(); ++i) {
      if (!pac::Find(*pach, path[i])) return err = rel + " " + entry + ": no entry " + Num(path[i]) + " at depth " + Num(i), false;
      Bytes next = pac::Get(*buf, *pach, path[i]);
      if (next.empty()) return err = rel + " " + entry + ": entry " + Num(path[i]) + " is empty or does not decode", false;
      if (i + 1 < path.size()) {
        pac::Pach np;
        if (!pac::ParsePach(next, 0, next.size(), np)) return err = rel + " " + entry + ": entry " + Num(path[i]) + " is not a PACH", false;
        cur = std::move(next), cur_pach = std::move(np);
        buf = &cur, pach = &cur_pach;
      } else {
        blob = std::move(next);
      }
    }
    return true;
  }

  static bool FromDds(const Bytes& blob, const std::string& what, UiImage& out, std::string& err) {
    assets::Texture t;
    std::string why;
    if (!assets::ParseDds(blob.data(), blob.size(), t, why)) return err = what + ": " + why, false;
    if (!why.empty()) return err = what + ": " + why, false;  // a short mip chain is a surprise: fail, do not draw half a texture
    if (!ToFormat(t.format, out.format)) return err = what + ": the DDS pixel format is not one the 2D pass uploads yet", false;
    out.width = t.width, out.height = t.height, out.mips = t.mips, out.data = std::move(t.data);
    if (t.format == assets::TexFormat::kB4G4R4A4) Repack4444(out.data);
    return true;
  }

  bool LoadStrings() {
    if (strings_loaded) return strings_error.empty();
    strings_loaded = true;
    Bytes raw;
    const std::string rel = "pac\\string.pac";
    if (!files.Exists(rel)) return strings_error = "missing " + rel, false;
    if (!files.Stream(rel, [&](const uint8_t* p, size_t n) { raw.insert(raw.end(), p, p + n); })) return strings_error = "read failed " + rel, false;
    Bytes plain;
    if (wwe2k14::xctd::IsXctd(raw)) {
      std::string e;
      if (!wwe2k14::xctd::Unpack(raw, plain, e)) return strings_error = rel + ": " + e, false;
    } else {
      plain = std::move(raw);
    }
    importer::io::Pac sp;
    if (!importer::io::ReadPac(plain, sp, strings_error)) return strings_error = rel + ": " + strings_error, false;
    importer::io::Dir* sdb = sp.FindDir("SDB ");
    if (!sdb) return strings_error = rel + ": no SDB directory", false;
    for (const char* wanted : {"0000", "5000"}) {
      const importer::io::Entry* ent = nullptr;
      for (const importer::io::Entry& e : sdb->ents)
        if (importer::io::Trim(e.name) == wanted) ent = &e;
      if (!ent) return strings_error = rel + ": no SDB entry " + wanted, false;
      std::vector<importer::io::Leaf> leaves;
      if (!importer::io::ReadPach(importer::io::Blob(*sdb, *ent), leaves)) return strings_error = rel + ": SDB " + wanted + " is not a PACH", false;
      for (const importer::io::Leaf& l : leaves) {
        if (l.raw.size() < 16 || std::memcmp(l.raw.data(), "ZLIB", 4) != 0) continue;
        const Bytes d = pac::Decode(l.raw.data(), l.raw.size());
        if (d.size() < 8 || assets::Le32(d, 0) != 0) continue;  // (a leaf can be an empty table)
        const uint32_t count = assets::Le32(d, 4);
        if (count > 200000 || 8 + 12ull * count > d.size()) return strings_error = rel + ": a string table claims " + Num(count) + " records", false;
        for (uint32_t i = 0; i < count; ++i) {
          const uint32_t off = assets::Le32(d, 8 + 12 * size_t(i)), len = assets::Le32(d, 8 + 12 * size_t(i) + 4), id = assets::Le32(d, 8 + 12 * size_t(i) + 8);
          if (uint64_t(off) + len > d.size() || len == 0) return strings_error = rel + ": a string record lies outside its table", false;
          if (strings.count(id)) continue;
          strings.emplace(id, std::string(reinterpret_cast<const char*>(d.data()) + off, len - 1));  // (the length counts the NUL)
        }
      }
    }
    if (strings.empty()) return strings_error = rel + ": no strings found", false;
    return true;
  }
};

UiAssets::UiAssets(std::filesystem::path game_root) : impl_(new Impl) { impl_->files.SetRoot(game_root); }
UiAssets::~UiAssets() = default;

bool UiAssets::Texture(const std::string& pac_rel, const std::string& entry, UiImage& out, std::string& err) {
  const std::string key = pac_rel + "|" + entry;
  if (auto it = impl_->textures.find(key); it != impl_->textures.end()) return out = it->second, true;
  Bytes blob;
  std::string name;
  if (!impl_->Leaf(pac_rel, entry, blob, name, err)) return false;
  const std::string what = pac_rel + " " + entry;
  UiImage image;
  if (name.empty()) {
    if (!Impl::FromDds(blob, what, image, err)) return false;
  } else {
    assets::TexSet set;
    if (!set.AddDir(std::move(blob))) return err = what + ": the leaf is not a texture directory (a name was given)", false;
    assets::Texture t;
    std::string why;
    if (!set.Peek(name, t, why)) return err = what + ": texture '" + name + "': " + why, false;
    if (!why.empty()) return err = what + ": texture '" + name + "': " + why, false;
    if (!ToFormat(t.format, image.format)) return err = what + ": texture '" + name + "': pixel format not uploaded by the 2D pass yet", false;
    image.width = t.width, image.height = t.height, image.mips = t.mips, image.data = std::move(t.data);
    if (t.format == assets::TexFormat::kB4G4R4A4) Repack4444(image.data);
  }
  impl_->textures[key] = image;
  return out = std::move(image), true;
}

bool UiAssets::Font(UiFont& out, std::string& err, uint32_t index) {
  out = {};
  if (index > 2) return err = "font index " + Num(index) + " is not one of 0 / 1 / 2", false;
  static const float kTrim[3] = {2.0f, 2.0f, 7.0f};  // lane 62 (UJ) census, see UiFont::advance_trim
  out.advance_trim = kTrim[index];
  const std::string name = std::string("font00") + char('0' + index);
  const std::string rel = "pac\\menu\\fontHD.pac";
  const pac::Pac* pc = impl_->OpenPac(rel, err);
  if (!pc) return false;
  if (pc->pachs.empty()) return err = rel + ": no PACH", false;
  const pac::Pach& top = pc->pachs[0];
  const Bytes sheets = pac::Get(pc->bytes, top, 10 * index), table = pac::Get(pc->bytes, top, 10 * index + 1);
  pac::Pach sheet_pach;
  if (!pac::ParsePach(sheets, 0, sheets.size(), sheet_pach) || sheet_pach.ents.empty()) return err = rel + ": entry 0 is not a PACH of sheets", false;
  const Bytes dds = pac::Get(sheets, sheet_pach, sheet_pach.ents[0].id);
  UiImage sheet;
  if (!Impl::FromDds(dds, rel + " " + name + " sheet", sheet, err)) return false;
  if (sheet.format != rdr::Format::BC3 || sheet.width != 512 || sheet.height != 512) return err = rel + ": the font sheet is not the 512 x 512 BC3 the layout notes describe", false;
  if (table.size() < 0x100 || std::memcmp(table.data() + 0x10, ("name=" + name + ".yfb").c_str(), 16) != 0) return err = rel + ": the glyph table is not " + name + ".yfb", false;
  const uint32_t n = assets::Le32(table, 0xA4);
  const size_t codes = 0xE8, recs = (codes + 2 * size_t(n) + 7) & ~size_t(7);
  if (n == 0 || n > 4096 || recs + 16 * size_t(n) > table.size()) return err = rel + ": the .yfb glyph table is malformed", false;
  for (uint32_t k = 0; k < n; ++k) {
    uint16_t c = 0, r[4];
    std::memcpy(&c, table.data() + codes + 2 * k, 2);
    std::memcpy(r, table.data() + recs + 16 * size_t(k), 8);
    if (r[0] + r[2] > sheet.width || r[1] + r[3] > sheet.height) return err = rel + ": a glyph record lies off the sheet (code " + Num(c) + ")", false;
    out.glyphs[c] = {r[0], r[1], r[2], r[3]};
    out.cell = std::max(out.cell, float(r[3]));
  }
  // S101 U1: the kerning pair list after the glyph records: u16 left, u16 right, u32 value (little-endian), `count` at +0xB4 (font000: 53, e.g. A-V 4).
  // A list that does not fit the table or names a code the font lacks is a failure (loud), not a silent zero.
  const size_t tail = recs + 16 * size_t(n);
  const uint32_t pairs = assets::Le32(table, 0xB4);
  if (pairs > 4096 || tail + 8 * size_t(pairs) > table.size()) return err = rel + ": the .yfb kerning pair list does not fit the table (" + Num(pairs) + " pairs)", false;
  for (uint32_t k = 0; k < pairs; ++k) {
    uint16_t lr[2];
    uint32_t v = 0;
    std::memcpy(lr, table.data() + tail + 8 * size_t(k), 4);
    std::memcpy(&v, table.data() + tail + 8 * size_t(k) + 4, 4);
    if (!out.glyphs.count(lr[0]) || !out.glyphs.count(lr[1]) || v > 64) return err = rel + ": a kerning pair names a code the font lacks or a value above 64 (pair " + Num(k) + ")", false;
    out.kerns[(uint32_t(lr[0]) << 16) | lr[1]] = int32_t(v);
  }
  out.sheet = std::move(sheet);
  if (out.cell <= 0) return err = rel + ": no glyph cell height", false;
  return true;
}

bool UiAssets::String(uint32_t id, std::string& out, std::string& err) {
  if (!impl_->LoadStrings()) return err = impl_->strings_error, false;
  auto it = impl_->strings.find(id);
  if (it == impl_->strings.end()) return err = "string id " + Num(id) + " is not in string.pac (entries 0000 / 5000)", false;
  return out = it->second, true;
}

size_t UiAssets::string_count() const { return impl_->strings.size(); }

}  // namespace rdr_scene
