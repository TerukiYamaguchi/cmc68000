/* PC履歴リングの実行時トグルの回帰テスト。内部状態には触れず、
 * 公開ダンプAPIとログハンドラ経由で記録内容を検証する。 */
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
#define NOP_OP    0x4E71u

typedef struct {
    uint8_t ram[RAM_SIZE];
} test_ctx_t;

typedef struct {
    char text[8192];
    size_t len;
} log_capture_t;

static bool rd_b(void *ctx, uint32_t addr, uint8_t *out) {
    test_ctx_t *t = (test_ctx_t *)ctx;
    *out = (addr < RAM_SIZE) ? t->ram[addr] : 0xFF;
    return addr < RAM_SIZE;
}

static bool rd_w(void *ctx, uint32_t addr, uint16_t *out) {
    uint8_t hi = 0, lo = 0;
    bool ok1 = rd_b(ctx, addr, &hi);
    bool ok2 = rd_b(ctx, addr + 1, &lo);
    *out = (uint16_t)((hi << 8) | lo);
    return ok1 && ok2;
}

static bool rd_l(void *ctx, uint32_t addr, uint32_t *out) {
    uint16_t hi = 0, lo = 0;
    bool ok1 = rd_w(ctx, addr, &hi);
    bool ok2 = rd_w(ctx, addr + 2, &lo);
    *out = ((uint32_t)hi << 16) | lo;
    return ok1 && ok2;
}

static bool wr_b(void *ctx, uint32_t addr, uint8_t value) {
    test_ctx_t *t = (test_ctx_t *)ctx;
    if (addr < RAM_SIZE)
        t->ram[addr] = value;
    return addr < RAM_SIZE;
}

static bool wr_w(void *ctx, uint32_t addr, uint16_t value) {
    return wr_b(ctx, addr, (uint8_t)(value >> 8)) && wr_b(ctx, addr + 1, (uint8_t)value);
}

static bool wr_l(void *ctx, uint32_t addr, uint32_t value) {
    return wr_w(ctx, addr, (uint16_t)(value >> 16)) && wr_w(ctx, addr + 2, (uint16_t)value);
}

static void sync_cb(void *ctx, int cycles) {
    (void)ctx;
    (void)cycles;
}

static void write_long_be(uint8_t *ram, uint32_t addr, uint32_t value) {
    ram[addr] = (uint8_t)(value >> 24);
    ram[addr + 1] = (uint8_t)(value >> 16);
    ram[addr + 2] = (uint8_t)(value >> 8);
    ram[addr + 3] = (uint8_t)value;
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

static void init_reset_vectors(test_ctx_t *t, uint32_t pc) {
    write_long_be(t->ram, 0, RESET_SSP);
    write_long_be(t->ram, 4, pc);
}

static void test_toggle_and_reset(void) {
    test_ctx_t t = {0};
    log_capture_t log = {0};
    init_reset_vectors(&t, RESET_PC);
    wr_w(&t, RESET_PC, NOP_OP);
    wr_w(&t, RESET_PC + 2, NOP_OP);
    wr_w(&t, RESET_PC + 4, NOP_OP);

    mc68k_state_t *cpu = make_cpu(&t);
    assert(cpu);
    mc68k_reset(cpu);
    mc68k_set_log_handler(cpu, capture_log, &log);

    assert(mc68k_get_pc_ring(cpu) == 0);
    mc68k_set_pc_ring(NULL, 1);
    assert(mc68k_get_pc_ring(NULL) == 0);
    mc68k_set_pc_ring(cpu, -1);
    assert(mc68k_get_pc_ring(cpu) == 1);
    mc68k_execute(cpu, 1);
    mc68k_set_pc_ring(cpu, 0);
    mc68k_execute(cpu, 1);
    mc68k_set_pc_ring(cpu, 1);
    mc68k_execute(cpu, 1);
    mc68k_dump_pc_ring(cpu, 2);
    assert(strstr(log.text, " 001000 001004") != NULL);

    clear_log(&log);
    init_reset_vectors(&t, 0x00002000u);
    wr_w(&t, 0x00002000u, NOP_OP);
    mc68k_reset(cpu);
    assert(mc68k_get_pc_ring(cpu) == 1);
    mc68k_execute(cpu, 1);
    mc68k_dump_pc_ring(cpu, 3);
    assert(strstr(log.text, " 001000 001004 002000") != NULL);
    mc68k_destroy(cpu);
}

static void test_default_off_does_not_record(void) {
    test_ctx_t t = {0};
    log_capture_t log = {0};
    init_reset_vectors(&t, RESET_PC);
    wr_w(&t, RESET_PC, NOP_OP);
    mc68k_state_t *cpu = make_cpu(&t);
    assert(cpu);
    mc68k_reset(cpu);
    mc68k_set_log_handler(cpu, capture_log, &log);
    mc68k_execute(cpu, 1);
    mc68k_dump_pc_ring(cpu, 1);
    assert(strstr(log.text, "last 0 instrs") != NULL);
    assert(strstr(log.text, " 000000") == NULL);
    assert(mc68k_read_pc_ring(cpu, NULL, 0) == 0);
    mc68k_destroy(cpu);
}

static void test_diagnostic_pc_dump_respects_toggle(void) {
    test_ctx_t t = {0};
    log_capture_t log = {0};
    init_reset_vectors(&t, RESET_PC);
    wr_w(&t, RESET_PC, 0x0000); /* ORI.B #0,D0: BADORI対象 */
    wr_w(&t, RESET_PC + 2, 0x0000);
    mc68k_state_t *cpu = make_cpu(&t);
    assert(cpu);
    mc68k_reset(cpu);
    mc68k_set_log_handler(cpu, capture_log, &log);
    mc68k_set_badori_log(cpu, 1);
    mc68k_execute(cpu, 1);
    assert(strstr(log.text, "BADORI pc=001000") != NULL);
    assert(strstr(log.text, "=== PC trace") == NULL);

    clear_log(&log);
    mc68k_reset(cpu); /* badori_dumpedを再許可する */
    mc68k_set_pc_ring(cpu, 1);
    mc68k_execute(cpu, 1);
    assert(strstr(log.text, "BADORI pc=001000") != NULL);
    assert(strstr(log.text, "=== PC trace") != NULL);

    /* JMP $01000000.L はBADJUMP対象。BADORIと同じく、本体ログは常に出し、
     * PC履歴の自動ダンプだけを記録状態で制御する。 */
    write_long_be(t.ram, RESET_PC, 0x4EF90100u);
    wr_w(&t, RESET_PC + 4, 0x0000);
    mc68k_set_badjump_log(cpu, 1);
    mc68k_set_pc_ring(cpu, 0);
    clear_log(&log);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 1);
    assert(strstr(log.text, "BADJUMP from=001000 to=01000000 opcode=4EF9") != NULL);
    assert(strstr(log.text, "=== PC trace") == NULL);

    clear_log(&log);
    mc68k_reset(cpu); /* badjump_dumpedを再許可する */
    mc68k_set_pc_ring(cpu, 1);
    mc68k_execute(cpu, 1);
    assert(strstr(log.text, "BADJUMP from=001000 to=01000000 opcode=4EF9") != NULL);
    assert(strstr(log.text, "=== PC trace") != NULL);
    mc68k_destroy(cpu);
}

int main(void) {
    test_toggle_and_reset();
    test_default_off_does_not_record();
    test_diagnostic_pc_dump_respects_toggle();
    puts("test_pc_ring: OK");
    return 0;
}
