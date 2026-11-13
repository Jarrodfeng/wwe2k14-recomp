r"""Lane 155 (S101): compare two fview `--hash-frames` files (frame,hash per line): which frames' presented surface differs.
  py -I tools\crowd\hash_diff.py A.csv B.csv [--dec DECISIONS.csv]       prints `frames A, differing N: [first frames]`; exit 0 whatever the result (the caller judges)
With --dec the differing frames are split into frames the cut touched (their mask is non-zero) and frames it did not."""
import sys


def read(p):
    out = {}
    for line in open(p):
        line = line.strip()
        if line and ',' in line:
            a, b = line.split(',')
            out[a if a == 'format' else int(a)] = b
    return out


a, b = read(sys.argv[1]), read(sys.argv[2])
dec = {}
if '--dec' in sys.argv:
    for line in open(sys.argv[sys.argv.index('--dec') + 1]):
        if line.strip() and not line.startswith('#'):
            k, m = line.split(',')
            dec[int(k)] = int(m)
frames = [k for k in a if k != 'format']
diff = [k for k in frames if a[k] != b.get(k)]
print('frames %d, differing %d: %s' % (len(frames), len(diff), diff[:30]))
if dec:
    touched = [k for k in frames if dec.get(k, 0)]
    print('frames the cut touched %d, differing among them %d; differing frames the cut did not touch %d' % (len(touched), len([k for k in diff if dec.get(k, 0)]), len([k for k in diff if not dec.get(k, 0)])))
