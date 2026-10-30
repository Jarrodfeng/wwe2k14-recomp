// Session 73 (gpu_native_ps_relaxed_precision): runs the SDK's RelaxedPrecision pass
// (thirdparty/rexglue-sdk-src/src/graphics/pipeline/shader/spirv_relaxed_precision.cpp) over SPIR-V files without the
// game. Built and driven by run.ps1 next to this file, which also runs spirv-val on every output.
//
//   fp16_pass_test <out_dir> [--verbose] <file or folder>...
//
// Each input module (a .spv / .bin.frag / .bin.vert file, or every such file in a folder) is passed through
// AddRelaxedPrecision; a module the pass changed is written to <out_dir>/<name>.relaxed.spv. One line per file:
// "RESULT <name> <fragment|skipped> decorated=N candidates=N kept_full=N already=N". --verbose also lists every
// candidate result id with its opcode and whether it was decorated (for the small hand-written test shaders).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "src/graphics/pipeline/shader/spirv_relaxed_precision.h"

namespace fs = std::filesystem;
using rex::graphics::spirv_relaxed_precision::AddRelaxedPrecision;
using rex::graphics::spirv_relaxed_precision::Stats;

namespace {

bool ReadWords(const fs::path& path, std::vector<uint32_t>& words) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::vector<char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (bytes.size() < 20 || bytes.size() % 4) return false;
  words.resize(bytes.size() / 4);
  std::memcpy(words.data(), bytes.data(), bytes.size());
  return words[0] == 0x07230203u;
}

const char* OpName(uint32_t op, uint32_t ext) {
  switch (op) {
    case 127: return "OpFNegate";
    case 129: return "OpFAdd";
    case 131: return "OpFSub";
    case 133: return "OpFMul";
    case 136: return "OpFDiv";
    case 140: return "OpFRem";
    case 141: return "OpFMod";
    case 142: return "OpVectorTimesScalar";
    case 143: return "OpMatrixTimesScalar";
    case 144: return "OpVectorTimesMatrix";
    case 145: return "OpMatrixTimesVector";
    case 146: return "OpMatrixTimesMatrix";
    case 148: return "OpDot";
    case 87: return "OpImageSampleImplicitLod";
    case 88: return "OpImageSampleExplicitLod";
    case 89: return "OpImageSampleDrefImplicitLod";
    case 90: return "OpImageSampleDrefExplicitLod";
    case 12: return ext ? "OpExtInst(GLSL)" : "OpExtInst";
    default: return nullptr;
  }
}

// Every float arithmetic / sample result id of the listed kinds with its opcode, and the ids decorated in `relaxed`.
void ListCandidates(const std::vector<uint32_t>& in, const std::vector<uint32_t>& relaxed) {
  std::set<uint32_t> decorated;
  for (size_t at = 5; at < relaxed.size();) {
    const uint32_t wc = relaxed[at] >> 16, op = relaxed[at] & 0xFFFF;
    if (!wc) break;
    if (op == 71 && wc == 3 && relaxed[at + 2] == 0) decorated.insert(relaxed[at + 1]);
    at += wc;
  }
  for (size_t at = 5; at < in.size();) {
    const uint32_t wc = in[at] >> 16, op = in[at] & 0xFFFF;
    if (!wc) break;
    const char* name = OpName(op, op == 12 && wc >= 5 ? in[at + 4] : 0);
    if (name && wc >= 3) {
      const uint32_t id = in[at + 2];
      std::printf("    %%%-5u %-28s%s%s\n", id, name, op == 12 && wc >= 5 ? (" #" + std::to_string(in[at + 4])).c_str() : "",
                  decorated.count(id) ? "  -> RelaxedPrecision" : "  (full)");
    }
    at += wc;
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: fp16_pass_test <out_dir> [--verbose] <file or folder>...\n");
    return 2;
  }
  const fs::path out_dir = argv[1];
  fs::create_directories(out_dir);
  bool verbose = false;
  std::vector<fs::path> files;
  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--verbose") {
      verbose = true;
      continue;
    }
    if (fs::is_directory(a)) {
      for (const auto& e : fs::directory_iterator(a)) {
        const std::string n = e.path().filename().string();
        if (n.find(".ucode.") != std::string::npos) continue;  // (the guest microcode next to the dumps)
        if (n.ends_with(".spv") || n.ends_with(".bin.frag") || n.ends_with(".bin.vert")) files.push_back(e.path());
      }
    } else {
      files.push_back(a);
    }
  }
  uint64_t total_decorated = 0, total_candidates = 0, total_kept = 0;
  uint32_t fragment = 0, skipped = 0, failed = 0;
  for (const fs::path& path : files) {
    std::vector<uint32_t> words, out;
    if (!ReadWords(path, words)) {
      std::printf("RESULT %s unreadable\n", path.filename().string().c_str());
      ++failed;
      continue;
    }
    Stats stats;
    const bool changed = AddRelaxedPrecision(words.data(), words.size(), out, &stats);
    const bool is_fragment = stats.candidates || changed;
    std::printf("RESULT %s %s decorated=%u candidates=%u kept_full=%u already=%u\n", path.filename().string().c_str(),
                is_fragment ? "fragment" : "skipped", stats.decorated, stats.candidates, stats.kept_full,
                stats.already_relaxed);
    if (is_fragment) {
      ++fragment;
    } else {
      ++skipped;
    }
    total_decorated += stats.decorated;
    total_candidates += stats.candidates;
    total_kept += stats.kept_full;
    if (verbose) ListCandidates(words, changed ? out : words);
    if (changed) {
      std::ofstream f(out_dir / (path.filename().string() + ".relaxed.spv"), std::ios::binary);
      f.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size() * 4));
    }
  }
  std::printf("TOTAL files=%zu fragment=%u skipped=%u unreadable=%u decorated=%llu candidates=%llu kept_full=%llu\n",
              files.size(), fragment, skipped, failed, (unsigned long long)total_decorated,
              (unsigned long long)total_candidates, (unsigned long long)total_kept);
  return failed ? 1 : 0;
}
