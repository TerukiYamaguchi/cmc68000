/*
 * test_move.c — CPU 実行基盤 + MOVE 命令ユニットテスト
 *
 * テストプログラムは生の MC68000 バイナリを RAM に書き込んで実行する。
 * 各テストの末尾に ILLEGAL（0x4AFC）を置いて実行を止める。
 *
 * opcode クイックリファレンス（よく使うもの）:
 *   MOVE.W #imm, Dn  : 0x303C + imm16       （#imm → D0）
 *   MOVE.W Dn, Dm    : 0x3000 | (dst<<9) | src  例: D0→D1 = 0x3200
 *   MOVE.L #imm, Dn  : 0x203C + imm32       （2 word）
 *   MOVEA.W #imm, An : 0x307C | (An<<9)     例: MOVEA.W #imm,A0 = 0x307C
 *   MOVEA.L #imm, An : 0x207C | (An<<9)
 *   MOVE.W Dn, (An)  : 0x3080 | (An<<9) | Dn
 *   MOVE.W (An), Dn  : 0x3010 | (Dn<<9) | An
 */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "mc68000/cpu.h"
#include "cpu/cpu_internal.h"
#include "memory/memory.h"
#include "memory/bus.h"

#define RAM_SIZE 0x10000

static mc68k_state_t *make_cpu_model(uint8_t *ram, mc68k_model_t model) {
    /* テスト終端の ILLEGAL(0x4AFC)/未割当 opcode は実機通りベクタ4 経由で例外
     * ディスパッチされる。個別設定が無ければ、CCR を壊さず停止する自己ループ
     * (BRA *) へ向けて、テストが意図通り停止できるようにする。 */
    if (ram[0x10] == 0 && ram[0x11] == 0 && ram[0x12] == 0 && ram[0x13] == 0) {
        ram[0x12] = 0x04; /* ベクタ4 → 0x00000400 */
        ram[0x400] = 0x60;
        ram[0x401] = 0xFE; /* 0x400: BRA *（停止） */
    }
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

static void write_long_be(uint8_t *ram, uint32_t addr, uint32_t val) {
    ram[addr + 0] = (uint8_t)(val >> 24);
    ram[addr + 1] = (uint8_t)(val >> 16);
    ram[addr + 2] = (uint8_t)(val >> 8);
    ram[addr + 3] = (uint8_t)(val);
}

static void write_word_be(uint8_t *ram, uint32_t addr, uint16_t val) {
    ram[addr + 0] = (uint8_t)(val >> 8);
    ram[addr + 1] = (uint8_t)(val);
}

/* reset + 最大 cycles サイクル実行して regs を返す */
static void run(mc68k_state_t *cpu, int cycles, mc68k_regs_t *out) {
    mc68k_reset(cpu);
    mc68k_execute(cpu, cycles);
    if (out)
        mc68k_get_regs(cpu, out);
}

/* ------------------------------------------------------------------ */
/* ステップ 3-1（実行基盤）のテスト                                    */
/* ------------------------------------------------------------------ */

static void test_reset(void) {
    uint8_t ram[RAM_SIZE];
    memset(ram, 0, sizeof(ram));
    write_long_be(ram, 0x000000, 0x00008000); /* SSP = 0x8000 */
    write_long_be(ram, 0x000004, 0x00000100); /* PC  = 0x0100 */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_regs_t regs;
    run(cpu, 0, &regs); /* cycle=0 なので命令は実行しない */

    assert(regs.pc == 0x00000100);
    assert(regs.ssp == 0x00008000);
    assert(regs.a[7] == 0x00008000);      /* スーパーバイザモード中は A7 = SSP */
    assert(regs.sr & 0x2000);             /* スーパーバイザビット */
    assert((regs.sr & 0x0700) == 0x0700); /* 割り込みマスク = 7 */

    mc68k_destroy(cpu);
}

static void test_execute_illegal(void) {
    /*
     * ILLEGAL (0x4AFC) は実機通りベクタ4（アドレス 0x10）へ例外ディスパッチする。
     * ベクタを有効なハンドラ（BRA * の自己ループ）へ向けて、確かに分岐したか検証する。
     */
    uint8_t ram[RAM_SIZE];
    memset(ram, 0, sizeof(ram));
    write_long_be(ram, 0x000000, 0x00008000); /* SSP */
    write_long_be(ram, 0x000004, 0x00000100); /* PC  */
    write_long_be(ram, 0x000010, 0x00000200); /* ベクタ4: ILLEGAL → 0x200 */
    write_word_be(ram, 0x0100, 0x4AFC);       /* ILLEGAL */
    write_word_be(ram, 0x0200, 0x60FE);       /* ハンドラ: BRA *（停止） */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_regs_t regs;
    run(cpu, 100, &regs);

    assert(regs.pc == 0x00000200);       /* ベクタ4 のハンドラへ分岐 */
    assert(regs.sr & 0x2000);            /* スーパーバイザモード */
    assert(regs.a[7] == 0x00008000 - 6); /* PC(4)+SR(2) をスタックへプッシュ */

    mc68k_destroy(cpu);
}

/* ------------------------------------------------------------------ */
/* ステップ 3-2: MOVE.B/W/L                                           */
/* ------------------------------------------------------------------ */

static void test_move_word_imm_to_dn(void) {
    /*
     * プログラム:
     *   MOVE.W #0x1234, D0   ; 0x303C 0x1234
     *   ILLEGAL              ; 0x4AFC
     *
     * MOVE.W #imm, D0 のオペコード 0x303C:
     *   bits 15-12: 0011 (ライン 3 = MOVE.W)
     *   bits 11-9:  000  (宛先 reg = D0)
     *   bits  8-6:  000  (宛先 mode = Dn)
     *   bits  5-3:  111  (ソース mode = 7)
     *   bits  2-0:  100  (ソース reg = 4 → #imm)
     */
    uint8_t ram[RAM_SIZE];
    memset(ram, 0, sizeof(ram));
    write_long_be(ram, 0x000000, 0x00008000);
    write_long_be(ram, 0x000004, 0x00000100);
    write_word_be(ram, 0x0100, 0x303C); /* MOVE.W #imm, D0 */
    write_word_be(ram, 0x0102, 0x1234); /* imm = 0x1234 */
    write_word_be(ram, 0x0104, 0x4AFC); /* ILLEGAL */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_regs_t regs;
    run(cpu, 100, &regs);

    assert(regs.d[0] == 0x1234);
    assert(!(regs.sr & 0x08)); /* N クリア */
    assert(!(regs.sr & 0x04)); /* Z クリア */
    assert(!(regs.sr & 0x02)); /* V クリア */
    assert(!(regs.sr & 0x01)); /* C クリア */

    mc68k_destroy(cpu);
}

static void test_move_word_dn_to_dn(void) {
    /*
     * プログラム:
     *   MOVE.W #0x5678, D0   ; D0 に値をロード
     *   MOVE.W D0, D1        ; D0 → D1 へコピー（0x3200）
     *   ILLEGAL
     *
     * MOVE.W D0, D1 = 0x3200:
     *   bits 11-9: 001 (D1)、bits 8-6: 000 (Dn)
     *   bits  5-3: 000 (Dn)、bits 2-0: 000 (D0)
     */
    uint8_t ram[RAM_SIZE];
    memset(ram, 0, sizeof(ram));
    write_long_be(ram, 0x000000, 0x00008000);
    write_long_be(ram, 0x000004, 0x00000100);
    write_word_be(ram, 0x0100, 0x303C); /* MOVE.W #0x5678, D0 */
    write_word_be(ram, 0x0102, 0x5678);
    write_word_be(ram, 0x0104, 0x3200); /* MOVE.W D0, D1 */
    write_word_be(ram, 0x0106, 0x4AFC); /* ILLEGAL */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_regs_t regs;
    run(cpu, 100, &regs);

    assert(regs.d[0] == 0x5678);
    assert(regs.d[1] == 0x5678);

    mc68k_destroy(cpu);
}

static void test_move_byte_preserves_upper(void) {
    /*
     * MOVE.B は宛先レジスタの下位 8bit のみ更新し、上位 24bit を保持する。
     * プログラム:
     *   MOVE.L #0xDEADBEEF, D0   ; D0 = 0xDEADBEEF
     *   MOVE.B #0x42, D0         ; D0 の下位バイトのみ 0x42 に → 0xDEADBE42
     *   ILLEGAL
     */
    uint8_t ram[RAM_SIZE];
    memset(ram, 0, sizeof(ram));
    write_long_be(ram, 0x000000, 0x00008000);
    write_long_be(ram, 0x000004, 0x00000100);
    /* MOVE.L #imm32, D0 = 0x203C + 4bytes */
    write_word_be(ram, 0x0100, 0x203C);
    write_long_be(ram, 0x0102, 0xDEADBEEF);
    /* MOVE.B #0x42, D0 = 0x103C + 0x0042 */
    write_word_be(ram, 0x0106, 0x103C);
    write_word_be(ram, 0x0108, 0x0042); /* バイトイミディエイトは下位バイトに入る */
    write_word_be(ram, 0x010A, 0x4AFC);

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_regs_t regs;
    run(cpu, 100, &regs);

    assert(regs.d[0] == 0xDEADBE42); /* 上位 24bit は保持 */

    mc68k_destroy(cpu);
}

static void test_move_ccr_negative(void) {
    /*
     * 負数（MSB = 1）を MOVE したとき N フラグが立つことを確認。
     * MOVE.W #0x8000, D0 → D0 = 0x8000（0x8000 は符号付きで -32768）
     */
    uint8_t ram[RAM_SIZE];
    memset(ram, 0, sizeof(ram));
    write_long_be(ram, 0x000000, 0x00008000);
    write_long_be(ram, 0x000004, 0x00000100);
    write_word_be(ram, 0x0100, 0x303C);
    write_word_be(ram, 0x0102, 0x8000); /* -32768 */
    write_word_be(ram, 0x0104, 0x4AFC);

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_regs_t regs;
    run(cpu, 100, &regs);

    assert(regs.sr & 0x08);    /* N セット */
    assert(!(regs.sr & 0x04)); /* Z クリア */

    mc68k_destroy(cpu);
}

static void test_move_ccr_zero(void) {
    /* MOVE.W #0, D0 → Z フラグが立つ */
    uint8_t ram[RAM_SIZE];
    memset(ram, 0, sizeof(ram));
    write_long_be(ram, 0x000000, 0x00008000);
    write_long_be(ram, 0x000004, 0x00000100);
    write_word_be(ram, 0x0100, 0x303C);
    write_word_be(ram, 0x0102, 0x0000); /* 0 */
    write_word_be(ram, 0x0104, 0x4AFC);

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_regs_t regs;
    run(cpu, 100, &regs);

    assert(!(regs.sr & 0x08)); /* N クリア */
    assert(regs.sr & 0x04);    /* Z セット */

    mc68k_destroy(cpu);
}

static void test_movea_word_sign_extend(void) {
    /*
     * MOVEA.W はワード値をアドレスレジスタに格納する前に 32bit に符号拡張する。
     *   0x8000（MSB=1）→ 0xFFFF8000（負数として符号拡張）
     *   0x7FFF（MSB=0）→ 0x00007FFF（正数としてゼロ拡張）
     * また MOVEA は CCR を変更しない。
     *
     * MOVEA.W #0x8000, A1 = 0x3240 + 0x8000:
     *   bits 11-9: 001 (A1)、bits 8-6: 001 (An mode)
     *   bits  5-3: 111、bits 2-0: 100 → #imm
     *   = 0x3000 | (001<<9) | (001<<6) | (111<<3) | 100
     *   = 0x3000 | 0x0200 | 0x0040 | 0x0038 | 0x0004 = 0x327C
     */
    uint8_t ram[RAM_SIZE];
    memset(ram, 0, sizeof(ram));
    write_long_be(ram, 0x000000, 0x00008000);
    write_long_be(ram, 0x000004, 0x00000100);
    write_word_be(ram, 0x0100, 0x327C); /* MOVEA.W #imm, A1 */
    write_word_be(ram, 0x0102, 0x8000); /* 0x8000 → 符号拡張で 0xFFFF8000 */
    write_word_be(ram, 0x0104, 0x4AFC);

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_regs_t regs;
    run(cpu, 100, &regs);

    assert(regs.a[1] == 0xFFFF8000U); /* 符号拡張確認 */
    /* MOVEA は CCR を変更しない（リセット後の初期値のまま） */
    assert(!(regs.sr & 0x04)); /* Z は立っていないはず（reset 後は 0） */

    mc68k_destroy(cpu);
}

static void test_move_memory(void) {
    /*
     * MOVE.W D0, (A0) でメモリに書いて、
     * MOVE.W (A0), D1 でメモリから読む。
     *
     * MOVE.W D0, (A0) = 0x3080:
     *   宛先: A0(mode=010, reg=000) → bits 11-6 = 000010
     *   ソース: D0(mode=000, reg=000) → bits 5-0 = 000000
     *   = 0x3000 | (000<<9) | (010<<6) | 0 = 0x3000 | 0x0080 = 0x3080
     *
     * MOVE.W (A0), D1 = 0x3210:
     *   宛先: D1(mode=000, reg=001) → bits 11-6 = 001000
     *   ソース: (A0)(mode=010, reg=000) → bits 5-0 = 010000 = 0x10
     *   = 0x3000 | (001<<9) | (000<<6) | (010<<3) | 000 = 0x3000|0x0200|0x0010 = 0x3210
     */
    uint8_t ram[RAM_SIZE];
    memset(ram, 0, sizeof(ram));
    write_long_be(ram, 0x000000, 0x00008000);
    write_long_be(ram, 0x000004, 0x00000100);

    /* MOVEA.L #0x0200, A0（A0 = 0x200 = メモリ書き込み先）
     * MOVEA.L #imm, A0 = 0x207C + imm32 */
    write_word_be(ram, 0x0100, 0x207C);     /* MOVEA.L #imm, A0 */
    write_long_be(ram, 0x0102, 0x00000200); /* A0 = 0x200 */
    write_word_be(ram, 0x0106, 0x303C);     /* MOVE.W #0xABCD, D0 */
    write_word_be(ram, 0x0108, 0xABCD);
    write_word_be(ram, 0x010A, 0x3080); /* MOVE.W D0, (A0) */
    write_word_be(ram, 0x010C, 0x3210); /* MOVE.W (A0), D1 */
    write_word_be(ram, 0x010E, 0x4AFC); /* ILLEGAL */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_regs_t regs;
    run(cpu, 200, &regs);

    assert(regs.d[0] == 0xABCD);
    assert(regs.d[1] == 0xABCD);
    /* メモリにも正しく書かれていること（ビッグエンディアン確認） */
    assert(ram[0x200] == 0xAB);
    assert(ram[0x201] == 0xCD);

    mc68k_destroy(cpu);
}

static void test_move_postinc(void) {
    /*
     * (A0)+ アドレッシングモード: 読み出し後に A0 をサイズ分進める。
     * プログラム:
     *   MOVEA.L #0x200, A0   ; A0 = 0x200
     *   MOVE.W #0x1122, D0   ; D0 = 0x1122
     *   MOVE.W D0, (A0)+     ; [0x200] = 0x1122, A0 → 0x202
     *   MOVE.W (A0)+, D1     ; D1 = [0x202], A0 → 0x204
     *
     * MOVE.W D0, (A0)+ = 0x30C0:
     *   宛先: (A0)+(mode=011, reg=000) → bits 11-6 = 000011
     *   = 0x3000 | (000011<<6) | 0 = 0x30C0
     *
     * MOVE.W (A0)+, D1 = 0x3218:
     *   宛先: D1(mode=000, reg=001) → bits 11-6 = 001000
     *   ソース: (A0)+(mode=011, reg=000) → bits 5-0 = 011000 = 0x18
     *   = 0x3000 | 0x0200 | 0x0018 = 0x3218
     */
    uint8_t ram[RAM_SIZE];
    memset(ram, 0, sizeof(ram));
    write_long_be(ram, 0x000000, 0x00008000);
    write_long_be(ram, 0x000004, 0x00000100);

    write_word_be(ram, 0x0100, 0x207C);
    write_long_be(ram, 0x0102, 0x00000200);
    write_word_be(ram, 0x0106, 0x303C);
    write_word_be(ram, 0x0108, 0x1122);
    write_word_be(ram, 0x010A, 0x30C0); /* MOVE.W D0, (A0)+ */
    write_word_be(ram, 0x010C, 0x3218); /* MOVE.W (A0)+, D1 */
    write_word_be(ram, 0x010E, 0x4AFC);

    /* 0x202 に 0xAABB を置いておく（(A0)+ で D1 に読まれるデータ） */
    write_word_be(ram, 0x0202, 0xAABB);

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_regs_t regs;
    run(cpu, 200, &regs);

    assert(regs.d[0] == 0x1122);
    assert(regs.d[1] == 0xAABB);
    assert(regs.a[0] == 0x204); /* 2 回インクリメント済み */

    mc68k_destroy(cpu);
}

/* : 040UM Table 10-4 + Table 10-2.  Execute one instruction at a
 * time so the asserted cycle count is the handler's nominal value, not the
 * following ILLEGAL exception. */
static void test_move_68040_table10_4_direct_forms(void) {
    struct {
        uint16_t opcode, extension;
        uint32_t a0, a1;
        int cycles;
    } cases[] = {
        {0x3200, 0, 0, 0, 2},          /* D0,D1 */
        {0x3210, 0, 0x200, 0, 3},      /* (A0),D1: source fetch */
        {0x32D8, 0, 0x200, 0x220, 4},  /* (A0)+,(A1)+ */
        {0x3380, 0x1000, 0, 0x220, 6}, /* D0,(d8,A1,D1) */
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        uint8_t ram[RAM_SIZE];
        memset(ram, 0, sizeof(ram));
        write_long_be(ram, 0, 0x8000);
        write_long_be(ram, 4, 0x100);
        write_word_be(ram, 0x100, cases[i].opcode);
        write_word_be(ram, 0x102, cases[i].extension);
        mc68k_state_t *cpu = make_cpu_model(ram, MC68K_MODEL_68040);
        mc68k_reset(cpu);
        cpu->d[0] = 0x1234;
        cpu->d[1] = 0;
        cpu->a[0] = cases[i].a0;
        cpu->a[1] = cases[i].a1;
        write_word_be(ram, 0x200, 0x5678);
        assert(mc68k_execute(cpu, 1) == cases[i].cycles);
        mc68k_destroy(cpu);
    }
}

/* Table 10-4's full-extension rows: direct source/destination and pre-index
 * source.  0x0120 is full format, D0.W index, bd.W, direct; 0x0121 is the
 * matching pre-index form with null outer displacement. */
static void test_move_68040_table10_4_full_extension(void) {
    uint8_t ram[RAM_SIZE];
    memset(ram, 0, sizeof(ram));
    write_long_be(ram, 0, 0x8000);
    write_long_be(ram, 4, 0x100);
    write_word_be(ram, 0x100, 0x3230); /* MOVE.W (bd.W,A0,D0.W),D1 */
    write_word_be(ram, 0x102, 0x0120);
    write_word_be(ram, 0x104, 0);
    write_word_be(ram, 0x200, 0x5678);
    mc68k_state_t *cpu = make_cpu_model(ram, MC68K_MODEL_68040);
    mc68k_reset(cpu);
    cpu->a[0] = 0x200;
    assert(mc68k_execute(cpu, 1) == 14); /* 13 calculate+execute + fetch */
    mc68k_destroy(cpu);

    memset(ram, 0, sizeof(ram));
    write_long_be(ram, 0, 0x8000);
    write_long_be(ram, 4, 0x100);
    write_word_be(ram, 0x100, 0x3381); /* MOVE.W D1,(bd.W,A1,D0.W) */
    write_word_be(ram, 0x102, 0x0120);
    write_word_be(ram, 0x104, 0);
    cpu = make_cpu_model(ram, MC68K_MODEL_68040);
    mc68k_reset(cpu);
    cpu->d[1] = 0x1234;
    cpu->a[1] = 0x220;
    assert(mc68k_execute(cpu, 1) == 13);
    assert(ram[0x220] == 0x12 && ram[0x221] == 0x34);
    mc68k_destroy(cpu);

    memset(ram, 0, sizeof(ram));
    write_long_be(ram, 0, 0x8000);
    write_long_be(ram, 4, 0x100);
    write_word_be(ram, 0x100, 0x3230); /* MOVE.W ([bd.W,A0,D0.W]),D1 */
    write_word_be(ram, 0x102, 0x0121);
    write_word_be(ram, 0x104, 0);
    write_long_be(ram, 0x200, 0x240);
    write_word_be(ram, 0x240, 0x9abc);
    cpu = make_cpu_model(ram, MC68K_MODEL_68040);
    mc68k_reset(cpu);
    cpu->a[0] = 0x200;
    assert(mc68k_execute(cpu, 1) == 21); /* 19 calculate+execute + two fetches */
    mc68k_destroy(cpu);
}

/* ------------------------------------------------------------------ */

int main(void) {
    /* ステップ 3-1: 実行基盤 */
    test_reset();
    test_execute_illegal();

    /* ステップ 3-2: MOVE */
    test_move_word_imm_to_dn();
    test_move_word_dn_to_dn();
    test_move_byte_preserves_upper();
    test_move_ccr_negative();
    test_move_ccr_zero();
    test_movea_word_sign_extend();
    test_move_memory();
    test_move_postinc();
    test_move_68040_table10_4_direct_forms();
    test_move_68040_table10_4_full_extension();

    return 0;
}
