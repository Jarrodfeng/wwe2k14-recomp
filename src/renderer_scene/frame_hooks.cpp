// See frame_hooks.h. Session 103, lane R2.
#include "frame_hooks.h"

#include <algorithm>
#include <vector>

#include "../assets/asset_model.h"
#include "../renderer/problem.h"   // S108 PRT: rdr::Log (the guest-sprite line)
#include "live_assets.h"
#include "particle_sprite_lookup.h"   // S108 PRT: MakeSpriteImage + the name -> sprite resolve (file map or the guest store)

namespace rdr_scene {

FrameHooks::~FrameHooks() {
  for (auto& kv : particle_images_)
    if (kv.second != rdr::kNoImage) renderer_.DestroyImage(kv.second);
}

rdr::ImageId FrameHooks::ParticleImage(const std::string& pac, const std::string& entry) {
  // S103 F5: render thread: NO loading. The textures were decoded on the namer's worker (live_assets.h BuildParticleSprites) into an immutable map; here only an image is made / updated from it.
  // Until the map is published the white stand-in stands (not cached: asked again next frame).
  const std::string key = pac + "|" + entry;
  const auto it = particle_images_.find(key);
  if (it != particle_images_.end()) return it->second;
  const bool guest_pac = pac == live::particles::kGuestSpritePac;
  const auto store = guest_pac ? nullptr : ParticleSpritesNow();
  if (!guest_pac && !store) return rdr::kNoImage;
  bool guest = false;
  const auto t = ParticleSpriteFor(pac, entry, store, live::particles::GuestSprites::Process(), guest);   // S108 PRT: a guest name -> the picture the namer read from the game's own texture
  if (!t && guest) {   // not cached: asked again next frame. LOUD, once per key: in a record replayed offline this is expected (the bytes live only in the live game), live it is a fault
    ++particle_guest_missing_;
    if (guest_told_.size() < 16 && guest_told_.insert(entry).second)   // bounded: 16 keys named, the rest counted (guest missing)
      rdr::Log("[scene][particles] guest sprite %s is not in this process: its bytes exist only in the live game that read them (a record replayed offline cannot draw it); its batches are skipped, the picture is not a renderer fault", entry.c_str());
    return rdr::kNoImage;
  }
  const rdr::ImageId id = t ? MakeSpriteImage(renderer_, *t) : rdr::kNoImage;
  (id == rdr::kNoImage ? particle_failed_ : particle_loaded_) += 1;
  if (guest && id != rdr::kNoImage) ++particle_guest_loaded_;
  particle_images_[key] = id;
  return id;
}

FrameHooks::FrameHooks(rdr::Renderer& renderer, rf::ModelCache& cache, const FrameHooksOptions& options) : options_(options), renderer_(renderer), source_(cache.source()) {
  if (options_.maps) {
    cache.KeepWrestlerCpu(true);
    rf::ModelCache* c = &cache;
    maps_ = std::make_unique<LiveMapsHook>(renderer, [c](uint32_t asset) -> const assets::Model* { return c->CpuModel(int(asset)); });
    maps_->SetGuestPredicate([c](uint32_t asset) { return c->IsGuestCaster(int(asset)); });   // S103 CS1B
    maps_->SetFaults(options_.maps_faults);   // S103 E3
    maps_->SetPropProvider([c](uint32_t asset, uint32_t& serial) -> const assets::Model* { return c->PropCpuModel(int(asset), serial); });   // S103 MBC
  }
  // particle textures: the lookup is set per frame in AttachFrameHooks (it reads the record's asset table); until then the hook's white stand-in is used and counted as no_texture
  if (options_.particles) {
    particles_ = std::make_unique<LiveParticles>(renderer, [](uint32_t) { return rdr::kNoImage; });
    particles_->SetSkipUnnamed(options_.particles_skip_unnamed);
  }
}

void AttachFrameHooks(SceneFrame& frame, const SceneTables& rec, FrameHooks& h) {
  ++h.frames_;
  frame.see_order = h.options_.order;
  if (h.maps_) {
    BuildMapsInput(rec, h.options_.maps_faults, nullptr, h.input_, h.options_.maps_fit);   // S103 E3: faults and the fit options (default = today)
    if (h.options_.maps_input_patch) h.options_.maps_input_patch(h.input_);   // S103 L9
    h.maps_->SetInput(&h.input_, h.options_.maps_input_repeatable);   // S103 E3
    frame.maps = &h.maps_->maps();   // set BEFORE the Maps stage runs, as the front-end sees it (the hook refills the same LiveMaps)
    frame.stage_hooks.push_back(h.maps_.get());
  }
  if (h.scene2d_) {   // S103 SCS: the game draws the gm controller's flashes / lens flares (category 36) before the particle node (47): after the maps, before the particles
    h.scene2d_->SetRows(rec);
    frame.stage_hooks.push_back(h.scene2d_.get());
  }
  if (h.particles_) {
    h.AttachParticles(rec);   // S111 TITLESMOKE: the stamp + lookup, shared with the flat frame's draw
    frame.particles = rec.particles.empty() ? nullptr : &rec.particles;
    frame.stage_hooks.push_back(h.particles_.get());
  }
}

void FrameHooks::AttachParticles(const SceneTables& rec) {
  if (!particles_) return;
  particles_->SetFrame(uint32_t(rec.header.guest_frame));
  // S103 F5: a batch's Texture asset (pac\prtcls.pac + "pBBBB/EEE/1/Name": the namer's result) -> the sprite image; any other asset is not a particle texture (white stand-in, counted)
  // (the lookup keeps only COPIES of the record's texture names: it stays installed after this frame and `rec` must not be touched then; this FrameHooks owns the hook, so it outlives the lookup)
  auto names = std::make_shared<std::vector<std::pair<std::string, std::string>>>(rec.assets.size());
  for (size_t i = 0; i < rec.assets.size(); ++i)
    if (rec.assets[i].kind == uint8_t(AssetKind::Texture)) (*names)[i] = {std::string(rec.Str(rec.assets[i].pac)), std::string(rec.Str(rec.assets[i].entry))};
  particles_->SetTextureLookup([this, names](uint32_t asset) -> rdr::ImageId {
    if (asset >= names->size() || (*names)[asset].first.empty() || (*names)[asset].second.empty()) return rdr::kNoImage;
    return ParticleImage((*names)[asset].first, (*names)[asset].second);
  });
}

std::string FrameHooks::CensusLine() const {
  std::string s = std::string("[scene][hooks] order ") + (options_.order == SeeOrder::Game ? "game" : "distance") + ", frames " + std::to_string(frames_);
  if (maps_) s += "; " + maps_->CensusLine();
  if (particles_) s += "; " + particles_->CensusLine() + ", sprite images " + std::to_string((unsigned long long)particle_loaded_) + " (failed " + std::to_string((unsigned long long)particle_failed_) + ", from the game's texture " + std::to_string((unsigned long long)particle_guest_loaded_) + ", guest missing " + std::to_string((unsigned long long)particle_guest_missing_) + ")";
  return s;
}

}  // namespace rdr_scene
