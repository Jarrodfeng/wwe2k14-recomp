#!/usr/bin/env python3
"""S103 lanes CW-2 / CW-2b check (offline, 0 game runs): the guest model reader, planner, texture resolver, the GuestModel record rows and the consumer.
  py -I tools\\scene\\guest_model_unit.py [--root DIR]
Builds tools\\scene\\guest_model_unit.cpp (forged guest memory only) against src\\renderer_scene\\record_io.cpp and runs it: forged models built from the game's PACKER rules (2/3/4-pair weights, the uv order of the
jboy loader, a non-identity palette, every F variant, unknown F bits refused), the 12-part stock-body oracle, the planner's filter + cost units (a named wrestler is NOT read; 6 named + 1 unnamed reads only the unnamed
one, once), texture key resolution through a forged [m+260] table and registry (bound / override / missing, the per-call budget, the override refresh), the record round trip (sections 62 / 63 / 64) and the consumer.
Then each planted twin must FAIL (exit 1): reader 1-14, decoder 21-28 (28 = the case-sensitive effect / parameter compare), CTF texture reasons 31 (key 0 uncounted), retry 32 (no re-ask), 33 (tex_checked re-set every call); CTX2 (CAWD): 29 (the actors_drawn increment left out: the window census disagrees with the draw path), 34 (allow_8888 off: the 1024 x 1024 linear BGRA fixture is refused), 35 (the 8888 in-place refresh off: a rewritten page is never seen).
Lints (each with a planted twin: `lint_selftest` feeds the rule a synthetic violating file and requires it to fire):
  L1 the raw hook on sub_82C04DF0 (draw_origin.h) carries exactly ONE call into the reader and the mid-asm hook none (CW-2c); U-OURS the real hook source runs with Ours=true and still calls it;   L2 the reader file includes only the standard library and same-folder headers;
  L3 kSc section ids are unique across all headers;   L4 no stride-only vertex decode (LayoutOfStride, switch on stride / 4);   L5 no function-local static containers in rf_*.cpp appliers (an allowlist of the
  existing two, each with its reason, so a NEW one fails);   L6 a non-Windows guard on bulk ReadGuest in OFF code (the guest_model files that read bulk guest memory name _WIN32);
  L7 every SceneTables member of a *Tables type has a static_assert on its size (existing: ParticleTables, allowlisted);   L8 a gate predicate (function-pointer g_*) whose setter has zero product callers;
  L9 the section-64 and the layout-2 constants are used once; no getenv in the lane's files.
Exit 0 = pass, 1 = fail."""
import argparse
import os
import re
import subprocess
import sys

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
PLANTS = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 51, 52, 53, 54, 55, 56, 57, 58, 59, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 41, 42, 43, 44]   # 42 / 43 = S105 A5b (no guest part blends; every src_kind 2 mesh answers the piece-row lookup); 41 = draw plant 11 (CAWH: the file -> viewer space mirror skipped)

# ---- lints, over a {relative path: text} map so a twin can add a synthetic file --------------------------------------------------------------------------------------------------------------------------
STRIDE_ONLY = [re.compile(r'\bLayoutOfStride\b'), re.compile(r'switch\s*\(\s*\w*stride\w*\s*/\s*4\s*\)')]
SC_DEF = re.compile(r'\b(kSc[A-Z]\w*)\s*=\s*(\d+)\s*[,;]')
LOCAL_STATIC = re.compile(r'^\s{2,}static\s+(?!const\b)(?:std::(?:unordered_map|map|set|unordered_set|vector|deque|string)\b|\w*(?:Store|Cache)\b)', re.M)
LOCAL_STATIC_ALLOW = {  # existing function-local statics of rf_*.cpp, not touched by this lane; a NEW one must fail the lint
    ('src/renderer_scene/rf_c_core.cpp', 'static std::map<int, State> s;'): 'pre-existing per-class state of the C core (S103 C; one cache owns it)',
    ('src/renderer_scene/rf_screens.cpp', 'static std::vector<uint8_t> black;'): 'pre-existing constant black picture (never written after init)',
    ('src/renderer_scene/rf_wrestlers.cpp', 'static std::map<std::pair<uint32_t, std::string>, std::pair<uint64_t, uint64_t>> age;   // (skeleton, object id) -> (last record frame counted, records); review 4: a body absent for > 8 records starts again at 0'): 'pre-existing (S103 HGF 86aed0dc7 / 7c5130025): the guest-piece age, behind the static mutex of the same function',
    ('src/renderer_scene/rf_wrestlers.cpp', 'static std::set<std::string> logged_ids;'): 'pre-existing (S103 HGF): log-once set, behind the same mutex',
    ('src/renderer_scene/rf_wrestlers.cpp', 'static std::set<std::string> seen;'): 'pre-existing (S103 AUSF bb5b179b4): census set, capped at 64, reached only on the record thread through rf::Classes',
    ('src/renderer_scene/rf_props.cpp', 'static std::set<std::pair<int32_t, uint64_t>> said;'): 'pre-existing (S105 PLATE 372ea055f): the belt-plate log-once set in ApplyWeaponRows (a log line per prop id and plate hash, bounded by the plates a run binds), reached only on the record thread through rf::Classes -> BuildFrame; no draw state',
}


def lint_stride_only(files):
    bad = []
    for p, t in files.items():
        if not p.startswith('src/') and not p.startswith('synthetic/'):
            continue
        code = re.sub(r'//[^\n]*', '', t)   # comments may name the old decoder
        for rx in STRIDE_ONLY:
            if rx.search(code):
                bad.append('%s: stride-only vertex decode (%s)' % (p, rx.pattern))
    return bad


def lint_section_ids(files):
    seen = {}
    bad = []
    for p, t in files.items():
        if not (p.startswith('src/') or p.startswith('synthetic/')):
            continue
        for name, val in SC_DEF.findall(t):
            if val in seen and seen[val][0] != name:
                bad.append('section id %s used by %s (%s) and %s (%s)' % (val, seen[val][0], seen[val][1], name, p))
            seen.setdefault(val, (name, p))
    return bad


def lint_local_statics(files):
    bad = []
    for p, t in files.items():
        if not re.match(r'(src/renderer_scene|synthetic)/rf_\w+\.cpp$', p):
            continue
        for m in LOCAL_STATIC.finditer(t):
            line = t[m.start():t.index('\n', m.start())].strip() if '\n' in t[m.start():] else t[m.start():].strip()
            if (p, line) in LOCAL_STATIC_ALLOW:
                continue
            bad.append('%s: function-local static container (%s): per-ModelCache state belongs in ModelCache::GuestStateSlot-style slots' % (p, line))
    return bad


def lint_win_guard(files):
    # S103 Android (e4672d6d5, safe_guest_pages.h): off Windows a bulk guest read is page-checked INSIDE wrestler_hook.h ReadGuest (GuestRangeReadable), so the guest-model files need no _WIN32 guard of their own
    # (the old rule: a _WIN32 guard on every file that reads, and on Register()). The rule that keeps the same promise: ReadGuest itself must keep the page check.
    bad = []
    w = files.get('src/scene_live/wrestler_hook.h')
    if w is not None:
        m = re.search(r'inline bool ReadGuest\s*\([^)]*\)\s*\{(.*?)\n\}\n', w, re.S)
        body = m.group(1) if m else None
        # S108 RDS: ReadGuest may serve a pass's line copy whose every read is ReadGuestDirect; then ReadGuestDirect must keep the check, and ReadGuest must not copy guest memory itself
        if body is not None and 'GuestRangeReadable' not in body and 'ReadGuestDirect' in body and 'SafeCopy' not in body:
            d = re.search(r'inline bool ReadGuestDirect\s*\([^)]*\)\s*\{(.*?)\n\}\n', w, re.S)
            body = d.group(1) if d else ''
        if body is None:
            bad.append('src/scene_live/wrestler_hook.h: ReadGuest not found')
        elif 'GuestRangeReadable' not in body:
            bad.append('src/scene_live/wrestler_hook.h: ReadGuest reads without the guest page check (off Windows SafeCopy is a plain memcpy)')
    return bad


def lint_tables_assert(files):
    bad = []
    rio = files.get('src/renderer_scene/record_io.h', '')
    allow = {'ParticleTables'}
    alltext = '\n'.join(t for p, t in files.items() if p.startswith('src/') or p.startswith('synthetic/'))
    for m in re.finditer(r'^\s+(\w+Tables)\s+\w+;', rio, re.M):
        ty = m.group(1)
        if ty in allow:
            continue
        if not re.search(r'static_assert\s*\(\s*sizeof\s*\(\s*(?:rdr_scene::)?%s\s*\)' % ty, alltext):
            bad.append('SceneTables member of type %s has no static_assert(sizeof(%s) == N)' % (ty, ty))
    return bad


def lint_dead_predicates(files):
    bad = []
    decl = re.compile(r'inline\s+[\w:<>\s]+\(\*(g_\w+)\)\s*\([^)]*\)\s*=\s*nullptr;')
    for p, t in files.items():
        if not (re.match(r'src/scene_live/guest_model_\w+\.h$', p) or p.startswith('synthetic/')):
            continue
        for name in decl.findall(t):
            setters = [q for q, u in files.items() if (q.startswith('src/') or q.startswith('synthetic/')) and q != p and re.search(r'\b%s\s*=' % name, u)]
            if not setters:
                bad.append('%s: gate predicate %s has no setter in product code' % (p, name))
    return bad


def lint_fx_lists(files):
    """L10 (CTF review 8): PipelineEffect's kFx list (guest_model_draw.h) = the fx_yCh_* includes of fx_registry.h = char_effects.cpp kEffects."""
    d = files.get('src/renderer_scene/guest_model_draw.h', '')
    reg = files.get('src/renderer/shaders/spv/fx_registry.h', '')
    ce = files.get('src/renderer_scene/char_effects.cpp', '')
    m1 = re.search(r'kFx\[\]\s*=\s*\{(.*?)\};', d, re.S)
    m2 = re.search(r'kEffects\[\]\s*=\s*\{(.*?)\};', ce, re.S)
    if not m1 or not m2 or not reg:
        return ['kFx / kEffects / fx_registry.h not found']
    kfx = set(re.findall(r'"(yCh_\w+)"', m1.group(1)))
    inc = set('yCh_' + x for x in re.findall(r'#include "fx_yCh_(\w+)\.h"', reg))
    ke = set(re.findall(r'"(yCh_\w+)"', m2.group(1)))
    bad = []
    if kfx != inc:
        bad.append('kFx differs from the fx_yCh_* includes: only in kFx %s, only in registry %s' % (sorted(kfx - inc), sorted(inc - kfx)))
    if kfx != ke:
        bad.append('kFx differs from char_effects.cpp kEffects: only in kFx %s, only in kEffects %s' % (sorted(kfx - ke), sorted(ke - kfx)))
    return bad


def lint_why_index(files):
    """L11 (CTF review 2): every st_.why[...] index of the texture resolver is last_why_ or kWhyNoKey (a value that cannot be Ok), never one computed from a Why that can be Ok (a size mismatch counted in slot 0 = 'ok')."""
    t = files.get('src/scene_live/guest_model_tex.h', '')
    bad = []
    for m in re.finditer(r'st_\.why\[([^\]]*)\]', t):
        idx = m.group(1).strip()
        if idx not in ('last_why_ & 15', 'kWhyNoKey'):
            bad.append('st_.why[%s]: index not last_why_ / kWhyNoKey' % idx)
    return bad


LINTS = [('L3 section ids unique', lint_section_ids), ('L4 no stride-only vertex decode', lint_stride_only), ('L5 no function-local static containers in rf_*.cpp', lint_local_statics),
         ('L6 bulk ReadGuest is page-checked', lint_win_guard), ('L7 static_assert on SceneTables members', lint_tables_assert), ('L8 gate predicates have product setters', lint_dead_predicates),
         ('L10 PipelineEffect kFx = fx_registry yCh_* = char_effects kEffects', lint_fx_lists), ('L11 st_.why[] indexed only by a value that cannot be Ok', lint_why_index)]

TWINS = {
    'L3 section ids unique': {'synthetic/dup_ids.h': 'inline constexpr uint32_t kScGuestModelDup = 62;\n'},
    'L4 no stride-only vertex decode': {'synthetic/old_decode.h': 'switch (stride_bytes / 4) { case 8: return Layout::Plain8; }\n'},
    'L5 no function-local static containers in rf_*.cpp': {'synthetic/rf_new.cpp': 'int F() {\n  static std::unordered_map<std::string, uint64_t> hashes;\n  return 0;\n}\n'},
    'L6 bulk ReadGuest is page-checked': {'src/scene_live/wrestler_hook.h': 'inline bool ReadGuest(uint32_t guest, void* dst, size_t n) {\n  return SafeCopy(dst, host, n);\n}\n'},
    'L7 static_assert on SceneTables members': {'src/renderer_scene/record_io.h': 'struct SceneTables {\n  FooTables foo;\n};\n'},
    'L8 gate predicates have product setters': {'synthetic/guest_model_gate.h': 'inline bool (*g_orphan)(uint32_t model) = nullptr;\n'},
    'L10 PipelineEffect kFx = fx_registry yCh_* = char_effects kEffects': {'src/renderer_scene/guest_model_draw.h': 'static const char* const kFx[] = {"yCh_skin"};\n'},
    'L11 st_.why[] indexed only by a value that cannot be Ok': {'src/scene_live/guest_model_tex.h': '++st_.why[uint32_t(rw) & 15];\n'},
}


def read_tree(root):
    files = {}
    for dp, dn, fn in os.walk(os.path.join(root, 'src')):
        for f in fn:
            if f.endswith(('.h', '.cpp')):
                full = os.path.join(dp, f)
                rel = os.path.relpath(full, root).replace('\\', '/')
                with open(full, encoding='utf-8', errors='replace') as fh:
                    files[rel] = fh.read()
    return files


HARNESS = r"""
#include <cstdint>
#include <cstdio>
#include <atomic>
namespace scene_skip { enum class Hook { Model }; }
namespace wwe2k14 { inline std::atomic<uint64_t> g_guest_frame{777}; namespace scene_skip { inline bool Ours(::scene_skip::Hook) { return true; } } }
namespace rdr_scene::live::guestmodel { inline int g_calls = 0; inline uint32_t g_model = 0; inline uint64_t g_frame = 0;
  inline void OnModelEntry(uint32_t m, uint64_t f) { ++g_calls, g_model = m, g_frame = f; } }
namespace wwe2k14::weaponrows { inline uint8_t ShadowKindHook(bool, uint32_t, uint64_t) { return 0; } inline bool RawEntry(bool ours, uint32_t, uint32_t, uint64_t, float, uint32_t, bool (*)(void*), void*, uint8_t) { return !ours; } }   // the weapons' raw entry (PWI2; S103 CGF added MdlAlpha, the caller lr, the GBR pass test): returns false when the frame is OURS
namespace wwe2k14::wrestlers { inline bool ReadWord(uint32_t, uint32_t&) { return false; } }   // S103 DRL: the hook's draw-ask line passes it on
namespace rdr_scene::live { inline bool WrestlersRegistered() { return false; } inline bool Enabled() { return false; }
  struct WrestlerStub { template <class F> void NoteDrawAsked(uint32_t, uint64_t, F) {} }; inline WrestlerStub& Wrestlers() { static WrestlerStub w; return w; } }   // S103 DRL: not registered in the unit, so the line is a no-op
namespace wwe2k14::drawcam { inline int g_calls = 0; inline uint32_t g_r13 = 0; inline void NoteDraw(uint32_t r13, uint64_t) { ++g_calls, g_r13 = r13; } }   // WC R5: the draw-thread camera call
struct F64 { double f64; };
struct Reg { union { uint64_t u64; uint32_t u32; }; };
struct Ctx { Reg r3, r5, r13; F64 f1; uint64_t lr; };
struct GbrPassCtx { Ctx* ctx; const unsigned char* base; };   // S103 GBR: draw_origin.h's pass-index context (defined above the hook there)
inline bool GbrPass0(void*) { return true; }
#define REX_HOOK_RAW(name) static void name(Ctx& ctx, const unsigned char* base)
// the raw hook's real source, from its opening brace through the end of the Ours block
@HOOK@
}
int main() {
  Ctx c{}; c.r3.u64 = 0; c.r3.u32 = 0x1234; c.r13.u64 = 0; c.r13.u32 = 0x7000;
  sub_82C04DF0(c, nullptr);
  std::printf("calls %d model %08X frame %llu r3 %llu\n", rdr_scene::live::guestmodel::g_calls, rdr_scene::live::guestmodel::g_model, (unsigned long long)rdr_scene::live::guestmodel::g_frame, (unsigned long long)c.r3.u64);
  return (rdr_scene::live::guestmodel::g_calls == 1 && rdr_scene::live::guestmodel::g_model == 0x1234 && rdr_scene::live::guestmodel::g_frame == 777 && c.r3.u64 == 1 && wwe2k14::drawcam::g_calls == 1 && wwe2k14::drawcam::g_r13 == 0x7000) ? 0 : 1;
}
"""


def ours_hook_unit(out, dro):
    """Compile the REAL raw-hook source (draw_origin.h, up to the end of its Ours block) with Ours() forced true and run it: the reader must be called once and the hook must return 1 (S103 CW-2c)."""
    m = re.search(r'REX_HOOK_RAW\(sub_82C04DF0\)\s*\{', dro)
    if not m:
        return False, 'hook not found'
    rest = dro[m.start():]
    k = rest.find('\n  }\n')   # the Ours block's closing brace
    if k < 0:
        return False, 'Ours block end not found'
    hook = rest[:k + len('\n  }\n')]
    src = os.path.join(out, 'ours_hook_unit.cpp')
    exe = os.path.join(out, 'ours_hook_unit.exe')
    with open(src, 'w') as f:
        f.write(HARNESS.replace('@HOOK@', hook))
    if os.path.exists(exe):
        os.remove(exe)
    b = subprocess.run([CLANG, '-std=c++20', '-o', exe, src], capture_output=True, text=True)
    if b.returncode:
        return False, 'compile: ' + (b.stderr or b.stdout)[:300]
    r = subprocess.run([exe], capture_output=True, text=True, timeout=60)
    return r.returncode == 0, r.stdout.strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    bad = []

    def need(ok, what):
        print(('ok   ' if ok else 'FAIL ') + what)
        if not ok:
            bad.append(what)

    out = os.path.join(root, 'out', 'cw2')
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(out, 'guest_model_unit.exe')
    if os.path.exists(exe):
        os.remove(exe)
    b = subprocess.run([CLANG, '-std=c++20', '-O1', '-Wall', '-Wextra', '-Werror', '-DNOMINMAX', '-o', exe, os.path.join(root, 'tools', 'scene', 'guest_model_unit.cpp'),
                        os.path.join(root, 'src', 'renderer_scene', 'record_io.cpp')], capture_output=True, text=True)
    if b.returncode or not os.path.isfile(exe):
        print('FAIL: guest_model_unit did not compile\n' + b.stdout + b.stderr)
        return 1
    r = subprocess.run([exe], capture_output=True, text=True, timeout=300)
    print(r.stdout.rstrip())
    need(r.returncode == 0 and 'all checks passed' in r.stdout, 'clean run: every check passes')
    for p in PLANTS:
        rp = subprocess.run([exe, '--plant', str(p)], capture_output=True, text=True, timeout=300)
        need(rp.returncode == 1 and 'FAIL ' in rp.stdout, 'planted twin %d is caught (a check fails, no crash)' % p)
    # lints
    files = read_tree(root)
    fp = files.get('src/fp_probe.h', '')
    m = re.search(r'void wwe2k14_model_entry\(.*?\n\}', fp, re.S)
    need(bool(m) and m.group(0).count('guestmodel::') == 0, 'L1a the mid-asm model entry (never runs under OURS) does not call the guest reader')
    dro = files.get('src/draw_origin.h', '')
    mr = re.search(r'REX_HOOK_RAW\(sub_82C04DF0\)\s*\{(.*?)\n\}\n', dro, re.S)
    need(bool(mr) and mr.group(1).count('guestmodel::OnModelEntry(') == 1, 'L1b the raw hook on sub_82C04DF0 calls the guest reader exactly once')
    ok, why = ours_hook_unit(out, dro)
    need(ok, 'U-OURS the real raw-hook source, run with Ours=true, calls the reader once with (model, frame) and returns 1: ' + why)
    twin = dro.replace(re.search(r'[^\n]*guestmodel::OnModelEntry\([^\n]*\n', dro).group(0), '', 1)
    ok2, why2 = ours_hook_unit(out, twin)
    need(not ok2, 'U-OURS twin (call removed from the raw hook) is caught')
    # S105 A5c L12: the unlock flush hook tells the dirty table BEFORE its `if (!mode)` return, in both builds (no #if around the call): a flush the hook returned on would never mark a logo / DXT picture dirty
    d3t = files.get('src/d3d_textures.h', '')
    call = 'rdr_scene::live::guest_tex_dirty::NoteFlush(ctx.r3.u32);'

    def flush_hook_ok(src):
        mh = re.search(r'REX_HOOK_RAW\(sub_82CA6BE0\)\s*\{(.*?)\n\}\n', src, re.S)
        if not mh:
            return False
        body = mh.group(1)
        i = body.find('guest_tex_dirty::NoteFlush(ctx.r3.u32)')
        j = body.find('if (!mode)')
        if i < 0 or j < 0 or i > j:
            return False
        before = body[:i]
        return before.count('#if') == before.count('#endif')
    need(flush_hook_ok(d3t), 'L12 the unlock flush hook sub_82CA6BE0 calls guest_tex_dirty::NoteFlush(r3) before its mode return, outside any #if')
    need(not flush_hook_ok(d3t.replace(call, '', 1)), 'L12 twin (the NoteFlush call removed) is caught')
    late = d3t.replace(call, '', 1).replace('__imp__sub_82CA6BE0(ctx, base);\n    return;\n  }\n', '__imp__sub_82CA6BE0(ctx, base);\n    return;\n  }\n  ' + call + '\n', 1)
    need(not flush_hook_ok(late), 'L12 twin (the call moved behind the mode return) is caught')
    twin_dc = dro.replace(re.search(r'[^\n]*drawcam::NoteDraw\([^\n]*\n', dro).group(0), '', 1)
    ok3, why3 = ours_hook_unit(out, twin_dc)
    need(not ok3, 'U-OURS twin (WC R5: the draw-thread camera call removed from the raw hook) is caught')
    need(bool(mr) and mr.group(1).count('drawcam::NoteDraw(') == 1 and mr.group(1).index('drawcam::NoteDraw(') < mr.group(1).index('RawEntry('), 'L1c the raw hook calls the draw-thread camera once, before the Ours return')
    rl = subprocess.run([sys.executable, '-I', os.path.join(root, 'tools', 'weapons', 'ours_feed_lint.py'), '--selftest'], capture_output=True, text=True)
    need(rl.returncode == 0, 'ours_feed_lint passes with its twins firing' + ('' if rl.returncode == 0 else ': ' + rl.stdout))
    rd = files.get('src/scene_live/guest_model_reader.h', '')
    inc = re.findall(r'#include\s+[<"]([^>"]+)[>"]', rd)
    need(all('/' not in i and '\\' not in i for i in inc), 'L2 the reader includes only the standard library and same-folder headers: %s' % inc)
    for name, fn in LINTS:
        v = fn(files)
        need(not v, '%s%s' % (name, ('' if not v else ': ' + '; '.join(v))))
        tw = dict(files)
        tw.update(TWINS[name])
        vt = fn(tw)
        need(bool(vt), '%s: the planted twin fires' % name)
    n62 = sum(len(re.findall(r'kScGuestModel\w+\s*=\s*6[2-4]\s*;', t)) for p, t in files.items() if p == 'src/renderer_scene/guest_model_rows.h')
    need(n62 == 3, 'L9 section ids 62 / 63 / 64 are defined once each (guest_model_rows.h)')
    for rel in ('src/scene_live/guest_model_reader.h', 'src/scene_live/guest_model_contrib.h', 'src/scene_live/guest_model_tex.h', 'src/scene_live/guest_model_emit.h', 'src/scene_live/guest_model_store.h',
                'src/scene_live/buffer_unlock_log.h', 'src/renderer_scene/guest_model_draw.h', 'src/renderer_scene/guest_model_rows.h'):
        need('getenv' not in files.get(rel, ''), '%s reads no environment variable' % rel)
    need('sub_82CACF10' in files.get('src/vb_unlock_report.h', ''), 'the IB unlock sub_82CACF10 is hooked beside the VB one (buffer_unlock_log.h is told by both)')
    print('guest_model_unit: %s' % ('FAIL (%d)' % len(bad) if bad else 'all passed'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
