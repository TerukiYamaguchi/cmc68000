/*
 * ops_shift.c — シフト/ローテート、Scc/DBcc 命令ハンドラ実装
 *
 * 実装する命令:
 *   - Scc / DBcc          : 条件セット / デクリメントと分岐
 *   - ASL/ASR/LSL/LSR     : シフト演算（レジスタ形式・メモリ形式）
 *   - ROL/ROR/ROXL/ROXR   : ローテート演算
 *
 * eval_cond() は ops_branch.c 実装を共用する。
 */
#include "cpu_internal.h"
#include <stdint.h>

/* 040UM 表 10-6: 4 種類のメモリシフト／ローテート列は、直接「計算 + 実行基準値」に
 * 還元する。表には IS=1 のフル形式行がない。 */
static int shift40_mem_cycles(const ea_desc_t *ea, int type, int dr) {
    int basic, brief, direct, indirect;
    if (type == 0) { /* AS */
        basic = dr ? 4 : 3;
        brief = dr ? 8 : 7;
        direct = dr ? 15 : 14;
        indirect = dr ? 21 : 20;
    } else if (type == 1) { /* LS */
        basic = 3;
        brief = 7;
        direct = 14;
        indirect = 20;
    } else if (type == 2) { /* ROX */
        basic = 3;
        brief = 7;
        direct = 12;
        indirect = 18;
    } else { /* RO */
        basic = 4;
        brief = 8;
        direct = 13;
        indirect = 19;
    }
    switch (ea->mode) {
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
        return -1;
    default:
        return -1;
    }
}

/* 040UM Table 10-6 (PDF 317, printed 10-27), Scc column. */
static int scc40_cycles(const ea_desc_t *ea) {
    switch (ea->mode) {
    case 0:
        return 3;
    case 2:
    case 3:
    case 4:
    case 5:
        return 3;
    case 6:
        if (ea->ext_form != EA_EXT_FULL)
            return 9;
        if (ea->index_suppressed && ea->iis == 0)
            return -1;
        switch (ea->iis) {
        case 0:
            return ea->bd_size == 0 ? 12 : 14;
        case 1:
            return 20;
        case 2:
        case 3:
            return 22;
        case 5:
            return 19;
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

/* サイズコード → バイト数のルックアップ（0=byte,1=word,2=long）*/
static const int g_sz[3] = {1, 2, 4};

/* ================================================================== */
/* Scc（条件セット）/ DBcc（デクリメントと分岐）                        */
/* ================================================================== */

/* Scc: 条件が真なら EA を 0xFF に、偽なら 0x00 にセット */
static int op_scc(mc68k_state_t *cpu, uint16_t opcode) {
    int cond = (opcode >> 8) & 0xF;
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg = opcode & 7;
    uint8_t result = eval_cond(cpu->sr, cond) ? 0xFF : 0x00;
    cpu->ea_read_before_write = true;
    ea_write(cpu, ea_mode, ea_reg, 1, (uint32_t)result);
    cpu->ea_read_before_write = false;
    if (cpu->exec_state != CPU_NORMAL)
        return 4;
    if (cpu_is_68040(cpu)) {
        int cycles = scc40_cycles(&cpu->last_ea);
        if (cycles >= 0)
            return cpu_040_tag(cpu, cycles);
    }
    /* [DS10 Table 7-12]: 条件真・Dn直接のみ68010=4（CYC_SCC_TRUE）。
     * 条件偽（4）・メモリ形式（8）は差分なし。 */
    if (ea_mode == 0) {
        if (cpu_is_68030_family(cpu))
            return 4;
        return result ? CYC(cpu, CYC_SCC_TRUE) : 4;
    }
    if (cpu_is_68030_family(cpu))
        return 5 + ea_cycles(cpu, EA_COST_CALC, &cpu->last_ea);
    return 8 + ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
}

/*
 * DBcc Dn, label: 条件が偽のとき Dn.W をデクリメントし、-1 でなければ分岐。
 * DBRA (DBF) はループ構造のコンパイラ出力に頻出。
 */
static int op_dbcc(mc68k_state_t *cpu, uint16_t opcode) {
    int cond = (opcode >> 8) & 0xF;
    int dn = opcode & 7;
    uint32_t disp_base = cpu->pc; /* opcode_addr + 2 */
    int16_t disp = (int16_t)cpu_fetch_word(cpu);
    if (cpu->exec_state != CPU_NORMAL)
        return 4;

    if (eval_cond(cpu->sr, cond)) {
        if (cpu_is_68040(cpu))
            return cpu_040_tag(cpu, 8);    /* 040UM Table 10-5: calculate 4 + execute 4 */
        return CYC(cpu, CYC_DBCC_CC_TRUE); /* 条件成立: デクリメントせず通過 */
    }
    uint16_t count = (uint16_t)(cpu->d[dn] & 0xFFFF) - 1;
    cpu->d[dn] = (cpu->d[dn] & 0xFFFF0000U) | count;
    if (count != 0xFFFF) { /* カウンタが -1 でない: 分岐 */
        cpu->pc = disp_base + (int32_t)disp;
        cpu->flow_changed = true;
        if (cpu_is_68040(cpu))
            return cpu_040_tag(cpu, 6); /* 040UM Table 10-5: calculate 3 + execute 3 */
        return CYC(cpu, CYC_DBCC_BRANCH);
    }
    /* MC68000 はカウンタ満了でも分岐先の1語を先に読む。UM Table 8-9 の 14(3/0) の
     * read 3回（分岐先1語 + 逐次側の再充填2語）に合わせる。 */
    cpu_prefetch_speculative_read(cpu, disp_base + (int32_t)disp);
    if (cpu_is_68040(cpu))
        return cpu_040_tag(cpu, 8);    /* 040UM Table 10-5: calculate 4 + execute 4 */
    return CYC(cpu, CYC_DBCC_EXPIRED); /* カウンタ終了 */
}

/* ================================================================== */
/* シフト / ローテート（ライン E: レジスタ形式）                       */
/* ================================================================== */

/*
 * 8 種類のシフト/ローテートを一つのハンドラで処理する。
 *
 * type: 0=AS（算術）, 1=LS（論理）, 2=ROX（X 経由ローテート）, 3=RO（純ローテート）
 * dr  : 0=右, 1=左
 *
 * CCR 更新規則:
 *   AS/LS: C,X = 最後にシフトアウトしたビット（count=0 なら C=0, X 不変）
 *   RO   : C  = 最後にシフトアウトしたビット。X は変化しない
 *   ROX  : C,X = 最後にシフトアウトしたビット（count=0 なら C=X, X 不変）
 *   AS L : V  = シフト途中で符号が変化した（算術オーバーフロー）
 *
 * ASR は符号ビットを保持しつつ右にシフトするため、常に V=0。
 */
static int op_shift(mc68k_state_t *cpu, uint16_t opcode) {
    int creg = (opcode >> 9) & 7;
    int dr = (opcode >> 8) & 1;
    int sc = (opcode >> 6) & 3;
    int ir = (opcode >> 5) & 1;
    int type = (opcode >> 3) & 3;
    int dn = opcode & 7;

    int size = g_sz[sc];
    int bits = size * 8;
    int count = ir ? (int)(cpu->d[creg] & 63) : (creg ? creg : 8);

    uint32_t sm = size_mask(size);
    uint32_t sb = sign_bit(size);
    uint32_t val = cpu->d[dn] & sm;

    int x_in = (cpu->sr & CCR_X) ? 1 : 0;
    int c_out = 0, x_out = x_in, v_flag = 0;
    uint32_t result = val;

    if (count == 0) {
        /* count=0: ROX のみ C=X、それ以外は C=0。X 不変。*/
        c_out = (type == 2) ? x_in : 0;
    } else {
        switch (type) {
        case 0:       /* AS: 算術シフト */
            if (dr) { /* ASL */
                if (count >= bits) {
                    c_out = (count == bits) ? (int)(val & 1) : 0;
                    result = 0;
                    v_flag = (val != 0) ? 1 : 0;
                } else {
                    c_out = (int)((val >> (bits - count)) & 1);
                    result = (val << count) & sm;
                    /* V: top(count+1)bits が全部 0 か全部 1 でなければオーバーフロー */
                    uint32_t top = val >> (bits - count - 1);
                    uint32_t all1 = (count == 31) ? UINT32_MAX : ((1u << (count + 1)) - 1);
                    v_flag = (top != 0 && top != all1) ? 1 : 0;
                }
                x_out = c_out;
            } else { /* ASR: 符号ビットで埋める */
                if (count >= bits) {
                    c_out = (val & sb) ? 1 : 0;
                    result = (val & sb) ? sm : 0;
                } else {
                    c_out = (int)((val >> (count - 1)) & 1);
                    /* C99 処理系の算術右シフトを利用（符号付き整数で実装） */
                    int32_t sv = (int32_t)(val << (32 - bits)) >> (32 - bits);
                    result = (uint32_t)(sv >> count) & sm;
                }
                x_out = c_out;
                v_flag = 0;
            }
            break;
        case 1:       /* LS: 論理シフト（V 常に 0）*/
            if (dr) { /* LSL */
                if (count >= bits) {
                    c_out = (count == bits) ? (int)(val & 1) : 0;
                    result = 0;
                } else {
                    c_out = (int)((val >> (bits - count)) & 1);
                    result = (val << count) & sm;
                }
            } else { /* LSR */
                if (count >= bits) {
                    c_out = (count == bits) ? (int)((val >> (bits - 1)) & 1) : 0;
                    result = 0;
                } else {
                    c_out = (int)((val >> (count - 1)) & 1);
                    result = (val >> count) & sm;
                }
            }
            x_out = c_out;
            break;
        case 2: { /* ROX: X ビット経由ローテート */
            /* modulo は (bits+1)。X ビットが「ローテートリング」に参加する */
            int eff = count % (bits + 1);
            int x = x_in;
            result = val;
            if (dr) { /* ROXL */
                for (int i = 0; i < eff; i++) {
                    int out = (result & sb) ? 1 : 0;
                    result = ((result << 1) & sm) | (uint32_t)x;
                    x = out;
                }
            } else { /* ROXR */
                for (int i = 0; i < eff; i++) {
                    int out = (int)(result & 1);
                    result = (result >> 1) | ((uint32_t)x << (bits - 1));
                    x = out;
                }
            }
            c_out = x_out = x;
            break;
        }
        case 3: { /* RO: 純循環ローテート（X は変化しない）*/
            int rc = count % bits;
            if (rc == 0) {
                /* 全回転: 値は変わらないが C は更新される */
                result = val;
                c_out = dr ? (int)(val & 1) : (int)((val >> (bits - 1)) & 1);
            } else {
                if (dr) { /* ROL */
                    result = ((val << rc) | (val >> (bits - rc))) & sm;
                    c_out = (int)((val >> (bits - rc)) & 1);
                } else { /* ROR */
                    result = ((val >> rc) | (val << (bits - rc))) & sm;
                    c_out = (int)((val >> (rc - 1)) & 1);
                }
            }
            /* ROL/ROR は X フラグを変更しない（x_out = x_in のまま）*/
            break;
        }
        }
    }

    /* CCR 更新 */
    cpu->sr &= ~(CCR_N | CCR_Z | CCR_V | CCR_C | CCR_X);
    if (!(result & sm))
        cpu->sr |= CCR_Z;
    if (result & sb)
        cpu->sr |= CCR_N;
    if (c_out)
        cpu->sr |= CCR_C;
    if (x_out)
        cpu->sr |= CCR_X;
    if (v_flag)
        cpu->sr |= CCR_V;

    cpu->d[dn] = (cpu->d[dn] & ~sm) | result;
    if (cpu_is_68040(cpu)) {
        if (type == 0)
            return cpu_040_tag(cpu, ir ? (dr ? 5 : 4) : (dr ? 4 : 3));
        if (type == 1)
            return cpu_040_tag(cpu, ir ? 4 : 3);
        if (type == 2)
            return cpu_040_tag(cpu, ir ? 7 : 6);
        return cpu_040_tag(cpu, ir ? 5 : 4);
    }
    if (cpu_is_68030_family(cpu)) {
        /* UM30 11.6.12 の ord 1--3、5--6、8--10、12--13、15。
         * 可変回数 LS/ASR 行はオペランド幅で分かれる。 */
        if (type == 1) /* LS */
            return ir ? (count <= bits ? 6 : 8) : 4;
        if (type == 0) /* AS */
            return ir ? (dr ? 8 : (count <= bits ? 6 : 10)) : (dr ? 6 : 4);
        if (type == 2) /* ROX */
            return 12;
        return ir ? 8 : 6; /* RO */
    }
    /* [UM00 Table 8-15] / [DS10 Table 7-15]: Longの基本時間は8（Word/Byteは6） */
    return (size == 4 ? 8 : 6) + 2 * count;
}

/*
 * メモリシフト形式（常に 1 回、ワードサイズのみ）。
 * type と dr はオペコードの bits 10-8 から決まる。
 * メモリ上のビットマップを 1 ビットずらすときに使う。
 */
static int op_shift_mem(mc68k_state_t *cpu, uint16_t opcode) {
    int type = (opcode >> 9) & 3;
    int dr = (opcode >> 8) & 1;
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg = opcode & 7;

    uint32_t rmw_addr;
    uint32_t val = ea_rmw_read(cpu, ea_mode, ea_reg, 2, &rmw_addr, false);
    if (cpu->exec_state != CPU_NORMAL)
        return 4;

    int x_in = (cpu->sr & CCR_X) ? 1 : 0;
    int c_out, x_out = x_in, v_flag = 0;
    uint32_t result;

    /* メモリシフトは count=1 固定 */
    switch (type) {
    case 0: /* AS */
        if (dr) {
            c_out = (int)((val >> 15) & 1);
            result = (val << 1) & 0xFFFF;
            /* V: 符号ビットが変化した */
            v_flag = ((val >> 15) ^ ((result >> 15))) & 1;
        } else {
            c_out = (int)(val & 1);
            result = (uint16_t)((int16_t)val >> 1);
        }
        x_out = c_out;
        break;
    case 1: /* LS */
        if (dr) {
            c_out = (int)((val >> 15) & 1);
            result = (val << 1) & 0xFFFF;
        } else {
            c_out = (int)(val & 1);
            result = val >> 1;
        }
        x_out = c_out;
        break;
    case 2: /* ROX */
        if (dr) {
            c_out = (int)((val >> 15) & 1);
            result = ((val << 1) & 0xFFFF) | (uint32_t)x_in;
        } else {
            c_out = (int)(val & 1);
            result = (val >> 1) | ((uint32_t)x_in << 15);
        }
        x_out = c_out;
        break;
    default: /* RO */
        if (dr) {
            c_out = (int)((val >> 15) & 1);
            result = ((val << 1) & 0xFFFF) | (uint32_t)c_out;
        } else {
            c_out = (int)(val & 1);
            result = (val >> 1) | ((uint32_t)c_out << 15);
        }
        break;
    }

    cpu->sr &= ~(CCR_N | CCR_Z | CCR_V | CCR_C | CCR_X);
    if (!(result & 0xFFFF))
        cpu->sr |= CCR_Z;
    if (result & 0x8000)
        cpu->sr |= CCR_N;
    if (c_out)
        cpu->sr |= CCR_C;
    if (x_out)
        cpu->sr |= CCR_X;
    if (v_flag)
        cpu->sr |= CCR_V;

    ea_rmw_write(cpu, ea_mode, ea_reg, 2, rmw_addr, result, false);
    if (cpu_is_68040(cpu)) {
        int cycles = shift40_mem_cycles(&cpu->last_ea, type, dr);
        if (cycles >= 0)
            return cpu_040_tag(cpu, cycles);
    }
    if (cpu_is_68030_family(cpu)) {
        int base = type == 0 ? (dr ? 6 : 4) : type == 1 ? 4 : type == 2 ? 4 : 6;
        return base + ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
    }
    return 8 + ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
}

/* ================================================================== */
/* テーブル登録                                                        */
/* ================================================================== */

void register_shift_ops(op_handler_t *table) {
    int cond, ea, i;

    /* ---- Scc / DBcc (ライン 5, bits 7-6 = 11) ---- */
    for (cond = 0; cond < 16; cond++) {
        uint16_t base = (uint16_t)(0x50C0 | (cond << 8));
        for (ea = 0; ea < 64; ea++) {
            if ((ea & 0x38) == 0x08) { /* mode=001 → DBcc */
                table[base | ea] = op_dbcc;
            } else if (ea_is_data_alterable(ea)) {
                table[base | ea] = op_scc;
            }
        }
    }

    /* ---- シフト / ローテート（ライン E: 0xE000-0xEFFF）---- */
    for (i = 0xE000; i <= 0xEFFF; i++) {
        int sz76 = (i >> 6) & 3;
        if (sz76 == 3) {
            /* メモリシフト形式: bits 10-9 = type、dst は Memory Alterable */
            if (i & 0x0800) /* 68020以降のビットフィールド領域 */
                continue;
            if (!ea_is_memory_alterable(i & 0x3F))
                continue;
            table[i] = op_shift_mem;
        } else {
            /* レジスタシフト形式: bits 4-3 = type */
            table[i] = op_shift;
        }
    }
}
