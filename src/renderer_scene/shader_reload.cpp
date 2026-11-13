// Live shader reload (Session 101, lane 24). See shader_reload.h and docs\renderer\SCENE_FRONTEND.md "Live reload".
#include "shader_reload.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "../renderer/shaders/spv/fx_registry.h"

namespace rdr_scene {
namespace fs = std::filesystem;
namespace {

using Clock = std::chrono::steady_clock;

std::unique_ptr<ShaderReload> g_instance;

std::string ReadText(const fs::path& path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream s;
  s << f.rdbuf();
  return s.str();
}

bool ReadWords(const fs::path& path, std::vector<uint32_t>& words) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::stringstream s;
  s << f.rdbuf();
  const std::string bytes = s.str();
  if (bytes.size() < 20 || bytes.size() % 4) return false;
  words.resize(bytes.size() / 4);
  std::memcpy(words.data(), bytes.data(), bytes.size());
  if (words[0] != 0x07230203u) {  // the SPIR-V magic: anything else is not a shader (a truncated or foreign file)
    words.clear();
    return false;
  }
  return true;
}

// Runs a command line without a window, its stdout + stderr into `out`. Returns the exit code (-1: could not start).
int RunCommand(const std::string& command, std::string& out) {
  out.clear();
#ifdef _WIN32
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE read_end = nullptr, write_end = nullptr;
  if (!CreatePipe(&read_end, &write_end, &sa, 0)) return -1;
  SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
  HANDLE nul = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
  STARTUPINFOA si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = nul;
  si.hStdOutput = write_end;
  si.hStdError = write_end;
  PROCESS_INFORMATION pi{};
  std::string line = command;
  const BOOL started = CreateProcessA(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  CloseHandle(write_end);
  if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
  if (!started) {
    CloseHandle(read_end);
    return -1;
  }
  // Read what the compiler prints while it runs (a full pipe would stall it); a child still running after 120 s is killed.
  const auto deadline = Clock::now() + std::chrono::seconds(120);
  char buffer[4096];
  bool killed = false;
  for (;;) {
    DWORD avail = 0;
    if (!PeekNamedPipe(read_end, nullptr, 0, nullptr, &avail, nullptr)) break;  // the write end is closed: the child (and its children) are done
    if (avail) {
      DWORD got = 0;
      if (ReadFile(read_end, buffer, sizeof buffer, &got, nullptr) && got) out.append(buffer, got);
      continue;
    }
    if (WaitForSingleObject(pi.hProcess, 20) == WAIT_OBJECT_0) {
      for (DWORD got = 0, left = 1; left;) {  // the rest the child wrote before it exited
        if (!PeekNamedPipe(read_end, nullptr, 0, nullptr, &left, nullptr) || !left) break;
        if (!ReadFile(read_end, buffer, sizeof buffer, &got, nullptr) || !got) break;
        out.append(buffer, got);
      }
      break;
    }
    if (Clock::now() > deadline) {
      TerminateProcess(pi.hProcess, 1);
      killed = true;
      break;
    }
  }
  CloseHandle(read_end);
  if (killed) {
    WaitForSingleObject(pi.hProcess, 5000);
    out += "\n[reload] the compiler was killed after 120 s";
  }
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  return int(code);
#else
  FILE* pipe = popen((command + " 2>&1").c_str(), "r");
  if (!pipe) return -1;
  char buffer[4096];
  size_t got;
  while ((got = fread(buffer, 1, sizeof buffer, pipe)) > 0) out.append(buffer, got);
  return pclose(pipe);
#endif
}

fs::path FindRoot() {
  const auto check = [](fs::path p) -> fs::path {
    for (int i = 0; i < 8 && !p.empty(); ++i, p = p.parent_path()) {
      std::error_code ec;
      if (fs::exists(p / "tools" / "fview" / "reload_effect.py", ec)) return p;
      if (p == p.root_path()) break;
    }
    return {};
  };
#ifdef _WIN32
  char exe[MAX_PATH] = {};
  if (GetModuleFileNameA(nullptr, exe, MAX_PATH)) {
    const fs::path r = check(fs::path(exe).parent_path());
    if (!r.empty()) return r;
  }
#endif
  std::error_code ec;
  return check(fs::current_path(ec));
}

// ---- the layout json: {"ranges":[[reg,count,offset],..],"attributes":[[fetch,location,integer],..],"samplers":[[slot,kind],..],"block_bytes":N,"bools":N} ----

bool KeyValue(const std::string& json, const char* key, size_t& pos) {
  const size_t k = json.find(std::string("\"") + key + "\"");
  if (k == std::string::npos) return false;
  pos = json.find(':', k);
  if (pos == std::string::npos) return false;
  ++pos;
  return true;
}

bool Number(const std::string& json, const char* key, int& value) {
  size_t pos;
  if (!KeyValue(json, key, pos)) return false;
  value = std::atoi(json.c_str() + pos);
  return true;
}

bool Rows(const std::string& json, const char* key, std::vector<std::vector<int>>& rows) {
  size_t pos;
  rows.clear();
  if (!KeyValue(json, key, pos)) return false;
  pos = json.find('[', pos);
  if (pos == std::string::npos) return false;
  int depth = 0;
  std::vector<int> row;
  for (size_t i = pos; i < json.size(); ++i) {
    const char c = json[i];
    if (c == '[') {
      if (++depth == 2) row.clear();
    } else if (c == ']') {
      if (depth == 2) rows.push_back(row);
      if (--depth == 0) return true;
    } else if ((c >= '0' && c <= '9') || c == '-') {
      if (depth == 2) row.push_back(std::atoi(json.c_str() + i));
      while (i + 1 < json.size() && json[i + 1] >= '0' && json[i + 1] <= '9') ++i;
    }
  }
  return false;
}

}  // namespace

int ShaderReload::VariantIndex(const std::string& name) {
  for (size_t i = 0; i < std::size(rdr::fx::kVariants); ++i)
    if (rdr::fx::kVariants[i].name && name == rdr::fx::kVariants[i].name) return int(i);
  return -1;
}

std::string ShaderReload::CheckLayoutJson(int variant, const std::string& json) {
  if (variant < 0 || variant >= int(std::size(rdr::fx::kVariants))) return "not in the compiled table";
  const rdr::fx::Variant& v = rdr::fx::kVariants[variant];
  std::string why;
  const auto add = [&](const std::string& s) { why += (why.empty() ? "" : "; ") + s; };
  std::vector<std::vector<int>> ranges, attributes, samplers;
  int block_bytes = -1, bools = 0xFFFF;
  if (!Rows(json, "ranges", ranges) || !Rows(json, "attributes", attributes) || !Rows(json, "samplers", samplers) || !Number(json, "block_bytes", block_bytes))
    return "the layout json cannot be read";
  Number(json, "bools", bools);
  const auto same = [&](const std::vector<int>& row, std::initializer_list<int> want) {
    if (row.size() < want.size()) return false;
    size_t i = 0;
    for (int w : want)
      if (row[i++] != w) return false;
    return true;
  };
  if (int(v.constant_count) != int(ranges.size())) add("constant ranges " + std::to_string(v.constant_count) + " compiled vs " + std::to_string(ranges.size()) + " rebuilt");
  else
    for (size_t i = 0; i < ranges.size(); ++i)
      if (!same(ranges[i], {v.constants[i].reg, v.constants[i].count, v.constants[i].offset})) {
        add("constant range " + std::to_string(i) + " (register/count/offset) differs");
        break;
      }
  if (int(v.block_bytes) != block_bytes) add("constant block " + std::to_string(v.block_bytes) + " vs " + std::to_string(block_bytes) + " bytes");
  if (int(v.bools_offset) != bools) add("bools offset differs");
  if (int(v.attribute_count) != int(attributes.size())) add("attribute count differs");
  else
    for (size_t i = 0; i < attributes.size(); ++i)
      if (!same(attributes[i], {v.attributes[i].fetch, v.attributes[i].location, v.attributes[i].integer})) {
        add("attribute " + std::to_string(i) + " differs");
        break;
      }
  if (int(v.sampler_count) != int(samplers.size())) add("sampler count differs");
  else
    for (size_t i = 0; i < samplers.size(); ++i)
      if (!same(samplers[i], {v.samplers[i].slot, v.samplers[i].kind})) {
        add("sampler " + std::to_string(i) + " differs");
        break;
      }
  return why;
}

// ---- configuration ---------------------------------------------------------------------------------------------------------------

ShaderReload* ShaderReload::Get() { return g_instance.get(); }

ShaderReload* ShaderReload::Configure(const Options& options) {
  if (!g_instance) g_instance.reset(new ShaderReload(options));
  return g_instance.get();
}

ShaderReload* ShaderReload::ConfigureFromEnvironment() {
  if (g_instance) return g_instance.get();
  const char* dir = std::getenv("WWE_SHADER_DIR");
  if (!dir || !*dir) return nullptr;
  Options o;
  o.dir = dir;
  if (const char* s = std::getenv("WWE_SHADER_SRC")) o.src_dir = s;
  if (const char* s = std::getenv("WWE_SHADER_ROOT")) o.root = s;
  if (const char* s = std::getenv("WWE_SHADER_PYTHON")) o.python = s;
  if (const char* s = std::getenv("WWE_SHADER_WATCH")) o.watch = std::atoi(s) != 0;
  return Configure(o);
}

ShaderReload::ShaderReload(const Options& options) : opt_(options) {
  std::error_code ec;
  if (opt_.root.empty()) opt_.root = FindRoot();
  if (opt_.src_dir.empty() && !opt_.root.empty()) opt_.src_dir = opt_.root / "src" / "renderer" / "shaders" / "fx";
  fs::create_directories(opt_.dir, ec);
  opt_.dir = fs::absolute(opt_.dir, ec);
  if (!opt_.src_dir.empty()) opt_.src_dir = fs::absolute(opt_.src_dir, ec);
  LoadFolder();
  std::fprintf(stderr, "[reload] shader folder %s: %d variant(s) overridden at start; watching %s\n", opt_.dir.string().c_str(), overrides(),
               opt_.watch && !opt_.src_dir.empty() ? opt_.src_dir.string().c_str() : "(nothing)");
  if (opt_.watch) {
    if (opt_.root.empty() || opt_.src_dir.empty() || !fs::exists(opt_.root / "tools" / "fview" / "reload_effect.py", ec))
      std::fprintf(stderr, "[reload] the watcher is off: tools\\fview\\reload_effect.py not found from the exe or the working folder (WWE_SHADER_ROOT names the repo)\n");
    else worker_ = std::thread([this] { WorkerMain(); });
  }
}

ShaderReload::~ShaderReload() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  if (worker_.joinable()) worker_.join();
}

void ShaderReload::LoadFolder() {
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(opt_.dir, ec)) {
    if (entry.is_regular_file() && entry.path().extension() == ".notwin") {  // S101 lane 24: "this variant has no alpha-test twin"
      const int nv = VariantIndex(entry.path().stem().string());
      if (nv >= 0) active_[nv].no_twin = true;
      continue;
    }
    if (!entry.is_regular_file() || entry.path().extension() != ".spv") continue;
    std::string name = entry.path().stem().string();
    bool alpha = false;
    if (name.size() > 6 && name.compare(name.size() - 6, 6, ".alpha") == 0) name.resize(name.size() - 6), alpha = true;
    const int v = VariantIndex(name);
    if (v < 0) {
      std::fprintf(stderr, "[reload] %s: not a variant of the compiled registry, ignored\n", entry.path().filename().string().c_str());
      continue;
    }
    const fs::path layout = opt_.dir / (name + ".json");
    if (fs::exists(layout, ec)) {
      const std::string why = CheckLayoutJson(v, ReadText(layout));
      if (!why.empty()) {
        std::fprintf(stderr, "[reload] %s: REFUSED, the layout differs from the compiled table (%s)\n", name.c_str(), why.c_str());
        continue;
      }
    }
    std::vector<uint32_t> words;
    if (!ReadWords(entry.path(), words)) {
      std::fprintf(stderr, "[reload] %s: cannot read\n", entry.path().string().c_str());
      continue;
    }
    (alpha ? active_[v].alpha : active_[v].spirv) = std::move(words);
  }
}

// ---- the render thread's side ---------------------------------------------------------------------------------------------------

bool ShaderReload::Poll(std::vector<int>* changed) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!pending_changed_) return false;
  for (auto& item : pending_) active_[item.first] = std::move(item.second);
  if (changed) *changed = pending_variants_;
  pending_.clear();
  pending_variants_.clear();
  pending_changed_ = false;
  return true;
}

bool ShaderReload::DropsTwin(int variant) const {
  const auto found = active_.find(variant);
  return found != active_.end() && found->second.no_twin;
}

void ShaderReload::Shutdown() { g_instance.reset(); }

const std::vector<uint32_t>* ShaderReload::Find(int variant, bool alpha) const {
  const auto found = active_.find(variant);
  if (found == active_.end()) return nullptr;
  const std::vector<uint32_t>& words = alpha ? found->second.alpha : found->second.spirv;
  return words.empty() ? nullptr : &words;
}

uint64_t ShaderReload::generation() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return generation_;
}
std::string ShaderReload::last_error() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return last_error_;
}
double ShaderReload::last_reload_ms() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return last_ms_;
}
int ShaderReload::overrides() const { return int(active_.size()); }

void ShaderReload::Publish(std::map<int, Entry>&& entries) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& item : entries) {
    pending_variants_.push_back(item.first);
    pending_[item.first] = std::move(item.second);
  }
  pending_changed_ = true;
  ++generation_;
}

// ---- the watcher --------------------------------------------------------------------------------------------------------------------

std::map<std::string, long long> ShaderReload::Snapshot() const {
  std::map<std::string, long long> out;
  std::error_code ec;
  const auto scan = [&](const fs::path& dir, bool all) {
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
      if (!entry.is_regular_file(ec)) continue;
      const std::string name = entry.path().filename().string();
      const std::string ext = entry.path().extension().string();
      const bool wanted = ext == ".glsl" || (all && (ext == ".frag" || ext == ".vert" || name.size() > 8 && name.compare(name.size() - 8, 8, ".fx.json") == 0));
      if (!wanted) continue;
      const auto time = fs::last_write_time(entry.path(), ec).time_since_epoch().count();
      out[entry.path().string()] = static_cast<long long>(time) ^ static_cast<long long>(fs::file_size(entry.path(), ec) << 40);
    }
  };
  scan(opt_.src_dir, true);
  scan(opt_.src_dir.parent_path(), false);
  return out;
}

// The effects whose sources use any of the changed files (by base name).
std::vector<std::string> ShaderReload::EffectsFor(const std::vector<std::string>& changed) const {
  const std::set<std::string> changed_names(changed.begin(), changed.end());
  std::vector<std::string> result;
  std::error_code ec;
  const std::regex source_re("\"source\"\\s*:\\s*\"([^\"]+)\"");  // (not static: a static would be destroyed before the instance stops its worker)
  const std::regex include_re("^[ \\t]*#[ \\t]*include[ \\t]+\"([^\"]+)\"");
  const auto closure = [&](const std::string& start, std::set<std::string>& names) {
    std::vector<std::string> todo{start};
    while (!todo.empty()) {
      const std::string name = todo.back();
      todo.pop_back();
      if (!names.insert(name).second) continue;
      for (const fs::path& dir : {opt_.src_dir, opt_.src_dir.parent_path()}) {
        const fs::path file = dir / name;
        if (!fs::exists(file, ec)) continue;
        const std::string text = ReadText(file);
        std::istringstream lines(text);
        std::string line;
        std::smatch m;
        while (std::getline(lines, line))
          if (std::regex_search(line, m, include_re)) todo.push_back(m[1].str());
        break;
      }
    }
  };
  for (const auto& entry : fs::directory_iterator(opt_.src_dir, ec)) {
    const std::string file = entry.path().filename().string();
    if (file.size() <= 8 || file.compare(file.size() - 8, 8, ".fx.json") != 0) continue;
    const std::string effect = file.substr(0, file.size() - 8);
    std::set<std::string> names{file};
    std::set<std::string> stems{effect};
    const std::string text = ReadText(entry.path());
    for (std::sregex_iterator it(text.begin(), text.end(), source_re), end; it != end; ++it) stems.insert((*it)[1].str());
    for (const std::string& stem : stems) {
      closure(stem + ".frag", names);
      closure(stem + ".vert", names);
    }
    for (const std::string& n : names)
      if (changed_names.count(n)) {
        result.push_back(effect);
        break;
      }
  }
  return result;
}

bool ShaderReload::BuildEffect(const std::string& effect, std::map<int, Entry>& published) {
  const auto started = Clock::now();
  std::error_code ec;
  static int counter = 0;
  const fs::path staging = opt_.dir / ".staging" / (effect + "_" + std::to_string(counter++));
  fs::remove_all(staging, ec);
  fs::create_directories(staging, ec);
  const std::string command = opt_.python + " -I \"" + (opt_.root / "tools" / "fview" / "reload_effect.py").string() + "\" --effect " + effect + " --fx-dir \"" +
                              opt_.src_dir.string() + "\" --out \"" + staging.string() + "\" --write-layouts";
  std::string log;
  const int code = RunCommand(command, log);
  std::vector<std::string> errors;
  {
    std::istringstream lines(log);
    std::string line;
    while (std::getline(lines, line)) {
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
      if (line.compare(0, 7, "FAILED ") == 0) errors.push_back(line.substr(7));
    }
  }
  if (code != 0 && errors.empty()) errors.push_back(effect + ": reload_effect.py exited " + std::to_string(code) + ": " + log.substr(0, 400));
  // Every rebuilt variant must keep the compiled layout (the front-end fills constants and attributes from the compiled table).
  std::map<int, Entry> entries;
  std::vector<std::string> skipped;
  if (errors.empty()) {
    for (const auto& file : fs::directory_iterator(staging, ec)) {
      if (file.path().extension() != ".spv") continue;
      std::string name = file.path().stem().string();
      bool alpha = false;
      if (name.size() > 6 && name.compare(name.size() - 6, 6, ".alpha") == 0) name.resize(name.size() - 6), alpha = true;
      const int v = VariantIndex(name);
      if (v < 0) {
        if (!alpha) skipped.push_back(name);
        continue;
      }
      if (!alpha) {
        const fs::path layout = staging / (name + ".json");
        if (!fs::exists(layout, ec)) {
          errors.push_back(name + ": reload_effect.py gave no layout for it");
          continue;
        }
        const std::string why = CheckLayoutJson(v, ReadText(layout));
        if (!why.empty()) {
          errors.push_back(name + ": the rebuilt layout differs from the compiled table (" + why + "): rebuild the registry (tools\\renderer\\compile_shaders.py)");
          continue;
        }
      }
      std::vector<uint32_t> words;
      if (!ReadWords(file.path(), words)) {
        errors.push_back("cannot read " + file.path().string());
        continue;
      }
      (alpha ? entries[v].alpha : entries[v].spirv) = std::move(words);
    }
    for (auto& item : entries)  // "no alpha-test twin" markers (reload_effect.py writes one for a fragment variant whose description says "alpha_test": false)
      item.second.no_twin = fs::exists(staging / (std::string(rdr::fx::kVariants[item.first].name) + ".notwin"), ec);
  }
  if (!errors.empty()) {
    std::string all;
    for (const std::string& e : errors) {
      std::fprintf(stderr, "[reload] FAILED %s\n", e.c_str());
      all += (all.empty() ? "" : "\n") + e;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      last_error_ = all;
    }
    fs::remove_all(staging, ec);
    return false;
  }
  for (const std::string& s : skipped) std::fprintf(stderr, "[reload] %s: not in the compiled registry (a new variant needs tools\\renderer\\compile_shaders.py); not drawable\n", s.c_str());
  // Published: the files move into the folder (so a restart keeps the reload), then the render thread adopts the SPIR-V at its next frame.
  for (const auto& file : fs::directory_iterator(staging, ec)) {
    std::error_code e2;
    fs::copy_file(file.path(), opt_.dir / file.path().filename(), fs::copy_options::overwrite_existing, e2);
  }
  for (const auto& item : entries) {
    const std::string stem = rdr::fx::kVariants[item.first].name;
    if (item.second.alpha.empty()) fs::remove(opt_.dir / (stem + ".alpha.spv"), ec);  // a stale twin of a variant rebuilt without one
    if (!item.second.no_twin) fs::remove(opt_.dir / (stem + ".notwin"), ec);          // a stale marker of a variant that has its twin again
  }
  fs::remove_all(staging, ec);
  const int variants = int(entries.size());
  {
    std::lock_guard<std::mutex> lock(mutex_);
    last_error_.clear();
  }
  for (auto& item : entries) published[item.first] = std::move(item.second);  // (published once for all the effects of this change: WorkerMain)
  std::fprintf(stderr, "[reload] %s: %d variant(s) rebuilt in %.0f ms\n", effect.c_str(), variants, std::chrono::duration<double, std::milli>(Clock::now() - started).count());
  return true;
}

void ShaderReload::WorkerMain() {
  std::map<std::string, long long> known = Snapshot();
  for (;;) {
    for (int i = 0; i < 4; ++i) {  // 4 x 50 ms = the poll interval, in steps so a stop is quick
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_) return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::map<std::string, long long> now = Snapshot();
    if (now == known) continue;
    const auto seen = Clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(120));  // (an editor may write twice)
    now = Snapshot();
    std::vector<std::string> changed;
    for (const auto& item : now) {
      const auto old = known.find(item.first);
      if (old == known.end() || old->second != item.second) changed.push_back(fs::path(item.first).filename().string());
    }
    for (const auto& item : known)
      if (!now.count(item.first)) changed.push_back(fs::path(item.first).filename().string());
    known = now;
    const std::vector<std::string> effects = EffectsFor(changed);
    if (effects.empty()) {
      std::fprintf(stderr, "[reload] %zu file(s) changed, no effect uses them\n", changed.size());
      continue;
    }
    bool ok = true;
    std::map<int, Entry> all;  // every effect this change touched, adopted by the render thread in ONE step (a shared include never shows half its effects)
    for (const std::string& effect : effects) ok = BuildEffect(effect, all) && ok;
    if (!all.empty()) {
      std::fprintf(stderr, "[reload] published %zu variant(s) of %zu effect(s) in one step\n", all.size(), effects.size());
      Publish(std::move(all));
    }
    if (ok) {
      std::lock_guard<std::mutex> lock(mutex_);
      last_ms_ = std::chrono::duration<double, std::milli>(Clock::now() - seen).count();
    }
  }
}

}  // namespace rdr_scene
