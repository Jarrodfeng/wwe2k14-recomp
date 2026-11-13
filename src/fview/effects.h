// Session 101 lane 2 (fview): the effect descriptions (src\renderer\shaders\fx\*.fx.json) as the viewer reads them: constant names per variant and the
// per-variant "alpha_test" flag (default true; false = the build makes no alpha-test twin, tools\renderer\compile_shaders.py). Read from a folder, so
// `--fx-dir` can point at another copy (the planted fault of the selftest).
#pragma once
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace fview {

struct FxConstant {
  std::string field;
  int reg = 0, count = 0;
};

struct FxVariant {
  std::string ident;   // "<effect>_<variant>_<vertex|fragment>": the registry's name for it (rdr::fx::kVariants[i].name)
  std::string effect;  // "yAudience"
  std::string name;    // "lm_pixel_color_alpha"
  bool fragment = false;
  bool alpha_test = true;  // the description's "alpha_test" (fragment variants)
  std::vector<FxConstant> constants;
  std::vector<std::string> defines;
};

class EffectIndex {
 public:
  // Reads every *.fx.json in `dir`. Returns false (and says why) when the folder cannot be read or holds none: a viewer that silently shows
  // no names is worse than one that stops.
  bool Load(const std::filesystem::path& dir, std::string& error);
  const FxVariant* Find(const std::string& ident) const {
    const auto found = by_ident_.find(ident);
    return found == by_ident_.end() ? nullptr : &variants_[found->second];
  }
  const std::filesystem::path& dir() const { return dir_; }
  size_t effect_count() const { return effects_; }
  size_t variant_count() const { return variants_.size(); }

 private:
  std::filesystem::path dir_;
  std::vector<FxVariant> variants_;
  std::unordered_map<std::string, size_t> by_ident_;
  size_t effects_ = 0;
};

}  // namespace fview
