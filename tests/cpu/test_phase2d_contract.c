/* Phase 2D 設計書 §6.4.4 のF軸契約を実行する回帰テスト。 */
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

typedef struct {
    uint8_t *ram;
    mem_ctx_t *mem;
    mem_region_t *region;
    mc68k_state_t *cpu;
} fixture_t;

static void ww(uint8_t *ram, uint32_t a, uint16_t v) {
    ram[a] = v >> 8;
    ram[a + 1] = v;
}
static void wl(uint8_t *ram, uint32_t a, uint32_t v) {
    ww(ram, a, v >> 16);
    ww(ram, a + 2, v);
}
static uint32_t rl(const uint8_t *ram, uint32_t a) {
    return ((uint32_t)ram[a] << 24) | ((uint32_t)ram[a + 1] << 16) |
           ((uint32_t)ram[a + 2] << 8) | ram[a + 3];
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

static int expected_cond(unsigned flags, int cond) {
    int n = (flags & 8) != 0, z = (flags & 4) != 0;
    int v = (flags & 2) != 0, c = (flags & 1) != 0;
    switch (cond) {
    case 0:
        return 1;
    case 1:
        return 0;
    case 2:
        return !c && !z;
    case 3:
        return c || z;
    case 4:
        return !c;
    case 5:
        return c;
    case 6:
        return !z;
    case 7:
        return z;
    case 8:
        return !v;
    case 9:
        return v;
    case 10:
        return !n;
    case 11:
        return n;
    case 12:
        return !(n ^ v);
    case 13:
        return n ^ v;
    case 14:
        return !z && !(n ^ v);
    default:
        return z || (n ^ v);
    }
}
static void test_conditions(void) {
    for (unsigned flags = 0; flags < 16; ++flags)
        for (int cond = 0; cond < 16; ++cond)
            assert(eval_cond((uint16_t)flags, cond) == expected_cond(flags, cond));
}
static void test_flow(void) {
    fixture_t f = make_fixture();
    ww(f.ram, RESET_PC, 0x6602);
    f.cpu->sr = 0x0010;
    one(&f);
    assert(f.cpu->pc == 0x1004 && f.cpu->sr == 0x0010);
    dispose(&f);
    f = make_fixture();
    ww(f.ram, RESET_PC, 0x6600);
    ww(f.ram, RESET_PC + 2, 2);
    f.cpu->sr = 0x0014;
    one(&f);
    assert(f.cpu->pc == 0x1004 && f.cpu->sr == 0x0014);
    dispose(&f);
    f = make_fixture();
    ww(f.ram, RESET_PC, 0x6100);
    ww(f.ram, RESET_PC + 2, 4);
    f.cpu->sr = 0x0010;
    one(&f);
    assert(f.cpu->pc == 0x1006 && f.cpu->a[7] == 0x7ffc && rl(f.ram, 0x7ffc) == 0x1004);
    dispose(&f);
    f = make_fixture();
    ww(f.ram, RESET_PC, 0x51c8);
    ww(f.ram, RESET_PC + 2, 0xfffe);
    f.cpu->d[0] = 0xabcd0001;
    f.cpu->sr = 0x0010;
    one(&f);
    assert(f.cpu->pc == RESET_PC && f.cpu->d[0] == 0xabcd0000 && f.cpu->sr == 0x0010);
    dispose(&f);
    f = make_fixture();
    ww(f.ram, RESET_PC, 0x57c8);
    ww(f.ram, RESET_PC + 2, 0);
    f.cpu->d[0] = 0xabcd0001;
    f.cpu->sr = 0x0014;
    one(&f);
    assert(f.cpu->pc == 0x1004 && f.cpu->d[0] == 0xabcd0001 && f.cpu->sr == 0x0014);
    dispose(&f);
    f = make_fixture();
    ww(f.ram, RESET_PC, 0x57c0);
    f.cpu->d[0] = 0x12340000;
    f.cpu->sr = 0x0014;
    one(&f);
    assert(f.cpu->d[0] == 0x123400ff && f.cpu->sr == 0x0014);
    dispose(&f);
    f = make_fixture();
    ww(f.ram, RESET_PC, 0x4ed0);
    f.cpu->a[0] = 0x2346; /* 偶数。奇数アドレスへの分岐は次命令の取得で address error になる */
    f.cpu->sr = 0x0010;
    one(&f);
    assert(f.cpu->pc == 0x2346 && f.cpu->sr == 0x0010);
    dispose(&f);
    f = make_fixture();
    ww(f.ram, RESET_PC, 0x4e90);
    ww(f.ram, 0x2000, 0x4e75);
    f.cpu->a[0] = 0x2000;
    f.cpu->sr = 0x0010;
    one(&f);
    one(&f);
    assert(f.cpu->pc == 0x1002 && f.cpu->a[7] == RESET_SSP && f.cpu->sr == 0x0010);
    dispose(&f);
}
static void shift_case(uint16_t op, uint32_t in, uint16_t sr, uint32_t out, uint16_t expected) {
    fixture_t f = make_fixture();
    ww(f.ram, RESET_PC, op);
    f.cpu->d[0] = in;
    f.cpu->sr = sr;
    one(&f);
    assert(f.cpu->d[0] == out && (f.cpu->sr & 0x1f) == expected);
    dispose(&f);
}
static void test_shifts(void) {
    shift_case(0xe380, 0x40000000, 0, 0x80000000, 0x000a);      /* ASL.L #1 */
    shift_case(0xe280, 0x80000001, 0, 0xc0000000, 0x0019);      /* ASR.L #1 */
    shift_case(0xe308, 0x12340080, 0, 0x12340000, 0x0015);      /* LSL.B #1 */
    shift_case(0xe248, 0x12340001, 0, 0x12340000, 0x0015);      /* LSR.W #1 */
    shift_case(0xe310, 0x12340080, 0, 0x12340000, 0x0015);      /* ROXL.B #1 */
    shift_case(0xe210, 0x12340001, 0, 0x12340000, 0x0015);      /* ROXR.B #1 */
    shift_case(0xe358, 0x12348000, 0x0010, 0x12340001, 0x0011); /* ROL.W #1 */
    shift_case(0xe258, 0x12340001, 0, 0x12348000, 0x0009);      /* ROR.W #1 */
    fixture_t f = make_fixture();
    ww(f.ram, RESET_PC, 0xe3a0);
    f.cpu->d[0] = 0x40000000;
    f.cpu->d[1] = 31;
    one(&f);
    assert(f.cpu->d[0] == 0 && (f.cpu->sr & 0x1f) == 0x0006);
    dispose(&f); /* ASL.L D1,D0 */
    f = make_fixture();
    ww(f.ram, RESET_PC, 0xe1d0);
    f.cpu->a[0] = 0x200;
    ww(f.ram, 0x200, 0x4000);
    one(&f);
    assert(((uint16_t)f.ram[0x200] << 8 | f.ram[0x201]) == 0x8000 &&
           (f.cpu->sr & 0x1f) == 0x000a);
    dispose(&f);
}
int main(void) {
    test_conditions();
    test_flow();
    test_shifts();
    return 0;
}
