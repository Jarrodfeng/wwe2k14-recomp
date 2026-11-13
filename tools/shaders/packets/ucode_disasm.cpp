// Session 100: stand-alone Xenos microcode disassembler for the shader packets (tools\shaders\packets). Reads a batch file
// {u32 LE type (0 VS, 1 PS), u32 LE dword count, count big-endian guest dwords}* and writes {u32 LE text length, text}*,
// the SDK's own D3D-style listing. Output stays under out\s100\ (rule 1). Built by build_disasm.ps1 against the SDK objects.
#include <bit>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <rex/graphics/pipeline/shader/spirv.h>
#include <rex/string/buffer.h>

using namespace rex::graphics;

int main(int argc, char** argv) {
  if (argc < 3) return 2;
  std::ifstream in(argv[1], std::ios::binary);
  std::vector<char> buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::ofstream out(argv[2], std::ios::binary);
  rex::string::StringBuffer disasm;
  size_t p = 0;
  uint64_t id = 1;
  while (p + 8 <= buf.size()) {
    uint32_t type, n;
    std::memcpy(&type, &buf[p], 4);
    std::memcpy(&n, &buf[p + 4], 4);
    p += 8;
    std::vector<uint32_t> dw(n);
    std::memcpy(dw.data(), &buf[p], n * 4);
    p += size_t(n) * 4;
    SpirvShader shader(type ? xenos::ShaderType::kPixel : xenos::ShaderType::kVertex, id++, dw.data(), dw.size(), std::endian::big);
    shader.AnalyzeUcode(disasm);
    const std::string& text = shader.ucode_disassembly();
    uint32_t len = uint32_t(text.size());
    out.write(reinterpret_cast<const char*>(&len), 4);
    out.write(text.data(), len);
  }
  return 0;
}
