/*
 * test_cycle_phase1a_real.c — Phase 1aで是正した28セルを
 * 実命令で検証する (現行仕様 T1-9)
 *
 * notes/phase1_68010_diff_survey.md §2.1 の35セルのうち「P1b重複」でない
 * 28セル（cyc_class_t 化されておらず ops_alu.c/ops_logic.c/ops_shift.c/
 * ops_arith_ext.c 内のリテラルのまま是正されたもの）が対象。
 * セルと命令の対応は同調査 §2.1（一覧）・§2.2（コード箇所）をそのまま使う。
 *
 * これらの命令はモデル分岐を持たない（cyc_class_t化されていない）ため、
 * MC68K_MODEL_68000 だけで検証すれば足りる（CYC_PROF_68000 行そのものの
 * 是正であり、他モデルにもそのまま伝わる。design書§10 Phase 1a）。
 *
 * オペコードは ops_alu.c / ops_logic.c / ops_shift.c / ops_arith_ext.c の
 * 各ハンドラと、その register_*_ops() での登録位置を実際に読んで組み立てた
 * （tests/cpu/test_alu.c・test_cycle_dbcc.c と同じ手書きオペコードのスタイル）。
 */
#include "mc68000/cpu.h"
#include "cpu/cpu_internal.h"
#include "memory/memory.h"
#include "memory/bus.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#define RAM_SIZE  0x10000
#define RESET_SSP 0x00008000u
#define RESET_PC  0x00001000u
#define MEM_A     0x00004000u /* (An) 系オペランド用のダミーアドレス */
#define MEM_B     0x00005000u

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

static mc68k_state_t *make_cpu(uint8_t *ram, mc68k_model_t model) {
    mem_ctx_t *mem = mem_create();
    mem_region_t *r = calloc(1, sizeof(mem_region_t));
    r->data = ram;
    r->base = 0x000000;
    r->size = RAM_SIZE;
    mem_add_region(mem, r);
    mc68k_bus_t bus = bus_make_hooks(mem);
    return mc68k_create_model(&bus, model);
}

/* 1命令だけ実行して消費サイクルを返す。opcode_words を RESET_PC から順に
 * 書き込み、reset 後 preset() でレジスタを整えてから実行する。 */
typedef void (*preset_fn)(mc68k_state_t *cpu);

static int exec_one_model(const uint16_t *opcode_words, size_t nwords, preset_fn preset,
                          mc68k_model_t model) {
    uint8_t ram[RAM_SIZE] = {0};
    write_long_be(ram, 0, RESET_SSP);
    write_long_be(ram, 4, RESET_PC);
    uint32_t p = RESET_PC;
    for (size_t i = 0; i < nwords; i++) {
        write_word_be(ram, p, opcode_words[i]);
        p += 2;
    }
    mc68k_state_t *cpu = make_cpu(ram, model);
    mc68k_reset(cpu);
    if (preset)
        preset(cpu);
    int consumed = mc68k_execute(cpu, 1);
    mc68k_destroy(cpu);
    return consumed;
}

static int exec_one(const uint16_t *opcode_words, size_t nwords, preset_fn preset) {
    return exec_one_model(opcode_words, nwords, preset, MC68K_MODEL_68000);
}

/* #2 [7-9] ADD.L Dn,<M>（EA=(A1)）。op_add: opmode=6(Long,Dn→EA)。
 * [UM00 Table 8-4] は 12(1/2)+EA で、(An) の Long は EA 時間 8 を加えて 20。 */
static void preset_a1_mem_a(mc68k_state_t *cpu) {
    cpu->a[1] = MEM_A;
}
static void test_cell2_add_l_dn_to_mem(void) {
    uint16_t code[] = {
        (uint16_t)(0xD000 | (0 << 9) | (6 << 6) | (2 << 3) | 1)}; /* ADD.L D0,(A1) */
    int consumed = exec_one(code, 1, preset_a1_mem_a);
    assert(consumed == 20);
}

/* #4 [7-9] AND.L Dn,<M>。op_and Dn→EA、size=4。[UM00 Table 8-4] は 12+EA（(An) の Long は EA 8 を加えて 20）。 */
static void test_cell4_and_l_dn_to_mem(void) {
    uint16_t code[] = {
        (uint16_t)(0xC000 | (0 << 9) | (6 << 6) | (2 << 3) | 1)}; /* AND.L D0,(A1) */
    int consumed = exec_one(code, 1, preset_a1_mem_a);
    assert(consumed == 20);
}

/* #5 [7-9] CMP.L <ea>,Dn（EA=Dn/An直接）。op_cmp opmode=2(Long)。是正後=6。
 * Dn直接・An直接の両方を検査する（設計書の備考どおり）。 */
static void preset_cmp_dn(mc68k_state_t *cpu) {
    cpu->d[0] = 5;
    cpu->d[1] = 3;
}
static void preset_cmp_an(mc68k_state_t *cpu) {
    cpu->d[0] = 5;
    cpu->a[1] = 3;
}
static void test_cell5_cmp_l_ea_dn(void) {
    uint16_t code_dn[] = {
        (uint16_t)(0xB000 | (0 << 9) | (2 << 6) | (0 << 3) | 1)}; /* CMP.L D1,D0 */
    assert(exec_one(code_dn, 1, preset_cmp_dn) == 6);

    uint16_t code_an[] = {
        (uint16_t)(0xB000 | (0 << 9) | (2 << 6) | (1 << 3) | 1)}; /* CMP.L A1,D0 */
    assert(exec_one(code_an, 1, preset_cmp_an) == 6);
}

/* #7 [7-9] EOR.L Dn,<M>。op_eor opmode=6(Long)。[UM00 Table 8-4] は 12+EA（20）。 */
static void test_cell7_eor_l_dn_to_mem(void) {
    uint16_t code[] = {
        (uint16_t)(0xB000 | (0 << 9) | (6 << 6) | (2 << 3) | 1)}; /* EOR.L D0,(A1) */
    int consumed = exec_one(code, 1, preset_a1_mem_a);
    assert(consumed == 20);
}

/* #9 [7-9] OR.L Dn,<M>。op_or Dn→EA、size=4。[UM00 Table 8-4] は 12+EA（20）。 */
static void test_cell9_or_l_dn_to_mem(void) {
    uint16_t code[] = {
        (uint16_t)(0x8000 | (0 << 9) | (6 << 6) | (2 << 3) | 1)}; /* OR.L D0,(A1) */
    int consumed = exec_one(code, 1, preset_a1_mem_a);
    assert(consumed == 20);
}

/* #11 [7-9] SUB.L Dn,<M>。op_sub Dn→EA、size=4。[UM00 Table 8-4] は 12(1/2)+EA で、
 * (An) の Long は EA 時間 8 を加えて 20。 */
static void test_cell11_sub_l_dn_to_mem(void) {
    uint16_t code[] = {
        (uint16_t)(0x9000 | (0 << 9) | (6 << 6) | (2 << 3) | 1)}; /* SUB.L D0,(A1) */
    int consumed = exec_one(code, 1, preset_a1_mem_a);
    assert(consumed == 20);
}

/* #12 [7-11] ADDQ.W #,An。op_addq ea_mode=1(An)、size_code=1(Word)。
 * 是正後=4（[UM00 Table 8-5]、ADDQ.WはSUBQ.Wと非対称・§2.3(b)）。 */
static void preset_a1_zero(mc68k_state_t *cpu) {
    cpu->a[1] = 0;
}
static void test_cell12_addq_w_an(void) {
    /* data=3, size_code=1(word), ea_mode=1(An), ea_reg=1(A1) */
    uint16_t code[] = {(uint16_t)(0x5000 | (3 << 9) | (1 << 6) | (1 << 3) | 1)};
    int consumed = exec_one(code, 1, preset_a1_zero);
    assert(consumed == 4);
}

/* #13 [7-11] ADDQ.L #,Dn。是正後=8（[UM00 Table 8-5]）。 */
static void test_cell13_addq_l_dn(void) {
    uint16_t code[] = {
        (uint16_t)(0x5000 | (3 << 9) | (2 << 6) | (0 << 3) | 0)}; /* ADDQ.L #3,D0 */
    int consumed = exec_one(code, 1, NULL);
    assert(consumed == 8);
}

/* #14 [7-11] ADDQ.L #,M。基本値12 + FEA((An).L)=8 = 20
 * （[UM00 Tables 8-5/8-1]、Phase 2a）。 */
static void test_cell14_addq_l_mem(void) {
    uint16_t code[] = {
        (uint16_t)(0x5000 | (3 << 9) | (2 << 6) | (2 << 3) | 1)}; /* ADDQ.L #3,(A1) */
    int consumed = exec_one(code, 1, preset_a1_mem_a);
    assert(consumed == 20);
}

/* #15 [7-11] SUBQ.L #,Dn。op_subq bit8=1で区別。是正後=8。 */
static void test_cell15_subq_l_dn(void) {
    uint16_t code[] = {
        (uint16_t)(0x5100 | (3 << 9) | (2 << 6) | (0 << 3) | 0)}; /* SUBQ.L #3,D0 */
    int consumed = exec_one(code, 1, NULL);
    assert(consumed == 8);
}

/* #16 [7-11] SUBQ.L #,M。基本値12 + FEA((An).L)=8 = 20
 * （[UM00 Tables 8-5/8-1]、Phase 2a）。 */
static void test_cell16_subq_l_mem(void) {
    uint16_t code[] = {
        (uint16_t)(0x5100 | (3 << 9) | (2 << 6) | (2 << 3) | 1)}; /* SUBQ.L #3,(A1) */
    int consumed = exec_one(code, 1, preset_a1_mem_a);
    assert(consumed == 20);
}

/* #17 [7-11] ANDI.L #,Dn。is正後=14（[UM00 Table 8-5]）。
 * イミディエイトは opcode 直後の2ワード（hi, lo）。 */
static void test_cell17_andi_l_dn(void) {
    uint16_t code[] = {(uint16_t)(0x0280 | 0), 0x0000, 0x00FF}; /* ANDI.L #$FF,D0 */
    int consumed = exec_one(code, 3, NULL);
    assert(consumed == 14);
}

/* #18 [7-11] ANDI.L #,M。基本値20 + FIEA((An).L)=8 = 28
 * （[UM00 Tables 8-5/8-1]、Phase 2a）。 */
static void test_cell18_andi_l_mem(void) {
    uint16_t code[] = {(uint16_t)(0x0280 | (2 << 3) | 1), 0x0000,
                       0x00FF}; /* ANDI.L #$FF,(A1) */
    int consumed = exec_one(code, 3, preset_a1_mem_a);
    assert(consumed == 28);
}

/* #20 [7-11] EORI.L #,M。基本値20 + FIEA((An).L)=8 = 28
 * （[UM00 Tables 8-5/8-1]、Dn形式#19はCYC_EORI_L_DNへ分類済み）。 */
static void test_cell20_eori_l_mem(void) {
    uint16_t code[] = {(uint16_t)(0x0A80 | (2 << 3) | 1), 0x0000,
                       0x00FF}; /* EORI.L #$FF,(A1) */
    int consumed = exec_one(code, 3, preset_a1_mem_a);
    assert(consumed == 28);
}

/* #22 [7-11] ORI.L #,M。基本値20 + FIEA((An).L)=8 = 28
 * （[UM00 Tables 8-5/8-1]、Dn形式#21はCYC_ORI_L_DNへ分類済み）。 */
static void test_cell22_ori_l_mem(void) {
    uint16_t code[] = {(uint16_t)(0x0080 | (2 << 3) | 1), 0x0000, 0x00FF}; /* ORI.L #$FF,(A1) */
    int consumed = exec_one(code, 3, preset_a1_mem_a);
    assert(consumed == 28);
}

/* #23 [7-12] NEG.L Memory。基本値12 + FEA((An).L)=8 = 20
 * （[UM00 Tables 8-12/8-1]、Phase 2a）。 */
static void test_cell23_neg_l_mem(void) {
    uint16_t code[] = {(uint16_t)(0x4400 | (2 << 6) | (2 << 3) | 1)}; /* NEG.L (A1) */
    int consumed = exec_one(code, 1, preset_a1_mem_a);
    assert(consumed == 20);
}

/* #24 [7-12] NEGX.L Memory。基本値12 + FEA((An).L)=8 = 20
 * （[UM00 Tables 8-12/8-1]、Phase 2a）。 */
static void test_cell24_negx_l_mem(void) {
    uint16_t code[] = {(uint16_t)(0x4000 | (2 << 6) | (2 << 3) | 1)}; /* NEGX.L (A1) */
    int consumed = exec_one(code, 1, preset_a1_mem_a);
    assert(consumed == 20);
}

/* #25 [7-12] NOT.L Memory。基本値12 + FEA((An).L)=8 = 20
 * （[UM00 Tables 8-12/8-1]、Phase 2a）。 */
static void test_cell25_not_l_mem(void) {
    uint16_t code[] = {(uint16_t)(0x4600 | (2 << 6) | (2 << 3) | 1)}; /* NOT.L (A1) */
    int consumed = exec_one(code, 1, preset_a1_mem_a);
    assert(consumed == 20);
}

/* Phase 2a integration: these memory forms use their table base plus FEA. */
static void test_memory_ea_integration(void) {
    uint16_t scc[] = {0x50D1};        /* ST (A1): 8 + FEA.B(An)=4 */
    uint16_t shift[] = {0xE2D1};      /* LSL.W (A1): 8 + FEA.W(An)=4 */
    uint16_t nbcd[] = {0x4811};       /* NBCD (A1): 8 + FEA.B(An)=4 */
    uint16_t tas[] = {0x4AD1};        /* TAS (A1): 14 + FEA.B(An)=4 */
    uint16_t btst[] = {0x0111};       /* BTST D0,(A1): 4 + FEA.B(An)=4 */
    uint16_t bclr[] = {0x0191};       /* BCLR D0,(A1): 8 + FEA.B(An)=4 */
    uint16_t move_to_sr[] = {0x46D1}; /* MOVE.W (A1),SR: 12 + FEA.W(An)=4 */
    assert(exec_one(scc, 1, preset_a1_mem_a) == 12);
    assert(exec_one(shift, 1, preset_a1_mem_a) == 12);
    assert(exec_one(nbcd, 1, preset_a1_mem_a) == 12);
    assert(exec_one(tas, 1, preset_a1_mem_a) == 18);
    assert(exec_one(btst, 1, preset_a1_mem_a) == 8);
    assert(exec_one(bclr, 1, preset_a1_mem_a) == 12);
    assert(exec_one_model(move_to_sr, 1, preset_a1_mem_a, MC68K_MODEL_68010) == 16);
}

/* #26-29 [7-15] ASR/ASL・LSR/LSL・ROR/ROL・ROXR/ROXL の .L Register 形式。
 * 是正後 = 8 + 2n（[UM00 Table 8-15]）。4種類 × count={1,3} で検算する。
 * type: 0=AS, 1=LS, 2=ROX, 3=RO（ops_shift.c op_shift の定義どおり）。
 * dr=0（右方向）で各typeを代表させる（左右で基本時間は変わらないため）。 */
static void run_shift_case(int type, int count, int expect) {
    /* creg<<9 | dr(0)<<8 | sc(2=long)<<6 | ir(0=immediate)<<5 | type<<3 | dn(0) */
    uint16_t code[] = {
        (uint16_t)(0xE000 | (count << 9) | (0 << 8) | (2 << 6) | (0 << 5) | (type << 3) | 0)};
    int consumed = exec_one(code, 1, NULL);
    assert(consumed == expect);
}
static void test_cell26_29_shift_l_register(void) {
    int types[] = {0, 1, 2, 3}; /* AS, LS, ROX, RO */
    int counts[] = {1, 3};
    for (size_t t = 0; t < sizeof(types) / sizeof(types[0]); t++) {
        for (size_t c = 0; c < sizeof(counts) / sizeof(counts[0]); c++) {
            run_shift_case(types[t], counts[c], 8 + 2 * counts[c]);
        }
    }
}

/* #30 [7-17] BCLR.L Dynamic Register。do_bit_op is_dn かつ dynamic。
 * 是正後=10（[UM00 Table 8-17]、BCHG/BSETとの非対称）。
 * ビット番号は D1 から取得（dn フィールド）、対象は D0（ea_reg）。 */
static void preset_bclr_dynamic(mc68k_state_t *cpu) {
    cpu->d[0] = 0xFFFFFFFFu;
    cpu->d[1] = 5; /* クリアするビット番号 */
}
static void test_cell30_bclr_l_dynamic_register(void) {
    /* 0x0180 | (dn<<9) | ea(mode0,reg0) : dn=1が bit番号レジスタ、reg=0が対象 */
    uint16_t code[] = {(uint16_t)(0x0180 | (1 << 9) | 0)}; /* BCLR D1,D0 */
    int consumed = exec_one(code, 1, preset_bclr_dynamic);
    assert(consumed == 10);
}

/* #31 [7-17] BCLR.L Static Register。是正後=14。ビット番号は拡張ワード。 */
static void preset_bclr_static(mc68k_state_t *cpu) {
    cpu->d[0] = 0xFFFFFFFFu;
}
static void test_cell31_bclr_l_static_register(void) {
    uint16_t code[] = {(uint16_t)(0x0880 | 0), 0x0005}; /* BCLR #5,D0 */
    int consumed = exec_one(code, 2, preset_bclr_static);
    assert(consumed == 14);
}

/* #32 [7-20] ADDX.B/W -(An),-(An)。R-D（自己完結、EA加算なし）。
 * 是正後=18（Byte/Word共通、[UM00 Table 8-11]）。A0/A1を非重複のダミー
 * アドレスへ事前設定し、実行後にデクリメントされることも確認する。 */
static void preset_addx_predec(mc68k_state_t *cpu) {
    cpu->a[0] = MEM_A + 4; /* Ax = rx, オペコードのビット並びに合わせる（下記） */
    cpu->a[1] = MEM_B + 4; /* Ay = ry */
}
static void test_cell32_addx_bw_predec(void) {
    /* rx=0(Ax=A0,格納先), ry=1(Ay=A1,ソース), m=1(predecrement) */
    uint16_t code_b[] = {
        (uint16_t)(0xD100 | (0 << 9) | (0 << 6) | 0x08 | 1)}; /* ADDX.B -(A1),-(A0) */
    assert(exec_one(code_b, 1, preset_addx_predec) == 18);

    uint16_t code_w[] = {
        (uint16_t)(0xD100 | (0 << 9) | (1 << 6) | 0x08 | 1)}; /* ADDX.W -(A1),-(A0) */
    assert(exec_one(code_w, 1, preset_addx_predec) == 18);
}

/* #33 [7-20] ADDX.L -(An),-(An)。是正後=30。 */
static void test_cell33_addx_l_predec(void) {
    uint16_t code[] = {
        (uint16_t)(0xD100 | (0 << 9) | (2 << 6) | 0x08 | 1)}; /* ADDX.L -(A1),-(A0) */
    int consumed = exec_one(code, 1, preset_addx_predec);
    assert(consumed == 30);
}

/* #34 [7-20] SUBX.B/W -(An),-(An)。是正後=18。 */
static void test_cell34_subx_bw_predec(void) {
    uint16_t code_b[] = {
        (uint16_t)(0x9100 | (0 << 9) | (0 << 6) | 0x08 | 1)}; /* SUBX.B -(A1),-(A0) */
    assert(exec_one(code_b, 1, preset_addx_predec) == 18);

    uint16_t code_w[] = {
        (uint16_t)(0x9100 | (0 << 9) | (1 << 6) | 0x08 | 1)}; /* SUBX.W -(A1),-(A0) */
    assert(exec_one(code_w, 1, preset_addx_predec) == 18);
}

/* #35 [7-20] SUBX.L -(An),-(An)。是正後=30。 */
static void test_cell35_subx_l_predec(void) {
    uint16_t code[] = {
        (uint16_t)(0x9100 | (0 << 9) | (2 << 6) | 0x08 | 1)}; /* SUBX.L -(A1),-(A0) */
    int consumed = exec_one(code, 1, preset_addx_predec);
    assert(consumed == 30);
}

int main(void) {
    test_cell2_add_l_dn_to_mem();
    test_cell4_and_l_dn_to_mem();
    test_cell5_cmp_l_ea_dn();
    test_cell7_eor_l_dn_to_mem();
    test_cell9_or_l_dn_to_mem();
    test_cell11_sub_l_dn_to_mem();
    test_cell12_addq_w_an();
    test_cell13_addq_l_dn();
    test_cell14_addq_l_mem();
    test_cell15_subq_l_dn();
    test_cell16_subq_l_mem();
    test_cell17_andi_l_dn();
    test_cell18_andi_l_mem();
    test_cell20_eori_l_mem();
    test_cell22_ori_l_mem();
    test_cell23_neg_l_mem();
    test_cell24_negx_l_mem();
    test_cell25_not_l_mem();
    test_memory_ea_integration();
    test_cell26_29_shift_l_register();
    test_cell30_bclr_l_dynamic_register();
    test_cell31_bclr_l_static_register();
    test_cell32_addx_bw_predec();
    test_cell33_addx_l_predec();
    test_cell34_subx_bw_predec();
    test_cell35_subx_l_predec();
    return 0;
}
