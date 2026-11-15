r"""S102 lane T19 lints (review fixes F1, F3, F4, F7): the class faults behind the T19 review findings, as checks.

  py -I tools\gate\t19_lint.py [--root DIR]     exit 1 on any violation
  py -I tools\gate\t19_lint.py --selftest        the planted twins: each lint must fail on its own mutation (exit 1 if one does not)

  rsc_readers   every .py under tools\ that reads scene record bytes (the 'RSCN' magic, or a .rsc file read as bytes / unpacked) must TEST the record version: a version 2 record (crowd
                run markers) read by a version-blind reader answers wrongly and silently (F1). The writers expand, so files on disk are version 1; this is the second line.
  estimate      src\case\case_scene.cpp's size estimate names every SceneTables vector (F3: crowd_runs / crowd_members were left out).
  expand_names  record_io.cpp's CopyOtherTables names every SceneTables member except the four the expansion rebuilds (objects, engine_objects, crowd_runs, crowd_members).
  live_order    in renderer_adapter\live.cpp's render loop, every read of message.record / message.d3_record comes after its expansion (F4).
  no_root_tmp   no *.tmp file at the repo root, tracked or not (F7)."""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))

MAGIC = re.compile(r"0x4e435352|b'RSCN'|b\"RSCN\"|RSCN", re.I)
RSC_BYTES = re.compile(r"\.rsc\b")
VERSION_TEST = re.compile(r"\bver(?:sion)?\w*\s*(?:!=|==|>=|<=|<|>)\s*(?:\d|[A-Z_]{3,})|\bver(?:sion)?\w*\s+(?:not\s+)?in\s*[(\[{]|\bver(?:sion)?\w*\s*\[\d\]\s*(?:!=|==)|rsc_version_ok\(")
EXEMPT_PY = {'t19_lint.py'}


def py_files(root):
    for d, dirs, files in os.walk(os.path.join(root, 'tools')):
        dirs[:] = [x for x in dirs if x not in ('__pycache__', 'out', 'node_modules')]
        for f in files:
            if f.endswith('.py') and f not in EXEMPT_PY:
                yield os.path.join(d, f)


HEADER3 = re.compile(r"magic\s*,\s*ver\w*\s*,[^=\n]*=\s*struct\.unpack_from\('<III'")   # a record header read as (magic, version, section count)
CROWD_BYTES = re.compile(r"b['\"]crowd:")                  # a record's strings searched as bytes


def reads_records(text):
    return bool(MAGIC.search(text)) or bool(HEADER3.search(text)) or (bool(RSC_BYTES.search(text)) and bool(CROWD_BYTES.search(text)))


def rsc_readers(root, files=None):
    bad = []
    for p in (files if files is not None else py_files(root)):
        text = open(p, encoding='utf-8', errors='replace').read()
        if reads_records(text) and not VERSION_TEST.search(text):
            bad.append(os.path.relpath(p, root))
    return bad


def struct_members(header_text):
    """(vectors, others) member names of struct SceneTables, from record_io.h."""
    a = header_text.index('struct SceneTables {')
    b = header_text.index('SceneTables();', a)
    body = header_text[a:b]
    vecs, other = [], []
    for line in body.split('\n'):
        code = line.split('//')[0].strip()
        m = re.match(r'std::vector<[^;]+>\s+(\w+)\s*;', code)
        if m:
            vecs.append(m.group(1))
            continue
        m = re.match(r'(?:uint32_t|bool|SceneHeader|Camera|std::string)\s+(\w+)\s*(?:=\s*[^;]+)?;', code)
        if m:
            other.append(m.group(1))
    return vecs, other


def estimate(root, case_text=None, header_text=None):
    header_text = header_text or open(os.path.join(root, 'src/renderer_scene/record_io.h'), encoding='utf-8').read()
    case_text = case_text or open(os.path.join(root, 'src/case/case_scene.cpp'), encoding='utf-8').read()
    vecs, _ = struct_members(header_text)
    a = case_text.index('const uint64_t estimate')
    b = case_text.index('case_hook::OnScene', a)
    est = case_text[a:b]
    return [v for v in vecs if not re.search(r'\br\.%s\.size\(\)' % v, est)], len(vecs)


def expand_names(root, cpp_text=None, header_text=None):
    header_text = header_text or open(os.path.join(root, 'src/renderer_scene/record_io.h'), encoding='utf-8').read()
    cpp_text = cpp_text or open(os.path.join(root, 'src/renderer_scene/record_io.cpp'), encoding='utf-8').read().replace('\r\n', '\n')
    vecs, other = struct_members(header_text)
    a = cpp_text.index('static void CopyOtherTables(')
    b = cpp_text.index('\n}\n', a)
    body = cpp_text[a:b]
    skip = {'objects', 'engine_objects', 'crowd_runs', 'crowd_members'}
    missing = [v for v in vecs + other if v not in skip and not re.search(r'\bo\.%s\b' % v, body)]
    return missing, len(vecs) + len(other)


def live_order(root, text=None):
    """Problems in the render loop's use of message.record / message.d3_record."""
    text = text or open(os.path.join(root, 'src/renderer_adapter/live.cpp'), encoding='utf-8').read()
    lines = [l.split('//')[0] for l in text.split('\n')]   # (comments do not read anything)
    recv = next((i for i, l in enumerate(lines) if 'message = std::move(mail.front())' in l), None)
    if recv is None:
        return ['the receive line (message = std::move(mail.front())) was not found: the lint input changed']
    end = min(len(lines), recv + 420)
    bad = []
    for name in ('record', 'd3_record'):
        pat = re.compile(r'\bmessage\.%s\b' % name)
        exp = [i for i in range(recv, end) if pat.search(lines[i]) and 'ExpandedForReading(' in lines[i]]
        if len(exp) != 1:
            bad.append('message.%s: expected exactly one expansion line in the render loop, found %d' % (name, len(exp)))
            continue
        for i in range(recv, end):
            if pat.search(lines[i]) and i < exp[0]:
                bad.append('message.%s is read at line %d, before its expansion (line %d)' % (name, i + 1, exp[0] + 1))
    return bad


def root_tmp(root):
    bad = [f for f in os.listdir(root) if f.endswith('.tmp') and os.path.isfile(os.path.join(root, f))]
    r = subprocess.run(['git', '-C', root, 'ls-files', '*.tmp'], capture_output=True, text=True)
    bad += [f for f in r.stdout.split() if '/' not in f and f not in bad]
    return bad


def main_checks(root):
    fails = 0

    def report(ok, what):
        nonlocal fails
        print(('PASS' if ok else 'FAIL') + '  ' + what)
        fails += 0 if ok else 1
    files = list(py_files(root))
    b = rsc_readers(root, files)
    n = sum(reads_records(open(p, encoding='utf-8', errors='replace').read()) for p in files)
    report(not b and n > 5, 'rsc_readers: %d python tools read scene record bytes, %d without a version test%s' % (n, len(b), (': ' + ', '.join(b)) if b else ''))
    m, nv = estimate(root)
    report(not m and nv > 20, 'estimate: the case ring estimate names %d SceneTables vectors, missing %s' % (nv, m))
    m2, nm = expand_names(root)
    report(not m2 and nm > 20, 'expand_names: CopyOtherTables names %d SceneTables members (4 rebuilt), missing %s' % (nm, m2))
    lo = live_order(root)
    report(not lo, 'live_order: message.record / message.d3_record are read only after their expansion%s' % ((': ' + '; '.join(lo)) if lo else ''))
    t = root_tmp(root)
    report(not t, 'no_root_tmp: no *.tmp file at the repo root%s' % ((': ' + ', '.join(t)) if t else ''))
    return fails


def selftest_real(root):
    import tempfile
    ok = True

    def twin(name, caught):
        nonlocal ok
        print(('PASS' if caught else 'FAIL') + '  planted twin: ' + name)
        ok = ok and caught
    hdr = open(os.path.join(root, 'src/renderer_scene/record_io.h'), encoding='utf-8').read()
    case = open(os.path.join(root, 'src/case/case_scene.cpp'), encoding='utf-8').read()
    cpp = open(os.path.join(root, 'src/renderer_scene/record_io.cpp'), encoding='utf-8').read().replace('\r\n', '\n')
    live = open(os.path.join(root, 'src/renderer_adapter/live.cpp'), encoding='utf-8').read()
    with tempfile.TemporaryDirectory() as d:
        blind = os.path.join(d, 'blind_reader.py')
        open(blind, 'w').write("import struct\nmagic, ver, n = struct.unpack_from('<III', raw, 0)\nif magic != 0x4E435352: raise ValueError('x')\n")
        sees = os.path.join(d, 'sees_version.py')
        open(sees, 'w').write("import struct\nmagic, ver, n = struct.unpack_from('<III', raw, 0)\nif magic != 0x4E435352 or ver != 1: raise ValueError('x')\n")
        rscb = os.path.join(d, 'rsc_blind.py')
        open(rscb, 'w').write("for p in cap.glob('frame_*.rsc'):\n    if b'crowd:floor:' in p.read_bytes():\n        pass\n")
        twin('a reader of the RSCN magic with no version test is flagged', rsc_readers(d, [blind]) != [] and rsc_readers(d, [sees]) == [])
        twin('a reader that searches .rsc bytes for crowd strings with no version test is flagged', rsc_readers(d, [rscb]) != [])
    twin('a case ring estimate without crowd_members is flagged', estimate(root, case_text=case.replace('r.crowd_members.size() * sizeof(rdr_scene::CrowdMember)', '0'), header_text=hdr)[0] == ['crowd_members'])
    twin('a CopyOtherTables without caster_mats is flagged', expand_names(root, cpp_text=cpp.replace('o.caster_mats.assign(', 'o.caster_mats_x.assign('), header_text=hdr)[0] == ['caster_mats'])
    a = 'if (message.d3_record && !message.d3_record->crowd_runs.empty()) message.d3_record = rdr_scene::ExpandedForReading(message.d3_record);'
    twin('the d3 expansion moved below D3AddRecord is flagged', a in live and live_order(root, text=live.replace(a, '').replace('binder.D3AddRecord(message.d3_record);', 'binder.D3AddRecord(message.d3_record);\n      ' + a)) != [])
    twin('the unmodified live.cpp passes', live_order(root, text=live) == [])
    with tempfile.TemporaryDirectory() as d2:
        open(os.path.join(d2, 'out_cc-18e31a9b.o.tmp'), 'w').close()
        twin('a *.tmp file at the repo root is flagged', root_tmp(d2) == ['out_cc-18e31a9b.o.tmp'])
    return ok


if __name__ == '__main__':
    root = ROOT
    if '--root' in sys.argv:
        root = os.path.abspath(sys.argv[sys.argv.index('--root') + 1])
    if '--selftest' in sys.argv:
        sys.exit(0 if selftest_real(root) else 1)
    f = main_checks(root)
    sys.exit(1 if f else 0)
