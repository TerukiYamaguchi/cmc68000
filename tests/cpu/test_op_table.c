/*
 * test_op_table.c — 実行用 opcode テーブルの構造・書換え回帰テスト
 *
 * opcode テーブルは実行ハンドラだけを保持する。ここでは生成済みテーブルの
 * 代表的な同一性と、インスタンスごとの forced-illegal 書換えを検証する。
 */
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "cpu/cpu_internal.h"
#include "mc68000/cpu.h"
#include "memory/bus.h"
#include "memory/memory.h"

#define RAM_SIZE   0x10000
#define RESET_SSP  0x00008000
#define RESET_PC   0x00001000
#define VECTOR4_PC 0x00001500

static void write_word_be(uint8_t *ram, uint32_t addr, uint16_t value) {
    ram[addr] = (uint8_t)(value >> 8);
    ram[addr + 1] = (uint8_t)value;
}

static void write_long_be(uint8_t *ram, uint32_t addr, uint32_t value) {
    ram[addr] = (uint8_t)(value >> 24);
    ram[addr + 1] = (uint8_t)(value >> 16);
    ram[addr + 2] = (uint8_t)(value >> 8);
    ram[addr + 3] = (uint8_t)value;
}

static uint32_t read_long_be(const uint8_t *ram, uint32_t addr) {
    return ((uint32_t)ram[addr] << 24) | ((uint32_t)ram[addr + 1] << 16) |
           ((uint32_t)ram[addr + 2] << 8) | ram[addr + 3];
}

static mc68k_state_t *make_cpu(uint8_t *ram) {
    mem_ctx_t *mem = mem_create();
    mem_region_t *region = calloc(1, sizeof(*region));
    assert(region);
    region->data = ram;
    region->base = 0;
    region->size = RAM_SIZE;
    mem_add_region(mem, region);
    mc68k_bus_t bus = bus_make_hooks(mem);
    mc68k_state_t *cpu = mc68k_create(&bus);
    assert(cpu);
    return cpu;
}

static uint8_t peek_byte(void *ctx, uint32_t addr) {
    return ((const uint8_t *)ctx)[addr & 0xFFFF];
}

static uint16_t peek_word(void *ctx, uint32_t addr) {
    const uint8_t *ram = ctx;
    return (uint16_t)((ram[addr & 0xFFFF] << 8) | ram[(addr + 1) & 0xFFFF]);
}

static uint32_t peek_long(void *ctx, uint32_t addr) {
    return ((uint32_t)peek_word(ctx, addr) << 16) | peek_word(ctx, addr + 2);
}

/* 関数アドレスそのものは ASLR により安定しないため、初出順のグループ ID に正規化する。 */
static uint64_t handler_fingerprint(const mc68k_state_t *cpu, size_t *unique_count) {
    op_handler_t *seen = calloc(65536, sizeof(*seen));
    assert(seen);
    size_t seen_count = 0;
    uint64_t hash = UINT64_C(1469598103934665603); /* FNV-1a 64-bit offset basis */

    for (unsigned opcode = 0; opcode < 65536; opcode++) {
        size_t group = 0;
        while (group < seen_count && seen[group] != cpu->op_table[opcode])
            group++;
        if (group == seen_count)
            seen[seen_count++] = cpu->op_table[opcode];

        uint16_t id = (uint16_t)group;
        hash ^= (uint8_t)(id >> 8);
        hash *= UINT64_C(1099511628211);
        hash ^= (uint8_t)id;
        hash *= UINT64_C(1099511628211);
    }
    free(seen);
    *unique_count = seen_count;
    return hash;
}

static void test_table_handler_layout(void) {
    uint8_t ram[RAM_SIZE] = {0};
    mc68k_state_t *cpu = make_cpu(ram);

    /* 型がハンドラだけであることを確認する。64bit 環境では 65536 エントリで 512 KiB。 */
    assert(sizeof(*cpu->op_table) == sizeof(op_handler_t));
#if UINTPTR_MAX == UINT64_MAX
    assert(sizeof(*cpu->op_table) == 8);
#endif

    /* 不正 opcode は有効な NOP と異なるハンドラになる。 */
    assert(cpu->op_table[0x4AFC] != cpu->op_table[0x4E71]);
    assert(cpu->op_table[0x6000] == cpu->op_table[0x60FF]);
    assert(cpu->op_table[0xA000] == cpu->op_table[0xAFFF]);
    assert(cpu->op_table[0xF000] == cpu->op_table[0xFFFF]);
    assert(cpu->op_table[0xA000] != cpu->op_table[0xF000]);

    /* 65536 opcode のハンドラ配置を回帰検出する。定数は本変更後の基準値。
     * MOVE from CCR (0x42C0) は MC68010 以降の命令のため ops_system_ext.c へ
     * 移設された。既定（68000）モデルでは専用ハンドラを持たず ILLEGAL 群に
     * 統合されるため、ユニークハンドラ数が 72 → 71 に減っている。
     * ADDA/SUBA/CMPA の予約 EA（mode=111/reg=101–111）の144 opcode は
     * ILLEGAL 群に移したため、配置の fingerprint を更新した（Issue #34）。 */
    size_t unique_count;
    assert(handler_fingerprint(cpu, &unique_count) == UINT64_C(0xDF8D64C495C72269));
    assert(unique_count == 71);

    mc68k_destroy(cpu);
}

static void test_line_f_dispatch(void) {
    static const uint16_t opcodes[] = {0xF000, 0xFFFF};

    for (size_t i = 0; i < sizeof(opcodes) / sizeof(opcodes[0]); i++) {
        uint8_t ram[RAM_SIZE] = {0};
        write_long_be(ram, 0, RESET_SSP);
        write_long_be(ram, 4, RESET_PC);
        write_long_be(ram, 11 * 4, VECTOR4_PC);
        write_word_be(ram, RESET_PC, opcodes[i]);
        write_word_be(ram, VECTOR4_PC, 0x60FE); /* BRA * */

        mc68k_state_t *cpu = make_cpu(ram);
        mc68k_reset(cpu);
        mc68k_execute(cpu, 34);
        mc68k_regs_t regs;
        mc68k_get_regs(cpu, &regs);
        assert(regs.pc == VECTOR4_PC);
        assert(read_long_be(ram, regs.a[7] + 2) == RESET_PC);
        mc68k_destroy(cpu);
    }
}

static void test_disassembly_is_independent_of_forced_illegal(void) {
    uint8_t ram[RAM_SIZE] = {0};
    char before[32], after[32];
    write_word_be(ram, RESET_PC, 0x4E71); /* NOP */

    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_set_peek_hooks(cpu, peek_byte, peek_word, peek_long, ram);
    int before_len = mc68k_disassemble(cpu, RESET_PC, before, sizeof(before));
    mc68k_set_opcode_illegal(cpu, 0x4E71);
    int after_len = mc68k_disassemble(cpu, RESET_PC, after, sizeof(after));

    assert(before_len == 2);
    assert(after_len == before_len);
    assert(strcmp(before, "NOP") == 0);
    assert(strcmp(after, before) == 0);
    mc68k_destroy(cpu);
}

static void test_forced_illegal_is_instance_local_and_persistent(void) {
    uint8_t ram1[RAM_SIZE] = {0};
    uint8_t ram2[RAM_SIZE] = {0};
    write_long_be(ram1, 0, RESET_SSP);
    write_long_be(ram1, 4, RESET_PC);
    write_long_be(ram1, 4 * 4, VECTOR4_PC);
    write_word_be(ram1, RESET_PC, 0x4E71);   /* NOP */
    write_word_be(ram1, VECTOR4_PC, 0x60FE); /* BRA * */

    mc68k_state_t *cpu1 = make_cpu(ram1);
    mc68k_state_t *cpu2 = make_cpu(ram2);
    op_handler_t original = cpu1->op_table[0x4E71];

    mc68k_set_opcode_illegal(cpu1, 0x4E71);
    assert(cpu1->op_table[0x4E71] != original);
    assert(cpu2->op_table[0x4E71] == original);

    /* reset は実行時の opcode 上書きを再構築せず、インスタンス内に保持する。 */
    mc68k_reset(cpu1);
    assert(cpu1->op_table[0x4E71] != original);
    mc68k_execute(cpu1, 34);
    mc68k_regs_t regs;
    mc68k_get_regs(cpu1, &regs);
    assert(regs.pc == VECTOR4_PC);

    mc68k_destroy(cpu2);
    mc68k_destroy(cpu1);
}

int main(void) {
    test_table_handler_layout();
    test_forced_illegal_is_instance_local_and_persistent();
    test_line_f_dispatch();
    test_disassembly_is_independent_of_forced_illegal();
    return 0;
}
