import numpy as np, sys
N = int(sys.argv[2]); seed = int(sys.argv[3]); out = sys.argv[1]
rng = np.random.default_rng(seed)
rec = np.zeros((N, 168), np.float32)
def U(a, b, n=None):
    return rng.uniform(a, b, size=(N,) if n is None else (N, n)).astype(np.float32)
def unit3():
    v = rng.normal(size=(N, 3)); v /= np.linalg.norm(v, axis=1, keepdims=True); return v.astype(np.float32)
I = [np.zeros((N, 4), np.float32) for _ in range(10)]
I[0][:, :3] = unit3() * U(0.7, 1.2, 1)[:, None] if False else unit3()
I[0][:, 3] = U(0, 1)
I[1][:, 0] = U(0, 1); I[1][:, 1] = U(0, 1); I[1][:, 2] = U(0, 1); I[1][:, 3] = U(0, 1)
I[2][:, :3] = unit3(); I[2][:, 3] = U(0, 1)
I[3][:, 0] = U(0, 1); I[3][:, 1:] = U(0, 1, 3)
w = U(0.5, 3)
I[4][:, 0] = U(0, 1) * w; I[4][:, 1] = U(0, 1) * w; I[4][:, 2] = U(0.2, 2.0); I[4][:, 3] = w
I[6][:, 0] = U(0, 1); I[6][:, 1] = U(0, 1); I[6][:, 2] = U(0, 1); I[6][:, 3] = U(0.2, 2)
I[8][:, :3] = U(0, 1, 3); I[8][:, 3] = U(0, 1)
for k in (5, 7, 9):
    I[k][:] = U(-1, 1, 4)
for k in range(10):
    rec[:, k*4:k*4+4] = I[k]
# constants by index: registers 4,12,52,53,54,142,144,161,162,166,168,169,170,180,181,182,187,189,190,195,197,204,205,206,208,218,234,246,252,253,254,255
regs = [4,12,52,53,54,142,144,161,162,166,168,169,170,180,181,182,187,189,190,195,197,204,205,206,208,218,234,246,252,253,254,255]
C = np.zeros((N, 32, 4), np.float32)
for i, r in enumerate(regs):
    C[:, i, :] = U(0.1, 1.5, 4)
def setc(r, vals):
    i = regs.index(r)
    for c, v in enumerate(vals):
        if v is not None:
            C[:, i, c] = v
# structured ones: directions and matrices
for r in (12, 166, 180):
    i = regs.index(r); v = rng.normal(size=(N, 3)); v /= np.linalg.norm(v, axis=1, keepdims=True); C[:, i, :3] = v
# rotation-ish rows
for r in (52, 53, 54):
    i = regs.index(r); C[:, i, :3] = rng.normal(size=(N, 3)).astype(np.float32)
setc(206, [U(0.5, 3), U(0, 1), U(0, 1), U(0, 1)])
setc(208, [U(0.5, 3), U(0, 1), U(0, 1), U(0, 1)])
setc(218, [U(0.3, 1.2), U(0.3, 1.2), U(0.5, 1.5), U(0, 1)])
# paint strength: half the records with zero (the else path)
cs = U(0.5, 3); cs[::2] = 0.0
setc(234, [cs, None, None, None])
# shadow range
setc(168, [U(0.0, 0.3), U(0.8, 2.0), U(0.0, 0.2), U(0.2, 1.0)])
setc(169, [None, U(0, 1), U(0.5, 3), U(0.5, 4)])
setc(170, [U(0.0, 0.3), U(0.5, 3), U(0.5, 4), U(0, 1)])
setc(161, [U(0.5, 3), U(0, 1), U(0, 2), None])
setc(205, [U(0.5, 2), U(0.5, 4), None, None])
setc(204, [None, None, None, U(0.5, 4)])
setc(162, [None, None, U(0, 2), None])
# the literal bank (guesses) with some noise on the unknowns
setc(252, [U(0, 0.001), None, None, None])
setc(253, [np.full(N, 2.0), U(0, 1.5), U(-2.5, 2.5), U(0, 1.5)])
setc(254, [np.full(N, 1.0), np.full(N, 1.5), np.full(N, 0.5), np.full(N, -1.0)])
setc(255, [U(-0.01, 0.01), U(-0.01, 0.01), U(-0.01, 0.01), U(-0.01, 0.01)])
# make the half of the records random literal bank to test the arithmetic form too
alt = np.arange(N) % 3 == 1
i = regs.index(254); C[alt, i, :] = U(-2, 2, 4)[alt]; C[alt, i, 1] = 1.5
i = regs.index(253); C[alt, i, :] = U(-2, 2, 4)[alt]
rec[:, 40:168] = C.reshape(N, 128)
rec.tofile(out)
print('wrote', out, rec.shape)
# special records: exactly vertical normals with a zero C252.x (the Xenos 0*inf case) every 7th record
sp = (np.arange(N) % 7 == 3)
rec2 = rec.copy()
rec2[sp, 0] = 0.0; rec2[sp, 2] = 0.0; rec2[sp, 1] = 1.0
i = regs.index(252)
rec2[sp, 40 + i*4] = 0.0
rec2.tofile(out)
print('special records', int(sp.sum()))
