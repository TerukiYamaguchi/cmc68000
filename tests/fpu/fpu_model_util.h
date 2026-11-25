/*
 * fpu_model_util.h — コプロセッサモデルの試験で共有する道具。
 *
 * 期待値はこのヘッダでも試験本体でも、production の定数・関数から取らない。
 * command word は設計書 §5 / [881UM §4.7 Table 4-11〜4-18] のビット配置から組み立てる。
 */
#pragma once
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mc68000/fpu.h"

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            exit(1);                                                                 \
        }                                                                            \
    } while (0)

#define CHECK_EQ(a, b)                                                                     \
    do {                                                                                   \
        uint64_t va_ = (uint64_t)(a), vb_ = (uint64_t)(b);                                 \
        if (va_ != vb_) {                                                                  \
            fprintf(stderr, "%s:%d: CHECK_EQ failed: %s=0x%" PRIx64 " %s=0x%" PRIx64 "\n", \
                    __FILE__, __LINE__, #a, va_, #b, vb_);                                 \
            exit(1);                                                                       \
        }                                                                                  \
    } while (0)

/* 文字列配列の要素がすべて異なること。名前リストの要素を別の要素の複製へ書き換える変異（件数だけの
 * assert では通る）を落とす */
#define CHECK_DISTINCT_STR(arr)                             \
    do {                                                    \
        size_t a_, b_, n_ = sizeof(arr) / sizeof((arr)[0]); \
        for (a_ = 0; a_ < n_; a_++)                         \
            for (b_ = 0; b_ < a_; b_++)                     \
                CHECK(strcmp((arr)[a_], (arr)[b_]) != 0);   \
    } while (0)

/* command word: 000 FPm,FPn / 010 <ea>,FPn / 011 FPn,<ea> / 100,101 control / 110,111 FMOVEM */
#define CMD(cls, field, dst, low) \
    ((uint16_t)(((cls) << 13) | ((field) << 10) | ((dst) << 7) | (low)))
#define FMT_L       0
#define FMT_S       1
#define FMT_X       2
#define FMT_P       3
#define FMT_W       4
#define FMT_D       5
#define FMT_B       6
#define OPM_FMOVE   0x00
#define OPM_FSQRT   0x04
#define OPM_FABS    0x18
#define OPM_FNEG    0x1A
#define OPM_FDIV    0x20
#define OPM_FMUL    0x23
#define OPM_FADD    0x22
#define OPM_FSUB    0x28
#define OPM_FCMP    0x38
#define OPM_FTST    0x3A
#define OPM_FSINCOS 0x30
#define CTL_FPCR    4
#define CTL_FPSR    2
#define CTL_FPIAR   1

#define ENABLE_BSUN  0x80
#define ENABLE_SNAN  0x40
#define ENABLE_OPERR 0x20
#define ENABLE_OVFL  0x10
#define ENABLE_UNFL  0x08
#define ENABLE_DZ    0x04
#define ENABLE_INEX2 0x02
#define ENABLE_INEX1 0x01

/* FPCR/FPSR の32bit値（[881UM Figure 2-2〜2-7]）。ENABLE/EXC は bit15-8 */
#define FPCR_ENABLE(e) ((uint32_t)(e) << 8)
#define FPSR_CC_N      0x08000000u
#define FPSR_CC_Z      0x04000000u
#define FPSR_CC_I      0x02000000u
#define FPSR_CC_NAN    0x01000000u
#define FPSR_EXC(e)    ((uint32_t)(e) << 8)

/* 機種ループの機種。配列でなく関数にして、2機種（68881・68882）の中身を1か所に固定する。各試験は
 * `for (i = 0; i < 2; i++)` の中で `model_cfg(i)` を使い、ループ直後に `CHECK_EQ(i, 2)` を置く
 */
static inline mc68k_fpu_config_t model_cfg(unsigned i) {
    CHECK(i < 2);
    return i == 0 ? MC68K_FPU_68881 : MC68K_FPU_68882;
}

static inline mc68k_fpu_t *mk(mc68k_fpu_config_t c) {
    mc68k_fpu_t *f = mc68k_fpu_create(c);

    CHECK(f != NULL);
    return f;
}

static inline mc68k_fpu_response_t rd(mc68k_fpu_t *f) {
    return mc68k_fpu_read_response(f);
}

/* 応答を読んで種類を検査する。読み捨てると、応答の種類が変わっても通る */
static inline mc68k_fpu_response_t rd_kind(mc68k_fpu_t *f, mc68k_fpu_response_kind_t kind) {
    mc68k_fpu_response_t r = rd(f);

    CHECK_EQ(r.kind, kind);
    return r;
}

static inline void expect_null(mc68k_fpu_t *f, int ca, int pc) {
    mc68k_fpu_response_t r = rd(f);

    CHECK_EQ(r.kind, MC68K_FPU_RESPONSE_NULL);
    CHECK_EQ(r.coprocessor_active, ca);
    CHECK_EQ(r.request_pc, pc);
}

/* 最終応答の NULL（CA=0、PC=0）。idle の NULL と同じ値 */
static inline void expect_idle(mc68k_fpu_t *f) {
    mc68k_fpu_response_t r = rd(f);

    CHECK_EQ(r.kind, MC68K_FPU_RESPONSE_NULL);
    CHECK_EQ(r.coprocessor_active, 0);
    CHECK_EQ(r.request_pc, 0);
    CHECK_EQ(r.interruptible, 0); /* IA=0 */
    CHECK_EQ(r.processing, 0);    /* PF=1（idle）。Table 7-3 の2行目 */
}

static inline void expect_take(mc68k_fpu_t *f, mc68k_fpu_response_kind_t kind, int vector,
                               int pc) {
    mc68k_fpu_response_t r = rd(f);

    CHECK_EQ(r.kind, kind);
    CHECK_EQ(r.vector, vector);
    CHECK_EQ(r.request_pc, pc);
    CHECK_EQ(r.coprocessor_active, 0);
}

static inline mc68k_fpu_regs_t regs(const mc68k_fpu_t *f) {
    mc68k_fpu_regs_t r;

    mc68k_get_fpu_regs(f, &r);
    return r;
}

/* ---- ダイアログの駆動 ----------------------------------------------------- */

/* <ea>,FPn の演算・FMOVE（入力）。PC=0 前提（ENABLE の算術例外ビットが0）。words は n 個 */
static inline void drive_ea_in(mc68k_fpu_t *f, uint16_t cmd, const uint32_t *words, unsigned n,
                               unsigned expect_len) {
    mc68k_fpu_response_t r;
    unsigned i;

    CHECK(mc68k_fpu_write_command_word(f, cmd));
    r = rd(f);
    CHECK_EQ(r.kind, MC68K_FPU_RESPONSE_EA_TRANSFER);
    CHECK_EQ(r.transfer_to_host, 0);
    CHECK_EQ(r.request_pc, 0);
    CHECK_EQ(r.length, expect_len);
    for (i = 0; i < n; i++)
        CHECK(mc68k_fpu_write_operand(f, words[i]));
    expect_idle(f);
}

static inline void drive_regreg(mc68k_fpu_t *f, uint16_t cmd) {
    CHECK(mc68k_fpu_write_command_word(f, cmd));
    expect_null(f, 0, 0);
    expect_idle(f);
}

/* FMOVE FPn,<ea>（出力、PC=0 前提）。出力 word 列を out に入れる */
static inline void drive_ea_out(mc68k_fpu_t *f, uint16_t cmd, uint32_t *out, unsigned n,
                                unsigned expect_len) {
    mc68k_fpu_response_t r;
    unsigned i;

    CHECK(mc68k_fpu_write_command_word(f, cmd));
    expect_null(f, 1, 0);
    r = rd(f);
    CHECK_EQ(r.kind, MC68K_FPU_RESPONSE_EA_TRANSFER);
    CHECK_EQ(r.transfer_to_host, 1);
    CHECK_EQ(r.length, expect_len);
    for (i = 0; i < n; i++)
        CHECK(mc68k_fpu_read_operand(f, &out[i]));
    expect_idle(f);
}

/* control register 入力（FMOVE <ea>,FPcr）。words は popcount(select) 個 */
static inline void drive_ctrl_in(mc68k_fpu_t *f, unsigned select, const uint32_t *words,
                                 unsigned n) {
    mc68k_fpu_response_t r;
    unsigned i;

    CHECK(mc68k_fpu_write_command_word(f, CMD(4, select, 0, 0)));
    r = rd(f);
    CHECK_EQ(r.kind, MC68K_FPU_RESPONSE_EA_TRANSFER);
    CHECK_EQ(r.transfer_to_host, 0);
    CHECK_EQ(r.length, 4 * n);
    for (i = 0; i < n; i++)
        CHECK(mc68k_fpu_write_operand(f, words[i]));
    expect_idle(f);
}

static inline void set_fpcr(mc68k_fpu_t *f, uint32_t v) {
    drive_ctrl_in(f, CTL_FPCR, &v, 1);
}

static inline void set_fpsr(mc68k_fpu_t *f, uint32_t v) {
    drive_ctrl_in(f, CTL_FPSR, &v, 1);
}

/* FMOVE.L #v,FPn */
static inline void load_long(mc68k_fpu_t *f, unsigned fpn, int32_t v) {
    uint32_t w = (uint32_t)v;

    drive_ea_in(f, CMD(2, FMT_L, fpn, OPM_FMOVE), &w, 1, 4);
}

/* FMOVE.X #(se,hi,lo),FPn */
static inline void load_x(mc68k_fpu_t *f, unsigned fpn, uint16_t se, uint64_t signif) {
    uint32_t w[3] = {(uint32_t)se << 16, (uint32_t)(signif >> 32), (uint32_t)signif};

    drive_ea_in(f, CMD(2, FMT_X, fpn, OPM_FMOVE), w, 3, 12);
}
