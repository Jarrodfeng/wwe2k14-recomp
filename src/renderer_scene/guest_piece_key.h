// S111 GUESTKEY (docs\research\s111\GUESTKEY.md): the piece id a guest-built model stamps in its meshes' src_key, from the record object it is built for. Pure: no renderer, no game.
// The record carries a SUBM piece's id in Object::pad under the three piece flags (scene_record.h: kObjDrawnByOwner / kObjDetachedPiece / kObjOwnRowsPiece); pad means something else for any other object
// (a GPU-skinned ring piece: its instance + 1), so the flags say whether pad is a piece id. 0 = a body (the pac's entry 0: the stock key of a body mesh). The game draws every SUBM piece by its instance id
// (sub_8259E488, the u16 at instance +16) whatever built the model; ours does once the key is the id (piece_draw_rule.h PieceCut, piece_row_rule.h, wrk_join.h JoinsBodyLevels).
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "scene_record.h"

namespace rdr_scene {

// unit plants (guest_model_unit): 1 (--plant 68) = the S111 CAWHAIR mask (kObjOwnRowsPiece | kObjDetachedPiece only): a kObjDrawnByOwner piece routed to its guest rows gets key 0 (never cut);
// 2 (--plant 69) = the piece id as a SUFFIX of the cache key ("guest:<hash>:p<id>": the body key of the same hash is then a prefix and a substring of it, FormatGuestModelKey)
inline int g_guest_key_plant = 0;

inline int GuestPieceKey(uint32_t flags, uint16_t pad) {
  const uint32_t piece = g_guest_key_plant == 1 ? uint32_t(kObjOwnRowsPiece | kObjDetachedPiece) : uint32_t(kObjOwnRowsPiece | kObjDetachedPiece | kObjDrawnByOwner);
  return (flags & piece) ? int(pad) : 0;
}

// S111 GUESTKEY review: the ModelCache key of a guest model (rf_wrestlers.cpp GuestModelFor): "guest:<hash>" for a body (piece_key 0), "guest:p<id>:<hash>" for a piece model. ModelCache::RetireOther
// drops a key by PREFIX and keeps one by SUBSTRING (record_frame.cpp), and the 16-key retire's TrimGuestImages then releases every picture the kept model does not use, so no key may be a prefix or a substring
// of another: one piece object seen unlinked (key 0) on one frame and linked (its id) on the next builds two models of ONE content hash, and with the id as a suffix the body key kept the piece model alive
// through the retire while its pictures were released (and a rebuild of the body key retired the piece model). `n` >= 32 holds every key ("guest:p65535:" + 16 hex + NUL = 30).
inline void FormatGuestModelKey(char* key, size_t n, uint64_t model_hash, int piece_key) {
  if (piece_key > 0 && g_guest_key_plant == 2) std::snprintf(key, n, "guest:%016llx:p%d", (unsigned long long)model_hash, piece_key);
  else if (piece_key > 0) std::snprintf(key, n, "guest:p%d:%016llx", piece_key, (unsigned long long)model_hash);
  else std::snprintf(key, n, "guest:%016llx", (unsigned long long)model_hash);
}

}  // namespace rdr_scene
