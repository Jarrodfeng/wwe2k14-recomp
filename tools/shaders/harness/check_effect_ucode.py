r"""Session 100: finds a game shader's microcode (+ literal constants) by effect and hash, from the player's own pacs.
Cached under outs100\harness\ucode\<hash>.bin (rule 1: game data stays under out\). Used by check_effect.py."""
import json, os, struct, sys
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, os.path.join(REPO, 'tools', 'wm30'))
sys.path.insert(0, os.path.join(REPO, 'tools', 'shaders'))
sys.path.insert(0, os.path.join(REPO, 'tools', 'shaders', 'packets'))
CACHE = os.path.join(REPO, 'out', 's100', 'harness', 'ucode')

def cache_path(h):
    return os.path.join(CACHE, '%016X.bin' % h)

def named_path(h):
    return os.path.join(CACHE, '%016X.named.json' % h)

def named_float_registers(h):
    """The float registers the shader's constant table names, as a set."""
    return {r for first, count in json.load(open(named_path(h))) for r in range(first, first + count)}

def load_cached(h):
    p = cache_path(h)
    if not os.path.exists(p) or not os.path.exists(named_path(h)):
        return None
    b = open(p, 'rb').read()
    kind = 'VS' if b[0] == 0 else 'PS'
    nl, = struct.unpack_from('<I', b, 1)
    lits = [struct.unpack_from('>4f', b, 5 + 16 * i) for i in range(nl)]
    return kind, b[5 + 16 * nl:], lits

def scan(effect, pacs, wanted):
    import pac, shaderlib as sl, build_packets as bp
    found = {}
    def add_blob(name, b):
        if name != effect:
            return
        for off, kind, coff, code in sl.containers(b):
            h = sl.xxh3(code)
            if h in wanted and h not in found:
                vsize, psize = struct.unpack_from('>II', b, off + 4)
                desc = struct.unpack_from('>I', b, off + 24)[0]
                moff = struct.unpack_from('>I', b, off + desc)[0]
                found[h] = (kind, bytes(code), bp.literals(b, off, vsize, moff))
                os.makedirs(CACHE, exist_ok=True)
                json.dump([[r, c] for _, s, r, c in (bp.parse_ctab(b, off, vsize) or []) if s == 'float4'], open(named_path(h), 'w'))
    for pacname, rel in bp.PACS:
        if pacname not in pacs:
            continue
        data = pac.load(os.path.join(sl.GAME, rel.replace('/', os.sep)))
        for lname, leaf in pac.entries(data, 6):
            if b'\x10\x2a\x11' not in leaf:
                continue
            t = bp.effect_table_ok(leaf)
            if t:
                for n, o, s in t:
                    add_blob(n, leaf[o:o + s])
            else:
                add_blob('%s_%s' % (pacname, lname.replace('/', '_')), leaf)
    if 'xex' in pacs:
        img = bp.IMAGE if os.path.exists(bp.IMAGE) else os.path.join(sl.GAME, 'default.xex')
        add_blob('_xex_image', open(img, 'rb').read())
    os.makedirs(CACHE, exist_ok=True)
    for h, (kind, code, lits) in found.items():
        open(cache_path(h), 'wb').write(bytes([0 if kind == 'VS' else 1]) + struct.pack('<I', len(lits)) +
                                        b''.join(struct.pack('>4f', *l) for l in lits) + code)
    return found

def get(effect, hashes):
    """{hash(int): (kind, microcode bytes, [literal vec4])} for the hashes; scans the effect's pacs for the missing ones."""
    out, missing = {}, set()
    for h in hashes:
        c = load_cached(h)
        if c:
            out[h] = c
        else:
            missing.add(h)
    if missing:
        inv = json.load(open(os.path.join(REPO, 'db', 'shaders', 'inventory.json')))['effects']
        for h, v in scan(effect, set(inv.get(effect, {}).get('pacs', {})), missing).items():
            out[h] = v
    return out
