// Session 11: offline test of the native BPE decoder (src/native_bpe.h) against the corpus the game recorded with
// --wwe2k14_bpe_corpus=true (out/harness/bpe/corpus/*.bpe; layout in src/decode_probe.h). No game needed.
//
// For every entry: decode the recorded input with DecodeReference and with Decode (the one the game hook uses) into
// a buffer of expanded + 64 KiB filled with a canary, and require for each: no error, produced == the guest's
// written length, consumed == the guest's stream length, the bytes equal the guest's output, and the canary after
// the written bytes untouched. Also checks the stored input hash. Prints totals, MB/s per decoder and every failure.
// Exit code 0 = every entry matches.
//
// Build: tools/bpe/build_test.ps1 (clang++ -O2). Run: out\harness\bpe\bpe_test.exe [DIR_OR_FILE ...] [--reps N]
// (default DIR = out\harness\bpe\corpus).

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../../src/native_bpe.h"

namespace fs = std::filesystem;
using namespace wwe2k14::native_bpe;

struct Header {
  char magic[4];
  uint32_t version, src, dst, expanded, produced, consumed, in_len, out_len, reserved;
  uint64_t in_hash, out_hash;
};
static_assert(sizeof(Header) == 56, "corpus header layout");

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

using DecodeFn = Result (*)(const uint8_t*, uint8_t*, uint32_t, uint32_t);

struct Stats {
  const char* name;
  DecodeFn fn;
  uint64_t ok = 0, fail = 0;
  double seconds = 0;
};

// --fuzz N: for N mutations per corpus entry (random stream bytes changed, or the stream cut short), decode with both
// decoders under the game's bound (out_cap = the declared size) and a stream bound, into a buffer with a canary
// past the bound. Requires: the two agree on success vs error (the game hook falls back on any error, so the fast
// decoder must fail exactly when the reference does), identical results when both succeed, and the canary intact.
static bool Fuzz(const std::vector<fs::path>& files, int n) {
  uint64_t state = 0x9E3779B97F4A7C15ull;
  auto rnd = [&state]() {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
  };
  uint64_t cases = 0, both_ok = 0, both_err = 0, disagree = 0, overwrite = 0;
  std::vector<uint8_t> data, a, b;
  const uint8_t kCanary = 0xCD;
  for (const auto& path : files) {
    if (!ReadFile(path, data) || data.size() < sizeof(Header)) continue;
    Header h;
    std::memcpy(&h, data.data(), sizeof(h));
    if (h.in_len <= 16 || ExpandedSize(data.data() + sizeof(Header)) == 0) continue;
    for (int k = 0; k < n; ++k) {
      std::vector<uint8_t> in(data.begin() + sizeof(Header), data.begin() + sizeof(Header) + h.in_len);
      uint32_t in_cap = h.in_len - 16;
      if (k % 4 == 3) {
        in_cap = uint32_t(rnd() % in_cap);  // stream cut short
      } else {
        const int flips = 1 + int(rnd() % 4);
        for (int f = 0; f < flips; ++f) in[16 + rnd() % (h.in_len - 16)] = uint8_t(rnd());
      }
      const uint32_t cap = ExpandedSize(in.data());
      a.assign(size_t(cap) + 64, kCanary);
      b.assign(size_t(cap) + 64, kCanary);
      const Result ra = DecodeReference(in.data(), a.data(), cap, in_cap);
      const Result rb = Decode(in.data(), b.data(), cap, in_cap);
      ++cases;
      for (size_t i = cap; i < a.size(); ++i) {
        if (a[i] != kCanary || b[i] != kCanary) {
          ++overwrite;
          std::printf("FUZZ OVERWRITE %s k=%d\n", path.filename().string().c_str(), k);
          break;
        }
      }
      if ((ra.error == 0) != (rb.error == 0)) {
        ++disagree;
        if (disagree <= 10)
          std::printf("FUZZ DISAGREE %s k=%d ref_error=%d fast_error=%d\n", path.filename().string().c_str(), k,
                      ra.error, rb.error);
      } else if (ra.error == 0) {
        if (ra.produced != rb.produced || ra.consumed != rb.consumed ||
            std::memcmp(a.data(), b.data(), ra.produced) != 0) {
          ++disagree;
          if (disagree <= 10)
            std::printf("FUZZ DIFFERENT %s k=%d\n", path.filename().string().c_str(), k);
        } else {
          ++both_ok;
        }
      } else {
        ++both_err;
      }
    }
  }
  std::printf("fuzz cases=%llu both_ok=%llu both_error=%llu disagree=%llu overwrite=%llu\n",
              (unsigned long long)cases, (unsigned long long)both_ok, (unsigned long long)both_err,
              (unsigned long long)disagree, (unsigned long long)overwrite);
  return disagree == 0 && overwrite == 0;
}

int main(int argc, char** argv) {
  std::vector<fs::path> inputs;
  int reps = 1;
  int fuzz = 0;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--reps" && i + 1 < argc) {
      reps = std::max(1, std::atoi(argv[++i]));
    } else if (a == "--fuzz" && i + 1 < argc) {
      fuzz = std::max(0, std::atoi(argv[++i]));
    } else {
      inputs.emplace_back(a);
    }
  }
  if (inputs.empty()) inputs.emplace_back("out/harness/bpe/corpus");
  std::vector<fs::path> files;
  for (const auto& in : inputs) {
    if (fs::is_directory(in)) {
      for (const auto& e : fs::directory_iterator(in))
        if (e.path().extension() == ".bpe") files.push_back(e.path());
    } else {
      files.push_back(in);
    }
  }
  std::sort(files.begin(), files.end());

  Stats stats[2] = {{"reference", &DecodeReference}, {"decode", &Decode}};
  uint64_t entries = 0, bad_files = 0, bytes_out = 0, bytes_in = 0, max_out = 0;
  std::vector<uint8_t> data, buf;
  const uint8_t kCanary = 0xCD;
  for (const auto& path : files) {
    if (!ReadFile(path, data) || data.size() < sizeof(Header)) {
      std::printf("BADFILE %s (unreadable or short)\n", path.string().c_str());
      ++bad_files;
      continue;
    }
    Header h;
    std::memcpy(&h, data.data(), sizeof(h));
    if (std::memcmp(h.magic, "BPEC", 4) != 0 || h.version != 1 || h.in_len < 16 ||
        data.size() != sizeof(Header) + uint64_t(h.in_len) + h.out_len) {
      std::printf("BADFILE %s (header)\n", path.string().c_str());
      ++bad_files;
      continue;
    }
    const uint8_t* in = data.data() + sizeof(Header);
    const uint8_t* want = in + h.in_len;
    if (Fnv1a64(in, h.in_len) != h.in_hash || Fnv1a64(want, h.out_len) != h.out_hash) {
      std::printf("BADFILE %s (hash)\n", path.string().c_str());
      ++bad_files;
      continue;
    }
    ++entries;
    bytes_out += h.out_len;
    bytes_in += h.in_len;
    if (h.out_len > max_out) max_out = h.out_len;
    const uint32_t cap = ExpandedSize(in) + 65536u;
    const uint32_t in_cap = h.in_len - 16;
    for (Stats& s : stats) {
      bool ok = true;
      Result r;
      for (int rep = 0; rep < reps; ++rep) {
        buf.assign(cap, kCanary);
        const auto t0 = std::chrono::steady_clock::now();
        r = s.fn(in, buf.data(), cap, in_cap);
        const auto t1 = std::chrono::steady_clock::now();
        s.seconds += std::chrono::duration<double>(t1 - t0).count();
      }
      // A size-0 block: the guest returns before reading the stream (consumed is reported as 0 for it).
      const uint32_t want_consumed = ExpandedSize(in) ? h.consumed : 0;
      if (r.error || r.produced != h.produced || r.consumed != want_consumed ||
          std::memcmp(buf.data(), want, h.out_len) != 0) {
        ok = false;
        uint32_t first = 0;
        const uint32_t n = std::min(r.produced, h.out_len);
        while (first < n && buf[first] == want[first]) ++first;
        std::printf("MISMATCH %s %s: error=%d produced=%u/%u consumed=%u/%u first_diff=%u\n", s.name,
                    path.filename().string().c_str(), r.error, r.produced, h.produced, r.consumed, want_consumed,
                    first);
      } else {
        for (uint32_t i = r.produced; i < cap; ++i) {
          if (buf[i] != kCanary) {
            ok = false;
            std::printf("OVERWRITE %s %s: byte %u past the written length %u changed\n", s.name,
                        path.filename().string().c_str(), i, r.produced);
            break;
          }
        }
      }
      (ok ? s.ok : s.fail)++;
    }
  }
  std::printf("entries=%llu bad_files=%llu out_MB=%.1f in_MB=%.1f max_out=%llu reps=%d\n",
              (unsigned long long)entries, (unsigned long long)bad_files, bytes_out / 1e6, bytes_in / 1e6,
              (unsigned long long)max_out, reps);
  bool all = bad_files == 0 && entries > 0;
  for (const Stats& s : stats) {
    std::printf("%-10s ok=%llu fail=%llu time_ms=%.1f MB_per_s=%.0f\n", s.name, (unsigned long long)s.ok,
                (unsigned long long)s.fail, s.seconds * 1000.0,
                s.seconds > 0 ? (bytes_out * double(reps)) / 1e6 / s.seconds : 0.0);
    if (s.fail) all = false;
  }
  if (fuzz > 0 && !Fuzz(files, fuzz)) all = false;
  std::printf(all ? "RESULT: PASS (every entry matches)\n" : "RESULT: FAIL\n");
  return all ? 0 : 1;
}
