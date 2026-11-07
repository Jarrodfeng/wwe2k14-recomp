// Session 95, Lane C of out\s95\android_structural.md: the attribute translations (kNativeExtVsInput) without a game run. Reads the microcode a --dump_shaders run left on this PC and translates every
// vertex shader with the modifications and native_exts that dump names, plain and as attributes, under two translator setups, and writes the modules to an out\ folder. No game code in here; the
// inputs and outputs stay on this PC (rule 1). Built and run by tools\shaders\vi_translate.ps1, which links it against the DLL's own objects (the DLL exports none of this) and then runs spirv-val.
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <rex/graphics/pipeline/shader/spirv.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/string/buffer.h>

using namespace rex::graphics;
namespace fs = std::filesystem;

namespace {

std::vector<uint32_t> ReadDwords(const fs::path& path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  const size_t size = size_t(file.tellg());
  std::vector<uint32_t> dwords(size / 4);
  file.seekg(0);
  file.read(reinterpret_cast<char*>(dwords.data()), std::streamsize(dwords.size() * 4));
  return dwords;
}

void Write(const fs::path& path, const std::vector<uint8_t>& bytes) {
  std::ofstream file(path, std::ios::binary);
  file.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::printf("vi_translate <dump folder> <out folder> [shader list]\n");
    return 2;
  }
  const fs::path dump = argv[1], out = argv[2];
  fs::create_directories(out);

  // Each shader's variants, from the names of the dumped modules.
  const std::regex module_name(R"(shader_([0-9A-F]{16})_([0-9A-F]{16})(?:_e([0-9A-F]{8}))?\.vk[a-z_]*\.bin\.vert)");
  const std::regex ucode_name(R"(shader_([0-9A-F]{16})\.ucode\.bin\.vert)");
  std::map<uint64_t, std::set<std::pair<uint64_t, uint32_t>>> variants;
  std::map<uint64_t, fs::path> ucode;
  for (const fs::directory_entry& entry : fs::directory_iterator(dump)) {
    const std::string name = entry.path().filename().string();
    std::smatch match;
    if (std::regex_match(name, match, module_name)) {
      variants[std::strtoull(match[1].str().c_str(), nullptr, 16)].emplace(std::strtoull(match[2].str().c_str(), nullptr, 16),
                                                                           match[3].matched ? uint32_t(std::strtoul(match[3].str().c_str(), nullptr, 16)) : 0u);
    } else if (std::regex_match(name, match, ucode_name)) {
      ucode[std::strtoull(match[1].str().c_str(), nullptr, 16)] = entry.path();
    }
  }

  // And from the V lines of a shader list, when one is given (keys only).
  if (argc > 3) {
    const std::regex list_line(R"(V (?:emu|nat) VS ([0-9A-F]{16}) ([0-9A-F]{16}) ([0-9A-F]{8}) .*)");
    std::ifstream list(argv[3]);
    for (std::string line; std::getline(list, line);) {
      std::smatch match;
      if (std::regex_match(line, match, list_line)) {
        variants[std::strtoull(match[1].str().c_str(), nullptr, 16)].emplace(std::strtoull(match[2].str().c_str(), nullptr, 16), uint32_t(std::strtoul(match[3].str().c_str(), nullptr, 16)));
      }
    }
  }

  // The PC's setup, and a phone's: SPIR-V 1.0 and the per-draw storage window.
  SpirvShaderTranslator::Features pc(true);
  SpirvShaderTranslator::Features phone(false);
  phone.shared_memory_window = true;
  struct Setup {
    const char* name;
    SpirvShaderTranslator translator;
  } setups[] = {{"pc", SpirvShaderTranslator(pc, false, false, false)}, {"phone", SpirvShaderTranslator(phone, false, false, false)}};

  rex::string::StringBuffer disasm;
  std::map<std::string, uint32_t> refusals;
  uint32_t shaders = 0, eligible = 0, no_fetch = 0, made = 0, failed = 0;
  for (const auto& [hash, path] : ucode) {
    ++shaders;
    const std::vector<uint32_t> dwords = ReadDwords(path);
    SpirvShader shader(xenos::ShaderType::kVertex, hash, dwords.data(), dwords.size(), std::endian::native);
    shader.AnalyzeUcode(disasm);
    const Shader::VertexInputFormInfo& form = shader.VertexInputForm();
    std::printf("shader %016llX form %s bindings %u vi %08X reads %04X", (unsigned long long)hash, form.eligible ? "yes" : form.refusal, form.binding_count, form.vi, form.read_locations);
    for (uint32_t i = 0; i < form.binding_count; ++i) {
      std::printf(" vf%u:%uw", form.fetch_constant[i], form.stride[i]);
    }
    std::printf("\n");
    if (!form.eligible) {
      ++refusals[form.refusal];
      continue;
    }
    ++eligible;
    no_fetch += form.vi ? 0 : 1;
    std::set<std::pair<uint64_t, uint32_t>> wanted;
    for (const auto& variant : variants[hash]) {
      const SpirvShaderTranslator::Modification modification(variant.first);
      if (modification.vertex.host_vertex_shader_type == Shader::HostVertexShaderType::kVertex && !(variant.second & SpirvShaderTranslator::kNativeExtVsSwap)) {
        wanted.emplace(variant.first, variant.second & ~uint32_t(SpirvShaderTranslator::kNativeExtVsInput));
      }
    }
    if (wanted.empty()) {  // (one of our own rows: the dump has only the replacement's modules)
      wanted.emplace(setups[0].translator.GetDefaultVertexShaderModification(shader.GetDynamicAddressableRegisterCount(8)), 0u);
    }
    // A translation is made once per shader: a fresh shader for each setup.
    for (Setup& setup : setups) {
      SpirvShader fresh(xenos::ShaderType::kVertex, hash, dwords.data(), dwords.size(), std::endian::native);
      fresh.AnalyzeUcode(disasm);
      for (const auto& variant : wanted) {
        for (const uint32_t bit : {0u, uint32_t(SpirvShaderTranslator::kNativeExtVsInput)}) {
          Shader::Translation* translation = fresh.GetOrCreateNativeTranslation(variant.first, variant.second | bit);
          const bool valid = setup.translator.TranslateAnalyzedShader(*translation);
          char name[96];
          std::snprintf(name, sizeof(name), "%016llX_%016llX_%08X_%s_%s.spv", (unsigned long long)hash, (unsigned long long)variant.first, variant.second, setup.name, bit ? "attr" : "plain");
          if (valid && !translation->translated_binary().empty()) {
            Write(out / name, translation->translated_binary());
            ++made;
          } else {
            std::printf("FAILED %s\n", name);
            ++failed;
          }
        }
      }
    }
  }
  std::printf("shaders %u, attribute form %u (no fetch %u), refused %u:", shaders, eligible, no_fetch, shaders - eligible);
  for (const auto& [name, count] : refusals) {
    std::printf(" %s %u", name.c_str(), count);
  }
  std::printf("\nmodules made %u, failed %u\n", made, failed);
  return failed ? 1 : 0;
}
