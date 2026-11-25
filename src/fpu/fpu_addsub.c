/*
 * fpu_addsub.c — FADD/FSUB共通コアの加減算。
 * 公開APIは include/mc68000/fpu.h、設計は docs/mc68000-internals/reference/fpu.md。
 *
 * SoftFloatの s_addMagsExtF80.c / s_subMagsExtF80.c は使わない（§3.2.2で確認した通り、
 * e=0オペランドの実効指数を暗黙に+1とみなす欠陥が複数箇所にある）。ここでは標準的な
 * 浮動小数点加算アルゴリズム（正規化→指数の大きい方に整列→同符号なら128bit加算・
 * 異符号なら128bit減算→再正規化→roundPack）を自前実装する。128bitのビット演算
 * プリミティブ（add128/sub128/shiftRightJam128/shortShiftLeft128/shortShiftRightJam128）は
 * ISA非依存の汎用ユーティリティなので、SoftFloatのものを改変せずそのまま使う（§3.5）。
 *
 * FSUB（dst - src）はFADDの核（add_signed）へ src の符号を反転して渡すことで実装する。
 * これにより、異符号無限大同士の invalid 判定（[881UM Table 6-2]）もFADD/FSUBの両方で
 * 共通のロジックがそのまま正しく機能する（FSUBのOPERR条件「dst/srcが共に+∞・共に-∞」は、
 * 符号反転後は「符号違いの無限大同士」というFADDのOPERR条件に一致する）。
 */
#include <stdbool.h>
#include <stdint.h>
#include "platform.h"
#include "internals.h"
#include "specialize.h"
#include "softfloat.h"
#include "fpu_internal.h"

static mc68k_fp80_t from_ext80(extFloat80_t r) {
    mc68k_fp80_t v;

    v.sign_exp = r.signExp;
    v.signif = r.signif;
    return v;
}

/* NaN伝播（[881UM §4.5.4]、design §5.2）。呼び出し側は事前条件として
 * fpu_is_nan(a) || fpu_is_nan(b) を満たすこと。dst優先規則は
 * softfloat_propagateNaNExtF80UI()（softfloat_target/の68881固有版）が実装する。
 * 第1引数はdstを渡す。 */
static mc68k_fp80_t nan_result(mc68k_fp80_t a, mc68k_fp80_t b) {
    struct uint128 z =
        softfloat_propagateNaNExtF80UI(a.sign_exp, a.signif, b.sign_exp, b.signif);

    return fpu_make((z.v64 & FPU_SIGN_BIT) != 0, (uint16_t)(z.v64 & FPU_EXP_MASK), z.v0);
}

/* a+b（両者ともNaNではないことが事前条件）。design §3.2.2。 */
static mc68k_fp80_t add_signed(mc68k_fp80_t a, mc68k_fp80_t b, mc68k_fpu_state_t *st) {
    bool a_inf, b_inf, a_zero, b_zero, sign_same, bigger_is_a;
    int32_t eA, eB, expZ, expDiff;
    uint64_t sA, sB, hiBig, loBig, hiSmall, loSmall;
    bool sign;
    mc68k_fp80_t result;
    uint_fast8_t prec;

    a_inf = fpu_is_inf(a);
    b_inf = fpu_is_inf(b);
    if (a_inf || b_inf) {
        if (a_inf && b_inf) {
            if (fpu_sign(a) == fpu_sign(b))
                return a;
            /* 異符号の無限大同士（[881UM Table 6-2]のOPERR。FSUBはdst-srcを
             * add_signed(dst, -src)として実装するため、FSUBの「共に+∞・共に-∞」条件は
             * ここでの「符号違い」条件に一致する。生成されるNaNは仮数全ビット1
             * （[881UM §3.2.5]）。 */
            fpu_commit_exc(st, (uint8_t)MC68K_FPU_EXC_OPERR);
            return fpu_make(false, FPU_EXP_MASK, UINT64_MAX);
        }
        return a_inf ? a : b;
    }

    a_zero = fpu_is_zero(a);
    b_zero = fpu_is_zero(b);
    prec = fpu_rounding_precision(st->fpcr_round_prec);
    if (a_zero && b_zero) {
        if (fpu_sign(a) == fpu_sign(b))
            sign = fpu_sign(a);
        else
            sign = st->fpcr_round_mode == MC68K_FPU_ROUND_MINUS_INF;
        return fpu_make(sign, 0, 0); /* 完全にゼロ同士。UNFL/INEX2は立たない（design §6.6） */
    }
    if (a_zero || b_zero) {
        uint_fast8_t flags;
        mc68k_fp80_t r = fpu_round_finite(a_zero ? b : a, st, prec, &flags);

        fpu_commit_exc(st, fpu_sf_flags_to_exc(flags));
        return r;
    }

    /* 両者とも有限かつ非ゼロ。正規化して指数の大きい方に整列する。 */
    sign_same = fpu_sign(a) == fpu_sign(b);
    fpu_normalize(fpu_exp_field(a), a.signif, &eA, &sA);
    fpu_normalize(fpu_exp_field(b), b.signif, &eB, &sB);

    bigger_is_a = (eA != eB) ? (eA > eB) : (sA >= sB);
    if (bigger_is_a) {
        expZ = eA;
        hiBig = sA;
        hiSmall = sB;
        expDiff = eA - eB;
        sign = fpu_sign(a);
    } else {
        expZ = eB;
        hiBig = sB;
        hiSmall = sA;
        expDiff = eB - eA;
        sign = fpu_sign(b);
    }
    loBig = 0;
    loSmall = 0;
    if (expDiff != 0) {
        /* softfloat_shiftRightJam128() はシフト距離が非ゼロであることが前提
         * （design §3.2.2「プリミティブの契約」）。 */
        struct uint128 sh =
            softfloat_shiftRightJam128(hiSmall, loSmall, (uint_fast32_t)expDiff);
        hiSmall = sh.v64;
        loSmall = sh.v0;
    }

    if (sign_same) {
        /* softfloat_add128() は mod 2^128 で結果を返し129bit目の桁上がりを報告しない
         * ため、桁上がりの有無を自前で検出する（design §3.2.2、Sol再レビュー2回目
         * blocking 1）。 */
        struct uint128 sum = softfloat_add128(hiBig, loBig, hiSmall, loSmall);
        uint64_t loSum = loBig + loSmall;
        bool carryLo = loSum < loBig;
        uint64_t hiSumNoCarry = hiBig + hiSmall;
        bool overflow1 = hiSumNoCarry < hiBig;
        uint64_t hiSum = hiSumNoCarry + (carryLo ? 1u : 0u);
        bool overflow2 = hiSum < hiSumNoCarry;
        bool carryHi = overflow1 || overflow2;
        uint64_t sigHi = sum.v64, sigLo = sum.v0;

        if (carryHi) {
            struct uint128 sh = softfloat_shortShiftRightJam128(sigHi, sigLo, 1);
            sigHi = sh.v64 | FPU_SIG_J;
            sigLo = sh.v0;
            expZ++;
        }
        fpu_sf_begin(st);
        result = from_ext80(softfloat_roundPackToExtF80(sign, expZ, sigHi, sigLo, prec));
        fpu_commit_exc(st, fpu_sf_flags_to_exc(fpu_sf_end()));
        return result;
    }

    {
        /* 異符号: 絶対値が大きい方（Big、整列で一切シフトされていない側）から
         * 小さい方を引く。符号は絶対値が大きい方（Big）に従う（上でsignに設定済み）。 */
        struct uint128 d = softfloat_sub128(hiBig, loBig, hiSmall, loSmall);
        uint64_t sigHi = d.v64, sigLo = d.v0;
        int shiftDist;

        if (sigHi == 0 && sigLo == 0) {
            /* 非ゼロ同士が厳密に打ち消し合う場合（design §3.2.2、IEEE754全般の規則）。 */
            sign = st->fpcr_round_mode == MC68K_FPU_ROUND_MINUS_INF;
            return fpu_make(sign, 0, 0);
        }
        if (sigHi == 0) {
            /* 上位64bitが完全に相殺した。下位64bitを上位へ引き上げて再正規化する
             * （design §3.2.2、Sol再レビューの反例「(0001,8000...)-(0000,ffff...)」）。 */
            sigHi = sigLo;
            sigLo = 0;
            expZ -= 64;
        }
        shiftDist = fpu_clz64(sigHi);
        if (shiftDist != 0) {
            /* softfloat_shortShiftLeft128() はシフト距離1〜63が前提（距離0は
             * 呼ばない。design §3.2.2「プリミティブの契約」）。 */
            struct uint128 sh =
                softfloat_shortShiftLeft128(sigHi, sigLo, (uint_fast8_t)shiftDist);
            sigHi = sh.v64;
            sigLo = sh.v0;
            expZ -= shiftDist;
        }
        fpu_sf_begin(st);
        result = from_ext80(softfloat_roundPackToExtF80(sign, expZ, sigHi, sigLo, prec));
        fpu_commit_exc(st, fpu_sf_flags_to_exc(fpu_sf_end()));
        return result;
    }
}

mc68k_fp80_t mc68k_fpu_add(mc68k_fp80_t dst, mc68k_fp80_t src, mc68k_fpu_state_t *st) {
    mc68k_fp80_t r;

    if (fpu_is_nan(dst) || fpu_is_nan(src)) {
        fpu_commit_exc(st, (fpu_is_snan(dst) || fpu_is_snan(src)) ? (uint8_t)MC68K_FPU_EXC_SNAN
                                                                  : 0);
        r = nan_result(dst, src);
    } else {
        r = add_signed(dst, src, st);
    }
    fpu_set_cc(st, r);
    return r;
}

mc68k_fp80_t mc68k_fpu_sub(mc68k_fp80_t dst, mc68k_fp80_t src, mc68k_fpu_state_t *st) {
    mc68k_fp80_t r, neg_src;

    if (fpu_is_nan(dst) || fpu_is_nan(src)) {
        fpu_commit_exc(st, (fpu_is_snan(dst) || fpu_is_snan(src)) ? (uint8_t)MC68K_FPU_EXC_SNAN
                                                                  : 0);
        r = nan_result(dst, src);
    } else {
        neg_src = fpu_make(!fpu_sign(src), fpu_exp_field(src), src.signif);
        r = add_signed(dst, neg_src, st);
    }
    fpu_set_cc(st, r);
    return r;
}
