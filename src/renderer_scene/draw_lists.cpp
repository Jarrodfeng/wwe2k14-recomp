// The kept draw lists (Session 103, lane G; docs\renderer\SCENE_PACING.md 3b). See draw_lists.h.
#include "draw_lists.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

namespace rdr_scene {

namespace {
struct NameMask {
  const char* name;
  uint32_t mask;
};
// What each constant field the two resolvers (front_end.cpp Resolve, char_effects.cpp ResolveChar) write depends on. Read from those functions:
// a field is listed only after its resolver was read; a name that is missing here is kStAll (rewritten on any change: slower, never wrong).
// A lane that makes a "written once" field depend on the record (lane B's wrestler values, lane E's maps: ShadowPrm, Sweat, Blood...) MUST change its mask here
// (the unit's table check lists every 0 entry); the rsc_draw `--lists` byte-equal check catches a miss on the captures.
constexpr uint32_t kWP = kStWorld | kStPalette;
const NameMask kTable[] = {
    // camera / world / time
    {"ViewProj", kStCamera}, {"CameraViewProj", kStCamera}, {"EyePos", kStCamera},
    {"WorldMatrix", kWP}, {"WorldMatrixArray", kWP /* (the mirror rule is per record: it is in the world stamp) */},
    {"Time", kStTime},
    {"Bools", kStPalette | kStMaterial},  // the bone count picks the skinning bits
    // the light set
    {"SkyDir", kStLight}, {"SkyCol", kStLight}, {"GroundCol", kStLight}, {"KeyLightColor", kStLight}, {"KeyLightVec", kStLight | kStCamera},   // S103 L7: the key light is camera-attached (the rotation reads the camera)
    {"DifLghMax", kStLight}, {"DifLghMaxA", kStLight},
    {"DifLghCol", kStLight | kStMaterial}, {"DifLghVec", kStLight | kStMaterial | kStCamera},   // S103 L7: [4] = the camera-attached key light
    // the material and the object's tint
    {"MatAmbCol", kStMaterial}, {"MatDifCol", kStMaterial}, {"MdlAlpha", kStMaterial}, {"LightMapParam", kStMaterial},
    {"SpecularCol", kStLight | kStMaterial}, {"SpecularLev", kStMaterial}, {"SpecularPow", kStMaterial}, {"iSpecularPow", kStMaterial},
    {"Param0", kStMaterial}, {"NoiseLodParam", kStMaterial}, {"LightLimit", kStMaterial}, {"fvLightLimit", kStMaterial},
    {"SSS", kStMaterial}, {"SSSSinCol2", kStMaterial}, {"Reflect", kStMaterial}, {"SpecCubPrm1", kStMaterial},
    {"NoiseTile", kStMaterial}, {"NoiseTileF", kStMaterial},
    // constants of the effect (written once)
    {"FogDistance", 0}, {"NormalFlip", 0}, {"RimLightDepth", kStLight}, {"RimLightParam", 0}, {"RimLightParam3", kStMaterial}, {"HdLig", kStMaterial /* S111 MATFEED: g_fHdLigLvl / St per material, the live row stamped (front_end.cpp) */}, {"SpecCubPrm3", kStMaterial /* S111 SPECCOL: g_f4SpecCol per material */},
    {"View", kStCamera /* S111 VIEWROWS: the camera's view rows (char_effects.cpp ResolveChar, CameraAxes) */}, {"MuscleParam", kStMaterial /* S111 MATFEED: per material, the live row stamped */}, {"MuscleUV", kStMaterial /* S111 MATFEED */}, {"WrinkUV", kStMaterial}, {"vMin", 0}, {"fvMin", 0}, {"LightMin", 0}, {"UVScroll0", kStMaterial}, {"UVScroll1", kStMaterial},
    {"MipmapBias", 0}, {"Level", kStMaterial /* S111 UNIFY5: c201.w = the material's g_f4TexBis_Dif */},
    {"FogColor", 0}, {"ChangeColor", kStMaterial /* S108 THR2: a change-colour draw's colours (hashed into the material stamp, front_end.cpp) */}, {"Blood", kStMaterial}, {"BSSBldClr", kStMaterial}, {"Sweat", kStMaterial},   /* S103 SWF: the wet variant reads the material's own wet constants (like Param0): per material, never written once */ {"DebugPS0", 0}, {"DebugPS1", 0},
    {"ShadowPrm", kStLight}, {"ViewLightDepth", kStLight}, {"TexProjLightXY", kStLight}, {"TexProjLightW", kStLight},   // S103 L6: the shadow reader's rows follow the frame's shadow map (hashed into the light stamp, front_end.cpp)
    {"Param1", kStMaterial}, {"ConvScreen", 0}, {"HairSpeParam", kStMaterial}, {"SpeCol1", kStMaterial},
    {"SpeCol2", kStMaterial}, {"SSColor", kStMaterial /* S111 MATFEED */}, {"SS", kStMaterial /* S111 MATFEED */}, {"SphereOccP", kStPalette}, {"SphereOccV", kStPalette /* S111 SPHEREOCC: the body's recorded spheres, hashed into the palette stamp (front_end.cpp) */}, {"IdMapCol", kStMaterial}, {"IdMapLvLst", kStMaterial}, {"WklLvLst", kStMaterial},
    {"ReflectAlpha", kStMaterial /* S103 RFL: the draw's own g_fReflectAlpha (reflect_alpha per material) */}, {"Noise", kStMaterial}, {"NoiseParam", kStMaterial}, {"PlaneInv", kStLight /* S103 L9: the ring mat reads the plane map; hashed into the light stamp */}, {"TexProjLight", kStLight}, {"ViewLight", kStLight}, {"SpeLodParam", kStMaterial},   // S103 D1: the stage FX_SHADOW receiver's rows follow the frame's shadow map (hashed into the light stamp, front_end.cpp)
};
}  // namespace

static std::unordered_map<std::string, uint32_t>& MaskMap() {
  static std::unordered_map<std::string, uint32_t> m = [] {
    std::unordered_map<std::string, uint32_t> r;
    for (const NameMask& e : kTable) r.emplace(e.name, e.mask);
    return r;
  }();
  return m;
}
void PlantFieldMask(const char* name, uint32_t mask) { MaskMap()[name] = mask; }

uint32_t FieldMask(const char* name) {
  const std::unordered_map<std::string, uint32_t>& map = MaskMap();
  if (!name) return kStAll;
  const auto it = map.find(name);
  return it == map.end() ? uint32_t(kStAll) : it->second;
}

KeptDraw& DrawLists::Acquire(const DrawKey& key, bool& fresh) {
  Object& o = objects_[ObjectKey(key)];
  o.seen = frame_;
  auto ins = o.draws.try_emplace(DrawKeyBits(key));
  fresh = ins.second;
  ++(fresh ? counts_.built : counts_.reused);
  if (!fresh && ins.first->second.record_seq == record_seq_) ++counts_.dup_keys;
  ins.first->second.last_frame = frame_;
  ins.first->second.record_seq = record_seq_;
  return ins.first->second;
}

void DrawLists::ReleaseAsset(uint32_t asset) {
  for (auto it = objects_.begin(); it != objects_.end();) {
    if (uint32_t(it->first & 0xFFFFFFFFu) == asset) {
      counts_.freed_draws += it->second.draws.size(), ++counts_.freed_objects;
      it = objects_.erase(it);
    } else {
      ++it;
    }
  }
}

void DrawLists::EndFrame() {
  if (!plant_nofree_) {
    for (auto it = objects_.begin(); it != objects_.end();) {
      Object& o = it->second;
      for (auto d = o.draws.begin(); d != o.draws.end();) {
        if (frame_ > d->second.last_frame + grace_) d = o.draws.erase(d), ++counts_.freed_draws;
        else ++d;
      }
      if (o.draws.empty()) it = objects_.erase(it), ++counts_.freed_objects;
      else ++it;
    }
  }
  uint64_t n = 0;
  for (const auto& kv : objects_) n += kv.second.draws.size();
  counts_.live_objects = objects_.size(), counts_.live_draws = n;
}

std::string DrawLists::Census() const {
  char line[400];
  const uint64_t total = counts_.built + counts_.reused;
  std::snprintf(line, sizeof line,
                "[scene][lists] frames %llu, live objects %llu draws %llu, built %llu, reused %llu (%.1f%%), blocks clean %llu partial %llu full %llu, fields resolved %llu, freed draws %llu objects %llu, duplicate keys %llu",
                (unsigned long long)counts_.frames, (unsigned long long)counts_.live_objects, (unsigned long long)counts_.live_draws, (unsigned long long)counts_.built,
                (unsigned long long)counts_.reused, total ? 100.0 * double(counts_.reused) / double(total) : 0.0, (unsigned long long)counts_.blocks_clean,
                (unsigned long long)counts_.blocks_partial, (unsigned long long)counts_.blocks_full, (unsigned long long)counts_.fields_resolved,
                (unsigned long long)counts_.freed_draws, (unsigned long long)counts_.freed_objects, (unsigned long long)counts_.dup_keys);
  return line;
}

}  // namespace rdr_scene
