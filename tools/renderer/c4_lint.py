r"""S102 lane C4 lints (offline, no build). py -I tools\renderer\c4_lint.py [--root DIR]
  F5   binder.h AlphaOpaque (the per-draw decision of the opaque-alpha rule) builds no std::string and takes no lock: the census is relaxed atomic counters per (vs, ps) row, names are built at print time.
  F11  src\gfx_applied.h kLowLevers: no string literal in the table holds a control-character escape (\r \t \n \b \f \a \v): a path written with a single backslash ("renderer_adapter\reader.h") is a CR.
  F8   tools\renderer\compile_shaders.py: alpha_meta reads the variant's source defines (FX_SCROLL is defined inside the UVScroll fragments), and src\renderer\shaders\spv\fx_registry.h marks no yBG_UVScroll* fragment out_alpha 2.
Each lint runs on the real file and on a planted twin that must fail."""
import argparse, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
FAILS = []


def say(ok, text):
    print(('PASS ' if ok else 'FAIL ') + text, flush=True)
    if not ok:
        FAILS.append(text)


def function_body(text, signature):
    """The text of the function whose signature line contains `signature`, up to its closing brace at the same indent."""
    i = text.index(signature)
    start = text.rfind('\n', 0, i) + 1
    indent = len(text[start:i]) - len(text[start:i].lstrip())
    close = text.index('\n' + ' ' * indent + '}', i)
    return text[i:close]


def f5_problems(body):
    code = '\n'.join(l.split('//')[0] for l in body.splitlines())
    out = []
    if re.search(r'std::string\s*[\w(]', code) or 'std::string(' in code or 'std::to_string' in code:
        out.append('std::string in the per-draw decision')
    if re.search(r'\bmutex\b|lock_guard|unique_lock|scoped_lock', code):
        out.append('a lock in the per-draw decision')
    if 'NoteAlpha(program' in code:
        out.append('the old string-keyed census')
    return out


def f11_problems(text):
    a = text.index('kLowLevers[] = {')
    b = text.index('};', a)
    out = []
    for n, line in enumerate(text[a:b].splitlines()):
        for lit in re.findall(r'"((?:[^"\\]|\\.)*)"', line):
            m = re.search(r'(?<!\\)(?:\\\\)*\\([rtnbfav])', lit)
            if m:
                out.append('table line %d: control escape \\%s in "%s..."' % (n, m.group(1), lit[:50]))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    root = ap.parse_args().root
    binder = open(os.path.join(root, 'src', 'renderer_adapter', 'binder.h'), encoding='utf-8').read()
    body = function_body(binder, 'uint8_t AlphaOpaque(')
    p = f5_problems(body)
    say(not p, 'F5: AlphaOpaque (%d chars) builds no string and takes no lock %s' % (len(body), p))
    say(bool(f5_problems(body + '\n    const std::string program = std::string("x");\n')), 'F5 twin: a std::string in the decision is caught')
    say(bool(f5_problems(body + '\n    std::lock_guard<std::mutex> lock(m);\n')), 'F5 twin: a lock in the decision is caught')
    gfx = open(os.path.join(root, 'src', 'gfx_applied.h'), encoding='utf-8').read()
    p = f11_problems(gfx)
    say(not p, 'F11: kLowLevers holds no control-character escape %s' % p[:3])
    twin = gfx.replace('"renderer_adapter\\\\reader.h rdr_switches.h ArenaOverdraw', '"renderer_adapter\\reader.h rdr_switches.h ArenaOverdraw', 1)
    say(twin != gfx and bool(f11_problems(twin)), 'F11 twin: a single backslash before reader.h is caught (and the twin applied)')
    cs = open(os.path.join(root, 'tools', 'renderer', 'compile_shaders.py'), encoding='utf-8').read()
    say('source_defines(effect, v, stage)' in cs and "assert not ((set(v.get('defines', [])) | source_defines" in cs, 'F8: alpha_meta reads the source defines and asserts none of the break list reaches out_alpha 2')
    reg = open(os.path.join(root, 'src', 'renderer', 'shaders', 'spv', 'fx_registry.h'), encoding='utf-8').read()
    bad = [m.group(0)[:60] for m in re.finditer(r'\{"yBG_UVScroll[A-Za-z_]*_fragment"[^\n]*?, 2, 255\}', reg)]
    say(not bad, 'F8: no yBG_UVScroll* fragment is out_alpha 2 in the registry %s' % bad[:2])
    n2 = len(re.findall(r', 2, 255\}', reg))
    say(n2 > 0, 'F8: the registry has %d out_alpha 2 fragment variants (0 would mean the lever is blind)' % n2)
    print('RESULT: %s' % ('FAIL (%d)' % len(FAILS) if FAILS else 'PASS'))
    sys.exit(1 if FAILS else 0)


if __name__ == '__main__':
    main()
