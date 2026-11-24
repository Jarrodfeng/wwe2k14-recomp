#!/usr/bin/env python3
"""S102 lane T15 check (offline, 0 game runs, 0 Surface runs): the reader's per-draw slot / sampler / geometry-hash memos (src\\renderer_adapter\\slot_geo_memo.h).

  py -I tools\\t15\\t15_check.py [--root DIR] [--part test|wiring|all]

test:   builds tools\\t15\\t15_memo_test.cpp (-DT15_PLANTS; the planted twins exist only in that build) with the main checkout's xxHash, runs it and requires: exit 0,
        "all checks passed", no FAIL line, every arm's off/on equality line, the lever-does-something lines, the 9 planted twins caught, the census lines. Prints the
        microbench numbers.
wiring: reader.h / rdr_switches.h / slot_geo_memo.h still have the shape the test's models transcribe (each pattern below), and each requirement removed from a copy of the
        text must be reported by name (a plant that no longer applies is its own failure: DID NOT APPLY).
A check that cannot read its input fails loudly (missing source, missing clang, no result line)."""
import argparse
import os
import re
import subprocess
import sys

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'


def fail(msg):
    print('FAIL: ' + msg)
    return 1


def main_checkout(root):
    out = subprocess.run(['git', '-C', root, 'rev-parse', '--git-common-dir'], capture_output=True, text=True)
    if out.returncode:
        return None
    return os.path.dirname(os.path.abspath(os.path.join(root, out.stdout.strip())))


def run_test(root):
    if not os.path.isfile(CLANG):
        return fail('%s not found' % CLANG)
    src = os.path.join(root, 'tools', 't15', 't15_memo_test.cpp')
    if not os.path.isfile(src):
        return fail('missing ' + src)
    main = main_checkout(root)
    xxh = os.path.join(main or '', 'thirdparty', 'rexglue-sdk-src', 'thirdparty', 'xxHash')
    if not os.path.isfile(os.path.join(xxh, 'xxhash.h')):
        return fail('xxhash.h not found under ' + xxh)
    out_dir = os.path.join(root, 'out', 't15')
    os.makedirs(out_dir, exist_ok=True)
    exe = os.path.join(out_dir, 't15_memo_test.exe')
    if os.path.exists(exe):
        os.remove(exe)
    b = subprocess.run([CLANG, '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-DNOMINMAX', '-DT15_PLANTS', '-I', xxh, '-I', os.path.join(root, 'src', 'renderer_adapter'),
                        '-o', exe, src], capture_output=True, text=True)
    if b.returncode or not os.path.isfile(exe):
        return fail('t15_memo_test did not compile\n' + b.stdout + b.stderr)
    r = subprocess.run([exe], capture_output=True, text=True, timeout=900)
    text = r.stdout
    print(text.rstrip())
    passes = len(re.findall(r'^PASS ', text, re.M))
    fails = len(re.findall(r'^FAIL ', text, re.M))
    twins = len(re.findall(r'^PASS planted twin caught', text, re.M))
    equal = len(re.findall(r'^PASS .*equal off / on', text, re.M))
    does = len(re.findall(r'^PASS +.*\(the lever does something\)|^PASS +sampler memo: fewer sampler builds', text, re.M))
    if r.returncode != 0 or fails or 'all checks passed' not in text:
        return fail('t15_memo_test exit %d, %d FAIL lines' % (r.returncode, fails))
    if not re.search(r'^PASS CombineSmall\(', text, re.M):
        return fail('the CombineSmall identity check did not run')
    if twins != 9:
        return fail('expected 9 planted twins caught, got %d' % twins)
    if equal != 9:  # 4 arms x 2 patterns + the geometry arm
        return fail('expected 9 off/on equality arms, got %d (a check that reads nothing proves nothing)' % equal)
    if does < 4:
        return fail('expected the lever-does-something lines of every lever, got %d' % does)
    print('PASS: t15_memo_test: %d checks, %d equality arms, %d planted twins caught, exit 0' % (passes, equal, twins))
    return 0


# ---- wiring ---------------------------------------------------------------------------------------------------------------------------------
def scope(text, start, end):
    a = text.find(start)
    b = text.find(end, a + 1) if a >= 0 else -1
    return text[a:b] if a >= 0 and b > a else ''


def requirements(files):
    reader, sw, memo = files['reader'], files['switches'], files['memo']
    ats = scope(reader, 'inline void AddTextureSlot(', '// ---- draw state')
    ens = scope(reader, 'inline void EnsureContent(uint32_t id', 'inline void CensusSourceLog(uint64_t from, uint64_t to) {')
    geo = scope(reader, 'inline GeoResult BuildGeometry(', 'inline void ApplyGeometry(')
    run = scope(reader, 'inline GeoResult RunGeometryBuild(', 'inline GeoResult BuildGeometry(')
    present = scope(reader, 'inline void OnPresent(', 'ProfileEnd') or reader[reader.find('inline void OnPresent('):][:20000]
    R = []  # (name, text, regex)
    for cv in ('slot_content_skip', 'slot_ways', 'sampler_memo', 'geo_hash_once'):
        R.append(('cvar %s defined default false' % cv, reader, r'REXCVAR_DEFINE_BOOL\(wwe2k14_rdr_%s, false, "wwe2k14"' % cv))
        R.append(('switch table row %s PerFrame 0' % cv, sw, r'X\(\w+, "wwe2k14_rdr_%s", PerFrame, 0\)' % cv))
    for k, g in (('SlotContentSkip', 'g_content_skip_on'), ('SlotWays', 'g_slot_ways_on'), ('SamplerMemo', 'g_sampler_memo_on'), ('GeoHashOnce', 'g_geo_hash_once_on')):
        R.append(('present reads %s every frame (BoolSeen -> %s)' % (k, g), present, r'dm::%s\.store\(rdr_sw::BoolSeen\(rdr_sw::k%s, false, guest_frame\)' % (g, k)))
    R.append(('present logs the census', present, r'dm::LogCensus\(guest_frame,'))
    R.append(('slot lookup through the way table', ats, r'g_slot_cache\.Find\(slot, ways_on \? dm::kSlotWays : 1, way, \[&\]\(const SlotCache& c\) \{ return c\.kind && c\.key\.Equal\(now\); \}\)'))
    R.append(('slot victim only when nothing matched', ats, r'SlotCache& sc = matched \? \*matched : g_slot_cache\.Victim\(slot, ways_on \? dm::kSlotWays : 1\);'))
    R.append(('slot key built from the generation, epoch, fetch, object, words, header, sampler inputs', ats,
              r'now\.generation = g_frame\.generation, now\.epoch = g_reg\.epoch, now\.fetch = &t, now\.object = object;[\s\S]{0,300}now\.aniso_override = g_aniso_override;[\s\S]{0,200}std::memcpy\(now\.w, w, sizeof\(w\)\);\s*if \(object\) std::memcpy\(now\.header_raw'))
    R.append(('slot remember keeps the key (epoch after the add)', ats, r'sc\.key = now;\s*sc\.key\.epoch = g_reg\.epoch;'))
    R.append(('slot hit still checks the image flags', ats, r'img\.packed == sc\.packed && img\.copy_dest == sc\.copy_dest && img\.swap == sc\.swap && img\.target16 == sc\.target16'))
    R.append(('slot hit calls the checked EnsureContent', ats, r'EnsureContentChecked\(sc\.id, w\);'))
    R.append(('slot miss calls the checked EnsureContent', ats, r'EnsureContentChecked\(id, w\);'))
    R.append(('slot miss builds the sampler through the memo', ats, r'out\.sampler = BuildSamplerMemo\(w, t\);'))
    R.append(('no plain EnsureContent left in AddTextureSlot', ats, lambda t: re.search(r'\bEnsureContent\(', t) is None,
              lambda t: t.replace('EnsureContentChecked(sc.id, w);', 'EnsureContent(sc.id, w);', 1)))
    # F3: EnsureContent's exact prefix, which ContentDone transcribes
    R.append(('EnsureContent: exact prefix up to the copy_dest block', ens,
              r'inline void EnsureContent\(uint32_t id, const uint32_t w\[6\]\) \{\n  if \(!Recording\(\)\) return;\n  ScopedNs timer\(g_times\.content\);\n'
              r'  if \(REXCVAR_GET\(wwe2k14_rdr_debug_no_content\)\) return;  // diagnosis: no texture memory read at all\n  Image& img = ById\(id\);\n  if \(img\.copy_dest\) \{',
              lambda t: t.replace('Image& img = ById(id);\n', 'Image& img = ById(id);\n  img.bound_frame = g_record_frame;\n', 1)))
    # F2: BuildSampler's inputs are exactly what SamplerKey carries
    R.append(('BuildSampler reads only w[0,3,4,5], t.mag/min/mip/aniso, the named globals, no cvar', reader, lambda t: not sampler_lint(t),
              lambda t: t.replace('  d.min_lod = float(', '  if (g_x) d.max_lod = 2;\n  d.min_lod = float(', 1)))
    R.append(('BuildSampler reads no other fetch word (plant: a border bit from w[2])', reader, lambda t: not sampler_lint(t),
              lambda t: t.replace('  d.min_lod = float(', '  d.border |= w[2] & 1;\n  d.min_lod = float(', 1)))
    # F4: what the slot miss path reads between `cap::TextureSlot out;` and remember(1, id, out)
    R.append(('AddTextureSlot miss path reads only the allowlisted image fields, globals, cvars and calls', ats, lambda t: not slot_miss_lint(t),
              lambda t: t.replace('  out.image = id;\n', '  out.image = id;\n  out.x = img.some_field;\n', 1)))
    # F5
    R.append(('HintGuard clears the stream hint on every way out', reader,
              r'struct HintGuard \{\n  ~HintGuard\(\) \{ t_stream_hint = rdr_adapter::slot_geo_memo::StreamHint\{\}; \}\n\};'))
    R.append(('BuildGeometry holds the guard and sets the hint on every path (the other branch is an empty hint)', geo,
              r'HintGuard hint_guard;[^\n]*\n    t_stream_hint = \(hash_once && small_geo && hashed\) \? dm::StreamHint\{[^\n]*\} : dm::StreamHint\{\};'))
    R.append(('content skip uses ContentDone on the image flags and the record frame', reader, r'dm::ContentDone\(img\.copy_dest, img\.checked_frame, g_record_frame\)'))
    # EnsureContent's own first lines: what ContentDone transcribes
    R.append(('EnsureContent: copy_dest block before the per-frame check', ens, r'if \(img\.copy_dest\) \{[\s\S]*?\n  \}\n  const uint64_t frame = g_record_frame;\n  if \(img\.checked_frame == frame\) return;  // read once per frame at most'))
    R.append(('sampler memo skips the border-colour note case', reader, r'if \(\(w\[5\] & 3\) >= 2\) \{  // BuildSampler\'s census note'))
    R.append(('sampler memo keyed on the frame tables generation', reader, r'g_sampler_memo\.Find\(k, g_frame\.generation, index\)'))
    R.append(('sampler memo key from the shared builder', reader, r'dm::MakeSamplerKey\(w, t\.mag, t\.min, t\.mip, t\.aniso, g_aniso_override, g_jitter\.lod_bias, g_debug_lod0, g_debug_no_aniso\)'))
    R.append(('GeoKey uses the handed stream hash', reader, r'j\.pre_vhash_ok \? j\.pre_vhash : cap::Xxh3\(j\.vsrc, j\.hash_len\)'))
    R.append(('GeoJob clears the handed hash', reader, r'pre_vhash_ok = false;\s*pre_vhash = 0;\s*vs = nullptr;'))
    R.append(('RunGeometryBuild matches the hint on pointer and length', run, r'dm::HintMatches\(t_stream_hint, here\.vsrc, here\.hash_len\)'))
    R.append(('RunGeometryBuild never hands a hint to an inline draw', run, r'if \(!here\.is_inline && t_stream_hint\.valid &&'))
    R.append(('BuildGeometry keeps the hash it made', geo, r'hashed = true, hashed_stream = sh, hashed_full = h;'))
    R.append(('BuildGeometry combines the parts as the old code did', geo, r'dm::CombineSmall\(sh, index_bytes != 0, index_bytes \? cap::Xxh3\(db::Host\(base, index_virt\), index_bytes\) : 0\)'))
    R.append(('SlotKey::Equal compares every field', memo, r'generation == o\.generation && epoch == o\.epoch && fetch == o\.fetch && object == o\.object && std::memcmp\(w, o\.w, sizeof\(w\)\) == 0 &&\s*aniso_override == o\.aniso_override && lod_bias_bits == o\.lod_bias_bits &&\s*\(!object \|\| std::memcmp\(header_raw, o\.header_raw, sizeof\(header_raw\)\) == 0\)'))
    R.append(('ContentDone as EnsureContent returns', memo, r'return !copy_dest && checked_frame == record_frame;'))
    return R


def sampler_lint(reader):
    """violations in BuildSampler's body (empty = clean); the whole reader text is given"""
    a = reader.find('inline uint32_t BuildSampler(const')
    if a < 0:
        return ['BuildSampler not found']
    a = reader.index('{', a)
    body = reader[a:reader.find('\n}\n', a)]
    bad = ['w[%s]' % n for n in re.findall(r'\bw\[(\w+)\]', body) if n not in ('0', '3', '4', '5')]
    bad += ['t.%s' % n for n in re.findall(r'\bt\.(\w+)', body) if n not in ('mag', 'min', 'mip', 'aniso')]
    ok_g = {'g_aniso_override', 'g_jitter.lod_bias', 'g_debug_lod0', 'g_debug_no_aniso', 'g_frame.samplers', 'g_frame.sampler_map'}
    bad += [g for g in re.findall(r'\bg_[A-Za-z0-9_]+(?:\.[A-Za-z0-9_]+)?', body) if g not in ok_g]
    if 'REXCVAR_GET' in body:
        bad.append('REXCVAR_GET')
    return bad


def slot_miss_lint(ats):
    a = ats.find('  cap::TextureSlot out;\n  out.slot')
    b = ats.find('remember(1, id, out);', a)
    if a < 0 or b < 0:
        return ['slot miss path not found']
    t = ats[a:b]
    ok_img = {'checked_frame', 'content_hash', 'content_written', 'copy_dest', 'game_format', 'header', 'is_texture', 'pack_host', 'packed', 'read_gen', 'rec', 'swap', 'target16'}
    bad = ['img.%s' % n for n in re.findall(r'\bimg\.(\w+)', t) if n not in ok_img]
    bad += [g for g in re.findall(r'\bg_\w+', t) if g not in ('g_frame', 'g_images_dirty', 'g_reg')]
    bad += [c for c in re.findall(r'REXCVAR_GET\((\w+)\)', t) if c != 'wwe2k14_rdr_plane_repoint_legacy']
    ok_fn = {'BaseFormat', 'BuildSamplerMemo', 'ById', 'ComposeSwizzle', 'FillTextureRecord', 'FormatOf', 'KeyOfHeaderWords', 'Ld32', 'Note', 'REXCVAR_GET',
             'RememberHeaderWords', 'SetTexture', 'ShapeOf', 'SignedView', 'TextureOfObject', 'TextureOfWords', 'ToPhysical'}
    bad += [f + '()' for f in re.findall(r'\b([A-Z]\w*)\(', t) if f not in ok_fn]
    return bad


def check_one(text, rx):
    if not text:
        return False
    return bool(rx(text)) if callable(rx) else bool(re.search(rx, text, re.M))


def evaluate(reqs):
    return [r[0] for r in reqs if not check_one(r[1], r[2])]


def run_wiring(root):
    paths = {'reader': 'src/renderer_adapter/reader.h', 'switches': 'src/renderer_adapter/rdr_switches.h', 'memo': 'src/renderer_adapter/slot_geo_memo.h'}
    files = {}
    for k, p in paths.items():
        fp = os.path.join(root, p)
        if not os.path.isfile(fp):
            return fail('missing ' + fp)
        files[k] = open(fp, encoding='utf8', errors='replace').read().replace('\r\n', '\n')
    reqs = requirements(files)
    bad = evaluate(reqs)
    for name in bad:
        print('FAIL wiring: ' + name)
    if bad:
        return fail('%d wiring requirement(s) missing' % len(bad))
    print('PASS wiring: %d requirements hold on the real files' % len(reqs))
    # planted twins: each requirement removed from its text must be reported by name
    missed = 0
    for req in reqs:
        name, text, rx = req[0], req[1], req[2]
        if len(req) > 3:
            planted = req[3](text)
        else:
            m = re.search(rx, text, re.M)
            planted = text[:m.start()] + text[m.end():]
        if planted == text:
            print('FAIL planted twin DID NOT APPLY: ' + name)
            missed += 1
            continue
        caught = evaluate([(name, planted, rx)]) == [name]
        if not caught:
            print('FAIL planted twin NOT caught: ' + name)
            missed += 1
    if missed:
        return fail('%d wiring plant(s) not caught' % missed)
    print('PASS wiring planted twins: all %d removals reported by name' % len(reqs))
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    ap.add_argument('--part', default='all', choices=['all', 'test', 'wiring'])
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    rc = 0
    if a.part in ('all', 'wiring'):
        rc |= run_wiring(root)
    if a.part in ('all', 'test'):
        rc |= run_test(root)
    return rc


if __name__ == '__main__':
    sys.exit(main())
