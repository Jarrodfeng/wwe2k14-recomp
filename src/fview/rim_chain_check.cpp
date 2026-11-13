// Lane 151: fview --rim-chain-check OUT.json --capture DIR [--game DIR] [--frames N] [--wrestlers-ours] [--ring-ours] [--rim-ours] [--replace-fault rimchain.NAME] [--rim-chain-images 222,223,224,225]
// Two sessions on the same capture play it frame by frame in lockstep: A = the stream's reductions and blur (the other classes as given, in both arms), B = the same classes plus the rim chain
// by our own programs (RimChainOurs, --rim-chain-ours semantics). After every frame the chain's images (by default the copies' destinations 222 / 223 / 224 / 225: the rim map's copy, the
// blurred map the wrestler materials read in slot 15, the two coverage maps) and the presented picture are read back from both and compared texel by texel; the json holds counts, never pixels.
// The python driver (tools\rim\rim_chain_check.py) judges. The shipped binder code runs in both arms; only the hook differs.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>

#include "app.h"
#include "../renderer_scene/replace_factories.h"

namespace fview {
namespace {

struct Acc {
  uint64_t frames = 0, shape_fail = 0, frames_differing = 0, texels = 0, differing = 0, over1 = 0, over4 = 0, lit = 0, frames_lit = 0;   // lit: texels of the STREAM's image that hold anything (the compare is not between two empty maps)
  double max_diff = 0, sum_mean = 0, max_mean = 0;
  uint32_t w = 0, h = 0;
};

void Compare(Acc& acc, const std::vector<uint8_t>& a, uint32_t aw, uint32_t ah, const std::vector<uint8_t>& b, uint32_t bw, uint32_t bh) {
  ++acc.frames;
  acc.w = aw, acc.h = ah;
  if (aw != bw || ah != bh || a.size() != b.size()) {
    ++acc.shape_fail;
    return;
  }
  double sum = 0, mx = 0;
  uint64_t differing = 0, over1 = 0, over4 = 0, lit = 0;
  for (size_t i = 0; i + 3 < a.size(); i += 4) {   // a texel counts once, by its largest channel difference
    int worst = 0;
    for (int c = 0; c < 4; ++c) {
      const int d = std::abs(int(a[i + c]) - int(b[i + c]));
      worst = std::max(worst, d);
      sum += d;
    }
    mx = std::max(mx, double(worst));
    lit += (a[i] | a[i + 1] | a[i + 2]) != 0;
    differing += worst > 0, over1 += worst > 1, over4 += worst > 4;
  }
  acc.texels += a.size() / 4;
  acc.differing += differing, acc.over1 += over1, acc.over4 += over4, acc.lit += lit, acc.frames_lit += lit ? 1 : 0;
  acc.frames_differing += differing ? 1 : 0;
  const double mean = a.empty() ? 0 : sum / double(a.size());
  acc.max_diff = std::max(acc.max_diff, mx), acc.max_mean = std::max(acc.max_mean, mean), acc.sum_mean += mean;
}

std::string One(const Acc& a) {
  char t[600];
  std::snprintf(t, sizeof t,
                "{\"frames\": %llu, \"shape_fail\": %llu, \"frames_differing\": %llu, \"texels\": %llu, \"differing\": %llu, \"over1\": %llu, \"over4\": %llu, \"lit\": %llu, \"frames_lit\": %llu, \"max_diff\": %.3f, \"max_mean\": %.5f, \"avg_mean\": %.6f, \"size\": \"%ux%u\"}",
                static_cast<unsigned long long>(a.frames), static_cast<unsigned long long>(a.shape_fail), static_cast<unsigned long long>(a.frames_differing), static_cast<unsigned long long>(a.texels),
                static_cast<unsigned long long>(a.differing), static_cast<unsigned long long>(a.over1), static_cast<unsigned long long>(a.over4), static_cast<unsigned long long>(a.lit), static_cast<unsigned long long>(a.frames_lit), a.max_diff, a.max_mean, a.frames ? a.sum_mean / double(a.frames) : 0.0, a.w, a.h);
  return t;
}

}  // namespace

int RunRimChainCheck(const Options& o, const std::filesystem::path& root) {
  rdr::RendererOptions ro;
  ro.validation = o.validation < 0 ? 0 : o.validation;
  ro.gpu_index = o.gpu;
  ro.render_scale = o.scale;
  Session a, b;
  std::string error;
  if (!a.Open(o.capture, ro, error)) return std::fprintf(stderr, "[fview] open A: %s\n", error.c_str()), 2;
  if (!b.Open(o.capture, ro, error)) return std::fprintf(stderr, "[fview] open B: %s\n", error.c_str()), 2;
  const std::string game = !o.game.empty() ? o.game : (root / "game_data" / "extracted").string();
  rdr_scene::InstallReplaceFactories();
  for (Session* s : {&a, &b}) {   // the other classes, the same in both arms (registration order is call order)
    if (o.wrestlers_ours && !s->EnableReplace("wrestlers", "", game, error)) return std::fprintf(stderr, "[fview] wrestlers: %s\n", error.c_str()), 2;
    if (o.ring_ours && !s->EnableReplace("ring", "", game, error)) return std::fprintf(stderr, "[fview] ring: %s\n", error.c_str()), 2;
    if (o.rim_ours && !s->EnableReplace("rim_map", "", game, error)) return std::fprintf(stderr, "[fview] rim_map: %s\n", error.c_str()), 2;
  }
  if (!o.rim_chain_override.empty()) {   // both arms read the same structured image in the reductions' slot 0
    const size_t colon = o.rim_chain_override.find(':');
    if (colon == std::string::npos) return std::fprintf(stderr, "[fview] --rim-chain-override wants FROM:TO\n"), 2;
    const uint32_t from = uint32_t(std::stoul(o.rim_chain_override.substr(0, colon))), to = uint32_t(std::stoul(o.rim_chain_override.substr(colon + 1)));
    a.SetTestSlotOverride(from, to), b.SetTestSlotOverride(from, to);
  }
  b.SetTestFsBlockCapacity(o.rim_chain_fs_capacity);
  if (!b.SetImageInfoMode(o.image_info, error)) return std::fprintf(stderr, "[fview] --image-info: %s\n", error.c_str()), 2;   // (before the hook is made: its size check reads it)
  if (!b.EnableReplace("rim_chain", o.replace_fault, game, error)) return std::fprintf(stderr, "[fview] rim_chain: %s\n", error.c_str()), 2;
  const uint32_t count = std::min<uint32_t>(a.frame_count(), o.frames > 0 ? uint32_t(o.frames) : a.frame_count());
  std::vector<uint32_t> ids;
  {
    std::stringstream list(o.rim_chain_images.empty() ? std::string("222,223,224,225") : o.rim_chain_images);
    std::string item;
    while (std::getline(list, item, ',')) ids.push_back(uint32_t(std::stoul(item)));
  }
  std::map<uint32_t, Acc> images;
  Acc picture;
  uint64_t compared = 0, no_image = 0;
  std::vector<uint8_t> ia, ib, pa, pb;
  for (uint32_t f = 0; f < count; ++f) {
    if (!a.StepFrame(f, error)) return std::fprintf(stderr, "[fview] A frame %u: %s\n", f, error.c_str()), 2;
    if (!b.StepFrame(f, error)) return std::fprintf(stderr, "[fview] B frame %u: %s\n", f, error.c_str()), 2;
    if (f < o.rim_chain_skip) continue;   // (a few first frames: the chain's maps are built up from nothing; the default compares every frame from 0)
    ++compared;
    for (const uint32_t id : ids) {
      uint32_t aw = 0, ah = 0, bw = 0, bh = 0;
      if (!a.ReadSurface(id, ia, aw, ah) || !b.ReadSurface(id, ib, bw, bh)) {
        ++no_image;
        continue;
      }
      Compare(images[id], ia, aw, ah, ib, bw, bh);
    }
    uint32_t aw = 0, ah = 0, bw = 0, bh = 0;
    if (a.ReadPresented(pa, aw, ah) && b.ReadPresented(pb, bw, bh)) Compare(picture, pa, aw, ah, pb, bw, bh);
  }
  const std::string report = b.ReplaceReport("rim_chain");
  std::printf("%s\n", report.c_str());
  const uint64_t errors = a.validation_errors() + b.validation_errors();
  std::printf("[fview] rim-chain-check: %u frames played, %llu compared, %zu chain images, picture max diff %.3f (%llu frames differ), validation errors %llu\n", count, static_cast<unsigned long long>(compared),
              images.size(), picture.max_diff, static_cast<unsigned long long>(picture.frames_differing), static_cast<unsigned long long>(errors));
  std::string escaped;
  for (const char c : report) escaped += c == '"' ? std::string("'") : c == '\n' ? std::string("\\n") : c == '\\' ? std::string("/") : std::string(1, c);
  std::ostringstream js;
  js << "{\n \"frames\": " << count << ", \"compared\": " << compared << ", \"no_image\": " << no_image << ", \"validation_errors\": " << errors << ",\n \"images\": {";
  bool first = true;
  for (const auto& item : images) js << (first ? "\n  \"" : ",\n  \"") << item.first << "\": " << One(item.second), first = false;
  js << "\n },\n \"picture\": " << One(picture) << ",\n \"report\": \"" << escaped << "\"\n}\n";
  std::filesystem::create_directories(std::filesystem::path(o.rim_chain_check).parent_path());
  std::ofstream(o.rim_chain_check, std::ios::binary) << js.str();
  return errors ? 4 : 0;
}

}  // namespace fview
