/*
 * ops_transfer.c — データ転送・レジスタ操作系命令ハンドラ実装
 *
 * 実装する命令:
 *   - MOVEQ              : 8bit 即値を 32bit 符号拡張、4 サイクルで高速
 *   - MOVE SR⇔EA         : ステータスレジスタの読み書き
 *   - LEA / PEA          : 実効アドレスのロード・プッシュ
 *   - SWAP / EXT         : レジスタ内ビット操作
 *   - MOVEM              : 複数レジスタ一括転送
 *   - LINK / UNLK        : C 言語スタックフレームの構築・解放
 *   - EXG                : レジスタ交換
 *   - MOVEP              : 8bit 周辺機器インターフェース用交互バイト転送
 */
#include "cpu_internal.h"
#include <stdint.h>

/* 040UM Table 10-6 (PDF 313, printed 10-22): MOVE SR,<ea>. */
static int move40_from_sr_cycles(const ea_desc_t *ea) {
    switch (ea->mode) {
    case 0:
    case 2:
    case 3:
    case 4:
    case 5:
        return 4;
    case 6:
        if (ea->ext_form != EA_EXT_FULL)
            return 9;
        if (ea->index_suppressed && ea->iis == 0)
            return -1;
        switch (ea->iis) {
        case 0:
            return ea->bd_size == 0 ? 12 : 14;
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

/* 040UM 表 10-6（PDF 313、印刷版 10-22）の MOVE <ea>,SR 最小時間。
 * 表には null ベースディスプレースメントかつ有効インデックスの項目がない。 */
static int move40_to_sr_cycles(const ea_desc_t *ea) {
    switch (ea->mode) {
    case 0:
        return 17;
    case 2:
    case 3:
    case 4:
    case 5:
        return 19;
    case 6:
        if (ea->ext_form != EA_EXT_FULL)
            return 23;
        if ((ea->index_suppressed && ea->iis == 0) || ea->bd_size == 0)
            return -1;
        switch (ea->iis) {
        case 0:
            return 28;
        case 1:
        case 5:
            return 35;
        case 2:
        case 3:
        case 6:
        case 7:
            return 37;
        default:
            return -1;
        }
    case 7:
        if (ea->reg == 0 || ea->reg == 1)
            return 19;
        if (ea->reg == 2)
            return 20;
        if (ea->reg == 3) {
            if (ea->ext_form != EA_EXT_FULL)
                return 24;
            if ((ea->index_suppressed && ea->iis == 0) || ea->bd_size == 0)
                return -1;
            switch (ea->iis) {
            case 0:
                return 28;
            case 1:
            case 5:
                return 35;
            case 2:
            case 3:
            case 6:
            case 7:
                return 37;
            default:
                return -1;
            }
        }
        if (ea->reg == 4)
            return 17;
        return -1;
    default:
        return -1;
    }
}

/* ================================================================== */
/* MOVEQ（ライン 7: 0x7000-0x7FFF, bit 8=0）                          */
/* ================================================================== */

/*
 * MOVEQ #<data>, Dn
 * 8bit 即値を 32bit 符号拡張してデータレジスタに代入し、CCR を更新する。
 */
static int op_moveq(mc68k_state_t *cpu, uint16_t opcode) {
    int dn = (opcode >> 9) & 7;
    int8_t d8 = (int8_t)(opcode & 0xFF);
    cpu->d[dn] = (uint32_t)(int32_t)d8;
    ccr_logic(cpu, cpu->d[dn], 4);
    if (cpu_is_68040(cpu))
        return cpu_040_tag(cpu, 2); /* 040UM Table 10-5: calculate 1 + execute 1 */
    return (cpu_is_68020(cpu) || cpu_is_68030_family(cpu)) ? 2 : 4;
}

/* ================================================================== */
/* MOVE SR, EA (0x40C0) / MOVE EA, SR (0x46C0)                        */
/* ================================================================== */

/* MOVE SR, EA: SR の内容を EA に書き出す（読み出し専用の状態取得）。
 * 無印 MC68000 では非特権だが、MC68010 で特権命令化される
 * （ops_system_ext.c の op_move_from_sr_priv() が特権チェック付きで上書き登録する）。
 * 他 TU から呼ぶため非 static。 */
int op_move_from_sr(mc68k_state_t *cpu, uint16_t opcode) {
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg = opcode & 7;
    cpu->ea_read_before_write = true;
    ea_write(cpu, ea_mode, ea_reg, 2, (uint32_t)cpu->sr);
    cpu->ea_read_before_write = false;
    if (cpu->exec_state != CPU_NORMAL)
        return 4;
    if (cpu_is_68040(cpu)) {
        int cycles = move40_from_sr_cycles(&cpu->last_ea);
        if (cycles >= 0)
            return cpu_040_tag(cpu, cycles);
    }
    /* [UM00 Table 8-21] / [DS10 Table 7-21]: レジスタ直接は68000=6、68010=4
     * （CYC_MOVE_FROM_SR_REG）。メモリ形式（8）は差分なし。 */
    if (cpu_is_68020(cpu))
        return ea_mode == 0 ? 4 : 5 + ea_cycles(cpu, EA_COST_CALC, &cpu->last_ea);
    if (cpu_is_68030_family(cpu))
        return ea_mode == 0 ? 4 : 4 + ea_cycles(cpu, EA_COST_CALC, &cpu->last_ea);
    if (ea_mode == 0)
        return CYC(cpu, CYC_MOVE_FROM_SR_REG);
    return 8 + ea_cycles(cpu, cyc_ea_kind(cpu, CYC_MOVE_FROM_SR_REG), &cpu->last_ea);
}

/* MOVE EA, SR: EA の内容を SR に書き込む（スーパーバイザ特権命令）*/
static int op_move_to_sr(mc68k_state_t *cpu, uint16_t opcode) {
    if (!(cpu->sr & SR_S))
        return cpu_privilege_violation(cpu); /* 特権違反: EA未読み出し */
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg = opcode & 7;
    uint32_t val = ea_read(cpu, ea_mode, ea_reg, 2);
    if (cpu->exec_state != CPU_NORMAL)
        return 4;
    cpu_set_sr(cpu, (uint16_t)val);
    cpu->flow_changed = true;
    if (cpu_is_68040(cpu)) {
        int cycles = move40_to_sr_cycles(&cpu->last_ea);
        if (cycles >= 0)
            return cpu_040_tag(cpu, cycles);
    }
    if (cpu_is_68020(cpu))
        return 8 + ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
    if (cpu_is_68030_family(cpu))
        return 8 + ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
    return ea_mode == 0 ? 12 : 12 + ea_cycles(cpu, EA_COST_FETCH, &cpu->last_ea);
}

/* ================================================================== */
/* LEA / PEA                                                           */
/* ================================================================== */

/* UM00 Table 8-10 / DS10 Table 7-19.  LEA and PEA use the same
 * control-EA column; it is distinct from the JMP/JSR JEA column. */
static int control_ea_cycles_00(int mode, int reg) {
    static const uint8_t costs[] = {0, 0, 0, 0, 0, 4, 8, 4, 8, 4, 8};
    unsigned row = mode < 7 ? (unsigned)mode : (unsigned)(7 + reg);
    return row < sizeof(costs) ? costs[row] : 0;
}

/* 040UM Table 10-6 (PDF 312/317, printed 10-21/10-26).  LEA and PEA use
 * control EAs only; their printed execute column already accounts for the
 * address-generation work, so no generic EA contribution is added. */
static int control40_cycles(const ea_desc_t *ea, int pea) {
    int an = pea ? 3 : 2;
    int d16 = pea ? 3 : 3;
    int pc16 = pea ? 5 : 5;
    int brief = pea ? 7 : 8;
    int pcbrief = pea ? 10 : 9;
    int direct = pea ? 12 : 11;
    int indirect = pea ? 18 : 17;
    switch (ea->mode) {
    case 2:
        return an;
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
            return an;
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

/* LEA EA, An: 実効アドレスをアドレスレジスタにロードする。 */
static int op_lea(mc68k_state_t *cpu, uint16_t opcode) {
    int an = (opcode >> 9) & 7;
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg = opcode & 7;
    uint32_t addr = ea_get_addr(cpu, ea_mode, ea_reg);
    if (cpu->exec_state != CPU_NORMAL)
        return 4;
    cpu->a[an] = addr;
    if (cpu_is_68040(cpu)) {
        int cycles = control40_cycles(&cpu->last_ea, 0);
        if (cycles >= 0)
            return cpu_040_tag(cpu, cycles);
    }
    if (!cpu_has_68020_isa(cpu))
        return 4 + control_ea_cycles_00(ea_mode, ea_reg);
    if (cpu_is_68020(cpu))
        return 2 + ea_cycles(cpu, EA_COST_CALC, &cpu->last_ea);
    if (cpu_is_68030_family(cpu))
        return 2 + ea_cycles(cpu, EA_COST_CALC, &cpu->last_ea);
    return (ea_mode == 2) ? 4 : 8;
}

/* PEA EA: 実効アドレスをスタックにプッシュする。 */
static int op_pea(mc68k_state_t *cpu, uint16_t opcode) {
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg = opcode & 7;
    uint32_t addr = ea_get_addr(cpu, ea_mode, ea_reg);
    if (cpu->exec_state != CPU_NORMAL)
        return 4;
    cpu->a[7] -= 4;
    cpu_write_long(cpu, cpu->a[7], addr);
    if (cpu->exec_state != CPU_NORMAL)
        return 8;
    if (cpu_is_68040(cpu)) {
        int cycles = control40_cycles(&cpu->last_ea, 1);
        if (cycles >= 0)
            return cpu_040_tag(cpu, cycles);
    }
    if (!cpu_has_68020_isa(cpu))
        return 12 + control_ea_cycles_00(ea_mode, ea_reg);
    if (cpu_is_68020(cpu))
        return 5 + ea_cycles(cpu, EA_COST_CALC, &cpu->last_ea);
    return cpu_is_68030_family(cpu) ? 4 + ea_cycles(cpu, EA_COST_CALC, &cpu->last_ea) : 12;
}

/* ================================================================== */
/* SWAP / EXT                                                          */
/* ================================================================== */

/* SWAP.W Dn: 32bit レジスタの上位ワードと下位ワードを交換し、CCR を更新する。 */
static int op_swap(mc68k_state_t *cpu, uint16_t opcode) {
    int dn = opcode & 7;
    uint32_t v = cpu->d[dn];
    cpu->d[dn] = (v >> 16) | (v << 16);
    ccr_logic(cpu, cpu->d[dn], 4);
    return cpu_is_68040(cpu) ? cpu_040_tag(cpu, 2)
                             : 4; /* 040UM Table 10-5: calculate 1 + execute 1 */
}

/*
 * EXT.W Dn / EXT.L Dn: バイトをワードへ、またはワードをロングに符号拡張する。
 * bit 6: 0=EXT.W（byte→word）, 1=EXT.L（word→long）
 */
static int op_ext(mc68k_state_t *cpu, uint16_t opcode) {
    int dn = opcode & 7;
    int sz = (opcode >> 6) & 1; /* 0=word, 1=long */
    if (sz == 0) {
        /* EXT.W: 下位バイトを符号拡張してワードに */
        int8_t b = (int8_t)(cpu->d[dn] & 0xFF);
        cpu->d[dn] = (cpu->d[dn] & 0xFFFF0000) | (uint16_t)(int16_t)b;
        ccr_logic(cpu, cpu->d[dn] & 0xFFFF, 2);
    } else {
        /* EXT.L: 下位ワードを符号拡張してロングに */
        int16_t w = (int16_t)(cpu->d[dn] & 0xFFFF);
        cpu->d[dn] = (uint32_t)(int32_t)w;
        ccr_logic(cpu, cpu->d[dn], 4);
    }
    if (cpu_is_68040(cpu))
        return cpu_040_tag(cpu, sz == 0 ? 3 : 2); /* 040UM Table 10-5: EXT.W / EXT.L */
    return 4;
}

/* ================================================================== */
/* MOVEM（複数レジスタ一括転送）                                       */
/* ================================================================== */

/*
 * MOVEM のレジスタリストマスク（16bit）の bit-レジスタ対応は転送方向で異なる。
 *
 * プレデクリメント -(An) の reg→メモリ:
 *   bit 0=A7, 1=A6, ..., 8=D7, ..., 15=D0（逆順）
 *
 * それ以外（コントロールモード reg→メモリ、および全モードの mem→レジスタ）:
 *   bit 0=D0, 1=D1, ..., 8=A0, ..., 15=A7
 *
 * プレデクリメントのみ逆順なのは、mem→レジスタ側の復元順序（D0 が先）と
 * 対称にするため。
 */
static bool movem_040_can_continue(int ea_mode, int ea_reg) {
    return ea_mode == 6 || (ea_mode == 7 && (ea_reg == 2 || ea_reg == 3));
}

static void movem_040_capture_fault(mc68k_state_t *cpu, int ea_mode, int ea_reg,
                                    uint32_t effective_address) {
    if (!cpu_is_68040(cpu) || !movem_040_can_continue(ea_mode, ea_reg) ||
        cpu->exec_state != CPU_EXCEPTION || cpu->fault_vector != 2)
        return;
    /* Keep the ATC bit an MMU translation failure already recorded; MA (bit 11)
     * is not copied. */
    uint16_t prev_atc = cpu->fault040.valid ? (uint16_t)(cpu->fault040.ssw & 0x0400u) : 0u;
    cpu->fault040.valid = true;
    cpu->fault040.effective_address = effective_address;
    cpu->fault040.ssw = (uint16_t)(0x1000u | prev_atc | (cpu->fault_access.rmw ? 0x0200u : 0u) |
                                   (cpu->addr_err_read ? 0x0100u : 0u) |
                                   ((uint16_t)(cpu_noted_size_code(cpu) & 3u) << 5) |
                                   (cpu->fault_access.fc & 7u));
    cpu->fault040.fault_address = cpu->addr_err_addr;
    cpu->movem040_continuation_valid = true;
    cpu->movem040_instr_pc = cpu->instr_pc;
    cpu->movem040_effective_address = effective_address;
}

static bool movem_040_resume(const mc68k_state_t *cpu) {
    return cpu_is_68040(cpu) && cpu->movem040_resume && cpu->movem040_continuation_valid &&
           cpu->movem040_instr_pc == cpu->instr_pc;
}

/* 040UM 表 10-6（PDF 314、印刷版 10-23）。MOVEM 列には転送アクセスが含まれるため、
 * 通常命令と異なり汎用 EA 分を加えない。 */
static int movem40_cycles(const ea_desc_t *ea, int dir, int sz, uint16_t mask, int ea_mode) {
    unsigned data = 0, address = 0;
    for (unsigned bit = 0; bit < 16; bit++) {
        if (!(mask & (UINT16_C(1) << bit)))
            continue;
        if (ea_mode == 4 && !dir) {
            if (bit < 8)
                address++;
            else
                data++;
        } else if (bit < 8)
            data++;
        else
            address++;
    }
    unsigned dprime = data ? data : 1;
    int registers = (int)dprime + (int)address;
    int base;

    if (!dir) {
        switch (ea->mode) {
        case 2:
        case 4:
        case 5:
            return 3 + 2 * registers;
        case 6:
            if (ea->ext_form != EA_EXT_FULL)
                return 16 + 2 * registers;
            if (ea->index_suppressed && ea->iis == 0)
                return -1;
            switch (ea->iis) {
            case 0:
                base = ea->bd_size == 0 ? 19 : 21;
                break;
            case 1:
            case 5:
                base = 27;
                break;
            case 2:
            case 3:
            case 6:
            case 7:
                base = 29;
                break;
            default:
                return -1;
            }
            return base + 2 * registers;
        case 7:
            if (ea->reg == 0 || ea->reg == 1)
                return 3 + 2 * registers;
            if (ea->reg == 3) {
                if (ea->ext_form != EA_EXT_FULL)
                    return -1;
                if (ea->index_suppressed && ea->iis == 0)
                    return -1;
                switch (ea->iis) {
                case 0:
                    base = ea->bd_size == 0 ? 19 : 21;
                    break;
                case 1:
                case 5:
                    base = 27;
                    break;
                case 2:
                case 3:
                case 6:
                case 7:
                    base = 29;
                    break;
                default:
                    return -1;
                }
                return base + 2 * registers;
            }
            return -1;
        default:
            return -1;
        }
    }

    switch (ea->mode) {
    case 2:
    case 3:
    case 5:
        base = 5;
        break;
    case 6:
        if (ea->ext_form != EA_EXT_FULL)
            base = 18;
        else {
            if (ea->index_suppressed && ea->iis == 0)
                return -1;
            switch (ea->iis) {
            case 0:
                base = ea->bd_size == 0 ? 21 : 23;
                break;
            case 1:
            case 5:
                base = 29;
                break;
            case 2:
            case 3:
            case 6:
            case 7:
                base = 31;
                break;
            default:
                return -1;
            }
        }
        break;
    case 7:
        if (ea->reg == 0 || ea->reg == 1)
            base = 5;
        else if (ea->reg == 2)
            base = 6;
        else if (ea->reg == 3) {
            if (ea->ext_form != EA_EXT_FULL)
                base = 19;
            else {
                if (ea->index_suppressed && ea->iis == 0)
                    return -1;
                switch (ea->iis) {
                case 0:
                    base = ea->bd_size == 0 ? 21 : 23;
                    break;
                case 1:
                case 5:
                    base = 29;
                    break;
                case 2:
                case 3:
                case 6:
                case 7:
                    base = 31;
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
    if (!sz && address)
        base += 2 * (int)address - 2;
    return base + 2 * registers;
}

/* MC68000 の MOVEM メモリ→レジスタは、最後のレジスタの次の語を余分に read する
 * （UM Table 8-10 の read 数 3+n／4+n）。値は捨てる。 */
static void movem_extra_read(mc68k_state_t *cpu, uint32_t addr) {
    if (cpu->model == MC68K_MODEL_68000)
        (void)cpu_read_word(cpu, addr);
}

static int op_movem(mc68k_state_t *cpu, uint16_t opcode) {
    int dir = (opcode >> 10) & 1; /* 0=reg→mem, 1=mem→reg */
    int sz = (opcode >> 6) & 1;   /* 0=word, 1=long */
    int size = sz ? 4 : 2;
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg = opcode & 7;
    uint32_t initial_ea = cpu->a[ea_reg];
    bool resume_040 = movem_040_resume(cpu);

    uint16_t mask = cpu_fetch_word(cpu);
    if (cpu->exec_state != CPU_NORMAL)
        return 4;
    cpu->t060_aux = mask;

    int cycles = 8;
    if (!cpu_has_68020_isa(cpu)) {
        /* UM00 表 8-10／DS10 表 7-19。EA
         * 分は転送方向ごとであり、転送サイズ／個数には依存しない。 */
        static const uint8_t mem_to_reg[] = {0, 0, 4, 4, 0, 8, 10, 8, 12, 8, 10};
        static const uint8_t reg_to_mem[] = {0, 0, 0, 0, 0, 4, 6, 4, 8};
        unsigned row = ea_mode < 7 ? (unsigned)ea_mode : (unsigned)(7 + ea_reg);
        const uint8_t *cost = dir ? mem_to_reg : reg_to_mem;
        unsigned count = dir ? sizeof(mem_to_reg) : sizeof(reg_to_mem);
        if (row < count)
            cycles += cost[row];
    }

    if (!dir) {
        if (ea_mode == 4) {
            /* プレデクリメント reg→メモリ。マスク: bit0=A7, bit1=A6, ..., bit8=D7, ...,
             * bit15=D0。 bit
             * を昇順に処理し、A7（または最上位レジスタ）が最高位アドレスに来るよう
             * デクリメントしてから書く。結果のメモリ並び（低→高）は D0..D7,A0..A7。 */
            for (int bit = 0; bit <= 15; bit++) {
                if (!(mask & (1u << bit)))
                    continue;
                int ridx = 15 - bit; /* bit0→A7(15) .. bit15→D0(0) */
                uint32_t val = (ridx < 8) ? cpu->d[ridx] : cpu->a[ridx - 8];
                if (ridx == 8 + ea_reg)
                    val = initial_ea - (cpu_has_68020_isa(cpu) ? (uint32_t)size : 0u);
                cpu->a[ea_reg] -= size;
                if (sz)
                    cpu_write_long(cpu, cpu->a[ea_reg], val);
                else
                    cpu_write_word(cpu, cpu->a[ea_reg], (uint16_t)val);
                if (cpu->exec_state != CPU_NORMAL)
                    return cycles;
                cycles += sz ? 8 : 4;
            }
        } else {
            /* コントロールモード reg→メモリ。マスク: bit0=D0, ..., bit7=D7, bit8=A0, ...,
             * bit15=A7。 低アドレスから昇順に書く（D0 が最低位）。 */
            uint32_t calculated_ea = ea_get_addr(cpu, ea_mode, ea_reg);
            if (cpu->exec_state != CPU_NORMAL)
                return 4;
            uint32_t addr = resume_040 ? cpu->movem040_effective_address : calculated_ea;
            for (int bit = 0; bit <= 15; bit++) {
                if (!(mask & (1u << bit)))
                    continue;
                int ridx = bit; /* bit0→D0(0) .. bit15→A7(15) */
                uint32_t val = (ridx < 8) ? cpu->d[ridx] : cpu->a[ridx - 8];
                if (sz)
                    cpu_write_long(cpu, addr, val);
                else
                    cpu_write_word(cpu, addr, (uint16_t)val);
                addr += (uint32_t)size;
                if (cpu->exec_state != CPU_NORMAL) {
                    movem_040_capture_fault(cpu, ea_mode, ea_reg, calculated_ea);
                    return cycles;
                }
                cycles += sz ? 8 : 4;
            }
        }
    } else {
        /* メモリ → レジスタ（全モード共通マスク: bit0=D0, ..., bit7=D7, bit8=A0, ...,
         * bit15=A7）。 低アドレスから昇順に読む（D0 が最低位）。 */
        if (ea_mode == 3) {
            /* ポストインクリメント */
            uint32_t addr = cpu->a[ea_reg];
            for (int bit = 0; bit <= 15; bit++) {
                if (!(mask & (1u << bit)))
                    continue;
                int ridx = bit; /* D0=0..D7=7, A0=8..A7=15 */
                uint32_t val;
                if (sz)
                    val = cpu_read_long(cpu, addr);
                else
                    val = (uint32_t)(int32_t)(int16_t)cpu_read_word(cpu, addr);
                if (cpu->exec_state != CPU_NORMAL) {
                    cpu->a[ea_reg] = addr;
                    return cycles;
                }
                addr += (uint32_t)size;
                if (ridx < 8)
                    cpu->d[ridx] = val;
                else if (ridx - 8 != ea_reg)
                    cpu->a[ridx - 8] = val;
                cycles += sz ? 8 : 4;
            }
            /* ベースレジスタがリストに含まれるとき、68020 MOVEM のポストインクリメントは
             * リスト全体の直後のアドレスを残す。転送カーソルはアーキテクチャ上の An
             * と分離する。 */
            cpu->a[ea_reg] = addr;
            movem_extra_read(cpu, addr);
        } else {
            uint32_t calculated_ea = ea_get_addr(cpu, ea_mode, ea_reg);
            if (cpu->exec_state != CPU_NORMAL)
                return 4;
            uint32_t addr = resume_040 ? cpu->movem040_effective_address : calculated_ea;
            for (int bit = 0; bit <= 15; bit++) {
                if (!(mask & (1u << bit)))
                    continue;
                int ridx = bit; /* D0=0..D7=7, A0=8..A7=15 */
                uint32_t val;
                if (sz)
                    val = cpu_read_long(cpu, addr);
                else
                    val = (uint32_t)(int32_t)(int16_t)cpu_read_word(cpu, addr);
                if (cpu->exec_state != CPU_NORMAL) {
                    movem_040_capture_fault(cpu, ea_mode, ea_reg, calculated_ea);
                    return cycles;
                }
                addr += (uint32_t)size;
                if (ridx < 8)
                    cpu->d[ridx] = val;
                else
                    cpu->a[ridx - 8] = val;
                cycles += sz ? 8 : 4;
            }
            movem_extra_read(cpu, addr);
        }
    }
    if (resume_040) {
        cpu->movem040_resume = false;
        cpu->movem040_continuation_valid = false;
    }
    if (cpu_is_68040(cpu)) {
        ea_desc_t implicit_ea = {.mode = (uint8_t)ea_mode, .reg = (uint8_t)ea_reg, .size = 2};
        const ea_desc_t *timing_ea =
            (ea_mode == 3 || ea_mode == 4) ? &implicit_ea : &cpu->last_ea;
        int timing = movem40_cycles(timing_ea, dir, sz, mask, ea_mode);
        if (timing >= 0)
            return cpu_040_tag(cpu, timing);
    }
    if (cpu_is_68020(cpu) || cpu_is_68030_family(cpu)) {
        ea_desc_t implicit_ea = {.mode = (uint8_t)ea_mode, .reg = (uint8_t)ea_reg, .size = 2};
        const ea_desc_t *timing_ea =
            (ea_mode == 3 || ea_mode == 4) ? &implicit_ea : &cpu->last_ea;
        unsigned n = 0;
        for (uint16_t bits = mask; bits; bits >>= 1)
            n += bits & 1u;
        /* UM20 8.2.7 uses 4 clocks per RL→EA register; UM30 11.6.7
         * uses 2.  Both tables add CIEA for the list extension word. */
        int base =
            dir ? 8 + 4 * (int)n : (cpu_is_68030_family(cpu) ? 4 + 2 * (int)n : 4 + 3 * (int)n);
        return base + ea_cycles_immediate_word(cpu, EA_COST_CALC_IMM, timing_ea);
    }
    return cycles;
}

/* ================================================================== */
/* LINK / UNLK（C 言語スタックフレームの構築・解放）                  */
/* ================================================================== */

/*
 * LINK An, #<displacement>:
 *   1. An をスタックに保存（SP -= 4; [SP] = An）
 *   2. An = 新しいフレームポインタ（An = SP）
 *   3. SP += displacement（通常は負数でローカル変数領域を確保）
 *
 * C コンパイラが生成するプロローグの典型:
 *   LINK  A6, #-32   ; フレームポインタをA6に、32バイトのローカル変数
 *   MOVEM.L D2-D7/A2-A5, -(SP)  ; 呼び出し先保存レジスタをプッシュ
 */
static int op_link(mc68k_state_t *cpu, uint16_t opcode) {
    int an = opcode & 7;
    int16_t d16 = (int16_t)cpu_fetch_word(cpu);
    if (cpu->exec_state != CPU_NORMAL)
        return 4;

    cpu->a[7] -= 4;
    cpu_write_long(cpu, cpu->a[7], cpu->a[an]);
    if (cpu->exec_state != CPU_NORMAL)
        return 8;

    cpu->a[an] = cpu->a[7];              /* フレームポインタ = 現在の SP */
    cpu->a[7] += (uint32_t)(int32_t)d16; /* ローカル変数領域を確保 */
    if (cpu_is_68040(cpu))
        return cpu_040_tag(cpu, 4); /* 040UM Table 10-5: calculate 3 + execute base 1 */
    return cpu_is_68030_family(cpu) ? 4 : cpu_is_68020(cpu) ? 5 : 16;
}

/*
 * UNLK An:
 *   1. SP = An（フレームポインタを SP に戻す）
 *   2. An = [SP]; SP += 4（保存された An を復元）
 *
 * LINK のエピローグ版。C 関数の末尾で RTS の直前に現れる。
 */
static int op_unlk(mc68k_state_t *cpu, uint16_t opcode) {
    int an = opcode & 7;
    cpu->a[7] = cpu->a[an]; /* SP をフレームベースに戻す */
    uint32_t saved = cpu_read_long(cpu, cpu->a[7]);
    if (cpu->exec_state != CPU_NORMAL)
        return 4;
    cpu->a[7] += 4;
    cpu->a[an] = saved;
    if (cpu_is_68040(cpu))
        return cpu_040_tag(cpu, 3); /* 040UM Table 10-5: calculate 2 + execute base 1 */
    return cpu_is_68030_family(cpu) ? 5 : cpu_is_68020(cpu) ? 6 : 12;
}

/* ================================================================== */
/* EXG（レジスタ交換）                                                 */
/* ================================================================== */

static int op_exg(mc68k_state_t *cpu, uint16_t opcode) {
    int rx = (opcode >> 9) & 7;
    int opmode = (opcode >> 3) & 0x1F; /* 8=Dm,Dn / 9=Am,An / 17=Dm,An */
    int ry = opcode & 7;
    uint32_t tmp;

    switch (opmode) {
    case 8: /* データレジスタ同士 */
        tmp = cpu->d[rx];
        cpu->d[rx] = cpu->d[ry];
        cpu->d[ry] = tmp;
        break;
    case 9: /* アドレスレジスタ同士 */
        tmp = cpu->a[rx];
        cpu->a[rx] = cpu->a[ry];
        cpu->a[ry] = tmp;
        break;
    case 17: /* データ ↔ アドレス */
        tmp = cpu->d[rx];
        cpu->d[rx] = cpu->a[ry];
        cpu->a[ry] = tmp;
        break;
    default:
        cpu->exec_state = CPU_EXCEPTION;
        return 4;
    }
    if (cpu_is_68040(cpu))
        return cpu_040_tag(cpu, opmode == 9 ? 3 : 2); /* 040UM Table 10-5: Ay,Ax / other EXG */
    return cpu_is_68020(cpu) ? 2 : cpu_is_68030_family(cpu) ? 4 : 6;
}

/* ================================================================== */
/* MOVEP（8bit 周辺機器インターフェース用：交互バイト転送）             */
/* ================================================================== */

/*
 * MOVEP Dn, (d16,An) / MOVEP (d16,An), Dn
 * 8bit データバスの周辺デバイスとやり取りするため、1 バイトおきに転送する。
 *
 * opmode:
 *   4 (100): MOVEP.W (d16,An), Dn  ← 2 バイト読み込み
 *   5 (101): MOVEP.L (d16,An), Dn  ← 4 バイト読み込み
 *   6 (110): MOVEP.W Dn, (d16,An)  ← 2 バイト書き込み
 *   7 (111): MOVEP.L Dn, (d16,An)  ← 4 バイト書き込み
 *
 * エンコード: 0x0000|(dn<<9)|(opmode<<6)|0x08|an
 * → BTST dynamic の mode=001 と同じ位置を占める。登録順で上書きする。
 */
static int op_movep(mc68k_state_t *cpu, uint16_t opcode) {
    int dn = (opcode >> 9) & 7;
    int opmode = (opcode >> 6) & 7;
    int an = opcode & 7;
    int16_t d16 = (int16_t)cpu_fetch_word(cpu);
    if (cpu->exec_state != CPU_NORMAL)
        return 4;
    uint32_t addr = cpu->a[an] + (int32_t)d16;

    switch (opmode) {
    case 4: /* MOVEP.W (d16,An), Dn: バイト×2 → Dn 下位ワード */
        cpu->d[dn] = (cpu->d[dn] & 0xFFFF0000U) | ((uint32_t)cpu_read_byte(cpu, addr) << 8) |
                     (uint32_t)cpu_read_byte(cpu, addr + 2);
        return cpu_is_68040(cpu)          ? cpu_040_tag(cpu, 9)
               : cpu_is_68020(cpu)        ? 12
               : cpu_is_68030_family(cpu) ? 10
                                          : 16;
    case 5: /* MOVEP.L (d16,An), Dn: バイト×4 → Dn 全体 */
        cpu->d[dn] = ((uint32_t)cpu_read_byte(cpu, addr) << 24) |
                     ((uint32_t)cpu_read_byte(cpu, addr + 2) << 16) |
                     ((uint32_t)cpu_read_byte(cpu, addr + 4) << 8) |
                     (uint32_t)cpu_read_byte(cpu, addr + 6);
        return cpu_is_68040(cpu)          ? cpu_040_tag(cpu, 16)
               : cpu_is_68020(cpu)        ? 18
               : cpu_is_68030_family(cpu) ? 14
                                          : 24;
    case 6: /* MOVEP.W Dn, (d16,An): Dn 下位ワード → バイト×2 */
        cpu_write_byte(cpu, addr, (uint8_t)(cpu->d[dn] >> 8));
        cpu_write_byte(cpu, addr + 2, (uint8_t)cpu->d[dn]);
        return cpu_is_68040(cpu)          ? cpu_040_tag(cpu, 20)
               : cpu_is_68020(cpu)        ? 11
               : cpu_is_68030_family(cpu) ? 10
                                          : 16;
    default: /* case 7: MOVEP.L Dn, (d16,An): Dn 全体 → バイト×4 */
        cpu_write_byte(cpu, addr, (uint8_t)(cpu->d[dn] >> 24));
        cpu_write_byte(cpu, addr + 2, (uint8_t)(cpu->d[dn] >> 16));
        cpu_write_byte(cpu, addr + 4, (uint8_t)(cpu->d[dn] >> 8));
        cpu_write_byte(cpu, addr + 6, (uint8_t)cpu->d[dn]);
        return cpu_is_68040(cpu)          ? cpu_040_tag(cpu, 24)
               : cpu_is_68020(cpu)        ? 17
               : cpu_is_68030_family(cpu) ? 14
                                          : 24;
    }
}

/* ================================================================== */
/* テーブル登録                                                        */
/* ================================================================== */

void register_transfer_ops(op_handler_t *table) {
    int i, dn, ea, an, rx, ry;

    /* ---- MOVEQ: ライン 7、bit 8=0 ---- */
    for (i = 0x7000; i <= 0x7FFF; i++) {
        if (i & 0x100)
            continue; /* bit 8=1 は無効 */
        table[i] = op_moveq;
    }

    /* ---- MOVE SR, EA (0x40C0-0x40FF, dst=Data Alterable) /
     *      MOVE EA, SR (0x46C0-0x46FF, src=Data) ---- */
    for (ea = 0; ea < 64; ea++) {
        if (ea_is_data_alterable(ea)) {
            table[0x40C0 | ea] = op_move_from_sr;
        }
        if (ea_is_data(ea)) {
            table[0x46C0 | ea] = op_move_to_sr;
        }
    }

    /* ---- LEA: 0x41C0, 0x43C0, ..., 0x4FC0 + Control EA ---- */
    for (an = 0; an < 8; an++) {
        uint16_t base = (uint16_t)(0x41C0 | (an << 9));
        for (ea = 0; ea < 64; ea++) {
            if (ea_is_control(ea))
                table[base | ea] = op_lea;
        }
    }

    /* ---- SWAP: 0x4840-0x4847 ---- */
    for (dn = 0; dn < 8; dn++) {
        table[0x4840 | dn] = op_swap;
    }

    /* ---- PEA: 0x4840+EA（コントロールモードのみ）---- */
    for (ea = 0; ea < 64; ea++) {
        if (ea_is_control(ea))
            table[0x4840 | ea] = op_pea;
    }

    /* ---- MOVEM: reg→mem(0x4880/0x48C0) は Control Alterable + -(An)、
     *             mem→reg(0x4C80/0x4CC0) は Control + (An)+ （PC相対許容） ---- */
    for (ea = 0; ea < 64; ea++) {
        int mode = (ea >> 3) & 7;
        if (ea_is_control_alterable(ea) || mode == 4) {
            table[0x4880 | ea] = op_movem;
            table[0x48C0 | ea] = op_movem;
        }
        if (ea_is_control(ea) || mode == 3) {
            table[0x4C80 | ea] = op_movem;
            table[0x4CC0 | ea] = op_movem;
        }
    }

    /* ---- EXT: MOVEM の mode 0 を上書き ---- */
    for (dn = 0; dn < 8; dn++) {
        table[0x4880 | dn] = op_ext;
        table[0x48C0 | dn] = op_ext;
    }

    /* ---- LINK / UNLK ---- */
    for (an = 0; an < 8; an++) {
        table[0x4E50 | an] = op_link;
        table[0x4E58 | an] = op_unlk;
    }

    /* ---- EXG ---- */
    for (rx = 0; rx < 8; rx++) {
        for (ry = 0; ry < 8; ry++) {
            uint16_t b = (uint16_t)(0xC100 | (rx << 9));
            table[b | 0x40 | ry] = op_exg;
            table[b | 0x48 | ry] = op_exg;
            table[b | 0x88 | ry] = op_exg;
        }
    }

    /* ---- MOVEP (BTST dynamic の mode=001 を上書き: 後から登録) ---- */
    /*
     * opcode: 0x0000 | (dn<<9) | (opmode<<6) | 0x08 | an
     * opmode = 4(100),5(101),6(110),7(111)
     * bits 5-3 = 001 = mode 1 (アドレスレジスタ間接 + オフセット)
     * BTST dynamic は同じ位置を使うが MOVEP が優先。
     */
    for (dn = 0; dn < 8; dn++) {
        for (an = 0; an < 8; an++) {
            uint16_t bd = (uint16_t)((dn << 9) | 0x08 | an);
            for (int op = 4; op <= 7; op++) {
                uint16_t mv = (uint16_t)(bd | (op << 6));
                table[mv] = op_movep;
            }
        }
    }
}
