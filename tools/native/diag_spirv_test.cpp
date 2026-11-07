// Session 95: the offline test of the diagnostic run's SPIR-V passes (thirdparty sdk src\graphics\vulkan\native\diag_spirv.cpp).
// Reads a vertex module, writes the patched module and the two fragment modules; tools\native\diag_spirv_test.py builds this, feeds it
// modules from glslang (our own test GLSL; nothing from the game) and runs them through spirv-val.
//   diag_spirv_test IN.spv OUT.spv OUT_capture_fs.spv OUT_solo_fs.spv [MAX_LOCATIONS]
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "diag_spirv.h"

using namespace rex::graphics::vulkan::diag;

static std::vector<uint32_t> Load(const char* path) {
  std::vector<uint32_t> words;
  FILE* f = std::fopen(path, "rb");
  if (!f) return words;
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  words.resize(size_t(size) / 4);
  if (std::fread(words.data(), 4, words.size(), f) != words.size()) words.clear();
  std::fclose(f);
  return words;
}
static bool Save(const char* path, const std::vector<uint32_t>& words) {
  FILE* f = std::fopen(path, "wb");
  if (!f) return false;
  const bool ok = std::fwrite(words.data(), 4, words.size(), f) == words.size();
  std::fclose(f);
  return ok;
}

int main(int argc, char** argv) {
  if (argc < 5) return 2;
  const std::vector<uint32_t> in = Load(argv[1]);
  if (in.empty()) {
    std::printf("cannot read %s\n", argv[1]);
    return 2;
  }
  const uint32_t max_locations = argc > 5 ? uint32_t(std::atoi(argv[5])) : 16;
  PositionPatch patch;
  std::string why;
  if (!PatchPositionCapture(in.data(), in.size(), max_locations, patch, &why)) {
    std::printf("REFUSED: %s\n", why.c_str());
    return 3;
  }
  if (!Save(argv[2], patch.words) || !Save(argv[3], CaptureFragmentModule(patch.location)) || !Save(argv[4], SoloFragmentModule())) return 2;
  std::printf("OK location %u, %zu -> %zu words\n", patch.location, in.size(), patch.words.size());
  return 0;
}
