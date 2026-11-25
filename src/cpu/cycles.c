/*
 * cycles.c — CPUモデル別の命令サイクルタイミング。
 */
#include "cpu_internal.h"
#include <stdio.h>
#include <stdlib.h>

/* g_cycles[プロファイル][クラス]。CYC_INHERIT は親プロファイル
 * （CYC_PROF_68000 → 68010 → 68020 → 68030 の順）の値をそのまま使う。
 *
 * 68000 行は現行コードのリテラルをそのまま書き写す（§9.1 の不変保証の起点）。
 * 68010 行は [DS10 Table 7-18]。68020 行は [UM20 §8.2.15] Cache Case。
 * 68030 行は [UM30 §11.6.15] I-Cache Case。DBcc は (r/p/w)=(0/0/0) なので
 * 020 と完全一致し全行 CYC_INHERIT（§5.4）。 */
/* 68010 固有のクラスは
 * §10 Phase 1b の方針どおり CYC_INHERIT を使わない。68020/68030 行は
 * 「命令が68000にも存在するなら68000行と同じ値」「68000に存在しないなら
 * Phase 1b 直前の実際の挙動」を明示リテラルで置く（暫定値: Phase 2c/3 で要検証）。
 * 68010 行のみ [DS10] の値。CYC_MOVES_L・CYC_MOVE_FROM_CCR_REG・
 * CYC_MOVEC_SRC_TO_REG・CYC_RTE_LONG_NORETRY・CYC_EXC_MOVEC_ILLEGAL は
 * 68000に存在しない命令なので68000行が CYC_ABSENT（§10 Phase 1b）。
 * CYC_RTE_LONG_RETRY_READ・CYC_RTE_LONG_RETRY_WRITE も同様に68010のみ到達
 * （[DS10 Table 7-21]）。 */
static const int16_t g_cycles[CYC_PROF_COUNT][CYC_CLASS_COUNT] = {
    /*                    DBCC_BRANCH  DBCC_EXPIRED  DBCC_CC_TRUE
     *                    ADDA_L_EA_REG  SUBA_L_EA_REG  ADD_L_EA_REG  AND_L_EA_REG  OR_L_EA_REG
     *                    SUB_L_EA_REG  EOR_L_DNDN  DIVS  DIVU  MULS  MULU
     *                    ADDI_L_DN  SUBI_L_DN  EORI_L_DN  ORI_L_DN  CMPI_L_DN  SUBQ_W_AN
     *                    SCC_TRUE
     *                    BCLR_MEM_DYNAMIC  BCLR_MEM_STATIC
     *                    BCC_BYTE_NOT_TAKEN  BCC_WORD_NOT_TAKEN
     *                    ADDX_L_REGREG  SUBX_L_REGREG
     *                    ANDI_CCRSR  EORI_CCRSR  ORI_CCRSR  CHK_NORMAL  MOVE_FROM_SR_REG
     *                    MOVE_FROM_CCR_REG  MOVE_USP  MOVEC_SRC_TO_REG  MOVES_L
     *                    RESET_INSTR  RTE_SHORT  RTE_LONG_NORETRY  RTE_LONG_RETRY_READ
     *                    RTE_LONG_RETRY_WRITE
     *                    EXC_CHK  EXC_DIVZERO  EXC_ILLEGAL  EXC_INTERRUPT  EXC_MOVEC_ILLEGAL
     *                    EXC_PRIVILEGE  EXC_TRACE  EXC_TRAP  EXC_TRAPV  EXC_BKPT */
    [CYC_PROF_68000] =
        {
            10,         14,         12,         8,          8,          8,          8,
            8,          8,          8,          158,        140,        70,         70,
            16,         16,         16,         16,         14,         8,          6,
            8,          12,         8,          12,         8,          8,          20,
            20,         20,         10,         6,          CYC_ABSENT, 4,          CYC_ABSENT,
            CYC_ABSENT, 132,        20,         CYC_ABSENT, CYC_ABSENT, CYC_ABSENT, 40,
            38,         34,         44,         CYC_ABSENT, 34,         34,         34,
            34,         CYC_ABSENT, CYC_ABSENT, CYC_ABSENT, CYC_ABSENT,
        },
    [CYC_PROF_68010] =
        {
            CYC_INHERIT, 16, 10, 6,   6,  6,   6,   6,          6,          6,          122,
            108,         42, 40, 14,  14, 14,  14,  12,         4,          4,          10,
            14,          6,  10, 6,   6,  16,  16,  16,         8,          4,          4,
            6,           12, 20, 130, 24, 110, 112, 112,        44,         42,         38,
            46,          46, 38, 38,  38, 40,  42,  CYC_ABSENT, CYC_ABSENT, CYC_ABSENT,
        },
    [CYC_PROF_68020] =
        {
            6,  10, 6,  8,  8,  8,  8,  8,  8,   8,  158,        140,        70,         70,
            16, 16, 16, 16, 14, 8,  6,  8,  12,  8,  12,         8,          8,          20,
            20, 20, 10, 6,  6,  4,  10, 16, 132, 20, CYC_ABSENT, CYC_ABSENT, CYC_ABSENT, 40,
            38, 34, 44, 34, 34, 34, 34, 34, 34,  40, 40,         40,
        },
    [CYC_PROF_68030] =
        {
            CYC_INHERIT, CYC_INHERIT, CYC_INHERIT, 8,          8,          8,  8,  8,  8,
            8,           158,         140,         70,         70,         16, 16, 16, 16,
            14,          8,           6,           8,          12,         8,  12, 8,  8,
            12,          12,          12,          8,          6,          6,  4,  10, 16,
            132,         20,          CYC_ABSENT,  CYC_ABSENT, CYC_ABSENT, 40, 38, 18, 23,
            34,          18,          22,          18,         22,         9,  28, 28, 40,
        },
    /* 040 の既存 class はこの行に明示的に固定する。個別 instruction の値は
     * 040UM Table 10-5/10-6 に従い handler 側又はこの行で上書きする。未調査
     * class を CYC_INHERIT にして 030 profile へ暗黙に落とすことはしない。 */
    [CYC_PROF_68040] =
        {
            6,  8,  8,  8,  8,  8,  8,  8,  8,    8,  158,        140,        70,         70,
            16, 16, 16, 16, 14, 8,  6,  8,  12,   8,  12,         8,          8,          20,
            20, 20, 10, 6,  6,  13, 21, 16, 1042, 15, CYC_ABSENT, CYC_ABSENT, CYC_ABSENT, 40,
            38, 32, 44, 34, 18, 22, 32, 38, 9,    28, 28,         40,
        },
    /* 060 の命令時間は mc68k_execute() が cyc060_instruction()（timing_060.c）で決める。
     * この行は handler が参照する class の値を [060UM §10] の値に固定し、040 以前の行へ
     * 暗黙に落とさないために全 class を明示する。 */
    [CYC_PROF_68060] =
        {
            [CYC_DBCC_BRANCH] = 3,
            [CYC_DBCC_EXPIRED] = 8,
            [CYC_DBCC_CC_TRUE] = 8,
            [CYC_ADDA_L_EA_REG] = 1,
            [CYC_SUBA_L_EA_REG] = 1,
            [CYC_ADD_L_EA_REG] = 1,
            [CYC_AND_L_EA_REG] = 1,
            [CYC_OR_L_EA_REG] = 1,
            [CYC_SUB_L_EA_REG] = 1,
            [CYC_EOR_L_DNDN] = 1,
            [CYC_DIVS] = 22,
            [CYC_DIVU] = 22,
            [CYC_MULS] = 2,
            [CYC_MULU] = 2,
            [CYC_ADDI_L_DN] = 1,
            [CYC_SUBI_L_DN] = 1,
            [CYC_EORI_L_DN] = 1,
            [CYC_ORI_L_DN] = 1,
            [CYC_CMPI_L_DN] = 1,
            [CYC_SUBQ_W_AN] = 1,
            [CYC_SCC_TRUE] = 1,
            [CYC_BCLR_MEM_DYNAMIC] = 1,
            [CYC_BCLR_MEM_STATIC] = 1,
            [CYC_BCC_BYTE_NOT_TAKEN] = 1,
            [CYC_BCC_WORD_NOT_TAKEN] = 1,
            [CYC_ADDX_L_REGREG] = 1,
            [CYC_SUBX_L_REGREG] = 1,
            [CYC_ANDI_CCRSR] = 12,
            [CYC_EORI_CCRSR] = 12,
            [CYC_ORI_CCRSR] = 5,
            [CYC_CHK_NORMAL] = 2,
            [CYC_MOVE_FROM_SR_REG] = 1,
            [CYC_MOVE_FROM_CCR_REG] = 1,
            [CYC_MOVE_USP] = 1,
            [CYC_MOVEC_SRC_TO_REG] = 11,
            [CYC_MOVES_L] = 1,
            [CYC_RESET_INSTR] = 520,
            [CYC_RTE_SHORT] = 17,
            [CYC_RTE_LONG_NORETRY] = 17,
            [CYC_RTE_LONG_RETRY_READ] = CYC_ABSENT,
            [CYC_RTE_LONG_RETRY_WRITE] = CYC_ABSENT,
            [CYC_EXC_CHK] = 20,
            [CYC_EXC_DIVZERO] = 20,
            [CYC_EXC_ILLEGAL] = 19,
            [CYC_EXC_INTERRUPT] = 23,
            [CYC_EXC_MOVEC_ILLEGAL] = 19,
            [CYC_EXC_PRIVILEGE] = 19,
            [CYC_EXC_TRACE] = 19,
            [CYC_EXC_TRAP] = 19,
            [CYC_EXC_TRAPV] = 19,
            [CYC_EXC_BKPT] = 19,
            [CYC_CHK_EXCEPTION_DN] = 20,
            [CYC_CHK_EXCEPTION_EA] = 20,
            [CYC_CHK2_EXCEPTION] = 20,
        },
};

/* EA kind 値は one-biased で格納する。zero は CYC_INHERIT とし、GNU range
 * initializer なしでも全zeroの既定 initializer を継承として扱えるようにする。
 * root profile の zero は EA_COST_NONE である。 */
static const uint8_t g_ea_kind[CYC_PROF_COUNT][CYC_CLASS_COUNT] = {
    [CYC_PROF_68000] =
        {
            [CYC_MOVE_FROM_SR_REG] = EA_COST_FETCH + 1,
        },
    [CYC_PROF_68010] =
        {
            /* DS10 Table 7-4 の No Fetch column。 */
            [CYC_MOVE_FROM_SR_REG] = EA_COST_CALC + 1,
            [CYC_MOVE_FROM_CCR_REG] = EA_COST_CALC + 1,
            [CYC_MOVES_L] = EA_COST_CALC + 1,
        },
    [CYC_PROF_68020] =
        {
            /* UM20 の MOVES table は単独で完結する。 */
            [CYC_MOVES_L] = EA_COST_NONE + 1,
        },
    [CYC_PROF_68030] =
        {
            /* UM30 は MOVE family の base に source FEA を加算する。 */
            [CYC_MOVES_L] = EA_COST_FETCH + 1,
            [CYC_CHK_EXCEPTION_EA] = EA_COST_FETCH + 1,
            [CYC_CHK2_EXCEPTION] = EA_COST_FETCH_IMM + 1,
        },
    [CYC_PROF_68040] =
        {
            /* 040 profile は 030 行を継承しない。040UM の単一値にまだ縮約
             * できない class は EA contribution を持たない値として明示する。 */
            [CYC_MOVE_FROM_SR_REG] = EA_COST_NONE + 1,
            [CYC_MOVE_FROM_CCR_REG] = EA_COST_NONE + 1,
            [CYC_MOVES_L] = EA_COST_NONE + 1,
            [CYC_CHK_EXCEPTION_EA] = EA_COST_NONE + 1,
            [CYC_CHK2_EXCEPTION] = EA_COST_NONE + 1,
        },
    [CYC_PROF_68060] =
        {
            /* EA の計算時間は cyc060_instruction() が t060_ea_extra で加算する。
             * 表の class 値へ EA contribution を重ねない。 */
            [CYC_MOVE_FROM_SR_REG] = EA_COST_NONE + 1,
            [CYC_MOVE_FROM_CCR_REG] = EA_COST_NONE + 1,
            [CYC_MOVES_L] = EA_COST_NONE + 1,
            [CYC_CHK_EXCEPTION_EA] = EA_COST_NONE + 1,
            [CYC_CHK2_EXCEPTION] = EA_COST_NONE + 1,
        },
};

/* テスト専用（T1-8）: g_cycles の生エントリをそのまま返す。cycles.h 参照。 */
int16_t cyc_raw_entry(cyc_profile_t prof, cyc_class_t cls) {
    return g_cycles[(int)prof][(int)cls];
}

static cyc_profile_t cyc_profile_for_model(mc68k_model_t model) {
    switch (model) {
    case MC68K_MODEL_68000:
        return CYC_PROF_68000;
    case MC68K_MODEL_68010:
        return CYC_PROF_68010;
    case MC68K_MODEL_68020:
        return CYC_PROF_68020;
    case MC68K_MODEL_68EC030:
    case MC68K_MODEL_68030:
        /* MC68K_MODEL_68EC030 と MC68K_MODEL_68030 は CYC_PROF_68030 を共有する
         * （§5.2。一次資料の直接の裏付けは無く、PMMU差のみという構成上の仮定）。 */
        return CYC_PROF_68030;
    case MC68K_MODEL_68040:
        return CYC_PROF_68040;
    case MC68K_MODEL_68060:
        return CYC_PROF_68060;
    default:
        return CYC_PROF_68000;
    }
}

ea_cost_kind_t cyc_ea_kind(const mc68k_state_t *cpu, cyc_class_t cls) {
    int p = (int)cyc_profile_for_model(cpu->model);
    while (p > 0 && g_ea_kind[p][cls] == 0)
        --p;
    uint8_t raw = g_ea_kind[p][cls];
    return raw == 0 ? EA_COST_NONE : (ea_cost_kind_t)(raw - 1);
}

enum {
    EA_DN,
    EA_AN,
    EA_AI,
    EA_PI,
    EA_PD,
    EA_DI,
    EA_IX,
    EA_AW,
    EA_AL,
    EA_PCDI,
    EA_PCIX,
    EA_IMM,
    EA_ROWS
};

static int ea_row(const ea_desc_t *ea) {
    static const int mode_row[7] = {EA_DN, EA_AN, EA_AI, EA_PI, EA_PD, EA_DI, EA_IX};
    if (ea->mode < 7)
        return mode_row[ea->mode];
    switch (ea->reg) {
    case 0:
        return EA_AW;
    case 1:
        return EA_AL;
    case 2:
        return EA_PCDI;
    case 3:
        return EA_PCIX;
    case 4:
        return EA_IMM;
    default:
        return -1;
    }
}

static int ea_full_cycles(cyc_profile_t prof, ea_cost_kind_t kind, const ea_desc_t *ea) {
    /* Full-format direct form は通常の An/PC+index と base/index (B) suppress を
     * 区別する。UM20 8.2.1/3/5、UM30 11.6.1/3/5。 */
    static const uint8_t f20_direct[2][3] = {{7, 7, 13}, {7, 9, 13}};
    static const uint8_t f20_indirect[3][3] = {{12, 14, 14}, {14, 16, 16}, {18, 20, 20}};
    static const uint8_t c20_direct[2][3] = {{6, 6, 12}, {6, 8, 12}};
    static const uint8_t c20_indirect[3][3] = {{11, 13, 13}, {13, 15, 15}, {17, 19, 19}};
    static const uint8_t j20_direct[2][3] = {{6, 6, 12}, {6, 8, 12}};
    static const uint8_t j20_indirect[3][3] = {{11, 13, 13}, {13, 15, 15}, {17, 19, 19}};
    static const uint8_t f30_direct[2][3] = {{6, 6, 12}, {6, 8, 12}};
    static const uint8_t f30_indirect[3][3] = {{10, 12, 12}, {12, 14, 14}, {16, 18, 18}};
    static const uint8_t c30_direct[2][3] = {{6, 6, 12}, {6, 8, 12}};
    static const uint8_t c30_indirect[3][3] = {{10, 12, 12}, {12, 14, 14}, {16, 18, 18}};
    static const uint8_t j30_direct[2][3] = {{6, 6, 12}, {6, 8, 12}};
    static const uint8_t j30_indirect[3][3] = {{10, 12, 12}, {12, 14, 14}, {16, 18, 18}};
    unsigned bd = ea->bd_size > 2 ? 2 : ea->bd_size;
    unsigned od = ea->od_size > 0 ? ea->od_size - 1 : 0;
    if (ea->iis == 4) /* reserved IIS encoding; full_ext_ea rejects it */
        return 0;
    bool fetch = kind == EA_COST_FETCH || kind == EA_COST_FETCH_IMM;
    const uint8_t (*direct)[3] = NULL;
    const uint8_t (*indirect)[3] = NULL;
    if (prof == CYC_PROF_68020) {
        direct = kind == EA_COST_JUMP ? j20_direct : fetch ? f20_direct : c20_direct;
        indirect = kind == EA_COST_JUMP ? j20_indirect : fetch ? f20_indirect : c20_indirect;
    } else if (prof == CYC_PROF_68030) {
        direct = kind == EA_COST_JUMP ? j30_direct : fetch ? f30_direct : c30_direct;
        indirect = kind == EA_COST_JUMP ? j30_indirect : fetch ? f30_indirect : c30_indirect;
    }
    if (!direct)
        return 0;
    int n = ea->iis == 0 ? direct[ea->base_suppressed != 0][bd] : indirect[bd][od];
    if (kind == EA_COST_FETCH_IMM || kind == EA_COST_CALC_IMM)
        n += ea->size == 4 ? 4 : 2;
    return n;
}

int ea_cycles(const mc68k_state_t *cpu, ea_cost_kind_t kind, const ea_desc_t *ea) {
    static const uint8_t fetch00[EA_ROWS][2] = {
        {0, 0},   {0, 0},  {4, 8},   {4, 8},  {6, 10},  {8, 12},
        {10, 14}, {8, 12}, {12, 16}, {8, 12}, {10, 14}, {4, 8},
    };
    static const uint8_t calc10[EA_ROWS][2] = {
        {0, 0}, {0, 0}, {2, 2}, {4, 4}, {4, 4}, {4, 4},
        {8, 8}, {4, 4}, {8, 8}, {8, 8}, {8, 8}, {0, 0},
    };
    static const uint8_t fetch20[EA_ROWS][2] = {
        {0, 0}, {0, 0}, {4, 4}, {4, 4}, {5, 5}, {5, 5},
        {7, 7}, {4, 4}, {4, 4}, {5, 5}, {7, 7}, {2, 4},
    };
    static const uint8_t calc20[EA_ROWS][2] = {
        {0, 0}, {0, 0}, {2, 2}, {2, 2}, {2, 2}, {2, 2},
        {4, 4}, {2, 2}, {4, 4}, {2, 2}, {4, 4}, {0, 0},
    };
    static const uint8_t fetch_imm20[EA_ROWS][2] = {
        {2, 4},  {0, 0}, {4, 4}, {6, 8}, {5, 7},  {5, 7},
        {9, 11}, {5, 7}, {6, 8}, {5, 7}, {9, 11}, {4, 8},
    };
    static const uint8_t calc_imm20[EA_ROWS][2] = {
        {2, 4}, {0, 0}, {2, 4}, {4, 6}, {2, 4}, {4, 6},
        {6, 8}, {4, 6}, {6, 8}, {4, 6}, {6, 8}, {4, 8},
    };
    static const uint8_t fetch30[EA_ROWS][2] = {
        {0, 0}, {0, 0}, {3, 3}, {3, 3}, {4, 4}, {4, 4},
        {6, 6}, {4, 4}, {4, 4}, {4, 4}, {6, 6}, {2, 4},
    };
    static const uint8_t calc30[EA_ROWS][2] = {
        {0, 0}, {0, 0}, {2, 2}, {2, 2}, {2, 2}, {2, 2},
        {4, 4}, {2, 2}, {4, 4}, {2, 2}, {4, 4}, {0, 0},
    };
    static const uint8_t fetch_imm30[EA_ROWS][2] = {
        {2, 4},  {0, 0}, {3, 4}, {5, 7}, {4, 4},  {4, 6},
        {8, 10}, {6, 8}, {6, 8}, {4, 6}, {8, 10}, {6, 8},
    };
    static const uint8_t calc_imm30[EA_ROWS][2] = {
        {2, 4}, {0, 0}, {2, 4}, {4, 6}, {2, 4}, {4, 6},
        {6, 8}, {4, 6}, {6, 8}, {4, 6}, {6, 8}, {6, 8},
    };
    static const uint8_t jump00[EA_ROWS] = {0, 0, 0, 0, 0, 2, 6, 2, 4, 2, 6, 0};
    static const uint8_t jump20[EA_ROWS] = {0, 0, 2, 0, 0, 4, 6, 2, 2, 4, 6, 0};
    static const uint8_t jump30[EA_ROWS] = {0, 0, 2, 0, 0, 4, 6, 2, 2, 4, 6, 0};
    if (!ea || kind == EA_COST_NONE)
        return 0;
    cyc_profile_t prof = cyc_profile_for_model(cpu->model);
    if (prof == CYC_PROF_68060)
        return 0; /* EA 時間は cyc060_instruction() が決める */
    if (ea->ext_form == EA_EXT_FULL)
        return ea_full_cycles(prof, kind, ea);
    int row = ea_row(ea);
    if (row < 0)
        return 0;
    int col = ea->size == 4;
    if (kind == EA_COST_JUMP)
        return prof == CYC_PROF_68030   ? jump30[row]
               : prof == CYC_PROF_68020 ? jump20[row]
                                        : jump00[row];
    if (prof == CYC_PROF_68000)
        return fetch00[row][col];
    if (prof == CYC_PROF_68010)
        return (kind == EA_COST_CALC || kind == EA_COST_CALC_IMM) ? calc10[row][col]
                                                                  : fetch00[row][col];
    if (prof == CYC_PROF_68020) {
        if (kind == EA_COST_FETCH_IMM)
            return fetch_imm20[row][col];
        if (kind == EA_COST_CALC_IMM)
            return calc_imm20[row][col];
        return kind == EA_COST_CALC ? calc20[row][col] : fetch20[row][col];
    }
    if (kind == EA_COST_FETCH_IMM)
        return fetch_imm30[row][col];
    if (kind == EA_COST_CALC_IMM)
        return calc_imm30[row][col];
    return kind == EA_COST_CALC ? calc30[row][col] : fetch30[row][col];
}

int ea_cycles_immediate_word(const mc68k_state_t *cpu, ea_cost_kind_t kind,
                             const ea_desc_t *ea) {
    if (!ea)
        return 0;
    ea_desc_t word_ea = *ea;
    word_ea.size = 2;
    return ea_cycles(cpu, kind, &word_ea);
}

void cyc_resolve(mc68k_state_t *cpu) {
    cyc_profile_t prof = cyc_profile_for_model(cpu->model);

    for (int cls = 0; cls < CYC_CLASS_COUNT; cls++) {
        int16_t v = CYC_ABSENT;
        int p = (int)prof;
        while (p >= 0) {
            int16_t raw = g_cycles[p][cls];
            if (raw != CYC_INHERIT) {
                v = raw;
                break;
            }
            p--;
        }
        if (p < 0) {
            /* CYC_PROF_68000 行まで CYC_INHERIT のまま遡った。表の記入漏れという
             * 静的な欠陥であり、モデルに依存しないので NDEBUG でも検出する
             * （§5.4「assert を唯一の安全機構にしない」）。 */
            fprintf(stderr,
                    "cyc_resolve: class %d has no root (CYC_PROF_68000) value — "
                    "table bug in g_cycles\n",
                    cls);
            abort();
        }
        cpu->cyc[cls] = v;
    }
}
