/* BADADDR診断ログの実行時トグルの回帰テスト。
 * 内部状態には触れず、公開APIとログハンドラだけで挙動を検証する。 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mc68000/bus_hooks.h"
#include "mc68000/cpu.h"
#include "mc68000/debug.h"

#define RAM_SIZE  0x10000u
#define RESET_SSP 0x00008000u
#define RESET_PC  0x00001000u

typedef struct {
    uint8_t ram[RAM_SIZE];
} test_ctx_t;

typedef struct {
    char text[1024];
    size_t len;
} log_capture_t;

/* 32-bitアドレスの低位16bitをテストRAMへ写す。高位バイトが不正なEAでも
 * バスエラーを発生させず、BADADDR診断だけを観測できるようにする。 */
static uint32_t ram_addr(uint32_t addr) {
    return addr & (RAM_SIZE - 1);
}

static bool rd_b(void *ctx, uint32_t addr, uint8_t *out) {
    test_ctx_t *t = (test_ctx_t *)ctx;
    *out = t->ram[ram_addr(addr)];
    return true;
}

static bool rd_w(void *ctx, uint32_t addr, uint16_t *out) {
    uint8_t hi = 0, lo = 0;
    rd_b(ctx, addr, &hi);
    rd_b(ctx, addr + 1, &lo);
    *out = (uint16_t)((hi << 8) | lo);
    return true;
}

static bool rd_l(void *ctx, uint32_t addr, uint32_t *out) {
    uint16_t hi = 0, lo = 0;
    rd_w(ctx, addr, &hi);
    rd_w(ctx, addr + 2, &lo);
    *out = ((uint32_t)hi << 16) | lo;
    return true;
}

static bool wr_b(void *ctx, uint32_t addr, uint8_t value) {
    test_ctx_t *t = (test_ctx_t *)ctx;
    t->ram[ram_addr(addr)] = value;
    return true;
}

static bool wr_w(void *ctx, uint32_t addr, uint16_t value) {
    wr_b(ctx, addr, (uint8_t)(value >> 8));
    wr_b(ctx, addr + 1, (uint8_t)value);
    return true;
}

static bool wr_l(void *ctx, uint32_t addr, uint32_t value) {
    wr_w(ctx, addr, (uint16_t)(value >> 16));
    wr_w(ctx, addr + 2, (uint16_t)value);
    return true;
}

static void sync_cb(void *ctx, int cycles) {
    (void)ctx;
    (void)cycles;
}

static void write_word_be(uint8_t *ram, uint32_t addr, uint16_t value) {
    ram[addr] = (uint8_t)(value >> 8);
    ram[addr + 1] = (uint8_t)value;
}

static void write_long_be(uint8_t *ram, uint32_t addr, uint32_t value) {
    write_word_be(ram, addr, (uint16_t)(value >> 16));
    write_word_be(ram, addr + 2, (uint16_t)value);
}

static mc68k_state_t *make_cpu(test_ctx_t *t) {
    mc68k_bus_t bus = {
        .read_byte = rd_b,
        .read_word = rd_w,
        .read_long = rd_l,
        .write_byte = wr_b,
        .write_word = wr_w,
        .write_long = wr_l,
        .sync = sync_cb,
        .ctx = t,
    };
    return mc68k_create(&bus);
}

static void capture_log(void *ctx, const char *msg) {
    log_capture_t *capture = (log_capture_t *)ctx;
    size_t remaining = sizeof(capture->text) - capture->len - 1;
    size_t len = strlen(msg);
    if (len > remaining)
        len = remaining;
    memcpy(capture->text + capture->len, msg, len);
    capture->len += len;
    capture->text[capture->len] = '\0';
}

static void clear_log(log_capture_t *capture) {
    capture->text[0] = '\0';
    capture->len = 0;
}

static void setup_absolute_long(test_ctx_t *t, uint16_t opcode, uint32_t addr) {
    write_long_be(t->ram, 0, RESET_SSP);
    write_long_be(t->ram, 4, RESET_PC);
    write_word_be(t->ram, RESET_PC, opcode);
    write_long_be(t->ram, RESET_PC + 2, addr);
}

static void execute_absolute_long(mc68k_state_t *cpu, test_ctx_t *t, uint16_t opcode,
                                  uint32_t addr) {
    setup_absolute_long(t, opcode, addr);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 1);
}

static void test_api_contract(void) {
    test_ctx_t t = {0};
    mc68k_state_t *cpu = make_cpu(&t);
    assert(cpu);

    assert(mc68k_get_badaddr_log(cpu) == 0);
    mc68k_set_badaddr_log(NULL, 1);
    assert(mc68k_get_badaddr_log(NULL) == 0);
    mc68k_set_badaddr_log(cpu, -1);
    assert(mc68k_get_badaddr_log(cpu) == 1);
    setup_absolute_long(&t, 0x3039, 0x01000000u);
    mc68k_reset(cpu);
    assert(mc68k_get_badaddr_log(cpu) == 1);
    mc68k_set_badaddr_log(cpu, 0);
    assert(mc68k_get_badaddr_log(cpu) == 0);
    mc68k_destroy(cpu);
}

static void test_toggle_and_all_word_long_kinds(void) {
    static const struct {
        uint16_t opcode;
        const char *line;
    } cases[] = {
        {0x3039, "BADADDR(rw) addr=01000000 pc=001006\n"},
        {0x2039, "BADADDR(rl) addr=01000000 pc=001006\n"},
        {0x33C0, "BADADDR(ww) addr=01000000 pc=001006\n"},
        {0x23C0, "BADADDR(wl) addr=01000000 pc=001006\n"},
    };
    test_ctx_t t = {0};
    log_capture_t log = {0};
    mc68k_state_t *cpu = make_cpu(&t);
    assert(cpu);
    mc68k_set_log_handler(cpu, capture_log, &log);

    /* 既定OFFと、OFF→ON→OFFの切り替えを最初のreadで確認する。 */
    execute_absolute_long(cpu, &t, cases[0].opcode, 0x01000000u);
    assert(log.len == 0);
    mc68k_set_badaddr_log(cpu, 1);
    execute_absolute_long(cpu, &t, cases[0].opcode, 0x01000000u);
    assert(strcmp(log.text, cases[0].line) == 0);
    clear_log(&log);
    mc68k_set_badaddr_log(cpu, 0);
    execute_absolute_long(cpu, &t, cases[0].opcode, 0x01000000u);
    assert(log.len == 0);

    mc68k_set_badaddr_log(cpu, 1);
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        clear_log(&log);
        execute_absolute_long(cpu, &t, cases[i].opcode, 0x01000000u);
        assert(strcmp(log.text, cases[i].line) == 0);
    }
    mc68k_destroy(cpu);
}

static void test_exclusions_and_repeated_logging(void) {
    test_ctx_t t = {0};
    log_capture_t log = {0};
    mc68k_state_t *cpu = make_cpu(&t);
    assert(cpu);
    mc68k_set_log_handler(cpu, capture_log, &log);
    mc68k_set_badaddr_log(cpu, 1);

    execute_absolute_long(cpu, &t, 0x3039, 0x00002000u);
    assert(log.len == 0);
    execute_absolute_long(cpu, &t, 0x3039, 0xFF002000u);
    assert(log.len == 0);
    execute_absolute_long(cpu, &t, 0x1039, 0x01000000u);
    assert(log.len == 0);
    execute_absolute_long(cpu, &t, 0x13C0, 0x01000000u);
    assert(log.len == 0);

    execute_absolute_long(cpu, &t, 0x3039, 0x01000000u);
    execute_absolute_long(cpu, &t, 0x3039, 0x01000000u);
    assert(strcmp(log.text, "BADADDR(rw) addr=01000000 pc=001006\n"
                            "BADADDR(rw) addr=01000000 pc=001006\n") == 0);
    mc68k_destroy(cpu);
}

int main(void) {
    test_api_contract();
    test_toggle_and_all_word_long_kinds();
    test_exclusions_and_repeated_logging();
    puts("test_badaddr_log: OK");
    return 0;
}
