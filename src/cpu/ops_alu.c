/*
 * ops_alu.c — ADD/SUB/CMP 系命令ハンドラ実装
 *
 * ADD/SUB/CMP（レジスタ・イミディエイト・クイック形式）をまとめる。
 */
#include "cpu_internal.h"
#include <stdint.h>

static int alu40_cmpi_cycles(const ea_desc_t *ea);

/* ================================================================== */
/* CCR 更新: ADD / SUB / CMP                                           */
/* ================================================================== */

/*
 * ADD CCR 更新（result = src + dst）:
 *   C,X : 符号なし加算の桁あふれ（キャリー）
 *   V   : 同符号の加算で結果の符号が変わった（符号付きオーバーフロー）
 *   N   : 結果の MSB が 1
 *   Z   : 結果が 0
 *
 * MC68000 の X フラグは多倍長演算（ADDX 等）のキャリー伝搬に使う。
 * 通常の ADD では C と X が同時に更新される。
 */
static void ccr_add(mc68k_state_t *cpu, uint32_t src, uint32_t dst, uint32_t result, int size) {
    uint32_t sm = size_mask(size);
    uint32_t sb = sign_bit(size);
    uint32_t res = result & sm;
    uint32_t s = src & sm;
    uint32_t d = dst & sm;

    cpu->sr &= ~(CCR_X | CCR_N | CCR_Z | CCR_V | CCR_C);
    if (!res)
        cpu->sr |= CCR_Z;
    if (res & sb)
        cpu->sr |= CCR_N;

    /* キャリー: 32bit は uint64_t で判定 */
    if (size == 4) {
        if ((uint64_t)s + d > 0xFFFFFFFFUL)
            cpu->sr |= CCR_C | CCR_X;
    } else {
        if (s + d > sm)
            cpu->sr |= CCR_C | CCR_X;
    }

    /* オーバーフロー: ~(src ^ dst) & (dst ^ result) の MSB が 1 */
    if ((~(s ^ d) & (d ^ res)) & sb)
        cpu->sr |= CCR_V;
}

/*
 * SUB CCR 更新（result = dst - src）:
 *   C,X : 符号なし減算のボロー（src > dst のとき）
 *   V   : src と dst の符号が異なり、結果の符号が dst と異なる
 *   N,Z : ADD と同様
 */
static void ccr_sub(mc68k_state_t *cpu, uint32_t src, uint32_t dst, uint32_t result, int size) {
    uint32_t sm = size_mask(size);
    uint32_t sb = sign_bit(size);
    uint32_t res = result & sm;
    uint32_t s = src & sm;
    uint32_t d = dst & sm;

    cpu->sr &= ~(CCR_X | CCR_N | CCR_Z | CCR_V | CCR_C);
    if (!res)
        cpu->sr |= CCR_Z;
    if (res & sb)
        cpu->sr |= CCR_N;
    if (s > d)
        cpu->sr |= CCR_C | CCR_X; /* ボロー */
    if (((s ^ d) & (d ^ res)) & sb)
        cpu->sr |= CCR_V;
}

/* 040UM 表 10-6 の ADD/SUB <ea>,Dn を #89 の公称値規則で還元する。
 * <ea> 計算 + 実行基準値 + 表 10-2 のソースフェッチアクセスとする。表の実行先行分は
 * 意図的に単一命令値へ含めない。インデックス抑制フル拡張には独立した表 10-6 行がないため、
 * 分類されるまで既存フォールバックを維持する。 */
static int alu40_read_dn_cycles(const ea_desc_t *ea) {
    switch (ea->mode) {
    case 0:
    case 1:
        return 2; /* 1 calculate + 1 execute */
    case 2:
        return 3; /* (An): 1 + 1 + fetch */
    case 3:
    case 4:
    case 5:
        return 4; /* (An)+, -(An), d16(An): 1 + 2 + fetch */
    case 6:
        if (ea->ext_form != EA_EXT_FULL)
            return 7; /* brief: 3 + 3 + fetch */
        if (ea->index_suppressed && ea->iis == 0)
            return -1;
        switch (ea->iis) {
        case 0:
            return ea->bd_size == 0 ? 12 : 14; /* (BR,Xn) / (bd,BR,Xn) */
        case 1:
        case 5:
            return 21; /* pre/post, null OD */
        case 2:
        case 3:
        case 6:
        case 7:
            return 24; /* pre/post, word/long OD */
        default:
            return -1;
        }
    case 7:
        if (ea->reg == 0 || ea->reg == 1)
            return 3; /* absolute */
        if (ea->reg == 2)
            return 5; /* d16(PC) */
        if (ea->reg == 3) {
            if (ea->ext_form != EA_EXT_FULL)
                return 10; /* brief PC index: 5 + 4 + fetch */
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
            return 2; /* immediate */
        return -1;
    default:
        return -1;
    }
}

/* 040UM 表 10-6 の ADDA/SUBA <ea>,An。alu40_read_dn_cycles() と同じ #89 還元を使う。
 * 表には有効インデックス抑制フル拡張行がなく、意図的にこのヘルパーの対象外とする。 */
static int alu40_adda_cycles(const ea_desc_t *ea) {
    switch (ea->mode) {
    case 0:
        return 3; /* Dn: 1 calculate + 2 execute */
    case 1:
        return 2; /* An: 1 + 1 */
    case 2:
        return 4; /* 1 + 2 + fetch */
    case 3:
    case 4:
    case 5:
        return 5; /* 2 + 2 + fetch */
    case 6:
        if (ea->ext_form != EA_EXT_FULL)
            return 10; /* brief: 4 + 5 + fetch */
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
            return 25;
        default:
            return -1;
        }
    case 7:
        if (ea->reg == 0 || ea->reg == 1)
            return 4; /* absolute */
        if (ea->reg == 2)
            return 6; /* d16(PC) */
        if (ea->reg == 3) {
            if (ea->ext_form != EA_EXT_FULL)
                return 11; /* brief PC index: 5 + 5 + fetch */
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
                return 25;
            default:
                return -1;
            }
        }
        if (ea->reg == 4)
            return 2; /* immediate */
        return -1;
    default:
        return -1;
    }
}

/* 040UM Table 10-6, ADDQ/SUBQ <ea>.  PC-relative and immediate are not
 * alterable destinations.  As elsewhere, IS=1 full format is left pending
 * because the table prints only active-index forms. */
static int alu40_addq_cycles(const ea_desc_t *ea) {
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
            return ea->bd_size == 0 ? 14 : 16;
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
        return -1;
    default:
        return -1;
    }
}

/*
 * CMP CCR 更新: SUB と同じだが X フラグは変化しない。
 * CMP はステータスフラグの確認のみで結果を格納しない。
 */
static void ccr_cmp(mc68k_state_t *cpu, uint32_t src, uint32_t dst, uint32_t result, int size) {
    uint16_t x_save = cpu->sr & CCR_X;
    ccr_sub(cpu, src, dst, result, size);
    cpu->sr = (cpu->sr & ~CCR_X) | x_save;
}

/* ================================================================== */
/* ADD / ADDA（ライン D: 0xD000-0xDFFF）                              */
/* ================================================================== */

/*
 * op-mode フィールド（bits 8-6）の意味:
 *   000: ADD.B EA, Dn   ← EA をバイトで読み Dn に加算
 *   001: ADD.W EA, Dn
 *   010: ADD.L EA, Dn
 *   011: ADDA.W EA, An  ← ワード値を符号拡張して An に加算（CCR 更新なし）
 *   100: ADD.B Dn, EA   → Dn をバイトで EA に加算
 *   101: ADD.W Dn, EA
 *   110: ADD.L Dn, EA
 *   111: ADDA.L EA, An
 */
static int op_add(mc68k_state_t *cpu, uint16_t opcode) {
    int dn = (opcode >> 9) & 7;
    int opmode = (opcode >> 6) & 7;
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg = (opcode >> 0) & 7;

    static const int sz[8] = {1, 2, 4, 2, 1, 2, 4, 4};
    int size = sz[opmode];

    if (opmode == 3 || opmode == 7) {
        /* ADDA: アドレスレジスタへの加算（CCR 更新なし）
         * ADDA.W はワードを 32bit に符号拡張してから加算する */
        uint32_t src = ea_read(cpu, ea_mode, ea_reg, size);
        if (cpu->exec_state != CPU_NORMAL)
            return 8;
        if (size == 2)
            src = (uint32_t)(int32_t)(int16_t)(uint16_t)src;
        cpu->a[dn] += src;
        if (cpu_is_68040(cpu)) {
            int cycles = alu40_adda_cycles(&cpu->last_ea);
            if (cycles >= 0)
                return cpu_040_tag(cpu, cycles);
        }
        if (cpu_is_68020(cpu))
            return 2 + ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
        if (cpu_is_68030_family(cpu))
            return (size == 2 ? 4 : 2) + ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
        /* [UM00 Table 8-4]** / [DS10 Table 7-9]: ADDA.W は 8+EA、ADDA.L は 6+EA。
         * ただし EA が Dn/An 直接・即値の Long は 68000=8、68010=6（CYC_ADDA_L_EA_REG）に
         * 増える（即値は EA 時間を別途加算する） */
        int base = size == 2 ? ADDA_W_BASE_CYCLES : ADD_L_EA_BASE_CYCLES;
        if (size == 4 && ea_is_direct_or_imm(ea_mode, ea_reg))
            base = CYC(cpu, CYC_ADDA_L_EA_REG);
        return base + ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);

    } else if (opmode <= 2) {
        /* ADD EA, Dn: Dn ← Dn + EA */
        uint32_t src = ea_read(cpu, ea_mode, ea_reg, size);
        if (cpu->exec_state != CPU_NORMAL)
            return 4;
        uint32_t dst = cpu->d[dn];
        uint32_t res = (src + dst) & size_mask(size);
        ccr_add(cpu, src, dst, res, size);
        uint32_t mask = size_mask(size);
        cpu->d[dn] = (cpu->d[dn] & ~mask) | res;
        if (cpu_is_68040(cpu)) {
            int cycles = alu40_read_dn_cycles(&cpu->last_ea);
            if (cycles >= 0)
                return cpu_040_tag(cpu, cycles);
        }
        if (cpu_is_68020(cpu))
            return 2 + ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
        if (cpu_is_68030_family(cpu))
            return 2 + ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
        if (cpu_is_68040(cpu) && ea_mode <= 1)
            return cpu_040_tag(cpu, 2); /* 040UM Table 10-6: ADD Dn/An,Dn */
        /* [UM00 Table 8-4]** / [DS10 Table 7-9]: ADD.B/W は 4+EA、ADD.L は 6+EA。
         * ただし EA が Dn/An 直接・即値の Long は 68000=8、68010=6（CYC_ADD_L_EA_REG）に
         * 増える（即値は EA 時間を別途加算する） */
        int base = size == 4 ? ADD_L_EA_BASE_CYCLES : ADD_EA_DN_BASE_CYCLES;
        if (size == 4 && ea_is_direct_or_imm(ea_mode, ea_reg))
            base = CYC(cpu, CYC_ADD_L_EA_REG);
        return base + ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);

    } else {
        /* ADD Dn, EA: EA ← EA + Dn */
        uint32_t src = cpu->d[dn] & size_mask(size);
        uint32_t rmw_addr;
        uint32_t dst = ea_rmw_read(cpu, ea_mode, ea_reg, size, &rmw_addr, false);
        if (cpu->exec_state != CPU_NORMAL)
            return 8;
        uint32_t res = (src + dst) & size_mask(size);
        ccr_add(cpu, src, dst, res, size);
        ea_rmw_write(cpu, ea_mode, ea_reg, size, rmw_addr, res, false);
        if (cpu_is_68040(cpu)) {
            int cycles = alu40_read_dn_cycles(&cpu->last_ea);
            if (cycles >= 0)
                return cpu_040_tag(cpu, cycles);
        }
        if (cpu_is_68020(cpu))
            return 4 + ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
        if (cpu_is_68030_family(cpu))
            return 3 + ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
        /* [UM00 Table 8-4]: ADD Dn,<M> は B/W が 8+EA、Long が 12+EA */
        return ((size == 4) ? ADD_DN_MEM_L_BASE_CYCLES : ADD_DN_MEM_BASE_CYCLES) +
               ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
    }
}

/* ================================================================== */
/* SUB / SUBA（ライン 9: 0x9000-0x9FFF）                              */
/* ================================================================== */

/*
 * op-mode の意味は ADD と同じ構造:
 *   000-010: SUB.{B,W,L} EA, Dn  → Dn ← Dn - EA
 *   011:     SUBA.W EA, An（CCR 更新なし）
 *   100-110: SUB.{B,W,L} Dn, EA  → EA ← EA - Dn
 *   111:     SUBA.L EA, An（CCR 更新なし）
 */
static int op_sub(mc68k_state_t *cpu, uint16_t opcode) {
    int dn = (opcode >> 9) & 7;
    int opmode = (opcode >> 6) & 7;
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg = (opcode >> 0) & 7;

    static const int sz[8] = {1, 2, 4, 2, 1, 2, 4, 4};
    int size = sz[opmode];

    if (opmode == 3 || opmode == 7) {
        /* SUBA */
        uint32_t src = ea_read(cpu, ea_mode, ea_reg, size);
        if (cpu->exec_state != CPU_NORMAL)
            return 8;
        if (size == 2)
            src = (uint32_t)(int32_t)(int16_t)(uint16_t)src;
        cpu->a[dn] -= src;
        if (cpu_is_68040(cpu)) {
            int cycles = alu40_adda_cycles(&cpu->last_ea);
            if (cycles >= 0)
                return cpu_040_tag(cpu, cycles);
        }
        if (cpu_is_68020(cpu))
            return 2 + ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
        if (cpu_is_68030_family(cpu))
            return (size == 2 ? 4 : 2) + ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
        /* [UM00 Table 8-4]** / [DS10 Table 7-9]: SUBA.W は 8+EA、SUBA.L は 6+EA。
         * ただし EA が Dn/An 直接・即値の Long は 68000=8、68010=6（CYC_SUBA_L_EA_REG）に
         * 増える（即値は EA 時間を別途加算する） */
        int base = size == 2 ? ADDA_W_BASE_CYCLES : ADD_L_EA_BASE_CYCLES;
        if (size == 4 && ea_is_direct_or_imm(ea_mode, ea_reg))
            base = CYC(cpu, CYC_SUBA_L_EA_REG);
        return base + ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);

    } else if (opmode <= 2) {
        /* SUB EA, Dn: Dn ← Dn - EA */
        uint32_t src = ea_read(cpu, ea_mode, ea_reg, size);
        if (cpu->exec_state != CPU_NORMAL)
            return 4;
        uint32_t dst = cpu->d[dn];
        uint32_t res = (dst - src) & size_mask(size);
        ccr_sub(cpu, src, dst, res, size);
        uint32_t mask = size_mask(size);
        cpu->d[dn] = (cpu->d[dn] & ~mask) | res;
        if (cpu_is_68040(cpu)) {
            int cycles = alu40_read_dn_cycles(&cpu->last_ea);
            if (cycles >= 0)
                return cpu_040_tag(cpu, cycles);
        }
        if (cpu_is_68020(cpu))
            return 2 + ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
        if (cpu_is_68030_family(cpu))
            return 2 + ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
        if (cpu_is_68040(cpu) && ea_mode <= 1)
            return cpu_040_tag(cpu, 2); /* 040UM Table 10-6: SUB Dn/An,Dn */
        /* [UM00 Table 8-4]** / [DS10 Table 7-9]: SUB.B/W は 4+EA、SUB.L は 6+EA。
         * ただし EA が Dn/An 直接・即値の Long は 68000=8、68010=6（CYC_SUB_L_EA_REG）に
         * 増える（即値は EA 時間を別途加算する） */
        int base = size == 4 ? ADD_L_EA_BASE_CYCLES : ADD_EA_DN_BASE_CYCLES;
        if (size == 4 && ea_is_direct_or_imm(ea_mode, ea_reg))
            base = CYC(cpu, CYC_SUB_L_EA_REG);
        return base + ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);

    } else {
        /* SUB Dn, EA: EA ← EA - Dn */
        uint32_t src = cpu->d[dn] & size_mask(size);
        uint32_t rmw_addr;
        uint32_t dst = ea_rmw_read(cpu, ea_mode, ea_reg, size, &rmw_addr, false);
        if (cpu->exec_state != CPU_NORMAL)
            return 8;
        uint32_t res = (dst - src) & size_mask(size);
        ccr_sub(cpu, src, dst, res, size);
        ea_rmw_write(cpu, ea_mode, ea_reg, size, rmw_addr, res, false);
        if (cpu_is_68040(cpu)) {
            int cycles = alu40_read_dn_cycles(&cpu->last_ea);
            if (cycles >= 0)
                return cpu_040_tag(cpu, cycles);
        }
        if (cpu_is_68020(cpu))
            return 4 + ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
        if (cpu_is_68030_family(cpu))
            return 3 + ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
        /* [UM00 Table 8-4]: SUB Dn,<M> は B/W が 8+EA、Long が 12+EA */
        return ((size == 4) ? ADD_DN_MEM_L_BASE_CYCLES : ADD_DN_MEM_BASE_CYCLES) +
               ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
    }
}

/* ================================================================== */
/* CMP / CMPA（ライン B: 0xB000-0xBFFF、op-mode 0-3, 7）              */
/* ================================================================== */

/*
 * ライン B の op-mode:
 *   000-010: CMP.{B,W,L} EA, Dn  → CCR のみ更新（Dn - EA を計算するが保存しない）
 *   011:     CMPA.W EA, An（ワードを符号拡張してから比較）
 *   100-110: EOR.{B,W,L} Dn, EA  → ops_logic.c で実装
 *   111:     CMPA.L EA, An
 */
static int op_cmp(mc68k_state_t *cpu, uint16_t opcode) {
    int dn = (opcode >> 9) & 7;
    int opmode = (opcode >> 6) & 7;
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg = (opcode >> 0) & 7;

    static const int sz[8] = {1, 2, 4, 2, 1, 2, 4, 4};
    int size = sz[opmode];

    if (opmode == 3 || opmode == 7) {
        /* CMPA: An - EA（CCR 更新のみ、An は変化しない） */
        uint32_t src = ea_read(cpu, ea_mode, ea_reg, size);
        if (cpu->exec_state != CPU_NORMAL)
            return 6;
        if (size == 2)
            src = (uint32_t)(int32_t)(int16_t)(uint16_t)src;
        uint32_t dst = cpu->a[dn];
        uint32_t res = (dst - src) & 0xFFFFFFFF;
        ccr_cmp(cpu, src, dst, res, 4); /* CMPA は常に 32bit 比較 */
        if (cpu_is_68040(cpu) && size == 4) {
            int cycles = alu40_cmpi_cycles(&cpu->last_ea);
            if (cycles >= 0)
                return cpu_040_tag(cpu, cycles);
        }
        if (cpu_is_68020(cpu))
            return 4 + ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
        if (cpu_is_68030_family(cpu))
            return 4 + ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
        return (ea_mode <= 1) ? 6 : 10;
    }

    if (opmode >= 4) {
        /* EOR — ops_logic.c の register_logic_ops が上書きする */
        cpu->exec_state = CPU_EXCEPTION;
        return 4;
    }

    /* CMP EA, Dn: Dn - EA の結果で CCR 更新（Dn は変化しない） */
    uint32_t src = ea_read(cpu, ea_mode, ea_reg, size);
    if (cpu->exec_state != CPU_NORMAL)
        return 4;
    uint32_t dst = cpu->d[dn];
    uint32_t res = (dst - src) & size_mask(size);
    ccr_cmp(cpu, src, dst, res, size);
    if (cpu_is_68040(cpu)) {
        int cycles = alu40_read_dn_cycles(&cpu->last_ea);
        if (cycles >= 0)
            return cpu_040_tag(cpu, cycles);
    }
    if (cpu_is_68020(cpu))
        return 2 + ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
    if (cpu_is_68030_family(cpu))
        return 2 + ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
    /* [UM00 Table 8-4] / [DS10 Table 7-9]: EA=Dn/An の Long CMP は 6（68010でも同値） */
    if (ea_mode <= 1)
        return (size == 4) ? 6 : 4;
    return 8;
}

/* ================================================================== */
/* ADDI / SUBI / CMPI（ライン 0: イミディエイト演算）                 */
/* ================================================================== */

/*
 * ADDI/SUBI/CMPI の opcode 構造:
 *   0000 xxxx | ss | EA
 *   ss: 00=byte, 01=word, 10=long
 * イミディエイト値は opcode の直後に続く:
 *   バイト/ワード: 1 ワード（バイトは下位 8bit を使う）
 *   ロング: 2 ワード（上位 word + 下位 word）
 */

/* イミディエイト値をフェッチして返す */
static uint32_t fetch_immediate(mc68k_state_t *cpu, int size) {
    if (size == 4) {
        uint32_t hi = cpu_fetch_word(cpu);
        uint32_t lo = cpu_fetch_word(cpu);
        return (hi << 16) | lo;
    }
    return cpu_fetch_word(cpu) & size_mask(size);
}

/* 040UM 表 10-6 の ADDI/SUBI 宛先 EA。即値オペランドはこの命令列で表現されるため、
 * RMW 宛先に対する表 10-2 のアクセスだけを加える。 */
static int alu40_immediate_rmw_cycles(const ea_desc_t *ea) {
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

/* 040UM 表 10-6（PDF 310、印刷版 10-19）の CMPI と CMPA.L。この列はフル拡張で
 * ADDI/SUBI と異なる。 */
static int alu40_cmpi_cycles(const ea_desc_t *ea) {
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
            return 4;
        if (ea->reg == 2)
            return 6;
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
                return 19;
            case 2:
            case 3:
            case 6:
            case 7:
                return 21;
            default:
                return -1;
            }
        }
        return -1;
    default:
        return -1;
    }
}

static int op_addi(mc68k_state_t *cpu, uint16_t opcode) {
    static const int sz[3] = {1, 2, 4};
    int sc = (opcode >> 6) & 3;
    if (sc == 3) {
        cpu->exec_state = CPU_EXCEPTION;
        return 4;
    }
    int size = sz[sc];
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg = (opcode >> 0) & 7;

    uint32_t src = fetch_immediate(cpu, size);
    /* 拡張語の補充で fault が確定したら、以降の read・An 更新・CCR 更新を行わない */
    if (cpu->exec_state != CPU_NORMAL)
        return 4;
    uint32_t rmw_addr;
    uint32_t dst = ea_rmw_read(cpu, ea_mode, ea_reg, size, &rmw_addr, false);
    if (cpu->exec_state != CPU_NORMAL)
        return 8;
    uint32_t res = (src + dst) & size_mask(size);
    ccr_add(cpu, src, dst, res, size);
    ea_rmw_write(cpu, ea_mode, ea_reg, size, rmw_addr, res, false);
    if (cpu_is_68040(cpu)) {
        int cycles = alu40_immediate_rmw_cycles(&cpu->last_ea);
        if (cycles >= 0)
            return cpu_040_tag(cpu, cycles);
    }
    if (cpu_is_68020(cpu))
        return (ea_mode == 0 ? 2 : 4) + ea_cycles(cpu, EA_COST_FETCH_IMM, &cpu->last_ea);
    if (cpu_is_68030_family(cpu))
        return (ea_mode == 0 ? 2 : 3) + ea_cycles(cpu, EA_COST_FETCH_IMM, &cpu->last_ea);
    /* [DS10 Table 7-11]: #,Dn の Long は68010=14（CYC_ADDI_L_DN） */
    if (ea_mode == 0)
        return (size == 4) ? CYC(cpu, CYC_ADDI_L_DN) : 8;
    return (size == 4 ? 20 : 12) + ea_cycles(cpu, EA_COST_FETCH_IMM, &cpu->last_ea);
}

static int op_subi(mc68k_state_t *cpu, uint16_t opcode) {
    static const int sz[3] = {1, 2, 4};
    int sc = (opcode >> 6) & 3;
    if (sc == 3) {
        cpu->exec_state = CPU_EXCEPTION;
        return 4;
    }
    int size = sz[sc];
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg = (opcode >> 0) & 7;

    uint32_t src = fetch_immediate(cpu, size);
    /* 拡張語の補充で fault が確定したら、以降の read・An 更新・CCR 更新を行わない */
    if (cpu->exec_state != CPU_NORMAL)
        return 4;
    uint32_t rmw_addr;
    uint32_t dst = ea_rmw_read(cpu, ea_mode, ea_reg, size, &rmw_addr, false);
    if (cpu->exec_state != CPU_NORMAL)
        return 8;
    uint32_t res = (dst - src) & size_mask(size);
    ccr_sub(cpu, src, dst, res, size);
    ea_rmw_write(cpu, ea_mode, ea_reg, size, rmw_addr, res, false);
    if (cpu_is_68040(cpu)) {
        int cycles = alu40_immediate_rmw_cycles(&cpu->last_ea);
        if (cycles >= 0)
            return cpu_040_tag(cpu, cycles);
    }
    if (cpu_is_68020(cpu))
        return (ea_mode == 0 ? 2 : 4) + ea_cycles(cpu, EA_COST_FETCH_IMM, &cpu->last_ea);
    if (cpu_is_68030_family(cpu))
        return (ea_mode == 0 ? 2 : 3) + ea_cycles(cpu, EA_COST_FETCH_IMM, &cpu->last_ea);
    /* [DS10 Table 7-11]: #,Dn の Long は68010=14（CYC_SUBI_L_DN） */
    if (ea_mode == 0)
        return (size == 4) ? CYC(cpu, CYC_SUBI_L_DN) : 8;
    return (size == 4 ? 20 : 12) + ea_cycles(cpu, EA_COST_FETCH_IMM, &cpu->last_ea);
}

int op_cmpi(mc68k_state_t *cpu, uint16_t opcode) {
    static const int sz[3] = {1, 2, 4};
    int sc = (opcode >> 6) & 3;
    if (sc == 3) {
        cpu->exec_state = CPU_EXCEPTION;
        return 4;
    }
    int size = sz[sc];
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg = (opcode >> 0) & 7;

    uint32_t src = fetch_immediate(cpu, size);
    /* 拡張語の補充で fault が確定したら、以降の read・An 更新・CCR 更新を行わない */
    if (cpu->exec_state != CPU_NORMAL)
        return 4;
    uint32_t dst = ea_read(cpu, ea_mode, ea_reg, size);
    if (cpu->exec_state != CPU_NORMAL)
        return 8;
    uint32_t res = (dst - src) & size_mask(size);
    ccr_cmp(cpu, src, dst, res, size);
    if (cpu_is_68040(cpu)) {
        int cycles = alu40_cmpi_cycles(&cpu->last_ea);
        if (cycles >= 0)
            return cpu_040_tag(cpu, cycles);
    }
    if (cpu_is_68020(cpu))
        return 2 + ea_cycles(cpu, EA_COST_FETCH_IMM, &cpu->last_ea);
    if (cpu_is_68030_family(cpu))
        return 2 + ea_cycles(cpu, EA_COST_FETCH_IMM, &cpu->last_ea);
    /* CMPI は結果を書き込まない。[DS10 Table 7-11]: #,Dn の Long は68010=12
     * （CYC_CMPI_L_DN） */
    if (ea_mode == 0)
        return (size == 4) ? CYC(cpu, CYC_CMPI_L_DN) : 8;
    return (size == 4 ? 12 : 8) + ea_cycles(cpu, EA_COST_FETCH_IMM, &cpu->last_ea);
}

/* ================================================================== */
/* ADDQ / SUBQ（ライン 5: 0x5000-0x5FFF、bits 7-6 != 11）            */
/* ================================================================== */

/*
 * ADDQ/SUBQ: 3bit イミディエイト（1〜8）を EA に加減算する高速命令。
 * bits 11-9 に埋め込まれた 3bit 値が即値で、0 は 8 を意味する。
 * ループカウンタや小さな定数の加算に広く使われる。
 *
 * An に対する ADDQ/SUBQ は CCR を更新しない（ADDA/SUBA と同様）。
 */
static int op_addq(mc68k_state_t *cpu, uint16_t opcode) {
    int data = (opcode >> 9) & 7;
    int size_code = (opcode >> 6) & 3; /* 3 のときは Scc/DBcc なので呼ばれないはず */
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg = (opcode >> 0) & 7;
    if (data == 0)
        data = 8; /* 3bit = 0 → 即値 8 */
    static const int sz[3] = {1, 2, 4};
    int size = sz[size_code];

    if (ea_mode == 1) {
        /* An への加算は CCR 更新なし。
         * [UM00 Table 8-5]: ADDQ.W #,An は4（SUBQ.W #,Anは8で非対称、§2.3(b)）。
         * Byteサイズの An 宛先は無効なため到達しない（register_alu_ops参照）。 */
        cpu->a[ea_reg] += (uint32_t)data;
        if (cpu_is_68040(cpu))
            return cpu_040_tag(cpu, 2); /* 040UM Table 10-6: calculate 1 + execute 1 */
        if (cpu_is_68020(cpu))
            return 2;
        if (cpu_is_68030_family(cpu))
            return 2;
        return (size == 2) ? 4 : 8;
    }
    uint32_t src = (uint32_t)data;
    uint32_t rmw_addr;
    uint32_t dst = ea_rmw_read(cpu, ea_mode, ea_reg, size, &rmw_addr, false);
    if (cpu->exec_state != CPU_NORMAL)
        return 4;
    uint32_t res = (src + dst) & size_mask(size);
    ccr_add(cpu, src, dst, res, size);
    ea_rmw_write(cpu, ea_mode, ea_reg, size, rmw_addr, res, false);
    if (cpu_is_68040(cpu)) {
        int cycles = alu40_addq_cycles(&cpu->last_ea);
        if (cycles >= 0)
            return cpu_040_tag(cpu, cycles);
    }
    if (cpu_is_68020(cpu))
        return (ea_mode == 0 ? 2 : 4) + ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
    if (cpu_is_68030_family(cpu))
        return (ea_mode == 0 ? 2 : 3) + ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
    /* [UM00 Table 8-5]: Long は #,Dn=8 / #,M=12 */
    if (ea_mode == 0)
        return (size == 4) ? 8 : 4;
    return (size == 4 ? 12 : 8) + ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
}

static int op_subq(mc68k_state_t *cpu, uint16_t opcode) {
    int data = (opcode >> 9) & 7;
    int size_code = (opcode >> 6) & 3;
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg = (opcode >> 0) & 7;
    if (data == 0)
        data = 8;
    static const int sz[3] = {1, 2, 4};
    int size = sz[size_code];

    if (ea_mode == 1) {
        /* [UM00 Table 8-5]: SUBQ #,An は68000でbyte/word/longとも8（ADDQ.W #,Anの4と
         * 非対称）。[DS10 Table 7-11]: 68010のWordのみ4（CYC_SUBQ_W_AN）。 */
        cpu->a[ea_reg] -= (uint32_t)data;
        if (cpu_is_68040(cpu))
            return cpu_040_tag(cpu, 2); /* 040UM Table 10-6: calculate 1 + execute 1 */
        if (cpu_is_68020(cpu))
            return 2;
        if (cpu_is_68030_family(cpu))
            return 2;
        return (size == 2) ? CYC(cpu, CYC_SUBQ_W_AN) : 8;
    }
    uint32_t src = (uint32_t)data;
    uint32_t rmw_addr;
    uint32_t dst = ea_rmw_read(cpu, ea_mode, ea_reg, size, &rmw_addr, false);
    if (cpu->exec_state != CPU_NORMAL)
        return 4;
    uint32_t res = (dst - src) & size_mask(size);
    ccr_sub(cpu, src, dst, res, size);
    ea_rmw_write(cpu, ea_mode, ea_reg, size, rmw_addr, res, false);
    if (cpu_is_68040(cpu)) {
        int cycles = alu40_addq_cycles(&cpu->last_ea);
        if (cycles >= 0)
            return cpu_040_tag(cpu, cycles);
    }
    if (cpu_is_68020(cpu))
        return (ea_mode == 0 ? 2 : 4) + ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
    if (cpu_is_68030_family(cpu))
        return (ea_mode == 0 ? 2 : 3) + ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
    /* [UM00 Table 8-5]: Long は #,Dn=8 / #,M=12 */
    if (ea_mode == 0)
        return (size == 4) ? 8 : 4;
    return (size == 4 ? 12 : 8) + ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
}

/* ================================================================== */
/* テーブル登録                                                        */
/* ================================================================== */

void register_alu_ops(op_handler_t *table) {
    /* ADDI/SUBI/CMPI: dst は Data Alterable（immediate/PC相対/An不可）。
     * CCR/SR特殊形式（ANDI/ORI/EORI相当）はADDI/SUBI/CMPIには存在しない。 */
    /* ADDI: 0x0600-0x06BF（byte/word/long、size=11 は除く） */
    for (int i = 0x0600; i <= 0x06FF; i++) {
        if (((i >> 6) & 3) == 3)
            continue;
        if (!ea_is_data_alterable(i & 0x3F))
            continue;
        table[i] = op_addi;
    }
    /* SUBI: 0x0400-0x04BF */
    for (int i = 0x0400; i <= 0x04FF; i++) {
        if (((i >> 6) & 3) == 3)
            continue;
        if (!ea_is_data_alterable(i & 0x3F))
            continue;
        table[i] = op_subi;
    }
    /* CMPI: 0x0C00-0x0CBF */
    for (int i = 0x0C00; i <= 0x0CFF; i++) {
        if (((i >> 6) & 3) == 3)
            continue;
        if (!ea_is_data_alterable(i & 0x3F))
            continue;
        table[i] = op_cmpi;
    }

    /* ADDQ/SUBQ: ライン 5、bits 7-6 != 11（そこは Scc/DBcc）。
     * dst は Alterable（An含む）。ただし byte サイズの An 宛先は無効
     * （ADDA.B/SUBA.B相当が存在しないため）。 */
    for (int i = 0x5000; i <= 0x5FFF; i++) {
        int sz76 = (i >> 6) & 3;
        if (sz76 == 3)
            continue; /* Scc/DBcc は ops_shift.c で実装 */
        int ea = i & 0x3F;
        if (!ea_is_alterable(ea))
            continue;
        if (((ea >> 3) & 7) == 1 && sz76 == 0)
            continue; /* ADDQ.B/SUBQ.B の An 宛先は無効 */
        if ((i >> 8) & 1) {
            table[i] = op_subq;
        } else {
            table[i] = op_addq;
        }
    }

    /*
     * SUB/SUBA（ライン 9）、CMP/CMPA/EOR（ライン B）、ADD/ADDA（ライン D）は
     * op-mode で3方向に分かれる:
     *   0-2: <op> EA,Dn   — src は Data
     *   3,7: <op>A EA,An  — src は全ての有効EA（An自身も含む）
     *   4-6: <op> Dn,EA   — dst は Memory Alterable（Dn/An/immediate/PC相対不可）
     * ライン B の op-mode 4-6 は EOR の領域（register_logic_ops が登録）。
     * ここで登録してしまうと無効な ea では op_cmp の中途半端な例外処理
     * （exec_state直接セットのみでベクタ未設定）が残ってしまうため、
     * CMP はそもそも op-mode 4-6 を登録しない（初期値の ILLEGAL のまま
     * register_logic_ops のフィルタ済み登録に委ねる）。
     */
    for (int dn = 0; dn < 8; dn++) {
        uint16_t sb = (uint16_t)(0x9000 | (dn << 9));
        uint16_t bb = (uint16_t)(0xB000 | (dn << 9));
        uint16_t db = (uint16_t)(0xD000 | (dn << 9));
        for (int ea = 0; ea < 64; ea++) {
            /* op=0（byte）: An直接は不可 */
            if (ea_is_data(ea)) {
                table[sb | ea] = op_sub;
                table[bb | ea] = op_cmp;
                table[db | ea] = op_add;
            }
            /* op=1,2（word/long）: An直接も許可（実機の特例） */
            if (ea_is_data_or_an(ea)) {
                for (int op = 1; op <= 2; op++) {
                    table[sb | (op << 6) | ea] = op_sub;
                    table[bb | (op << 6) | ea] = op_cmp;
                    table[db | (op << 6) | ea] = op_add;
                }
            }
            /* SUBA/CMPA/ADDA: source は An を含む有効EA。
             * mode=111/reg=101--111 は予約済みなので ILLEGAL のまま残す。 */
            if (ea_is_data_or_an(ea)) {
                table[sb | (3 << 6) | ea] = op_sub;
                table[sb | (7 << 6) | ea] = op_sub;
                table[bb | (3 << 6) | ea] = op_cmp;
                table[bb | (7 << 6) | ea] = op_cmp;
                table[db | (3 << 6) | ea] = op_add;
                table[db | (7 << 6) | ea] = op_add;
            }

            if (ea_is_memory_alterable(ea)) {
                for (int op = 4; op <= 6; op++) {
                    table[sb | (op << 6) | ea] = op_sub;
                    /* bb（ライン B）op-mode 4-6 は EOR 用地なので触らない */
                    table[db | (op << 6) | ea] = op_add;
                }
            }
        }
    }
}
