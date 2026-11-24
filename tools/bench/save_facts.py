r"""S108 OPT: what save options a test run ran with, and how its match was started (docs\research\s108\OPT_save_options.md section 5).

  py -I tools\bench\save_facts.py --data PATH [--route R] [--final "FLAGS"]   print the ONE line run_list prints before a launch
  py -I tools\bench\save_facts.py --selftest                                    the line against the frozen save and a synthetic save, the launch reader

The save half is read from a user-data mirror WITHOUT running the game (tools\save\save_read.Save, the reader behind `q save`), only the fields the
inventory marks [C]: blood (S+81), the 30 Years caption word, the unlock-flag count, the saved DLC mask and a hash of the S and B option blocks
(the 50 unnamed option bytes are not labelled: a hash is enough to show that two runs' saves differed).
The launch half is decided from the route text and the final flag line:
  direct NAME   --wwe2k14_launch=NAME (or `cvar wwe2k14_launch NAME`): the captured match; its blood is off whatever the save says [C, T1_blood.md]
  @quick        the type setter copies the save's S block: options come from the save [C]
  match builder `cvar wwe2k14_match ...`: `o=K:V` overrides the option byte S+K; what the builder takes for the other bytes is [L] (not asserted here)
  menus         neither: the route types through the menus
"""
import hashlib
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'save'))
import save_read as S  # noqa: E402

B_BLOCK = 0x804B4          # P + : the 22-byte block B right after S (OPT note section 2b)
B_LEN = 22
UNLOCK_IDS = range(3, 196)  # P + UNLOCK_FLAGS + id: the 193 ids the note counts (3..195)
UNLOCK_TOTAL = len(UNLOCK_IDS)


def facts(sv):
    """A decoded Save -> {blood, caption, unlock, unlock_total, dlc, hash}."""
    o = sv.options()
    base = sv.pb(S.UNLOCK_FLAGS)
    sb = sv.p[sv.pb(S.S_BLOCK):sv.pb(S.S_BLOCK) + S.S_LEN] + sv.p[sv.pb(B_BLOCK):sv.pb(B_BLOCK) + B_LEN]
    return {'blood': o['blood'], 'caption': o['caption'], 'unlock': sum(1 for i in UNLOCK_IDS if sv.p[base + i]),
            'unlock_total': UNLOCK_TOTAL, 'dlc': sv.dlc_mask()['mask'], 'hash': hashlib.sha1(sb).hexdigest()[:8]}


def load(spec):
    """A mirror (path or name) -> facts dict, or {'error': why}. Never raises: a save the reader cannot read is a fact, not a crash."""
    try:
        return facts(S.Save(spec))
    except SystemExit as e:
        return {'error': str(e.code) if e.code else 'unreadable'}
    except Exception as e:   # noqa: BLE001
        return {'error': '%s: %s' % (type(e).__name__, e)}


def onoff(v):
    return 'on' if v else 'off'


def save_text(f):
    if 'error' in f:
        return 'save options: UNREADABLE (%s)' % f['error']
    return ('save options: blood %s (S+81), captions %s (the 30 Years caption word), unlock flags %d/%d, dlc mask 0x%08X, options hash %s'
            % (onoff(f['blood']), onoff(f['caption']), f['unlock'], f['unlock_total'], f['dlc'], f['hash']))


def _strip(v):
    return v.strip().strip('"')


def launch_how(text, flags):
    """(route text, final flag line as {name: value}) -> {'kind': direct|quick|builder|menus|both, 'name': .., 'o': {K: V}}.
    `cvar` lines of the route itself only (an `include`d route's lines are not followed)."""
    name = flags.get('wwe2k14_launch')
    match = None
    for ln in text.split('\n'):
        m = re.match(r'^\s*cvar\s+wwe2k14_launch\s+(\S+)', ln)
        if m:
            name = _strip(m.group(1))
        m = re.match(r'^\s*cvar\s+wwe2k14_match\s+(.+?)\s*$', ln)
        if m:
            match = _strip(m.group(1))
    if name is not None:
        name = _strip(name)
    if name and match is not None:
        return {'kind': 'both', 'name': name, 'o': {}}
    if name:
        return {'kind': 'quick' if name == '@quick' else 'direct', 'name': name, 'o': {}}
    if match is not None:
        o = {}
        for mm in re.finditer(r'(?:^|;)\s*o=([^;]*)', match):
            for pair in mm.group(1).split(','):
                if ':' in pair:
                    k, v = pair.split(':', 1)
                    try:
                        o[int(k)] = int(v)
                    except ValueError:
                        pass
        return {'kind': 'builder', 'name': '', 'o': o}
    return {'kind': 'menus', 'name': '', 'o': {}}


def launch_text(h):
    k = h['kind']
    if k == 'direct':
        return 'launch: direct %s (captured match: blood off whatever the save says)' % h['name']
    if k == 'quick':
        return 'launch: @quick (options from the save)'
    if k == 'builder':
        ov = ','.join('%d:%d' % kv for kv in sorted(h['o'].items())) or 'none'
        return 'launch: match builder (o= overrides: %s; the other option bytes as the builder takes them)' % ov
    if k == 'both':
        return 'launch: %s AND a match builder line (the route starts two ways: check which one runs)' % h['name']
    return 'launch: menus'


def route_flags(final):
    return {m.group(1): (m.group(2) if m.group(2) is not None else 'true') for m in re.finditer(r'(?<![\w-])--([A-Za-z][\w.]*)(?:=(\S*))?', final or '')}


def full_line(f, h):
    return save_text(f) + '; ' + launch_text(h)


# ---------------------------------------------------------------------------------------------------- selftest
def synthetic(blood=0, caption=0, unlock_ids=(), dlc=0, s_byte=None):
    """A decodable SaveData payload built here (zeros + the fields flipped). Never a copy of a real save."""
    n = BLOB_TOTAL
    p = bytearray(n)
    struct.pack_into('>II', p, 0, S.SIZE_A, S.SIZE_B)
    struct.pack_into('>I', p, S.BLOB + S.TITLE_COUNT, S.TITLE_N)
    pb = S.BLOB + S.SIZE_A
    p[pb + S.BLOOD] = blood
    struct.pack_into('>I', p, S.BLOB + S.CAPTION, caption)
    for i in unlock_ids:
        p[pb + S.UNLOCK_FLAGS + i] = 1
    struct.pack_into('>I', p, pb + S.DLC_MASK, dlc)
    if s_byte is not None:
        p[pb + S.S_BLOCK + s_byte[0]] = s_byte[1]
    return bytes(p)


BLOB_TOTAL = S.BLOB + S.SIZE_A + S.SIZE_B + 64


def selftest():
    ok = True

    def chk(label, cond, extra=''):
        nonlocal ok
        print('  %s %s %s' % ('ok  ' if cond else 'FAIL', label, extra))
        ok &= bool(cond)
    # 1. synthetic saves: every field flips, the hash follows the S / B blocks
    f0 = facts(S.Save(payload=synthetic()))
    chk('synthetic zero save: blood off, captions off, 0/193, dlc 0', (f0['blood'], f0['caption'], f0['unlock'], f0['unlock_total'], f0['dlc']) == (0, 0, 0, 193, 0), save_text(f0))
    f1 = facts(S.Save(payload=synthetic(blood=1, caption=1, unlock_ids=[3, 4, 100, 195], dlc=0x7C)))
    chk('synthetic flipped save: blood on, captions on, 4/193, dlc 0x7C', (f1['blood'], f1['caption'], f1['unlock'], f1['dlc']) == (1, 1, 4, 0x7C), save_text(f1))
    chk('line text for the flipped save', save_text(f1).startswith('save options: blood on (S+81), captions on (the 30 Years caption word), unlock flags 4/193, dlc mask 0x0000007C, options hash '))
    f2 = facts(S.Save(payload=synthetic(s_byte=(40, 9))))
    chk('hash follows an S byte (S+40)', f2['hash'] != f0['hash'] and f1['hash'] != f0['hash'], '%s %s %s' % (f0['hash'], f1['hash'], f2['hash']))
    f3 = facts(S.Save(payload=synthetic(caption=1)))
    chk('hash ignores the caption word (it is not in S / B)', f3['hash'] == f0['hash'])
    # 2. a save the reader cannot read says so
    bad = load(os.path.join(HERE, 'no_such_mirror_folder'))
    chk('unreadable save: the line says so', 'error' in bad and save_text(bad).startswith('save options: UNREADABLE ('), save_text(bad))
    # 3. the frozen save (values the OPT note records: blood off, 0/193, captions off)
    fz = load('userdata_frozen')
    if 'error' in fz:
        chk('frozen save readable', False, fz['error'])
    else:
        chk('frozen save: blood off, captions off, 0/193', (fz['blood'], fz['caption'], fz['unlock'], fz['unlock_total']) == (0, 0, 0, 193), save_text(fz))
        print('  line for the frozen save: ' + full_line(fz, launch_how('', route_flags('--wwe2k14_launch=lesnar_bryan'))))
    # 4. the launch reader
    h = launch_how('', route_flags('--x=1 --wwe2k14_launch=lesnar_bryan'))
    chk('direct launch', h['kind'] == 'direct' and launch_text(h) == 'launch: direct lesnar_bryan (captured match: blood off whatever the save says)', launch_text(h))
    h = launch_how('', route_flags('--wwe2k14_launch=@quick'))
    chk('@quick', launch_text(h) == 'launch: @quick (options from the save)')
    h = launch_how('cvar wwe2k14_match type=0;p=139:-1;o=81:1,5:2\ncvar wwe2k14_match_go 1\n', {})
    chk('builder with overrides', h['kind'] == 'builder' and h['o'] == {81: 1, 5: 2}, launch_text(h))
    h = launch_how('cvar wwe2k14_match type=0;p=1:-1\n', {})
    chk('builder without overrides', h['o'] == {} and 'overrides: none' in launch_text(h) and 'save' not in launch_text(h))
    chk('menus', launch_text(launch_how('wait 5\n', {})) == 'launch: menus')
    chk('cvar launch line', launch_how('cvar wwe2k14_launch foo\n', {})['kind'] == 'direct')
    chk('both ways', launch_how('cvar wwe2k14_match type=0\n', route_flags('--wwe2k14_launch=x'))['kind'] == 'both')
    print('selftest ' + ('PASSED' if ok else 'FAILED'))
    return ok


def main():
    a = sys.argv[1:]
    if '--selftest' in a:
        sys.exit(0 if selftest() else 1)

    def opt(n):
        return a[a.index(n) + 1] if n in a else None
    f = load(opt('--data') or 'userdata_frozen')
    text, final = '', opt('--final') or ''
    r = opt('--route')
    if r:
        try:
            text = open(r, encoding='utf-8', errors='replace').read()
        except OSError as e:
            print('save options: route unreadable (%s)' % e)
    flags = route_flags(final)
    if r and not final:   # standalone: the route's own header flags
        for ln in text.split('\n')[:40]:
            m = re.match(r'^#\s*harness:\s*flags\s+(.+)$', ln.strip())
            if m:
                flags.update(route_flags(m.group(1)))
    print(full_line(f, launch_how(text, flags)))


if __name__ == '__main__':
    main()
