// Session 101 lane 2 (fview): reload, save case, history JSON, case unpack.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

#include "../renderer/player/png.h"
#include "app.h"

#if defined(_WIN32)
#include <process.h>
#endif

namespace fview {

namespace fs = std::filesystem;

fs::path RepoRoot(const Options& options, const char* argv0) {
  if (!options.root.empty()) return fs::absolute(options.root);
  std::error_code ec;
  fs::path exe = fs::absolute(fs::path(argv0 ? argv0 : ""), ec);
  // out\renderer\<kind>\fview.exe -> the repo
  fs::path candidate = exe.parent_path().parent_path().parent_path().parent_path();
  if (fs::exists(candidate / "src" / "fview", ec)) return candidate;
  return fs::current_path();
}

namespace {

std::string Timestamp() {
  const std::time_t t = std::time(nullptr);
  std::tm local{};
#if defined(_WIN32)
  localtime_s(&local, &t);
#else
  localtime_r(&t, &local);
#endif
  char text[32];
  std::strftime(text, sizeof(text), "%Y%m%d-%H%M%S", &local);
  return text;
}

// Runs a command line through the shell, returns its exit code, fills `output` with stdout + stderr.
int RunCommand(const std::string& command, std::string& output) {
  output.clear();
  const std::string line = "\"" + command + " 2>&1\"";  // (cmd /c strips the outer pair)
#if defined(_WIN32)
  std::FILE* pipe = _popen(line.c_str(), "r");
#else
  std::FILE* pipe = popen(command.c_str(), "r");
#endif
  if (!pipe) return -1;
  char buffer[4096];
  size_t n;
  while ((n = std::fread(buffer, 1, sizeof(buffer), pipe)) > 0) output.append(buffer, n);
#if defined(_WIN32)
  return _pclose(pipe);
#else
  return pclose(pipe);
#endif
}

std::string Quote(const std::string& s) { return "\"" + s + "\""; }

bool ReadWords(const fs::path& path, std::vector<uint32_t>& words) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return false;
  std::stringstream data;
  data << file.rdbuf();
  const std::string bytes = data.str();
  if (bytes.empty() || bytes.size() % 4) return false;
  words.resize(bytes.size() / 4);
  std::memcpy(words.data(), bytes.data(), bytes.size());
  return true;
}

}  // namespace

bool ReloadEffect(Session& session, const std::string& effect, const fs::path& fx_dir, const fs::path& root, const std::string& python, ReloadResult& result) {
  result = ReloadResult{};
  const auto started = std::chrono::steady_clock::now();
  static int counter = 0;
  const fs::path out = root / "out" / "fview_reload" / (std::to_string(counter++) + "_" + Timestamp());
  std::error_code ec;
  fs::create_directories(out, ec);
  const fs::path script = root / "tools" / "fview" / "reload_effect.py";
  if (!fs::exists(script, ec)) {
    result.log = "missing " + script.string();
    return false;
  }
  const std::string command = python + " -I " + Quote(script.string()) + " --effect " + Quote(effect) + " --fx-dir " + Quote(fx_dir.string()) + " --out " + Quote(out.string());
  const int code = RunCommand(command, result.log);
  // The script's last line is its JSON summary.
  JsonValue summary;
  const size_t brace = result.log.rfind("{\"effect\"");
  if (brace != std::string::npos) ParseJson(result.log.substr(brace), summary);
  if (const JsonValue* failed = summary.Get("failed"))
    for (const JsonValue& f : failed->array) result.failed.push_back(f.string);
  if (code != 0 || !result.failed.empty() || summary.type != JsonValue::Object) {
    if (result.failed.empty()) result.failed.push_back("reload_effect.py exited " + std::to_string(code) + ": " + result.log.substr(0, 400));
    return false;
  }
  // Collect the files per variant.
  struct Pair {
    std::vector<uint32_t> spirv, alpha;
  };
  std::map<std::string, Pair> variants;
  if (const JsonValue* files = summary.Get("files")) {
    for (const JsonValue& f : files->array) {
      std::string name = f.string;
      name.resize(name.size() - 4);  // ".spv"
      bool alpha = false;
      if (name.size() > 6 && name.compare(name.size() - 6, 6, ".alpha") == 0) {
        name.resize(name.size() - 6);
        alpha = true;
      }
      std::vector<uint32_t> words;
      if (!ReadWords(out / f.string, words)) {
        result.failed.push_back("cannot read " + (out / f.string).string());
        return false;
      }
      (alpha ? variants[name].alpha : variants[name].spirv) = std::move(words);
    }
  }
  // Every rebuilt variant's layout (constant ranges and offsets, attributes, samplers, block size) must equal the compiled table's: the binder fills constants and
  // attributes from the table, so a different layout would draw garbage. One difference refuses the whole reload (nothing is applied).
  const JsonValue* layouts = summary.Get("layouts");
  const auto pairs = [](const JsonValue* list, size_t n, auto&& sink) {
    if (!list || list->type != JsonValue::Array) return;
    for (const JsonValue& row : list->array) {
      if (row.type != JsonValue::Array || row.array.size() < n) continue;
      sink(row);
    }
  };
  for (auto& item : variants) {
    const int v = session.FindVariantByName(item.first);
    if (v < 0) {  // a variant the registry does not have (the description gained one): not drawable until the registry is rebuilt
      result.skipped.push_back(item.first);
      continue;
    }
    const JsonValue* l = layouts ? layouts->Get(item.first.c_str()) : nullptr;
    if (!l) {
      result.failed.push_back(item.first + ": reload_effect.py gave no layout for it, so it cannot be checked against the compiled table");
      continue;
    }
    Session::VariantLayout layout;
    pairs(l->Get("ranges"), 3, [&](const JsonValue& r) { layout.ranges.push_back({int(r.array[0].number), int(r.array[1].number), int(r.array[2].number)}); });
    pairs(l->Get("attributes"), 3, [&](const JsonValue& r) { layout.attributes.push_back({int(r.array[0].number), int(r.array[1].number), int(r.array[2].number)}); });
    pairs(l->Get("samplers"), 2, [&](const JsonValue& r) { layout.samplers.push_back({int(r.array[0].number), int(r.array[1].number)}); });
    layout.block_bytes = int(l->Num("block_bytes", -1));
    layout.bools = int(l->Num("bools", 0xFFFF));
    const std::string why = session.CheckLayout(v, layout);
    if (!why.empty()) result.failed.push_back(item.first + ": the rebuilt layout differs from the compiled table (" + why + "): the registry must be rebuilt (tools\\renderer\\compile_shaders.py)");
  }
  if (!result.failed.empty()) return false;  // nothing applied
  for (auto& item : variants) {
    const int v = session.FindVariantByName(item.first);
    if (v < 0) continue;
    ReloadVariant rv;
    rv.name = item.first;
    rv.before = session.VariantSpirvHash(v, false);
    rv.before_alpha = session.VariantSpirvHash(v, true);
    // A fragment variant rebuilt without an alpha-test twin (its description says "alpha_test": false) is drawn plain; the compiled twin is not used.
    const bool drop_twin = item.second.alpha.empty() && item.first.size() > 9 && item.first.compare(item.first.size() - 9, 9, "_fragment") == 0;
    const bool had_twin = session.VariantHasAlphaTwin(v);  // S101 lane 20: a dropped twin is a change too (the hashes below still read the compiled twin)
    session.SetOverride(v, item.second.spirv, item.second.alpha, drop_twin);
    rv.after = session.VariantSpirvHash(v, false);
    rv.after_alpha = session.VariantSpirvHash(v, true);
    rv.changed = rv.before != rv.after || rv.before_alpha != rv.after_alpha || (had_twin && !session.VariantHasAlphaTwin(v));
    result.variants.push_back(rv);
  }
  std::string error;
  result.ok = session.Rebind(error);
  if (!result.ok) result.failed.push_back(error);
  for (const std::string& refused : session.TakeOverrideErrors()) {  // a reloaded shader the driver would not take (found while binding)
    result.failed.push_back(refused);
    result.ok = false;
  }
  result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  return result.ok;
}

void WriteReloadJson(JsonWriter& w, const ReloadResult& r) {
  const auto hex = [](uint64_t v) {
    char t[24];
    std::snprintf(t, sizeof(t), "%016llX", static_cast<unsigned long long>(v));
    return std::string(t);
  };
  w.Begin('{');
  w.Key("ok"), w.Bool(r.ok);
  w.Key("seconds"), w.Num(r.seconds);
  int changed = 0;
  for (const ReloadVariant& v : r.variants) changed += v.changed;
  w.Key("changed_count"), w.Int(changed);
  w.Key("failed");
  w.Begin('[');
  for (const std::string& f : r.failed) w.Str(f);
  w.End();
  w.Key("skipped");
  w.Begin('[');
  for (const std::string& f : r.skipped) w.Str(f);
  w.End();
  w.Key("variants");
  w.Begin('[');
  for (const ReloadVariant& v : r.variants) {
    w.Begin('{');
    w.Key("name"), w.Str(v.name);
    w.Key("before"), w.Str(hex(v.before));
    w.Key("after"), w.Str(hex(v.after));
    w.Key("before_alpha"), w.Str(hex(v.before_alpha));
    w.Key("after_alpha"), w.Str(hex(v.after_alpha));
    w.Key("changed"), w.Bool(v.changed);
    w.End();
  }
  w.End();
  w.End();
}

void WriteHistoryJson(JsonWriter& w, Session& session, const EffectIndex* effects, uint32_t surface, int px, int py, int through_op) {
  const std::vector<HistoryEntry>& history = session.PixelHistory(surface, px, py, through_op);
  w.Begin('{');
  w.Key("surface"), w.Int(surface);
  w.Key("pixel");
  w.Begin('[');
  w.Int(px), w.Int(py);
  w.End();
  int top = -1;
  for (size_t i = 0; i < history.size(); ++i)
    if (history[i].wrote && history[i].kind == "draw") {
      top = int(i);
      break;
    }
  w.Key("entry_count"), w.Int(long(history.size()));
  w.Key("top_wrote_index"), w.Int(top);
  w.Key("entries");
  w.Begin('[');
  for (size_t i = 0; i < history.size(); ++i) {
    const HistoryEntry& e = history[i];
    w.Begin('{');
    w.Key("op"), w.Int(e.op);
    w.Key("kind"), w.Str(e.kind);
    w.Key("index"), w.Int(e.index);
    w.Key("drawn"), w.Bool(e.drawn);
    w.Key("result"), w.Str(e.kind == "draw" ? (e.drawn ? (e.value_known ? (e.wrote ? "wrote" : "covered, unchanged") : "covers (value unknown)") : "not drawn") : (e.value_known && !e.wrote ? e.kind + ", unchanged" : e.kind));
    w.Key("note"), w.Str(e.note);
    if (e.value_known) {
      w.Key("before");
      w.Begin('[');
      for (int k = 0; k < 4; ++k) w.Int(e.before[k]);
      w.End();
      w.Key("after");
      w.Begin('[');
      for (int k = 0; k < 4; ++k) w.Int(e.after[k]);
      w.End();
    }
    if (e.kind == "draw") {
      DrawInfo info;
      std::string error;
      if (DescribeDraw(session, effects, e.op, info, error)) {
        w.Key("effect"), w.Str(info.effect);
        w.Key("vs_variant"), w.Str(info.vs_variant);
        w.Key("ps_variant"), w.Str(info.ps_variant);
        w.Key("alpha_line"), w.Str(info.alpha_line);
        if (int(i) == top) {  // the full description for the draw that put the pixel there
          w.Key("draw");
          WriteDrawJson(w, info);
        }
      }
    }
    w.End();
  }
  w.End();
  w.End();
}

std::string SaveCase(Session& session, const EffectIndex* effects, const fs::path& root, uint32_t surface, int op, bool have_pixel, int px, int py, std::string& error) {
  std::error_code ec;
  const fs::path cases = root / "out" / "cases";
  fs::create_directories(cases, ec);
  const fs::path keep = cases / "KEEP";
  if (!fs::exists(keep, ec)) {
    std::ofstream k(keep);
    k << "case folders: game imagery, local only, Session 101\n";
  }
  const std::string capture_name = fs::path(session.folder()).filename().string();
  if (op < 0) op = int(session.frame().ops.count) - 1;
  const fs::path dir = cases / ("fview_" + capture_name + "_" + std::to_string(session.frame_index()) + "_" + std::to_string(op) + "_" + Timestamp());
  fs::create_directories(dir, ec);
  if (!surface) surface = session.DefaultSurface();
  session.RenderThrough(op);
  std::vector<uint8_t> rgba;
  uint32_t w = 0, h = 0;
  if (!session.ReadSurface(surface, rgba, w, h) || !rdr::WritePng((dir / "picture.png").string().c_str(), rgba.data(), w, h)) {
    error = "could not read / write surface " + std::to_string(surface) + " after op " + std::to_string(op);
    return {};
  }
  JsonWriter j;
  j.Begin('{');
  j.Key("tool"), j.Str("fview (Session 101 lane 2)");
  j.Key("capture"), j.Str(session.folder());
  j.Key("frame"), j.Int(session.frame_index());
  j.Key("op"), j.Int(op);
  j.Key("surface"), j.Int(surface);
  j.Key("picture"), j.Str("picture.png");
  j.Key("picture_size");
  j.Begin('[');
  j.Int(w), j.Int(h);
  j.End();
  if (have_pixel) {
    j.Key("pixel");
    j.Begin('[');
    j.Int(px), j.Int(py);
    j.End();
    j.Key("history");
    WriteHistoryJson(j, session, effects, surface, px, py, op);  // the history as of the saved op
  }
  const rdr_adapter::LoadedFrame& frame = session.frame();
  if (size_t(op) < frame.ops.count) {
    j.Key("op_kind"), j.Str(OpKindName(frame.ops[uint32_t(op)].kind));
    if (frame.ops[uint32_t(op)].kind == rdr_adapter::OpKind::Draw) {
      DrawInfo info;
      std::string e2;
      if (DescribeDraw(session, effects, uint32_t(op), info, e2)) {
        j.Key("draw");
        WriteDrawJson(j, info);
      }
    }
  }
  j.End();
  std::ofstream out(dir / "case.json", std::ios::binary);
  out << j.text() << "\n";
  if (!out) {
    error = "could not write " + (dir / "case.json").string();
    return {};
  }
  return dir.string();
}

std::string UnpackCase(const std::string& zip, const fs::path& root, std::string& error) {
  std::error_code ec;
  if (!fs::exists(zip, ec)) {
    error = "case zip not found: " + zip;
    return {};
  }
  const fs::path dir = root / "out" / "cases" / ("fview_unpack_" + Timestamp());
  fs::create_directories(dir, ec);
  std::string output;
  // Windows ships bsdtar as tar.exe; it reads zip files.
  // S101 lane 59: Windows' own bsdtar by path (a GNU tar earlier on PATH, from Git Bash, reads "C:" as a host name and fails).
  const char* system_root = std::getenv("SystemRoot");
  const std::string tar = system_root ? Quote((fs::path(system_root) / "System32" / "tar.exe").string()) : std::string("tar");
  const int code = RunCommand(tar + " -xf " + Quote(zip) + " -C " + Quote(dir.string()), output);
  if (code != 0) {
    error = "could not unpack " + zip + ": " + output.substr(0, 300);
    return {};
  }
  const fs::path capture = dir / "capture";
  if (!fs::exists(capture / "images.bin", ec)) {
    error = "the case zip holds no capture\\ folder with images.bin (docs\\renderer\\CASE_ZIP.md)";
    return {};
  }
  return capture.string();
}

}  // namespace fview
