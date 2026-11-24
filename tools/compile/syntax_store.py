"""Result store for game_syntax's per-unit syntax checks (S105 CC).

One file per key under %USERPROFILE%\\.cache\\wwe2k14-syntax (GAME_SYNTAX_CACHE_DIR overrides), shared by every worktree and every process.
Key = sha256 of `clang -E -P` of the unit (its whole preprocessed content: no paths, no mtimes) + the placeholder-normalised flags + the clang id
(computed by game_syntax.pp_key). Value = {rc, text}: a pass AND a failure are stored; a hit replays the same verdict and the same diagnostics.
Safety: the key is content, so two trees with different header content can never share an entry, and a fixed header changes the key (a stored
failure is never served for fixed text, a stored pass never for broken text). Writes: a temp file in the same folder, then os.replace (atomic;
never one shared file written by many). Each entry is written the moment its unit finishes, so a killed or timed-out run keeps its work.
LRU: a hit touches the entry's mtime; prune() deletes the oldest entries over the size cap (default 256 MB, GAME_SYNTAX_CACHE_MB) or the entry cap (default 20000, GAME_SYNTAX_CACHE_ENTRIES).
Only storable verdicts are written (game_syntax.storable: a pass, or exit 1 with a real `file:L:C: error:` line and no crash / OOM / timeout marker).
"""
import json
import os
import threading
import time

MAX_TEXT = 8000


def store_dir():
    d = os.environ.get('GAME_SYNTAX_CACHE_DIR')
    if not d:
        d = os.path.join(os.environ.get('USERPROFILE') or os.path.expanduser('~'), '.cache', 'wwe2k14-syntax')
    return d


def _path(key):
    return os.path.join(store_dir(), key[:2], key + '.json')


def get(key):
    """(rc, text) or None. Touches the entry (LRU). A damaged file is a miss."""
    p = _path(key)
    try:
        with open(p, 'r', encoding='utf-8') as f:
            d = json.load(f)
        rc, text = int(d['rc']), str(d.get('text', ''))
    except (OSError, ValueError, KeyError, TypeError):
        return None
    try:
        os.utime(p, None)
    except OSError:
        pass
    return rc, text


def put(key, rc, text):
    p = _path(key)
    tmp = '%s.%d.%d.tmp' % (p, os.getpid(), threading.get_ident())
    try:
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(tmp, 'w', encoding='utf-8') as f:
            json.dump({'rc': int(rc), 'text': (text or '')[:MAX_TEXT]}, f)
        for _ in range(25):
            try:
                os.replace(tmp, p)
                return True
            except OSError:
                time.sleep(0.1)   # a concurrent reader can hold the file for a moment on Windows
        os.remove(tmp)
    except OSError:
        pass
    return False


def prune(cap_mb=None, max_entries=None):
    """Delete the oldest entries (by mtime) while the store is over the cap. Returns the number removed. Races with other writers are harmless (every unlink is tried alone)."""
    if cap_mb is None:
        try:
            cap_mb = float(os.environ.get('GAME_SYNTAX_CACHE_MB', '256'))
        except ValueError:
            cap_mb = 256.0
    if max_entries is None:
        try:
            max_entries = int(os.environ.get('GAME_SYNTAX_CACHE_ENTRIES', '20000'))
        except ValueError:
            max_entries = 20000
    root, items, total = store_dir(), [], 0
    try:
        for sub in os.scandir(root):
            if not sub.is_dir():
                continue
            for e in os.scandir(sub.path):
                try:
                    st = e.stat()
                except OSError:
                    continue
                if e.name.endswith('.tmp') and time.time() - st.st_mtime > 3600:
                    try:
                        os.remove(e.path)
                    except OSError:
                        pass
                    continue
                if e.name.endswith('.json'):
                    items.append((st.st_mtime, st.st_size, e.path))
                    total += st.st_size
    except OSError:
        return 0
    n = 0
    items.sort()
    left = len(items)
    for _m, size, path in items:
        if total <= cap_mb * 1048576 and left <= max_entries:
            break
        try:
            os.remove(path)
            total -= size
            left -= 1
            n += 1
        except OSError:
            pass
    return n
