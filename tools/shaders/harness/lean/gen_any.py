# usage: gen_any.py name  (reads <name>_plain.glsl) -> <name>_in.bin ; prints N
import re,sys,numpy as np
n=sys.argv[1]
t=open(n+'_plain.glsl').read()
N=int(re.search(r'vec4 c\[(\d+)\]',t).group(1))
reg={int(m.group(1)):int(m.group(2)) for m in re.finditer(r'#define C(\d+)\s+fc\.c\[(\d+)\]',t)}
M=65536
rng=np.random.default_rng(3)
P=40+N*4
a=np.zeros((M,P),np.float32)
U=lambda lo,hi,s: rng.uniform(lo,hi,size=s).astype(np.float32)
def unit(): 
    v=rng.normal(size=(M,3)); v/=np.linalg.norm(v,axis=1,keepdims=True); return v.astype(np.float32)
for k in range(10):
    a[:,k*4:k*4+4]=U(0,1,(M,4))
# directional interpolators: normal(0), view(2), tangent(6) as unit-ish
for k in (0,2,6): a[:,k*4:k*4+3]=unit()
a[:,3*4+3]=U(0.5,2,M)  # shadow w placeholder (loc differs per shader, harmless)
class _D(dict):
    pass
def c(r,k): return 40+reg[r]*4+k if r in reg else 0
for r,i in reg.items():
    a[:,40+i*4:44+i*4]=U(0.05,1.5,(M,4))
for r in (12,166,180):
    if r in reg: a[:,c(r,0):c(r,0)+3]=unit()
for r in (52,53,54):
    if r in reg: a[:,c(r,0):c(r,0)+3]=rng.normal(size=(M,3)).astype(np.float32)
for m in re.finditer(r'#define K_(\w+)\s+C(\d+)\.(\w)',t):
    nm,r,ch=m.group(1),int(m.group(2)),'xyzw'.index(m.group(3))
    v={'ONE':1.0,'HALF':0.5,'MINUS_ONE':-1.0,'EPS':0.0,'TWO':2.0}.get(nm)
    if v is not None and r in reg: a[:,c(r,ch)]=v
# plausible shadow range
a[:,c(168,0)]=U(0,0.3,M); a[:,c(168,1)]=U(0.8,2,M); a[:,c(168,2)]=U(0,0.2,M)
a[:,c(168,3)]=U(0.2,1,M)
i=np.arange(M)
a[i%5==0,c(168,3)]=0
a[i%7==0,c(161,2)]=0
a[i%11==0,c(197,3)]=0
a[i%13==0,c(142,0)]=0; a[i%13==0,c(142,2)]=0
a[i%17==0,c(142,0)]=0
a[i%19==0,c(169,3)]=0
a[i%23==0,c(234,0)]=0   # paint off
a[i%29==0,4*4+3]=0 if False else a[i%29==0,4*4+3]
a[i%31==0,1*4+3]=0
a.tofile(n+'_in.bin'); print(N,P)
