import os, random, subprocess, sys
from fractions import Fraction
SP=sys.argv[1]
rng=random.Random(777)
def rnd(lo,hi):
    e=rng.randint(lo,hi); return ((rng.getrandbits(1)<<15)|(e+16383), rng.getrandbits(64)|(1<<63))
def pos(lo,hi):
    e,s=rnd(lo,hi); return (e&0x7FFF,s)
IN={}
for fn in ("sin","cos","tan"): IN[fn]=[rnd(-30,10) for _ in range(300)]+[rnd(11,60) for _ in range(60)]
for fn in ("atan",): IN[fn]=[rnd(-30,20) for _ in range(300)]
for fn in ("asin","acos","atanh"): IN[fn]=[rnd(-30,-1) for _ in range(300)]
for fn in ("sinh","cosh","etox","etoxm1","twotox","tentox"): IN[fn]=[rnd(-30,8) for _ in range(300)]
IN["tanh"]=[rnd(-30,4) for _ in range(300)]
for fn in ("logn","log2","log10","sqrt","getexp","getman"): IN[fn]=[pos(-200,200) for _ in range(300)]
IN["lognp1"]=[rnd(-30,10) for _ in range(300)]
IN["lognp1"]=[(e,s) for e,s in IN["lognp1"] if not ((e>>15) and (e&0x7FFF)>=16383)]
BIN={}
for fn in ("rem","mod"): BIN[fn]=[(rnd(-20,30),rnd(-20,30)) for _ in range(300)]
BIN["scale"]=[(rnd(-50,50),rnd(0,8)) for _ in range(300)]
lines=[]
for fn,l in IN.items():
    for e,s in l: lines.append("%s %d %04x %016x"%(fn,rng.randint(0,3),e,s))
for fn,l in BIN.items():
    for (e,s),(e2,s2) in l: lines.append("%s %d %04x %016x %04x %016x"%(fn,rng.randint(0,3),e,s,e2,s2))
inp="\n".join(lines)+"\n"
xe=subprocess.run(["java","-cp",os.environ["XEIJ_CLASS_DIR"]+":"+SP,"Harness"],input=inp,capture_output=True,text=True).stdout.strip().split("\n")
me=subprocess.run([SP+"/drv"],input=inp,capture_output=True,text=True).stdout.strip().split("\n")
assert len(xe)==len(me)==len(lines),(len(xe),len(me),len(lines))
def val(e,s):
    if e&0x7FFF==0x7FFF: return None
    return (-1 if e>>15 else 1)*Fraction(s)*Fraction(2)**((e&0x7FFF)-16383-63)
stats={}
for ln,a,b in zip(lines,xe,me):
    fn=ln.split()[0]
    _,ae,asg,af=a.split(); _,be,bsg,bf=b.split()
    st=stats.setdefault(fn,{"n":0,"same":0,"maxulp":0,"flagdiff":0,"ex":[]})
    st["n"]+=1
    ae,asg,be,bsg=int(ae,16),int(asg,16),int(be,16),int(bsg,16)
    if (ae,asg)==(be,bsg): st["same"]+=1
    else:
        va,vb=val(ae,asg),val(be,bsg)
        if va is None or vb is None or va==0 or vb==0 or (ae>>15)!=(be>>15) and va*vb<=0:
            ulp=10**9
        else:
            u=Fraction(2)**((max(ae,be)&0x7FFF)-16383-63); ulp=abs(va-vb)/u
        st["maxulp"]=max(st["maxulp"],float(ulp))
        if len(st["ex"])<2: st["ex"].append((ln,a,b))
    if int(af,16)!=int(bf,16):
        st["flagdiff"]+=1
        if len(st.setdefault("fex",[]))<2: st["fex"].append((ln,af,bf))
for fn,st in stats.items():
    print("%-7s n=%d same=%d maxulp=%.1f flagdiff=%d"%(fn,st["n"],st["same"],st["maxulp"],st["flagdiff"]))
    for e in st["ex"][:1]: print("   val:",e)
    for e in st.get("fex",[])[:1]: print("   flg:",e)
