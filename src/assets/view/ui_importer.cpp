// Session 98 (Superstar Importer): see ui_importer.h. Pure Dear ImGui + std; the look is the launcher's (red header, slanted red
// buttons, dark panels, the game's menu face read from the player's own files by ui.cpp).
#include "ui_importer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

#include "imgui_internal.h"
#include "json_lite.h"
#include "loader_seam.h"
#include "shot.h"
#include "stb_image.h"  // declarations only (the loader seam holds the implementation, PNG + JPEG)

#include "../char_mods.h"
#include "../id_names.h"
#include "../../importer/importer_core.h"

namespace view {
namespace ui {

namespace fs = std::filesystem;
namespace {

constexpr ImU32 kRed = IM_COL32(214, 22, 30, 255);
constexpr ImU32 kDim = IM_COL32(150, 151, 158, 255);
constexpr ImU32 kPanel = IM_COL32(19, 20, 24, 255);
constexpr ImU32 kPanel2 = IM_COL32(28, 29, 35, 255);
constexpr int kBaseDefault = 100;  // Jarrod (bulk import): "base The Rock 100"
const char* const kKindName[3] = {"A  HEAD TO BELT", "B  HEAD TO TRUNKS", "C  HEAD TO HIPS"};
const int kKindSize[3] = {512, 512, 256};

std::string U8(const fs::path& p) {
  const auto u = p.u8string();
  return std::string(u.begin(), u.end());
}
std::string Stem(const std::string& path) {
  const size_t s = path.find_last_of("\\/");
  std::string n = s == std::string::npos ? path : path.substr(s + 1);
  const size_t dot = n.find_last_of('.');
  return dot == std::string::npos ? n : n.substr(0, dot);
}
std::string FileName(const std::string& path) {
  const size_t s = path.find_last_of("\\/");
  return s == std::string::npos ? path : path.substr(s + 1);
}
std::string Esc(const std::string& s) { return jl::Esc(s); }
std::string Upper(std::string t) {
  for (char& c : t)
    if (c >= 'a' && c <= 'z') c = char(c - 32);
  return t;
}
std::string WrestlerName(int id) { return id < 0 ? std::string("-") : assets::CharacterLabel(id); }
bool Ok(const std::string& r) { return r.find("\"ok\":true") != std::string::npos; }
std::string ErrOf(const std::string& r) {
  const size_t p = r.find("\"error\":\"");
  if (p == std::string::npos) return r.substr(0, 160);
  const size_t e = r.find('"', p + 9);
  return r.substr(p + 9, e == std::string::npos ? std::string::npos : e - p - 9);
}
// A name from a file name when the core gives none: "Mr_Kennedy 09" -> "Mr Kennedy 09". The default short name is the same name
// (review: not the file name's last word, "Kennedy Zip.pac" -> "Zip"); the player shortens it.
std::string NiceName(const std::string& stem) {
  std::string n;
  for (char c : stem) n += c == '_' ? ' ' : c;
  while (!n.empty() && n.back() == ' ') n.pop_back();
  return n.empty() ? std::string("New superstar") : n;
}

// Straight RGBA image -> square crop (centre cx, cy and side s in image pixels) resampled to n x n, bilinear; outside = transparent.
std::vector<uint8_t> Crop(const std::vector<uint8_t>& img, int iw, int ih, float cx, float cy, float s, int n) {
  std::vector<uint8_t> o(size_t(n) * n * 4, 0);
  const float k = s / float(n), x0 = cx - s / 2, y0 = cy - s / 2;
  for (int y = 0; y < n; ++y)
    for (int x = 0; x < n; ++x) {
      const float fx = x0 + (x + 0.5f) * k - 0.5f, fy = y0 + (y + 0.5f) * k - 0.5f;
      const int ix = int(std::floor(fx)), iy = int(std::floor(fy));
      const float ax = fx - ix, ay = fy - iy;
      float acc[4] = {0, 0, 0, 0};
      for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i) {
          const int sx = ix + i, sy = iy + j;
          const float wgt = (i ? ax : 1 - ax) * (j ? ay : 1 - ay);
          if (sx < 0 || sy < 0 || sx >= iw || sy >= ih || wgt <= 0) continue;
          const uint8_t* p = &img[(size_t(sy) * iw + sx) * 4];
          const float a = p[3] / 255.f;  // premultiplied while filtering
          for (int c = 0; c < 3; ++c) acc[c] += p[c] * a * wgt;
          acc[3] += a * wgt;
        }
      uint8_t* q = &o[(size_t(y) * n + x) * 4];
      if (acc[3] > 1e-4f)
        for (int c = 0; c < 3; ++c) q[c] = uint8_t(std::clamp(acc[c] / acc[3], 0.f, 255.f));
      q[3] = uint8_t(std::clamp(acc[3] * 255.f + 0.5f, 0.f, 255.f));
    }
  return o;
}

struct Tex {
  ImTextureID id = 0;
  int w = 0, h = 0;
};

}  // namespace

// ------------------------------------------------------------------------------------------------ settings file (a true / false key)
// The importer edits "superstars", the Arena Viewer's LIGHT tab "lighting" (Session 98, Jarrod: USE MY LIGHTING belongs to the viewer).
// The text position of the KEY (a quote not escaped, then the name, then ':'), npos when none.
static size_t FindKey(const std::string& t, const std::string& key) {
  const std::string q = "\"" + key + "\"";
  for (size_t k = t.find(q); k != std::string::npos; k = t.find(q, k + 1)) {
    if (k > 0 && t[k - 1] == '\\') continue;
    const size_t c = t.find_first_not_of(" \t\r\n", k + q.size());
    if (c != std::string::npos && t[c] == ':') return k;
  }
  return std::string::npos;
}
bool ReadBoolSetting(const std::string& file, const std::string& key, bool& on) {
  on = false;
  if (file.empty()) return false;
  std::ifstream f(fs::u8path(file), std::ios::binary);
  if (!f) return false;
  const std::string t((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  jl::Value v;
  std::string perr;
  if (t.size() > (4u << 20) || !jl::Parse(t, v, perr) || v.t != jl::Value::Obj) return false;  // (mid-write or damaged: keep the last value)
  const jl::Value* s = v.get(key.c_str());
  on = s && s->t == jl::Value::Bool && s->b;
  return true;
}
bool ReadSuperstarsSetting(const std::string& file, bool& on) { return ReadBoolSetting(file, "superstars", on); }
bool WriteSuperstarsSetting(const std::string& file, bool on, std::string& err) { return WriteBoolSetting(file, "superstars", on, err); }
bool WriteBoolSetting(const std::string& file, const std::string& key, bool on, std::string& err) {
  if (file.empty()) return err = "no settings file", false;
  std::string t;
  {
    std::ifstream f(fs::u8path(file), std::ios::binary);
    if (!f) return err = "the launcher's settings file is missing", false;
    t.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  }
  {  // (review: never write into a file that is not a whole JSON object - a half-written or foreign file stays as it is)
    jl::Value v;
    std::string perr;
    if (t.size() > (4u << 20) || !jl::Parse(t, v, perr) || v.t != jl::Value::Obj) return err = "the launcher's settings file is not readable JSON", false;
  }
  const std::string val = on ? "true" : "false";
  const size_t k = FindKey(t, key);
  if (k != std::string::npos) {
    const size_t c = t.find(':', k);
    const size_t v = c == std::string::npos ? c : t.find_first_not_of(" \t\r\n", c + 1);
    if (v == std::string::npos) return err = "the settings file is damaged", false;
    const size_t e = t.compare(v, 4, "true") == 0 ? v + 4 : t.compare(v, 5, "false") == 0 ? v + 5 : std::string::npos;
    if (e == std::string::npos) return err = "the settings file's " + key + " value is not true / false", false;
    t.replace(v, e - v, val);
  } else {
    const size_t b = t.find('{');
    if (b == std::string::npos) t = "{\n  \"" + key + "\": " + val + "\n}\n";
    else {
      const size_t nx = t.find_first_not_of(" \t\r\n", b + 1);
      const bool empty_obj = nx != std::string::npos && t[nx] == '}';
      t.insert(b + 1, "\n  \"" + key + "\": " + val + (empty_obj ? "" : ","));
    }
  }
  const fs::path p = fs::u8path(file), tmp = fs::u8path(file + ".imp.tmp");
  {
    std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
    if (!o || !o.write(t.data(), std::streamsize(t.size()))) return err = "cannot write the settings file", false;
  }
  std::error_code ec;
  fs::rename(tmp, p, ec);
  if (ec) return fs::remove(tmp, ec), err = "cannot replace the settings file", false;
  return true;
}

// ------------------------------------------------------------------------------------------------ the panel
struct ImporterPanel::Impl {
  ImporterHost* host = nullptr;
  bool started = false;

  // the pack
  importer::PackState pack;
  std::string pack_err;
  bool enabled = false, settings_linked = false;
  std::chrono::steady_clock::time_point setting_read{};
  std::vector<int> free_slots;  // from the core (game + DLC), before the pack's own
  std::vector<int> wrestlers;   // base picker (100..396)
  std::vector<int> entrants;    // entrance picker (100..999)
  int list_sel = -1;            // the pack entry being edited (-1 = a new one)
  std::string pack_sig;         // <mods>\superstars as last read / written here (another program's change = a reload)
  std::chrono::steady_clock::time_point pack_checked{};

  // the draft (the entry being made / edited)
  importer::Superstar draft;
  char name_buf[64] = {}, short_buf[32] = {};
  std::string src_note, src_err;
  std::vector<std::string> src_warn;
  bool have_model = false, entrance_attire = false, has_entrance = false;
  std::string model_note;
  Tex rtex[3];
  bool rdirty = true;
  std::vector<ImTextureID> dead;

  // the crop step (UPLOAD IMAGE)
  bool cropping = false;
  std::vector<uint8_t> img;
  int img_w = 0, img_h = 0, crop_kind = 0;
  Tex img_tex;
  float crop[3][3] = {};  // cx, cy, side (image pixels)
  bool img_alpha = false;

  // bulk
  struct BulkItem {
    std::string file, entry, name, err;
    std::vector<std::string> warnings;
    int slot = -1;
    bool take = true, done = false;
  };
  bool bulk_open = false, bulk_running = false;
  std::vector<BulkItem> bulk;
  size_t bulk_next = 0;
  int bulk_added = 0;

  // build
  struct Job {
    std::atomic<float> frac{0};
    std::atomic<int> state{0};  // 1 running, 2 done, 3 failed
    std::mutex m;
    std::string phase, err;
  };
  std::shared_ptr<Job> job;
  std::thread job_thread;

  std::string msg;  // the last thing to tell the player (footer / form)
  bool msg_bad = false;
  bool confirm_remove = false, scroll_list = false;
  char base_filter[32] = {}, ent_filter[32] = {};
  ImGuiID touch_drag = 0;

  ~Impl() {
    if (job_thread.joinable()) job_thread.join();
  }

  // ---------------------------------------------------------------- helpers
  void Say(const std::string& m, bool bad = false) { msg = m, msg_bad = bad; }
  // Session 101 (lane 199): why the game would leave the built pack out with the DLC the launcher gives it (src\importer\pack_fit.h,
  // the rule src\dlc.h applies at boot); "" = it fits. A misfit marks the pack to build again and says why in the footer.
  std::string fit_why;
  bool BuiltOnDisk() const {  // a built pack is in the mods folder (an empty list then builds = takes it out of the game)
    std::error_code ec;
    return host && fs::is_regular_file(fs::u8path(host->mods_dir) / "superstars" / "pack6" / "info" / "catalog.dlc", ec);
  }
  void RefreshFit() {
    fit_why = host ? importer::PackProblem(host->mods_dir, host->dlc_dir, host->dlc_off) : std::string();
    if (!fit_why.empty()) pack.needs_build = true;
  }
  bool Building() const { return job && job->state == 1; }
  // The pack folder's signature (file names, sizes, times; the launcher's StarsSig rule): the launcher's REMOVE shows up here.
  std::string PackSig() const {
    std::string sig;
    if (!host || host->mods_dir.empty()) return sig;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(fs::u8path(host->mods_dir) / "superstars", ec)) {
      std::error_code e2;
      if (!e.is_regular_file(e2)) continue;
      sig += U8(e.path().filename()) + ":" + std::to_string(fs::file_size(e.path(), e2)) + ":" +
             std::to_string(static_cast<long long>(fs::last_write_time(e.path(), e2).time_since_epoch().count())) + ";";
    }
    return sig;
  }
  bool SavePackNow(std::string& err) {
    const bool ok = importer::SavePack(host->mods_dir, pack, err);
    pack_sig = PackSig();
    return ok;
  }
  // Another program (the launcher's REMOVE) changed the pack: read it again; the entry being edited stays selected by its slot.
  void CheckReload(bool force = false) {
    if (!host || Building() || bulk_running) return;
    const std::string sig = PackSig();
    if (!force && sig == pack_sig) return;
    pack_sig = sig;
    importer::PackState st;
    std::string err;
    if (!importer::LoadPack(host->mods_dir, st, err)) return Say("The pack could not be read again: " + err, true);
    const int slot = list_sel >= 0 && list_sel < int(pack.stars.size()) ? pack.stars[size_t(list_sel)].slot : -1;
    pack = std::move(st);
    RefreshFit();  // (Session 101: the game's own rule, with the switched-off packs)
    list_sel = -1;
    for (size_t i = 0; i < pack.stars.size(); ++i)
      if (slot >= 0 && pack.stars[i].slot == slot) list_sel = int(i);
    if (slot >= 0 && list_sel < 0) NewDraft(), Say("That superstar was removed from the pack elsewhere.");
  }
  // The phone's picked copies (<files>/importer/picked/...): the pack keeps its own copy in <mods>\superstars\sources (review).
  void CleanPicked() {
    if (!host || host->picked_dir.empty()) return;
    std::error_code ec;
    fs::remove_all(fs::u8path(host->picked_dir), ec);
  }
  int Used() const { return int(pack.stars.size()); }
  bool SlotTaken(int slot, int except) const {
    for (size_t i = 0; i < pack.stars.size(); ++i)
      if (int(i) != except && pack.stars[i].slot == slot) return true;
    return false;
  }
  std::vector<int> SlotsFor(int except) const {  // free slots not used by another pack entry
    std::vector<int> v;
    for (int s : free_slots)
      if (!SlotTaken(s, except)) v.push_back(s);
    if (except >= 0 && except < int(pack.stars.size())) {
      const int own = pack.stars[size_t(except)].slot;
      if (own >= 0 && std::find(v.begin(), v.end(), own) == v.end()) v.insert(v.begin(), own);
    }
    return v;
  }
  void Start() {
    if (started || !host) return;
    started = true;
    std::string err;
    if (!importer::LoadPack(host->mods_dir, pack, err)) pack_err = err, Say("The pack could not be read: " + err, true);
    RefreshFit();  // (your DLC changed: the game leaves the old pack out; Session 101: the game's rule, with the switched-off packs)
    pack_sig = PackSig();
    // (review: the launcher's "superstars" setting is the ONLY on / off; PackState::enabled is never used for it. Not started from
    // the launcher = no settings file = the switch shows OFF and cannot be changed here.)
    settings_linked = !host->settings_file.empty();
    ReadSuperstarsSetting(host->settings_file, enabled);
    launcher::GameFiles g;
    g.SetRoot(fs::u8path(host->game));
    free_slots = importer::FreeSlots(g, host->dlc_dir);
    // BASED ON: the game's own roster ids 100..396 (the base's roster record); ENTRANCE: any character 100..999 (review)
    for (int id : ViewerList("wrestler")) {
      if (id >= 100 && id < importer::kFirstSlot) wrestlers.push_back(id);
      if (id >= 100 && id <= 999) entrants.push_back(id);
    }
    auto by_name = [](int a, int b) { return assets::CharacterLabel(a) < assets::CharacterLabel(b); };
    std::sort(wrestlers.begin(), wrestlers.end(), by_name);
    std::sort(entrants.begin(), entrants.end(), by_name);
    NewDraft();
    if (host->edit_slot >= 0)
      for (size_t i = 0; i < pack.stars.size(); ++i)
        if (pack.stars[i].slot == host->edit_slot) Edit(int(i));
    if (host->bulk_at_start) PickMany(true);
  }
  void DropTex(Tex& t) {
    if (t.id) dead.push_back(t.id);
    t = Tex();
  }
  void NewDraft() {
    list_sel = -1;
    draft = importer::Superstar();
    draft.base_id = kBaseDefault, draft.copy_ratings = true, draft.entrance_from = -1;
    const std::vector<int> s = SlotsFor(-1);
    draft.slot = s.empty() ? -1 : s.front();
    name_buf[0] = short_buf[0] = 0;
    src_note.clear(), src_err.clear(), src_warn.clear();
    have_model = false, entrance_attire = false;
    rdirty = true;
  }
  void Edit(int i) {
    if (i < 0 || i >= int(pack.stars.size())) return;
    list_sel = i;
    draft = pack.stars[size_t(i)];
    std::snprintf(name_buf, sizeof name_buf, "%s", draft.name.c_str());
    std::snprintf(short_buf, sizeof short_buf, "%s", draft.short_name.c_str());
    src_err.clear(), src_warn.clear(), src_note = FileName(draft.source_pac);
    rdirty = true;
    have_model = false, entrance_attire = false;
    LoadPreview();
  }
  // The preview: the source pac (a bare pac, or one copied out of a zip) into the scene with the match light.
  bool LoadPreview() {
    if (draft.source_pac.empty() || !exec) return false;
    const std::string r = exec(std::string("{\"cmd\":\"importer_preview\",\"path\":\"") + Esc(draft.source_pac) + "\",\"attire\":\"" +
                               (entrance_attire ? "entrance" : "ring") + "\"}");
    if (!Ok(r)) {
      have_model = false;
      Say("Preview: " + ErrOf(r), true);
      return false;
    }
    have_model = true;
    has_entrance = r.find("\"has_entrance\":true") != std::string::npos;
    model_note = r;
    return true;
  }
  std::function<std::string(const std::string&)> exec;

  // CHOOSE: one file with one pac = the single editor; a zip of several (or several files) = the bulk list.
  void PickMany(bool always_bulk) {
    namespace ml = assets::mods_lookup;
    std::vector<std::string> files;
    if (ml::Plat().pick_characters) files = ml::Plat().pick_characters();
    else if (ml::Plat().pick_character) {
      const std::string f = ml::Plat().pick_character();
      if (!f.empty()) files.push_back(f);
    }
    if (files.empty()) {
      if (!ml::Plat().pick_error.empty()) Say(ml::Plat().pick_error, true);
      return;
    }
    Choose(files, always_bulk);
  }
  void Choose(const std::vector<std::string>& files, bool always_bulk) {
    std::vector<BulkItem> items;
    for (const std::string& f : files) {
      const std::vector<importer::SourceInfo> found = importer::InspectSources(f);
      if (found.empty()) {  // (said in the list, not dropped silently)
        BulkItem b;
        b.file = f, b.name = NiceName(Stem(f)), b.err = "no .pac file in it", b.take = false;
        items.push_back(b);
      }
      for (const importer::SourceInfo& si : found) {
        BulkItem b;
        b.file = f, b.entry = si.entry_name, b.warnings = si.warnings;
        b.name = NiceName(si.suggested_name.empty() ? Stem(si.entry_name.empty() ? f : si.entry_name) : si.suggested_name);
        if (!si.ok) b.err = si.err.empty() ? "not a usable character pac" : si.err, b.take = false;
        items.push_back(b);
      }
    }
    int usable = 0;
    for (const BulkItem& b : items) usable += b.take ? 1 : 0;
    if (!always_bulk && items.size() == 1 && usable == 1) {
      const BulkItem& b = items.front();
      SetSource(b.file, b.entry, b.name, b.warnings);
      return;
    }
    if (items.empty()) return Say("Nothing usable was picked", true);
    bulk = std::move(items), bulk_open = true, bulk_running = false, bulk_next = 0, bulk_added = 0;
    cropping = false;
    AssignBulkSlots();
  }
  void AssignBulkSlots() {
    std::vector<int> slots = SlotsFor(-1);
    size_t k = 0;
    int room = importer::kMaxStars - Used();
    for (BulkItem& b : bulk) {
      b.slot = -1;
      if (!b.take || !b.err.empty()) continue;
      if (room <= 0 || k >= slots.size()) {
        b.err = room <= 0 ? "no room: the pack holds " + std::to_string(importer::kMaxStars) : "no free slot left";
        b.take = false;
        continue;
      }
      b.slot = slots[k++], --room;
    }
  }
  // Every source, a zip member or a plain pac, is copied into <mods>\superstars\sources (review: the pack never points outside it).
  std::string StableSource(const std::string& file, const std::string& entry, std::string& err) {
    err.clear();
    const std::string p = importer::ExtractSource(file, entry, host->mods_dir, &err);
    if (p.empty() && err.empty()) err = "could not copy it into the mods folder";
    return p;
  }
  void SetSource(const std::string& file, const std::string& entry, const std::string& name, const std::vector<std::string>& warns) {
    std::string xerr;
    const std::string pac = StableSource(file, entry, xerr);
    CleanPicked();
    if (pac.empty()) return Say(FileName(entry.empty() ? file : entry) + ": " + xerr, true);
    const importer::SourceInfo si = importer::InspectSource(pac);
    if (!si.ok) {
      src_err = si.err;
      return Say(FileName(pac) + ": " + si.err, true);
    }
    draft.source_pac = pac;
    src_err.clear(), src_warn = warns;
    for (const std::string& w : si.warnings) src_warn.push_back(w);
    char b[200];
    std::snprintf(b, sizeof b, "%s  -  %d attire%s  -  %d bones%s", FileName(entry.empty() ? file : entry).c_str(), si.attires, si.attires == 1 ? "" : "s",
                  si.bip_bones, si.claimed_id >= 0 ? ("  -  made for slot " + std::to_string(si.claimed_id)).c_str() : "");
    src_note = b;
    if (!name_buf[0]) std::snprintf(name_buf, sizeof name_buf, "%s", name.c_str());
    if (!short_buf[0]) std::snprintf(short_buf, sizeof short_buf, "%s", name.c_str());  // (the core's suggested name, not a last word)
    entrance_attire = false;
    for (auto& r : draft.render_rgba) r.clear();
    rdirty = true;
    if (LoadPreview()) Retake();
    Say("Loaded " + FileName(pac) + ". Check the name, the base and the slot, then ADD TO PACK.");
  }
  bool Retake() {
    if (!have_model || !host || !host->portrait) return Say("Load a character first", true), false;
    for (int k = 0; k < 3; ++k) {
      std::string err;
      int w = 0, h = 0;
      std::vector<uint8_t> px;
      if (!host->portrait(k, px, w, h, err)) return Say("Portrait: " + err, true), false;
      draft.render_rgba[k] = std::move(px), draft.render_w[k] = w, draft.render_h[k] = h;
    }
    rdirty = true;
    return true;
  }
  bool LoadImage(const std::string& path) {
    // (review: the size before reading, the dimensions before decoding)
    std::error_code ec;
    const uint64_t sz = fs::file_size(fs::u8path(path), ec);
    if (ec || sz == 0) return Say("Cannot read " + FileName(path), true), false;
    if (sz > (64u << 20)) return Say("The picture is too big (over 64 MB)", true), false;
    std::ifstream f(fs::u8path(path), std::ios::binary);
    std::vector<uint8_t> bytes(static_cast<size_t>(sz));
    if (!f || !f.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(sz))) return Say("Cannot read " + FileName(path), true), false;
    int w = 0, h = 0, n = 0;
    if (!stbi_info_from_memory(bytes.data(), int(bytes.size()), &w, &h, &n)) return Say(FileName(path) + " is not a PNG or JPG picture", true), false;
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return Say("The picture is too large (over 8192 pixels)", true), false;
    stbi_uc* px = stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &n, 4);
    if (!px) return Say(FileName(path) + " is not a PNG or JPG picture", true), false;
    img.assign(px, px + size_t(w) * h * 4);
    stbi_image_free(px);
    img_w = w, img_h = h, img_alpha = n == 4 || n == 2;
    DropTex(img_tex);
    // the starting frames: A the top of the picture, the figure filling the width; B a little more of him; C the whole width with room
    const float m = float(std::min(w, h));
    crop[0][2] = m * 0.80f, crop[1][2] = m * 0.92f, crop[2][2] = m * 1.0f;
    for (int k = 0; k < 3; ++k) crop[k][0] = w / 2.f, crop[k][1] = crop[k][2] / 2.f + (k == 2 ? 0 : 0.f);
    crop_kind = 0, cropping = true, bulk_open = false;
    Say(img_alpha ? "Frame each render, then DONE." : "Frame each render, then DONE. (A JPG has no transparency: a PNG with a cut-out looks like the game's.)");
    return true;
  }
  void CropDone() {
    for (int k = 0; k < 3; ++k) {
      const int n = kKindSize[k];
      draft.render_rgba[k] = Crop(img, img_w, img_h, crop[k][0], crop[k][1], crop[k][2], n);
      draft.render_w[k] = draft.render_h[k] = n;
    }
    rdirty = true, cropping = false;
    DropTex(img_tex);
    img.clear();
    Say("Portrait set from your picture.");
  }
  bool SaveDraft() {
    if (Building() || bulk_running) return Say("Wait for the build to finish", true), false;
    CheckReload();  // (the launcher may have removed one meanwhile)
    draft.name = name_buf, draft.short_name = short_buf;
    if (draft.name.empty()) return Say("Give the superstar a name", true), false;
    if (draft.short_name.empty()) draft.short_name = draft.name;
    if (draft.source_pac.empty()) return Say("Choose a .zip or .pac first", true), false;
    if (draft.slot < 0) return Say("No free slot: pick one", true), false;
    if (SlotTaken(draft.slot, list_sel)) return Say("Slot " + std::to_string(draft.slot) + " is already used in the pack", true), false;
    if (list_sel < 0 && Used() >= importer::kMaxStars)
      return Say("The pack is full: it holds up to " + std::to_string(importer::kMaxStars) + " superstars", true), false;
    if (draft.render_rgba[0].empty() && have_model) Retake();
    if (list_sel >= 0) pack.stars[size_t(list_sel)] = draft;
    else pack.stars.push_back(draft), list_sel = int(pack.stars.size()) - 1;
    pack.needs_build = true;
    std::string err;
    if (!SavePackNow(err)) return Say("Saved here, but the pack list could not be written: " + err, true), false;
    Say(draft.name + " is in the pack (slot " + std::to_string(draft.slot) + "). BUILD INTO GAME puts the pack in the game.");
    return true;
  }
  void RemoveSel() {
    if (Building() || bulk_running) return Say("Wait for the build to finish", true);
    if (list_sel < 0 || list_sel >= int(pack.stars.size())) return;
    const int slot = pack.stars[size_t(list_sel)].slot;  // (review: by slot - the list may have changed under the index)
    const std::string n = pack.stars[size_t(list_sel)].name;
    CheckReload();
    const auto it = std::find_if(pack.stars.begin(), pack.stars.end(), [slot](const importer::Superstar& s) { return s.slot == slot; });
    if (it == pack.stars.end()) return NewDraft(), Say(n + " was already removed.");
    pack.stars.erase(it);
    pack.needs_build = true;
    std::string err;
    SavePackNow(err);
    NewDraft();
    exec("{\"cmd\":\"importer_preview\",\"clear\":true}");
    Say(err.empty() ? n + " removed. BUILD INTO GAME to take it out of the game too." : "Removed here; the list could not be written: " + err, !err.empty());
  }
  void SetEnabled(bool on) {
    // The launcher's own USE MY SUPERSTARS setting is edited, nothing else (review: the one switch; the build never carries it).
    if (!settings_linked) return Say("Start the importer from the launcher to switch the pack on or off", true);
    std::string err;
    if (!WriteSuperstarsSetting(host->settings_file, on, err)) return Say("The switch could not be saved: " + err, true);
    enabled = on;
    setting_read = std::chrono::steady_clock::now();
  }
  void StartBuild() {
    if (Building() || bulk_running) return;
    CheckReload();
    if (pack.stars.empty() && !BuiltOnDisk()) return Say("The pack is empty: add a superstar first", true);
    if (job_thread.joinable()) job_thread.join();
    job = std::make_shared<Job>();
    job->state = 1;
    auto j = job;
    const importer::PackState st = pack;
    const std::string mods = host->mods_dir, game = host->game, dlc = host->dlc_dir, off = host->dlc_off;
    job_thread = std::thread([j, st, mods, game, dlc, off] {
      launcher::GameFiles g;
      g.SetRoot(fs::u8path(game));
      std::string err;
      bool ok = false;
      try {
        ok = importer::BuildPack(mods, g, dlc, st, [j](float f, const char* phase) {
          j->frac = f;
          std::lock_guard<std::mutex> lk(j->m);
          j->phase = phase ? phase : "";
        }, err, off);
      } catch (const std::exception& e) {
        err = std::string("build failed: ") + e.what();
      }
      std::lock_guard<std::mutex> lk(j->m);
      j->err = err;
      j->state = ok ? 2 : 3;
    });
    Say("Building the pack from your own game files...");
  }
  // One bulk entry per call (outside the frame): copy it out, show it, render its three portraits, add it with the defaults.
  void BulkStep() {
    while (bulk_next < bulk.size() && (!bulk[bulk_next].take || bulk[bulk_next].done)) ++bulk_next;
    if (bulk_next >= bulk.size()) {
      bulk_running = false;
      pack.needs_build = bulk_added > 0 || pack.needs_build;
      std::string err;
      if (bulk_added) SavePackNow(err);
      CleanPicked();
      Say(std::to_string(bulk_added) + " superstar" + (bulk_added == 1 ? "" : "s") + " added. Edit any of them in the list, then BUILD INTO GAME.",
          !err.empty());
      if (!err.empty()) Say("Added, but the pack list could not be written: " + err, true);
      bulk_open = false;
      NewDraft();
      exec("{\"cmd\":\"importer_preview\",\"clear\":true}");  // (the last one shown: the form is empty again)
      return;
    }
    BulkItem& b = bulk[bulk_next];
    b.done = true;
    if (Used() >= importer::kMaxStars) { b.err = "no room: the pack holds " + std::to_string(importer::kMaxStars); return; }
    std::string xerr;
    const std::string pac = StableSource(b.file, b.entry, xerr);
    if (pac.empty()) { b.err = xerr; return; }
    importer::Superstar s;
    s.source_pac = pac, s.name = b.name, s.short_name = b.name, s.base_id = kBaseDefault, s.entrance_from = -1, s.copy_ratings = true;
    s.slot = b.slot;
    if (s.slot < 0 || SlotTaken(s.slot, -1)) { b.err = "no free slot left"; return; }
    draft = s, entrance_attire = false, have_model = false;
    if (LoadPreview()) {
      Retake();
      s.render_rgba[0] = draft.render_rgba[0], s.render_rgba[1] = draft.render_rgba[1], s.render_rgba[2] = draft.render_rgba[2];
      for (int k = 0; k < 3; ++k) s.render_w[k] = draft.render_w[k], s.render_h[k] = draft.render_h[k];
    } else {
      b.warnings.push_back("no preview: " + msg + " (portrait left empty)");
    }
    pack.stars.push_back(std::move(s));
    ++bulk_added;
  }

  // ---------------------------------------------------------------- drawing
  bool SlantButton(const ImporterCtx& cx, const char* label, ImVec2 size, bool primary, bool enabled = true) {
    const float d = cx.d, sk = size.y * 0.22f;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::InvisibleButton(label, size);
    ImGui::EndDisabled();
    const bool hover = enabled && ImGui::IsItemHovered(), held = enabled && ImGui::IsItemActive();
    ImU32 top = primary ? IM_COL32(160, 20, 28, 255) : IM_COL32(52, 53, 60, 255), bottom = primary ? IM_COL32(82, 8, 12, 255) : IM_COL32(30, 31, 36, 255);
    if (!enabled) top = IM_COL32(56, 56, 62, 255), bottom = IM_COL32(36, 36, 41, 255);
    else if (held) top = IM_COL32(112, 12, 18, 255), bottom = IM_COL32(64, 6, 10, 255);
    else if (hover) top = IM_COL32(205, 30, 38, 255), bottom = IM_COL32(118, 12, 18, 255);
    const ImVec2 q[4] = {{pos.x + sk, pos.y}, {pos.x + size.x, pos.y}, {pos.x + size.x - sk, pos.y + size.y}, {pos.x, pos.y + size.y}};
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 uv = dl->_Data->TexUvWhitePixel;
    const auto i0 = ImDrawIdx(dl->_VtxCurrentIdx);
    dl->PrimReserve(6, 4);
    dl->PrimWriteIdx(i0), dl->PrimWriteIdx(ImDrawIdx(i0 + 1)), dl->PrimWriteIdx(ImDrawIdx(i0 + 2));
    dl->PrimWriteIdx(i0), dl->PrimWriteIdx(ImDrawIdx(i0 + 2)), dl->PrimWriteIdx(ImDrawIdx(i0 + 3));
    dl->PrimWriteVtx(q[0], uv, top), dl->PrimWriteVtx(q[1], uv, top);
    dl->PrimWriteVtx(q[2], uv, bottom), dl->PrimWriteVtx(q[3], uv, bottom);
    dl->AddPolyline(q, 4, enabled ? (primary ? IM_COL32_WHITE : IM_COL32(150, 150, 158, 255)) : IM_COL32(100, 100, 106, 255), ImDrawFlags_Closed,
                    (primary ? 2.5f : 1.5f) * d);
    const std::string text(label, ImGui::FindRenderedTextEnd(label));
    float px = std::min(18.f * d, size.y * 0.5f);
    ImVec2 ts = cx.size(px, text.c_str());
    const float room = size.x - 2 * sk - 12 * d;
    if (ts.x > room) px *= room / ts.x, ts = cx.size(px, text.c_str());
    cx.text(dl, ImVec2(pos.x + (size.x - ts.x) / 2, pos.y + (size.y - ts.y) / 2), px, enabled ? IM_COL32_WHITE : IM_COL32(150, 150, 156, 255), text.c_str());
    return pressed;
  }
  void Head(const ImporterCtx& cx, const char* t) {
    cx.text_raw(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), 15 * cx.d, kRed, t);
    ImGui::Dummy(ImVec2(0, 19 * cx.d));
  }
  void Label(const ImporterCtx& cx, const char* t, float w) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetFrameHeight(), px = 16 * cx.d;
    cx.text(ImGui::GetWindowDrawList(), ImVec2(p.x, p.y + (h - px) / 2), px, IM_COL32(205, 206, 211, 255), t);
    ImGui::Dummy(ImVec2(w, h));
    ImGui::SameLine(0, 0);
  }
  void Checker(ImDrawList* dl, ImVec2 a, ImVec2 b, float sq) {
    dl->AddRectFilled(a, b, IM_COL32(58, 58, 64, 255));
    for (float y = a.y; y < b.y; y += sq)
      for (float x = a.x + (int((y - a.y) / sq) % 2 ? sq : 0); x < b.x; x += sq * 2)
        dl->AddRectFilled(ImVec2(x, y), ImVec2(std::min(x + sq, b.x), std::min(y + sq, b.y)), IM_COL32(44, 44, 49, 255));
  }
  void TouchScroll(const ImporterCtx& cx) {  // (ui.cpp's rule: a mostly vertical finger drag scrolls the window it started in)
    ImGuiIO& io = ImGui::GetIO();
    if (io.MouseSource != ImGuiMouseSource_TouchScreen || !ImGui::IsMouseDown(0)) { touch_drag = 0; return; }
    const ImGuiID me = ImGui::GetCurrentWindow()->ID;
    if (!touch_drag && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
      const ImVec2 dd = ImGui::GetMouseDragDelta(0, 0);
      if (std::fabs(dd.y) > 10 * cx.d && std::fabs(dd.y) > std::fabs(dd.x) * 1.2f) touch_drag = me;
    }
    if (touch_drag == me) {
      ImGui::SetScrollY(ImGui::GetScrollY() - io.MouseDelta.y);
      ImGui::ClearActiveID();
    }
  }
  void Refresh(const ImporterCtx& cx) {
    if (!rdirty) return;
    rdirty = false;
    for (int k = 0; k < 3; ++k) {
      DropTex(rtex[k]);
      if (!draft.render_rgba[k].empty())
        rtex[k].id = cx.make_tex(draft.render_rgba[k].data(), draft.render_w[k], draft.render_h[k]), rtex[k].w = draft.render_w[k], rtex[k].h = draft.render_h[k];
    }
  }
  bool WrestlerCombo(const char* id, const std::vector<int>& ids, int& value, const char* none_label, char* filter, size_t filter_n, float w) {
    const std::string cur = value < 0 ? std::string(none_label ? none_label : "-") : WrestlerName(value) + "  (" + std::to_string(value) + ")";
    bool changed = false;
    ImGui::SetNextItemWidth(w);
    if (ImGui::BeginCombo(id, cur.c_str(), ImGuiComboFlags_HeightLarge)) {
      if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
      ImGui::SetNextItemWidth(-1);
      ImGui::InputTextWithHint("##filter", "Search name or id", filter, filter_n);
      const std::string q = Upper(filter);
      if (none_label && ImGui::Selectable(none_label, value < 0)) value = -1, changed = true;
      for (int wid : ids) {
        const std::string n = WrestlerName(wid);
        if (!q.empty() && Upper(n).find(q) == std::string::npos && std::to_string(wid).find(q) == std::string::npos) continue;
        const std::string row = n + "  (" + std::to_string(wid) + ")";
        if (ImGui::Selectable(row.c_str(), wid == value)) value = wid, changed = true;
      }
      ImGui::EndCombo();
    }
    return changed;
  }

  void Draw(const ImporterCtx& cx) {
    exec = cx.exec;
    Refresh(cx);
    const float d = cx.d;
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    const bool narrow = disp.x < 1100 * d || disp.y < 640 * d;  // a phone (or a small window): tighter, finger-sized rows
    if (host && host->phone) ImGui::GetStyle().FramePadding = ImVec2(12 * d, 11 * d);
    const float head = (narrow ? 44 : 56) * d, foot = (narrow ? 58 : 66) * d;
    const float left_w = std::max(300 * d, std::floor(disp.x * (narrow ? 0.46f : 0.42f)));
    const float body_h = disp.y - head - foot;
    const float pv_h = std::floor(std::min(left_w * 0.9f, body_h * (narrow ? 0.44f : 0.45f)));
    const ImGuiWindowFlags fixed = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;

    // the preview hole (no window over it: the scene shows, the mouse / fingers turn the model)
    if (host) host->view[0] = 0, host->view[1] = head, host->view[2] = left_w, host->view[3] = head + pv_h;
    {
      ImDrawList* bg = ImGui::GetBackgroundDrawList();
      bg->AddRect(ImVec2(1, head + 1), ImVec2(left_w - 1, head + pv_h - 1), IM_COL32(255, 255, 255, 40), 0, 0, 1.5f * d);
      const char* cap = !have_model ? "3D PREVIEW"
                        : (host && host->phone) ? "DRAG TO TURN  -  TWO FINGERS TO MOVE  -  PINCH TO ZOOM"
                                                : "DRAG TO TURN  -  RIGHT-DRAG TO MOVE  -  WHEEL TO ZOOM";
      cx.text(bg, ImVec2(12 * d, head + 10 * d), 13 * d, IM_COL32(255, 255, 255, 150), cap);
      if (!have_model) {
        const char* t = draft.source_pac.empty() ? "CHOOSE A .ZIP OR .PAC BELOW" : "NO PREVIEW";
        const ImVec2 ts = cx.size(20 * d, t);
        cx.text(bg, ImVec2((left_w - ts.x) / 2, head + (pv_h - ts.y) / 2), 20 * d, IM_COL32(200, 200, 206, 200), t);
      } else if (!src_note.empty()) {
        bg->AddText(ImVec2(12 * d, head + pv_h - 24 * d), IM_COL32(220, 220, 226, 200), src_note.c_str());
      }
    }

    // header
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(disp.x, head));
    ImGui::Begin("##imp_head", nullptr, fixed | ImGuiWindowFlags_NoBackground);
    {
      ImDrawList* dl = ImGui::GetWindowDrawList();
      dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(disp.x, head), IM_COL32(176, 20, 28, 255), IM_COL32(150, 14, 22, 255), IM_COL32(96, 8, 13, 255),
                                  IM_COL32(110, 10, 15, 255));
      dl->AddLine(ImVec2(0, head - 1), ImVec2(disp.x, head - 1), IM_COL32(255, 255, 255, 90), 1.5f * d);
      const float px = (narrow ? 22 : 28) * d;
      cx.text(dl, ImVec2(22 * d, (head - px) / 2), px, IM_COL32_WHITE, "SUPERSTAR IMPORTER");
      char b[96];
      std::snprintf(b, sizeof b, "MOD PACK: %d OF UP TO %d SLOTS USED", Used(), importer::kMaxStars);
      const float sp = (narrow ? 14 : 17) * d;
      const ImVec2 ts = cx.size(sp, b);
      cx.text(dl, ImVec2(disp.x - ts.x - 22 * d, (head - sp) / 2), sp, IM_COL32(255, 205, 205, 255), b);
    }
    ImGui::End();

    // left column under the preview
    ImGui::SetNextWindowPos(ImVec2(0, head + pv_h));
    ImGui::SetNextWindowSize(ImVec2(left_w, body_h - pv_h));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(kPanel));
    ImGui::Begin("##imp_left", nullptr, fixed);
    LeftColumn(cx, left_w);
    TouchScroll(cx);
    ImGui::End();
    // right column
    ImGui::SetNextWindowPos(ImVec2(left_w, head));
    ImGui::SetNextWindowSize(ImVec2(disp.x - left_w, body_h));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(kPanel2));
    ImGui::Begin("##imp_right", nullptr, fixed);
    {
      ImDrawList* dl = ImGui::GetWindowDrawList();
      dl->AddRectFilled(ImGui::GetWindowPos(), ImVec2(ImGui::GetWindowPos().x + 4 * d, ImGui::GetWindowPos().y + body_h), kRed);
    }
    if (cropping) CropStep(cx, disp.x - left_w);
    else if (bulk_open) BulkList(cx, disp.x - left_w);
    else Form(cx, disp.x - left_w, narrow);
    TouchScroll(cx);
    ImGui::End();
    ImGui::PopStyleColor(2);
    // footer
    ImGui::SetNextWindowPos(ImVec2(0, disp.y - foot));
    ImGui::SetNextWindowSize(ImVec2(disp.x, foot));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.045f, 0.047f, 0.056f, 1));
    ImGui::Begin("##imp_foot", nullptr, fixed | ImGuiWindowFlags_NoScrollbar);
    Footer(cx, disp.x, foot);
    ImGui::End();
    ImGui::PopStyleColor();
  }

  void LeftColumn(const ImporterCtx& cx, float w) {
    const float d = cx.d, bh = 40 * d, inner = w - 2 * ImGui::GetStyle().WindowPadding.x;
    ImGui::Dummy(ImVec2(0, 2 * d));
    const float bw1 = std::floor(inner * 0.5f - 4 * d);
    if (SlantButton(cx, "CHOOSE .ZIP OR .PAC", ImVec2(bw1, bh), true, !bulk_running)) cx.queued->push_back([this] { PickMany(false); });
    ImGui::SameLine(0, 8 * d);
    const bool can_ent = have_model && has_entrance;
    if (SlantButton(cx, entrance_attire ? "ENTRANCE ATTIRE##att" : "RING ATTIRE##att", ImVec2(bw1, bh), false, have_model && (can_ent || entrance_attire))) {
      entrance_attire = !entrance_attire;
      cx.queued->push_back([this] { LoadPreview(); });
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ImGui::SetTooltip(have_model && !has_entrance ? "This pac has no entrance attire" : "Show the ring or the entrance attire");
    if (!src_err.empty()) ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "%s", src_err.c_str());
    for (const std::string& wn : src_warn) ImGui::TextColored(ImVec4(1, 0.8f, 0.35f, 1), "%s", wn.c_str());

    ImGui::Dummy(ImVec2(0, 6 * d));
    Head(cx, "ROSTER PORTRAIT");
    // the three renders side by side over a checkerboard (the cut-out shows), RE-TAKE / UPLOAD IMAGE stacked on their right
    const float gap = 6 * d, bcol = std::max(120 * d, std::floor(inner * 0.34f));
    const float tw = std::floor(std::min((inner - bcol - 10 * d - 2 * gap) / 3, 100 * d)), th = tw;
    const float bw2 = inner - 3 * tw - 2 * gap - 10 * d;
    const ImVec2 row = ImGui::GetCursorScreenPos();
    {
      static const char* const kCap[3] = {"A  BELT", "B  TRUNKS", "C  HIPS"};
      ImDrawList* dl = ImGui::GetWindowDrawList();
      for (int k = 0; k < 3; ++k) {
        if (k) ImGui::SameLine(0, gap);
        const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + tw, a.y + th);
        ImGui::Dummy(ImVec2(tw, th + 15 * d));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s, %d x %d", kKindName[k], kKindSize[k], kKindSize[k]);
        Checker(dl, a, b, 6 * d);
        if (rtex[k].id) dl->AddImage(ImTextureRef(rtex[k].id), a, b);
        else dl->AddText(ImVec2(a.x + 6 * d, a.y + th / 2 - 8 * d), IM_COL32(170, 170, 176, 255), "none");
        dl->AddRect(a, b, IM_COL32(255, 255, 255, 50));
        cx.text_raw(dl, ImVec2(a.x, b.y + 3 * d), 10 * d, kDim, kCap[k]);
      }
    }
    const float bh2 = std::min(36 * d, (th - 8 * d) / 2);
    ImGui::SetCursorScreenPos(ImVec2(row.x + 3 * tw + 2 * gap + 10 * d, row.y));
    if (SlantButton(cx, "RE-TAKE", ImVec2(bw2, bh2), false, have_model)) cx.queued->push_back([this] {
      if (Retake()) Say("Portrait rendered from the model with the match light (turn him in the preview first to pick the angle).");
    });
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Render the three roster pictures from the model, at the preview's angle");
    ImGui::SetCursorScreenPos(ImVec2(row.x + 3 * tw + 2 * gap + 10 * d, row.y + bh2 + 8 * d));
    if (SlantButton(cx, "UPLOAD IMAGE", ImVec2(bw2, bh2), false, assets::mods_lookup::Plat().pick_image != nullptr)) cx.queued->push_back([this] {
      const std::string f = assets::mods_lookup::Plat().pick_image();
      if (!f.empty()) LoadImage(f);
      else if (!assets::mods_lookup::Plat().pick_error.empty()) Say(assets::mods_lookup::Plat().pick_error, true);
    });
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Your own PNG or JPG, framed in the next step");
    ImGui::SetCursorScreenPos(ImVec2(row.x, row.y + th + 15 * d));

    ImGui::Dummy(ImVec2(0, 6 * d));
    Head(cx, "IN THE MOD PACK");
    ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt, ImVec4(1, 1, 1, 0.035f));
    const float row_h = (host && host->phone ? 40 : 26) * d;
    const float list_h = std::max(row_h * 4.5f, std::min(row_h * (pack.stars.size() + 1.6f), 260 * d));
    if (ImGui::BeginTable("##pack", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersOuterH | ImGuiTableFlags_SizingStretchProp,
                          ImVec2(inner, list_h))) {
      ImGui::TableSetupScrollFreeze(0, 1);
      ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthStretch, 0.55f);
      ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 2.0f);
      ImGui::TableSetupColumn("Based on", ImGuiTableColumnFlags_WidthStretch, 1.6f);
      ImGui::TableHeadersRow();
      for (int i = 0; i < int(pack.stars.size()); ++i) {
        const importer::Superstar& s = pack.stars[size_t(i)];
        ImGui::TableNextRow(0, row_h);
        ImGui::TableNextColumn();
        char id[32];
        std::snprintf(id, sizeof id, "%d##row%d", s.slot, i);
        if (ImGui::Selectable(id, list_sel == i, ImGuiSelectableFlags_SpanAllColumns, ImVec2(0, row_h - 4 * d)) && !bulk_running) {
          const int k = i;
          cx.queued->push_back([this, k] { cropping = false, bulk_open = false, Edit(k); });
        }
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(s.name.c_str());
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", WrestlerName(s.base_id).c_str());
      }
      if (pack.stars.empty()) {
        ImGui::TableNextRow(0, row_h);
        ImGui::TableNextColumn();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("Empty: choose a .zip or .pac above");
      }
      ImGui::EndTable();
    }
    ImGui::PopStyleColor();
    const float bw3 = std::floor((inner - 16 * d) / 3);
    if (SlantButton(cx, "NEW", ImVec2(bw3, 32 * d), false, !bulk_running && !Building())) cx.queued->push_back([this] {
      NewDraft(), cropping = false, bulk_open = false;
      exec("{\"cmd\":\"importer_preview\",\"clear\":true}");
    });
    ImGui::SameLine(0, 8 * d);
    if (SlantButton(cx, "REMOVE", ImVec2(bw3, 32 * d), false, list_sel >= 0 && !bulk_running && !Building())) confirm_remove = true;
    ImGui::SameLine(0, 8 * d);
    if (SlantButton(cx, "IMPORT MANY", ImVec2(bw3, 32 * d), false, !bulk_running && !Building())) cx.queued->push_back([this] { PickMany(true); });
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("A zip full of pac files, or several files at once");
    if (scroll_list) ImGui::SetScrollHereY(1.f), scroll_list = false;  // (test hook: the list and its buttons in view)
    if (confirm_remove) {
      ImGui::OpenPopup("##remove");
      confirm_remove = false;
    }
    if (ImGui::BeginPopupModal("##remove", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
      if (list_sel >= 0 && list_sel < int(pack.stars.size())) ImGui::Text("Remove %s from the pack?", pack.stars[size_t(list_sel)].name.c_str());
      ImGui::TextDisabled("BUILD INTO GAME afterwards takes him out of the game too.");
      if (ImGui::Button("Remove", ImVec2(120 * d, 0))) { cx.queued->push_back([this] { RemoveSel(); }); ImGui::CloseCurrentPopup(); }
      ImGui::SameLine();
      if (ImGui::Button("Cancel", ImVec2(120 * d, 0))) ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }
  }

  void Form(const ImporterCtx& cx, float w, bool narrow) {
    const float d = cx.d, lab = (narrow ? 120 : 150) * d, field = w - lab - 2 * ImGui::GetStyle().WindowPadding.x - 8 * d;
    const float row_gap = (narrow ? 6 : 12) * d;
    ImGui::Dummy(ImVec2(0, 4 * d));
    Head(cx, list_sel >= 0 ? "EDIT SUPERSTAR" : "NEW SUPERSTAR");
    Label(cx, "NAME", lab);
    ImGui::SetNextItemWidth(field);
    ImGui::InputTextWithHint("##name", "Shown on the roster", name_buf, sizeof name_buf);
    ImGui::Dummy(ImVec2(0, row_gap));
    Label(cx, "SHORT NAME", lab);
    ImGui::SetNextItemWidth(field);
    ImGui::InputTextWithHint("##short", "Shown in matches", short_buf, sizeof short_buf);
    ImGui::Dummy(ImVec2(0, row_gap));
    Label(cx, "BASED ON", lab);
    WrestlerCombo("##base", wrestlers, draft.base_id, nullptr, base_filter, sizeof base_filter, field);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + lab);
    ImGui::TextDisabled("Moveset, entrance, music and tron come from him.");
    ImGui::Dummy(ImVec2(0, row_gap));
    Label(cx, "ENTRANCE", lab);
    WrestlerCombo("##ent", entrants, draft.entrance_from, "Same as base", ent_filter, sizeof ent_filter, field);
    ImGui::Dummy(ImVec2(0, row_gap));
    Label(cx, "RATINGS", lab);
    {
      int r = draft.copy_ratings ? 0 : 1;
      static const char* const kR[2] = {"Copy from base", "Edit later in the game"};
      ImGui::SetNextItemWidth(field);
      if (ImGui::Combo("##ratings", &r, kR, 2)) draft.copy_ratings = r == 0;
    }
    ImGui::Dummy(ImVec2(0, row_gap));
    Label(cx, "SLOT", lab);
    {
      const std::vector<int> slots = SlotsFor(list_sel);
      auto show = [](int s) { return s < 0 ? std::string("no free slot") : std::to_string(s) + " (free) - Downloadable Content"; };
      ImGui::SetNextItemWidth(field);
      if (ImGui::BeginCombo("##slot", show(draft.slot).c_str(), ImGuiComboFlags_HeightLarge)) {
        for (int s : slots)
          if (ImGui::Selectable(show(s).c_str(), s == draft.slot)) draft.slot = s;
        ImGui::EndCombo();
      }
    }
    ImGui::Dummy(ImVec2(0, row_gap * 1.5f));
    const float bw = std::min(field + lab, 360 * d);
    if (SlantButton(cx, list_sel >= 0 ? "SAVE CHANGES" : "ADD TO PACK", ImVec2(bw, 44 * d), true, !draft.source_pac.empty() && name_buf[0] && !bulk_running && !Building()))
      cx.queued->push_back([this] { SaveDraft(); });

  }

  void CropStep(const ImporterCtx& cx, float w) {
    const float d = cx.d, inner = w - 2 * ImGui::GetStyle().WindowPadding.x;
    if (!img_tex.id && !img.empty()) img_tex.id = cx.make_tex(img.data(), img_w, img_h), img_tex.w = img_w, img_tex.h = img_h;
    ImGui::Dummy(ImVec2(0, 4 * d));
    Head(cx, "FRAME YOUR PICTURE");
    for (int k = 0; k < 3; ++k) {
      if (k) ImGui::SameLine(0, 6 * d);
      if (SlantButton(cx, kKindName[k], ImVec2(std::floor((inner - 12 * d) / 3), 32 * d), crop_kind == k)) crop_kind = k;
    }
    const float avail_h = ImGui::GetContentRegionAvail().y - 110 * d;
    const float box = std::max(120 * d, std::min(inner, avail_h));
    const ImVec2 a = ImGui::GetCursorScreenPos();
    const float k = std::min(box / img_w, box / img_h), iw = img_w * k, ih = img_h * k;
    const ImVec2 i0(a.x + (inner - iw) / 2, a.y);
    ImGui::SetCursorScreenPos(i0);
    ImGui::InvisibleButton("##crop", ImVec2(std::max(1.f, iw), std::max(1.f, ih)));
    float* c = crop[crop_kind];
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0, 0)) {
      const ImVec2 dd = ImGui::GetIO().MouseDelta;
      c[0] += dd.x / k, c[1] += dd.y / k;
    }
    if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0) c[2] *= std::pow(0.93f, ImGui::GetIO().MouseWheel);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    Checker(dl, i0, ImVec2(i0.x + iw, i0.y + ih), 8 * d);
    if (img_tex.id) dl->AddImage(ImTextureRef(img_tex.id), i0, ImVec2(i0.x + iw, i0.y + ih));
    const ImVec2 f0(i0.x + (c[0] - c[2] / 2) * k, i0.y + (c[1] - c[2] / 2) * k), f1(f0.x + c[2] * k, f0.y + c[2] * k);
    dl->PushClipRect(i0, ImVec2(i0.x + iw, i0.y + ih), true);
    dl->AddRectFilled(i0, ImVec2(i0.x + iw, f0.y), IM_COL32(0, 0, 0, 140));
    dl->AddRectFilled(ImVec2(i0.x, f1.y), ImVec2(i0.x + iw, i0.y + ih), IM_COL32(0, 0, 0, 140));
    dl->AddRectFilled(ImVec2(i0.x, f0.y), ImVec2(f0.x, f1.y), IM_COL32(0, 0, 0, 140));
    dl->AddRectFilled(ImVec2(f1.x, f0.y), ImVec2(i0.x + iw, f1.y), IM_COL32(0, 0, 0, 140));
    dl->PopClipRect();
    dl->AddRect(f0, f1, kRed, 0, 0, 2.5f * d);
    ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + ih + 8 * d));
    ImGui::TextDisabled("Drag the picture to move the frame. %s",
                        crop_kind == 0 ? "A: head at the top, cut at the belt, the figure filling the width."
                        : crop_kind == 1 ? "B: head to the top of the trunks, a little smaller."
                                         : "C: head to the hips, with room around him.");
    float sz = c[2] / std::max(img_w, img_h);
    ImGui::SetNextItemWidth(inner);
    if (ImGui::SliderFloat("##size", &sz, 0.1f, 1.5f, "frame size %.2f")) c[2] = sz * std::max(img_w, img_h);
    const float bw = std::floor((inner - 8 * d) / 2);
    if (SlantButton(cx, "DONE", ImVec2(bw, 40 * d), true)) cx.queued->push_back([this] { CropDone(); });
    ImGui::SameLine(0, 8 * d);
    if (SlantButton(cx, "CANCEL", ImVec2(bw, 40 * d), false)) cropping = false, Say("Picture not used.");
  }

  void BulkList(const ImporterCtx& cx, float w) {
    const float d = cx.d, inner = w - 2 * ImGui::GetStyle().WindowPadding.x;
    ImGui::Dummy(ImVec2(0, 4 * d));
    Head(cx, "IMPORT MANY");
    int take = 0;
    for (const BulkItem& b : bulk) take += b.take ? 1 : 0;
    ImGui::TextWrapped("%d of %zu file(s) will be added with the defaults: based on %s, his entrance, ratings copied, the portrait rendered from each "
                       "model. %d of %d slots used now. Each one can be edited afterwards.",
                       take, bulk.size(), WrestlerName(kBaseDefault).c_str(), Used(), importer::kMaxStars);
    const float tab_h = std::max(140 * d, ImGui::GetContentRegionAvail().y - 120 * d);
    if (ImGui::BeginTable("##bulk", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersOuterH,
                          ImVec2(inner, tab_h))) {
      ImGui::TableSetupScrollFreeze(0, 1);
      ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthStretch, 1.8f);
      ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 1.6f);
      ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthStretch, 0.5f);
      ImGui::TableSetupColumn("Notes", ImGuiTableColumnFlags_WidthStretch, 1.8f);
      ImGui::TableHeadersRow();
      for (size_t i = 0; i < bulk.size(); ++i) {
        BulkItem& b = bulk[i];
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushID(int(i));
        ImGui::BeginDisabled(!b.err.empty() || bulk_running);
        if (ImGui::Checkbox("##take", &b.take)) AssignBulkSlots();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextUnformatted(FileName(b.entry.empty() ? b.file : b.entry).c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(b.name.c_str());
        ImGui::TableNextColumn();
        if (b.slot >= 0) ImGui::Text("%d", b.slot);
        else ImGui::TextDisabled("-");
        ImGui::TableNextColumn();
        if (!b.err.empty()) ImGui::TextColored(ImVec4(1, 0.5f, 0.45f, 1), "skipped: %s", b.err.c_str());
        else if (b.done) ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1), "added");
        else if (!b.warnings.empty()) ImGui::TextColored(ImVec4(1, 0.8f, 0.35f, 1), "%s", b.warnings.front().c_str());
        else ImGui::TextDisabled(b.take ? "ok" : "left out");
        ImGui::PopID();
      }
      ImGui::EndTable();
    }
    if (bulk_running) {
      size_t done = 0, all = 0;
      for (const BulkItem& b : bulk) all += b.take ? 1 : 0, done += b.take && b.done ? 1 : 0;
      char t[64];
      std::snprintf(t, sizeof t, "rendering portraits %zu / %zu", done, all);
      ImGui::ProgressBar(all ? float(done) / float(all) : 0.f, ImVec2(inner, 0), t);
    } else {
      const float bw = std::floor((inner - 8 * d) / 2);
      if (SlantButton(cx, "ADD ALL", ImVec2(bw, 42 * d), true, take > 0 && !Building())) bulk_running = true, bulk_next = 0, bulk_added = 0;
      ImGui::SameLine(0, 8 * d);
      if (SlantButton(cx, "CANCEL", ImVec2(bw, 42 * d), false)) bulk_open = false, bulk.clear(), CleanPicked(), Say("Nothing added.");
    }
  }

  void Footer(const ImporterCtx& cx, float w, float h) {
    const float d = cx.d;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetWindowPos();
    dl->AddLine(o, ImVec2(o.x + w, o.y), IM_COL32(255, 255, 255, 50), 1.f * d);
    // the switch: the launcher's USE MY SUPERSTARS (the same setting, mirrored through its settings file)
    const float tw = 250 * d, th = std::min(40 * d, h - 18 * d), y = (h - th) / 2;
    ImGui::SetCursorPos(ImVec2(18 * d, y));
    char tl[48];
    std::snprintf(tl, sizeof tl, "USE MY SUPERSTARS: %s##sw", enabled ? "ON" : "OFF");
    if (SlantButton(cx, tl, ImVec2(tw, th), enabled)) SetEnabled(!enabled);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip(settings_linked ? "The same switch as USE MY SUPERSTARS on the launcher's MODS page"
                                        : "Not started from the launcher: the switch is not saved");
    const float bw = 260 * d;
    const bool building = job && job->state == 1;
    float x = 18 * d + tw + 18 * d;
    ImGui::SetCursorPos(ImVec2(x, y));
    const float mid_w = w - x - bw - 36 * d;
    if (building) {
      std::string ph;
      {
        std::lock_guard<std::mutex> lk(job->m);
        ph = job->phase;
      }
      ImGui::SetCursorPos(ImVec2(x, (h - ImGui::GetFrameHeight()) / 2));
      ImGui::ProgressBar(job->frac.load(), ImVec2(mid_w, 0), ph.c_str());
    } else {
      const std::string t = !msg.empty() ? msg
                            : pack.needs_build && !fit_why.empty()  // (Session 101)
                                ? "The game would leave the pack out: " + fit_why + ". BUILD INTO GAME puts it right (so does PLAY in the launcher)."
                            : pack.needs_build ? std::string("The pack changed since the last build: BUILD INTO GAME to put it in the game.")
                                               : std::string("Built into your mods folder from your own game files.");
      ImGui::SetCursorPos(ImVec2(x, (h - ImGui::GetTextLineHeight() * (mid_w < 420 * d ? 2 : 1)) / 2));
      ImGui::PushTextWrapPos(x + mid_w);
      ImGui::TextColored(msg_bad ? ImVec4(1, 0.5f, 0.45f, 1) : pack.needs_build && msg.empty() ? ImVec4(1, 0.8f, 0.35f, 1) : ImVec4(0.62f, 0.63f, 0.68f, 1),
                         "%s", t.c_str());
      ImGui::PopTextWrapPos();
    }
    ImGui::SetCursorPos(ImVec2(w - bw - 18 * d, y));
    if (SlantButton(cx, building ? "BUILDING..." : "BUILD INTO GAME", ImVec2(bw, th), true,
                    !building && !bulk_running && (!pack.stars.empty() || BuiltOnDisk())))  // (Session 101: empty = take the old pack out)
      StartBuild();
  }

  // ---------------------------------------------------------------- outside the frame
  void Pump(const ImporterCtx& cx) {
    exec = cx.exec;
    if (!started) Start();
    for (ImTextureID t : dead) cx.free_tex(t);
    dead.clear();
    if (settings_linked && std::chrono::steady_clock::now() - setting_read > std::chrono::seconds(1)) {  // (the launcher's switch, flipped there)
      setting_read = std::chrono::steady_clock::now();
      bool on = enabled;
      if (ReadSuperstarsSetting(host->settings_file, on)) enabled = on;
    }
    if (std::chrono::steady_clock::now() - pack_checked > std::chrono::seconds(1)) {  // (the launcher's REMOVE: review)
      pack_checked = std::chrono::steady_clock::now();
      CheckReload();
    }
    if (bulk_running) BulkStep();
    if (job && job->state >= 2) {
      std::string err;
      {
        std::lock_guard<std::mutex> lk(job->m);
        err = job->err;
      }
      if (job->state == 2) {
        pack.needs_build = false;
        fit_why.clear();
        // Session 101 (lane 199: "as simple as possible"): BUILD INTO GAME means "in the game", so the one switch goes on with it
        // (a player who built and never found USE MY SUPERSTARS saw nothing in the game). Not linked = no settings file to write.
        const bool was_on = enabled;
        if (pack.stars.empty()) {
          Say("The pack is taken out of the game: the game's own roster only.");
        } else {
          if (!enabled && settings_linked) SetEnabled(true);
          std::string moved;  // (Session 101, review S5b: a character moved to another slot is said, not only logged)
          for (const std::string& m : importer::LastBuildMoves()) moved += " " + m;
          Say("Built: " + std::to_string(pack.stars.size()) + " superstar" + (pack.stars.size() == 1 ? "" : "s") +
              (enabled ? (was_on ? " in the game (USE MY SUPERSTARS is on)." : " in the game (USE MY SUPERSTARS switched on).")
                       : ". Switch USE MY SUPERSTARS on to see them in the game.") + moved);
        }
      } else {
        Say("The build failed: " + err, true);
      }
      job.reset();
      if (job_thread.joinable()) job_thread.join();
    }
  }

  // ---------------------------------------------------------------- test hooks
  std::string StateJson() const {
    std::string s = "{\"stars\":[";
    for (size_t i = 0; i < pack.stars.size(); ++i) {
      const importer::Superstar& x = pack.stars[i];
      s += std::string(i ? "," : "") + "{\"slot\":" + std::to_string(x.slot) + ",\"name\":\"" + Esc(x.name) + "\",\"base\":" + std::to_string(x.base_id) +
           ",\"portrait\":" + (x.render_rgba[0].empty() ? "false" : "true") + "}";
    }
    s += "],\"enabled\":" + std::string(enabled ? "true" : "false") + ",\"linked\":" + (settings_linked ? "true" : "false") +
         ",\"needs_build\":" + (pack.needs_build ? "true" : "false") + ",\"selected\":" + std::to_string(list_sel) + ",\"draft\":{\"name\":\"" + Esc(name_buf) +
         "\",\"short\":\"" + Esc(short_buf) + "\",\"slot\":" + std::to_string(draft.slot) + ",\"base\":" + std::to_string(draft.base_id) + ",\"source\":\"" +
         Esc(draft.source_pac) + "\",\"renders\":[" + std::to_string(draft.render_w[0]) + "," + std::to_string(draft.render_w[1]) + "," +
         std::to_string(draft.render_w[2]) + "],\"model\":" + (have_model ? "true" : "false") + ",\"entrance\":" + (entrance_attire ? "true" : "false") +
         "},\"cropping\":" + (cropping ? "true" : "false") + ",\"bulk\":{\"open\":" + (bulk_open ? "true" : "false") + ",\"running\":" +
         (bulk_running ? "true" : "false") + ",\"items\":" + std::to_string(bulk.size()) + ",\"added\":" + std::to_string(bulk_added) + "},\"building\":" +
         (job && job->state == 1 ? "true" : "false") + ",\"free_slots\":" + std::to_string(free_slots.size()) + ",\"msg\":\"" + Esc(msg) + "\"}";
    return s;
  }
  std::string Command(const jl::Value& c) {
    if (!started) Start();
    auto str_list = [&](const char* k) {
      std::vector<std::string> v;
      if (const jl::Value* a = c.get(k)) {
        if (a->t == jl::Value::Str) v.push_back(a->s);
        else if (a->t == jl::Value::Arr)
          for (const jl::Value& x : a->a)
            if (x.t == jl::Value::Str) v.push_back(x.s);
      }
      return v;
    };
    if (c.has("new") && !Building()) NewDraft(), exec("{\"cmd\":\"importer_preview\",\"clear\":true}");
    if (c.has("source")) Choose(str_list("source"), false);
    if (c.has("bulk")) Choose(str_list("bulk"), true);
    if (const jl::Value* s = c.get("set")) {
      if (s->has("name")) std::snprintf(name_buf, sizeof name_buf, "%s", s->str("name").c_str());
      if (s->has("short")) std::snprintf(short_buf, sizeof short_buf, "%s", s->str("short").c_str());
      if (s->has("base")) draft.base_id = int(s->num("base", draft.base_id));
      if (s->has("entrance")) draft.entrance_from = int(s->num("entrance", -1));
      if (s->has("slot")) draft.slot = int(s->num("slot", draft.slot));
      if (const jl::Value* r = s->get("copy_ratings")) draft.copy_ratings = r->t == jl::Value::Bool ? r->b : r->n != 0;
    }
    if (const jl::Value* a = c.get("attire"); a && a->t == jl::Value::Str) entrance_attire = a->s == "entrance", LoadPreview();
    if (c.has("retake")) Retake();
    if (c.has("upload")) LoadImage(c.str("upload"));
    if (c.has("crop_kind")) crop_kind = std::clamp(int(c.num("crop_kind", 0)), 0, 2);
    if (const jl::Value* f = c.get("crop"); f && f->t == jl::Value::Arr && f->a.size() == 3 && cropping)
      for (int k = 0; k < 3; ++k) crop[crop_kind][k] = float(f->a[size_t(k)].n);
    if (c.has("crop_done") && cropping) CropDone();
    if (c.has("add")) SaveDraft();
    if (c.has("select")) Edit(int(c.num("select", -1)));
    if (c.has("remove")) RemoveSel();
    if (c.has("scroll_list")) scroll_list = true;
    if (c.has("bulk_add") && bulk_open && !Building()) {
      bulk_running = true, bulk_next = 0, bulk_added = 0;
      while (bulk_running) BulkStep();
    }
    if (const jl::Value* e = c.get("enabled"); e && e->t == jl::Value::Bool) SetEnabled(e->b);
    if (c.has("build")) {
      StartBuild();
      if (job_thread.joinable()) job_thread.join();
    }
    if (c.has("save_renders")) {  // debug: the draft's three renders as PNGs (DIR/ssfa.png ...)
      static const char* const kN[3] = {"ssfa.png", "ssfb.png", "ssfc.png"};
      for (int k = 0; k < 3; ++k) {
        if (draft.render_rgba[k].empty()) continue;
        std::vector<uint8_t> bgra = draft.render_rgba[k];
        for (size_t i = 0; i < bgra.size(); i += 4) std::swap(bgra[i], bgra[i + 2]);
        std::string err;
        WritePng(U8(fs::u8path(c.str("save_renders")) / kN[k]), bgra.data(), draft.render_w[k], draft.render_h[k], err);
      }
    }
    return "{\"ok\":true,\"importer\":" + StateJson() + "}";
  }
};

ImporterPanel::ImporterPanel() : p_(std::make_unique<Impl>()) {}
ImporterPanel::~ImporterPanel() = default;
void ImporterPanel::SetHost(ImporterHost* h) { p_->host = h; }
bool ImporterPanel::Active() const { return p_->host != nullptr; }
void ImporterPanel::Draw(const ImporterCtx& cx) { p_->Draw(cx); }
void ImporterPanel::Pump(const ImporterCtx& cx) { p_->Pump(cx); }
std::string ImporterPanel::Command(const jl::Value& cmd, const ImporterCtx& cx) {
  p_->exec = cx.exec;
  return p_->Command(cmd);
}
std::string ImporterPanel::State() const { return p_->StateJson(); }

}  // namespace ui
}  // namespace view
