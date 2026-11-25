/*
 * test_invalid_ea.c — 無効な宛先/ソースEA（An直接/PC相対/immediate）を
 * ILLEGAL命令例外（ベクタ4）として弾くことを検証する回帰テスト。
 * また、ADD/SUB/CMP の EA→Dn方向は word/long サイズに限り An 直接も有効という
 * サイズ依存の特例を過剰に拒否しないことも検証する。
 *
 * decode.c の build_op_table() は全エントリを ILLEGAL で初期化後、各
 * register_*_ops() が有効な opcode 範囲だけを上書きする。本テストは、
 * 実機では無効なEAエンコーディングの組み合わせが「ILLEGAL のまま
 * （＝上書きされずに）残っている」ことを、実際に例外が発生することで確認する。
 *
 * disasm_length_diff（tests/diff/）はこの種の修正を検出できない
 * （disasm.c は op_table を参照しない独立実装のため）。CPU実行を通した
 * 検証が本Issueの唯一の回帰ガードになる。
 *
 * フレーム形状はシンプルフレーム（PC 4バイト + SR 2バイト、計6バイト）。
 * ILLEGAL は opcode 1ワードのみ消費した時点で判定されるため、拡張ワードの
 * 内容（immediate値等）は無視されてよい。
 */
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "mc68000/cpu.h"
#include "mc68000/cpu_types.h"
#include "mc68000/bus_hooks.h"
#include "memory/memory.h"
#include "memory/bus.h"

#define RAM_SIZE     0x10000
#define RESET_SSP    0x00008000
#define RESET_PC     0x00001000
#define ILLEGAL_VEC4 0x00001500 /* ベクタ4（ILLEGAL）ハンドラ（自己ループ） */

static void write_word_be(uint8_t *ram, uint32_t addr, uint16_t val) {
    ram[addr + 0] = (uint8_t)(val >> 8);
    ram[addr + 1] = (uint8_t)(val & 0xFF);
}

static void write_long_be(uint8_t *ram, uint32_t addr, uint32_t val) {
    ram[addr + 0] = (uint8_t)(val >> 24);
    ram[addr + 1] = (uint8_t)(val >> 16);
    ram[addr + 2] = (uint8_t)(val >> 8);
    ram[addr + 3] = (uint8_t)(val >> 0);
}

static uint32_t read_long_be(const uint8_t *ram, uint32_t addr) {
    return ((uint32_t)ram[addr] << 24) | ((uint32_t)ram[addr + 1] << 16) |
           ((uint32_t)ram[addr + 2] << 8) | (uint32_t)ram[addr + 3];
}

static mc68k_state_t *make_cpu_model(uint8_t *ram, mc68k_model_t model) {
    mem_ctx_t *mem = mem_create();
    mem_region_t *r = calloc(1, sizeof(mem_region_t));
    r->data = ram;
    r->base = 0x000000;
    r->size = RAM_SIZE;
    mem_add_region(mem, r);
    mc68k_bus_t bus = bus_make_hooks(mem);
    return mc68k_create_model(&bus, model);
}

static mc68k_state_t *make_cpu(uint8_t *ram) {
    return make_cpu_model(ram, MC68K_MODEL_68000);
}

/* リセットベクタ + ベクタ4ハンドラ（BRA * で自己ループ）を書く共通セットアップ。 */
static void setup_common(uint8_t *ram) {
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    write_long_be(ram, 0x10, ILLEGAL_VEC4);   /* ベクタ4 = 0x10 */
    write_word_be(ram, ILLEGAL_VEC4, 0x60FE); /* BRA * */
}

/* ILLEGAL命令例外（ベクタ4）が起きたことを検証する。
 * pushed PC は違反命令の先頭アドレス（op_illegal: cpu->pc - 2）。 */
static void assert_illegal(uint8_t *ram, mc68k_regs_t *r, uint32_t fault_pc,
                           mc68k_model_t model) {
    assert(r->pc == ILLEGAL_VEC4);
    assert(r->a[7] == RESET_SSP - (model == MC68K_MODEL_68000 ? 6 : 8));
    uint32_t saved_pc = read_long_be(ram, r->a[7] + 2);
    assert(saved_pc == fault_pc);
}

/* 例外が起きず、opcode の直後（末尾の BRA * の着地点）まで正常実行できたことを検証する。 */
static void assert_not_illegal(mc68k_regs_t *r, uint32_t expected_pc) {
    assert(r->pc == expected_pc);
    assert(r->pc != ILLEGAL_VEC4);
}

static void run_illegal_model(uint16_t opcode, mc68k_model_t model) {
    uint8_t ram[RAM_SIZE] = {0};
    setup_common(ram);
    write_word_be(ram, RESET_PC + 0, opcode);
    write_word_be(ram, RESET_PC + 2,
                  0x60FE); /* BRA *（違反しなければここへ、ILLEGALなら未到達） */

    mc68k_state_t *cpu = make_cpu_model(ram, model);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert_illegal(ram, &r, RESET_PC, model);
}

static void run_illegal(uint16_t opcode) {
    run_illegal_model(opcode, MC68K_MODEL_68000);
}

static void run_illegal_both(uint16_t opcode) {
    run_illegal_model(opcode, MC68K_MODEL_68000);
    run_illegal_model(opcode, MC68K_MODEL_68010);
}

/* ================================================================== */
/* ILLEGAL になるべきケース（ で新たに弾かれるようになった無効EA） */
/* ================================================================== */

static void test_ori_b_dst_an(void) {
    run_illegal(0x0008); /* ORI.B #imm,A0 */
}

static void test_addi_w_dst_pc(void) {
    run_illegal(0x067A); /* ADDI.W #imm,(PC) */
}

static void test_ori_l_dst_sr_invalid(void) {
    /* ORI.L #x,SR: long には CCR/SR 特殊形式が存在しない。
     * 従来は sc==2 でも SR 経路に誤って入ってしまうバグがあった（本Issue調査中に発見）。 */
    run_illegal(0x00BC);
}

static void test_bset_static_dst_imm(void) {
    run_illegal(0x08FC); /* BSET #bit,#imm（static形式、宛先がimmediate） */
}

static void test_eor_w_dst_pc(void) {
    /* EOR.W D0,A0（0xB148相当）は CMPM.W (A0)+,(A0)+ とビットパターンが衝突する
     * 実機仕様上の正当な再利用（CMPM が EOR の「An宛先」枠を上書きする）のため、
     * ここでは CMPM と衝突しない PC相対を無効宛先として使う。 */
    run_illegal(0xB17A); /* EOR.W D0,(PC) */
}

static void test_and_w_dst_pc(void) {
    run_illegal(0xC17A); /* AND.W D0,(PC) */
}

static void test_add_w_dst_imm(void) {
    run_illegal(0xD17C); /* ADD.W D0,#imm（Dn→EA方向、宛先がimmediate） */
}

static void test_addq_b_dst_pc(void) {
    run_illegal(0x523A); /* ADDQ.B #1,(PC) */
}

static void test_addq_b_dst_an(void) {
    run_illegal(0x5208); /* ADDQ.B #1,A0（ADDA.B相当は存在しない） */
}

static void test_mulu_src_an(void) {
    run_illegal(0xC0C8); /* MULU.W A0,D0 */
}

static void test_asl_mem_dst_an(void) {
    run_illegal(0xE1C8); /* ASL (An)相当のメモリシフト、宛先がAn直接 */
}

static void test_move_b_dst_an(void) {
    run_illegal(0x1040); /* MOVE.B D0,A0（MOVEA.Bは存在しない） */
}

static void test_move_w_dst_pc(void) {
    run_illegal(0x35C0); /* MOVE.W D0,(PC) */
}

static void test_movem_regmem_postinc(void) {
    run_illegal(0x4898); /* MOVEM.W D0-,(A0)+（reg→memに(An)+は無効） */
}

static void test_movem_memreg_predec(void) {
    run_illegal(0x4CA0); /* MOVEM.W -(A0),D0-（mem→regに-(An)は無効） */
}

static void test_cmp_b_src_an(void) {
    run_illegal(0xB009); /* CMP.B A1,D0（byteサイズのEA→Dn方向はAn直接不可） */
}

/* ================================================================== */
/* : 68000/68010で未定義の命令EAが別命令にならないこと             */
/* ================================================================== */

static void test_issue30_invalid_ea_both_models(void) {
    /* Data Alterable 限定: CLR/NEGX/NEG/NOT/TST/NBCD/TAS のAn直接。 */
    static const uint16_t data_alterable_only[] = {
        0x4208, 0x4008, 0x4408, 0x4608, 0x4A08, 0x4808, 0x4AC8,
    };
    /* Control 限定: LEA/JSR/JMP/PEA のDn直接、およびPEA immediate。 */
    static const uint16_t control_only[] = {
        0x41C0,
        0x4E80,
        0x4EC0,
        0x487C,
    };
    /* SccのPC相対はTRAPcc拡張用、ラインE bit 11はBFxxx用に未登録へ戻す。 */
    static const uint16_t reserved_68020[] = {0x50FA, 0xE8D0};

    for (size_t i = 0; i < sizeof(data_alterable_only) / sizeof(data_alterable_only[0]); i++)
        run_illegal_both(data_alterable_only[i]);
    for (size_t i = 0; i < sizeof(control_only) / sizeof(control_only[0]); i++)
        run_illegal_both(control_only[i]);
    for (size_t i = 0; i < sizeof(reserved_68020) / sizeof(reserved_68020[0]); i++)
        run_illegal_both(reserved_68020[i]);
}

static void test_issue30_valid_boundaries(void) {
    /* Data Alterable、Control、Scc、DBcc、従来のメモリシフトは維持する。 */
    static const uint16_t valid[] = {0x4200, 0x41D0, 0x50C0, 0xE0D0};

    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); i++) {
        uint8_t ram[RAM_SIZE] = {0};
        setup_common(ram);
        write_word_be(ram, RESET_PC, valid[i]);
        write_word_be(ram, RESET_PC + 2, 0x60FE);
        mc68k_state_t *cpu = make_cpu(ram);
        mc68k_reset(cpu);
        mc68k_execute(cpu, 200);
        mc68k_regs_t r;
        mc68k_get_regs(cpu, &r);
        mc68k_destroy(cpu);
        assert_not_illegal(&r, RESET_PC + 2);
    }
}

/* ================================================================== */
/* 引き続き有効であるべきケース（回帰固定）                             */
/* ================================================================== */

static void test_btst_dynamic_immediate_ok(void) {
    uint8_t ram[RAM_SIZE] = {0};
    setup_common(ram);
    write_word_be(ram, RESET_PC + 0, 0x033C); /* BTST D1,#imm（dynamic、src=immediateは有効） */
    write_word_be(ram, RESET_PC + 2, 0x0001); /* immediate byte拡張ワード */
    write_word_be(ram, RESET_PC + 4, 0x60FE); /* BRA * */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert_not_illegal(&r, RESET_PC + 4);
}

static void test_addq_w_dst_an_ok(void) {
    uint8_t ram[RAM_SIZE] = {0};
    setup_common(ram);
    write_word_be(ram, RESET_PC + 0,
                  0x5248); /* ADDQ.W #1,A0（word/longのAn宛先はADDA相当で有効） */
    write_word_be(ram, RESET_PC + 2, 0x60FE); /* BRA * */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert_not_illegal(&r, RESET_PC + 2);
}

static void test_move_w_dst_an_ok(void) {
    uint8_t ram[RAM_SIZE] = {0};
    setup_common(ram);
    write_word_be(ram, RESET_PC + 0, 0x3040); /* MOVE.W D0,A0（MOVEA.Wとして有効） */
    write_word_be(ram, RESET_PC + 2, 0x60FE); /* BRA * */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert_not_illegal(&r, RESET_PC + 2);
}

static void test_movem_regmem_predec_ok(void) {
    uint8_t ram[RAM_SIZE] = {0};
    setup_common(ram);
    write_word_be(ram, RESET_PC + 0, 0x48A0); /* MOVEM.W reglist,-(A0)（有効） */
    write_word_be(ram, RESET_PC + 2, 0x0000); /* レジスタマスク: 転送なし */
    write_word_be(ram, RESET_PC + 4, 0x60FE); /* BRA * */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert_not_illegal(&r, RESET_PC + 4);
}

static void test_movem_memreg_postinc_ok(void) {
    uint8_t ram[RAM_SIZE] = {0};
    setup_common(ram);
    write_word_be(ram, RESET_PC + 0, 0x4C98); /* MOVEM.W (A0)+,reglist（有効） */
    write_word_be(ram, RESET_PC + 2, 0x0000); /* レジスタマスク: 転送なし */
    write_word_be(ram, RESET_PC + 4, 0x60FE); /* BRA * */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert_not_illegal(&r, RESET_PC + 4);
}

/* CMP.L An,Dn。
 * word/long サイズの EA→Dn 方向は An 直接も許可される（byteのみ不可のサイズ
 * 依存の特例）。 の修正で An を一律除外してしまい誤ってILLEGALに
 * なっていた回帰を検出する。 */
static void test_cmp_l_src_an_ok(void) {
    uint8_t ram[RAM_SIZE] = {0};
    setup_common(ram);
    write_word_be(ram, RESET_PC + 0, 0xB089); /* CMP.L A1,D0 */
    write_word_be(ram, RESET_PC + 2, 0x60FE); /* BRA * */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert_not_illegal(&r, RESET_PC + 2);
}

static void test_add_w_src_an_ok(void) {
    uint8_t ram[RAM_SIZE] = {0};
    setup_common(ram);
    write_word_be(ram, RESET_PC + 0, 0xD049); /* ADD.W A1,D0 */
    write_word_be(ram, RESET_PC + 2, 0x60FE); /* BRA * */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert_not_illegal(&r, RESET_PC + 2);
}

static void test_sub_l_src_an_ok(void) {
    uint8_t ram[RAM_SIZE] = {0};
    setup_common(ram);
    write_word_be(ram, RESET_PC + 0, 0x9089); /* SUB.L A1,D0 */
    write_word_be(ram, RESET_PC + 2, 0x60FE); /* BRA * */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert_not_illegal(&r, RESET_PC + 2);
}

static void test_cmpm_an_slot_ok(void) {
    /* 0xB148 は EOR.W D0,A0 のビットパターンだが、実機では CMPM.W (A0)+,(A0)+
     * として予約されている（register_arith_ext_ops が register_logic_ops の
     * EOR登録を上書きする、ISA仕様上の正当な再利用）。ILLEGALにならないことを確認する。 */
    uint8_t ram[RAM_SIZE] = {0};
    setup_common(ram);
    write_word_be(ram, RESET_PC + 0, 0xB148); /* CMPM.W (A0)+,(A0)+ */
    write_word_be(ram, RESET_PC + 2, 0x60FE); /* BRA * */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    mc68k_destroy(cpu);

    assert_not_illegal(&r, RESET_PC + 2);
}

/* ================================================================== */
/* メインエントリ                                                        */
/* ================================================================== */

int main(void) {
    test_ori_b_dst_an();
    test_addi_w_dst_pc();
    test_ori_l_dst_sr_invalid();
    test_bset_static_dst_imm();
    test_eor_w_dst_pc();
    test_and_w_dst_pc();
    test_add_w_dst_imm();
    test_addq_b_dst_pc();
    test_addq_b_dst_an();
    test_mulu_src_an();
    test_asl_mem_dst_an();
    test_move_b_dst_an();
    test_move_w_dst_pc();
    test_movem_regmem_postinc();
    test_movem_memreg_predec();
    test_cmp_b_src_an();
    test_issue30_invalid_ea_both_models();

    test_btst_dynamic_immediate_ok();
    test_addq_w_dst_an_ok();
    test_move_w_dst_an_ok();
    test_movem_regmem_predec_ok();
    test_movem_memreg_postinc_ok();
    test_cmpm_an_slot_ok();
    test_cmp_l_src_an_ok();
    test_add_w_src_an_ok();
    test_sub_l_src_an_ok();
    test_issue30_valid_boundaries();
    return 0;
}
