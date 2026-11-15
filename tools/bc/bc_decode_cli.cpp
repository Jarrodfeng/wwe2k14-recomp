// S102 lane U1: command-line wrapper over src\renderer\bc_decode.h for the offline proof (tools\bc\bc_decode_check.py).
//   bc_decode_cli FORMAT(1..5 = BC1..BC5) WIDTH HEIGHT IN.bin OUT.raw
// Reads the encoded blocks, writes the decoded texels (RGBA8 / R8 / RG8) and prints "short" when the input had too few blocks. Exit 0 ok, 2 short input, 1 usage.
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "../../src/renderer/bc_decode.h"

int main(int argc, char** argv) {
  if (argc != 6) {
    std::fprintf(stderr, "usage: bc_decode_cli FORMAT(1..5) WIDTH HEIGHT IN.bin OUT.raw\n");
    return 1;
  }
  const int f = std::atoi(argv[1]);
  const uint32_t w = uint32_t(std::atoi(argv[2])), h = uint32_t(std::atoi(argv[3]));
  if (f < 1 || f > 5 || !w || !h) return 1;
  const rdr::Format fmt = rdr::Format(int(rdr::Format::BC1) + f - 1);
  std::vector<uint8_t> in;
  if (FILE* fp = std::fopen(argv[4], "rb")) {
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, fp)) > 0) in.insert(in.end(), buf, buf + n);
    std::fclose(fp);
  } else {
    std::fprintf(stderr, "cannot read %s\n", argv[4]);
    return 1;
  }
  std::vector<uint8_t> out(rdr::bc::DecodedBytes(fmt, w, h));
  const bool ok = rdr::bc::Decode(fmt, in.data(), in.size(), w, h, out.data());
  if (FILE* fp = std::fopen(argv[5], "wb")) {
    std::fwrite(out.data(), 1, out.size(), fp);
    std::fclose(fp);
  } else {
    return 1;
  }
  if (!ok) {
    std::printf("short\n");
    return 2;
  }
  return 0;
}
