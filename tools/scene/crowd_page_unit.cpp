// S103 B4F + UP unit (offline, no game files): the crowd models' stage page follows the arena's, and the stage page is ONE image per arena set. crowd_page_unit [--plant N]
// Linked with the rig's objects (build_rig.ps1 -> out\tools\ups_page_unit.exe): part 1 is the header-only CrowdPages bookkeeping, part 2 runs the REAL FrontEnd (a headless renderer, 4x4 pages):
// AdoptPage / UpdatePage / JoinCurrentPage / LeavePage (through ReleaseModel and the re-join) / PageKeyOf / the build-group image / the AddModelSharing assert.
// Planted faults (the run must FAIL, exit 0 here means "the fault was caught"):
//   1 = the crowd ignores the page (keeps key 0, the match page it was built with): "every crowd model's key equals the arena's" fails.
//   2 = the old fan-out: AdoptPage refuses and the build-group image is off (g_plant_page_per_model): every model keeps a private page, every key change is sent to each.
//   3 = JoinCurrentPage keeps a model in the group it joined first (g_plant_page_no_rejoin, review F1): a rebuilt set leaves TWO page groups and two uploads.
//   4 = CrowdPages::Add without the held key (the old assumption "a new image holds key 0", review F2): the crowd thinks a joined image holds the match page and skips the upload it needs.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../src/renderer/renderer.h"
#include "../../src/renderer_scene/crowd_page.h"
#include "../../src/renderer_scene/front_end.h"

using rdr_scene::CrowdPages;
using rdr_scene::FrontEnd;
using rdr_scene::TexTag;
static int g_fail = 0;
static void Check(bool ok, const char* what) {
  std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_fail;
}

// A model with one triangle and a 4x4 RGBA8 live texture that is its stage light-map page (texture 0).
static assets::Model PageModel(const char* label) {
  assets::Model m;
  m.label = label;
  assets::Texture t;
  t.name = "fade_lightmap01", t.format = assets::TexFormat::kR8G8B8A8, t.width = t.height = 4, t.mips = 1, t.live = true;
  t.data.assign(4 * 4 * 4, 0x11);
  m.textures.push_back(t);
  m.stage_lightmap = 0;
  assets::Mesh me;
  me.part = "tri";
  me.vertices.resize(3);
  me.indices = {0, 1, 2};
  m.meshes.push_back(me);
  return m;
}

int main(int argc, char** argv) {
  int plant = 0;
  for (int i = 1; i + 1 < argc; ++i)
    if (std::string(argv[i]) == "--plant") plant = std::atoi(argv[i + 1]);
  rdr_scene::g_plant_page_per_model = plant == 2;
  rdr_scene::g_plant_page_no_rejoin = plant == 3;

  // ---------------- part 1: CrowdPages (no renderer) ----------------
  {
    CrowdPages pg;
    if (plant == 1) pg.PlantIgnore();
    int uploads = 0;
    std::vector<uint8_t> last;
    auto up = [&](int, int, uint32_t w, uint32_t h, const uint8_t* px) {
      ++uploads;
      last.assign(px, px + size_t(w) * h * 4);
      return true;
    };
    std::vector<uint8_t> entrance(4 * 4 * 4, 0x55), match(4 * 4 * 4, 0x22);
    // 11 models on ONE shared page image (id 7: LiveCrowd's shared_images makes the classes, the card quad and the props share it); the 12th has its own image (id 9)
    for (int m = 0; m < 12; ++m) pg.Add(m, 3, m == 11 ? 9 : 7);
    Check(pg.AllAt(0), "built: every crowd model holds the match page (key 0)");
    pg.Set(0xABCD, 4, 4, entrance.data(), up);  // an entrance UpdatePages
    Check(pg.AllAt(0xABCD), "entrance: every crowd model's page key equals the arena's");
    Check(uploads == 2 && !last.empty() && last[0] == 0x55, "entrance: one upload per distinct image (2), with the entrance pixels");
    pg.Set(0xABCD, 4, 4, entrance.data(), up);
    Check(uploads == 2, "a steady entrance uploads nothing");
    pg.Add(12, 3, 7);  // a class resident later, its image already holds the page
    pg.Sync(up);
    Check(pg.AllAt(0xABCD) && uploads == 2, "a later model on the shared image takes the current key with no upload");
    pg.Add(13, 3, 21);  // a later model with a NEW image (built with the match page)
    pg.Sync(up);
    Check(pg.AllAt(0xABCD) && uploads == 3, "a later model with a new image is uploaded the current page");
    pg.Set(0, 4, 4, match.data(), up);  // back to the match
    Check(pg.AllAt(0) && !last.empty() && last[0] == 0x22, "match: every crowd model back on key 0 with the match pixels");
    pg.Clear();
    Check(pg.slots().empty() && pg.key() == 0, "cleared (arena change): no slots, key reset");
    pg.Set(1, 4, 4, entrance.data(), up);
    pg.Clear();
    const int before_clear = uploads;
    pg.Add(0, 3, 31);
    pg.Sync(up);
    Check(uploads == before_clear && pg.key() == 0 && pg.AllAt(0), "arena change: a new model uploads nothing (the old arena's page is gone), key 0");
    pg.Set(1, 4, 4, entrance.data(), up);
    Check(uploads == before_clear + 1 && pg.AllAt(1), "arena change: the next UpdatePages resends the page");
    bool refuse = true;
    auto up2 = [&](int, int, uint32_t w, uint32_t h, const uint8_t* px) { if (refuse) return false; ++uploads; last.assign(px, px + size_t(w) * h * 4); return true; };
    pg.Set(0x77, 4, 4, match.data(), up2);
    Check(pg.HeldKey() != 0x77 && !pg.AllAt(0x77), "refused upload: the crowd is stale (HeldKey != key)");
    refuse = false;
    pg.Set(0x77, 4, 4, match.data(), up2);
    Check(pg.HeldKey() == 0x77 && pg.AllAt(0x77), "refused upload: retried and held");
    {  // S103 UP review F2: Add seeds the image's key from the front end's held key; Rebind follows a slot to another image
      CrowdPages sd;
      int n = 0;
      auto cnt = [&](int, int, uint32_t, uint32_t, const uint8_t*) { ++n; return true; };
      sd.Add(1, 3, 50, 0x9);   // the image holds the entrance page 0x9 already
      Check(sd.AllAt(0x9) && sd.slots()[0].key == 0x9, "Add(held): a joined image's slot takes the key the image holds, not 0");
      sd.Set(0x9, 4, 4, entrance.data(), cnt);
      Check(n == 0, "Add(held): the held page is not sent again");
      sd.Rebind(0, 60, CrowdPages::kUnknown);   // the model left the group: its own image, who-knows-what
      sd.Set(0x9, 4, 4, entrance.data(), cnt);
      Check(n == 1, "Rebind(unknown): the page is sent to the image the slot came back to");
    }
  }

  // ---------------- part 2: the real FrontEnd ----------------
  std::string err;
  rdr::RendererOptions ro;
  auto renderer = rdr::Renderer::Create(ro, err);
  if (!renderer) { std::printf("FAIL renderer: %s\n", err.c_str()); return 1; }
  {
    FrontEnd fe(*renderer);
    std::vector<std::string> warn;
    const std::vector<uint8_t> A(4 * 4 * 4, 0x55), B(4 * 4 * 4, 0x22);
    const auto pages = [&] { return fe.texture_update_count(TexTag::Page); };
    // --- group 1: 8 arena models; the first adopter's image becomes the group's, the rest are BUILT on it (SetBuildPageGroup: no private copy) ---
    std::vector<int> set1;
    for (int i = 0; i < 8; ++i) {
      fe.SetBuildPageGroup(1);
      const int id = fe.AddModel(PageModel("arena"), warn);
      fe.SetBuildPageGroup(0);
      fe.AdoptPage(id, 0, 1);
      set1.push_back(id);
    }
    bool same = true;
    for (int id : set1) same = same && fe.TextureImage(id, 0) == fe.TextureImage(set1[0], 0) && fe.TextureImage(id, 0) != 0;
    Check(same == (plant != 2), "8 arena models: ONE page image (planted fan-out: private copies)");
    Check(fe.page_groups() == (plant == 2 ? 0u : 1u) && fe.page_share_models() == (plant == 2 ? 0u : 8u), "8 arena models: 1 page group, 8 models on it");
    // --- uploads: one per key change for the whole set ---
    for (int id : set1) fe.UpdatePage(id, 0, 0x11, 4, 4, A.data());
    Check(pages() == 1, "key change: ONE page upload for the whole set (8 callers)");
    for (int id : set1) fe.UpdatePage(id, 0, 0x11, 4, 4, A.data());
    Check(pages() == 1, "the same key again sends nothing");
    for (int id : set1) fe.UpdatePage(id, 0, 0x12, 4, 4, A.data());
    Check(pages() == 1 && fe.page_skips_hash() >= 1, "a new key with the SAME bytes sends nothing (memcmp guard: same bytes counted)");
    for (int id : set1) fe.UpdatePage(id, 0, 0x13, 4, 4, B.data());
    Check(pages() == 2, "a new key with new bytes: ONE more upload");
    rdr_scene::g_rig_page_per_caller = true;   // the +dev switch's stand-in: every caller sends into the same image
    for (int id : set1) fe.UpdatePage(id, 0, 0x14, 4, 4, A.data());
    rdr_scene::g_rig_page_per_caller = false;
    Check(pages() == 2 + 8, "page_per_caller ON: every caller sends (the old byte cost: 8 uploads for 8 callers)");
    for (int id : set1) fe.UpdatePage(id, 0, 0x15, 4, 4, A.data());   // the guard's kept copy was cleared by the per-caller sends: this one re-sends once
    Check(pages() == 2 + 8 + 1, "after per-caller sends the next key change is ONE upload again");
    const uint64_t k1 = 0x15;
    // --- AddModelSharing from a page-group owner is refused (assert); from a plain owner it works ---
    {
      const int plain = fe.AddModel(PageModel("plain owner"), warn);
      const int s_ok = fe.AddModelSharing(PageModel("sharer"), plain, warn);
      Check(s_ok >= 0 && fe.page_share_violations() == 0, "AddModelSharing from an owner with no page group: allowed");
      const int s_bad = plant == 2 ? -1 : fe.AddModelSharing(PageModel("sharer"), set1[0], warn);
      Check(plant == 2 || (s_bad == -1 && fe.page_share_violations() == 1), "AddModelSharing from a page-group owner: refused and counted");
      if (s_ok >= 0) fe.ReleaseModel(s_ok);
      fe.ReleaseModel(plain);
    }
    // --- the crowd: a model whose images belong to the crowd (images_external) joins the current group ---
    std::unordered_map<std::string, rdr::ImageId> crowd_images;
    const int crowd = fe.AddModel(PageModel("crowd"), warn, &crowd_images);
    Check(!fe.JoinCurrentPage(crowd, 0), "no current group: a crowd model joins nothing");
    fe.SetCurrentPageGroup(1);
    const bool joined = fe.JoinCurrentPage(crowd, 0);
    Check(joined == (plant != 2) && (plant == 2 || (fe.TextureImage(crowd, 0) == fe.TextureImage(set1[0], 0) && fe.PageGroupOf(crowd) == 1)), "a crowd model joins the current group: the arena's one page image");
    // review F2: the crowd's bookkeeping takes the key the joined image HOLDS (k1), never an assumed 0
    CrowdPages cp;
    if (plant == 4) cp.Add(crowd, 0, fe.TextureImage(crowd, 0));
    else cp.Add(crowd, 0, fe.TextureImage(crowd, 0), fe.PageKeyOf(crowd, 0));
    const auto crowd_up = [&](uint64_t key) {
      return [&, key](int m, int t, uint32_t w, uint32_t h, const uint8_t* px) { return fe.UpdatePage(m, size_t(t), key, w, h, px); };
    };
    cp.Set(0, 4, 4, B.data(), crowd_up(0));   // the arena goes back to the match page (key 0)
    Check(plant == 2 || fe.PageKeyOf(crowd, 0) == 0, "review F2: the image the crowd joined holds the key the crowd set (a joined image is not assumed to hold key 0)");
    for (int id : set1) fe.UpdatePage(id, 0, 0, 4, 4, B.data());   // the arena's own call: the key is held already
    (void)k1;
    // --- a rebuilt set (a new signature): group 2 with new models; the old set's models are released; the crowd keeps its model (review F1) ---
    std::vector<int> set2;
    fe.SetCurrentPageGroup(0);   // review F2: the retiring set's group is no longer joinable
    {
      const int stray = fe.AddModel(PageModel("late crowd"), warn, &crowd_images);
      Check(!fe.JoinCurrentPage(stray, 0), "retired set: no current group, a new crowd model joins nothing (never a retired set's group)");
      fe.ReleaseModel(stray);
    }
    for (int i = 0; i < 8; ++i) {
      fe.SetBuildPageGroup(2);
      const int id = fe.AddModel(PageModel("arena2"), warn);
      fe.SetBuildPageGroup(0);
      fe.AdoptPage(id, 0, 2);
      set2.push_back(id);
    }
    fe.SetCurrentPageGroup(2);
    for (int id : set1) fe.ReleaseModel(id);   // the old set's models (the live drawer releases them a few frames later)
    Check(fe.page_groups() == (plant == 2 ? 0u : 2u), "rebuilt set: the old group lives while the crowd still sits on it (1 reference)");
    const bool rejoined = fe.JoinCurrentPage(crowd, 0);
    Check(plant == 2 || plant == 3 || (rejoined && fe.PageGroupOf(crowd) == 2 && fe.TextureImage(crowd, 0) == fe.TextureImage(set2[0], 0)), "review F1: the crowd leaves the retired group and joins the rebuilt set's: one page image");
    Check(plant == 2 || fe.page_groups() == 1, "review F1: ONE page group alive after the rebuild (the old group dies with its last reference)");
    // the crowd's slot follows its image and the next key sends ONE upload for the whole new set
    const uint64_t before = pages();
    if (plant != 2 && fe.TextureImage(crowd, 0) != cp.slots()[0].image) cp.Rebind(0, fe.TextureImage(crowd, 0), fe.PageKeyOf(crowd, 0));
    for (int id : set2) fe.UpdatePage(id, 0, 0x21, 4, 4, A.data());
    cp.Set(0x21, 4, 4, A.data(), crowd_up(0x21));
    Check(plant == 2 || pages() == before + 1, "rebuilt set: ONE upload for the key change (arena models + crowd)");
    Check(plant == 2 || (fe.PageKeyOf(crowd, 0) == 0x21 && cp.AllAt(0x21)), "rebuilt set: the crowd's bookkeeping matches the image it sits on");
    // --- release everything: no reference leaks ---
    fe.ReleaseModel(crowd);
    for (int id : set2) fe.ReleaseModel(id);
    Check(fe.page_groups() == 0 && fe.page_share_models() == 0, "all models released: no page group, no reference left");
    renderer->WaitIdle();
    for (auto& kv : crowd_images) if (kv.second != rdr::kNoImage) renderer->DestroyImage(kv.second);
  }
  std::printf(g_fail ? "FAILED %d\n" : "ALL PASS\n", g_fail);
  return g_fail ? 1 : 0;
}
