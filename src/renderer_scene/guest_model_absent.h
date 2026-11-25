// S110 STOREABSENT (docs\research\s110\STOREABSENT.md): the provisional-build gate of the guest-model cache. Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Header only, pure (the unit builds it).
// A guest-model build made while a picture its rows NAME (sampler state bound / override, a hash) has no bytes in reach (no row in the record, or in neither the cache's Store nor the writer's process
// ByteStore) draws that part with the white stand-in. Its model key (geometry + every sampler's hash and state: guest_model_emit.h TexSignature) does not say "the bytes were absent", so the key alone
// would keep it for good. Such a build is PROVISIONAL: GuestModelFor (rf_wrestlers.cpp) draws it and keeps it, notes it here (NoteBuild), and before reusing a kept build asks KeptBuildStands, which
// says "rebuild" only when one of the absent pictures is now in reach: the event that changes the build, never a timer, and a permanently absent picture never rebuilds.
// Cost: a whole build leaves no entry, so a normal match pays one empty() test per call; a provisional key pays one lookup per absent picture per call until a picture arrives.
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "guest_model_draw.h"

namespace rdr_scene::guestmodel {

// The picture `h` is within reach of a build from record `t`: the record has its row and a store holds bytes of the row's size (BuildModel's own test).
inline bool PictureInReach(const SceneTables& t, Store& store, uint64_t h) {
  const GuestModelTexture* tr = t.guest_models.FindTexture(h);
  if (!tr) return false;
  const Store::Bytes b = store.Tex(h);
  return b && b->size() == tr->nbytes;
}

// After a build kept under `key`: a whole build forgets the key, a provisional one remembers the pictures it lacked.
inline void NoteBuild(CacheState& s, const char* key, const BuildStats& st) {
  if (!st.absent.empty()) s.absent_of[std::string(key)] = st.absent;
  else if (!s.absent_of.empty()) s.absent_of.erase(std::string(key));
}

// Before reusing the build kept under `key`: true = it stands; false = rebuild it now (it was provisional and a picture it lacked is now in reach).
// Test only: g_draw_plant 15 (unit 61) = a provisional build is final (the old rule), 16 (unit 62) = a provisional build is rebuilt every call.
inline bool KeptBuildStands(CacheState& s, const SceneTables& t, const char* key) {
  if (s.absent_of.empty()) return true;   // the normal case: nothing provisional, no lookup
  const auto it = s.absent_of.find(std::string(key));
  if (it == s.absent_of.end()) return true;
  if (g_draw_plant == 15) return true;
  if (g_draw_plant == 16) return false;
  for (uint64_t h : it->second)
    if (PictureInReach(t, s.store, h)) return false;
  return true;
}

}  // namespace rdr_scene::guestmodel
