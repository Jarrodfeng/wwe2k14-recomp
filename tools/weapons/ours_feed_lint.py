#!/usr/bin/env python3
r"""ours_feed_lint (S103 PWI2, the PW review's finding 1; CW-2c readers merged in): "dies under OURS".

The stream-free (OFF) build runs EVERY frame as OURS: scene_skip::Ours(Hook::Model) makes the raw hook on sub_82C04DF0 return before the original, so a feed placed INSIDE the original (the mid-asm hook
wwe2k14_model_entry at 0x82C04E1C) or in the D3 skip lambdas (g_d3_*) never runs. A contributor registered with `!WWE2K14_STREAM()` (on in the OFF build) whose only feed is such a hook records nothing in the build
it was registered for (the PW-2 weapon rows did exactly this).

Rule: every namespace that is registered under `!WWE2K14_STREAM()` in src\wwe2k14_app.h AND is fed through a `wwe2k14::<ns>::ModelEntryHook(` / `RawEntry(` call must have at least one feed that sits in a
REX_HOOK_RAW body BEFORE any `return` (the Ours() test returns before the original); otherwise docs\renderer\OURS_INPUT_AUDIT.md's "Feeds table" must name it with the verdict DIES (a known cut, with its audit row), and a row that says LIVE must
be true (a LIVE row with no raw feed before the skip is an error too: the table stays the truth).

  py -I tools\weapons\ours_feed_lint.py --root .        scan the tree (exit 1 on an error)
  py -I tools\weapons\ours_feed_lint.py --selftest      planted twins (the PW-2 hook as it was must be flagged)
Text scan, no compile."""
import argparse
import os
import re
import sys

APP = 'src/wwe2k14_app.h'
AUDIT = 'docs/renderer/OURS_INPUT_AUDIT.md'
FEED = re.compile(r'wwe2k14::(\w+)::(?:ModelEntryHook|RawEntry)\s*\(')
REG = re.compile(r'\b(?:\w+::)*(\w+)::Register\w*\s*\(')


def strip_comments(t):
    t = re.sub(r'/\*.*?\*/', lambda m: re.sub(r'[^\n]', ' ', m.group(0)), t, flags=re.S)
    return re.sub(r'//[^\n]*', '', t)


def body_at(code, i):
    j = code.find('{', i)
    depth, k = 0, j
    while k < len(code):
        depth += {'{': 1, '}': -1}.get(code[k], 0)
        if depth == 0:
            return j, k
        k += 1
    return j, len(code)


def feeds(texts):
    """{ns: [(file, kind)]}: kind = 'raw_before_skip' | 'raw_after_skip' | 'mid_asm' (wwe2k14_model_entry / g_d3_ lambdas / anything else)."""
    out = {}
    for f, t in texts.items():
        code = strip_comments(t)
        raw_spans = []
        for m in re.finditer(r'REX_HOOK_RAW\s*\(\s*\w+\s*\)\s*\{', code):
            a, b = body_at(code, m.start())
            raw_spans.append((a, b))
        for m in FEED.finditer(code):
            kind = 'mid_asm'
            for a, b in raw_spans:
                if a < m.start() < b:
                    early_return = re.search(r'\breturn\b', code[a:m.start()])   # any return before the feed can skip it (the Ours() test returns)
                    kind = 'raw_after_skip' if early_return else 'raw_before_skip'
            out.setdefault(m.group(1), []).append((f, kind))
    return out


def registered_off(app_text):
    """namespaces whose Register*() sits on a line that is on in the stream-free build (`!WWE2K14_STREAM()` in the same statement)."""
    code = strip_comments(app_text)
    ns = set()
    for stmt in re.split(r';', code):
        if '!WWE2K14_STREAM()' in stmt:
            for m in REG.finditer(stmt):
                ns.add(m.group(1))
    return ns


def table_rows(audit_text):
    """{ns: verdict} from the '## Feeds table' section: `| ns | feed | LIVE|DIES |`."""
    m = re.search(r'^## Feeds table[^\n]*\n(.*?)(?=^## |\Z)', audit_text, re.S | re.M)
    rows = {}
    if not m:
        return None
    for ln in m.group(1).splitlines():
        c = [x.strip().strip('`') for x in ln.strip().strip('|').split('|')]
        if len(c) >= 3 and c[2] in ('LIVE', 'DIES') and re.fullmatch(r'\w+', c[0]):
            rows[c[0]] = c[2]
    return rows


def lint(texts, app_text, audit_text):
    errs = []
    rows = table_rows(audit_text)
    if rows is None:
        return ['%s: no "## Feeds table" section (the lint reads nothing: refusing)' % AUDIT]
    fd = feeds(texts)
    for ns in sorted(registered_off(app_text)):
        if ns not in fd:
            continue   # not a model-call feed (a latch reader): other lints
        live = any(k == 'raw_before_skip' for _, k in fd[ns])
        verdict = rows.get(ns)
        if live and verdict == 'DIES':
            errs.append('%s: row %s says DIES but a raw hook feeds it before the Ours() test: make it LIVE' % (AUDIT, ns))
        elif live and verdict != 'LIVE':
            errs.append('%s: namespace %s is registered in the stream-free build and fed before the skip: add a LIVE row to the Feeds table' % (AUDIT, ns))
        elif not live and verdict != 'DIES':
            sites = ', '.join('%s (%s)' % s for s in fd[ns])
            errs.append('%s: contributor %s is registered under !WWE2K14_STREAM() but dies under OURS: its only feeds are %s, inside a function scene_skip::Ours(Hook::Model) cuts. '
                        'Feed it from the REX_HOOK_RAW BEFORE the Ours() test, or add a DIES row to the Feeds table of %s' % (APP, ns, sites, AUDIT))
    return errs


# S103 CW-2c (merged from tools\scene\ours_feed_lint.py): readers the OURS pictures depend on whatever the registration line says (the guest model reader registers unconditionally):
# each must be called from the raw hook on sub_82C04DF0 BEFORE the Ours() test and must not be called from the mid-asm wwe2k14_model_entry (src\fp_probe.h), which never runs under OURS.
OURS_FED = {
    'guestmodel::OnModelEntry': 'S103 CW-2c: the CAW body and any model the record cannot name',
}
RAW_MODEL = re.compile(r'REX_HOOK_RAW\(sub_82C04DF0\)\s*\{')


def lint_always_fed(files):
    bad = []
    dro = files.get('src/draw_origin.h', '')
    fpp = files.get('src/fp_probe.h', '')
    m = RAW_MODEL.search(dro)
    if not m:
        return ['src/draw_origin.h: REX_HOOK_RAW(sub_82C04DF0) not found']
    body = strip_comments(dro[m.end():])
    ours = body.find('scene_skip::Ours(')
    if ours < 0:
        return ['src/draw_origin.h: the raw hook no longer has the Ours check; the lint must be updated']
    mid = re.search(r'void wwe2k14_model_entry\(.*?\n\}', strip_comments(fpp), re.S)
    for name, why in OURS_FED.items():
        i = body.find(name)
        if i < 0:
            bad.append('%s (%s) is not called from the raw hook on sub_82C04DF0: it dies under OURS' % (name, why))
        elif i > ours:
            bad.append('%s (%s) is called AFTER the Ours() test in the raw hook: it dies under OURS' % (name, why))
        if mid and name in mid.group(0):
            bad.append('%s is called from the mid-asm wwe2k14_model_entry (never runs under OURS): remove it' % name)
    return bad


def selftest():
    bad = 0

    def case(name, errs, want):
        nonlocal bad
        ok = (len(errs) > 0) == want
        print('%s  %s%s' % ('PASS' if ok else 'FAIL', name, '' if ok else '  -> ' + ' | '.join(errs)))
        bad += 0 if ok else 1

    app = 'void OnPostSetup() { if (!WWE2K14_STREAM() || X) rdr_scene::live::weaponrows::RegisterWeaponRows(); }'
    table_live = '## Feeds table\n| ns | feed | verdict |\n| --- | --- | --- |\n| weaponrows | raw hook | LIVE |\n## next\n'
    table_dies = table_live.replace('LIVE', 'DIES')
    table_none = '## Feeds table\n| ns | feed | verdict |\n## next\n'
    raw_first = 'REX_HOOK_RAW(sub_82C04DF0) {\n  wwe2k14::weaponrows::RawEntry(a, b);\n  if (Ours(Hook::Model)) return;\n}\n'
    raw_after = 'REX_HOOK_RAW(sub_82C04DF0) {\n  if (Ours(Hook::Model)) return;\n  wwe2k14::weaponrows::RawEntry(a, b);\n}\n'
    mid = 'void wwe2k14_model_entry(PPCRegister& r3) {\n  wwe2k14::weaponrows::ModelEntryHook(r3.u32, 1, 2);\n}\n'
    case('raw hook feeds BEFORE the Ours() test, LIVE row: passes', lint({'src/draw_origin.h': raw_first}, app, table_live), False)
    case('the PW-2 hook as it was (mid-asm only, no DIES row): flagged', lint({'src/fp_probe.h': mid}, app, table_none), True)
    case('planted twin: the same call placed AFTER the Ours() test: flagged', lint({'src/draw_origin.h': raw_after}, app, table_none), True)
    case('a known cut with a DIES row passes', lint({'src/fp_probe.h': mid}, app, table_dies), False)
    case('a DIES row for a namespace that is fed before the skip: flagged (the table stays true)', lint({'src/draw_origin.h': raw_first}, app, table_dies), True)
    case('a namespace not registered under !WWE2K14_STREAM() is not checked', lint({'src/fp_probe.h': mid}, 'void OnPostSetup() { if (X) rdr_scene::live::weaponrows::RegisterWeaponRows(); }', table_none), False)
    case('a missing Feeds table is refused', lint({'src/fp_probe.h': mid}, app, '# nothing\n'), True)
    # CW-2c twins (the guest model reader)
    gm_ok = 'REX_HOOK_RAW(sub_82C04DF0) {\n  rdr_scene::live::guestmodel::OnModelEntry(ctx.r3.u32, f);\n  if (wwe2k14::scene_skip::Ours(Hook::Model)) return;\n}\n'
    gm_after = 'REX_HOOK_RAW(sub_82C04DF0) {\n  if (wwe2k14::scene_skip::Ours(Hook::Model)) return;\n  rdr_scene::live::guestmodel::OnModelEntry(ctx.r3.u32, f);\n}\n'
    gm_none = 'REX_HOOK_RAW(sub_82C04DF0) {\n  if (wwe2k14::scene_skip::Ours(Hook::Model)) return;\n}\n'
    gm_mid = 'void wwe2k14_model_entry(PPCRegister& r3, PPCRegister& r5, PPCRegister& r13) {\n  rdr_scene::live::guestmodel::OnModelEntry(r3.u32, 0);\n}\n'
    case('CW-2c: guest reader called before the Ours() test: passes', lint_always_fed({'src/draw_origin.h': gm_ok, 'src/fp_probe.h': ''}), False)
    case('CW-2c twin: the guest reader call is missing from the raw hook: flagged', lint_always_fed({'src/draw_origin.h': gm_none, 'src/fp_probe.h': ''}), True)
    case('CW-2c twin: the guest reader call is AFTER the Ours() test: flagged', lint_always_fed({'src/draw_origin.h': gm_after, 'src/fp_probe.h': ''}), True)
    case('CW-2c twin: the guest reader call is in the mid-asm function: flagged', lint_always_fed({'src/draw_origin.h': gm_ok, 'src/fp_probe.h': gm_mid}), True)
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        bad = selftest()
        print('ours_feed_lint selftest: %s' % ('ALL PASS' if not bad else '%d FAILED' % bad))
        return 1 if bad else 0
    root = os.path.abspath(a.root)
    texts = {}
    for dp, _, fns in os.walk(os.path.join(root, 'src')):
        for fn in fns:
            if fn.endswith('.h'):
                p = os.path.join(dp, fn)
                texts[os.path.relpath(p, root).replace('\\', '/')] = open(p, encoding='utf-8', errors='replace').read()
    app = open(os.path.join(root, APP), encoding='utf-8', errors='replace').read()
    audit = open(os.path.join(root, AUDIT), encoding='utf-8', errors='replace').read()
    if not feeds(texts):
        print('FAIL: no ModelEntryHook / RawEntry feed found under src (the lint reads nothing: refusing)')
        return 1
    errs = lint(texts, app, audit) + lint_always_fed(texts)
    for e in errs:
        print('ERROR ' + e)
    print('ours_feed_lint: %d error(s); registered in the stream-free build: %s' % (len(errs), ', '.join(sorted(registered_off(app))) or '-'))
    return 1 if errs else 0


if __name__ == '__main__':
    sys.exit(main())
