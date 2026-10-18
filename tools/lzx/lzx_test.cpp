// Session 12: offline test of the native LZX block decoder (src/native_lzx.h) against the corpus the game recorded
// with --wwe2k14_lzx_corpus=true (out/harness/lzx/corpus/*.lzx; layout in src/lzx_probe.h). No game needed.
//
// For every entry: decode the recorded block (input + 16 zero bytes of padding) into a buffer of total + 64 bytes
// filled with a canary, and require: no error, the slice [offset, offset + written) equals the game's output,
// written == min(wanted, total - offset) (what a full decode gives the chunk loop), the canary past `total` intact
// apart from the decoder's documented 7-byte copy slack, and the stored input hash. Prints totals, the error kinds,
// MB/s (output bytes of whole blocks) and every failure. Exit code 0 = every entry matches.
//
// Build: tools/lzx/build_test.ps1 (clang++ -O2). Run: out\harness\lzx\lzx_test.exe [DIR_OR_FILE ...] [--reps N]
// (default DIR = out\harness\lzx\corpus).

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../../src/native_lzx.h"

namespace fs = std::filesystem;
using namespace wwe2k14::native_lzx;

struct Header {
  char magic[4];
  uint32_t version, window, total, src_len, offset, wanted, written, in_len, out_len, frames, reserved;
  uint64_t in_hash, out_hash;
};
static_assert(sizeof(Header) == 64, "lzx corpus header layout");

static uint64_t Fnv1a64(const uint8_t* p, size_t n) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 1099511628211ull;
  }
  return h;
}

static bool ReadFile(const fs::path& p, std::vector<uint8_t>& data) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  data.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return true;
}

static const char* ErrorName(int e) {
  switch (e) {
    case kOk: return "ok";
    case kBadWindow: return "bad_window";
    case kStoredShort: return "stored_short";
    case kChunkMissing: return "chunk_missing";
    case kChunkOverrun: return "chunk_overrun";
    case kE8: return "e8";
    case kBlockType: return "block_type";
    case kTree: return "tree";
    case kSymbol: return "symbol";
    case kOffset: return "offset";
    case kOverrun: return "overrun";
    case kUncompressedShort: return "uncompressed_short";
    case kHeaderInput: return "header_input";
  }
  return "?";
}

int main(int argc, char** argv) {
  std::vector<fs::path> inputs;
  int reps = 1, fuzz = 0;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--reps" && i + 1 < argc) {
      reps = std::atoi(argv[++i]);
    } else if (a == "--fuzz" && i + 1 < argc) {
      fuzz = std::atoi(argv[++i]);
    } else {
      inputs.emplace_back(a);
    }
  }
  if (inputs.empty()) {
    const fs::path exe = fs::absolute(argv[0]).parent_path();
    inputs.push_back(exe / "corpus");
  }
  std::vector<fs::path> files;
  for (const auto& in : inputs) {
    if (fs::is_directory(in)) {
      for (const auto& e : fs::directory_iterator(in)) {
        if (e.path().extension() == ".lzx") files.push_back(e.path());
      }
    } else {
      files.push_back(in);
    }
  }
  std::sort(files.begin(), files.end());

  auto st = std::make_unique<State>();
  uint64_t ok = 0, fail = 0, stored = 0, out_bytes = 0, in_bytes = 0, frames = 0, blocks = 0;
  std::map<int, uint64_t> errors;
  std::map<uint32_t, uint64_t> windows;
  double seconds = 0;
  uint64_t fuzz_state = 0x2545F4914F6CDD1Dull, fuzz_runs = 0, fuzz_errors = 0, fuzz_overwrite = 0, full_fail = 0;
  std::vector<uint8_t> data, in, out;
  int printed = 0;
  for (const auto& path : files) {
    if (!ReadFile(path, data) || data.size() < sizeof(Header)) {
      std::printf("FAIL %s: unreadable\n", path.filename().string().c_str());
      ++fail;
      continue;
    }
    Header h;
    std::memcpy(&h, data.data(), sizeof(h));
    if (std::memcmp(h.magic, "LZXC", 4) != 0 || data.size() != sizeof(Header) + h.in_len + h.out_len) {
      std::printf("FAIL %s: bad header\n", path.filename().string().c_str());
      ++fail;
      continue;
    }
    const uint8_t* rin = data.data() + sizeof(Header);
    const uint8_t* rout = rin + h.in_len;
    if ((Fnv1a64(rin, h.in_len) ^ (uint64_t(h.total) << 32) ^ h.window) != h.in_hash) {
      std::printf("FAIL %s: input hash\n", path.filename().string().c_str());
      ++fail;
      continue;
    }
    ++windows[h.window];
    in.assign(rin, rin + h.in_len);
    in.resize(h.in_len + 16, 0);
    out.assign(size_t(h.total) + 64, 0xA5);
    Result r;
    const auto t0 = std::chrono::steady_clock::now();
    const uint32_t need = h.offset + h.written;  // what the hook asks for (src/lzx_probe.h NativeBlock)
    for (int k = 0; k < reps; ++k) r = DecodeBlock(in.data(), h.in_len, h.total, h.window, out.data(), *st, need);
    seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (h.in_len >= 2 && rin[0] == 0 && rin[1] == 0) ++stored;
    ++errors[r.error];
    frames += r.frames;
    blocks += r.blocks;
    out_bytes += uint64_t(r.produced) * uint64_t(reps);
    in_bytes += h.in_len;
    const uint32_t expect = h.offset < h.total ? std::min(h.total - h.offset, h.wanted) : 0;
    bool good = r.error == kOk && r.produced >= need && r.produced <= h.total && h.written == expect &&
                (h.written == 0 || std::memcmp(out.data() + h.offset, rout, h.written) == 0);
    uint32_t first_diff = ~0u;
    if (r.error == kOk && !good) {
      for (uint32_t i = 0; i < h.written; ++i) {
        if (out[size_t(h.offset) + i] != rout[i]) {
          first_diff = h.offset + i;
          break;
        }
      }
    }
    for (size_t i = size_t(h.total) + 8; i < out.size(); ++i) {
      if (out[i] != 0xA5) {
        good = false;
        break;
      }
    }
    // Whole-block pass (no limit): every frame of the block must decode too, and the slice must not change.
    {
      std::vector<uint8_t> full(size_t(h.total) + 64, 0xA5);
      const Result fr = DecodeBlock(in.data(), h.in_len, h.total, h.window, full.data(), *st);
      if (fr.error || fr.produced != h.total || (h.written && std::memcmp(full.data() + h.offset, rout, h.written) != 0)) ++full_fail;
    }
    // --fuzz N: N mutated copies (1-8 random bytes changed, or the input cut short); the decoder may fail any way it
    // likes but must not crash or write past total + 7.
    for (int k = 0; k < fuzz; ++k) {
      std::vector<uint8_t> m(rin, rin + h.in_len);
      uint32_t len = h.in_len;
      fuzz_state = fuzz_state * 6364136223846793005ull + 1442695040888963407ull;
      if ((fuzz_state >> 60) < 3 && len > 4) {
        len = uint32_t((fuzz_state >> 20) % len);
      } else if (len) {
        const int n = 1 + int((fuzz_state >> 40) & 7);
        for (int j = 0; j < n; ++j) {
          fuzz_state = fuzz_state * 6364136223846793005ull + 1442695040888963407ull;
          m[(fuzz_state >> 16) % len] ^= uint8_t(1 + ((fuzz_state >> 8) & 0xFE));
        }
      }
      m.resize(size_t(len) + 16, 0);
      std::vector<uint8_t> fo(size_t(h.total) + 64, 0xA5);
      const Result fr = DecodeBlock(m.data(), len, h.total, h.window, fo.data(), *st);
      ++fuzz_runs;
      if (fr.error) ++fuzz_errors;
      for (size_t i = size_t(h.total) + 8; i < fo.size(); ++i) {
        if (fo[i] != 0xA5) {
          ++fuzz_overwrite;
          break;
        }
      }
    }
    if (good) {
      ++ok;
    } else {
      ++fail;
      if (printed++ < 40) {
        std::printf("FAIL %s: error=%s produced=%u total=%u offset=%u wanted=%u written=%u window=%u frames=%u "
                    "blocks=%u first_diff=%d\n",
                    path.filename().string().c_str(), ErrorName(r.error), r.produced, h.total, h.offset, h.wanted,
                    h.written, h.window, r.frames, r.blocks, first_diff == ~0u ? -1 : int(first_diff));
      }
    }
  }
  std::printf("entries=%zu ok=%llu fail=%llu stored=%llu frames=%llu lzx_blocks=%llu in=%llu MB out(per rep)=%llu MB\n",
              files.size(), (unsigned long long)ok, (unsigned long long)fail, (unsigned long long)stored,
              (unsigned long long)frames, (unsigned long long)blocks, (unsigned long long)(in_bytes >> 20),
              (unsigned long long)((out_bytes / std::max(reps, 1)) >> 20));
  for (const auto& [e, n] : errors) std::printf("  result %s: %llu\n", ErrorName(e), (unsigned long long)n);
  for (const auto& [w, n] : windows) std::printf("  window %u: %llu\n", w, (unsigned long long)n);
  std::printf("native: %.1f MB/s (%d reps, %.3f s)\n", seconds > 0 ? double(out_bytes) / 1e6 / seconds : 0.0, reps,
              seconds);
  std::printf("whole-block pass: %llu failures\n", (unsigned long long)full_fail);
  if (fuzz) {
    std::printf("fuzz: runs=%llu decode_errors=%llu writes_past_bound=%llu\n", (unsigned long long)fuzz_runs,
                (unsigned long long)fuzz_errors, (unsigned long long)fuzz_overwrite);
  }
  return fail == 0 && full_fail == 0 && fuzz_overwrite == 0 ? 0 : 1;
}
