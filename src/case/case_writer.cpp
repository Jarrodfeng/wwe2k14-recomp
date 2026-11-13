#include "case_writer.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <cstring>
#include <ctime>
#include <exception>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>

#if __has_include("../../thirdparty/rexglue-sdk-src/thirdparty/xxHash/xxhash.h")
#define XXH_INLINE_ALL
#include "../../thirdparty/rexglue-sdk-src/thirdparty/xxHash/xxhash.h"
#else
#define XXH_INLINE_ALL
#include <xxHash/xxhash.h>
#endif

#include "case_info.h"  // S101 lane 19
#include "zip_writer.h"

namespace rdr_case {

using namespace rdr_adapter;

namespace {

using Sink = std::function<void(const void*, size_t)>;

struct Entry {
  std::string path;
  Part part = Part::Ours;
  uint64_t size = 0;
  std::function<void(const Sink&)> produce;
  uint64_t xxh3 = 0;
};

// S101 lane 11: a content is only valid for the image record it was read for. The ring keeps ONE image table (the newest) but contents from
// before a record changed (an image the reader later recreated as a copy destination or with another format: the live path resends its
// contents after SyncImages). Replayed under the newer record, such a stale content is smaller than the image and the upload copies past
// its buffer (case 7041: 1 MB of RG16 into a 2 MB RGBA16F image, vkCmdCopyBufferToImage). A stale content is left out of the zip, which is
// what the live renderer does too (it resends; the window's later frames carry the matching one).
bool ContentFitsRecord(const Content& c, const ImageRecord& rec) {
  TextureBlobHeader h;
  if (c.bytes.size() < sizeof(h)) return false;
  std::memcpy(&h, c.bytes.data(), sizeof(h));
  if (h.magic != kTextureMagic || h.image != rec.id) return false;
  if (h.format != rec.format) return false;
  if (h.subresource_count == 0) return false;
  const uint64_t subs_end = sizeof(h) + uint64_t(h.subresource_count) * sizeof(Subresource);
  if (subs_end > c.bytes.size() || h.data_bytes > c.bytes.size() - subs_end) return false;
  const uint32_t layers = std::max<uint32_t>(rec.layers, rec.depth);
  for (uint32_t i = 0; i < h.subresource_count; ++i) {
    Subresource sub;
    std::memcpy(&sub, c.bytes.data() + sizeof(h) + i * sizeof(Subresource), sizeof(sub));
    if (sub.mip >= std::max<uint32_t>(rec.mips, 1) || sub.layer >= std::max<uint32_t>(layers, 1)) return false;
    if (sub.width > std::max<uint32_t>(rec.width, 1) || sub.height > std::max<uint32_t>(rec.height, 1)) return false;
    if (sub.offset > h.data_bytes || uint64_t(sub.row_bytes) * sub.rows > h.data_bytes - sub.offset) return false;
  }
  return true;
}

// ---- frames: the ring's keys become offsets into the zip's own geometry.bin / textures.bin --------------------------------------------
struct Plan {
  std::vector<ContentPtr> geometry_order, texture_order;
  std::unordered_map<uint64_t, uint64_t> geometry_offset, texture_offset;
  uint64_t geometry_bytes = 0, texture_bytes = 0;
  std::vector<std::pair<uint32_t, uint64_t>> extra_refs;  // the baseline refs for frame 0: (image, key)
  std::vector<uint64_t> extra_sizes;
};

std::vector<uint8_t> RewriteFrame(const RingFrame& frame, uint32_t index, const Plan& plan, bool first) {
  FrameFileHeader h;
  std::memcpy(&h, frame.bytes.data(), sizeof(h));
  std::vector<SectionEntry> entries(h.section_count);
  std::vector<std::vector<uint8_t>> data(h.section_count);
  for (uint32_t s = 0; s < h.section_count; ++s) {
    std::memcpy(&entries[s], frame.bytes.data() + sizeof(FrameFileHeader) + s * sizeof(SectionEntry), sizeof(SectionEntry));
    const SectionEntry& e = entries[s];
    const uint8_t* at = frame.bytes.data() + e.offset;
    data[s].assign(at, at + e.count * e.element_size);
    if (e.id == kSecDraws && e.element_size == sizeof(Draw)) {
      for (uint64_t i = 0; i < e.count; ++i) {
        Draw d;
        std::memcpy(&d, data[s].data() + i * sizeof(Draw), sizeof(Draw));
        if (!(d.flags & kDrawNoGeometry)) {
          // S101 lane 11: a draw whose bytes this frame does not hold (the ring did not have them) says so: no geometry, never a stale offset.
          const bool vertex_ok = !d.vertex_bytes || (frame.geometry.count(d.vertex_offset) && plan.geometry_offset.count(d.vertex_offset));
          const bool index_ok = !d.index_bytes || (frame.geometry.count(d.index_offset) && plan.geometry_offset.count(d.index_offset));
          if (!vertex_ok || !index_ok) {
            d.flags |= kDrawNoGeometry;
          } else {
            if (d.vertex_bytes) d.vertex_offset = plan.geometry_offset.at(d.vertex_offset);
            if (d.index_bytes) d.index_offset = plan.geometry_offset.at(d.index_offset);
          }
          std::memcpy(data[s].data() + i * sizeof(Draw), &d, sizeof(Draw));
        }
      }
    } else if (e.id == kSecContents && e.element_size == sizeof(ContentRef)) {
      // S101 lane 11: a content ref the ring did not have is dropped (the image keeps whatever it held), never left pointing at a stale offset.
      std::vector<uint8_t> kept;
      uint64_t kept_count = 0;
      for (uint64_t i = 0; i < e.count; ++i) {
        ContentRef r;
        std::memcpy(&r, data[s].data() + i * sizeof(ContentRef), sizeof(ContentRef));
        auto it = plan.texture_offset.find(r.offset);
        if (!frame.textures.count(r.offset) || it == plan.texture_offset.end()) continue;
        r.offset = it->second;
        const auto* p = reinterpret_cast<const uint8_t*>(&r);
        kept.insert(kept.end(), p, p + sizeof(r));
        ++kept_count;
      }
      data[s] = std::move(kept);
      entries[s].count = kept_count;
      if (first) {
        for (size_t k = 0; k < plan.extra_refs.size(); ++k) {
          ContentRef r;
          r.image = plan.extra_refs[k].first;
          r.offset = plan.texture_offset.at(plan.extra_refs[k].second);
          r.size = plan.extra_sizes[k];
          const auto* p = reinterpret_cast<const uint8_t*>(&r);
          data[s].insert(data[s].end(), p, p + sizeof(r));
        }
        entries[s].count += plan.extra_refs.size();
      }
    }
  }
  // A frame with no contents section still needs one to carry the baseline.
  if (first && !plan.extra_refs.empty()) {
    bool has = false;
    for (const SectionEntry& e : entries) has |= e.id == kSecContents;
    if (!has) {
      SectionEntry e;
      e.id = kSecContents;
      e.element_size = sizeof(ContentRef);
      e.count = plan.extra_refs.size();
      std::vector<uint8_t> d;
      for (size_t k = 0; k < plan.extra_refs.size(); ++k) {
        ContentRef r;
        r.image = plan.extra_refs[k].first;
        r.offset = plan.texture_offset.at(plan.extra_refs[k].second);
        r.size = plan.extra_sizes[k];
        const auto* p = reinterpret_cast<const uint8_t*>(&r);
        d.insert(d.end(), p, p + sizeof(r));
      }
      entries.push_back(e);
      data.push_back(std::move(d));
      h.section_count++;
    }
  }
  h.capture_frame = index;
  std::vector<uint8_t> out(sizeof(FrameFileHeader) + entries.size() * sizeof(SectionEntry));
  for (size_t s = 0; s < entries.size(); ++s) {
    out.resize((out.size() + 7) & ~size_t(7));
    entries[s].offset = out.size();
    out.insert(out.end(), data[s].begin(), data[s].end());
  }
  std::memcpy(out.data(), &h, sizeof(h));
  for (size_t s = 0; s < entries.size(); ++s)
    std::memcpy(out.data() + sizeof(FrameFileHeader) + s * sizeof(SectionEntry), &entries[s], sizeof(SectionEntry));
  return out;
}

std::string LocalStamp(const std::time_t t, bool compact, int& utc_offset_min) {
  std::tm local{}, gm{};
#if defined(_WIN32)
  localtime_s(&local, &t);
  gmtime_s(&gm, &t);
#else
  localtime_r(&t, &local);
  gmtime_r(&t, &gm);
#endif
  gm.tm_isdst = local.tm_isdst;
  utc_offset_min = int(std::difftime(std::mktime(&local), std::mktime(&gm)) / 60.0);
  char b[40];
  std::strftime(b, sizeof(b), compact ? "%Y%m%d_%H%M%S" : "%Y-%m-%d %H:%M:%S", &local);
  return b;
}

std::string TimingCsv(const Snapshot& s) {
  std::set<uint64_t> captured;
  for (const auto& f : s.frames) captured.insert(f->guest_frame);
  std::string o = std::string(kTimingHeader) + "\n";
  auto cell = [](double v) {
    if (!(v >= 0)) return std::string();  // not measured: empty, never 0
    char b[32];
    std::snprintf(b, sizeof(b), "%.3f", v);
    return std::string(b);
  };
  for (const TimingRow& r : s.timing) {
    o += std::to_string(r.guest_frame) + "," + cell(r.step_ms) + "," + cell(r.render_ms) + "," + cell(r.present_ms) + "," + cell(r.shown_ms) + "," +
         (captured.count(r.guest_frame) ? "1" : "0") + "\n";
  }
  return o;
}

// S101 lane 19: a PNG that ends with its IEND chunk (length 0, "IEND", CRC AE426082) was written to the end.
bool PngComplete(const std::vector<uint8_t>& b) {
  static const uint8_t tail[12] = {0, 0, 0, 0, 'I', 'E', 'N', 'D', 0xAE, 0x42, 0x60, 0x82};
  return b.size() > 20 && std::memcmp(b.data() + b.size() - 12, tail, 12) == 0;
}

Entry MemoryEntry(std::string path, Part part, std::string bytes) {
  Entry e;
  e.path = std::move(path);
  e.part = part;
  e.size = bytes.size();
  auto shared = std::make_shared<std::string>(std::move(bytes));
  e.produce = [shared](const Sink& sink) {
    if (!shared->empty()) sink(shared->data(), shared->size());
  };
  return e;
}

}  // namespace

CaseResult WriteCase(const Snapshot& snap, CaseRequest req) {
  CaseResult result;
  const auto fail = [&](const std::string& why) {
    result.ok = false;
    result.error = why;
    result.ms_from_trigger = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - req.triggered).count();
    return result;
  };
  if (snap.frames.empty()) return fail("the ring holds no frames");
  Manifest m = req.manifest;
  const bool with_game = !m.ours_only;

  // ---- the plan: every distinct content once, in a fixed order ----
  Plan plan;
  std::set<uint64_t> stale_keys;  // S101 lane 11: texture contents that do not fit the image record the zip carries
  std::unordered_map<uint32_t, ImageRecord> record_of;
  for (const ImageRecord& r : snap.images) record_of[r.id] = r;
  for (const auto& f : snap.frames) {
    result.unresolved += f->unresolved;
    std::vector<uint64_t> keys;
    for (const auto& kv : f->geometry) keys.push_back(kv.first);
    std::sort(keys.begin(), keys.end());
    for (uint64_t k : keys)
      if (!plan.geometry_offset.count(k)) {
        plan.geometry_offset[k] = plan.geometry_bytes;
        plan.geometry_order.push_back(f->geometry.at(k));
        plan.geometry_bytes += f->geometry.at(k)->bytes.size();
      }
    keys.clear();
    for (const auto& ic : f->contents) {
      const auto tex = f->textures.find(ic.second);
      if (tex == f->textures.end() || plan.texture_offset.count(ic.second) || stale_keys.count(ic.second)) continue;
      const auto rec = record_of.find(ic.first);
      if (rec == record_of.end() || !ContentFitsRecord(*tex->second, rec->second)) {
        stale_keys.insert(ic.second);
        ++result.stale_contents;
      }
    }
    for (const auto& kv : f->textures) keys.push_back(kv.first);
    std::sort(keys.begin(), keys.end());
    for (uint64_t k : keys)
      if (!plan.texture_offset.count(k) && !stale_keys.count(k)) {
        plan.texture_offset[k] = plan.texture_bytes;
        plan.texture_order.push_back(f->textures.at(k));
        plan.texture_bytes += f->textures.at(k)->bytes.size();
      }
  }
  {
    // The state before the window: images frame 0 does not itself fill take their baseline content.
    const RingFrame& first = *snap.frames.front();
    std::set<uint32_t> own;
    for (const auto& c : first.contents)
      if (!stale_keys.count(c.second)) own.insert(c.first);  // (a stale content of frame 0 is not left in the zip: the baseline may fill the image)
    uint64_t synthetic_key = ~uint64_t(0);  // keys no sink hands out
    for (const auto& [image, content] : snap.baseline) {
      if (own.count(image)) continue;
      const auto rec = record_of.find(image);
      if (rec == record_of.end() || !ContentFitsRecord(*content, rec->second)) {
        ++result.stale_contents;
        continue;
      }
      const uint64_t key = synthetic_key--;
      plan.texture_offset[key] = plan.texture_bytes;
      plan.texture_order.push_back(content);
      plan.texture_bytes += content->bytes.size();
      plan.extra_refs.emplace_back(image, key);
      plan.extra_sizes.push_back(content->bytes.size());
    }
  }

  // ---- S101 lane 19: what the window's frames let the game side fill in now (names counts, the recorder's rows) ----
  {
    const uint64_t first_gf = snap.frames.front()->guest_frame, last_gf = snap.frames.back()->guest_frame;
    if (req.finish_manifest) req.finish_manifest(m, first_gf, last_gf);
    if (req.inputs_rec.empty() && req.inputs_for_window) {
      uint32_t rows = 0;
      std::string text = req.inputs_for_window(first_gf, last_gf);
      // (the cut file is counted here: S / K / C rows inside the window)
      for (size_t at = 0; at < text.size();) {
        size_t eol = text.find('\n', at);
        if (eol == std::string::npos) eol = text.size();
        if (eol - at > 2 && text[at + 1] == ' ' && (text[at] == 'S' || text[at] == 'K' || text[at] == 'C') && text[at + 2] >= '0' && text[at + 2] <= '9') {
          const uint64_t f = std::strtoull(text.c_str() + at + 2, nullptr, 10);
          if (f >= first_gf && f <= last_gf) ++rows;
        }
        at = eol + 1;
      }
      if (!text.empty()) {
        req.inputs_rec = std::move(text);
        m.way_back.inputs_rows = rows;
      }
    }
    // timing columns the window never measured: a reason, not a zero
    for (const char* col : {"render_ms", "present_ms", "shown_ms"}) {
      bool any = false;
      for (const TimingRow& r : snap.timing) {
        const double v = std::string(col) == "render_ms" ? r.render_ms : std::string(col) == "present_ms" ? r.present_ms : r.shown_ms;
        if (v >= 0) { any = true; break; }
      }
      if (!any && !m.IsUnavailable(std::string("timing.") + col)) {
        m.Unavailable(std::string("timing.") + col,
                      std::string(col) == "shown_ms" ? "no part of the renderer records when a picture reached the screen (a present is not a shown frame)"
                                                     : "no frame of the window was timed by the render thread (owner renderer off, or every frame skipped)");
      }
    }
    info::Complete(m);
  }

  // ---- the entries ----
  std::vector<Entry> entries;
  entries.push_back({});  // manifest.json goes first; filled in after the rest is known
  entries.push_back(MemoryEntry(kTimingName, Part::Ours, TimingCsv(snap)));
  if (req.log_text.empty() && !req.log_path.empty()) if (g_drain_log) g_drain_log();  // lane 302: the log file is written by a thread; the zip reads it next
  if (req.log_text.empty() && !req.log_path.empty()) req.log_text = RecentLogText(req.log_path, req.log_window_s, req.log_now);  // (on this worker thread)
  entries.push_back(MemoryEntry(kLogName, Part::Ours, req.log_text));
  entries.push_back(MemoryEntry(kInputsName, Part::Ours,
                                req.inputs_rec.empty() ? std::string("# case zip: no controller recording was running (replay_record off)\n") : req.inputs_rec));
  entries.push_back(MemoryEntry(kStatesName, Part::Ours, req.states_text));
  uint64_t capture_bytes = 0;
  if (with_game) {
    std::vector<uint8_t> picture = req.picture_png;
    if (picture.empty() && !req.picture_path.empty()) {
      for (int attempt = 0; attempt < 100 && picture.empty(); ++attempt) {
        std::error_code ec;
        if (std::filesystem::exists(req.picture_path, ec)) {
          std::FILE* f =
#if defined(_WIN32)
              _wfopen(req.picture_path.c_str(), L"rb");
#else
              std::fopen(req.picture_path.c_str(), "rb");
#endif
          if (f) {
            std::fseek(f, 0, SEEK_END);
            const long n = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            picture.resize(size_t(std::max(n, 0L)));
            if (n <= 0 || std::fread(picture.data(), 1, picture.size(), f) != picture.size()) picture.clear();
            if (!picture.empty() && !PngComplete(picture)) picture.clear();  // S101 lane 19: a partly written PNG is waited for, never zipped
            std::fclose(f);
          }
        }
        if (picture.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
    }
    if (req.picture_temp && !req.picture_path.empty()) {
      std::error_code rm;
      std::filesystem::remove(req.picture_path, rm);
    }
    result.has_picture = !picture.empty();
    if (!picture.empty()) entries.push_back(MemoryEntry(kPictureName, Part::Game, std::string(picture.begin(), picture.end())));

    // capture\images.bin
    {
      ImageTableHeader ih;
      ih.count = uint32_t(snap.images.size());
      std::string b(sizeof(ih) + snap.images.size() * sizeof(ImageRecord), '\0');
      std::memcpy(b.data(), &ih, sizeof(ih));
      if (!snap.images.empty()) std::memcpy(b.data() + sizeof(ih), snap.images.data(), snap.images.size() * sizeof(ImageRecord));
      Entry e = MemoryEntry("capture/images.bin", Part::Game, std::move(b));
      capture_bytes += e.size;
      entries.push_back(std::move(e));
    }
    auto blob = [&](const char* path, const std::vector<ContentPtr>& order, uint64_t total) {
      Entry e;
      e.path = path;
      e.part = Part::Game;
      e.size = total;
      e.produce = [&order](const Sink& sink) {
        for (const ContentPtr& c : order)
          if (!c->bytes.empty()) sink(c->bytes.data(), c->bytes.size());
      };
      capture_bytes += total;
      entries.push_back(std::move(e));
    };
    blob("capture/geometry.bin", plan.geometry_order, plan.geometry_bytes);
    blob("capture/textures.bin", plan.texture_order, plan.texture_bytes);
    for (size_t i = 0; i < snap.frames.size(); ++i) {
      Entry e;
      char name[64];
      std::snprintf(name, sizeof(name), "capture/frames/frame_%06u.bin", unsigned(i));
      e.path = name;
      e.part = Part::Game;
      const RingFrame* frame = snap.frames[i].get();
      e.size = RewriteFrame(*frame, uint32_t(i), plan, i == 0).size();
      e.produce = [frame, i, &plan](const Sink& sink) {
        const std::vector<uint8_t> b = RewriteFrame(*frame, uint32_t(i), plan, i == 0);
        sink(b.data(), b.size());
      };
      capture_bytes += e.size;
      entries.push_back(std::move(e));
    }
    // S101 lane26: the window's scene records beside the frames: capture\scene\frame_NNNNNN.rsc, NNNNNN = the index of the ring frame (not a
    // read-back part) that carries the record's guest frame, so a reader pairs frames\frame_N.bin with scene\frame_N.rsc. Bytes are produced here,
    // on the worker (the game thread never serialised). A record no frame carries is left out and counted.
    {
      std::unordered_map<uint64_t, size_t> frame_of;  // guest frame -> index of the last non-part ring frame with it
      for (size_t i = 0; i < snap.frames.size(); ++i)
        if (!snap.frames[i]->part) frame_of[snap.frames[i]->guest_frame] = i;
      std::set<size_t> used;
      for (size_t si = snap.scene.size(); si-- > 0;) {  // newest first: the last record of a guest frame wins its frame
        const SceneEntry& se = snap.scene[si];
        auto it = frame_of.find(se.guest_frame);
        if (it == frame_of.end() || !se.produce || used.count(it->second)) {
          ++result.scene_unmatched;
          continue;
        }
        used.insert(it->second);
        Entry e;
        char name[64];
        std::snprintf(name, sizeof(name), "capture/scene/frame_%06u.rsc", unsigned(it->second));
        e.path = name;
        e.part = Part::Game;
        e.size = se.produce().size();
        const auto produce = se.produce;
        e.produce = [produce](const Sink& sink) {
          const std::vector<uint8_t> b = produce();
          sink(b.data(), b.size());
        };
        capture_bytes += e.size;
        entries.push_back(std::move(e));
        ++result.scene_records;
      }
    }
  }

  // ---- hash every entry (pass 1), then the manifest, then write (pass 2) ----
  for (size_t i = 1; i < entries.size(); ++i) {
    XXH3_state_t* st = XXH3_createState();
    XXH3_64bits_reset(st);
    uint64_t seen = 0;
    entries[i].produce([&](const void* d, size_t n) {
      XXH3_64bits_update(st, d, n);
      seen += n;
    });
    entries[i].xxh3 = XXH3_64bits_digest(st);
    XXH3_freeState(st);
    if (seen != entries[i].size) {
      return fail("entry " + entries[i].path + " produced " + std::to_string(seen) + " bytes, declared " + std::to_string(entries[i].size));
    }
  }

  const std::time_t now = std::time(nullptr);
  m.created = LocalStamp(now, false, m.utc_offset_min);
  const std::string stamp = LocalStamp(now, true, m.utc_offset_min);
  // S101 lane 11: two triggers in one second must not share a file: milliseconds and a process counter are in the id, and an existing file
  // (another process, the same millisecond) bumps the counter until the name is free.
  const int ms = int(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count() % 1000);
  static std::atomic<unsigned> g_case_counter{0};
  std::error_code ec;
  std::filesystem::create_directories(req.out_dir, ec);
  std::filesystem::path zip_path;
  for (int tries = 0; tries < 1000; ++tries) {
    char tail[24];
    std::snprintf(tail, sizeof(tail), "_%03d_%u", ms, g_case_counter.fetch_add(1));
    m.id = "case_" + stamp + tail + "_" + req.device_tag + "_" + TriggerName(m.trigger);
    zip_path = req.out_dir / (m.id + ".zip");
    std::error_code ex;
    if (!std::filesystem::exists(zip_path, ex) && !std::filesystem::exists(zip_path.string() + ".part", ex)) break;
  }
  m.window.unresolved = result.unresolved;
  m.window.baseline_dropped = snap.baseline_dropped;
  const RingFrame& first = *snap.frames.front();
  const RingFrame& last = *snap.frames.back();
  m.window.seconds = last.t_s - first.t_s;
  m.window.frames = uint32_t(snap.frames.size());
  m.window.first_guest_frame = first.guest_frame;
  m.window.capture_frames = with_game ? uint32_t(snap.frames.size()) : 0;
  m.window.capture_bytes = with_game ? capture_bytes : 0;
  if (!m.where.guest_frame) m.where.guest_frame = last.guest_frame;
  m.way_back.first_frame = first.guest_frame;
  m.way_back.moment_frame = last.guest_frame;
  m.files.clear();
  m.files.push_back({kManifestName, Part::Ours, 0, 0});
  for (size_t i = 1; i < entries.size(); ++i) m.files.push_back({entries[i].path, entries[i].part, entries[i].size, entries[i].xxh3});
  result.manifest_json = ToJson(m);
  entries[0] = MemoryEntry(kManifestName, Part::Ours, result.manifest_json);

  ZipWriter zip;
  if (!zip.Begin(zip_path)) return fail(zip.error());
  for (Entry& e : entries) {
    if (!zip.BeginEntry(e.path, e.size)) return fail(zip.error());
    e.produce([&](const void* d, size_t n) { zip.Write(d, n); });
    if (!zip.EndEntry()) return fail(zip.error());
  }
  if (!zip.Finish()) return fail(zip.error());

  result.ok = true;
  result.path = zip_path;
  result.zip_bytes = std::filesystem::file_size(zip_path, ec);
  result.frames = uint32_t(snap.frames.size());
  result.capture_bytes = capture_bytes;
  result.ms_from_trigger = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - req.triggered).count();
  return result;
}

std::string RecentLogText(const std::filesystem::path& path, double window_s, std::time_t now_t) {
  std::FILE* f =
#if defined(_WIN32)
      _wfopen(path.c_str(), L"rb");
#else
      std::fopen(path.c_str(), "rb");
#endif
  if (!f) return "(run log not readable)\n";
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  const long from = std::max<long>(0, size - (1 << 20));
  std::fseek(f, from, SEEK_SET);
  std::string text(size_t(std::max<long>(size - from, 0)), '\0');
  const size_t got = text.empty() ? 0 : std::fread(text.data(), 1, text.size(), f);
  std::fclose(f);
  text.resize(got);
  if (!now_t) now_t = std::time(nullptr);
  std::tm now_tm{};
#if defined(_WIN32)
  localtime_s(&now_tm, &now_t);
#else
  localtime_r(&now_t, &now_tm);
#endif
  const int now_s = now_tm.tm_hour * 3600 + now_tm.tm_min * 60 + now_tm.tm_sec;
  std::string out;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t end = text.find('\n', pos);
    if (end == std::string::npos) end = text.size();
    const std::string line = text.substr(pos, end - pos);
    pos = end + 1;
    int h = 0, m = 0, sec = 0;
    if (line.size() < 40 || line[0] != '[' || std::sscanf(line.substr(12, 8).c_str(), "%d:%d:%d", &h, &m, &sec) != 3) continue;
    int age = now_s - (h * 3600 + m * 60 + sec);
    if (age < 0) age += 24 * 3600;
    if (age <= window_s) out += line + "\n";
  }
  return out;
}

// ---- the worker: one thread, a bounded queue (S101 lane 11: two triggers could race two threads and share a file name) ----------------
namespace {
struct Job {
  Snapshot snapshot;
  CaseRequest request;
  std::function<void(const CaseResult&)> done;
};
struct Worker {
  std::mutex mu;
  std::condition_variable cv, idle_cv;
  std::deque<Job> queue;
  bool busy = false, started = false;
};
Worker& W() {
  static Worker* w = new Worker;  // leaked on purpose: the worker thread may outlive static destruction
  return *w;
}
constexpr size_t kQueueMax = 3;

void WorkerLoop() {
  Worker& w = W();
  for (;;) {
    Job job;
    {
      std::unique_lock lock(w.mu);
      w.cv.wait(lock, [&] { return !w.queue.empty(); });
      job = std::move(w.queue.front());
      w.queue.pop_front();
      w.busy = true;
    }
    CaseResult res;
    try {
      res = WriteCase(job.snapshot, std::move(job.request));
    } catch (const std::exception& e) {
      res.error = std::string("exception: ") + e.what();
    }
    if (job.done) job.done(res);
    {
      std::lock_guard lock(w.mu);
      w.busy = false;
    }
    w.idle_cv.notify_all();
  }
}
}  // namespace

void WriteCaseAsync(Snapshot snapshot, CaseRequest request, std::function<void(const CaseResult&)> done) {
  Worker& w = W();
  {
    std::lock_guard lock(w.mu);
    if (w.queue.size() >= kQueueMax) {
      CaseResult r;
      r.error = "case queue full (a zip is still being written)";
      if (done) done(r);
      return;
    }
    w.queue.push_back({std::move(snapshot), std::move(request), std::move(done)});
    if (!w.started) {
      w.started = true;
      std::thread(WorkerLoop).detach();
    }
  }
  w.cv.notify_one();
}

void WaitIdle() {
  Worker& w = W();
  std::unique_lock lock(w.mu);
  w.idle_cv.wait(lock, [&] { return w.queue.empty() && !w.busy; });
}

}  // namespace rdr_case
