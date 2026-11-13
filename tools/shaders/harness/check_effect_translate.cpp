// Session 100: stand-alone "game microcode -> SPIR-V" for check_effect.py (offline, 0 game runs). Reads a batch file
// {u32 LE type (0 VS, 1 PS), u32 LE dword count, count big-endian guest dwords}* and writes one SPIR-V module per record
// to <out dir>\<index>.spv (+ <index>.map: dynamic flag and float/bool/loop bitmaps) with the SDK translator's default modification (the PC setup, as vi_translate.cpp).
// Inputs/outputs stay under out\s100\ (rule 1). Built by check_effect_build_translate.ps1 against the SDK objects.
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <rex/graphics/pipeline/shader/spirv.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/string/buffer.h>

using namespace rex::graphics;
namespace fs = std::filesystem;

int main(int argc, char** argv) {
  if (argc < 3) return 2;
  std::ifstream in(argv[1], std::ios::binary);
  std::vector<char> buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  fs::path out = argv[2];
  fs::create_directories(out);
  rex::string::StringBuffer disasm;
  SpirvShaderTranslator::Features pc(true);
  SpirvShaderTranslator translator(pc, false, false, false);
  size_t p = 0;
  uint64_t id = 1;
  int idx = 0, failed = 0;
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
    uint32_t regs = shader.GetDynamicAddressableRegisterCount(64);
    uint64_t mod = type ? translator.GetDefaultPixelShaderModification(regs) : translator.GetDefaultVertexShaderModification(regs);
    {  // every interpolator the shader writes (VS) / reads (PS) is kept, so the comparison sees all of them
      SpirvShaderTranslator::Modification m(mod);
      if (type) m.pixel.interpolator_mask = 0xFFFF; else m.vertex.interpolator_mask = 0xFFFF;
      mod = m.value;
    }
    Shader::Translation* t = shader.GetOrCreateTranslation(mod);
    bool ok = translator.TranslateAnalyzedShader(*t);
    if (ok && !t->translated_binary().empty()) {
      std::ofstream o(out / (std::to_string(idx) + ".spv"), std::ios::binary);
      o.write(reinterpret_cast<const char*>(t->translated_binary().data()), std::streamsize(t->translated_binary().size()));
    } else {
      std::printf("FAILED %d\n", idx);
      ++failed;
    }
    {
      const Shader::ConstantRegisterMap& m = shader.constant_register_map();
      std::ofstream o(out / (std::to_string(idx) + ".map"));
      o << "dynamic " << (m.float_dynamic_addressing ? 1 : 0) << "\nfloat";
      for (int i = 0; i < 4; ++i) o << " " << m.float_bitmap[i];
      o << "\nbool";
      for (int i = 0; i < 8; ++i) o << " " << m.bool_bitmap[i];
      o << "\nloop " << m.loop_bitmap << "\n";
    }
    ++idx;
  }
  std::printf("translated %d, failed %d\n", idx - failed, failed);
  return failed ? 1 : 0;
}
