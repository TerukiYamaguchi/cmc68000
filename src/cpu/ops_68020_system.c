#include "cpu_internal.h"

/* 040UM 表 10-6（PDF 308--310、印刷版 10-17--10-19）。CHK2 と CMP2 は
 * 相互に関連した別列を使うため、汎用 EA タイミング API は使えない。
 * このヘルパーは例外なし経路の公称値を返す。 */
static int chk2_40_cycles(const ea_desc_t *ea, bool cmp2) {
    int basic = cmp2 ? 24 : 20;
    int pc16 = cmp2 ? 25 : 21;
    int brief = cmp2 ? 29 : 25;
    int pcbrief = cmp2 ? 30 : 26;
    int direct = cmp2 ? 32 : 28;
    int indirect = cmp2 ? 40 : 36;

    switch (ea->mode) {
    case 2:
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
        return -1;
    default:
        return -1;
    }
}

static int op_link_long(mc68k_state_t *c, uint16_t op) {
    int n = op & 7;
    uint32_t d = ((uint32_t)cpu_fetch_word(c) << 16) | cpu_fetch_word(c);
    if (c->exec_state != CPU_NORMAL)
        return 4;
    c->a[7] -= 4;
    cpu_write_long(c, c->a[7], c->a[n]);
    if (c->exec_state != CPU_NORMAL)
        return 8;
    c->a[n] = c->a[7];
    c->a[7] += (int32_t)d;
    if (cpu_is_68040(c))
        return cpu_040_tag(c, 4); /* 040UM 表 10-5: 計算 3 + 実行基準値 1 */
    return cpu_is_68030_family(c) ? 6 : 16;
}
static int op_trapcc(mc68k_state_t *c, uint16_t op) {
    int cc = (op >> 8) & 15, form = op & 7;
    if (form == 2)
        cpu_fetch_word(c);
    else if (form == 3) {
        cpu_fetch_word(c);
        cpu_fetch_word(c);
    }
    if (c->exec_state != CPU_NORMAL)
        return 4;
    if (!eval_cond(c->sr, cc)) {
        if (cpu_is_68040(c))
            return cpu_040_tag(c, 10); /* 040UM 表 10-5: 計算 5 + 実行 5 */
        return cpu_is_68030_family(c) ? (form == 2 ? 6 : form == 3 ? 8 : 4) : 4;
    }
    c->flow_changed = true;
    if (cpu_is_68040(c))
        return cpu_040_tag(c, cpu_dispatch_exception(c, c->pc, 7, 38));
    return cpu_dispatch_exception(c, c->pc, 7,
                                  cpu_is_68030_family(c) ? (form == 2   ? 24
                                                            : form == 3 ? 26
                                                                        : 22)
                                                         : 34);
}
static void cas_ccr(mc68k_state_t *c, uint32_t src, uint32_t dst, uint32_t result, int sz) {
    uint32_t mask = size_mask(sz), sign = sign_bit(sz), s = src & mask, d = dst & mask,
             res = result & mask;
    c->sr &= ~(CCR_N | CCR_Z | CCR_V | CCR_C);
    if (!res)
        c->sr |= CCR_Z;
    if (res & sign)
        c->sr |= CCR_N;
    if (s > d)
        c->sr |= CCR_C;
    if (((s ^ d) & (d ^ res)) & sign)
        c->sr |= CCR_V;
}
static int op_cas(mc68k_state_t *c, uint16_t op) {
    int sc = (op >> 9) & 3, sz = sc == 1 ? 1 : sc == 2 ? 2 : 4, m = (op >> 3) & 7, r = op & 7;
    uint16_t e = cpu_fetch_word(c);
    if (c->exec_state != CPU_NORMAL)
        return 4;
    int du = (e >> 6) & 7, dc = e & 7;
    uint32_t a;
    /* CAS は、このヘルパーを共有する通常の <ea> RMW 命令と異なり、TAS と同じく
     * 実際にロックされた RMW バスサイクルである。 */
    uint32_t v;
    if (cpu_is_68060(c)) {
        /* 非整列 CAS（word は奇数、long は 4 で割り切れない、byte は対象外 [060UM §7.6]）は
         * operand access より前に vector 61 へ入る。 */
        const uint32_t an_before = c->a[r];
        a = ea_rmw_addr(c, m, r, sz);
        if (c->exec_state != CPU_NORMAL)
            return 4;
        if (sz > 1 && (a & (uint32_t)(sz == 2 ? 1 : 3))) {
            if (m == 3 || m == 4)
                c->a[r] =
                    an_before; /* trap した命令は ISP が再デコードするので、An は更新しない */
            return cpu_dispatch_exception(c, c->instr_pc, 61, 34);
        }
        v = ea_rmw_load(c, sz, a, true);
    } else {
        v = ea_rmw_read(c, m, r, sz, &a, true);
    }
    if (c->exec_state != CPU_NORMAL)
        return 4;
    uint32_t mask = size_mask(sz), cmp = c->d[dc] & mask, res = (v - cmp) & mask;
    cas_ccr(c, cmp, v, res, sz);
    if (v == cmp)
        ea_rmw_write(c, m, r, sz, a, c->d[du], true);
    else
        c->d[dc] = (c->d[dc] & ~mask) | v;
    if (cpu_is_68020(c))
        return (v == cmp ? 15 : 12) +
               ea_cycles_immediate_word(c, EA_COST_CALC_IMM, &c->last_ea);
    if (cpu_is_68030_family(c))
        /* UM30 §11.6.16: ## は即値 EA 計算時間を表す。 */
        return (v == cmp ? 13 : 11) +
               ea_cycles_immediate_word(c, EA_COST_CALC_IMM, &c->last_ea);
    return 12;
}
static int op_chk2(mc68k_state_t *c, uint16_t op) {
    int sc = (op >> 9) & 3, sz = sc == 0 ? 1 : sc == 1 ? 2 : 4, m = (op >> 3) & 7, r = op & 7;
    uint16_t e = cpu_fetch_word(c);
    if (c->exec_state != CPU_NORMAL)
        return 4;
    uint32_t a = ea_get_addr(c, m, r),
             lo = sz == 1   ? cpu_read_byte(c, a)
                  : sz == 2 ? cpu_read_word(c, a)
                            : cpu_read_long(c, a),
             hi = sz == 1   ? cpu_read_byte(c, a + sz)
                  : sz == 2 ? cpu_read_word(c, a + sz)
                            : cpu_read_long(c, a + sz);
    if (c->exec_state != CPU_NORMAL)
        return 4;
    int an = e >> 15, d = (e >> 12) & 7;
    uint32_t v = an ? c->a[d] : c->d[d];
    uint32_t mask = size_mask(sz);
    if (an && sz < 4) {
        lo = (uint32_t)(int32_t)(sz == 1 ? (int8_t)lo : (int16_t)lo);
        hi = (uint32_t)(int32_t)(sz == 1 ? (int8_t)hi : (int16_t)hi);
        mask = 0xffffffffu;
    } else
        v &= mask;
    bool out = (v - lo) > (hi - lo);
    c->sr &= ~(CCR_Z | CCR_C);
    if (v == lo || v == hi)
        c->sr |= CCR_Z;
    if (out)
        c->sr |= CCR_C;
    if ((e & 0x0800) && out) {
        c->flow_changed = true;
        if (cpu_is_68030_family(c))
            return cpu_dispatch_exception(
                c, c->pc, 6,
                CYC(c, CYC_CHK2_EXCEPTION) +
                    ea_cycles_immediate_word(c, EA_COST_FETCH_IMM, &c->last_ea));
        return cpu_dispatch_exception(c, c->pc, 6, 40);
    }
    if (cpu_is_68040(c)) {
        int cycles = chk2_40_cycles(&c->last_ea, !(e & 0x0800));
        if (cycles >= 0) {
            /* 表 10-6 の CHK2 時間は、Dn が範囲内かつ UB > LB の条件による。
             * 範囲の逆転と An 比較はそれぞれ計算・実行段へ 1 クロック加える。 */
            if (e & 0x0800) {
                if (hi < lo)
                    cycles += 6;
                if (an)
                    cycles += 2;
            }
            return cpu_040_tag(c, cycles);
        }
    }
    if (cpu_is_68020(c))
        /* CMP2（bit 11 が 0）と通常の CHK2 は UM20 の基準値を共有する。
         * 比較幅にかかわらず拡張語は #<data>.W である。 */
        return 18 + ea_cycles_immediate_word(c, EA_COST_FETCH_IMM, &c->last_ea);
    if (cpu_is_68030_family(c))
        return (e & 0x0800) ? 18 + ea_cycles_immediate_word(c, EA_COST_FETCH_IMM, &c->last_ea)
                            : 20 + ea_cycles_immediate_word(c, EA_COST_FETCH_IMM, &c->last_ea);
    return 8;
}
/* mc68000#96: フォルトした命令は先頭から再開するため、CAS2 は第 1 オペランドを
 * 格納した後に第 2 オペランドでフォルトしてはならない。先に両方の書込みアドレスを
 * 変換し、変換フォルト時は何も格納せず報告する。検査するのは各オペランドの先頭バイトだけ。 */
static bool cas2_write_probe(mc68k_state_t *c, uint32_t a, const mc68k_access_t *write) {
    uint32_t physical;
    bool cache_inhibit;
    return cpu_translate_execute_access(c, a, write, &physical, &cache_inhibit);
}

static int op_cas2(mc68k_state_t *c, uint16_t op) {
    int sz = (op == 0x0CFC) ? 2 : 4;
    uint16_t e1 = cpu_fetch_word(c), e2 = cpu_fetch_word(c);
    if (c->exec_state != CPU_NORMAL)
        return 4;
    int r1 = (e1 >> 12) & 7, r2 = (e2 >> 12) & 7, dc1 = e1 & 7, dc2 = e2 & 7,
        du1 = (e1 >> 6) & 7, du2 = (e2 >> 6) & 7;
    uint32_t a1 = (e1 & 0x8000) ? c->a[r1] : c->d[r1], a2 = (e2 & 0x8000) ? c->a[r2] : c->d[r2];
    const mc68k_access_t rmw_read = cpu_data_access(c, true, (uint8_t)sz, true);
    const mc68k_access_t rmw_write = cpu_data_access(c, false, (uint8_t)sz, true);
    uint32_t v1 = sz == 2 ? cpu_read_word_access(c, a1, &rmw_read)
                          : cpu_read_long_access(c, a1, &rmw_read),
             v2 = sz == 2 ? cpu_read_word_access(c, a2, &rmw_read)
                          : cpu_read_long_access(c, a2, &rmw_read);
    if (c->exec_state != CPU_NORMAL)
        return 4;
    uint32_t mask = size_mask(sz);
    bool ok1 = (v1 & mask) == (c->d[dc1] & mask), ok2 = (v2 & mask) == (c->d[dc2] & mask);
    c->sr &= ~(CCR_N | CCR_Z | CCR_V | CCR_C);
    if (ok1 && ok2) {
        c->sr |= CCR_Z;
        if ((cpu_is_68030_family(c) || cpu_is_68040(c)) &&
            (!cas2_write_probe(c, a1, &rmw_write) || !cas2_write_probe(c, a2, &rmw_write)))
            return 4;
        if (sz == 2) {
            cpu_write_word_access(c, a1, (uint16_t)c->d[du1], &rmw_write);
            cpu_write_word_access(c, a2, (uint16_t)c->d[du2], &rmw_write);
        } else {
            cpu_write_long_access(c, a1, c->d[du1], &rmw_write);
            cpu_write_long_access(c, a2, c->d[du2], &rmw_write);
        }
    } else {
        c->d[dc1] = (c->d[dc1] & ~mask) | (v1 & mask);
        if (dc2 != dc1)
            c->d[dc2] = (c->d[dc2] & ~mask) | (v2 & mask);
    }
    if (cpu_is_68020(c))
        return (ok1 && ok2) ? 25 : 22;
    return cpu_is_68030_family(c) ? 24 : 20;
}
void register_68020_system_ops(op_handler_t *t) {
    t[0x0CFC] = op_cas2;
    t[0x0EFC] = op_cas2;
    for (int n = 0; n < 8; n++) {
        t[0x4808 | n] = op_link_long;
    }
    for (int cc = 0; cc < 16; cc++) {
        t[0x50FA | (cc << 8)] = op_trapcc;
        t[0x50FB | (cc << 8)] = op_trapcc;
        t[0x50FC | (cc << 8)] = op_trapcc;
    }
    for (int ea = 0; ea < 64; ea++) {
        if (ea_is_memory_alterable(ea)) {
            t[0x0AC0 | ea] = op_cas;
            t[0x0CC0 | ea] = op_cas;
            t[0x0EC0 | ea] = op_cas;
        }
        if (ea_is_control(ea)) {
            t[0x00C0 | ea] = op_chk2;
            t[0x02C0 | ea] = op_chk2;
            t[0x04C0 | ea] = op_chk2;
        }
        /* 68020 以降の TST は PC 相対・イミディエイト（$3A/$3B/$3C）を、
         * ワード/ロングではさらに An（モード001＝$08-$0F）を取れる。
         * バイトサイズの An だけは 68020 でも不可。 */
        if (ea == 0x3a || ea == 0x3b || ea == 0x3c) {
            t[0x4A00 | ea] = op_tst;
        }
        if ((ea >> 3) == 1 || ea == 0x3a || ea == 0x3b || ea == 0x3c) {
            t[0x4A40 | ea] = op_tst;
            t[0x4A80 | ea] = op_tst;
        }
        if (ea == 0x3a || ea == 0x3b) {
            for (int s = 0; s < 3; s++)
                t[0x0C00 | (s << 6) | ea] = op_cmpi;
        }
    }
}
