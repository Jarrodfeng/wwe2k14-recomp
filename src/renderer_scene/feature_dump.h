// S103 lane T2 (tools\scene\feature_diff.py): a dev-only dump of WHAT OUR frame graph drew, one JSON line per draw, written only when WWE_RDR_FEATURE_DUMP=<file> is set
// (appended; the offline rig sets it, the game never does). Per draw: the vertex / pixel variant names the front-end chose (the same "<effect>_<variant>_<stage>" names the game's
// shader hashes resolve to through the fx.json files), what each sampler slot of the pixel variant is bound to (a real map / white / black / grey / neutral normal / black cube),
// and every constant field of both variants resolved by name (the values the draw is fed). Read-only on the frame: nothing here changes a draw.
// "pass" lines (FeatureDumpPass) say which post steps our frame ran. No game data is written (names, kinds and numbers only).
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace rdr_scene::fdump {

inline FILE* File() {
  static FILE* f = [] { const char* p = std::getenv("WWE_RDR_FEATURE_DUMP"); return (p && *p) ? std::fopen(p, "ab") : nullptr; }();
  return f;
}

inline std::string Esc(const std::string& s) {
  std::string o;
  for (char c : s) { if (c == '"' || c == '\\') o += '\\'; if (static_cast<unsigned char>(c) >= 32) o += c; }
  return o;
}

// One draw. `kind_of(slot_image)` -> a short word ("map", "white", "black", ...); `resolve(name, vec4s, out)` fills out[4 * vec4s] (false = unresolved: the value is omitted).
template <class KindOf, class Resolver, class Table>
void Draw(FILE* f, const std::string& site, const std::string& shader, const std::string& part, bool chr, int blend, const char* vs_name, const char* ps_name, const Table* vt, const Table* ft,
          uint32_t sampler_count, const int* slots, const uint32_t* images, KindOf kind_of, Resolver resolve) {
  std::string js = "{\"k\":\"draw\",\"site\":\"" + site + "\",\"shader\":\"" + Esc(shader) + "\",\"part\":\"" + Esc(part) + "\",\"chr\":" + (chr ? "1" : "0") + ",\"blend\":" + std::to_string(blend) +
                   ",\"vs\":\"" + vs_name + "\",\"ps\":\"" + ps_name + "\",\"slots\":{";
  for (uint32_t s = 0; s < sampler_count; ++s) js += (s ? "," : "") + std::string("\"") + std::to_string(slots[s]) + "\":\"" + kind_of(images[s]) + "\"";
  js += "},\"vals\":{";
  bool first = true;
  float out[64];
  for (const Table* t : {vt, ft}) {
    if (!t) continue;
    for (uint32_t k = 0; k < t->field_count; ++k) {
      const auto& fld = t->fields[k];
      if (fld.vec4s == 0 || fld.vec4s > 16) continue;
      for (float& v : out) v = 0.f;
      if (!resolve(std::string(fld.name), uint32_t(fld.vec4s), out)) continue;
      js += std::string(first ? "" : ",") + "\"" + fld.name + "\":[";
      first = false;
      char num[32];
      for (uint32_t i = 0; i < uint32_t(fld.vec4s) * 4u; ++i) { std::snprintf(num, sizeof num, "%s%.6g", i ? "," : "", double(out[i])); js += num; }
      js += "]";
    }
  }
  js += "}}\n";
  std::fwrite(js.data(), 1, js.size(), f);
}

// S103 B2: the render state of the draw line just written (tools\scene\b2_check.py pairs it with the game's DrawFixed): alpha test on / func / reference (0-255, rounded), depth test / write / compare, cull, blend.
template <class State, class Dyn>
void StateLine(FILE* f, const State& s, bool alpha_test, const Dyn& dyn, int pass_class, float diffuse_alpha_clear, int out_alpha) {
  char buf[320];
  const int n = std::snprintf(buf, sizeof buf, "{\"k\":\"state\",\"dal\":%.4f,\"oa\":%d,\"pc\":%d,\"at\":%d,\"af\":%d,\"ar\":%d,\"dt\":%d,\"dw\":%d,\"dc\":%d,\"cull\":%d,\"ben\":%d,\"bsrc\":%d,\"bdst\":%d}\n", double(diffuse_alpha_clear), out_alpha, pass_class, alpha_test ? 1 : 0,
                              int(dyn.system[9]), int(dyn.system[8] * 255.f + 0.5f), int(s.depth_test), int(s.depth_write), int(s.depth_compare), int(s.cull), int(s.blend[0].enable), int(s.blend[0].src_color),
                              int(s.blend[0].dst_color));
  std::fwrite(buf, 1, size_t(n), f);
}

inline void Pass(FILE* f, const std::string& json_fields) {
  const std::string js = "{\"k\":\"pass\"," + json_fields + "}\n";
  std::fwrite(js.data(), 1, js.size(), f);
}

}  // namespace rdr_scene::fdump
