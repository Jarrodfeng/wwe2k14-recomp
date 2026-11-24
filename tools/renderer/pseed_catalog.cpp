// S106 PSEED: prints the shader catalog's hashes (one line per SPIR-V blob of kVariants and its alpha twin, then of our own kOwnShaders (S110): "<hash hex> <name> <V|F|?>"), hashed the way the renderer registers them
// (pstore::HashWords over the compiled words, renderer.cpp StoreFindProgram). The third column is the blob's stage from its OpEntryPoint execution model (V vertex, F fragment, ? neither):
// tools\renderer\pipeline_seed.py --check reads this to refuse a stale seed (a seed line whose hash the current shaders no longer have is useless: it stays unresolved and builds
// nothing) and a record whose vertex hash is a fragment blob (or the reverse). No Python re-implementation of the hash.
// (a catalog printer, not a unit: it calls the renderer's public rdr::pstore::HashWords (pipeline_store.h) over the compiled kVariants, as the catalog registration does. No `entry:` line: handback check
// e.units matches the symbol by its last name, and frame_builder.h has a private static HashWords of its own, so it would read this one as file-local.)
// Build: clang++ -std=c++20 -o out\tools\pseed_catalog.exe tools\renderer\pseed_catalog.cpp     Run: out\tools\pseed_catalog.exe > catalog.txt
#include <cstdio>

#include "../../src/renderer/pipeline_store.h"
#include "../../src/renderer/shaders/spv/fx_registry.h"

// SPIR-V: a 5-word header, then instructions (word 0 = wordcount << 16 | opcode). OpEntryPoint (15): word 1 = the execution model (0 vertex, 4 fragment).
static char StageOf(const uint32_t* w, size_t bytes) {
  const size_t n = bytes / 4;
  for (size_t i = 5; i < n;) {
    const uint32_t op = w[i] & 0xFFFF, len = w[i] >> 16;
    if (!len) break;
    if (op == 15 && i + 1 < n) return w[i + 1] == 0 ? 'V' : w[i + 1] == 4 ? 'F' : '?';
    i += len;
  }
  return '?';
}

int main() {
  size_t n = 0;
  for (const auto& v : rdr::fx::kVariants) {
    if (v.spirv) std::printf("%016llx %s %c\n", static_cast<unsigned long long>(rdr::pstore::HashWords(v.spirv, v.bytes)), v.name, StageOf(v.spirv, v.bytes)), ++n;
    if (v.spirv_alpha) std::printf("%016llx %s %c\n", static_cast<unsigned long long>(rdr::pstore::HashWords(v.spirv_alpha, v.bytes_alpha)), v.name, StageOf(v.spirv_alpha, v.bytes_alpha)), ++n;
  }
  for (const auto& o : rdr::fx::kOwnShaders)   // S110 RDRPREP: our own blobs are in the shader catalog too (pipeline_catalog.h): a seed line of an own program resolves and is kept
    if (o.words) std::printf("%016llx %s %c\n", static_cast<unsigned long long>(rdr::pstore::HashWords(o.words, o.bytes)), o.name, StageOf(o.words, o.bytes)), ++n;
  std::fprintf(stderr, "pseed_catalog: %zu blobs\n", n);
  return n ? 0 : 1;
}
