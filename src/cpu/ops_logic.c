/*
 * ops_logic.c — 論理演算・単項演算・ビット操作命令ハンドラ実装
 *
 * 実装する命令:
 *   - CLR / NEG / NEGX / NOT / TST : 単項演算
 *   - AND / OR / EOR     : 論理演算（レジスタ + メモリ）
 *   - ANDI / ORI / EORI  : 即値論理演算（CCR/SR 操作を含む）
 *   - BTST / BCHG / BCLR / BSET : ビット操作
 *   - MOVE CCR            : CCR ⇔ EA
 *
 * ccr_logic() は ops_transfer.c（MOVEQ/SWAP/EXT）からも呼ばれる共有関数。
 */
#include "cpu_internal.h"
#include <stdint.h>

/* サイズコード → バイト数のルックアップ（0=byte,1=word,2=long）*/
static const int g_sz[3] = {1, 2, 4};

/* ================================================================== */
/* 論理演算 CCR ヘルパー                                               */
/* ================================================================== */

/*
 * AND/OR/EOR/NOT/TST/MOVEQ/SWAP/EXT で共用する CCR 更新。
 * V=0, C=0 をクリア。X は変化なし。N/Z を result から計算。
 */
void ccr_logic(mc68k_state_t *cpu, uint32_t result, int size) {
    cpu->sr &= ~(CCR_N | CCR_Z | CCR_V | CCR_C);
    if (!(result & size_mask(size)))
        cpu->sr |= CCR_Z;
    if (result & sign_bit(size))
        cpu->sr |= CCR_N;
}

/* 040UM 表 10-6 は、読出し EA の AND/OR/TST を ADD/SUB と同じ群に置く。#89 に従い
 * 各行を「計算 + 実行基準値 + 表 10-2 のフェッチ」へ還元する。マニュアルには IS=1 の
 * フル拡張行がないため、その場合は保留する。 */
static int logic40_read_cycles(const ea_desc_t *ea) {
    switch (ea->mode) {
    case 0:
    case 1:
        return 2;
    case 2:
        return 3;
    case 3:
    case 4:
    case 5:
        return 4;
    case 6:
        if (ea->ext_form != EA_EXT_FULL)
            return 7;
        if (ea->index_suppressed && ea->iis == 0)
            return -1;
        switch (ea->iis) {
        case 0:
            return ea->bd_size == 0 ? 12 : 14;
        case 1:
        case 5:
            return 21;
        case 2:
        case 3:
        case 6:
        case 7:
            return 24;
        default:
            return -1;
        }
    case 7:
        if (ea->reg == 0 || ea->reg == 1)
            return 3;
        if (ea->reg == 2)
            return 5;
        if (ea->reg == 3) {
            if (ea->ext_form != EA_EXT_FULL)
                return 10;
            if (ea->index_suppressed && ea->iis == 0)
                return -1;
            switch (ea->iis) {
            case 0:
                return ea->bd_size == 0 ? 12 : 14;
            case 1:
            case 5:
                return 21;
            case 2:
            case 3:
            case 6:
            case 7:
                return 24;
            default:
                return -1;
            }
        }
        if (ea->reg == 4)
            return 2;
        return -1;
    default:
        return -1;
    }
}

/* 040UM 表 10-6 の即値論理 RMW 列。 */
static int logic40_immediate_rmw_cycles(const ea_desc_t *ea) {
    switch (ea->mode) {
    case 0:
        return 2;
    case 2:
        return 3;
    case 3:
    case 4:
    case 5:
        return 4;
    case 6:
        if (ea->ext_form != EA_EXT_FULL)
            return 7;
        if (ea->index_suppressed && ea->iis == 0)
            return -1;
        switch (ea->iis) {
        case 0:
            return ea->bd_size == 0 ? 14 : 16;
        case 1:
        case 5:
            return 22;
        case 2:
        case 3:
        case 6:
        case 7:
            return 24;
        default:
            return -1;
        }
    case 7:
        if (ea->reg == 0 || ea->reg == 1)
            return 4;
        return -1;
    default:
        return -1;
    }
}

/* 040UM 表 10-6（PDF 309、印刷版 10-18）の CLR 列。通常の読出し EA 列と異なり、
 * CLR は宛先オペランドを読まないため、表 10-2 のフェッチ分を加えない。 */
static int logic40_clr_cycles(const ea_desc_t *ea) {
    switch (ea->mode) {
    case 0:
        return 2;
    case 2:
    case 3:
    case 4:
    case 5:
        return 2;
    case 6:
        if (ea->ext_form != EA_EXT_FULL)
            return 6;
        if (ea->index_suppressed && ea->iis == 0)
            return -1;
        switch (ea->iis) {
        case 0:
            return ea->bd_size == 0 ? 11 : 13;
        case 1:
        case 5:
            return 17;
        case 2:
        case 3:
        case 6:
        case 7:
            return 19;
        default:
            return -1;
        }
    case 7:
        if (ea->reg == 0 || ea->reg == 1)
            return 2;
        return -1;
    default:
        return -1;
    }
}

/* 040UM Table 10-6 (PDF 316, printed 10-26), NEG/NEGX/NOT.  These forms
 * read their old destination value, unlike CLR, so the #89 projection adds
 * the Table 10-2 operand fetches. */
static int logic40_unary_rmw_cycles(const ea_desc_t *ea) {
    switch (ea->mode) {
    case 0:
        return 2;
    case 2:
    case 3:
    case 4:
    case 5:
        return 3;
    case 6:
        if (ea->ext_form != EA_EXT_FULL)
            return 7;
        if (ea->index_suppressed && ea->iis == 0)
            return -1;
        switch (ea->iis) {
        case 0:
            return ea->bd_size == 0 ? 12 : 14;
        case 1:
        case 5:
            return 19;
        case 2:
        case 3:
        case 6:
        case 7:
            return 21;
        default:
            return -1;
        }
    case 7:
        if (ea->reg == 0 || ea->reg == 1)
            return 3;
        return -1;
    default:
        return -1;
    }
}

/* 040UM Table 10-6 (PDF 306/308, printed 10-15/10-17).  op is BTST=0,
 * BCHG/BCLR/BSET=1; static selects the #<xxx> half of paired entries. */
static int logic40_bit_cycles(const ea_desc_t *ea, int op, int is_static) {
    int basic, brief, direct, indirect;
    if (op == 0) { /* BTST */
        basic = is_static ? 2 : 3;
        brief = is_static ? 6 : 7;
        direct = is_static ? 13 : 13;
        indirect = is_static ? 19 : 19;
    } else { /* BCHG/BCLR/BSET */
        basic = is_static ? 4 : 5;
        brief = is_static ? 8 : 9;
        direct = is_static ? 15 : 16;
        indirect = is_static ? 21 : 21;
    }
    switch (ea->mode) {
    case 0:
        return op == 0 ? (is_static ? 2 : 3) : (is_static ? 4 : 5);
    case 2:
    case 3:
    case 4:
    case 5:
        return basic;
    case 6:
        if (ea->ext_form != EA_EXT_FULL)
            return brief;
        if (ea->index_suppressed && ea->iis == 0)
            return -1;
        switch (ea->iis) {
        case 0:
            return ea->bd_size == 0 ? direct : direct + 2;
        case 1:
        case 5:
            return indirect;
        case 2:
        case 3:
        case 6:
        case 7:
            return indirect + 2;
        default:
            return -1;
        }
    case 7:
        if (ea->reg == 0 || ea->reg == 1)
            return basic;
        if (op == 0 && ea->reg == 2)
            return is_static ? 5 : 6;
        if (op == 0 && ea->reg == 3) {
            if (ea->ext_form != EA_EXT_FULL)
                return is_static ? 10 : 11;
            if (ea->index_suppressed && ea->iis == 0)
                return -1;
            switch (ea->iis) {
            case 0:
                return direct;
            case 1:
            case 5:
                return indirect;
            case 2:
            case 3:
            case 6:
            case 7:
                return indirect + 2;
            default:
                return -1;
            }
        }
        return -1;
    default:
        return -1;
    }
}

/* 040UM Table 10-6 (PDF 313, printed 10-22), MOVE <ea>,CCR. */
static int logic40_to_ccr_cycles(const ea_desc_t *ea) {
    switch (ea->mode) {
    case 0:
        return 3;
    case 2:
    case 3:
    case 4:
    case 5:
        return 4;
    case 6:
        if (ea->ext_form != EA_EXT_FULL)
            return 8;
        if (ea->index_suppressed && ea->iis == 0)
            return -1;
        switch (ea->iis) {
        case 0:
            return ea->bd_size == 0 ? 13 : 15;
        case 1:
        case 5:
            return 22;
        case 2:
        case 3:
        case 6:
        case 7:
            return 24;
        default:
            return -1;
        }
    case 7:
        if (ea->reg == 0 || ea->reg == 1)
            return 4;
        if (ea->reg == 2)
            return 6;
        if (ea->reg == 3) {
            if (ea->ext_form != EA_EXT_FULL)
                return 9;
            if (ea->index_suppressed && ea->iis == 0)
                return -1;
            switch (ea->iis) {
            case 0:
                return ea->bd_size == 0 ? 13 : 15;
            case 1:
            case 5:
                return 22;
            case 2:
            case 3:
            case 6:
            case 7:
                return 24;
            default:
                return -1;
            }
        }
        if (ea->reg == 4)
            return 3;
        return -1;
    default:
        return -1;
    }
}

/* ================================================================== */
/* CLR / NEG / NOT / TST（ライン 4 単項演算）                         */
/* ================================================================== */

static int op_clr(mc68k_state_t *cpu, uint16_t opcode) {
    int sc = (opcode >> 6) & 3;
    if (sc == 3) {
        cpu->exec_state = CPU_EXCEPTION;
        return 4;
    }
    int size = g_sz[sc];
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg = opcode & 7;

    cpu->ea_read_before_write = true;
    ea_write(cpu, ea_mode, ea_reg, size, 0);
    cpu->ea_read_before_write = false;
    if (cpu->exec_state != CPU_NORMAL)
        return 4;

    /* N=0, Z=1, V=0, C=0, X unchanged */
    cpu->sr &= ~(CCR_N | CCR_Z | CCR_V | CCR_C);
    cpu->sr |= CCR_Z;
    if (cpu_is_68040(cpu)) {
        int cycles = logic40_clr_cycles(&cpu->last_ea);
        if (cycles >= 0)
            return cpu_040_tag(cpu, cycles);
    }
    if (ea_mode == 0 && cpu_is_68030_family(cpu))
        return 2;
    if (ea_mode == 0)
        return size == 4 ? 6 : 4;
    if (cpu_is_68030_family(cpu))
        return 3 + ea_cycles(cpu, EA_COST_CALC, &cpu->last_ea);
    if (cpu_is_68000(cpu))
        return (size == 4 ? 12 : 8) + ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
    if (cpu_is_68010(cpu)) {
        /* DS10 表 7-13 は UM00 表 8-6 と異なり自己完結している。 */
        static const uint8_t bw[] = {0, 0, 8, 8, 10, 12, 16, 12, 16};
        static const uint8_t lng[] = {0, 0, 12, 12, 14, 16, 20, 16, 20};
        unsigned row = ea_mode < 7 ? (unsigned)ea_mode : (unsigned)(7 + ea_reg);
        if (row < sizeof(bw))
            return size == 4 ? lng[row] : bw[row];
    }
    return 8;
}

/*
 * NEG dst: dst ← 0 - dst
 *   C,X: dst ≠ 0 のとき（ボロー発生）
 *   V  : dst が INT_MIN のとき（否定すると元の値に戻る地獄）
 */
static int op_neg(mc68k_state_t *cpu, uint16_t opcode) {
    int sc = (opcode >> 6) & 3;
    if (sc == 3) {
        cpu->exec_state = CPU_EXCEPTION;
        return 4;
    }
    int size = g_sz[sc];
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg = opcode & 7;

    uint32_t rmw_addr;
    uint32_t dst = ea_rmw_read(cpu, ea_mode, ea_reg, size, &rmw_addr, false);
    if (cpu->exec_state != CPU_NORMAL)
        return 4;

    uint32_t sm = size_mask(size);
    uint32_t sb = sign_bit(size);
    uint32_t d = dst & sm;
    uint32_t res = (uint32_t)(0u - d) & sm;

    cpu->sr &= ~(CCR_X | CCR_N | CCR_Z | CCR_V | CCR_C);
    if (!res)
        cpu->sr |= CCR_Z;
    if (res & sb)
        cpu->sr |= CCR_N;
    if (d)
        cpu->sr |= CCR_C | CCR_X; /* ボロー */
    if (d == sb)
        cpu->sr |= CCR_V; /* -INT_MIN = INT_MIN: オーバーフロー */

    ea_rmw_write(cpu, ea_mode, ea_reg, size, rmw_addr, res, false);
    if (cpu_is_68040(cpu)) {
        int cycles = logic40_unary_rmw_cycles(&cpu->last_ea);
        if (cycles >= 0)
            return cpu_040_tag(cpu, cycles);
    }
    /* [UM00 Table 8-12]: Memory形式のLongは12 */
    if (ea_mode == 0 && cpu_is_68030_family(cpu))
        return 2;
    if (ea_mode == 0)
        return (size == 4) ? 6 : 4;
    if (cpu_is_68030_family(cpu))
        return 3 + ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
    return (size == 4 ? 12 : 8) + ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
}

/*
 * NEGX dst: dst ← 0 - dst - X
 *
 * Z フラグが特殊: 結果が非ゼロのときだけクリア（セットはしない）。
 * 多倍長否定演算を NEGX チェーンで実現するとき、Z フラグが
 * 「全ワードが 0 だったか」を表す累積フラグとして機能する。
 */
static int op_negx(mc68k_state_t *cpu, uint16_t opcode) {
    int sc = (opcode >> 6) & 3;
    if (sc == 3) {
        cpu->exec_state = CPU_EXCEPTION;
        return 4;
    }
    int size = g_sz[sc];
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg = opcode & 7;

    uint32_t rmw_addr;
    uint32_t dst = ea_rmw_read(cpu, ea_mode, ea_reg, size, &rmw_addr, false);
    if (cpu->exec_state != CPU_NORMAL)
        return 4;

    uint32_t sm = size_mask(size);
    uint32_t sb = sign_bit(size);
    uint32_t d = dst & sm;
    int x = (cpu->sr & CCR_X) ? 1 : 0;
    uint32_t res = (uint32_t)(0u - d - (uint32_t)x) & sm;

    cpu->sr &= ~(CCR_X | CCR_N | CCR_V | CCR_C);
    if (res)
        cpu->sr &= ~CCR_Z; /* Z は非ゼロのときのみクリア */
    if (res & sb)
        cpu->sr |= CCR_N;
    if (d || x)
        cpu->sr |= CCR_C | CCR_X; /* ボロー */
    if ((d & sb) && (res & sb))
        cpu->sr |= CCR_V; /* 符号付きオーバーフロー */

    ea_rmw_write(cpu, ea_mode, ea_reg, size, rmw_addr, res, false);
    if (cpu_is_68040(cpu)) {
        int cycles = logic40_unary_rmw_cycles(&cpu->last_ea);
        if (cycles >= 0)
            return cpu_040_tag(cpu, cycles);
    }
    /* [UM00 Table 8-12]: Memory形式のLongは12 */
    if (ea_mode == 0 && cpu_is_68030_family(cpu))
        return 2;
    if (ea_mode == 0)
        return (size == 4) ? 6 : 4;
    if (cpu_is_68030_family(cpu))
        return 3 + ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
    return (size == 4 ? 12 : 8) + ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
}

static int op_not(mc68k_state_t *cpu, uint16_t opcode) {
    int sc = (opcode >> 6) & 3;
    if (sc == 3) {
        cpu->exec_state = CPU_EXCEPTION;
        return 4;
    }
    int size = g_sz[sc];
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg = opcode & 7;

    uint32_t rmw_addr;
    uint32_t dst = ea_rmw_read(cpu, ea_mode, ea_reg, size, &rmw_addr, false);
    if (cpu->exec_state != CPU_NORMAL)
        return 4;

    uint32_t res = (~dst) & size_mask(size);
    ccr_logic(cpu, res, size);
    ea_rmw_write(cpu, ea_mode, ea_reg, size, rmw_addr, res, false);
    if (cpu_is_68040(cpu)) {
        int cycles = logic40_unary_rmw_cycles(&cpu->last_ea);
        if (cycles >= 0)
            return cpu_040_tag(cpu, cycles);
    }
    /* [UM00 Table 8-12]: Memory形式のLongは12 */
    if (ea_mode == 0 && cpu_is_68030_family(cpu))
        return 2;
    if (ea_mode == 0)
        return (size == 4) ? 6 : 4;
    if (cpu_is_68030_family(cpu))
        return 3 + ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
    return (size == 4 ? 12 : 8) + ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
}

int op_tst(mc68k_state_t *cpu, uint16_t opcode) {
    int sc = (opcode >> 6) & 3;
    if (sc == 3) {
        cpu->exec_state = CPU_EXCEPTION;
        return 4;
    }
    int size = g_sz[sc];
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg = opcode & 7;

    uint32_t val = ea_read(cpu, ea_mode, ea_reg, size);
    if (cpu->exec_state != CPU_NORMAL)
        return 4;

    ccr_logic(cpu, val, size);
    if (cpu_is_68040(cpu)) {
        int cycles = logic40_read_cycles(&cpu->last_ea);
        if (cycles >= 0)
            return cpu_040_tag(cpu, cycles);
    }
    if (cpu_is_68030_family(cpu))
        return (ea_mode == 0) ? 2 : 2 + ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
    return (ea_mode == 0) ? 4 : 4 + ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
}

/* ================================================================== */
/* AND / OR / EOR（レジスタ ↔ EA）                                    */
/* ================================================================== */

/*
 * AND/OR/EOR 共通実装。op_mode で方向とサイズを判断する。
 *   opmode 0-2 (xxx): EA op Dn → 結果を Dn に
 *   opmode 4-6 (1xx): Dn op EA → 結果を EA に
 * opmode & 3 がサイズコード（0=B, 1=W, 2=L）。
 */
static int op_and(mc68k_state_t *cpu, uint16_t opcode) {
    int dn = (opcode >> 9) & 7;
    int opmode = (opcode >> 6) & 7;
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg = opcode & 7;
    int size = g_sz[opmode & 3];
    uint32_t sm = size_mask(size);

    if (opmode < 4) {
        /* AND EA, Dn */
        uint32_t src = ea_read(cpu, ea_mode, ea_reg, size);
        if (cpu->exec_state != CPU_NORMAL)
            return 4;
        uint32_t res = (cpu->d[dn] & sm) & (src & sm);
        ccr_logic(cpu, res, size);
        cpu->d[dn] = (cpu->d[dn] & ~sm) | (res & sm);
        if (cpu_is_68040(cpu)) {
            int cycles = logic40_read_cycles(&cpu->last_ea);
            if (cycles >= 0)
                return cpu_040_tag(cpu, cycles);
        }
        /* [UM00 Table 8-4] / [DS10 Table 7-9]: EA=Dn の Long は68000=8、
         * 68010=6（CYC_AND_L_EA_REG） */
        if (cpu_is_68030_family(cpu))
            return 2 + ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
        /* B/W は 4+EA、Long は 6+EA。EA が Dn 直接・即値の Long は 68000=8、68010=6
         * （CYC_AND_L_EA_REG）に増える（即値は EA 時間を別途加算する） */
        int base = size == 4 ? ADD_L_EA_BASE_CYCLES : ADD_EA_DN_BASE_CYCLES;
        if (size == 4 && ea_is_direct_or_imm(ea_mode, ea_reg))
            base = CYC(cpu, CYC_AND_L_EA_REG);
        return base + ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
    } else {
        /* AND Dn, EA */
        uint32_t rmw_addr;
        uint32_t dst = ea_rmw_read(cpu, ea_mode, ea_reg, size, &rmw_addr, false);
        if (cpu->exec_state != CPU_NORMAL)
            return 4;
        uint32_t res = (dst & sm) & (cpu->d[dn] & sm);
        ccr_logic(cpu, res, size);
        ea_rmw_write(cpu, ea_mode, ea_reg, size, rmw_addr, res, false);
        if (cpu_is_68040(cpu)) {
            int cycles = logic40_read_cycles(&cpu->last_ea);
            if (cycles >= 0)
                return cpu_040_tag(cpu, cycles);
        }
        if (cpu_is_68030_family(cpu))
            return 3 + ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
        /* [UM00 Table 8-4]: Dn,<M> は B/W が 8+EA、Long が 12+EA */
        return ((size == 4) ? ADD_DN_MEM_L_BASE_CYCLES : ADD_DN_MEM_BASE_CYCLES) +
               ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
    }
}

static int op_or(mc68k_state_t *cpu, uint16_t opcode) {
    int dn = (opcode >> 9) & 7;
    int opmode = (opcode >> 6) & 7;
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg = opcode & 7;
    int size = g_sz[opmode & 3];
    uint32_t sm = size_mask(size);

    if (opmode < 4) {
        uint32_t src = ea_read(cpu, ea_mode, ea_reg, size);
        if (cpu->exec_state != CPU_NORMAL)
            return 4;
        uint32_t res = (cpu->d[dn] & sm) | (src & sm);
        ccr_logic(cpu, res, size);
        cpu->d[dn] = (cpu->d[dn] & ~sm) | (res & sm);
        if (cpu_is_68040(cpu)) {
            int cycles = logic40_read_cycles(&cpu->last_ea);
            if (cycles >= 0)
                return cpu_040_tag(cpu, cycles);
        }
        /* [UM00 Table 8-4] / [DS10 Table 7-9]: EA=Dn の Long は68000=8、
         * 68010=6（CYC_OR_L_EA_REG） */
        if (cpu_is_68030_family(cpu))
            return 2 + ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
        /* B/W は 4+EA、Long は 6+EA。EA が Dn 直接・即値の Long は 68000=8、68010=6
         * （CYC_OR_L_EA_REG）に増える（即値は EA 時間を別途加算する） */
        int base = size == 4 ? ADD_L_EA_BASE_CYCLES : ADD_EA_DN_BASE_CYCLES;
        if (size == 4 && ea_is_direct_or_imm(ea_mode, ea_reg))
            base = CYC(cpu, CYC_OR_L_EA_REG);
        return base + ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
    } else {
        uint32_t rmw_addr;
        uint32_t dst = ea_rmw_read(cpu, ea_mode, ea_reg, size, &rmw_addr, false);
        if (cpu->exec_state != CPU_NORMAL)
            return 4;
        uint32_t res = (dst & sm) | (cpu->d[dn] & sm);
        ccr_logic(cpu, res, size);
        ea_rmw_write(cpu, ea_mode, ea_reg, size, rmw_addr, res, false);
        if (cpu_is_68040(cpu)) {
            int cycles = logic40_read_cycles(&cpu->last_ea);
            if (cycles >= 0)
                return cpu_040_tag(cpu, cycles);
        }
        if (cpu_is_68030_family(cpu))
            return 3 + ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
        /* [UM00 Table 8-4]: Dn,<M> は B/W が 8+EA、Long が 12+EA */
        return ((size == 4) ? ADD_DN_MEM_L_BASE_CYCLES : ADD_DN_MEM_BASE_CYCLES) +
               ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
    }
}

/*
 * EOR: ライン B の opmode 4-6 専用。常に Dn → EA 方向のみ。
 * XOR 演算で、暗号化・チェックサム・ビットフリップに使われる。
 */
static int op_eor(mc68k_state_t *cpu, uint16_t opcode) {
    int dn = (opcode >> 9) & 7;
    int opmode = (opcode >> 6) & 7; /* 4=B, 5=W, 6=L */
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg = opcode & 7;
    int size = g_sz[opmode & 3];
    uint32_t sm = size_mask(size);

    uint32_t rmw_addr;
    uint32_t dst = ea_rmw_read(cpu, ea_mode, ea_reg, size, &rmw_addr, false);
    if (cpu->exec_state != CPU_NORMAL)
        return 4;
    uint32_t res = (dst & sm) ^ (cpu->d[dn] & sm);
    ccr_logic(cpu, res, size);
    ea_rmw_write(cpu, ea_mode, ea_reg, size, rmw_addr, res, false);
    if (cpu_is_68040(cpu)) {
        int cycles = logic40_read_cycles(&cpu->last_ea);
        if (cycles >= 0)
            return cpu_040_tag(cpu, cycles);
    }
    /* [UM00 Table 8-4] / [DS10 Table 7-9]: Dn,Dn の Long は68000=8、68010=6
     * （CYC_EOR_L_DNDN）。Dn,<M> は B/W が 8+EA、Long が 12+EA（68010でも同値） */
    if (cpu_is_68030_family(cpu))
        return ea_mode == 0 ? 2 : 3 + ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
    if (ea_mode == 0)
        return (size == 4) ? CYC(cpu, CYC_EOR_L_DNDN) : 4;
    return ((size == 4) ? ADD_DN_MEM_L_BASE_CYCLES : ADD_DN_MEM_BASE_CYCLES) +
           ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
}

/* ================================================================== */
/* ANDI / ORI / EORI（即値論理演算・CCR/SR 操作を含む）               */
/* ================================================================== */

/*
 * 即値論理演算の共通パターン: opcode の EA フィールドが 0x3C のとき
 * (mode 7, reg 4 = 即値) は CCR または SR を対象とする特殊形式。
 *
 * ANDI/ORI/EORI #imm, CCR は CCR の下位 5bit を操作する。
 * ANDI/ORI/EORI #imm, SR は SR 全体（スーパーバイザ特権命令）。
 * ともに X68000 の割り込ルーティンやシステム初期化コードに登場する。
 */
static int op_andi(mc68k_state_t *cpu, uint16_t opcode) {
    int sc = (opcode >> 6) & 3;
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg = opcode & 7;

    if (sc == 3) {
        cpu->exec_state = CPU_EXCEPTION;
        return 4;
    }

    if (ea_mode == 7 && ea_reg == 4) {
        if (sc != 0 && !(cpu->sr & SR_S))
            return cpu_privilege_violation(cpu); /* #imm,SR は特権 */
        uint16_t imm = cpu_fetch_word(cpu);
        /* 拡張語の補充 fault 後は SR/CCR を更新しない（積まれる SR を保つ） */
        if (cpu->exec_state != CPU_NORMAL)
            return 4;
        if (sc == 0) {
            /* ANDI #imm, CCR（非特権） */
            cpu->sr = (cpu->sr & 0xFF00) | ((cpu->sr & 0xFF) & (uint16_t)(imm & 0xFF));
            cpu->prefetch_flush = true; /* MC68000 は CCR 即値でもキューを再充填する */
        } else {
            /* ANDI #imm, SR */
            cpu_set_sr(cpu, cpu->sr & imm);
            cpu->flow_changed = true;
        }
        /* [DS10 Table 7-21]: ANDI to CCR/SR は68010=16（CYC_ANDI_CCRSR） */
        if (cpu_is_68040(cpu))
            return cpu_040_tag(cpu, sc == 0 ? 5 : 17); /* 040UM Table 10-5: CCR / SR */
        return CYC(cpu, CYC_ANDI_CCRSR);
    }

    int size = g_sz[sc];
    uint32_t imm;
    if (size == 4) {
        uint32_t hi = cpu_fetch_word(cpu);
        imm = (hi << 16) | cpu_fetch_word(cpu);
    } else {
        imm = cpu_fetch_word(cpu) & size_mask(size);
    }
    if (cpu->exec_state != CPU_NORMAL)
        return 4;

    uint32_t rmw_addr;
    uint32_t dst = ea_rmw_read(cpu, ea_mode, ea_reg, size, &rmw_addr, false);
    if (cpu->exec_state != CPU_NORMAL)
        return 8;

    uint32_t res = (dst & size_mask(size)) & imm;
    ccr_logic(cpu, res, size);
    ea_rmw_write(cpu, ea_mode, ea_reg, size, rmw_addr, res, false);
    if (cpu_is_68040(cpu)) {
        int cycles = logic40_immediate_rmw_cycles(&cpu->last_ea);
        if (cycles >= 0)
            return cpu_040_tag(cpu, cycles);
    }
    /* [UM00 Table 8-5]: Long は #,Dn=14 / #,M=20（68010でも同値、差分なし） */
    if (cpu_is_68030_family(cpu))
        return (ea_mode == 0 ? 2 : 3) + ea_cycles(cpu, EA_COST_FETCH_IMM, &cpu->last_ea);
    if (ea_mode == 0)
        return size == 4 ? 14 : 8;
    return (size == 4 ? 20 : 12) + ea_cycles(cpu, EA_COST_FETCH_IMM, &cpu->last_ea);
}

static int op_ori(mc68k_state_t *cpu, uint16_t opcode) {
    int sc = (opcode >> 6) & 3;
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg = opcode & 7;

    if (sc == 3) {
        cpu->exec_state = CPU_EXCEPTION;
        return 4;
    }

    if (ea_mode == 7 && ea_reg == 4) {
        if (sc != 0 && !(cpu->sr & SR_S))
            return cpu_privilege_violation(cpu); /* #imm,SR は特権 */
        uint16_t imm = cpu_fetch_word(cpu);
        /* 拡張語の補充 fault 後は SR/CCR を更新しない（積まれる SR を保つ） */
        if (cpu->exec_state != CPU_NORMAL)
            return 4;
        if (sc == 0) {
            cpu->sr |= (uint16_t)(imm & 0x1F); /* ORI #imm, CCR（非特権） */
            cpu->prefetch_flush = true; /* MC68000 は CCR 即値でもキューを再充填する */
        } else {
            cpu_set_sr(cpu, cpu->sr | imm); /* ORI #imm, SR */
            cpu->flow_changed = true;
        }
        /* [DS10 Table 7-21]: ORI to CCR/SR は68010=16（CYC_ORI_CCRSR） */
        if (cpu_is_68040(cpu))
            return cpu_040_tag(cpu, sc == 0 ? 5 : 17); /* 040UM Table 10-5: CCR / SR */
        return CYC(cpu, CYC_ORI_CCRSR);
    }

    int size = g_sz[sc];
    uint32_t imm;
    if (size == 4) {
        uint32_t hi = cpu_fetch_word(cpu);
        imm = (hi << 16) | cpu_fetch_word(cpu);
    } else {
        imm = cpu_fetch_word(cpu) & size_mask(size);
    }
    if (cpu->exec_state != CPU_NORMAL)
        return 4;

    uint32_t rmw_addr;
    uint32_t dst = ea_rmw_read(cpu, ea_mode, ea_reg, size, &rmw_addr, false);
    if (cpu->exec_state != CPU_NORMAL)
        return 8;

    uint32_t res = (dst & size_mask(size)) | imm;
    ccr_logic(cpu, res, size);
    ea_rmw_write(cpu, ea_mode, ea_reg, size, rmw_addr, res, false);
    if (cpu_is_68040(cpu)) {
        int cycles = logic40_immediate_rmw_cycles(&cpu->last_ea);
        if (cycles >= 0)
            return cpu_040_tag(cpu, cycles);
    }
    /* [UM00 Table 8-5] / [DS10 Table 7-11]: #,Dn の Long は68000=16、68010=14
     * （CYC_ORI_L_DN）。#,M は20（68010でも同値、差分なし） */
    if (cpu_is_68030_family(cpu))
        return (ea_mode == 0 ? 2 : 3) + ea_cycles(cpu, EA_COST_FETCH_IMM, &cpu->last_ea);
    if (ea_mode == 0)
        return size == 4 ? CYC(cpu, CYC_ORI_L_DN) : 8;
    return (size == 4 ? 20 : 12) + ea_cycles(cpu, EA_COST_FETCH_IMM, &cpu->last_ea);
}

static int op_eori(mc68k_state_t *cpu, uint16_t opcode) {
    int sc = (opcode >> 6) & 3;
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg = opcode & 7;

    if (sc == 3) {
        cpu->exec_state = CPU_EXCEPTION;
        return 4;
    }

    if (ea_mode == 7 && ea_reg == 4) {
        if (sc != 0 && !(cpu->sr & SR_S))
            return cpu_privilege_violation(cpu); /* #imm,SR は特権 */
        uint16_t imm = cpu_fetch_word(cpu);
        /* 拡張語の補充 fault 後は SR/CCR を更新しない（積まれる SR を保つ） */
        if (cpu->exec_state != CPU_NORMAL)
            return 4;
        if (sc == 0) {
            cpu->sr ^= (uint16_t)(imm & 0x1F); /* EORI #imm, CCR（非特権） */
            cpu->prefetch_flush = true; /* MC68000 は CCR 即値でもキューを再充填する */
        } else {
            cpu_set_sr(cpu, cpu->sr ^ imm); /* EORI #imm, SR */
            cpu->flow_changed = true;
        }
        /* [DS10 Table 7-21]: EORI to CCR/SR は68010=16（CYC_EORI_CCRSR） */
        if (cpu_is_68040(cpu))
            return cpu_040_tag(cpu, sc == 0 ? 5 : 17); /* 040UM Table 10-5: CCR / SR */
        return CYC(cpu, CYC_EORI_CCRSR);
    }

    int size = g_sz[sc];
    uint32_t imm;
    if (size == 4) {
        uint32_t hi = cpu_fetch_word(cpu);
        imm = (hi << 16) | cpu_fetch_word(cpu);
    } else {
        imm = cpu_fetch_word(cpu) & size_mask(size);
    }
    if (cpu->exec_state != CPU_NORMAL)
        return 4;

    uint32_t rmw_addr;
    uint32_t dst = ea_rmw_read(cpu, ea_mode, ea_reg, size, &rmw_addr, false);
    if (cpu->exec_state != CPU_NORMAL)
        return 8;

    uint32_t res = (dst & size_mask(size)) ^ imm;
    ccr_logic(cpu, res, size);
    ea_rmw_write(cpu, ea_mode, ea_reg, size, rmw_addr, res, false);
    if (cpu_is_68040(cpu)) {
        int cycles = logic40_immediate_rmw_cycles(&cpu->last_ea);
        if (cycles >= 0)
            return cpu_040_tag(cpu, cycles);
    }
    /* [UM00 Table 8-5] / [DS10 Table 7-11]: #,Dn の Long は68000=16、68010=14
     * （CYC_EORI_L_DN）。#,M は20（68010でも同値、差分なし） */
    if (cpu_is_68030_family(cpu))
        return (ea_mode == 0 ? 2 : 3) + ea_cycles(cpu, EA_COST_FETCH_IMM, &cpu->last_ea);
    if (ea_mode == 0)
        return size == 4 ? CYC(cpu, CYC_EORI_L_DN) : 8;
    return (size == 4 ? 20 : 12) + ea_cycles(cpu, EA_COST_FETCH_IMM, &cpu->last_ea);
}

/* ================================================================== */
/* ビット操作: BTST / BCHG / BCLR / BSET                              */
/* ================================================================== */

/*
 * do_bit_op: op=0=BTST, 1=BCHG, 2=BCLR, 3=BSET
 * opcode bit 8 = 0 → static（拡張ワードからビット番号取得）
 * opcode bit 8 = 1 → dynamic（Dn からビット番号取得）
 * データレジスタは mod 32、メモリは mod 8。
 */
static int do_bit_op(mc68k_state_t *cpu, uint16_t opcode, int op) {
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg = opcode & 7;
    int is_static = !(opcode & 0x0100);
    int bit_num;

    if (is_static) {
        bit_num = (int)(cpu_fetch_word(cpu) & 0xFF);
        if (cpu->exec_state != CPU_NORMAL)
            return 4;
    } else {
        bit_num = (int)cpu->d[(opcode >> 9) & 7];
    }

    int is_dn = (ea_mode == 0);
    bit_num &= is_dn ? 31 : 7;
    uint32_t mask = 1U << bit_num;

    if (is_dn) {
        uint32_t val = cpu->d[ea_reg];
        cpu->sr &= ~CCR_Z;
        if (!(val & mask))
            cpu->sr |= CCR_Z;
        if (op == 0) {
            if (cpu_is_68040(cpu))
                return cpu_040_tag(cpu, logic40_bit_cycles(&cpu->last_ea, op, is_static));
            if (cpu_is_68030_family(cpu))
                return 4;
            return is_static ? 10 : 6;
        }
        switch (op) {
        case 1:
            cpu->d[ea_reg] ^= mask;
            break; /* BCHG */
        case 2:
            cpu->d[ea_reg] &= ~mask;
            break; /* BCLR */
        case 3:
            cpu->d[ea_reg] |= mask;
            break; /* BSET */
        }
        if (cpu_is_68040(cpu))
            return cpu_040_tag(cpu, logic40_bit_cycles(&cpu->last_ea, op, is_static));
        if (cpu_is_68030_family(cpu))
            return 6;
        /* [UM00 Table 8-17]: レジスタ形式のBCLRのみ2clock重い（10/14、BCHG/BSETは8/12） */
        if (op == 2)
            return is_static ? 14 : 10;
        return is_static ? 12 : 8;
    } else {
        uint32_t rmw_addr = 0;
        /* BTST は読むだけの命令なので RMW 経路を通さない。通すとバスエラー時の
         * SSW に RM が立ち、ゲストが書き込みフォルトと誤認する。 */
        uint32_t val = op == 0 ? ea_read(cpu, ea_mode, ea_reg, 1)
                               : ea_rmw_read(cpu, ea_mode, ea_reg, 1, &rmw_addr, false);
        if (cpu->exec_state != CPU_NORMAL)
            return 4;
        cpu->sr &= ~CCR_Z;
        if (!(val & mask))
            cpu->sr |= CCR_Z;
        if (op == 0) {
            if (cpu_is_68040(cpu)) {
                int cycles = logic40_bit_cycles(&cpu->last_ea, op, is_static);
                if (cycles >= 0)
                    return cpu_040_tag(cpu, cycles);
            }
            if (cpu_is_68030_family(cpu))
                return 4 + ea_cycles(cpu, is_static ? EA_COST_FETCH_IMM : EA_COST_FETCH,
                                     &cpu->last_ea);
            return (is_static ? 8 : 4) +
                   ea_cycles(cpu, is_static ? EA_COST_FETCH_IMM : EA_COST_FETCH, &cpu->last_ea);
        }
        uint32_t res = val;
        switch (op) {
        case 1:
            res ^= mask;
            break;
        case 2:
            res &= ~mask;
            break;
        case 3:
            res |= mask;
            break;
        }
        ea_rmw_write(cpu, ea_mode, ea_reg, 1, rmw_addr, res, false);
        if (cpu->exec_state != CPU_NORMAL)
            return 4;
        if (cpu_is_68040(cpu)) {
            int cycles = logic40_bit_cycles(&cpu->last_ea, op, is_static);
            if (cycles >= 0)
                return cpu_040_tag(cpu, cycles);
        }
        /* [DS10 Table 7-17]: BCLRのメモリ形式のみ68010で+2（CYC_BCLR_MEM_*）。
         * BCHG/BSETは68000/68010で差分なし（12/8のまま）。 */
        if (cpu_is_68030_family(cpu))
            return 6 +
                   ea_cycles(cpu, is_static ? EA_COST_FETCH_IMM : EA_COST_FETCH, &cpu->last_ea);
        if (op == 2)
            return CYC(cpu, is_static ? CYC_BCLR_MEM_STATIC : CYC_BCLR_MEM_DYNAMIC) +
                   ea_cycles(cpu, is_static ? EA_COST_FETCH_IMM : EA_COST_FETCH, &cpu->last_ea);
        return (is_static ? 12 : 8) +
               ea_cycles(cpu, is_static ? EA_COST_FETCH_IMM : EA_COST_FETCH, &cpu->last_ea);
    }
}

static int op_btst(mc68k_state_t *cpu, uint16_t opcode) {
    return do_bit_op(cpu, opcode, 0);
}
static int op_bchg(mc68k_state_t *cpu, uint16_t opcode) {
    return do_bit_op(cpu, opcode, 1);
}
static int op_bclr(mc68k_state_t *cpu, uint16_t opcode) {
    return do_bit_op(cpu, opcode, 2);
}
static int op_bset(mc68k_state_t *cpu, uint16_t opcode) {
    return do_bit_op(cpu, opcode, 3);
}

/* ================================================================== */
/* MOVE CCR: MOVE.W EA, CCR (0x44C0)                                   */
/* MOVE.W CCR, EA (0x42C0、MOVE from CCR) は MC68010 以降の命令のため      */
/* ops_system_ext.c へ移設した。                  */
/* ================================================================== */

static int op_move_to_ccr(mc68k_state_t *cpu, uint16_t opcode) {
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg = opcode & 7;
    uint32_t val = ea_read(cpu, ea_mode, ea_reg, 2);
    if (cpu->exec_state != CPU_NORMAL)
        return 4;
    cpu->sr = (cpu->sr & 0xFF00U) | (uint16_t)(val & 0x001FU);
    if (cpu_is_68040(cpu)) {
        int cycles = logic40_to_ccr_cycles(&cpu->last_ea);
        if (cycles >= 0)
            return cpu_040_tag(cpu, cycles);
    }
    if (cpu_is_68030_family(cpu))
        return ea_mode == 0 ? 4 : 4 + ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
    return ea_mode == 0 ? 12 : 12 + ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
}

/* ================================================================== */
/* テーブル登録                                                        */
/* ================================================================== */

void register_logic_ops(op_handler_t *table) {
    int dn, ea, op, sc;

    /* ---- CLR: 0x4200-0x42BF（ss=00/01/10）---- */
    for (ea = 0; ea < 64; ea++) {
        if (!ea_is_data_alterable(ea))
            continue;
        for (sc = 0; sc < 3; sc++) {
            table[0x4200 | (sc << 6) | ea] = op_clr;
            table[0x4000 | (sc << 6) | ea] = op_negx;
            table[0x4400 | (sc << 6) | ea] = op_neg;
            table[0x4600 | (sc << 6) | ea] = op_not;
            table[0x4A00 | (sc << 6) | ea] = op_tst;
        }
    }

    /* ---- AND（ライン C）、OR（ライン 8）---- */
    for (dn = 0; dn < 8; dn++) {
        for (ea = 0; ea < 64; ea++) {
            uint16_t ca = (uint16_t)(0xC000 | (dn << 9));
            uint16_t oa = (uint16_t)(0x8000 | (dn << 9));
            /* EA → Dn: src は Data カテゴリ（An不可、immediate可） */
            if (ea_is_data(ea)) {
                for (op = 0; op <= 2; op++) {
                    table[ca | (op << 6) | ea] = op_and;
                    table[oa | (op << 6) | ea] = op_or;
                }
            }
            /* op-mode 3: MULU/DIVU（register_arith_ext_ops が上書き） */
            /* Dn → EA: dst は Memory Alterable。ea mode 0(Dn)/1(An) は
             * EXG/ABCD/SBCD 用のビット並びなので、そもそも Memory Alterable
             * 判定で自動的に除外される。 */
            if (ea_is_memory_alterable(ea)) {
                for (op = 4; op <= 6; op++) {
                    table[ca | (op << 6) | ea] = op_and;
                    table[oa | (op << 6) | ea] = op_or;
                }
            }
            /* op-mode 7: MULS/DIVS（register_arith_ext_ops が上書き） */
        }
    }

    /* ---- EOR（ライン B、opmode 4-6、dst は Data Alterable）----
     * EOR は AND/OR と異なり EA→Dn 方向を持たない（常に Dn→EA 単方向）ため、
     * dst には Dn 自身（EOR Dn,Dn）も含む Data Alterable が正しいカテゴリ。 */
    for (dn = 0; dn < 8; dn++) {
        for (ea = 0; ea < 64; ea++) {
            if (!ea_is_data_alterable(ea))
                continue;
            uint16_t b = (uint16_t)(0xB000 | (dn << 9));
            for (op = 4; op <= 6; op++) {
                table[b | (op << 6) | ea] = op_eor;
            }
        }
    }

    /* ---- ORI / ANDI / EORI（ライン 0）---- */
    /*
     * dst は Data Alterable が基本。ただし ea==0x3C（immediate）は
     * #imm,CCR（byte のみ）/ #imm,SR（word のみ）の特殊形式として例外的に許可する
     * （Data Alterable 自体は immediate を含まないので、この場合だけ別途許可）。
     * long（sc=2）には CCR/SR 形式が存在しないため、0x3C は long では登録しない
     * （これにより ORI.L #x,SR が誤って SR 経路に入るバグも解消される）。
     */
    for (ea = 0; ea < 64; ea++) {
        int alt = ea_is_data_alterable(ea);
        int imm_ccrsr = (ea == 0x3C);

        if (alt || imm_ccrsr) {
            /* byte（sc=0） */
            table[0x0000 | ea] = op_ori;
            table[0x0200 | ea] = op_andi;
            table[0x0A00 | ea] = op_eori;
            /* word（sc=1） */
            table[0x0040 | ea] = op_ori;
            table[0x0240 | ea] = op_andi;
            table[0x0A40 | ea] = op_eori;
        }
        if (alt) {
            /* long（sc=2）: immediate/CCR/SR 特殊形式は対象外 */
            table[0x0080 | ea] = op_ori;
            table[0x0280 | ea] = op_andi;
            table[0x0A80 | ea] = op_eori;
        }
    }

    /* ---- ビット操作: BTST/BCHG/BCLR/BSET ---- */
    /* BTST: src は Data カテゴリ（An不可、immediateは可。BTST Dn,#imm は
     * test_bit_dynamic_immediate で実機相当の正当な挙動として検証済み）。
     * BCHG/BCLR/BSET: dst は Data Alterable（書込あり、immediate不可）。 */
    /* static forms (0x0800-0x08FF): bits 11-9=100, bit 8=0 */
    for (ea = 0; ea < 64; ea++) {
        if (ea_is_data(ea)) {
            table[0x0800 | ea] = op_btst;
        }
        if (ea_is_data_alterable(ea)) {
            table[0x0840 | ea] = op_bchg;
            table[0x0880 | ea] = op_bclr;
            table[0x08C0 | ea] = op_bset;
        }
    }
    /* dynamic forms: bit 8=1, bits 11-9=Dn */
    for (dn = 0; dn < 8; dn++) {
        for (ea = 0; ea < 64; ea++) {
            uint16_t bd = (uint16_t)(dn << 9);
            if (ea_is_data(ea)) {
                table[0x0100 | bd | ea] = op_btst;
            }
            if (ea_is_data_alterable(ea)) {
                table[0x0140 | bd | ea] = op_bchg;
                table[0x0180 | bd | ea] = op_bclr;
                table[0x01C0 | bd | ea] = op_bset;
            }
        }
    }

    /* ---- MOVE to CCR: 0x44C0 (EA→CCR, src=Data)。
     * MOVE from CCR (0x42C0) は ops_system_ext.c 側で登録する（68010+ のみ）。---- */
    for (ea = 0; ea < 64; ea++) {
        if (ea_is_data(ea)) {
            table[0x44C0 | ea] = op_move_to_ccr;
        }
    }
}
