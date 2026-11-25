/*
 * test_bus_error.c — バスエラー例外（ベクタ2）のユニットテスト
 *
 * mc68000 コアはメモリマップを持たないため、「未マップアドレス」の判定は
 * ホスト（x68000側）が自前のバスコールバック（read_byte 等）内で行い、
 * mc68k_signal_bus_error() で CPU に通知する設計になっている。
 * このテストは、その通知経路を素朴なバスフック実装で再現し、
 * 実際に CPU がベクタ2へグループ0例外ディスパッチすることを検証する。
 *
 * また、アドレスエラー（ベクタ3）と混同していないこと（別ベクタに飛ぶこと）も確認する。
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "mc68000/cpu.h"
#include "mc68000/cpu_types.h"
#include "mc68000/bus_hooks.h"

#define RAM_SIZE         0x10000u /* このテストの「マップ済み」領域サイズ */
#define RESET_SSP        0x00008000u
#define RESET_PC         0x00001000u
#define BUS_ERR_HANDLER  0x00000300u /* ベクタ2ハンドラ（自己ループで停止） */
#define ADDR_ERR_HANDLER 0x00000340u /* ベクタ3ハンドラ（自己ループで停止） */
#define HOLE_ADDR        0x00800000u /* RAM_SIZE の外 = 未マップとして扱う */

typedef struct {
    uint8_t ram[RAM_SIZE];
    mc68k_state_t *cpu; /* mc68k_create() 後にセットする（コールバックへ渡すため） */
} test_ctx_t;

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

/* --- バスコールバック: RAM_SIZE 未満だけが「マップ済み」、それ以外はバスエラー --- */

static bool tb_read_byte(void *ctx, uint32_t addr, uint8_t *out) {
    test_ctx_t *t = (test_ctx_t *)ctx;
    if (addr >= RAM_SIZE) {
        *out = 0xFF;
        return false;
    }
    *out = t->ram[addr];
    return true;
}

static bool tb_read_word(void *ctx, uint32_t addr, uint16_t *out) {
    test_ctx_t *t = (test_ctx_t *)ctx;
    if (addr + 1 >= RAM_SIZE) {
        *out = 0xFFFF;
        return false;
    }
    *out = (uint16_t)((t->ram[addr] << 8) | t->ram[addr + 1]);
    return true;
}

static bool tb_read_long(void *ctx, uint32_t addr, uint32_t *out) {
    if (addr + 3 >= RAM_SIZE) {
        *out = 0xFFFFFFFF;
        return false;
    }
    uint16_t hi = 0, lo = 0;
    tb_read_word(ctx, addr, &hi);
    tb_read_word(ctx, addr + 2, &lo);
    *out = ((uint32_t)hi << 16) | lo;
    return true;
}

static bool tb_write_byte(void *ctx, uint32_t addr, uint8_t val) {
    test_ctx_t *t = (test_ctx_t *)ctx;
    if (addr >= RAM_SIZE)
        return false;
    t->ram[addr] = val;
    return true;
}

static bool tb_write_word(void *ctx, uint32_t addr, uint16_t val) {
    test_ctx_t *t = (test_ctx_t *)ctx;
    if (addr + 1 >= RAM_SIZE)
        return false;
    t->ram[addr] = (uint8_t)(val >> 8);
    t->ram[addr + 1] = (uint8_t)(val & 0xFF);
    return true;
}

static bool tb_write_long(void *ctx, uint32_t addr, uint32_t val) {
    bool ok1 = tb_write_word(ctx, addr, (uint16_t)(val >> 16));
    bool ok2 = tb_write_word(ctx, addr + 2, (uint16_t)(val & 0xFFFF));
    return ok1 && ok2;
}

static void tb_sync(void *ctx, int cycles) {
    (void)ctx;
    (void)cycles;
}
static void tb_write_hook(void *ctx, uint32_t addr, uint32_t size) {
    (void)ctx;
    (void)addr;
    (void)size;
}

static mc68k_state_t *make_cpu_model(test_ctx_t *t, mc68k_model_t model) {
    mc68k_bus_t bus = {
        .read_byte = tb_read_byte,
        .read_word = tb_read_word,
        .read_long = tb_read_long,
        .write_byte = tb_write_byte,
        .write_word = tb_write_word,
        .write_long = tb_write_long,
        .sync = tb_sync,
        .write_hook = tb_write_hook,
        .iack = NULL,
        .reset_peripheral = NULL,
        .ctx = t,
    };
    /* ctx (t) は既に .ctx にセット済み。cpu だけ後から埋める */
    t->cpu = mc68k_create_model(&bus, model);
    return t->cpu;
}

static mc68k_state_t *make_cpu(test_ctx_t *t) {
    return make_cpu_model(t, MC68K_MODEL_68000);
}

/* 共通のベクタ/ハンドラ設定: SSP/PC、ベクタ2/3 ハンドラ（BRA * で自己ループ停止） */
static void setup_common(uint8_t *ram) {
    write_long_be(ram, 0x000000, RESET_SSP);
    write_long_be(ram, 0x000004, RESET_PC);
    write_long_be(ram, 0x000008, BUS_ERR_HANDLER);  /* ベクタ2: バスエラー */
    write_long_be(ram, 0x00000C, ADDR_ERR_HANDLER); /* ベクタ3: アドレスエラー */
    write_word_be(ram, BUS_ERR_HANDLER, 0x60FE);    /* BRA * */
    write_word_be(ram, ADDR_ERR_HANDLER, 0x60FE);   /* BRA * */
}

/*
 * TST.B (A0) で未マップアドレスを読む → バスエラーでベクタ2へ。
 *   MOVEA.L #HOLE_ADDR, A0 : 0x207C + imm32
 *   TST.B (A0)             : 0x4A10
 */
static void test_bus_error_on_read(void) {
    test_ctx_t t;
    memset(t.ram, 0, sizeof(t.ram));
    setup_common(t.ram);

    uint32_t p = RESET_PC;
    write_word_be(t.ram, p, 0x207C);
    p += 2; /* MOVEA.L #imm, A0 */
    write_long_be(t.ram, p, HOLE_ADDR);
    p += 4;
    write_word_be(t.ram, p, 0x4A10);
    p += 2; /* TST.B (A0) */

    mc68k_state_t *cpu = make_cpu(&t);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);

    assert(r.pc == BUS_ERR_HANDLER);
    assert(r.sr & 0x2000);            /* スーパーバイザモード */
    assert(r.a[7] == RESET_SSP - 14); /* グループ0例外の14バイトフレーム */
    assert(mc68k_get_addr_err_addr(cpu) == HOLE_ADDR);

    /* SSW (SSP+0): R/W ビット = 1 (読み出し) */
    uint16_t ssw = (uint16_t)((t.ram[r.a[7] + 0] << 8) | t.ram[r.a[7] + 1]);
    assert(ssw & 0x0010);
    /* アクセスアドレス (SSP+2, long) */
    uint32_t frame_addr = ((uint32_t)t.ram[r.a[7] + 2] << 24) |
                          ((uint32_t)t.ram[r.a[7] + 3] << 16) |
                          ((uint32_t)t.ram[r.a[7] + 4] << 8) | (uint32_t)t.ram[r.a[7] + 5];
    assert(frame_addr == HOLE_ADDR);
    /* スタックされたPC (SSP+10, long) は、フォルトした TST.B (A0) の
     * 命令語をフェッチした後の値（RESET_PC+8）であること。 */
    uint32_t frame_pc = ((uint32_t)t.ram[r.a[7] + 10] << 24) |
                        ((uint32_t)t.ram[r.a[7] + 11] << 16) |
                        ((uint32_t)t.ram[r.a[7] + 12] << 8) | (uint32_t)t.ram[r.a[7] + 13];
    assert(frame_pc == RESET_PC + 8);

    mc68k_destroy(cpu);
}

/*
 * CLR.B (A0) で未マップアドレスに書く → バスエラーでベクタ2へ。
 *   MOVEA.L #HOLE_ADDR, A0 : 0x207C + imm32
 *   MOVE.B D0,(A0)         : 0x1080
 */
static void test_bus_error_on_write(void) {
    test_ctx_t t;
    memset(t.ram, 0, sizeof(t.ram));
    setup_common(t.ram);

    uint32_t p = RESET_PC;
    write_word_be(t.ram, p, 0x207C);
    p += 2; /* MOVEA.L #imm, A0 */
    write_long_be(t.ram, p, HOLE_ADDR);
    p += 4;
    write_word_be(t.ram, p, 0x1080);
    p += 2; /* MOVE.B D0,(A0)（CLR は書込み前に read するため write の試験には使えない） */

    mc68k_state_t *cpu = make_cpu(&t);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);

    assert(r.pc == BUS_ERR_HANDLER);
    assert(r.a[7] == RESET_SSP - 14);
    assert(mc68k_get_addr_err_addr(cpu) == HOLE_ADDR);

    /* SSW (SSP+0): R/W ビット = 0 (書き込み) */
    uint16_t ssw = (uint16_t)((t.ram[r.a[7] + 0] << 8) | t.ram[r.a[7] + 1]);
    assert(!(ssw & 0x0010));

    /* スタックされたPC (SSP+10, long) は CLR.B (A0) の命令語フェッチ後（RESET_PC+8）。*/
    uint32_t frame_pc = ((uint32_t)t.ram[r.a[7] + 10] << 24) |
                        ((uint32_t)t.ram[r.a[7] + 11] << 16) |
                        ((uint32_t)t.ram[r.a[7] + 12] << 8) | (uint32_t)t.ram[r.a[7] + 13];
    assert(frame_pc == RESET_PC + 8);

    mc68k_destroy(cpu);
}

/*
 * 奇数アドレスへの word アクセス（アドレスエラー）は、
 * バスエラーではなくベクタ3（ADDR_ERR_HANDLER）へディスパッチされることを確認する。
 * fault_vector によるベクタ2/3の混同がないことの回帰確認。
 *   MOVEA.L #0x1001, A0 : 0x207C + imm32
 *   TST.W (A0)          : 0x4A50
 */
static void test_address_error_not_confused_with_bus_error(void) {
    test_ctx_t t;
    memset(t.ram, 0, sizeof(t.ram));
    setup_common(t.ram);

    uint32_t p = RESET_PC;
    write_word_be(t.ram, p, 0x207C);
    p += 2; /* MOVEA.L #imm, A0 */
    write_long_be(t.ram, p, RESET_PC + 1);
    p += 4; /* 奇数アドレス（マップ済み領域内）*/
    write_word_be(t.ram, p, 0x4A50);
    p += 2; /* TST.W (A0) */

    mc68k_state_t *cpu = make_cpu(&t);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 200);

    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);

    assert(r.pc == ADDR_ERR_HANDLER); /* ベクタ2ではなくベクタ3 */
    assert(r.a[7] == RESET_SSP - 14);

    /* スタックされたPC (SSP+10, long) は TST.W (A0) の命令語フェッチ後（RESET_PC+8）。 */
    uint32_t frame_pc = ((uint32_t)t.ram[r.a[7] + 10] << 24) |
                        ((uint32_t)t.ram[r.a[7] + 11] << 16) |
                        ((uint32_t)t.ram[r.a[7] + 12] << 8) | (uint32_t)t.ram[r.a[7] + 13];
    assert(frame_pc == RESET_PC + 8);

    mc68k_destroy(cpu);
}

/* ------------------------------------------------------------------ */
/* 連続するグループ0フォルトで、前のフォルトの情報が残らないこと        */
/* ------------------------------------------------------------------ */

#define SECOND_HANDLER 0x00000380u          /* 2回目のフォルト用ハンドラ（自己ループで停止） */
#define PROG2_PC       0x00002000u          /* reset後に実行する2本目のプログラム */
#define SECOND_HOLE    (HOLE_ADDR + 0x100u) /* 2回目のフォルトのアクセス先 */

static const mc68k_model_t k_all_models[] = {MC68K_MODEL_68000, MC68K_MODEL_68010,
                                             MC68K_MODEL_68020, MC68K_MODEL_68EC030,
                                             MC68K_MODEL_68030};

static uint32_t read_long_be(const uint8_t *ram, uint32_t addr) {
    return ((uint32_t)ram[addr] << 24) | ((uint32_t)ram[addr + 1] << 16) |
           ((uint32_t)ram[addr + 2] << 8) | (uint32_t)ram[addr + 3];
}

static uint16_t read_word_be(const uint8_t *ram, uint32_t addr) {
    return (uint16_t)((ram[addr] << 8) | ram[addr + 1]);
}

/* バスエラーフレームのフォールトアドレス。
 * 68000: 14バイトフレームのアクセスアドレス（SP+2）。
 * 68010: フォーマット$8のフォールトアドレス（SP+$0A）。
 * 68020/68EC030/68030: フォーマット$A/$Bのフォールトアドレス（SP+$10）。 */
static uint32_t frame_fault_addr(const uint8_t *ram, uint32_t sp, mc68k_model_t model) {
    if (model == MC68K_MODEL_68000)
        return read_long_be(ram, sp + 2);
    if (model == MC68K_MODEL_68010)
        return read_long_be(ram, sp + 0x0A);
    return read_long_be(ram, sp + 0x10);
}

/* バスエラーフレームのR/Wビット（1=読出し）。
 * 68000: 状態ワード（SP+0）bit4。68010: SSW（SP+$08）bit8。
 * 68020/68EC030/68030: SSW（SP+$0A）bit6。 */
static bool frame_is_read(const uint8_t *ram, uint32_t sp, mc68k_model_t model) {
    if (model == MC68K_MODEL_68000)
        return (read_word_be(ram, sp) & 0x0010u) != 0;
    if (model == MC68K_MODEL_68010)
        return (read_word_be(ram, sp + 0x08) & 0x0100u) != 0;
    return (read_word_be(ram, sp + 0x0A) & 0x0040u) != 0;
}

/* 2回目のフォルト（SECOND_HOLE への CLR.B 書込み）を起こす命令列を p から置く。
 *   MOVEA.L #SECOND_HOLE,A1 : 0x227C + imm32
 *   MOVE.B D0,(A1)          : 0x1281
 * （CLR は書込み前に read するため、write のフォルトには使えない） */
static void put_second_fault(uint8_t *ram, uint32_t p) {
    write_word_be(ram, p, 0x227C);
    write_long_be(ram, p + 2, SECOND_HOLE);
    write_word_be(ram, p + 6, 0x1281);
}

/* 2回目のフォルトが書込み・SECOND_HOLE として積まれていること。 */
static void assert_second_fault_frame(const test_ctx_t *t, mc68k_state_t *cpu,
                                      mc68k_model_t model) {
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    assert(mc68k_get_exec_state(cpu) != CPU_HALTED);
    assert(r.pc == SECOND_HANDLER);
    assert(mc68k_get_addr_err_addr(cpu) == SECOND_HOLE);
    assert(frame_fault_addr(t->ram, r.a[7], model) == SECOND_HOLE);
    assert(!frame_is_read(t->ram, r.a[7], model));
}

/*
 * 1回目: HOLE_ADDR の読出しでバスエラー。ハンドラはベクタ2を SECOND_HANDLER に
 * 差し替え、フレームを捨てて（A7を戻して）から、別アドレスへの書込みで2回目の
 * バスエラーを起こす。2回目のフレームに1回目のアドレス・R/W が残っていないこと。
 *   MOVEA.L #HOLE_ADDR,A0 : 0x207C + imm32
 *   TST.B (A0)            : 0x4A10
 * ハンドラ（BUS_ERR_HANDLER）:
 *   MOVE.L #SECOND_HANDLER,$8.W : 0x21FC + imm32 + 0x0008
 *   MOVEA.L #RESET_SSP,A7       : 0x2E7C + imm32
 *   （以降 put_second_fault）
 */
static void test_second_fault_after_dispatch_is_fresh(mc68k_model_t model) {
    test_ctx_t t;
    memset(t.ram, 0, sizeof(t.ram));
    setup_common(t.ram);
    write_word_be(t.ram, SECOND_HANDLER, 0x60FE); /* BRA * */

    uint32_t p = RESET_PC;
    write_word_be(t.ram, p, 0x207C);
    write_long_be(t.ram, p + 2, HOLE_ADDR);
    write_word_be(t.ram, p + 6, 0x4A10);

    p = BUS_ERR_HANDLER;
    write_word_be(t.ram, p, 0x21FC);
    write_long_be(t.ram, p + 2, SECOND_HANDLER);
    write_word_be(t.ram, p + 6, 0x0008);
    write_word_be(t.ram, p + 8, 0x2E7C);
    write_long_be(t.ram, p + 10, RESET_SSP);
    put_second_fault(t.ram, p + 14);

    mc68k_state_t *cpu = make_cpu_model(&t, model);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 400);
    assert_second_fault_frame(&t, cpu, model);
    mc68k_destroy(cpu);
}

/*
 * 1回目: A7 を未マップ領域に置いてから HOLE_ADDR を読み、フレーム書込みで
 * 二重フォルト → CPU_HALTED。reset後、2本目のプログラムで別アドレスへの
 * 書込みバスエラーを起こし、そのフレームに1回目の情報が残っていないこと。
 *   MOVEA.L #HOLE_ADDR,A7 : 0x2E7C + imm32
 *   MOVEA.L #HOLE_ADDR,A0 : 0x207C + imm32
 *   TST.B (A0)            : 0x4A10
 */
static void test_first_fault_after_halt_reset_is_fresh(mc68k_model_t model) {
    test_ctx_t t;
    memset(t.ram, 0, sizeof(t.ram));
    setup_common(t.ram);
    write_long_be(t.ram, 0x000008, SECOND_HANDLER); /* 2回目はここに来る */
    write_word_be(t.ram, SECOND_HANDLER, 0x60FE);   /* BRA * */

    uint32_t p = RESET_PC;
    write_word_be(t.ram, p, 0x2E7C);
    write_long_be(t.ram, p + 2, HOLE_ADDR);
    write_word_be(t.ram, p + 6, 0x207C);
    write_long_be(t.ram, p + 8, HOLE_ADDR);
    write_word_be(t.ram, p + 12, 0x4A10);
    put_second_fault(t.ram, PROG2_PC);

    mc68k_state_t *cpu = make_cpu_model(&t, model);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 400);
    assert(mc68k_get_exec_state(cpu) == CPU_HALTED);

    write_long_be(t.ram, 0x000004, PROG2_PC);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 400);
    assert_second_fault_frame(&t, cpu, model);
    mc68k_destroy(cpu);
}

/*
 * 1回目: A7 を未マップ領域に置いてから割込みマスクを下げ、IRQ受付のフレーム
 * 書込みでバスエラーを起こす。配送後の実行状態はモデルの frame 配置に依存する。
 * ここでは reset 後、
 * 2本目のプログラムの書込みバスエラーのフレームに1回目の情報が残っていないこと
 * （reset でのラッチ解除を検証する）。
 *   MOVEA.L #HOLE_ADDR,A7 : 0x2E7C + imm32
 *   MOVE.W #$2000,SR      : 0x46FC 0x2000（S=1のままマスク0。以降の命令境界でIRQ受付）
 *   BRA *                 : 0x60FE
 */
static void test_first_fault_after_irq_halt_reset_is_fresh(mc68k_model_t model) {
    test_ctx_t t;
    memset(t.ram, 0, sizeof(t.ram));
    setup_common(t.ram);
    write_long_be(t.ram, 0x000008, SECOND_HANDLER); /* 2回目はここに来る */
    write_word_be(t.ram, SECOND_HANDLER, 0x60FE);   /* BRA * */
    write_long_be(t.ram, 25u * 4u, SECOND_HANDLER); /* レベル1オートベクタ（到達しない） */

    uint32_t p = RESET_PC;
    write_word_be(t.ram, p, 0x2E7C);
    write_long_be(t.ram, p + 2, HOLE_ADDR);
    write_word_be(t.ram, p + 6, 0x46FC);
    write_word_be(t.ram, p + 8, 0x2000);
    write_word_be(t.ram, p + 10, 0x60FE);
    put_second_fault(t.ram, PROG2_PC);

    mc68k_state_t *cpu = make_cpu_model(&t, model);
    mc68k_reset(cpu);
    mc68k_set_irq(cpu, 1);
    mc68k_execute(cpu, 400);
    /* #67: 68020+ はここで normal frame fault を vector 2 へ配送する。
     * 本試験の目的は直後の reset が fault latch を確実に消すこと。 */

    mc68k_set_irq(cpu, 0);
    write_long_be(t.ram, 0x000004, PROG2_PC);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 400);
    assert_second_fault_frame(&t, cpu, model);
    mc68k_destroy(cpu);
}

int main(void) {
    test_bus_error_on_read();
    test_bus_error_on_write();
    test_address_error_not_confused_with_bus_error();
    for (size_t i = 0; i < sizeof(k_all_models) / sizeof(k_all_models[0]); i++) {
        test_second_fault_after_dispatch_is_fresh(k_all_models[i]);
        test_first_fault_after_halt_reset_is_fresh(k_all_models[i]);
        test_first_fault_after_irq_halt_reset_is_fresh(k_all_models[i]);
    }
    return 0;
}
