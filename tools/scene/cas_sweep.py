r"""S110 CASSWEEP: run the Create a Superstar go-to (--wwe2k14_cas_goto=<record>, src\cas_goto.h) over the editor's flow table and sort
the results into classes.

  py -I tools\scene\cas_sweep.py --list                       every record: id, tab, depth, code, gate word, design class, name path
  py -I tools\scene\cas_sweep.py --run 3,13,56 --batch pilot  one batch through tools\bench\run_list.ps1 (-Parallel, one run per record,
                                                              the route tools\input_scripts\scenes\s110_cas_goto_probe.txt), then parse
  py -I tools\scene\cas_sweep.py --run all-left --batch b2    every record not yet in a batch
  py -I tools\scene\cas_sweep.py --report                     rebuild sweep.tsv + summary.txt from every batch's json
  py -I tools\scene\cas_sweep.py --selftest                   the log parser on synthetic lines (exit 1 on a miss)

Options: --exe PATH (default: the pinned exe below; the whole sweep runs on one build), --parallel N (1..3, run_list's cap), --wait S
(run_list's per-run kill), --dump PATH (the flow dump, default out\s110\casentry\cas_flow_slot1.bin), --out DIR (default out\s110\cassweep).
Names come at run time from the player's own files through tools\menus\flow_graph.py's json (out\harness\atlas\flow_cas_flow_slot1.json);
this script holds no game text. Outputs stay under out\.

Each run is identified by its own log ('modified: wwe2k14_cas_goto = 'R''), never by list order. Result per record:
  arrived   'arrived at record R (page word ...' (a page) or 'arrived at record R: row ...' (a row: slider / DEFAULT / EDIT LAYERS / overlay)
  refused   'STOPPED: REFUSED: ...' (template chooser on the path) or a 'not a row' stop of a record whose +28 word is a superstar-bit
            word (6561 / 6562: the builder builds one of the pair)
  stopped   any other STOPPED line (class from its text), or no final cas_goto line (no_result); since S110 SCENESRC an item page's arrival
            waits for its parts panel (up to 900 frames from the page's settle), so a panel that never readies is not_settled:panel_*
"""
import argparse
import glob
import json
import os
import re
import struct
import subprocess
import sys
import time

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
EXE = os.path.join(ROOT, 'out', 'exe', 'tip-1483b8720', 'wwe2k14.exe')
ROUTE = os.path.join(ROOT, 'tools', 'input_scripts', 'scenes', 's110_cas_goto_probe.txt')
RUN_LIST = os.path.join(ROOT, 'tools', 'bench', 'run_list.ps1')
DUMP = os.path.join(ROOT, 'out', 's110', 'casentry', 'cas_flow_slot1.bin')
NAMES_JSON = os.path.join(ROOT, 'out', 'harness', 'atlas', 'flow_cas_flow_slot1.json')
OUT = os.path.join(ROOT, 'out', 's110', 'cassweep')
REC = 76
NO_PARENT = 0xFFFFFFFF
GATE_WORDS = (6561, 6562)          # src\cas_goto_core.h kGateBit0 / kGateBit1
TEMPLATE_CODES = (1003, 1008)      # src\cas_goto_core.h kCodeTemplate / kCodeTemplateGated


# ---- the table ----
def load_records(path):
    data = open(path, 'rb').read()
    out = {}
    for i in range(len(data) // REC):
        w = struct.unpack_from('>19I', data, REC * i)
        out[w[0]] = {'id': w[0], 'parent': w[1], 'menu': w[2], 'item': w[3] >> 16, 'label': w[6], 'gate': w[7], 'flags': w[10],
                     'code': w[11]}
    return out


def load_names(path=NAMES_JSON):
    try:
        g = json.load(open(path, encoding='utf-8'))
    except (OSError, ValueError):
        return {}
    return {i['record']: (i.get('label') or '?') for m in g.get('menus', []) for i in m.get('items', [])}


def path_of(recs, rid):
    p, r = [], recs.get(rid)
    while r is not None and len(p) < 64:
        p.insert(0, r)
        r = None if r['parent'] == NO_PARENT else recs.get(r['parent'])
    return p


def design(recs, rid):
    """What the go-to is built to do with record rid, from the table alone (src\\cas_goto_core.h RefusePath / Shown)."""
    p = path_of(recs, rid)
    if any(r['code'] in TEMPLATE_CODES for r in p[1:]):
        return 'refuse:template'
    if recs[rid]['gate'] in GATE_WORDS:
        return 'bit_pair'
    return 'drive'


def describe(recs, names, rid):
    p = path_of(recs, rid)
    r = recs[rid]
    return {'record': rid, 'tab': p[0]['item'] if p else -1, 'depth': len(p) - 1, 'code': r['code'] if r['flags'] & 0x10 else 1001,
            'gate': r['gate'], 'design': design(recs, rid), 'path': ' > '.join(names.get(x['id'], str(x['id'])) for x in p)}


# ---- one run's log ----
RX_TARGET = re.compile(r"modified: wwe2k14_cas_goto = '(\d+)'")
RX_EXIT = re.compile(r'@cas: exit 8 sent at frame (\d+)')
RX_FINAL = re.compile(r'\[wwe2k14\]\[cas_goto\] record (\d+): (arrived at record \d+.*|STOPPED: .*)$')
RX_FRAME = re.compile(r'\(frame (\d+)\)\s*$')
BUSY = [('the host\'s start has not ended', 'host_start'), ('no current page', 'no_page'), ('message flow', 'message'),
        ('tab change H+976', 'tab_change'), ('page transition', 'transition'), ('an overlay is open', 'overlay'),
        ('page result P+992', 'page_result'), ('page state P+16', 'page_state'),
        # S110 SCENESRC review: the parts panel's tests (src\cas_goto.h PanelNotReady), which an item page's arrival now waits for (and the way up's B)
        ('has built no panel yet', 'panel_build'), ('is not idle (+16', 'panel_idle'), ('is in a sub-mode', 'panel_submode'),
        ('B enable test', 'panel_enable'), ('a pad bit the parts panel', 'panel_pad'), ('part data is not ready', 'panel_parts'),
        # S110 CASREADY: every arrival then waits for the preview (src\cas_goto.h PreviewNotReady)
        ('the preview model is not ready', 'preview_model'), ("the game's loading screen is up", 'preview_loading')]


def classify(text, gate=0):
    """(result, class) for the final cas_goto text ('arrived ...' or 'STOPPED: ...'); gate = the target's +28 word."""
    if text.startswith('arrived'):
        if re.match(r'arrived at record \d+: row', text):
            k = re.search(r'row kind (\d+)', text)
            return 'arrived', 'row_kind%s' % (k.group(1) if k else '?')
        return 'arrived', 'page'
    s = text[len('STOPPED: '):]
    if s.startswith('REFUSED:'):
        return 'refused', 'template' if 'template chooser' in s else 'refused_other'
    if 'is not a row of page' in s:
        return ('refused', 'superstar_bit') if gate in GATE_WORDS else ('stopped', 'row_missing')
    if s.startswith('not settled'):
        for key, cls in BUSY:
            if key in s:
                return 'stopped', 'not_settled:' + cls
        return 'stopped', 'not_settled:other'
    table = [('the step settled on page', 'landed_elsewhere'), ('the tab step settled on tab', 'tab_refused'), ('tabs are off', 'tabs_off'),
             ('root page', 'tab_root_mismatch'), ('only a list\'s B is driven', 'not_list_up'), ('is not a list page', 'not_list_down'),
             ('opens no page, but the path goes on', 'row_no_page'), ('template pick did not end', 'template_pick'),
             ('preset page\'s enable test', 'template_preset'), ('template mode', 'template_other'), ('host went away', 'host_gone'),
             ('superstar object', 'no_superstar_obj'), ('flow table is not usable', 'flow_table')]
    for key, cls in table:
        if key in s:
            return 'stopped', cls
    return 'stopped', 'other'


def parse_log(text, gates=None):
    """{'record', 'result', 'class', 'line', 'exit_frame', 'end_frame', 'frames'} from one game log's text."""
    m = RX_TARGET.search(text)
    rid = int(m.group(1)) if m else None
    e = RX_EXIT.search(text)
    out = {'record': rid, 'result': 'stopped', 'class': 'no_result', 'line': '', 'exit_frame': int(e.group(1)) if e else None,
           'end_frame': None, 'frames': None}
    for ln in text.splitlines():
        f = RX_FINAL.search(ln)
        if not f:
            continue
        body = f.group(2)
        out['line'] = body
        out['result'], out['class'] = classify(body, (gates or {}).get(rid, 0))
        fr = RX_FRAME.search(body)
        out['end_frame'] = int(fr.group(1)) if fr else None
        break
    if out['exit_frame'] is not None and out['end_frame'] is not None:
        out['frames'] = out['end_frame'] - out['exit_frame']
    return out


# ---- a batch ----
def ps_quote(s):
    return "'" + s.replace("'", "''") + "'"


def run_batch(rids, batch, exe, parallel, wait, outdir):
    os.makedirs(os.path.join(outdir, 'batches'), exist_ok=True)
    scripts = ','.join(ps_quote(ROUTE) for _ in rids)
    extras = ','.join(ps_quote('--wwe2k14_cas_goto=%d' % r) for r in rids)
    cmd = "& %s -Scripts @(%s) -ScriptExtras @(%s) -Exe %s -Parallel %d -WaitSeconds %d" % (
        ps_quote(RUN_LIST), scripts, extras, ps_quote(exe), parallel, wait)
    hard = 180 + (wait + 30) * ((len(rids) + parallel - 1) // parallel)
    hard = min(hard, 120 + 120 * ((len(rids) + parallel - 1) // parallel))   # a run takes ~10-60 s: 2 min a wave is generous
    logf = os.path.join(outdir, 'batches', batch + '.runlist.txt')
    t0 = time.time()
    with open(logf, 'w', encoding='utf-8') as fh:
        fh.write('# %s\n# records %s\n# hard timeout %d s\n' % (cmd, ','.join(map(str, rids)), hard))
        fh.flush()
        try:
            p = subprocess.run(['pwsh', '-NoProfile', '-NonInteractive', '-Command', cmd], cwd=ROOT, stdout=fh, stderr=subprocess.STDOUT,
                               timeout=hard)
            code = p.returncode
        except subprocess.TimeoutExpired:
            fh.write('\n# HARD TIMEOUT after %d s: run_list killed\n' % hard)
            code = 'timeout'
    wall = time.time() - t0
    text = open(logf, encoding='utf-8', errors='replace').read()
    logs = re.findall(r'-> (wwe2k14_\d+)', text)
    exedir = os.path.dirname(exe)
    recs = load_records(DUMP)
    gates = {k: v['gate'] for k, v in recs.items()}
    rows = []
    seen = set()
    for lg in logs:
        lp = os.path.join(exedir, 'logs', lg + '.log')
        if not os.path.exists(lp):
            continue
        r = parse_log(open(lp, encoding='utf-8', errors='replace').read(), gates)
        num = lg.split('_')[1]
        sheets = glob.glob(os.path.join(exedir, 'captures', '%s_p*' % num, '%s__sheet.jpg' % num))
        shots = sorted(glob.glob(os.path.join(exedir, 'captures', '%s_sc_s110_cas_goto_*.png' % num)))
        r.update({'log': lp, 'sheet': sheets[0] if sheets else '', 'shots': shots, 'batch': batch})
        rows.append(r)
        seen.add(r['record'])
    for rid in rids:
        if rid not in seen:
            rows.append({'record': rid, 'result': 'stopped', 'class': 'no_log', 'line': '', 'exit_frame': None, 'end_frame': None,
                         'frames': None, 'log': '', 'sheet': '', 'shots': [], 'batch': batch})
    json.dump({'batch': batch, 'exe': exe, 'records': rids, 'runlist_exit': code, 'wall_s': round(wall, 1), 'rows': rows},
              open(os.path.join(outdir, 'batches', batch + '.json'), 'w', encoding='utf-8'), indent=1)
    print('batch %s: %d records, %d logs, run_list exit %s, wall %.0f s' % (batch, len(rids), len(logs), code, wall))
    return rows


# ---- the report ----
def all_rows(outdir):
    best = {}
    for f in sorted(glob.glob(os.path.join(outdir, 'batches', '*.json')), key=os.path.getmtime):
        for r in json.load(open(f, encoding='utf-8'))['rows']:
            if r['record'] is not None:
                best[r['record']] = r   # the newest batch wins
    return best


def report(outdir, dump):
    recs = load_records(dump)
    names = load_names()
    rows = all_rows(outdir)
    cols = ['record', 'tab', 'depth', 'code', 'design', 'path', 'result', 'class', 'frames', 'line', 'log', 'sheet']
    lines = ['\t'.join(cols)]
    for rid in sorted(recs):
        d = describe(recs, names, rid)
        r = rows.get(rid, {})
        v = dict(d, result=r.get('result', 'not_run'), **{'class': r.get('class', ''), 'frames': '' if r.get('frames') is None else r['frames'],
                                                          'line': (r.get('line') or '').replace('\t', ' '), 'log': r.get('log', ''),
                                                          'sheet': r.get('sheet', '')})
        lines.append('\t'.join(str(v[c]) for c in cols))
    open(os.path.join(outdir, 'sweep.tsv'), 'w', encoding='utf-8').write('\n'.join(lines) + '\n')
    by = {}
    for rid in sorted(recs):
        r = rows.get(rid)
        key = (r['result'], r['class']) if r else ('not_run', '')
        by.setdefault(key, []).append(rid)
    out = ['records %d, run %d' % (len(recs), len(rows))]
    for (res, cls), ids in sorted(by.items()):
        out.append('%-8s %-28s %3d  e.g. %s' % (res, cls, len(ids), ', '.join(map(str, ids[:12])) + (' ...' if len(ids) > 12 else '')))
    fr = [r['frames'] for r in rows.values() if r.get('result') == 'arrived' and r.get('frames') is not None]
    if fr:
        out.append('frames exit -> arrival: min %d, median %d, max %d' % (min(fr), sorted(fr)[len(fr) // 2], max(fr)))
    open(os.path.join(outdir, 'summary.txt'), 'w', encoding='utf-8').write('\n'.join(out) + '\n')
    print('\n'.join(out))


# ---- selftest ----
def selftest():
    bad = 0
    log = '\n'.join([
        "[x] [wwe2k14] modified: wwe2k14_cas_goto = '76' (source: command line)",
        "[x] [wwe2k14][launch] @cas: exit 8 sent at frame 3324 (0x83929434 = 8; free superstar slot 0)",
        "[x] [wwe2k14][cas_goto] record 76: tab 2, path 2 (label 6361) > 21 (label 6509) > 76 (label 6396) (frame 3986)",
        "[x] [wwe2k14][cas_goto] record 76: arrived at record 76 (page word 821BBAC8; page A6D3D260, label 6396, tab 2, cursor row 0) (frame 4328)"])
    r = parse_log(log)
    want = {'record': 76, 'result': 'arrived', 'class': 'page', 'exit_frame': 3324, 'end_frame': 4328, 'frames': 1004}
    for k, v in want.items():
        if r[k] != v:
            print('FAIL arrived log: %s = %r, want %r' % (k, r[k], v)); bad += 1
    cases = [
        ('arrived at record 13: row 1 of page A0 (page word 821BB310), cursor on it; row kind 1: it opens an overlay (frame 9)', 0, ('arrived', 'row_kind1')),
        ('STOPPED: REFUSED: record 23 (label 6593, code 1003) is a template chooser: entering it ...; current page 0 (vtable 0, label 0, depth 0, parent 0); template mode 1 (frame 5)', 0, ('refused', 'template')),
        ('STOPPED: record 77 (label 6396, +28 word 6562) is not a row of page A0 (7 rows, superstar bit 0; ...); current page ... (frame 5)', 6562, ('refused', 'superstar_bit')),
        ('STOPPED: record 4 (label 6354, +28 word 0) is not a row of page A0 (8 rows, superstar bit 0; ...) (frame 5)', 0, ('stopped', 'row_missing')),
        ('STOPPED: not settled 900 frames after the last step: an overlay is open (H+952 1, H+956 0, H+1004 0) (page A0); current ... (frame 5)', 0, ('stopped', 'not_settled:overlay')),
        ('STOPPED: not settled 900 frames after the last step: page state P+16 = 1 (the host waits for 2) (page A0) (frame 5)', 0, ('stopped', 'not_settled:page_state')),
        ('STOPPED: the step settled on page A0 (label 1), not the expected page B0 (the game refused ...) (frame 5)', 0, ('stopped', 'landed_elsewhere')),
        ('STOPPED: page A0 of record 21 is not a list page (vtable 821BBBA0); current ... (frame 5)', 0, ('stopped', 'not_list_down')),
        ('STOPPED: the current page A0 (vtable 821BBBA0) is not a list page; only a list\'s B is driven; current ... (frame 5)', 0, ('stopped', 'not_list_up')),
        ('STOPPED: something new (frame 5)', 0, ('stopped', 'other')),
        # S110 SCENESRC review: an item page's arrival waits for its parts panel (src\cas_goto.h ArrivalPanelReady): a panel that never readies
        # is a stop with PanelNotReady's words, one class per test
        ("STOPPED: not settled 900 frames after the last step: the overlay A0 has built no panel yet (+936 = 0; its state +16 = 1) (page B0; the arrival waits for the item page's panel); current ... (frame 5)", 0, ('stopped', 'not_settled:panel_build')),
        ("STOPPED: not settled 900 frames after the last step: the parts panel A1 is not idle (+16 = 1) (page B0; the arrival waits for the item page's panel) (frame 5)", 0, ('stopped', 'not_settled:panel_idle')),
        ("STOPPED: not settled 900 frames after the last step: the parts panel A1 is in a sub-mode (mixin+1000 = 2): its B would only leave that (page B0; the arrival waits for the item page's panel) (frame 5)", 0, ('stopped', 'not_settled:panel_submode')),
        ("STOPPED: not settled 900 frames after the last step: the parts panel's B enable test (sub_833FC6B8) is 0 (mixin+956 = 1) (page B0; the arrival waits for the item page's panel) (frame 5)", 0, ('stopped', 'not_settled:panel_enable')),
        ("STOPPED: not settled 900 frames after the last step: a pad bit the parts panel's B refuses is held (mixin+60 = 00000010) (page B0; the arrival waits for the item page's panel) (frame 5)", 0, ('stopped', 'not_settled:panel_pad')),
        ("STOPPED: not settled 900 frames after the last step: the part data is not ready (00000000: sub_822DC1A8 = 0) (page B0; the arrival waits for the item page's panel) (frame 5)", 0, ('stopped', 'not_settled:panel_parts')),
        # S110 CASREADY: the preview wait's two stops
        ("STOPPED: not settled 900 frames after the last step: the preview model is not ready (sub_822D38A0 = 0: the superstar is still being rebuilt or loaded) (page B0; the arrival waits for the preview model); current ... (frame 5)", 0, ('stopped', 'not_settled:preview_model')),
        ("STOPPED: not settled 900 frames after the last step: the game's loading screen is up (requester A0: state +4 = 2, request +8 = 0, screen +84 = A1) (page B0; the arrival waits for the preview model); current page B0 (vtable 821BBBA0, label 6388, depth 2, parent C0) (frame 5)", 0, ('stopped', 'not_settled:preview_loading')),
    ]
    for text, gate, want in cases:
        got = classify(text, gate)
        if got != want:
            print('FAIL classify %r: %r, want %r' % (text[:60], got, want)); bad += 1
    r = parse_log("[x] [wwe2k14] modified: wwe2k14_cas_goto = '5' (source: command line)\n[x] nothing else")
    if (r['record'], r['class'], r['frames']) != (5, 'no_result', None):
        print('FAIL no-result log: %r' % r); bad += 1
    # planted twin: a line from another module must not count as the go-to's end
    r = parse_log("[x] [wwe2k14] modified: wwe2k14_cas_goto = '5' (x)\n[x] [wwe2k14][menu_goto] record 5: arrived at record 5 (frame 9)")
    if r['class'] != 'no_result':
        print('FAIL planted twin: another module\'s arrival counted'); bad += 1
    print('cas_sweep selftest: %s (%d checks)' % ('ok' if not bad else '%d FAILED' % bad, 1 + len(cases) + 2))
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--run', help="record ids 'a,b,c' or 'all-left'")
    ap.add_argument('--batch', default='batch')
    ap.add_argument('--report', action='store_true')
    ap.add_argument('--selftest', action='store_true')
    ap.add_argument('--exe', default=EXE)
    ap.add_argument('--parallel', type=int, default=3)
    ap.add_argument('--wait', type=int, default=300)
    ap.add_argument('--dump', default=DUMP)
    ap.add_argument('--out', default=OUT)
    a = ap.parse_args()
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')   # names hold private-use glyphs (a button icon)
    if a.selftest:
        return selftest()
    recs = load_records(a.dump)
    if a.list:
        names = load_names()
        for rid in sorted(recs):
            d = describe(recs, names, rid)
            print('%3d  tab %d  depth %d  code %d  gate %5d  %-16s %s' % (rid, d['tab'], d['depth'], d['code'], d['gate'], d['design'], d['path']))
        return 0
    if a.run:
        if a.run == 'all-left':
            done = set(all_rows(a.out))
            rids = [r for r in sorted(recs) if r not in done]
        else:
            rids = [int(x) for x in a.run.split(',') if x.strip()]
        if not rids:
            print('nothing to run'); return 0
        if not 1 <= a.parallel <= 3:
            print('--parallel is 1..3'); return 2
        run_batch(rids, a.batch, a.exe, a.parallel, a.wait, a.out)
        report(a.out, a.dump)
        return 0
    if a.report:
        report(a.out, a.dump)
        return 0
    ap.print_help()
    return 0


if __name__ == '__main__':
    sys.exit(main())
