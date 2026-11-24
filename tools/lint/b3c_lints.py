r"""S103 B3c lints (review findings that became checks). Offline, no game.

  py -I tools\lint\b3c_lints.py [--write-baseline]

  frame_field_unit   every field of SceneFrame (src\renderer_scene\front_end.h) is read by a C++ unit / rig under tools\scene\*.cpp (the name appears there) -
                     a field nothing reads is untested. Old fields without one: tools\lint\b3c_baseline.txt (frame_field_unit NAME).
  counter_reader     every atomic counter (`std::atomic<uint64_t|uint32_t> name`, an `inline` global or a member) and every `uint64_t name() const { return name_; }` getter under src\
                     is read somewhere: its name appears again outside its declaration line (a census / log / test). Old ones: the baseline (counter_reader NAME).
  f32_port           a line marked `[f32]` (a port of the game's single-precision maths) uses f constants only: no `double`, no float literal without the f suffix.
Exit 1 on any finding not in the baseline."""
import os
import re
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
BASE = os.path.join(ROOT, 'tools', 'lint', 'b3c_baseline.txt')


def read(p):
    with open(p, encoding='utf-8', errors='replace') as f:
        return f.read()


def walk(top, exts):
    for d, _, fs in os.walk(os.path.join(ROOT, top)):
        for n in fs:
            if n.endswith(exts):
                yield os.path.join(d, n)


def frame_fields():
    s = read(os.path.join(ROOT, 'src', 'renderer_scene', 'front_end.h'))
    i = s.index('struct SceneFrame {')
    j = s.index('\n};', i)
    out = []
    depth = 0
    for line in s[i:j].split('\n')[1:]:
        code = re.sub(r'//.*', '', line).strip()
        depth += code.count('{') - code.count('}')
        m = re.match(r'^(?:[\w:<>,\*&\s]+?)\s+(\w+)\s*(?:=\s*[^;]*|\[[^\]]*\])?\s*;$', code)
        if depth == 0 and m and '(' not in code.split('=')[0] and not code.startswith(('using', 'friend', 'return')):
            out.append(m.group(1))
    return out


def counters():
    found = {}
    pat = re.compile(r'std::atomic<\s*(?:uint64_t|uint32_t|size_t|int64_t)\s*>\s*((?:\w+\s*(?:\{[^}]*\})?\s*,\s*)*\w+\s*(?:\{[^}]*\})?)\s*;')
    get = re.compile(r'\b(?:uint64_t|uint32_t)\s+(\w+)\(\)\s+const\s*\{\s*return\s+\w+_\s*;\s*\}')
    for p in list(walk('src', ('.h', '.cpp'))):
        for ln, line in enumerate(read(p).split('\n'), 1):
            for m in pat.finditer(line):
                for nm in re.findall(r'(\w+)\s*(?:\{[^}]*\})?\s*(?:,|$)', m.group(1)):
                    found.setdefault(nm, (p, ln))
            m = get.search(line)
            if m:
                found.setdefault(m.group(1), (p, ln))
    return found


def main():
    base = set()
    if os.path.exists(BASE):
        base = {l.split('#')[0].strip() for l in read(BASE).split('\n') if l.split('#')[0].strip()}
    bad = []
    units = '\n'.join(read(p) for p in walk(os.path.join('tools', 'scene'), ('.cpp',)))
    for f in frame_fields():
        if not re.search(r'\b%s\b' % re.escape(f), units):
            bad.append(('frame_field_unit', f, 'SceneFrame::%s is read by no tools\\scene\\*.cpp unit / rig' % f))
    # a reader = the name on a line that is not its declaration and does not only write it (fetch_add / store / ++): a census line, a log, a test
    import collections
    ident = re.compile(r'[A-Za-z_]\w*')
    reads = collections.Counter()
    for p in list(walk('src', ('.h', '.cpp'))) + list(walk('tools', ('.cpp', '.h'))):
        for line in read(p).split('\n'):
            # the returned value of fetch_add / fetch_sub / exchange is consumed (`x = n.fetch_add(1)`, an id generator): that is a read, not a bare write
            used = re.search(r'(?:=|\breturn)\s*[\w:.>\-\[\]]+\.(?:fetch_add|fetch_sub|exchange)\(', line)
            # S103: a result compared in a condition is consumed too (`if (x.exchange(v) != v)`: a change detector)
            used = used or re.search(r'\b(?:if|while)\s*\(\s*[\w:.>\-\[\]]+\.(?:fetch_add|fetch_sub|exchange)\([^)]*\)\s*(?:[!=<>]=|[<>])', line)
            if not used and ('std::atomic<' in line or 'fetch_add' in line or '.store(' in line or '++' in line or 'fetch_sub' in line):
                continue
            for w in set(ident.findall(line)):
                reads[w] += 1
    for nm, (p, ln) in counters().items():
        stem = nm.rstrip('_')
        if reads[stem] + reads[stem + '_'] + (reads[nm] if nm not in (stem, stem + '_') else 0) == 0:
            bad.append(('counter_reader', nm, '%s:%d: counter %s has no reader (census / log / test)' % (os.path.relpath(p, ROOT), ln, nm)))
    for p in list(walk('src', ('.h', '.cpp'))) + list(walk('tools', ('.cpp', '.h'))):
        for ln, line in enumerate(read(p).split('\n'), 1):
            if '[f32]' not in line:
                continue
            code = re.sub(r'//.*', '', line)
            if re.search(r'\bdouble\b', code) or re.search(r'(?<![\w.])\d+\.\d*(?:[eE][-+]?\d+)?(?![\dfFeE])', code):
                bad.append(('f32_port', '%s:%d' % (os.path.relpath(p, ROOT), ln), '%s:%d: [f32] line uses a double or a float literal without f' % (os.path.relpath(p, ROOT), ln)))
    if '--write-baseline' in sys.argv:
        with open(BASE, 'w', encoding='utf-8', newline='\n') as f:
            f.write('# S103 B3c: findings that already existed when the lints were made (rule NAME); a new one fails the lint.\n')
            for rule, key, _ in sorted(set(bad)):
                if rule != 'f32_port':
                    f.write('%s %s\n' % (rule, key))
        print('baseline written: %d entries' % len([b for b in bad if b[0] != 'f32_port']))
        return 0
    new = [b for b in bad if '%s %s' % (b[0], b[1]) not in base]
    for rule, key, msg in new:
        print('FAIL  [%s] %s' % (rule, msg))
    print('%d finding(s) not in the baseline (%d baselined)' % (len(new), len(bad) - len(new)))
    return 1 if new else 0


if __name__ == '__main__':
    sys.exit(main())
