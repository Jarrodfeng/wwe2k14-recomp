// S106 PSEED: prints the shader catalog's hashes (one line per SPIR-V blob of kVariants and its alpha twin, then of our own kOwnShaders (S110): "<hash hex> <name> <iface hex> <V|F|?>"), hashed the way the renderer registers them
// (pstore::HashWords over the compiled words, renderer.cpp StoreFindProgram). The last column is the blob's stage from its OpEntryPoint execution model (V vertex, F fragment, ? neither):
// tools\renderer\pipeline_seed.py --check reads this to refuse a stale seed (a seed line whose hash the current shaders no longer have is useless: it stays unresolved and builds
// nothing) and a record whose vertex hash is a fragment blob (or the reverse). No Python re-implementation of the hash.
// S111 PSEEDSTALE: the third column is the blob's interface (IfaceOf: its located Input / Output variables, each with its location and scalar kind / width / components), so
// tools\renderer\pseed_rekey.py moves a seed record onto a changed shader's new hash only when the pipeline it describes is the same (a changed input or output needs a harvest).
// (a catalog printer, not a unit: it calls the renderer's public rdr::pstore::HashWords (pipeline_store.h) over the compiled kVariants, as the catalog registration does. No `entry:` line: handback check
// e.units matches the symbol by its last name, and frame_builder.h has a private static HashWords of its own, so it would read this one as file-local.)
// Build: clang++ -std=c++20 -o out\tools\pseed_catalog.exe tools\renderer\pseed_catalog.cpp     Run: out\tools\pseed_catalog.exe > catalog.txt
#include <algorithm>
#include <cstdio>
#include <map>
#include <utility>
#include <vector>

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

// S111 PSEEDSTALE: FNV-1a 64 over the sorted (storage class, Location, kind / components / width) of every Input (1) / Output (3) OpVariable that carries a Location
// decoration (built-ins such as gl_Position carry none). Opcodes: OpDecorate 71 (decoration Location 30), OpTypeInt 21, OpTypeFloat 22, OpTypeVector 23, OpTypePointer 32,
// OpVariable 59. A type it does not shape (a matrix, an array, a struct) counts as kind 0: the location and storage still count.
static uint64_t IfaceOf(const uint32_t* w, size_t bytes) {
  const size_t n = bytes / 4;
  std::map<uint32_t, uint32_t> loc, shape;                     // id -> Location; type id -> kind << 16 | components << 8 | width
  std::map<uint32_t, std::pair<uint32_t, uint32_t>> ptr, var;  // pointer type id -> (storage, pointee); variable id -> (storage, pointer type)
  for (size_t i = 5; i < n;) {
    const uint32_t op = w[i] & 0xFFFF, len = w[i] >> 16;
    if (!len || i + len > n) break;
    if (op == 71 && len >= 4 && w[i + 2] == 30) loc[w[i + 1]] = w[i + 3];
    else if (op == 21 && len >= 4) shape[w[i + 1]] = ((w[i + 3] ? 2u : 3u) << 16) | (1u << 8) | (w[i + 2] & 0xFF);
    else if (op == 22 && len >= 3) shape[w[i + 1]] = (1u << 16) | (1u << 8) | (w[i + 2] & 0xFF);
    else if (op == 23 && len >= 4) {
      const auto c = shape.find(w[i + 2]);
      shape[w[i + 1]] = (c == shape.end() ? 0u : (c->second & 0xFF00FFu)) | ((w[i + 3] & 0xFF) << 8);
    } else if (op == 32 && len >= 4) ptr[w[i + 1]] = {w[i + 2], w[i + 3]};
    else if (op == 59 && len >= 4 && (w[i + 3] == 1 || w[i + 3] == 3)) var[w[i + 2]] = {w[i + 3], w[i + 1]};
    i += len;
  }
  std::vector<uint64_t> items;
  for (const auto& [id, sv] : var) {
    const auto l = loc.find(id);
    if (l == loc.end()) continue;
    uint32_t s = 0;
    const auto p = ptr.find(sv.second);
    if (p != ptr.end()) {
      const auto t = shape.find(p->second.second);
      if (t != shape.end()) s = t->second;
    }
    items.push_back((uint64_t(sv.first) << 48) | (uint64_t(l->second & 0xFFFF) << 32) | s);
  }
  std::sort(items.begin(), items.end());
  uint64_t h = 0xcbf29ce484222325ull;
  for (const uint64_t x : items)
    for (int b = 0; b < 8; ++b) h = (h ^ ((x >> (8 * b)) & 0xFF)) * 0x100000001b3ull;
  return h;
}

static void Print(const uint32_t* words, size_t bytes, const char* name) {
  std::printf("%016llx %s %016llx %c\n", static_cast<unsigned long long>(rdr::pstore::HashWords(words, bytes)), name, static_cast<unsigned long long>(IfaceOf(words, bytes)), StageOf(words, bytes));
}

int main() {
  size_t n = 0;
  for (size_t i = 0; i < sizeof(rdr::fx::kVariants) / sizeof(rdr::fx::kVariants[0]); ++i) {   // S111 TOOLLINK: the words by index (rdr::fx::Words), as the catalog registration reads them
    const auto& v = rdr::fx::kVariants[i];
    const uint32_t* w = rdr::fx::Words(i, false);
    const uint32_t* wa = rdr::fx::Words(i, true);
    if (w) Print(w, v.bytes, v.name), ++n;
    if (wa) Print(wa, v.bytes_alpha, v.name), ++n;
  }
  for (const auto& o : rdr::fx::kOwnShaders)   // S110 RDRPREP: our own blobs are in the shader catalog too (pipeline_catalog.h): a seed line of an own program resolves and is kept
    if (o.words) Print(o.words, o.bytes, o.name), ++n;
  std::fprintf(stderr, "pseed_catalog: %zu blobs\n", n);
  return n ? 0 : 1;
}
