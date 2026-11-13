"""Test fixtures for the ONE combined custom-music live check (lane 303, 2026-11-13). Synthetic audio only: nothing from the game, no
copyrighted music; everything is generated here at run time under out\\l303 (gitignored, tidied by tools\\harness\\tidy.py).

  py tools\\audio\\music_live_fixtures.py [--out DIR] [--no-build]

What it makes (DIR default = <repo>\\out\\l303):
  DIR\\mods\\music\\343.wav                 Brock Lesnar's entrance theme replaced (music id 343, --wwe2k14_entrance_music=true): 75 s
  DIR\\mods\\music\\new\\l303_new_a.wav     a NEW song slot (id 6001, pinned in slots.txt; shown "L303 NEW A") for the Create-an-Entrance preview
  DIR\\mods\\music\\new\\l303_new_b.mp3     a second slot (id 6002; mp3 through ffmpeg when it is installed, else wav)
  DIR\\mods\\music\\library\\*.wav|mp3      3 songs of the USER PLAYLIST picker library (a non-empty library\\ means new\\ is not the library)
  DIR\\mods\\music\\.cache\\                written by the REAL importer (tools\\audio\\music_live_import.exe -> src\\audio_import):
                                          normalised copies, index.txt. Nothing in the cache is written by this script.
  DIR\\fixtures.json                       what the checker (music_live_check.py) and the run command need
The songs are given different native loudness (quiet to loud) so the normaliser has real work.
Prints the exact run command at the end (also in fixtures.json "run_command").
Needs: Python 3, clang-cl through tools\\env.ps1 (first build only), ffmpeg on PATH for the .mp3 files (optional)."""
import json, math, os, random, shutil, struct, subprocess, sys, wave

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
RATE = 44100


def song(path, secs, seed, rms_db, chords):
    """A chord progression with a click train and a little noise at the given rough RMS level (dBFS), 16-bit stereo WAV."""
    rnd = random.Random(seed)
    n = RATE * secs
    out = bytearray()
    peak_guess = 10 ** (rms_db / 20.0)
    two_pi = 2 * math.pi
    for i in range(n):
        t = i / RATE
        ch = chords[int(t / 3) % len(chords)]
        s = sum(math.sin(two_pi * f * t) for f in ch) / len(ch)
        s += 0.15 * (rnd.random() - 0.5)
        if i % (RATE // 2) < 220:
            s += 0.35 * (rnd.random() - 0.5)
        s *= peak_guess * 1.6
        l = max(-1.0, min(1.0, s)) * 32767
        r = max(-1.0, min(1.0, s * 0.92 + 0.02 * math.sin(two_pi * 0.7 * t))) * 32767
        out += struct.pack('<hh', int(l), int(r))
    with wave.open(path, 'wb') as w:
        w.setnchannels(2), w.setsampwidth(2), w.setframerate(RATE)
        w.writeframes(bytes(out))


def to_mp3(wav, mp3):
    ff = shutil.which('ffmpeg')
    if not ff:
        return False
    r = subprocess.run([ff, '-y', '-hide_banner', '-loglevel', 'error', '-i', wav, '-c:a', 'libmp3lame', '-b:a', '128k', mp3])
    return r.returncode == 0 and os.path.isfile(mp3)


def main():
    out = os.path.join(ROOT, 'out', 'l303')
    build = True
    a = sys.argv[1:]
    while a:
        x = a.pop(0)
        if x == '--out':
            out = os.path.abspath(a.pop(0))
        elif x == '--no-build':
            build = False
        else:
            sys.exit(__doc__)
    out = os.path.abspath(out)
    if os.path.basename(out).lower() != 'l303' and 'l303' not in out.lower():
        sys.exit('refusing to wipe %s: the fixtures folder must have l303 in its name' % out)
    mods = os.path.join(out, 'mods')
    if os.path.isdir(mods):
        shutil.rmtree(mods)
    for sub in ('', 'new', 'library'):
        os.makedirs(os.path.join(mods, 'music', sub), exist_ok=True)
    run_dir = os.path.join(out, 'run')
    os.makedirs(run_dir, exist_ok=True)
    music = os.path.join(mods, 'music')

    cm = [(220, 277, 330), (196, 247, 294), (175, 220, 262), (196, 247, 294)]
    cx = [(262, 330, 392), (233, 294, 349), (196, 247, 294), (220, 277, 330)]
    cl = [(147, 185, 220), (165, 207, 247), (131, 165, 196), (147, 185, 220)]
    print('generating songs...')
    song(os.path.join(music, '343.wav'), 75, 1, -26.0, cm)               # quiet source: lifted by the importer
    song(os.path.join(music, 'new', 'l303_new_a.wav'), 20, 2, -12.0, cx)  # loud source: pulled down
    nb_wav = os.path.join(out, 'l303_new_b.wav')
    song(nb_wav, 20, 3, -20.0, cl)
    nb = os.path.join(music, 'new', 'l303_new_b.mp3')
    if not to_mp3(nb_wav, nb):
        nb = os.path.join(music, 'new', 'l303_new_b.wav')
        shutil.move(nb_wav, nb)
    elif os.path.isfile(nb_wav):
        os.remove(nb_wav)
    lib = []
    for i, (secs, seed, db, ch) in enumerate([(24, 11, -18.0, cm), (22, 12, -9.0, cx), (21, 13, -30.0, cl)], 1):
        w = os.path.join(music, 'library', 'l303_lib_%d.wav' % i)
        song(w, secs, seed, db, ch)
        if i == 2 and to_mp3(w, w[:-4] + '.mp3'):  # the second library song as an mp3
            os.remove(w)
            w = w[:-4] + '.mp3'
        lib.append(w)
    # pinned slot ids (file-name order would give the same; written so the checker does not guess)
    with open(os.path.join(music, 'new', 'slots.txt'), 'w', newline='\n') as f:
        f.write('6001\t%s\n6002\t%s\n' % (os.path.basename(os.path.join(music, 'new', 'l303_new_a.wav')), os.path.basename(nb)))

    exe = os.path.join(ROOT, 'out', 'audio_import', 'music_live_import.exe')
    if build or not os.path.isfile(exe):
        print('building the importer CLI...')
        r = subprocess.run(['pwsh', '-NoProfile', '-File', os.path.join(HERE, 'music_live_import_build.ps1')], capture_output=True, text=True)
        if r.returncode != 0 or not os.path.isfile(exe):
            sys.exit('importer build failed:\n' + r.stdout[-2000:] + r.stderr[-2000:])
    print('importing with the real importer (index.txt included)...')
    r = subprocess.run([exe, mods], capture_output=True, text=True)
    rows = []
    for line in r.stdout.splitlines():
        c = line.split('\t')
        if c[0] == 'FILE' and len(c) >= 7:
            rows.append(dict(file=c[1], status=c[2], game_plays=c[3], out_lufs=float(c[4]), true_peak_db=float(c[5]), seconds=float(c[6]),
                             note=c[7] if len(c) > 7 else ''))
        elif line.strip():
            print('  ' + line)
    bad = [x for x in rows if x['status'] != 'ok' or not x['game_plays'].lower().endswith('.wav') or '.cache' not in x['game_plays']]
    for x in rows:
        print('  %-8s %-34s -> %s (%.1f LUFS, %.1f dBTP)' % (x['status'], os.path.relpath(x['file'], music), os.path.basename(x['game_plays']), x['out_lufs'], x['true_peak_db']))
    if r.returncode != 0 or bad or len(rows) != 6 + (0):
        sys.exit('the importer did not produce 6 ok cache copies (343, 2 new, 3 library): %d row(s), %d bad' % (len(rows), len(bad)))
    index = os.path.join(music, '.cache', 'index.txt')
    if not os.path.isfile(index):
        sys.exit('no index.txt after the import: ' + index)

    capture = os.path.join(run_dir, 'a.wav')
    settings = os.path.join(ROOT, 'out', 'owned_check', 'settings_native720.json')
    extra = '--wwe2k14_mods_dir=%s --wwe2k14_audio_capture=%s --wwe2k14_settings_file=%s' % (mods, capture, settings)
    cmd = ("& .\\tools\\bench\\run_list.ps1 -Scripts @('tools\\input_scripts\\scenes\\music_live_check.txt') -WaitSeconds 1200 -Extra '%s'" % extra)
    manifest = dict(
        made_by='tools/audio/music_live_fixtures.py (lane 303)', out=out, mods_dir=mods, music_dir=music, cache_dir=os.path.join(music, '.cache'),
        index_txt=index, capture=capture, stamps=capture[:-4] + '.stamps.csv', choices_file=os.path.join(music, 'library', 'choices.txt'),
        theme_id=343, stock_control_id=324, new_ids=[6001, 6002], new_shown=['L303 NEW A', os.path.splitext(os.path.basename(nb))[0].replace('_', ' ').upper()],
        library_files=[os.path.basename(x) for x in lib], library_count=len(lib), new_count=2, stock_list_records=123,
        import_rows=rows, run_command=cmd, settings_file=settings)
    with open(os.path.join(out, 'fixtures.json'), 'w') as f:
        json.dump(manifest, f, indent=1)
    print('\nfixtures ready in', out)
    print('\nRUN (from the repo root, with the lane-283 + lane-288 + lane-298 build; the run takes ~6 min at normal speed):\n  ' + cmd)
    print('\nCHECK (after the run, N = the run number it printed):\n  py tools\\audio\\music_live_check.py --run N --manifest %s' % os.path.join(out, 'fixtures.json'))


if __name__ == '__main__':
    main()
