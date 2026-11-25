"""PREC=S/D の演算・FMOVECR・FSGLMUL/FSGLDIV の差分（XEiJ と本実装）。使い方は README.md。"""
import os, random, subprocess, sys
from fractions import Fraction
SP = sys.argv[1]
rng = random.Random(9090)
U = ["sin", "cos", "tan", "atan", "asin", "acos", "sinh", "cosh", "tanh", "atanh", "etox", "logn", "log2", "log10",
     "etoxm1", "lognp1", "twotox", "tentox", "sqrt", "getexp", "getman"]
B = ["rem", "mod", "scale", "sglmul", "sgldiv"]
def rnd(lo, hi, positive=False):
    e = rng.randint(lo, hi)
    sg = 0 if positive else rng.getrandbits(1)
    return ((sg << 15) | (e + 16383), rng.getrandbits(64) | (1 << 63))
def un_in(fn):
    if fn in ("asin", "acos", "atanh"): return rnd(-30, -1)
    if fn in ("logn", "log2", "log10", "sqrt"): return rnd(-140, 140, True) if rng.random() < 0.7 else rnd(-1030, 1030, True)
    if fn == "lognp1": return rnd(-30, 10)
    if fn in ("getexp", "getman"): return rnd(-400, 400)
    if fn in ("etox", "sinh", "cosh", "etoxm1", "twotox", "tentox"): return rnd(-12, 8)   # 結果が S/D の範囲の内外
    if fn == "tanh": return rnd(-30, 4)
    return rnd(-30, 10)
lines = []
for prec in (1, 2):
    for fn in U:
        for _ in range(120):
            e, s = un_in(fn)
            lines.append("%s@%d %d %04x %016x" % (fn, prec, rng.randint(0, 3), e, s))
    for fn in B:
        for _ in range(160):
            if fn == "scale":
                a, b = rnd(-140, 140), rnd(0, 9)
            elif fn in ("sglmul", "sgldiv"):
                a, b = rnd(-200, 200), rnd(-200, 200)
            else:
                a, b = rnd(-30, 140), rnd(-30, 140)
            lines.append("%s@%d %d %04x %016x %04x %016x" % (fn, prec, rng.randint(0, 3), a[0], a[1], b[0], b[1]))
for fn in ("sglmul", "sgldiv"):                     # FSGL*: PREC は無視される。指数の大きい組（拡張精度の OVFL/UNFL）
    for _ in range(300):
        a, b = rnd(-16000, 16000), rnd(-16000, 16000)
        lines.append("%s@0 %d %04x %016x %04x %016x" % (fn, rng.randint(0, 3), a[0], a[1], b[0], b[1]))
for prec in (0, 1, 2):
    for mode in range(4):
        for off in list(range(0x00, 0x10)) + list(range(0x30, 0x40)):
            lines.append("movecr@%d %d %x" % (prec, mode, off))
inp = "\n".join(lines) + "\n"
xe = subprocess.run(["java", "-cp", os.environ["XEIJ_CLASS_DIR"] + ":" + SP, "Harness"], input=inp, capture_output=True,
                    text=True).stdout.strip().split("\n")
me = subprocess.run([SP + "/drv"], input=inp, capture_output=True, text=True).stdout.strip().split("\n")
assert len(xe) == len(me) == len(lines), (len(xe), len(me), len(lines))
def norm(e, s):
    if (int(e, 16) & 0x7FFF) == 0x7FFF and (int(s, 16) & ((1 << 63) - 1)) == 0: return (e, "inf")
    return (e, s)
agg = {}
for ln, a, b in zip(lines, xe, me):
    key = ln.split()[0]
    _, ae, asg, af = a.split(); _, be, bsg, bf = b.split()
    st = agg.setdefault(key, {"n": 0, "v": [], "f": []})
    st["n"] += 1
    if norm(ae, asg) != norm(be, bsg): st["v"].append((ln, a, b))
    elif af != bf: st["f"].append((ln, a, b))
for key, st in sorted(agg.items()):
    print("%-10s n=%d valuediff=%d flagdiff(値一致)=%d" % (key, st["n"], len(st["v"]), len(st["f"])))
    for kind in ("v", "f"):
        for x in st[kind][:2]: print("    %s:" % kind, x)
