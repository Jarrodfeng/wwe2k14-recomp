// Session 34: unpacks a whole XCTD-compressed pac (first word 0x0FF512ED, docs/systems/file_loading.md) from the
// player's own game files into a plain file (src/xctd_file.h, which uses src/native_lzx.h). Read-only on the input;
// the output goes where the caller says (keep it under out\, never in the repo: it is game data). A file that is not
// XCTD is copied as is.
//
// Build: tools/wm30/build.ps1. Use: xctd_unpack.exe IN.pac OUT.bin   (exit 0 = whole file decoded)

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "../../src/xctd_file.h"

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: xctd_unpack IN.pac OUT.bin\n");
    return 2;
  }
  std::ifstream f(argv[1], std::ios::binary);
  if (!f) {
    std::fprintf(stderr, "cannot open %s\n", argv[1]);
    return 2;
  }
  const std::vector<uint8_t> in((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  std::vector<uint8_t> out;
  std::string error;
  if (!wwe2k14::xctd::Unpack(in, out, error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  std::FILE* o = std::fopen(argv[2], "wb");
  if (!o) {
    std::fprintf(stderr, "cannot write %s\n", argv[2]);
    return 2;
  }
  std::fwrite(out.data(), 1, out.size(), o);
  std::fclose(o);
  std::printf("%s %zu bytes\n", wwe2k14::xctd::IsXctd(in) ? "xctd" : "plain", out.size());
  return 0;
}
