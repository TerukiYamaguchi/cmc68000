#include "mc68000/cpu.h"
#include "mc68000/debug.h"
#include "cpu/cpu_internal.h"
#include "fpu/fpu_exec.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

enum { RAM_SIZE = 0x10000, ROOT = 0x2000, POINTER = 0x4000, PAGE = 0x6000 };

typedef struct {
    uint8_t m[RAM_SIZE];
    uint32_t fail_read_at;
} ctx_t;

static uint8_t pk(void *p, uint32_t a) {
    return a < RAM_SIZE ? ((ctx_t *)p)->m[a] : 0;
}
static bool rb(void *p, uint32_t a, uint8_t *v) {
    *v = ((ctx_t *)p)->m[a];
    return a < RAM_SIZE;
}
static bool rw(void *p, uint32_t a, uint16_t *v) {
    ctx_t *c = p;
    if (a + 1 >= RAM_SIZE)
        return false;
    *v = (uint16_t)((c->m[a] << 8) | c->m[a + 1]);
    return true;
}
static bool rl(void *p, uint32_t a, uint32_t *v) {
    ctx_t *c = p;
    if (a + 3 >= RAM_SIZE || a == c->fail_read_at)
        return false;
    *v = ((uint32_t)c->m[a] << 24) | ((uint32_t)c->m[a + 1] << 16) |
         ((uint32_t)c->m[a + 2] << 8) | c->m[a + 3];
    return true;
}
static bool wb(void *p, uint32_t a, uint8_t v) {
    if (a >= RAM_SIZE)
        return false;
    ((ctx_t *)p)->m[a] = v;
    return true;
}
static bool ww(void *p, uint32_t a, uint16_t v) {
    ctx_t *c = p;
    if (a + 1 >= RAM_SIZE)
        return false;
    c->m[a] = (uint8_t)(v >> 8);
    c->m[a + 1] = (uint8_t)v;
    return true;
}
static bool wl(void *p, uint32_t a, uint32_t v) {
    ctx_t *c = p;
    if (a + 3 >= RAM_SIZE)
        return false;
    c->m[a] = (uint8_t)(v >> 24);
    c->m[a + 1] = (uint8_t)(v >> 16);
    c->m[a + 2] = (uint8_t)(v >> 8);
    c->m[a + 3] = (uint8_t)v;
    return true;
}
static void pl(ctx_t *c, uint32_t a, uint32_t v) {
    if (!wl(c, a, v))
        abort();
}
static void pw(ctx_t *c, uint32_t a, uint16_t v) {
    if (!ww(c, a, v))
        abort();
}
static uint32_t gl(ctx_t *c, uint32_t a) {
    uint32_t v;
    if (!rl(c, a, &v))
        abort();
    return v;
}

static void map_page(ctx_t *c, uint32_t logical, unsigned shift, uint32_t physical,
                     uint32_t flags) {
    unsigned ri = logical >> 25, pi = (logical >> 18) & 0x7f;
    unsigned pgi = shift == 12 ? (logical >> 12) & 0x3f : (logical >> 13) & 0x1f;
    pl(c, ROOT + ri * 4u, POINTER | 2u);
    pl(c, POINTER + pi * 4u, PAGE | 2u);
    pl(c, PAGE + pgi * 4u, (physical & ~((UINT32_C(1) << shift) - 1u)) | flags | 1u);
}
static void clear_atc(mc68k_state_t *cpu) {
    memset(cpu->atc040_i, 0, sizeof(cpu->atc040_i));
    memset(cpu->atc040_d, 0, sizeof(cpu->atc040_d));
}

/* mc68000#96: a read or complex data access fault restarts the instruction (a simple
 * store is a deferred WB2, mc68000#97).  The handler
 * (PFLUSHA; RTE) is preceded by a host step that validates the leaf. */
enum {
    CODE = 0x400,
    HANDLER = 0x1000,
    SSP0 = 0xf000,
    FA_PAGE = 0x12345000,
    FA_PHYS = 0xa000,
    OK_PAGE = 0x12344000,
    OK_PHYS = 0x8000,
    USP_PAGE = 0x12346000,
    USP_PHYS = 0xc000,
    FRAME_BYTES = 60
};

typedef struct {
    ctx_t c;
    mc68k_state_t *cpu;
    uint32_t leaf[3];
} rig_t;

static uint32_t leaf_of(uint32_t logical) {
    return PAGE + ((logical >> 12) & 0x3f) * 4u;
}

static void rig_init_fpu(rig_t *r, mc68k_fpu_config_t fpu_config) {
    memset(r, 0, sizeof(*r));
    r->c.fail_read_at = UINT32_MAX;
    ctx_t *c = &r->c;
    mc68k_bus_t bus = {rb, rw, rl, wb, ww, wl, NULL, NULL, NULL, NULL, c};
    pl(c, 0, SSP0);
    pl(c, 4, CODE);
    pl(c, 8, HANDLER);
    pw(c, HANDLER, 0xf518);
    pw(c, HANDLER + 2, 0x4e73);
    r->cpu = mc68k_create_model_with_fpu(&bus, MC68K_MODEL_68040, fpu_config);
    assert(r->cpu);
    mc68k_reset(r->cpu);
    r->cpu->mmu040_tc = 0x8000;
    r->cpu->mmu040_srp = r->cpu->mmu040_urp = ROOT;
    r->cpu->mmu040_dtt0 = 0x0000e000; /* logical 0x00xxxxxx, user and supervisor */
    r->cpu->mmu040_itt0 = 0x0000e000;
    map_page(c, FA_PAGE, 12, FA_PHYS, 0);
    pl(c, leaf_of(FA_PAGE), FA_PHYS); /* invalid */
    map_page(c, OK_PAGE, 12, OK_PHYS, 0);
    map_page(c, USP_PAGE, 12, USP_PHYS, 0);
}

static void rig_init(rig_t *r) {
    rig_init_fpu(r, MC68K_FPU_NONE_040);
}

/* Run until the handler is entered; return the frame base. */
static uint32_t rig_run_to_handler(rig_t *r) {
    for (int i = 0; i < 4 && r->cpu->pc != HANDLER; i++)
        mc68k_execute(r->cpu, 1);
    assert(r->cpu->pc == HANDLER);
    return r->cpu->a[7];
}

static void rig_validate(rig_t *r) {
    pl(&r->c, leaf_of(FA_PAGE), FA_PHYS | 1u);
}

static uint16_t frame_word(rig_t *r, uint32_t a) {
    return (uint16_t)(r->c.m[a] << 8 | r->c.m[a + 1]);
}

static void test_data_fault_restart(void) {
    rig_t r;
    uint32_t base;

    /* Write fault: MOVE.W -(A0),-(A1) with an invalid destination.  The store is a
     * pending WB2 (040UM 8.4.6): the instruction completes, the frame stacks the
     * next PC, and nothing is rewound. */
    rig_init(&r);
    pw(&r.c, CODE, 0x3320);
    pw(&r.c, 0x2000, 0x8001);
    r.cpu->a[0] = 0x2002;
    r.cpu->a[1] = FA_PAGE + 2;
    base = rig_run_to_handler(&r);
    assert(gl(&r.c, base + 2) == CODE + 2 && frame_word(&r, base + 12) == 0x0445);
    assert(gl(&r.c, base + 20) == FA_PAGE);
    assert(r.c.m[base + 15] == 0 && r.c.m[base + 17] == 0xc5 && r.c.m[base + 19] == 0);
    assert(gl(&r.c, base + 32) == FA_PAGE && (gl(&r.c, base + 36) & 0xffff) == 0x8001);
    assert(r.cpu->a[0] == 0x2000 && r.cpu->a[1] == FA_PAGE);
    assert((r.c.m[FA_PHYS] << 8 | r.c.m[FA_PHYS + 1]) == 0); /* not stored */
    assert((frame_word(&r, base) & 0x1f) == 0x08);           /* N set, X untouched */
    assert(r.cpu->a[7] == SSP0 - FRAME_BYTES);
    rig_validate(&r);
    mc68k_execute(r.cpu, 1); /* PFLUSHA */
    mc68k_execute(r.cpu, 1); /* RTE */
    assert(r.cpu->pc == CODE + 2);
    mc68k_destroy(r.cpu);

    /* A second write in the same instruction cannot stay pending: the fault falls
     * back to the restart model (a MOVEM is never deferred). */
    rig_init(&r);
    pw(&r.c, CODE, 0x4890);
    pw(&r.c, CODE + 2, 0x0003); /* MOVEM.W D0-D1,(A0) */
    r.cpu->a[0] = FA_PAGE;
    r.cpu->d[0] = 0x1111;
    r.cpu->d[1] = 0x2222;
    base = rig_run_to_handler(&r);
    assert(gl(&r.c, base + 2) == CODE && r.c.m[base + 17] == 0 && r.cpu->a[0] == FA_PAGE);
    mc68k_destroy(r.cpu);

    /* A read-modify-write fault is never pending (TAS (A0)). */
    rig_init(&r);
    pw(&r.c, CODE, 0x4ad0);
    r.cpu->a[0] = FA_PAGE;
    base = rig_run_to_handler(&r);
    assert(gl(&r.c, base + 2) == CODE && r.c.m[base + 17] == 0);
    mc68k_destroy(r.cpu);

    /* Byte store: WB2S SIZE=01 (0x81|0x20 = 0xa5 with TM 5). */
    rig_init(&r);
    pw(&r.c, CODE, 0x1080); /* MOVE.B D0,(A0) */
    r.cpu->a[0] = FA_PAGE + 3;
    r.cpu->d[0] = 0x5a;
    base = rig_run_to_handler(&r);
    assert(gl(&r.c, base + 2) == CODE + 2 && r.c.m[base + 17] == 0xa5);
    assert(gl(&r.c, base + 32) == FA_PAGE + 3 && gl(&r.c, base + 36) == 0x5a);
    assert(r.c.m[base + 15] == 0 && r.c.m[base + 19] == 0 && gl(&r.c, base + 24) == 0 &&
           gl(&r.c, base + 40) == 0);
    mc68k_destroy(r.cpu);

    /* MOVES.B D0,(A0) with DFC=1: WB2S TT = 10 (alternate function code) and TM = DFC,
     * 0x80|0x20|0x10|1 = 0xb1. */
    rig_init(&r);
    pw(&r.c, CODE, 0x0e10);
    pw(&r.c, CODE + 2, 0x0800);
    r.cpu->a[0] = FA_PAGE + 3;
    r.cpu->d[0] = 0x5a;
    r.cpu->dfc = 1;
    base = rig_run_to_handler(&r);
    assert(gl(&r.c, base + 2) == CODE + 4 && r.c.m[base + 17] == 0xb1);
    assert(gl(&r.c, base + 32) == FA_PAGE + 3 && gl(&r.c, base + 36) == 0x5a);
    assert(frame_word(&r, base + 12) == 0x0431); /* SSW: ATC, byte, TT=10, TM=DFC */
    mc68k_destroy(r.cpu);

    /* MOVES.B (A0),D0 with SFC=2: a read fault restarts, and the SSW TT is 10 too. */
    rig_init(&r);
    pw(&r.c, CODE, 0x0e10);
    pw(&r.c, CODE + 2, 0x0000);
    r.cpu->a[0] = FA_PAGE + 3;
    r.cpu->sfc = 2;
    base = rig_run_to_handler(&r);
    assert(gl(&r.c, base + 2) == CODE && frame_word(&r, base + 12) == 0x0532);
    mc68k_destroy(r.cpu);

    /* A host bus error on a MOVES.L read has no MMU record: the fallback SSW carries TT=10
     * too. */
    rig_init(&r);
    pw(&r.c, CODE, 0x0e90);
    pw(&r.c, CODE + 2, 0x0000);
    r.cpu->a[0] = OK_PAGE;
    r.cpu->sfc = 2;
    r.c.fail_read_at = OK_PHYS;
    base = rig_run_to_handler(&r);
    assert(frame_word(&r, base + 12) == 0x0112);
    mc68k_destroy(r.cpu);

    /* A single-register MOVEM, and a store under trace, restart. */
    rig_init(&r);
    pw(&r.c, CODE, 0x4890);
    pw(&r.c, CODE + 2, 0x0001); /* MOVEM.W D0,(A0) */
    r.cpu->a[0] = FA_PAGE;
    base = rig_run_to_handler(&r);
    assert(gl(&r.c, base + 2) == CODE && r.c.m[base + 17] == 0);
    mc68k_destroy(r.cpu);
    rig_init(&r);
    pw(&r.c, CODE, 0x3080); /* MOVE.W D0,(A0) */
    r.cpu->a[0] = FA_PAGE;
    cpu_set_sr(r.cpu, 0xa700);
    base = rig_run_to_handler(&r);
    assert(gl(&r.c, base + 2) == CODE && r.c.m[base + 17] == 0);
    mc68k_destroy(r.cpu);

    /* A split store across the page boundary is never deferred. */
    rig_init(&r);
    pw(&r.c, CODE, 0x3080);
    r.cpu->a[0] = FA_PAGE - 1;
    base = rig_run_to_handler(&r);
    assert(gl(&r.c, base + 2) == CODE && r.c.m[base + 17] == 0);
    mc68k_destroy(r.cpu);

    /* Read fault: A0 is decremented once. */
    rig_init(&r);
    pw(&r.c, CODE, 0x3020); /* MOVE.W -(A0),D0 */
    r.cpu->a[0] = FA_PAGE + 2;
    pw(&r.c, FA_PHYS, 0x1234);
    base = rig_run_to_handler(&r);
    assert(gl(&r.c, base + 2) == CODE && r.cpu->a[0] == FA_PAGE + 2);
    rig_validate(&r);
    mc68k_execute(r.cpu, 1);
    mc68k_execute(r.cpu, 1);
    mc68k_execute(r.cpu, 1);
    assert(r.cpu->a[0] == FA_PAGE && (r.cpu->d[0] & 0xffff) == 0x1234);
    mc68k_destroy(r.cpu);

    /* The extension word crosses into the invalid page. */
    rig_init(&r);
    r.cpu->pc = OK_PAGE + 0xffe;
    pw(&r.c, OK_PHYS + 0xffe, 0x203c);
    pw(&r.c, FA_PHYS, 0xdead);
    pw(&r.c, FA_PHYS + 2, 0xbeef);
    base = rig_run_to_handler(&r);
    assert(gl(&r.c, base + 2) == OK_PAGE + 0xffe);
    rig_validate(&r);
    mc68k_execute(r.cpu, 1);
    mc68k_execute(r.cpu, 1);
    mc68k_execute(r.cpu, 1);
    assert(r.cpu->d[0] == 0xdeadbeef);
    mc68k_destroy(r.cpu);

    /* JSR pushing onto an invalid user stack page: the push is a pending WB2, so the
     * branch completes and the frame stacks the target. */
    rig_init(&r);
    pw(&r.c, CODE, 0x4eb9);
    pl(&r.c, CODE + 2, 0x800);
    pw(&r.c, 0x800, 0x4e71);
    cpu_set_sr(r.cpu, 0); /* user mode */
    r.cpu->a[7] = FA_PAGE + 4;
    r.cpu->ssp = SSP0;
    base = rig_run_to_handler(&r);
    assert(gl(&r.c, base + 2) == 0x800 && r.cpu->usp == FA_PAGE);
    assert(r.c.m[base + 17] == 0x81 && gl(&r.c, base + 32) == FA_PAGE &&
           gl(&r.c, base + 36) == CODE + 6);
    mc68k_destroy(r.cpu);

    /* MOVEM through an index EA keeps the ATC bit alongside CM. */
    rig_init(&r);
    pw(&r.c, CODE, 0x48f0);
    pw(&r.c, CODE + 2, 0x0003);
    pw(&r.c, CODE + 4, 0x2000);
    r.cpu->a[0] = FA_PAGE;
    r.cpu->d[2] = 0;
    base = rig_run_to_handler(&r);
    assert((r.c.m[base + 12] << 8 | r.c.m[base + 13]) == 0x1405); /* CM|ATC, long, FC 5 */
    mc68k_destroy(r.cpu);
}

/* mc68000#97 review B6: guards that must keep a store out of the pending-WB2 path, and the
 * escalation of a pending store.  The white-box part sets wb2_defer_ok itself so that each
 * write after the first is the access under test. */
enum { WPT_PAGE = 0x12347000, WPT_PHYS = 0xd000, PTE_WP = 4 };

static void expect_restart(rig_t *r, uint32_t base) {
    assert(gl(&r->c, base + 2) == CODE && r->c.m[base + 17] == 0 && r->c.m[base + 15] == 0 &&
           r->c.m[base + 19] == 0);
}

static void test_wb2_not_deferred(void) {
    static const struct {
        uint16_t op;
        uint32_t a0;
    } split[] = {
        {0x3080, FA_PAGE - 1}, /* MOVE.W D0,(A0): 2nd byte faults */
        {0x3080, FA_PAGE + 1}, /* odd word: 1st byte faults */
        {0x2080, FA_PAGE + 2}, /* long on a word boundary: 1st word faults */
        {0x2080, FA_PAGE - 2}, /* long on a word boundary: only the 2nd word faults */
        {0x2080, FA_PAGE + 1}, /* odd long: 1st byte faults */
        {0x2080, FA_PAGE - 1}, /* odd long: middle word faults */
        {0x2080, FA_PAGE - 3}, /* odd long: last byte faults */
    };
    rig_t r;
    uint32_t base;
    for (unsigned i = 0; i < sizeof(split) / sizeof(split[0]); i++) {
        rig_init(&r);
        pw(&r.c, CODE, split[i].op);
        r.cpu->a[0] = split[i].a0;
        base = rig_run_to_handler(&r);
        expect_restart(&r, base);
        mc68k_destroy(r.cpu);
    }
}

/* mc68000#105: the read portion of TAS/CAS is a write for the W-bit check and the M update
 * (040UM §3.1.2 M-bit/W-bit, §3.3), so it faults or sets M at the read cycle. */
enum { RMW_U = 0x08, RMW_M = 0x10, SSW_RM = 0x0200, SSW_R = 0x0100 };
static const uint16_t rmw_ops[][2] = {{0x4ad0, 0},
                                      {0x0ad0, 0x0040}}; /* TAS (A0); CAS.B D0,D1,(A0) */

static void rmw_rig(rig_t *r, unsigned i, uint32_t flags) {
    rig_init(r);
    map_page(&r->c, WPT_PAGE, 12, WPT_PHYS, flags);
    pw(&r->c, CODE, rmw_ops[i][0]);
    if (rmw_ops[i][0] != 0x4ad0)
        pw(&r->c, CODE + 2, rmw_ops[i][1]);
    r->cpu->a[0] = WPT_PAGE;
    r->cpu->d[1] = 0x55; /* CAS update value; D0 = 0 matches the zeroed byte */
}

static void test_rmw_read_is_write(void) {
    rig_t r;
    for (unsigned i = 0; i < 2; i++) {
        /* WP page: the read faults (R and RM set), nothing is written, U is set and M stays
         * clear. */
        rmw_rig(&r, i, PTE_WP);
        uint32_t base = rig_run_to_handler(&r);
        expect_restart(&r, base);
        assert((frame_word(&r, base + 12) & (SSW_RM | SSW_R)) == (SSW_RM | SSW_R));
        assert(r.c.m[WPT_PHYS] == 0);
        assert((gl(&r.c, leaf_of(WPT_PAGE)) & (RMW_U | RMW_M)) == RMW_U);
        bool resident = false;
        for (unsigned j = 0; j < MC68K_ATC_ENTRIES; j++)
            resident |= r.cpu->atc040_d[j].valid &&
                        r.cpu->atc040_d[j].logical_page == WPT_PAGE &&
                        r.cpu->atc040_d[j].write_protect;
        assert(resident);
        mc68k_destroy(r.cpu);

        /* M=0, M=1 (U=1): the access completes and the leaf has M set. */
        static const uint32_t flags[] = {RMW_U, RMW_U | RMW_M, 0};
        for (unsigned k = 0; k < 3; k++) {
            rmw_rig(&r, i, flags[k]);
            mc68k_execute(r.cpu, 1);
            assert(r.cpu->exec_state == CPU_NORMAL && r.cpu->pc != HANDLER);
            assert((gl(&r.c, leaf_of(WPT_PAGE)) & (RMW_U | RMW_M)) == (RMW_U | RMW_M));
            assert(r.c.m[WPT_PHYS] == (i == 0 ? 0x80 : 0x55));
            mc68k_destroy(r.cpu);
        }
    }
}

/* mc68000#97 review B7: the in-place CT frame conversion of RTE is a frame under
 * construction, not the instruction's pending store. */
static void test_wb2_rte_conversion(void) {
    rig_t r;
    rig_init(&r);
    const uint32_t frame = FA_PAGE - 56; /* the converted frame ends in the invalid page */
    const uint32_t f = frame - OK_PAGE + OK_PHYS;
    pw(&r.c, CODE, 0x4e73); /* RTE */
    pw(&r.c, f, 0x2700);
    pl(&r.c, f + 2, CODE + 2);
    pw(&r.c, f + 6, 0x7000 | 2 * 4);
    pw(&r.c, f + 12, 0x2000); /* format 7, SSW = CT */
    r.cpu->a[7] = frame;
    uint32_t base = rig_run_to_handler(&r) - OK_PAGE + OK_PHYS; /* the stack is in OK_PAGE */
    expect_restart(&r, base);
    mc68k_destroy(r.cpu);
}

/* B8: a pending FMOVE store whose post-instruction exception would also be raised is
 * restarted as a whole, so the restarted FMOVE still stores. */
static void test_wb2_fpu_post_exception(void) {
    rig_t r;
    rig_init_fpu(&r, MC68K_FPU_68040);
    mc68k_fpu_state_t *st = &r.cpu->fpu040->state;
    st->fpcr_enable = MC68K_FPU_EXC_INEX2;
    st->fp[0] = (mc68k_fp80_t){0x3FFF, 0xC000000000000000ull}; /* 1.5: inexact as a long */
    pw(&r.c, CODE, 0xf210);
    pw(&r.c, CODE + 2, 0x6000); /* FMOVE.L FP0,(A0) */
    r.cpu->a[0] = FA_PAGE;
    uint32_t base = rig_run_to_handler(&r);
    expect_restart(&r, base);
    assert(!r.cpu->fpu040->exc.valid);
    rig_validate(&r);
    mc68k_execute(r.cpu, 1);
    mc68k_execute(r.cpu, 1); /* PFLUSHA, RTE */
    assert(r.cpu->pc == CODE);
    mc68k_execute(r.cpu, 1);
    assert(gl(&r.c, FA_PHYS) == 2 && r.cpu->fpu040->exc.valid);
    mc68k_destroy(r.cpu);
}

static void test_wb2_escalation(void) {
    rig_t r;
    rig_init(&r);
    mc68k_state_t *cpu = r.cpu;
    cpu->wb2_defer_ok = true;

    /* A store of a frame under construction is never deferred; a bound origin (frame done,
     * waiting for RTE) does not stop later stores from being deferred. */
    if (!cpu_exception_frame_origin_begin(cpu, cpu->sr, CODE))
        abort();
    cpu_write_word(cpu, FA_PAGE, 1);
    assert(cpu->exec_state == CPU_EXCEPTION && !cpu->wb2.valid);
    cpu->exec_state = CPU_NORMAL;
    cpu->group0_fault_latched = false;
    cpu->fault040.valid = false;
    cpu->exception_frame_origins->bound = true;
    cpu_write_word(cpu, FA_PAGE, 1);
    assert(cpu->exec_state == CPU_NORMAL && cpu->wb2.valid);
    cpu->wb2.valid = false;
    cpu_exception_frame_origins_clear(cpu);

    /* Outside an instruction handler nothing is deferred. */
    cpu->wb2_defer_ok = false;
    cpu_write_word(cpu, FA_PAGE, 1);
    assert(cpu->exec_state == CPU_EXCEPTION && !cpu->wb2.valid);
    cpu->exec_state = CPU_NORMAL;
    cpu->group0_fault_latched = false;
    cpu->fault040.valid = false;
    cpu->wb2_defer_ok = true;

    /* A store to a valid page is performed, notes its own access and the cache mode. */
    map_page(&r.c, WPT_PAGE, 12, WPT_PHYS, 0x40); /* CM = 2 */
    cpu->access_cache_mode = 0;
    (void)cpu_read_byte(cpu, OK_PAGE);
    cpu_write_word(cpu, WPT_PAGE, 0x1234);
    assert(cpu->exec_state == CPU_NORMAL && !cpu->wb2.valid &&
           gl(&r.c, WPT_PHYS) >> 16 == 0x1234);
    assert(cpu->access_cache_mode == 2);
    assert(cpu_noted_size_code(cpu) == 2 && cpu->fault_access_kind == FAULT_DATA_WRITE);

    /* An ATC fault is pending; a read does not disturb it, and the store is not performed. */
    (void)cpu_read_byte(cpu, OK_PAGE);
    cpu_write_word(cpu, FA_PAGE, 0x5678);
    assert(cpu->exec_state == CPU_NORMAL && cpu->wb2.valid && cpu->wb2.data == 0x5678);
    assert(cpu->wb2.logical == FA_PAGE && gl(&r.c, FA_PHYS) == 0);
    (void)cpu_read_byte(cpu, OK_PAGE);
    assert(cpu->exec_state == CPU_NORMAL && cpu->wb2.valid);

    /* A second write, even to a valid page, falls back to the restart model for the first. */
    uint8_t before[RAM_SIZE];
    memcpy(before, r.c.m, sizeof(before));
    cpu_write_byte(cpu, OK_PAGE, 0x77);
    assert(memcmp(before, r.c.m, sizeof(before)) == 0); /* the second store is not performed */
    assert(cpu->exec_state == CPU_EXCEPTION && !cpu->wb2.valid && cpu->fault_vector == 2);
    assert(cpu->addr_err_addr == FA_PAGE && !cpu->addr_err_read);
    assert(cpu->fault040.valid && cpu->fault040.fault_address == FA_PAGE &&
           cpu->fault040.ssw == 0x0445 && cpu->fault040.wb_status[1] == 0);
    assert(cpu_noted_size_code(cpu) == 2 && cpu->fault_access_kind == FAULT_DATA_WRITE);
    assert(r.c.m[OK_PHYS] == 0);
    cpu->exec_state = CPU_NORMAL;
    cpu->group0_fault_latched = false;
    cpu->fault040.valid = false;

    /* Escalation keeps the attributes of a fault that is already latched. */
    cpu->g0_mmu_latched = false;
    cpu_write_word(cpu, FA_PAGE, 0x5678);
    assert(cpu->wb2.valid);
    (void)cpu_read_long(cpu, FA_PAGE + 8); /* read fault latches first */
    assert(cpu->exec_state == CPU_EXCEPTION && cpu->wb2.valid && cpu->fault040.ssw == 0x0505);
    cpu_write_byte(cpu, OK_PAGE, 1);
    assert(!cpu->wb2.valid && cpu->fault040.ssw == 0x0505 && cpu->addr_err_read);
    cpu->exec_state = CPU_NORMAL;
    cpu->group0_fault_latched = false;
    cpu->fault040.valid = false;

    /* Delivery notes the pending access even after later accesses of another kind. */
    cpu_write_word(cpu, FA_PAGE + 4, 0x9abc);
    assert(cpu->wb2.valid);
    (void)cpu_read_byte(cpu, OK_PAGE);
    cpu_040_wb2_begin_delivery(cpu);
    assert(cpu->exec_state == CPU_EXCEPTION && !cpu->wb2.valid && cpu->g0_wb2_deferred);
    assert(cpu_noted_size_code(cpu) == 2 && cpu->fault_access_kind == FAULT_DATA_WRITE);
    assert(cpu->addr_err_addr == FA_PAGE + 4 && !cpu->addr_err_read && cpu->g0_mmu_latched);
    assert(cpu->fault040.wb_status[1] == 0xc5 && cpu->fault040.wb_address[1] == FA_PAGE + 4 &&
           cpu->fault040.wb_data[1] == 0x9abc && cpu->fault040.ssw == 0x0445);
    mc68k_destroy(cpu);
}

/* Reset drops a pending store and the deferral state. */
static void test_wb2_reset(void) {
    rig_t r;
    rig_init(&r);
    r.cpu->wb2.valid = true;
    r.cpu->wb2_defer_ok = true;
    r.cpu->g0_wb2_deferred = true;
    mc68k_reset(r.cpu);
    assert(!r.cpu->wb2.valid && !r.cpu->wb2_defer_ok && !r.cpu->g0_wb2_deferred);
    mc68k_destroy(r.cpu);
}

/* The flag set for a deferred delivery is consumed by that delivery. */
static void test_wb2_flag_consumed(void) {
    rig_t r;
    rig_init(&r);
    pw(&r.c, CODE, 0x3080); /* MOVE.W D0,(A0) */
    r.cpu->a[0] = FA_PAGE;
    (void)rig_run_to_handler(&r);
    assert(!r.cpu->g0_wb2_deferred && !r.cpu->wb2.valid && !r.cpu->wb2_defer_ok);
    mc68k_destroy(r.cpu);
}

/* The IRQ/TRAP frames land just below FA_PAGE, i.e. in OK_PAGE. */
static uint32_t ok_phys(uint32_t logical) {
    assert(logical >= OK_PAGE && logical < FA_PAGE);
    return logical - OK_PAGE + OK_PHYS;
}

/* mc68000#96 review A-B1: RTE of a $1 frame has switched to MSP when the real
 * frame read faults; the rewind must leave ISP and MSP where they were. */
static void test_restart_after_stack_switch(void) {
    rig_t r;
    const uint32_t isp = 0xdf00;
    rig_init(&r);
    pw(&r.c, CODE, 0x4e73);
    pw(&r.c, isp, 0x3000);
    pl(&r.c, isp + 2, 0x500);
    pw(&r.c, isp + 6, 0x1000);
    cpu_set_sr(r.cpu, 0x2000);
    r.cpu->a[7] = isp;
    r.cpu->msp = FA_PAGE;
    uint32_t base = rig_run_to_handler(&r);
    assert(base == isp - FRAME_BYTES && r.cpu->msp == FA_PAGE);
    assert(frame_word(&r, base) == 0x2000 && gl(&r.c, base + 2) == CODE);
    assert(r.cpu->sr == 0x2000);
    mc68k_destroy(r.cpu);
}

/* A-B5: D registers are rewound; a CM fault keeps the registers and PC of the
 * partial progress; a later fault without a latched record rewinds again. */
static void test_restart_register_rewind(void) {
    rig_t r;
    uint32_t base;

    rig_init(&r);
    pw(&r.c, CODE, 0x4cd0);
    pw(&r.c, CODE + 2, 0x0003); /* MOVEM.L (A0),D0-D1 */
    r.cpu->a[0] = FA_PAGE - 4;
    r.cpu->d[0] = 0x55555555;
    pl(&r.c, OK_PHYS + 0xffc, 0x11223344);
    base = rig_run_to_handler(&r);
    assert(r.cpu->d[0] == 0x55555555 && gl(&r.c, base + 2) == CODE);
    mc68k_destroy(r.cpu);

    /* CM: D0 loaded before D1 faulted stays loaded; PC is not rewound. */
    rig_init(&r);
    pw(&r.c, CODE, 0x4cf0);
    pw(&r.c, CODE + 2, 0x0003);
    pw(&r.c, CODE + 4, 0x2000);
    r.cpu->a[0] = FA_PAGE - 4;
    r.cpu->d[0] = 0x55555555;
    r.cpu->d[2] = 0;
    pl(&r.c, OK_PHYS + 0xffc, 0x11223344);
    base = rig_run_to_handler(&r);
    assert(frame_word(&r, base + 12) == 0x1505 && r.cpu->d[0] == 0x11223344);
    assert(gl(&r.c, base + 2) == CODE + 6);
    /* A later fault without a latched record must rewind despite the stale SSW. */
    r.cpu->mmu040_tc = 0;
    pw(&r.c, CODE + 0x100, 0x3020); /* MOVE.W -(A0),D0 from unmapped RAM */
    r.cpu->pc = CODE + 0x100;
    r.cpu->a[0] = RAM_SIZE + 2;
    r.cpu->a[7] = SSP0;
    for (int i = 0; i < 4 && r.cpu->pc != HANDLER; i++)
        mc68k_execute(r.cpu, 1);
    assert(r.cpu->pc == HANDLER);
    base = r.cpu->a[7];
    assert(r.cpu->a[0] == RAM_SIZE + 2 && gl(&r.c, base + 2) == CODE + 0x100);
    mc68k_destroy(r.cpu);

    /* A-N2: a physical bus error (no MMU translation failure) carries no ATC bit. */
    rig_init(&r);
    r.cpu->mmu040_tc = 0;
    pw(&r.c, CODE, 0x4cf0);
    pw(&r.c, CODE + 2, 0x0003);
    pw(&r.c, CODE + 4, 0x2000);
    r.cpu->a[0] = RAM_SIZE;
    r.cpu->d[2] = 0;
    base = rig_run_to_handler(&r);
    assert(frame_word(&r, base + 12) == 0x1105);
    mc68k_destroy(r.cpu);

    /* CP: an FPU post-instruction fault is not rewound. */
    rig_init(&r);
    r.cpu->mmu040_tc = 0;
    pw(&r.c, CODE, 0x3020);
    r.cpu->a[0] = RAM_SIZE + 2;
    r.cpu->fault040 = (mc68k_040_access_error_t){.valid = true, .ssw = 0x8000};
    base = rig_run_to_handler(&r);
    assert(r.cpu->a[0] == RAM_SIZE && gl(&r.c, base + 2) == CODE + 2);
    mc68k_destroy(r.cpu);
}

/* A-B5: IRQ acceptance and exception-frame writes keep their own origin. */
static void test_restart_origin(void) {
    rig_t r;
    uint32_t base;

    /* M=1 IRQ: the throwaway frame on ISP faults after the real frame went to MSP. */
    rig_init(&r);
    pw(&r.c, CODE, 0x4e71);
    pl(&r.c, 25 * 4, 0x2000);
    cpu_set_sr(r.cpu, 0x3000);
    r.cpu->a[7] = 0xd000;
    r.cpu->ssp = FA_PAGE + 8;
    mc68k_set_irq(r.cpu, 1);
    mc68k_execute(r.cpu, 1);
    mc68k_set_irq(r.cpu, 0);
    for (int i = 0; i < 4 && r.cpu->pc != HANDLER; i++)
        mc68k_execute(r.cpu, 1);
    assert(r.cpu->pc == HANDLER);
    assert(r.cpu->ssp == FA_PAGE && r.cpu->a[7] == 0xd000 - 8 - FRAME_BYTES);
    mc68k_destroy(r.cpu);

    /* M=0 IRQ: the frame push faults. */
    rig_init(&r);
    pw(&r.c, CODE, 0x4e71);
    pl(&r.c, 25 * 4, 0x2000);
    cpu_set_sr(r.cpu, 0x2000);
    r.cpu->a[7] = FA_PAGE + 4;
    mc68k_set_irq(r.cpu, 1);
    mc68k_execute(r.cpu, 1);
    mc68k_set_irq(r.cpu, 0);
    for (int i = 0; i < 4 && r.cpu->pc != HANDLER; i++)
        mc68k_execute(r.cpu, 1);
    assert(r.cpu->pc == HANDLER);
    base = r.cpu->a[7];
    /* The frame origin owns the fault: no rewind, the partial frame stays pushed. */
    assert(base == FA_PAGE + 4 - 6 - FRAME_BYTES);
    assert(frame_word(&r, ok_phys(base)) == 0x2000 && gl(&r.c, ok_phys(base) + 2) == CODE);
    mc68k_destroy(r.cpu);
}

enum { WP_PAGE = 0x12348000, WP_PHYS = 0xe000 };

/* A-B3: CAS2 stores nothing when the second operand faults, so the restarted instruction
 * compares against unmodified memory.  mc68000#105: the fault is raised by the locked read of
 * the second operand (R=1, RM=1) after the first operand's read has set U and M. */
static void test_cas2_write_fault_restart(void) {
    rig_t r;
    rig_init(&r);
    map_page(&r.c, WP_PAGE, 12, WP_PHYS, 4); /* write protected */
    pw(&r.c, CODE, 0x0efc);
    pw(&r.c, CODE + 2, 0x8080);
    pw(&r.c, CODE + 4, 0x90c1);
    pl(&r.c, OK_PHYS, 0x11111111);
    pl(&r.c, WP_PHYS, 0x22222222);
    r.cpu->a[0] = OK_PAGE;
    r.cpu->a[1] = WP_PAGE;
    r.cpu->d[0] = 0x11111111;
    r.cpu->d[1] = 0x22222222;
    r.cpu->d[2] = 0xaaaaaaaa;
    r.cpu->d[3] = 0xbbbbbbbb;
    uint32_t base = rig_run_to_handler(&r);
    assert(gl(&r.c, base + 2) == CODE && gl(&r.c, OK_PHYS) == 0x11111111 &&
           gl(&r.c, WP_PHYS) == 0x22222222);
    assert(gl(&r.c, base + 0x14) == WP_PAGE); /* FA */
    assert((frame_word(&r, base + 12) & (SSW_RM | SSW_R)) == (SSW_RM | SSW_R));
    assert((gl(&r.c, leaf_of(OK_PAGE)) & (RMW_U | RMW_M)) == (RMW_U | RMW_M));
    assert((gl(&r.c, leaf_of(WP_PAGE)) & (RMW_U | RMW_M)) == RMW_U);
    pl(&r.c, leaf_of(WP_PAGE), WP_PHYS | 1u); /* drop WP */
    mc68k_execute(r.cpu, 1);
    mc68k_execute(r.cpu, 1);
    mc68k_execute(r.cpu, 1); /* PFLUSHA, RTE, CAS2 */
    assert((r.cpu->sr & 0x4) && gl(&r.c, OK_PHYS) == 0xaaaaaaaa &&
           gl(&r.c, WP_PHYS) == 0xbbbbbbbb);
    mc68k_destroy(r.cpu);
}

int main(void) {
    ctx_t c = {.fail_read_at = UINT32_MAX};
    mc68k_bus_t bus = {rb, rw, rl, wb, ww, wl, NULL, NULL, NULL, NULL, &c};
    pl(&c, 0, 0x9000);
    pl(&c, 4, 0x0100);
    mc68k_state_t *cpu = mc68k_create_model(&bus, MC68K_MODEL_68040);
    assert(cpu);
    mc68k_reset(cpu);
    const uint32_t logical = 0x12345678;
    const mc68k_access_t read_super = {.fc = 5, .read = true, .size = 4};
    const mc68k_access_t read_user = {.fc = 1, .read = true, .size = 4};
    const mc68k_access_t read_instruction = {
        .fc = 6, .read = true, .size = 2, .instruction = true};
    cpu->mmu040_tc = 0x8000;
    cpu->mmu040_srp = cpu->mmu040_urp = ROOT;
    map_page(&c, logical, 12, 0xa000, 0);

    /* Debug translation is read-only: it neither fills an ATC entry nor sets U. */
    uint32_t leaf = PAGE + (((logical >> 12) & 0x3f) * 4u);
    mc68k_debug_translate_result_t debug;
    if (mc68k_debug_translate(cpu, logical, 5, 1, &debug) != 0)
        abort();
    assert(debug.physical == 0xa678 && debug.mmusr == 0 && !cpu->atc040_d[0].valid);
    assert((gl(&c, leaf) & 8u) == 0);

    mc68k_translate_result_t r =
        cpu_mmu040_translate_result(cpu, logical, &read_super, MC68K_TRANSLATE_EXECUTE);
    assert(r.ok && r.source == MC68K_TRANSLATE_WALK && r.physical == 0xa678);
    assert((gl(&c, leaf) & 8u) != 0);
    pl(&c, leaf, 0xb001); /* ATC still has the prior translation. */
    r = cpu_mmu040_translate_result(cpu, logical, &read_super, MC68K_TRANSLATE_EXECUTE);
    assert(r.ok && r.source == MC68K_TRANSLATE_ATC && r.physical == 0xa678);
    cpu->a[0] = logical;
    assert(cpu->op_table[0xf508](cpu, 0xf508) == 21); /* Table 10-5: PFLUSH (A0) */
    r = cpu_mmu040_translate_result(cpu, logical, &read_super, MC68K_TRANSLATE_EXECUTE);
    assert(r.ok && r.source == MC68K_TRANSLATE_WALK && r.physical == 0xb678);

    /* URP and SRP select independently using the FC supervisor bit. */
    unsigned ri = logical >> 25, pi = (logical >> 18) & 0x7f, pgi = (logical >> 12) & 0x3f;
    cpu->mmu040_urp = 0x8000;
    pl(&c, 0x8000 + ri * 4u, 0x7002);
    pl(&c, 0x7000 + pi * 4u, 0x7802);
    pl(&c, 0x7800 + pgi * 4u, 0xd001);
    clear_atc(cpu);
    r = cpu_mmu040_translate_result(cpu, logical, &read_user, MC68K_TRANSLATE_DEBUG);
    assert(r.ok && r.physical == 0xd678);
    cpu->mmu040_urp = ROOT;

    /* An explicit FC reads the supervisor space even while SR is in user mode. */
    uint8_t dbg[4] = {0};
    mc68k_set_peek_hooks(cpu, pk, NULL, NULL, &c);
    pl(&c, 0xd678 & ~3u, 0x01020304);
    pl(&c, 0xb678 & ~3u, 0x0a0b0c0d); /* the supervisor leaf was remapped to 0xb000 above */
    cpu->sr = 0;
    clear_atc(cpu);
    cpu->mmu040_urp = 0x8000;
    if (mc68k_debug_read_bytes(cpu, logical, dbg, 4) != 0)
        abort();
    assert(dbg[0] == 0x01);
    if (mc68k_debug_read_bytes_fc(cpu, logical, dbg, 4, 6) != 0)
        abort();
    assert(dbg[0] == 0x0a);
    if (mc68k_debug_read_bytes_fc(cpu, logical, dbg, 4, 8) != -1)
        abort();
    cpu->mmu040_urp = ROOT;
    cpu->sr = SR_S;

    /* PTEST flushes only the D-ATC selected by DFC and reloads it from tables. */
    cpu->dfc = 5;
    pl(&c, leaf, 0xc001);
    cpu->atc040_i[0] = (mc68k_atc_entry_t){.logical_page = logical & ~0xfffu,
                                           .physical_page = 0xd000,
                                           .fc = 6,
                                           .page_shift = 12,
                                           .valid = true};
    assert(cpu->op_table[0xf568](cpu, 0xf568) == 25); /* PTESTR (A0) */
    assert((cpu->mmu040_mmusr & 1u) != 0 && !cpu->atc040_i[0].valid == false);
    r = cpu_mmu040_translate_result(cpu, logical, &read_super, MC68K_TRANSLATE_EXECUTE);
    assert(r.ok && r.physical == 0xc678);
    pl(&c, leaf, 0xc001);
    assert(cpu->op_table[0xf548](cpu, 0xf548) == 25); /* PTESTW (A0) */
    assert((gl(&c, leaf) & 0x18u) == 0x18u && (cpu->mmu040_mmusr & 0x11u) == 0x11u);
    clear_atc(cpu);
    c.fail_read_at = ROOT + (logical >> 25) * 4u;
    assert(cpu->op_table[0xf568](cpu, 0xf568) == 25 && cpu->mmu040_mmusr == 0x800);
    c.fail_read_at = UINT32_MAX;

    /* Global entries survive PFLUSHN/PFLUSHAN, but not PFLUSHA. */
    clear_atc(cpu);
    cpu->atc040_d[0] = (mc68k_atc_entry_t){.logical_page = logical & ~0xfffu,
                                           .fc = 5,
                                           .page_shift = 12,
                                           .valid = true,
                                           .global = true};
    cpu->atc040_d[1] = (mc68k_atc_entry_t){
        .logical_page = (logical + 0x1000) & ~0xfffu, .fc = 5, .page_shift = 12, .valid = true};
    assert(cpu->op_table[0xf510](cpu, 0xf510) == 53); /* Table 10-5: PFLUSHAN */
    assert(cpu->atc040_d[0].valid && !cpu->atc040_d[1].valid);
    assert(cpu->op_table[0xf518](cpu, 0xf518) == 21); /* Table 10-5: PFLUSHA */
    assert(!cpu->atc040_d[0].valid);

    /* ITT and DTT are separate.  A user data hit must not cover instruction access. */
    clear_atc(cpu);
    cpu->mmu040_dtt0 = 0x12008000;
    cpu->mmu040_itt0 = 0;
    r = cpu_mmu040_translate_result(cpu, logical, &read_user, MC68K_TRANSLATE_DEBUG);
    assert(r.ok && r.source == MC68K_TRANSLATE_TT);
    r = cpu_mmu040_translate_result(cpu, logical, &read_instruction, MC68K_TRANSLATE_DEBUG);
    assert(r.ok && r.source == MC68K_TRANSLATE_WALK && r.physical == 0xc678);
    cpu->mmu040_dtt0 = 0;

    /* Supervisor protection is checked against the supplied FC, not CPU SR. */
    clear_atc(cpu);
    pl(&c, leaf, 0xc081);
    r = cpu_mmu040_translate_result(cpu, logical, &read_user, MC68K_TRANSLATE_DEBUG);
    assert(!r.ok && r.fault == MMU_FAULT_SUPERVISOR);

    /* The 8 KiB table form uses its distinct page index and offset. */
    clear_atc(cpu);
    cpu->mmu040_tc = 0xc000;
    const uint32_t logical8 = 0x22345678;
    map_page(&c, logical8, 13, 0xe000, 0);
    r = cpu_mmu040_translate_result(cpu, logical8, &read_super, MC68K_TRANSLATE_DEBUG);
    assert(r.ok && r.physical == 0xf678);

    /* MOVEC exposes only the MC68040 control-register bank and masks TTR reserved bits. */
    cpu->mmu040_tc = 0;
    cpu->d[0] = 0xffffffff;
    pw(&c, 0x100, 0x4e7b);
    pw(&c, 0x102, 0x0004);
    cpu->pc = 0x102;
    cpu->op_table[0x4e7b](cpu, 0x4e7b);
    assert(cpu->pc == 0x104 && cpu->exec_state == CPU_NORMAL);
    mc68k_mmu040_regs_t regs;
    mc68k_get_mmu040_regs(cpu, &regs);
    assert(regs.itt0 == 0xffffe364);
    pw(&c, 0x104, 0x4e7a);
    pw(&c, 0x106, 0x0004);
    cpu->pc = 0x104;
    cpu->d[0] = 0;
    cpu->pc = 0x106;
    cpu->op_table[0x4e7a](cpu, 0x4e7a);
    assert(cpu->d[0] == 0xffffe364);
    cpu->d[0] = 0x12345fff;
    pw(&c, 0x108, 0x0806);
    cpu->pc = 0x108;
    cpu->op_table[0x4e7b](cpu, 0x4e7b);
    cpu->d[0] = 0xffffffff;
    pw(&c, 0x10a, 0x0003);
    cpu->pc = 0x10a;
    cpu->op_table[0x4e7b](cpu, 0x4e7b);
    mc68k_get_mmu040_regs(cpu, &regs);
    assert(regs.urp == 0x12345e00 && regs.tc == 0xc000);

    mc68k_destroy(cpu);
    test_data_fault_restart();
    test_wb2_not_deferred();
    test_rmw_read_is_write();
    test_wb2_rte_conversion();
    test_wb2_fpu_post_exception();
    test_wb2_escalation();
    test_wb2_flag_consumed();
    test_wb2_reset();
    test_restart_after_stack_switch();
    test_restart_register_rewind();
    test_restart_origin();
    test_cas2_write_fault_restart();
    return 0;
}
