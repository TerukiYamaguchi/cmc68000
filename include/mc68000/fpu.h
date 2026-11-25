/*
 * fpu.h — MC68881/MC68882 共通 FPU コアの公開 API。
 *
 * 接続形態（CIR / FPCP / 68040内蔵）に依存しない状態表現とデータ型変換。
 * 共通コアはホストから直接生成・操作できる独立した値として扱える（CPU には mc68k_get_fpu() の
 * コプロセッサモデルが所有する形で組み込まれる。下の「コプロセッサモデル」の節）。
 * 一次資料は MC68881/MC68882 User's Manual
 * （以下 [881UM]）。
 *
 * 例外ビットの扱い: 各関数は st->fpsr_exc / fpsr_aexc をクリアせず、検出したビットを
 * OR のみで立てる（design §6.1）。1個の68881命令に対応する共通コア呼び出し列の先頭で
 * EXC byte をクリアするのは呼び出し側（バックエンド）の責務。AEXC byte は誰もクリア
 * しない（reset を除く）。BSUN の設定は条件命令を実装するバックエンド層の責務で、
 * 1a のどの関数も設定しない。
 *
 * スレッド安全性: 内部で Berkeley SoftFloat のグローバル状態を使うため、同一スレッド内
 * では複数の mc68k_fpu_state_t を交互に使ってよいが、関数呼び出しの同時実行はしない。
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cpu_types.h"

/* 80-bit拡張精度実数の内部値表現。FP0-FP7の格納形式（[881UM §2.1]）。
 * sign_exp: bit15=符号、bit14-0=指数。signif: 整数部ビット(bit63)を含む64bit仮数。
 * X68000バス上のメモリ表現（96-bit、ビッグエンディアン、未使用16bit含む。
 * [881UM Figure 3-2]）とはバイトレイアウトが異なる。バスとこの型の変換は
 * 本APIの範囲外（バックエンド層の責務）。 */
typedef struct {
    uint16_t sign_exp;
    uint64_t signif;
} mc68k_fp80_t;

/* FPCR MODEバイトの丸めモード（[881UM Figure 2-3]） */
typedef enum {
    MC68K_FPU_ROUND_NEAREST = 0,
    MC68K_FPU_ROUND_ZERO = 1,
    MC68K_FPU_ROUND_MINUS_INF = 2,
    MC68K_FPU_ROUND_PLUS_INF = 3,
} mc68k_fpu_round_mode_t;

/* FPCR MODEバイトの丸め精度（[881UM Figure 2-3]）。
 * destinationがFPnの演算（mc68k_fpu_move() を含む）にだけ影響し、S/D外部形式への
 * 変換には影響しない（[881UM §2.2.2]「destinationがメモリの場合、PRECは無視される」）。 */
typedef enum {
    MC68K_FPU_PREC_EXTENDED = 0,
    MC68K_FPU_PREC_SINGLE = 1,
    MC68K_FPU_PREC_DOUBLE = 2,
} mc68k_fpu_round_prec_t;

/* FPSR EXC byte / FPCR ENABLE byte の例外クラス（[881UM Figure 2-2, 2-6]）。
 * ビット位置はハードウェアのbit15-8をbit7-0に詰め替えた内部表現。 */
typedef enum {
    MC68K_FPU_EXC_INEX1 = 1u << 0,
    MC68K_FPU_EXC_INEX2 = 1u << 1,
    MC68K_FPU_EXC_DZ = 1u << 2,
    MC68K_FPU_EXC_UNFL = 1u << 3,
    MC68K_FPU_EXC_OVFL = 1u << 4,
    MC68K_FPU_EXC_OPERR = 1u << 5,
    MC68K_FPU_EXC_SNAN = 1u << 6,
    MC68K_FPU_EXC_BSUN = 1u << 7,
} mc68k_fpu_exc_bit_t;

/* FPSR AEXC byte（[881UM Figure 2-7]） */
typedef enum {
    MC68K_FPU_AEXC_INEX = 1u << 0,
    MC68K_FPU_AEXC_DZ = 1u << 1,
    MC68K_FPU_AEXC_UNFL = 1u << 2,
    MC68K_FPU_AEXC_OVFL = 1u << 3,
    MC68K_FPU_AEXC_IOP = 1u << 4,
} mc68k_fpu_aexc_bit_t;

/* FPSR FPCC byte（[881UM Figure 2-4]） */
typedef enum {
    MC68K_FPU_CC_N = 1u << 0,
    MC68K_FPU_CC_Z = 1u << 1,
    MC68K_FPU_CC_I = 1u << 2,
    MC68K_FPU_CC_NAN = 1u << 3,
} mc68k_fpu_cc_bit_t;

/* FPU共通コアの状態。 */
typedef struct {
    mc68k_fp80_t fp[8];         /* FP0-FP7（[881UM §2.1]） */
    uint8_t fpcr_enable;        /* ENABLE byte。mc68k_fpu_exc_bit_t のOR（[881UM §2.2.1]） */
    uint8_t fpcr_round_mode;    /* mc68k_fpu_round_mode_t（[881UM §2.2.2]） */
    uint8_t fpcr_round_prec;    /* mc68k_fpu_round_prec_t */
    uint8_t fpsr_cc;            /* mc68k_fpu_cc_bit_t のOR（[881UM §2.3.1]） */
    uint8_t fpsr_quotient_sign; /* FMOD/FREMの商の符号。1aでは常に0 */
    uint8_t fpsr_quotient;      /* 商の下位7bit。1aでは常に0 */
    uint8_t fpsr_exc;           /* EXC byte。mc68k_fpu_exc_bit_t のOR（[881UM §2.3.3]） */
    uint8_t fpsr_aexc;          /* AEXC byte。mc68k_fpu_aexc_bit_t のOR（[881UM §2.3.4]） */
    uint32_t fpiar;             /* 格納のみ。更新規則はバックエンド側の責務 */
} mc68k_fpu_state_t;

/* リセット / null state restore（[881UM §2.1-§2.4]）:
 * FP0-7 は非シグナリングNaN（指数7FFF・仮数全ビット1）、FPCR/FPSR/FPIAR は0。 */
void mc68k_fpu_reset(mc68k_fpu_state_t *st);

/* 整数 → 内部拡張精度（[881UM §3.5.1]）。常に厳密（丸め・例外なし）。 */
mc68k_fp80_t mc68k_fpu_from_b(int8_t v);
mc68k_fp80_t mc68k_fpu_from_w(int16_t v);
mc68k_fp80_t mc68k_fpu_from_l(int32_t v);

/* 外部実数形式 → 内部拡張精度（[881UM §3.5.1]）。常に厳密で、PRECによる丸めはしない
 * （算術命令のオペランド変換にも共用するため。二重丸めを避ける）。S/D の非正規化数、
 * X の unnormalized数・unnormalized zero は正規化する（X の非正規化数は e=0 のまま保つ）。NaN
 * は値のまま変換し、 SNaN を quiet 化しない。 */
mc68k_fp80_t mc68k_fpu_from_s(uint32_t bits, mc68k_fpu_state_t *st);
mc68k_fp80_t mc68k_fpu_from_d(uint64_t bits, mc68k_fpu_state_t *st);
mc68k_fp80_t mc68k_fpu_from_x(mc68k_fp80_t v, mc68k_fpu_state_t *st);

/* FMOVEロード（<ea>,FPn / FPm,FPn）のPREC依存の丸め。from_* が作った厳密な拡張精度値を
 * st->fpcr_round_prec / st->fpcr_round_mode で丸めて返す（[881UM §2.2.2, p.4-64]）。
 * 算術命令のオペランド変換には使わない。
 * NaNはSNaNをquiet化してから返す（[881UM §4.5.4.2, §6.1.2]。FMOVEロードは
 * "only an FMOVEM instruction can write a SNAN" の対象外）。±Infは値のまま返す。
 * ゼロ（unnormalized zero を含む）は符号付きの正規のゼロで返す。
 * 有限の非ゼロ値は算術演算と同じ丸め（PRECの指数範囲・オーバーフロー結果・非正規化域への
 * 丸め）を通る。
 *
 * EXC byte（[881UM] p.4-65、実測済み）: OVFLは常にクリア。UNFLはPRECに関わらず
 * source自体が拡張精度の非正規化数のときだけ立つ（ストアのUNFLとは別規則。design §4.1）。
 * INEX2はroundPackのinexactをそのまま採用する。BSUN/OPERR/DZは常にクリア、SNANは検出する。 */
mc68k_fp80_t mc68k_fpu_move(mc68k_fp80_t src, mc68k_fpu_state_t *st);

/* 内部拡張精度 → 外部形式（[881UM §3.5.2]）。st->fpcr_round_mode に従って丸める
 * （destinationはメモリなので fpcr_round_prec は無視する）。
 *
 * B/W/L: 非シグナリングNaN・SNaNは仮数の上位8/16/32bit（SNaNビットを立てたもの）、
 * ±無限大と整数レンジ外は符号に応じた最大/最小整数（[881UM Table 6-2, §6.1.3]）。
 * OPERRは非シグナリングNaN・±Inf・整数レンジ超過で立つ（[881UM Table 6-2]の条件は
 * 文字どおり"Non-Signaling NAN"）。SNaN入力はSNANビットのみでOPERRは立たない
 * （[881UM §6.1.2]はSNaN→B/W/Lを自己完結した経路として記述し、OPERRに触れない）。
 * INEX2は整数への丸めで端数が切り捨てられた場合に立つ（[881UM §6.1.7]「選択した
 * 丸め精度または destination data format で正確に表現できない」。オーバーフロー
 * [OPERR]のときは立てない）。
 * S/D/X: NaNはSNaNビット（仮数のbit62）を立てて出力する（[881UM §6.1.2]）。OPERRは
 * 常にクリア（[881UM §6.1.3]「destinationがS/D/Xならoperand errorは発生しない」）。
 * UNFL/OVFL/INEX2は算術演算と同じtininess判定（roundPackの検出結果をそのまま採用）。 */
int8_t mc68k_fpu_to_b(mc68k_fp80_t v, mc68k_fpu_state_t *st);
int16_t mc68k_fpu_to_w(mc68k_fp80_t v, mc68k_fpu_state_t *st);
int32_t mc68k_fpu_to_l(mc68k_fp80_t v, mc68k_fpu_state_t *st);
uint32_t mc68k_fpu_to_s(mc68k_fp80_t v, mc68k_fpu_state_t *st);
uint64_t mc68k_fpu_to_d(mc68k_fp80_t v, mc68k_fpu_state_t *st);
mc68k_fp80_t mc68k_fpu_to_x(mc68k_fp80_t v, mc68k_fpu_state_t *st);

/*
 * 基本算術演算と条件コード。
 *
 * dst = 命令実行前からFPnに入っている値、src = <ea>/FPmから供給される値
 * （[881UM]の"Operation:"行に対応。FADD: "Source + FPn"、FSUB/FCMP: "FPn - Source"、
 * FMUL: "Source × FPn"、FDIV: "FPn ÷ Source"。§5.6）。
 *
 * NaN伝播（§5.2）: 片方だけがNaNならそのNaNを、両方がNaNならdst側を返す（SNaNは
 * フラクション最上位ビットのみ立てて非シグナリング化してから同じ規則を適用）。
 * 加減乗除・abs/negは演算後に st->fpsr_cc を結果のデータ型から設定する（§5.5）。
 *
 * EXC byte（design §6.2）: SoftFloatのinexact/underflow/overflowはそのままINEX2/UNFL/OVFLへ
 * 写像する。OPERRは[881UM Table 6-2]の個別条件（FADD/FSUBは異符号±∞同士の加算・
 * 同符号±∞同士の減算、FMULは片方0・他方±∞、FDIVは0/0・∞/∞）で判定し、SoftFloatの
 * invalidフラグを直接は使わない（SNaN入力単体ではOPERRを立てない）。FDIVのDZは
 * 非零÷0。SNANはSNaN入力を検出して立てる。BSUNは1aのスコープ外（常にクリア）。
 */
mc68k_fp80_t mc68k_fpu_add(mc68k_fp80_t dst, mc68k_fp80_t src, mc68k_fpu_state_t *st);
mc68k_fp80_t mc68k_fpu_sub(mc68k_fp80_t dst, mc68k_fp80_t src,
                           mc68k_fpu_state_t *st); /* dst - src */
mc68k_fp80_t mc68k_fpu_mul(mc68k_fp80_t dst, mc68k_fp80_t src, mc68k_fpu_state_t *st);
mc68k_fp80_t mc68k_fpu_div(mc68k_fp80_t dst, mc68k_fp80_t src,
                           mc68k_fpu_state_t *st); /* dst ÷ src */

/* abs/negは符号操作した値を mc68k_fpu_move() と同じ経路（normalize→roundPack、
 * roundingPrecisionはfpcr_round_prec）で丸める（[881UM §2.2.2]の"any operation"）。
 * NaN・±Infは符号操作のみ（NaNのquiet化は§5.2と同じ）。±0は符号操作した正規のゼロ。
 * EXC byte（design §5.4）: OVFL/INEX2は常にクリア（roundPackの検出結果は使わない）。
 * UNFLはmc68k_fpu_move()と同じ規則（destinationのPRECに依らずsource自体が拡張精度の
 * 非正規化数か）。BSUN/OPERR/DZは常にクリア、SNANは検出する。 */
mc68k_fp80_t mc68k_fpu_abs(mc68k_fp80_t src, mc68k_fpu_state_t *st);
mc68k_fp80_t mc68k_fpu_neg(mc68k_fp80_t src, mc68k_fpu_state_t *st);

/* FCMP: dst・srcのデータ型の組み合わせから§5.4の確定表を直接引いて条件コードを設定する
 * （dst - src を実際に計算して分類してはならない。表と実減算の結果が食い違う反例がある）。
 * dst/src自体は変更されない。EXC byte（design §5.4）: BSUN/OPERR/OVFL/UNFL/DZ/INEX2は
 * 常にクリア、SNANはSNaN入力を検出して立てる。 */
void mc68k_fpu_cmp(mc68k_fp80_t dst, mc68k_fp80_t src, mc68k_fpu_state_t *st);

/* FTST: srcのデータ型を検査し条件コードのみ設定する（演算は行わない）。EXC byteは
 * mc68k_fpu_cmp()と同じ規則（SNANのみ検出、他は常にクリア）。 */
void mc68k_fpu_tst(mc68k_fp80_t src, mc68k_fpu_state_t *st);

/*
 * FMOVECR。offset は ROM オフセット
 * （$00=π, $0B=log10(2), $0C=e, $0D=log2(e), $0E=log10(e), $0F=0.0, $30=ln2, $31=ln10,
 * $32〜$3F=10^0,10^1,10^2,10^4,…,10^4096）。定義済み以外は +0.0。
 * 値は数学的な真値を FPCR の丸めモード・丸め精度で丸める（[881UM] p.4-72、値の出典は
 * 設計書 §4.1）。PREC=S/D は仮数だけを丸め、指数範囲は拡張精度のまま。
 * EXC byte（[881UM] p.4-72）: INEX2 は丸めで端数が出たとき。他は常にクリア。
 * 条件コードは通常算術と同じ規則で設定する。
 */
mc68k_fp80_t mc68k_fpu_movecr(uint8_t offset, mc68k_fpu_state_t *st);

/*
 * FSQRT・FINT・FINTRZ・FMOD。
 *
 * mc68k_fpu_sqrt: 正しく丸めた結果（PREC・RM に従う。ビット一致）。OVFL・UNFL
 * は常にクリア、INEX2 は 丸めで端数が出たとき。負値（-Inf 含む）は OPERR で既定NaN、±0 は ±0。
 * mc68k_fpu_int / mc68k_fpu_intrz: 整数へ丸める（int は FPCR の丸めモード、intrz は 0
 * 方向）。結果に PREC の 丸めはかけない。OPERR・OVFL・UNFL は常にクリア、INEX2
 * は値が変わったとき。 mc68k_fpu_mod: dst - src*N（N=INT(dst/src)、0 方向）。剰余の符号は dst
 * と同じ。商 byte（fpsr_quotient・ fpsr_quotient_sign）を設定する（命令の先頭で 0
 * にする）。src=±0・dst=±Inf は OPERR。
 */
mc68k_fp80_t mc68k_fpu_sqrt(mc68k_fp80_t src, mc68k_fpu_state_t *st);
mc68k_fp80_t mc68k_fpu_int(mc68k_fp80_t src, mc68k_fpu_state_t *st);
mc68k_fp80_t mc68k_fpu_intrz(mc68k_fp80_t src, mc68k_fpu_state_t *st);
mc68k_fp80_t mc68k_fpu_mod(mc68k_fp80_t dst, mc68k_fp80_t src, mc68k_fpu_state_t *st);

/*
 * 超越関数。mc68k_fpu_sin/cos/tan/atan/asin/acos/etox/logn/log10/log2/sinh/cosh/
 * tanh/atanh(src, st)。float128 の中間精度で計算し、拡張精度または S/D への丸めは 1
 * 回だけ行う（PREC・RM に従う）。 精度基準は忠実丸め（設計書
 * §3.2）。実機とのビット一致はしない。特殊値・定義域外・EXC byte は [881UM] の各命令
 * ページの表に従い（設計書 §5.2）、有限値の結果の INEX2 は厳密な結果（FETOX(0)=1
 * など）を除き常に立てる。 極小引数は 1 ulp128 だけ真値の側へずらした値を丸めるので、RZ/RM/RP
 * でも方向が正しい。
 */
mc68k_fp80_t mc68k_fpu_sin(mc68k_fp80_t src, mc68k_fpu_state_t *st);
mc68k_fp80_t mc68k_fpu_cos(mc68k_fp80_t src, mc68k_fpu_state_t *st);
mc68k_fp80_t mc68k_fpu_tan(mc68k_fp80_t src, mc68k_fpu_state_t *st);
mc68k_fp80_t mc68k_fpu_atan(mc68k_fp80_t src, mc68k_fpu_state_t *st);
mc68k_fp80_t mc68k_fpu_asin(mc68k_fp80_t src, mc68k_fpu_state_t *st);
mc68k_fp80_t mc68k_fpu_acos(mc68k_fp80_t src, mc68k_fpu_state_t *st);
mc68k_fp80_t mc68k_fpu_etox(mc68k_fp80_t src, mc68k_fpu_state_t *st);
mc68k_fp80_t mc68k_fpu_logn(mc68k_fp80_t src, mc68k_fpu_state_t *st);
mc68k_fp80_t mc68k_fpu_log10(mc68k_fp80_t src, mc68k_fpu_state_t *st);
mc68k_fp80_t mc68k_fpu_log2(mc68k_fp80_t src, mc68k_fpu_state_t *st);
mc68k_fp80_t mc68k_fpu_sinh(mc68k_fp80_t src, mc68k_fpu_state_t *st);
mc68k_fp80_t mc68k_fpu_cosh(mc68k_fp80_t src, mc68k_fpu_state_t *st);
mc68k_fp80_t mc68k_fpu_tanh(mc68k_fp80_t src, mc68k_fpu_state_t *st);
mc68k_fp80_t mc68k_fpu_atanh(mc68k_fp80_t src, mc68k_fpu_state_t *st);

/*
 * 剰余・単精度演算・指数／仮数操作。
 *
 * mc68k_fpu_rem: IEEE 剰余 dst - src*N（N=INT(dst/src) を最近接偶数で丸めたもの。剰余の符号は
 * dst と同じとは 限らない）。商 byte・OPERR は mc68k_fpu_mod と同じ規則。「dst
 * の値を返す」経路（src=±Inf）は FMOD が OVFL・INEX2、FREM が INEX2
 * だけを丸めの結果から採用する（FREM の OVFL は 0 強制）。通常経路の INEX2 は FREM
 * だけ常にクリア。 mc68k_fpu_sglmul / mc68k_fpu_sgldiv: FPCR
 * の丸め精度を無視し（丸めモードは使う）、入力の仮数を有効数字 24bit
 * へ切り詰めて演算し、仮数だけ 24bit・指数範囲は拡張精度で丸める。OVFL・UNFL・INEX2
 * は丸めの結果どおり。 mc68k_fpu_getexp / mc68k_fpu_getman: 指数（バイアス除去）/
 * 仮数（[1,2)）を厳密に返す。UNFL・INEX2 は常にクリア。 mc68k_fpu_scale: dst × 2^INT(src)（0
 * 方向に整数化）。|INT(src)| >= 2^14 は dst によらず OVFL/UNFL。INEX2 は「src=±0 で dst
 * の値を返す」経路以外でクリア。 mc68k_fpu_etoxm1 / lognp1 / twotox / tentox: 超越関数（設計書
 * §5.2。方式・精度基準は B1 と同じ）。 mc68k_fpu_sincos: 戻り値が sin（FPs）、*out_cos が
 * cos（FPc）。条件コードは sin の結果、EXC は両者の OR。
 */
mc68k_fp80_t mc68k_fpu_rem(mc68k_fp80_t dst, mc68k_fp80_t src, mc68k_fpu_state_t *st);
mc68k_fp80_t mc68k_fpu_sglmul(mc68k_fp80_t dst, mc68k_fp80_t src, mc68k_fpu_state_t *st);
mc68k_fp80_t mc68k_fpu_sgldiv(mc68k_fp80_t dst, mc68k_fp80_t src, mc68k_fpu_state_t *st);
mc68k_fp80_t mc68k_fpu_getexp(mc68k_fp80_t src, mc68k_fpu_state_t *st);
mc68k_fp80_t mc68k_fpu_getman(mc68k_fp80_t src, mc68k_fpu_state_t *st);
mc68k_fp80_t mc68k_fpu_scale(mc68k_fp80_t dst, mc68k_fp80_t src, mc68k_fpu_state_t *st);
mc68k_fp80_t mc68k_fpu_etoxm1(mc68k_fp80_t src, mc68k_fpu_state_t *st);
mc68k_fp80_t mc68k_fpu_lognp1(mc68k_fp80_t src, mc68k_fpu_state_t *st);
mc68k_fp80_t mc68k_fpu_twotox(mc68k_fp80_t src, mc68k_fpu_state_t *st);
mc68k_fp80_t mc68k_fpu_tentox(mc68k_fp80_t src, mc68k_fpu_state_t *st);
mc68k_fp80_t mc68k_fpu_sincos(mc68k_fp80_t src, mc68k_fp80_t *out_cos, mc68k_fpu_state_t *st);

/*
 * Packed decimal。words は 12 バイトのビッグエンディアン並びを
 * 32bit ×3 に組んだもの（バイト列との変換はバックエンド層の責務）。words[0] = SM・SE・yy・指数
 * 3 桁（BCD）・EXP3・ 整数 1 桁、words[1] = 小数の上位 8 桁、words[2] = 下位 8 桁。
 *
 * mc68k_fpu_from_p: 値 = ±(整数 1 桁 + 小数 16 桁) ×
 * 10^(±指数)。丸めモードで拡張精度へ正しく丸める（PREC は無視）。 端数が出たとき INEX1。指数
 * $FFF は Inf（仮数 0）・NaN（仮数を bit 単位で写す）。仮数が 0 なら指数の桁によらず ±0。 非 10
 * 進の桁は検出せず 10 進と同じ式で変換する（[881UM] Table 3-4 注）。NaN は値のまま返す（from_x
 * と同じ）。 mc68k_fpu_to_p: k は 7bit 符号付き（下位 7bit を符号拡張して使う）。k<=0
 * は小数点以下 -k 桁（有効桁数は 1〜17 に 切り詰める）、k=+1〜+17 は有効桁数、k>+17
 * は値によらず（Inf・NaN・ゼロでも）OPERR で k=+17。丸めモードに従って 10
 * 進で丸め、端数があれば INEX2。 指数が 999 を超えるときは 4 桁目を EXP3 に書き OPERR。Inf
 * は指数 $FFF・仮数 0、NaN は仮数を bit 単位で書く （SNaN は quiet 化して
 * SNAN）。ゼロは符号つきのゼロ。条件コードは変更しない。
 */
mc68k_fp80_t mc68k_fpu_from_p(const uint32_t words[3], mc68k_fpu_state_t *st);
void mc68k_fpu_to_p(mc68k_fp80_t v, int8_t k, uint32_t words[3], mc68k_fpu_state_t *st);

/*
 * ============================================================================
 * コプロセッサモデル。
 *
 * 接続形態に依存しない1個の MC68881/MC68882 の状態機械。X68000 の CIR と
 * 68020/68030 の FPCP 命令経路は、この API を駆動する別の入口である。入口は応答 CIR の
 * 生値を保存せず、型付きの応答プリミティブだけを使う。
 * ============================================================================
 */

/* 不透明型。CPU が所有するもの（mc68k_get_fpu()）は destroy してはならない */
typedef struct mc68k_fpu mc68k_fpu_t;
typedef struct mc68k_fpu_snapshot mc68k_fpu_snapshot_t;

/* 独立モデルの生成（CIR 用）。MC68K_FPU_NONE と未定義値は NULL を返す */
mc68k_fpu_t *mc68k_fpu_create(mc68k_fpu_config_t config);
void mc68k_fpu_destroy(mc68k_fpu_t *fpu);

/* 外付け FPU の状態を保存・復元する。restore は確保を行わない。 */
mc68k_fpu_snapshot_t *mc68k_fpu_snapshot_create(const mc68k_fpu_t *fpu);
bool mc68k_fpu_snapshot_check(const mc68k_fpu_t *fpu, const mc68k_fpu_snapshot_t *snap);
void mc68k_fpu_snapshot_restore(mc68k_fpu_t *fpu, const mc68k_fpu_snapshot_t *snap);
void mc68k_fpu_snapshot_destroy(mc68k_fpu_snapshot_t *snap);
/* FPU が無い CPU は NULL */
mc68k_fpu_t *mc68k_get_fpu(mc68k_state_t *cpu);
const mc68k_fpu_t *mc68k_get_fpu_const(const mc68k_state_t *cpu);
mc68k_fpu_config_t mc68k_fpu_get_config(const mc68k_fpu_t *fpu);

typedef enum {
    MC68K_FPU_RESPONSE_NULL,
    MC68K_FPU_RESPONSE_EA_TRANSFER,
    MC68K_FPU_RESPONSE_MP_REGISTER,
    MC68K_FPU_RESPONSE_FP_REGISTERS,
    MC68K_FPU_RESPONSE_TAKE_PRE_EXCEPTION,
    MC68K_FPU_RESPONSE_TAKE_MID_EXCEPTION,
} mc68k_fpu_response_kind_t;

typedef enum {
    MC68K_FPU_COMMAND_ARITHMETIC,
    MC68K_FPU_COMMAND_MOVE, /* FMOVE（<ea>/FPm,FPn、FPn,<ea>）と FMOVECR */
    MC68K_FPU_COMMAND_MOVEM,
    MC68K_FPU_COMMAND_MOVE_CONTROL,
} mc68k_fpu_command_class_t;

typedef enum {
    MC68K_FPU_SOURCE_FP_REGISTER, /* FPm。FPU 内部の値（FPn,<ea> などの出力側を含む） */
    MC68K_FPU_SOURCE_EXTERNAL,    /* <ea> から operand CIR 経由 */
    MC68K_FPU_SOURCE_IMMEDIATE,   /* FMOVECR の ROM 定数 */
} mc68k_fpu_source_kind_t;

typedef enum {
    MC68K_FPU_FORMAT_B,
    MC68K_FPU_FORMAT_W,
    MC68K_FPU_FORMAT_L,
    MC68K_FPU_FORMAT_S,
    MC68K_FPU_FORMAT_D,
    MC68K_FPU_FORMAT_X,
    MC68K_FPU_FORMAT_P,
} mc68k_fpu_format_t;

/* control register select（mc68k_fpu_command_t.control_mask のビット） */
#define MC68K_FPU_CONTROL_FPCR  4u
#define MC68K_FPU_CONTROL_FPSR  2u
#define MC68K_FPU_CONTROL_FPIAR 1u

typedef struct {
    bool valid;            /* command word が有効なら true */
    uint16_t command_word; /* 元の16-bit command word（idle frame の command image 用） */
    mc68k_fpu_command_class_t command_class;
    uint8_t opmode; /* arithmetic の [881UM] opmode */
    uint8_t dst_fp; /* FPn、該当しない class では 0 */
    mc68k_fpu_source_kind_t source_kind;
    uint8_t src_fp; /* source_kind==FP_REGISTER のときだけ有効 */
    mc68k_fpu_format_t format;
    uint16_t register_mask; /* FMOVEM の静的 mask（command word の下位 8bit。上位 byte は 0） */
    bool dynamic_register_mask;
    uint8_t register_mask_data_register; /* dynamic_register_mask 時の Dn (0..7) */
    bool transfer_to_external;           /* FMOVE/MOVEM/control: FPU→外部なら true */
    uint8_t secondary_dst_fp;            /* FSINCOS の FPc (0..7)、それ以外は 0 */
    uint8_t movecr_offset;               /* FMOVECR の ROM offset (0..127) */
    int8_t packed_k_factor;              /* FMOVE.P 静的 k-factor */
    bool dynamic_packed_k_factor;
    uint8_t packed_k_factor_data_register; /* dynamic 時の Dn (0..7) */
    uint8_t control_mask; /* MOVE_CONTROL の対象（MC68K_FPU_CONTROL_* の OR） */
    bool predecrement;    /* FMOVEM 出力の predecrement mode（mask の MSB=FP7） */
} mc68k_fpu_command_t;

typedef enum {
    MC68K_FPU_CONTROL_ABORT,
    MC68K_FPU_CONTROL_EXCEPTION_ACK,
    MC68K_FPU_CONTROL_RESET,
} mc68k_fpu_control_t;

typedef struct {
    mc68k_fpu_response_kind_t kind;
    bool coprocessor_active; /* CA */
    bool request_pc;         /* PC */
    bool interruptible;      /* IA */
    bool processing;         /* !PF */
    bool transfer_to_host;   /* DR */
    uint8_t length;          /* 1/2/4/8/12 */
    uint8_t valid_ea;        /* [881UM Table 7-4] の3-bit分類 */
    uint8_t data_register;   /* MP_REGISTER の Dn */
    uint8_t condition_true;  /* condition CIR の TF */
    uint8_t vector;          /* TAKE_* の例外ベクタ */
} mc68k_fpu_response_t;

/* command word（cpGEN の拡張 word 相当の16-bit）の共有デコーダ。純関数。
 * 不正な word は false を返し out->valid=false。入口は EA 検証等のためにこれを呼んでよい。 */
bool mc68k_fpu_decode_command_word(mc68k_fpu_config_t config, uint16_t word,
                                   mc68k_fpu_command_t *out);

/* CIR アクセス。戻り値 false は phase に対して不正なアクセスで、モデルは即時に
 * protocol violation 状態へ遷移する（次の mc68k_fpu_read_response() が vector 13 の
 * TAKE_MID_EXCEPTION を返す）。 */
bool mc68k_fpu_write_command_word(mc68k_fpu_t *, uint16_t word);
bool mc68k_fpu_write_condition(mc68k_fpu_t *, uint8_t predicate); /* 下位6bitのみ使用 */
/* operand CIR。EA 転送、MP register 転送、FSAVE/FRESTORE の body はこの1組で扱い、モデルが
 * phase で振り分ける */
bool mc68k_fpu_write_operand(mc68k_fpu_t *, uint32_t word);
bool mc68k_fpu_read_operand(mc68k_fpu_t *, uint32_t *word);
bool mc68k_fpu_read_register_select(mc68k_fpu_t *, uint16_t *mask);
bool mc68k_fpu_write_register_select(mc68k_fpu_t *); /* 常に protocol violation（false） */
bool mc68k_fpu_write_instruction_address(
    mc68k_fpu_t *, uint32_t pc); /* 68881 は常に合法、68882 は phase による */
mc68k_fpu_response_t mc68k_fpu_read_response(mc68k_fpu_t *);
void mc68k_fpu_control(mc68k_fpu_t *, mc68k_fpu_control_t control);

/* save/restore CIR。begin_* は常に合法。body は operand CIR で longword 単位に転送する。
 * 予約 word は入口が frame 組立て時に置く（null は0、idle/busy は $FFFF）。 */
void mc68k_fpu_begin_save(mc68k_fpu_t *, uint16_t *format_word, size_t *body_bytes);
void mc68k_fpu_begin_restore(mc68k_fpu_t *, uint16_t format_word, uint16_t *readback,
                             size_t *body_bytes);

/* 観測 API（設計書 §9）。FPCR/FPSR の32bitパッキングは control register move の転送と同一。
 * NULL 引数では何もしない。書込み API は設けない（テストも command/operand/FRESTORE を通す）。
 */
typedef struct {
    mc68k_fp80_t fp[8];
    uint32_t fpcr;
    uint32_t fpsr;
    uint32_t fpiar;
} mc68k_fpu_regs_t;

void mc68k_get_fpu_regs(const mc68k_fpu_t *fpu, mc68k_fpu_regs_t *out);

/*
 * 丸め前の中間結果（MC68040 の E3 state frame 用）。
 *
 * 共通コアの値演算（add/sub/mul/div/sqrt/sglmul/sgldiv のほか move、to_s/to_d/to_x など
 * roundPack を通る関数）は、最後に 行った 1
 * 回の丸め（roundPack）の入力をここへ残す。結果・EXC/AEXC/CC
 * は変わらない。丸めを通らない経路（NaN・無限大・ 厳密な
 * 0・厳密に打ち消し合う加減算）は何も残さず、前の演算の値がそのまま残りうる。そのため、読む前に
 * mc68k_fpu_unrounded_reset() で消し、演算の直後に読む。EXC の OVFL/UNFL/INEX2
 * が立った演算だけが、同じ演算の 丸めの値を確実に残す（mc68k_fpu_to_l/w/b は roundPack
 * を通さずに INEX2 を立てるので、整数宛ての INEX2 には当てはまらない）。
 *
 * exp は idealized 指数（指数フィールド 0・整数部 1 が 0、非正規化域は負、最大有限値の指数は
 * 0x7FFE）。 sig は 64 bit 仮数（bit63 = 1）、sig_extra はその下の 64 bit（bit63 = guard、bit62
 * = round、 残りの OR = sticky）。mantissa_bits は丸め先の仮数の有効ビット数（拡張 64、倍精度
 * 53、単精度 24）。
 */
typedef struct {
    bool valid;
    bool sign;
    int32_t exp;
    uint64_t sig;
    uint64_t sig_extra;
    uint8_t mantissa_bits;
    uint8_t round_mode; /* mc68k_fpu_round_mode_t */
} mc68k_fpu_unrounded_t;

void mc68k_fpu_unrounded_reset(void);
/* 中間結果があれば *out に入れて true。無ければ false（*out は変えない）。 */
bool mc68k_fpu_unrounded_get(mc68k_fpu_unrounded_t *out);
/* 指数の範囲を制限せずに仮数だけを mantissa_bits へ丸める（OVFL の E3 frame の
 * WBTEMP。桁上がりは exp に反映）。 out->sig_extra は 0。 */
void mc68k_fpu_unrounded_round_mantissa(const mc68k_fpu_unrounded_t *in,
                                        mc68k_fpu_unrounded_t *out);
