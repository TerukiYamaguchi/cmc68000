/* Phase 2E 設計書 §6.5.5 のF軸デコード契約を実行する回帰テスト。 */
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include "mc68000/cpu.h"
#include "cpu/cpu_internal.h"
#include "memory/memory.h"
#include "memory/bus.h"

#define RAM_SIZE  0x10000u
#define RESET_PC  0x1000u
#define RESET_SSP 0x8000u
#define HANDLER   0x1800u

typedef struct {
    uint8_t *ram;
    mem_ctx_t *mem;
    mem_region_t *region;
    mc68k_state_t *cpu;
} fixture_t;
static int reset_calls;
static void peripheral_reset(void *ctx) {
    (void)ctx;
    ++reset_calls;
}
static void ww(uint8_t *r, uint32_t a, uint16_t v) {
    r[a] = v >> 8;
    r[a + 1] = v;
}
static void wl(uint8_t *r, uint32_t a, uint32_t v) {
    ww(r, a, v >> 16);
    ww(r, a + 2, v);
}
static fixture_t make_fixture(void) {
    fixture_t f = {0};
    f.ram = calloc(1, RAM_SIZE);
    assert(f.ram);
    wl(f.ram, 0, RESET_SSP);
    wl(f.ram, 4, RESET_PC);
    f.mem = mem_create();
    f.region = calloc(1, sizeof(*f.region));
    assert(f.mem && f.region);
    f.region->data = f.ram;
    f.region->size = RAM_SIZE;
    mem_add_region(f.mem, f.region);
    mc68k_bus_t bus = bus_make_hooks(f.mem);
    bus.reset_peripheral = peripheral_reset;
    f.cpu = mc68k_create(&bus);
    mc68k_reset(f.cpu);
    return f;
}
static void dispose(fixture_t *f) {
    mc68k_destroy(f->cpu);
    mem_destroy(f->mem);
    free(f->region);
    free(f->ram);
}
static void one(fixture_t *f) {
    mc68k_execute(f->cpu, 1);
}
static void vector(fixture_t *f, unsigned n) {
    wl(f->ram, n * 4u, HANDLER);
}

static void test_status_transfers(void) {
    fixture_t f = make_fixture();
    ww(f.ram, RESET_PC, 0x40c0);
    f.cpu->sr = 0x0015;
    one(&f);
    assert(f.cpu->d[0] == 0x15 && f.cpu->sr == 0x0015);
    dispose(&f);
    f = make_fixture();
    ww(f.ram, RESET_PC, 0x44c0);
    f.cpu->d[0] = 0x001f;
    f.cpu->sr = 0x2700;
    one(&f);
    assert(f.cpu->sr == 0x271f);
    dispose(&f);
    f = make_fixture();
    ww(f.ram, RESET_PC, 0x46c0);
    f.cpu->d[0] = 0x2001;
    f.cpu->sr = 0x2700;
    one(&f);
    assert(f.cpu->sr == 0x2001);
    dispose(&f);
    f = make_fixture();
    ww(f.ram, RESET_PC, 0x4e60);
    f.cpu->a[0] = 0x12345678;
    f.cpu->sr = 0x2700;
    one(&f);
    assert(f.cpu->usp == 0x12345678 && f.cpu->sr == 0x2700);
    dispose(&f);
    f = make_fixture();
    ww(f.ram, RESET_PC, 0x4e68);
    f.cpu->usp = 0x87654321;
    f.cpu->sr = 0x2700;
    one(&f);
    assert(f.cpu->a[0] == 0x87654321 && f.cpu->sr == 0x2700);
    dispose(&f);
    f = make_fixture();
    ww(f.ram, RESET_PC, 0x003c);
    ww(f.ram, RESET_PC + 2, 0x0011);
    f.cpu->sr = 0x2700;
    one(&f);
    assert(f.cpu->sr == 0x2711);
    dispose(&f);
    f = make_fixture();
    ww(f.ram, RESET_PC, 0x007c);
    ww(f.ram, RESET_PC + 2, 0x0011);
    f.cpu->sr = 0x2000;
    one(&f);
    assert(f.cpu->sr == 0x2011);
    dispose(&f);
    f = make_fixture();
    ww(f.ram, RESET_PC, 0x023c);
    ww(f.ram, RESET_PC + 2, 0x000e);
    f.cpu->sr = 0x271f;
    one(&f);
    assert(f.cpu->sr == 0x270e);
    dispose(&f);
    f = make_fixture();
    ww(f.ram, RESET_PC, 0x027c);
    ww(f.ram, RESET_PC + 2, 0x20ff);
    f.cpu->sr = 0x271f;
    one(&f);
    assert(f.cpu->sr == 0x201f);
    dispose(&f);
    f = make_fixture();
    ww(f.ram, RESET_PC, 0x0a3c);
    ww(f.ram, RESET_PC + 2, 0x0011);
    f.cpu->sr = 0x271f;
    one(&f);
    assert(f.cpu->sr == 0x270e);
    dispose(&f);
    f = make_fixture();
    ww(f.ram, RESET_PC, 0x0a7c);
    ww(f.ram, RESET_PC + 2, 0x0011);
    f.cpu->sr = 0x271f;
    one(&f);
    assert(f.cpu->sr == 0x270e);
    dispose(&f);
}
static void test_check_and_exceptions(void) {
    fixture_t f = make_fixture();
    ww(f.ram, RESET_PC, 0x41bc);
    ww(f.ram, RESET_PC + 2, 10);
    f.cpu->d[0] = 10;
    one(&f);
    assert(f.cpu->pc == 0x1004 && f.cpu->last_exception_vector == 0);
    dispose(&f);
    f = make_fixture();
    vector(&f, 6);
    ww(f.ram, RESET_PC, 0x41bc);
    ww(f.ram, RESET_PC + 2, 10);
    f.cpu->d[0] = 11;
    one(&f);
    assert(f.cpu->pc == HANDLER && f.cpu->last_exception_vector == 6);
    dispose(&f);
    f = make_fixture();
    vector(&f, 4);
    ww(f.ram, RESET_PC, 0x4afc);
    one(&f);
    assert(f.cpu->pc == HANDLER && f.cpu->last_exception_vector == 4);
    dispose(&f);
    f = make_fixture();
    vector(&f, 35);
    ww(f.ram, RESET_PC, 0x4e43);
    one(&f);
    assert(f.cpu->pc == HANDLER && f.cpu->last_exception_vector == 35);
    dispose(&f);
    f = make_fixture();
    vector(&f, 7);
    ww(f.ram, RESET_PC, 0x4e76);
    f.cpu->sr = 0x2702;
    one(&f);
    assert(f.cpu->pc == HANDLER && f.cpu->last_exception_vector == 7);
    dispose(&f);
    f = make_fixture();
    ww(f.ram, RESET_PC, 0x4e76);
    f.cpu->sr = 0x2700;
    one(&f);
    assert(f.cpu->pc == 0x1002 && f.cpu->last_exception_vector == 0 && f.cpu->sr == 0x2700);
    dispose(&f);
    f = make_fixture();
    vector(&f, 8);
    ww(f.ram, RESET_PC, 0x46fc);
    ww(f.ram, RESET_PC + 2, 0);
    f.cpu->sr = 0;
    one(&f);
    assert(f.cpu->pc == HANDLER && f.cpu->last_exception_vector == 8);
    dispose(&f);
}
static void test_system_and_tas(void) {
    fixture_t f = make_fixture();
    ww(f.ram, RESET_PC, 0x4e71);
    f.cpu->d[0] = 0x1234;
    one(&f);
    assert(f.cpu->pc == 0x1002 && f.cpu->d[0] == 0x1234);
    dispose(&f);
    f = make_fixture();
    reset_calls = 0;
    ww(f.ram, RESET_PC, 0x4e70);
    one(&f);
    assert(reset_calls == 1 && f.cpu->pc == 0x1002);
    dispose(&f);
    f = make_fixture();
    ww(f.ram, RESET_PC, 0x4e72);
    ww(f.ram, RESET_PC + 2, 0x2700);
    one(&f);
    assert(f.cpu->exec_state == CPU_STOPPED && f.cpu->pc == 0x1004 && f.cpu->sr == 0x2700);
    dispose(&f);
    f = make_fixture();
    ww(f.ram, RESET_PC, 0x4e77);
    ww(f.ram, RESET_SSP, 0x0015);
    wl(f.ram, RESET_SSP + 2, 0x1234);
    one(&f);
    assert(f.cpu->pc == 0x1234 && f.cpu->a[7] == RESET_SSP + 6 && f.cpu->sr == 0x2715);
    dispose(&f);
    f = make_fixture();
    ww(f.ram, RESET_PC, 0x4e73);
    ww(f.ram, RESET_SSP, 0x2700);
    wl(f.ram, RESET_SSP + 2, 0x1234);
    one(&f);
    assert(f.cpu->pc == 0x1234 && f.cpu->a[7] == RESET_SSP + 6 && f.cpu->sr == 0x2700);
    dispose(&f);
    f = make_fixture();
    ww(f.ram, RESET_PC, 0x4ac0);
    f.cpu->d[0] = 0x12340000;
    f.cpu->sr = 0x2710;
    one(&f);
    assert(f.cpu->d[0] == 0x12340080 && (f.cpu->sr & 0x1f) == 0x0014);
    dispose(&f);
}
int main(void) {
    test_status_transfers();
    test_check_and_exceptions();
    test_system_and_tas();
    return 0;
}
