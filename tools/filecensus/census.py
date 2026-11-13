#!/usr/bin/env python3
"""Lane 21: the file-fed census on "rdr" (RENDERER_ARC.md section 4, tool 5). Offline, 0 game runs (the Arena Viewer is not the game).

Every arena and every wrestler x attire is loaded from the player's own files into TWO viewer processes (one per backend: vk = the viewer's own
picture, rdr = our renderer + the scene front-end), shot with the same camera, and compared with the tile check of tools\\scene\\selftest.py.
The board says per item: pass / tile fault / no effect yet / load failure. Sheets (vk | rdr, labelled) are written for shot-checkers.

  py -I tools\\filecensus\\census.py [--only arenas|wrestlers] [--ids 5,7] [--limit N] [--wrestlers 197,210] [--tag NAME] [--exe PATH]
                                   [--plant mirror,tex=arena 7] [--timeout 120] [--resume] [--report]
Outputs (the main checkout's out\\s101\\filecensus\\<tag>\\, game pictures: local only, never committed): board.json, board.md, shots\\*.png,
sheet_*.png, vk.err.txt / rdr.err.txt. Resumable (--resume skips the keys already in board.json).

Mechanism: one viewer process per backend (named pipe, one JSON command per line), many items per process; a command that times out or a viewer that
dies is a board row ("load failure") and the batch goes on with a fresh process. The arenas run in the viewer's "ring" camera preset, the wrestlers
stand in arena 5 at the ring in their fixed idle (frame 30), framed by spot_cam (the same as selftest.py's wrestler check).
--plant (tests, needs the census build, tools\\filecensus\\make_census_build.py): mirror = the reader that ignores the r / y flags;
tex=LABEL = a missing texture on the model with that label."""
import argparse
import json
import os
import re
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'scene'))
import selftest  # noqa: E402  (the tile check: compare, compare_crop, read_png)
sys.path.insert(0, HERE)
import build_util  # noqa: E402
import filecensus_flags  # noqa: E402



def _main_checkout():
    # where out\ and game_data\ live: the main checkout, also when this file runs from a worktree (git's common dir is the main .git)
    if os.environ.get('RDR_MAIN_CHECKOUT'):
        return os.environ['RDR_MAIN_CHECKOUT']
    try:
        common = subprocess.run(['git', '-C', ROOT, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return ROOT


MAIN = _main_checkout()
SIZE = (640, 360)
LOOK = {'cmd': 'look', 'mode': 'lit', 'shadows': False, 'dof': False, 'fxaa': False, 'bloom': False, 'glare': False, 'gamma': False, 'haze': False}
STAGE_FOR_WRESTLERS = 5
ARENA_ANGLES = ['ring', 'ring_side', 'ring_back', 'top']  # presets of the viewer's camera (camera.h): every arena is judged on all four
MAX_ATTIRES = 20


class Link:
    """One viewer process on a named pipe; stderr to a file. send() times out through a watchdog that kills the process."""
    def __init__(self, exe, backend, game, name, errfile, env):
        self.exe, self.backend, self.game, self.name, self.errfile, self.env = exe, backend, game, name, errfile, env
        self.proc = None
        self.pipe = None
        self.starts = 0
        self.val_last = 0  # the running total of "validation errors so far" last seen in this process (it counts for the whole process)

    def alive(self):
        return self.proc is not None and self.proc.poll() is None and self.pipe is not None

    def start(self):
        self.stop()
        self.starts += 1
        self.val_last = 0
        err = open(self.errfile, 'ab')
        err.write(('\n=== census: start %d of the %s process ===\n' % (self.starts, self.backend)).encode())
        err.flush()
        self.proc = subprocess.Popen([self.exe, '--backend', self.backend, '--game', self.game, '--pipe', self.name, '--size', '%dx%d' % SIZE,
                                      '--parent', str(os.getpid()), '--hidden', '--no-dlc'],
                                     stdout=subprocess.DEVNULL, stderr=err, env=dict(os.environ, **self.env))
        err.close()
        path = r'\\.\pipe\%s' % self.name
        for _ in range(300):
            if self.proc.poll() is not None:
                raise RuntimeError('viewer exited at start (%s)' % self.proc.returncode)
            try:
                self.pipe = open(path, 'r+b', buffering=0)
                break
            except OSError:
                time.sleep(0.1)
        else:
            self._kill()
            raise RuntimeError('viewer pipe never appeared')
        self.send(LOOK, 60)
        self.send({'cmd': 'lights', 'haze': False}, 60)  # S101 lane 34: the rdr backend's post pass runs for haze alone (yGlareEtc_fog); the census wants the scene as drawn

    def _kill(self):
        try:
            if self.proc and self.proc.poll() is None:
                self.proc.kill()
        except OSError:
            pass

    def send(self, cmd, timeout):
        proc = self.proc
        timer = threading.Timer(timeout, lambda: proc.kill() if proc and proc.poll() is None else None)
        timer.daemon = True
        try:
            timer.start()
            self.pipe.write((json.dumps(cmd) + '\n').encode())
            buf = b''
            while not buf.endswith(b'\n'):
                ch = self.pipe.read(1)
                if not ch:
                    raise RuntimeError('no answer within %.0f s, or the viewer died (exit %s)' % (timeout, proc.poll() if proc else None))
                buf += ch
            return json.loads(buf.decode('utf-8', 'replace'))
        except BaseException:
            timer.cancel()
            self.stop()
            raise
        finally:
            timer.cancel()

    def stop(self):
        try:
            if self.pipe:
                self.pipe.write(b'{"cmd":"quit"}\n')
        except OSError:
            pass
        try:
            if self.pipe:
                self.pipe.close()
        except OSError:
            pass
        self.pipe = None
        if self.proc and self.proc.poll() is None:
            try:
                self.proc.wait(5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        self.proc = None


def err_size(path):
    return os.path.getsize(path) if os.path.exists(path) else 0


def err_slice(path, start):
    with open(path, 'rb') as f:
        f.seek(start)
        return f.read().decode('utf-8', 'replace')


def parse_rdr_text(text):
    """From the rdr stderr of one item: validation errors, the coverage classes, the [census] lines, the front-end warnings."""
    info = {'validation': None, 'cov': {}, 'also': {}, 'flagged': None, 'warn': []}
    errs = [int(x) for x in re.findall(r'validation errors(?: so far)?: (\d+)', text)]
    if errs:
        info['validation'] = errs[-1]  # (the process-global total after the item's last shot; item() turns it into this item's delta)
    # the coverage list of the LAST shot of the item only (each shot prints a fresh one)
    last = text.rfind('[rdr] shot ')
    cov_text = text[last:] if last >= 0 else text
    for m in re.finditer(r'^  (\S+)\s+(bound to our effect|no effect yet)\s+.*?\((\d+) draws', cov_text, re.M):
        info['cov'][m.group(1)] = (m.group(2), int(m.group(3)))
    for m in re.finditer(r'^    also (\d+) draws of (\S+) in the debug colour', cov_text, re.M):
        info['also'][m.group(2)] = info['also'].get(m.group(2), 0) + int(m.group(1))
    for m in re.finditer(r"^\[census\] model '([^']*)': (\d+) mirrored/turned placements, (\d+) textures", text, re.M):
        info.setdefault('models', {})[m.group(1)] = (int(m.group(2)), int(m.group(3)))
    subs = re.findall(r'^\[census\] submitted: (\d+) flagged placements with more than one instance, (\d+) instances', text, re.M)
    if subs:
        info['submitted'] = (int(subs[-1][0]), int(subs[-1][1]))  # the last shot of the item
    sl = re.findall(r'^\[census\] submitted slots: ([0-9,]*)\r?$', text, re.M)
    if sl:
        info['submitted_slots'] = [int(x) for x in sl[-1].split(',') if x]  # S101 lane 37: WHICH slots the draw list drew more than once
    for m in re.finditer(r'^\[rdr\] (texture .*|mesh .*)$', text, re.M):
        info['warn'].append(m.group(1)[:160])
    return info


def item_validation(last, link):
    """The viewer's "validation errors so far" is a total for the whole process: this item's errors are the rise since the previous item of the same process
    (link.val_last, reset when the process starts). last = the total after the item's last shot, None = the item printed none."""
    if last is None:
        return None
    delta = max(0, last - link.val_last)
    link.val_last = last
    return delta


def names():
    out = {'arena': {}, 'wrestler': {}}
    try:
        for e in json.load(open(os.path.join(ROOT, 'db', 'ids', 'arenas.json')))['entries']:
            out['arena'][e['id']] = e.get('name', '')
        for f in ('characters.json', 'dlc_characters.json'):
            p = os.path.join(ROOT, 'db', 'ids', f)
            if os.path.exists(p):
                for e in json.load(open(p)).get('entries', []):
                    if e.get('kind', 'wrestler') == 'wrestler':
                        out['wrestler'].setdefault(e['id'], e.get('name', ''))
    except Exception as e:  # names are a convenience
        print('note: names not read (%s)' % e)
    return out


class Census:
    def __init__(self, a):
        self.a = a
        self.exe = a.exe
        self.game = a.game
        self.tag = a.tag
        self.out = os.path.join(MAIN, 'out', 's101', 'filecensus', a.tag)
        self.shots = os.path.join(self.out, 'shots')
        os.makedirs(self.shots, exist_ok=True)
        env = {}
        for p in [x for x in a.plant.split(',') if x]:
            if p.startswith('slot='):
                env['WWE_CENSUS_PLANT_SLOT'] = p[5:]  # S101 lane 37: the loader never made the flagged copies of this one slot
            elif p == 'mirror':
                env['WWE_CENSUS_PLANT_MIRROR'] = '1'
            elif p == 'loader':
                env['WWE_CENSUS_PLANT_LOADER'] = '1'
            elif p.startswith('tex='):
                env['WWE_CENSUS_PLANT_TEX'] = p[4:]
            elif p.startswith('hang='):
                env['WWE_CENSUS_PLANT_HANG'] = p[5:]
            else:
                raise SystemExit('unknown --plant %s' % p)
        if env:
            env['WWE_RDR_VALIDATION'] = '1'
        self.env = dict(env, WWE_RDR_VALIDATION='1')
        for kv in a.env:
            k, _, v = kv.partition('=')
            self.env[k] = v
        self.errs = {'vk': os.path.join(self.out, 'vk.err.txt'), 'rdr': os.path.join(self.out, 'rdr.err.txt')}
        pid = os.getpid()
        self.links = {b: Link(self.exe, b, self.game, 'cen_%s_%d_%s' % (a.tag, pid, b), self.errs[b], self.env) for b in ('vk', 'rdr')}
        self.stage_loaded = {'vk': False, 'rdr': False}
        self.nm = names()
        self.rows = []
        self.t0 = time.time()

    # -- process helpers ---------------------------------------------------------------------------------------------
    def ensure(self, b, need_stage):
        link = self.links[b]
        if not link.alive():
            for attempt in range(3):  # (a viewer that exits at start is retried: the first start after a fresh build has failed once without a message)
                try:
                    link.start()
                    break
                except Exception:
                    if attempt == 2:
                        raise
                    time.sleep(2)
            self.stage_loaded[b] = False
        if need_stage and not self.stage_loaded[b]:
            r = link.send({'cmd': 'load', 'kind': 'arena', 'id': STAGE_FOR_WRESTLERS}, self.a.timeout * 2)
            if not r.get('ok'):
                raise RuntimeError('stage arena %d did not load: %s' % (STAGE_FOR_WRESTLERS, r.get('error')))
            self.stage_loaded[b] = True
        if not need_stage and self.stage_loaded[b]:
            self.stage_loaded[b] = False  # (an arena load replaces it)

    def run_commands(self, b, cmds, need_stage):
        """Sends cmds to backend b (each shot command carries its path); returns (ok, error, seconds, stderr slice, reply). A raised error / timeout =
        a dead process (restarted by ensure)."""
        t0 = time.time()
        start = None
        paths = [c['path'] for c in cmds if c.get('cmd') == 'shot']
        for p in paths:
            if os.path.exists(p):
                os.remove(p)
        try:
            self.ensure(b, need_stage)
            start = err_size(self.errs[b])
            for c in cmds:
                r = self.links[b].send(c, self.a.timeout * (2 if c.get('cmd') == 'load' else 1))
                if r.get('ok') is False or r.get('error'):
                    return False, '%s: %s' % (c.get('cmd'), r.get('error') or r), time.time() - t0, err_slice(self.errs[b], start), r
            if not all(os.path.exists(p) for p in paths):
                return False, 'no shot file written', time.time() - t0, err_slice(self.errs[b], start), None
            return True, '', time.time() - t0, err_slice(self.errs[b], start), None
        except Exception as e:
            txt = err_slice(self.errs[b], start) if start is not None else ''
            self.stage_loaded[b] = False
            return False, 'viewer: %s' % e, time.time() - t0, txt, None

    # -- one item ----------------------------------------------------------------------------------------------------
    def item(self, kind, ident, attire):
        key = '%s_%d_%d' % (kind, ident, attire) if kind == 'wrestler' else 'arena_%d' % ident
        row = {'key': key, 'kind': kind, 'id': ident, 'attire': attire, 'name': self.nm[kind].get(ident, ''), 'status': '', 'notes': []}
        angles = ARENA_ANGLES if kind == 'arena' else ['wrestler']
        res = {}
        for b in ('vk', 'rdr'):
            shot = os.path.join(self.shots, '%s_%s.png' % (key, b))  # (the first angle's; the other angles: key_ANGLE_b.png)
            paths = {an: (shot if an == angles[0] else os.path.join(self.shots, '%s_%s_%s.png' % (key, an, b))).replace(os.sep, '/') for an in angles}
            if kind == 'arena':
                cmds = [{'cmd': 'load', 'kind': 'arena', 'id': ident}]
                for an in angles:
                    cmds += [{'cmd': 'camera', 'preset': an}, {'cmd': 'shot', 'w': SIZE[0], 'h': SIZE[1], 'path': paths[an]}]
            else:
                cmds = [{'cmd': 'place', 'wrestler': ident, 'attire': attire, 'spot': 'ring', 'id': 'w1', 'motion': 'idle', 'frame': 30},
                        {'cmd': 'spot_cam', 'actor': 'w1', 'dist': 300, 'side': 25, 'pitch': 8, 'fov': 40},
                        {'cmd': 'shot', 'w': SIZE[0], 'h': SIZE[1], 'path': paths['wrestler']}]
            ok, error, secs, text, reply = self.run_commands(b, cmds, kind == 'wrestler')
            res[b] = (ok, error, secs, text, shot, reply, paths)
            row['%s_seconds' % b] = round(secs, 2)
            row['%s_shot' % b] = shot if ok else ''
            if not ok:
                row['notes'].append('%s: %s' % (b, error))
            row['%s_ok' % b] = bool(ok)
            row['%s_no_attire' % b] = bool(not ok and reply and reply.get('no_attire'))
        if kind == 'wrestler' and row['vk_no_attire'] and row['rdr_no_attire']:
            row['no_attire'] = True
            return row  # both backends say: past the last attire (not a board row)
        if kind == 'wrestler' and row['vk_no_attire'] != row['rdr_no_attire']:
            row['notes'].append('the backends disagree on this attire (vk no_attire %s, rdr no_attire %s)' % (row['vk_no_attire'], row['rdr_no_attire']))
        info = parse_rdr_text(res['rdr'][3])
        if 'models' in info:
            mods = [v for k, v in info['models'].items() if k.startswith('arena')]
            row['mirrored_slots'] = sum(v[0] for v in mods)  # what the loader's in-memory model holds (not independent of the loader)
        if kind == 'arena':
            try:
                # read from the FILE: entry 50001 slots flagged r / y that the match look draws (S101 lane 37: `e` events-only slots are not drawn in a match)
                row['file_slot_list'] = filecensus_flags.arena_flagged_slot_list(ident)
                row['file_slots'] = len(row['file_slot_list'])
            except Exception as e:
                row['file_slots'] = None
                row['notes'].append('file slots not read: %s' % e)
        if 'submitted' in info:
            row['submitted_flagged'], row['submitted_instances'] = info['submitted']
        if 'submitted_slots' in info:
            row['submitted_slots'] = info['submitted_slots']
        row['validation'] = item_validation(info['validation'], self.links['rdr'])
        warns = info['warn']
        if not res['vk'][0] or not res['rdr'][0]:
            row['status'] = 'load failure'
            return row
        if kind == 'arena' and row.get('file_slots') is not None and row.get('submitted_flagged', 0) < row['file_slots']:
            # the file flags more slots r / y than the draw list got multi-instance placements for: a reader (the loader or rdr) dropped some
            row['status'] = 'load failure'
            row['obj_short'] = True
            row['notes'].append('draw list short: the file flags %d slots r / y, the draw list got %d of them drawn more than once'
                                % (row['file_slots'], row.get('submitted_flagged', 0)))
            if row.get('submitted_slots') is not None and row.get('file_slot_list') is not None:  # S101 lane 37: name them
                row['missing_slots'] = sorted(set(row['file_slot_list']) - set(row['submitted_slots']))
                row['notes'].append('missing slots (flagged in the file, not drawn twice): %s' % ','.join(str(x) for x in row['missing_slots']))
        if warns:
            row['status'] = 'load failure'
            row['notes'].append('rdr warnings: ' + '; '.join(warns[:3]))
            row['rdr_warnings'] = len(warns)
        # the tile check
        try:
            import numpy as np
            from PIL import Image
            if kind == 'arena':
                ok, bad_notes, row['angles'] = True, [], {}
                for an in angles:  # every angle must pass: the ring view alone does not see the half of an arena that is behind it
                    aok, anotes = selftest.compare(selftest.read_png(res['vk'][6][an]), selftest.read_png(res['rdr'][6][an]))
                    row['angles'][an] = [res['vk'][6][an], res['rdr'][6][an], aok, anotes[-1] if anotes else '']
                    if not aok:
                        ok = False
                        bad_notes.append('%s: %s' % (an, anotes[-1] if anotes else ''))
                notes = ['; '.join(bad_notes)] if bad_notes else [row['angles'][angles[0]][3]]
            else:
                ref = np.asarray(Image.open(res['vk'][4]).convert('RGB'))
                got = np.asarray(Image.open(res['rdr'][4]).convert('RGB'))
                ok, notes = selftest.compare_crop(np, ref, got)
        except Exception as e:
            row['status'] = 'load failure'
            row['notes'].append('compare: %s' % e)
            return row
        row['tile_ok'] = ok
        row['tile'] = row['angles'][angles[0]][3] if kind == 'arena' else (notes[-1] if notes else '')  # (arenas: the ring view's numbers)
        row['fault'] = notes[-1] if notes else ''
        m = re.search(r'tiles close (\d+)/(\d+).*structure correlation (-?[\d.]+)', row['tile'])
        if m:
            row['close'], row['corr'] = int(m.group(1)) / int(m.group(2)), float(m.group(3))
        if info['validation'] is None:
            row['notes'].append('no validation line in the rdr log')
            if not row['status']:
                row['status'] = 'load failure'
        elif row['validation'] > 0:
            row['notes'].append('rdr validation errors (new in this item): %d' % row['validation'])
        unbound = {}
        for k, (state, n) in info['cov'].items():
            if state != 'bound to our effect' and n > 0 and (kind == 'arena' or k.startswith('yCh_')):
                unbound[k] = unbound.get(k, 0) + n
        for k, n in info['also'].items():
            if kind == 'arena' or k.startswith('yCh_'):
                unbound[k] = unbound.get(k, 0) + n
        row['no_effect'] = unbound
        if not row['status']:
            if not ok or (row['validation'] or 0) > 0:
                row['status'] = 'tile fault'
                if not ok:
                    row['notes'].append(row['fault'])
            elif unbound:
                row['status'] = 'no effect yet'
                row['notes'].append('magenta: ' + ', '.join('%s %d' % kv for kv in sorted(unbound.items())))
            else:
                row['status'] = 'pass'
        elif not ok:
            row['notes'].append(row['fault'])
        return row

    # -- the batches -------------------------------------------------------------------------------------------------
    def list_ids(self, kind):
        self.ensure('vk', False)
        return self.links['vk'].send({'cmd': 'list', 'kind': kind}, 60).get('ids', [])

    def run(self):
        a = self.a
        bpath = os.path.join(self.out, 'board.json')
        if a.resume and os.path.exists(bpath):
            self.rows = json.load(open(bpath))['rows']
        done = {r['key'] for r in self.rows}
        ids = {}
        todo = []
        kinds = ['arena', 'wrestler'] if not a.only else [{'arenas': 'arena', 'wrestlers': 'wrestler'}[a.only]]
        for kind in kinds:
            ids[kind] = self.list_ids(kind)
            want = {int(x) for x in (a.ids if kind == 'arena' else a.wrestlers).split(',') if x.strip()}
            if want:
                ids[kind] = [i for i in ids[kind] if i in want]
            if a.limit:
                ids[kind] = ids[kind][:a.limit]
        print('arenas listed: %d, wrestlers listed: %d' % (len(ids.get('arena', [])), len(ids.get('wrestler', []))), flush=True)
        n = 0
        for kind in kinds:
            for ident in ids[kind]:
                misses = 0
                for attire in (range(MAX_ATTIRES) if kind == 'wrestler' else [0]):
                    key = 'wrestler_%d_%d' % (ident, attire) if kind == 'wrestler' else 'arena_%d' % ident
                    if key in done:
                        continue
                    row = self.item(kind, ident, attire)
                    if row.get('no_attire') and kind == 'wrestler':
                        misses += 1
                        if misses >= 2 and attire > 0:  # two attires in a row that both backends refuse: the end (one miss alone may be a gap)
                            break
                        if attire == 0:
                            row['status'] = 'load failure'
                            row['notes'].append('attire 0 does not exist on either backend')
                            self.rows.append(row)
                            done.add(key)
                        continue
                    misses = 0
                    if kind == 'wrestler' and attire == MAX_ATTIRES - 1 and row.get('vk_ok') and row.get('rdr_ok'):
                        row['notes'].append('attire limit reached (%d): later attires of this wrestler, if any, are NOT on the board' % MAX_ATTIRES)
                    if not row['status']:
                        row['status'] = 'load failure'
                    self.rows.append(row)
                    done.add(key)
                    n += 1
                    print('%-18s %-13s %6.1f s  %s' % (key, row['status'], row.get('vk_seconds', 0) + row.get('rdr_seconds', 0), '; '.join(row['notes'])[:100]), flush=True)
                    if n % 10 == 0:
                        self.save()
        self.save()
        for l in self.links.values():
            l.stop()
        print('census wall time %.0f s' % (time.time() - self.t0))

    def save(self):
        with open(os.path.join(self.out, 'board.json'), 'w') as f:
            json.dump({'tag': self.tag, 'exe': self.exe, 'plant': self.a.plant, 'seconds': round(time.time() - self.t0, 1), 'rows': self.rows}, f, indent=1)


def board_md(out, rows, tag, seconds, plant, extra_notes=''):
    statuses = ['pass', 'tile fault', 'no effect yet', 'load failure']
    lines = ['# File census board: %s' % tag, '']
    if plant:
        lines += ['Planted: %s' % plant, '']
    lines += ['Run time %.0f s.' % seconds, '']
    for kind, title in (('arena', 'Arenas'), ('wrestler', 'Wrestlers x attires')):
        rs = [r for r in rows if r['kind'] == kind]
        if not rs:
            continue
        cnt = {s: sum(1 for r in rs if r['status'] == s) for s in statuses}
        lines.append('- **%s:** %d items: %s' % (title, len(rs), ', '.join('%d %s' % (cnt[s], s) for s in statuses)))
    ws = [r for r in rows if r['kind'] == 'wrestler']
    if ws:
        lines.append('- **Attire reconciliation:** %d wrestlers, %d rows; attires placed ok: vk %d, rdr %d; rows where the backends disagreed %d; rows that hit the attire limit %d; '
                     'wrestlers whose attire 0 failed on both backends %d' % (
                         len({r['id'] for r in ws}), len(ws), sum(1 for r in ws if r.get('vk_ok')), sum(1 for r in ws if r.get('rdr_ok')),
                         sum(1 for r in ws if r.get('vk_no_attire') != r.get('rdr_no_attire')), sum(1 for r in ws if any('attire limit' in n for n in r['notes'])),
                         sum(1 for r in ws if r['attire'] == 0 and any('does not exist on either' in n for n in r['notes']))))
    lines += ['']
    if extra_notes:
        lines += [extra_notes, '']
    lines += ['| item | name | attire | status | close | corr | validation | mirrored slots | notes |', '|---|---|---|---|---|---|---|---|---|']
    for r in rows:
        lines.append('| %s | %s | %s | %s | %s | %s | %s | %s | %s |' % (
            r['key'], r.get('name', ''), r['attire'] if r['kind'] == 'wrestler' else '', r['status'],
            '%.2f' % r['close'] if 'close' in r else '', '%.3f' % r['corr'] if 'corr' in r else '', r.get('validation', ''),
            r.get('mirrored_slots', '') if r['kind'] == 'arena' else '', '; '.join(r['notes']).replace('|', '/')[:200]))
    with open(os.path.join(out, 'board.md'), 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')


def sheets(out, rows):
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        return []
    made = []
    for kind in ('arena', 'wrestler'):
        rs = [r for r in rows if r['kind'] == kind and r.get('vk_shot') and r.get('rdr_shot')]
        if kind == 'arena':
            tw, th, per_row, rows_per = 240, 135, 1, 11  # an item = vk | rdr at the ring view, then vk | rdr at the first failing angle (else the top view)
            crop = (0.0, 1.0)
        else:
            tw, th, per_row, rows_per = 240, 240, 3, 5  # the wrestler's region only
            crop = selftest.CROP
        per = per_row * rows_per
        for page in range(0, len(rs), per):
            chunk = rs[page:page + per]
            npan = 4 if kind == 'arena' else 2
            cell_w, cell_h = npan * (tw + 8), th + 18
            sheet = Image.new('RGB', (per_row * cell_w, ((len(chunk) + per_row - 1) // per_row) * cell_h), (24, 24, 24))
            d = ImageDraw.Draw(sheet)
            for i, r in enumerate(chunk):
                x, y = (i % per_row) * cell_w, (i // per_row) * cell_h
                label = '%s %s  %s  [%s]' % (r['key'], r.get('name', ''), '' if kind == 'arena' else 'attire %d' % r['attire'], r['status'])
                d.text((x + 3, y + 2), label[:90], fill=(255, 255, 255) if r['status'] == 'pass' else (255, 200, 80))
                panels = [('vk', r['vk_shot'], 'ring' if kind == 'arena' else ''), ('rdr', r['rdr_shot'], 'ring' if kind == 'arena' else '')]
                if kind == 'arena':
                    an = next((k for k, v in r.get('angles', {}).items() if not v[2]), 'top')
                    if an in r.get('angles', {}):
                        panels += [('vk', r['angles'][an][0], an), ('rdr', r['angles'][an][1], an)]
                for j, (b, path, an) in enumerate(panels):
                    im = Image.open(path).convert('RGB')
                    w, h = im.size
                    im = im.crop((int(w * crop[0]), 0, int(w * crop[1]), h)).resize((tw, th))
                    sheet.paste(im, (x + j * (tw + 8), y + 16))
                    d.text((x + j * (tw + 8) + 3, y + 17), (b + ' ' + an).strip(), fill=(0, 255, 0))
            p = os.path.join(out, 'sheet_%s_%02d.png' % (kind, page // per))
            sheet.save(p)
            made.append(p)
    return made


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--only', choices=['arenas', 'wrestlers'])
    ap.add_argument('--ids', default='', help='arena ids')
    ap.add_argument('--wrestlers', default='', help='wrestler ids')
    ap.add_argument('--limit', type=int, default=0)
    ap.add_argument('--tag', default='full')
    ap.add_argument('--exe', default='')
    ap.add_argument('--game', default=os.path.join(MAIN, 'game_data', 'extracted'))
    ap.add_argument('--plant', default='')
    ap.add_argument('--timeout', type=float, default=120.0, help='seconds per command (a load gets twice)')
    ap.add_argument('--resume', action='store_true')
    ap.add_argument('--env', action='append', default=[], help='KEY=VALUE for both viewer processes (e.g. WWE_RDR_NO_PALETTE=1)')
    ap.add_argument('--report', action='store_true', help='sheets + board.md from board.json only')
    ap.add_argument('--no-sheets', action='store_true')
    a = ap.parse_args()
    if not a.exe:  # always this checkout's own census build, rebuilt when any source it is made from is newer (never another checkout's exe)
        try:
            a.exe = build_util.ensure_census_exe(ROOT, MAIN)
        except Exception as e:
            print('FAIL  the census viewer could not be built: %s' % e)
            return 1
    for need in (a.exe, a.game):
        if not a.exe or not os.path.exists(need):
            print('FAIL  cannot read its input: %s' % (need or 'no viewer exe (build it: pwsh tools\\assets\\build_viewer.ps1 -Backend rdr)'))
            return 1
    c = Census(a)
    if not a.report:
        c.run()
    else:
        data = json.load(open(os.path.join(c.out, 'board.json')))
        c.rows, c.t0 = data['rows'], time.time() - data.get('seconds', 0)
    board_md(c.out, c.rows, a.tag, time.time() - c.t0 if not a.report else json.load(open(os.path.join(c.out, 'board.json'))).get('seconds', 0), a.plant)
    if not a.no_sheets:
        for p in sheets(c.out, c.rows):
            print('sheet', p)
    print('board', os.path.join(c.out, 'board.md'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
