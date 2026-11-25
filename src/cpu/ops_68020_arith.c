/* MC68020 の算術命令追加分。 */
#include "cpu_internal.h"

/* 040UM 表 10-6（PDF 311、316、印刷版 10-20、10-25）。
 * MUL.L と DIV.L は専用のロングオペランド列を使う。実行先行分は除外し、
 * 表 10-2 のソースフェッチは保持する。 */
static int muldiv40_long_cycles(const ea_desc_t *ea, bool div) {
    const int dn = div ? 53 : 21;
    const int memory = div ? 54 : 22;
    const int d16 = div ? 56 : 23;
    const int pc16 = div ? 57 : 24;
    const int immediate = div ? 54 : 21;
    const int brief = div ? 60 : 26;
    const int pcbrief = div ? 61 : 29;
    const int direct = div ? 63 : 31;
    const int indirect = div ? 72 : 38;

    switch (ea->mode) {
    case 0:
        return dn;
    case 2:
    case 3:
    case 4:
        return memory;
    case 5:
        return d16;
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
            return d16;
        if (ea->reg == 2)
            return pc16;
        if (ea->reg == 3) {
            if (ea->ext_form != EA_EXT_FULL)
                return pcbrief;
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
            return immediate;
        return -1;
    default:
        return -1;
    }
}

static int op_extb(mc68k_state_t *cpu, uint16_t op) {
    int dn = op & 7;
    cpu->d[dn] = (uint32_t)(int32_t)(int8_t)cpu->d[dn];
    ccr_logic(cpu, cpu->d[dn], 4);
    if (cpu_is_68040(cpu))
        return cpu_040_tag(cpu, 2); /* 040UM 表 10-5: 計算 1 + 実行 1 */
    return 4;
}

static int op_chk_long(mc68k_state_t *cpu, uint16_t op) {
    int dn = (op >> 9) & 7, mode = (op >> 3) & 7, reg = op & 7;
    int32_t bound = (int32_t)ea_read(cpu, mode, reg, 4), val = (int32_t)cpu->d[dn];
    if (cpu->exec_state != CPU_NORMAL)
        return 4;
    if (val >= 0 && val <= bound && cpu_is_68040(cpu)) {
        int cycles = chk40_in_bounds_cycles(&cpu->last_ea);
        if (cycles >= 0)
            return cpu_040_tag(cpu, cycles);
    }
    if (val >= 0 && val <= bound)
        return (cpu_is_68020(cpu) || cpu_is_68030_family(cpu))
                   ? 8 + ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea)
                   : 10;
    cpu->sr &= ~CCR_N;
    if (val < 0)
        cpu->sr |= CCR_N;
    cpu->flow_changed = true;
    if (cpu_is_68030_family(cpu)) {
        cyc_class_t cls = mode == 0 ? CYC_CHK_EXCEPTION_DN : CYC_CHK_EXCEPTION_EA;
        return cpu_dispatch_exception(cpu, cpu->pc, 6,
                                      CYC(cpu, cls) +
                                          ea_cycles(cpu, cyc_ea_kind(cpu, cls), &cpu->last_ea));
    }
    return cpu_dispatch_exception(cpu, cpu->pc, 6, 40);
}
static int op_muldiv_long(mc68k_state_t *c, uint16_t op) {
    int m = (op >> 3) & 7, r = op & 7;
    uint16_t e = cpu_fetch_word(c);
    if (c->exec_state != CPU_NORMAL)
        return 4;
    bool sign = e & 0x0800, wide = e & 0x0400, div = op & 0x40;
    if (cpu_is_68060(c) && wide)
        return cpu_dispatch_exception(c, c->instr_pc, 61, 34);
    uint32_t v = ea_read(c, m, r, 4);
    if (c->exec_state != CPU_NORMAL)
        return 4;
    int q = (e >> 12) & 7, h = e & 7;
    if (!div) {
        uint64_t z = sign ? (uint64_t)((int64_t)(int32_t)c->d[q] * (int64_t)(int32_t)v)
                          : (uint64_t)c->d[q] * v;
        c->d[q] = (uint32_t)z;
        if (wide)
            c->d[h] = (uint32_t)(z >> 32);
        ccr_logic(c, c->d[q], 4);
        if (cpu_is_68040(c)) {
            int cycles = muldiv40_long_cycles(&c->last_ea, false);
            if (cycles >= 0)
                return cpu_040_tag(c, cycles);
        }
        if (cpu_is_68020(c))
            return 43 + ea_cycles_immediate_word(c, EA_COST_FETCH_IMM, &c->last_ea);
        if (cpu_is_68030_family(c))
            return 44 + ea_cycles_immediate_word(c, EA_COST_FETCH_IMM, &c->last_ea);
        return 20;
    }
    if (!v)
        return cpu_dispatch_exception(c, c->pc, 5, 38);
    /* UM30 11.6.8 では DIV.L Dn,Dn 行に FIEA 印はなく、FIEA を加えるのは
     * EA,Dn 行だけである。UM20 は既存の計算を維持する。 */
    int div30_ea = m == 0 ? 0 : ea_cycles_immediate_word(c, EA_COST_FETCH_IMM, &c->last_ea);
    if (sign) {
        int64_t n = wide ? (int64_t)(int32_t)c->d[h] * INT64_C(4294967296) + (uint32_t)c->d[q]
                         : (int32_t)c->d[q],
                d = (int32_t)v;
        int64_t z = n / d;
        if (z > INT32_MAX || z < INT32_MIN) {
            c->sr |= CCR_V;
            if (cpu_is_68040(c)) {
                int cycles = muldiv40_long_cycles(&c->last_ea, true);
                if (cycles >= 0)
                    return cpu_040_tag(c, cycles);
            }
            if (cpu_is_68020(c))
                return 90 + ea_cycles_immediate_word(c, EA_COST_FETCH_IMM, &c->last_ea);
            if (cpu_is_68030_family(c))
                return 90 + div30_ea;
            return 20;
        }
        c->d[q] = (uint32_t)z;
        if (h != q)
            c->d[h] = (uint32_t)(n % d);
    } else {
        uint64_t n = wide ? ((uint64_t)c->d[h] << 32) | c->d[q] : c->d[q], z = n / v;
        if (z > UINT32_MAX) {
            c->sr |= CCR_V;
            if (cpu_is_68040(c)) {
                int cycles = muldiv40_long_cycles(&c->last_ea, true);
                if (cycles >= 0)
                    return cpu_040_tag(c, cycles);
            }
            if (cpu_is_68020(c))
                return 78 + ea_cycles_immediate_word(c, EA_COST_FETCH_IMM, &c->last_ea);
            if (cpu_is_68030_family(c))
                return 78 + div30_ea;
            return 20;
        }
        c->d[q] = (uint32_t)z;
        if (h != q)
            c->d[h] = (uint32_t)(n % v);
    }
    ccr_logic(c, c->d[q], 4);
    if (cpu_is_68040(c)) {
        int cycles = muldiv40_long_cycles(&c->last_ea, true);
        if (cycles >= 0)
            return cpu_040_tag(c, cycles);
    }
    if (cpu_is_68020(c))
        return (sign ? 90 : 78) + ea_cycles_immediate_word(c, EA_COST_FETCH_IMM, &c->last_ea);
    if (cpu_is_68030_family(c))
        return (sign ? 90 : 78) + div30_ea;
    return 20;
}

static int op_pack(mc68k_state_t *cpu, uint16_t op) {
    int dst = (op >> 9) & 7, src = op & 7, mem = (op >> 3) & 1;
    uint16_t adjust = cpu_fetch_word(cpu);
    if (cpu->exec_state != CPU_NORMAL)
        return 4;
    uint16_t value;
    if (mem) {
        cpu->a[src] -= (src == 7 ? 2 : 1);
        uint8_t lo = cpu_read_byte(cpu, cpu->a[src]);
        cpu->a[src] -= (src == 7 ? 2 : 1);
        uint8_t hi = cpu_read_byte(cpu, cpu->a[src]);
        if (cpu->exec_state != CPU_NORMAL)
            return 8;
        value = (uint16_t)((hi << 8) | lo);
        value = (uint16_t)(value + adjust);
        cpu->a[dst] -= (dst == 7 ? 2 : 1);
        cpu_write_byte(cpu, cpu->a[dst], (uint8_t)((value & 0x0F) | ((value >> 4) & 0xF0)));
    } else {
        value = (uint16_t)(cpu->d[src] + adjust);
        cpu->d[dst] = (cpu->d[dst] & ~0xFFu) | ((value & 0x0F) | ((value >> 4) & 0xF0));
    }
    if (cpu_is_68020(cpu))
        return mem ? 13 : 6;
    if (cpu_is_68040(cpu))
        return cpu_040_tag(cpu, mem ? 6 : 4); /* 040UM 表 10-5: メモリ / レジスタ PACK */
    if (cpu_is_68030_family(cpu))
        return mem ? 11 : 6;
    return 8;
}

static int op_unpk(mc68k_state_t *cpu, uint16_t op) {
    int dst = (op >> 9) & 7, src = op & 7, mem = (op >> 3) & 1;
    uint16_t adjust = cpu_fetch_word(cpu);
    if (cpu->exec_state != CPU_NORMAL)
        return 4;
    uint8_t packed;
    if (mem) {
        cpu->a[src] -= (src == 7 ? 2 : 1);
        packed = cpu_read_byte(cpu, cpu->a[src]);
    } else
        packed = (uint8_t)cpu->d[src];
    if (cpu->exec_state != CPU_NORMAL)
        return 8;
    uint16_t value = (uint16_t)(((packed & 0xF0) << 4) | (packed & 0x0F));
    value = (uint16_t)(value + adjust);
    if (mem) {
        cpu->a[dst] -= (dst == 7 ? 2 : 1);
        cpu_write_byte(cpu, cpu->a[dst], (uint8_t)value);
        cpu->a[dst] -= (dst == 7 ? 2 : 1);
        cpu_write_byte(cpu, cpu->a[dst], (uint8_t)(value >> 8));
    } else
        cpu->d[dst] = (cpu->d[dst] & ~0xFFFFu) | value;
    if (cpu_is_68020(cpu))
        return mem ? 13 : 8;
    if (cpu_is_68040(cpu))
        return cpu_040_tag(cpu, mem ? 7 : 5); /* 040UM 表 10-5: メモリ / レジスタ UNPK */
    if (cpu_is_68030_family(cpu))
        return mem ? 11 : 8;
    return 8;
}

void register_68020_arith_ops(op_handler_t *t) {
    for (int n = 0; n < 8; n++)
        t[0x49C0 | n] = op_extb;
    for (int ea = 0; ea < 64; ea++)
        if (ea_is_data(ea)) {
            for (int n = 0; n < 8; n++)
                t[0x4100 | (n << 9) | ea] = op_chk_long;
            t[0x4C00 | ea] = op_muldiv_long;
            t[0x4C40 | ea] = op_muldiv_long;
        }
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            t[0x8140 | (y << 9) | x] = op_pack;
            t[0x8148 | (y << 9) | x] = op_pack;
            t[0x8180 | (y << 9) | x] = op_unpk;
            t[0x8188 | (y << 9) | x] = op_unpk;
        }
}
