// Assets by name from the player's own game folder, for the record -> frame path (Session 103, lane S; FRAME_CONTRACT.md 7).
// The SAME loaders offline (the rig, rsc_draw) and live (the game exe): asset_load.h (header-only, already in the game exe) and
// assets::crowd_body. No viewer, no game memory. Thread: Load() may run on any thread; one instance is not shared between threads at once.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

#include "../assets/asset_model.h"
#include "../scene_live/particle_namer.h"

#include "record_frame.h"

namespace rdr_scene {

class GameFilesSource : public rf::AssetSource {
 public:
  GameFilesSource();
  ~GameFilesSource() override;
  // The folder with default.xex (the player's game, or game_data\extracted). False when it holds no game files.
  bool SetRoot(const std::string& game_root);
  bool ready() const { return ready_; }
  // pac\bg\bgNN.pac (entry empty: the arena; entry 20000 + 40 * LOD + type or "card:TT": a crowd person), pac\ch\chNNN.pac (variant = the
  // attire), pac\evt\evtobj.pac (entry = the prop's PACH id). Anything else: false with the reason.
  bool Load(const std::string& pac, const std::string& entry, uint32_t variant, assets::Model& out, std::string& error) override;
 private:
  struct Impl;
  Impl* impl_ = nullptr;
  bool ready_ = false;
};

// S103 F5: the particle sprite textures, decoded ONCE off the render thread. BuildParticleSprites opens the player's own pac\prtcls.pac, feeds every sprite directory to `index` (the namer's
// NameIndex, particle_namer.h), decodes each DISTINCT texture the index holds (entry "pBBBB/EEE/1/Name" -> the DDS, assets\particle_sprites.h) into an immutable map, publishes it
// (PublishParticleSprites) and lets the opened pac go (freed on return). Any thread (own file reader, no shared pac cache, no load lock: the render thread never runs this). The live game runs it on
// the namer's worker; the offline rig runs it once at start.
using ParticleSpriteMap = std::unordered_map<std::string, assets::Texture>;
bool BuildParticleSprites(const std::string& game_root, rdr_scene::live::particles::NameIndex& index, std::string& error);
std::shared_ptr<const ParticleSpriteMap> ParticleSpritesNow();   // null until built; the render thread only reads this, then creates images

}  // namespace rdr_scene
