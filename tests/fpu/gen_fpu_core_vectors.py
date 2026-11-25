#!/usr/bin/env python3
"""FPU共通コア Phase A のテストベクタ生成。

期待値は production のビット操作を写さず、fractions.Fraction による厳密な有理数演算で
求める。値の定義は [881UM Table 3-3] の式だけを使う:

    value = (-1)^s * signif * 2^(e - 16383 - 63)        （e=0 も同じ式）

丸めは「指数が emin 以上なら p ビットの有効数字、emin 未満なら 2^(emin-(p-1)) の格子」
という IEEE754 と同じ格子への丸めで、tie は偶数へ、RZ/RM/RP は方向丸め。
S/D の結果は拡張精度の正規化数として表す（設計書 §3.3.1）。

生成物（tests/fpu/vectors/*.txt）はコミットする。再生成:
    python3 tests/fpu/gen_fpu_core_vectors.py
各ファイルの先頭行 `count N` は行数で、test_fpu_core.c が実行件数と照合する。
コミット済みの生成物が最新かは `--check`（ctest の test_fpu_vectors_fresh）で確認する。
"""
import os
import random
import sys
from fractions import Fraction

BIAS = 16383
J = 1 << 63
QUIET = 1 << 62
FRAC_MASK = J - 1
MASK64 = (1 << 64) - 1

RN, RZ, RM, RP = 0, 1, 2, 3
MODES = (RN, RZ, RM, RP)

# 精度: (rounding_precision, 有効ビット数, 最小正規化数の指数E, 最大指数E)
FORMATS = {
    80: (64, -16383, 16383),
    32: (24, -126, 127),
    64: (53, -1022, 1023),
}
# roundPack に渡す指数（アイデアライズド exp = E + BIAS）の下限
MIN_NORM_EXP = {prec: FORMATS[prec][1] + BIAS for prec in FORMATS}

FLAG_INEXACT, FLAG_UNDERFLOW, FLAG_OVERFLOW = 1, 2, 4


def floor_log2(x):
    """x > 0 の Fraction に対する floor(log2 x)。"""
    n, d = x.numerator, x.denominator
    e = n.bit_length() - d.bit_length()
    if Fraction(n, d) < Fraction(2) ** e:
        e -= 1
    return e


def value_of(sign_exp, signif):
    """有限値（NaN/Inf を除く）の厳密な値。"""
    s = -1 if sign_exp >> 15 else 1
    e = sign_exp & 0x7FFF
    return s * Fraction(signif) * Fraction(2) ** (e - BIAS - 63)


def encode_x(sign, mag):
    """非負の Fraction mag を Motorola X形式の正規表現に符号化する（厳密に表せること）。"""
    if mag == 0:
        return (sign << 15, 0)
    e_unb = floor_log2(mag)
    e = max(e_unb + BIAS, 0)
    if e == 0:
        signif = mag * Fraction(2) ** (BIAS + 63)
    else:
        signif = mag * Fraction(2) ** (63 - e_unb)
    if signif.denominator != 1 or not 0 <= signif < (1 << 64):
        raise ValueError("拡張精度で厳密に表せない: %s" % mag)
    return ((sign << 15) | e, int(signif))


def round_increment(sign, mode, frac, odd):
    """frac(0<=frac<1) を捨てるときに1つ上げるか。"""
    if frac == 0:
        return False
    if mode == RZ:
        return False
    if mode == RM:
        return sign == 1
    if mode == RP:
        return sign == 0
    half = Fraction(1, 2)
    return frac > half or (frac == half and odd)


def round_to_grid(sign, mag, prec_bits, emin, mode):
    """mag(>0) を格子へ丸める。戻り値は (丸め後の大きさ, inexact)。指数の上限は見ない。"""
    e_unb = max(floor_log2(mag), emin)
    quantum = Fraction(2) ** (e_unb - (prec_bits - 1))
    q = mag / quantum
    whole = q.numerator // q.denominator
    frac = q - whole
    if round_increment(sign, mode, frac, whole & 1 == 1):
        whole += 1
    return whole * quantum, frac != 0


def max_finite(fmt_prec):
    p, _, emax = FORMATS[fmt_prec]
    return (2 ** p - 1) * Fraction(2) ** (emax - (p - 1))


def round_result(sign, mag, fmt_prec, mode):
    """mag を精度 fmt_prec(80/32/64) へ丸めた X 形式の結果と (inexact, overflow)。"""
    p, emin, emax = FORMATS[fmt_prec]
    if mag == 0:
        return (sign << 15, 0), False, False
    rounded, inexact = round_to_grid(sign, mag, p, emin, mode)
    if rounded > max_finite(fmt_prec):
        to_inf = (mode == RN) or (mode == RM and sign == 1) or (mode == RP and sign == 0)
        if to_inf:
            return ((sign << 15) | 0x7FFF, J), True, True
        biased_e = emax + BIAS
        sig = ((1 << p) - 1) << (64 - p)
        return ((sign << 15) | biased_e, sig), True, True
    return encode_x(sign, rounded), inexact, False


def hx(v, width=16):
    return format(v, "0%dx" % width)


class Writer:
    def __init__(self, name):
        self.name = name
        self.lines = []

    def add(self, line):
        self.lines.append(line)

    def text(self):
        return "count %d\n" % len(self.lines) + "".join(line + "\n" for line in self.lines)

    def write(self, outdir):
        with open(os.path.join(outdir, self.name), "w") as f:
            f.write(self.text())

    def matches(self, outdir):
        try:
            with open(os.path.join(outdir, self.name)) as f:
                return f.read() == self.text()
        except OSError:
            return False


def gen_roundpack(rng):
    w = Writer("roundpack.txt")
    j = J
    sigs = [j, j | 1, MASK64, 0xC000000000000000, j | (1 << 39), j | (1 << 40),
            j | (1 << 10), j | (1 << 11), MASK64 - (1 << 11) + 1]
    extras = [0, 1, J, J | 1, MASK64]
    for prec in (80, 32, 64):
        p, emin, emax = FORMATS[prec]
        mn = MIN_NORM_EXP[prec]
        max_exp = emax + BIAS
        exps = set(range(mn - p - 4, mn + 3))
        exps |= {max_exp - 1, max_exp, max_exp + 1, max_exp + 2}
        if prec == 80:
            exps |= {-70, -100, -1000, -20000}
        for exp in sorted(exps):
            for _ in range(6):
                sig = rng.choice(sigs) if rng.random() < 0.7 else (rng.getrandbits(64) | j)
                extra = rng.choice(extras) if rng.random() < 0.7 else rng.getrandbits(64)
                for sign in (0, 1):
                    for mode in MODES:
                        add_roundpack(w, sign, exp, sig, extra, prec, mode)
    # keep==0 / keep<0 の境界（S/D）を厳密に網羅する
    for prec in (32, 64):
        p, emin, emax = FORMATS[prec]
        mn = MIN_NORM_EXP[prec]
        for exp in (mn - p, mn - p - 1, mn - p + 1):
            for sig, extra in ((j, 0), (j, 1), (j | 1, 0), (MASK64, MASK64)):
                for sign in (0, 1):
                    for mode in MODES:
                        add_roundpack(w, sign, exp, sig, extra, prec, mode)
    return w


def add_roundpack(w, sign, exp, sig, extra, prec, mode):
    mag = (Fraction(sig) + Fraction(extra, 1 << 64)) * Fraction(2) ** (exp - BIAS - 63)
    (sexp, signif), inexact, overflow = round_result(sign, mag, prec, mode)
    flags = 0
    if exp < MIN_NORM_EXP[prec]:
        flags |= FLAG_UNDERFLOW
    if inexact:
        flags |= FLAG_INEXACT
    if overflow:
        flags |= FLAG_OVERFLOW
    w.add("rp %d %d %s %s %d %d => %s %s %x" % (
        sign, exp, hx(sig), hx(extra), prec, mode, hx(sexp, 4), hx(signif), flags))


def x_special_and_random_inputs(rng):
    """変換系に共通の入力（sign_exp, signif）。有限値・ゼロ・非正規化・unnormalized・Inf・NaN。"""
    vals = []
    for sign in (0, 1):
        s = sign << 15
        vals += [
            (s | 0x0000, 0), (s | 0x0001, 0), (s | 0x3FFF, 0),      # ゼロ（unnormalized zero含む）
            (s | 0x3FFF, J), (s | 0x3FFE, J), (s | 0x4000, J),      # ±1, 0.5, 2
            (s | 0x3FFF, J | (1 << 62)), (s | 0x4001, 0xA000000000000000),
            (s | 0x0000, 1), (s | 0x0000, J), (s | 0x0000, 0x4000000000000000),
            (s | 0x0001, J), (s | 0x0000, MASK64),                   # 最小非正規化/最小正規化/境界
            (s | 0x7FFE, MASK64), (s | 0x7FFE, J),                   # 最大有限/大きい値
            (s | 0x0005, 0x0000000000000001), (s | 0x0001, 0x4000000000000000),  # unnormalized
            (s | 0x3FFF, 0x0000000000000001), (s | 0x4003, 0x0123456789ABCDEF),
            (s | 0x7FFF, 0), (s | 0x7FFF, J),                        # 無限大（j=0/1）
            (s | 0x7FFF, J | QUIET), (s | 0x7FFF, J | QUIET | 0x1234),  # quiet NaN
            (s | 0x7FFF, J | 0x2000000000000000),                    # SNaN
            (s | 0x7FFF, 0x1000000000000000),                        # j=0のSNaN
            (s | 0x7FFF, MASK64), (s | 0x7FFF, 1),
        ]
    for _ in range(80):
        e = rng.choice([rng.randrange(0, 0x7FFE), rng.randrange(0x3F00, 0x4100), rng.randrange(0, 70),
                        0x3F6A + rng.randrange(-4, 6), 0x3FFF + rng.randrange(-40, 40)])
        sig = rng.getrandbits(64) | J
        vals.append((rng.getrandbits(1) << 15 | e, sig))
    return vals


def classify(sexp, signif):
    e = sexp & 0x7FFF
    if e == 0x7FFF:
        return "inf" if (signif & FRAC_MASK) == 0 else "nan"
    return "zero" if signif == 0 else "finite"


def gen_from_x():
    w = Writer("from_x.txt")
    rng = random.Random(1)
    for sexp, signif in x_special_and_random_inputs(rng):
        kind = classify(sexp, signif)
        sign = sexp >> 15
        if kind in ("inf", "nan"):
            out = (sexp, signif)
        elif kind == "zero":
            out = (sign << 15, 0)
        else:
            out = encode_x(sign, abs(value_of(sexp, signif)))
        w.add("from_x %s %s => %s %s" % (hx(sexp, 4), hx(signif), hx(out[0], 4), hx(out[1])))
    return w


def gen_to_x():
    w = Writer("to_x.txt")
    rng = random.Random(2)
    for sexp, signif in x_special_and_random_inputs(rng):
        kind = classify(sexp, signif)
        sign = sexp >> 15
        if kind == "nan":
            out = (sexp, signif | QUIET)
        elif kind == "inf":
            out = (sexp, signif)
        elif kind == "zero":
            out = (sign << 15, 0)
        else:
            out = encode_x(sign, abs(value_of(sexp, signif)))
        for mode in MODES:
            w.add("to_x %d %s %s => %s %s" % (mode, hx(sexp, 4), hx(signif), hx(out[0], 4), hx(out[1])))
    return w


def pack_binary(sign, mag, mbits, ebits, bias, mode_prec):
    """厳密な mag を IEEE754 の外部形式ビット列へ丸めて符号化する。"""
    emin = 1 - bias
    exp_max = (1 << ebits) - 1
    if mag == 0:
        return sign << (mbits + ebits)
    (sexp, signif), _, overflow = round_result(sign, mag, mode_prec[0], mode_prec[1])
    e = sexp & 0x7FFF
    if e == 0x7FFF:
        return (sign << (mbits + ebits)) | (exp_max << mbits)
    if signif == 0:
        return sign << (mbits + ebits)
    val = abs(value_of(sexp, signif))
    e_unb = floor_log2(val)
    if e_unb >= emin:
        frac = (val / Fraction(2) ** e_unb - 1) * (1 << mbits)
        assert frac.denominator == 1
        return (sign << (mbits + ebits)) | ((e_unb + bias) << mbits) | int(frac)
    frac = val / Fraction(2) ** (emin - mbits)
    assert frac.denominator == 1
    return (sign << (mbits + ebits)) | int(frac)


def nan_bits(sexp, signif, mbits, ebits):
    sign = sexp >> 15
    frac = ((signif | QUIET) >> (63 - mbits)) & ((1 << mbits) - 1)
    return (sign << (mbits + ebits)) | (((1 << ebits) - 1) << mbits) | frac


def rounding_inputs(rng, prec):
    """S/D の丸め境界を狙った有限入力（sign_exp, signif）。"""
    p, emin, emax = FORMATS[prec]
    vals = []
    for keep in range(-3, p + 2):
        e_unb = emin - p + keep
        if keep > p:
            e_unb = emin + (keep - p)
        drop = 64 - keep
        for tail in ("tie", "tie+1", "tie-1", "zero", "ones", "rand"):
            if keep >= 1:
                top = (rng.getrandbits(keep - 1) if keep > 1 else 0) | (1 << (keep - 1))
                sig = top << drop if drop < 64 else top
                if tail == "tie":
                    sig |= 1 << (drop - 1)
                elif tail == "tie+1":
                    sig |= (1 << (drop - 1)) | 1
                elif tail == "tie-1":
                    sig |= (1 << (drop - 1)) - 1
                elif tail == "ones":
                    sig |= (1 << drop) - 1
                elif tail == "rand":
                    sig |= rng.getrandbits(drop)
            else:
                sig = J
                if tail == "tie+1":
                    sig |= 1
                elif tail == "ones":
                    sig = MASK64
                elif tail == "rand":
                    sig |= rng.getrandbits(63)
            if not sig & J:
                sig |= J
            for sign in (0, 1):
                vals.append(((sign << 15) | (e_unb + BIAS), sig))
    for e_unb in (emax - 1, emax, emax + 1, emax + 2):
        for sig in (J, MASK64, J | (1 << 39), ((1 << p) - 1) << (64 - p),
                    (((1 << p) - 1) << (64 - p)) | (1 << (63 - p)),
                    (((1 << p) - 1) << (64 - p)) | (1 << (63 - p)) | 1):
            for sign in (0, 1):
                vals.append(((sign << 15) | (e_unb + BIAS), sig))
    return [(s, g) for s, g in vals if 0 <= (s & 0x7FFF) < 0x7FFF]


def gen_to_binary(name, mbits, ebits, bias, prec):
    w = Writer(name)
    rng = random.Random(prec)
    inputs = x_special_and_random_inputs(rng) + rounding_inputs(rng, prec)
    for sexp, signif in inputs:
        kind = classify(sexp, signif)
        sign = sexp >> 15
        for mode in MODES:
            if kind == "nan":
                bits = nan_bits(sexp, signif, mbits, ebits)
            elif kind == "inf":
                bits = (sign << (mbits + ebits)) | (((1 << ebits) - 1) << mbits)
            elif kind == "zero":
                bits = sign << (mbits + ebits)
            else:
                bits = pack_binary(sign, abs(value_of(sexp, signif)), mbits, ebits, bias, (prec, mode))
            w.add("%s %d %s %s => %s" % (name[:-4], mode, hx(sexp, 4), hx(signif),
                                          hx(bits, (1 + ebits + mbits) // 4)))
    return w


def gen_from_binary(name, mbits, ebits, bias):
    w = Writer(name)
    rng = random.Random(mbits)
    exp_max = (1 << ebits) - 1
    total = 1 + ebits + mbits
    bitsets = set()
    for sign in (0, 1):
        for e in (0, 1, 2, exp_max - 1, exp_max, bias, bias - 1, bias + 1, 1 + rng.randrange(exp_max - 1)):
            for frac in (0, 1, 2, (1 << mbits) - 1, 1 << (mbits - 1), (1 << (mbits - 1)) + 1,
                         1 << (mbits - 2), rng.getrandbits(mbits), rng.getrandbits(mbits)):
                bitsets.add((sign << (mbits + ebits)) | (e << mbits) | frac)
    for bits in sorted(bitsets):
        sign = bits >> (mbits + ebits)
        e = (bits >> mbits) & exp_max
        frac = bits & ((1 << mbits) - 1)
        if e == exp_max:
            if frac == 0:
                out = ((sign << 15) | 0x7FFF, J)
            else:
                out = ((sign << 15) | 0x7FFF, J | (frac << (63 - mbits)))
        elif e == 0:
            mag = Fraction(frac) * Fraction(2) ** (1 - bias - mbits)
            out = encode_x(sign, mag)
        else:
            mag = Fraction((1 << mbits) | frac) * Fraction(2) ** (e - bias - mbits)
            out = encode_x(sign, mag)
        w.add("%s %s => %s %s" % (name[:-4], hx(bits, total // 4), hx(out[0], 4), hx(out[1])))
    return w


def gen_move():
    w = Writer("move.txt")
    rng = random.Random(7)
    inputs = x_special_and_random_inputs(rng)
    for prec_code, prec in ((0, 80), (1, 32), (2, 64)):
        inputs_prec = inputs + (rounding_inputs(rng, prec) if prec != 80 else [])
        for sexp, signif in inputs_prec:
            kind = classify(sexp, signif)
            sign = sexp >> 15
            for mode in MODES:
                if kind == "nan":
                    # SNaNをquiet化してから返す（[881UM §4.5.4.2, §6.1.2]）
                    out = (sexp, signif | QUIET)
                elif kind == "inf":
                    out = (sexp, signif)
                elif kind == "zero":
                    out = (sign << 15, 0)
                else:
                    out, _, _ = round_result(sign, abs(value_of(sexp, signif)), prec, mode)
                w.add("move %d %d %s %s => %s %s" % (prec_code, mode, hx(sexp, 4), hx(signif),
                                                     hx(out[0], 4), hx(out[1])))
    return w


def int_inputs():
    """整数丸めの境界を狙った (sign_exp, signif)。厳密に表せる値だけ。"""
    fracs = [Fraction(0), Fraction(1, 4), Fraction(1, 2), Fraction(1, 2) + Fraction(1, 1 << 40),
             Fraction(3, 4), Fraction(1, 1 << 40)]
    bases = [0, 1, 2, 3, 126, 127, 128, 129, 254, 255, 256, 32766, 32767, 32768, 32769,
             65535, 65536, (1 << 31) - 2, (1 << 31) - 1, 1 << 31, (1 << 31) + 1, (1 << 32) - 1,
             1 << 32, (1 << 63) - 1024, 1 << 63, 1 << 64]
    vals = []
    for base in bases:
        for f in fracs:
            for sign in (0, 1):
                mag = base + f
                if mag == 0:
                    continue
                try:
                    vals.append(encode_x(sign, mag))
                except ValueError:
                    pass  # 64bit仮数に厳密に収まらない値は入力にしない
    vals += [(0x7FFE, MASK64), (0xFFFE, MASK64), (0x0000, 1), (0x8000, 1), (0x3FFE, J),
             (0x3FFD, J), (0xBFFE, J), (0x3FFE, J | 1), (0xBFFE, J | 1), (0x3FFF, 0x0000000000000001)]
    return vals


def gen_to_int():
    w = Writer("to_int.txt")
    rng = random.Random(9)
    inputs = int_inputs() + x_special_and_random_inputs(rng)
    for bits in (8, 16, 32):
        vmax = (1 << (bits - 1)) - 1
        vmin = -(1 << (bits - 1))
        for sexp, signif in inputs:
            kind = classify(sexp, signif)
            sign = sexp >> 15
            for mode in MODES:
                if kind == "nan":
                    top = (signif | QUIET) >> (64 - bits)
                    out = top - (1 << bits) if top > vmax else top
                elif kind == "inf":
                    out = vmin if sign else vmax
                elif kind == "zero":
                    out = 0
                else:
                    mag = abs(value_of(sexp, signif))
                    whole = mag.numerator // mag.denominator
                    frac = mag - whole
                    if round_increment(sign, mode, frac, whole & 1 == 1):
                        whole += 1
                    out = -whole if sign else whole
                    out = max(vmin, min(vmax, out))
                w.add("to_int %d %d %s %s => %d" % (bits, mode, hx(sexp, 4), hx(signif), out))
    return w


def gen_from_int():
    w = Writer("from_int.txt")
    for bits in (8, 16, 32):
        vmax = (1 << (bits - 1)) - 1
        vmin = -(1 << (bits - 1))
        vals = {0, 1, -1, 2, -2, 3, vmax, vmin, vmax - 1, vmin + 1, 100 % (vmax + 1)}
        if bits >= 16:
            vals |= {256, -256, 255, 12345 % (vmax + 1)}
        if bits == 32:
            vals |= {16777216, 16777217, -16777217, 0x12345678, -0x12345678}
        for v in sorted(vals):
            out = encode_x(1 if v < 0 else 0, Fraction(abs(v)))
            w.add("from_int %d %d => %s %s" % (bits, v, hx(out[0], 4), hx(out[1])))
    return w


def check_oracle_against_design():
    """オラクル自体の自己検査。一次資料（[881UM Table 3-3, §6.1.4, §6.1.5]）から手で導いた値。
    ただし keep==0 境界の半分超のRN（2^-150+2^-175）は、一次資料に行が無く IEEE754 に基づく
    推論（設計書 §9「S/D丸めのkeep=0境界」）。他はすべて一次資料の文言どおり。"""
    x = 80
    # 丸め前 (exp,sig)=(-1,J)→2^-16384、(0,J)→最小正規化数、(-63,J)→最小非正規化数
    def rp(exp, sig, extra, prec, mode, sign=0):
        mag = (Fraction(sig) + Fraction(extra, 1 << 64)) * Fraction(2) ** (exp - BIAS - 63)
        return round_result(sign, mag, prec, mode)[0]
    assert rp(-1, J, 0, x, RN) == (0x0000, 0x4000000000000000)
    assert rp(0, J, 0, x, RN) == (0x0000, J)
    assert rp(-63, J, 0, x, RN) == (0x0000, 1)
    # 2^-150 + 2^-175 を PREC=S・RN で丸めると最小非正規化値2^-149
    assert rp(0x3FFF - 150, J | (1 << 38), 0, 32, RN) == (0x3F6A, J)
    # ちょうど 2^-150 は偶数（0）へ
    assert rp(0x3FFF - 150, J, 0, 32, RN) == (0x0000, 0)
    # PREC=S・RZ のオーバーフローは S の最大有限値
    assert rp(0x407F, J, 0, 32, RZ) == (0x407E, 0xFFFFFF0000000000)
    assert rp(0x43FF, J, 0, 64, RZ) == (0x43FE, 0xFFFFFFFFFFFFF800)
    assert rp(0x407F, J, 0, 32, RN) == (0x7FFF, J)


def main():
    check_oracle_against_design()
    outdir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "vectors")
    os.makedirs(outdir, exist_ok=True)
    rng = random.Random(20260929)
    writers = [
        gen_roundpack(rng),
        gen_from_x(), gen_to_x(),
        gen_to_binary("to_s.txt", 23, 8, 127, 32),
        gen_to_binary("to_d.txt", 52, 11, 1023, 64),
        gen_from_binary("from_s.txt", 23, 8, 127),
        gen_from_binary("from_d.txt", 52, 11, 1023),
        gen_move(), gen_to_int(), gen_from_int(),
    ]
    if "--check" in sys.argv:
        stale = [wr.name for wr in writers if not wr.matches(outdir)]
        if stale:
            print("stale vectors (regenerate with this script): " + ", ".join(stale))
            sys.exit(1)
        print("vectors are up to date")
        return
    for wr in writers:
        wr.write(outdir)
        print("%s: %d" % (wr.name, len(wr.lines)))


if __name__ == "__main__":
    main()
