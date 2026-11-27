"""S111 EQUIVHOOK: the table of OUR hooks that pass straight through to the game's original under named flags, and the lint of it.

`equiv.py plan / run` refuses a target (or a callee in its closure) that one of our hooks sits on: the test would call our hook, not the game.
A hook whose every path before the call of the original is closed by a cvar the exe can be started with (`--wwe2k14_gfx_menu=false`) runs
the original and nothing else, so such a function is testable when the run passes those flags. This file is the one place that says which
hooks those are. A hook that is not listed is refused as before: there is no blanket bypass.

Each entry: hook address -> where the hook is, `flags` (cvar -> the value the run passes), `early_returns` (how many `return` the hook's text
holds before it calls the original: the answers it gives without the game, all closed by the flags or by state the headless tester run never
sets), `state` (names the hook tests before the call that are not cvars, each with why it is off in a tester run).

The lint (`check_entry`, run on the live src text by `allowed` and by equiv_selftest) fails an entry when:
  - the hook is not found in its file, or does not call `__imp__sub_X(ctx, base)`,
  - the cvars the hook reads (REXCVAR_GET) before that call are not exactly the entry's flags (a flag dropped from the entry, or a new cvar the
    hook began to read),
  - a `state` name no longer appears before the call, or the count of `return` before the call changed (a new early answer nobody listed).
Contains nothing from the game.
"""
import os
import re

PLANT = os.environ.get('EQUIV_PLANT', '')   # equiv_selftest's planted fault: the flag comparison skipped

TABLE = {
    # Flow table lookup (menu id, item index -> record): EnsureFlowPatched only while one of the two menu cvars is on, then the original.
    'sub_83424808': {
        'file': 'src\\gfx_menu.h',
        'flags': {'wwe2k14_gfx_menu': 'false', 'wwe2k14_quit_item': 'false'},
        'early_returns': 0,
        'state': {},
    },
    # Text manager GetString: our own ids are answered before the original; every one of those answers is closed as below.
    'sub_822563F8': {
        'file': 'src\\gfx_menu.h',
        'flags': {'wwe2k14_gfx_menu': 'false', 'wwe2k14_quit_item': 'false'},
        'early_returns': 4,
        'state': {
            'music_slots::NameFor': 'answers only when music_slots::Active() (the slot files of a mods folder)',
            'tron_slots::NameFor': 'answers only when tron_slots::Active() (a mods folder is set)',
            'g_music_hint_on': 'null until music_picker.h enables it',
            'g_photo_pause_item': 'false until photo_mode.h sets it from its cvars (default off)',
            'g_ramp_pause_item': 'false until ramp_menu.h sets it from its row (default off)',
        },
    },
}

HOOK_AT = r'\bREX_HOOK_RAW\(\s*%s\s*\)\s*\{'


def _blank_literals(text):
    """The text with comments and string / char literal bodies blanked (same length), so a brace or a word in them never counts."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith('//', i):
            j = text.find('\n', i)
            j = n if j < 0 else j
            out.append(' ' * (j - i))
            i = j
        elif text.startswith('/*', i):
            j = text.find('*/', i)
            j = n if j < 0 else j + 2
            out.append(re.sub(r'[^\n]', ' ', text[i:j]))
            i = j
        elif c in '"\'':
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == '\\' else 1
            out.append(c + ' ' * (j - i - 1) + c)
            i = j + 1
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def hook_text(text, fn):
    """(text of the hook's body before its call of the original, whether it calls the original), the body found by brace matching on the
    text with comments and literals blanked; None when the hook is not in `text`."""
    flat = _blank_literals(text)
    m = re.search(HOOK_AT % re.escape(fn), flat)
    if not m:
        return None
    depth, i = 1, m.end()
    while i < len(flat) and depth:
        depth += flat[i] == '{'
        depth -= flat[i] == '}'
        i += 1
    body = flat[m.end():i - 1]
    call = re.search(r'\b__imp__%s\s*\(\s*ctx\s*,\s*base\s*\)' % re.escape(fn), body)
    return (body[:call.start()], True) if call else (body, False)


def check_entry(fn, entry, texts):
    """[] when the entry holds against the live hook text, else the reasons it does not."""
    text = texts.get(entry['file'])
    if text is None:
        return [f'{fn}: {entry["file"]} is not in the scanned source']
    got = hook_text(text, fn)
    if got is None:
        return [f'{fn}: no REX_HOOK_RAW({fn}) in {entry["file"]}']
    before, calls = got
    why = []
    if not calls:
        why.append(f'{fn}: the hook in {entry["file"]} does not call __imp__{fn}(ctx, base)')
    read = set(re.findall(r'\bREXCVAR_GET\(\s*(\w+)\s*\)', before))
    if PLANT != 'passthrough_ignores_flags' and read != set(entry['flags']):
        why.append(f'{fn}: the hook reads {sorted(read)} before the original, the table lists flags {sorted(entry["flags"])}')
    for name in entry['state']:
        if name.split('::')[-1] not in before:
            why.append(f'{fn}: state name {name} no longer appears before the original')
    rets = len(re.findall(r'\breturn\b', before))
    if rets != entry['early_returns']:
        why.append(f'{fn}: {rets} return before the original, the table says {entry["early_returns"]} (a new early answer: list it or close it)')
    return why


def allowed(hooked, texts, table=None):
    """({sub_X: {cvar: value}} for each hooked function whose table entry holds, {sub_X: reasons} for each listed one that does not)."""
    ok, bad = {}, {}
    for fn, entry in (TABLE if table is None else table).items():
        if fn not in hooked:
            continue
        why = check_entry(fn, entry, texts)
        if why:
            bad[fn] = why
        else:
            ok[fn] = dict(entry['flags'])
    return ok, bad


def merge_flags(per_fn):
    """One {cvar: value} from the flags of every passthrough hook a test reaches; a conflict is an error text (second value)."""
    out = {}
    for fn, flags in sorted(per_fn.items()):
        for k, v in flags.items():
            if out.setdefault(k, v) != v:
                return out, f'{fn} wants {k}={v} but another hook in the closure wants {k}={out[k]}'
    return out, ''
