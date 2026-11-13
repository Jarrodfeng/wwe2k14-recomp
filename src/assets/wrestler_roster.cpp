// See wrestler_roster.h. Session 101, lane 45.
// stb_image is compiled INTO this translation unit, static (no stbi_* symbol leaves it), exactly as src\assets\crowd_body.cpp does: the game exe and the phone library
// must not export stbi_* symbols. pac.h needs stbi_zlib_decode_buffer from the including translation unit.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
#if __has_include("stb_image.h")
#include "stb_image.h"
#else
#include "../../thirdparty/rexglue-sdk-src/thirdparty/stb/stb_image.h"  // (main checkout layout; the build gives the folder with -I)
#endif
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include "wrestler_roster.h"

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <tuple>
#include <vector>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <system_error>
#include <type_traits>
#include <mutex>
#include <thread>

#include "pac.h"

namespace assets::wrestler_roster {
namespace {
std::string PacRel(int id) {
  char b[64];
  std::snprintf(b, sizeof b, "pac\\ch\\ch%03d.pac", id);
  return b;
}
bool ParseChName(const std::string& n, int& id) {
  if (n.size() != 9) return false;
  std::string l = n;
  for (char& c : l) c = char(std::tolower(static_cast<unsigned char>(c)));
  if (l.compare(0, 2, "ch") != 0 || l.compare(5, 4, ".pac") != 0) return false;
  int v = 0;
  for (int i = 2; i < 5; ++i) {
    if (l[size_t(i)] < '0' || l[size_t(i)] > '9') return false;
    v = v * 10 + (l[size_t(i)] - '0');
  }
  id = v;
  return true;
}
// The entry ids that are models the stream draws (the body, two accessory sets, hair / hats and the other pieces).
bool IsEntryWanted(uint32_t id) { return id == 0 || id == 26 || id == 27 || (id >= 10000 && id < 40000); }
}  // namespace

struct Roster::Impl {
  launcher::GameFiles files;
  std::mutex mu;  // LoadModel (any thread) shares the open pac and the file reader
  pac::Pac open_pac;
  std::atomic<int> open_pac_id{-1};   // (lane 142: the loader thread reads it to pick the job of the open pac)
  std::chrono::steady_clock::time_point pac_last_use{};   // lane 142
  pac::Pac edit_pac;       // lane 153 (review S2): the edNN.pac LoadEditLeaf opened last, kept so the next leaf of the same pac does not inflate it again
  int edit_pac_id = -1;
  std::thread worker;
  // lane 142: the single loader thread (started by the first LoadModelAsync, stopped in ~Roster)
  struct Job { int index; bool keep; std::promise<std::shared_ptr<Model>> done; };
  std::mutex qmu;
  std::condition_variable qcv;
  std::vector<Job> jobs;
  bool stop = false;
  std::atomic<int> pac_opens{0};
  std::atomic<bool> arrival_order{false};
  std::thread loader;
};

Roster::Roster() : impl_(std::make_unique<Impl>()) {}
Roster::~Roster() {
  Join();
  if (impl_->loader.joinable()) {   // lane 142
    { std::lock_guard<std::mutex> lk(impl_->qmu); impl_->stop = true; }
    impl_->qcv.notify_all();
    impl_->loader.join();
  }
}

void Roster::Join() {
  if (impl_->worker.joinable()) impl_->worker.join();
}

bool Roster::Build(const std::string& game_root, std::string& error) {
  const auto t0 = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> lock(impl_->mu);
  root_ = game_root;
  entries_.clear();
  pacs_ = 0;
  impl_->files.SetRoot(std::filesystem::u8path(game_root));
  if (!impl_->files.Exists("pac\\ch")) return error = "no pac\\ch folder in the game files at " + game_root, false;
  std::vector<int> ids;
  for (const std::string& n : impl_->files.List("pac\\ch")) {
    int id;
    if (ParseChName(n, id)) ids.push_back(id);
  }
  std::sort(ids.begin(), ids.end());
  if (ids.empty()) return error = "no pac\\ch\\chNNN.pac in the game folder", false;
  for (int id : ids) {
    pac::Pac p;
    std::string e;
    if (!pac::Open(impl_->files, PacRel(id), p, e)) continue;  // (a pac that does not open is skipped; the census says how many were read)
    ++pacs_;
    int attire = 0;
    for (const pac::Pach& h : p.pachs) {
      if (!pac::Find(h, 0)) continue;
      for (const pac::Ent& en : h.ents) {
        if (!IsEntryWanted(en.id)) continue;
        Bytes blob = pac::Decode(p.bytes.data() + en.off, en.size);
        if (blob.size() < 8 || std::memcmp(blob.data(), "JBOY", 4) != 0) continue;
        jboy_parts::Parts jb;
        std::string e2;
        if (!jboy_parts::Parse(blob, jb, e2)) continue;
        Entry c;
        c.pac = id, c.attire = attire, c.entry = en.id, c.node_count = jb.node_count;
        for (const jboy_parts::Part& pt : jb.parts) {
          PartInfo pi;
          pi.vertices = pt.vertices, pi.index_count = pt.index_count + (pt.index_groups > 1 ? pt.index_groups - 1 : 0) /* lane 179: the DRAW count: restart markers between index groups */, pi.palette_count = pt.palette_count, pi.ok = pt.ok;
          std::memcpy(pi.palette, pt.palette, sizeof pi.palette);
          c.parts.push_back(pi);
        }
        entries_.push_back(std::move(c));
      }
      ++attire;
    }
  }
  seconds_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  if (entries_.empty()) return error = "the roster is empty", false;
  state_ = 1;
  return true;
}

void Roster::BuildAsync(const std::string& game_root) {
  Join();
  state_ = 0;
  impl_->worker = std::thread([this, game_root] {
    std::string e;
    if (Build(game_root, e)) state_ = 1;
    else error_ = e, state_ = 2;
  });
}

std::shared_future<std::shared_ptr<Model>> Roster::LoadModelAsync(int index, bool keep_pac) {   // lane 142
  Impl::Job job{index, keep_pac, {}};
  std::shared_future<std::shared_ptr<Model>> fut = job.done.get_future().share();
  {
    std::lock_guard<std::mutex> lk(impl_->qmu);
    impl_->jobs.push_back(std::move(job));
    if (!impl_->loader.joinable()) {
      impl_->loader = std::thread([this] {
        for (;;) {
          Impl::Job j;
          {
            std::unique_lock<std::mutex> lk(impl_->qmu);
            impl_->qcv.wait(lk, [this] { return impl_->stop || !impl_->jobs.empty(); });
            if (impl_->stop && impl_->jobs.empty()) return;
            // the job of the pac that is open first (no second unpack), else the lowest (pac, attire, entry): the same order every run
            size_t pick = 0;
            bool found = impl_->arrival_order;   // (test plant: first come, first served)
            for (size_t k = 0; k < impl_->jobs.size() && !found; ++k) {
              const int ix = impl_->jobs[k].index;
              if (ix < 0 || size_t(ix) >= entries_.size()) { pick = k; found = true; break; }
              const Entry& e = entries_[size_t(ix)];
              if (e.pac == impl_->open_pac_id) { pick = k; found = true; break; }
            }
            if (!found) {
              auto key = [this](size_t k) {
                const int ix = impl_->jobs[k].index;
                const Entry& e = entries_[size_t(ix)];
                return std::make_tuple(e.pac, e.attire, ix);
              };
              for (size_t k = 1; k < impl_->jobs.size(); ++k)
                if (key(k) < key(pick)) pick = k;
            }
            j = std::move(impl_->jobs[pick]);
            impl_->jobs.erase(impl_->jobs.begin() + std::ptrdiff_t(pick));
          }
          std::string err;
          std::shared_ptr<Model> m;
          try { m = LoadModel(j.index, err, j.keep); } catch (...) { m.reset(); }
          j.done.set_value(m);
        }
      });
    }
  }
  impl_->qcv.notify_one();
  return fut;
}

int Roster::OpenPacId() const { return impl_->open_pac_id; }
int Roster::PacOpens() const { return impl_->pac_opens; }
void Roster::SetArrivalOrderForTest(bool on) { impl_->arrival_order = on; }
void Roster::LockedForTest(const std::function<void()>& f) {
  std::lock_guard<std::mutex> lock(impl_->mu);
  f();
}

void Roster::TrimIdle(double seconds) {   // lane 142
  std::unique_lock<std::mutex> lock(impl_->mu, std::try_to_lock);
  if (!lock.owns_lock() || impl_->open_pac_id < 0) return;
  if (std::chrono::duration<double>(std::chrono::steady_clock::now() - impl_->pac_last_use).count() < seconds) return;
  impl_->open_pac = pac::Pac{};
  impl_->open_pac_id = -1;
}

// ---- lane 153: the editor's part leaves ---------------------------------------------------------------------------------------------------------------------------------
namespace {
constexpr uint32_t kEditCacheMagic = 0x49574143u, kEditCacheVersion = 2;   // 'CAWI'; 2 = lane 169 adds the editor's backdrop pac (bg66.pac, pseudo id 1066)
// lane 169: pseudo pac id 1066 = pac\bg\bg66.pac (the Create a Superstar editor's backdrop: CAS_wall, CAS_moya); 0..99 = pac\edit\edNN.pac
void EditPacRel(int id, char* rel, size_t n) {
  if (id == Roster::kCawBackdropPacId) std::snprintf(rel, n, "pac\\bg\\bg66.pac");
  else std::snprintf(rel, n, "pac\\edit\\ed%02d.pac", id);
}
static_assert(std::is_trivially_copyable<Roster::EditPart>::value, "the cache writes EditPart as raw bytes");
// The cache key: every edNN.pac's size and last-write time (ids and counts only are cached, never game bytes).
uint64_t EditSignature(const std::string& game_root) {
  uint64_t h = 1469598103934665603ull;
  auto mix = [&](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
  std::error_code ec;
  std::vector<std::pair<std::string, std::pair<uint64_t, uint64_t>>> files;
  for (const auto& de : std::filesystem::directory_iterator(std::filesystem::u8path(game_root) / "pac" / "edit", ec)) {
    const std::string n = de.path().filename().string();
    if (n.size() != 8 || n.compare(0, 2, "ed") != 0) continue;
    std::error_code e2;
    files.push_back({n, {uint64_t(de.file_size(e2)), uint64_t(de.last_write_time(e2).time_since_epoch().count())}});
  }
  std::sort(files.begin(), files.end());
  for (const auto& f : files) {
    for (char c : f.first) mix(uint8_t(c));
    mix(f.second.first), mix(f.second.second);
  }
  mix(files.size());
  {   // lane 169: bg66.pac (the editor's backdrop pieces) is indexed too: its size and time key the cache
    std::error_code e3;
    const auto bp = std::filesystem::u8path(game_root) / "pac" / "bg" / "bg66.pac";
    mix(uint64_t(std::filesystem::file_size(bp, e3))), mix(uint64_t(std::filesystem::last_write_time(bp, e3).time_since_epoch().count()));
  }
  return h;
}
}  // namespace

void Roster::DropEditCache() {
  std::lock_guard<std::mutex> lock(impl_->mu);
  impl_->edit_pac = pac::Pac{};
  impl_->edit_pac_id = -1;
}

bool Roster::BuildEdit(const std::string& game_root, std::string& error, const std::string& cache_path) {
  const auto t0 = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> lock(impl_->mu);
  root_ = game_root;
  edit_parts_.clear();
  pacs_ = 0;
  impl_->files.SetRoot(std::filesystem::u8path(game_root));
  const uint64_t signature = cache_path.empty() ? 0 : EditSignature(game_root);
  if (!cache_path.empty()) {   // a cache hit: the index without reading one pac
    if (FILE* f = std::fopen(cache_path.c_str(), "rb")) {
      uint32_t head[3] = {};
      uint64_t sig = 0;
      if (std::fread(head, 4, 3, f) == 3 && std::fread(&sig, 8, 1, f) == 1 && head[0] == kEditCacheMagic && head[1] == kEditCacheVersion && sig == signature && head[2] > 0 && head[2] < 4000000u) {
        edit_parts_.resize(head[2]);
        if (std::fread(edit_parts_.data(), sizeof(EditPart), head[2], f) == head[2]) {
          std::fclose(f);
          seconds_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
          state_ = 1;
          return true;
        }
        edit_parts_.clear();
      }
      std::fclose(f);
    }
  }
  if (!impl_->files.Exists("pac\\edit")) return error = "no pac\\edit folder in the game files at " + game_root, false;
  std::vector<int> ids;
  for (const std::string& n : impl_->files.List("pac\\edit")) {
    std::string l = n;
    for (char& c : l) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (l.size() == 8 && l.compare(0, 2, "ed") == 0 && l.compare(4, 4, ".pac") == 0 && std::isdigit(static_cast<unsigned char>(l[2])) && std::isdigit(static_cast<unsigned char>(l[3]))) ids.push_back((l[2] - '0') * 10 + (l[3] - '0'));
  }
  std::sort(ids.begin(), ids.end());
  if (ids.empty()) return error = "no pac\\edit\\edNN.pac in the game folder", false;
  std::vector<uint64_t> seen;   // FNV-1a of each distinct leaf blob (+ its size)
  uint32_t groups = 0;
  ids.push_back(kCawBackdropPacId);   // lane 169: the editor's backdrop pieces (bg66.pac) after the edNN.pacs; a missing file is skipped by the open below
  for (int id : ids) {
    pac::Pac p;
    std::string e;
    char rel[64];
    EditPacRel(id, rel, sizeof rel);
    if (!pac::Open(impl_->files, rel, p, e)) continue;
    ++pacs_;
    for (size_t k = 0; k < p.pachs.size(); ++k) {
      for (const pac::Ent& en : p.pachs[k].ents) {
        Bytes blob = pac::Decode(p.bytes.data() + en.off, en.size);
        if (blob.size() < 8 || std::memcmp(blob.data(), "JBOY", 4) != 0) continue;
        uint64_t h = 1469598103934665603ull ^ blob.size();
        for (uint8_t b : blob) h = (h ^ b) * 1099511628211ull;
        if (std::find(seen.begin(), seen.end(), h) != seen.end()) continue;
        jboy_parts::Parts jb;
        std::string e2;
        if (!jboy_parts::Parse(blob, jb, e2)) continue;
        seen.push_back(h);
        for (const jboy_parts::Part& pt : jb.parts) {
          if (!pt.ok) continue;
          EditPart ep;
          ep.pac = uint16_t(id), ep.pach = uint16_t(k), ep.leaf = en.id, ep.mesh = pt.mesh_index;
          ep.vertices = pt.vertices, ep.index_count = pt.index_count, ep.palette_count = pt.palette_count, ep.group = groups;
          edit_parts_.push_back(ep);
        }
        ++groups;
      }
    }
  }
  seconds_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  if (edit_parts_.empty()) return error = "no edit part leaf was read", false;
  if (!cache_path.empty()) {   // best effort: a failed write only means the next start builds again
    if (FILE* f = std::fopen(cache_path.c_str(), "wb")) {
      const uint32_t head[3] = {kEditCacheMagic, kEditCacheVersion, uint32_t(edit_parts_.size())};
      std::fwrite(head, 4, 3, f);
      std::fwrite(&signature, 8, 1, f);
      std::fwrite(edit_parts_.data(), sizeof(EditPart), edit_parts_.size(), f);
      std::fclose(f);
    }
  }
  state_ = 1;
  return true;
}

void Roster::BuildEditAsync(const std::string& game_root, const std::string& cache_path) {
  Join();
  state_ = 0;
  impl_->worker = std::thread([this, game_root, cache_path] {
    std::string e;
    if (BuildEdit(game_root, e, cache_path)) state_ = 1;
    else error_ = e, state_ = 2;
  });
}

std::shared_ptr<Model> Roster::LoadEditLeaf(size_t index, std::string& error) {
  if (index >= edit_parts_.size()) return error = "no such edit part", nullptr;
  const EditPart ep = edit_parts_[index];
  std::lock_guard<std::mutex> lock(impl_->mu);
  if (impl_->edit_pac_id != int(ep.pac)) {   // lane 153 (review S2): the pac stays open for the next leaf of the same pac
    char rel[64];
    EditPacRel(int(ep.pac), rel, sizeof rel);
    impl_->edit_pac = pac::Pac{};
    impl_->edit_pac_id = -1;
    if (!pac::Open(impl_->files, rel, impl_->edit_pac, error)) return nullptr;
    impl_->edit_pac_id = int(ep.pac);
  }
  pac::Pac& p = impl_->edit_pac;
  if (ep.pach >= p.pachs.size()) return error = "archive missing", nullptr;
  const pac::Ent* en = pac::Find(p.pachs[ep.pach], ep.leaf);
  if (!en) return error = "leaf missing", nullptr;
  auto m = std::make_shared<Model>();
  m->entry_index = -1;
  m->blob = pac::Decode(p.bytes.data() + en->off, en->size);
  if (!jboy_parts::Parse(m->blob, m->parts, error)) return nullptr;
  return m;
}

std::shared_ptr<Model> Roster::LoadModel(int index, std::string& error, bool keep_pac) {
  if (index < 0 || size_t(index) >= entries_.size()) return error = "no such roster entry", nullptr;
  const Entry& C = entries_[size_t(index)];
  std::lock_guard<std::mutex> lock(impl_->mu);
  if (impl_->open_pac_id != C.pac) {
    std::string e;
    ++impl_->pac_opens;   // lane 142
    if (!pac::Open(impl_->files, PacRel(C.pac), impl_->open_pac, e)) return error = e, impl_->open_pac_id = -1, nullptr;
    impl_->open_pac_id = C.pac;
  }
  int attire = 0;
  for (const pac::Pach& h : impl_->open_pac.pachs) {
    if (!pac::Find(h, 0)) continue;
    if (attire++ != C.attire) continue;
    const pac::Ent* en = pac::Find(h, C.entry);
    if (!en) return error = "entry missing", nullptr;
    auto m = std::make_shared<Model>();
    m->entry_index = index;
    m->blob = pac::Decode(impl_->open_pac.bytes.data() + en->off, en->size);
    const bool parsed = jboy_parts::Parse(m->blob, m->parts, error);
    if (keep_pac) impl_->pac_last_use = std::chrono::steady_clock::now();   // lane 142: kept for the next load of this pac; TrimIdle lets it go
    else {
      impl_->open_pac = pac::Pac{};   // the whole unpacked pac (tens of MB) is not kept after its entry was decoded
      impl_->open_pac_id = -1;
    }
    if (!parsed) return nullptr;
    return m;
  }
  return error = "attire missing", nullptr;
}

}  // namespace assets::wrestler_roster
