// Surface roles (Session 101, arc A1b; docs\renderer\NAMES.md "Surface roles"): which game function created each render-target header.
// The surface creator 0x82CA88E0 (r3 width, r4 height, r5 format word; returns the 48-byte header: listing read S101) is hooked in
// src\surface_roles_hook.h; the reader looks a header up here when it first sees it and gives the image the role the creating function
// declares (surface_role_table.h, generated from db\ids\surface_roles.json). Creates are rare (loads), so one mutex is enough.
// No game or SDK includes.
#pragma once
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace rdr_adapter::roles {

struct Created {
  uint32_t call_site = 0;  // the creator's caller (lr - 4): the function holding it declares the role
  uint32_t width = 0, height = 0, format_word = 0;
};

class Creators {
 public:
  void Note(uint32_t header, const Created& c) {
    std::lock_guard lock(mutex_);
    by_header_[header] = c;  // a freed header reused by a later create is simply overwritten
    ++count_;
  }
  bool Find(uint32_t header, Created& out) const {
    std::lock_guard lock(mutex_);
    auto it = by_header_.find(header);
    if (it == by_header_.end()) return false;
    out = it->second;
    return true;
  }
  uint64_t count() const { std::lock_guard lock(mutex_); return count_; }

 private:
  mutable std::mutex mutex_;
  std::unordered_map<uint32_t, Created> by_header_;
  uint64_t count_ = 0;
};

inline Creators& Global() {
  static Creators* c = new Creators;  // never destroyed (game threads may create at exit)
  return *c;
}

}  // namespace rdr_adapter::roles
