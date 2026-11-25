#!/usr/bin/env python3
# /// script
# requires-python = ">=3.9"
# dependencies = ["mpmath"]
# ///
"""FPU共通コア 1b Phase A（FMOVECR 定数ROM）の生成物。

生成物（コミットする）:
  tests/data/fpu/fpu_1b_rom_constants.csv   テストの期待値（12通りの丸め済み値）と 128bit 表現
  src/fpu/fpu_rom_table.c          製品側の 128bit 定数表（jam 込み）

再生成:  uv run --script tests/fpu/gen_fpu_1b_vectors.py
最新かの検査:  python3 tests/fpu/gen_fpu_1b_vectors.py --check   （ctest の test_fpu_1b_fresh）
--check は mpmath が無くても動く（mpmath との突き合わせだけ省く）。

値の出典は [881UM] ではなく数学的な真値（[881UM] p.4-73 は定数名だけを載せる。設計書 §4.1）。
7 つの無理数定数は整数の固定小数点演算（Machin 公式・級数）で求め、mpmath があれば別経路として
突き合わせる。10^(2^k) は厳密な整数。丸めは fractions.Fraction による格子丸め
（gen_fpu_core_vectors.py の round_to_grid）で、製品側とは別の計算経路。
PREC=S/D は仮数だけを 24/53bit に丸め、指数範囲は拡張精度のまま（設計書 §4.1-5）。
"""
import decimal
import math
import os
import random
import shutil
import subprocess
import sys
from fractions import Fraction

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_fpu_core_vectors import (  # noqa: E402
    round_increment, BIAS, FLAG_UNDERFLOW, FORMATS, J, MIN_NORM_EXP, MODES, RN, RZ, RM, RP, Writer,
    encode_x, floor_log2, hx, max_finite, round_result, round_to_grid, value_of)

VECTOR_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "vectors")
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CSV_PATH = os.path.join(ROOT, "tests", "data", "fpu", "fpu_1b_rom_constants.csv")
TABLE_PATH = os.path.join(ROOT, "src", "fpu", "fpu_rom_table.c")
TRANSCEND_PATH = os.path.join(ROOT, "src", "fpu", "fpu_transcend_tables.c")

FIXED_BITS = 900       # 無理数定数を求める固定小数点の小数部ビット数
EMIN_X = -16383
PRECS = ((64, "x"), (24, "s"), (53, "d"))    # (仮数ビット数, 列名)
MODE_NAMES = {RN: "rn", RZ: "rz", RM: "rm", RP: "rp"}


def atanh_inv(n):
    """atanh(1/n) の固定小数点値（FIXED_BITS）。"""
    one = 1 << FIXED_BITS
    term = one // n
    total = 0
    k = 1
    n2 = n * n
    while term:
        total += term // k
        term //= n2
        k += 2
    return total


def atan_inv(n):
    one = 1 << FIXED_BITS
    term = one // n
    total = 0
    k = 1
    sign = 1
    n2 = n * n
    while term:
        total += sign * (term // k)
        term //= n2
        k += 2
        sign = -sign
    return total


def fixed_pi():
    return 4 * (4 * atan_inv(5) - atan_inv(239))       # Machin


def fixed_e():
    one = 1 << FIXED_BITS
    total, term, k = 0, one, 0
    while term:
        total += term
        k += 1
        term //= k
    return total


def fixed_ln2():
    return 2 * atanh_inv(3)                            # ln2 = 2 atanh(1/3)


def fixed_ln10():
    # ln10 = 3 ln2 + ln(5/4)、ln(5/4) = 2 atanh(1/9)
    return 3 * fixed_ln2() + 2 * atanh_inv(9)


def as_fraction(fixed):
    return Fraction(fixed, 1 << FIXED_BITS)


def irrational_values():
    """名前 -> Fraction（FIXED_BITS の近似。丸め位置から十分遠い）。"""
    one = Fraction(1)
    ln2, ln10 = as_fraction(fixed_ln2()), as_fraction(fixed_ln10())
    return {
        "pi": as_fraction(fixed_pi()),
        "log10(2)": ln2 / ln10,
        "e": as_fraction(fixed_e()),
        "log2(e)": one / ln2,
        "log10(e)": one / ln10,
        "ln(2)": ln2,
        "ln(10)": ln10,
    }


def check_with_mpmath(values):
    """別経路（mpmath）と 200bit 相当で突き合わせる。mpmath が無ければ False。"""
    try:
        import mpmath
    except ImportError:
        return False
    mpmath.mp.prec = 300
    ref = {
        "pi": mpmath.pi, "log10(2)": mpmath.log10(2), "e": mpmath.e,
        "log2(e)": 1 / mpmath.log(2), "log10(e)": 1 / mpmath.log(10),
        "ln(2)": mpmath.log(2), "ln(10)": mpmath.log(10),
    }
    for name, frac in values.items():
        got = mpmath.mpf(frac.numerator) / mpmath.mpf(frac.denominator)
        assert abs(got - ref[name]) < mpmath.mpf(2) ** -250, name
    return True


# (オフセット, 名前, 種別, 引数)。種別: "irr"=無理数の名前、"pow10"=10^k、"zero"
ENTRIES = (
    [(0x00, "pi", "irr", "pi"),
     (0x0B, "log10(2)", "irr", "log10(2)"),
     (0x0C, "e", "irr", "e"),
     (0x0D, "log2(e)", "irr", "log2(e)"),
     (0x0E, "log10(e)", "irr", "log10(e)"),
     (0x0F, "0.0", "zero", 0),
     (0x30, "ln(2)", "irr", "ln(2)"),
     (0x31, "ln(10)", "irr", "ln(10)")]
    + [(0x32 + i, "10^%d" % k, "pow10", k)
       for i, k in enumerate([0, 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096])]
)

# 備考（設計書 §4.1-4。【二次】XEiJ の EFPBox.java のコメントによる実機との相違）
NOTES = {
    0x0B: "XEiJ comment: MC68882 RN value differs from true rounding (not reproduced, O3)",
    0x0C: "XEiJ comment: MC68882 differs from true rounding (not reproduced, O3)",
    0x0E: "XEiJ comment: MC68882 RP value wrong and INEX2 not set (not reproduced, O3)",
}


def entry_value(kind, arg, irr):
    if kind == "irr":
        return irr[arg]
    if kind == "pow10":
        return Fraction(10 ** arg)
    return Fraction(0)


def to_128(value):
    """value>0 -> (exp, hi, lo)。exp は roundPack へ渡すアイデアライズド指数（bit127=1、jam 込み）。"""
    e = floor_log2(value)
    scaled = value * Fraction(2) ** (127 - e)
    whole = scaled.numerator // scaled.denominator
    if scaled != whole:
        whole |= 1                                     # 切り捨てた端数を最下位ビットへ jam
    assert whole >> 127 == 1
    return e + BIAS, whole >> 64, whole & ((1 << 64) - 1)


def verify_128(value, exp, hi, lo):
    """to_128 の結果 (exp, hi, lo) を、割り算・floor を使わない別の形で検算する（設計書 §4.1-2・-3）。
    W=(hi:lo)、u=2^(exp-BIAS-127) として、厳密なら W*u == value。そうでなければ W は奇数（jam）で
    (W-1)*u < value < (W+1)*u（切り捨て値 T に対し W は T か T+1 のうち奇数の方）。"""
    w = (hi << 64) | lo
    assert w >> 127 == 1, "bit127 must be set"
    unit = Fraction(2) ** (exp - BIAS - 127)
    if w * unit == value:
        return
    assert w & 1 == 1, "inexact 128-bit value must carry the jam bit"
    assert (w - 1) * unit < value < (w + 1) * unit, "128-bit value is not the truncation of value"


def rounded_cell(value, mode, mbits):
    """value>0 を mbits ビット仮数・拡張精度の指数範囲で丸めた ((sexp, signif), inexact)。"""
    mag, inexact = round_to_grid(0, value, mbits, EMIN_X, mode)
    return encode_x(0, mag), inexact


def decimal30(value):
    ctx = decimal.Context(prec=30)
    return format(ctx.divide(decimal.Decimal(value.numerator), decimal.Decimal(value.denominator)), "e")


def build():
    irr = irrational_values()
    used_mpmath = check_with_mpmath(irr)
    rows, table = [], []
    header = ["offset", "name", "value", "exp", "hi", "lo_jam"]
    for mode in MODES:
        for _, col in PRECS:
            header.append("%s_%s" % (MODE_NAMES[mode], col))
    header.append("note")
    for off, name, kind, arg in ENTRIES:
        value = entry_value(kind, arg, irr)
        if value == 0:
            cells = ["0000/0000000000000000/0"] * 12
            row = ["%02x" % off, name, "0", "0", "0000000000000000", "0000000000000000"] + cells
            table.append((off, 0, 0, 0, 0))
        else:
            exp, hi, lo = to_128(value)
            verify_128(value, exp, hi, lo)
            cells = []
            for mode in MODES:
                for mbits, _ in PRECS:
                    (sexp, signif), inexact = rounded_cell(value, mode, mbits)
                    cells.append("%04x/%016x/%d" % (sexp, signif, 1 if inexact else 0))
            digits = "1e%d" % arg if kind == "pow10" and arg >= 32 else decimal30(value)
            row = ["%02x" % off, name, digits, "%04x" % exp, "%016x" % hi, "%016x" % lo] + cells
            table.append((off, 1, exp, hi, lo))
        row.append(NOTES.get(off, ""))
        rows.append(row)
    return header, rows, table, used_mpmath


def csv_text(header, rows):
    lines = [
        "# FMOVECR 定数ROMの期待値（Phase A。tests/fpu/gen_fpu_1b_vectors.py が生成）。",
        "# 値の出典は [881UM] ではなく数学的な真値。[881UM] p.4-73 は定数名だけを載せ、値のビット列は無い。",
        "# 各セルは sexp/signif/inex2（16進）。列名は <丸めモード>_<PREC>。S/D は仮数だけを 24/53bit に丸め、",
        "# 指数範囲は拡張精度のまま（設計書 §4.1-5。U17 で XEiJ と照合）。",
        "# exp/hi/lo_jam は製品側の 128bit 表（jam 込み）。value は 10進30桁（10^k の大きい k は 1eK）。",
        ",".join(header),
    ]
    lines += [",".join(r) for r in rows]
    return "\n".join(lines) + "\n"


def table_text(table):
    out = [
        "/* fpu_rom_table.c — FMOVECR の定数ROM（128bit の仮数＋jam）。",
        " * tests/fpu/gen_fpu_1b_vectors.py が生成する。手で編集しない。 */",
        '#include "fpu_internal.h"',
        "",
        "const struct fpu_rom_entry fpu_rom_table[] = {",
    ]
    for off, nonzero, exp, hi, lo in table:
        out.append("    { 0x%02X, %d, 0x%04X, UINT64_C(0x%016X), UINT64_C(0x%016X) }," % (off, nonzero, exp, hi, lo))
    out += ["};", "", "const int fpu_rom_table_count = %d;" % len(table), ""]
    return "\n".join(out)


# ---- Phase B1: FSQRT / FINT / FINTRZ / FMOD（IEEE 規定演算。ビット一致。設計書 §5.3・§5.6） ----------

PREC_CODES = ((0, 80), (1, 32), (2, 64))   # (mc68k_fpu_round_prec_t, roundPack の精度)
EXC_INEX2, EXC_UNFL = 0x02, 0x08


def rand_finite(rng, exp_choices):
    """(sexp, signif)。exp_choices からフィールド e を選ぶ。e=0 は非正規化数を含む。"""
    e = rng.choice(exp_choices)
    sign = rng.getrandbits(1)
    kind = rng.random()
    if kind < 0.2:
        sig = J
    elif kind < 0.3:
        sig = J | rng.getrandbits(4)
    elif kind < 0.4:
        sig = (1 << 64) - 1
    else:
        sig = rng.getrandbits(64) | (J if e != 0 or rng.random() < 0.5 else 0)
    if e == 0 and sig == 0:
        sig = 1
    return (sign << 15) | e, sig


def exp_choices_wide():
    base = [0, 1, 2, 3, BIAS - 70, BIAS - 2, BIAS - 1, BIAS, BIAS + 1, BIAS + 2, BIAS + 62,
            BIAS + 63, BIAS + 64, 0x7FFD, 0x7FFE]
    base += [MIN_NORM_EXP[32] + 3, MIN_NORM_EXP[64] + 3, 0x407E, 0x43FE]
    return base


def ref_sqrt(mag, prec, mode):
    """sqrt(mag) を prec/mode で丸めた ((sexp,signif), inexact)。二重丸めしない厳密な参照。"""
    n, d = mag.numerator, mag.denominator
    k = 400
    x = n * d * 4 ** k
    t = math.isqrt(x)
    if t * t == x:
        exact = Fraction(t, d * 2 ** k)
    else:
        exact = Fraction(2 * t + 1, 2 * d * 2 ** k)     # 区間の中点。格子点・半点を含まない
    out, inexact, _ = round_result(0, exact, prec, mode)
    return out, inexact


def odd_sqrts_mod_pow2(a, k):
    """r^2 ≡ a (mod 2^k) を満たす奇数 r（0 <= r < 2^k）の全数。a ≡ 1 (mod 8) のときだけ解がある。"""
    roots = {r for r in (1, 3, 5, 7) if (r * r - a) % 8 == 0}
    for j in range(4, k + 1):
        roots = {c % (1 << j) for r in roots for c in (r, r + (1 << (j - 1))) if (c * c - a) % (1 << j) == 0}
    return sorted(roots)


def sqrt_tie_inputs():
    """fpu_sqrt.c の isqrt_256 は X = sig·2^shift（shift は 191 か 192）の平方根 root（128bit）を求める。root の下位
    64bit がちょうど 2^63 で余りが非零なら、余りの jam の有無で PREC=X の RN の結果が変わり得る。そのような sig の全数を
    整数論で求める: root = M = (2m+1)·2^63 のとき X - M^2 = 2^126·t、t = sig·2^(shift-126) - (2m+1)^2 は奇数で、
    0 < X - M^2 < 2M+1 < 2^129 から t < 8。奇数の平方は 1 (mod 8) なので t ≡ 7 (mod 8)、つまり t = 7。
    下位 64bit が 0 で余りが非零（jam が無いと厳密に見える）の場合は、root = m·2^64 で X - root^2 = 2^128·t、
    0 < X - root^2 < 2^129 から t = 1、m^2 = sig·2^(shift-128) - 1 ≡ -1 (mod 8) となり解が無い。
    PREC=S/D（非正規化域を含む）では丸め位置が上にあり、X と格子点・半点の平方の差が 2^148 以上の倍数になるので、
    余りが非零なら丸め位置より下に必ず 1 のビットがある。
    戻り値は (指数フィールド, sig)。m が奇数なら、jam が無くてもタイの偶数丸めは切り上げで、結果は変わらない。"""
    out = []
    for shift, exp_field in ((192, BIAS + 63), (191, BIAS + 64)):      # p = exp - BIAS - 63 が偶数なら shift=192
        k = shift - 126
        for t in range(1, 8, 2):
            for n in odd_sqrts_mod_pow2((-t) % (1 << k), k):
                if not (1 << 64) <= n < (1 << 65) or (n * n + t) % (1 << k):
                    continue
                sig = (n * n + t) >> k
                if not (1 << 63) <= sig < (1 << 64):
                    continue
                x = sig << shift
                root = math.isqrt(x)
                if root & ((1 << 64) - 1) == 1 << 63 and root * root != x:
                    assert (root >> 64) & 1, "tie with even m: the jam in isqrt_256 would not be equivalent"
                    out.append((exp_field, sig))
    assert out == [(BIAS + 63, 0xE6F077A8C3DC6DCA)], out
    return out


def gen_b1_sqrt():
    w = Writer("b1_sqrt.txt")
    rng = random.Random(59001)
    vals = [rand_finite(rng, exp_choices_wide()) for _ in range(260)]
    vals += [(0x3FFF, J), (0x4000, J), (0x4001, J), (0x3FFE, J), (0x3FFF, 0xC000000000000000),
             (0x4000, 0x9000000000000000)]   # 1, 2, 4, 0.5, 1.5, 2.25 = 1.5^2
    for m in [3, 12345, 65535, 4294967295, 4294967296, 2 ** 31 + 1]:
        sq = m * m
        e = sq.bit_length() - 1
        vals.append((BIAS + e, sq << (63 - e) if e <= 63 else sq >> (e - 63)))
    # 128bit の平方根の下位 64bit がちょうど半分（0x8000...0）で余りが非零になる唯一の拡張精度仮数
    # （sqrt_tie_inputs() が全数を求める。設計書 §5.6 の等価変異の根拠）
    vals += sqrt_tie_inputs()
    for pc, prec in PREC_CODES:
        for sexp, signif in vals:
            if sexp >> 15:
                sexp &= 0x7FFF       # 負値は特殊値としてテスト本体で扱う
            mag = value_of(sexp, signif)
            if mag == 0:
                continue
            for mode in MODES:
                out, inexact = ref_sqrt(mag, prec, mode)
                w.add("sqrt %d %d %s %s => %s %s %x" % (
                    pc, mode, hx(sexp, 4), hx(signif), hx(out[0], 4), hx(out[1]),
                    EXC_INEX2 if inexact else 0))
    return w


def round_int_mag(sign, mag, mode):
    """mag(>=0) を整数へ丸めた大きさ。"""
    fl = mag.numerator // mag.denominator
    frac = mag - fl
    if frac == 0:
        return Fraction(fl)
    if mode == RZ:
        up = False
    elif mode == RM:
        up = sign == 1
    elif mode == RP:
        up = sign == 0
    else:
        up = frac > Fraction(1, 2) or (frac == Fraction(1, 2) and fl % 2 == 1)
    return Fraction(fl + 1 if up else fl)


def gen_b1_int():
    w = Writer("b1_int.txt")
    rng = random.Random(59002)
    vals = []
    for u in list(range(-4, 68)) + [-100, -16383, -16384, -16445]:
        for _ in range(3):
            e = u + BIAS
            if e < 0:
                continue
            sexp, signif = rand_finite(rng, [e])
            vals.append((sexp, signif))
        if u >= -1 and u < 64:
            vals.append((BIAS + u, J))                    # 2^u（整数または 0.5）
            vals.append((BIAS + u, J | (J >> 1)))         # 1.5 * 2^u（.5 の端数を作る）
            vals.append((BIAS + u, J | 1))                # ちょうど半分より上
    vals += [(0, 1), (0, 0x0123456789ABCDEF), (0x8000 | 0, 1)]
    # FINT/FINTRZ は PREC の丸めをかけない（設計書 §5.3 の暫定決定、U17 で XEiJ と一致）。期待値は PREC に
    # よらず同じで、PREC=S/D でも 24/53bit を超える整数がそのまま返ることを検査する（設計書 §5.6 の 4×3）。
    for pc, _ in PREC_CODES:
        for sexp, signif in vals:
            sign = sexp >> 15
            mag = abs(value_of(sexp, signif))
            for fn in ("int", "intrz"):
                for mode in MODES:
                    m = RZ if fn == "intrz" else mode
                    r = round_int_mag(sign, mag, m)
                    out = encode_x(sign, r)
                    if r == 0:
                        out = (sign << 15, 0)
                    w.add("%s %d %d %s %s => %s %s %x" % (
                        fn, pc, mode, hx(sexp, 4), hx(signif), hx(out[0], 4), hx(out[1]),
                        EXC_INEX2 if r != mag else 0))
    return w


def gen_b1_mod():
    w = Writer("b1_mod.txt")
    rng = random.Random(59003)
    cases = []
    for _ in range(300):
        de = rng.choice([BIAS, BIAS + 5, BIAS + 70, BIAS - 60, 0x4200, 0x7FFE, 0x0100, 1, 0,
                         BIAS + 3, BIAS + 64, BIAS + 130])
        se = rng.choice([BIAS, BIAS - 3, BIAS + 2, BIAS - 100, 0x0100, 0, 1, 0x3E00, BIAS + 64,
                         0x7FFE, 0x7FFD, 0x7000])
        cases.append((rand_finite(rng, [de]), rand_finite(rng, [se])))
    cases += [(((BIAS + 4) | 0, J | (J >> 2)), ((BIAS + 1), J)),         # 割り切れない
              ((0x7FFE, J), (0x0000, 1)),                                # 最大指数差
              ((0x7FFE, (1 << 64) - 1), (0x0001, J)),
              ((BIAS + 3, J), (BIAS + 1, J)),                            # 割り切れる（剰余 0）
              ((0x8000 | (BIAS + 3), J), (BIAS + 1, J)),
              ((BIAS, J), (0x8000 | (BIAS + 1), J))]
    for (dsexp, dsig), (ssexp, ssig) in cases:
        dv, sv = value_of(dsexp, dsig), value_of(ssexp, ssig)
        if dv == 0 or sv == 0:
            continue
        dsign, ssign = dsexp >> 15, ssexp >> 15
        q = abs(dv) // abs(sv)
        rem = abs(dv) - abs(sv) * q
        for pc, prec in PREC_CODES:
            for mode in (MODES if pc != 0 else (RN, RZ)):
                if rem == 0:
                    out, exc = (dsign << 15, 0), 0
                else:
                    out, inexact, _ = round_result(dsign, rem, prec, mode)
                    exc = EXC_INEX2 if inexact else 0
                    if floor_log2(rem) + BIAS < MIN_NORM_EXP[prec]:
                        exc |= EXC_UNFL
                w.add("mod %d %d %s %s %s %s => %s %s %x %d %d" % (
                    pc, mode, hx(dsexp, 4), hx(dsig), hx(ssexp, 4), hx(ssig),
                    hx(out[0], 4), hx(out[1]), exc, dsign ^ ssign, q % 128))
    return w


# ---- Phase B1: 超越関数の係数表（float128。設計書 §5.2・§5.4） ----------------------------------

F128_BIAS = 16383
TWO_OVER_PI_BITS = 17100     # 2/π の小数部ビット数。最大指数の Payne–Hanek 窓（約 16.7k bit）を覆う


def f128_words(value):
    """Fraction -> IEEE binary128 の (hi64, lo64)。最近接偶数。0 は (0, 0)。"""
    if value == 0:
        return 0, 0
    sign = 1 if value < 0 else 0
    mag = abs(value)
    e = floor_log2(mag)
    q = mag / Fraction(2) ** (e - 112)
    whole = q.numerator // q.denominator
    frac = q - whole
    if frac > Fraction(1, 2) or (frac == Fraction(1, 2) and whole & 1):
        whole += 1
    if whole >> 113:
        whole >>= 1
        e += 1
    assert whole >> 112 == 1 and -16382 <= e <= 16383
    bits = (sign << 127) | ((e + F128_BIAS) << 112) | (whole & ((1 << 112) - 1))
    return bits >> 64, bits & ((1 << 64) - 1)


def transcend_values():
    """名前 -> Fraction または Fraction のリスト。生成は整数・有理数の演算だけで行い、mpmath は突き合わせ。"""
    irr = irrational_values()
    pi = irr["pi"]
    ln2 = irr["ln(2)"]
    out = {"PI_2": pi / 2, "PI": pi}
    # ln2 = HI(上位 64 bit の切り捨て) + LO
    e = floor_log2(ln2)
    hi = Fraction((ln2 / Fraction(2) ** (e - 63)).numerator // (ln2 / Fraction(2) ** (e - 63)).denominator) \
        * Fraction(2) ** (e - 63)
    out["LN2_HI"] = hi
    out["LN2_LO"] = ln2 - hi
    out["INV_LN2"] = 1 / ln2
    out["INV_LN10"] = irr["log10(e)"]
    # FTENTOX（B2）の x·ln10。x は 64bit なので、上位 48bit（積 112bit が f128 に厳密に収まる）と残りに分ける
    ln10 = irr["ln(10)"]
    e10 = floor_log2(ln10)
    scaled = ln10 / Fraction(2) ** (e10 - 47)
    out["LN10_A"] = Fraction(scaled.numerator // scaled.denominator) * Fraction(2) ** (e10 - 47)
    out["LN10_B"] = ln10 - out["LN10_A"]
    out["LOG2_10"] = ln10 / ln2
    fact = [math.factorial(n) for n in range(80)]
    out["EXP_COEF"] = [Fraction(1, fact[n]) for n in range(27)]
    out["SIN_COEF"] = [Fraction((-1) ** n, fact[2 * n + 1]) for n in range(16)]
    out["COS_COEF"] = [Fraction((-1) ** n, fact[2 * n]) for n in range(16)]
    out["SINH_COEF"] = [Fraction(1, fact[2 * n + 1]) for n in range(16)]
    out["COSH_COEF"] = [Fraction(1, fact[2 * n]) for n in range(16)]
    out["ODD_RECIP"] = [Fraction(1, 2 * j + 1) for j in range(31)]
    out["ATAN_COEF"] = [Fraction((-1) ** n, 2 * n + 1) for n in range(16)]
    return out


def fixed_atan_ratio(num, den):
    """atan(num/den) の固定小数点値（FIXED_BITS）。級数 Σ(-1)^n x^(2n+1)/(2n+1)、x=num/den<1。"""
    one = 1 << FIXED_BITS
    x = one * num // den
    x2 = x * x >> FIXED_BITS
    term, total, k, sign = x, 0, 1, 1
    while term:
        total += sign * (term // k)
        term = term * x2 >> FIXED_BITS
        k += 2
        sign = -sign
    return total


def atan_table_values():
    """atan(j/8) j=0..8。整数演算で求め、mpmath があれば別経路として突き合わせる。"""
    vals = [as_fraction(fixed_atan_ratio(j, 8)) if j < 8 else as_fraction(fixed_pi()) / 4 for j in range(9)]
    try:
        import mpmath
    except ImportError:
        return vals
    mpmath.mp.prec = 400
    for j, v in enumerate(vals):
        ref = mpmath.atan(mpmath.mpf(j) / 8)
        assert abs(mpmath.mpf(v.numerator) / v.denominator - ref) < mpmath.mpf(2) ** -300, j
    return vals


def two_over_pi_words():
    """2/π = 0.b1b2b3... の先頭 TWO_OVER_PI_BITS ビットを 32bit 語（先頭が最上位）で返す。"""
    global FIXED_BITS
    saved = FIXED_BITS
    FIXED_BITS = TWO_OVER_PI_BITS + 200
    try:
        pi = as_fraction(fixed_pi())
    finally:
        FIXED_BITS = saved
    t = 2 / pi
    n = (t * (1 << TWO_OVER_PI_BITS)).numerator // (t * (1 << TWO_OVER_PI_BITS)).denominator
    nwords = TWO_OVER_PI_BITS // 32 + 1
    n <<= nwords * 32 - TWO_OVER_PI_BITS
    return [(n >> (32 * (nwords - 1 - i))) & 0xFFFFFFFF for i in range(nwords)]


def transcend_text():
    vals = transcend_values()
    atan_tab = atan_table_values()
    try:
        import mpmath
        mpmath.mp.prec = 400
        assert abs(mpmath.mpf(vals["PI"].numerator) / vals["PI"].denominator - mpmath.pi) < mpmath.mpf(2) ** -300
    except ImportError:
        pass
    out = [
        "/* fpu_transcend_tables.c — 超越関数の係数表（float128 = {下位64bit, 上位64bit}）。",
        " * tests/fpu/gen_fpu_1b_vectors.py が生成する。手で編集しない。 */",
        '#include "fpu_wide.h"',
        "",
    ]

    def const(name, value):
        hi, lo = f128_words(value)
        return "const wide_t %s = { { UINT64_C(0x%016X), UINT64_C(0x%016X) } };" % (name, lo, hi)

    def array(name, values):
        lines = ["const wide_t %s[%d] = {" % (name, len(values))]
        for v in values:
            hi, lo = f128_words(v)
            lines.append("    { { UINT64_C(0x%016X), UINT64_C(0x%016X) } }," % (lo, hi))
        lines.append("};")
        return "\n".join(lines)

    for name in ("PI_2", "PI", "LN2_HI", "LN2_LO", "INV_LN2", "INV_LN10", "LN10_A", "LN10_B", "LOG2_10"):
        out.append(const("w_" + name.lower(), vals[name]))
    out.append("")
    for name in ("EXP_COEF", "SIN_COEF", "COS_COEF", "SINH_COEF", "COSH_COEF", "ODD_RECIP", "ATAN_COEF"):
        out.append(array("w_" + name.lower(), vals[name]))
        out.append("")
    out.append(array("w_atan_tab", atan_tab))
    out.append("")
    out.append(array("w_atan_c", [Fraction(j, 8) for j in range(9)]))
    out.append("")
    out.append("/* 128bit 仮数（bit127=1）と jam。π/2・π を丸め方向つきの特殊結果として返すときに使う */")
    for name, key in (("pi_2", "PI_2"), ("pi", "PI")):
        exp, hi, lo = to_128(vals[key])
        out.append("const struct wide_const128 c128_%s = { 0x%04X, UINT64_C(0x%016X), UINT64_C(0x%016X) };"
                   % (name, exp, hi, lo))
    out.append("")
    words = two_over_pi_words()
    out.append("/* 2/π の小数部の先頭 %d ビット（先頭語の最上位ビットが 2^-1） */" % TWO_OVER_PI_BITS)
    out.append("const uint32_t two_over_pi_words[%d] = {" % len(words))
    for i in range(0, len(words), 6):
        out.append("    " + ", ".join("0x%08X" % w for w in words[i:i + 6]) + ",")
    out.append("};")
    out.append("const int two_over_pi_bits = %d;" % TWO_OVER_PI_BITS)
    out.append("")
    return "\n".join(out)


# ---- Phase B1: 超越関数（有限値。忠実丸め。設計書 §3.2・§5.6） --------------------------------------

# 0 に強制する EXC ビット（設計書 §5.2 のマスク表。EXC_OVFL=0x10, EXC_UNFL=0x08）
EXC_OVFL = 0x10
FORCE_CLEAR = {
    "sin": EXC_OVFL, "cos": EXC_OVFL | EXC_UNFL, "tan": 0, "atan": EXC_OVFL,
    "asin": EXC_OVFL | EXC_UNFL, "acos": EXC_OVFL | EXC_UNFL, "atanh": EXC_OVFL,
    "sinh": 0, "cosh": EXC_UNFL, "tanh": EXC_OVFL, "etox": 0,
    "logn": EXC_OVFL | EXC_UNFL, "log2": EXC_OVFL | EXC_UNFL, "log10": EXC_OVFL | EXC_UNFL,
    # Phase B2（b2_tr_*.txt）。他の関数の乱数列を変えないよう末尾に置く
    "etoxm1": 0, "lognp1": EXC_OVFL, "twotox": 0, "tentox": 0,
}
B2_TR_FUNCS = ("etoxm1", "lognp1", "twotox", "tentox")
TR_MP_PREC = 700
# 極小引数の特殊経路に入る |x| < 2^-N（fpu_wide.h の WIDE_TINY_EXP と fpu_transcend_explog.c の ETOX_TINY_EXP）
TINY_LIMIT = {"sin": 40, "cos": 40, "tan": 40, "atan": 40, "asin": 40, "atanh": 40, "sinh": 40,
              "cosh": 40, "tanh": 40, "etox": 100, "etoxm1": 100, "lognp1": 100, "twotox": 100, "tentox": 100}


def mp_eval(fn, x):
    """fn(x) の真値（Fraction、TR_MP_PREC bit 精度）。x は Fraction。"""
    import mpmath
    mpmath.mp.prec = TR_MP_PREC
    xm = mpmath.mpf(x.numerator) / mpmath.mpf(x.denominator)
    f = {"sin": mpmath.sin, "cos": mpmath.cos, "tan": mpmath.tan, "atan": mpmath.atan,
         "asin": mpmath.asin, "acos": mpmath.acos, "etox": mpmath.exp, "logn": mpmath.log,
         "log10": mpmath.log10, "log2": lambda v: mpmath.log(v, 2), "sinh": mpmath.sinh,
         "cosh": mpmath.cosh, "tanh": mpmath.tanh, "atanh": mpmath.atanh,
         "etoxm1": mpmath.expm1, "lognp1": mpmath.log1p,
         "twotox": lambda v: mpmath.mpf(2) ** v, "tentox": lambda v: mpmath.mpf(10) ** v}[fn]
    v = f(xm)
    sign, man, exp, _ = v._mpf_
    val = Fraction(int(man)) * (Fraction(2) ** int(exp))
    return -val if sign else val


def taylor_tiny(fn, x):
    """極小引数の真値の 2 項近似（mpmath の精度で分解できない |x| 用）。補正項の向きだけが結果に効く。"""
    return {
        "sin": x - x ** 3 / 6, "tan": x + x ** 3 / 3, "atan": x - x ** 3 / 3, "asin": x + x ** 3 / 6,
        "sinh": x + x ** 3 / 6, "tanh": x - x ** 3 / 3, "atanh": x + x ** 3 / 3,
        "cos": 1 - x ** 2 / 2, "cosh": 1 + x ** 2 / 2, "etox": 1 + x + x ** 2 / 2,
        "etoxm1": x + x ** 2 / 2, "lognp1": x - x ** 2 / 2,
        # 2^x・10^x は 1 + x·ln2・1 + x·ln10。方向だけが要るので ln の近似（分母 10^9）で足りる
        "twotox": 1 + x * Fraction(693147181, 10 ** 9), "tentox": 1 + x * Fraction(2302585093, 10 ** 9),
    }[fn]


def lattice_neighbors(t, prec):
    """t（0 でない Fraction）を挟む、精度 prec(80/32/64) の隣接 2 値（t が表現可能なら同じ値 2 つ）。"""
    p, emin, _ = FORMATS[prec]
    mag = abs(t)
    e = max(floor_log2(mag), emin)
    q = Fraction(2) ** (e - (p - 1))
    whole = (mag / q).numerator // (mag / q).denominator
    lo, hi = whole * q, (whole + 1) * q
    if lo == mag:
        hi = lo
    dist = min(mag - lo, hi - mag) / q if lo != hi else Fraction(1)
    if t < 0:
        lo, hi = -hi, -lo
    return lo, hi, dist >= Fraction(1, 2 ** 40)


def encode_signed(v, negative):
    """符号つきの値を X 形式へ。0 は negative に従って ±0（負の t の隣接値 0 は -0）。"""
    if v == 0:
        return (0x8000 if negative else 0), 0
    return encode_x(1 if v < 0 else 0, abs(v))


def rand_ext(rng, e_lo, e_hi):
    """指数 E（アンバイアス）が [e_lo, e_hi] の乱数 (sexp, signif)（正規化数）。"""
    e = rng.randint(e_lo, e_hi)
    sign = rng.getrandbits(1)
    sig = rng.getrandbits(64) | J
    return ((sign << 15) | (e + BIAS), sig)


def ext_of(v, exact_only=False):
    """Fraction v を最も近い拡張精度値（RN）へ丸めた (sexp, signif)。"""
    if v == 0:
        return 0, 0
    sign = 1 if v < 0 else 0
    mag = abs(v)
    rounded, _ = round_to_grid(sign, mag, 64, EMIN_X, RN)
    return encode_x(sign, rounded)


PI_FRAC = None


def pi_frac():
    global PI_FRAC
    if PI_FRAC is None:
        PI_FRAC = as_fraction(fixed_pi())
    return PI_FRAC


def tr_inputs(fn, rng, count):
    """関数ごとの入力（(sexp, signif) のリスト）。定義域の内側の有限値だけ。"""
    out = []
    def add(sexp, sig):
        out.append((sexp, sig))
    def rnd(lo, hi):
        add(*rand_ext(rng, lo, hi))
    if fn in ("sin", "cos", "tan"):
        for _ in range(count):
            rnd(-45, 12)
        for _ in range(count // 3):
            rnd(13, 16000)
        for k in list(range(1, 40)) + [2 ** 20 + 1, 2 ** 40 + 3, 10 ** 30, 2 ** 200 + 1, 2 ** 5000 + 7]:
            base = ext_of(Fraction(k) * pi_frac() / 2)     # kπ/2 に最も近い拡張精度値（縮約後が極小）
            add(*base)
            add(base[0] ^ 0x8000, base[1])
        for e in (-1, 0, 1, 62, 63, 64):                   # 縮約の切替点付近
            rnd(e, e)
        add(0x3FFE, J)                                     # 0.5
        add(0x3FFE, J - 1 if False else 0xFFFFFFFFFFFFFFFF)  # 0.5 未満の最大
    elif fn == "atan":
        for _ in range(count):
            rnd(-45, 40)
        for _ in range(count // 4):
            rnd(41, 16000)
        for e in (-1, 0):
            rnd(e, e)
        add(0x3FFF, J); add(0x3FFF, J | 1); add(0x3FFE, 0xFFFFFFFFFFFFFFFF)
    elif fn in ("asin", "acos", "atanh"):
        for _ in range(count):
            rnd(-45, -1)
        for k in range(1, 70):                             # 1 に近い値（1 - 2^-k）
            add(0x3FFE, (1 << 64) - (1 << max(0, 64 - k)) if k < 64 else 0xFFFFFFFFFFFFFFFF)
            add(0xBFFE, (1 << 64) - (1 << max(0, 64 - k)) if k < 64 else 0xFFFFFFFFFFFFFFFF)
        add(0x3FFF, J); add(0x3FFF, J)  # ±1 は特殊値（生成側で除く）
    elif fn == "etox":
        for _ in range(count):
            rnd(-100, 13)
        for _ in range(count // 4):
            add((rng.getrandbits(1) << 15) | (BIAS + 13), rng.getrandbits(64) | J)   # 8192..16384 の大きさ
        for v in (Fraction(11350), Fraction(11356), Fraction(-11355), Fraction(-11390), Fraction(-11400)):
            add(*ext_of(v)); 
    elif fn in ("logn", "log2", "log10"):
        for _ in range(count):
            e = rng.choice([rng.randint(-16445, 16383), rng.randint(-60, 60), rng.randint(-8, 8)])
            if e < -16382:      # 非正規化数
                add(0, rng.getrandbits(max(1, e + 16446)) | 1)
            else:
                add(e + BIAS, rng.getrandbits(64) | J)
        for k in range(20, 64):                            # 1 の近傍
            add(0x3FFF, J | (1 << (63 - k)) if k < 63 else J | 1)
            add(0x3FFE, (1 << 64) - (1 << max(0, 63 - k)) if k < 63 else 0xFFFFFFFFFFFFFFFF)
    elif fn in ("sinh", "cosh", "tanh"):
        top = 5 if fn == "tanh" else 13
        for _ in range(count):
            rnd(-45, top)
        for e in (-2, -1, 0):
            rnd(e, e)
        add(0x3FFE, J); add(0x3FFD, 0xFFFFFFFFFFFFFFFF)
        if fn != "tanh":
            for v in (Fraction(11355), Fraction(-11355), Fraction(11356)):
                add(*ext_of(v))
    elif fn == "etoxm1":
        for _ in range(count):
            rnd(-100, 13)
        for _ in range(count // 4):
            add((rng.getrandbits(1) << 15) | (BIAS + 13), rng.getrandbits(64) | J)
        for v in (Fraction(1, 2), Fraction(-1, 2), Fraction(83), Fraction(-83), Fraction(84), Fraction(-84),
                  Fraction(83, 1) + Fraction(1, 3), Fraction(-90), Fraction(-1000), Fraction(11350),
                  Fraction(-11355), Fraction(-20000), Fraction(-30000)):
            add(*ext_of(v))
        for v in (42, -42, 43, -43, 44, -44, Fraction(89, 2), Fraction(-89, 2), 50, -50, 60, -60, 70, -70, 80, -80,
                  Fraction(165, 2), Fraction(-165, 2), 87, -87):
            add(*ext_of(Fraction(v)))                        # k=61〜125（-1 を引く経路の上端。k = ±120 の切替の前後）
        for e in range(-40, -1):                             # 級数と wide_exp の切替（2^-40〜0.5）
            rnd(e, e)
            rnd(e, e)
        add(0x3FFD, 0xFFFFFFFFFFFFFFFF); add(0xBFFD, 0xFFFFFFFFFFFFFFFF)     # 0.5 未満の最大（級数の側）
        add(0x3FFE, J | 1); add(0xBFFE, J | 1)                                # 0.5 の次（wide_exp の側）
        for v in (Fraction(1), Fraction(-1), Fraction(3, 2), Fraction(-3, 2), Fraction(7, 4), Fraction(-7, 4),
                  Fraction(5, 8), Fraction(-3, 4)):
            add(*ext_of(v))
        add(0x3FFF, 0xFFFFFFFFFFFFFFFF); add(0xBFFF, 0xFFFFFFFFFFFFFFFF)     # 2 未満の最大
    elif fn == "lognp1":
        for _ in range(count):
            rnd(-100, -1)
        for _ in range(count // 3):
            rnd(0, 16000)
        for k in range(1, 70):                             # -1 に近い値 -(1 - 2^-k)（1+x が小さい）
            add(0xBFFE, (1 << 64) - (1 << max(0, 64 - k)) if k < 64 else 0xFFFFFFFFFFFFFFFF)
        for sexp, sig in ((0x3FFD, J), (0x3FFD, 0xFFFFFFFFFFFFFFFF), (0xBFFD, J), (0xBFFD, 0xFFFFFFFFFFFFFFFF),
                          (0x3FFC, 0xFFFFFFFFFFFFFFFF), (0xBFFC, 0xFFFFFFFFFFFFFFFF), (0x3FFF, J)):
            add(sexp, sig)                                 # 級数と ln(1+x) の切替点（|x| = 0.25）
        for v in (Fraction(2) ** 60, Fraction(2) ** 5000):
            add(*ext_of(v))
    elif fn == "twotox":
        for _ in range(count):
            rnd(-100, 14)
        for _ in range(count // 4):
            add((rng.getrandbits(1) << 15) | (BIAS + 14), rng.getrandbits(64) | J)
        for v in (Fraction(32767, 2), Fraction(32767), Fraction(-32767), Fraction(-32768), Fraction(-40000),
                  Fraction(32768), Fraction(131071, 2), Fraction(16383) + Fraction(1, 2),
                  Fraction(-16446), Fraction(-16446) - Fraction(1, 2), Fraction(-16447) - Fraction(1, 3),
                  Fraction(-16384) - Fraction(1, 3), Fraction(1, 2), Fraction(-1, 2), Fraction(3, 2)):
            add(*ext_of(v))
    elif fn == "tentox":
        for _ in range(count):
            rnd(-100, 12)
        for _ in range(count // 4):
            add((rng.getrandbits(1) << 15) | (BIAS + 12), rng.getrandbits(64) | J)
        for v in (Fraction(4931), Fraction(4932), Fraction(4932) + Fraction(1, 10), Fraction(4933), Fraction(-4930),
                  Fraction(-4950), Fraction(-4951), Fraction(-4952), Fraction(5000), Fraction(-5000),
                  Fraction(8191), Fraction(-8191), Fraction(8192), Fraction(-8192), Fraction(20000), Fraction(28),
                  Fraction(-1), Fraction(-27), Fraction(1, 2), Fraction(-1, 2), Fraction(63), Fraction(64),
                  Fraction(31) + Fraction(1, 4)):
            add(*ext_of(v))
    # 極小引数（設計書 §5.2「極小引数の方向」）。正規化数と非正規化数
    if fn in TINY_LIMIT:
        for _ in range(60):
            add(*rand_ext(rng, -TINY_LIMIT[fn] - 60, -TINY_LIMIT[fn] - 1))
        for _ in range(8):
            add(((rng.getrandbits(1) << 15) | 0), rng.getrandbits(rng.randint(1, 63)) | 1)   # 非正規化数
    # 以下は乱数を消費しない固定の入力（第三者レビュー B1-B4。他の関数の乱数列を変えない）。
    # 2 の冪の極小引数: 補正項が負の向きのとき 128bit 表現が binade をまたぐ（fpu_tiny_result() の正規化し直し）
    if fn in TINY_LIMIT:
        for sexp, sig in ((BIAS - TINY_LIMIT[fn] - 7, J), (BIAS - 16000, J), (0, 1 << 40), (1, J)):
            add(sexp, sig)
            add(sexp | 0x8000, sig)
    # FACOS の |x| < 2^-70（π/2 を丸めて返す経路。fpu_transcend_atan.c の ACOS_TINY_EXP）
    if fn == "acos":
        for sexp, sig in ((BIAS - 71, 0xFFFFFFFFFFFFFFFF), (BIAS - 500, J), (1, J | 0x123456789), (0, 0x123456789)):
            add(sexp, sig)
            add(sexp | 0x8000, sig)
    # FTANH の |x| >= 64（1 のごく近くを返す経路。fpu_transcend_hyp.c の TANH_ONE_EXP）
    if fn == "tanh":
        for v in (Fraction(64), Fraction(100), Fraction(2) ** 20):
            add(*ext_of(v))
            add(*ext_of(-v))
    # FETOX の |x| >= 2^14 の負側（指数を飽和させてアンダーフローさせる経路。fpu_wide.c の EXP_SATURATE_FIELD）
    if fn == "etox":
        for v in (Fraction(-16384), Fraction(-20000), Fraction(-30000)):
            add(*ext_of(v))
    return out


def tr_exact_cases(fn):
    """入力 -> 厳密な結果（Fraction）。INEX2 が立たない入力（設計書 §5.2 の厳密ケース表）。"""
    out = []
    if fn == "log2":
        for n in (-3, -1, 1, 2, 10, 100, -100, 16000, -16000):
            out.append((ext_of(Fraction(2) ** n), Fraction(n)))
    if fn == "log10":
        for n in range(1, 28):
            out.append((ext_of(Fraction(10) ** n), Fraction(n)))
    if fn == "twotox":       # 2^n（整数 n）は厳密。範囲外の n は PREC=X でも表現できないので入れない
        for n in (-16446, -16383, -1000, -100, -3, -1, 1, 2, 10, 100, 1000, 16383):
            out.append((ext_of(Fraction(n)), Fraction(2) ** n))
    if fn == "tentox":       # 10^n（n = 1..27）は拡張精度で厳密
        for n in range(1, 28):
            out.append((ext_of(Fraction(n)), Fraction(10) ** n))
    return out


def gen_b1_transcend():
    rng = random.Random(59004)
    writers = []
    for fn in FORCE_CLEAR:
        w = Writer("%s_tr_%s.txt" % ("b2" if fn in B2_TR_FUNCS else "b1", fn))
        force = FORCE_CLEAR[fn]
        base = tr_inputs(fn, rng, 220)
        cases = []
        for pc, prec in PREC_CODES:
            if pc == 0:
                cases += [(pc, prec, x) for x in base]
            else:
                cases += [(pc, prec, x) for x in base[:: max(1, len(base) // 40)]]
        for sexp, sig in [c[2] for c in cases if False]:
            pass
        for pc, prec, (sexp, sig) in cases:
            x = value_of(sexp, sig)
            if fn in ("asin", "acos", "atanh") and abs(x) >= 1:
                continue
            if fn == "lognp1" and x <= -1:
                continue
            if x == 0:
                continue
            tiny = fn in TINY_LIMIT and floor_log2(abs(x)) < -TINY_LIMIT[fn]
            t = taylor_tiny(fn, x) if tiny else mp_eval(fn, x)
            if fn == "tanh" and abs(t) == 1:
                # 1 - |tanh x| = 2e^(-2|x|)/(1+e^(-2|x|)) が TR_MP_PREC bit で分解できない |x|。真値は ±1 より内側
                # （差は非零）なので、内側の値に置き換えて隣接 2 値と向きを決める
                t = t * (1 - Fraction(1, 2 ** (TR_MP_PREC + 64)))
            if fn == "etoxm1" and t == -1:
                # e^x - 1 = -1 + e^x が TR_MP_PREC bit で分解できない x（大きな負）。真値は -1 より内側（差は非零）
                t = t * (1 - Fraction(1, 2 ** (TR_MP_PREC + 64)))
            if t == 0 or (fn in ("logn", "log2", "log10") and x <= 0):
                continue
            p, emin, emax = FORMATS[prec]
            if prec != 80 and not (Fraction(2) ** -120 < abs(t) < Fraction(2) ** 100):
                continue
            if prec == 80 and abs(t) >= max_finite(80):
                continue
            lo, hi, directed = lattice_neighbors(t, prec)
            if tiny:
                directed = True       # 特殊経路が方向を保証する。mpmath の真値は x との差を分解できる
            exact = lo == hi
            exc = 0 if exact else EXC_INEX2
            if floor_log2(abs(t)) + BIAS < MIN_NORM_EXP[prec]:
                exc |= EXC_UNFL
            exc &= ~force
            neg = t < 0
            (lsexp, lsig), (hsexp, hsig) = encode_signed(lo, neg), encode_signed(hi, neg)
            w.add("tr %d %s %s => %s %s %s %s %d %d %x" % (
                pc, hx(sexp, 4), hx(sig), hx(lsexp, 4), hx(lsig), hx(hsexp, 4), hx(hsig),
                1 if directed else 0, 1 if neg else 0, exc))
        for (sexp, sig), res in tr_exact_cases(fn):
            for pc, prec in PREC_CODES:
                rr, _, _ = round_result(1 if res < 0 else 0, abs(res), prec, RN)
                if prec != 80 and rr != encode_signed(res, res < 0):
                    continue
                exc = 0                                    # 厳密でも tiny なら UNFL（丸め前の指数で判定。roundPack の規則）
                if floor_log2(abs(res)) + BIAS < MIN_NORM_EXP[prec]:
                    exc |= EXC_UNFL
                w.add("tr %d %s %s => %s %s %s %s %d %d %x" % (
                    pc, hx(sexp, 4), hx(sig), hx(rr[0], 4), hx(rr[1]), hx(rr[0], 4), hx(rr[1]), 1,
                    1 if res < 0 else 0, exc & ~force))
        writers.append(w)
    return writers


def gen_b1_const():
    """無理数を返す特殊値（±π/2・π。設計書 §3.2 の表・§5.2 の表）の PREC・RM 依存。正しく丸めた値（ビット一致）。
    π は Machin 公式の固定小数点値（fixed_pi）を Fraction の格子丸め（round_result）で丸める。"""
    w = Writer("b1_const.txt")
    pi = pi_frac()
    inf, one = (0x7FFF, J), (BIAS, J)
    cases = [("atan", inf, 0, pi / 2), ("atan", (0x8000 | inf[0], J), 1, pi / 2),
             ("asin", one, 0, pi / 2), ("asin", (0x8000 | one[0], J), 1, pi / 2),
             ("acos", (0, 0), 0, pi / 2), ("acos", (0x8000, 0), 0, pi / 2),
             ("acos", (0x8000 | one[0], J), 0, pi)]
    for fn, (sexp, sig), sign, mag in cases:
        for pc, prec in PREC_CODES:
            for mode in MODES:
                out, inexact, _ = round_result(sign, mag, prec, mode)
                assert inexact
                w.add("const %s %d %d %s %s => %s %s %x" % (
                    fn, pc, mode, hx(sexp, 4), hx(sig), hx(out[0], 4), hx(out[1]), EXC_INEX2))
    return w


# ---- Phase B1: 超越関数の float128 中間値（設計書 §5.4 の誤差予算の検証） -------------------------------

CORE_LIMIT = Fraction(11300)        # exp/sinh/cosh の入力の大きさの上限（結果が f128 の範囲に収まる）


def core_inputs(fn, rng):
    """core 検査の入力 (sexp, signif)。指数フィールド >= 1 の正規化数で、特殊経路に入らないもの。"""
    out = []
    for sexp, sig in tr_inputs(fn, rng, 260):
        e = sexp & 0x7FFF
        x = abs(value_of(sexp, sig))
        if e < 1 or x == 0:
            continue
        lim = 100 if fn == "etox" else 40
        if fn in TINY_LIMIT and floor_log2(x) < -TINY_LIMIT[fn] and fn != "etox":
            continue
        if fn == "etox" and floor_log2(x) < -100:
            continue
        if fn in ("etox", "sinh", "cosh") and x > CORE_LIMIT:
            continue
        if fn in ("asin", "acos", "atanh") and x >= 1:
            continue
        if fn == "tanh" and x >= 64:
            continue
        if fn in ("logn", "log2", "log10") and sexp >> 15:
            continue
        if fn == "acos" and floor_log2(x) < -70:
            continue
        out.append((sexp, sig))
    return out


def gen_b1_core():
    w = Writer("b1_core.txt")
    rng = random.Random(59005)
    jobs = [("sin", "sin"), ("cos", "cos"), ("etox", "etox"), ("logn", "ln"), ("log2", "log2"),
            ("log10", "log10"), ("atan", "atan"), ("asin", "asin"), ("acos", "acos"),
            ("sinh", "sinh"), ("cosh", "cosh"), ("tanh", "tanh"), ("atanh", "atanh")]
    for src_fn, name in jobs:
        for sexp, sig in core_inputs("sin" if name == "cos" else src_fn, rng):
            x = value_of(sexp, sig)
            if name in ("sin", "cos", "sinh", "cosh", "tanh", "atanh", "atan", "asin"):
                x = abs(x)
                sexp &= 0x7FFF
            if name in ("logn", "log2", "log10"):
                x = abs(x)
                sexp &= 0x7FFF
            t = mp_eval({"ln": "logn"}.get(name, name), x)
            hi, lo = f128_words(t)
            w.add("core %s %s %s => %s %s" % (name, hx(sexp, 4), hx(sig), hx(hi), hx(lo)))
    return w


# ---- Phase B2: FREM / FSGLMUL / FSGLDIV / FSCALE / FGETEXP / FGETMAN（ビット一致。設計書 §5.3） ------------

def rem_nearest(a, b):
    """|dst|=a, |src|=b（Fraction、正）から IEEE 剰余。戻り値は (N, 剰余の大きさ, 符号が反転したか)。N は最近接偶数。"""
    q = a // b
    r = a - b * q
    if 2 * r > b or (2 * r == b and q % 2 == 1):
        return q + 1, b - r, True
    return q, r, False


def gen_b2_rem():
    w = Writer("b2_rem.txt")
    rng = random.Random(59006)
    cases = []
    for _ in range(300):
        de = rng.choice([BIAS, BIAS + 5, BIAS + 70, BIAS - 60, 0x4200, 0x7FFE, 0x0100, 1, 0,
                         BIAS + 3, BIAS + 64, BIAS + 130, BIAS - 1, BIAS - 2])
        se = rng.choice([BIAS, BIAS - 3, BIAS + 2, BIAS - 100, 0x0100, 0, 1, 0x3E00, BIAS + 64,
                         0x7FFE, 0x7FFD, 0x7000, BIAS + 1])
        cases.append((rand_finite(rng, [de]), rand_finite(rng, [se])))
    one, half = (BIAS, J), (BIAS - 1, J)
    tie_cases = [((BIAS + 1, 0xA000000000000000), one),            # 2.5 / 1: N=2（偶数）、剰余 +0.5
                 ((BIAS + 1, 0xE000000000000000), one),            # 3.5 / 1: N=4、剰余 -0.5
                 ((BIAS, 0xC000000000000000), one),                # 1.5 / 1: N=2、剰余 -0.5
                 ((BIAS - 1, J), one),                             # 0.5 / 1: d=-1 の半分ちょうど、N=0
                 ((BIAS - 1, J | 1), one),                         # 0.5 + ulp: N=1、剰余 -(0.5 - ulp)
                 ((BIAS - 1, 0xFFFFFFFFFFFFFFFF), one),            # 0.5 未満の最大
                 ((BIAS - 1, 0xC000000000000000), one),            # 0.75: N=1、剰余 -0.25
                 ((BIAS - 2, 0xFFFFFFFFFFFFFFFF), one),            # d=-2: N=0
                 ((BIAS, J), one),                                 # 割り切れる
                 ((BIAS + 62, J | 1), one),                        # 2^62 + 0.5: N=2^62（偶数）、剰余 +0.5
                 ((BIAS + 62, J | 3), one),                        # 2^62 + 1.5: N=2^62+2、剰余 -0.5
                 ((BIAS + 62, J | 5), one),                        # 2^62 + 2.5: N=2^62+2（偶数）、剰余 +0.5
                 ((BIAS + 3, J), (BIAS + 1, 0xC000000000000000)),  # 8 / 3: N=3、剰余 -1
                 ((BIAS + 2, 0xC000000000000000), (BIAS + 1, J)),  # 6 / 2: 割り切れる
                 ((BIAS + 2, 0xE000000000000000), (BIAS + 1, J)),  # 7 / 2: N=4（3.5 の半分ちょうど）
                 ((0x7FFE, J), (0x0000, 1)),                       # 最大指数差
                 ((0x7FFE, (1 << 64) - 1), (0x0001, J)),
                 ((0x0000, 1), (0x0000, 3)),                       # 非正規化数どうし: N=0
                 ((0x0000, 2), (0x0000, 3)),                       # 2/3: N=1、剰余 -1（非正規化数）
                 ((0x0000, 3), (0x0000, 2))]                       # 3/2: N=2、剰余 -1
    for base in tie_cases:
        cases.append(base)
        cases.append((((base[0][0] ^ 0x8000), base[0][1]), base[1]))
        cases.append((base[0], ((base[1][0] ^ 0x8000), base[1][1])))
    for (dsexp, dsig), (ssexp, ssig) in cases:
        dv, sv = value_of(dsexp, dsig), value_of(ssexp, ssig)
        if dv == 0 or sv == 0:
            continue
        dsign, ssign = dsexp >> 15, ssexp >> 15
        q, rem, flip = rem_nearest(abs(dv), abs(sv))
        rsign = dsign ^ (1 if flip else 0)
        for pc, prec in PREC_CODES:
            for mode in (MODES if pc != 0 else (RN, RZ)):
                if rem == 0:
                    out, exc = (dsign << 15, 0), 0
                else:
                    out, inexact, _ = round_result(rsign, rem, prec, mode)
                    exc = 0                               # INEX2 は常に 0 に強制（[881UM] p.4-87）。UNFL は tiny のとき
                    if floor_log2(rem) + BIAS < MIN_NORM_EXP[prec]:
                        exc |= EXC_UNFL
                w.add("rem %d %d %s %s %s %s => %s %s %x %d %d" % (
                    pc, mode, hx(dsexp, 4), hx(dsig), hx(ssexp, 4), hx(ssig),
                    hx(out[0], 4), hx(out[1]), exc, dsign ^ ssign, q % 128))
    return w


SGL_KEEP = 24        # FSGLMUL/FSGLDIV が入力の仮数に残す有効数字（設計書 §5.3・O6・U19）
EXC_OVFL_B2 = 0x10
EXC_DZ = 0x04


def trunc24(sexp, sig):
    """(sexp, sig) の大きさを有効数字 24bit へ 0 方向に切り詰めた Fraction（非ゼロの有限値）。"""
    mag = abs(value_of(sexp, sig))
    q = Fraction(2) ** (floor_log2(mag) - (SGL_KEEP - 1))
    return (mag / q).numerator // (mag / q).denominator * q


def round_sxr(sign, mag, mode):
    """mag(>0) を S_XR 書式（仮数 24bit・指数範囲は拡張精度。tiny は拡張精度の量子）で丸めた
    ((sexp, signif), exc)。設計書 §5.1-5。"""
    exc = 0
    if floor_log2(mag) < -BIAS:                       # tiny（丸め前の指数が拡張精度の最小指数未満）
        out, inexact, _ = round_result(sign, mag, 80, mode)
        exc = EXC_UNFL | (EXC_INEX2 if inexact else 0)
        return out, exc
    rounded, inexact = round_to_grid(sign, mag, SGL_KEEP, -BIAS, mode)
    if rounded >= Fraction(2) ** (BIAS + 1):          # 拡張精度の最大指数を超えた: OVFL
        to_inf = mode == RN or (mode == RM and sign == 1) or (mode == RP and sign == 0)
        if to_inf:
            return ((sign << 15) | 0x7FFF, J), EXC_OVFL_B2 | EXC_INEX2
        return ((sign << 15) | 0x7FFE, (1 << 64) - 1), EXC_OVFL_B2 | EXC_INEX2
    return encode_x(sign, rounded), EXC_INEX2 if inexact else 0


def sgl_div_sticky_cases():
    """商の 24bit 丸めで、捨てるビットがちょうど半分の形（100…0）になり、余りが非零の (a, b)（24bit 整数）。
    余りを sticky に畳まないと RN がタイに見える境界。固定の乱数で探す（設計書 §5.6）。"""
    rng = random.Random(59007)
    found = []
    while len(found) < 6:
        a = rng.getrandbits(24) | (1 << 23)
        b = rng.getrandbits(24) | (1 << 23)
        num = a << 40
        q, rem = num // b, num % b
        drop = 16 if q >> 40 else 17                  # 商は 40〜41bit。24bit を残して捨てるビット数
        if rem != 0 and q & ((1 << drop) - 1) == 1 << (drop - 1):
            found.append((a, b))
    return found


def sgl_div_tiny_sticky_cases():
    """FSGLDIV の商 (q1:q2)（80〜81bit）で、q2 の下位ビットが全て 0 で余りだけが非零（下位 16〜17bit の零の連なりの後に
    非零が続く）(a, b)。結果が tiny のとき丸め位置が上位 64bit の下端に近いと、余りの sticky を落とすと
    ちょうど半分に見えて偶数丸めになる境界（設計書 §5.6）。戻り値は (a, b, k)。k = hi の下位から数えた最初の 1 の
    位置 + 1 で、結果の idealized 指数が -k のとき hi の捨てるビットが 100…0 になる。"""
    rng = random.Random(59012)
    found = []
    while len(found) < 12:
        a = rng.getrandbits(24) | (1 << 23)
        b = rng.getrandbits(24) | (1 << 23)
        q1, r1 = (a << 40) // b, (a << 40) % b
        q2, r2 = (r1 << 40) // b, (r1 << 40) % b
        lead = 64 - q1.bit_length()
        q2_low = 40 - lead
        if r2 == 0 or q2 & ((1 << q2_low) - 1) != 0:
            continue
        hi = ((q1 << 40 | q2) >> q2_low) & ((1 << 64) - 1)
        k = ((hi & -hi).bit_length())                # 最下位の 1 の位置 + 1
        if k <= 16:
            found.append((a, b, k))
    return found


def gen_b2_sgl():
    w = Writer("b2_sgl.txt")
    rng = random.Random(59008)
    cases = []
    for _ in range(260):
        ee = [BIAS, BIAS + 5, BIAS - 5, 0x7FFE, 0x7FFD, 0x7000, 0x1000, 1, 0, 2, BIAS + 64, BIAS - 64, 0x4000, 0x3E00,
              0x0100, 0x7E00]
        cases.append((rand_finite(rng, [rng.choice(ee)]), rand_finite(rng, [rng.choice(ee)])))
    one = (BIAS, J)
    cases += [((BIAS, 0x8008000000000000), (BIAS, 0x8008000000000000)),      # (1+2^-12)^2: 24bit 目の半分ちょうど（積）
              ((BIAS, 0xFFFFFFFFFFFFFFFF), (BIAS, 0xFFFFFFFFFFFFFFFF)),      # 仮数の下位 40bit は切り詰め
              ((BIAS, J | 0xFFFFFFFFFF), one),                               # 下位 40bit だけが立つ: 1.0 に切り詰め
              ((0x7FFE, 0xFFFFFF0000000000), (BIAS + 1, J)),                # 最大近くの乗算でオーバーフロー
              ((0x7FFE, 0xFFFFFF0000000000), (BIAS, 0xFFFFFF0000000000)),   # 丸めで最大指数を超える（積が 2^16384 直下）
              ((0, 1), (0, 1)),                                              # 非正規化数どうし
              ((0, 0x0000FFFFFFFFFFFF), (BIAS - 10, J)),                     # 非正規化数の切り詰め（正規化してから 24bit）
              ((1, J), (BIAS - 1, J)),                                       # 最小正規化数 × 0.5: tiny
              ((1, J), (BIAS - 30, 0xC000000000000000))]
    for a, b in sgl_div_sticky_cases():
        cases.append(((BIAS, a << 40), (BIAS, b << 40)))
    for a, b, k in sgl_div_tiny_sticky_cases():
        q1 = (a << 40) // b
        lead = 64 - q1.bit_length()
        s = k - (lead - 23)                           # 結果の idealized 指数 = lead - 23 - s... を -k にする dst の右シフト量
        assert s >= 0
        cases.append(((0, (a << 40) >> s), (BIAS, b << 40)))
    for (dsexp, dsig), (ssexp, ssig) in cases:
        dv, sv = value_of(dsexp, dsig), value_of(ssexp, ssig)
        if dv == 0 or sv == 0:
            continue
        sign = (dsexp >> 15) ^ (ssexp >> 15)
        a, b = trunc24(dsexp, dsig), trunc24(ssexp, ssig)
        for op, mag in (("mul", a * b), ("div", a / b)):
            for pc in (0, 1, 2):                      # PREC は無視される
                for mode in MODES:
                    out, exc = round_sxr(sign, mag, mode)
                    w.add("sgl %s %d %d %s %s %s %s => %s %s %x" % (
                        op, pc, mode, hx(dsexp, 4), hx(dsig), hx(ssexp, 4), hx(ssig), hx(out[0], 4), hx(out[1]), exc))
    return w


SCALE_FORCE = 1 << 14   # |INT(src)| がこれ以上なら dst によらず OVFL/UNFL（[881UM] p.4-93。設計書 §5.3）
SCALE_HUGE = 70000      # 強制時のオラクルの冪（どの dst でも OVFL/UNFL になる大きさ）


def gen_b2_scale():
    w = Writer("b2_scale.txt")
    rng = random.Random(59009)
    dsts = [rand_finite(rng, [rng.choice(exp_choices_wide())]) for _ in range(90)]
    dsts += [(BIAS, J), (0x8000 | BIAS, J), (BIAS, 0xFFFFFFFFFFFFFFFF), (0, 1), (0x8000, 3), (0, J >> 3),
             (0x7FFE, J), (1, J), (BIAS, J | 1), (BIAS - 100, J), (BIAS + 100, J)]
    srcs = []
    for n in list(range(-5, 6)) + [63, 64, 100, -100, 1000, -1000, 16383, -16383, 16384, -16384, 16385, 32766, -32766,
                                    32767, 49000, -49000, 65535, 65536, -65536, 70000, -70000]:
        srcs.append(ext_of(Fraction(n)))
    for u in range(-3, 40):
        srcs.append(rand_ext(rng, u, u))
    srcs += [(0x3FFE, J), (0x3FFE, 0xFFFFFFFFFFFFFFFF), (0xBFFE, 0xFFFFFFFFFFFFFFFF), (0x4000, 0xE000000000000000),
             (0xC000, 0xE000000000000000), (0x3FFF, 0xFFFFFFFFFFFFFFFF), (0, 5), (0x8000, 5), (BIAS + 100, J), (0x7FFE, J)]
    for i, (dsexp, dsig) in enumerate(dsts):
        dv = value_of(dsexp, dsig)
        if dv == 0:
            continue
        dsign = dsexp >> 15
        for j, (ssexp, ssig) in enumerate(srcs):
            if (i + j) % 12 != 0 and not (dsexp, dsig) in ((BIAS, J), (0x7FFE, J), (0, 1), (1, J), (BIAS - 100, J), (BIAS + 100, J)):
                continue                              # 組み合わせを間引く（件数を抑える）
            sv = value_of(ssexp, ssig)
            n = int(abs(sv))                          # 0 方向
            if n >= SCALE_FORCE:
                n = SCALE_HUGE
            if sv < 0:
                n = -n
            mag = abs(dv) * Fraction(2) ** n
            for pc, prec in PREC_CODES:
                for mode in (MODES if pc != 0 or abs(n) > 60 else (RN, RZ)):
                    out, inexact, ovfl = round_result(dsign, mag, prec, mode)
                    exc = EXC_OVFL_B2 if ovfl else 0                   # INEX2 は 0 に強制（p.4-94）。src=±0 は採用（Note 1）
                    if sv == 0 and inexact:                            # オーバーフローも inexact を伴う
                        exc |= EXC_INEX2
                    if not ovfl and floor_log2(mag) + BIAS < MIN_NORM_EXP[prec]:
                        exc |= EXC_UNFL
                    w.add("scale %d %d %s %s %s %s => %s %s %x" % (
                        pc, mode, hx(dsexp, 4), hx(dsig), hx(ssexp, 4), hx(ssig), hx(out[0], 4), hx(out[1]), exc))
    return w


def gen_b2_getexp():
    """FGETEXP・FGETMAN。値は PREC・RM によらず厳密（設計書 §5.3 の暫定決定。U17）。非正規化数は正規化してから。"""
    w = Writer("b2_getexp.txt")
    rng = random.Random(59010)
    vals = [rand_finite(rng, [rng.choice(exp_choices_wide() + [BIAS + 1, 0x4000, 0x3FFE])]) for _ in range(80)]
    vals += [(0, 1), (0x8000, 0x8000000000000000 >> 1), (0, J), (0, 0x00000000FFFFFFFF), (0x8000, 3), (1, J),
             (BIAS, J), (BIAS, 0xFFFFFFFFFFFFFFFF), (BIAS - 1, J), (0x8000 | (BIAS - 1), J), (0x7FFE, J)]
    for pc, _ in PREC_CODES:
        for mode in MODES:
            for sexp, sig in vals:
                mag = abs(value_of(sexp, sig))
                sign = sexp >> 15
                e = floor_log2(mag)
                eo = encode_x(1 if e < 0 else 0, Fraction(abs(e)))
                if e == 0:
                    eo = (0, 0)
                mo = encode_x(sign, mag / Fraction(2) ** e)
                w.add("getexp %d %d %s %s => %s %s %s %s" % (pc, mode, hx(sexp, 4), hx(sig), hx(eo[0], 4), hx(eo[1]),
                                                             hx(mo[0], 4), hx(mo[1])))
    return w


CORE_TR = (("expm1s", "etoxm1"), ("log1ps", "lognp1"), ("exp2", "twotox"), ("exp10", "tentox"),
           ("expm1", "etoxm1"), ("log1p", "lognp1"))


def core_inputs_b2(name, rng):
    """b2_core の入力 (sexp, signif)。特殊経路（極小引数・飽和）に入らない正規化数。"""
    out = []
    def add(e, sign=None):
        sg = rng.getrandbits(1) if sign is None else sign
        out.append(((sg << 15) | (e + BIAS), rng.getrandbits(64) | J))
    if name == "expm1s":
        for e in range(-99, -1):                             # |x| < 0.5（指数 e <= -2）
            for _ in range(4):
                add(e)
    elif name == "log1ps":
        for e in range(-99, -2):                             # |x| < 0.25（指数 e <= -3）
            for _ in range(4):
                add(e)
    elif name == "expm1":                                    # 関数としての経路（級数・wide_exp・-1 の省略・near_one の切替）
        for e in range(-99, 13):
            for _ in range(3):
                add(e)
        for _ in range(200):                                 # k = 0..125 の帯（-1 を引く・引かない・near_one の切替の前後）
            v = Fraction(rng.randint(-9000, 9000), 100)
            out.append(ext_of_pair(v))
        for v in (Fraction(1, 2), Fraction(-1, 2)):
            out.append(ext_of_pair(v))
    elif name == "log1p":
        for e in range(-99, 13):
            for _ in range(3):
                add(e)
        for k in range(1, 65):                               # -1 に近い値
            out.append((0xBFFE, (1 << 64) - (1 << max(0, 64 - k)) if k < 64 else 0xFFFFFFFFFFFFFFFF))
        for v in (Fraction(1, 4), Fraction(-1, 4)):
            out.append(ext_of_pair(v))
    elif name == "exp2":
        for e in range(-99, 15):
            for _ in range(3):
                add(e)
        for _ in range(60):
            add(14, rng.getrandbits(1))
    else:
        for e in range(-99, 13):
            for _ in range(3):
                add(e)
        for _ in range(60):
            add(12, rng.getrandbits(1))
    return out


def ext_of_pair(v):
    return ext_of(v)


def gen_b2_core():
    w = Writer("b2_core.txt")
    rng = random.Random(59011)
    for name, fn in CORE_TR:
        for sexp, sig in core_inputs_b2(name, rng):
            x = value_of(sexp, sig)
            if name == "exp10" and abs(x) > 4900:
                continue
            if name == "exp2" and abs(x) > 16000:
                continue
            if name == "log1p" and x <= -1:
                continue
            if name == "expm1" and abs(x) > 11300:
                continue
            t = mp_eval(fn, x)
            hi, lo = f128_words(t)
            w.add("core %s %s %s => %s %s" % (name, hx(sexp, 4), hx(sig), hx(hi), hx(lo)))
    return w


# ---- Phase C: packed decimal（設計書 §7。期待値は Fraction の厳密な有理数演算） ----------------------------

def pack_words(sm, se, exp_digits, exp3, dontcare, int_digit, frac_digits):
    """packed の 3 ワード。exp_digits は指数 3 桁（EXP2, EXP1, EXP0 の nibble）、frac_digits は小数 16 桁の nibble。"""
    w0 = (sm << 31) | (se << 30) | (exp_digits[0] << 24) | (exp_digits[1] << 20) | (exp_digits[2] << 16) \
        | (exp3 << 12) | (dontcare << 4) | int_digit
    frac = 0
    for d in frac_digits:
        frac = frac << 4 | d
    return w0, frac >> 32, frac & 0xFFFFFFFF


def packed_value(sm, se, exp_digits, int_digit, frac_digits):
    """(符号, M, n): 値 = M × 10^n。桁の nibble は 10 進と同じ式で足す（非 10 進の桁も同じ。設計書 §7.1）。"""
    m = int_digit
    for d in frac_digits:
        m = m * 10 + d
    e = exp_digits[0] * 100 + exp_digits[1] * 10 + exp_digits[2]
    return sm, m, (-e if se else e) - 16


def gen_c_from_p():
    w = Writer("c_from_p.txt")
    rng = random.Random(59020)
    cases = []
    def add(sm, se, ed, i, fr, exp3=0, dc=0):
        cases.append((sm, se, ed, exp3, dc, i, fr))
    def rnd_digits(n, hi=9):
        return [rng.randint(0, hi) for _ in range(n)]
    for _ in range(260):
        e = rng.choice([0, 1, 5, 16, 17, 27, 100, 308, 500, 999, rng.randint(0, 999), rng.randint(0, 999)])
        add(rng.getrandbits(1), rng.getrandbits(1), [e // 100, e // 10 % 10, e % 10], rng.randint(0, 9), rnd_digits(16),
            rng.randint(0, 15), rng.randint(0, 255))          # EXP3・don't care は入力では無視される
    for e in (999, 998, 0):                                  # 範囲の端
        for se in (0, 1):
            add(0, se, [e // 100, e // 10 % 10, e % 10], 9, [9] * 16)
            add(1, se, [e // 100, e // 10 % 10, e % 10], 1, [0] * 15 + [1])
    for n in range(0, 28):                                   # 10^n（拡張精度で厳密なのは n<=27）
        add(0, 0, [n // 100, n // 10 % 10, n % 10], 1, [0] * 16)
    add(0, 0, [0, 2, 7], 2, [0] * 16)                        # 2×10^27
    add(0, 0, [0, 4, 3], 0, [7, 2, 0, 5, 7, 5, 9, 4, 0, 3, 7, 9, 2, 7, 9, 3])
    for _ in range(40):                                      # 非 10 進の桁を含む入力（値は 10 進と同じ式）
        e = [rng.randint(0, 15), rng.randint(0, 15), rng.randint(0, 15)]
        add(rng.getrandbits(1), rng.getrandbits(1), e, rng.randint(0, 15), rnd_digits(16, 15))
    for ed in ([15, 15, 14], [15, 14, 15], [14, 15, 15], [15, 15, 0], [15, 15, 9]):   # 非 10 進の指数の上端
        for se in (0, 1):
            add(0, se, ed, 15, [15] * 16)
            add(1, se, ed, 0, [15] * 15 + [1])
    add(0, 0, [0, 0, 0], 0, [0] * 15 + [1])                  # 整数桁 0（非正規化した仮数）
    add(0, 1, [9, 9, 9], 0, [0] * 15 + [1])
    for sm, se, ed, exp3, dc, i, fr in cases:
        s_, m, n = packed_value(sm, se, ed, i, fr)
        if m == 0:
            continue
        val = Fraction(m) * Fraction(10) ** n
        for mode in MODES:
            mag, inexact = round_to_grid(sm, val, 64, EMIN_X, mode)
            out = encode_x(sm, mag)
            wd = pack_words(sm, se, ed, exp3, dc, i, fr)
            w.add("from_p %d %08x %08x %08x => %s %s %x" % (mode, wd[0], wd[1], wd[2], hx(out[0], 4), hx(out[1]),
                                                             1 if inexact else 0))
    return w


def floor_log10(mag):
    e = int((mag.numerator.bit_length() - mag.denominator.bit_length()) * 0.30102999566398120)
    while mag >= Fraction(10) ** (e + 1):
        e += 1
    while mag < Fraction(10) ** e:
        e -= 1
    return e


def to_p_oracle(sign, mag, k, mode):
    """mag(>0) を k-factor・mode で packed へ。(3 ワード, exc)。"""
    exc = 0
    if k > 17:
        exc |= 0x20
        k = 17
    e10 = floor_log10(mag)
    ln = k if k > 0 else e10 + 1 - k
    ln = max(1, min(17, ln))
    q = mag / Fraction(10) ** (e10 - ln + 1)
    whole = q.numerator // q.denominator
    frac = q - whole
    if round_increment(sign, mode, frac, whole & 1 == 1):
        whole += 1
    if frac != 0:
        exc |= EXC_INEX2
    if whole == 10 ** ln:
        whole = 10 ** (ln - 1)
        e10 += 1
    digs = [int(c) for c in str(whole).ljust(17, "0")]
    if abs(e10) > 999:
        exc |= 0x20
    m3, e3 = abs(e10) % 1000, abs(e10) // 1000
    return pack_words(sign, 1 if e10 < 0 else 0, [m3 // 100, m3 // 10 % 10, m3 % 10], e3, 0, digs[0], digs[1:]), exc


TO_P_KS = (-64, -20, -17, -5, -3, -1, 0, 1, 2, 3, 5, 14, 16, 17, 18, 30, 63)
TO_P_MODE_KS = (-3, 0, 1, 3, 16, 17)


def to_p_inputs(rng):
    vals = []
    for _ in range(90):                                       # 全指数範囲（非正規化数を含む）
        e = rng.choice([0, 1, rng.randint(0, 0x7FFE), rng.randint(BIAS - 70, BIAS + 70), rng.randint(0, 0x7FFE)])
        sig = rng.getrandbits(64) | (J if e != 0 or rng.random() < 0.5 else 0)
        if e == 0 and sig == 0:
            sig = 1
        vals.append((rng.getrandbits(1) << 15 | e, sig))
    for n in list(range(-8, 9)) + [27, 28, 100, 999, 1000, 4000, 4932, -999, -1000, -4000, -4950]:
        v = Fraction(10) ** n
        for d in (0, -1, 1):                                  # 10^n とその前後の拡張精度値（繰り上がり・桁の境界）
            sexp, sig = ext_of(v)
            sig += d
            if 0 < sig < (1 << 64) and (sig & J or sexp & 0x7FFF == 0):
                vals.append((sexp, sig))
    for m in (17, 5, 1, 3):                                   # 9.99…9 × 10^n（丸めで桁が繰り上がる）
        for n in (-5, 0, 4, 27):
            sexp, sig = ext_of(Fraction(10 ** m - 1, 10 ** m) * Fraction(10) ** (n + 1))
            vals.append((sexp, sig))
            vals.append((sexp | 0x8000, sig))
    for v in (Fraction(1, 2), Fraction(3, 2), Fraction(5, 2), Fraction(7, 2), Fraction(9, 2), Fraction(1, 8),
              Fraction(3, 8), Fraction(25, 2), Fraction(2469, 2), Fraction(45, 4), Fraction(1, 16) * 25, Fraction(25, 1),
              Fraction(15, 1), Fraction(35, 1), Fraction(5, 1)):   # 10 進でちょうど半分になる値（タイの偶数丸め）
        sexp, sig = ext_of(v)
        vals.append((sexp, sig))
        vals.append((sexp | 0x8000, sig))
    for v in (10 ** 17 + 1, 10 ** 17 + 5, 15 * 10 ** 16 + 5, Fraction(2) ** 50 + Fraction(1, 1024)):   # 18 桁目だけが非 0 / 2 進の端数が部分 limb
        vals.append(ext_of(Fraction(v)))
        vals.append(ext_of(-Fraction(v)))
    for v in (255, 155, 2550, 355, 1550, 25500, 35, 45, 95, 995):   # 丸め桁の次が 5・以降が 0 でない/0（k=1 でタイの判定）
        vals.append(ext_of(Fraction(v)))
        vals.append(ext_of(-Fraction(v)))
    for t in range(-16446, 16384):                            # E の推定が真値より大きい 2^t（e10 を減らす補正経路）
        est = (t * 78913) >> 18
        if est > floor_log10(Fraction(2) ** t):
            vals.append(ext_of(Fraction(2) ** t))
    vals.append(ext_of(Fraction(12345678765, 10 ** 6)))       # p.4-69 の例 12345.678765
    vals.append(ext_of(Fraction(1, 10 ** 4)))                 # 0.0001（k=0 で LEN の下限 1）
    vals.append(ext_of(Fraction(2) ** 83 * Fraction(5) ** 27))
    return vals


def gen_c_to_p():
    w = Writer("c_to_p.txt")
    rng = random.Random(59021)
    for sexp, sig in to_p_inputs(rng):
        val = value_of(sexp, sig)
        if val == 0:
            continue
        sign = sexp >> 15
        for k in TO_P_KS:
            for mode in (MODES if k in TO_P_MODE_KS else (RN,)):
                words, exc = to_p_oracle(sign, abs(val), k, mode)
                w.add("to_p %d %d %s %s => %08x %08x %08x %x" % (mode, k, hx(sexp, 4), hx(sig), words[0], words[1],
                                                                  words[2], exc))
    return w


def vector_writers():
    writers = [gen_b1_sqrt(), gen_b1_int(), gen_b1_mod(), gen_b1_const(), gen_b2_rem(), gen_b2_sgl(), gen_b2_scale(),
               gen_b2_getexp(), gen_c_from_p(), gen_c_to_p()]
    try:
        import mpmath  # noqa: F401
    except ImportError:
        if "--check" in sys.argv:
            return writers                 # 超越関数のベクタは mpmath が無いと再生成できない
        sys.exit("mpmath が必要です: uv run --script tests/fpu/gen_fpu_1b_vectors.py")
    return writers + gen_b1_transcend() + [gen_b1_core(), gen_b2_core()]


def clang_formatted(path, text):
    """C の生成物をリポジトリの .clang-format で整形する。clang-format が無ければ None。"""
    exe = shutil.which("clang-format")
    if exe is None:
        return None
    return subprocess.run([exe, "--assume-filename=" + path], input=text, capture_output=True,
                          text=True, check=True).stdout


def main():
    header, rows, table, used_mpmath = build()
    outputs = [(CSV_PATH, csv_text(header, rows))]
    skipped = []
    for path, text in ((TABLE_PATH, table_text(table)), (TRANSCEND_PATH, transcend_text())):
        formatted = clang_formatted(path, text)
        if formatted is not None:
            outputs.append((path, formatted))
        elif "--check" in sys.argv:
            skipped.append(os.path.relpath(path, ROOT))   # 整形後の形と比べられない
        else:
            sys.exit("clang-format が必要です（C の生成物を整形してから書き込みます）")
    os.makedirs(VECTOR_DIR, exist_ok=True)
    for wr in vector_writers():
        outputs.append((os.path.join(VECTOR_DIR, wr.name), wr.text()))
    if "--check" in sys.argv:
        stale = []
        for path, text in outputs:
            try:
                with open(path) as f:
                    same = f.read() == text
            except OSError:
                same = False
            if not same:
                stale.append(os.path.relpath(path, ROOT))
        if stale:
            print("stale (regenerate with this script): " + ", ".join(stale))
            sys.exit(1)
        notes = ([] if used_mpmath else ["mpmath cross-check skipped"]) + \
                (["clang-format missing: not compared: " + ", ".join(skipped)] if skipped else [])
        print("ROM constants are up to date" + (" (" + "; ".join(notes) + ")" if notes else ""))
        return
    if not used_mpmath:
        sys.exit("mpmath が必要です: uv run --script tests/fpu/gen_fpu_1b_vectors.py")
    for path, text in outputs:
        with open(path, "w") as f:
            f.write(text)
        print("wrote " + os.path.relpath(path, ROOT))


if __name__ == "__main__":
    main()
