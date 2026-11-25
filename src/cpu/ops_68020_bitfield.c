/* MC68020 bit-field 命令。メモリ field は MSB 側からアドレス指定する。 */
#include "cpu_internal.h"

/* 040UM Table 10-6 (PDF 306--307, printed 10-15--10-16), Dn forms.
 * offset と width の両方が immediate なら T1、それ以外に register 指定があれば
 * T2 を使う。 */
static int bf40_dn_cycles(int kind, uint16_t ext) {
    const bool variable = (ext & 0x0820u) != 0;
    switch (kind) {
    case 0:
        return variable ? 6 : 4; /* BFTST */
    case 1:
    case 3:
        return variable ? 7 : 5; /* BFEXTU/BFEXTS */
    case 2:
    case 4:
    case 6:
        return variable ? 11 : 9; /* change/clear/set */
    case 5:
        return variable ? 11 : 9; /* BFFFO */
    case 7:
        return variable ? 9 : 7; /* BFINS */
    default:
        return -1;
    }
}

/* 040UM Table 10-6 (PDF 306--307, printed 10-15--10-16), memory forms.
 * 表の境界条件は、機能上の byte access の境界ではなく long-word 境界に基づく。 */
static int bf40_memory_cycles(int kind, const ea_desc_t *ea, uint32_t offset, uint32_t width) {
    int base, brief, direct;
    int span_add = 0;
    switch (kind) {
    case 2:
    case 4:
    case 6:
        base = 17;
        brief = 21;
        direct = 26;
        span_add = 19;
        break;
    case 5:
        base = 18;
        brief = 22;
        direct = 27;
        span_add = 2;
        break;
    case 7:
        base = 16;
        brief = 20;
        direct = 25;
        span_add = 14;
        break;
    case 0:
    case 1:
    case 3:
        base = 16;
        brief = 20;
        direct = 25;
        span_add = (kind == 1 || kind == 3) ? 2 : 0;
        break;
    default:
        return -1;
    }

    switch (ea->mode) {
    case 2:
    case 5:
        break;
    case 6:
        if (ea->ext_form != EA_EXT_FULL)
            base = brief;
        else {
            if (ea->index_suppressed && ea->iis == 0)
                return -1;
            switch (ea->iis) {
            case 0:
                base = ea->bd_size == 0 ? direct : direct + 2;
                break;
            case 1:
            case 5:
                base = direct + 6;
                break;
            case 2:
            case 3:
                base = direct + 8;
                break;
            case 6:
                base = direct + 6;
                break;
            case 7:
                base = direct + 8;
                break;
            default:
                return -1;
            }
        }
        break;
    case 7:
        if (ea->reg == 0 || ea->reg == 1)
            break;
        if (ea->reg == 2 || ea->reg == 3) {
            if (ea->reg == 2)
                base++;
            else if (ea->ext_form != EA_EXT_FULL)
                base = brief + 1;
            else {
                if (ea->index_suppressed && ea->iis == 0)
                    return -1;
                switch (ea->iis) {
                case 0:
                    base = ea->bd_size == 0 ? direct : direct + 2;
                    break;
                case 1:
                case 5:
                    base = direct + 6;
                    break;
                case 2:
                case 3:
                    base = direct + 8;
                    break;
                case 6:
                    base = direct + 6;
                    break;
                case 7:
                    base = direct + 8;
                    break;
                default:
                    return -1;
                }
            }
        } else
            return -1;
        break;
    default:
        return -1;
    }
    if (((offset & 31u) + width) > 32u)
        base += span_add;
    return base;
}

static int op_bf(mc68k_state_t *c, uint16_t op) {
    int kind = (op >> 8) & 7, m = (op >> 3) & 7, r = op & 7;
    uint16_t e = cpu_fetch_word(c);
    if (c->exec_state != CPU_NORMAL)
        return 4;
    int dst = (e >> 12) & 7;
    uint32_t off = (e & 0x0800) ? c->d[(e >> 6) & 7] : (e >> 6) & 31;
    uint32_t wid = (e & 0x20) ? c->d[e & 7] : (e & 31);
    wid = (wid & 31);
    if (!wid)
        wid = 32;
    /* 060 の timing: field が 5 byte にまたがるか（[060UM Table 10-16]。Dn の列にも =5 Bytes
     * がある）。 */
    c->t060_aux = ((int32_t)off % 8 + 8) % 8 + (int)wid > 32;
    uint32_t field = 0;
    /* メモリオペランドの実効アドレスは読み出しと書き戻しで共有する。
     * ea_get_addr() は EA の拡張ワードをフェッチして PC を進めるため、
     * 同じ命令で2回呼ぶと次の命令を丸ごと取りこぼす。 */
    uint32_t addr = 0;
    int bit = 0;
    if (m != 0) {
        int32_t signed_off = (int32_t)off;
        addr = ea_get_addr(c, m, r) + (signed_off >> 3);
        bit = signed_off & 7;
    }
    if (m == 0) {
        for (uint32_t i = 0; i < wid; i++)
            field = (field << 1) | ((c->d[r] >> (31 - ((off + i) & 31))) & 1);
    } else {
        /* BFCHG/BFCLR/BFSET/BFINS (kind>=2) は書き戻すが、独立した read cycle と
         * 後続の独立した write cycle であり locked RMW bus cycle ではない。
         * locked を設定するのは TAS/CAS/CAS2 だけである。 */
        const mc68k_access_t read_access = cpu_data_access(c, true, 1, false);
        for (uint32_t i = 0; i < wid; i++) {
            uint8_t b = cpu_read_byte_access(c, addr + (bit + i) / 8, &read_access);
            field = (field << 1) | ((b >> (7 - ((bit + i) & 7))) & 1);
        }
        if (c->exec_state != CPU_NORMAL)
            return 4;
    }
    uint32_t mask = wid == 32 ? 0xffffffffu : ((1u << wid) - 1);
    uint32_t cc = (kind == 7) ? (c->d[dst] & mask) : field;
    c->sr &= ~(CCR_N | CCR_Z | CCR_V | CCR_C);
    if (!cc)
        c->sr |= CCR_Z;
    if (cc & (1u << (wid - 1)))
        c->sr |= CCR_N;
    if (kind == 1)
        c->d[dst] = field;
    else if (kind == 3)
        c->d[dst] = (uint32_t)((int32_t)(field << (32 - wid)) >> (32 - wid));
    else if (kind == 5) {
        uint32_t i;
        for (i = 0; i < wid && !((field >> (wid - 1 - i)) & 1); i++) {
        }
        c->d[dst] = off + i;
    } else if (kind >= 2) {
        uint32_t nv = kind == 2   ? (field ^ mask)
                      : kind == 4 ? 0
                      : kind == 6 ? mask
                                  : (c->d[dst] & mask);
        if (m == 0) {
            for (uint32_t i = 0; i < wid; i++) {
                uint32_t b = 1u << (31 - ((off + i) & 31));
                if ((nv >> (wid - 1 - i)) & 1)
                    c->d[r] |= b;
                else
                    c->d[r] &= ~b;
            }
        } else {
            const mc68k_access_t rmw_read = cpu_data_access(c, true, 1, false);
            const mc68k_access_t rmw_write = cpu_data_access(c, false, 1, false);
            for (uint32_t i = 0; i < wid; i++) {
                uint32_t p = (uint32_t)bit + i;
                uint8_t b = cpu_read_byte_access(c, addr + p / 8, &rmw_read);
                uint8_t bm = (uint8_t)(1u << (7 - (p & 7)));
                if ((nv >> (wid - 1 - i)) & 1)
                    b |= bm;
                else
                    b &= (uint8_t)~bm;
                cpu_write_byte_access(c, addr + p / 8, b, &rmw_write);
                if (c->exec_state != CPU_NORMAL)
                    return 4;
            }
        }
    }
    if (cpu_is_68040(c) && m == 0)
        return cpu_040_tag(c, bf40_dn_cycles(kind, e));
    if (cpu_is_68040(c) && m != 0) {
        int cycles = bf40_memory_cycles(kind, &c->last_ea, off, wid);
        if (cycles >= 0)
            return cpu_040_tag(c, cycles);
    }
    if (cpu_is_68020(c)) {
        /* UM20 8.2.14 Cache Case。extension word は常に1 wordで、5-byte column を
         * 選ぶのは memory field の span だけである。 */
        static const uint8_t dn_cycles[8] = {6, 8, 12, 8, 12, 18, 12, 10};
        static const uint8_t mem4_cycles[8] = {11, 13, 16, 13, 16, 24, 16, 14};
        static const uint8_t mem5_cycles[8] = {15, 18, 24, 18, 24, 32, 24, 20};
        if (m == 0)
            return dn_cycles[kind];
        /* op_bf() は memory に符号付き offset を使う。負の dynamic offset に対しても
         * 数学上の modulo 定義を保つ。 */
        int start_bit = ((int32_t)off % 8 + 8) % 8;
        int base = start_bit + (int)wid > 32 ? mem5_cycles[kind] : mem4_cycles[kind];
        return base + ea_cycles_immediate_word(c, EA_COST_CALC_IMM, &c->last_ea);
    }
    if (cpu_is_68030_family(c)) {
        /* UM30 §11.6.14 I-Cache Case。他の timing table と異なり、この表の '*' は
         * Add Calculate Immediate EA Time (CIEA) を意味する。 */
        static const uint8_t dn_cycles[8] = {8, 10, 14, 10, 14, 20, 14, 12};
        static const uint8_t mem4_cycles[8] = {10, 12, 14, 12, 14, 22, 14, 12};
        static const uint8_t mem5_cycles[8] = {14, 18, 22, 18, 22, 28, 22, 18};
        if (m == 0)
            return dn_cycles[kind];
        int start_bit = ((int32_t)off % 8 + 8) % 8;
        int base = start_bit + (int)wid > 32 ? mem5_cycles[kind] : mem4_cycles[kind];
        return base + ea_cycles_immediate_word(c, EA_COST_CALC_IMM, &c->last_ea);
    }
    return 8;
}
void register_68020_bitfield_ops(op_handler_t *t) {
    for (int k = 0; k < 8; k++)
        for (int ea = 0; ea < 64; ea++)
            /* Dn は全8本が対象（モード000・レジスタ0-7）。D0 だけ登録すると
             * BFEXTU D2{8:24},D2 のような命令が不当に ILLEGAL になる。 */
            if (ea < 8 ||
                ((k == 0 || k == 1 || k == 3 || k == 5) ? ea_is_control(ea)
                                                        : ea_is_control_alterable(ea)))
                t[0xE8C0 | (k << 8) | ea] = op_bf;
}
