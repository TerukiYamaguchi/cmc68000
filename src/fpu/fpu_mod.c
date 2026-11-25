/*
 * fpu_mod.c — FMOD・FREM。
 *
 * 剰余 dst - src*N。FMOD は N = INT(dst ÷ src)（0 方向）、FREM は N = 最近接偶数（IEEE 剰余）。
 * 指数差ぶんのシフト・引き算の繰り返しで正確に求める（SoftFloat の extF80_rem
 * は使わない。§5.1-1）。 商 byte は下位 7bit と符号を設定する。
 */
#include <stdbool.h>
#include <stdint.h>
#include "platform.h"
#include "internals.h"
#include "softfloat.h"
#include "fpu_internal.h"

#define QUOTIENT_MASK 0x7Fu

static void set_quotient(mc68k_fpu_state_t *st, bool sign, uint32_t n) {
    st->fpsr_quotient_sign = sign ? 1 : 0;
    st->fpsr_quotient = (uint8_t)(n & QUOTIENT_MASK);
}

/* 命令ごとの EXC マスク（設計書 §5.2）。「dst の値を返す」経路は Note 2（FMOD p.4-62、FREM
 * p.4-86）に従い FMOD は OVFL・INEX2、FREM は INEX2 を roundPack の結果から採用する（FREM の
 * OVFL は常に 0 強制）。通常経路の INEX2 は FMOD が Refer（p.4-63）、FREM が
 * クリア（p.4-87）。 */
#define FMOD_FORCE_CLEAR ((uint8_t)MC68K_FPU_EXC_OVFL)
#define FREM_FORCE_CLEAR ((uint8_t)(MC68K_FPU_EXC_OVFL | MC68K_FPU_EXC_INEX2))
#define FREM_PASS_FORCE  ((uint8_t)MC68K_FPU_EXC_OVFL)

static mc68k_fp80_t remainder_op(mc68k_fp80_t dst, mc68k_fp80_t src, mc68k_fpu_state_t *st,
                                 bool nearest) {
    mc68k_fp80_t r;
    int32_t ea, eb, d;
    uint64_t sa, sb;
    uint32_t q = 0;
    bool sign_d = fpu_sign(dst);
    bool qsign = sign_d != fpu_sign(src);
    bool flip = false;
    uint_fast8_t sf;

    /* 商 byte は命令の先頭でクリアする（暫定決定。U17）。 */
    set_quotient(st, false, 0);
    if (fpu_dyadic_nan(dst, src, st, &r))
        return r;
    if (fpu_is_inf(dst) || fpu_is_zero(src)) { /* dst=±Inf または src=±0 */
        r = fpu_default_nan();
        fpu_finish_result(st, r, (uint8_t)MC68K_FPU_EXC_OPERR, 0);
        return r;
    }
    if (fpu_is_zero(dst)) {
        set_quotient(st, qsign, 0);
        r = fpu_make(sign_d, 0, 0);
        fpu_finish_result(st, r, 0, 0);
        return r;
    }
    fpu_normalize(fpu_exp_field(dst), dst.signif, &ea, &sa);
    if (fpu_is_inf(src)) { /* dst は有限。dst の値を通常の終了処理で丸める */
        set_quotient(st, qsign, 0);
        r = fpu_round_pack128(sign_d, ea, sa, 0, st, &sf);
        fpu_finish_result(st, r, fpu_sf_flags_to_exc(sf), nearest ? FREM_PASS_FORCE : 0);
        return r;
    }
    fpu_normalize(fpu_exp_field(src), src.signif, &eb, &sb);
    d = ea - eb;
    if (d >= 0) {
        uint64_t rem = sa;

        for (int32_t i = 0; i <= d; i++) {
            /* rem < 2*sb <= 2^65。1 ビットの繰り上がりを carry で持つ */
            bool carry = false;

            if (i > 0) {
                carry = (rem >> 63) != 0;
                rem <<= 1;
            }
            q <<= 1;
            if (carry || rem >= sb) {
                rem -= sb;
                q |= 1;
            }
        }
        if (nearest) { /* 2*rem と sb の比較。半分ちょうどは商が偶数になる側 */
            bool carry = (rem >> 63) != 0;
            uint64_t twice = rem << 1;

            if (carry || twice > sb || (twice == sb && (q & 1u) != 0)) {
                rem = sb - rem;
                q++;
                flip = true;
            }
        }
        /* 剰余は sb と同じ重みの整数。0 なら符号付きゼロ */
        fpu_normalize(eb, rem, &ea, &sa);
    } else if (nearest && d == -1 &&
               sa > sb) {    /* dst > src/2。商 1 で src - dst（dst と同じ重み ea） */
        sa = (sb << 1) - sa; /* 2^64 を法とする演算で真値（< sb）が得られる */
        q = 1;
        flip = true;
        fpu_normalize(ea, sa, &ea, &sa);
    }
    set_quotient(st, qsign, q);
    if (sa == 0) {
        r = fpu_make(sign_d, 0, 0);
        fpu_finish_result(st, r, 0, 0);
        return r;
    }
    r = fpu_round_pack128(sign_d != flip, ea, sa, 0, st, &sf);
    /* 通常経路: OVFL は 0 に強制、UNFL は roundPack の結果、INEX2 は FMOD だけ roundPack の結果
     * （[881UM] p.4-63・p.4-87） */
    fpu_finish_result(st, r, fpu_sf_flags_to_exc(sf),
                      nearest ? FREM_FORCE_CLEAR : FMOD_FORCE_CLEAR);
    return r;
}

mc68k_fp80_t mc68k_fpu_mod(mc68k_fp80_t dst, mc68k_fp80_t src, mc68k_fpu_state_t *st) {
    return remainder_op(dst, src, st, false);
}

mc68k_fp80_t mc68k_fpu_rem(mc68k_fp80_t dst, mc68k_fp80_t src, mc68k_fpu_state_t *st) {
    return remainder_op(dst, src, st, true);
}
