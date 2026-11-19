// See frame_hooks.h. Session 103, lane R2.
#include "frame_hooks.h"

#include <algorithm>
#include <vector>

#include "../assets/asset_model.h"
#include "live_assets.h"

namespace rdr_scene {

namespace {
size_t SpriteMipBytes(assets::TexFormat f, uint32_t w, uint32_t h) {
  const size_t blocks = size_t(std::max(1u, (w + 3) / 4)) * std::max(1u, (h + 3) / 4);
  return f == assets::TexFormat::kBC1 ? blocks * 8 : blocks * 16;
}
// A particle sprite (BC1 / BC2 / BC3 DDS, as prtcls.pac stores them) as a sampled image with the mips the file holds. kNoImage for anything else.
rdr::ImageId MakeSpriteImage(rdr::Renderer& r, const assets::Texture& t) {
  const bool bc = t.format == assets::TexFormat::kBC1 || t.format == assets::TexFormat::kBC2 || t.format == assets::TexFormat::kBC3;
  if (!bc || t.width == 0 || t.height == 0 || (t.width % 4) || (t.height % 4) || t.width > 4096 || t.height > 4096) return rdr::kNoImage;
  uint32_t mx = std::max(t.width, t.height), maxmips = 1;
  while (mx > 1) mx >>= 1, ++maxmips;
  const uint32_t want = std::min(std::max(1u, t.mips), maxmips);
  size_t total = 0;
  uint32_t fit = 0;
  for (uint32_t i = 0; i < want; ++i) {
    const size_t sz = SpriteMipBytes(t.format, std::max(1u, t.width >> i), std::max(1u, t.height >> i));
    if (total + sz > t.data.size()) break;
    total += sz, ++fit;
  }
  if (!fit) return rdr::kNoImage;
  rdr::ImageDesc d;
  d.width = uint16_t(t.width), d.height = uint16_t(t.height), d.mips = uint8_t(fit), d.usage = rdr::kUsageSampled;
  d.format = t.format == assets::TexFormat::kBC1 ? rdr::Format::BC1 : t.format == assets::TexFormat::kBC2 ? rdr::Format::BC2 : rdr::Format::BC3;
  const rdr::ImageId id = r.CreateImage(d);
  if (id == rdr::kNoImage) return id;
  size_t at = 0;
  for (uint32_t i = 0; i < fit; ++i) {
    const size_t sz = SpriteMipBytes(t.format, std::max(1u, t.width >> i), std::max(1u, t.height >> i));
    r.UpdateImage(id, i, 0, t.data.data() + at, sz);
    at += sz;
  }
  return id;
}
}  // namespace

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
  const auto store = ParticleSpritesNow();
  if (!store) return rdr::kNoImage;
  rdr::ImageId id = rdr::kNoImage;
  const auto t = store->find(entry);
  if (t != store->end()) id = MakeSpriteImage(renderer_, t->second);
  (id == rdr::kNoImage ? particle_failed_ : particle_loaded_) += 1;
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
    h.particles_->SetFrame(uint32_t(rec.header.guest_frame));
    frame.particles = rec.particles.empty() ? nullptr : &rec.particles;
    // S103 F5: a batch's Texture asset (pac\prtcls.pac + "pBBBB/EEE/1/Name": the namer's result) -> the sprite image; any other asset is not a particle texture (white stand-in, counted)
    // (the lookup keeps only COPIES of the record's texture names: it stays installed after this frame and `rec` must not be touched then; `h` owns the hook, so it outlives the lookup)
    auto names = std::make_shared<std::vector<std::pair<std::string, std::string>>>(rec.assets.size());
    for (size_t i = 0; i < rec.assets.size(); ++i)
      if (rec.assets[i].kind == uint8_t(AssetKind::Texture)) (*names)[i] = {std::string(rec.Str(rec.assets[i].pac)), std::string(rec.Str(rec.assets[i].entry))};
    h.particles_->SetTextureLookup([&h, names](uint32_t asset) -> rdr::ImageId {
      if (asset >= names->size() || (*names)[asset].first.empty() || (*names)[asset].second.empty()) return rdr::kNoImage;
      return h.ParticleImage((*names)[asset].first, (*names)[asset].second);
    });
    frame.stage_hooks.push_back(h.particles_.get());
  }
}

bool FrameHooks::ShadowInputHole(const SceneTables& rec, std::string& why) {
  if (!maps_) return false;
  BuildMapsInput(rec, options_.maps_faults, nullptr, scratch_, options_.maps_fit);
  if (options_.maps_input_patch) options_.maps_input_patch(scratch_);
  if (scratch_.shadow_hole) why = scratch_.fit_key_why;
  return scratch_.shadow_hole;
}

std::string FrameHooks::CensusLine() const {
  std::string s = std::string("[scene][hooks] order ") + (options_.order == SeeOrder::Game ? "game" : "distance") + ", frames " + std::to_string(frames_);
  if (maps_) s += "; " + maps_->CensusLine();
  if (particles_) s += "; " + particles_->CensusLine() + ", sprite images " + std::to_string((unsigned long long)particle_loaded_) + " (failed " + std::to_string((unsigned long long)particle_failed_) + ")";
  return s;
}

}  // namespace rdr_scene
