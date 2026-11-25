/*
 * ops_move.c — MOVE / MOVEA 実装
 *
 * MOVE の宛先 EA は opcode 内でフィールド順序が逆になっている点に注意:
 *   ソース  : bits 5-3 = mode, bits 2-0 = reg  （自然な順序）
 *   宛先    : bits 11-9 = reg,  bits 8-6 = mode （逆！）
 */
#include "cpu_internal.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* ================================================================== */
/* CCR 更新: MOVE                                                      */
/* ================================================================== */

/*
 * MOVE: N/Z をセット、V/C をクリア、X は変化なし。
 */
static void ccr_update_move(mc68k_state_t *cpu, uint32_t val, int size) {
    cpu->sr &= ~(CCR_N | CCR_Z | CCR_V | CCR_C);
    if (!(val & size_mask(size)))
        cpu->sr |= CCR_Z;
    if (val & sign_bit(size))
        cpu->sr |= CCR_N;
}

/* UM20 8.2.6 の Cache Case。MOVE は意図的に汎用 EA 表から再構成しない。
 * そうすると二重計上し、プレデクリメントの例外も取りこぼす。 */
enum {
    MOVE20_SRC_RN,
    MOVE20_SRC_IMM_BW,
    MOVE20_SRC_IMM_L,
    MOVE20_SRC_AI,
    MOVE20_SRC_PI,
    MOVE20_SRC_PD,
    MOVE20_SRC_DI,
    MOVE20_SRC_AW,
    MOVE20_SRC_AL,
    MOVE20_SRC_IX_BRIEF,
    MOVE20_SRC_IX_BD_W,
    MOVE20_SRC_B,
    MOVE20_SRC_BD_W,
    MOVE20_SRC_BD_L,
    MOVE20_SRC_IND_B_NULL,
    MOVE20_SRC_IND_B_W,
    MOVE20_SRC_IND_B_L,
    MOVE20_SRC_IND_BD_W_NULL,
    MOVE20_SRC_IND_BD_W_W,
    MOVE20_SRC_IND_BD_W_L,
    MOVE20_SRC_IND_BD_L_NULL,
    MOVE20_SRC_IND_BD_L_W,
    MOVE20_SRC_IND_BD_L_L,
    MOVE20_SRC_COUNT,
};

enum {
    MOVE20_DST_AN,
    MOVE20_DST_DN,
    MOVE20_DST_AI,
    MOVE20_DST_PI,
    MOVE20_DST_PD,
    MOVE20_DST_DI,
    MOVE20_DST_AW,
    MOVE20_DST_AL,
    MOVE20_DST_IX_BRIEF,
    MOVE20_DST_IX_BD_W,
    MOVE20_DST_B,
    MOVE20_DST_BD_W,
    MOVE20_DST_BD_L,
    MOVE20_DST_IND_B_NULL,
    MOVE20_DST_IND_B_W,
    MOVE20_DST_IND_B_L,
    MOVE20_DST_IND_BD_W_NULL,
    MOVE20_DST_IND_BD_W_W,
    MOVE20_DST_IND_BD_W_L,
    MOVE20_DST_IND_BD_L_NULL,
    MOVE20_DST_IND_BD_L_W,
    MOVE20_DST_IND_BD_L_L,
    MOVE20_DST_COUNT,
};

static int move20_indexed_kind(const ea_desc_t *ea, int brief, int bd_w, int base,
                               int bd_w_direct, int bd_l_direct, int ind_b) {
    if (ea->ext_form == EA_EXT_BRIEF)
        return brief;
    if (ea->ext_form != EA_EXT_FULL)
        return -1;
    if (ea->iis == 0) {
        if (ea->bd_size == 0)
            return base;
        if (ea->bd_size == 1)
            return ea->base_suppressed ? bd_w_direct : bd_w;
        if (ea->bd_size == 2)
            return bd_l_direct;
        return -1;
    }
    if (ea->iis == 1 || ea->iis == 5)
        return ind_b + ea->bd_size * 3;
    if (ea->iis == 2 || ea->iis == 6)
        return ind_b + ea->bd_size * 3 + 1;
    if (ea->iis == 3 || ea->iis == 7)
        return ind_b + ea->bd_size * 3 + 2;
    return -1;
}

static int move20_source_kind(int size, const ea_desc_t *ea) {
    switch (ea->mode) {
    case 0:
    case 1:
        return MOVE20_SRC_RN;
    case 2:
        return MOVE20_SRC_AI;
    case 3:
        return MOVE20_SRC_PI;
    case 4:
        return MOVE20_SRC_PD;
    case 5:
        return MOVE20_SRC_DI;
    case 6:
        return move20_indexed_kind(ea, MOVE20_SRC_IX_BRIEF, MOVE20_SRC_IX_BD_W, MOVE20_SRC_B,
                                   MOVE20_SRC_BD_W, MOVE20_SRC_BD_L, MOVE20_SRC_IND_B_NULL);
    case 7:
        if (ea->reg == 0)
            return MOVE20_SRC_AW;
        if (ea->reg == 1)
            return MOVE20_SRC_AL;
        if (ea->reg == 2)
            return MOVE20_SRC_DI;
        if (ea->reg == 3)
            return move20_indexed_kind(ea, MOVE20_SRC_IX_BRIEF, MOVE20_SRC_IX_BD_W,
                                       MOVE20_SRC_B, MOVE20_SRC_BD_W, MOVE20_SRC_BD_L,
                                       MOVE20_SRC_IND_B_NULL);
        if (ea->reg == 4)
            return size == 4 ? MOVE20_SRC_IMM_L : MOVE20_SRC_IMM_BW;
        return -1;
    default:
        return -1;
    }
}

static int move20_destination_kind(const ea_desc_t *ea) {
    switch (ea->mode) {
    case 0:
        return MOVE20_DST_DN;
    case 1:
        return MOVE20_DST_AN;
    case 2:
        return MOVE20_DST_AI;
    case 3:
        return MOVE20_DST_PI;
    case 4:
        return MOVE20_DST_PD;
    case 5:
        return MOVE20_DST_DI;
    case 6:
        return move20_indexed_kind(ea, MOVE20_DST_IX_BRIEF, MOVE20_DST_IX_BD_W, MOVE20_DST_B,
                                   MOVE20_DST_BD_W, MOVE20_DST_BD_L, MOVE20_DST_IND_B_NULL);
    case 7:
        if (ea->reg == 0)
            return MOVE20_DST_AW;
        if (ea->reg == 1)
            return MOVE20_DST_AL;
        return -1;
    default:
        return -1;
    }
}

static int move20_cycles(int size, const ea_desc_t *src, const ea_desc_t *dst) {
    static const uint8_t matrix[MOVE20_SRC_COUNT][MOVE20_DST_COUNT] = {
        {2, 2, 4, 4, 5, 5, 4, 6, 7, 9, 8, 10, 14, 12, 14, 15, 14, 16, 17, 18, 20, 21},
        {4, 4, 6, 6, 7, 7, 6, 8, 7, 9, 8, 10, 14, 12, 14, 15, 14, 16, 17, 18, 20, 21},
        {6, 6, 8, 8, 9, 9, 8, 10, 9, 11, 10, 12, 16, 14, 16, 17, 16, 18, 19, 20, 22, 23},
        {6, 6, 7, 7, 7, 7, 7, 9, 9, 11, 10, 12, 16, 14, 16, 17, 16, 18, 19, 20, 22, 23},
        {6, 6, 7, 7, 7, 7, 7, 9, 9, 11, 10, 12, 16, 14, 16, 17, 16, 18, 19, 20, 22, 23},
        {7, 7, 8, 8, 8, 8, 8, 10, 10, 12, 11, 13, 17, 15, 17, 18, 17, 19, 20, 21, 23, 24},
        {7, 7, 8, 8, 8, 8, 8, 10, 10, 12, 11, 13, 17, 15, 17, 18, 17, 19, 20, 21, 23, 24},
        {6, 6, 7, 7, 7, 7, 7, 9, 9, 11, 10, 12, 16, 14, 16, 17, 16, 18, 19, 20, 22, 23},
        {6, 6, 7, 7, 7, 7, 7, 9, 9, 11, 10, 12, 16, 14, 16, 17, 16, 18, 19, 20, 22, 23},
        {9, 9, 10, 10, 10, 10, 10, 12, 12, 14, 13, 15, 19, 17, 19, 20, 19, 21, 22, 23, 25, 26},
        {9, 9, 10, 10, 10, 10, 10, 12, 12, 14, 13, 15, 19, 17, 19, 20, 19, 21, 22, 23, 25, 26},
        {9, 9, 10, 10, 10, 10, 10, 12, 12, 14, 13, 15, 19, 17, 19, 20, 19, 21, 22, 23, 25, 26},
        {11, 11, 12, 12, 12, 12, 12, 14, 14, 16, 15,
         17, 21, 19, 21, 22, 21, 23, 24, 25, 27, 28},
        {15, 15, 16, 16, 16, 16, 16, 18, 18, 20, 19,
         21, 25, 23, 25, 26, 25, 27, 28, 29, 31, 32},
        {14, 14, 15, 15, 15, 15, 15, 17, 17, 19, 18,
         20, 24, 22, 24, 25, 24, 26, 27, 28, 30, 31},
        {16, 16, 17, 17, 17, 17, 17, 19, 19, 21, 20,
         22, 26, 24, 26, 27, 26, 28, 29, 30, 32, 33},
        {16, 16, 17, 17, 17, 17, 17, 19, 19, 21, 20,
         22, 26, 24, 26, 27, 26, 28, 29, 30, 32, 33},
        {16, 16, 17, 17, 17, 17, 17, 19, 19, 21, 20,
         22, 26, 24, 26, 27, 26, 28, 29, 30, 32, 33},
        {18, 18, 19, 19, 19, 19, 19, 21, 21, 23, 22,
         24, 28, 26, 28, 29, 28, 30, 31, 32, 34, 35},
        {18, 18, 19, 19, 19, 19, 19, 21, 21, 23, 22,
         24, 28, 26, 28, 29, 28, 30, 31, 32, 34, 35},
        {20, 20, 21, 21, 21, 21, 21, 23, 23, 25, 24,
         26, 30, 28, 30, 31, 30, 32, 33, 34, 36, 37},
        {22, 22, 23, 23, 23, 23, 23, 25, 25, 27, 26,
         28, 32, 30, 32, 33, 32, 34, 35, 36, 38, 39},
        {22, 22, 23, 23, 23, 23, 23, 25, 25, 27, 26,
         28, 32, 30, 32, 33, 32, 34, 35, 36, 38, 39},
    };
    int s = src ? move20_source_kind(size, src) : -1;
    int d = dst ? move20_destination_kind(dst) : -1;
    if (s < 0 || d < 0) {
        fprintf(stderr, "mc68000: invalid 68020 MOVE timing selector (src=%d dst=%d)\n", s, d);
        abort();
    }
    return matrix[s][d];
}

/* UM30 11.6.6 の I-Cache Case。表は宛先処理を示し、EA/SOURCE 行の '*' は
 * ソース FEA を加える印である。レジスタソースは別掲の Rn 行を使い、メモリ宛先の
 * 基準値は 1 クロック低い。表にない唯一の正当なフル拡張組合せは、通常ベースの
 * null/long BD である。印刷された通常 BD.W 行と対応するベース抑制行から推定し、
 * 同じ BD/IIS クラスなら通常ベースは 2 クロック低い。 */
static int move30_full_destination_cycles(const ea_desc_t *dst) {
    static const uint8_t direct[2][3] = {
        {6, 8, 12},  /* normal base: inferred null/long BD; BD.W printed */
        {8, 10, 14}, /* base suppressed: (B), (d16,B), (d32,B) */
    };
    static const uint8_t indirect[2][3][3] = {
        {{8, 10, 12}, {10, 12, 14}, {14, 16, 18}},
        {{10, 12, 14}, {12, 14, 16}, {16, 18, 20}},
    };
    unsigned bd = dst->bd_size;
    unsigned od = (unsigned)(dst->iis & 3);

    if (bd > 2 || dst->iis == 4)
        return -1;
    if (dst->iis == 0)
        return direct[dst->base_suppressed != 0][bd];
    if (od == 0)
        return -1;
    return indirect[dst->base_suppressed != 0][bd][od - 1];
}

static int move30_destination_cycles(const ea_desc_t *dst) {
    switch (dst->mode) {
    case 0:
    case 1:
        return 2;
    case 2:
    case 3:
    case 4:
    case 5:
        return 4;
    case 6:
        if (dst->ext_form == EA_EXT_FULL)
            return move30_full_destination_cycles(dst);
        return 6;
    case 7:
        if (dst->reg == 0)
            return 4;
        if (dst->reg == 1)
            return 6;
        return -1;
    default:
        return -1;
    }
}

static int move30_cycles(mc68k_state_t *cpu, const ea_desc_t *src, const ea_desc_t *dst) {
    int destination = move30_destination_cycles(dst);
    int register_source = src->mode <= 1;
    if (destination < 0) {
        fprintf(stderr, "mc68000: invalid 68030 MOVE timing destination\n");
        abort();
    }
    if (register_source) {
        /* §11.6.6 では Rn,mem は SOURCE,mem 行より 1 クロック低い。 */
        if (dst->mode == 2 || dst->mode == 3)
            destination--;
        return destination;
    }
    return destination + ea_cycles(cpu, EA_COST_FETCH, src);
}

/* 040UM 表 10-4（PDF 300--301、印刷版 10-9--10-10）。マニュアルの行列は命令固有であり、
 * <ea> 計算列がソース／宛先組を対象とするため、ea_cycles() から組み立てられない。
 *
 * 直接転記した形式には有効なインデックスを持つフル拡張を含める。MOVEA（宛先 An）と
 * インデックス抑制フル拡張は、表 10-4 に専用カテゴリがないため 68030 経路を維持する。 */
enum {
    MOVE40_SRC_DN,
    MOVE40_SRC_AI,
    MOVE40_SRC_PI,
    MOVE40_SRC_PD,
    MOVE40_SRC_DI,
    MOVE40_SRC_PCDI,
    MOVE40_SRC_ABS,
    MOVE40_SRC_IMM,
    MOVE40_SRC_IX_AN,
    MOVE40_SRC_IX_PC,
    MOVE40_SRC_FULL_DIRECT,
    MOVE40_SRC_FULL_PRE,
    MOVE40_SRC_FULL_PRE_OD,
    MOVE40_SRC_FULL_POST,
    MOVE40_SRC_FULL_POST_OD,
    MOVE40_SRC_COUNT,
};
enum {
    MOVE40_DST_DN,
    MOVE40_DST_AI,
    MOVE40_DST_PI,
    MOVE40_DST_PD,
    MOVE40_DST_DI,
    MOVE40_DST_ABS,
    MOVE40_DST_IX_AN,
    MOVE40_DST_FULL_DIRECT,
    MOVE40_DST_FULL_PRE,
    MOVE40_DST_FULL_PRE_OD,
    MOVE40_DST_FULL_POST,
    MOVE40_DST_FULL_POST_OD,
    MOVE40_DST_COUNT,
};

static int move40_full_kind(const ea_desc_t *ea, int direct, int pre, int pre_od, int post,
                            int post_od) {
    if (ea->ext_form != EA_EXT_FULL || ea->index_suppressed || ea->iis == 4)
        return -1;
    if (ea->iis == 0)
        return direct;
    if (ea->iis <= 3)
        return ea->iis == 1 ? pre : pre_od;
    return ea->iis == 5 ? post : post_od;
}

static int move40_source_kind(const ea_desc_t *ea) {
    switch (ea->mode) {
    case 0:
        return MOVE40_SRC_DN;
    case 2:
        return MOVE40_SRC_AI;
    case 3:
        return MOVE40_SRC_PI;
    case 4:
        return MOVE40_SRC_PD;
    case 5:
        return MOVE40_SRC_DI;
    case 6:
        return ea->ext_form == EA_EXT_FULL
                   ? move40_full_kind(ea, MOVE40_SRC_FULL_DIRECT, MOVE40_SRC_FULL_PRE,
                                      MOVE40_SRC_FULL_PRE_OD, MOVE40_SRC_FULL_POST,
                                      MOVE40_SRC_FULL_POST_OD)
                   : MOVE40_SRC_IX_AN;
    case 7:
        if (ea->reg == 0 || ea->reg == 1)
            return MOVE40_SRC_ABS;
        if (ea->reg == 2)
            return MOVE40_SRC_PCDI;
        if (ea->reg == 3)
            return ea->ext_form == EA_EXT_FULL
                       ? move40_full_kind(ea, MOVE40_SRC_FULL_DIRECT, MOVE40_SRC_FULL_PRE,
                                          MOVE40_SRC_FULL_PRE_OD, MOVE40_SRC_FULL_POST,
                                          MOVE40_SRC_FULL_POST_OD)
                       : MOVE40_SRC_IX_PC;
        if (ea->reg == 4)
            return MOVE40_SRC_IMM;
        return -1;
    default:
        return -1;
    }
}

static int move40_destination_kind(const ea_desc_t *ea) {
    switch (ea->mode) {
    case 0:
        return MOVE40_DST_DN;
    case 2:
        return MOVE40_DST_AI;
    case 3:
        return MOVE40_DST_PI;
    case 4:
        return MOVE40_DST_PD;
    case 5:
        return MOVE40_DST_DI;
    case 6:
        return ea->ext_form == EA_EXT_FULL
                   ? move40_full_kind(ea, MOVE40_DST_FULL_DIRECT, MOVE40_DST_FULL_PRE,
                                      MOVE40_DST_FULL_PRE_OD, MOVE40_DST_FULL_POST,
                                      MOVE40_DST_FULL_POST_OD)
                   : MOVE40_DST_IX_AN;
    case 7:
        return (ea->reg == 0 || ea->reg == 1) ? MOVE40_DST_ABS : -1;
    default:
        return -1;
    }
}

static int move40_cycles(const ea_desc_t *src, const ea_desc_t *dst) {
    /* Table 10-4's calculate + execute-base columns.  Table 10-2 source
     * fetch and indirect-destination send accesses are added below. */
    static const uint8_t calc_execute[MOVE40_SRC_COUNT][MOVE40_DST_COUNT] = {
        {2, 2, 2, 2, 2, 2, 6, 13, 19, 21, 19, 21},
        {2, 2, 3, 3, 3, 2, 8, 13, 19, 21, 19, 21},
        {3, 3, 3, 3, 3, 3, 8, 15, 21, 23, 21, 23},
        {3, 3, 3, 3, 3, 3, 8, 15, 21, 23, 21, 23},
        {3, 3, 3, 3, 3, 3, 8, 15, 21, 23, 21, 23},
        {4, 4, 4, 4, 5, 5, 12, 16, 22, 24, 22, 24},
        {2, 2, 3, 3, 3, 3, 8, 13, 19, 21, 19, 21},
        {2, 2, 3, 3, 3, 3, 6, 13, 19, 21, 19, 21},
        {6, 8, 10, 10, 10, 10, 16, 20, 26, 28, 28, 30},
        {9, 9, 11, 11, 11, 11, 17, 21, 27, 29, 29, 31},
        {13, 13, 15, 15, 15, 15, 21, 25, 31, 33, 31, 33},
        {19, 19, 21, 21, 21, 21, 27, 31, 37, 39, 37, 39},
        {21, 21, 23, 23, 23, 23, 29, 33, 39, 41, 39, 41},
        {19, 19, 21, 21, 21, 21, 27, 31, 37, 39, 35, 37},
        {21, 21, 23, 23, 23, 23, 29, 33, 39, 41, 39, 41},
    };
    int s = move40_source_kind(src);
    int d = move40_destination_kind(dst);
    if (s < 0 || d < 0)
        return -1;
    int source_fetch = s == MOVE40_SRC_DN || s == MOVE40_SRC_IMM ? 0
                       : s >= MOVE40_SRC_FULL_PRE                ? 2
                                                                 : 1;
    int destination_send = d >= MOVE40_DST_FULL_PRE ? 1 : 0;
    return calc_execute[s][d] + source_fetch + destination_send;
}

/* ================================================================== */
/* MOVE / MOVEA 実装                                                   */
/* ================================================================== */

static int do_move(mc68k_state_t *cpu, uint16_t opcode, int size) {
    int src_mode = (opcode >> 3) & 7;
    int src_reg = (opcode >> 0) & 7;
    int dst_reg = (opcode >> 9) & 7;
    int dst_mode = (opcode >> 6) & 7;

    uint32_t val = ea_read(cpu, src_mode, src_reg, size);
    if (cpu->exec_state != CPU_NORMAL)
        return 4;
    ea_desc_t src_ea = cpu->last_ea;

    if (dst_mode == 1) {
        /* MOVEA: CCR 更新なし。MOVEA.W は符号拡張 */
        if (size == 2)
            val = (uint32_t)(int32_t)(int16_t)(uint16_t)val;
        /* 不正ポインタ代入の検出: 上位8bit が $04〜$FE の値を An に積んだら記録。
         * X68000 は 24bit アドレスなので暴走源（不正ポインタ/リロケート漏れ）の兆候。
         * $00-$03 は Human68k がフラグ付きポインタとして正常に使うため除外、
         * $FF は -1/ROM ミラーのため除外。MOVEA.W は符号拡張で除外済み。*/
        if (cpu->badmovea_log_on && (val >> 24) >= 0x04u && (val >> 24) != 0xFFu)
            cpu_log(cpu, "BADMOVEA a%d=%08X pc=%06X opcode=%04X\n", dst_reg, val, cpu->pc,
                    opcode);
        cpu->a[dst_reg] = val;
    } else {
        ea_write(cpu, dst_mode, dst_reg, size, val);
        if (cpu->exec_state != CPU_NORMAL)
            return 4;
        ccr_update_move(cpu, val, size);
    }

    if (!cpu_has_68020_isa(cpu)) {
        /* UM00 Tables 8-2/8-3 and DS10 Tables 7-5/7-7: MOVE is
         * base 4 plus the fetch time of each non-register operand. */
        int cycles = 4 + ea_cycles(cpu, EA_COST_FETCH, &src_ea);
        if (dst_mode != 1)
            cycles += ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
        /* プレデクリメント宛先の項目は、両方の MOVE 行列で通常の FEA 分より 2 クロック少ない。
         */
        if (dst_mode == 4)
            cycles -= 2;
        /* DS10 Table 7-7 has the only 68010 MOVE matrix delta. */
        if (cpu_is_68010(cpu) && size == 4 && src_mode <= 1 && dst_mode == 4)
            cycles += 2;
        return cycles;
    }
    ea_desc_t dst_ea = {
        .mode = (uint8_t)dst_mode, .reg = (uint8_t)dst_reg, .size = (uint8_t)size};
    if (dst_mode != 1)
        dst_ea = cpu->last_ea;
    if (cpu_is_68020(cpu))
        return move20_cycles(size, &src_ea, &dst_ea);
    if (cpu_is_68040(cpu)) {
        int cycles = move40_cycles(&src_ea, &dst_ea);
        if (cycles >= 0)
            return cpu_040_tag(cpu, cycles);
    }
    return move30_cycles(cpu, &src_ea, &dst_ea);
}

static int op_move_byte(mc68k_state_t *cpu, uint16_t opcode) {
    return do_move(cpu, opcode, 1);
}
static int op_move_word(mc68k_state_t *cpu, uint16_t opcode) {
    return do_move(cpu, opcode, 2);
}
static int op_move_long(mc68k_state_t *cpu, uint16_t opcode) {
    return do_move(cpu, opcode, 4);
}

/* ================================================================== */
/* テーブル登録                                                        */
/* ================================================================== */

/*
 * MOVE の src/dst 有効判定。
 *   src: 全モード可（MOVEもAn直接を出典として使える）。ただし byte サイズは
 *        src=An 不可（MOVEA.B相当が存在しないため）。
 *   dst: word/long は Alterable（An含む、= MOVEA経路）。byte は Data
 *        Alterable（An不可、byteのMOVEA相当は存在しない）。
 */
static int move_src_ok(int ea, int size) {
    int mode = (ea >> 3) & 7;
    if (mode == 7 && (ea & 7) > 4)
        return 0; /* mode7 reg5-7 は未割当 */
    if (size == 1 && mode == 1)
        return 0;
    return 1;
}

static int move_dst_ok(int ea, int size) {
    return (size == 1) ? ea_is_data_alterable(ea) : ea_is_alterable(ea);
}

void register_move_ops(op_handler_t *table) {
    /* MOVE.B: ライン 1（0x1000-0x1FFF） */
    for (int i = 0x1000; i <= 0x1FFF; i++) {
        int src = i & 0x3F;
        int dst = (uint16_t)(((i >> 9) & 7) | (((i >> 6) & 7) << 3));
        if (!move_src_ok(src, 1) || !move_dst_ok(dst, 1))
            continue;
        table[i] = op_move_byte;
    }
    /* MOVE.L: ライン 2（0x2000-0x2FFF） */
    for (int i = 0x2000; i <= 0x2FFF; i++) {
        int src = i & 0x3F;
        int dst = (uint16_t)(((i >> 9) & 7) | (((i >> 6) & 7) << 3));
        if (!move_src_ok(src, 4) || !move_dst_ok(dst, 4))
            continue;
        table[i] = op_move_long;
    }
    /* MOVE.W: ライン 3（0x3000-0x3FFF） */
    for (int i = 0x3000; i <= 0x3FFF; i++) {
        int src = i & 0x3F;
        int dst = (uint16_t)(((i >> 9) & 7) | (((i >> 6) & 7) << 3));
        if (!move_src_ok(src, 2) || !move_dst_ok(dst, 2))
            continue;
        table[i] = op_move_word;
    }
}
