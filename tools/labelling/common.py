"""Shared helpers for the labelling pilot scripts (pick.py, export.py). Session 10, Track B.

GuestImage reads out\\harness\\index\\image.bin (export v2: data constants, data words pointing at a function).

Everything these scripts write is derived from the game (addresses, listings, strings) and goes to
out\\harness\\labelling\\ (gitignored) only.
"""
import glob
import json
import os
import re
import struct
import sys

REPO = r'C:\Users\Jarrod\wwe2k14-recomp'
OUT = os.path.join(REPO, 'out', 'harness', 'labelling')
PILOT = os.path.join(OUT, 'pilot.json')
COVERAGE = os.path.join(REPO, 'out', 'harness', 'coverage', 'coverage.json')
GEN = os.path.join(REPO, 'generated', 'default')
COMPARE_DB = 'match_setup.json'   # the db file whose 20 entries form the comparison batch (b10)

sys.path.insert(0, os.path.join(REPO, 'tools', 'q'))
import q  # noqa: E402  (tools/q/q.py: the code index queries, imported instead of one subprocess per call)


def h(a):
    return f'0x{a:08X}'


def connect():
    return q.connect(q.DB)


def index_warnings(con):
    """q's staleness check, without the map (host addresses are not used here). A changed codegen fingerprint with
    no generated source newer than the index (codegen reran, e.g. after a manifest edit, and rewrote only its stamps)
    is reported as a note: the indexed code is then still what is on disk."""
    warn = q.staleness(q.meta(con), ('init_cpp', 'image'))
    if any(w.startswith('codegen changed') for w in warn):
        built = os.stat(q.DB).st_mtime
        newer = [p for p in glob.glob(os.path.join(GEN, '*.cpp')) + glob.glob(os.path.join(GEN, '*.h'))
                 if os.stat(p).st_mtime > built]
        if not newer:
            rest = [w for w in warn if not w.startswith('codegen changed') and not w.startswith('rebuild:')]
            warn = rest + (['rebuild: py tools\\q\\build_index.py (~10 s)'] if rest else [])
            print('note: codegen fingerprint changed since the index was built, but no generated .cpp/.h is newer '
                  'than the index: the indexed code matches the files on disk', file=sys.stderr)
    return warn


def load_coverage():
    with open(COVERAGE, encoding='utf-8') as f:
        return json.load(f)


def db_entries():
    """[(addr, file, entry)] from db/*.json (db/ids/ holds id tables, not functions: not globbed)."""
    return q.load_db_entries()


def docs_addresses():
    """Addresses mentioned anywhere in docs/systems/*.md and docs/modules/*.md (sub_X / 0xX tokens)."""
    out = set()
    for p in glob.glob(os.path.join(REPO, 'docs', 'systems', '*.md')) + \
            glob.glob(os.path.join(REPO, 'docs', 'modules', '*.md')):
        with open(p, encoding='utf-8', errors='replace') as f:
            for m in re.finditer(r'(?:sub_|0x)([0-9A-Fa-f]{8})\b', f.read()):
                out.add(int(m.group(1), 16))
    return out


def callees(con, a):
    """Direct callees as q.py shows them (save/restore helpers hidden): {dst: [sites]}."""
    return q.callees_of(con, a, False)


def n_callers(con, a):
    return con.execute('SELECT COUNT(DISTINCT src) FROM edges WHERE dst = ?', (a,)).fetchone()[0]


IMAGE = os.path.join(REPO, 'out', 'harness', 'index', 'image.bin')


class GuestImage:
    """out\\harness\\index\\image.bin (tools/q/build_index.py's input, written by src/image_dump.h before guest code
    runs): the guest range [0x82000000, 0x82000000 + image size) as one flat dump, so guest address A is at file
    offset A - 0x82000000. The first bytes are the XEX basefile's PE header ('MZ' ... 'PE'); its section table gives
    each section's guest range: [VA, VA + raw size) holds initialised bytes, [VA + raw size, VA + virtual size) is
    BSS (zero at load). Every value read here is the value at load time; writable sections may change at run time.
    verify() checks all of that against the index before anything is read (see export.py)."""
    def __init__(self, path=IMAGE, base=q.IMAGE_BASE):
        with open(path, 'rb') as f:
            self.data = f.read()
        self.path, self.base = path, base
        self.sections = []   # (lo, hi_initialised, hi_virtual, name, characteristics)
        d = self.data
        if d[:2] == b'MZ':
            e = struct.unpack_from('<I', d, 0x3C)[0]
            if d[e:e + 4] == b'PE\0\0':
                nsec, opt = struct.unpack_from('<H', d, e + 6)[0], struct.unpack_from('<H', d, e + 20)[0]
                sh = e + 24 + opt
                for i in range(nsec):
                    name, vsize, va, raw = struct.unpack_from('<8sIII', d, sh + 40 * i)
                    ch = struct.unpack_from('<I', d, sh + 40 * i + 36)[0]
                    lo = base + va
                    self.sections.append((lo, lo + min(raw, vsize), lo + vsize,
                                          name.rstrip(b'\0').decode('ascii', 'replace'), ch))

    def end(self):
        return self.base + len(self.data)

    def inside(self, a, n=1):
        return self.base <= a and a + n <= self.end()

    def section(self, a):
        """(name, kind) with kind 'code' | 'rdata' (read-only data) | 'data' (writable, initialised) | 'bss' (zero at
        load) | None (in the image but in no section: PE header / padding) or ('', None) outside the image."""
        if not self.inside(a):
            return '', None
        for lo, hi_init, hi_v, name, ch in self.sections:
            if lo <= a < hi_v:
                if ch & 0x20:
                    return name, 'code'
                if a >= hi_init or ch & 0x80:
                    return name, 'bss'
                return name, 'data' if ch & 0x80000000 else 'rdata'
        return '(no section)', None

    def describe(self, a):
        name, kind = self.section(a)
        return {'code': f'{name}: code', 'rdata': f'{name}: read-only data', 'data': f'{name}: writable data',
                'bss': f'{name}: BSS, zero at load', None: name or 'outside the image'}[kind]

    def u32(self, a):
        return struct.unpack_from('>I', self.data, a - self.base)[0] if self.inside(a, 4) else None

    def raw(self, a, n):
        return self.data[a - self.base:a - self.base + n] if self.inside(a, n) else None

    def verify(self, con):
        """Checks that this file is the image the index was built from and that address A really is at offset
        A - base. Returns lines describing what was checked; raises SystemExit on any mismatch."""
        import hashlib
        m = q.meta(con)
        out, bad = [], []
        info = m.get('image') or {}
        sha = hashlib.sha256(self.data).hexdigest()
        if info.get('size') != len(self.data) or info.get('sha256') != sha:
            bad.append(f'image size/sha256 {len(self.data)}/{sha[:16]} differ from the index '
                       f'({info.get("size")}/{str(info.get("sha256"))[:16]})')
        else:
            out.append(f'image {self.path}: {len(self.data)} bytes (0x{len(self.data):X}), sha256 {sha[:16]}... '
                       f'= the image the index was built from')
        text = next((s for s in self.sections if s[3] == '.text'), None)
        lo = (m.get('code_range') or [None])[0]
        if not self.sections or text is None or text[0] != lo:
            bad.append(f'PE section table: .text {h(text[0]) if text else None} vs the index code range start '
                       f'{h(lo) if lo else None}')
        else:
            out.append(f'PE header at {h(self.base)}: {len(self.sections)} sections; .text starts {h(text[0])} = the '
                       f'index code range start (code from the generated files)')
        n = con.execute("SELECT COUNT(*) FROM strings WHERE enc = 'ascii' AND length(text) >= 12").fetchone()[0]
        for k in (n // 7, n // 2, n - n // 5):
            r = con.execute("SELECT addr, text FROM strings WHERE enc = 'ascii' AND length(text) >= 12 ORDER BY addr LIMIT 1 OFFSET ?",
                            (k,)).fetchone()
            got = self.raw(r['addr'], len(r['text']) + 1)
            if got != r['text'].encode('ascii') + b'\0':
                bad.append(f'string {h(r["addr"])}: image bytes {got!r} != index text {r["text"]!r}')
            else:
                out.append(f'string {h(r["addr"])} ({len(r["text"])} chars) decodes at offset 0x{r["addr"] - self.base:X}')
        if bad:
            raise SystemExit('image check FAILED: ' + '; '.join(bad))
        return out
