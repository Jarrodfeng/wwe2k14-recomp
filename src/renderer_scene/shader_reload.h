// Live shader reload for the scene front-end (Session 101, lane 24; docs\renderer\SCENE_FRONTEND.md "Live reload"). DEV ONLY: it exists when the viewer is started
// with `--shader_dir DIR` (or WWE_SHADER_DIR is set); a normal build never creates it, and every call site checks `Get() != nullptr` first.
//
// Two parts:
//  1. A FOLDER OVERRIDE. `DIR\<variant>.spv` (and `<variant>.alpha.spv`, the alpha-test twin of a fragment variant) replaces that registry variant's compiled SPIR-V;
//     `DIR\<variant>.json` (optional) is its layout (constant ranges, attributes, samplers, block size, bools offset: what tools\fview\reload_effect.py
//     --write-layouts writes) and is checked against the compiled table: a variant whose layout differs is refused (the front-end fills constants and attributes
//     from the compiled table, so a different layout would draw garbage; rebuild the registry with tools\renderer\compile_shaders.py instead).
//  2. A WATCHER (a worker thread, so a frame never waits on a compile). It polls the effect sources (`<src>\*.frag|.vert|.fx.json|.glsl` and the folder above, where
//     the compiler also looks for includes) twice a second; for a changed file it finds the effects that use it (by name, `"source"` in the description, and
//     `#include` closure), runs tools\fview\reload_effect.py on each (the renderer's own compiler functions, one effect, variants in parallel) into a staging folder,
//     checks the layouts, and only if the whole effect built moves the files into DIR. A build that fails is said (`[reload] FAILED <variant>: file:line: message`,
//     and last_error()) and changes nothing: the old pipelines stay.
// The render thread calls Poll() once per frame: true = the overrides changed, the front-end must forget its programs and rebuild them (FrontEnd::ReloadPrograms).
#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rdr_scene {

class ShaderReload {
 public:
  struct Options {
    std::filesystem::path dir;      // the override folder (created if missing)
    std::filesystem::path src_dir;  // the effect sources to watch; empty = <root>\src\renderer\shaders\fx
    std::filesystem::path root;     // the repo (tools\fview\reload_effect.py lives there); empty = found from the exe / the working folder
    std::string python = "py";
    bool watch = true;
  };
  // Environment: WWE_SHADER_DIR (required), WWE_SHADER_SRC, WWE_SHADER_ROOT, WWE_SHADER_WATCH=0 (override only). Returns null when WWE_SHADER_DIR is unset.
  static ShaderReload* ConfigureFromEnvironment();
  static ShaderReload* Configure(const Options& options);
  static ShaderReload* Get();  // null = not configured (every normal run)
  ~ShaderReload();
  // Stops the worker and frees the instance (the backend's destructor / fview's exit call it: no thread may outlive the statics it reads).
  static void Shutdown();

  // Render thread, once a frame: adopts the finished work. true = the overrides changed since the last call.
  // `changed` (optional) receives the registry variants whose SPIR-V / twin state changed (the front-end forgets only the programs that use them).
  bool Poll(std::vector<int>* changed = nullptr);
  // The override for a registry variant (alpha: the alpha-test twin), or null = use the compiled one. The pointer stays valid until the next Poll() that returns true.
  const std::vector<uint32_t>* Find(int variant, bool alpha) const;
  // True when the folder says this fragment variant has NO alpha-test twin (its description says "alpha_test": false; the file `<variant>.notwin`): the alpha test is
  // not applied, the plain variant is drawn instead of the compiled twin.
  bool DropsTwin(int variant) const;

  uint64_t generation() const;       // counts every published change
  std::string last_error() const;    // the last failed reload ("" = the last one worked)
  double last_reload_ms() const;     // change seen -> files published, of the last good reload
  int overrides() const;             // variants (either half) currently overridden

  // The same check the loader uses, for tests: "" = the layout json matches the compiled table of the variant, else what differs.
  static std::string CheckLayoutJson(int variant, const std::string& json);
  // The registry variant of a name, -1 = none.
  static int VariantIndex(const std::string& name);

 private:
  explicit ShaderReload(const Options& options);
  struct Entry {
    std::vector<uint32_t> spirv, alpha;
    bool no_twin = false;
  };
  void LoadFolder();                     // the files already in dir
  void WorkerMain();
  std::map<std::string, long long> Snapshot() const;
  std::vector<std::string> EffectsFor(const std::vector<std::string>& changed) const;
  bool BuildEffect(const std::string& effect, std::map<int, Entry>& entries);  // moves the files into dir; the entries are published once for all effects
  void Publish(std::map<int, Entry>&& entries);

  Options opt_;
  mutable std::mutex mutex_;
  std::map<int, Entry> active_;          // render thread
  std::map<int, Entry> pending_;         // worker -> render thread (merged into active_ by Poll)
  bool pending_changed_ = false;
  std::vector<int> pending_variants_;
  uint64_t generation_ = 0;
  std::string last_error_;
  double last_ms_ = 0;
  std::thread worker_;
  bool stop_ = false;
};

}  // namespace rdr_scene
