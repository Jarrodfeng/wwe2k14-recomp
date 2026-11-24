"""S106 C1: `q save` = decode a user-data mirror's SaveData.Dat (tools/save/save_read.py; docs/formats/save_data.md). Read-only, 0 game runs.

  py tools/q/q.py save [titles|options|dlc|caw|threads|all] [--data NAME|PATH] [--json] [--names]
  py tools/q/q.py save check EXPR... [--data NAME|PATH]     exit 1 when any EXPR is false (the precondition check a recipe runner calls)

--data: a folder name under <main>\\out\\bench (default userdata_frozen) or a path. --names: titles / characters as the game's text (q text).

EXPR grammar (one expression per argument; true or false, exit 0 only when all are true; an expression that cannot be read exits 2):
  holder:title=T:char=C | :char!=C | :vacant       title T (0..90, a real record) holders +156 do / do not include C (0..511); :vacant = holders[0] is 512; char=512 is refused
  plate:title=T | noplate:title=T                  left-hand BOBJ plate in the asset index + holder set + save unlock byte on; unreadable (2) when only the save byte locks it
  option:caption=N | option:blood=N | option:KEY   numeric option keys only; ops = != >= <= > < &
  dlc:mask=0x7C | dlc:mask&0x40 | dlc:mask==0      the saved DLC pack mask (hex or decimal; & = all of those bits set; == != >= <= > < too)
  caw:count OP N | caw:slot=N | caw:slot!=N | threads:char=C[:set=N] | threads:count OP N    slots WITH their .cas package; = only for threads keys
Unknown / repeated keys, an operator a key does not support, a title outside the table, char=512, a crash, an empty --data, another save layout: exit 2 (never 0 or 1).
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'save'))
import save_read as S  # noqa: E402

NO_HOLDER = S.NO_HOLDER
OPS = ('>=', '<=', '!=', '==', '&', '=', '>', '<')


class ExprError(Exception):
    pass


def _num(s):
    try:
        return int(s, 0)
    except ValueError:
        raise ExprError('not a number: %r' % s)


_CMP = {'=': lambda a, b: a == b, '==': lambda a, b: a == b, '!=': lambda a, b: a != b, '>=': lambda a, b: a >= b, '<=': lambda a, b: a <= b,
        '>': lambda a, b: a > b, '<': lambda a, b: a < b, '&': lambda a, b: (a & b) == b}


def _cmp(a, op, b):
    # S106 C1F diff review F1: every value compared here is an unsigned word or count, so these are always true (or always false) whatever
    # the save says; a typo like dlc:mask&0 for dlc:mask==0 must not pass on any mirror.
    if b < 0:
        raise ExprError('a negative comparand on an unsigned value is always true or always false')
    if op == '&' and b == 0:
        raise ExprError('&0 is true for every value (did you mean ==0?)')
    if (op == '>=' and b == 0) or (op == '<' and b == 0):
        raise ExprError('%s0 on an unsigned value is always %s' % (op, 'true' if op == '>=' else 'false'))
    return _CMP[op](a, b)      # lazy: only the asked operator runs (S106 C1F F1)


def _split_op(part):
    """'name>=3' -> ('name', '>=', '3'): the leftmost operator, longest first."""
    best = None
    for op in OPS:
        i = part.find(op)
        if i > 0 and (best is None or i < best[0] or (i == best[0] and len(op) > len(best[1]))):
            best = (i, op)
    if best is None:
        raise ExprError('no operator in %r' % part)
    i, op = best
    return part[:i], op, part[i + len(op):]


def _kv(parts, allowed, flags=()):
    """key OP value parts -> {key: (op, value)}. An unknown or repeated key is an error (S106 C1F B2); `flags` are bare words (e.g. vacant)."""
    d = {}
    for p in parts:
        if p in flags:
            n, op, v = p, None, None
        else:
            n, op, v = _split_op(p)
            if n in flags:   # S106 C1F diff review B1: holder:title=2:vacant=0 was parsed, then the =0 ignored
                raise ExprError('%s takes no operator or value (got %r)' % (n, p))
        if n not in allowed and n not in flags:
            raise ExprError('unknown key %r (allowed: %s)' % (n, ' '.join(sorted(set(allowed) | set(flags)))))
        if n in d:
            raise ExprError('key %r given twice' % n)
        d[n] = (op, v)
    return d


def _only_eq(kv, *keys):
    for k in keys:
        if k in kv and kv[k][0] != '=':
            raise ExprError('%s supports only = (got %s)' % (k, kv[k][0]))


def _title(sv, t):
    """The record of title T; ExprError when T is outside the table (0..title_count-1) or its kind is 0 (no record). S106 C1F B1."""
    n = sv.title_count()
    if not 0 <= t < n:
        raise ExprError('title %d is outside the table 0..%d' % (t, n - 1))
    rec = {x['title']: x for x in sv.titles()}.get(t)
    if rec is None:
        raise ExprError('title %d has kind 0 (no record)' % t)
    return rec


def _alt_note(rec):
    return ' ; ALT SET +164 DIFFERS (%s): the game reads it only when [0x836F2DA4] == 1 and routes outside boot-to-exhibition are unproven' % rec['alt'] \
        if rec['alt'] != rec['holders'] else ''


def eval_expr(sv, expr):
    """-> (bool, why). Raises ExprError for a malformed expression or an unreadable input (asset index missing); main turns every other exception into exit 2 too."""
    kind, _, rest = expr.partition(':')
    parts = [x for x in rest.split(':') if x]
    if kind == 'holder':
        kv = _kv(parts, ('title', 'char'), ('vacant',))
        if 'title' not in kv or ('char' in kv) == ('vacant' in kv):
            raise ExprError('holder wants holder:title=T:char=C, holder:title=T:char!=C or holder:title=T:vacant')
        _only_eq(kv, 'title')
        rec = _title(sv, _num(kv['title'][1]))
        hs, t = rec['holders'], rec['title']
        if 'vacant' in kv:
            return hs[0] == NO_HOLDER, 'title %d holders %s%s' % (t, hs, _alt_note(rec))
        op, c = kv['char'][0], _num(kv['char'][1])
        if op not in ('=', '!='):
            raise ExprError('holder char op must be = or !=')
        if c == NO_HOLDER:
            raise ExprError('char=512 is the "none" marker (singles titles store [X, 512]): use holder:title=T:vacant')
        if not 0 <= c < NO_HOLDER:
            raise ExprError('char %d is not a character id' % c)
        has = c in hs
        return (has if op == '=' else not has), 'title %d holders %s (alt %s not used)%s' % (t, hs, rec['alt'], _alt_note(rec))
    if kind in ('plate', 'noplate'):
        kv = _kv(parts, ('title',))
        if 'title' not in kv:
            raise ExprError('%s wants %s:title=T' % (kind, kind))
        _only_eq(kv, 'title')
        rec = _title(sv, _num(kv['title'][1]))
        t = rec['title']
        pl = S.title_plated(rec)
        if pl is None:
            raise ExprError('asset index %s missing: cannot say whether belt %d has a plate' % (S.asset_db(), rec['belt']))
        if S.has_plate(rec['belt']) and rec['holders'][0] != NO_HOLDER and not rec['unlocked']:
            # the save byte says locked, but sub_822BCE48 also asks the DLC manager [0x8392476C]: "no plate" is not provable from the save (F4)
            raise ExprError('title %d: unlock id %d is off in the save, the DLC manager may still unlock it: plate state unknown from the save alone' % (t, rec['unlock_id']))
        return (pl if kind == 'plate' else not pl), ('title %d belt %d holders %s unlocked(save only) %s -> plated %s (left-hand BOBJ plate only: waist belts '
                                                     'with table 103 are not checked)%s' % (t, rec['belt'], rec['holders'], rec['unlocked'], pl, _alt_note(rec)))
    if kind == 'option':
        n, op, v = _split_op(rest)
        o = sv.options()
        if n not in o or not isinstance(o[n], int):
            raise ExprError('option names: %s' % ' '.join(k for k, x in o.items() if isinstance(x, int)))
        return _cmp(o[n], op, _num(v)), 'option %s = %d' % (n, o[n])
    if kind == 'dlc':
        n, op, v = _split_op(rest)
        if n != 'mask':
            raise ExprError('dlc wants dlc:mask OP VALUE')
        m = sv.dlc_mask()['mask']
        return _cmp(m, op, _num(v)), 'dlc mask = 0x%X' % m
    if kind == 'caw':
        n, op, v = _split_op(rest)
        cs = sv.caws()
        if any(c['package'] is None for c in cs):
            raise ExprError('cannot tell whether the CAW slots have their .cas packages (no save path)')
        used = [c['slot'] for c in cs if c['package'] is True]   # a used slot with no package is wiped at boot (save_data.md)
        gone = [c['slot'] for c in cs if c['package'] is not True]
        note = ' (slots with no .cas package, wiped at boot, not counted: %s)' % gone if gone else ''
        if n == 'count':
            return _cmp(len(used), op, _num(v)), 'caw count = %d%s' % (len(used), note)
        if n == 'slot':
            if op not in ('=', '!='):
                raise ExprError('caw:slot supports only = and !=')
            s = _num(v)
            if not 0 <= s < S.CAW_SLOTS:
                raise ExprError('caw slot %d is outside 0..%d' % (s, S.CAW_SLOTS - 1))
            return (s in used) == (op == '='), 'caw slots used %s%s' % (used, note)
        raise ExprError('caw wants count OP N or slot=N / slot!=N')
    if kind == 'threads':
        th = sv.threads()
        if len(parts) == 1 and parts[0].startswith('count'):
            n, op, v = _split_op(parts[0])
            if n != 'count':
                raise ExprError('threads wants threads:count OP N')
            tot = sum(len(x['sets']) for x in th)
            return _cmp(tot, op, _num(v)), 'threads sets = %d' % tot
        kv = _kv(parts, ('char', 'set'))
        if 'char' not in kv:
            raise ExprError('threads wants threads:char=C[:set=N] or threads:count OP N')
        _only_eq(kv, 'char', 'set')
        c = _num(kv['char'][1])
        if c not in S.CHARS:
            raise ExprError('char %d is not a roster id (100..499)' % c)
        sets = [s['set'] for x in th if x['char'] == c for s in x['sets']]
        if 'set' in kv:
            n = _num(kv['set'][1])
            if not 0 <= n < 3:
                raise ExprError('set %d is outside 0..2' % n)
            return n in sets, 'char %d sets %s' % (c, sets)
        return bool(sets), 'char %d sets %s' % (c, sets)
    raise ExprError('unknown expression kind %r (holder plate noplate option dlc caw threads)' % kind)


def _names(rows):
    """Best effort: title / character names through q text (the player's own string.pac); {} when unavailable."""
    try:
        import qgx
    except Exception:
        return {}
    cache = {}

    def txt(i):
        if i not in cache:
            try:
                h = qgx.cmd_text(i)
                cache[i] = h[0]['text'] if h else None
            except Exception:
                cache[i] = None
        return cache[i]
    return {'text': txt}


def main(argv):
    """Every failure to understand the input is exit 2 (never 0, never 1): a crash included (S106 C1F F1)."""
    try:
        return _main(argv)
    except SystemExit as e:
        if e.code in (0, None):
            return 0
        print('q save: %s' % e)
        return 2
    except Exception as e:  # noqa: BLE001
        print('q save: unreadable (%s: %s)' % (type(e).__name__, e))
        return 2


def _main(argv):
    argv = list(argv)
    data = None
    as_json = '--json' in argv
    names = '--names' in argv
    argv = [a for a in argv if a not in ('--json', '--names')]
    if '--data' in argv:
        i = argv.index('--data')
        if i + 1 >= len(argv):
            print('q save: --data wants a folder name or a path')
            return 2
        data = argv[i + 1]
        del argv[i:i + 2]
    try:
        sv = S.Save(data)
    except SystemExit as e:
        print('q save: %s' % e)
        return 2
    if argv and argv[0] == 'check':
        exprs = argv[1:]
        if not exprs:
            print('q save check: no expressions (see tools/q/qsave.py)')
            return 2
        bad = err = 0
        for e in exprs:
            try:
                ok, why = eval_expr(sv, e)
            except ExprError as x:
                print('ERROR  %s  (%s)' % (e, x))
                err += 1
                continue
            except Exception as x:  # noqa: BLE001  a crash is unreadable, never a verdict
                print('ERROR  %s  (%s: %s)' % (e, type(x).__name__, x))
                err += 1
                continue
            print('%s  %s  (%s)' % ('true ' if ok else 'FALSE', e, why))
            bad += 0 if ok else 1
        print('q save check: %s (%s)' % ('all true' if not (bad or err) else '%d false, %d unreadable' % (bad, err), sv.path))
        return 2 if err else (1 if bad else 0)
    what = argv[0] if argv else 'all'
    if what not in ('titles', 'options', 'dlc', 'caw', 'threads', 'all'):
        print(__doc__)
        return 2
    out = {'path': sv.path}
    nm = _names(None) if names else {}
    if what in ('titles', 'all'):
        ts = sv.titles()
        for t in ts:
            t['plated'] = S.title_plated(t)
            if nm:
                t['name'] = nm['text'](t['name_id'])
                t['holder_names'] = [nm['text'](60000 + h) if h != 512 else None for h in t['holders']]
        out['titles'] = ts
    if what in ('options', 'all'):
        out['options'] = sv.options()
    if what in ('dlc', 'all'):
        out['dlc'] = sv.dlc_mask()
    if what in ('caw', 'all'):
        out['caw'] = sv.caws()
    if what in ('threads', 'all'):
        out['threads'] = sv.threads()
    if as_json:
        print(json.dumps(out, indent=1))
        return 0
    print('save: %s' % sv.path)
    for t in out.get('titles', []):
        held = [h for h in t['holders'] if h != 512]
        print('title %2d kind %d belt %d holders %s%s unlock %d %s plated %s%s' % (
            t['title'], t['kind'], t['belt'], held, ' alt %s' % [h for h in t['alt'] if h != 512] if t['alt'] != t['holders'] else '',
            t['unlock_id'], 'on' if t['unlocked'] else 'OFF', t['plated'],
            ('  %s / %s' % (t.get('name'), t.get('holder_names'))) if nm else ''))
    if 'options' in out:
        o = out['options']
        print('options: caption %d (0 = off)  blood %d (0 = off)  30Years words %s' % (o['caption'], o['blood'], {k: v for k, v in o.items() if k.startswith('opt30')}))
    if 'dlc' in out:
        print('dlc mask: 0x%08X (copy 0x%08X)' % (out['dlc']['mask'], out['dlc']['copy']))
    if 'caw' in out:
        print('caw: %d used' % len(out['caw']))
        for c in out['caw']:
            print('  slot %d %r short %r long %r stamp %d package %s' % (c['slot'], c['name'], c['short'], c['long'], c['stamp'], c['package']))
    if 'threads' in out:
        print('threads: %d characters with sets' % len(out['threads']))
        for x in out['threads']:
            print('  char %d sets %s default attire %d colour %d' % (x['char'], [(s['set'], s['slot'], s['name']) for s in x['sets']], x['default_attire'], x['default_colour']))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
