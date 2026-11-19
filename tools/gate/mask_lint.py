r"""H2 / L2 WRITTEN-ONCE MASK LINT (S103 B3F, D1: a constant a resolver fills from per-frame / per-material data, but whose draw_lists.cpp refresh mask is 0, is written once and goes stale).

  py -I tools\gate\mask_lint.py [--root DIR]      exit 1 on a violation
  py -I tools\gate\mask_lint.py --selftest         the planted twins and the re-found bugs (B3F's HairSpeParam / SpeCol1 / SpeCol2 / RimLightParam3, D1's TexProjLight / ViewLight with mask 0 put back)

Reads src\renderer_scene\front_end.cpp (Resolve) and char_effects.cpp (ResolveChar): every `name == "X"` branch whose body reads DATA (a member of the material `mt.`, the frame / object context `c.` / `cam.`, the light set `ls.`)
is DATA-FED. Each data-fed name must have a NON-ZERO mask in draw_lists.cpp's kTable (a name missing from the table is kStAll: safe). A mask-0 entry that is fed from data and is a known-constant
for another reason goes in BASELINE below with its reason.
The branch body is the rest of its line, or, when it opens a brace that closes on a later line, up to that brace. The data tokens are DATA_RX."""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
RESOLVERS = ['src/renderer_scene/front_end.cpp', 'src/renderer_scene/char_effects.cpp']
TABLE = 'src/renderer_scene/draw_lists.cpp'
NAME_RX = re.compile(r'name == "(\w+)"')
DATA_RX = re.compile(r'\b(?:mt|ls|cam|rec|mat)\.\w+|\bc\.\w+')
ENTRY_RX = re.compile(r'\{"(\w+)",\s*([^}]*?)\}')
# name -> reason: fed from data in a resolver but a mask of 0 is right (the data does not change once an object is kept). EMPTY on purpose: justify each line.
BASELINE = {}


def strip_comment(line):
    i = line.find('//')
    return line if i < 0 else line[:i]


def table_masks(text):
    a = text.index('const NameMask kTable[]')
    b = text.index('};', a)
    out = {}
    for m in ENTRY_RX.finditer(text[a:b]):
        mask = re.sub(r'/\*.*?\*/', '', m.group(2)).strip()
        out[m.group(1)] = mask
    return out


def data_fed(text):
    """{name: [(line no, body)]} of the data-fed branches of one resolver."""
    lines = text.split('\n')
    out = {}
    for i, ln in enumerate(lines):
        code = strip_comment(ln)
        names = NAME_RX.findall(code)
        if not names:
            continue
        # the branch body: from the first name on this line to its closing brace (same line or later), else the rest of the line
        k = code.find('name == "')
        body = code[k:]
        depth = body.count('{') - body.count('}')
        j = i
        while depth > 0 and j + 1 < len(lines) and j - i < 12:
            j += 1
            nxt = strip_comment(lines[j])
            body += '\n' + nxt
            depth += nxt.count('{') - nxt.count('}')
        # a condition-only line that opens a wider block (if (name == "A" || name == "B") return true;) has no data: DATA_RX finds nothing
        cond = body.split('{', 1)[0]
        stmt = body[len(cond):] if '{' in body else body
        if DATA_RX.search(stmt):
            for n in names:
                out.setdefault(n, []).append((i + 1, stmt.strip().split('\n')[0][:110]))
    return out


def lint(root, tables=None, resolvers=None):
    bad = []
    masks = table_masks(tables if tables is not None else open(os.path.join(root, TABLE), encoding='utf-8').read())
    texts = resolvers if resolvers is not None else {p: open(os.path.join(root, p), encoding='utf-8').read() for p in RESOLVERS}
    for p, t in texts.items():
        for n, where in data_fed(t).items():
            if masks.get(n) == '0' and n not in BASELINE:
                bad.append(f'{p}:{where[0][0]}: "{n}" is fed from data ({where[0][1]}) but draw_lists.cpp has mask 0: written once, stale after the first draw')
    return bad


def selftest(root):
    ok = True
    table = open(os.path.join(root, TABLE), encoding='utf-8').read()
    res = {p: open(os.path.join(root, p), encoding='utf-8').read() for p in RESOLVERS}
    clean = lint(root)
    print(f"  {'ok  ' if not clean else 'FAIL'} the current tree: {len(clean)} violation(s) {clean[:2]}")
    ok &= not clean
    plants = {
        'B3F: HairSpeParam / SpeCol1 / SpeCol2 / RimLightParam3 back to 0': [('{"RimLightParam3", kStMaterial}', '{"RimLightParam3", 0}'), ('{"HairSpeParam", kStMaterial}', '{"HairSpeParam", 0}'),
                                                                          ('{"SpeCol1", kStMaterial}', '{"SpeCol1", 0}'), ('{"SpeCol2", kStMaterial}', '{"SpeCol2", 0}')],
        'D1: TexProjLight / ViewLight back to 0': [('{"TexProjLight", kStLight}', '{"TexProjLight", 0}'), ('{"ViewLight", kStLight}', '{"ViewLight", 0}')],
        'L6: ShadowPrm to 0': [('{"ShadowPrm", kStLight}', '{"ShadowPrm", 0}')],
    }
    for label, subs in plants.items():
        t = table
        for a, b in subs:
            if a not in t:
                print(f'  FAIL plant "{label}": {a} is not in the table (the lint twin needs updating)')
                ok = False
            t = t.replace(a, b)
        got = lint(root, tables=t, resolvers=res)
        want = [b.split('"')[1] for _, b in subs]
        hit = [w for w in want if any(f'"{w}"' in g for g in got)]
        good = len(hit) == len(want)
        print(f"  {'ok  ' if good else 'FAIL'} {label}: caught {hit} of {want}")
        ok &= good
    print('selftest ' + ('PASSED' if ok else 'FAILED'))
    return ok


def main():
    a = sys.argv[1:]
    root = a[a.index('--root') + 1] if '--root' in a else ROOT
    if '--selftest' in a:
        sys.exit(0 if selftest(root) else 1)
    bad = lint(root)
    for b in bad:
        print('ERROR ' + b)
    print(f'mask_lint: {len(bad)} violation(s)')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
