// S103 W2B: the node-name alias table of the game's "find the owner's node by name", sub_82BD24C8 (docs\agents\s103\W2_ADV.md row 1b). The SUBM component maps each piece node through that lookup
// (sub_8259E720 0x8259EC3C / 0x8259EC6C): the name itself in the owner's node table first; when it is absent and its first byte is a-z (0x82BD2A2C-0x82BD2A44), the `to` name of the table's pair (a `to` of
// "null" = -1); not found = -1. 42 (from, to) pairs, table at 0x8392DDC0, stride 32, filled once from string constants at 0x82BD2A80-0x82BD2E88. SOURCE: decoded from the player's own image by
// tools\scene\w2b_alias_table.py (a symbolic pass over the listing; tools\scene\w2b_alias_check.py compares its output with this table, w2b_unit.cpp checks the count and the pairs the Rock's pads need). These are bone NAMES only. A name whose `to` is not a node of the
// owner is -1 as well; exactly four pairs are null (bone01-04: 16-byte ld / std copies of the "null" buffer at 0x8392DDB0, whose base the tool resolves). [U] whether the game chains a second alias on the retry: this port retries once (a mismatch shows up as a tag difference: a skipped piece, never a wrong pose).
#pragma once
#include <cstring>

namespace assets {

struct NodeAliasPair { const char* from; const char* to; };
inline constexpr NodeAliasPair kNodeAliases[] = {
    {"root", "Bip"},
    {"koshi", "Bip Spine"},
    {"mune", "Bip Spine1"},
    {"kubi", "Bip Neck"},
    {"atama", "Bip Head"},
    {"l_sakotsu", "Bip L Clavicle"},
    {"l_ninoude", "Bip L UpperArm"},
    {"l_kote", "Bip L Forearm"},
    {"l_te", "Bip L Hand"},
    {"r_sakotsu", "Bip R Clavicle"},
    {"r_ninoude", "Bip R UpperArm"},
    {"r_kote", "Bip R Forearm"},
    {"r_te", "Bip R Hand"},
    {"l_momo", "Bip L Thigh"},
    {"l_sune", "Bip L Calf"},
    {"l_ashi", "Bip L Foot"},
    {"l_tsumasaki", "Bip L Toe0"},
    {"r_momo", "Bip R Thigh"},
    {"r_sune", "Bip R Calf"},
    {"r_ashi", "Bip R Foot"},
    {"r_tsumasaki", "Bip R Toe0"},
    {"bone01", "null"},
    {"bone02", "null"},
    {"bone03", "null"},
    {"bone04", "null"},
    {"l_ninoude_x", "L_UpperArm_x"},
    {"r_ninoude_x", "R_UpperArm_x"},
    {"l_kote_x", "L_Wrist_x"},
    {"r_kote_x", "R_Wrist_x"},
    {"l_momo_x", "L_Thigh_x"},
    {"r_momo_x", "R_Thigh_x"},
    {"l_kote_x2", "L_Wrist_x2"},
    {"r_kote_x2", "R_Wrist_x2"},
    {"kubi_x", "Neck_x"},
    {"r_ninoude_m", "R_UpperArm_m"},
    {"l_ninoude_m", "L_UpperArm_m"},
    {"r_kote_m", "R_Elbow_m"},
    {"l_kote_m", "L_Elbow_m"},
    {"l_sakotsu_m", "L_Clavicle_m"},
    {"r_sakotsu_m", "R_Clavicle_m"},
    {"l_kata_m", "L_Shoulder_m"},
    {"r_kata_m", "R_Shoulder_m"},
};
inline constexpr int kNodeAliasCount = int(sizeof kNodeAliases / sizeof kNodeAliases[0]);

// The `to` name for `name`, or nullptr when the table has none for it (or the name does not start with a-z, the game's gate). "null" would be returned as is: the caller treats it as -1.
inline const char* NodeAliasOf(const char* name) {
  if (!name || name[0] < 'a' || name[0] > 'z') return nullptr;
  for (const NodeAliasPair& p : kNodeAliases)
    if (std::strncmp(p.from, name, 16) == 0) return p.to;
  return nullptr;
}

}  // namespace assets
