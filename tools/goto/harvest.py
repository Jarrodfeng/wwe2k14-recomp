"""S101 lane 111 (go-anywhere L-2, docs/research/s101/GOANY_MOMENTS.md 2.4 "Harvest"): plan one harvest run, then index its result.

A harvest is an unattended CPU-vs-CPU match played from boot with the replay recorder, the event bus and the hash track on; the recording it
leaves goes into the moment library, and every pin, count, kickout, elimination and finish of the bus stream becomes an address
(`moment:NAME@pin`, ...). This tool PLANS and INDEXES. It never starts the game: the main session runs the printed command through
tools\\run\\run.py (the run contract, the run lock).

    py -I tools\\goto\\harvest.py plan "Daniel Bryan vs Brock Lesnar; players=0; pace=quick" [--slug harvest_bryan_lesnar] [--out DIR]
        refuses a spec with hands ("a harvest has no hands": players > 0, no players=, or a raw p= list with a pad >= 0); forces pace=quick
        when the spec has none; writes DIR/route.txt (title -> main menu, then waits for the match and for the sequence to return to the menu)
        and DIR/plan.json; prints the flags and the run.py command, then the add command to run afterwards.
    py -I tools\\goto\\harvest.py add SLUG [--rec REC] [--events EVENTS.jsonl] [--det DET.csv] [--out DIR]
        `moments.py add` with the recording, the bus stream and the hash stream (the place comes from DIR/plan.json), refuses a recording whose
        header lacks the carried place flags (a replay would not rebuild the match: src/replay_carried.h), then prints every addressable moment
        with the frame it resolves to.
    py -I tools\\goto\\harvest.py selftest        (also gate lane moments_harvest)
"""
import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, 'tools', 'atlas'))
import moments  # noqa: E402
import resolve  # noqa: E402
import when as W  # noqa: E402

DET_SPEC = 'default,objects,sim'  # the reference stream's own spec (tools/goto/resolve.py moment_start)
PLACE_FLAGS = ('wwe2k14_match', 'wwe2k14_match_go')  # a harvest recording must carry these (wwe2k14_wm30 / _scene_victory only when used)
SLUG_RE = re.compile(r'^[A-Za-z0-9_.-]{1,48}$')


class HarvestError(ValueError):
    """The spec or the files cannot make a harvest: the message says which and why."""


def slugify(spec):
    names = [n for n in re.split(r'\s*,\s*|\s+vs\.?\s+', spec.split(';')[0].strip(), flags=re.I) if n]
    base = '_'.join(re.sub(r'[^a-z0-9]+', '', n.split()[-1].lower()) for n in names[:2] if n.split()) or 'match'
    return 'harvest_' + base


def canonical_place(spec):
    """The spec as the library will compare it: text as given, `players=0` and `pace=quick` added when absent (a harvest has no hands and plays
    at the quick pace). A raw numeric spec (type=..;p=ID:PAD) keeps its pads: any pad >= 0 is a hand and is refused."""
    parts = [p.strip() for p in spec.split(';') if p.strip()]
    if not parts:
        raise HarvestError('empty spec')
    raw = resolve.raw_spec(spec)
    if raw:
        for p in parts:
            k, _, v = p.partition('=')
            if k.strip() == 'p':
                hands = [x for x in v.split(',') if x and int(x.split(':')[1]) >= 0]
                if hands:
                    raise HarvestError('a harvest has no hands: p= gives a controller to %s (use pad -1 for every seat)' % ', '.join(hands))
        keys = {p.partition('=')[0].strip() for p in parts}
        if 'pace' not in keys:
            parts.append('pace=1')
        return '; '.join(parts), ';'.join(parts)
    players = None
    keys = set()
    for p in parts[1:]:
        k, _, v = p.partition('=')
        keys.add(k.strip().lower())
        if k.strip().lower() == 'players':
            try:
                players = int(v)
            except ValueError:
                raise HarvestError('players=%s is not a number' % v.strip())
    if '=' in parts[0]:   # the first field is the wrestlers; a key there means a spec with no names (type=..;..): not a raw spec either
        keys.add(parts[0].partition('=')[0].strip().lower())
    if players is None:
        raise HarvestError('a harvest has no hands: add "players=0" to the spec (players defaults to 1, a controller in the match)')
    if players > 0:
        raise HarvestError('a harvest has no hands: players=%d gives %d wrestler(s) a controller (use players=0)' % (players, players))
    if players < 0:
        raise HarvestError('players=%d is not a count' % players)
    if 'pace' not in keys:
        parts.append('pace=quick')
    text = '; '.join(parts)
    try:
        import match
        numeric, _ = match.build_spec(text)
    except Exception as x:   # SpecError, or no atlas data in this checkout
        raise HarvestError('match spec refused: %s' % x)
    return text, numeric


def route_text(slug):
    return '\n'.join([
        '# S101 lane 111 (tools/goto/harvest.py): the harvest route for %s. Generated: title -> main menu (the builder fires there, --wwe2k14_match_go),' % slug,
        '# then waits for the match (sequence 9) and for the sequence to come back to the menu (75), then quits. No pad input in the match: CPU vs CPU.',
        '# [U] that the sequence returns to 75 at the post-match menu (states.toml: 9 in-game, 75 menus); the first harvest run confirms it.',
        '# harness: flags --wwe2k14_replay_mode=true',
        'on_fail quit',
        'include boot_to_menu',
        'wait_state seq 9 7200 30',
        'wait_state seq 75 400000 30',
        'wait_frames 120',
        'quit', ''])


def flags_for(slug, numeric, wm30='', scene_victory=''):
    f = ['--wwe2k14_replay_record=' + slug, '--wwe2k14_match=' + numeric, '--wwe2k14_match_go=1', '--wwe2k14_events=true',
         '--wwe2k14_det_hash=' + DET_SPEC, '--wwe2k14_det_hash_v2=true']
    if wm30:
        f.append('--wwe2k14_wm30=' + wm30)
    if scene_victory:
        f.append('--wwe2k14_scene_victory=' + scene_victory)
    return f


def quote_flag(flag):
    """S101 lane 111 (review F7): "--name=value" -> the same in double quotes when the value has a character a shell splits or interprets."""
    return flag if re.fullmatch(r'[A-Za-z0-9_.=,:/\\+-]+', flag) else '"%s"' % flag.replace('"', '\\"')


def out_dir(slug, out=''):
    return out or os.path.join(moments.main_checkout(), 'out', 'harvest', slug)


def plan(spec, slug='', out='', quiet=False):
    place, numeric = canonical_place(spec)
    slug = slug or slugify(spec)
    if not SLUG_RE.match(slug):
        raise HarvestError('"%s" is not a moment name (letters, digits, _ . -)' % slug)
    d = out_dir(slug, out)
    os.makedirs(d, exist_ok=True)
    route = os.path.join(d, 'route.txt')
    with open(route, 'w', encoding='utf-8', newline='') as fh:
        fh.write(route_text(slug))
    flags = flags_for(slug, numeric)
    p = {'slug': slug, 'place': place, 'numeric': numeric, 'route': route, 'flags': flags, 'det_spec': DET_SPEC}
    with open(os.path.join(d, 'plan.json'), 'w', encoding='utf-8') as fh:
        json.dump(p, fh, indent=1)
    # Each flag is double-quoted when it holds anything a shell splits on (the match spec has ';'): PowerShell and a POSIX split both read it back whole.
    p['run'] = 'py -I tools\\run\\run.py "%s" --check %s' % (route, ' '.join(quote_flag(f) for f in flags))
    p['add'] = 'py -I tools\\goto\\harvest.py add %s --rec <out\\replays\\%s.rec> --events <logs\\events_NNN.jsonl> --det <det_NNN.csv>%s' % (
        slug, slug, ' --out "%s"' % d if out else '')
    if not quiet:
        print('harvest plan %s (nothing started; the main session runs the command below)' % slug)
        print('  place   : %s' % place)
        print('  numeric : %s' % numeric)
        print('  route   : %s' % route)
        for fl in flags:
            print('  flag    : %s' % fl)
        print('  run     : %s' % p['run'])
        print('  then    : %s' % p['add'])
    return p


def rec_flags(path):
    """The `flag NAME VALUE` lines of a recording's header (the loader's rule, src/replay_carried.h ParseFlagLine)."""
    out = {}
    with open(path, encoding='utf-8', errors='replace', newline='') as fh:
        for line in fh:
            line = line.rstrip('\r\n')
            m = re.match(r'flag +(\S+) *(.*)$', line)
            if m:
                out[m.group(1)] = m.group(2)
            elif line.startswith(('S ', 'K ', 'C ', 'T ', 'E ')):
                break
    return out


def addresses(name, lib=None):
    """-> [(address text, frame or None, note)]: every pin / count / kickout / elimination / finish of the moment's bus index, each resolved with the
    library's own resolver (moments.when_frame), so an address printed here is one the goto accepts."""
    meta, rec, events, _ = moments.load_indexed(name, lib)
    cand = []
    pins = [e for e in events if e['kind'] == 'match' and e.get('what') == 'pin']
    n_by_pin = {}
    # counts belong to the pin before them
    pin_idx = -1
    for e in events:
        if e['kind'] != 'match':
            continue
        if e.get('what') == 'pin':
            pin_idx += 1
        elif e.get('what') == 'count' and pin_idx >= 0:
            n_by_pin.setdefault(pin_idx, []).append(e['count'])
    for i, _ in enumerate(pins, 1):
        cand.append('@pin' + ('#%d' % i if i > 1 else ''))
        for c in n_by_pin.get(i - 1, []):
            cand.append('@pin%s@count:%d' % ('#%d' % i if i > 1 else '', c))
    kicks = [e for e in events if e['kind'] == 'match' and e.get('what') == 'kickout']
    for i, _ in enumerate(kicks, 1):
        cand.append('@kickout' + ('#%d' % i if i > 1 else ''))
    elims = [e for e in events if e['kind'] == 'match' and e.get('what') == 'eliminated']
    for i, _ in enumerate(elims, 1):
        cand.append('@eliminated#%d' % i)
    if any(e['kind'] == 'match' and e.get('what') == 'finish' for e in events):
        cand.append('@finish')
    out = []
    for w in cand:
        try:
            f, h, _, _ = moments.when_frame(name, w, lib)
            note = h.line.get('what', '') if isinstance(h.line, dict) else ''
            out.append(('moment:%s%s' % (name, w), f, note))
        except moments.MomentError as x:
            out.append(('moment:%s%s' % (name, w), None, str(x)))
    return out


def add(slug, rec='', events='', det='', out='', lib=None, shows='harvest'):
    d = out_dir(slug, out)
    pj = os.path.join(d, 'plan.json')
    if not os.path.isfile(pj):
        raise HarvestError('no plan for "%s" (%s missing): run `harvest.py plan` first' % (slug, pj))
    with open(pj, encoding='utf-8') as fh:
        p = json.load(fh)
    rec = rec or os.path.join(moments.main_checkout(), 'out', 'replays', slug + '.rec')
    if not os.path.isfile(rec):
        raise HarvestError('recording %s not found' % rec)
    if not events or not os.path.isfile(events):
        raise HarvestError('the bus stream (--events logs\\events_NNN.jsonl) is needed and %s not found' % (events or '(not given)'))
    flags = rec_flags(rec)
    missing = [n for n in PLACE_FLAGS if n not in flags]
    if missing:
        raise HarvestError('the recording %s carries no flag line for %s: a replay would not rebuild the match (the game that wrote it predates '
                           'src/replay_carried.h\'s place cvars, or the run was not given them)' % (rec, ', '.join(missing)))
    if flags.get('wwe2k14_match') != p['numeric']:
        raise HarvestError('the recording was made with wwe2k14_match=%s, the plan says %s' % (flags.get('wwe2k14_match'), p['numeric']))
    meta = moments.add_moment(slug, rec, det or None, shows, '', lib=lib, events=events, place=p['place'])
    return meta, addresses(slug, lib)


def print_addresses(meta, addrs):
    print('added %s: %d frames, %s' % (meta['name'], meta['frames'], json.dumps(meta['index']['counts'])))
    print('addressable moments (frame = where the replay hands over):')
    for a, f, note in addrs:
        print('  %-52s %s' % (a, ('frame %d' % f) if f is not None else 'NOT RESOLVABLE: ' + note))
    if not addrs:
        print('  none (the stream has no pin, count, kickout, elimination or finish)')


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('cmd', choices=['plan', 'add', 'selftest'])
    ap.add_argument('args', nargs='*')
    ap.add_argument('--slug', default='')
    ap.add_argument('--out', default='')
    ap.add_argument('--rec', default='')
    ap.add_argument('--events', default='')
    ap.add_argument('--det', default='')
    a = ap.parse_args(argv)
    try:
        if a.cmd == 'plan':
            if len(a.args) != 1:
                ap.error('plan SPEC')
            plan(a.args[0], a.slug, a.out)
            return 0
        if a.cmd == 'add':
            if len(a.args) != 1:
                ap.error('add SLUG')
            meta, addrs = add(a.args[0], a.rec, a.events, a.det, a.out)
            print_addresses(meta, addrs)
            return 0
        import harvest_selftest
        return harvest_selftest.main()
    except (HarvestError, moments.MomentError) as x:
        print('REFUSED: %s' % x)
        return 1


if __name__ == '__main__':
    sys.exit(main())
