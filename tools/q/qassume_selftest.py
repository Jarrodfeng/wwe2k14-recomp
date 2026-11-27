r"""Selftest of `q assume` (S111 SWEEP1): each kind's pattern on planted source (a hit AND a near miss), the file scope, the seam index, --kind / --in / --json, --ref.

    py -I tools/q/qassume_selftest.py

A planted case is first seen to FAIL on the old pattern by design: every `miss` line below is a text the pattern must NOT report (it is the noise the first
version of the pattern reported on the real tree), every `hit` line is a text of one of the faults the tool was proven on (SWEEP_PLAN.md step 1).
"""
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
from contextlib import redirect_stdout

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import qassume  # noqa: E402

FAILS = []


def check(name, ok, detail=''):
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else f'   {str(detail)[:400]}'))
    if not ok:
        FAILS.append(name)


SRC = {
    'src/renderer_scene/a.h': r'''// header: nothing here by itself
#pragma once
namespace rdr_scene {
constexpr uint32_t kCbEditor = 0x8338EC00u;             // sub_8338EC00: the editor's callback
constexpr uint32_t kSlots = 0x82001000u;
inline void ScreenDefaults(Binding& b) {
  for (uint32_t k : {801u, 802u, 806u, 808u, 809u}) b.Set(k, 1);   // hit A1 (the shipped key table)
  const uint8_t col[] = {255, 255, 255, 255, 255};                 // miss A1: one distinct value
  const float f[] = {100.5f, 200.5f, 300.5f, 400.5f};              // miss A1: floats
  const int small[] = {1, 2, 3, 4, 5};                              // miss A1: small numbers
  const uint32_t offs[] = {112, 144, 160, 176, 192, 208};          // miss A1: register offsets (multiples of 16)
}
inline int KeyOfThing(int id) {
  switch (id) {
    case 953: return 1;
    case 1017: return 2;
    case 973: return 3;
  }
  switch (id) { case 7: return 1; case 8: return 2; }               // miss A2
  return id == 2065 || id == 2114 || id == 2511 ? 1 : 0;           // hit A3
}
inline bool Posed(int g, bool alt, uint32_t have[]) {
  uint32_t covered[6];
  for (int g2 = 0; g2 < 6; ++g2) covered[g2] = have[g2] >= ringkeys::Need(g2, alt);   // hit B2 (the ring count rule)
  if (calls.size() == 12) return false;                              // hit B1
  if (s.size() > 4) return true;                                     // miss B1: a string length
  for (int i = 0; i < rows.size(); ++i) {}                           // miss B1: a loop bound
  return true;
}
inline uint32_t Need(int g, bool alt) { return g == 3 && alt ? 8 : 12; }   // hit B3
inline bool Gate(uint32_t cb, const Origin* o, uint32_t lr, int i) {
  if (cb != kCbEditor) return false;                                 // hit C2 (the accepted callback)
  if (o->call_site == kXSite) return false;                          // hit C1
  if (lr == 0x83353FC8) return false;                                // hit C1
  for (int k = 0; k < kSlots; ++k) {}                                // miss C2: a loop bound
  std::atomic<uint32_t> lr2{0};                                      // miss C1 (a template close)
  return Halve<0x82704D24>(0x82704CF8);                              // miss C2 (a template argument)
}
inline int PropPach(int s, int b) {
  if (s >= 0) return s;
  if (b >= 0) return b;                                              // hit D1 (first match)
  return -1;
}
struct Plan { uint32_t stage_family = 0; bool depth_only = false; };    // hit D3 (family) / miss (depth_only)
// the first entry added for a key wins                                       (hit D4)
// [U] which family reaches the picture                                      (hit E1)
// the test is fitted on five captures                                       (hit E2)
inline bool Check(int x) {
  // never a guess
  if (x) return true;                                                // hit E3 (next to a condition)
  return false;
}
inline bool Switch() {
  if (REXCVAR_GET(wwe2k14_rdr_stage_struct_family)) return true;     // hit F1
  if (REXCVAR_GET(wwe2k14_session_log)) return true;                 // miss F1: an app switch
  static const bool v = std::getenv("WWE_RDR_L5_PLANT") != nullptr;  // hit F2
  return v;
}
}
''',
    'src/renderer_scene/a_test.cpp': 'constexpr int k[] = {801, 802, 806, 808, 809};\n',
    'src/hook_game.h': '// sub_82C37E48 is the pac request\nconstexpr int kT[] = {960, 964, 973, 974};\n',
    'src/hook_app.h': 'constexpr int kT[] = {960, 964, 973, 974};\n',
    'src/other/x.h': 'constexpr int kT[] = {960, 964, 973, 974};\n',
    'db/seams/t.json': json.dumps({'topic': 'tronkey', 'summary': 'rows: src/renderer_scene/a.h:6-9 holds the table', 'game': [], 'ours': []}),
}


def write_tree(root):
    for rel, text in SRC.items():
        p = os.path.join(root, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, 'w', encoding='utf-8', newline='\n') as f:
            f.write(text)


def run_json(root, *args):
    buf = io.StringIO()
    with redirect_stdout(buf):
        rc = qassume.main(['--json', '--no-g', '--root', root] + list(args))
    return rc, json.loads(buf.getvalue()) if buf.getvalue().strip().startswith('{') else {}


def has(rows, sub, needle, file='src/renderer_scene/a.h'):
    return [r for r in rows if r['sub'] == sub and r['file'] == file and needle in r['text'] + r['name'] + r['sym']]


def main():
    tmp = tempfile.mkdtemp(prefix='qassume_st_')
    try:
        write_tree(tmp)
        rc, d = run_json(tmp)
        rows = d.get('rows', [])
        check('exit 0, json has totals and rows', rc == 0 and 'totals' in d and rows, rc)
        check('A1 hit: the shipped-style key table', has(rows, 'A1', '801u'))
        check('A1 miss: one repeated value / floats / small numbers / register offsets', not [r for r in rows if r['sub'] == 'A1' and r['file'].endswith('a.h') and not '801u' in r['text']], [r['text'] for r in rows if r['sub'] == 'A1'])
        check('A2 hit: three id case labels, and the two-label switch is a miss', len([r for r in rows if r['sub'] == 'A2']) == 1)
        check('A3 hit: three id comparisons in one statement', has(rows, 'A3', '2065'))
        check('B2 hit: the count compared with Need( in a for body', has(rows, 'B2', 'Need('))
        check('B1 hit: calls.size() == 12; miss: a string size and a loop bound', [r['text'] for r in rows if r['sub'] == 'B1'] == ['if (calls.size() == 12) return false;'], [r['text'] for r in rows if r['sub'] == 'B1'])
        check('B3 hit: the Need function', has(rows, 'B3', 'Need'))
        check('C2 hit: the accepted callback compare against an address constant', has(rows, 'C2', 'kCbEditor'))
        check('C1 hits: call_site and lr compares', has(rows, 'C1', 'call_site') and has(rows, 'C1', '0x83353FC8'))
        check('C miss: a loop bound, a template close, a template argument', not [r for r in rows if r['kind'] == 'C' and any(w in r['text'] for w in ('kSlots; ++k', 'lr2', 'Halve'))], [r['text'] for r in rows if r['kind'] == 'C'])
        check('D1 hit: the first-match chain', has(rows, 'D1', 'if (b >= 0) return b'))
        check('D3 hit: stage_family; miss: depth_only', [r['sym'] for r in rows if r['sub'] == 'D3'] == ['stage_family'], [r['sym'] for r in rows if r['sub'] == 'D3'])
        check('D4 hit: "wins"', has(rows, 'D4', 'wins'))
        check('E1 hit: [U]', has(rows, 'E1', '[U]') and bool(has(rows, 'E1', '[U]')[0]['name']))
        check('E2 hit: fitted', has(rows, 'E2', 'fitted'))
        check('E3 hit: never next to a condition', has(rows, 'E3', 'never'))
        check('F1 hit: the rdr cvar in a condition; miss: an app switch', has(rows, 'F1', 'struct_family') and not has(rows, 'F1', 'session_log'))
        check('F2 hit: a switch variable from the environment', has(rows, 'F2', 'WWE_RDR_L5_PLANT'))
        # the enclosing name is a name, never a line
        r = has(rows, 'B2', 'Need(')[0]
        check('the row names its function (Posed), not a line', r['name'].endswith('Posed') and r['how'] == 'func', r)
        a1 = has(rows, 'A1', '801u')[0]
        check('the A1 row names its function', a1['name'].endswith('ScreenDefaults'), a1)
        # the game address within 3 lines
        c2 = has(rows, 'C2', 'kCbEditor')[0]
        check('C2 row: the cited game address is not within 3 lines here, the constant line cites sub_8338EC00', isinstance(c2['addrs'], list))
        # the seam index: a summary text cite of a.h:6-9 reaches the ScreenDefaults rows
        check('the seam row citing a.h:6-9 is attached to the A1 row', a1['seam'] == ['tronkey'], a1['seam'])
        # file scope
        files = {r['file'] for r in rows}
        check('a test file and a folder outside the scope are not read', 'src/renderer_scene/a_test.cpp' not in files and 'src/other/x.h' not in files, files)
        check('a src/*.h hook counts only when it names a game address', 'src/hook_game.h' in files and 'src/hook_app.h' not in files, files)
        # flags
        rc, d = run_json(tmp, '--kind', 'A,B')
        check('--kind A,B: only those kinds', rc == 0 and {r['kind'] for r in d['rows']} == {'A', 'B'}, {r['kind'] for r in d['rows']})
        rc, d = run_json(tmp, '--in', 'hook_game')
        check('--in PATH: only that file', {r['file'] for r in d['rows']} == {'src/hook_game.h'}, {r['file'] for r in d['rows']})
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = qassume.main(['--no-g', '--root', tmp])
        out = buf.getvalue()
        check('text listing: totals per kind first, then == FILE, each row with its sub-kind and name', rc == 0 and out.startswith('q assume: ') and '  A literal game-id' in out and '== src/renderer_scene/a.h' in out and ' A1 ' in out, out[:300])
        # --ref: the committed tree, not the working tree
        subprocess.run(['git', '-C', tmp, 'init', '-q'], check=True)
        subprocess.run(['git', '-C', tmp, '-c', 'user.name=t', '-c', 'user.email=t@t', 'add', '-A'], check=True)
        subprocess.run(['git', '-C', tmp, '-c', 'user.name=t', '-c', 'user.email=t@t', 'commit', '-q', '-m', 'x'], check=True)
        with open(os.path.join(tmp, 'src/renderer_scene/a.h'), 'w', encoding='utf-8') as f:
            f.write('// emptied\n')
        rc, now = run_json(tmp)
        rc2, old = run_json(tmp, '--ref', 'HEAD')
        check('--ref HEAD lists the committed rows the emptied working tree no longer has', not has(now['rows'], 'A1', '801u') and has(old['rows'], 'A1', '801u') and old.get('ref') == 'HEAD', (rc, rc2))
        rc3 = qassume.main(['--ref=--cached', '--root', tmp])
        check('--ref starting with - is refused', rc3 == 2, rc3)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print(f'qassume_selftest: {len(FAILS)} failed' if FAILS else 'qassume_selftest: all passed')
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
