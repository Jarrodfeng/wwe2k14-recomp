r"""qasset_mods (S111 MODINDEX): the player's MODS folder in the asset index (`q asset`), read-only, so a mod's materials / textures are looked up and diffed like the game's.

Where the mods live and what the game reads of them (docs\research\s111\MODINDEX.md, from our code):
  the folder   the launcher's settings file (%APPDATA%\wwe2k14-launcher\settings.json: src\launcher\launcher_core.h SettingsPath) key `mods_dir`; empty = <game data>\..\mods
               (launcher_core.h FindPaths; the game's own default, src\mods.h Install) = game_data\mods in the dev layout.
  its pacs     superstars\frame\...          the Superstar Importer's FRAME pack: offered to the game while USE MY SUPERSTARS is on and frame\info\catalog.dlc exists (src\superstars.h Load)
               superstars\library\<KEY>\...  one imported character (card.txt `name`; pac\ch\*.pac): seated from the library while the frame is (src\seats_core.h)
               superstars\pack6\...          the OLD pack (before S110): never mounted any more (src\superstars.h Load: the launcher migrates it into the library)
               superstars\sources\*.pac      the community pacs the importer copied in: its input, never read by the game
               characters\*.pac              the Arena Viewer's custom characters (src\assets\char_mods.h): the viewer only
               anything else                 the game reads no other pac of the mods folder (src\assets\mods_lookup.h: titantron\, music\, lighting\, logos\, superstars\)
A mod pac is indexed under the path `mods/<its path inside the mods folder>`; its SOURCE is `mod:<name>` (superstars/frame, superstars/library/<KEY>, superstars/pack6,
superstars/sources/<stem>, characters/<stem>, else the first folder); every other pac is `game`. Whether the game reads a mod NOW is worked out when a query runs, from the
launcher's settings (a toggle flip changes no file, so nothing is re-read). Nothing is ever written into the mods folder (pac.load only reads; an XCTD unpack goes to out\wm30\cache).
Env: WWE_QASSET_MODS=<folder> ('none' = no mods) and WWE_QASSET_SETTINGS=<settings.json> point a test elsewhere; under WWE_QASSET_GAME (a fixture) the player's own settings
file is never read and the mods folder defaults to <fixture>\mods.
"""
import json
import os

PREFIX = 'mods/'
SOURCES = "all | game | mods | mod:NAME (NAME = a source name or its start: superstars, superstars/library/0000000A, characters/X)"
_SETTINGS = {}
_NAMES = {}


def settings_path():
    """the launcher's settings file ('' = none to read)"""
    if os.environ.get('WWE_QASSET_SETTINGS'):
        return os.environ['WWE_QASSET_SETTINGS']
    if os.environ.get('WWE_QASSET_GAME'):
        return ''
    ad = os.environ.get('APPDATA')
    return os.path.join(ad, 'wwe2k14-launcher', 'settings.json') if ad else ''


def settings():
    """the launcher's settings as a dict ({} when there is no file: the launcher's defaults then apply, USE MY SUPERSTARS off)"""
    p = settings_path()
    if p not in _SETTINGS:
        s = {}
        if p and os.path.isfile(p):
            try:
                with open(p, encoding='utf-8') as f:
                    s = json.load(f)
            except (OSError, ValueError):
                s = {}
        _SETTINGS[p] = s if isinstance(s, dict) else {}
    return _SETTINGS[p]


def mods_dir(game_dir):
    """the mods folder the launcher hands the game ('' = none): WWE_QASSET_MODS, else the settings' mods_dir, else <game data folder>\\..\\mods"""
    e = os.environ.get('WWE_QASSET_MODS')
    if e:
        return '' if e.lower() == 'none' else e
    s = settings()
    if s.get('mods_dir'):
        return str(s['mods_dir'])
    if s.get('game_data_dir'):
        return os.path.join(os.path.dirname(os.path.normpath(str(s['game_data_dir']))), 'mods')
    return os.path.join(game_dir, 'mods')


def _key(p):
    return os.path.normcase(os.path.abspath(p)).rstrip('\\/')


def _overlaps(a, b):
    a, b = _key(a), _key(b)
    return a == b or a.startswith(b + os.sep) or b.startswith(a + os.sep)


def scan(game_dir):
    """{'mods/<rel>': (abs path, size, mtime_ns)} of every .pac under the mods folder; {} without one, or when it overlaps the game's own pac folders (indexed as game already)"""
    d = mods_dir(game_dir)
    out = {}
    if not d or not os.path.isdir(d):
        return out
    if any(_overlaps(d, os.path.join(game_dir, t)) for t in ('extracted', 'dlc')):
        return out
    for dp, dn, fn in os.walk(d):
        for f in fn:
            if not f.lower().endswith('.pac'):
                continue
            fp = os.path.join(dp, f)
            try:
                st = os.stat(fp)
            except OSError:
                continue
            out[PREFIX + os.path.relpath(fp, d).replace('\\', '/')] = (fp, st.st_size, st.st_mtime_ns)
    return out


def is_mod(rel):
    return rel.startswith(PREFIX)


def _stem(f):
    return f[:-4] if f.lower().endswith('.pac') else f


def source(rel):
    """an index path -> 'game' or 'mod:<name>'"""
    if not is_mod(rel):
        return 'game'
    parts = rel[len(PREFIX):].split('/')
    low = [p.lower() for p in parts]
    if low[0] == 'superstars' and len(parts) > 2:
        if low[1] == 'library' and len(parts) > 3:
            return 'mod:superstars/library/' + parts[2]
        if low[1] == 'sources':
            return 'mod:superstars/sources/' + _stem(parts[-1])
        return 'mod:superstars/' + parts[1]
    if low[0] == 'characters' and len(parts) > 1:
        return 'mod:characters/' + _stem(parts[-1])
    return 'mod:' + (parts[0] if len(parts) > 1 else '(top folder)')


def state(src, game_dir):
    """-> (the game reads it now, why) for a source"""
    if src == 'game':
        return True, "the game's own files"
    name = src[4:]
    d = mods_dir(game_dir)
    if name == 'superstars/frame' or name.startswith('superstars/library/'):
        if not settings().get('superstars', False):
            return False, 'USE MY SUPERSTARS is off'
        if not (d and os.path.isfile(os.path.join(d, 'superstars', 'frame', 'info', 'catalog.dlc'))):
            return False, 'no frame built (superstars\\frame\\info\\catalog.dlc): the game seats no import'
        return True, 'USE MY SUPERSTARS is on'
    if name == 'superstars/pack6':
        return False, 'the old pack (pack6): no longer mounted, the launcher migrates it into the library'
    if name.startswith('superstars/sources/'):
        return False, "an importer source: never read by the game"
    if name.startswith('characters/'):
        return False, "an Arena Viewer custom character: the viewer only"
    return False, 'the game reads no pac of this folder'


def _read_text(p):
    try:
        with open(p, encoding='utf-8', errors='replace') as f:
            return f.read()
    except OSError:
        return ''


def name_of(rel, game_dir):
    """the character name the mod's own files carry ('' = none): a library card's `name` line; for the old pack6, pack.json's star of that slot (ch<N>_dlc..)"""
    if not is_mod(rel):
        return ''
    d = mods_dir(game_dir)
    parts = rel[len(PREFIX):].split('/')
    low = [p.lower() for p in parts]
    key = None
    if len(parts) > 3 and low[0] == 'superstars' and low[1] == 'library':
        key = ('card', parts[2])
        if key not in _NAMES:
            nm = ''
            for line in _read_text(os.path.join(d, 'superstars', 'library', parts[2], 'card.txt')).splitlines():
                if line.startswith('name '):
                    nm = line[5:].strip()
                    break
            _NAMES[key] = nm
    elif len(parts) > 2 and low[0] == 'superstars' and low[1] == 'pack6':
        stem = _stem(parts[-1]).lower()
        if not (stem.startswith('ch') and '_' in stem and stem[2:stem.index('_')].isdigit()):
            return ''
        slot = int(stem[2:stem.index('_')])
        key = ('pack6', slot)
        if key not in _NAMES:
            nm = ''
            try:
                for s in json.loads(_read_text(os.path.join(d, 'superstars', 'pack.json')) or '{}').get('stars', []):
                    if s.get('slot') == slot:
                        nm = str(s.get('name') or '')
            except (ValueError, AttributeError):
                nm = ''
            _NAMES[key] = nm
    return _NAMES.get(key, '') if key else ''


def tag(rel, game_dir):
    """'game', or `mod:<name> "<character>" [ON|off: why]` for a printed row"""
    src = source(rel)
    if src == 'game':
        return 'game'
    on, why = state(src, game_dir)
    nm = name_of(rel, game_dir)
    return '%s%s [%s: %s]' % (src, (' "%s"' % nm) if nm else '', 'ON' if on else 'off', why)


def where(spec, col='pacs.path'):
    """--source SPEC -> (an SQL condition on the pac path, its arguments). SPEC: all | game | mods | mod:NAME. ValueError for anything else."""
    s = (spec or 'all').strip()
    if s == 'all':
        return '1=1', []
    if s == 'game':
        return 'substr(%s, 1, %d) != ?' % (col, len(PREFIX)), [PREFIX]
    if s in ('mods', 'mod'):
        return 'substr(%s, 1, %d) = ?' % (col, len(PREFIX)), [PREFIX]
    if s.startswith('mod:') and len(s) > 4:
        p = PREFIX + s[4:]
        return 'substr(%s, 1, %d) = ?' % (col, len(p)), [p]
    raise ValueError('--source %r: give %s' % (spec, SOURCES))


def arg(spec):
    """argparse type= for --source"""
    try:
        where(spec)
    except ValueError as e:
        import argparse
        raise argparse.ArgumentTypeError(str(e))
    return spec


def summary(con, game_dir):
    """-> {'dir', 'settings', 'superstars', 'sources': [{source, pacs, models, materials, textures, on, why, names}]}: one row per mod source of the index"""
    by = {}
    for path, nmod, nmat in con.execute('SELECT pacs.path, COUNT(DISTINCT model.entry_id), COUNT(material.id) FROM pacs LEFT JOIN pach ON pach.pac_id = pacs.id '
                                        'LEFT JOIN entry ON entry.pach_id = pach.id LEFT JOIN model ON model.entry_id = entry.id '
                                        'LEFT JOIN material ON material.entry_id = entry.id WHERE substr(pacs.path, 1, ?) = ? GROUP BY pacs.id', (len(PREFIX), PREFIX)):
        r = by.setdefault(source(path), dict(pacs=0, models=0, materials=0, textures=0, names=set()))
        r['pacs'] += 1
        r['models'] += nmod
        r['materials'] += nmat
        nm = name_of(path, game_dir)
        if nm:
            r['names'].add(nm)
    for path, ntex in con.execute('SELECT pacs.path, COUNT(texture.id) FROM pacs JOIN pach ON pach.pac_id = pacs.id JOIN entry ON entry.pach_id = pach.id '
                                  'JOIN texture ON texture.entry_id = entry.id WHERE substr(pacs.path, 1, ?) = ? GROUP BY pacs.id', (len(PREFIX), PREFIX)):
        by.setdefault(source(path), dict(pacs=0, models=0, materials=0, textures=0, names=set()))['textures'] += ntex
    rows = []
    for src in sorted(by):
        on, why = state(src, game_dir)
        r = by[src]
        rows.append(dict(source=src, pacs=r['pacs'], models=r['models'], materials=r['materials'], textures=r['textures'], on=on, why=why, names=sorted(r['names'])))
    return dict(dir=mods_dir(game_dir), settings=settings_path(), superstars=bool(settings().get('superstars', False)), sources=rows)


def info_text(m):
    """the `asset info` lines of summary()"""
    out = ['mods folder: %s (settings %s; USE MY SUPERSTARS %s)' % (m['dir'] or '(none)', m['settings'] or '(none read)', 'on' if m['superstars'] else 'off')]
    if not m['sources']:
        out.append('  no mod pacs indexed')
    for r in m['sources']:
        out.append('  %s: %d pacs, %d models, %d materials, %d textures; %s (%s)%s' % (r['source'], r['pacs'], r['models'], r['materials'], r['textures'],
                                                                                 'ON' if r['on'] else 'off', r['why'], ('; names: ' + ', '.join(r['names'])) if r['names'] else ''))
    return '\n'.join(out)


def seed(path, schema, out):
    """Schema 5 = schema 4 + the mods folder's rows (the same tables, nothing else). A missing asset.s5.db of the default name is seeded from the asset.s4.db beside it
    (sqlite's backup API: a consistent copy even while an s4 writer runs; the s4 file is opened read-only and left as it is), so the first s5 query reads only the mods
    instead of the ~6 min cold build. -> the file it was seeded from, or '' (no seed: the build makes the file as before)."""
    import sqlite3
    if schema != 5 or os.path.exists(path) or os.path.basename(path) != 'asset.s5.db':
        return ''
    prev = os.path.join(os.path.dirname(path), 'asset.s4.db')
    if not os.path.isfile(prev):
        return ''
    tmp = '%s.seed%d' % (path, os.getpid())
    try:
        src = sqlite3.connect('file:%s?mode=ro' % prev, uri=True)
        try:
            row = src.execute("SELECT value FROM meta WHERE key='schema'").fetchone()
            if not row or str(row[0]) != '4':
                return ''
            dst = sqlite3.connect(tmp)
            try:
                src.backup(dst)
                dst.execute("UPDATE meta SET value='5' WHERE key='schema'")
                dst.commit()
            finally:
                dst.close()
        finally:
            src.close()
        os.replace(tmp, path)
    except (sqlite3.Error, OSError) as e:
        print('q asset: seeding %s from %s failed (%s): a full build instead' % (path, prev, e), file=out)
        try:
            os.remove(tmp)
        except OSError:
            pass
        return ''
    print('q asset: %s seeded from %s (the same tables; only the new and changed pacs, the mods folder\'s, are read now)' % (path, prev), file=out)
    return prev
