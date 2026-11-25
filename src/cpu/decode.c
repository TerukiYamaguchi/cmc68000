/*
 * decode.c — 命令デコードテーブルの構築
 *
 * 65536 エントリの op_table を起動時に一度だけ構築する。
 * 実行時は op_table[opcode] を参照するだけなのでキャッシュに乗りやすい。
 *
 * MC68000 の opcode 上位 4bit がグループ（ライン）を示す:
 *   0x0xxx: ビット操作 / イミディエイト演算（ORI, ANDI, SUBI, ADDI...）
 *   0x1xxx: MOVE.B
 *   0x2xxx: MOVE.L
 *   0x3xxx: MOVE.W
 *   0x4xxx: 雑命令（CLR, NEG, NOT, EXT, LEA, PEA, TRAP 等）
 *   0x5xxx: ADDQ / SUBQ / Scc / DBcc
 *   0x6xxx: BRA / BSR / Bcc
 *   0x7xxx: MOVEQ
 *   0x8xxx: OR / DIVU / SBCD
 *   0x9xxx: SUB / SUBX
 *   0xAxxx: ライン A 例外（Mac の ROM コールがここを使った）
 *   0xBxxx: CMP / EOR
 *   0xCxxx: AND / MULU / ABCD / EXG
 *   0xDxxx: ADD / ADDX
 *   0xExxx: シフト / ローテート
 *   0xFxxx: ライン F 例外（コプロセッサ用・X68000 では未使用）
 *
 * 各命令グループの登録関数が該当 opcode 範囲のエントリを上書きしていく。
 */
#include "cpu_internal.h"

/*
 * 未実装命令または本当に不正な opcode に対するハンドラ。
 *
 * 実機では ILLEGAL 命令例外（ベクタ 4 番、アドレス 0x0010）が発生する。
 * MC68000 例外スタックフレーム: SP-=6 (PC 4B + SR 2B) → vec[4] へジャンプ。
 *
 * 0x4AFC は MC68000 仕様書が唯一「ILLEGAL 命令」として予約したオペコードで、
 * ソフトウェアが意図的に例外を起こしたいときに使う。
 * それ以外の未割り当て opcode も同じ例外になるが、
 */
static int op_illegal(mc68k_state_t *cpu, uint16_t opcode) {
    (void)opcode;
    /* opcode 1ワードのみ消費済み → 命令先頭 = pc-2。ベクタ4 (ILLEGAL 命令例外)。
     * [DS10 Table 7-22]: 68010=38（CYC_EXC_ILLEGAL） */
    return cpu_dispatch_exception(cpu, cpu->pc - 2, 4,
                                  cpu_is_68040(cpu) ? cpu_040_tag(cpu, 32)
                                                    : CYC(cpu, CYC_EXC_ILLEGAL));
}

static int op_line_a(mc68k_state_t *cpu, uint16_t opcode) {
    (void)opcode;
    return cpu_dispatch_exception(cpu, cpu->pc - 2, 10,
                                  cpu_is_68040(cpu)          ? cpu_040_tag(cpu, 32)
                                  : cpu_is_68030_family(cpu) ? CYC(cpu, CYC_EXC_ILLEGAL)
                                                             : 34);
}

static int op_line_f(mc68k_state_t *cpu, uint16_t opcode) {
    if (cpu_has_cpid0_privilege_check(cpu) && !(cpu->sr & SR_S) && ((opcode >> 9) & 7) == 0)
        return cpu_privilege_violation(cpu);
    return cpu_dispatch_exception(cpu, cpu->pc - 2, 11,
                                  cpu_is_68040(cpu)          ? cpu_040_tag(cpu, 32)
                                  : cpu_is_68030_family(cpu) ? CYC(cpu, CYC_EXC_ILLEGAL)
                                                             : 34);
}

void build_op_table(op_handler_t *table, mc68k_model_t model) {
    /*
     * まず全 65536 エントリを op_illegal で埋める。
     * その後、各命令グループの登録関数が該当範囲を上書きしていく。
     */
    for (int i = 0; i < 65536; i++) {
        table[i] = op_illegal;
    }
    for (int op = 0xA000; op <= 0xAFFF; op++)
        table[op] = op_line_a;
    for (int op = 0xF000; op <= 0xFFFF; op++)
        table[op] = op_line_f;

    /* MOVE.B/W/L / MOVEA.W/L */
    register_move_ops(table);

    /* ADD/ADDA/ADDI/ADDQ, SUB/SUBA/SUBI/SUBQ, CMP/CMPA/CMPI */
    register_alu_ops(table);

    /* BRA/Bcc/BSR, JMP/JSR/RTS */
    register_branch_ops(table);

    /* TRAP #0-#15 */
    register_trap_ops(table);

    /*
     * 呼び出し順序に依存関係あり:
     *   - register_logic_ops（EOR）は register_alu_ops（CMP の暫定登録）より後
     *   - register_transfer_ops（MOVEP）は register_logic_ops（BTST dynamic）より後
     *   - register_arith_ext_ops（CMPM）は register_logic_ops（EOR）より後
     * のように、後段が前段の一部テーブルエントリを上書きする箇所があるため、
     * この呼び出し順序を変更しないこと。
     */
    register_logic_ops(table); /* AND/OR/EOR, CLR/NEG/NOT/TST, ビット操作, MOVE CCR */
    register_transfer_ops(
        table); /* MOVEQ, MOVE SR, LEA/PEA, SWAP/EXT, MOVEM, LINK/UNLK, EXG, MOVEP */
    register_arith_ext_ops(
        table);                 /* MULU/MULS, DIVU/DIVS, ADDX/SUBX, CMPM, CHK, ABCD/SBCD/NBCD */
    register_shift_ops(table);  /* シフト/ローテート, Scc/DBcc */
    register_system_ops(table); /* NOP/RTE/RTR, TAS, RESET/STOP/TRAPV, MOVE USP */

    /* 68010 以降の追加・変更命令（MOVE from CCR 新設、MOVE from SR 特権化等）。
     * 68000 ベースラインの登録が全て終わったあとに上書きする必要があるため最後。 */
    if (model_has_68010_isa(model))
        register_68010_ops(table);
    if (model_has_68020_isa(model)) {
        register_68020_arith_ops(table);
        register_68020_bitfield_ops(table);
        register_68020_system_ops(table);
    }
    if (model_has_030_access_control(model))
        register_68030_mmu_ops(table);
    if (model == MC68K_MODEL_68040)
        register_68040_mmu_ops(table);
    if (model == MC68K_MODEL_68040)
        register_68040_cache_ops(table);
    if (model == MC68K_MODEL_68040)
        register_68040_fpu_ops(table); /* cpID 1。cpID 2/3 は上の MMU/cache/MOVE16 が所有 */
    if (model == MC68K_MODEL_68060)
        register_68060_ops(table);
    /* コプロセッサインタフェース（cpID 1..7。cpID 0 は上の PMMU と従来の F-line）。
     * 68040 の F-line は内蔵FPU専用デコーダが扱うため、CIR/FPCP 入口は登録しない。 */
    if (model_has_68020_isa(model) && model != MC68K_MODEL_68040 && model != MC68K_MODEL_68060)
        register_coproc_ops(table);
}
