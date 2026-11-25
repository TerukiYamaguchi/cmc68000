/*
 * fpu_internal.h — FPU共通コアの内部ヘルパー。公開しない。
 *
 * 値の分類・正規化・丸め精度の変換と、Berkeley SoftFloat のグローバル状態と
 * mc68k_fpu_state_t の同期。
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "mc68000/fpu.h"

#define FPU_SIGN_BIT 0x8000u
#define FPU_EXP_MASK 0x7FFFu /* 指数フィールド全ビット。無限大・NaNの指数 */
#define FPU_SIG_J    UINT64_C(0x8000000000000000) /* 整数部ビット */
#define FPU_SIG_QUIET \
    UINT64_C(0x4000000000000000) /* フラクション最上位ビット。1=非シグナリングNaN */
#define FPU_SIG_FRAC UINT64_C(0x7FFFFFFFFFFFFFFF) /* 整数部を除くフラクション */

/* SoftFloat の roundingPrecision（softfloat_roundPackToExtF80() の引数） */
#define FPU_ROUNDING_PRECISION_X    80
#define FPU_ROUNDING_PRECISION_S    32
#define FPU_ROUNDING_PRECISION_D    64
/* 仮数だけを24/53bitへ丸め、指数範囲は拡張精度のまま（設計書 1b §5.1-5）。
 * s_roundPackToExtF80.c の PREC_S_XR/PREC_D_XR と同じ値。 */
#define FPU_ROUNDING_PRECISION_S_XR 96
#define FPU_ROUNDING_PRECISION_D_XR 97

static inline bool fpu_sign(mc68k_fp80_t v) {
    return (v.sign_exp & FPU_SIGN_BIT) != 0;
}

static inline uint16_t fpu_exp_field(mc68k_fp80_t v) {
    return (uint16_t)(v.sign_exp & FPU_EXP_MASK);
}

/* 分類（設計書 §6.2）。ゼロ・無限大・NaN は互いに排他的。 */
static inline bool fpu_is_nan(mc68k_fp80_t v) {
    return fpu_exp_field(v) == FPU_EXP_MASK && (v.signif & FPU_SIG_FRAC) != 0;
}

static inline bool fpu_is_inf(mc68k_fp80_t v) {
    return fpu_exp_field(v) == FPU_EXP_MASK && (v.signif & FPU_SIG_FRAC) == 0;
}

/* 指数が0でないunnormalized zero（sig==0）もゼロ */
static inline bool fpu_is_zero(mc68k_fp80_t v) {
    return fpu_exp_field(v) != FPU_EXP_MASK && v.signif == 0;
}

/* フラクション最上位ビットが0のNaN */
static inline bool fpu_is_snan(mc68k_fp80_t v) {
    return fpu_is_nan(v) && (v.signif & FPU_SIG_QUIET) == 0;
}

static inline mc68k_fp80_t fpu_make(bool sign, uint16_t exp, uint64_t signif) {
    mc68k_fp80_t v;
    v.sign_exp = (uint16_t)((sign ? FPU_SIGN_BIT : 0u) | exp);
    v.signif = signif;
    return v;
}

/* 先頭ゼロ個数（sig==0 は64） */
int fpu_clz64(uint64_t sig);

/* 有限値の正規化。value = sig * 2^(exp - 16383 - 63) を保ったまま sig のbit63を1にする。
 * baselineは指数フィールドそのまま（e=0を1に読み替えない。[881UM Table 3-3]）。
 * 非正規化数は exp<0 になる。sig==0 は (0,0)（expの値によらずゼロ）。 */
void fpu_normalize(int32_t exp, uint64_t sig, int32_t *out_exp, uint64_t *out_sig);

/* mc68k_fpu_round_prec_t → SoftFloat の roundingPrecision（80/32/64） */
uint_fast8_t fpu_rounding_precision(uint8_t prec);

/* 非ゼロの有限値を rounding_precision（80/32/64）で丸める。st の fpcr_round_prec は
 * 使わない（呼び出し側が明示する）。§3.2.2 のゼロ+非ゼロ早期リターン・abs/neg・to_x/to_s/to_d
 * で共用（design §4.1, §3.2.2, §5.1）。*out_sf_flags に fpu_sf_end() が返す生の
 * SoftFloatフラグ（inexact/underflow/overflow）を渡す。呼び出し側は命令ごとに異なる
 * マスク規則（design §6.2, §5.4）でEXC byteへ反映するため、ここでは反映しない。 */
mc68k_fp80_t fpu_round_finite(mc68k_fp80_t v, const mc68k_fpu_state_t *st,
                              uint_fast8_t rounding_precision, uint_fast8_t *out_sf_flags);

/* SNaNビット（フラクション最上位ビット）を立てる。[881UM §6.1.2] */
static inline uint64_t fpu_quiet_signif(uint64_t signif) {
    return signif | FPU_SIG_QUIET;
}

/* [881UM Table 2-1] 通常算術・abs/negの結果から fpsr_cc を設定する（design §5.5）。
 * FCMP/FTSTは別規則（§5.4）でst->fpsr_ccを直接設定するため使わない。 */
void fpu_set_cc(mc68k_fpu_state_t *st, mc68k_fp80_t result);

/* §3.4 手順1: st の丸めモードと tininess 検出方式（丸め前）を SoftFloat に反映し、
 * 例外フラグを0にする。 */
void fpu_sf_begin(const mc68k_fpu_state_t *st);

/* §3.4 手順3: SoftFloat の例外フラグ（softfloat_flag_*）を読み取って0に戻し、返す。
 * 呼び出し側（各公開関数）が命令ごとのマスク規則（design §6.2, §5.4）で fpsr_exc へ
 * 反映する（Phase C, §6.1）。 */
uint_fast8_t fpu_sf_end(void);

/* fpu_sf_end() が返す生の SoftFloat フラグのうち inexact/underflow/overflow を
 * EXC.INEX2/UNFL/OVFL へ写像する（design §6.2）。invalid/infinite は OPERR/DZ の
 * 判定基準がSoftFloatのフラグと異なる（§6.2、mul/divの0×∞・0/0・∞/∞は例外）ため、
 * 呼び出し側が個別に判定する。 */
uint8_t fpu_sf_flags_to_exc(uint_fast8_t sf_flags);

/* 非ゼロ・非NaN・非Infの値vが、拡張精度として非正規化数か（アイデアライズドexp<0、
 * design §3.3.1）。mc68k_fpu_move()・abs/negのUNFL判定（destinationのPRECに依らず
 * source自体で決まる規則。design §4.1・§3.3.1・§5.4）に使う。ストア（to_x/to_s/to_d）の
 * UNFL判定（destinationのPREC依存のtiny判定）とは別。 */
bool fpu_src_is_x_denormal(mc68k_fp80_t v);

/* EXC byteへ検出したビットをORで積み、AEXC byteへ§6.5の等式を適用する。EXC byteの
 * クリアはバックエンド（呼び出し列全体の先頭）の責務のため、この関数はクリアしない
 * （design §6.1・§6.5）。 */
void fpu_commit_exc(mc68k_fpu_state_t *st, uint8_t bits);

/* FMOVECR の定数ROM（fpu_rom_table.c。tests/fpu/gen_fpu_1b_vectors.py が生成）。
 * 値は (hi:lo) の128bit仮数（bit127=1）と拡張精度の指数フィールド exp。128bitに
 * 収まらない端数は lo の最下位ビットへ jam 済み（設計書 1b §4.1-2）。 */
struct fpu_rom_entry {
    uint8_t offset;
    uint8_t nonzero; /* 0 のとき +0.0（$0F） */
    uint16_t exp;
    uint64_t hi;
    uint64_t lo;
};
extern const struct fpu_rom_entry fpu_rom_table[];
extern const int fpu_rom_table_count;

/* 68881 の既定NaN（OPERR の結果）。仮数全ビット1（[881UM §3.2.5]、1a design §5.6・§9）。 */
static inline mc68k_fp80_t fpu_default_nan(void) {
    return fpu_make(false, FPU_EXP_MASK, UINT64_MAX);
}

/* 単項演算の NaN 入力の共通処理（1a design §5.2）。src が NaN なら SNAN を検出し、quiet 化した
 * NaN を *out に入れて条件コードを設定し true を返す（EXC/AEXC は commit 済み）。NaN
 * でなければ何もせず false。 */
bool fpu_monadic_nan(mc68k_fp80_t src, mc68k_fpu_state_t *st, mc68k_fp80_t *out);

/* 二項演算の NaN 入力の共通処理。どちらかが NaN なら dst 優先（dst が NaN なら
 * dst、そうでなければ src）で quiet 化した NaN を返す（[881UM §4.5.4.1]）。SNaN があれば SNAN
 * を検出する。 */
bool fpu_dyadic_nan(mc68k_fp80_t dst, mc68k_fp80_t src, mc68k_fpu_state_t *st,
                    mc68k_fp80_t *out);

/* 正規化済みの 128bit 仮数 (hi:lo)（hi のbit63=1）と idealized 指数 exp（value = sig *
 * 2^(exp-16383-127)）を st の PREC・RM で 1 回だけ丸めて拡張精度値にする。lo の下位に sticky
 * を含めてよい。*out_sf_flags に fpu_sf_end() の生フラグを返す。 */
mc68k_fp80_t fpu_round_pack128(bool sign, int32_t exp, uint64_t hi, uint64_t lo,
                               const mc68k_fpu_state_t *st, uint_fast8_t *out_sf_flags);

/* fpu_round_pack128() の丸め書式を呼び出し側が指定する版（FSGLMUL/FSGLDIV の S_XR。設計書 1b
 * §5.3）。 rounding_precision は FPU_ROUNDING_PRECISION_*。丸めモードは st の fpcr_round_mode。
 */
mc68k_fp80_t fpu_round_pack128_prec(bool sign, int32_t exp, uint64_t hi, uint64_t lo,
                                    const mc68k_fpu_state_t *st,
                                    uint_fast8_t rounding_precision,
                                    uint_fast8_t *out_sf_flags);

/* 命令ごとのマスク規則（1b design §5.2 の 2）を適用して EXC へ積み、条件コードを設定する。
 * force_clear は 0 に強制する EXC ビット。 */
void fpu_finish_result(mc68k_fpu_state_t *st, mc68k_fp80_t result, uint8_t exc_bits,
                       uint8_t force_clear);
