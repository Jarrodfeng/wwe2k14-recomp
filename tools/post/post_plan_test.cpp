// Session 101, lane PS: the unit test of the post plan (src\renderer_scene\post_plan.*), against lane PC's census numbers (tools\post\expect.json).
// Plays every frame of each capture folder through the SHIPPED code (ReadStreamChain -> BuildPostPlan) and compares what the plans say with the census, group by group:
//   shapes        frames, the shape of every frame (match / film / entrance / two_layer / editor / cards), the variant sequences, post draws and restores a frame, no frame refused
//   layers        dof layers a frame (Bryan: two), the combine's slot 2 vs slot 6 (different on Bryan, the same image elsewhere), who made slot 6 (the first layer's hole_fill)
//   combine       the combine's variant, its texture slots, its call site (named captures)
//   dead_work     the bloom / glare strength flags (c31.x, c32.x), the draws and copies of each side, and that the plan skips exactly 13 draws + 14 copies when both are zero
//   dof_filter    the dof depth filter: the plan's rule (point where the frame has a restore, linear where it has none) == the stream's own sampler, on every valid capture
//   constants     c20 / c21 / c27 / c29 / c30 / c31 / c32 and the film bank, min / max / distinct over the frames, read from the stream's draws
//   chain_end     the chain's last draw is the structural last chain draw (the film pass or the combine), found WITHOUT the vertex hash (Draw::vs_patched_hash is zeroed here)
//   inputs        the chain fetch slots that read image 0 (the null_input family: brock_tron / brock_s15_300), after_combine: the draws the plan leaves to the stream
// Plants (--plant): layer_merged, dead_bloom_kept, dof_filter_fixed, last_pass_by_hash (each runs through the same code; each must fail exactly its check).
//   post_plan_test [--plant NAME] [-q] [--expect FILE] LABEL=CAPTURE_FOLDER...      exit 0 all PASS, 1 any FAIL, 2 usage
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "renderer_adapter/capture_reader.h"
#include "renderer_scene/post_plan.h"

using namespace rdr_scene;

// ---- a small JSON value + parser (expect.json) --------------------------------------------------------------------------------------------
struct Json {
  enum T { Null, Num, Str, Arr, Obj } t = Null;
  double n = 0;
  std::string s;
  std::vector<Json> a;
  std::map<std::string, Json> o;
  static Json Number(double v) { Json j; j.t = Num, j.n = v; return j; }
  static Json String(const std::string& v) { Json j; j.t = Str, j.s = v; return j; }
  static Json Array() { Json j; j.t = Arr; return j; }
  static Json Object() { Json j; j.t = Obj; return j; }
  const Json* Get(const std::string& k) const { const auto it = o.find(k); return it == o.end() ? nullptr : &it->second; }
};
struct Parser {
  const std::string& s;
  size_t i = 0;
  bool bad = false;
  explicit Parser(const std::string& text) : s(text) {}
  void Ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) ++i; }
  std::string Str() {
    std::string out;
    ++i;
    while (i < s.size() && s[i] != '"') {
      if (s[i] == '\\' && i + 1 < s.size()) { ++i; out += s[i] == 'n' ? '\n' : s[i] == 't' ? '\t' : s[i]; }
      else out += s[i];
      ++i;
    }
    ++i;
    return out;
  }
  Json Value() {
    Ws();
    Json j;
    if (i >= s.size()) { bad = true; return j; }
    if (s[i] == '{') {
      j.t = Json::Obj, ++i;
      for (;;) {
        Ws();
        if (i < s.size() && s[i] == '}') { ++i; break; }
        if (i >= s.size() || s[i] != '"') { bad = true; return j; }
        const std::string k = Str();
        Ws();
        if (i >= s.size() || s[i] != ':') { bad = true; return j; }
        ++i;
        j.o[k] = Value();
        Ws();
        if (i < s.size() && s[i] == ',') ++i;
      }
    } else if (s[i] == '[') {
      j.t = Json::Arr, ++i;
      for (;;) {
        Ws();
        if (i < s.size() && s[i] == ']') { ++i; break; }
        if (i >= s.size()) { bad = true; return j; }
        j.a.push_back(Value());
        Ws();
        if (i < s.size() && s[i] == ',') ++i;
      }
    } else if (s[i] == '"') {
      j.t = Json::Str, j.s = Str();
    } else if (s.compare(i, 4, "true") == 0) { j = Json::Number(1), i += 4; }
    else if (s.compare(i, 5, "false") == 0) { j = Json::Number(0), i += 5; }
    else if (s.compare(i, 4, "null") == 0) { i += 4; }
    else {
      char* end = nullptr;
      j.t = Json::Num, j.n = std::strtod(s.c_str() + i, &end);
      if (end == s.c_str() + i) { bad = true; return j; }
      i = size_t(end - s.c_str());
    }
    return j;
  }
};

static double Num(const Json& j) {  // 'inf' / '-inf' / 'nan' come as strings in the census json
  if (j.t == Json::Str) return j.s == "inf" ? INFINITY : j.s == "-inf" ? -INFINITY : NAN;
  return j.n;
}
static bool Same(const Json& a, const Json& b, std::string& why, const std::string& path) {
  const bool an = a.t == Json::Num || (a.t == Json::Str && (a.s == "inf" || a.s == "-inf" || a.s == "nan"));
  const bool bn = b.t == Json::Num || (b.t == Json::Str && (b.s == "inf" || b.s == "-inf" || b.s == "nan"));
  if (an && bn) {
    const double x = Num(a), y = Num(b);
    const bool ok = (std::isnan(x) && std::isnan(y)) || x == y || std::fabs(x - y) <= 2e-5 + 1e-6 * std::fabs(x);
    if (!ok) { char buf[160]; std::snprintf(buf, sizeof buf, "%s: expect %.7g, plan %.7g", path.c_str(), y, x); why = buf; }
    return ok;
  }
  if (a.t != b.t) { why = path + ": different kinds"; return false; }
  if (a.t == Json::Str) { if (a.s != b.s) { why = path + ": '" + a.s + "' vs '" + b.s + "'"; return false; } return true; }
  if (a.t == Json::Arr) {
    if (a.a.size() != b.a.size()) { why = path + ": array lengths " + std::to_string(a.a.size()) + " vs " + std::to_string(b.a.size()); return false; }
    for (size_t i = 0; i < a.a.size(); ++i) if (!Same(a.a[i], b.a[i], why, path + "[" + std::to_string(i) + "]")) return false;
    return true;
  }
  if (a.t == Json::Obj) {
    for (const auto& kv : a.o) {
      const Json* o = b.Get(kv.first);
      if (!o) { why = path + "." + kv.first + ": in the plan, not in the census"; return false; }
      if (!Same(kv.second, *o, why, path + "." + kv.first)) return false;
    }
    for (const auto& kv : b.o) if (!a.Get(kv.first)) { why = path + "." + kv.first + ": in the census, not in the plan"; return false; }
  }
  return true;
}

// ---- the aggregates ---------------------------------------------------------------------------------------------------------------------
struct MinMax {
  bool any = false;
  double lo = 0, hi = 0;
  void Add(double v) { if (!any) lo = hi = v, any = true; else lo = std::min(lo, v), hi = std::max(hi, v); }
  Json ToJson() const { Json j = Json::Array(); j.a.push_back(Json::Number(lo)); j.a.push_back(Json::Number(hi)); return j; }
};
using Counts = std::map<std::string, int>;
static Json CountsJson(const Counts& c) { Json j = Json::Object(); for (const auto& kv : c) j.o[kv.first] = Json::Number(kv.second); return j; }
static Json MinMaxJson(const std::map<std::string, MinMax>& m) { Json j = Json::Object(); for (const auto& kv : m) j.o[kv.first] = kv.second.ToJson(); return j; }
static double R5(double x) { return std::round(x * 1e5) / 1e5; }
static Json Vec4Json(const float* v) {
  Json j = Json::Array();
  for (int k = 0; k < 4; ++k) {
    const double x = v[k];
    j.a.push_back(std::isinf(x) ? Json::String(x > 0 ? "inf" : "-inf") : std::isnan(x) ? Json::String("nan") : Json::Number(R5(x)));
  }
  return j;
}

struct Capture {
  std::string label;
  int frames = 0, refused = 0, bad_read = 0;
  std::string first_refusal;
  Counts shapes, sequences, combine_variants, combine_slots, combine_sites, dof_layers, slot26, slot6_source, dead_flags, dof_depth;
  std::map<std::string, MinMax> post_draws, dead_draws, dead_copies, after;
  MinMax post_copies_all;
  int filter_bad = 0, filter_checked = 0, end_bad = 0, end_film_mismatch = 0, skip_bad = 0, skip_checked = 0, site_bad = 0, null_total = 0;
  std::string first_filter_bad, first_end_bad, first_skip_bad;
  int chain_after_combine_film = 0;
};

static const char* FilterName(uint8_t f) { return f == 0 ? "point" : f == 1 ? "linear" : "mixed"; }

static void Fold(Capture& c, uint32_t fi, const StreamChain& s, const PostPlan& p) {
  ++c.frames;
  c.shapes[PostShapeName(p.shape)]++;
  c.sequences[p.sequence]++;
  if (!p.ok) {
    ++c.refused;
    if (c.first_refusal.empty()) c.first_refusal = "frame " + std::to_string(fi) + ": " + p.refusal + " [" + p.sequence + "]";
    return;
  }
  c.post_draws["all"].Add(double(s.draws.size()));
  c.post_draws["restores"].Add(double(p.restores));
  c.post_draws["dof_layers"].Add(double(p.layers.size()));
  c.dof_layers[std::to_string(p.layers.size())]++;
  c.combine_variants[p.combine_variant]++;
  c.combine_slots[p.slots]++;
  const ChainDraw& C = s.draws[p.combine];
  if (C.site) {
    char site[32];
    std::snprintf(site, sizeof site, "0x%x", C.site);
    c.combine_sites[site]++;
  }
  if (s.named) for (const ChainDraw& d : s.draws) if (d.site != 0x82C1F48Cu && d.site != 0x82C1F674u) ++c.site_bad;
  if (C.Slot(2) && C.Slot(6)) {
    const bool same = p.slot_image[2] == p.slot_image[6];
    c.slot26[same ? "same" : "different"]++;
    if (!same) {
      c.slot6_source["hole_fill <- slot2 hole_fill"]++;
      c.slot6_source["layers " + std::to_string(p.slot6_layer + 1) + " vs " + std::to_string(p.slot2_layer + 1)]++;
    }
  }
  // copies the chain makes: the ones whose source is a chain draw's target
  std::set<uint32_t> targets;
  for (const ChainDraw& d : s.draws) targets.insert(d.target);
  int own = 0;
  for (const ChainCopy& k : s.copies) own += targets.count(k.src) ? 1 : 0;
  c.post_copies_all.Add(own);
  // dead work (census definitions: only frames whose combine has the bloom permutation)
  if (p.bloom_variant && p.hdr_drawn) {
    c.dead_flags[std::string("bloom_zero=") + (p.bloom_zero ? "1" : "0") + " glare_zero=" + (p.glare_zero ? "1" : "0")]++;
    if (p.bloom_zero) c.dead_draws["bloom"].Add(p.bloom_side_draws), c.dead_copies["bloom"].Add(p.bloom_side_copies);
    if (p.glare_zero) c.dead_draws["glare"].Add(p.glare_side_draws), c.dead_copies["glare"].Add(p.glare_side_copies);
    if (p.bloom_zero && p.glare_zero) {
      c.dead_draws["both"].Add(p.bloom_side_draws + p.glare_side_draws), c.dead_copies["both"].Add(p.bloom_side_copies + p.glare_side_copies);
      ++c.skip_checked;
      if (p.skipped_draws != p.bloom_side_draws + p.glare_side_draws || p.skipped_copies != p.bloom_side_copies + p.glare_side_copies) {
        ++c.skip_bad;
        if (c.first_skip_bad.empty()) c.first_skip_bad = "frame " + std::to_string(fi) + ": plan skips " + std::to_string(p.skipped_draws) + " draws + " + std::to_string(p.skipped_copies) + " copies";
      }
    } else if (!p.bloom_zero && !p.glare_zero && (p.skipped_draws || p.skipped_copies)) {
      ++c.skip_bad;
      if (c.first_skip_bad.empty()) c.first_skip_bad = "frame " + std::to_string(fi) + ": live bloom and glare, yet the plan skips draws";
    }
    c.dead_draws["bloom_side_drawn"].Add(p.bloom_side_draws), c.dead_draws["glare_side_drawn"].Add(p.glare_side_draws);
    c.dead_copies["bloom_side_copies"].Add(p.bloom_side_copies), c.dead_copies["glare_side_copies"].Add(p.glare_side_copies);
  }
  // the dof depth filter
  for (const PostLayer& l : p.layers) {
    char key[128];
    if (l.depth_image == 0) std::snprintf(key, sizeof key, "%s | image 0 (a fetch of nothing)", FilterName(l.stream_filter));
    else std::snprintf(key, sizeof key, "%s | restores in frame %u", FilterName(l.stream_filter), p.restores);
    c.dof_depth[key]++;
    if (l.depth_image != 0) {  // the null_input family (image 0) says nothing about the game's rule (POST_PROMOTION_MAP.md section 9, correction 1)
      ++c.filter_checked;
      if (l.depth_linear != (l.stream_filter == 1)) {
        ++c.filter_bad;
        if (c.first_filter_bad.empty()) c.first_filter_bad = "frame " + std::to_string(fi) + ": the plan says " + (l.depth_linear ? "linear" : "point") + ", the stream's sampler is " + FilterName(l.stream_filter);
      }
    }
  }
  // the chain's end
  const bool film_in_stream = p.chain_last != p.combine;
  if (film_in_stream) ++c.chain_after_combine_film;
  if (!p.chain_end_found || p.chain_last_draw != s.draws.back().draw || p.chain_last != uint32_t(s.draws.size() - 1)) {
    ++c.end_bad;
    if (c.first_end_bad.empty()) c.first_end_bad = "frame " + std::to_string(fi) + ": " + (p.chain_end_found ? "the plan ends at the wrong draw" : "the end of the chain was not found");
  }
  c.after["all"].Add(p.later_draws);
  c.after["chain_after_combine"].Add(film_in_stream ? 1 : 0);
  c.null_total += int(p.null_inputs);
}

// constants need component-wise min / max: kept in their own table (a vec4 of MinMax per register)
struct ConstAgg {
  MinMax v[4];
  std::set<std::string> distinct;
};
static std::map<std::string, std::map<std::string, ConstAgg>> g_const;  // label -> register name -> aggregate
static std::map<std::string, std::set<std::string>> g_film;             // label -> distinct film banks

static void FoldConstants(const std::string& label, const PostPlan& p) {
  if (!p.ok) return;
  const PostParams& q = p.params;
  const auto add = [&](const char* name, const float* v) {
    ConstAgg& a = g_const[label][name];
    for (int k = 0; k < 4; ++k) a.v[k].Add(v[k]);
    a.distinct.insert(std::string(reinterpret_cast<const char*>(v), 16));
  };
  if (p.dof) add("c20_DOFParam", q.DOFParam), add("c21_DOFParam2", q.DOFParam2);
  if (p.fog) add("c27_RGB", q.RGB), add("c29_Alpha", q.Alpha), add("c30_ZAdj", q.ZAdj);
  if (p.bloom_variant) add("c31_BloomScale", q.BloomScale), add("c32_StarScale", q.StarScale);
  if (p.film) {
    std::string bank(reinterpret_cast<const char*>(q.film_cb), 16);
    bank += std::string(reinterpret_cast<const char*>(q.film_cr), 16) + std::string(reinterpret_cast<const char*>(q.film_alpha), 16);
    g_film[label].insert(bank);
  }
}

static Json ConstantsJson(const std::string& label) {
  Json j = Json::Object();
  const auto it = g_const.find(label);
  if (it == g_const.end()) return j;
  for (const auto& kv : it->second) {
    float lo[4], hi[4];
    for (int k = 0; k < 4; ++k) lo[k] = float(kv.second.v[k].lo), hi[k] = float(kv.second.v[k].hi);
    Json e = Json::Object();
    e.o["min"] = Vec4Json(lo), e.o["max"] = Vec4Json(hi), e.o["distinct"] = Json::Number(double(kv.second.distinct.size()));
    j.o[kv.first] = e;
  }
  return j;
}

// ---- the census side: normalise expect.json to the plan's keys ---------------------------------------------------------------------------------
static Json ExpectDofDepth(const Json* e) {  // "point | restores in frame 3 | image written by ext | fmt23" -> "point | restores in frame 3"
  Json out = Json::Object();
  if (!e) return out;
  for (const auto& kv : e->o) {
    const std::string& k = kv.first;
    const size_t a = k.find(" | ");
    std::string key = k;
    if (a != std::string::npos) {
      const size_t b = k.find(" | ", a + 3);
      key = b == std::string::npos ? k : k.substr(0, b);
    }
    Json& slot = out.o[key];
    slot.t = Json::Num, slot.n += kv.second.n;
  }
  return out;
}

static int g_bad = 0;
static bool g_quiet = false;
static void Report(bool ok, const std::string& label, const std::string& name, const std::string& detail) {
  if (!ok) ++g_bad;
  if (!ok || !g_quiet) std::printf("%s  %-14s %-12s %s\n", ok ? "PASS" : "FAIL", label.c_str(), name.c_str(), detail.c_str());
}

static bool CompareGroup(const Capture& c, const Json& E, const std::string& group, const std::vector<std::pair<std::string, Json>>& mine) {
  std::string why;
  bool ok = true;
  for (const auto& kv : mine) {
    const Json* e = E.Get(kv.first);
    Json empty = Json::Object();
    if (!e) e = &empty;
    std::string w;
    if (!Same(kv.second, *e, w, kv.first)) { ok = false; if (why.empty()) why = w; }
  }
  Report(ok, c.label, group, ok ? "matches expect.json" : why);
  return ok;
}

int main(int argc, char** argv) {
  std::string plant, expect_path = "tools/post/expect.json";
  std::vector<std::pair<std::string, std::string>> caps;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--plant" && i + 1 < argc) plant = argv[++i];
    else if (a == "--expect" && i + 1 < argc) expect_path = argv[++i];
    else if (a == "-q") g_quiet = true;
    else if (a.find('=') != std::string::npos) caps.emplace_back(a.substr(0, a.find('=')), a.substr(a.find('=') + 1));
    else { std::printf("usage: post_plan_test [--plant layer_merged|dead_bloom_kept|dof_filter_fixed|last_pass_by_hash] [-q] [--expect FILE] LABEL=CAPTURE_FOLDER...\n"); return 2; }
  }
  if (plant == "layer_merged") g_plan_plant = PlanPlant::LayerMerged;
  else if (plant == "dead_bloom_kept") g_plan_plant = PlanPlant::DeadBloomKept;
  else if (plant == "dof_filter_fixed") g_plan_plant = PlanPlant::DofFilterFixed;
  else if (plant == "last_pass_by_hash") g_plan_plant = PlanPlant::LastPassByHash;
  else if (!plant.empty()) { std::printf("unknown plant %s\n", plant.c_str()); return 2; }
  if (caps.empty()) { std::printf("FAIL  input: no captures given (a check that reads nothing must fail)\n"); return 1; }
  std::string text;
  {
    std::ifstream in(expect_path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    text = ss.str();
  }
  Parser parser(text);
  const Json expect = text.empty() ? Json() : parser.Value();
  if (text.empty() || parser.bad || expect.t != Json::Obj) { std::printf("FAIL  input: cannot read %s\n", expect_path.c_str()); return 1; }

  for (const auto& cp : caps) {
    Capture c;
    c.label = cp.first;
    const Json* E = expect.Get(cp.first);
    if (!E) { Report(false, c.label, "input", "no census entry in " + expect_path); continue; }
    rdr_adapter::CaptureReader reader;
    if (!reader.Open(cp.second)) { Report(false, c.label, "input", "cannot open capture " + cp.second); continue; }
    for (uint32_t fi = 0; fi < reader.frame_count(); ++fi) {
      rdr_adapter::LoadedFrame f;
      if (!reader.LoadFrame(fi, f)) { ++c.bad_read; continue; }
      // the plan must not need the vertex-program hash that marked the end of the chain: zero it in the copy of the frame we read
      for (uint32_t d = 0; d < f.draws.count; ++d) const_cast<rdr_adapter::Draw&>(f.draws[d]).vs_patched_hash = 0;
      StreamChain s;
      std::string err;
      if (!ReadStreamChain(reader, f, s, err)) { ++c.bad_read; if (c.first_refusal.empty()) c.first_refusal = "frame " + std::to_string(fi) + ": " + err; continue; }
      if (s.draws.empty()) { ++c.frames; c.shapes["none"]++; ++c.refused; continue; }
      const PostPlan p = BuildPostPlan(s);
      Fold(c, fi, s, p);
      FoldConstants(c.label, p);
    }
    if (c.bad_read) { Report(false, c.label, "input", std::to_string(c.bad_read) + " frames could not be read: " + c.first_refusal); continue; }

    // ---- shapes
    {
      std::vector<std::pair<std::string, Json>> m;
      m.push_back({"frames", Json::Number(c.frames)});
      m.push_back({"shapes", CountsJson(c.shapes)});
      m.push_back({"sequences", CountsJson(c.sequences)});
      Json pd = Json::Object();
      if (c.post_draws.count("all")) pd.o["all"] = c.post_draws["all"].ToJson(), pd.o["restores"] = c.post_draws["restores"].ToJson();
      const Json* ep = E->Get("post_draws");
      Json epd = Json::Object();
      if (ep) for (const char* k : {"all", "restores"}) if (ep->Get(k)) epd.o[k] = *ep->Get(k);
      bool ok = true;
      std::string why, w;
      for (const auto& kv : m) { const Json* e = E->Get(kv.first); if (!e || !Same(kv.second, *e, w, kv.first)) { ok = false; if (why.empty()) why = w.empty() ? kv.first + ": missing" : w; } }
      if (!Same(pd, epd, w, "post_draws")) { ok = false; if (why.empty()) why = w; }
      if (c.refused) { ok = false; why = std::to_string(c.refused) + " frames refused, first " + c.first_refusal + (why.empty() ? "" : "; " + why); }
      Report(ok, c.label, "shapes", ok ? "every frame planned; shapes " + [&] { std::string r; for (auto& kv : c.shapes) r += kv.first + " " + std::to_string(kv.second) + " "; return r; }() + "== census" : why);
    }
    // ---- layers
    {
      Json pdl = Json::Object();
      if (c.post_draws.count("dof_layers")) pdl = c.post_draws["dof_layers"].ToJson();
      const Json* ep = E->Get("post_draws");
      bool ok = true;
      std::string why, w;
      Json mine_pd = Json::Object();
      if (c.post_draws.count("dof_layers")) mine_pd.o["dof_layers"] = pdl;
      Json e_pd = Json::Object();
      if (ep && ep->Get("dof_layers")) e_pd.o["dof_layers"] = *ep->Get("dof_layers");
      if (!Same(mine_pd, e_pd, w, "post_draws")) { ok = false; why = w; }
      const std::pair<const char*, Counts*> rows[] = {{"dof_layers", &c.dof_layers}, {"slot2_vs_slot6", &c.slot26}, {"slot6_source", &c.slot6_source}};
      for (const auto& r : rows) {
        const Json* e = E->Get(r.first);
        if (!e || !Same(CountsJson(*r.second), *e, w, r.first)) { ok = false; if (why.empty()) why = w.empty() ? std::string(r.first) + ": missing" : w; }
      }
      if (!Same(CountsJson(c.dof_depth), ExpectDofDepth(E->Get("dof_depth")), w, "dof_depth (one per layer)")) { ok = false; if (why.empty()) why = w; }
      Report(ok, c.label, "layers", ok ? "layers per frame, slot 2 vs slot 6 and slot 6's source match the census" : why);
    }
    // ---- combine
    {
      std::vector<std::pair<std::string, Json>> m = {{"combine_variants", CountsJson(c.combine_variants)}, {"combine_slots", CountsJson(c.combine_slots)}, {"combine_call_sites", CountsJson(c.combine_sites)}};
      bool ok = CompareGroup(c, *E, "combine", m);
      (void)ok;
      if (c.site_bad) Report(false, c.label, "combine", std::to_string(c.site_bad) + " chain draws of a named frame are not at the post queue / combine call sites");
    }
    // ---- dead work
    {
      std::vector<std::pair<std::string, Json>> m = {{"dead_work_flags", CountsJson(c.dead_flags)}, {"dead_draws", MinMaxJson(c.dead_draws)}, {"dead_copies", MinMaxJson(c.dead_copies)}};
      CompareGroup(c, *E, "dead_work", m);
      if (c.skip_bad) Report(false, c.label, "dead_work", "the skips do not match the dead work: " + c.first_skip_bad);
      else if (c.skip_checked && !g_quiet) Report(true, c.label, "dead_work", std::to_string(c.skip_checked) + " frames with both strengths zero: the plan skips every dead draw and copy");
    }
    // ---- dof filter
    {
      std::vector<std::pair<std::string, Json>> m = {{"dof_depth", CountsJson(c.dof_depth)}};
      Json ed = ExpectDofDepth(E->Get("dof_depth"));
      std::string w;
      for (auto& kv : m[0].second.o) kv.second.n = 1;  // which filters were read; how many layers read them is the `layers` check's
      for (auto& kv : ed.o) kv.second.n = 1;
      const bool read_ok = Same(m[0].second, ed, w, "dof_depth");
      const bool rule_ok = c.filter_bad == 0;
      Report(read_ok && rule_ok, c.label, "dof_filter", !read_ok ? "the stream's sampler read: " + w : !rule_ok ? std::to_string(c.filter_bad) + " of " + std::to_string(c.filter_checked) + " layers: " + c.first_filter_bad
             : std::to_string(c.filter_checked) + " layers: the plan's rule (point where the frame has a restore, linear where it has none) == the stream's sampler" + (c.filter_checked == 0 ? " (null inputs: image 0, nothing to check)" : ""));
    }
    // ---- constants
    {
      Json mine = ConstantsJson(c.label);
      const Json* e = E->Get("constants");
      std::string w;
      bool ok = e && Same(mine, *e, w, "constants");
      const Json* em = E->Get("monotone_distinct_banks");
      const size_t film = g_film.count(c.label) ? g_film[c.label].size() : 0;
      if (!em || double(film) != em->n) { ok = false; w = "monotone banks: plan " + std::to_string(film) + ", census " + (em ? std::to_string(int(em->n)) : std::string("missing")); }
      Report(ok, c.label, "constants", ok ? "c20 c21 c27 c29 c30 c31 c32 and the film bank read from the stream's draws match (min / max / distinct)" : w.empty() ? "missing" : w);
    }
    // ---- chain end
    {
      const Json* ea = E->Get("after_combine");
      bool ok = c.end_bad == 0;
      std::string why = c.first_end_bad;
      if (ok && ea && ea->Get("chain_after_combine")) {
        const Json* cc = ea->Get("chain_after_combine");
        const bool film_expected = cc->a.size() == 2 && cc->a[0].n >= 1;
        const bool film_plan = c.chain_after_combine_film > 0;
        if (film_expected != film_plan || (film_plan && c.chain_after_combine_film != c.frames)) { ok = false; why = "the film pass after the combine: plan " + std::to_string(c.chain_after_combine_film) + " frames, census " + (film_expected ? "all" : "none"); }
      }
      Report(ok, c.label, "chain_end", ok ? "the chain's last draw is found structurally (the film pass, else the combine) with the vertex hash zeroed" : why);
    }
    // ---- inputs + after_combine
    {
      int expect_null = 0;
      if (const Json* nn = E->Get("null_slots")) for (const auto& kv : nn->o) expect_null += int(kv.second.n);
      std::vector<std::pair<std::string, Json>> m;
      Json af = Json::Object();
      if (c.after.count("all")) af.o["all"] = c.after["all"].ToJson();
      Json eaf = Json::Object();
      if (const Json* ea = E->Get("after_combine")) if (ea->Get("all")) eaf.o["all"] = *ea->Get("all");
      std::string w;
      bool ok = Same(af, eaf, w, "after_combine");
      if (ok && c.null_total != expect_null) { ok = false; w = "null inputs: plan " + std::to_string(c.null_total) + ", census " + std::to_string(expect_null); }
      Report(ok, c.label, "inputs", ok ? "chain slots reading image 0: " + std::to_string(c.null_total) + "; draws left to the stream after the combine match" : w);
    }
    // ---- the chain's copies (the census's "post_copies.all")
    {
      Json mine = Json::Object();
      if (c.post_copies_all.any) mine.o["all"] = c.post_copies_all.ToJson();
      Json e = Json::Object();
      if (const Json* ep = E->Get("post_copies")) if (ep->Get("all")) e.o["all"] = *ep->Get("all");
      std::string w;
      const bool ok = Same(mine, e, w, "post_copies");
      Report(ok, c.label, "copies", ok ? "copies made from the chain's targets match" : w);
    }
  }
  std::printf("%s%s\n", g_bad ? "FAIL: " : "all checks passed", g_bad ? (std::to_string(g_bad) + " check(s)" + (plant.empty() ? "" : "  (plant: " + plant + ")")).c_str() : (plant.empty() ? "" : "  (plant: NONE FAILED)"));
  return g_bad ? 1 : 0;
}
