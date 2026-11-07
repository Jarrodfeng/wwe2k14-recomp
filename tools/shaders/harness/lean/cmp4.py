import numpy as np,sys
s=sys.argv[1]
a=np.fromfile(s+'_out_plain.bin',np.float32).reshape(-1,4); b=np.fromfile(s+'_out_lean.bin',np.float32).reshape(-1,4)
d=np.abs(a-b).max(1)
print(s,'nan plain',np.isnan(a).any(1).sum(),'nan lean',np.isnan(b).any(1).sum(),'max abs diff',np.nanmax(d),' n>1e-5:',(d>1e-5).sum(),' n>1e-3:',(d>1e-3).sum(),'of',len(a),' max |plain|',np.nanmax(np.abs(a)))
