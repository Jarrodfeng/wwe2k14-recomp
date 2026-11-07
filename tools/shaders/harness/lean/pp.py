import sys,re
src,defs,out=sys.argv[1],set(sys.argv[3:]),sys.argv[2]
import os
d=os.path.dirname(src)
def proc(path):
    res=[];stack=[]
    for line in open(path).read().split('\n'):
        m=re.match(r'#\s*(ifdef|ifndef|else|endif)\s*(\w*)',line)
        if m:
            k,n=m.groups()
            if k=='ifdef': stack.append((n in defs, True))
            elif k=='ifndef': stack.append((n not in defs, True))
            elif k=='else': stack[-1]=(not stack[-1][0],True)
            else: stack.pop()
            continue
        if not all(s for s,_ in stack): continue
        m=re.match(r'#include "(wrestler_lean.glsl)"',line)
        if m: res.append(proc(os.path.join(d,m.group(1)))); continue
        res.append(line)
    return '\n'.join(res)
open(out,'w').write(proc(src))
t=open(out).read()
t=t.replace('vec3((uv), 0.0), (bias))','vec3((uv), 0.0))')
t=t.replace('vec3(uv, 0.0), C201.w)','vec3(uv, 0.0))')
open(out,'w').write(t)
t=open(out).read()
t=re.sub(r'XeOutputColor\((.*)\);',r'XeOutputColor(\1);',t)
t=t.replace('XeOutputColor(vec4(color, in_vertex.w * hair.w));','XeOutputColor(vec4(color, in_vertex.w));')  # alpha is not part of this comparison
open(out,'w').write(t)
