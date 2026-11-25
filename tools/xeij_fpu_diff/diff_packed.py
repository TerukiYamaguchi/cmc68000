import os, random, subprocess, sys
SP=sys.argv[1]; rng=random.Random(4242)
lines=[]
for _ in range(1500):
    e=rng.choice([rng.randint(1,0x7ffe),rng.randint(0x3f00,0x4100)]); s=(rng.getrandbits(1)<<15)|e
    lines.append("pout %d %d %04x %016x"%(rng.randint(0,3),rng.choice([-64,-17,-5,-3,-1,0,1,3,5,14,17,18]),s,rng.getrandbits(64)|1<<63))
for _ in range(1500):
    ed=[rng.randint(0,9) for _ in range(3)]
    w0=(rng.getrandbits(1)<<31)|(rng.getrandbits(1)<<30)|(ed[0]<<24)|(ed[1]<<20)|(ed[2]<<16)|rng.randint(0,9)
    fr=[rng.randint(0,9) for _ in range(16)]; f=0
    for d in fr: f=f<<4|d
    lines.append("pin %d %08x %08x %08x"%(rng.randint(0,3),w0,f>>32,f&0xffffffff))
inp="\n".join(lines)+"\n"
xe=subprocess.run(["java","-cp",os.environ["XEIJ_CLASS_DIR"]+":"+SP,"H2"],input=inp,capture_output=True,text=True).stdout.strip().split("\n")
me=subprocess.run([SP+"/drv2"],input=inp,capture_output=True,text=True).stdout.strip().split("\n")
d={"pout":[0,[]],"pin":[0,[]]}
for l,a,b in zip(lines,xe,me):
    k=l.split()[0]; d[k][0]+=1
    if a!=b and len(d[k][1])<40: d[k][1].append((l,a,b))
print(len(lines),len(xe),len(me))
for k,(n,l) in d.items():
    print(k,"n=%d diffs(shown<=40)=%d"%(n,len(l)))
    for x in l[:8]: print("  ",x)
