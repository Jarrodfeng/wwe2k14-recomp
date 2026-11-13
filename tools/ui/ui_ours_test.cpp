// Session 101 lane 66: the unit test of our UI textures (src\renderer_scene\ui_ours_core.*, the SHIPPED code UiOurs calls) on the player's files and a capture. No GPU, no game, no SDK.
//
//   ui_ours_test.exe index   --game DIR [--cache DIR]
//   ui_ours_test.exe synth   --game DIR [--cache DIR] [--fault NAME]
//   ui_ours_test.exe capture --game DIR --capture DIR --kind menu|editor|match [--cache DIR] [--step N] [--fault NAME] [--copy-after FRAME]
//
// `capture` replays the capture's frames through the same feeder the game and fview use (FeedImageContents), asks UiCore::Decide for every claimed 2D draw (a test provider
// loads each joined picture through the shipped UiFileLoader, so the faults rb_swap / short_mips act), and for every replaced draw compares OUR bytes with the bytes the capture
// holds for the stream's image (the blob in textures.bin, read back here: not the hash the join used). Lines are `PASS|FAIL name detail`; exit 1 on any FAIL, 2 when an input
// cannot be read. A planted fault (--fault) must make its check FAIL: tools\ui\ui_ours_check.py runs every plant and demands it.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#include "../../src/renderer_scene/ui_ours_core.h"

using namespace rdr_scene;
using rdr_adapter::CaptureReader;
using rdr_adapter::ContentRef;
using rdr_adapter::LoadedFrame;
using rdr_adapter::LoadedTexture;
using rdr_adapter::ContentHasher;

namespace {

struct Args {
  std::string mode, game, capture, kind = "menu", cache, fault;
  uint32_t step = 1;
  uint32_t copy_after = 0;
  uint32_t copy_width = 0;   // lane 126 (review F5): --copy-width W picks the most-used picture of that width as the copy destination (512 = fade_lightmap00, the arena page)
  int arena = -1;            // lane 126: the arena of the capture (the second tier's bgNN); -1: none
};

int g_fail = 0;
void Line(bool ok, const char* name, const std::string& detail) {
  std::printf("%s  %-14s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
  if (!ok) g_fail = 1;
}
std::string Num(unsigned long long v) { return std::to_string(v); }

class TestProvider : public UiTextureProvider {
 public:
  TestProvider(const std::string& game, const UiFaults& faults) : loader_(game, faults) {}
  const UiTexture* Texture(uint32_t entry_id, const UiFileEntry& e, uint32_t mips, bool& pending) override {
    const uint64_t entry = uint64_t(entry_id) | uint64_t(mips) << 32;
    pending = false;
    if (auto it = loaded_.find(entry); it != loaded_.end()) return &it->second.texture;
    if (failed_.count(entry)) return nullptr;
    Item item;
    std::string err;
    if (!loader_.Load(e, mips, item.data, err)) {
      std::printf("      load failed: %s\n", err.c_str());
      failed_[entry] = true;
      return nullptr;
    }
    item.texture.handle = entry_id + 1, item.texture.width = item.data.width, item.texture.height = item.data.height, item.texture.mips = item.data.mips;
    item.texture.format = item.data.format, item.texture.prefix = item.data.prefix;
    return &(loaded_[entry] = std::move(item)).texture;
  }
  const UiLoaded* Loaded(uint32_t entry_id, uint32_t mips) const {
    const uint64_t entry = uint64_t(entry_id) | uint64_t(mips) << 32;
    const auto it = loaded_.find(entry);
    return it == loaded_.end() ? nullptr : &it->second.data;
  }
  void ReleasePacs() { loader_.ReleasePacs(); }

 private:
  struct Item {
    UiLoaded data;
    UiTexture texture;
  };
  UiFileLoader loader_;
  std::map<uint64_t, Item> loaded_;
  std::map<uint64_t, bool> failed_;
};

bool Parse(int argc, char** argv, Args& a) {
  if (argc < 2) return false;
  a.mode = argv[1];
  for (int i = 2; i < argc; ++i) {
    const std::string s = argv[i];
    const auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
    if (s == "--game") a.game = next();
    else if (s == "--capture") a.capture = next();
    else if (s == "--kind") a.kind = next();
    else if (s == "--cache") a.cache = next();
    else if (s == "--fault") a.fault = next();
    else if (s == "--step") a.step = uint32_t(std::strtoul(next().c_str(), nullptr, 10));
    else if (s == "--copy-after") a.copy_after = uint32_t(std::strtoul(next().c_str(), nullptr, 10));
    else if (s == "--copy-width") a.copy_width = uint32_t(std::strtoul(next().c_str(), nullptr, 10));   // lane 126
    else if (s == "--arena") a.arena = std::atoi(next().c_str());   // lane 126
    else return false;
  }
  return !a.game.empty() && (a.mode == "index" || a.mode == "synth" || (a.mode == "capture" && !a.capture.empty())) && a.step >= 1;
}

// lane 126 (review F3): the unit mirrors the shipped tiers. BuildIndex indexes the FIRST tier only (kUiTier1Count pacs: what start-up does); the second tier (bgEtc and the
// capture's arena bgNN) is added by DeepIndex when a claimed draw stays unjoined, exactly the trigger UiOurs::MaybeStartDeep has.
bool IndexPacs(const Args& a, UiFileIndex& index, uint8_t from, uint8_t to, const char* label) {
  UiIndexOptions o;
  o.game_root = a.game, o.cache_dir = a.cache;
  bool all = true;
  for (uint8_t pac = from; pac < to; ++pac) {
    std::vector<UiFileEntry> entries;
    std::string err;
    bool cached = false;
    const auto t0 = std::chrono::steady_clock::now();
    if (!BuildPacIndex(o, pac, entries, err, &cached)) {
      std::printf("      %s: %s\n", UiPacPath(pac).c_str(), err.c_str());
      all = false;
      continue;
    }
    std::printf("      %-24s %5zu textures, %6.0f ms%s\n", UiPacPath(pac).c_str(), entries.size(), std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), cached ? " (cache)" : "");
    index.AddPac(pac, std::move(entries));
  }
  Line(all && index.texture_count() > 0, label, index.Summary() + (all ? "" : "; a pac could not be read"));
  return all;
}

bool BuildIndex(const Args& a, UiFileIndex& index) { return IndexPacs(a, index, 0, uint8_t(kUiTier1Count), "index"); }

}  // namespace

int main(int argc, char** argv) {
  Args a;
  if (!Parse(argc, argv, a)) {
    std::fprintf(stderr, "usage: ui_ours_test index --game DIR [--cache DIR] | capture --game DIR --capture DIR --kind menu|editor|match [--cache DIR] [--step N] [--fault NAME]\n");
    return 2;
  }
  UiFaults faults;
  if (!UiFaults::Parse(a.fault, faults)) {
    std::fprintf(stderr, "unknown fault %s\n", a.fault.c_str());
    return 2;
  }
  UiFileIndex index;
  if (!BuildIndex(a, index) && a.mode == "index") return 1;
  if (a.mode == "index") return g_fail;

  if (a.mode == "synth") {
    // The mip-prefix join and the mips check, on a picture of the files with several mips (no joined picture of the menu / editor zips has more than one): a SYNTHETIC stream image
    // whose content is the first 3 mips of the file's picture (hash from the index's prefix list), asked of the shipped UiCore::Decide through the shipped loader. Clean: replaced.
    // short_mips (ours keeps mip 0 only): the mips check must refuse. Not a capture: the stream side is built here, the file side is the real files.
    rdr_adapter::ImageContentTable table;
    UiFileEntry entry;
    uint32_t id = 0;
    bool found = false;
    for (; index.Get(id, entry); ++id)
      if (entry.mips >= 3 && (entry.format == rdr::Format::BC1 || entry.format == rdr::Format::BC3)) { found = true; break; }
    Line(found, "synth_entry", found ? "a multi-mip picture of the files: " + entry.path + " " + Num(entry.width) + "x" + Num(entry.height) + " mips " + Num(entry.mips) : "no picture with 3+ mips in the index");
    if (!found) return 1;
    rdr_adapter::ImageContent c;
    c.width = entry.width, c.height = entry.height, c.format = uint32_t(entry.format), c.mips = 3, c.hash = entry.prefix[2];
    table.Put(1, c);
    TestProvider provider(a.game, faults);
    UiCore core(faults, table.Supplier(), &index);
    core.BeginFrame();
    const UiCore::Verdict v = core.Decide(77, 1, nullptr, 0, 0, 15, provider);
    const UiCensus cs = core.census();   // (a copy: the view case below adds to the live census)
    Line(cs.claimed == 1 && v.ours && cs.replaced == 1, "synth_join", "the stream holds the first 3 of " + Num(entry.mips) + " mips: " + (v.ours ? "joined and replaced" : "kept") + "; " + core.Report(index));
    if (!faults.short_mips) {   // review F3: a slot viewed as another class than its storage (a signed / biased reinterpretation) keeps the stream's draw
      core.BeginFrame();
      const UiCore::Verdict w = core.Decide(77, 1, nullptr, uint32_t(rdr::Format::RG8), 0, 15, provider);
      Line(!w.ours && core.census().mismatch_view == 1, "synth_view", "view format RG8 on a BC picture: " + std::string(w.ours ? "replaced (wrong)" : "kept") + ", mismatch view " + Num(core.census().mismatch_view));
    }
    Line(cs.mismatch == 0 && cs.mismatch_mips == 0, "checks", "mismatch " + Num(cs.mismatch) + " (mips " + Num(cs.mismatch_mips) + ")");
    return g_fail;
  }

  CaptureReader capture;
  if (!capture.Open(a.capture)) {
    std::fprintf(stderr, "cannot open the capture %s\n", a.capture.c_str());
    return 2;
  }
  rdr_adapter::ImageContentTable table;
  TestProvider provider(a.game, faults);
  UiCore core(faults, table.Supplier(), &index);
  std::map<uint32_t, ContentRef> latest;
  LoadedFrame frame;
  LoadedTexture scratch, ours_blob;
  struct Usage {
    uint32_t first = 0, last = 0;
    uint64_t n = 0;
  };
  std::map<uint32_t, Usage> usage;
  uint64_t fed = 0, draws_2d = 0, bytes_checked = 0, bytes_equal = 0, frames_decided = 0, images_checked_total = 0;
  uint32_t copied_image = 0;
  uint64_t copy_violations = 0;
  std::map<uint32_t, int> unjoined_seen, explained;
  std::map<std::string, uint64_t> swz;
  std::map<uint32_t, bool> image_ok;   // per stream image: every check of it passed
  bool deep_done = false;              // lane 126: the second tier was indexed (the trigger fired)
  for (uint32_t f = 0; f < capture.frame_count(); ++f) {
    if (!capture.LoadFrame(f, frame)) {
      std::fprintf(stderr, "cannot read frame %u\n", f);
      return 2;
    }
    fed += rdr_adapter::FeedImageContents(frame, capture, table, scratch, !faults.gpu_copy_after_join);
    if (a.copy_after && f == a.copy_after && !usage.empty()) {   // the scenario: the most-used picture so far becomes a GPU copy destination (a doctored frame carrying only that copy)
      uint64_t best = 0;
      for (const auto& [img, u] : usage) {
        rdr_adapter::ImageContent ic;
        if (a.copy_width && (!table.Lookup(img, ic) || ic.width != a.copy_width)) continue;   // lane 126 (review F5)
        if (u.n > best) best = u.n, copied_image = img;
      }
      LoadedFrame doctored;
      rdr::CopyOp copy;
      copy.dest.image = rdr::ImageId(copied_image);
      doctored.copies.data = &copy, doctored.copies.count = 1;
      rdr_adapter::FeedImageContents(doctored, capture, table, scratch, !faults.gpu_copy_after_join);
    }
    for (const ContentRef& ref : frame.contents) latest[ref.image] = ref;
    if (f % a.step != 0 && f + 1 != capture.frame_count()) continue;
    ++frames_decided;
    core.BeginFrame();
    for (uint32_t i = 0; i < frame.draws.count; ++i) {
      const rdr_adapter::DrawOrigin* origin = frame.OriginOf(i);
      if (!origin || !core.Claims(*origin)) continue;
      ++draws_2d;
      const rdr_adapter::Draw& d = frame.draws[i];
      uint32_t image = 0;
      const uint8_t* swz4 = nullptr;
      uint32_t swl_min = 0, swl_max = 15, swl_view = 0;
      for (uint32_t t = 0; t < d.texture_count && d.texture_first + t < frame.texture_slots.count; ++t)
        if (frame.texture_slots[d.texture_first + t].slot == 0) image = frame.texture_slots[d.texture_first + t].image, swz4 = frame.texture_slots[d.texture_first + t].swizzle, swl_view = frame.texture_slots[d.texture_first + t].view_format, swl_min = frame.texture_slots[d.texture_first + t].min_level, swl_max = frame.texture_slots[d.texture_first + t].max_level;
      {
        rdr_adapter::ImageContent sc;
        for (uint32_t t = 0; t < d.texture_count && d.texture_first + t < frame.texture_slots.count; ++t) {
          const rdr_adapter::TextureSlot& ts = frame.texture_slots[d.texture_first + t];
          if (ts.slot != 0 || !table.Lookup(ts.image, sc)) continue;
          char key[160];
          std::snprintf(key, sizeof key, "content format %u view %u swizzle %u%u%u%u sign %u%u%u%u dim %u minlevel %u target16 %u", sc.format, ts.view_format, ts.swizzle[0], ts.swizzle[1], ts.swizzle[2], ts.swizzle[3], ts.sign[0], ts.sign[1], ts.sign[2], ts.sign[3], ts.dimension, ts.min_level, ts.target16_read);
          ++swz[key];
        }
      }
      UiCore::Verdict v = core.Decide(origin->part, image, swz4, swl_view, swl_min, swl_max, provider);
      if (!v.ours && v.why == rdr_adapter::KeepWhy::Unjoined && !deep_done && !faults.no_deep && a.kind == "match") {   // lane 126: the trigger (UiOurs::MaybeStartDeep): bgEtc and the arena's bgNN, then ask again
        deep_done = true;
        IndexPacs(a, index, uint8_t(kUiBgEtc), uint8_t(kUiBgEtc + 1), "tier2_etc");
        if (a.arena >= 0) IndexPacs(a, index, uint8_t(kUiBgBase + a.arena), uint8_t(kUiBgBase + a.arena + 1), "tier2_arena");
        core.RetractUnjoined();
        v = core.Decide(origin->part, image, swz4, swl_view, swl_min, swl_max, provider);
      }
      if (!v.ours) {
        rdr_adapter::ImageContent ic;
        if (v.why == rdr_adapter::KeepWhy::Unjoined && image && table.Lookup(image, ic) && unjoined_seen.emplace(image, 0).second)
          std::printf("      unjoined image %u (key %08X, call site %08X): %ux%u format %u mips %u, %s\n", image, origin->part, origin->call_site, ic.width, ic.height, ic.format, ic.mips, ic.hash ? "hashed" : "movie plane");
        // Why an unjoined picture found no file: look it up again as the other 4-byte format and with R and B exchanged (a printed diagnosis, not a join).
        if (v.why == rdr_adapter::KeepWhy::Unjoined && image && explained.emplace(image, 0).second && table.Lookup(image, ic) && ic.hash && latest.count(image) &&
            capture.ReadTexture(latest[image], ours_blob) && (ic.format == uint32_t(rdr::Format::RGBA8) || ic.format == uint32_t(rdr::Format::BGRA8))) {
          std::vector<uint8_t> swapped(ours_blob.data, ours_blob.data + ours_blob.header.data_bytes);
          for (size_t b = 0; b + 3 < swapped.size(); b += 4) std::swap(swapped[b], swapped[b + 2]);
          ContentHasher hs;
          hs.Update(swapped.data(), swapped.size());
          uint32_t id = 0;
          std::printf("      explain image %u: as RGBA8 %d, as BGRA8 %d, R/B swapped as RGBA8 %d, R/B swapped as BGRA8 %d\n", image,
                      index.Find(ic.width, ic.height, rdr::Format::RGBA8, ic.mips, ic.hash, id), index.Find(ic.width, ic.height, rdr::Format::BGRA8, ic.mips, ic.hash, id),
                      index.Find(ic.width, ic.height, rdr::Format::RGBA8, ic.mips, hs.Digest(), id), index.Find(ic.width, ic.height, rdr::Format::BGRA8, ic.mips, hs.Digest(), id));
        }
        continue;
      }
      // OUR bytes against the capture's bytes for this image (independent of the hash the join used).
      const UiLoaded* ours = provider.Loaded(v.entry, v.texture->mips);
      const auto ref = latest.find(image);
      bool equal = false;
      if (ours && ref != latest.end() && capture.ReadTexture(ref->second, ours_blob) && ours->data.size() <= ours_blob.header.data_bytes)
        equal = std::memcmp(ours_blob.data, ours->data.data(), ours->data.size()) == 0 && UiByteClass(rdr::Format(uint8_t(ours_blob.header.format))) == UiByteClass(ours->format);
      {
        if (v.texture && copied_image && image == copied_image) ++copy_violations;
        auto& u = usage[image];
        if (!u.n) u.first = f;
        u.last = f, ++u.n;
      }
      ++bytes_checked, bytes_equal += equal;
      auto it = image_ok.emplace(image, true).first;
      it->second = it->second && equal;
    }
  }
  for (const auto& [k, n] : swz) std::printf("      slot 0 of claimed draws: %s x%llu\n", k.c_str(), (unsigned long long)n);
  for (const auto& [img, u] : usage) {
    rdr_adapter::ImageContent ic;
    table.Lookup(img, ic);
    std::printf("      image %u: %ux%u format %u mips %u, first seen at frame %u, last %u, %llu replaced draws\n", img, ic.width, ic.height, ic.format, ic.mips, u.first, u.last, (unsigned long long)u.n);
  }
  images_checked_total = image_ok.size();
  uint64_t bad_images = 0;
  for (const auto& [image, ok] : image_ok) bad_images += !ok;
  const UiCensus& c = core.census();
  std::printf("      frames %u (decided %llu), contents fed %llu, 2D draws claimed %llu, %s\n", capture.frame_count(), (unsigned long long)frames_decided, (unsigned long long)fed,
              (unsigned long long)draws_2d, core.Report(index).c_str());
  const bool menu_like = a.kind == "menu" || a.kind == "editor";
  if (menu_like) Line(!deep_done, "tier2_idle", "menus / editor never start the second tier (their pictures all join in the first tier)");
  if (a.copy_after) Line(copied_image != 0 && copy_violations == 0 && c.gpu > 0, "gpu_copy", "image " + Num(copied_image) + " became a GPU copy destination after frame " + Num(a.copy_after) + ": " + Num(copy_violations) + " later draws still replaced (bar 0), " + Num(c.gpu) + " kept as gpu");
  Line(draws_2d > 0, "claimed", Num(draws_2d) + " 2D draws claimed (origins present)");
  Line(c.replaced > 0 || !menu_like, "replaced", Num(c.replaced) + " draws replaced, " + Num(core.joined_keys()) + " keys known");
  Line(bytes_checked == c.replaced && (bytes_checked > 0 || !menu_like) && bytes_equal == bytes_checked, "bytes",
       Num(bytes_equal) + " of " + Num(bytes_checked) + " replaced draws: our upload's bytes equal the capture image's bytes (" + Num(images_checked_total) + " images, " + Num(bad_images) + " differ)");
  Line(c.mismatch == 0 && c.load_failed == 0 && c.pending == 0, "checks", "per-draw checks: mismatch " + Num(c.mismatch) + " (size " + Num(c.mismatch_size) + ", mips " + Num(c.mismatch_mips) + ", bytes " +
       Num(c.mismatch_bytes) + ", key " + Num(c.mismatch_key) + "), load failed " + Num(c.load_failed) + ", pending " + Num(c.pending));
  if (menu_like)
    Line(c.unjoined == 0 && c.nokey == 0, "unjoined", "menu-pac keys unjoined " + Num(c.unjoined) + ", without a key " + Num(c.nokey) + " (kept only as: gpu " + Num(c.gpu) + ", movie " + Num(c.movie) + ", untextured " +
         Num(c.untextured) + ")");
  else if (a.kind == "match" && !a.copy_after) {   // lane 126 (review F3): the entrance zip's 2D draws ALL join once the second tier ran (it ran, once, because the first tier alone leaves them unjoined)
    Line(deep_done || faults.no_deep, "tier2_trigger", std::string("the first tier alone left a draw unjoined and the trigger indexed bgEtc + the arena's bgNN: ") + (deep_done ? "yes" : "no"));
    Line(c.replaced == c.claimed && c.unjoined == 0, "unjoined", "match zip: replaced " + Num(c.replaced) + " of claimed " + Num(c.claimed) + ", unjoined " + Num(c.unjoined) + " (bar: all replaced, 0 unjoined), gpu " + Num(c.gpu) + ", movie " + Num(c.movie) + ", untextured " + Num(c.untextured));
  }
  Line(c.claimed == c.replaced + c.unjoined + c.mismatch + c.movie + c.gpu + c.untextured + c.pending + c.nokey, "census", "claimed " + Num(c.claimed) + " = replaced + the kept reasons");
  return g_fail;
}
