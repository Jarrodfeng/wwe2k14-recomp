#!/usr/bin/env python3
"""report_facts.py - the problem report's IN PLAY / PROBLEMS / SHORTFALLS / ERRORS / REPRODUCE blocks (S108 lane RPT; docs\\systems\\problem_report.md).

The Python twin of src\\launcher\\report_facts.h (the launcher's C++, which players run): both read the ONE table src\\launcher\\report_facts.inc
and print the same text; tools\\report\\findings_compare.py fails on any difference. triage.py feeds every line of a log to Facts.feed (the same
one pass that reads the findings) and prints Facts.render under each LOG line. Every list is capped (bounded memory); every printed value goes
through scrub() (a drive, UNC or absolute POSIX path keeps only its last name: `<path>\\NAME`), because the phone's zip has no Redact pass.
Only ASCII decisions are made (the C++ reads bytes): a non-ASCII character is never a separator, a digit or a letter here.
"""
# entry: Facts
import re
from pathlib import Path

TABLE_PATH = Path(__file__).resolve().parents[2] / "src" / "launcher" / "report_facts.inc"
TEXT_MAX = 4000
CAP = {"flags": 300, "route": 12, "chars": 200, "arenas": 50, "props": 200, "made": 200, "dlc": 50, "problems": 100, "errors": 100,
       "shortfalls": 300, "cast": 200}
LETTERS = frozenset("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz")
DIGITS = frozenset("0123456789")
ALNUM = LETTERS | DIGITS
WORD = ALNUM | frozenset("_-")              # a counter name's word characters
AFTER_NUM = ALNUM | frozenset("_.")         # a number followed by one of these is part of a word
WIN_STOP = frozenset("'\"\t|<>,;)")
POSIX_START_PREV = frozenset(" \t'\"(=[:,")
POSIX_STOP = frozenset(" \t'\",;)]")
TAG_HEADS = ("scene", "rdr", "host", "native", "wwe2k14")
RX_NUM = re.compile(r"-?[0-9]+(?:\.[0-9]+)?")
RX_SEQ = re.compile(r"\[wwe2k14\]\[scene\] sequence (\d+) -> (\d+) \((.*?)\) at frame (\d+)")
RX_FILE = re.compile(r"\[wwe2k14\]\[file\] Nt(?:Create|Open)File '([^']*)'(?: \(relative to handle 0x[0-9A-Fa-f]+\))? \S+ access=\S+ -> 0x([0-9A-Fa-f]+)")
RX_CARD = re.compile(r"card \(([^)]*)\): mode (-?\d+) type (-?\d+) arena (-?\d+)")
RX_PART = re.compile(r"(\d+):(\d+) \(slot (\d+)\)")
RX_SETUP = re.compile(r"setup: mode (-?\d+) type (-?\d+) pace (-?\d+) arena (-?\d+); rules ([0-9A-Fa-f]*); seats (.*?) frame=(\d+)")
RX_SETUP_MORE = re.compile(r"setup\+: seats (.*?) frame=(\d+)")
RX_SEAT = re.compile(r"(\d+):(\d+)/(cpu|p\d+)/a(\d+|\?)(?:/s(-?\d+))?")
RX_PROP = re.compile(r"event prop id (\d+) -> PACH (\S+) \(entry (\d+)\)")
RX_MADE = re.compile(r"event prop made: id (\d+) k (\d+)")
RUMBLE = 23


def load_table(path=TABLE_PATH):
    """-> [(kind, arg, text, writer)]. The file is a C++ raw string literal: its first and last lines are the wrapper."""
    lines = Path(path).read_text(encoding="utf8").splitlines()
    if len(lines) < 2 or not lines[0].startswith('R"RPT_TABLE(') or lines[-1].strip() != ')RPT_TABLE"':
        raise ValueError("%s: not the RPT_TABLE raw string" % path)
    rows = []
    for ln in lines[1:-1]:
        s = ln.strip()
        if not s or s.startswith("#"):
            continue
        parts = [p.strip() for p in s.split(" | ", 3)]
        if len(parts) != 4:
            raise ValueError("%s: a row needs KIND | ARG | TEXT | WRITER: %r" % (path, s))
        rows.append(tuple(parts))
    return rows


def norm(name):
    return re.sub(r"[ \-]+", "_", name.lower())


def _path_end(s, j, stop, posix):
    n = len(s)
    while j < n:
        c = s[j]
        if c in stop:
            break
        if not posix and (s.startswith(" (", j) or s.startswith(" [", j) or s.startswith(" -> ", j)):
            break
        j += 1
    return j


def _profile_tail(seg, k):
    """S108 RPT review: a path that IS a profile folder (...\\Users\\NAME, /home/NAME): its last name is the person's name. k = seg's last separator."""
    if k <= 0:
        return False
    j = max(seg.rfind("\\", 0, k), seg.rfind("/", 0, k))
    parent = "".join(chr(ord(ch) + 32) if "A" <= ch <= "Z" else ch for ch in seg[j + 1:k])
    return parent in ("users", "home")


def scrub(s):
    """Every drive (X:\\ or X:/), UNC (\\\\host) or absolute POSIX (/a/b, at a token start) path -> `<path>` + its last separator and name (USER for
    a profile folder's own name)."""
    out = []
    i, n = 0, len(s)
    while i < n:
        c = s[i]
        j = -1
        if c in LETTERS and i + 2 < n and s[i + 1] == ":" and s[i + 2] in "\\/" and (i == 0 or (s[i - 1] not in ALNUM and s[i - 1] != "_")):
            j = _path_end(s, i + 3, WIN_STOP, False)
            while j > i + 3 and s[j - 1] == " ":
                j -= 1
        elif c == "\\" and i + 2 < n and s[i + 1] == "\\" and s[i + 2] in ALNUM and (i == 0 or s[i - 1] != "\\"):
            j = _path_end(s, i + 2, WIN_STOP, False)
            while j > i + 2 and s[j - 1] == " ":
                j -= 1
        elif c == "/" and i + 1 < n and s[i + 1] in LETTERS and (i == 0 or s[i - 1] in POSIX_START_PREV):
            j = _path_end(s, i + 1, POSIX_STOP, True)
            if s.count("/", i, j) < 2:
                j = -1
        if j > i:
            seg = s[i:j]
            k = max(seg.rfind("\\"), seg.rfind("/"))
            out.append("<path>" + (seg[k] + "USER" if _profile_tail(seg, k) else seg[k:]))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def clean(s, n=TEXT_MAX):
    return scrub(s[:TEXT_MAX])[:n]


def fold_digits(s):
    return re.sub(r"[0-9]+", "#", s)


def level_of(line):
    """The SDK pattern `[stamp] [level] [logger] [tN] message`: (level, message) of a stamped line."""
    if line[25:27] != " [":
        return None, line
    j = line.find("]", 27)
    if j < 0:
        return None, line
    level = line[27:j]
    rest = line[j + 2:] if line[j + 1:j + 2] == " " else line[j + 1:]
    if rest.startswith("["):
        k = rest.find("] ")
        if k > 0 and rest[k + 2:k + 4] == "[t":
            k2 = rest.find("] ", k + 2)
            dg = rest[k + 4:k2] if k2 > 0 else ""
            if dg and all(ch in DIGITS for ch in dg):
                rest = rest[k2 + 2:]
    return level, rest


def frame_of(line):
    i = line.find("frame")
    while i >= 0:
        if (i == 0 or (line[i - 1] not in ALNUM and line[i - 1] != "_")) and line[i + 5:i + 6] in (" ", "=") and line[i + 6:i + 7] in DIGITS:
            j = i + 6
            while j < len(line) and line[j] in DIGITS:
                j += 1
            return line[i + 6:j]
        i = line.find("frame", i + 1)
    return None


def err_key(msg):
    out = []
    i, n = 0, len(msg)
    while i < n:
        if msg[i] == "0" and i + 1 < n and msg[i + 1] == "x":
            i += 2
            while i < n and msg[i] in "0123456789abcdefABCDEF":
                i += 1
            out.append(" ")
            continue
        out.append(msg[i] if msg[i] in LETTERS else " ")
        i += 1
    return " ".join("".join(out).split())[:60]


def tags_of(line):
    """[(start, end, tag)] of the census tags `[scene|rdr|host|native|wwe2k14][word]` (+ an optional `[word]`), leftmost, not overlapping."""
    out = []
    i, n = 0, len(line)
    while i < n:
        if line[i] == "[":
            e = _tag_at(line, i)
            if e > 0:
                out.append((i, e, line[i:e]))
                i = e
                continue
        i += 1
    return out


def _word_br(line, i):
    """`[word]` at i (word = ASCII alnum / _ , one or more) -> its end, else -1."""
    n = len(line)
    if i >= n or line[i] != "[":
        return -1
    j = i + 1
    while j < n and (line[j] in ALNUM or line[j] == "_"):
        j += 1
    if j == i + 1 or j >= n or line[j] != "]":
        return -1
    return j + 1


def _tag_at(line, i):
    for h in TAG_HEADS:
        if line.startswith("[" + h + "]", i):
            e = _word_br(line, i + len(h) + 2)
            if e < 0:
                return -1
            e2 = _word_br(line, e)
            return e2 if e2 > 0 else e
    return -1


def name_before(b, end):
    words = []
    j = end
    while True:
        k = j
        while k > 0 and b[k - 1] in WORD:
            k -= 1
        if k == j or (b[k] not in LETTERS and b[k] != "_"):
            break
        words.insert(0, b[k:j])
        if k >= 2 and b[k - 1] == " " and b[k - 2] in WORD:
            j = k - 1
            continue
        break
    return " ".join(words)


def counters_in(body):
    """[(name, number text)] of `WORDS N` / `WORDS=N` / `WORDS: N` / `WORDS:N` in one census block, in order."""
    out = []
    for m in RX_NUM.finditer(body):
        p, e = m.start(), m.end()
        if e < len(body) and body[e] in AFTER_NUM:
            continue
        if p > 0 and body[p - 1] == "=":
            ne = p - 1
        elif p > 1 and body[p - 1] == " " and body[p - 2] == ":":
            ne = p - 2
        elif p > 0 and body[p - 1] in ": ":
            ne = p - 1
        else:
            continue
        nm = name_before(body, ne)
        if nm:
            out.append((nm, m.group(0)))
    return out


class Facts:
    """One log's facts, fed line by line (one pass), bounded."""

    def __init__(self, table):
        self.rows = table
        k = lambda kind: [(a, t) for kk, a, t, _w in table if kk == kind]
        self.builds, self.rends, self.mods, self.modlines = k("build"), k("renderer"), k("mod"), k("modline")
        self.probs = [(a, kk == "problem#") for kk, a, _t, _w in table if kk in ("problem", "problem#")]
        self.shorts = [(norm(a), t) for a, t in k("shortfall")]
        self.role = {}
        for a, t in k("line"):
            self.role.setdefault(t, []).append(a)
        self.switch = {a: t for a, t in k("switch")}
        self.build = {}
        self.rend = set()
        self.first_ts = self.last_ts = None
        self.ended = False
        self.flags, self.flags_more = {}, 0
        self.modn = [0] * len(self.modlines)
        self.dlc, self.dlc_more = {}, 0
        self.route_head, self.route_tail, self.route_n, self.route_last, self.seq_lines = [], [], 0, None, 0
        self.mode = "(before the first sequence line)"
        self.scene_mark = None
        self.cards, self.card = 0, None
        self.cast, self.cast_more = {}, 0
        self.titles = None
        self.setups, self.setup = 0, None
        self.chars, self.chars_more, self.arenas, self.arenas_more = {}, 0, {}, 0
        self.props, self.props_more, self.made, self.made_more = {}, 0, {}, 0
        self.problems, self.problems_more = {}, 0
        self.errors, self.errors_more = {}, 0
        self.counters, self.counters_more = {}, 0
        self.last_tag = None

    def M(self, role):
        return self.role[role][0]

    # ---------------------------------------------------------------------------------------------------------------------------
    def feed(self, line, stamped, hm, last_part):
        level = None
        if stamped:
            ts = line[1:20]
            if self.first_ts is None:
                self.first_ts = ts
            self.last_ts = ts
            level, _msg = level_of(line)
        for i, (mk, _label) in enumerate(self.builds):
            if i not in self.build:
                p = line.find(mk)
                if p >= 0:
                    self.build[i] = clean(line[p + len(mk):].strip(), 300)
        for i, (mk, _t) in enumerate(self.rends):
            if mk in line:
                self.rend.add(i)
        for i, (mk, _t) in enumerate(self.modlines):
            if mk in line:
                self.modn[i] += 1
        if last_part and any(mk in line for mk in self.role["end"]):
            self.ended = True
        p = line.find(self.M("modified"))
        if p >= 0:
            self._flag(line[p + len(self.M("modified")):])
        if stamped and "[wwe2k14][scene] sequence " in line:
            m = RX_SEQ.search(line[:TEXT_MAX])
            if m:
                self._seq(int(m.group(2)), m.group(3))
        p = line.find(self.M("scene") + " ")
        if stamped and p >= 0 and " at frame " in line:
            t = line[p:]
            a = t.find(" at frame ")
            c = t.find(", ", a) if a >= 0 else -1
            self.scene_mark = (clean(t[:c] if c >= 0 else t, 300), hm)
        p = line.find(self.M("card"))
        if p >= 0:
            self._card(line[p:][:TEXT_MAX], hm)
        p = line.find(self.M("titles"))
        if p >= 0:
            self.titles = clean(line[p:], 600)
        p = line.find(self.M("setup"))
        if p >= 0:
            self._setup(line[p:][:TEXT_MAX], hm)
        p = line.find(self.M("setup_more"))
        if p >= 0:
            self._setup_more(line[p:][:TEXT_MAX])
        p = line.find(self.M("dlc"))
        if p >= 0:
            t = clean(line[line.find("pack", p):], 300)
            self.dlc_more += self._bump(self.dlc, t, CAP["dlc"])
        if self.M("prop") in line:
            m = RX_PROP.search(line[:TEXT_MAX])
            if m:
                self.props_more += self._bump(self.props, "event prop id %s -> PACH %s (entry %s)" % (m.group(1), clean(m.group(2), 40), m.group(3)), CAP["props"])
        if self.M("prop_made") in line:
            m = RX_MADE.search(line[:TEXT_MAX])
            if m:
                self.made_more += self._bump(self.made, "event prop made: id %s k %s" % m.groups(), CAP["made"])
        if ("NtCreateFile" in line or "NtOpenFile" in line) and "[file]" in line:
            m = RX_FILE.search(line)
            if m and m.group(2) == "00000000":
                base = "".join(ch.upper() if ch in LETTERS else ch for ch in re.split(r"[\\/]", m.group(1))[-1])
                if base.endswith(".PAC") and len(base) > 2 and base[2:3] in DIGITS:
                    if base.startswith("CH"):
                        self.chars_more += self._bump(self.chars, clean(base, 80), CAP["chars"])
                    elif base.startswith("BG"):
                        self.arenas_more += self._bump(self.arenas, clean(base, 80), CAP["arenas"])
        hit = False
        for mk, fold in self.probs:
            p = line.find(mk)
            if p >= 0:
                hit = True
                t = line[p:][:TEXT_MAX]
                key = fold_digits(t) if fold else t
                e = self.problems.get(key)
                if e is None:
                    if len(self.problems) >= CAP["problems"]:
                        self.problems_more += 1
                        break
                    e = self.problems[key] = {"n": 0, "hm": hm, "frame": frame_of(line), "mode": self.mode, "text": clean(t)}
                e["n"] += 1
                break
        if not hit and level in ("error", "critical"):
            _lv, msg = level_of(line)
            key = err_key(msg[:TEXT_MAX])
            e = self.errors.get(key)
            if e is None:
                if len(self.errors) >= CAP["errors"]:
                    self.errors_more += 1
                else:
                    e = self.errors[key] = {"n": 0, "hm": hm, "text": clean(msg, 300)}
            if e is not None:
                e["n"] += 1
        self._census(line, stamped, hm)

    @staticmethod
    def _bump(d, key, cap):
        if key in d:
            d[key] += 1
            return 0
        if len(d) >= cap:
            return 1
        d[key] = 1
        return 0

    def _flag(self, rest):
        rest = rest[1:] if rest.startswith(" ") else rest
        a = rest.find(" = '")
        b = rest.rfind("' (source: ")
        if a <= 0 or b < a + 4:
            return
        name, val = rest[:a], rest[a + 4:b]
        if name in self.flags:
            return
        if len(self.flags) >= CAP["flags"]:
            self.flags_more += 1
            return
        self.flags[name] = clean(val, 300)

    def _seq(self, b, name):
        self.seq_lines += 1
        self.mode = "%d %s" % (b, clean(name, 120))
        short = name.split(": ")[0]
        entry = "%d %s" % (b, clean(short, 80))
        if entry == self.route_last:
            return
        self.route_last = entry
        self.route_n += 1
        if len(self.route_head) < CAP["route"]:
            self.route_head.append(entry)
        else:
            self.route_tail.append(entry)
            if len(self.route_tail) > CAP["route"]:
                self.route_tail.pop(0)

    def _card(self, t, hm):
        m = RX_CARD.search(t)
        if not m:
            return
        self.cards += 1
        pi = t.find("participants ")
        plist = t[pi + 13:] if pi >= 0 else ""
        parts = []
        for pm in RX_PART.finditer(plist):
            seat, cid, key = int(pm.group(1)), int(pm.group(2)), int(pm.group(3))
            if seat >= 40:
                continue
            att = str((key // 10) % 10) if key // 100 == cid else "?"
            parts.append((seat, cid, att))
            ck = (cid, att)
            if ck not in self.cast:
                if len(self.cast) >= CAP["cast"]:
                    self.cast_more += 1
                else:
                    self.cast[ck] = 1
        self.card = {"when": clean(m.group(1), 80), "mode": int(m.group(2)), "type": int(m.group(3)), "arena": int(m.group(4)), "parts": parts, "hm": hm}

    def _setup(self, t, hm):
        m = RX_SETUP.search(t)
        if not m:
            return
        self.setups += 1
        rules = m.group(5)
        self.setup = {"mode": int(m.group(1)), "type": int(m.group(2)), "pace": int(m.group(3)), "arena": int(m.group(4)),
                      "rules": [int(rules[i:i + 2], 16) for i in range(0, 128, 2)] if len(rules) == 128 else None,
                      "seats": [], "hm": hm, "frame": m.group(7)}
        self._seats(m.group(6))

    def _setup_more(self, t):
        m = RX_SETUP_MORE.search(t)
        if m and self.setup is not None:
            self._seats(m.group(1))

    def _seats(self, text):
        for sm in RX_SEAT.finditer(text):
            if len(self.setup["seats"]) >= 64:
                break
            pad = None if sm.group(3) == "cpu" else sm.group(3)[1:]
            self.setup["seats"].append((int(sm.group(1)), int(sm.group(2)), pad, sm.group(4), sm.group(5)))

    def _census(self, line, stamped, hm):
        marks = tags_of(line)
        segs = []
        if marks:
            if marks[0][0] > 0 and self.last_tag is not None and not stamped:
                segs.append((self.last_tag, line[:marks[0][0]]))
            for i, (_s, e, tag) in enumerate(marks):
                end = marks[i + 1][0] if i + 1 < len(marks) else len(line)
                segs.append((tag, line[e:end]))
            self.last_tag = marks[-1][2]
        elif self.last_tag is not None and not stamped:
            segs.append((self.last_tag, line))
        if stamped and not marks:
            self.last_tag = None
        for tag, body in segs:
            seen = {}
            for nm, num in counters_in(body):
                n = norm(nm)
                meaning = None
                for ph, why in self.shorts:
                    if ("_" + ph + "_") in ("_" + n + "_"):
                        meaning = why
                        break
                if meaning is None:
                    continue
                seen[n] = seen.get(n, 0) + 1
                key = (tag, n if seen[n] == 1 else "%s#%d" % (n, seen[n]))
                c = self.counters.get(key)
                if c is None:
                    if len(self.counters) >= CAP["shortfalls"]:
                        self.counters_more += 1
                        continue
                    c = self.counters[key] = {"nz": None, "nz_hm": None, "last": num, "why": meaning}
                c["last"] = num
                if c["nz"] is None and float(num) != 0.0:
                    c["nz"], c["nz_hm"] = num, hm

    # ---------------------------------------------------------------------------------------------------------------------------
    def render(self, n_lines):
        o = []
        P = o.append
        more = lambda k: " (+%d more line(s), not listed)" % k if k else ""
        P("  IN PLAY (what this log says was in play; ids, names and option values only)")
        bits = ["%s %s" % (self.builds[i][1], self.build[i]) for i in range(len(self.builds)) if i in self.build]
        P("    build: " + ("; ".join(bits) if bits else "no version line in this log"))
        rb = [self.rends[i][1] for i in range(len(self.rends)) if i in self.rend]
        P("    renderer: " + ("; ".join(rb) if rb else "no stream-free marker: a stream build (an older release or a dev build)"))
        P("    run: %d lines, first %s, last %s; ended: %s" % (n_lines, self.first_ts or "none", self.last_ts or "none",
                                                           "yes (an end line in the newest part)" if self.ended else
                                                           "no (no Execution complete / Title terminated in the newest part: cut, killed, crashed or still running)"))
        P("    settings that differ from default: %s" % ("%d%s" % (len(self.flags), more(self.flags_more)) if self.flags else "none logged"))
        for nm, v in self.flags.items():
            P("      %s = %s" % (nm, v))
        on = ["%s (%s)" % (label, fl) for fl, label in self.mods if self.flags.get(fl, "") not in ("", "false", "0")]
        on += ["%s: %d line(s)" % (self.modlines[i][1], n) for i, n in enumerate(self.modn) if n]
        P("    mods on: " + (", ".join(on) if on else "none"))
        P("    DLC packs: " + ("%d" % len(self.dlc) if self.dlc else "none") + more(self.dlc_more))
        for t, n in self.dlc.items():
            P("      %s%s" % (t, " (x%d)" % n if n > 1 else ""))
        if self.route_n:
            mid = self.route_n - len(self.route_head) - len(self.route_tail)
            r = self.route_head + (["(%d more)" % mid] if mid > 0 else []) + self.route_tail
            P("    route (%d sequence line(s)): %s" % (self.seq_lines, " -> ".join(r)))
        else:
            P("    route: none (no sequence line)")
        P("    last scene mark: " + ("%s (at %s)" % self.scene_mark if self.scene_mark else "none"))
        if self.card:
            c = self.card
            P("    matches: %d card line(s); last (%s, at %s): mode %d type %d arena %d" % (self.cards, c["when"], c["hm"] or "?", c["mode"], c["type"], c["arena"]))
        else:
            P("    matches: none (no card line)")
        cast = ["%d attire %s%s" % (cid, att, " (a created superstar)" if cid < 100 else "") for cid, att in self.cast]
        P("    characters in the cards: " + (", ".join(cast) if cast else "none") + more(self.cast_more))
        P("    characters / attires opened: " + (", ".join(self.chars) if self.chars else "none") + more(self.chars_more))
        P("    arena opened: " + (", ".join(self.arenas) if self.arenas else "none") + more(self.arenas_more))
        for title, d, k in (("event props", self.props, self.props_more), ("event props made", self.made, self.made_more)):
            P("    %s: %s%s" % (title, "%d distinct" % len(d) if d else "none", more(k)))
            for t, n in d.items():
                P("      %s%s" % (t, " (x%d)" % n if n > 1 else ""))
        P("  PROBLEMS (whole lines from their marker on, deduplicated: count, first time / frame / mode)")
        for e in self.problems.values():
            P("    x%d first %s%s in %s: %s" % (e["n"], e["hm"] or "?", " frame %s" % e["frame"] if e["frame"] else "", e["mode"], e["text"]))
        if not self.problems:
            P("    none")
        if self.problems_more:
            P("    (+%d more line(s) of other problems, not listed)" % self.problems_more)
        P("  SHORTFALLS (census counters whose last value is not 0; not all are faults)")
        rows = [(k, c) for k, c in sorted(self.counters.items()) if float(c["last"]) != 0.0]
        for (tag, nm), c in rows:
            P("    %s %s: first non-zero %s at %s, last %s  (%s)" % (clean(tag, 120), nm, c["nz"], c["nz_hm"] or "?", c["last"], c["why"]))
        if not rows:
            P("    none")
        if self.counters_more:
            P("    (+%d more counter reading(s), not read)" % self.counters_more)
        P("  ERRORS (error / critical lines, grouped by their words)")
        for e in self.errors.values():
            P("    x%d first %s: %s" % (e["n"], e["hm"] or "?", e["text"]))
        if not self.errors:
            P("    none")
        if self.errors_more:
            P("    (+%d more line(s) of other groups, not listed)" % self.errors_more)
        self._reproduce(P)
        return o

    def _reproduce(self, P):
        P("  REPRODUCE (the last match set up in this log; ids and option values only)")
        s, c = self.setup, self.card
        if s is None and c is None:
            last = self.route_tail[-1] if self.route_tail else (self.route_head[-1] if self.route_head else None)
            P("    no match was set up in this log: the route above is the menu path; our launcher has no direct launch for %s" % (last or "this run"))
            return
        if s is not None:
            P("    from: the setup line at %s, frame %s (%d setup line(s) in this log)" % (s["hm"] or "?", s["frame"], self.setups))
            mode, typ, arena, pace, rules = s["mode"], s["type"], s["arena"], s["pace"], s["rules"]
            seats = s["seats"]
        else:
            P("    from: the card line at %s (no setup line in this log: a build before S108; pads, pace and rules are not known)" % (c["hm"] or "?"))
            mode, typ, arena, pace, rules = c["mode"], c["type"], c["arena"], None, None
            seats = [(seat, cid, None, att, None) for seat, cid, att in c["parts"]]
        P("    mode %d (%s)" % (mode, "exhibition" if mode == 0 else "not an exhibition: the launch line rebuilds the same match as an exhibition; the mode's own story / objectives are not rebuilt"))
        P("    type %d, pace %s, arena %d%s" % (typ, "?" if pace is None else pace, arena, " (the type's own)" if arena == 300 else ""))
        for seat, cid, pad, att, cset in seats:
            who = ("pad %s" % pad) if pad is not None else ("CPU" if s is not None else "pad ?")
            P("    seat %d: %d %s, attire %s%s%s" % (seat, cid, who, att, ", colour set %s" % cset if cset is not None else "",
                                                 ": a created superstar: the player's own; id %d on our PC is our own save's slot" % cid if cid < 100 else ""))
        if self.titles:
            P("    titles: " + self.titles)
        if [x[0] for x in seats] != list(range(len(seats))):
            P("    note: the seats are not 0..n-1: the launch line fills slots 0..n-1 in seat order")
        sw = self.switch
        if typ == RUMBLE:
            P("    launch line: none: a built Royal Rumble (type 23) does not load (docs\\systems\\match_setup.md:270): it needs a saved setup")
        else:
            toks = []
            for seat, cid, pad, att, cset in seats[:40]:
                # ID[:PAD[:ATTIRE]] (src\launch_participant.h): with no attire part, `ID:` alone is pad 0, so a CPU seat is the bare id (S108 RPT review)
                t = "%d" % cid
                if att != "?":
                    t += ":%s:%s" % (pad if pad is not None else "", att) + ("/%s" % cset if cset is not None else "")
                elif pad is not None:
                    t += ":%s" % pad
                toks.append(t)
            ln = "--%s=@quick --%s=%d" % (sw["launch"], sw["quick_type"], typ)
            if pace is not None:
                ln += " --%s=%d" % (sw["quick_pace"], pace)
            if 0 <= arena < 300:
                ln += " --%s=%d" % (sw["launch_arena"], arena)
            if toks:
                ln += " --%s=%s" % (sw["launch_participants"], ",".join(toks))
            if rules is not None:
                ln += " --%s=%s" % (sw["quick_rules"], ",".join("%d=%d" % (12 + i, b) for i, b in enumerate(rules)))
            P("    launch line: " + ln)
        P("    not carried: titles held, alternate looks, a created superstar's or arena's own data, the random draws%s; the run is not frame-exact"
          % ("" if rules is not None else ", the rule bytes (the type's defaults are used)"))

    def to_json(self, n_lines):
        return {"text": self.render(n_lines), "setups": self.setups, "cards": self.cards,
                "problems": [dict(v) for v in self.problems.values()], "flags": dict(self.flags)}
