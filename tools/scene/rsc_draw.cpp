// rsc_draw: loads a scene record from a file and draws it through the scene front-end into a PNG, with no viewer state
// (Session 101, lane 14). Assets are loaded by pac + entry from the player's own files with our loaders (the viewer's loader seam).
//   rsc_draw FILE.rsc --game DIR --out PICTURE.png [--size 640x360]
// Exit 0 when the picture is written; 2 on a bad record; 3 on any other failure (the reason on stderr).
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <array>
#include <filesystem>
#include <map>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "../../src/assets/crowd_body.h"  // S101 lane27: the crowd's people by pac + entry 20000.., "card:TT"
#include "../../src/assets/view/loader_seam.h"
#include "../../src/renderer/renderer.h"
#include "../../src/renderer_scene/front_end.h"
#include "../../src/renderer_scene/record_io.h"
#include "../../src/scene_live/arena_apply.h"  // S101 lane28: the arena contributor's slot objects and light sets

using namespace rdr_scene;

namespace {

uint32_t Crc32(const uint8_t* d, size_t n, uint32_t crc = 0) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    init = true;
  }
  crc = ~crc;
  for (size_t i = 0; i < n; ++i) crc = table[(crc ^ d[i]) & 255] ^ (crc >> 8);
  return ~crc;
}

void Put32(std::vector<uint8_t>& v, uint32_t x) { for (int s = 24; s >= 0; s -= 8) v.push_back(uint8_t(x >> s)); }

void Chunk(std::vector<uint8_t>& out, const char* kind, const std::vector<uint8_t>& body) {
  Put32(out, uint32_t(body.size()));
  std::vector<uint8_t> kb(kind, kind + 4);
  kb.insert(kb.end(), body.begin(), body.end());
  out.insert(out.end(), kb.begin(), kb.end());
  Put32(out, Crc32(kb.data(), kb.size()));
}

// RGBA8 -> RGB PNG with stored (uncompressed) deflate blocks: no zlib needed, the files are for comparison, not for sharing.
bool WritePngRgb(const std::string& path, const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h) {
  std::vector<uint8_t> raw;
  raw.reserve(size_t(h) * (size_t(w) * 3 + 1));
  for (uint32_t y = 0; y < h; ++y) {
    raw.push_back(0);
    for (uint32_t x = 0; x < w; ++x) {
      const uint8_t* p = &rgba[(size_t(y) * w + x) * 4];
      raw.push_back(p[0]), raw.push_back(p[1]), raw.push_back(p[2]);
    }
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  uint32_t a = 1, b = 0;
  for (uint8_t c : raw) a = (a + c) % 65521, b = (b + a) % 65521;
  for (size_t pos = 0; pos < raw.size() || pos == 0;) {
    const size_t n = std::min<size_t>(65535, raw.size() - pos);
    const bool last = pos + n >= raw.size();
    z.push_back(last ? 1 : 0);
    z.push_back(uint8_t(n & 255)), z.push_back(uint8_t(n >> 8)), z.push_back(uint8_t(~n & 255)), z.push_back(uint8_t((~n >> 8) & 255));
    z.insert(z.end(), raw.begin() + long(pos), raw.begin() + long(pos + n));
    pos += n;
    if (last) break;
  }
  Put32(z, (b << 16) | a);
  std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  std::vector<uint8_t> ihdr;
  Put32(ihdr, w), Put32(ihdr, h);
  ihdr.push_back(8), ihdr.push_back(2), ihdr.push_back(0), ihdr.push_back(0), ihdr.push_back(0);
  Chunk(out, "IHDR", ihdr);
  Chunk(out, "IDAT", z);
  Chunk(out, "IEND", {});
  std::error_code ec;
  const std::filesystem::path p(path);
  if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
  return bool(f);
}

// "pac\\bg\\bg05.pac" -> arena 5; "pac\\ch\\ch107.pac" + variant -> wrestler 107 attire V; "pac\\evt\\evtobj.pac" + entry N -> prop N.
bool LoadAsset(const AssetRef& a, const std::string& pac, const std::string& entry, assets::Model& m, std::string& err) {
  std::string norm = pac;
  std::replace(norm.begin(), norm.end(), '/', '\\');
  int id = 0;
  // S101 lane27: a crowd person: arena pac + entry 20000 + 40 * LOD + type (only LOD 0, the near body, is loaded: the viewer has no other), or
  // "card:TT" (our renderer draws the type's body for the 360's run-time impostor; C2-1). The stored pose: the record's palette poses it.
  {
    int arena = 0, num = 0, type = 0;
    if (std::sscanf(norm.c_str(), "pac\\bg\\bg%d.pac", &arena) == 1 && (std::sscanf(entry.c_str(), "card:%d", &type) == 1 ||
                                                                          (std::sscanf(entry.c_str(), "%d", &num) == 1 && num >= 20000 && num < 20200))) {
      if (entry.rfind("card:", 0) != 0) {
        // 20000 + 40 * LOD + type. The viewer has no LOD 1 / 2 bodies: they are drawn as the type's near body (LOD 0) with the palette the record
        // gives them (right when the LOD's remap is the identity, as in the offline fixture; the live remap is a subset [U]); said on stderr.
        const int lod = (num - 20000) / 40;
        type = (num - 20000) % 40;
        if (lod > 2 || type > 19) { err = "crowd body " + entry + ": not a person body (20000 + 40 * LOD(0..2) + type(0..19))"; return false; }
        if (lod > 0) std::fprintf(stderr, "[rsc_draw] crowd body %s: LOD %d drawn as the near body of type %d\n", entry.c_str(), lod, type);
      }
      if (type < 0 || type > 19) { err = "crowd body type out of range: " + entry; return false; }
      return assets::crowd_body::LoadBody(arena, type, m, err);
    }
  }
  if (std::sscanf(norm.c_str(), "pac\\bg\\bg%d.pac", &id) == 1) return ViewerLoad("arena", id, 0, m, err);
  if (std::sscanf(norm.c_str(), "pac\\ch\\ch%d.pac", &id) == 1) return ViewerLoad("wrestler", id, int(a.variant), m, err);
  if (norm == "pac\\evt\\evtobj.pac" && std::sscanf(entry.c_str(), "%d", &id) == 1) return ViewerLoad("prop", id, 0, m, err);
  err = "no loader for pac \"" + pac + "\" entry \"" + entry + "\"";
  return false;
}

// S101 lane27 / lane 1 "One space rule for Live records": a Live record is in the game's space (file space, the game's camera); our loaders build meshes in the
// viewer's space (Y mirrored), so the WHOLE record is converted once at load: world -> X matrices take S on the world side (the camera's view / view-projection
// rows: S * VP), every object's world and every bone palette S X S (S = diag(1,-1,1) = assets::MirrorY), the eye's y negates. Then it draws as a Viewer record.
void ConvertLiveToViewer(SceneTables& t) {
  auto mirror = [](Mat4& m) {
    assets::Matrix a;
    std::memcpy(a.data(), m.m, sizeof(float) * 16);
    a = assets::MirrorY(a);
    std::memcpy(m.m, a.data(), sizeof(float) * 16);
  };
  for (int k = 4; k < 8; ++k) t.camera.view.m[k] = -t.camera.view.m[k];  // world -> view / clip: p_game * S * V
  t.camera.eye[1] = -t.camera.eye[1];
  for (Object& o : t.objects) mirror(o.world);
  for (Mat4& m : t.bones) mirror(m);
  t.header.source = uint8_t(Source::Viewer);  // (converted: from here on it is a viewer-space record)
}

}  // namespace

// S101 lane 26: `path` names a folder: it becomes <folder>/scene/frame_NNNNNN.rsc (or <folder>/frame_NNNNNN.rsc when it is the scene folder itself).
static bool ResolveRecord(std::string& path, long frame, std::string& err) {
  std::error_code ec;
  if (!std::filesystem::is_directory(path, ec)) return true;
  if (frame < 0) { err = "a folder needs --frame N"; return false; }
  char name[32];
  std::snprintf(name, sizeof(name), "frame_%06ld.rsc", frame);
  std::filesystem::path p = std::filesystem::path(path) / "scene" / name;
  if (!std::filesystem::exists(p, ec)) p = std::filesystem::path(path) / name;
  if (!std::filesystem::exists(p, ec)) { err = "no scene record for frame " + std::to_string(frame) + " under " + path + " (taken without --wwe2k14_scene_records?)"; return false; }
  path = p.string();
  return true;
}

int main(int argc, char** argv) {
  std::string rec, game, out;
  uint32_t w = 640, h = 360;
  int keep_kind = 0;
  long frame = -1;  // S101 lane 26: FILE may be a capture folder (or its scene folder): --frame N picks frame_NNNNNN.rsc
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--game" && i + 1 < argc) game = argv[++i];
    else if (a == "--out" && i + 1 < argc) out = argv[++i];
    else if (a == "--frame" && i + 1 < argc) frame = std::atol(argv[++i]);
    else if (a == "--size" && i + 1 < argc) { if (std::sscanf(argv[++i], "%ux%u", &w, &h) != 2) { std::fprintf(stderr, "bad --size\n"); return 1; } }
    else if (a == "--keep-src-kind" && i + 1 < argc) keep_kind = std::atoi(argv[++i]);  // S101 lane27: keep only meshes of Mesh::src_kind K (3 = the crowd)
    else if (rec.empty()) rec = a;
  }
  if (rec.empty() || game.empty() || out.empty()) {
    std::fprintf(stderr, "usage: rsc_draw FILE.rsc|CAPTURE_FOLDER --frame N --game DIR --out PICTURE.png [--size 640x360]\n");
    return 1;
  }
  SceneTables t;
  std::string err;
  if (!ResolveRecord(rec, frame, err)) { std::fprintf(stderr, "rsc_draw: FAIL %s\n", err.c_str()); return 2; }
  if (!ReadRecord(rec, t, err)) { std::fprintf(stderr, "rsc_draw: FAIL bad record: %s\n", err.c_str()); return 2; }
  if (!t.has_camera) { std::fprintf(stderr, "rsc_draw: FAIL the record has no camera\n"); return 2; }
  if (t.header.source == uint8_t(Source::Live)) ConvertLiveToViewer(t);  // S101 lane27: one space rule
  ViewerInit(game);
  assets::crowd_body::Init(game);  // S101 lane27

  rdr::RendererOptions ro;
  if (const char* v = std::getenv("WWE_RDR_VALIDATION")) ro.validation = std::atoi(v);
  auto renderer = rdr::Renderer::Create(ro, err);
  if (!renderer) { std::fprintf(stderr, "rsc_draw: FAIL renderer: %s\n", err.c_str()); return 3; }
  FrontEndOptions fo;
  if (const char* v = std::getenv("WWE_RDR_NO_TEXTURES")) fo.textures = std::atoi(v) == 0;
  int rc = 0;
  {
    FrontEnd fe(*renderer, fo);
    std::vector<int> model_of(t.assets.size(), -1);
    std::vector<assets::Model> crowd_src(t.assets.size());  // S101 lane27: the stored-pose crowd bodies, posed per palette below
    std::vector<char> is_crowd(t.assets.size(), 0);
    // S101 lane28: the arena contributor writes one object per stage slot ("arena:slot:K[:r|:y]", asset = pac\bg\bgNN.pac + entry "slot K").
    // The front-end draws a whole arena model, so the arena is loaded once, made to show what the record says (light sets by id, each slot's
    // set, the twins present, absent slots hidden: arena_apply.h) and drawn as one object. Counts go to stderr for the selftest.
    live::arena::RecordView arena_view;
    std::map<int, int> arena_model;  // arena id -> front-end model
    {
      std::string aerr;
      if (!live::arena::ReadArenaRecord(t, arena_view, aerr)) { std::fprintf(stderr, "rsc_draw: FAIL bad arena part: %s\n", aerr.c_str()); return 2; }
    }
    size_t unnamed_assets = 0;
    for (size_t i = 0; i < t.assets.size(); ++i) {
      const AssetRef& a = t.assets[i];
      if (a.kind != uint8_t(AssetKind::Model)) continue;  // textures / fonts / movies: the 2D layer, not drawn here
      if (a.pac == 0) { ++unnamed_assets; continue; }  // S101 lane29: asset unknown (a live contributor placeholder): its objects are skipped and counted
      int arena_id = 0, slot = 0;
      if (live::arena::ParseSlotAsset(t, a, arena_id, slot)) {
        auto it = arena_model.find(arena_id);
        if (it == arena_model.end()) {
          assets::Model am;
          if (!ViewerLoad("arena", arena_id, 0, am, err)) { std::fprintf(stderr, "rsc_draw: FAIL arena %d: %s\n", arena_id, err.c_str()); rc = 3; break; }
          const live::arena::ApplyStats st = live::arena::ApplyToModel(arena_view, am);
          std::fprintf(stderr, "[rsc_draw] arena %d from %zu slot objects: sets replaced %d added %d, slot sets %d, slots hidden %d, twins kept %d dropped %d\n", arena_id,
                       arena_view.slots.size(), st.sets_replaced, st.sets_added, st.slots_set, st.slots_hidden, st.twins_kept, st.twins_dropped);
          std::vector<std::string> warnings;
          it = arena_model.emplace(arena_id, fe.AddModel(am, warnings)).first;
          for (const std::string& s : warnings) std::fprintf(stderr, "[rsc_draw] %s\n", s.c_str());
        }
        model_of[i] = it->second;
        continue;
      }
      assets::Model m;
      if (!LoadAsset(a, std::string(t.Str(a.pac)), std::string(t.Str(a.entry)), m, err)) { std::fprintf(stderr, "rsc_draw: FAIL asset %zu: %s\n", i, err.c_str()); rc = 3; break; }
      const std::string pac_s(t.Str(a.pac)), entry_s(t.Str(a.entry));
      const bool crowd_asset = entry_s.rfind("card:", 0) == 0 || (std::atoi(entry_s.c_str()) >= 20000 && std::atoi(entry_s.c_str()) < 20200 && pac_s.find("bg") != std::string::npos);
      if (keep_kind > 0 && !crowd_asset) {  // S101 lane27: the filter (a picture of one source kind only)
        std::vector<assets::Mesh> kept;
        for (assets::Mesh& me : m.meshes) if (me.src_kind == keep_kind) kept.push_back(std::move(me));
        m.meshes = std::move(kept);
      }
      if (crowd_asset) { is_crowd[i] = 1; crowd_src[i] = m; }
      std::vector<std::string> warnings;
      model_of[i] = fe.AddModel(m, warnings);
      for (const std::string& s : warnings) std::fprintf(stderr, "[rsc_draw] %s\n", s.c_str());
    }
    if (rc == 0) {
      SceneFrame f;
      f.camera = t.camera;
      f.width = w, f.height = h;
      f.time_s = t.header.time_s;
      std::map<std::array<uint32_t, 3>, int> posed_of;  // S101 lane27: (asset, bones_first, bones_count) -> the model posed with that palette
      uint32_t crowd_objects = 0, crowd_posed = 0;
      f.bones = t.bones;                                         // S101 lane29: the skin palettes (Object::bones_*); a skinned object was drawn rigid without them
      f.viewer_space = true;  // the tables are in viewer space: a Viewer record as written, a Live record after ConvertLiveToViewer (SCENE_RECORD "One space rule")
      for (const Object& o : t.objects) {
        Object c = o;
        if (o.asset >= model_of.size() || model_of[o.asset] < 0) continue;
        int ai = 0, sl = 0;
        if (live::arena::ParseSlotAsset(t, t.assets[o.asset], ai, sl)) continue;  // S101 lane28: drawn once per arena below
        c.asset = uint32_t(model_of[o.asset]);
        if (o.klass == rdr_adapter::DrawClass::Crowd && is_crowd[o.asset]) {
          // (the record is already in viewer space: ConvertLiveToViewer; the palette is applied on the CPU as the game skins the crowd on the CPU)
          ++crowd_objects;
          if ((o.flags & kObjSkinned) && o.bones_count && size_t(o.bones_first) + o.bones_count <= t.bones.size()) {
            const std::array<uint32_t, 3> key = {o.asset, o.bones_first, o.bones_count};
            auto it = posed_of.find(key);
            if (it == posed_of.end()) {
              assets::Model posed;
              assets::crowd_body::PoseBody(crowd_src[o.asset], t.bones[o.bones_first].m, o.bones_count, posed);
              std::vector<std::string> pw;
              it = posed_of.emplace(key, fe.AddModel(posed, pw)).first;
            }
            c.asset = uint32_t(it->second);
            ++crowd_posed;
          }
          c.flags &= ~uint32_t(kObjSkinned);  // (already posed: the front-end must not look for a palette)
        }
        f.objects.push_back(c);
      }
      if (crowd_objects) std::fprintf(stderr, "[rsc_draw] crowd: %u objects, %u with a palette (%zu posed models)\n", crowd_objects, crowd_posed, posed_of.size());
      for (auto& kv : arena_model) {  // S101 lane28: the arena of the slot objects, as one object
        Object c;
        c.asset = uint32_t(kv.second);
        c.world = IdentityMat4();
        f.objects.push_back(c);
      }
      std::vector<uint8_t> rgba;
      if (!fe.RenderToPixels(f, rgba, err)) { std::fprintf(stderr, "rsc_draw: FAIL draw: %s\n", err.c_str()); rc = 3; }
      else if (!WritePngRgb(out, rgba, w, h)) { std::fprintf(stderr, "rsc_draw: FAIL cannot write %s\n", out.c_str()); rc = 3; }
      else {
        if (unnamed_assets) std::fprintf(stderr, "[rsc_draw] %zu asset(s) with no pac (asset unknown): their objects are skipped\n", unnamed_assets);
        std::fprintf(stderr, "[rsc_draw] %zu objects drawn, validation errors: %llu\n%s", f.objects.size(), (unsigned long long)fe.validation_errors(), fe.CoverageText().c_str());
        std::printf("wrote %s (%ux%u)\n", out.c_str(), w, h);
      }
    }
    renderer->WaitIdle();
  }
  return rc;
}
