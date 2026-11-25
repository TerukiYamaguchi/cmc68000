/* 68020+ group-0 restart state regressions (x68000 ). */
#include "mc68000/cpu.h"
#include "cpu/cpu_internal.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t mem[0x4000];
    uint32_t fail_write_at;
    uint32_t fail_read_at;
    unsigned reads; /* rw が呼ばれた回数 */
} ctx_t;
static bool rb(void *p, uint32_t a, uint8_t *v) {
    ctx_t *c = p;
    if (a == c->fail_read_at)
        return false;
    *v = c->mem[a];
    return true;
}
static bool rw(void *p, uint32_t a, uint16_t *v) {
    ctx_t *c = p;
    c->reads++;
    if (a == c->fail_read_at)
        return false;
    *v = (uint16_t)((c->mem[a] << 8) | c->mem[a + 1]);
    return true;
}
static bool rl(void *p, uint32_t a, uint32_t *v) {
    ctx_t *c = p;
    *v = ((uint32_t)c->mem[a] << 24) | ((uint32_t)c->mem[a + 1] << 16) |
         ((uint32_t)c->mem[a + 2] << 8) | c->mem[a + 3];
    return true;
}
static bool wb(void *p, uint32_t a, uint8_t v) {
    ((ctx_t *)p)->mem[a] = v;
    return true;
}
static bool ww(void *p, uint32_t a, uint16_t v) {
    ctx_t *c = p;
    c->mem[a] = (uint8_t)(v >> 8);
    c->mem[a + 1] = (uint8_t)v;
    return true;
}
static bool wl(void *p, uint32_t a, uint32_t v) {
    ctx_t *c = p;
    if (a == c->fail_write_at)
        return false;
    return ww(p, a, (uint16_t)(v >> 16)) && ww(p, a + 2, (uint16_t)v);
}
static void pw(ctx_t *c, uint32_t a, uint16_t v) {
    if (!ww(c, a, v))
        abort();
}
static void pl(ctx_t *c, uint32_t a, uint32_t v) {
    if (!wl(c, a, v))
        abort();
}
static uint32_t gl(ctx_t *c, uint32_t a) {
    uint32_t v = 0;
    if (!rl(c, a, &v))
        abort();
    return v;
}

static void test_supervisor_stack_selection(void) {
    static const struct {
        uint16_t sr;
        uint32_t active_sp;
    } cases[] = {
        {SR_S, 0x1e00},       /* ISP */
        {SR_S | SR_M, 0x2e00} /* MSP */
    };
    for (unsigned n = 0; n < sizeof cases / sizeof cases[0]; ++n) {
        ctx_t c = {0};
        c.fail_write_at = 0xffffffffu; /* address 0 is a legitimate write target below */
        c.fail_read_at = 0xffffffffu;
        pl(&c, 0, 0x1f00);
        pl(&c, 4, 0x100);
        pl(&c, 2 * 4, 0x300);
        pw(&c, 0x300, 0x60fe); /* inspect the handler entry */
        mc68k_bus_t bus = {rb, rw, rl, wb, ww, wl, NULL, NULL, NULL, NULL, &c};
        mc68k_state_t *cpu = mc68k_create_model(&bus, MC68K_MODEL_68020);
        mc68k_reset(cpu);
        cpu->ssp = 0x1e00;
        cpu->msp = 0x2e00;
        cpu_set_sr(cpu, cases[n].sr);
        cpu->a[7] = cases[n].active_sp;
        for (unsigned i = 0; i < 8; ++i)
            cpu->restart_a[i] = cpu->a[i];
        cpu->restart_sr = cases[n].sr;
        cpu->instr_pc = 0x100;
        cpu->opcode_fetch_complete = false;
        cpu->fault_access_kind = FAULT_OPCODE;
        cpu_signal_bus_error(cpu, 0x100, true);
        mc68k_execute(cpu, 1);
        assert(cpu->pc == 0x300 && cpu->a[7] == cases[n].active_sp - 32);
        assert(cpu->sr == cases[n].sr);
        /* Switching away commits the active A7 into its selected slot. */
        cpu_set_sr(cpu, 0);
        assert((cases[n].sr & SR_M) ? cpu->msp == cases[n].active_sp - 32
                                    : cpu->ssp == cases[n].active_sp - 32);
        mc68k_destroy(cpu);
    }
}

/* : base+0x18 (Data Output Buffer) must hold the faulting write's
 * data, not the fault address. cpu_write_long()'s frame push for the Fault
 * Address field (base+0x10) overwrites cpu->fault_data_out as a side effect,
 * so the value must be captured before the frame push begins. */
static void test_data_output_buffer_holds_write_data(void) {
    ctx_t c = {0};
    c.fail_write_at = 0xffffffffu; /* address 0 is a legitimate write target below */
    c.fail_read_at = 0xffffffffu;
    pl(&c, 0, 0x1f00);
    pl(&c, 4, 0x100);
    pl(&c, 2 * 4, 0x300);
    pw(&c, 0x300, 0x60fe); /* hold at handler entry for frame inspection */
    c.fail_write_at = 0x3f00;
    mc68k_bus_t bus = {rb, rw, rl, wb, ww, wl, NULL, NULL, NULL, NULL, &c};
    mc68k_state_t *cpu = mc68k_create_model(&bus, MC68K_MODEL_68020);
    mc68k_reset(cpu);
    cpu->ssp = 0x1f00;
    cpu->a[7] = 0x1f00;
    for (unsigned i = 0; i < 8; ++i)
        cpu->restart_a[i] = cpu->a[i];
    cpu->restart_sr = 0;
    cpu->instr_pc = 0x100;
    cpu->opcode_fetch_complete = true; /* mid-instruction data write -> format $B */
    cpu->fault_access_kind = FAULT_DATA_WRITE;
    cpu_write_long(cpu, 0x3f00, 0xcafebabeu); /* faulting write: fail_write_at rejects it */
    assert(cpu->exec_state == CPU_EXCEPTION);
    assert(cpu->addr_err_addr == 0x3f00);

    mc68k_execute(cpu, 1); /* build the $B frame and enter vector 2 */
    assert(cpu->pc == 0x300 && cpu->last_exception_vector == 2);
    assert(gl(&c, cpu->a[7] + 0x10) == 0x3f00);      /* Fault Address */
    assert(gl(&c, cpu->a[7] + 0x18) == 0xcafebabeu); /* Data Output Buffer */
    mc68k_destroy(cpu);
}

/* : cpu_note_access() used to overwrite fault_fc/fault_size_code/
 * fault_rm/fault_access_kind on every bus cycle, even after a group-0 fault
 * had already latched. A multi-cycle instruction (MOVEM/MOVEP, the bitfield
 * and CAS2 loops) can still issue a further bus cycle in the same loop
 * iteration before its caller notices exec_state != CPU_NORMAL; that later
 * cycle's attributes must not clobber the ones describing the actual fault,
 * or e.g. a read fault comes out of the frame mislabeled as a write. */
static void test_note_access_latch_survives_later_cycles(void) {
    ctx_t c = {0};
    c.fail_write_at = 0xffffffffu;
    c.fail_read_at = 0xffffffffu;
    pl(&c, 0, 0x1f00);
    pl(&c, 4, 0x100);
    pl(&c, 2 * 4, 0x300);
    pw(&c, 0x300, 0x60fe); /* hold at handler entry for frame inspection */
    c.fail_read_at = 0x3000;
    mc68k_bus_t bus = {rb, rw, rl, wb, ww, wl, NULL, NULL, NULL, NULL, &c};
    mc68k_state_t *cpu = mc68k_create_model(&bus, MC68K_MODEL_68020);
    mc68k_reset(cpu);
    cpu->ssp = 0x1f00;
    cpu->a[7] = 0x1f00;
    for (unsigned i = 0; i < 8; ++i)
        cpu->restart_a[i] = cpu->a[i];
    cpu->restart_sr = 0;
    cpu->instr_pc = 0x100;
    cpu->opcode_fetch_complete = true; /* mid-instruction data access -> format $B */
    cpu->fault_access_kind = FAULT_DATA_READ;

    cpu_read_byte(cpu, 0x3000); /* faulting read: fail_read_at rejects it */
    assert(cpu->exec_state == CPU_EXCEPTION);
    assert(cpu->addr_err_addr == 0x3000 && cpu->fault_access_kind == FAULT_DATA_READ);

    /* Simulate the further bus cycle a real multi-cycle loop can still issue
     * after the fault, before its caller checks exec_state. This unrelated,
     * successful write must not relabel the already-latched fault. */
    cpu_write_word(cpu, 0x2000, 0x1234u);
    assert(cpu->fault_access_kind == FAULT_DATA_READ);

    mc68k_execute(cpu, 1); /* build the $B frame and enter vector 2 */
    assert(cpu->pc == 0x300 && cpu->last_exception_vector == 2);
    assert(gl(&c, cpu->a[7] + 0x10) == 0x3000); /* Fault Address: the read */
    uint16_t ssw = 0;
    if (!rw(&c, cpu->a[7] + 0x0a, &ssw))
        abort();
    assert((ssw & 0x0040u) != 0); /* RW=1 (read), not overwritten to write */
    mc68k_destroy(cpu);
}

/*  (mc68000リポジトリ): cpu_fetch_word() は fault_access_kind の代入を
 * cpu_note_access() の group0_fault_latched ガードの外で行っていた。データ
 * フォールトが既にラッチされた後の同じ反復で拡張語の fetch を続けるハンドラは
 * 現状存在しないが、将来そのようなハンドラが追加された場合に備えて、
 * cpu_fetch_word() 自身にも同じガードを効かせる（test_note_access_latch_survives_later_cycles
 * と対の回帰テスト）。 */
static void test_fetch_word_kind_survives_after_latch(void) {
    ctx_t c = {0};
    c.fail_write_at = 0xffffffffu;
    c.fail_read_at = 0xffffffffu;
    pl(&c, 0, 0x1f00);
    pl(&c, 4, 0x100);
    pl(&c, 2 * 4, 0x300);
    pw(&c, 0x300, 0x60fe);  /* hold at handler entry for frame inspection */
    pw(&c, 0x2000, 0x4e71); /* NOP: exec_state を NORMAL に戻したときの取得対象 */
    c.fail_read_at = 0x3000;
    mc68k_bus_t bus = {rb, rw, rl, wb, ww, wl, NULL, NULL, NULL, NULL, &c};
    mc68k_state_t *cpu = mc68k_create_model(&bus, MC68K_MODEL_68020);
    mc68k_reset(cpu);
    cpu->ssp = 0x1f00;
    cpu->a[7] = 0x1f00;
    for (unsigned i = 0; i < 8; ++i)
        cpu->restart_a[i] = cpu->a[i];
    cpu->restart_sr = 0;
    cpu->instr_pc = 0x100;
    cpu->opcode_fetch_complete = true; /* mid-instruction data access -> format $B */
    cpu->fault_access_kind = FAULT_DATA_READ;

    cpu_read_byte(cpu, 0x3000); /* faulting read: fail_read_at rejects it */
    assert(cpu->exec_state == CPU_EXCEPTION);
    assert(cpu->addr_err_addr == 0x3000 && cpu->fault_access_kind == FAULT_DATA_READ);

    /* Simulate a handler that keeps fetching an extension word after the
     * fault, before its caller checks exec_state. This later fetch must not
     * relabel the already-latched data fault as EXTENSION. */
    cpu->pc = 0x2000;
    const unsigned reads_before = c.reads;
    /* mc68000#121: exec_state が NORMAL でないときは取得せず 0 を返し、PC も進めない。 */
    assert(cpu_fetch_word(cpu) == 0);
    assert(c.reads == reads_before && cpu->pc == 0x2000);
    assert(cpu->fault_access_kind == FAULT_DATA_READ);

    /* exec_state の確認を通った場合でも、ラッチ済みの種別は上書きされない（ラッチのガード）。
     */
    cpu->exec_state = CPU_NORMAL;
    assert(cpu_fetch_word(cpu) == 0x4e71);
    assert(cpu->fault_access_kind == FAULT_DATA_READ);
    cpu->exec_state = CPU_EXCEPTION;

    mc68k_execute(cpu, 1); /* build the $B frame and enter vector 2 */
    assert(cpu->pc == 0x300 && cpu->last_exception_vector == 2);
    assert(gl(&c, cpu->a[7] + 0x10) == 0x3000); /* Fault Address: the read */
    uint16_t ssw = 0;
    if (!rw(&c, cpu->a[7] + 0x0a, &ssw))
        abort();
    assert((ssw & 0x0040u) != 0); /* RW=1 (read), not overwritten to write */
    mc68k_destroy(cpu);
}

/* mc68000#101: RTE of a $1 frame switches to MSP, then the real frame read
 * faults.  The rewind must leave ISP and MSP where they were. */
static void test_rewind_after_stack_switch(void) {
    static const mc68k_model_t models[] = {MC68K_MODEL_68020, MC68K_MODEL_68030};
    enum { ISP = 0x1f00, MSP_FRAME = 0x2800, FRAME_BYTES = 0x5c };
    for (unsigned n = 0; n < sizeof models / sizeof models[0]; ++n) {
        ctx_t c = {0};
        c.fail_write_at = 0xffffffffu;
        c.fail_read_at = MSP_FRAME + 6; /* format word of the real frame */
        pl(&c, 0, 0x1f00);
        pl(&c, 4, 0x100);
        pl(&c, 2 * 4, 0x300);
        pw(&c, 0x100, 0x4e73); /* RTE */
        pw(&c, 0x300, 0x60fe);
        pw(&c, ISP, 0x3000);
        pl(&c, ISP + 2, 0x500);
        pw(&c, ISP + 6, 0x1000);
        mc68k_bus_t bus = {rb, rw, rl, wb, ww, wl, NULL, NULL, NULL, NULL, &c};
        mc68k_state_t *cpu = mc68k_create_model(&bus, models[n]);
        mc68k_reset(cpu);
        cpu_set_sr(cpu, SR_S);
        cpu->a[7] = ISP;
        cpu->msp = MSP_FRAME;
        for (int i = 0; i < 4 && cpu->pc != 0x300; ++i)
            mc68k_execute(cpu, 1);
        assert(cpu->pc == 0x300);
        assert(cpu->a[7] == ISP - FRAME_BYTES && cpu->msp == MSP_FRAME && cpu->sr == SR_S);
        mc68k_destroy(cpu);
    }
}

int main(void) {
    ctx_t c = {0};
    c.fail_write_at = 0xffffffffu; /* address 0 is a legitimate write target below */
    c.fail_read_at = 0xffffffffu;
    pl(&c, 0, 0x1f00);
    pl(&c, 4, 0x100);
    pl(&c, 2 * 4, 0x300);
    pw(&c, 0x100, 0x4e71); /* restart target */
    pw(&c, 0x300, 0x60fe); /* hold at handler entry for frame inspection */
    mc68k_bus_t bus = {rb, rw, rl, wb, ww, wl, NULL, NULL, NULL, NULL, &c};
    mc68k_state_t *cpu = mc68k_create_model(&bus, MC68K_MODEL_68020);
    mc68k_reset(cpu);

    const uint16_t restart_sr = 0x801fu; /* user, trace, distinct CCR */
    for (unsigned i = 0; i < 8; ++i) {
        cpu->restart_d[i] = 0x11110000u + i;
        cpu->restart_a[i] = 0x22220000u + i;
        cpu->d[i] = 0xdead0000u + i;
        cpu->a[i] = 0xbeef0000u + i;
    }
    cpu->restart_a[7] = 0x700;
    cpu->restart_sr = restart_sr;
    cpu->sr = 0x0000; /* controlled divergence from the instruction snapshot */
    cpu->ssp = 0x1f00;
    cpu->instr_pc = 0x100;
    cpu->opcode_fetch_complete = false;
    cpu->fault_access_kind = FAULT_OPCODE;
    cpu_signal_bus_error(cpu, 0x100, true);
    assert(cpu->exec_state == CPU_EXCEPTION);

    mc68k_execute(cpu, 1); /* build $A frame and enter vector 2 */
    assert(cpu->pc == 0x300 && cpu->last_exception_vector == 2);
    assert(cpu->sr == (uint16_t)((restart_sr | SR_S) & ~SR_T));
    assert(cpu->usp == 0x700 && cpu->a[7] == 0x1f00 - 32);
    for (unsigned i = 0; i < 8; ++i) {
        assert(cpu->d[i] == 0x11110000u + i);
        if (i != 7)
            assert(cpu->a[i] == 0x22220000u + i);
    }
    uint16_t stacked_sr = 0;
    assert(rw(&c, cpu->a[7], &stacked_sr) && stacked_sr == restart_sr);

    pw(&c, 0x300, 0x4e73); /* handler RTE */
    mc68k_execute(cpu, 1); /* RTE restores the same restart snapshot */
    assert(cpu->pc == 0x100 && cpu->sr == restart_sr && cpu->a[7] == 0x700);
    mc68k_destroy(cpu);
    test_supervisor_stack_selection();
    test_data_output_buffer_holds_write_data();
    test_note_access_latch_survives_later_cycles();
    test_fetch_word_kind_survives_after_latch();
    test_rewind_after_stack_switch();
    return 0;
}
