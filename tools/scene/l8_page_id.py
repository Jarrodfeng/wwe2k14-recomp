#!/usr/bin/env python3
r"""S103 L8: which file texture is each base image of the stage page pass? Compares the capture's image data (textures.bin) of the page pass's textures with every arena's
fade_lightmap00 / fade_lightmap01 / lightmap00 (the player's own pacs, read-only).
  py -I tools\scene\l8_page_id.py CAPDIR FRAME IMAGE [IMAGE...]
Prints, per image, the arena + texture name when the bytes are equal (join_census.compare_texture). Numbers only."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'arena'))
sys.path.insert(0, os.path.join(HERE, '..', 'solver'))
import arena_files as af  # noqa: E402
import join_census as jc  # noqa: E402


def main():
    cap = jc.Capture(sys.argv[1])
    last = int(sys.argv[2])
    want = [int(x) for x in sys.argv[3:]]
    for i in range(min(last + 1, len(cap.frames))):
        f = cap.frame(i)
        cap.advance_contents(f, i)
    for img in want:
        got = cap.image_data(img)
        if got is None:
            print('image', img, 'no data in the capture')
            continue
        print('image', img, 'bytes', len(got), 'subs', [s[:4] for s in got.subs][:4])
        for aid in af.arena_ids():
            try:
                a = af.load_arena(aid, False)
            except Exception as e:  # noqa: BLE001
                continue
            for name in ('fade_lightmap00', 'fade_lightmap01', 'lightmap00'):
                t = a.textures.get(name)
                if t is None:
                    continue
                r = jc.compare_texture(got, t)
                if r == 'equal':
                    print('   image %d == arena %d %s' % (img, aid, name))


if __name__ == '__main__':
    main()
