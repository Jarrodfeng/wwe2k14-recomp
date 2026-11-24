r"""S110 QREBUILD: the host linker map's symbols (`q addr --host`, the host RVA in `q addr` / `q fn`) in their OWN small file per map version.

The map changes with every relink of the exe (every check run links one); the game's code, which index.db describes, does not. Before S110 the map's symbols
lived in index.db and a changed map made every `addr` / `fn` lookup rebuild the whole index (13-40 s). Now the first call that needs host symbols after a relink
parses the map once into out\harness\index\hostmap.<size>.<mtime_ns>.db: a name that did not exist before, so putting it in place never replaces a file a reader
holds open. Older hostmap files are removed best-effort (one a reader holds goes on a later call).

    open_map(idx_dir, map_path=None) -> (sqlite connection, rows by name; or None, info)
        info: {'path': the map, 'timestamp': its link timestamp line, 'sections': [[rva_lo, rva_hi, name]], 'file': the hostmap file, 'error': why None}
"""
import glob
import json
import os
import sqlite3
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_index  # noqa: E402  (parse_map, map_timestamp, MAP)


def _build(map_path, st, target):
    """parse the map into `target`; -> None, or the reason it was not built"""
    sections = []
    syms = build_index.parse_map(map_path, sections)
    ts = build_index.map_timestamp(map_path)
    st2 = os.stat(map_path)
    if (st2.st_size, st2.st_mtime_ns) != (st.st_size, st.st_mtime_ns):
        return f'the map {map_path} changed while it was read (a link is running): ask again in a few seconds'
    if not syms:
        return f'no symbols parsed from {map_path}'
    tmp = f'{target}.{os.getpid()}.tmp'
    con = sqlite3.connect(tmp)
    try:
        con.executescript('PRAGMA journal_mode=OFF; PRAGMA synchronous=OFF;'
                          'CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT); CREATE TABLE syms(rva INTEGER, name TEXT, obj TEXT);')
        con.executemany('INSERT INTO syms VALUES (?,?,?)', syms)
        con.executescript('CREATE INDEX syms_rva ON syms(rva); CREATE INDEX syms_name ON syms(name);')
        con.executemany('INSERT INTO meta VALUES (?,?)', [('path', json.dumps(map_path)), ('timestamp', json.dumps(ts)), ('sections', json.dumps(sections))])
        con.commit()
    finally:
        con.close()
    try:
        os.replace(tmp, target)
    except OSError:
        try:                       # another process built this same map version a moment ago and holds it open: theirs is the same data
            os.remove(tmp)
        except OSError:
            pass
        if not os.path.exists(target):
            raise
    return None


def _prune(idx_dir, keep):
    for p in glob.glob(os.path.join(glob.escape(idx_dir), 'hostmap.*.db')):
        if os.path.normcase(p) != os.path.normcase(keep):
            try:
                os.remove(p)
            except OSError:
                pass
    for p in glob.glob(os.path.join(glob.escape(idx_dir), 'hostmap.*.tmp')):     # a killed parse's leftover
        try:
            if time.time() - os.path.getmtime(p) > 600:
                os.remove(p)
        except OSError:
            pass


def open_map(idx_dir, map_path=None):
    map_path = map_path or build_index.MAP
    info = {'path': map_path, 'timestamp': None, 'sections': [], 'file': None, 'error': None}
    try:
        st = os.stat(map_path)
    except OSError:
        info['error'] = f'no linker map at {map_path} (link the exe first)'
        return None, info
    target = os.path.join(idx_dir, f'hostmap.{st.st_size}.{st.st_mtime_ns}.db')
    if not os.path.exists(target):
        os.makedirs(idx_dir, exist_ok=True)
        err = _build(map_path, st, target)
        if err:
            info['error'] = err
            return None, info
        _prune(idx_dir, target)
    try:
        con = sqlite3.connect(f'file:{target}?mode=ro', uri=True)
        con.row_factory = sqlite3.Row
        m = {r['key']: json.loads(r['value']) for r in con.execute('SELECT key, value FROM meta')}
    except sqlite3.Error as e:      # pruned by a call that saw a newer map in between
        info['error'] = f'the map changed while it was read ({e}): ask again'
        return None, info
    info.update(timestamp=m.get('timestamp'), sections=m.get('sections') or [], file=target)
    return con, info
