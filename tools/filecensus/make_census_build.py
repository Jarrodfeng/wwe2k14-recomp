#!/usr/bin/env python3
"""Lane 21 (file census): prepares the TEMP copy of the scene front-end the census build compiles, and the build script (both under out\\filecensus\\build\\
of --root; nothing is committed, src\\ is not touched).

  py -I tools\\filecensus\\make_census_build.py [--root DIR]      then:  pwsh -NoProfile -File out\\filecensus\\build\\build_census_viewer.ps1

The copy of src\\renderer_scene\\front_end.cpp differs by test-only code, off unless an environment variable is set (S101 lane 21):
  * always: "[census] model 'LABEL': N mirrored/turned placements, M textures" per model (the loader's in-memory count; NOT independent of the loader) and, per
    shot, "[census] submitted: A flagged placements with more than one instance, B instances" = what RenderToPixels' draw loop really puts in the draw list
    (the board compares A with the r / y slot count read from the FILE, tools\\filecensus\\filecensus_flags.py);
  * WWE_CENSUS_PLANT_MIRROR=1: rdr's object list drops the flagged copies (every placement labelled "slot N mirrored / turned" keeps only its first transform);
  * WWE_CENSUS_PLANT_LOADER=1: S97's fault at the loader level, simulated in this copy: the flags were never read, so the model holds no mirrored placement (the
    in-memory count is 0 as well; only the file count and the draw list can see it);
  * WWE_CENSUS_PLANT_TEX="arena 7": the first diffuse texture of the model with that label is "not found" (kNoImage + the front-end's own warning);
  * WWE_CENSUS_PLANT_HANG="arena 5:30": AddModel of that model sleeps 30 s (a hang must become a board row).
The build script is build_viewer.ps1 with text substitutions (the repo path, the exe and object folder names, the front_end.cpp path, a seed of the
imgui / SDL3 objects from the main checkout); it fails loudly when an anchor line of either file is gone."""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))

TEX_OLD = ('  for (const assets::Texture& t : model.textures) {\n    std::string w;\n    const rdr::ImageId id = MakeTexture(t, w);\n'
           '    if (id == rdr::kNoImage) warnings.push_back("texture \'" + t.name + "\': " + w);\n')
TEX_NEW = '''  // S101 lane 21 (census build only): the planted missing texture
  int census_drop = -1;
  if (const char* pt = std::getenv("WWE_CENSUS_PLANT_TEX")) {
    if (model.label == pt)
      for (const assets::Material& mt : model.materials)
        if (mt.diffuse >= 0 && size_t(mt.diffuse) < model.textures.size() && mt.diffuse != model.stage_lightmap) { census_drop = mt.diffuse; break; }
  }
  for (size_t census_i = 0; census_i < model.textures.size(); ++census_i) {
    const assets::Texture& t = model.textures[census_i];
    std::string w;
    rdr::ImageId id = MakeTexture(t, w);
    if (int(census_i) == census_drop) { if (id != rdr::kNoImage) renderer_.DestroyImage(id); id = rdr::kNoImage; w = "entry not found (census plant)"; }
    if (id == rdr::kNoImage) warnings.push_back("texture '" + t.name + "': " + w);
'''
PL_OLD = '  for (const assets::Placement& p : model.placements) m->placements.push_back(p.transforms);\n'
PL_NEW = '''  {  // S101 lane 21 (census build only): the ground truth line and the planted "ignores the r / y flags"
    int census_flagged = 0;
    const bool census_plant = std::getenv("WWE_CENSUS_PLANT_MIRROR") && std::atoi(std::getenv("WWE_CENSUS_PLANT_MIRROR")) != 0;   // rdr's list drops the flagged copies
    const bool census_loader = std::getenv("WWE_CENSUS_PLANT_LOADER") && std::atoi(std::getenv("WWE_CENSUS_PLANT_LOADER")) != 0;  // the loader never made them
    const char* census_slot_env = std::getenv("WWE_CENSUS_PLANT_SLOT");  // S101 lane 37: drop the flagged copies of ONE slot ("44")
    for (const assets::Placement& p : model.placements) {
      std::vector<assets::Matrix> t = p.transforms;
      if (p.label.find(" mirrored") != std::string::npos || p.label.find(" turned") != std::string::npos) {
        const bool census_slot_plant = census_slot_env && p.label.compare(0, 5, "slot ") == 0 && std::atoi(p.label.c_str() + 5) == std::atoi(census_slot_env);
        if (census_loader || census_slot_plant) {
          if (t.size() > 1) t.resize(1);  // S97's fault at the loader level: the flags were not read, so the model holds no mirrored placement at all
        } else {
          ++census_flagged;
          g_census_flagged[m].insert(int(m->placements.size()));
          g_census_slot[{m, int(m->placements.size())}] = std::atoi(p.label.c_str() + 5);  // S101 lane 37: "slot N mirrored" -> N
          if (census_plant && t.size() > 1) t.resize(1);
        }
      }
      m->placements.push_back(t);
    }
    std::fprintf(stderr, "[census] model '%s': %d mirrored/turned placements, %d textures\\n", model.label.c_str(), census_flagged, int(model.textures.size()));
  }
'''
# globals for the draw-list count, before AddModel
GL_OLD = 'int FrontEnd::AddModel(const assets::Model& model, std::vector<std::string>& warnings) {\n'
GL_NEW = '''// S101 lane 21 (census build only): which placements of which model carry the loader's "mirrored" / "turned" label, and which of them the draw list got
// more than once in the last RenderToPixels.
static std::map<const void*, std::set<int>> g_census_flagged;
static std::set<std::pair<const void*, int>> g_census_sub;
static size_t g_census_sub_instances = 0;
static std::map<std::pair<const void*, int>, int> g_census_slot;  // S101 lane 37: (model, placement) -> the stage slot of a flagged placement

''' + GL_OLD
HANG_OLD = '  auto* m = new ModelData();\n  m->label = model.label;\n'
HANG_NEW = HANG_OLD + '''  if (const char* hang = std::getenv("WWE_CENSUS_PLANT_HANG")) {  // planted hang "LABEL:SECONDS": the model's load sleeps (a hang must become a board row)
    const std::string spec = hang;
    const size_t colon = spec.rfind(':');
    if (colon != std::string::npos && model.label == spec.substr(0, colon)) std::this_thread::sleep_for(std::chrono::seconds(std::atoi(spec.c_str() + colon + 1)));
  }
'''
INST_OLD = '      const size_t instances = me.placement >= 0 ? m.placements[size_t(me.placement)].size() : 1;\n'
INST_NEW = INST_OLD + '''      g_census_sub_instances += instances;  // S101 lane 21 (census build only): what the draw list really gets
      if (me.placement >= 0 && instances > 1) {
        const auto census_f = g_census_flagged.find(&m);
        if (census_f != g_census_flagged.end() && census_f->second.count(me.placement)) g_census_sub.insert({&m, me.placement});
      }
'''
RT_OLD = 'bool FrontEnd::RenderToPixels(const SceneFrame& frame, std::vector<uint8_t>& rgba, std::string& error) {\n'
RT_NEW = RT_OLD + '  g_census_sub.clear(), g_census_sub_instances = 0;  // S101 lane 21 (census build only)\n'
CT_OLD = 'std::string FrontEnd::CoverageText() const {\n'
CT_NEW = CT_OLD + '''  {  // S101 lane 37 (census build only): which slots the draw list drew more than once
    std::set<int> census_sl;
    for (const auto& k : g_census_sub) { const auto it = g_census_slot.find(k); if (it != g_census_slot.end()) census_sl.insert(it->second); }
    std::string census_txt;
    for (int v : census_sl) census_txt += (census_txt.empty() ? "" : ",") + std::to_string(v);
    std::fprintf(stderr, "[census] submitted slots: %s\\n", census_txt.c_str());
  }
'''
CT_NEW = CT_NEW + '''  std::fprintf(stderr, "[census] submitted: %zu flagged placements with more than one instance, %zu instances\\n", g_census_sub.size(), g_census_sub_instances);  // S101 lane 21 (census build only)
'''
INC_OLD = '#include <algorithm>\n'
INC_NEW = '#include <algorithm>\n#include <chrono>\n#include <map>\n#include <set>\n#include <string>\n#include <thread>\n#include <utility>\n'


def must_replace(text, old, new, what):
    if text.count(old) != 1:
        print(f'FAIL  anchor for "{what}" found {text.count(old)} times (expected 1); the source changed, update make_census_build.py')
        sys.exit(1)
    return text.replace(old, new)


def patch_front_end(src, scene_dir):
    def fix(m):  # the copy lives elsewhere, so every local include becomes absolute
        p = os.path.normpath(os.path.join(scene_dir, m.group(1)))
        return '#include "%s"' % p.replace('\\', '/') if os.path.exists(p) else m.group(0)
    src = re.sub(r'#include "([^"]+)"', fix, src)
    src = must_replace(src, TEX_OLD, TEX_NEW, 'texture loop')
    src = must_replace(src, PL_OLD, PL_NEW, 'placements loop')
    src = must_replace(src, GL_OLD, GL_NEW, 'AddModel head')
    src = must_replace(src, HANG_OLD, HANG_NEW, 'AddModel body start')
    src = must_replace(src, INST_OLD, INST_NEW, 'instances line')
    src = must_replace(src, RT_OLD, RT_NEW, 'RenderToPixels head')
    src = must_replace(src, CT_OLD, CT_NEW, 'CoverageText head')
    return must_replace(src, INC_OLD, INC_NEW, 'includes')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=DEFAULT_ROOT)
    root = os.path.abspath(ap.parse_args().root)
    scene_dir = os.path.join(root, 'src', 'renderer_scene')
    build = os.path.join(root, 'out', 'filecensus', 'build')
    os.makedirs(build, exist_ok=True)
    fe = os.path.join(scene_dir, 'front_end.cpp')
    vb = os.path.join(root, 'tools', 'assets', 'build_viewer.ps1')
    for need in (fe, vb):
        if not os.path.exists(need):
            print(f'FAIL  cannot read its input: {need}')
            return 1
    patched = os.path.join(build, 'front_end_census.cpp')
    with open(patched, 'w', encoding='utf-8', newline='\n') as f:
        f.write(patch_front_end(open(fe, encoding='utf-8').read(), scene_dir))
    ps = open(vb, encoding='utf-8').read()
    ps = must_replace(ps, "$repo = Resolve-Path (Join-Path $PSScriptRoot '..\\..')", "$repo = Resolve-Path '%s'" % root, 'repo line')
    ps = must_replace(ps, "'wwe2k14_assets_rdr.exe'", "'wwe2k14_assets_rdr_census.exe'", 'exe name')
    ps = must_replace(ps, "'viewer_obj_rdr'", "'viewer_obj_rdr_census'", 'object folder')
    ps = must_replace(ps, "$srcs += @(Join-Path $repo 'src\\renderer_scene\\front_end.cpp')", "$srcs += @('%s')" % patched, 'front_end source')
    # imgui objects and SDL3 are shared with the normal viewer build: seed them from the main checkout when this root has none (a read-only copy)
    seed = ("$mainOut = Join-Path (Resolve-Path (Join-Path $repo '..\\..\\..')).Path 'out\\tools'\n"
            "foreach ($d in @('imgui_obj', 'sdl3_mt')) { $dst = Join-Path $outDir $d; $srcd = Join-Path $mainOut $d\n"
            "  if (-not (Test-Path $dst) -and (Test-Path $srcd) -and ($srcd -ne $dst)) { Copy-Item -Recurse $srcd $dst } }\n")
    ps = must_replace(ps, "$exe = Join-Path $outDir $(if", seed + "$exe = Join-Path $outDir $(if", 'seed anchor')
    out = os.path.join(build, 'build_census_viewer.ps1')
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        f.write(ps)
    print('ok  %s\nok  %s\nnext: pwsh -NoProfile -File "%s"  ->  %s' % (patched, out, out, os.path.join(root, 'out', 'tools', 'wwe2k14_assets_rdr_census.exe')))
    return 0


if __name__ == '__main__':
    sys.exit(main())
