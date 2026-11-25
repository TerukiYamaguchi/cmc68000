#include "mc68000/cpu.h"
#include "cpu/cpu_internal.h"
#include <assert.h>
#include <string.h>

typedef struct {
    uint8_t ram[0x1000];
    unsigned cpu_space_writes, cpu_space_reads, data_reads;
    uint32_t cpu_space_value;
} fixture_t;
#define DATA_AREA 0x400u
static void count_data_read(fixture_t *f, uint32_t a) {
    if (a >= DATA_AREA)
        f->data_reads++;
}

static bool rb(void *ctx, uint32_t a, uint8_t *v) {
    count_data_read(ctx, a);
    *v = ((fixture_t *)ctx)->ram[a];
    return true;
}
static bool rw(void *ctx, uint32_t a, uint16_t *v) {
    fixture_t *f = ctx;
    count_data_read(f, a);
    *v = ((uint16_t)f->ram[a] << 8) | f->ram[a + 1];
    return true;
}
static bool rl(void *ctx, uint32_t a, uint32_t *v) {
    fixture_t *f = ctx;
    count_data_read(f, a);
    *v = ((uint32_t)f->ram[a] << 24) | ((uint32_t)f->ram[a + 1] << 16) |
         ((uint32_t)f->ram[a + 2] << 8) | f->ram[a + 3];
    return true;
}
static bool wb(void *ctx, uint32_t a, uint8_t v) {
    ((fixture_t *)ctx)->ram[a] = v;
    return true;
}
static bool ww(void *ctx, uint32_t a, uint16_t v) {
    fixture_t *f = ctx;
    f->ram[a] = v >> 8;
    f->ram[a + 1] = v;
    return true;
}
static bool wl(void *ctx, uint32_t a, uint32_t v) {
    fixture_t *f = ctx;
    f->ram[a] = v >> 24;
    f->ram[a + 1] = v >> 16;
    f->ram[a + 2] = v >> 8;
    f->ram[a + 3] = v;
    return true;
}
static bool csw(void *ctx, uint8_t fc, uint32_t a, uint8_t size, uint32_t value) {
    fixture_t *f = ctx;
    assert(fc == MC68K_FC_CPU_SPACE && a == 0xFFFFFFFEu && size == 2);
    f->cpu_space_writes++;
    f->cpu_space_value = value;
    return true;
}
static bool csr(void *ctx, uint8_t fc, uint32_t a, uint8_t size, uint32_t *value) {
    fixture_t *f = ctx;
    assert(fc == MC68K_FC_CPU_SPACE && a == 0 && size == 1);
    f->cpu_space_reads++;
    *value = 0;
    return true;
}
static void word(fixture_t *f, uint32_t a, uint16_t v) {
    f->ram[a] = v >> 8;
    f->ram[a + 1] = v;
}
static void lng(fixture_t *f, uint32_t a, uint32_t v) {
    f->ram[a] = v >> 24;
    f->ram[a + 1] = v >> 16;
    f->ram[a + 2] = v >> 8;
    f->ram[a + 3] = v;
}
static mc68k_state_t *make_cpu(fixture_t *f) {
    mc68k_bus_t bus = {.read_byte = rb,
                       .read_word = rw,
                       .read_long = rl,
                       .write_byte = wb,
                       .write_word = ww,
                       .write_long = wl,
                       .ctx = f,
                       .cpu_space_read = csr,
                       .cpu_space_write = csw};
    lng(f, 0, 0x800);
    lng(f, 4, 0x100);
    lng(f, 61 * 4, 0x300);
    mc68k_state_t *cpu = mc68k_create_model(&bus, MC68K_MODEL_68060);
    assert(cpu);
    mc68k_reset(cpu);
    return cpu;
}
static void expect_unimplemented(mc68k_state_t *cpu, fixture_t *f, uint16_t opcode,
                                 uint16_t extension) {
    cpu->pc = 0x100;
    word(f, 0x100, opcode);
    word(f, 0x102, extension);
    mc68k_execute(cpu, 1);
    assert(cpu->last_exception_vector == 61 && cpu->pc == 0x300);
    assert(((uint16_t)f->ram[0x7fe] << 8 | f->ram[0x7ff]) == (61 * 4));
    assert(((uint32_t)f->ram[0x7fa] << 24 | (uint32_t)f->ram[0x7fb] << 16 |
            (uint32_t)f->ram[0x7fc] << 8 | f->ram[0x7fd]) == 0x100);
    cpu->a[7] = 0x800;
}
int main(void) {
    fixture_t f = {0};
    mc68k_state_t *cpu = make_cpu(&f);
    expect_unimplemented(cpu, &f, 0x0108, 0);      /* MOVEP.W (d16,A0),D0 */
    expect_unimplemented(cpu, &f, 0x00d0, 0);      /* CHK2.B (A0),D0 */
    expect_unimplemented(cpu, &f, 0x0cfc, 0);      /* CAS2.W */
    expect_unimplemented(cpu, &f, 0x4c3c, 0x0400); /* MULU.L #imm,D0:D0 */

    /* 非整列 CAS は operand を読む前に vector 61 へ入る。 */
    cpu->a[0] = DATA_AREA + 1;
    f.data_reads = 0;
    cpu->d[1] = 0;
    cpu->d[2] = 0;
    expect_unimplemented(cpu, &f, 0x0cd0, 0x0081); /* CAS.W D1,D2,(A0) */
    assert(f.data_reads == 0 && cpu->a[0] == DATA_AREA + 1);
    cpu->a[0] = DATA_AREA;
    word(&f, DATA_AREA, 0x1234);
    cpu->pc = 0x120;
    word(&f, 0x120, 0x0cd0);
    word(&f, 0x122, 0x0081);
    mc68k_execute(cpu, 1);
    assert(f.data_reads == 1 && cpu->pc == 0x124);

    cpu->pc = 0x140;
    word(&f, 0x140, 0x4c3c);
    word(&f, 0x142, 0);
    lng(&f, 0x144, 2);
    cpu->d[0] = 3;
    mc68k_execute(cpu, 1);
    assert(cpu->d[0] == 6);

    cpu->pc = 0x160;
    word(&f, 0x160, 0x4e7b);
    word(&f, 0x162, 0x0008);
    cpu->d[0] = UINT32_C(0xf0000000);
    mc68k_execute(cpu, 1);
    assert(cpu->buscr == UINT32_C(0xf0000000));
    cpu->pc = 0x164;
    word(&f, 0x164, 0x4e7a);
    word(&f, 0x166, 0x0008);
    cpu->d[0] = 0;
    mc68k_execute(cpu, 1);
    assert(cpu->d[0] == UINT32_C(0xf0000000));

    cpu->pc = 0x180;
    word(&f, 0x180, 0xf800);
    word(&f, 0x182, 0x01c0);
    word(&f, 0x184, 0x2700);
    mc68k_execute(cpu, 1);
    assert(cpu->exec_state == CPU_STOPPED && f.cpu_space_writes == 1 &&
           f.cpu_space_value == 0x2700);

    mc68k_reset(cpu);
    cpu->pc = 0x1a0;
    word(&f, 0x1a0, 0x4848);
    mc68k_execute(cpu, 1);
    assert(cpu->last_exception_vector == 4 && f.cpu_space_reads == 1);

    mc68k_reset(cpu);
    cpu->sr |= SR_T1;
    expect_unimplemented(cpu, &f, 0x0108, 0);
    assert(cpu->last_exception_vector == 61);

    mc68k_reset(cpu);
    cpu->sr |= SR_T0;
    cpu->pc = 0x1c0;
    word(&f, 0x1c0, 0x6002);
    mc68k_execute(cpu, 1);
    assert(cpu->pc == 0x1c4 && cpu->last_exception_vector == 0);

    mc68k_reset(cpu);
    cpu->a[7] = 0x700;
    word(&f, 0x700, 0x2700);
    lng(&f, 0x702, 0x1e0);
    word(&f, 0x706, 0);
    cpu->pc = 0x1d0;
    word(&f, 0x1d0, 0x4e73);
    mc68k_execute(cpu, 1);
    assert(cpu->pc == 0x1e0 && cpu->a[7] == 0x708);
    mc68k_destroy(cpu);
    return 0;
}
