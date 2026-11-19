// rf_b_keys (Session 103, lane B, review B7): file-only check of the record's model naming. For every pac\ch\chNNN.pac of the game folder and every model PACH of it
// (EMD name = the model key K = character * 100 + game attire * 10 + kind), GameFilesSource::Load(pac, "model_key:K", variant = garbage) must load THAT PACH
// (the model's label says "wrestler N attire <ordinal>" and the ordinal's own key is K). --fault resolves the old way (variant = the key's attire digit read as the n-th PACH):
// ch161's 1610 2 / 1610 4 / 1611 2 then load the wrong PACH and the check must FAIL.
// S103 CMB: `rf_b_keys --game DIR --composite COLOURS.bin` loads Heenan's ch249 key 24902 plain and with the composite tag of those 48 colour bytes (attire_compose.h EntryTag): the tagged
//   model's "color" texture must be BC1 2048x1024 with 4 mips, differ from the plain one, and its jacket box (x 820-940, y 150-250 of the atlas) must be dark where the plain base is light.
//   rf_b_keys --game DIR [--fault]      exit 0 = every key resolved to its own PACH; 1 = mismatches (printed); 3 = no game files
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#if __has_include("stb_image.h")
#include "stb_image.h"
#else
#include "../../thirdparty/rexglue-sdk-src/thirdparty/stb/stb_image.h"
#endif
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "../../src/assets/asset_load.h"
#include "../../src/assets/attire_compose.h"
#include "../../src/renderer_scene/live_assets.h"

// (S103 CMB: live_assets.cpp says its census lines through rdr::Log, which this file-only build does not link: print them)
namespace rdr {
void Log(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(stderr, fmt, ap);
  std::fputc('\n', stderr);
  va_end(ap);
}
}  // namespace rdr

int main(int argc, char** argv) {
  std::string game;
  bool fault = false, composite = false;
  std::string composite_colours;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--game") && i + 1 < argc) game = argv[++i];
    else if (!std::strcmp(argv[i], "--fault")) fault = true;
    else if (!std::strcmp(argv[i], "--composite") && i + 1 < argc) composite = true, composite_colours = argv[++i];
  }
  rdr_scene::GameFilesSource src;
  if (game.empty() || !src.SetRoot(game)) { std::fprintf(stderr, "rf_b_keys: no game files\n"); return 3; }
  launcher::GameFiles files;
  files.SetRoot(std::filesystem::u8path(game));
  if (composite) {
    uint8_t cols[48];
    FILE* f = std::fopen(composite_colours.c_str(), "rb");
    if (!f || std::fread(cols, 1, 48, f) != 48) { std::fprintf(stderr, "rf_b_keys: cannot read 48 colour bytes from %s\n", composite_colours.c_str()); return 3; }
    std::fclose(f);
    assets::Model plain, comp;
    std::string err;
    const char* pac = "pac\\ch\\ch249.pac";
    if (!src.Load(pac, "model_key:24902", 0, plain, err) || !src.Load(pac, "model_key:24902" + attire_compose::EntryTag(cols), 0, comp, err)) { std::fprintf(stderr, "composite: load failed: %s\n", err.c_str()); return 1; }
    auto color = [](assets::Model& m) -> assets::Texture* { for (auto& t : m.textures) if (assets::Lower(t.name) == "color" && t.width == 2048 && t.height == 1024) return &t; return nullptr; };
    assets::Texture *a = color(plain), *b = color(comp);
    if (!a || !b) { std::fprintf(stderr, "composite: no 2048x1024 \"color\" texture (plain %d, composite %d)\n", a != nullptr, b != nullptr); return 1; }
    auto box = [](const assets::Texture& t) {
      std::vector<uint8_t> px;
      attire_compose::DecodeBc1Level(t.data.data(), t.data.size(), t.width, t.height, px);
      double sum = 0;
      int n = 0;
      for (int y = 150; y < 250; ++y) for (int x = 820; x < 940; ++x) for (int c = 0; c < 3; ++c) sum += px[(size_t(y) * t.width + size_t(x)) * 4 + size_t(c)], ++n;
      return sum / n;
    };
    const double ja = box(*a), jb = box(*b);
    assets::Model refused;
    std::string rerr;
    const bool refusal_fails = !src.Load(pac, "model_key:24902" + std::string(attire_compose::RefusalTag()), 0, refused, rerr) && !rerr.empty();   // CMB fix 3: a refusal is a failed load (a hole), never the plain base
    if (!refusal_fails) { std::fprintf(stderr, "composite: the refusal tag did not fail the load\n"); return 1; }
    const bool ok =a->format == assets::TexFormat::kBC1 && b->format == assets::TexFormat::kBC1 && b->mips == 4 && b->data.size() == a->data.size() && b->data != a->data && ja > 150 && jb < 70;
    std::printf("rf_b_keys composite: plain jacket box %.1f, composite %.1f, composite %u mips %zu bytes (plain %u mips %zu): %s\n", ja, jb, b->mips, b->data.size(), a->mips, a->data.size(), ok ? "ok" : "FAIL");
    return ok ? 0 : 1;
  }
  int pacs = 0, keys = 0, bad = 0, unreadable = 0;
  for (const auto& e : std::filesystem::directory_iterator(std::filesystem::u8path(game) / "pac" / "ch")) {
    int id = 0;
    const std::string name = e.path().filename().string();
    if (std::sscanf(name.c_str(), "ch%d.pac", &id) != 1 || name.size() != 9) continue;  // chNNN.pac only
    const int n = assets::AttireCount(files, id);
    if (n <= 0) { ++unreadable; continue; }
    ++pacs;
    for (int ord = 0; ord < n; ++ord) {
      const int key = assets::ModelKey(files, id, ord);
      if (key < 0) continue;
      ++keys;
      char pac[32];
      std::snprintf(pac, sizeof pac, "pac\\ch\\ch%03d.pac", id);
      assets::Model m;
      std::string err;
      int got = -1;
      if (!fault) {
        if (src.Load(pac, "model_key:" + std::to_string(key), 99, m, err)) std::sscanf(m.label.c_str(), "wrestler %*d attire %d", &got);
      } else {  // the old rule: variant = the attire digit, read as the n-th model PACH
        const int digit = (key % 100) / 10;
        if (src.Load(pac, "", uint32_t(digit), m, err)) std::sscanf(m.label.c_str(), "wrestler %*d attire %d", &got);
      }
      if (got != ord) {
        ++bad;
        if (bad <= 12) std::fprintf(stderr, "MISMATCH ch%03d key %d: wanted PACH ordinal %d, loaded %d%s%s\n", id, key, ord, got, err.empty() ? "" : " : ", err.c_str());
      }
    }
  }
  std::printf("rf_b_keys: %d pacs, %d model keys, %d mismatches, %d unreadable pacs%s\n", pacs, keys, bad, unreadable, fault ? " (FAULT: variant used as the ordinal)" : "");
  return bad ? 1 : 0;
}
