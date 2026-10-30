// Session 73 (a phone never kept its shader storage): where does a read start after fopen(path, "a+b")? The SDK's
// shader / pipeline storage opened its files that way and read the header at once. Built with the NDK (bionic's stdio)
// and run under QEMU by run_aplus_test.ps1; glibc and Windows start reading at 0.
#include <cstdio>
#include <cstring>

int main() {
  const char* path = "/t/aplus_probe.bin";
  FILE* w = std::fopen(path, "wb");
  std::fwrite("HEADER", 1, 6, w);
  std::fclose(w);
  FILE* f = std::fopen(path, "a+b");
  char buf[8] = {};
  const size_t n = std::fread(buf, 1, 6, f);
  std::printf("fopen a+b then fread: %zu bytes \"%s\" -> %s\n", n, buf,
              n == 6 && !std::memcmp(buf, "HEADER", 6) ? "reads from the start" : "reads from the END (header lost)");
  std::fseek(f, 0, SEEK_SET);
  const size_t m = std::fread(buf, 1, 6, f);
  std::printf("after fseek(0, SEEK_SET): %zu bytes \"%s\"\n", m, buf);
  std::fclose(f);
  return 0;
}
