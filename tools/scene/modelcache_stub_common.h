// S103 TLF: the ONE shared stand-in for the parts of record_frame.cpp's ModelCache that the appliers call by name (rf_props.cpp: StoredNamed / DropStored / RetireOther;
// rf_wrestlers.cpp, rf_arena.cpp, rf_c_core.cpp: RetireOther). Included at file scope by every tool that supplies its own ModelCache constructor / Get / Put / Find
// instead of linking record_frame.cpp (c_driver.cpp, rf_b_modelcache_stub.cpp). A new public ModelCache method an applier starts calling goes HERE once, not into each
// tool's stub; tools\scene\link_check.py fails when a tool build lacks one. The signatures are record_frame.h's; the drop rule is
// record_frame.cpp's, without the front-end release (the tools never retire GPU models).
// NOT inline: an inline member is emitted only in a TU that uses it, so the applier TUs would still fail to link. Include it in exactly ONE TU per tool.
#pragma once
#include <iterator>
#include <string>

#include "../../src/renderer_scene/record_frame.h"

namespace rdr_scene::rf {

const assets::Model* ModelCache::StoredNamed(const std::string& pac, const std::string& entry, uint32_t variant, std::string* why, bool /*count_miss*/) {
  const auto it = stored_.find(pac + "|" + entry + "|" + std::to_string(variant));
  if (it != stored_.end()) return &it->second;
  if (why) *why = "tool stand-in: nothing is loaded by name";
  return nullptr;
}
void ModelCache::DropStored(const std::string& pac, const std::string& entry, uint32_t variant) { stored_.erase(pac + "|" + entry + "|" + std::to_string(variant)); }
void ModelCache::RetireOther(const std::string& prefix, const std::string& keep) {
  const auto drop = [&](const std::string& key) { return key.rfind(prefix, 0) == 0 && key.find(keep) == std::string::npos; };
  for (auto it = pools_.begin(); it != pools_.end();) {
    if (drop(it->first)) {
      for (size_t i = it->second.size(); i-- > 0;) if (it->second[i] >= 0) retired_.push_back(it->second[i]);
      it = pools_.erase(it);
    } else ++it;
  }
  for (auto it = stored_.begin(); it != stored_.end();) it = drop(it->first) ? stored_.erase(it) : std::next(it);
}

}  // namespace rdr_scene::rf
