// levelsbyte_unit: the offline proof of S111 lane LEVELSBYTE (docs\research\s111\LEVELSBYTE.md): the level count a character's textures are built with is the game's
// load-request byte +6, read at the model build (src\scene_live\build_levels_note.h RequestByte / NoteBuild), carried in the record's entry (EntryTag, src\assets\
// level_tag.h) and handed to the loader (level_tag::TexLevelsOfEntry, what GameFilesSource::Load gives LoadWrestlerAt). 0 game runs, no SDK, no game files: a fake
// guest memory and entry strings of our own.
//   levelsbyte_unit [--plant fixed5]
// `--plant fixed5` = the count before this lane (MIPLEVELS: 5 for every character load, whatever the request said): the stock checks must fail.
// Exit 1 when any check failed; the last line `N checks, N failed`.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#include "../../src/assets/change_mask.h"
#include "../../src/assets/level_tag.h"
#include "../../src/scene_live/build_levels_note.h"

namespace lt = assets::level_tag;
namespace bn = rdr_scene::live::build_levels_note;

static int g_checks = 0, g_failed = 0;
static bool g_plant = false;

static void Check(const char* name, bool ok, const std::string& detail = std::string()) {
  ++g_checks;
  if (!ok) ++g_failed;
  std::printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", name, detail.empty() ? "" : ": ", detail.c_str());
}

// The count the loader gives a character load named by `entry` (the plant: MIPLEVELS' fixed 5).
static uint32_t LoadCount(const std::string& entry) { return g_plant ? 5u : lt::TexLevelsOfEntry(entry); }

// A fake guest: big-endian words by address, as the hook's ReadWord returns them.
static std::map<uint32_t, uint32_t> g_mem;
static bool Read(uint32_t a, uint32_t& v) {
  const auto it = g_mem.find(a);
  if (it == g_mem.end()) return false;
  v = it->second;
  return true;
}

// The entry KeyLink writes for a descriptor and key (the same expression: "model_key:" + digits + EntryTag + composite tag).
static std::string Entry(uint32_t desc, uint32_t key, const std::string& composite = std::string()) {
  return "model_key:" + std::to_string(key) + bn::EntryTag(desc, key) + composite;
}

int main(int argc, char** argv) {
  g_plant = argc >= 3 && std::strcmp(argv[1], "--plant") == 0 && std::strcmp(argv[2], "fixed5") == 0;
  constexpr uint32_t kReader = 0x40001000u, kReq = 0x40002000u, kDesc = 0x40003000u, kDesc2 = kDesc + 192;

  // 1. The hook's read: reader+100 -> the request, byte +6 = bits 15..8 of the word at +4 (bytes +4, +5, +7 set apart so a wrong shift is seen).
  g_mem[kReader + 100] = kReq;
  auto byte_of = [&](uint8_t b6) {
    g_mem[kReq + 4] = (9u << 24) | (10u << 16) | (uint32_t(b6) << 8) | 1u;
    uint8_t b = 0xEE;
    return bn::RequestByte(Read, kReader, b) ? int(b) : -1;
  };
  Check("read.byte6_4", byte_of(4) == 4);
  Check("read.byte6_1", byte_of(1) == 1);
  Check("read.byte6_0", byte_of(0) == 0);
  g_mem.erase(kReader + 100);
  uint8_t none = 0;
  Check("read.unreadable", !bn::RequestByte(Read, kReader, none), "no request word: false (the hook then drops the descriptor's note)");

  // 2. A stock wrestler's build (the fresh request's 4, every filler keeps it for a key >= 10000): no tag, the loader's 4.
  bn::Clear();
  bn::NoteBuild(kDesc, 39712, 39712, 4);
  const std::string stock = Entry(kDesc, 39712);
  Check("stock.entry_untagged", stock == "model_key:39712", stock);
  Check("stock.match_count_4", LoadCount(stock) == 4, std::to_string(LoadCount(stock)));
  // The entrance slots (descriptors 9..11) with the online word off: the request keeps 4 too (sub_82311D30 sets 5 only inside its test). The MIPLEVELS fixed 5 here is the planted twin.
  bn::NoteBuild(kDesc2, 39702, 39702, 4);
  Check("stock.entrance_lock_off_count_4", LoadCount(Entry(kDesc2, 39702)) == 4, std::to_string(LoadCount(Entry(kDesc2, 39702))));
  Check("stock.default_is_fresh_request", lt::kStockTexLevels == 4 && LoadCount("model_key:39712/m") == 4, "no tag (and the /m tag alone) = 4");

  // 3. Another count: the Threads preview's 1, carried through the entry with the other tags in their places.
  uint8_t colours[attire_compose::kColourBytes];
  for (int i = 0; i < attire_compose::kColourBytes; ++i) colours[i] = uint8_t(i * 5);
  bn::NoteBuild(kDesc, 24902, 24912, 1);
  const std::string tagged = change_mask::WithTag(Entry(kDesc, 24902, attire_compose::EntryTag(colours)));
  int key = 0;
  uint8_t back[attire_compose::kColourBytes];
  Check("tag.threads_count_1", LoadCount(tagged) == 1, tagged);
  Check("tag.key_still_parses", std::sscanf(tagged.c_str(), "model_key:%d", &key) == 1 && key == 24902);
  Check("tag.mask_tag_kept", change_mask::HasTag(tagged) && tagged.rfind("model_key:24902/m/l1/c:", 0) == 0, tagged);
  Check("tag.colours_kept", attire_compose::ParseEntryTag(tagged, back) && std::memcmp(back, colours, sizeof back) == 0);
  Check("tag.chosen_key_names_it", Entry(kDesc, 24912) == "model_key:24912/l1", "the slot-key route asks by the chosen key (desc+92)");
  Check("tag.other_key_untagged", Entry(kDesc, 39712) == "model_key:39712", "a descriptor that holds another key: no tag");
  const std::string refused = Entry(kDesc, 24902, attire_compose::RefusalTag());
  Check("tag.refusal_suffix_kept", attire_compose::IsRefusalTag(refused), refused);

  // 4. The game's 0 (keys whose hundreds are 255) = the whole chain; a 5 (a created key) is carried as 5.
  bn::NoteBuild(kDesc, 25510, 25510, 0);
  Check("tag.zero_is_whole_chain", Entry(kDesc, 25510) == "model_key:25510/l0" && LoadCount(Entry(kDesc, 25510)) == lt::kWholeChainLevels);
  bn::NoteBuild(kDesc, 39712, 39712, 5);
  Check("tag.five_carried", LoadCount(Entry(kDesc, 39712)) == 5);

  // 5. A rebuild at 4 drops the old count; a failed read forgets it.
  bn::NoteBuild(kDesc, 39712, 39712, 4);
  Check("rebuild.drops_note", Entry(kDesc, 39712) == "model_key:39712" && LoadCount(Entry(kDesc, 39712)) == 4);
  bn::NoteBuild(kDesc, 39712, 39712, 1);
  bn::Forget(kDesc);
  Check("forget.drops_note", Entry(kDesc, 39712) == "model_key:39712");

  // 6. The parser: only "/l<1-3 digits>" followed by '/' or the end.
  Check("parse.plain", lt::ByteOfEntry("model_key:39712") == 4);
  Check("parse.end", lt::ByteOfEntry("model_key:39712/l2") == 2);
  Check("parse.middle", lt::ByteOfEntry("model_key:39712/m/l3/c:00") == 3);
  Check("parse.not_digits", lt::ByteOfEntry("model_key:39712/lx") == 4 && lt::ByteOfEntry("model_key:39712/l") == 4);
  Check("parse.too_long", lt::ByteOfEntry("model_key:39712/l1234") == 4 && lt::ByteOfEntry("model_key:39712/l300") == 4);

  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
