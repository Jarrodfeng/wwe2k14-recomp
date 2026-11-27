// S111 BLOODMOD2 (docs\research\s111\BLOODMOD2.md): the identity of each blood splat row, for the permanent blood mod's store (src\renderer_scene\floor_blood_keep.h floor_blood::Apply). Header only, std + the row
// contract: the tracker (weapon_rows_core.h Tracker: Scan once a frame, IdOf in PullBloodRecs) and tools\weapons\floor_blood_test.cpp run THIS code. Read only: nothing here changes what the game does.
// The game: the pool Q = [0x837689D4] holds 30 particles (Q + 48 + 80 k) and 30 draw records (Q + 2448 + 48 k); record k is particle k's copy (the pool update sub_826983B8, while [Q + 3888] == 0). A particle's state
// only goes up in its life (sub_82697EB8 writes 4 or state + 1); the spawn sub_82697C28 takes a FREE particle (state 4) and writes state 0. So a slot whose state is seen going DOWN holds a new splat: a new serial.
// A record is its particle's own splat when the replay word is 0 and the record's alpha equals the particle's alpha bit for bit (the copy is a plain float move). The replay track's records (alpha / colour
// quantised to 1/15, places as half floats: vtable 0x82028CE0 slot 3 sub_827173F0) and a frame after the replay that still shows them have no identity: the store never keeps them.
#pragma once
#include <cstdint>

#include "../renderer_scene/weapon_rows.h"

namespace rdr_scene::live::bloodid {

namespace bw = rdr_scene::weapons;

// Planted twins (the unit sets them; the game never does).
struct Plants {
  bool replay_as_own = false;   // twin: every record is taken as its particle's own copy (the BLOODMOD landing: a played-back record gets the live particle's identity)
};

class Slots {
 public:
  static constexpr uint32_t kUnseen = 0xFFFFFFFFu;

  // Once a frame, before the frame's first blood call (the tracker's first model call). `pools` = floor_blood::PoolCount() now (it moves when the game makes a pool or the match is restarted).
  template <class ReadWordFn>
  void Scan(ReadWordFn& read_word, uint32_t pools) {
    uint32_t q = 0;
    ok_ = read_word(bw::kBloodPoolGlobal, q) && q >= 0x10000;
    if (!ok_) return;
    if (q != q_ || pools != pools_) Reset(q, pools);   // another pool, or a new scene count: every live splat is a new one
    for (uint32_t k = 0; k < bw::kBloodRecCount; ++k) {
      uint32_t st = 0;
      if (!read_word(q + bw::kBloodPartOff + bw::kBloodPartStride * k + bw::kBloodPartOffState, st)) {
        state_[k] = kUnseen, serial_[k] = 0;
        continue;
      }
      if (st < bw::kBloodStateFree && st < state_[k]) serial_[k] = NextSerial();   // the state went down (or the slot was free / unseen): a new splat
      state_[k] = st;
    }
  }
  // [Q + 3888] != 0: the replay track writes the records this frame (read once, at the record pull).
  template <class ReadWordFn>
  bool ReplayOn(ReadWordFn& read_word) const {
    uint32_t v = 0;
    return ok_ && read_word(q_ + bw::kBloodPoolOffReplay, v) && v != 0;
  }
  // Record k's identity (weapon_rows.h BloodIdentity), or 0 when the record is not particle k's own copy (the replay word, or its alpha bits differ) or the slot was not scanned.
  template <class ReadWordFn>
  uint32_t IdOf(ReadWordFn& read_word, uint32_t k, uint32_t rec_alpha_bits, bool replay, const Plants& p = Plants()) const {
    if (!ok_ || k >= bw::kBloodRecCount || serial_[k] == 0) return 0;
    if (!p.replay_as_own) {
      if (replay) return 0;
      uint32_t pa = 0;
      if (!read_word(q_ + bw::kBloodPartOff + bw::kBloodPartStride * k + bw::kBloodPartOffAlpha, pa) || pa != rec_alpha_bits) return 0;
    }
    return bw::BloodIdentity(pools_, serial_[k]);
  }
  uint32_t Pools() const { return pools_; }
  // S111 BLOODMOD3: slot k's splat identity as the last Scan saw the particle (0 = never seen alive, or no pool), whatever its record holds (blood_held.h: the held rows made from the particle).
  uint32_t LiveId(uint32_t k) const { return ok_ && k < bw::kBloodRecCount && serial_[k] != 0 ? bw::BloodIdentity(pools_, serial_[k]) : 0u; }

 private:
  void Reset(uint32_t q, uint32_t pools) {
    q_ = q, pools_ = pools, next_ = 0;
    for (uint32_t k = 0; k < bw::kBloodRecCount; ++k) state_[k] = kUnseen, serial_[k] = 0;
  }
  uint32_t NextSerial() {
    if ((++next_ & 0xFFFFu) == 0) ++next_;   // a serial whose low 16 bits are 0 would read as "no identity"
    return next_;
  }
  uint32_t q_ = 0, pools_ = 0, next_ = 0;
  bool ok_ = false;
  uint32_t state_[bw::kBloodRecCount] = {}, serial_[bw::kBloodRecCount] = {};
};

}  // namespace rdr_scene::live::bloodid
