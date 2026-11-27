// S111 READANSWER (docs\research\s111\READANSWER.md): where a hit of the game's pac-list walk sub_82C51D20 on an entrance prop / mask path goes. src\pac_answer_hook.h OnLookup calls
// NotePropHit once it has copied the asked path and the matched node's 48 path bytes out of guest memory.
// A hit made while this thread is inside a pac request (sub_82C37E48: scene_live\model_file_note.h ReaderScope is set around it by the hook of that function in src\load_probe.h) is
// the request's own walk (sub_82C37E48 -> sub_82C365A0 / sub_82C36600 -> sub_82C51D20, the request's path), so a READ: the recoloured prop reader sub_83391F18 and the plain prop
// reader sub_825AE720 both read through that request. Any other hit is a probe (pac_path_resolve sub_82C35E30 called outside a request: sub_825AE5D8, sub_825AE680,
// sub_825AE720's existence test). Header-only, std only (the hook and tools\assets\modbelt_unit.cpp include it).
#pragma once

#include <string_view>

#include "assets/pac_path_answer.h"
#include "scene_live/model_file_note.h"

namespace wwe2k14::pac_lookup_route {

// True when the hit was stored (an entrance prop / mask path with a complete pac name: assets::pac_path_answer::Note).
// S111 FOLLOWUPS3 (docs\research\s111\FOLLOWUPS3.md; READANSWER review F2): the reader's CLASS. The recoloured prop task sub_83391F18 (vtable 0x821B9454, stored before its one request)
// reads /SOBJ/<id> only, whatever the plain reader sub_825AE720 reads for the same id. Its read is the answer of its own models (weapon_draw_core.h EvtEntryOf's "/s" entry), never the id's
// read section: such a hit keeps its path's answer (assets::pac_path_answer::PackOf) and is noted as no read, so the id's read section stays the plain readers' (BOBJ when it resolves, else SOBJ).
inline constexpr uint32_t kRecolourPropTaskVtable = 0x821B9454;
inline bool NotePropHit(std::string_view asked, std::string_view node_path48, bool recoloured_reader = false) {
  return ::assets::pac_path_answer::Note(asked, node_path48, rdr_scene::live::model_file_note::CurrentReader() != 0 && !recoloured_reader);
}

}  // namespace wwe2k14::pac_lookup_route
