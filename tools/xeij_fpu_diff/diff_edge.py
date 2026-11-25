import os, subprocess, sys, itertools
SP=sys.argv[1]
J=1<<63
S=[(0,0),(0x8000,0),(0x7fff,J),(0xffff,J),(0x7fff,J|1<<62|5),(0x7fff,J|1),(0x0000,1),(0x8000,3),(0x0000,J),(0x7ffe,2**64-1),
   (0x3fff,J),(0xbfff,J),(0x4000,J),(0x3ffe,J),(0x4002,0xA000000000000000),(0x4058,0xCECB8F27F4200F3A),(0x3fff,J|1),(0x3ffe,2**64-1),
   (0xc002,0xA000000000000000),(0x400e,J|1<<47),(0x4010,J),(0xc010,J),(0x400d,J),(0xc00d,0xC000000000000000),(0x0001,J),(0x3f00,J),(0xbf00,J)]
U=["sin","cos","tan","atan","asin","acos","sinh","cosh","tanh","atanh","etox","logn","log2","log10","etoxm1","lognp1","twotox","tentox","sqrt","getexp","getman"]
lines=[]
for fn in U:
    for m in (0,1):
        for e,s in S: lines.append("%s %d %04x %016x"%(fn,m,e,s))
for fn in ("rem","mod","scale"):
    for (a,b) in itertools.product(S,S): lines.append("%s 0 %04x %016x %04x %016x"%(fn,a[0],a[1],b[0],b[1]))
inp="\n".join(lines)+"\n"
xe=subprocess.run(["java","-cp",os.environ["XEIJ_CLASS_DIR"]+":"+SP,"Harness"],input=inp,capture_output=True,text=True).stdout.strip().split("\n")
me=subprocess.run([SP+"/drv"],input=inp,capture_output=True,text=True).stdout.strip().split("\n")
print(len(lines),len(xe),len(me))
agg={}
for ln,a,b in zip(lines,xe,me):
    fn=ln.split()[0]
    _,ae,asg,af=a.split(); _,be,bsg,bf=b.split()
    vd=(ae,asg)!=(be,bsg)
    fd=af!=bf
    # NaN payload differences are not interesting when both are NaN
    both_nan = ae[-4:]=="7fff" or int(ae,16)&0x7fff==0x7fff
    if vd and int(ae,16)&0x7fff==0x7fff and int(be,16)&0x7fff==0x7fff and asg!="0"*16 and bsg!="0"*16: vd=False
    if vd or fd:
        agg.setdefault(fn,[]).append((ln,a.split(None,1)[1],b.split(None,1)[1],"V" if vd else "", "F" if fd else ""))
def special(ln):
    t=ln.split()
    ops=[(int(t[2],16),int(t[3],16))]+([(int(t[4],16),int(t[5],16))] if len(t)>4 else [])
    return any((e&0x7fff)==0x7fff or s==0 for e,s in ops)
for fn,l in agg.items():
    l=[x for x in l if not special(x[0])]
    if not l: continue
    print(fn,len(l))
    for x in l[:6]: print("   ",x)
