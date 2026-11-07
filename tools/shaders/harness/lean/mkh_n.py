import re,sys,subprocess
src=open(r'C:\Users\Jarrod\wwe2k14-recomp\tools\shaders\harness\mkharness.py').read()
t=open(sys.argv[2]).read()
N=int(re.search(r'vec4 c\[(\d+)\]',t).group(1))
src=src.replace('PER_THREAD = 168','PER_THREAD = %d'%(40+N*4)).replace('vec4 c[32]','vec4 c[%d]'%N).replace('k < 32u','k < %du'%N).replace('10u + k','10u + k')
open('mkharness_n.py','w').write(src)
