/*
 * ops_system_ext.c — MC68010 以降の拡張 system 命令。
 *
 * build_op_table() が 68000 ベースラインを一通り登録し終えたあと、モデルが
 * MC68K_MODEL_68010 以上のときだけ register_68010_ops() が呼ばれ、該当する
 * op_table エントリを上書きする。
 *
 * 現時点で扱う命令:
 *   - MOVE from CCR (0x42C0)  : 68010 で新設。68000 では未定義のまま残す。
 *   - MOVE from SR  (0x40C0)  : 68010 で特権命令化（68000 は非特権のまま）。
 *   - BKPT (0x4848-0x484F)    : 68010 で新設。
 *   - MOVEC/MOVES/RTD         : 68010 で追加。
 */
#include "cpu_internal.h"

/* 040UM Table 10-6 (PDF 312, printed 10-21), MOVE CCR,<ea>. */
static int move40_from_ccr_cycles(const ea_desc_t *ea) {
    switch (ea->mode) {
    case 0:
    case 2:
    case 3:
    case 4:
    case 5:
        return 3;
    case 6:
        if (ea->ext_form != EA_EXT_FULL)
            return 8;
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
            return 3;
        return -1;
    default:
        return -1;
    }
}

/* ================================================================== */
/* MOVE from CCR: MOVE.W CCR, EA (0x42C0)                              */
/* 68000 では未定義命令（ILLEGAL）。 */
/* ================================================================== */

static int op_move_from_ccr(mc68k_state_t *cpu, uint16_t opcode) {
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg = opcode & 7;
    ea_write(cpu, ea_mode, ea_reg, 2, (uint32_t)(cpu->sr & 0x001FU));
    if (cpu->exec_state != CPU_NORMAL)
        return 4;
    if (cpu_is_68040(cpu)) {
        int cycles = move40_from_ccr_cycles(&cpu->last_ea);
        if (cycles >= 0)
            return cpu_040_tag(cpu, cycles);
    }
    /* 68000には存在しない命令。[DS10 Table 7-21]: レジスタ直接は68010=4
     * （CYC_MOVE_FROM_CCR_REG）。メモリ形式（8）は暫定値のまま
     * （出典未確認、Phase 1bの対象外）。 */
    if (cpu_is_68030_family(cpu))
        return ea_mode == 0 ? 4 : 4 + ea_cycles(cpu, EA_COST_CALC, &cpu->last_ea);
    if (ea_mode == 0)
        return CYC(cpu, CYC_MOVE_FROM_CCR_REG);
    return 8 + ea_cycles(cpu, cyc_ea_kind(cpu, CYC_MOVE_FROM_CCR_REG), &cpu->last_ea);
}

/* ================================================================== */
/* MOVE from SR: MOVE.W SR, EA (0x40C0) — 68010 で特権化                */
/* ================================================================== */

/*
 * 特権チェック付きラッパ。副作用（EA書き込み）より前にチェックする
 * （CLAUDE.md「副作用より前にチェック」規約）。実体は ops_transfer.c の
 * op_move_from_sr()（68000 の非特権実装をそのまま再利用）。
 */
static int op_move_from_sr_priv(mc68k_state_t *cpu, uint16_t opcode) {
    if (!(cpu->sr & SR_S))
        return cpu_privilege_violation(cpu);
    return op_move_from_sr(cpu, opcode);
}

/* ================================================================== */
/* MOVEC: Rc,Rn (0x4E7A) / Rn,Rc (0x4E7B) — 特権                        */
/* 68010 で新設。制御レジスタ（SFC/DFC/USP/VBR）と汎用レジスタ間の転送。   */
/* ================================================================== */

/*
 * 68010 で有効な制御レジスタ番号（`M68000PM.pdf` §6
 * MOVEC の表で確認）。68020 以降で番号が増える（CACR=0x002, CAAR=0x802,
 * MSP=0x803, ISP=0x804）ため、switch 文をモデルで分岐せず「68010 で有効な
 * 番号のみ許可」と書いておき、68020 対応時に追加する。
 */
enum {
    MC68K_CREG_SFC = 0x000,
    MC68K_CREG_DFC = 0x001,
    MC68K_CREG_CACR = 0x002,
    MC68K_CREG_USP = 0x800,
    MC68K_CREG_VBR = 0x801,
    MC68K_CREG_CAAR = 0x802,
    MC68K_CREG_MSP = 0x803,
    MC68K_CREG_ISP = 0x804,
    MC68K_CREG_040_TC = 0x003,
    MC68K_CREG_040_ITT0 = 0x004,
    MC68K_CREG_040_ITT1 = 0x005,
    MC68K_CREG_040_DTT0 = 0x006,
    MC68K_CREG_040_DTT1 = 0x007,
    MC68K_CREG_040_MMUSR = 0x805,
    MC68K_CREG_040_URP = 0x806,
    MC68K_CREG_040_SRP = 0x807,
};

#define MC68K_TTR_040_MASK        UINT32_C(0xffffe364)
#define MC68K_CACR_060_READ_MASK  UINT32_C(0xf880e000)
#define MC68K_CACR_060_WRITE_MASK UINT32_C(0xf8e0e000)
#define MC68K_TC_060_MASK         UINT32_C(0x0000fffe)
#define MC68K_PCR_060_MASK        UINT32_C(0x00000083)
#define MC68K_PCR_060_ID          UINT32_C(0x04300000)
#define MC68K_PCR_060_REVISION UINT32_C(0x00000700) /* リビジョン 7（bit15-8）。XEiJ と同じ */

static bool movec_040(mc68k_state_t *cpu, uint16_t creg, uint32_t *gp, bool to_general) {
    uint32_t *reg = NULL;
    switch (creg) {
    case MC68K_CREG_040_TC:
        if (to_general)
            *gp = cpu->mmu040_tc;
        else
            cpu->mmu040_tc = (uint16_t)(*gp & 0xc000u);
        return true;
    case MC68K_CREG_040_DTT0:
        reg = &cpu->mmu040_dtt0;
        break;
    case MC68K_CREG_040_DTT1:
        reg = &cpu->mmu040_dtt1;
        break;
    case MC68K_CREG_040_ITT0:
        reg = &cpu->mmu040_itt0;
        break;
    case MC68K_CREG_040_ITT1:
        reg = &cpu->mmu040_itt1;
        break;
    case MC68K_CREG_040_MMUSR:
        reg = &cpu->mmu040_mmusr;
        break;
    case MC68K_CREG_040_URP:
        reg = &cpu->mmu040_urp;
        break;
    case MC68K_CREG_040_SRP:
        reg = &cpu->mmu040_srp;
        break;
    default:
        return false;
    }
    if (to_general)
        *gp = *reg;
    else if (creg == MC68K_CREG_040_URP || creg == MC68K_CREG_040_SRP)
        *reg = *gp & ~UINT32_C(0x1ff);
    else if (creg == MC68K_CREG_040_DTT0 || creg == MC68K_CREG_040_DTT1 ||
             creg == MC68K_CREG_040_ITT0 || creg == MC68K_CREG_040_ITT1)
        *reg = *gp & MC68K_TTR_040_MASK;
    else
        *reg = *gp;
    return true;
}

static bool movec_060(mc68k_state_t *cpu, uint16_t creg, uint32_t *gp, bool to_general) {
    uint32_t *reg = NULL, mask = UINT32_MAX;
    switch (creg) {
    case MC68K_CREG_SFC:
        if (to_general)
            *gp = cpu->sfc;
        else
            cpu->sfc = (uint8_t)(*gp & 7u);
        return true;
    case MC68K_CREG_DFC:
        if (to_general)
            *gp = cpu->dfc;
        else
            cpu->dfc = (uint8_t)(*gp & 7u);
        return true;
    case MC68K_CREG_CACR:
        reg = &cpu->cacr;
        mask = to_general ? MC68K_CACR_060_READ_MASK : MC68K_CACR_060_WRITE_MASK;
        break;
    case MC68K_CREG_040_TC:
        if (to_general)
            *gp = cpu->mmu060_tc & MC68K_TC_060_MASK;
        else
            cpu->mmu060_tc = *gp & MC68K_TC_060_MASK;
        return true;
    case MC68K_CREG_040_ITT0:
        reg = &cpu->mmu060_itt0;
        mask = MC68K_TTR_040_MASK;
        break;
    case MC68K_CREG_040_ITT1:
        reg = &cpu->mmu060_itt1;
        mask = MC68K_TTR_040_MASK;
        break;
    case MC68K_CREG_040_DTT0:
        reg = &cpu->mmu060_dtt0;
        mask = MC68K_TTR_040_MASK;
        break;
    case MC68K_CREG_040_DTT1:
        reg = &cpu->mmu060_dtt1;
        mask = MC68K_TTR_040_MASK;
        break;
    case 0x008:
        reg = &cpu->buscr;
        mask = UINT32_C(0xf0000000);
        break;
    case MC68K_CREG_USP:
        reg = &cpu->usp;
        break;
    case MC68K_CREG_VBR:
        reg = &cpu->vbr;
        break;
    case MC68K_CREG_040_URP:
        reg = &cpu->mmu060_urp;
        mask = UINT32_C(0xfffffe00);
        break;
    case MC68K_CREG_040_SRP:
        reg = &cpu->mmu060_srp;
        mask = UINT32_C(0xfffffe00);
        break;
    case 0x808:
        reg = &cpu->pcr;
        mask = MC68K_PCR_060_MASK;
        break;
    default:
        return false;
    }
    if (to_general)
        *gp = creg == 0x808 ? ((*reg & mask) | MC68K_PCR_060_ID | MC68K_PCR_060_REVISION)
                            : (*reg & mask);
    else
        *reg = (*reg & ~mask) | (*gp & mask);
    return true;
}

static int op_movec(mc68k_state_t *cpu, uint16_t opcode) {
    if (!(cpu->sr & SR_S))
        return cpu_privilege_violation(cpu); /* 特権違反: 拡張ワード未消費 */

    /* 拡張ワード: bit15=A/D (0=Dn,1=An), bit14-12=汎用レジスタ番号,
     * bit11-0=制御レジスタ番号。 */
    uint16_t ext = cpu_fetch_word(cpu);
    if (cpu->exec_state != CPU_NORMAL)
        return 4;
    cpu->t060_aux = ext & 0x0FFFu;

    int is_an = (ext >> 15) & 1;
    int reg = (ext >> 12) & 7;
    uint16_t creg = ext & 0x0FFF;
    int to_general = (opcode & 1) == 0; /* 0x4E7A: Rc→Rn / 0x4E7B: Rn→Rc */
    uint32_t *gp = is_an ? &cpu->a[reg] : &cpu->d[reg];

    if (cpu_is_68060(cpu)) {
        if (movec_060(cpu, creg, gp, to_general)) {
            /* TC・ITT0/1 は MOVEC でだけ書かれる。命令フェッチ高速経路の前提が変わりうる。 */
            if (!to_general)
                cpu_ifetch_fast_invalidate(cpu);
            return 4;
        }
        goto illegal;
    }

    if (cpu_is_68040(cpu)) {
        if (movec_040(cpu, creg, gp, to_general))
            return cpu_040_tag(cpu, to_general ? 21 : 13); /* 040UM Table 10-5: Rc→Rn / Rn→Rc */
        /* CAAR/MSP/ISP are not MC68040 control registers. */
        if (creg == MC68K_CREG_CAAR || creg == MC68K_CREG_MSP || creg == MC68K_CREG_ISP)
            goto illegal;
    }

    if (to_general) {
        /* USP は MOVEC が特権命令＝実行時は必ずスーパーバイザモードのため
         * 無条件で cpu->usp を直接参照する（A7 は常に SSP。分岐不要）。
         * SFC/DFC の読み出しは long 転送で
         * ゼロ拡張（uint8_t → uint32_t 代入で自然にゼロ拡張される）。 */
        switch (creg) {
        case MC68K_CREG_SFC:
            *gp = cpu->sfc;
            break;
        case MC68K_CREG_DFC:
            *gp = cpu->dfc;
            break;
        case MC68K_CREG_USP:
            *gp = cpu->usp;
            break;
        case MC68K_CREG_VBR:
            *gp = cpu->vbr;
            break;
        case MC68K_CREG_CACR:
            if (cpu_has_68020_isa(cpu)) {
                *gp = cpu->cacr;
                break;
            }
            goto illegal;
        case MC68K_CREG_CAAR:
            if (cpu_has_68020_isa(cpu)) {
                *gp = cpu->caar;
                break;
            }
            goto illegal;
        case MC68K_CREG_MSP:
            if (cpu_has_68020_isa(cpu)) {
                *gp = (cpu_sp_slot(cpu, cpu->sr) == &cpu->msp) ? cpu->a[7] : cpu->msp;
                break;
            }
            goto illegal;
        case MC68K_CREG_ISP:
            if (cpu_has_68020_isa(cpu)) {
                *gp = (cpu_sp_slot(cpu, cpu->sr) == &cpu->ssp) ? cpu->a[7] : cpu->ssp;
                break;
            }
            goto illegal;
        default:
            /* 未定義の制御レジスタ番号: ILLEGAL命令例外（ベクタ4）。
             * 拡張ワードは消費済みなので、積む PC は命令先頭（cpu->pc - 4）。 */
        illegal:
            /* [DS10 Table 7-22] "MOVEC, Illegal Cr": 68010=46（CYC_EXC_MOVEC_ILLEGAL、
             * Illegal Instruction(38)とはベクタ4を共有するが値が違うため別クラス）。
             * 68000には本命令が存在しないため68000行はCYC_ABSENT。 */
            return cpu_dispatch_exception(
                cpu, cpu->pc - 4, 4, cpu_is_68060(cpu) ? 34 : CYC(cpu, CYC_EXC_MOVEC_ILLEGAL));
        }
    } else {
        uint32_t val = *gp;
        switch (creg) {
        case MC68K_CREG_SFC:
            cpu->sfc = (uint8_t)(val & 0x07U);
            break; /* 下位3bitのみ有効 */
        case MC68K_CREG_DFC:
            cpu->dfc = (uint8_t)(val & 0x07U);
            break;
        case MC68K_CREG_USP:
            cpu->usp = val;
            break;
        case MC68K_CREG_VBR:
            cpu->vbr = val;
            break;
        case MC68K_CREG_CACR:
            if (cpu_has_68020_isa(cpu)) {
                cpu->cacr = cpu_is_68040(cpu)
                                ? val & (MC68K_CACR_040_DE | MC68K_CACR_040_IE)
                                : val & (model_has_030_cache(cpu->model) ? MC68K_CACR_030_MASK
                                                                         : MC68K_CACR_020_MASK);
                break;
            }
            goto illegal_write;
        case MC68K_CREG_CAAR:
            if (cpu_has_68020_isa(cpu)) {
                cpu->caar = model_has_030_cache(cpu->model) ? val : (val & 0xfcu);
                break;
            }
            goto illegal_write;
        case MC68K_CREG_MSP:
            if (cpu_has_68020_isa(cpu)) {
                if (cpu_sp_slot(cpu, cpu->sr) == &cpu->msp)
                    cpu->a[7] = val;
                else
                    cpu->msp = val;
                break;
            }
            goto illegal_write;
        case MC68K_CREG_ISP:
            if (cpu_has_68020_isa(cpu)) {
                if (cpu_sp_slot(cpu, cpu->sr) == &cpu->ssp)
                    cpu->a[7] = val;
                else
                    cpu->ssp = val;
                break;
            }
            goto illegal_write;
        default:
        illegal_write:
            /* [DS10 Table 7-22] "MOVEC, Illegal Cr": 68010=46（CYC_EXC_MOVEC_ILLEGAL、
             * Illegal Instruction(38)とはベクタ4を共有するが値が違うため別クラス）。
             * 68000には本命令が存在しないため68000行はCYC_ABSENT。 */
            return cpu_dispatch_exception(
                cpu, cpu->pc - 4, 4, cpu_is_68060(cpu) ? 34 : CYC(cpu, CYC_EXC_MOVEC_ILLEGAL));
        }
    }

    /* [DS10 Table 7-21] "MOVEC Source→Register"（Rc→Rn）: 68010=12
     * （CYC_MOVEC_SRC_TO_REG）。68000には本命令が存在しないため68000行は
     * CYC_ABSENT。Rn→Rc方向は現行値10のまま変更なし（DONE、MC68010データシート
     * Table 7-21）。 */
    if (cpu_is_68040(cpu)) /* SFC/DFC/USP/VBR/CACR も Table 10-5 の MOVEC 1 行に含まれる */
        return cpu_040_tag(cpu, to_general ? 21 : 13);
    if (cpu_is_68020(cpu))
        return to_general ? 6 : 12;
    if (cpu_is_68030_family(cpu)) {
        /* UM30 11.6.7 の ord 2--4: Cr-A は 0x800 レジスタバンク
         * （USP/VBR/CAAR/MSP/ISP）、SFC/DFC/CACR は Cr-B である。 */
        if (to_general)
            return 6;
        return creg >= MC68K_CREG_USP ? 6 : 12;
    }
    if (to_general)
        return CYC(cpu, CYC_MOVEC_SRC_TO_REG);
    return 10;
}

/* ================================================================== */
/* MOVES: MOVES.x EA,Rn / Rn,EA (0x0E00-0x0EFF) — 特権                  */
/* メモリ→レジスタは SFC、レジスタ→メモリは DFC を各EAの実バスサイクルへ */
/* 明示的に渡す。命令/拡張ワードのフェッチは通常どおりプログラム空間。    */
/* ================================================================== */

/* 040UM Table 10-6 (PDF 315, printed 10-24).  MOVES synchronizes parts
 * of the processor and the table labels its values typical; nevertheless,
 * these are the model's documented hit-cache nominal timings. */
static int moves40_cycles(const ea_desc_t *ea, int to_memory, int to_address) {
    int an, dn, memory;
    if (to_memory) {
        an = dn = 0;
        memory = 22;
    } else if (to_address) {
        an = 52;
        dn = 0;
        memory = 0;
    } else {
        an = 0;
        dn = 39;
        memory = 0;
    }

    switch (ea->mode) {
    case 2:
    case 3:
        return to_memory ? memory : (to_address ? an : dn);
    case 4:
        return to_memory ? 20 : (to_address ? 32 : 23);
    case 5:
        return to_memory ? 23 : (to_address ? 53 : 40);
    case 6:
        if (ea->ext_form != EA_EXT_FULL)
            return to_memory ? 26 : (to_address ? 56 : 43);
        if (ea->index_suppressed && ea->iis == 0)
            return -1;
        switch (ea->iis) {
        case 0:
            if (ea->bd_size == 0)
                return to_memory ? 28 : (to_address ? 40 : 29);
            return to_memory ? 30 : (to_address ? 42 : 31);
        case 1:
        case 5:
            return to_memory ? 38 : (to_address ? 67 : 53);
        case 2:
        case 3:
            return to_memory ? 38 : (to_address ? 60 : 47);
        case 6:
            return to_memory ? 37 : (to_address ? 67 : 53);
        case 7:
            return to_memory ? 38 : (to_address ? 60 : 47);
        default:
            return -1;
        }
    case 7:
        if (ea->reg == 0 || ea->reg == 1)
            return to_memory ? 20 : (to_address ? 32 : 21);
        return -1;
    default:
        return -1;
    }
}

/* MOVES の access。060 は命令空間の FC（2・6）を同じ data 空間（1・5）へ変換して TM に載せ、
 * SFC/DFC が 0・3・4・7 のとき TT=10 で変換せずに物理アドレスとして使う（[060UM §4.2.5、
 * Table 4-2]）。alternate な空間（0・3・4・7）の MOVES は noncachable（[060UM
 * §7.7.1]。変換結果の cache mode が 2 になり、 一致した line は cpu_040_cache_read/write が
 * push・無効化する）。それ以外の機種は FC のまま TT=10。 */
static mc68k_access_t moves_access(const mc68k_state_t *cpu, uint8_t function_code, bool read,
                                   uint8_t size) {
    const uint8_t fc = function_code & 7u;
    mc68k_access_t access = {.fc = fc, .read = read, .size = size, .alternate = true};
    if (cpu_is_68060(cpu)) {
        access.fc = (fc == 2 || fc == 6) ? (uint8_t)(fc - 1) : fc;
        access.alternate = fc == 0 || fc == 3 || fc == 4 || fc == 7;
    }
    return access;
}
static int op_moves(mc68k_state_t *cpu, uint16_t opcode) {
    if (!(cpu->sr & SR_S))
        return cpu_privilege_violation(cpu); /* 特権違反: 拡張ワード・EA共に未消費 */

    int ea_mode = (opcode >> 3) & 7;
    int ea_reg = opcode & 7;
    int size_code = (opcode >> 6) & 3;
    int size = (size_code == 0) ? 1 : (size_code == 1) ? 2 : 4; /* 00=B,01=W,10=L */

    /* 拡張ワード: bit15=A/D, bit14-12=汎用レジスタ番号,
     * bit11=dr（0: メモリ→レジスタ, 1: レジスタ→メモリ）。 */
    uint16_t ext = cpu_fetch_word(cpu);
    if (cpu->exec_state != CPU_NORMAL)
        return 4;

    int is_an = (ext >> 15) & 1;
    int reg = (ext >> 12) & 7;
    int dr = (ext >> 11) & 1;

    if (dr == 0) {
        /* メモリ → レジスタ */
        const mc68k_access_t access = moves_access(cpu, cpu->sfc, true, (uint8_t)size);
        uint32_t val = ea_read_access(cpu, ea_mode, ea_reg, size, &access);
        if (cpu->exec_state != CPU_NORMAL)
            return 8;
        if (is_an) {
            /* An 宛先は byte/word でも符号拡張して32bit書き込み（MOVEAと同じ規則）。 */
            if (size == 1)
                val = (uint32_t)(int32_t)(int8_t)(uint8_t)val;
            else if (size == 2)
                val = (uint32_t)(int32_t)(int16_t)(uint16_t)val;
            cpu->a[reg] = val;
        } else {
            uint32_t mask = size_mask(size);
            cpu->d[reg] = (cpu->d[reg] & ~mask) | (val & mask);
        }
    } else {
        /* レジスタ → メモリ。ポストインクリメント/プリデクリメントで同じ An を
         * レジスタ側にも指定した場合の挙動は実機で未定義（M68000PRM記載）。
         * ここではレジスタ値を読んでから EA 書き込み（アドレス副作用を含む）を
         * 行う順序で固定する。 */
        uint32_t val = is_an ? cpu->a[reg] : cpu->d[reg];
        const mc68k_access_t access = moves_access(cpu, cpu->dfc, false, (uint8_t)size);
        ea_write_access(cpu, ea_mode, ea_reg, size, val, &access);
        if (cpu->exec_state != CPU_NORMAL)
            return 8;
    }

    if (cpu_is_68040(cpu)) {
        int cycles = moves40_cycles(&cpu->last_ea, dr != 0, is_an != 0);
        if (cycles >= 0)
            return cpu_040_tag(cpu, cycles);
    }
    if (cpu_is_68020(cpu) || cpu_is_68030_family(cpu))
        return (dr ? 5 : 7) + ea_cycles_immediate_word(cpu, EA_COST_CALC_IMM, &cpu->last_ea);

    /* MOVES.L is the Phase-2a integration path: its 68010 base is 20 and
     * the source/destination EA fetch is selected by g_ea_kind.  68020 keeps
     * its pre-Phase-2c value explicitly at EA_COST_NONE. */
    if (size == 4)
        return CYC(cpu, CYC_MOVES_L) +
               ea_cycles(cpu, cyc_ea_kind(cpu, CYC_MOVES_L), &cpu->last_ea);
    return 16;
}

/* ================================================================== */
/* RTD: RTD #d (0x4E74) — 非特権                                       */
/* RTS + LEA d(SP),SP 相当。RTS（ops_branch.c op_rts）と同じ流儀で実装。 */
/* ================================================================== */

static int op_rtd(mc68k_state_t *cpu, uint16_t opcode) {
    (void)opcode;
    int16_t disp = (int16_t)cpu_fetch_word(cpu); /* 符号付きディスプレースメント */
    if (cpu->exec_state != CPU_NORMAL)
        return 4;
    uint32_t ret = cpu_read_long(cpu, cpu->a[7]);
    if (cpu->exec_state != CPU_NORMAL)
        return 8;
    cpu->a[7] += 4u + (uint32_t)(int32_t)disp;
    cpu->pc = ret;
    cpu->flow_changed = true;
    if (cpu_is_68040(cpu))
        return cpu_040_tag(cpu, 11); /* 040UM Table 10-5: calculate 6 + execute base 5 */
    return cpu_is_68030_family(cpu) ? 10 : 16;
}

/* ================================================================== */
/* BKPT: Breakpoint #n (0x4848-0x484F) — 68010 で新設                   */
/* 下位3bitはブレークポイント番号だが、外部ハードウェアのブレークポイント */
/* 確認機構を持たない本コアでは番号によらず常にILLEGAL命令例外（ベクタ4） */
/* へ落ちる（[UM20]§8/[PM]）。: 68010以降で未登録のままILLEGAL   */
/* 経路（誤った公称値）に落ちていた欠落を修正する。                       */
/* ================================================================== */

static int op_bkpt(mc68k_state_t *cpu, uint16_t opcode) {
    (void)opcode;
    if (cpu_is_68060(cpu) && cpu->bus.cpu_space_read) {
        uint32_t ignored;
        (void)cpu_bus_cpu_space_read(cpu, MC68K_FC_CPU_SPACE, 0, 1, &ignored);
    }
    if (cpu_is_68060(cpu))
        return cpu_dispatch_exception(cpu, cpu->instr_pc, 4, 34);
    /* [DS10 Table 7-22] Breakpoint Instruction: 68010=42（CYC_EXC_BKPT）。
     * 68020/68030は[UM20]/[UM30]未確認のため、本Issue以前からのop_bkpt()の
     * 実挙動（34）を暫定値として保持する（Phase 2c/3で要検証）。68000には
     * 本命令が存在しないため68000行はCYC_ABSENT。 */
    return cpu_dispatch_exception(cpu, cpu->instr_pc, 4, CYC(cpu, CYC_EXC_BKPT));
}

/* ================================================================== */
/* テーブル登録                                                        */
/* ================================================================== */

void register_68010_ops(op_handler_t *table) {
    int ea;

    /* MOVEC: 固定 opcode 2 個（EAなし、拡張ワードでレジスタ指定）。 */
    table[0x4E7A] = op_movec;
    table[0x4E7B] = op_movec;

    /* RTD: 固定 opcode 1 個（続く拡張ワードがディスプレースメント）。 */
    table[0x4E74] = op_rtd;

    /* BKPT: 固定 opcode 8個（下位3bitはブレークポイント番号、挙動は共通）。 */
    for (int n = 0; n < 8; n++) {
        table[0x4848 | n] = op_bkpt;
    }

    /* MOVES: 0x0E00-0x0EFF。size(bit7-6)=00/01/10 × EA(bit5-0)=Memory Alterable。 */
    for (int size_code = 0; size_code < 3; size_code++) {
        for (ea = 0; ea < 64; ea++) {
            if (ea_is_memory_alterable(ea)) {
                table[0x0E00 | (size_code << 6) | ea] = op_moves;
            }
        }
    }

    /* MOVE from CCR: dst は Data Alterable のみ（ops_logic.c の MOVE to CCR と対称）。 */
    for (ea = 0; ea < 64; ea++) {
        if (ea_is_data_alterable(ea)) {
            table[0x42C0 | ea] = op_move_from_ccr;
        }
    }

    /* MOVE from SR: register_transfer_ops() が非特権版で登録した 0x40C0-0x40FF を
     * 特権チェック付きラッパで上書きする。EA範囲（Data Alterable）は変わらない。 */
    for (ea = 0; ea < 64; ea++) {
        if (ea_is_data_alterable(ea)) {
            table[0x40C0 | ea] = op_move_from_sr_priv;
        }
    }
}
