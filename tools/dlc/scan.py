"""List the DLC packs in a folder (Session 32).

    py tools\\dlc\\scan.py [DIR] [--json]

DIR defaults to game_data\\dlc. A pack is a folder holding info\\catalog.dlc (file magic DLCC): the game mounts each pack
as dlcN: and reads dlcN:\\info/catalog.dlc (docs\\systems\\dlc.md). STFS packages (magic CON / LIVE / PIRS) are listed
too but are not read. Prints pack id, entry names and types, size, and the catalog's file list checked against the
folder. Prints names and counts only, never file contents (DLC is the player's own game content: CLAUDE.md rule 1).
"""

import argparse
import json
import os
import struct
import sys

HEADER_SIZE = 28
ENTRY_SIZE = 64


def parse_catalog(data):
    """Returns a dict, or raises ValueError with the reason."""
    if len(data) < HEADER_SIZE or data[:4] != b"DLCC":
        raise ValueError("not a DLC catalog (magic)")
    version, header_size, pack_id, unk_a, n_entries, unk_b, unk_c, n_files = struct.unpack_from("<8H", data, 4)
    if header_size != HEADER_SIZE:
        raise ValueError(f"unexpected header size {header_size}")
    entries = []
    for i in range(n_entries):
        o = HEADER_SIZE + ENTRY_SIZE * i
        if o + ENTRY_SIZE > len(data):
            raise ValueError("entry table runs past the end")
        size, eid, etype, epack, lic_bit, eid2 = struct.unpack_from("<6H", data, o)
        char_id = struct.unpack_from("<i", data, o + 16)[0]
        if size != ENTRY_SIZE:
            raise ValueError(f"entry {i}: size {size}")
        name = data[o + 32:o + 64].split(b"\0")[0].decode("latin-1")
        entries.append({"id": eid, "type": etype, "pack": epack, "licence_bit": lic_bit, "char_id": char_id,
                        "name": name})
    files = find_file_list(data, HEADER_SIZE + ENTRY_SIZE * n_entries, n_files)
    if files is None:
        raise ValueError("file list not found")
    return {"version": version, "pack_id": pack_id, "entries": entries, "files": files}


def find_file_list(data, start, count):
    """The catalog ends with `count` records: u16 record size, s16, s16, u8 name length, name (record size = 7 +
    length). The tables between the entries and the list are not decoded; find the start where `count` records chain
    and only zero padding follows."""
    if count == 0:
        return []
    for p in range(start, len(data) - 7):
        names, q = [], p
        for _ in range(count):
            if q + 7 > len(data):
                break
            size = struct.unpack_from("<H", data, q)[0]
            n = data[q + 6]
            if size != 7 + n or n == 0 or q + size > len(data):
                break
            raw = data[q + 7:q + size]
            if not all(32 <= c < 127 for c in raw):
                break
            names.append(raw.decode("ascii"))
            q += size
        if len(names) == count and not any(data[q:]):
            return names
    return None


def stfs_magic(path):
    try:
        with open(path, "rb") as f:
            m = f.read(4)
    except OSError:
        return None
    return m.decode("ascii") if m in (b"CON ", b"LIVE", b"PIRS") else None


def folder_size(path):
    total = 0
    for dirpath, _, names in os.walk(path):
        for n in names:
            try:
                total += os.path.getsize(os.path.join(dirpath, n))
            except OSError:
                pass
    return total


def scan(root):
    packs, others = [], []
    for dirpath, dirnames, names in os.walk(root):
        dirnames.sort()
        if os.path.basename(dirpath).lower() == "info" and "catalog.dlc" in [n.lower() for n in names]:
            pack_root = os.path.dirname(dirpath)
            cat = os.path.join(dirpath, next(n for n in names if n.lower() == "catalog.dlc"))
            rel = os.path.relpath(pack_root, root)
            rec = {"folder": rel, "size": folder_size(pack_root)}
            try:
                with open(cat, "rb") as f:
                    c = parse_catalog(f.read())
                missing = [p for p in c["files"] if not os.path.isfile(os.path.join(pack_root, p))]
                rec.update(pack_id=c["pack_id"], entries=c["entries"], listed=len(c["files"]), missing=missing,
                           status="ready" if not missing else "incomplete")
            except (OSError, ValueError) as e:
                rec.update(status="damaged", reason=str(e))
            packs.append(rec)
        for n in names:
            p = os.path.join(dirpath, n)
            m = stfs_magic(p)
            if m:
                others.append({"file": os.path.relpath(p, root), "kind": f"STFS package ({m.strip()})",
                               "size": os.path.getsize(p)})
            elif n.lower() == "catalog.dlc" and os.path.basename(dirpath).lower() != "info":
                others.append({"file": os.path.relpath(p, root), "kind": "catalog outside an info folder (not read)",
                               "size": os.path.getsize(p)})
    seen = {}
    for rec in packs:
        pid = rec.get("pack_id")
        if pid is None:
            continue
        if pid in seen:
            rec["status"] = f"duplicate of {seen[pid]}"
        else:
            seen[pid] = rec["folder"]
    return packs, others


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("dir", nargs="?", default=os.path.join("game_data", "dlc"))
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()
    if not os.path.isdir(a.dir):
        print(f"no folder: {a.dir}")
        return 1
    packs, others = scan(a.dir)
    if a.json:
        print(json.dumps({"packs": packs, "other": others}, indent=1))
        return 0
    for p in packs:
        print(f"{p['folder']}: pack {p.get('pack_id', '?')}, {p['size'] / 2**20:.1f} MB, {p['status']}"
              + (f" ({p['reason']})" if "reason" in p else ""))
        for e in p.get("entries", []):
            print(f"    entry {e['id']:3d} type {e['type']:3d} licence bit {e['licence_bit']} char {e['char_id']:4d}"
                  f"  {e['name']}")
        if "listed" in p:
            print(f"    files listed {p['listed']}, missing {len(p['missing'])}"
                  + (": " + ", ".join(p["missing"][:8]) if p["missing"] else ""))
    for o in others:
        print(f"{o['file']}: {o['kind']}, {o['size']} bytes")
    if not packs and not others:
        print("no packs found")
    return 0


if __name__ == "__main__":
    sys.exit(main())
