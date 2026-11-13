#include "effects.h"

#include <fstream>
#include <sstream>

#include "json_lite.h"

namespace fview {

bool EffectIndex::Load(const std::filesystem::path& dir, std::string& error) {
  dir_ = dir;
  variants_.clear();
  by_ident_.clear();
  effects_ = 0;
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec)) {
    error = "effect folder not found: " + dir.string();
    return false;
  }
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    const std::string name = entry.path().filename().string();
    if (name.size() < 9 || name.compare(name.size() - 8, 8, ".fx.json") != 0) continue;
    std::ifstream file(entry.path(), std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    JsonValue root;
    if (!ParseJson(text.str(), root) || root.type != JsonValue::Object) {
      error = "cannot parse " + entry.path().string();
      return false;
    }
    const std::string effect = root.Str("effect");
    if (effect.empty()) {
      error = "no \"effect\" in " + entry.path().string();
      return false;
    }
    ++effects_;
    for (const char* stage : {"vertex", "fragment"}) {
      const JsonValue* list = root.Get(stage);
      if (!list || list->type != JsonValue::Array) continue;
      for (const JsonValue& v : list->array) {
        FxVariant out;
        out.effect = effect;
        out.name = v.Str("name");
        out.fragment = stage[0] == 'f';
        out.ident = effect + "_" + out.name + "_" + stage;
        out.alpha_test = v.Flag("alpha_test", true);
        if (const JsonValue* defines = v.Get("defines"))
          for (const JsonValue& d : defines->array) out.defines.push_back(d.string);
        if (const JsonValue* constants = v.Get("constants")) {
          for (const JsonValue& c : constants->array) {
            FxConstant constant;
            constant.field = c.Str("field");
            constant.reg = int(c.Num("register"));
            constant.count = int(c.Num("count", 1));
            out.constants.push_back(constant);
          }
        }
        by_ident_[out.ident] = variants_.size();
        variants_.push_back(std::move(out));
      }
    }
  }
  if (variants_.empty()) {
    error = "no *.fx.json files in " + dir.string();
    return false;
  }
  return true;
}

}  // namespace fview
