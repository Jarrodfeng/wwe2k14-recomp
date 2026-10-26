"""Session 67: read the label probe's dumps (probes/probes.cpp: out/s67/labels_<frame>.bin) and look for the fields
that hold each menu label's position.

    py tools/menus/label_dump.py out/s67/labels_3755.bin [--floats] [--words OFF,OFF,...]

Lists each element (address, text id, caller, text), then, with --floats, every 4-byte offset whose big-endian float
is finite, in [-5000, 5000] and not the same in all elements (a position differs per row); --words prints those
offsets' raw words and floats per element.
"""
import argparse
import math
import struct

REC_HEAD = 12 + 64


def read(path, dump=1600):
    data = open(path, "rb").read()
    size = REC_HEAD + dump
    out = []
    for i in range(0, len(data) - size + 1, size):
        elem, tid, caller = struct.unpack_from("<III", data, i)
        text = data[i + 12:i + 76].split(b"\0")[0].decode("ascii", "replace")
        out.append((elem, tid, caller, text, data[i + REC_HEAD:i + size]))
    return out


def f32(b, o):
    return struct.unpack_from(">f", b, o)[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--floats", action="store_true")
    ap.add_argument("--words", default="")
    ap.add_argument("--dump", type=int, default=1600)
    a = ap.parse_args()
    recs = read(a.file, a.dump)
    for e, tid, caller, text, _ in recs:
        print(f"{e:08X} id {tid:08X} caller {caller:08X} {text!r}")
    if a.floats:
        print("\noffsets whose float differs between elements:")
        for o in range(0, a.dump, 4):
            vals = [f32(b, o) for *_, b in recs]
            if all(math.isfinite(v) and abs(v) < 5000 for v in vals) and len({round(v, 3) for v in vals}) > 1 \
                    and any(abs(v) > 0.5 for v in vals):
                print(f"  +{o:4d}: " + " ".join(f"{v:8.2f}" for v in vals))
    if a.words:
        for o in [int(x, 0) for x in a.words.split(",")]:
            print(f"+{o}: " + " ".join(f"{struct.unpack_from('>I', b, o)[0]:08X}" for *_, b in recs))


if __name__ == "__main__":
    main()
