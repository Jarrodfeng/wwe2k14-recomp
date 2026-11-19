"""qcensus_selftest: proves the S103 QCF work (`q ours census FIELD`, `q runlog field FIELD [--run RUN]`) on fixtures in a TEMP folder (no clang, no game, 0 runs).
  printf    the Nth %-spec after the field is the Nth argument (adjacent literals over two lines, `%%`, `*` widths, a PRIu64 macro); the counter is the member of the
            argument (`(unsigned long long)c.black_no_3d` -> black_no_3d) and every write is listed: ++x, x++, x += n, x = n, x[i]++, .fetch_add / .store.
  concat    `" field " + std::to_string(x.load(...))`, `+ n(c.f)`, `<< expr`.
  local     the printed expression is a local built from statements that read the real counter (depth 3): found through the local, marked `via local`.
  index     an array counter whose subscript names a member (`pic_why[q.why]`): the writers of that member inside functions that name its type are listed
            (the Why::Size path), and a same-named member written in a function that does NOT name the type is not.
  name      a field that no argument resolves for falls back to a name inside the statements after the literal, marked BY NAME (unconfirmed); a field that is not in any
            literal but is a counter name lists its writers (and says so).
  PLANTED   (stale twins) a counter RENAMED (format now prints c.holes_new while an old `++o.holes` of an unrelated struct and a commented-out old format line stay):
            the census names holes_new's writers and NOT the old holes / holes_old lines; a longer name (`black_no_3d_old`), a comment, and a string literal naming the
            counter are never writers; a #define literal is reported as unexpanded.
  runlog    `field_values` pulls per-window values (a number with its parenthesis / `of N`, a [bracket list], the rest of the line for a [tag] field).
Run:  py -I tools/q/qcensus_selftest.py
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
Q = os.path.join(HERE, 'q.py')
ENV = dict(os.environ, WWE_Q_OURS_NO_AUTOUPDATE='1', PYTHONIOENCODING='utf-8')


class Fail(Exception):
    pass


def check(cond, what, out=''):
    if not cond:
        raise Fail(f'{what}\n--- output ---\n{str(out)[-2500:]}')
    print(f'PASS  {what}')


def write(root, rel, text):
    p = os.path.join(root, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)


def census(root, *a):
    r = subprocess.run([sys.executable, '-I', Q, 'ours', 'census'] + list(a) + ['--root', root, '--json'], capture_output=True, text=True, env=ENV, timeout=120,
                       errors='replace')
    try:
        return json.loads(r.stdout), r.stdout
    except ValueError:
        raise Fail(f'census {a}: not json\n{r.stdout[-800:]}\n{r.stderr[-800:]}')


def writer_lines(c):
    return {(w['file'], w['line']) for w in c['writers']}


FIX_HOST = '''#pragma once
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <string>
struct Counters {
  uint64_t frames = 0, black_no_3d = 0, black_no_3d_old = 0, strikes = 0;   // black_no_3d_old is a longer name, never a write of black_no_3d
  uint64_t never_fed_bit[32] = {};
  uint64_t holes_new = 0;
};
inline std::atomic<uint64_t> g_bits[8] = {};
inline std::atomic<uint64_t> miss_{0};
inline void Describe(const Counters& c, char* b, size_t n) {
  std::snprintf(b, n, "frames %llu, 100%% done, black_no_3d %llu (mid %llu), width %*d, strikes %llu", (unsigned long long)c.frames, (unsigned long long)c.black_no_3d,
                (unsigned long long)c.strikes, 4, 7, (unsigned long long)c.strikes);
  std::snprintf(b, n, "x %" PRIu64 ", "
                      "holes %llu", c.frames, c.holes_new);
  // old: std::snprintf(b, n, "holes %llu", (unsigned long long)c.holes_old);
}
'''

FIX_WRITERS = '''#include "host.h"
void Decide(Counters& c, int b, bool z) {
  ++c.black_no_3d;
  c.black_no_3d += 2;
  c.black_no_3d_old++;                // the longer name: not a write of black_no_3d
  // ++c.black_no_3d;                 // a comment
  const char* s = "++c.black_no_3d";  // a string
  if (z) c.never_fed_bit[b]++;
  c.holes_new = 5;
  if (c.frames == c.black_no_3d) c.strikes = c.frames;   // == is a read
}
void Stamp(int b) { g_bits[b].fetch_add(1, std::memory_order_relaxed); miss_.store(0); }
'''

FIX_OLD = '''struct OldCounters { int holes = 0; };
void OldCode(OldCounters& o) { ++o.holes; }   // the OLD counter of an unrelated struct: the format no longer prints it
'''

FIX_CONCAT = '''#include "host.h"
std::string Line(const Counters& c) {
  std::string d;
  for (int b = 0; b < 8; ++b) {
    const uint64_t a = g_bits[b].load(std::memory_order_relaxed);
    if (a) d += " bit " + std::to_string(b) + ":" + std::to_string((unsigned long long)a);
  }
  std::string s = "[t] shown_without_row " + std::to_string((unsigned long long)miss_.load(std::memory_order_relaxed)) + ", strikes " + std::to_string(c.strikes);
  s += " fed " + std::to_string(c.never_fed_bit[0]);
  return "[t] decision:" + (d.empty() ? std::string(" -") : d) + s;
}
'''

FIX_INDEX = '''#include <cstdint>
#include <string>
struct Obs { uint8_t why = 0; uint32_t key = 0; };
struct Stats { uint64_t pic_why[9] = {}; };
struct Reader { Obs Read(int w); };
Obs ReadPic(int w) {
  Obs o;
  o.why = uint8_t(w);
  if (w > 5) o.why = uint8_t(7);   // the Size path
  return o;
}
struct Unrelated { int why = 0; };
void Other(Unrelated& u) { u.why = 3; }   // a member of the same NAME in a function that never names Obs
void Feed(Stats& st, const Obs& q) { ++st.pic_why[q.why < 9 ? q.why : 8]; }
std::string Census(const Stats& l) {
  std::string out;
  out += " pic_why [";
  for (int i = 0; i < 9; ++i) out += std::to_string((unsigned long long)l.pic_why[i]);
  return out;
}
'''

FIX_VF = '''#include <cstdio>
#include <cstdint>
struct Frame { struct { unsigned count = 0; } ops; };
void Dump(unsigned frame_index, const Frame& frame) {
  unsigned kinds[5] = {};
  for (int i = 0; i < 5; ++i) ++kinds[i];
  std::printf("[fview] frame %u: %u ops (%u targets, %u clears, %u draws, %u copies, %u presents)\\n", frame_index, frame.ops.count, kinds[0], kinds[1], kinds[2], kinds[3], kinds[4]);
}
void Dump2() {
  unsigned kinds[2] = {};
  kinds[1] = 9;       // another function's `kinds`: never a writer of Dump's
}
struct Window { unsigned long long frames = 0, drawn = 0, kept_frames = 0, draws = 0; };
void Time(const Window& window, char* line, size_t n, double seconds) {
  std::snprintf(line, n,
                "[rdr][live][time] %llu frames handled (%llu drawn, %llu skipped with lasting work) in %.2f s (%.0f drawn a second): ms a "
                "frame: parse %.3f",
                static_cast<unsigned long long>(window.frames), static_cast<unsigned long long>(window.drawn),
                static_cast<unsigned long long>(window.kept_frames), seconds, seconds > 0 ? double(window.drawn) / seconds : 0.0, 0.0);
}
void Tick(Window& w) { ++w.frames; ++w.drawn; w.kept_frames += 2; }
'''

FIX_LOC = '''#include <string>
struct Tr {
  unsigned long long hole_total_ = 0;
  void Add(unsigned long long pending) { hole_total_ += pending; }
  std::string Line() { return " hole_total " + std::to_string(hole_total_); }   // a member: never expanded into `pending`
};
std::string Loc(unsigned long long alpha_, unsigned long long beta_) {
  std::string s;
  {
    unsigned long long nn = 0;
    nn += alpha_;                       // a sibling block's local of the same name
  }
  {
    unsigned long long nn = 0;
    ++nn;
    s += " nnfield " + std::to_string(nn);
    nn += beta_;                        // after the site
  }
  return s;
}
void OtherNn() { int nn = 0; ++nn; }    // another function's nn: not a writer
unsigned long long g_early = 0, g_late = 0;
std::string Loc2() {
  unsigned long long v = 0;
  v = g_early;
  std::string s = " vfield " + std::to_string(v);
  v = g_late;                            // after the site: not read
  return s;
}
'''

FIX_MISC = '''#include <cstdio>
#define LOGF(x) std::printf("mac_field %d", x)
void Args(char* b) {
  const char* fmt = "elsewhere %d";
  std::printf("later_field %d\\n");
}
constexpr int kThing = 4;
'''


def test_all(d):
    write(d, 'src/host.h', FIX_HOST)
    write(d, 'src/writers.cpp', FIX_WRITERS)
    write(d, 'src/old.cpp', FIX_OLD)
    write(d, 'src/line.cpp', FIX_CONCAT)
    write(d, 'src/index.cpp', FIX_INDEX)
    write(d, 'src/misc.cpp', FIX_MISC)
    write(d, 'src/vf.cpp', FIX_VF)
    write(d, 'src/loc.cpp', FIX_LOC)

    # ---- printf: spec counting with %% and a `*` width before the field's spec, the member of a cast argument, every write
    r, raw = census(d, 'black_no_3d')
    s = r['sites'][0]
    check(s['file'] == 'src/host.h' and s['arg'] == '(unsigned long long)c.black_no_3d', 'printf: the 2nd arg of the format is the field (`%%` not counted)', raw)
    c = s['counters'][0]
    check(c['name'] == 'black_no_3d' and 'member' in c['how'], 'printf: the counter is the member black_no_3d', raw)
    wl = sorted(w['line'] for w in c['writers'])
    check(wl == [3, 4], '++c.black_no_3d and c.black_no_3d += 2 are the writers (the == read, the comment, the string and black_no_3d_old are not)', raw)
    check(all(w['func'] == 'Decide' for w in c['writers']), 'the enclosing function is named (Decide)', raw)
    check(c['decls'] and c['decls'][0]['file'] == 'src/host.h', 'the declaration is listed', raw)

    # ---- a spec after a `*` width: strikes is the arg after the two width/value ints
    r, raw = census(d, 'strikes')
    sites = [x for x in r['sites'] if x['file'] == 'src/host.h' and x['arg']]
    check(sites and sites[0]['arg'] == '(unsigned long long)c.strikes', 'printf: `%*d` counts two arguments before the next spec', raw)

    # ---- renamed counter: the format now prints c.holes_new; the old counter / commented-out format must not resolve
    r, raw = census(d, 'holes')
    check(len(r['sites']) == 1 and r['sites'][0]['arg'] == 'c.holes_new', 'planted twin: adjacent literals over two lines + PRIu64: the field is arg 2, the commented-out old format is not a site', raw)
    c = r['sites'][0]['counters'][0]
    check(c['name'] == 'holes_new', 'planted twin: the counter is the NEW name holes_new', raw)
    check({(w['file'], w['line']) for w in c['writers']} == {('src/writers.cpp', 9)}, 'planted twin: only holes_new\'s write is listed, the old `++o.holes` is not', raw)

    # ---- concat
    r, raw = census(d, 'shown_without_row')
    c = r['sites'][0]['counters'][0]
    check(c['name'] == 'miss_' and {w['kind'] for w in c['writers']} == {'store'} and [w['line'] for w in c['writers']] == [12], 'concat: `+ to_string(x.load(...))` -> miss_ and its .store', raw)
    r, raw = census(d, 'fed')
    c = r['sites'][0]['counters'][0]
    check(c['name'] == 'never_fed_bit' and [w['line'] for w in c['writers']] == [8], 'concat: an array element -> the array, its x[i]++ writer', raw)

    # ---- via a local
    r, raw = census(d, 'decision')
    c = r['sites'][0]['counters'][0]
    check(c['name'] == 'g_bits' and 'via local' in c['how'] and [w['line'] for w in c['writers']] == [12], 'local: `d` is built from `a` which reads g_bits: found through the locals', raw)

    # ---- the index feeder
    r, raw = census(d, 'pic_why')
    s = r['sites'][0]
    c = s['counters'][0]
    check(c['name'] == 'pic_why' and 'BY NAME' in c['how'], 'name: a field whose value is printed by a loop on the next line is found by name and marked unconfirmed', raw)
    check([w['line'] for w in c['writers']] == [14], 'index: the pic_why write', raw)
    fd = c['feeders'][0]
    check(fd['member'] == 'why' and fd['type'] == 'Obs', 'index: the subscript q.why is a member of the type Obs', raw)
    check(sorted(x['line'] for x in fd['writers']) == [8, 9], 'index: o.why = ... (both paths) listed; the same-named member of Unrelated is NOT', raw)

    # ---- no literal: the name itself; a constant; a #define; unresolved args
    r, raw = census(d, 'holes_new', '--in', 'writers')
    check(r['sites'] and r['sites'][0]['file'] == '(no format literal)' or r['sites'][0]['arg'], 'a counter name given as the field still lists its writers', raw)
    r, raw = census(d, 'kThing')
    check(r['sites'][0]['counters'][0]['decls'] and not r['sites'][0]['counters'][0]['writers'], 'a constant: declared, no writers', raw)
    r, raw = census(d, 'mac_field')
    check(r['sites'][0]['notes'] and 'LOGF' in r['sites'][0]['notes'][0], 'a literal inside a #define is reported as not expanded, naming the macro', raw)
    r, raw = census(d, 'later_field')
    check(not r['sites'][0]['counters'] and r['sites'][0]['notes'], 'a format with no argument list: unresolved, with a note, never a guess', raw)
    r, raw = census(d, 'no_such_field_anywhere')
    check(r['sites'] == [], 'an unknown field: no sites', raw)

    # ---- B1: value-first formats (the real fview / live.cpp lines): the spec BEFORE the field is its value; an ambiguous one is UNCONFIRMED, never "traced"
    r, raw = census(d, 'targets')
    s0 = [x for x in r['sites'] if x['file'] == 'src/vf.cpp'][0]
    check(s0['confirmed'] and s0['arg'] == 'kinds[0]' and 'value-first' in s0['how'], 'B1 twin (fview): `%u targets` is value-first: kinds[0], not the next spec (kinds[1])', raw)
    c = s0['counters'][0]
    check(c['name'] == 'kinds' and [w['line'] for w in c['writers']] == [6], 'B2 twin: a subscripted local array is a local: only its own function\'s write, not Dump2\'s kinds[1] = 9', raw)
    r, raw = census(d, 'ops')
    s0 = [x for x in r['sites'] if x['file'] == 'src/vf.cpp'][0]
    check(not s0['confirmed'] and any('UNCONFIRMED' in n for n in s0['notes']), 'B1 twin (fview): `%u ops (%u targets` is ambiguous: marked UNCONFIRMED', raw)
    out = subprocess.run([sys.executable, '-I', Q, 'ours', 'census', 'ops', '--root', d], capture_output=True, text=True, env=ENV, timeout=120, errors='replace').stdout
    check('candidate (UNCONFIRMED' in out and 'prints:' not in out.split('src/vf.cpp')[1].split('[2]')[0], 'B1: an ambiguous site prints "candidate (UNCONFIRMED", never "prints:"', out)
    for fld, want in (('drawn', 'window.drawn'), ('frames', 'window.frames'), ('skipped', 'window.kept_frames')):
        r, raw = census(d, fld)
        ss = [x for x in r['sites'] if x['file'] == 'src/vf.cpp' and 'live][time]' in x['format']]
        check(ss and ss[0]['confirmed'] and want in ss[0]['arg'] and 'value-first' in ss[0]['how'], f'B1 twin (live.cpp): `%llu {fld}` is value-first -> {want}', raw)
    r, raw = census(d, 'skipped')
    ss = [x for x in r['sites'] if x['file'] == 'src/vf.cpp'][0]
    check([w['text'] for w in ss['counters'][0]['writers']] == ['void Tick(Window& w) { ++w.frames; ++w.drawn; w.kept_frames += 2; }'] or ss['counters'][0]['name'] == 'kept_frames', 'B1: the counter behind `%llu skipped` is kept_frames', raw)

    # ---- B2: a member is never expanded; a local only through statements before the site, declared in a block holding it
    r, raw = census(d, 'hole_total')
    c = r['sites'][0]['counters']
    check([x['name'] for x in c] == ['hole_total_'] and 'via local' not in c[0]['how'] and c[0]['writers'] and 'pending' in c[0]['writers'][0]['text'], 'B2 twin: `hole_total_ += pending; printf hole_total_` -> the member itself, NOT `pending`', raw)
    r, raw = census(d, 'nnfield')
    c = r['sites'][0]['counters']
    check([x['name'] for x in c] == ['nn'] and 'local' in c[0]['how'], 'B2 twin: a local of the innermost block: counter nn, no alpha_ (sibling block) / beta_ (after the site)', raw)
    check({w['text'] for w in c[0]['writers']} <= {'nn += alpha_;                       // a sibling block\'s local of the same name', '++nn;', 'nn += beta_;                        // after the site'.split('//')[0].strip() + '                        // after the site'} or len(c[0]['writers']) == 3, 'B2: nn\'s writers are limited to Loc (OtherNn\'s ++nn is not listed)', raw)
    check(all(w['func'] == 'Loc' for w in c[0]['writers']) and c[0]['writers'], 'B2: every listed writer of the local is inside its own function', raw)
    r, raw = census(d, 'vfield')
    c = r['sites'][0]['counters']
    check([x['name'] for x in c] == ['g_early'] and 'via local v' in c[0]['how'], 'B2 twin: the local v is expanded through the statement BEFORE the site only (g_early, not g_late)', raw)

    # ---- text output prints the chain
    out = subprocess.run([sys.executable, '-I', Q, 'ours', 'census', 'black_no_3d', '--root', d], capture_output=True, text=True, env=ENV, timeout=120, errors='replace').stdout
    check('format:' in out and 'prints:' in out and 'counter: black_no_3d' in out and 'writers (2)' in out, 'the printout shows field -> format -> counter -> writers', out)


def test_runlog(d):
    import qrunlog
    p = os.path.join(d, 'wwe2k14_999.log')
    lines = [
        '[2026-11-19 08:21:02.391] [info] [core] [t1] [host] frames 510, black_no_3d 0 (mid_match 0), ticks_dropped 2998 of 3508, holes 0',
        '[2026-11-19 08:21:03.001] [info] [core] [t1] [host] frames 600, black_no_3d 0 (mid_match 0), ticks_dropped 2998 of 3598, holes 0',
        '[2026-11-19 08:21:04.001] [info] [core] [t1] [host] frames 700, black_no_3d 12 (mid_match 3), ticks_dropped 2998 of 3698, holes 0',
        '[2026-11-19 08:21:05.001] [info] [core] [t1] [scene][live] frames 0-600: pic_why [ok 5 size 2], holes 7',
        '[2026-11-19 08:21:06.001] [info] [core] [t1] [scene][ours_missing] decision: bit 9:84; record: -',
        '[2026-11-19 08:21:07.001] [info] [core] [t1] [host] black_no_3d_old 4 and a long unrelated line',
    ]
    with open(p, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    r = qrunlog.field_values('black_no_3d', path=p)
    check([x['value'] for x in r['rows']] == ['0 (mid_match 0)', '0 (mid_match 0)', '12 (mid_match 3)'] and [x['window'] for x in r['rows']] == ['510', '600', '700'],
          'runlog field: a number with its parenthesis per window; black_no_3d_old is not the field', r)
    r = qrunlog.field_values('ticks_dropped', path=p)
    check(r['rows'][0]['value'] == '2998 of 3508', 'runlog field: `N of M` kept whole', r)
    r = qrunlog.field_values('pic_why', path=p)
    check(r['rows'][0]['value'] == '[ok 5 size 2]' and r['rows'][0]['window'] == '0-600', 'runlog field: a [bracket list] and the frame window', r)
    r = qrunlog.field_values('ours_missing', path=p)
    check(r['rows'][0]['value'].startswith('decision: bit 9:84'), 'runlog field: a field that is part of the [tag] takes the rest of the line', r)
    r = qrunlog.field_values('holes', path=p)
    check([x['value'] for x in r['rows']] == ['0', '0', '0', '7'], 'runlog field: a plain number', r)
    # B4: RUN only via --run (a number among the words belongs to the field); the CLI reads the log path
    r = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'q.py'), 'runlog', 'field', 'holes', '--run', p], capture_output=True, text=True, env=ENV, timeout=120, errors='replace')
    check('4 line(s)' in r.stdout and 'wwe2k14_999.log' in r.stdout, 'B4: `q runlog field holes --run PATH` reads that log', r.stdout + r.stderr)
    import qcensus
    check(qcensus.type_of_var('const Obs& q = z; x = a * q;', 'q') == 'Obs' and qcensus.type_of_var('y = a * q;', 'q') is None and qcensus.type_of_var('return q;', 'q') is None,
          'B4: type_of_var accepts declarations only (`a * q`, `return q` are not)', '')
    check([m.group(3) for m in qcensus.SPEC_RX.finditer('%o %a %A %n %Lf %lu')] == ['o', 'a', 'A', 'n', 'f', 'u'], 'B4: %o %a %A %n %L are specs', '')
    # the printout collapses identical values and shows the delta
    import io
    import contextlib
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        qrunlog.print_field_values(qrunlog.field_values('black_no_3d', path=p))
    out = buf.getvalue()
    check('(+12)' in out and 'same value' not in out or '(... ' in out, 'runlog field: the printout shows the change between windows', out)


def main():
    d = tempfile.mkdtemp(prefix='qcensus_selftest_')
    try:
        os.makedirs(os.path.join(d, 'fx'))
        test_all(os.path.join(d, 'fx'))
        test_runlog(d)
    except Fail as e:
        print('FAIL ' + str(e))
        return 1
    finally:
        shutil.rmtree(d, ignore_errors=True)
    print('qcensus_selftest OK')
    return 0


if __name__ == '__main__':
    sys.exit(main())
