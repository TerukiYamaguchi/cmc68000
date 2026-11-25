/*
 * ops_arith_ext.c — 拡張算術演算命令ハンドラ実装
 *
 * 実装する命令:
 *   - MULU / MULS        : 16⅞16→32bit 乗算
 *   - CHK                : 範囲チェック
 *   - ABCD / SBCD / NBCD  : BCD 演算
 *   - DIVU / DIVS        : 32bit ÷ 16bit 除算
 *   - ADDX / SUBX        : X フラグ付き加減算（多倍長演算用）
 *   - CMPM               : (Ay)+, (Ax)+ 比較
 *
 * register_arith_ext_ops() は register_alu_ops()（ADD/SUB を全域登録）と
 * register_logic_ops()（EOR を含む）の後に呼ばれる前提。
 * ADDX/SUBX は ADD/SUB の一部を、CMPM は EOR の一部を上書きする。
 */
#include "cpu_internal.h"
#include <stdint.h>

/* MC68000 の MULU.W／MULS.W の時間は 38+2n（n は MULU が 1 のビット数、MULS が 01／10 の組の数）。[UM00 Table 8-4] */
enum {
    MULU_68000_BASE_CYCLES = 38,
    MULU_68000_CYCLES_PER_BIT = 2,
};

static int popcount16(uint32_t v) {
    v &= 0xFFFF;
    int n = 0;
    for (; v != 0; v &= v - 1)
        n++;
    return n;
}

/* サイズコード → バイト数のルックアップ（0=byte,1=word,2=long）*/
static const int g_sz[3] = {1, 2, 4};

/* 040UM 表 10-6（PDF 311、印刷版 10-20）の DIVS.W/DIVU.W。#89 の公称値規則により
 * 印刷上の実行先行分は除外するが、表 10-2 のオペランドフェッチは値に含める。 */
static int div40_word_cycles(const ea_desc_t *ea) {
    switch (ea->mode) {
    case 0:
        return 35;
    case 2:
    case 3:
    case 4:
    case 5:
        return 36;
    case 6:
        if (ea->ext_form != EA_EXT_FULL)
            return 42;
        if (ea->index_suppressed && ea->iis == 0)
            return -1;
        switch (ea->iis) {
        case 0:
            return ea->bd_size == 0 ? 45 : 47;
        case 1:
        case 5:
            return 54;
        case 2:
        case 3:
        case 6:
        case 7:
            return 56;
        default:
            return -1;
        }
    case 7:
        if (ea->reg == 0 || ea->reg == 1)
            return 36;
        if (ea->reg == 2)
            return 39;
        if (ea->reg == 3) {
            if (ea->ext_form != EA_EXT_FULL)
                return 43;
            if (ea->index_suppressed && ea->iis == 0)
                return -1;
            switch (ea->iis) {
            case 0:
                return ea->bd_size == 0 ? 45 : 47;
            case 1:
            case 5:
                return 54;
            case 2:
            case 3:
            case 6:
            case 7:
                return 56;
            default:
                return -1;
            }
        }
        if (ea->reg == 4)
            return 35;
        return -1;
    default:
        return -1;
    }
}

/* 040UM 表 10-6（PDF 316、印刷版 10-25）の word オペランド列。 */
static int mul40_word_cycles(const ea_desc_t *ea, int is_signed) {
    int dn = is_signed ? 17 : 15;
    int memory = is_signed ? 18 : 16;
    int brief = is_signed ? 22 : 20;
    int direct = is_signed ? 27 : 25;
    int indirect = is_signed ? 34 : 32;
    switch (ea->mode) {
    case 0:
        return dn;
    case 2:
    case 3:
    case 4:
    case 5:
        return memory;
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
            return memory;
        if (ea->reg == 2)
            return memory + 2;
        if (ea->reg == 3) {
            if (ea->ext_form != EA_EXT_FULL)
                return brief + 2;
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
        }
        if (ea->reg == 4)
            return dn;
        return -1;
    default:
        return -1;
    }
}

/* 040UM 表 10-6（PDF 316、印刷版 10-25）の NBCD 列。 */
static int nbcd40_cycles(const ea_desc_t *ea) {
    switch (ea->mode) {
    case 0:
        return 4;
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
            return 20;
        case 2:
        case 3:
        case 6:
        case 7:
            return 22;
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

/* ================================================================== */
/* MULU / MULS（16⅞16→32bit 乗算）                                   */
/* ================================================================== */

/*
 * MULU.W EA, Dn: 符号なし 16bit × 16bit → 32bit
 * X68000 のグラフィック座標計算（スクリーン幅×Y座標）などで多用される。
 * MC68000 の乗算は 38-70 サイクル（オペランドの 1 の数に依存）。
 */
static int op_mulu(mc68k_state_t *cpu, uint16_t opcode) {
    int dn = (opcode >> 9) & 7;
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg = opcode & 7;

    uint32_t src = ea_read(cpu, ea_mode, ea_reg, 2); /* 16bit 符号なし */
    if (cpu->exec_state != CPU_NORMAL)
        return 4;

    uint32_t res = (cpu->d[dn] & 0xFFFF) * (src & 0xFFFF);
    cpu->d[dn] = res;

    cpu->sr &= ~(CCR_N | CCR_Z | CCR_V | CCR_C);
    if (!res)
        cpu->sr |= CCR_Z;
    if (res & 0x80000000)
        cpu->sr |= CCR_N;
    if (cpu_is_68040(cpu)) {
        int cycles = mul40_word_cycles(&cpu->last_ea, 0);
        if (cycles >= 0)
            return cpu_040_tag(cpu, cycles);
    }
    if (cpu_is_68030_family(cpu))
        return 28 + ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
    /* [UM00 Table 8-4]: MC68000 は 38+2n+EA（n はソースの 1 のビット数）。 */
    if (cpu->model == MC68K_MODEL_68000)
        return MULU_68000_BASE_CYCLES + MULU_68000_CYCLES_PER_BIT * popcount16(src & 0xFFFF) +
               ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
    /* それ以外は最悪ケース: オペランドの 1 が多い場合。[DS10 Table 7-9]: 68010=40（CYC_MULU） */
    return CYC(cpu, CYC_MULU);
}

/*
 * MULS.W EA, Dn: 符号付き 16bit × 16bit → 32bit
 * 符号付き座標変換や固定小数点演算に使われる。
 */
static int op_muls(mc68k_state_t *cpu, uint16_t opcode) {
    int dn = (opcode >> 9) & 7;
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg = opcode & 7;

    uint32_t src = ea_read(cpu, ea_mode, ea_reg, 2);
    if (cpu->exec_state != CPU_NORMAL)
        return 4;

    int32_t s = (int32_t)(int16_t)(src & 0xFFFF);
    int32_t d = (int32_t)(int16_t)(cpu->d[dn] & 0xFFFF);
    uint32_t res = (uint32_t)(s * d);
    cpu->d[dn] = res;

    cpu->sr &= ~(CCR_N | CCR_Z | CCR_V | CCR_C);
    if (!res)
        cpu->sr |= CCR_Z;
    if (res & 0x80000000)
        cpu->sr |= CCR_N;
    if (cpu_is_68040(cpu)) {
        int cycles = mul40_word_cycles(&cpu->last_ea, 1);
        if (cycles >= 0)
            return cpu_040_tag(cpu, cycles);
    }
    if (cpu_is_68030_family(cpu))
        return 28 + ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
    /* [UM00 Table 8-4]: MC68000 は 38+2n+EA（n は末尾に 0 を足したソースの 01／10 の組の数） */
    if (cpu->model == MC68K_MODEL_68000) {
        uint32_t s16 = src & 0xFFFF;
        return MULU_68000_BASE_CYCLES + MULU_68000_CYCLES_PER_BIT * popcount16(s16 ^ (s16 << 1)) +
               ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
    }
    /* それ以外は最悪ケース。[DS10 Table 7-9]: 68010=42（CYC_MULS） */
    return CYC(cpu, CYC_MULS);
}

/* ================================================================== */
/* CHK（範囲チェック）                                                  */
/* ================================================================== */

/* 040UM 表 10-6（PDF 308、印刷版 10-17）の CHK <ea>,Dn、Dn が範囲内の場合（注 c/d）。
 * 公称値 = 計算 + 実行基準値 + 表 10-2 のフェッチであり、実行先行分は除外する（#89）。
 * 範囲外例外は動的のままとする。マニュアルに IS=1 直接フル拡張行はないため、その場合は -1
 * を返す。 */
int chk40_in_bounds_cycles(const ea_desc_t *ea) {
    switch (ea->mode) {
    case 0:
        return 15;
    case 2:
    case 3:
    case 4:
    case 5:
        return 17;
    case 6:
        if (ea->ext_form != EA_EXT_FULL)
            return 21;
        break;
    case 7:
        if (ea->reg == 0 || ea->reg == 1)
            return 17;
        if (ea->reg == 2)
            return 18;
        if (ea->reg == 4)
            return 15;
        if (ea->reg != 3)
            return -1;
        if (ea->ext_form != EA_EXT_FULL)
            return 22;
        break;
    default:
        return -1;
    }
    if (ea->index_suppressed && ea->iis == 0)
        return -1;
    switch (ea->iis) {
    case 0:
        return ea->bd_size == 0 ? 24 : 26;
    case 1:
    case 5:
        return 33;
    case 2:
    case 3:
    case 6:
    case 7:
        return 35;
    default:
        return -1;
    }
}

/*
 * CHK.W EA, Dn: 0 ≤ Dn ≤ EA でなければベクタ 6 へ例外ジャンプ。
 * コンパイラの配列境界チェックとして出力される。
 */
static int op_chk(mc68k_state_t *cpu, uint16_t opcode) {
    int dn = (opcode >> 9) & 7;
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg = opcode & 7;
    int16_t bound = (int16_t)(uint16_t)ea_read(cpu, ea_mode, ea_reg, 2);
    if (cpu->exec_state != CPU_NORMAL)
        return 4;

    int16_t val = (int16_t)(uint16_t)cpu->d[dn];
    /* 範囲内: 例外なし。[DS10 Table 7-21]: 68010=8（CYC_CHK_NORMAL） */
    if (val >= 0 && val <= bound) {
        if (cpu_is_68040(cpu)) {
            int cycles = chk40_in_bounds_cycles(&cpu->last_ea);
            if (cycles >= 0)
                return cpu_040_tag(cpu, cycles);
        }
        if (cpu_is_68030_family(cpu))
            return 8 + ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
        /* [UM00 Table 8-6]: CHK は 10+EA（68000/68010）。 */
        if (cpu->model == MC68K_MODEL_68000 || cpu->model == MC68K_MODEL_68010)
            return CYC(cpu, CYC_CHK_NORMAL) + ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
        return CYC(cpu, CYC_CHK_NORMAL);
    }

    /* 範囲外: CHK 例外（ベクタ 6）。[DS10 Table 7-22]: 68010=44（CYC_EXC_CHK） */
    cpu->sr &= ~CCR_N;
    if (val < 0)
        cpu->sr |= CCR_N;
    cpu->flow_changed = true;
    if (cpu_is_68030_family(cpu)) {
        cyc_class_t cls = ea_mode == 0 ? CYC_CHK_EXCEPTION_DN : CYC_CHK_EXCEPTION_EA;
        return cpu_dispatch_exception(cpu, cpu->pc, 6,
                                      CYC(cpu, cls) +
                                          ea_cycles(cpu, cyc_ea_kind(cpu, cls), &cpu->last_ea));
    }
    return cpu_dispatch_exception(cpu, cpu->pc, 6, CYC(cpu, CYC_EXC_CHK));
}

/* ================================================================== */
/* ABCD / SBCD / NBCD（BCD 演算）                                      */
/* ================================================================== */

/*
 * BCD 加算補正ヘルパー。
 * 低ニブルが 9 を超えたら +6、高ニブルが 9 を超えたら +0x60。
 * Z フラグは ADDX 同様「0 なら変化なし、0 でなければクリア」。
 */
static int do_bcd_add(mc68k_state_t *cpu, uint8_t src, uint8_t dst) {
    uint32_t x = (cpu->sr & CCR_X) ? 1u : 0u;

    uint32_t lo = (src & 0xF) + (dst & 0xF) + x;
    uint32_t bin = (uint32_t)src + (uint32_t)dst + x;
    uint32_t res = bin;
    uint32_t carry = 0;

    if (lo > 9)
        res += 6;
    if (res > 0x99) {
        res += 0x60;
        carry = 1;
    }
    res &= 0xFF;

    uint16_t old_z = cpu->sr & CCR_Z;
    cpu->sr &= ~(CCR_X | CCR_N | CCR_Z | CCR_V | CCR_C);
    if (!res)
        cpu->sr |= old_z;
    if (res & 0x80)
        cpu->sr |= CCR_N;
    if (carry)
        cpu->sr |= CCR_C | CCR_X;
    return (int)res;
}

static int do_bcd_sub(mc68k_state_t *cpu, uint8_t src, uint8_t dst) {
    uint32_t x = (cpu->sr & CCR_X) ? 1u : 0u;

    uint32_t bin = (uint32_t)dst - (uint32_t)src - x;
    uint32_t res = bin;
    uint32_t borrow = 0;

    /* 低ニブルでボローが発生していれば -6 補正 */
    if ((dst & 0xF) < (src & 0xF) + x)
        res -= 6;

    /* 高ニブルでボロー（全体的なアンダーフロー） */
    if (bin > 0xFF) {
        res -= 0x60;
        borrow = 1;
    }
    res &= 0xFF;

    uint16_t old_z = cpu->sr & CCR_Z;
    cpu->sr &= ~(CCR_X | CCR_N | CCR_Z | CCR_V | CCR_C);
    if (!res)
        cpu->sr |= old_z;
    if (res & 0x80)
        cpu->sr |= CCR_N;
    if (borrow)
        cpu->sr |= CCR_C | CCR_X;
    return (int)res;
}

/* ABCD: Ry + Rx + X → Rx (BCD) */
static int op_abcd(mc68k_state_t *cpu, uint16_t opcode) {
    int rx = (opcode >> 9) & 7;
    int m = (opcode >> 3) & 1;
    int ry = opcode & 7;

    uint8_t src, dst;
    if (!m) {
        src = (uint8_t)(cpu->d[ry] & 0xFF);
        dst = (uint8_t)(cpu->d[rx] & 0xFF);
    } else {
        int sy = (ry == 7) ? 2 : 1;
        int sx = (rx == 7) ? 2 : 1;
        cpu->a[ry] -= sy;
        cpu->a[rx] -= sx;
        const mc68k_access_t rmw_read = cpu_data_access(cpu, true, 1, false);
        src = cpu_read_byte_access(cpu, cpu->a[ry], &rmw_read);
        dst = cpu_read_byte_access(cpu, cpu->a[rx], &rmw_read);
    }

    int res = do_bcd_add(cpu, src, dst);

    if (!m) {
        cpu->d[rx] = (cpu->d[rx] & 0xFFFFFF00U) | (uint32_t)res;
        if (cpu_is_68040(cpu))
            return cpu_040_tag(cpu, 4); /* 040UM Table 10-5: calculate 1 + execute 3 */
        if (cpu_is_68030_family(cpu))
            return 4;
        return 6;
    }
    const mc68k_access_t rmw_write = cpu_data_access(cpu, false, 1, false);
    cpu_write_byte_access(cpu, cpu->a[rx], (uint8_t)res, &rmw_write);
    if (cpu_is_68040(cpu))
        return cpu_040_tag(cpu,
                           8); /* 040UM Table 10-5: calculate 3 + execute 3 + two fetches */
    return cpu_is_68030_family(cpu) ? 13 : 18;
}

/* SBCD: Rx - Ry - X → Rx (BCD) */
static int op_sbcd(mc68k_state_t *cpu, uint16_t opcode) {
    int rx = (opcode >> 9) & 7;
    int m = (opcode >> 3) & 1;
    int ry = opcode & 7;

    uint8_t src, dst;
    if (!m) {
        src = (uint8_t)(cpu->d[ry] & 0xFF);
        dst = (uint8_t)(cpu->d[rx] & 0xFF);
    } else {
        int sy = (ry == 7) ? 2 : 1;
        int sx = (rx == 7) ? 2 : 1;
        cpu->a[ry] -= sy;
        cpu->a[rx] -= sx;
        const mc68k_access_t rmw_read = cpu_data_access(cpu, true, 1, false);
        src = cpu_read_byte_access(cpu, cpu->a[ry], &rmw_read);
        dst = cpu_read_byte_access(cpu, cpu->a[rx], &rmw_read);
    }

    int res = do_bcd_sub(cpu, src, dst);

    if (!m) {
        cpu->d[rx] = (cpu->d[rx] & 0xFFFFFF00U) | (uint32_t)res;
        if (cpu_is_68040(cpu))
            return cpu_040_tag(cpu, 4); /* 040UM Table 10-5: calculate 1 + execute 3 */
        if (cpu_is_68030_family(cpu))
            return 4;
        return 6;
    }
    const mc68k_access_t rmw_write = cpu_data_access(cpu, false, 1, false);
    cpu_write_byte_access(cpu, cpu->a[rx], (uint8_t)res, &rmw_write);
    if (cpu_is_68040(cpu))
        return cpu_040_tag(cpu,
                           8); /* 040UM Table 10-5: calculate 3 + execute 3 + two fetches */
    return cpu_is_68030_family(cpu) ? 13 : 18;
}

/* NBCD EA: 0 - EA - X → EA (BCD) */
static int op_nbcd(mc68k_state_t *cpu, uint16_t opcode) {
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg = opcode & 7;
    uint32_t rmw_addr;
    uint8_t dst = (uint8_t)(ea_rmw_read(cpu, ea_mode, ea_reg, 1, &rmw_addr, false) & 0xFF);
    if (cpu->exec_state != CPU_NORMAL)
        return 4;

    int res = do_bcd_sub(cpu, dst, 0); /* 0 - dst - X */

    ea_rmw_write(cpu, ea_mode, ea_reg, 1, rmw_addr, (uint32_t)res, false);
    if (cpu->exec_state != CPU_NORMAL)
        return 4;
    if (cpu_is_68040(cpu)) {
        int cycles = nbcd40_cycles(&cpu->last_ea);
        if (cycles >= 0)
            return cpu_040_tag(cpu, cycles);
    }
    if (cpu_is_68030_family(cpu))
        return ea_mode == 0 ? 6 : 3 + ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
    return (ea_mode == 0) ? 6 : 8 + ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
}

/* ================================================================== */
/* DIVU / DIVS（32bit ÷ 16bit → 商下位 16bit + 余り上位 16bit）      */
/* ================================================================== */

static int trap_div_zero(mc68k_state_t *cpu) {
    /* ゼロ除算: ベクタ 5。[DS10 Table 7-22]: 68010=42（CYC_EXC_DIVZERO） */
    return cpu_dispatch_exception(cpu, cpu->pc, 5, CYC(cpu, CYC_EXC_DIVZERO));
}

/* DIVU.W EA, Dn: 符号なし 32÷16 */
static int op_divu(mc68k_state_t *cpu, uint16_t opcode) {
    int dn = (opcode >> 9) & 7;
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg = opcode & 7;
    uint32_t divisor = ea_read(cpu, ea_mode, ea_reg, 2) & 0xFFFF;
    if (cpu->exec_state != CPU_NORMAL)
        return 4;

    if (divisor == 0)
        return trap_div_zero(cpu);

    uint32_t dividend = cpu->d[dn];
    uint32_t quotient = dividend / divisor;
    uint32_t remainder = dividend % divisor;

    cpu->sr &= ~(CCR_N | CCR_Z | CCR_V | CCR_C);
    if (quotient > 0xFFFF) {
        cpu->sr |= CCR_V; /* オーバーフロー: Dn は変化しない */
        if (cpu_is_68040(cpu)) {
            int cycles = div40_word_cycles(&cpu->last_ea);
            if (cycles >= 0)
                return cpu_040_tag(cpu, cycles);
        }
        if (cpu_is_68030_family(cpu))
            return 44 + ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
        return CYC(cpu, CYC_DIVU);
    }
    cpu->d[dn] = (remainder << 16) | (quotient & 0xFFFF);
    if (!quotient)
        cpu->sr |= CCR_Z;
    if (quotient & 0x8000)
        cpu->sr |= CCR_N;
    if (cpu_is_68040(cpu)) {
        int cycles = div40_word_cycles(&cpu->last_ea);
        if (cycles >= 0)
            return cpu_040_tag(cpu, cycles);
    }
    /* [DS10 Table 7-9]: 68010は固定値108（CYC_DIVU） */
    if (cpu_is_68030_family(cpu))
        return 44 + ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
    return CYC(cpu, CYC_DIVU);
}

/* DIVS.W EA, Dn: 符号付き 32÷16 */
static int op_divs(mc68k_state_t *cpu, uint16_t opcode) {
    int dn = (opcode >> 9) & 7;
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg = opcode & 7;
    uint32_t src = ea_read(cpu, ea_mode, ea_reg, 2);
    if (cpu->exec_state != CPU_NORMAL)
        return 4;

    int32_t divisor = (int32_t)(int16_t)(uint16_t)(src & 0xFFFF);
    if (divisor == 0)
        return trap_div_zero(cpu);

    int32_t dividend = (int32_t)cpu->d[dn];
    int32_t quotient = dividend / divisor;
    int32_t remainder = dividend % divisor;

    cpu->sr &= ~(CCR_N | CCR_Z | CCR_V | CCR_C);
    if (quotient > 32767 || quotient < -32768) {
        cpu->sr |= CCR_V;
        if (cpu_is_68040(cpu)) {
            int cycles = div40_word_cycles(&cpu->last_ea);
            if (cycles >= 0)
                return cpu_040_tag(cpu, cycles);
        }
        if (cpu_is_68030_family(cpu))
            return 56 + ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
        return CYC(cpu, CYC_DIVS);
    }
    cpu->d[dn] = ((uint32_t)(uint16_t)(uint32_t)remainder << 16) |
                 (uint32_t)(uint16_t)(uint32_t)quotient;
    if (!quotient)
        cpu->sr |= CCR_Z;
    if ((uint32_t)quotient & 0x8000)
        cpu->sr |= CCR_N;
    if (cpu_is_68040(cpu)) {
        int cycles = div40_word_cycles(&cpu->last_ea);
        if (cycles >= 0)
            return cpu_040_tag(cpu, cycles);
    }
    /* [DS10 Table 7-9]: 68010は固定値122（CYC_DIVS） */
    if (cpu_is_68030_family(cpu))
        return 56 + ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
    return CYC(cpu, CYC_DIVS);
}

/* ================================================================== */
/* ADDX / SUBX（X フラグ付き加減算、多倍長演算用）                     */
/* ================================================================== */

/*
 * ADDX/SUBX 共通: opcode bit 3 = M (0=Dreg, 1=predecrement)
 * Z フラグのみ特殊: 結果が 0 なら変化なし、0 でなければクリア。
 * これにより ADDX.L / ADDX.L のペアで 64bit 加算の Z が正しく機能する。
 */
static int op_addx(mc68k_state_t *cpu, uint16_t opcode) {
    int rx = (opcode >> 9) & 7;
    int sz = (opcode >> 6) & 3;
    int m = (opcode >> 3) & 1;
    int ry = opcode & 7;
    int size = g_sz[sz];
    uint32_t sm = size_mask(size);
    uint32_t sb = sign_bit(size);
    uint32_t x = (cpu->sr & CCR_X) ? 1u : 0u;

    uint32_t src, dst;
    if (!m) {
        src = cpu->d[ry] & sm;
        dst = cpu->d[rx] & sm;
    } else {
        int sy = (size == 1 && ry == 7) ? 2 : size;
        int sx = (size == 1 && rx == 7) ? 2 : size;
        cpu->a[ry] -= sy;
        cpu->a[rx] -= sx;
        src = ea_read(cpu, 2, ry, size); /* (Ay) */
        if (cpu->exec_state != CPU_NORMAL)
            return 4;
        dst = ea_read(cpu, 2, rx, size); /* (Ax) */
        if (cpu->exec_state != CPU_NORMAL)
            return 4;
    }

    uint32_t res = (dst + src + x) & sm;
    uint16_t old_z = cpu->sr & CCR_Z;
    cpu->sr &= ~(CCR_X | CCR_N | CCR_Z | CCR_V | CCR_C);
    if (!res)
        cpu->sr |= old_z; /* 0 なら Z を保持 */
    if (res & sb)
        cpu->sr |= CCR_N;
    if (size == 4) {
        if ((uint64_t)(dst & sm) + (src & sm) + x > 0xFFFFFFFFUL)
            cpu->sr |= CCR_C | CCR_X;
    } else {
        if ((dst & sm) + (src & sm) + x > sm)
            cpu->sr |= CCR_C | CCR_X;
    }
    if ((~(src ^ dst) & (dst ^ res)) & sb)
        cpu->sr |= CCR_V;

    if (!m) {
        cpu->d[rx] = (cpu->d[rx] & ~sm) | res;
        /* [UM00 Table 8-20] / [DS10 Table 7-20]: Dn,Dn の Long は68000=8、
         * 68010=6（CYC_ADDX_L_REGREG） */
        if (cpu_is_68040(cpu))
            return cpu_040_tag(cpu, 2); /* 040UM Table 10-5: calculate 1 + execute 1 */
        if (cpu_is_68030_family(cpu))
            return 2;
        return (size == 4) ? CYC(cpu, CYC_ADDX_L_REGREG) : 4;
    }
    ea_write(cpu, 2, rx, size, res);
    if (cpu->exec_state != CPU_NORMAL)
        return 4;
    /* [UM00 Table 8-11] / [DS10 Table 7-20]: -(An),-(An)形式は自己完結（EA加算なし）。
     * Byte/Word=18, Long=30 */
    if (cpu_is_68040(cpu))
        return cpu_040_tag(cpu,
                           7); /* 040UM Table 10-5: calculate 3 + execute 2 + two fetches */
    if (cpu_is_68030_family(cpu))
        return 9;
    return (size == 4) ? 30 : 18;
}

static int op_subx(mc68k_state_t *cpu, uint16_t opcode) {
    int rx = (opcode >> 9) & 7;
    int sz = (opcode >> 6) & 3;
    int m = (opcode >> 3) & 1;
    int ry = opcode & 7;
    int size = g_sz[sz];
    uint32_t sm = size_mask(size);
    uint32_t sb = sign_bit(size);
    uint32_t x = (cpu->sr & CCR_X) ? 1u : 0u;

    uint32_t src, dst;
    if (!m) {
        src = cpu->d[ry] & sm;
        dst = cpu->d[rx] & sm;
    } else {
        int sy = (size == 1 && ry == 7) ? 2 : size;
        int sx = (size == 1 && rx == 7) ? 2 : size;
        cpu->a[ry] -= sy;
        cpu->a[rx] -= sx;
        src = ea_read(cpu, 2, ry, size);
        if (cpu->exec_state != CPU_NORMAL)
            return 4;
        dst = ea_read(cpu, 2, rx, size);
        if (cpu->exec_state != CPU_NORMAL)
            return 4;
    }

    uint32_t res = (dst - src - x) & sm;
    uint16_t old_z = cpu->sr & CCR_Z;
    cpu->sr &= ~(CCR_X | CCR_N | CCR_Z | CCR_V | CCR_C);
    if (!res)
        cpu->sr |= old_z;
    if (res & sb)
        cpu->sr |= CCR_N;
    if ((uint64_t)(src & sm) + x > (uint64_t)(dst & sm))
        cpu->sr |= CCR_C | CCR_X;
    if (((src ^ dst) & (dst ^ res)) & sb)
        cpu->sr |= CCR_V;

    if (!m) {
        cpu->d[rx] = (cpu->d[rx] & ~sm) | res;
        /* [UM00 Table 8-20] / [DS10 Table 7-20]: Dn,Dn の Long は68000=8、
         * 68010=6（CYC_SUBX_L_REGREG） */
        if (cpu_is_68040(cpu))
            return cpu_040_tag(cpu, 2); /* 040UM Table 10-5: calculate 1 + execute 1 */
        if (cpu_is_68030_family(cpu))
            return 2;
        return (size == 4) ? CYC(cpu, CYC_SUBX_L_REGREG) : 4;
    }
    ea_write(cpu, 2, rx, size, res);
    if (cpu->exec_state != CPU_NORMAL)
        return 4;
    /* [UM00 Table 8-11] / [DS10 Table 7-20]: -(An),-(An)形式は自己完結（EA加算なし）。
     * Byte/Word=18, Long=30 */
    if (cpu_is_68040(cpu))
        return cpu_040_tag(cpu,
                           7); /* 040UM Table 10-5: calculate 3 + execute 2 + two fetches */
    if (cpu_is_68030_family(cpu))
        return 9;
    return (size == 4) ? 30 : 18;
}

/* ================================================================== */
/* CMPM (Ay)+, (Ax)+                                                   */
/* ================================================================== */

static int op_cmpm(mc68k_state_t *cpu, uint16_t opcode) {
    int ax = (opcode >> 9) & 7;
    int sz = (opcode >> 6) & 3;
    int ay = opcode & 7;
    int size = g_sz[sz];
    uint32_t sm = size_mask(size);
    uint32_t sb = sign_bit(size);

    uint32_t src = ea_read(cpu, 3, ay, size); /* (Ay)+ */
    if (cpu->exec_state != CPU_NORMAL)
        return 4;
    uint32_t dst = ea_read(cpu, 3, ax, size); /* (Ax)+ */
    if (cpu->exec_state != CPU_NORMAL)
        return 4;

    uint32_t res = (dst - src) & sm;
    uint16_t x_sv = cpu->sr & CCR_X;
    cpu->sr &= ~(CCR_N | CCR_Z | CCR_V | CCR_C);
    cpu->sr |= x_sv;
    if (!res)
        cpu->sr |= CCR_Z;
    if (res & sb)
        cpu->sr |= CCR_N;
    if ((src & sm) > (dst & sm))
        cpu->sr |= CCR_C;
    if (((src ^ dst) & (dst ^ res)) & sb)
        cpu->sr |= CCR_V;
    if (cpu_is_68040(cpu))
        return cpu_040_tag(cpu,
                           7); /* 040UM Table 10-5: calculate 3 + execute 2 + two fetches */
    if (cpu_is_68030_family(cpu))
        return 8;
    return (size == 4) ? 20 : 12;
}

/* ================================================================== */
/* テーブル登録                                                        */
/* ================================================================== */

void register_arith_ext_ops(op_handler_t *table) {
    int dn, ea, rx, ry, sz;

    /* ---- MULU（ライン C、opmode 3）/ MULS（ライン C、opmode 7）、src=Data ---- */
    for (dn = 0; dn < 8; dn++) {
        for (ea = 0; ea < 64; ea++) {
            if (!ea_is_data(ea))
                continue;
            uint16_t b = (uint16_t)(0xC000 | (dn << 9));
            table[b | (3 << 6) | ea] = op_mulu;
            table[b | (7 << 6) | ea] = op_muls;
        }
    }

    /* ---- DIVU / DIVS (ライン 8, opmode 3/7)、src=Data ---- */
    for (dn = 0; dn < 8; dn++) {
        for (ea = 0; ea < 64; ea++) {
            if (!ea_is_data(ea))
                continue;
            uint16_t bo = (uint16_t)(0x8000 | (dn << 9));
            table[bo | (3 << 6) | ea] = op_divu;
            table[bo | (7 << 6) | ea] = op_divs;
        }
    }

    /* ---- ADDX / SUBX (ライン D/9, opmode 4-6, ea_mode=0/1 を上書き) ---- */
    for (rx = 0; rx < 8; rx++) {
        for (ry = 0; ry < 8; ry++) {
            for (sz = 0; sz < 3; sz++) {
                /* register form (M=0): bits 5-3=000 */
                uint16_t ar = (uint16_t)(0xD100 | (rx << 9) | (sz << 6) | ry);
                /* predecrement form (M=1): bits 5-3=001 */
                uint16_t am = (uint16_t)(0xD100 | (rx << 9) | (sz << 6) | 0x08 | ry);
                uint16_t sr2 = (uint16_t)(0x9100 | (rx << 9) | (sz << 6) | ry);
                uint16_t sm2 = (uint16_t)(0x9100 | (rx << 9) | (sz << 6) | 0x08 | ry);
                table[ar] = op_addx;
                table[am] = op_addx;
                table[sr2] = op_subx;
                table[sm2] = op_subx;
            }
        }
    }

    /* ---- CMPM (Ay)+, (Ax)+  (ライン B, opmode 4-6, ea_mode=1 を上書き)
     * register_logic_ops() の EOR 登録より後に呼ばれる前提（EOR の一部を上書き） ---- */
    for (rx = 0; rx < 8; rx++) {
        for (ry = 0; ry < 8; ry++) {
            for (sz = 0; sz < 3; sz++) {
                uint16_t cm = (uint16_t)(0xB100 | (rx << 9) | (sz << 6) | 0x08 | ry);
                table[cm] = op_cmpm;
            }
        }
    }

    /* ---- TAS (0x4AC0-0x4AFF, ただし 0x4AFC=ILLEGAL を除く) ---- */
    /* CHK.W EA, Dn (ライン 4, opmode=6)、src=Data */
    for (dn = 0; dn < 8; dn++) {
        for (ea = 0; ea < 64; ea++) {
            if (!ea_is_data(ea))
                continue;
            uint16_t ck = (uint16_t)(0x4180 | (dn << 9) | ea);
            table[ck] = op_chk;
        }
    }

    /* ---- ABCD / SBCD (AND/OR の mode=0/1 を上書き) ---- */
    for (rx = 0; rx < 8; rx++) {
        for (ry = 0; ry < 8; ry++) {
            /* ABCD: ライン C (0xC000), AND と同じ位置 */
            table[0xC100 | (rx << 9) | ry] = op_abcd;
            table[0xC108 | (rx << 9) | ry] = op_abcd;
            /* SBCD: ライン 8 (0x8000), OR と同じ位置 */
            table[0x8100 | (rx << 9) | ry] = op_sbcd;
            table[0x8108 | (rx << 9) | ry] = op_sbcd;
        }
    }

    /* ---- NBCD (0x4800-0x483F) ---- */
    for (ea = 0; ea < 64; ea++) {
        if (ea_is_data_alterable(ea))
            table[0x4800 | ea] = op_nbcd;
    }
}
