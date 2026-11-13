"""Makes one synthetic song in every format the importer accepts (lane 283), for tools/audio/import_selftest.ps1 -Samples DIR.
Synthetic audio only (no game audio): a 12 s chord progression with a drum-like click train, quiet (-30 dBFS rms) so the
normaliser has to lift it, and a coloured square cover picture embedded where ffmpeg can (mp3, m4a, flac).
  py tools/audio/import_samples.py OUT_DIR       needs ffmpeg on PATH (developer tool)
Writes OUT_DIR/manifest.txt: 'file ok|refused|plays' per line (plays = imports but plays as it is, not normalised) (the self-test reads it)."""
import math, os, struct, subprocess, sys, wave, random

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
rate, secs = 44100, 12
rnd = random.Random(1)
frames = []
chords = [(220, 277, 330), (196, 247, 294), (175, 220, 262), (196, 247, 294)]
for i in range(rate * secs):
    t = i / rate
    ch = chords[int(t / 3) % 4]
    s = sum(math.sin(2 * math.pi * f * t) for f in ch) / 3 * 0.03
    s += 0.02 * math.sin(2 * math.pi * 3 * t) * (rnd.random() - 0.5)
    if i % (rate // 2) < 200:
        s += 0.05 * (rnd.random() - 0.5)
    frames.append((s, s * 0.9))
wav_path = os.path.join(out, 'song.wav')
with wave.open(wav_path, 'wb') as w:
    w.setnchannels(2), w.setsampwidth(2), w.setframerate(rate)
    w.writeframes(b''.join(struct.pack('<hh', int(a * 32767), int(b * 32767)) for a, b in frames))
# cover: 200x120 plain gradient PNG written by hand (no PIL needed)
import zlib
W, H = 200, 120
raw = b''.join(b'\x00' + b''.join(bytes((x * 255 // W, y * 255 // H, 90)) for x in range(W)) for y in range(H))
def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xFFFFFFFF)
cover = os.path.join(out, 'cover.png')
open(cover, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))

def ff(*args):
    subprocess.run(['ffmpeg', '-y', '-hide_banner', '-loglevel', 'error', *args], check=True)

meta = ['-metadata', 'title=Sample Song', '-metadata', 'artist=Sample Artist']
art = ['-i', cover, '-map', '0:a', '-map', '1:v', '-c:v', 'copy', '-disposition:v', 'attached_pic']
manifest = ['song.wav ok']
ff('-i', wav_path, '-c:a', 'libmp3lame', '-b:a', '128k', *meta, '-id3v2_version', '3', os.path.join(out, 'song.mp3')); manifest.append('song.mp3 ok')
ff('-i', wav_path, *art, '-c:a', 'libmp3lame', '-b:a', '128k', *meta, '-id3v2_version', '3', os.path.join(out, 'song_art.mp3')); manifest.append('song_art.mp3 ok')
ff('-i', wav_path, '-c:a', 'libvorbis', '-q:a', '4', *meta, os.path.join(out, 'song.ogg')); manifest.append('song.ogg ok')
ff('-i', wav_path, '-c:a', 'flac', *meta, os.path.join(out, 'song.flac')); manifest.append('song.flac ok')
ff('-i', wav_path, *art, '-c:a', 'flac', *meta, os.path.join(out, 'song_art.flac')); manifest.append('song_art.flac ok')
ff('-i', wav_path, '-c:a', 'aac', '-b:a', '128k', *meta, os.path.join(out, 'song.m4a')); manifest.append('song.m4a ok')
ff('-i', wav_path, *art, '-c:a', 'aac', '-b:a', '128k', *meta, os.path.join(out, 'song_art.m4a')); manifest.append('song_art.m4a ok')
ff('-i', wav_path, '-c:a', 'aac', '-b:a', '128k', '-f', 'adts', os.path.join(out, 'song.aac')); manifest.append('song.aac ok')
ff('-i', wav_path, '-c:a', 'libopus', '-b:a', '96k', *meta, os.path.join(out, 'song.opus')); manifest.append('song.opus ok')
ff('-i', wav_path, '-c:a', 'wmav2', '-b:a', '128k', *meta, os.path.join(out, 'song.wma')); manifest.append('song.wma ok')
# refused: garbage with an audio extension, an empty file, a text file, a silent wav
open(os.path.join(out, 'garbage.mp3'), 'wb').write(bytes(rnd.randrange(256) for _ in range(40000))); manifest.append('garbage.mp3 refused')
open(os.path.join(out, 'empty.ogg'), 'wb').close(); manifest.append('empty.ogg refused')
open(os.path.join(out, 'notes.m4a'), 'w').write('this is not audio'); manifest.append('notes.m4a refused')
with wave.open(os.path.join(out, 'silent.wav'), 'wb') as w:
    w.setnchannels(2), w.setsampwidth(2), w.setframerate(rate), w.writeframes(b'\x00\x00\x00\x00' * rate * 3)
manifest.append('silent.wav plays')
open(os.path.join(out, 'manifest.txt'), 'w').write('\n'.join(manifest) + '\n')
print('wrote', len(manifest), 'samples to', out)
