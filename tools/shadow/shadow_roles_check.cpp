// S101 lane 86 (SH0): offline check of src\renderer_adapter\shadow_roles.h on captures. Builds the SHIPPED header (the binder's IsCaster / Sets) and plays every
// frame of each capture through it, next to the old shader-hash lists (LegacyCaster / LegacyChainNot, the reference):
//   casters   every draw: the origin / role arm (with the draw's shader hashes zeroed, so it cannot be reading them) == the hash arm
//   targets   after every frame the half-rate maps' skip set (targets with casters, minus targets other draws also go into) is the same set in both arms
//   rim       the rim light map's draws (origin pass site 0x82BFBD58) are never casters, and their targets are never in the skip set
//   squares   caster targets are square, rim targets are 512x256 (the images the two chains draw into never overlap)
//   roles     the shipped creator table gives the rim map's creator (0x827EA4F0) the rim_map role and keeps the shadow maps' creators as shadow_map
//   names     the captures carry draw origins (a version-1 capture cannot show this check anything)
// Plants (-DPLANT or --plant): hash_only (the role arm classifies by hash, with the hashes zeroed -> empty), rim_as_shadow (the rim light map counted as a shadow).
//   shadow_roles_check [--plant hash_only|rim_as_shadow] [-q] NAME=CAPTURE_FOLDER...      exit 0 all PASS, 1 any FAIL, 2 usage
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include "renderer_adapter/capture_reader.h"
#include "renderer_adapter/shadow_roles.h"
#include "renderer_adapter/surface_role_table.h"

using namespace rdr_adapter;
namespace sr = rdr_adapter::shadow_roles;

struct Tally {
  uint64_t draws = 0, unnamed = 0, casters_hash = 0, casters_role = 0, mismatches = 0;
  uint64_t by_map = 0, by_plane = 0, by_circle = 0, rim_draws = 0, rim_as_caster = 0;
  uint64_t frames = 0, set_mismatch_frames = 0, rim_in_skip = 0, nonsquare_caster_targets = 0, nonrim_shape_rim_targets = 0;
  uint64_t no_origin_frames = 0;
  uint32_t first_bad_frame = ~0u;
};

static int g_bad = 0;
static bool g_quiet = false;
static void Report(bool ok, const std::string& name, const std::string& detail) {
  if (!ok) ++g_bad;
  if (!ok || !g_quiet) std::printf("%s  %s: %s\n", ok ? "PASS" : "FAIL", name.c_str(), detail.c_str());
}

int main(int argc, char** argv) {
  std::string plant;
  std::vector<std::pair<std::string, std::string>> caps;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--plant" && i + 1 < argc) plant = argv[++i];
    else if (a == "-q") g_quiet = true;
    else if (a.find('=') != std::string::npos) caps.emplace_back(a.substr(0, a.find('=')), a.substr(a.find('=') + 1));
    else {
      std::printf("usage: shadow_roles_check [--plant hash_only|rim_as_shadow] [-q] NAME=CAPTURE_FOLDER...\n");
      return 2;
    }
  }
  if (caps.empty()) {
    std::printf("FAIL  input: no captures given (a check that reads nothing must fail)\n");
    return 1;
  }
  if (plant == "hash_only") sr::g_hash_keying = true;
  else if (plant == "rim_as_shadow") sr::g_rim_as_shadow = true;
  else if (!plant.empty()) {
    std::printf("unknown plant %s\n", plant.c_str());
    return 2;
  }

  // roles: the shipped creator table (the rim light map's creator is its own role; the shadow maps' creators stay shadow maps)
  {
    namespace ro = rdr_adapter::roles;
    const ro::CreatorRow* rim = ro::FindCreator(0x827EA5FCu);   // inside sub_827EA4F0 (the first of its three creates: A0-2.md, render_targets.md)
    const ro::CreatorRow* maps = ro::FindCreator(0x827E8E00u);  // inside sub_827E8CC8 (props map, map A, map B)
    const ro::CreatorRow* plane = ro::FindCreator(0x827EE5C0u); // the ring plane mask
    Report(rim && rim->role == SurfaceRole::RimMap && maps && maps->role == SurfaceRole::ShadowMap && plane && plane->role == SurfaceRole::ShadowMap, "roles",
           std::string("creator 0x827EA4F0 -> ") + (rim ? SurfaceRoleName(rim->role) : "none") + ", 0x827E8CC8 -> " + (maps ? SurfaceRoleName(maps->role) : "none") +
               ", 0x827EE5C0 -> " + (plane ? SurfaceRoleName(plane->role) : "none"));
  }

  for (const auto& [name, folder] : caps) {
    CaptureReader reader;
    if (!reader.Open(folder)) {
      Report(false, name + ".input", "cannot open capture " + folder);
      continue;
    }
    Tally t;
    std::unordered_set<uint32_t> hash_targets, hash_excluded;  // the old binder's learning, verbatim
    sr::Sets role_sets;
    std::set<uint32_t> rim_targets, caster_targets;
    for (uint32_t f = 0; f < reader.frame_count(); ++f) {
      LoadedFrame frame;
      if (!reader.LoadFrame(f, frame)) {
        Report(false, name + ".input", "cannot read frame " + std::to_string(f));
        break;
      }
      ++t.frames;
      if (frame.origins.count == 0) ++t.no_origin_frames;
      uint32_t current = 0;
      for (const Op& op : frame.ops) {
        if (op.kind == OpKind::SetTargets) {
          const rdr::TargetsOp& targets = frame.targets[op.index];
          current = targets.color_count ? targets.color[0].image : targets.depth.image;
          continue;
        }
        if (op.kind != OpKind::Draw) continue;
        const Draw& d = frame.draws[op.index];
        const DrawOrigin* o = frame.OriginOf(op.index);
        const ImageRecord* image = reader.FindImage(current);
        const uint8_t role = image ? image->role : 0;
        ++t.draws;
        if (!sr::Named(o)) ++t.unnamed;
        const bool hash_caster = sr::LegacyCaster(d.ps_hash, d.vs_patched_hash);
        const bool role_caster = sr::IsCaster(o, 0, 0, role);  // the shader hashes are zero: this arm cannot read them
        if (current) {
          if (hash_caster) hash_targets.insert(current);
          else if (sr::LegacyChainNot(d.ps_hash)) hash_excluded.insert(current);
          role_sets.Observe(o, 0, 0, current, role);
        }
        t.casters_hash += hash_caster, t.casters_role += role_caster;
        if (hash_caster != role_caster) {
          ++t.mismatches;
          if (t.first_bad_frame == ~0u) t.first_bad_frame = f;
        }
        if (sr::Named(o) && role_caster) {
          t.by_map += o->pass_site == sr::kMapPassSite, t.by_plane += o->pass_site == sr::kPlanePassSite, t.by_circle += o->call_site == sr::kCircleCallSite;
          if (image && image->width != image->height) ++t.nonsquare_caster_targets;
          caster_targets.insert(current);
        }
        if (sr::Named(o) && o->pass_site == sr::kRimPassSite) {
          ++t.rim_draws;
          t.rim_as_caster += role_caster;
          rim_targets.insert(current);
          if (!image || image->width != 512 || image->height != 256) ++t.nonrim_shape_rim_targets;
        }
      }
      bool same = true;
      for (uint32_t id : hash_targets)
        if ((hash_excluded.count(id) == 0) != role_sets.Skips(id)) same = false;
      for (uint32_t id : role_sets.targets)
        if (role_sets.Skips(id) && (hash_targets.count(id) == 0 || hash_excluded.count(id) != 0)) same = false;
      if (!same) {
        ++t.set_mismatch_frames;
        if (t.first_bad_frame == ~0u) t.first_bad_frame = f;
      }
      for (uint32_t id : rim_targets)
        if (role_sets.Skips(id)) ++t.rim_in_skip;
      for (const ImageRecord& im : reader.images())
        if (sr::IsRimRole(im.role) && role_sets.Skips(im.id)) ++t.rim_in_skip;
    }
    char line[512];
    std::snprintf(line, sizeof line, "%llu frames, %llu draws; casters hash %llu = role %llu (map %llu, plane %llu, circle %llu); mismatches %llu (first frame %d)",
                  (unsigned long long)t.frames, (unsigned long long)t.draws, (unsigned long long)t.casters_hash, (unsigned long long)t.casters_role,
                  (unsigned long long)t.by_map, (unsigned long long)t.by_plane, (unsigned long long)t.by_circle, (unsigned long long)t.mismatches,
                  t.first_bad_frame == ~0u ? -1 : int(t.first_bad_frame));
    Report(t.mismatches == 0 && t.casters_hash > 0 && t.by_map > 0, name + ".casters", line);
    std::snprintf(line, sizeof line, "skip set (targets %zu, excluded %zu) differs from the hash arm's on %llu of %llu frames", role_sets.targets.size(),
                  role_sets.excluded.size(), (unsigned long long)t.set_mismatch_frames, (unsigned long long)t.frames);
    Report(t.set_mismatch_frames == 0 && !role_sets.targets.empty(), name + ".targets", line);
    std::snprintf(line, sizeof line, "%llu rim draws into %zu targets; counted as casters %llu, rim targets in the skip set %llu", (unsigned long long)t.rim_draws,
                  rim_targets.size(), (unsigned long long)t.rim_as_caster, (unsigned long long)t.rim_in_skip);
    Report(t.rim_draws > 0 && t.rim_as_caster == 0 && t.rim_in_skip == 0, name + ".rim", line);
    std::snprintf(line, sizeof line, "caster targets not square %llu, rim draws not into a 512x256 target %llu", (unsigned long long)t.nonsquare_caster_targets,
                  (unsigned long long)t.nonrim_shape_rim_targets);
    Report(t.nonsquare_caster_targets == 0 && t.nonrim_shape_rim_targets == 0, name + ".squares", line);
    std::snprintf(line, sizeof line, "%llu of %llu draws unnamed, %llu of %llu frames without origins", (unsigned long long)t.unnamed, (unsigned long long)t.draws,
                  (unsigned long long)t.no_origin_frames, (unsigned long long)t.frames);
    Report(t.no_origin_frames == 0 && t.unnamed * 1000 < t.draws, name + ".names", line);
  }
  std::printf("%d problems\n", g_bad);
  return g_bad ? 1 : 0;
}
