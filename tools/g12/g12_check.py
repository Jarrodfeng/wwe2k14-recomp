#!/usr/bin/env python3
"""S102 lane G12: the copies that remain (docs/renderer/COPIES_LEFT.md). Offline, 0 game runs, 0 Surface runs.

  py -I tools\\g12\\g12_check.py [--root DIR] [--only unit|wiring|bryan|match|counts]

Two cuts, each behind its own PerFrame switch (default off):
  copy_batch   wwe2k14_rdr_copy_batch  (src/renderer/copy_batch.h, renderer.cpp step loop): adjacent copies of one source into distinct layers / mips of one destination as ONE vkCmdCopyImage
               and ONE barrier flush (the 5 light-cube slots x 6 faces a frame in Bryan's entrance).
  alias_small  wwe2k14_rdr_alias_small (src/renderer_adapter/copy_alias.h SmallTakes): lane 190's strict verdict acted on whatever the class mask and the copy-alias mode say (1: bake / shadow /
               rim / post; 2: also scene colour and other).
unit     out\\tools\\copy_batch_test.exe: the batch rule's expectations + 4 planted weakenings that must each fail it.
wiring   each link of both switches (table PerFrame default 0, cvar, live.cpp per-frame read, the renderer / binder read) is in the source; each removed in a text copy must fail the lint.
bryan    fview headless on the Bryan entrance, every frame's presented-surface hash: A = the whole bundle (every *-ours switch the players run), B = + copy batch, S1 / S2 = + alias small, the
         default bundle (shadows ours + casters + plane), the --copy-alias 2 (LOW) combination, the planted twins (plant 1: a region dropped, caught by the picture; plant 2: the barrier left out,
         caught ONLY by validation; alias src_dirty). Validation errors AND the exit code are read, never the csv's existence.
match    the same pair on the standard match capture.
counts   rplay --counts on Bryan: copies and barriers a frame before / after, equal to what the census says (copies in batches minus batches; the alias census's elided a frame)."""
import argparse
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
CAPTURES = {
    'bryan': 'out/s101/runs/20261112_122700_s100_owner_bryan_entrance/case_20261112_122719_969_0_pc_run_exit.zip',
    'match': 'out/s101/runs/20261112_131008_s100_owner_match/case_20261112_131027_265_0_pc_run_exit.zip',
}
BRYAN_COUNTS_DIR = 'out/s101/lane92/case_20261112_122719_969_0_pc_run_exit/capture'
# the whole bundle the players run (the S101 promotions, wwe2k14_app.h) and the three that are on by default in the app's own list (shadows ours, casters, plane)
BUNDLE = ['--crowd-ours', '--crowd-cards', '--arena-ours', '--wrestlers-ours', '--caw-ours', '--ring-ours', '--ui-ours', '--ui-quads', '--ui-text-ours', '--flash-ours', '--props-ours', '--post-ours',
          '--movie-ours', '--screens-ours', '--shadow-casters-ours', '--rim-ours', '--circles-ours', '--plane-shadow-ours', '--small-passes-ours', '--rim-chain-ours', '--particles-ours', '--shadows-ours']
DEFAULT3 = ['--crowd-ours', '--crowd-cards', '--shadows-ours', '--shadow-casters-ours', '--plane-shadow-ours']
LOW = ['--shadow-map-size', '256', '--shadow-half', '--copy-alias', '2', '--copy-alias-classes', '11', '--alias-fullsize', '1']
fails = 0


def line(ok, text):
    global fails
    print(('PASS  ' if ok else 'FAIL  ') + text, flush=True)
    fails += 0 if ok else 1


def main_checkout(here):
    try:
        common = subprocess.run(['git', '-C', here, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return here


def read_hashes(path):
    out = {}
    with open(path) as f:
        for ln in f:
            ln = ln.strip()
            if ln:
                a, b = ln.split(',')
                out[a if a == 'format' else int(a)] = b
    return out


BATCH = re.compile(r'copy batch: (on|off), batches (\d+), copies in them (\d+), most regions in one (\d+)')
SMALL = re.compile(r'small-class override \(level (\d+)\): elided (\d+) \(([\d.]+) MPix; bake (\d+), shadow (\d+), rim (\d+), post (\d+), scene (\d+), other (\d+)\), read-backs of aliased destinations \(small\) (\d+)')


class Arm:
    def __init__(self, exe, zip_path, game, work, name, extra, allow_stop=False):
        hashes = os.path.join(work, name + '.csv')
        if os.path.exists(hashes):
            os.remove(hashes)
        cmd = [exe, '--case', zip_path, '--replay', '--frames', '1000', '--validation', '1', '--game', game, '--hash-frames', hashes] + extra
        for _ in range(12):
            p = subprocess.run(cmd, capture_output=True, text=True, timeout=1700, errors='replace')
            self.text = (p.stdout or '') + (p.stderr or '')
            if 'Not enough graphics memory for the frame ring' not in self.text:
                break
            time.sleep(20)
        self.rc = p.returncode
        with open(os.path.join(work, name + '.log'), 'w', encoding='utf-8') as f:
            f.write(self.text)
        self.stopped = not os.path.isfile(hashes)
        if self.stopped and not allow_stop:
            print(self.text[-2500:])
            raise RuntimeError('arm %s wrote no hashes' % name)
        self.hashes = {} if self.stopped else read_hashes(hashes)
        v = re.findall(r'validation errors (\d+)', self.text)
        self.valid = int(v[-1]) if v else -1
        m = re.search(r'frames planned (\d+) \(skipped (\d+)\), violations (\d+)', self.text)
        self.frames, self.viol = (int(m.group(1)), int(m.group(3))) if m else (0, 0)
        m = BATCH.search(self.text)
        self.batches, self.batched, self.batch_max = (int(m.group(2)), int(m.group(3)), int(m.group(4))) if m else (0, 0, 0)
        m = SMALL.search(self.text)
        self.sm = dict(zip(('level', 'elided', 'mpix', 'bake', 'shadow', 'rim', 'post', 'scene', 'other', 'hits'), [float(x) if '.' in x else int(x) for x in m.groups()])) if m else dict.fromkeys(
            ('level', 'elided', 'mpix', 'bake', 'shadow', 'rim', 'post', 'scene', 'other', 'hits'), 0)
        m = re.search(r'alias_elide_on_claimed\) (\d+)', self.text)
        self.claimed = int(m.group(1)) if m else -1
        self.frames_hashed = len([k for k in self.hashes if k != 'format'])
        self.sm_per_frame = self.sm['elided'] / max(1, self.frames)
        # the exit code AND the error count: fview writes its csv before it exits 4 on validation errors (G5 review)
        self.clean = self.rc == 0 and self.valid == 0

    def differing(self, other):
        return sum(1 for k in self.hashes if k != 'format' and self.hashes.get(k) != other.hashes.get(k))


def rplay_counts(rplay, capdir, game, work, name, extra):
    out = os.path.join(work, name + '.json')
    p = subprocess.run([rplay, '--capture', capdir, '--frames', '100', '--game', game, '--counts', out] + BUNDLE + extra, capture_output=True, text=True, timeout=1700, errors='replace')
    if p.returncode != 0:
        raise RuntimeError('rplay %s exit %d: %s' % (name, p.returncode, (p.stdout + p.stderr)[-800:]))
    copies, barriers, n = 0, 0, 0
    with open(out, encoding='utf-8') as f:
        for ln in f:
            ln = ln.strip().rstrip(',')
            if ln.startswith('{"frame"'):
                d = json.loads(ln)
                if d['frame'] >= 10:   # (the first frames carry the start-up uploads)
                    copies += d['total']['copies']
                    barriers += d['total']['barriers']
                    n += 1
    return copies / n, barriers / n, p.stdout + p.stderr


# ---- unit -----------------------------------------------------------------------------------------------------------------------------------------------------------
def unit(root):
    r = subprocess.run(['pwsh', '-NoProfile', '-File', os.path.join(root, 'tools', 'g12', 'build_copy_batch_test.ps1')], capture_output=True, text=True, timeout=600)
    exe = os.path.join(root, 'out', 'tools', 'copy_batch_test.exe')
    line(r.returncode == 0 and os.path.isfile(exe), 'unit test builds')
    if not os.path.isfile(exe):
        return
    r = subprocess.run([exe], capture_output=True, text=True, timeout=120)
    oks = sum(1 for l in r.stdout.splitlines() if l.startswith('ok'))
    line(r.returncode == 0 and 'all passed' in r.stdout and oks >= 30, 'unit test: the shipped batch rule passes its %d expectations' % oks)
    for plant in ('cap', 'source', 'dest_image', 'distinct', 'not_copy', 'records', 'compat'):
        r = subprocess.run([exe, '--plant', plant], capture_output=True, text=True, timeout=120)
        first = next((l for l in r.stdout.splitlines() if l.startswith('FAIL')), '')
        line(r.returncode == 1, 'unit plant %s is caught: %s' % (plant, first[:100] if first else 'NOT CAUGHT'))


# ---- wiring ---------------------------------------------------------------------------------------------------------------------------------------------------------
FILES = ('src/renderer_adapter/rdr_switches.h', 'src/renderer_adapter/reader.h', 'src/renderer_adapter/live.cpp', 'src/renderer_adapter/binder.h', 'src/renderer_adapter/copy_alias.h',
         'src/renderer/renderer.cpp', 'src/renderer/copy_batch.h', 'src/fview/main.cpp', 'src/renderer/player/rplay.cpp')


PLAN_TIME = ('AliasBegin', 'PlaneBlankBegin', 'PlanePlan', 'OwnGroupPlan', 'TurnsBegin', 'PreLightBegin', 'DirectCheck')   # S102 C5 review F5: DirectCheck runs inside the alias plan


def unguarded_others(body):
    """S102 G4c review F4: every plan-time `ctx.others.push_back(X.data())` must sit on a line that checks `X.size() >= ...` (or goes through PostClaimPlanned(ops)):
    a drop set of an earlier frame's size is indexed by this frame's ops."""
    out = []
    for ln in body.splitlines():
        if 'others.push_back(' in ln and '.size() >=' not in ln and 'PostClaimPlanned(' not in ln:
            out.append(ln)
    return out


# the lint's own planted twin (run by wiring): an unguarded push must be flagged, a guarded one not
assert unguarded_others('if (dead_on_) ctx.others.push_back(dead_plan_.drop.data());') and not unguarded_others('if (x_on_ && x_.drop.size() >= frame.ops.count) ctx.others.push_back(x_.drop.data());')


def func_body(text, name):
    """the text of `void|int|bool NAME(...) { ... }` (brace matched), or None"""
    m = re.search(r'(?:void|int|bool) %s\(' % name, text)   # S102 C5 review F5: DirectCheck returns int
    if not m:
        return None
    i = text.find('{', m.end())
    if i < 0:
        return None
    depth = 0
    for j in range(i, len(text)):
        if text[j] == '{':
            depth += 1
        elif text[j] == '}':
            depth -= 1
            if depth == 0:
                return text[i:j + 1]
    return None


def lint(fs):
    bad = []
    t = fs['src/renderer_adapter/rdr_switches.h']
    for name, cvar in (('AliasSmall', 'wwe2k14_rdr_alias_small'), ('CopyBatch', 'wwe2k14_rdr_copy_batch')):
        m = re.search(r'X\(%s, "%s", (\w+), (\d+)\)' % (name, cvar), t)
        if not m or m.group(1) != 'PerFrame' or m.group(2) != '0':
            bad.append('table line %s (PerFrame, default 0)' % name)
    r = fs['src/renderer_adapter/reader.h']
    if 'REXCVAR_DEFINE_INT32(wwe2k14_rdr_alias_small, 0,' not in r:
        bad.append('cvar alias_small default 0')
    if 'REXCVAR_DEFINE_BOOL(wwe2k14_rdr_copy_batch, false,' not in r:
        bad.append('cvar copy_batch default false')
    lv = fs['src/renderer_adapter/live.cpp']
    if 'copy_alias::g_small = copy_alias::ClampSmall(int(rdr_sw::IntSeen(rdr_sw::kAliasSmall' not in lv:
        bad.append('live.cpp reads alias_small every frame')
    # S102 main 07:2x: R21-B (fcda9b68) moved the per-frame read into the frame's render globals (read on the swap thread, stored on the render
    # thread from the frame params): either form is a per-frame read.
    if 'rdr::g_copy_batch.store(rdr_sw::BoolSeen(rdr_sw::kCopyBatch' not in lv and not (
            'render_globals.copy_batch = rdr_sw::BoolSeen(rdr_sw::kCopyBatch' in lv and 'rdr::g_copy_batch.store(p.copy_batch' in lv):
        bad.append('live.cpp reads copy_batch every frame')
    b = fs['src/renderer_adapter/binder.h']
    if 'facts.small_level = copy_alias::ClampSmall(copy_alias::g_small);' not in b:
        bad.append('binder carries the small level into the planner')
    if 'copy_alias::g_mode <= 0 && copy_alias::g_fullsize <= 0 && copy_alias::g_small <= 0' not in b:
        bad.append('binder plans when only the small switch is on')
    c = fs['src/renderer_adapter/copy_alias.h']
    if 'inline int g_small = 0;' not in c:
        bad.append('copy_alias.h small default 0')
    if 'return level >= 1 && (cls <= kPost || (level >= 2 && (cls == kScene || cls == kOther)));' not in c or 'return SmallClassOk(facts.small_level, facts.small_mask, cls);' not in c:
        bad.append('SmallTakes: level 1 = bake / shadow / rim / post, level 2 adds scene / other')
    if 'else if (SmallTakes(facts, cls) && g_fault != Fault::Empty)' not in c:
        bad.append('SmallTakes acts only on the strict verdict (why == elided) and only without the null fault')
    if 'if (g_sm_aliased_ids.count(image)) ++g_sm_readback_hits;' not in c:
        bad.append('read-back of a small-aliased destination is counted')
    rc = fs['src/renderer/renderer.cpp']
    if 'const bool copy_batch_on = rdr::g_copy_batch.load(std::memory_order_relaxed);' not in rc:
        bad.append('renderer reads copy_batch every frame')
    if 'if (copy_batch_on && readback_taking.empty()) {' not in rc:
        bad.append('no batching while a read-back is taken')
    if 'const uint32_t n = rdr::BatchSpan(views, cand);' not in rc:
        bad.append('the pure BatchSpan decides where a batch ends')
    if "views[cand].records = !((mode == RenderMode::PersistentOnly && !cs.persistent) || (mode == RenderMode::DisplayOnly && cs.persistent));" not in rc:
        bad.append('a step the mode would not record ends the batch')
    if 'FlushBarriers(cmd);\n  } else {' not in rc.replace('\r\n', '\n'):
        bad.append('the batch flushes its barriers before the copy')
    cb = fs['src/renderer/copy_batch.h']
    if 'inline std::atomic<bool> g_copy_batch{false};' not in cb:
        bad.append('copy_batch.h default off')
    if 'for (uint32_t i = 0; i < count; ++i)\n    if (batch[i].dest_sub == next.dest_sub) return false;' not in cb.replace('\r\n', '\n'):
        bad.append('Compatible: a destination subresource is written once per batch')
    for fn in PLAN_TIME:   # S102 G12 review F1: post_state_ is 1 from PlanPost until the walk's first claimed draw (then 2): a plan-time test of `== 2` never sees the claim
        body = func_body(b, fn)
        if body is None:
            bad.append('plan-time function %s not found in binder.h (the lint is stale)' % fn)
        elif re.search(r'post_state_\s*==\s*2', body):
            bad.append('%s tests post_state_ == 2 at plan time (use PostClaimsOp / PostClaimPlanned)' % fn)
        for ln in unguarded_others(body):
            bad.append('%s: ctx.others.push_back without a size guard: %s' % (fn, ln.strip()[:90]))
    if 'bool PostClaimPlanned(uint32_t ops) const {' not in b or '(post_state_ == 1 || post_state_ == 2)' not in b:
        bad.append('PostClaimPlanned sees state 1 and 2')
    if 'facts.small_mask = copy_alias::g_small_bake_off.load' not in b:
        bad.append('binder carries the bake exclusion into the planner (F2)')
    if 'copy_alias::g_small_bake_off.store(REXCVAR_GET(wwe2k14_rdr_crowd_bake_skip) >= 2' not in r:
        bad.append('reader.h clears the bake class of alias small while crowd_bake_skip >= 2, per frame (F2)')
    for path in ('src/fview/main.cpp', 'src/renderer/player/rplay.cpp'):
        for flag in ('--alias-small', '--copy-batch'):
            if flag not in fs[path]:
                bad.append('%s flag %s' % (os.path.basename(path), flag))
    return bad


def wiring(root):
    files = {}
    for k in FILES:
        with open(os.path.join(root, k), encoding='utf-8', errors='replace') as f:
            files[k] = f.read()
    bad = lint(files)
    line(not bad, 'wiring: both switches are in the table (PerFrame, default 0), defined, read per frame, carried to their readers ' + ('' if not bad else 'MISSING: ' + '; '.join(bad)))
    twins = [('src/renderer_adapter/rdr_switches.h', 'X(CopyBatch, "wwe2k14_rdr_copy_batch", PerFrame, 0)', 'X(CopyBatch, "wwe2k14_rdr_copy_batch", StartOnly, 0)'),
             ('src/renderer_adapter/rdr_switches.h', 'X(AliasSmall, "wwe2k14_rdr_alias_small", PerFrame, 0)', 'X(AliasSmall, "wwe2k14_rdr_alias_small", PerFrame, 1)'),
             ('src/renderer_adapter/reader.h', 'REXCVAR_DEFINE_BOOL(wwe2k14_rdr_copy_batch, false,', 'REXCVAR_DEFINE_BOOL(wwe2k14_rdr_copy_batch, true,'),
             ('src/renderer_adapter/live.cpp', 'rdr_sw::BoolSeen(rdr_sw::kCopyBatch', 'rdr_sw::BoolSeen(rdr_sw::kCardCut'),
             ('src/renderer_adapter/live.cpp', 'rdr_sw::IntSeen(rdr_sw::kAliasSmall', 'rdr_sw::IntSeen(rdr_sw::kAliasFullsize'),
             ('src/renderer/renderer.cpp', 'if (copy_batch_on && readback_taking.empty()) {', 'if (copy_batch_on) {'),
             ('src/renderer/renderer.cpp', 'const uint32_t n = rdr::BatchSpan(views, cand);', 'const uint32_t n = cand;'),
             ('src/renderer/copy_batch.h', 'if (batch[i].dest_sub == next.dest_sub) return false;', 'if (false) return false;'),
             ('src/renderer_adapter/binder.h', 'facts.small_level = copy_alias::ClampSmall(copy_alias::g_small);', 'facts.small_level = 0;'),
             ('src/renderer_adapter/binder.h', 'copy_alias::g_mode <= 0 && copy_alias::g_fullsize <= 0 && copy_alias::g_small <= 0', 'copy_alias::g_mode <= 0 && copy_alias::g_fullsize <= 0'),
             ('src/renderer_adapter/copy_alias.h', 'return level >= 1 && (cls <= kPost || (level >= 2 && (cls == kScene || cls == kOther)));', 'return level >= 1;'),
             ('src/renderer_adapter/copy_alias.h', 'if (g_sm_aliased_ids.count(image)) ++g_sm_readback_hits;', ''),
             ('src/renderer_adapter/binder.h', 'PostClaimsOp(frame.ops.count, op) || (bake_on_', '(post_state_ == 2 && post_claim_.drop[op]) || (bake_on_'),
             ('src/renderer_adapter/binder.h', 'PostClaimsOp(frame.ops.count, o) ||', '(post_state_ == 2 && post_claim_.drop[o]) ||'),
             ('src/renderer_adapter/binder.h', 'if (PostClaimPlanned(frame.ops.count)) ctx.others', 'if (post_state_ == 2 && post_claim_.drop.size() >= frame.ops.count) ctx.others'),
             ('src/renderer_adapter/binder.h', 'if (PostClaimsOp(frame.ops.count, p)) return true;', 'if (post_state_ == 2 && post_claim_.drop[p]) return true;'),
             ('src/renderer_adapter/binder.h', 'int DirectCheck(const LoadedFrame& frame, uint32_t p, const rdr::CopyOp& copy) {', 'int DirectCheck(const LoadedFrame& frame, uint32_t p, const rdr::CopyOp& copy) { if (post_state_ == 2) return 0;'),   # S102 C5 review F5
             ('src/renderer_adapter/binder.h', 'facts.small_mask = copy_alias::g_small_bake_off.load', 'facts.small_mask = copy_alias::kClassAll; (void)copy_alias::g_small_bake_off.load'),
             ('src/renderer_adapter/reader.h', 'g_small_bake_off.store(REXCVAR_GET(wwe2k14_rdr_crowd_bake_skip) >= 2', 'g_small_bake_off.store(REXCVAR_GET(wwe2k14_rdr_crowd_bake_skip) >= 9')]
    for path, old, new in twins:
        if old not in files[path]:
            line(False, 'wiring twin: the text %r is not in %s (the lint is stale: DID NOT APPLY)' % (old[:40], path))
            continue
        twin = dict(files)
        twin[path] = files[path].replace(old, new, 1)
        if twin[path] == files[path]:
            line(False, 'wiring twin %r DID NOT APPLY' % old[:40])
            continue
        line(bool(lint(twin)), 'wiring twin (%s: %r -> %r) is caught' % (os.path.basename(path), old[:36], new[:30]))


# ---- replay ---------------------------------------------------------------------------------------------------------------------------------------------------------
def replay(root, which):
    main_co = main_checkout(root)
    game = os.path.join(main_co, 'game_data', 'extracted')
    sys.path.insert(0, os.path.join(root, 'tools', 'fview'))
    import fview_exe as fe
    exe = fe.find_or_exit(root=root, kind='dev')
    work = os.path.join(root, 'out', 'g12_check')
    os.makedirs(work, exist_ok=True)
    z = os.path.join(main_co, CAPTURES[which])
    if not os.path.exists(z):
        line(False, 'input missing: ' + z)
        return
    n = which
    A = Arm(exe, z, game, work, n + '_A', BUNDLE)
    base_ok = A.clean and len(A.hashes) > 1
    line(base_ok, 'baseline %s (the whole bundle, switches off): %d frames hashed, validation %d, exit %d' % (n, A.frames_hashed, A.valid, A.rc))
    # ---- cut A: copy batch
    B = Arm(exe, z, game, work, n + '_B', BUNDLE + ['--copy-batch', '1'])
    d = B.differing(A)
    want = 4.0 if n == 'bryan' else 0.0
    per = B.batches / max(1, B.frames_hashed)
    line(d == 0 and len(B.hashes) == len(A.hashes) and B.clean and per >= want and (B.batches > 0 or n != 'bryan'),
         'copy batch %s: %d frames, %d differ, validation %d, exit %d, batches %d (%.2f a frame; %d copies in them, most regions %d) over %d frames' %
         (n, B.frames_hashed, d, B.valid, B.rc, B.batches, per, B.batched, B.batch_max, B.frames_hashed))
    # ---- cut B: alias small, levels 1 and 2 (whatever lane 190's class mask and mode say: none of them is set here)
    arms = {}
    for lv, minpf in ((1, 3.0 if n == 'bryan' else 0.0), (2, 3.0 if n == 'bryan' else 0.0)):
        S = Arm(exe, z, game, work, '%s_S%d' % (n, lv), BUNDLE + ['--alias-small', str(lv)])
        arms[lv] = S
        d = S.differing(A)
        line(d == 0 and len(S.hashes) == len(A.hashes) and S.clean and S.viol == 0 and S.sm['hits'] == 0 and S.sm_per_frame >= minpf and S.claimed == 0,
             'alias small level %d %s (copy alias OFF): %d differ, validation %d, exit %d, violations %d, elided %.2f a frame (%.3f MPix; bake %d shadow %d rim %d post %d scene %d other %d), read-backs of aliased dests %d, elided on claimed ops %d (must be 0)' %
             (lv, n, d, S.valid, S.rc, S.viol, S.sm_per_frame, S.sm['mpix'], S.sm['bake'], S.sm['shadow'], S.sm['rim'], S.sm['post'], S.sm['scene'], S.sm['other'], S.sm['hits'], S.claimed))
    # F4: level 2 is what takes scene colour / other; level 1 never does
    line(arms[2].sm['other'] > 0 and arms[1].sm['other'] == 0 and arms[1].sm['scene'] == 0,
         'level 2 takes copies level 1 does not (%s): other %d against %d, scene colour %d against %d' % (n, arms[2].sm['other'], arms[1].sm['other'], arms[2].sm['scene'], arms[1].sm['scene']))
    if n != 'bryan':
        return
    # the default bundle (shadows ours + casters + plane): frames taken
    D0 = Arm(exe, z, game, work, n + '_D0', DEFAULT3)
    D1 = Arm(exe, z, game, work, n + '_D1', DEFAULT3 + ['--copy-batch', '1', '--alias-small', '2'])
    line(D1.differing(D0) == 0 and D0.clean and D1.clean and D1.batches > 0 and D1.sm['elided'] > 0,
         'default bundle %s: %d differ, validation %d/%d, exit %d/%d, batches taken %d (%.2f a frame), alias small elided %d' % (n, D1.differing(D0), D0.valid, D1.valid, D0.rc, D1.rc, D1.batches, D1.batches / max(1, D1.frames_hashed), D1.sm['elided']))
    # --copy-alias 2 (the LOW combination: shadows ours, 256, half rate, classes 11, the full-size rule at level 1)
    C0 = Arm(exe, z, game, work, n + '_C0', BUNDLE + LOW)
    C1 = Arm(exe, z, game, work, n + '_C1', BUNDLE + LOW + ['--copy-batch', '1', '--alias-small', '2'])
    line(C1.differing(C0) == 0 and C0.clean and C1.clean and C1.viol == 0 and C1.batches > 0 and C1.sm['hits'] == 0,
         '--copy-alias 2 arm (LOW) %s: %d differ, validation %d/%d, exit %d/%d, violations %d, batches %d, alias small elided %d (the scene colour / other copies the class mask kept)' %
         (n, C1.differing(C0), C0.valid, C1.valid, C0.rc, C1.rc, C1.viol, C1.batches, C1.sm['elided']))
    # F1: the post claim is excluded from the alias plan; the twin reverts the plan-time test to `post_state_ == 2` (the claim is then never seen) and phantom elisions return
    F1 = Arm(exe, z, game, work, n + '_F1', BUNDLE + ['--alias-small', '1', '--copy-alias-fault', 'post_claim_late'], allow_stop=True)
    line(arms[1].claimed == 0 and F1.claimed >= F1.frames, 'twin post_claim_late (the old plan-time test) is caught: elided on claimed ops %d over %d frames (the shipped test: %d)' % (F1.claimed, F1.frames, arms[1].claimed))
    # planted twins
    P1 = Arm(exe, z, game, work, n + '_P1', BUNDLE + ['--copy-batch', '1', '--copy-batch-plant', '1'], allow_stop=True)
    line((P1.stopped and '[rdr][PROBLEM]' in P1.text) or (not P1.stopped and P1.differing(A) > 0), 'twin copy-batch plant 1 (the last region of every batch is not copied) is caught by a renderer [PROBLEM] or the picture: %d frames differ%s' % (P1.differing(A), ', the replay stopped on [PROBLEM]' if P1.stopped else ''))
    P2 = Arm(exe, z, game, work, n + '_P2', BUNDLE + ['--copy-batch', '1', '--copy-batch-plant', '2'], allow_stop=True)
    line(not P2.stopped and len(P2.hashes) == len(A.hashes) and P2.differing(A) == 0 and P2.valid > 0 and P2.rc != 0 and 'VUID' in P2.text,
         'twin copy-batch plant 2 (the barrier flush left out) runs to the end (%d hashes of %d), is invisible to the picture (%d differ) and is caught by validation only: errors %d, exit %d, VUID in the log %s' % (len(P2.hashes), len(A.hashes), P2.differing(A), P2.valid, P2.rc, 'VUID' in P2.text))
    P3 = Arm(exe, z, game, work, n + '_P3', BUNDLE + ['--copy-batch', '1', '--copy-batch-plant', '3'])
    line(P3.differing(A) == 0 and P3.clean and P3.batches > 0, 'plant 3 (the control: the batch as shipped) stays clean: %d differ, validation %d, exit %d' % (P3.differing(A), P3.valid, P3.rc))
    Q = Arm(exe, z, game, work, n + '_Q', BUNDLE + ['--alias-small', '2', '--copy-alias-fault', 'src_dirty'], allow_stop=True)
    stopped = Q.stopped and '[rdr][PROBLEM]' in Q.text
    line(stopped or (not Q.stopped and Q.differing(A) > 0), 'twin alias small + src_dirty (an aliased destination whose source is rewritten before the read) is caught: %s' % ('the replay stopped on a renderer [PROBLEM]' if stopped else '%d frames change' % Q.differing(A)))
    # a planted CPU read-back of destinations the small rule takes (the bake's pictures 414..416): refused from then on, counted
    S2 = Arm(exe, z, game, work, n + '_S2b', BUNDLE + ['--alias-small', '2'])
    R = Arm(exe, z, game, work, n + '_R', BUNDLE + ['--alias-small', '2', '--copy-alias-readback', '30:414,415,416,417,418,419,420,421,422,423'])
    line(R.differing(A) == 0 and R.clean and R.sm['hits'] >= 1 and R.sm_per_frame < S2.sm_per_frame,
         'a planted CPU read-back of ten bake destinations at frame 30: counted (%d), refused from then on (elided %.2f a frame, was %.2f), pictures equal (%d differ)' % (R.sm['hits'], R.sm_per_frame, S2.sm_per_frame, R.differing(A)))
    R2 = Arm(exe, z, game, work, n + '_R2', BUNDLE + ['--alias-small', '2', '--copy-alias-readback', '30:414,415,416,417,418,419,420,421,422,423', '--copy-alias-fault', 'ignore_readback'], allow_stop=True)
    line(R2.sm_per_frame > R.sm_per_frame + 0.3, 'twin ignore_readback (a read-back destination stays aliased) is caught: elided %.2f a frame against %.2f with the rule' % (R2.sm_per_frame, R.sm_per_frame))


def counts(root):
    main_co = main_checkout(root)
    game = os.path.join(main_co, 'game_data', 'extracted')
    rplay = os.path.join(root, 'out', 'renderer', 'dev', 'rplay.exe')
    capdir = os.path.join(main_co, BRYAN_COUNTS_DIR)
    work = os.path.join(root, 'out', 'g12_check')
    os.makedirs(work, exist_ok=True)
    if not (os.path.isfile(rplay) and os.path.isdir(capdir)):
        line(False, 'rplay or its capture is missing: %s %s' % (rplay, capdir))
        return
    ca, ba, _ = rplay_counts(rplay, capdir, game, work, 'cnt_A', [])
    cb, bb, tb = rplay_counts(rplay, capdir, game, work, 'cnt_B', ['--copy-batch', '1'])
    m = re.search(r'\[rdr\]\[copy_batch\] on: batches (\d+), copies in them (\d+)', tb)
    frames = 100.0
    saved = (int(m.group(2)) - int(m.group(1))) / frames if m else -1
    line(m is not None and abs((ca - cb) - saved) < 0.35 and saved >= 20 and (ba - bb) >= saved - 0.35,
         'copy batch counts (rplay, 100 frames, the bundle): copies %.2f -> %.2f a frame (-%.2f), barriers %.2f -> %.2f (-%.2f); the batches replace %.2f copies a frame' % (ca, cb, ca - cb, ba, bb, ba - bb, saved))
    cs, bs, ts = rplay_counts(rplay, capdir, game, work, 'cnt_S', ['--alias-small', '2'])
    ms = SMALL.search(ts)
    elided = (int(ms.group(2)) / frames) if ms else -1   # (S102 G12 review F1: with the post claim excluded from the plan the planner's count is the renderer's)
    line(ms is not None and abs((ca - cs) - elided) < 0.35 and elided >= 20.0 and (ba - bs) >= elided - 0.35,
         'alias small counts (rplay, 100 frames, level 2): copies %.2f -> %.2f a frame (-%.2f), barriers %.2f -> %.2f (-%.2f); the census elides %.2f a frame (bake + shadow + post-free other)' % (ca, cs, ca - cs, ba, bs, ba - bs, elided))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--only', default='')
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    if args.only in ('', 'unit'):
        unit(root)
    if args.only in ('', 'wiring'):
        wiring(root)
    if args.only in ('', 'bryan'):
        replay(root, 'bryan')
    if args.only in ('', 'match'):
        replay(root, 'match')
    if args.only in ('', 'counts'):
        counts(root)
    print('G12: %s' % ('all passed' if not fails else '%d FAILED' % fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
