/*
 * ops_branch.c — Bcc/BRA/BSR, JMP/JSR/RTS 命令ハンドラ実装
 */
#include "cpu_internal.h"
#include <stdint.h>

/* 040UM Table 10-6 (PDF 311--312, printed 10-20--10-21).  JMP and JSR
 * share this control-EA timing column.  Execute leads are not added. */
static int jump40_cycles(const ea_desc_t *ea) {
    switch (ea->mode) {
    case 2:
        return 4;
    case 5:
        return 5;
    case 6:
        if (ea->ext_form != EA_EXT_FULL)
            return 12;
        if (ea->index_suppressed && ea->iis == 0)
            return -1;
        switch (ea->iis) {
        case 0:
            return ea->bd_size == 0 ? 15 : 17;
        case 1:
        case 5:
            return 23;
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
            return 7;
        if (ea->reg == 3) {
            if (ea->ext_form != EA_EXT_FULL)
                return 13;
            if (ea->index_suppressed && ea->iis == 0)
                return -1;
            switch (ea->iis) {
            case 0:
                return ea->bd_size == 0 ? 15 : 17;
            case 1:
            case 5:
                return 23;
            case 2:
            case 3:
            case 6:
            case 7:
                return 24;
            default:
                return -1;
            }
        }
        return -1;
    default:
        return -1;
    }
}

/* ================================================================== */
/* Bcc / BRA / BSR（ライン 6: 0x6000-0x6FFF）                         */
/* ================================================================== */

/*
 * MC68000 の 14 種条件コード（cond bits 3-0）:
 *   BHI=2  BLS=3  BCC=4  BCS=5  BNE=6  BEQ=7
 *   BVC=8  BVS=9  BPL=A  BMI=B  BGE=C  BLT=D  BGT=E  BLE=F
 *
 * BGE/BLT/BGT/BLE は符号付き比較（N と V の組み合わせ）で、
 * CMP 後の分岐に使われる。X68000 のゲームロジックや
 * 座標計算など至るところで登場する命令群。
 */
int eval_cond(uint16_t sr, int cond) {
    int N = (sr & CCR_N) != 0;
    int Z = (sr & CCR_Z) != 0;
    int V = (sr & CCR_V) != 0;
    int C = (sr & CCR_C) != 0;
    switch (cond) {
    case 0:
        return 1; /* T: 常に真（ST / DBT）*/
    case 1:
        return 0; /* F: 常に偽（SF / DBRA）*/
    case 2:
        return !C && !Z; /* BHI: Higher（符号なし大）*/
    case 3:
        return C || Z; /* BLS: Lower or Same */
    case 4:
        return !C; /* BCC: Carry Clear（BHS とも呼ぶ）*/
    case 5:
        return C; /* BCS: Carry Set（BLO とも呼ぶ）*/
    case 6:
        return !Z; /* BNE: Not Equal */
    case 7:
        return Z; /* BEQ: Equal */
    case 8:
        return !V; /* BVC: Overflow Clear */
    case 9:
        return V; /* BVS: Overflow Set */
    case 10:
        return !N; /* BPL: Plus（非負）*/
    case 11:
        return N; /* BMI: Minus（負）*/
    case 12:
        return !(N ^ V); /* BGE: Greater or Equal signed */
    case 13:
        return (N ^ V); /* BLT: Less Than signed */
    case 14:
        return !Z && !(N ^ V); /* BGT: Greater Than signed */
    case 15:
        return Z || (N ^ V); /* BLE: Less or Equal signed */
    default:
        return 0;
    }
}

/*
 * BRA / Bcc 共通ハンドラ。
 *
 * ディスプレースメントの基点は「命令の直後（opcode_addr + 2）」で、
 * これはハンドラ呼び出し時の cpu->pc と等しい（cpu_fetch_word が opcode を
 * 取り出すときに PC を +2 しているため）。
 *
 * disp8 == 0 のとき: 次のワードに 16bit ディスプレースメントが続く（BRA.W）
 * disp8 != 0 のとき: opcode の下位 8bit がディスプレースメント（BRA.S）
 */
static int op_bcc(mc68k_state_t *cpu, uint16_t opcode) {
    int cond = (opcode >> 8) & 0xF;
    int8_t d8 = (int8_t)(opcode & 0xFF);
    uint32_t base = cpu->pc; /* opcode_addr + 2（ディスプレースメント加算の基点）*/

    if (d8 == -1 && cpu_has_68020_isa(cpu)) {
        uint32_t hi = cpu_fetch_word(cpu);
        uint32_t lo = cpu_fetch_word(cpu);
        uint32_t d32 = (hi << 16) | lo;
        if (cpu->exec_state != CPU_NORMAL)
            return 4;
        cpu->t060_aux = (int32_t)d32 < 0;
        int taken = (cond == 0) || eval_cond(cpu->sr, cond);
        if (taken) {
            cpu->pc = base + (int32_t)d32;
            cpu->flow_changed = true;
            if (cpu_is_68040(cpu))
                return cpu_040_tag(cpu, 4);
            return cpu_is_68030_family(cpu) || cpu_is_68020(cpu) ? 6 : 10;
        }
        if (cpu_is_68040(cpu))
            return cpu_040_tag(cpu, 6);
        return cpu_is_68030_family(cpu) || cpu_is_68020(cpu) ? 6 : 12;
    }
    if (d8 == 0) {
        /* BRA.W / Bcc.W: 16bit ディスプレースメント */
        int16_t d16 = (int16_t)cpu_fetch_word(cpu); /* PC → opcode_addr + 4 */
        if (cpu->exec_state != CPU_NORMAL)
            return 4; /* 補充の fault 後は PC を書換えない */
        cpu->t060_aux = d16 < 0;
        int taken = (cond == 0) || eval_cond(cpu->sr, cond);
        if (taken) {
            cpu->pc = base + (int32_t)d16;
            cpu->flow_changed = true;
            if (cpu_is_68040(cpu))
                return cpu_040_tag(cpu, 4);
            return cpu_is_68030_family(cpu) || cpu_is_68020(cpu) ? 6 : 10;
        }
        /* not taken: cpu_fetch_word が既に PC を進めているのでそのまま。
         * [UM00 Table 8-18] / [DS10 Table 7-18]: Bcc.W Not Taken は
         * 68000=12、68010=10（CYC_BCC_WORD_NOT_TAKEN。BRAはtaken固定で
         * ここへ到達しない） */
        return cpu_is_68040(cpu) ? cpu_040_tag(cpu, 6)
               : cpu_is_68030_family(cpu) || cpu_is_68020(cpu)
                   ? 6
                   : CYC(cpu, CYC_BCC_WORD_NOT_TAKEN);
    }

    /* BRA.S / Bcc.S: 8bit ディスプレースメント */
    cpu->t060_aux = d8 < 0;
    int taken = (cond == 0) || eval_cond(cpu->sr, cond);
    if (taken) {
        cpu->pc = base + (int32_t)d8;
        cpu->flow_changed = true;
        if (cpu_is_68040(cpu))
            return cpu_040_tag(cpu, 4);
        return cpu_is_68030_family(cpu) || cpu_is_68020(cpu) ? 6 : 10;
    }
    /* [UM00 Table 8-18] / [DS10 Table 7-18]: Bcc.S Not Taken は68000=8、
     * 68010=6（CYC_BCC_BYTE_NOT_TAKEN） */
    return cpu_is_68040(cpu)          ? cpu_040_tag(cpu, 6)
           : cpu_is_68030_family(cpu) ? 4
           : cpu_is_68020(cpu)        ? 4
                                      : CYC(cpu, CYC_BCC_BYTE_NOT_TAKEN);
}

/*
 * BSR（Branch to Subroutine）:
 * 戻りアドレス（次命令のアドレス）をシステムスタックに積んでから分岐する。
 *
 * BSR.S: 戻りアドレス = opcode_addr + 2（BSR 命令は 2 バイト）
 * BSR.W: 戻りアドレス = opcode_addr + 4（opcode + 拡張ワードで 4 バイト）
 */
static int op_bsr(mc68k_state_t *cpu, uint16_t opcode) {
    int8_t d8 = (int8_t)(opcode & 0xFF);
    uint32_t base = cpu->pc; /* opcode_addr + 2 */
    uint32_t target, return_addr;

    if (d8 == -1 && cpu_has_68020_isa(cpu)) {
        uint32_t hi = cpu_fetch_word(cpu);
        uint32_t lo = cpu_fetch_word(cpu);
        uint32_t d32 = (hi << 16) | lo;
        if (cpu->exec_state != CPU_NORMAL)
            return 4;
        return_addr = cpu->pc;
        target = base + (int32_t)d32;
    } else if (d8 == 0) {
        int16_t d16 = (int16_t)cpu_fetch_word(cpu);
        if (cpu->exec_state != CPU_NORMAL)
            return 4; /* 補充の fault 後は戻りアドレスを push しない */
        return_addr = cpu->pc; /* opcode_addr + 4 */
        target = base + (int32_t)d16;
    } else {
        return_addr = cpu->pc; /* opcode_addr + 2 = base */
        target = base + (int32_t)d8;
    }

    /* スタックへプッシュ（MC68000 はスタックが下向きに伸びる）*/
    cpu->a[7] -= 4;
    cpu_write_long(cpu, cpu->a[7], return_addr);
    if (cpu->exec_state != CPU_NORMAL)
        return 8;
    cpu->pc = target;
    cpu->flow_changed = true;
    if (cpu_is_68040(cpu))
        return cpu_040_tag(cpu, 3); /* 040UM Table 10-5: calculate 2 + execute base 1 */
    if (cpu_is_68030_family(cpu))
        return 6;
    return cpu_is_68020(cpu) ? 7 : 18;
}

/* ================================================================== */
/* JMP / JSR / RTS（ライン 4 の一部: 0x4E75, 0x4E80-0x4EFF）          */
/* ================================================================== */

/*
 * JMP: 制御 EA が示すアドレスへ無条件ジャンプ。
 * スタックは変化しない（サブルーチンではなくジャンプテーブルや
 * 末尾最適化に使われる）。
 */
static int op_jmp(mc68k_state_t *cpu, uint16_t opcode) {
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg = (opcode >> 0) & 7;
    uint32_t target = ea_get_addr(cpu, ea_mode, ea_reg);
    if (cpu->exec_state != CPU_NORMAL)
        return 4;
    cpu->pc = target;
    cpu->flow_changed = true;
    if (cpu_is_68040(cpu)) {
        int cycles = jump40_cycles(&cpu->last_ea);
        if (cycles >= 0)
            return cpu_040_tag(cpu, cycles);
    }
    if (cpu_has_68020_isa(cpu))
        return 4 + ea_cycles(cpu, EA_COST_JUMP, &cpu->last_ea);
    return !cpu_has_68020_isa(cpu) ? 8 + ea_cycles(cpu, EA_COST_JUMP, &cpu->last_ea) : 8;
}

/*
 * JSR: 戻りアドレスをスタックに積んでから制御 EA へジャンプ。
 * RTS とペアで使う。X68000 BIOS（IOCS）の呼び出しでも使用。
 */
static int op_jsr(mc68k_state_t *cpu, uint16_t opcode) {
    int ea_mode = (opcode >> 3) & 7;
    int ea_reg = (opcode >> 0) & 7;
    uint32_t target = ea_get_addr(cpu, ea_mode, ea_reg);
    if (cpu->exec_state != CPU_NORMAL)
        return 4;
    uint32_t return_addr = cpu->pc; /* 拡張ワード読み取り後の PC = 次命令アドレス */
    cpu->a[7] -= 4;
    cpu_write_long(cpu, cpu->a[7], return_addr);
    if (cpu->exec_state != CPU_NORMAL)
        return 8;
    cpu->pc = target;
    cpu->flow_changed = true;
    if (cpu_is_68040(cpu)) {
        int cycles = jump40_cycles(&cpu->last_ea);
        if (cycles >= 0)
            return cpu_040_tag(cpu, cycles);
    }
    if (cpu_is_68030_family(cpu))
        return 4 + ea_cycles(cpu, EA_COST_JUMP, &cpu->last_ea);
    if (cpu_has_68020_isa(cpu))
        return 5 + ea_cycles(cpu, EA_COST_JUMP, &cpu->last_ea);
    return !cpu_has_68020_isa(cpu) ? 16 + ea_cycles(cpu, EA_COST_JUMP, &cpu->last_ea) : 16;
}

/*
 * RTS（Return from Subroutine）:
 * スタックから 32bit アドレスをポップして PC にセットする。
 * A7 は +4 される。
 */
static int op_rts(mc68k_state_t *cpu, uint16_t opcode) {
    (void)opcode;
    uint32_t ret = cpu_read_long(cpu, cpu->a[7]);
    if (cpu->exec_state != CPU_NORMAL)
        return 4;
    cpu->a[7] += 4;
    cpu->pc = ret;
    cpu->flow_changed = true;
    if (cpu_is_68040(cpu))
        return cpu_040_tag(cpu, 10); /* 040UM Table 10-5: calculate 5 + execute 5 */
    return cpu_is_68030_family(cpu) ? 9 : cpu_is_68020(cpu) ? 10 : 16;
}

void register_branch_ops(op_handler_t *table) {
    /* BRA: ライン 6、cond=0（0x6000-0x60FF） */
    for (int i = 0x6000; i <= 0x60FF; i++) {
        table[i] = op_bcc;
    }
    /* BSR: ライン 6、cond=1（0x6100-0x61FF） */
    for (int i = 0x6100; i <= 0x61FF; i++) {
        table[i] = op_bsr;
    }
    /* Bcc: ライン 6、cond=2-15（0x6200-0x6FFF） */
    for (int i = 0x6200; i <= 0x6FFF; i++) {
        table[i] = op_bcc;
    }

    /* RTS: 0x4E75 */
    table[0x4E75] = op_rts;

    /* JSR / JMP: EA bits 5-0 は制御モードのみ */
    for (int ea = 0; ea < 64; ea++) {
        if (!ea_is_control(ea))
            continue;
        table[0x4E80 | ea] = op_jsr;
        table[0x4EC0 | ea] = op_jmp;
    }
}
