# Lane C sheet + checks (Session 103): reads the PNGs and the log c_driver wrote, builds one labelled sheet, checks every planted fault moved the picture, the rest-matrix
# rows left it unchanged, and that validation and magenta (the debug colour of an unbound material) are 0.
#   py -I tools\scene\c_sheet.py OUTDIR TAG        (the driver's stdout is OUTDIR\TAG_log.txt)
import sys, os, re
import numpy as np
from PIL import Image, ImageDraw
out, tag = sys.argv[1], sys.argv[2]
def load(n): return np.asarray(Image.open(os.path.join(out, f"{tag}_{n}.png")).convert("RGB")).astype(int)
def diff(a, b): return int((np.abs(a - b).max(axis=2) > 12).sum())
ok = True
# planted faults: (what, correct panel, planted panel, pixels that must differ)
for name, a, b, need in [("light set ids ignored", "dim", "dim_plant_light_sets", 2000), ("light set ids shifted by one", "dim", "dim_plant_shift", 500),
                         ("moving pieces ignored", "whole", "plant_slot_worlds", 2000), ("moving pieces' rotation dropped", "whole", "plant_rotation", 300),
                         ("S103 RGF: moving slots drawn after the static arena (the old order)", "whole_plain", "rest_rows_last", 1000),
                         ("screen texture never updated", "screens_next", "screens_plant", 300), ("flashes ignored", "whole", "plant_flashes", 100),
                         ("S103 C3: live flash rows drawn (vs none)", "flash_none", "flash_live", 100), ("S103 C3: live flash rows with y/z swapped in the guest", "flash_live", "flash_live_plant", 100)]:
    d = diff(load(a), load(b)); good = d >= need; ok &= good
    print(f"plant {name}: {d} pixels differ (need >= {need}) -> {'FAILS the check as required' if good else 'NOT DETECTED'}")
# the loader's own rest matrices as the rows must leave the picture unchanged
d = diff(load("whole_plain"), load("rest_rows")); good = d <= 300; ok &= good
print(f"rest-matrix rows vs no rows: {d} pixels differ (allowed <= 300) -> {'unchanged' if good else 'MOVED (the loader placement and the row are both applied?)'}")
# validation / magenta from the driver's log
log = open(os.path.join(out, f"{tag}_log.txt"), encoding="utf-8", errors="replace").read()
# S103 SOF: the arena's draw order (the game draws the stage array in ascending slot order; a moving slot sits where its meshes sit, the static meshes after it come after it). Tokens: S = a static segment, M<slot> = a moving slot.
def order_of(panel):
    m = re.search(r"arena order " + panel + r": ([SM0-9 ]+) \| rows (\d+)", log)
    return (m.group(1).split(), int(m.group(2))) if m else None
for panel, want_last in (("rest_rows", "S"), ("rest_rows_last", "M")):
    r = order_of(panel)
    o, nrows = r if r else (None, -1)
    slots = [int(t[1:]) for t in o if t[0] == "M"] if o else []
    # ascending, no slot twice, none dropped (one M per moving slot that has a row and a model)
    good = bool(o) and slots == sorted(set(slots)) and len(slots) == nrows and len(slots) >= 2 and o[-1][0] == want_last
    if panel == "rest_rows":   # every moving slot (the mirrored 111 and the plain 116 among them) sits before the static meshes that follow the last one (slot 162 is among them)
        good = good and o.count("S") >= 2 and o.index("M%d" % slots[-1]) < len(o) - 1
    ok &= good
    print(f"S103 SOF arena order {panel}: {' '.join(o) if o else 'MISSING'} -> {'as the game orders it' if panel == 'rest_rows' and good else 'the planted old order (moving slots last)' if good else 'WRONG'}")
# S103 FOB: the stage families' order (the game's, XFO.md G1-G5): the token line of whole_plain must be the game's; the planted old orders must fail it; the price of each difference is printed (tools\scene\fo_order.py)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fo_order
ok &= fo_order.run(log, lambda panel: (diff(load("whole_plain"), load(panel)), int((np.abs(load("whole_plain") - load(panel)).max(axis=2) > 0).sum())))
for m in re.finditer(r"panel (\S+) crc \S+ drawn_px \d+ magenta_px (\d+) validation (\d+)", log):
    if int(m.group(2)) or int(m.group(3)):
        ok = False; print(f"panel {m.group(1)}: magenta_px {m.group(2)} validation {m.group(3)} -> FAIL")
print("validation / magenta:", "all panels 0" if ok else "see above")
names = ["whole", "arena", "ring", "props", "screens", "flashes", "whole_plain", "rest_rows", "rest_rows_last", "dim", "dim_plant_light_sets", "dim_plant_shift", "plant_slot_worlds",
         "plant_rotation", "plant_screens", "screens_plant", "plant_flashes", "flash_none", "flash_live", "flash_live_plant", "fo_old_skirts", "fo_old_screens", "fo_old_rank", "fo_old_all"]
ims = [Image.open(os.path.join(out, f"{tag}_{n}.png")).convert("RGB") for n in names]
w, h = ims[0].size; sw, sh = w // 2, h // 2
sheet = Image.new("RGB", (sw * 4, (sh + 18) * ((len(names) + 3) // 4)), (20, 20, 20)); dr = ImageDraw.Draw(sheet)
for i, (n, im) in enumerate(zip(names, ims)):
    x, y = (i % 4) * sw, (i // 4) * (sh + 18)
    sheet.paste(im.resize((sw, sh)), (x, y + 18)); dr.text((x + 4, y + 3), f"{tag} {n}", fill=(255, 255, 255))
sheet.save(os.path.join(out, f"{tag}_sheet.png")); print("sheet", os.path.join(out, f"{tag}_sheet.png"))
sys.exit(0 if ok else 1)
