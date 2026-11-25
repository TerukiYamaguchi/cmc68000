/* : MC68040 table search and the data cache (MC68040UM §3.2.5, §4.3.3, Table 3-1).
 * Design: 現行仕様
 *
 * Every case builds a 3-level walk with the leaf descriptor in the D-cache and/or on the bus,
 * runs cpu_mmu040_translate_result() and checks the bus log, the cache lines and the bus
 * backing store.  Failures are counted rather than aborting so that one run lists every case
 * that still reproduces the issue. */
#include "mc68000/cpu.h"
#include "cpu/cpu_internal.h"
#include <stdio.h>
#include <string.h>

enum {
    RAM_SIZE = 0x10000,
    ROOT = 0x2000,
    POINTER = 0x4000,
    PAGE = 0x6000,
    CODE = 0x400,
    SSP0 = 0xf000,
    LOGICAL = 0x12344000,
    PHYS_OLD = 0x8000,
    PHYS_NEW = 0xa000,
    INDIRECT_DESC = 0x6100,
    ROOT_ENTRY = ROOT + (LOGICAL >> 25) * 4,
    POINTER_ENTRY = POINTER + ((LOGICAL >> 18) & 0x7f) * 4,
    LEAF = PAGE + ((LOGICAL >> 12) & 0x3f) * 4,
    LOG_MAX = 64,
    LOGICAL2 = LOGICAL + 0x2000,
    LEAF2 = PAGE + ((LOGICAL2 >> 12) & 0x3f) * 4 /* same line as LEAF, offset 8 */
};
#define DESC_TYPE_PAGE     UINT32_C(1)
#define DESC_TYPE_POINTER  UINT32_C(2)
#define DESC_TYPE_INDIRECT UINT32_C(2)
#define DESC_WP            UINT32_C(0x04)
#define DESC_U             UINT32_C(0x08)
#define DESC_M             UINT32_C(0x10)
#define DESC_S             UINT32_C(0x80)
#define MMUSR_B_BIT        UINT32_C(0x800)
#define MMUSR_W_BIT        UINT32_C(0x004)
#define MMUSR_R_BIT        UINT32_C(0x001)
#define TC_E_BIT           UINT16_C(0x8000)
#define OP_CPUSHA_DC       0xf478u

typedef struct {
    char op; /* 'R' or 'W' */
    uint32_t addr;
    bool rmw;
    bool timed; /* access_timing saw this cycle at this address and direction */
    unsigned size;
    uint8_t fc;
} log_t;

typedef struct {
    uint8_t m[RAM_SIZE];
    uint32_t fail_read_at, fail_write_at;
    bool poison; /* a failing read stores garbage in its result */
    bool mirror; /* addresses above RAM_SIZE alias RAM (wide-address cases); the log keeps the
                    full address */
    log_t log[LOG_MAX];
    unsigned nlog;
} ctx_t;

static unsigned failures;
static const char *current_case = "";
#define CHECK(cond)                                                                           \
    do {                                                                                      \
        if (!(cond)) {                                                                        \
            fprintf(stderr, "FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, current_case, #cond); \
            failures++;                                                                       \
        }                                                                                     \
    } while (0)

static void log_add(ctx_t *c, char op, uint32_t addr) {
    if (c->nlog < LOG_MAX)
        c->log[c->nlog++] = (log_t){.op = op, .addr = addr};
}
static bool rb(void *p, uint32_t a, uint8_t *v) {
    *v = ((ctx_t *)p)->m[a % RAM_SIZE];
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
    log_add(c, 'R', a);
    if (a == c->fail_read_at) {
        if (c->poison)
            *v = UINT32_C(0xfffffff7);
        return false;
    }
    if (c->mirror)
        a &= RAM_SIZE - 1u;
    if (a + 3 >= RAM_SIZE)
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
    log_add(c, 'W', a);
    if (a == c->fail_write_at)
        return false;
    if (c->mirror)
        a &= RAM_SIZE - 1u;
    if (a + 3 >= RAM_SIZE)
        return false;
    c->m[a] = (uint8_t)(v >> 24);
    c->m[a + 1] = (uint8_t)(v >> 16);
    c->m[a + 2] = (uint8_t)(v >> 8);
    c->m[a + 3] = (uint8_t)v;
    return true;
}
/* access_timing runs right after the bus cycle it describes: it records whether the core marked
 * that cycle as part of a locked read-modify-write, and its size and function code. */
static void timing(void *p, const mc68k_bus_access_t *a, mc68k_bus_timing_t *out) {
    ctx_t *c = p;
    out->wait_cycles = 1; /* every accounted bus cycle adds 1 to cpu->bus_wait_accum */
    for (unsigned i = c->nlog; i-- > 0;)
        if (c->log[i].addr == a->physical && c->log[i].op == (a->read ? 'R' : 'W')) {
            c->log[i].rmw = a->rmw;
            c->log[i].timed = true;
            c->log[i].size = a->size;
            c->log[i].fc = a->fc;
            return;
        }
}

static void pl(ctx_t *c, uint32_t a, uint32_t v) {
    if (!wl(c, a, v)) {
        fprintf(stderr, "setup\n");
        failures++;
    }
}
static uint32_t gl(ctx_t *c, uint32_t a) {
    a &= RAM_SIZE - 1u;
    return ((uint32_t)c->m[a] << 24) | ((uint32_t)c->m[a + 1] << 16) |
           ((uint32_t)c->m[a + 2] << 8) | c->m[a + 3];
}

typedef struct {
    ctx_t c;
    mc68k_state_t *cpu;
} rig_t;

static const mc68k_access_t READ_SUPER = {.fc = 5, .read = true, .size = 4};
static const mc68k_access_t WRITE_SUPER = {.fc = 5, .read = false, .size = 4};
static const mc68k_access_t WRITE_USER = {.fc = 1, .read = false, .size = 4};
static const mc68k_access_t READ_USER = {.fc = 1, .read = true, .size = 4};
static const mc68k_access_t READ_INSTR = {
    .fc = 6, .read = true, .size = 4, .instruction = true};

static uint32_t leaf_desc(uint32_t physical, uint32_t flags) {
    return physical | flags | DESC_TYPE_PAGE;
}

/* root/pointer descriptors carry the given U/WP flags; the leaf is written as given. */
static void map(rig_t *r, uint32_t root_flags, uint32_t pointer_flags, uint32_t leaf) {
    pl(&r->c, ROOT_ENTRY, POINTER | DESC_TYPE_POINTER | root_flags);
    pl(&r->c, POINTER_ENTRY, PAGE | DESC_TYPE_POINTER | pointer_flags);
    pl(&r->c, LEAF, leaf);
}

static void rig_init(rig_t *r) {
    memset(r, 0, sizeof(*r));
    r->c.fail_read_at = r->c.fail_write_at = UINT32_MAX;
    mc68k_bus_t bus = {.read_byte = rb,
                       .read_word = rw,
                       .read_long = rl,
                       .write_byte = wb,
                       .write_word = ww,
                       .write_long = wl,
                       .ctx = &r->c,
                       .access_timing = timing};
    pl(&r->c, 0, SSP0);
    pl(&r->c, 4, CODE);
    r->cpu = mc68k_create_model(&bus, MC68K_MODEL_68040);
    if (!r->cpu) {
        fprintf(stderr, "create\n");
        failures++;
        return;
    }
    mc68k_reset(r->cpu);
    r->cpu->sr = SR_S;
    r->cpu->cacr = MC68K_CACR_040_DE;
    r->cpu->mmu040_tc = TC_E_BIT;
    r->cpu->mmu040_srp = r->cpu->mmu040_urp = ROOT;
    r->c.nlog = 0;
}

static mc68k_040_cache_line_t *find_line(rig_t *r, uint32_t addr) {
    uint32_t base = addr & ~UINT32_C(15);
    unsigned set = (base >> 4) & (MC68K_040_CACHE_SETS - 1u);
    for (unsigned way = 0; way < MC68K_040_CACHE_WAYS; way++)
        if (r->cpu->dcache040[set][way].valid &&
            r->cpu->dcache040[set][way].physical_line == base)
            return &r->cpu->dcache040[set][way];
    return NULL;
}
static unsigned valid_dcache_lines(rig_t *r) {
    unsigned n = 0;
    for (unsigned s = 0; s < MC68K_040_CACHE_SETS; s++)
        for (unsigned w = 0; w < MC68K_040_CACHE_WAYS; w++)
            n += r->cpu->dcache040[s][w].valid;
    return n;
}
/* Put a longword into the D-cache the way a normal CPU access would have: the line is filled
 * from the bus, then the longword is replaced (and marked dirty if requested). */
static void cache_set(rig_t *r, uint32_t addr, uint32_t value, bool dirty) {
    mc68k_040_cache_line_t *line = find_line(r, addr);
    uint32_t base = addr & ~UINT32_C(15);
    if (!line) {
        line = &r->cpu->dcache040[(base >> 4) & (MC68K_040_CACHE_SETS - 1u)][0];
        *line = (mc68k_040_cache_line_t){.physical_line = base, .valid = true, .loaded = true};
        for (unsigned i = 0; i < 4; i++)
            line->data[i] = gl(&r->c, base + i * 4u);
    }
    unsigned index = (addr & 15u) >> 2;
    line->data[index] = value;
    if (dirty)
        line->dirty |= (uint8_t)(1u << index);
}
static unsigned atc_valid_entries(const rig_t *r) {
    unsigned n = 0;
    for (unsigned i = 0; i < MC68K_ATC_ENTRIES; i++)
        n += r->cpu->atc040_d[i].valid;
    return n;
}
static void cpusha_dc(rig_t *r) {
    r->cpu->op_table[OP_CPUSHA_DC](r->cpu, OP_CPUSHA_DC);
}

static const log_t *last_log(const rig_t *r, char op, uint32_t addr) {
    for (unsigned i = r->c.nlog; i-- > 0;)
        if (r->c.log[i].op == op && r->c.log[i].addr == addr)
            return &r->c.log[i];
    return NULL;
}
static unsigned count_log(const rig_t *r, char op, uint32_t addr) {
    unsigned n = 0;
    for (unsigned i = 0; i < r->c.nlog; i++)
        n += r->c.log[i].op == op && r->c.log[i].addr == addr;
    return n;
}
static unsigned count_writes(const rig_t *r) {
    unsigned n = 0;
    for (unsigned i = 0; i < r->c.nlog; i++)
        n += r->c.log[i].op == 'W';
    return n;
}

static mc68k_translate_result_t
walk_at(rig_t *r, uint32_t logical, const mc68k_access_t *access, mc68k_translate_mode_t mode) {
    r->c.nlog = 0;
    r->cpu->bus_wait_accum = 0;
    return cpu_mmu040_translate_result(r->cpu, logical, access, mode);
}
static mc68k_translate_result_t walk(rig_t *r, const mc68k_access_t *access,
                                     mc68k_translate_mode_t mode) {
    return walk_at(r, LOGICAL, access, mode);
}
/* Every bus cycle of a non-debug walk is accounted once, and a D-cache hit makes no bus cycle.
 * Each accounted cycle is a supervisor-data longword access of the direction and address of the
 * bus cycle it describes (Review C-B18). */
static void check_accounting(const rig_t *r) {
    CHECK(r->cpu->bus_wait_accum == (int)r->c.nlog);
    for (unsigned i = 0; i < r->c.nlog; i++)
        CHECK(r->c.log[i].timed && r->c.log[i].size == 4 && r->c.log[i].fc == 5);
}
/* Only the locked read-modify-write of a U/M update carries rmw; the other cycles must not. */
static unsigned count_rmw(const rig_t *r) {
    unsigned n = 0;
    for (unsigned i = 0; i < r->c.nlog; i++)
        n += r->c.log[i].rmw;
    return n;
}

/* §4 table read hit: a dirty descriptor that exists only in the D-cache is used. */
static void test_table_read_hit(void) {
    current_case = "table read hit";
    rig_t r;
    rig_init(&r);
    map(&r, DESC_U, DESC_U, leaf_desc(PHYS_OLD, DESC_U));
    cache_set(&r, LEAF, leaf_desc(PHYS_NEW, DESC_U), true);
    mc68k_translate_result_t t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(t.ok && t.physical == PHYS_NEW);
    CHECK(count_log(&r, 'R', LEAF) == 0);
    check_accounting(&r);
    CHECK(find_line(&r, LEAF) && find_line(&r, LEAF)->dirty != 0);
    CHECK(valid_dcache_lines(&r) == 1);
    CHECK(gl(&r.c, LEAF) == leaf_desc(PHYS_OLD, DESC_U));
    mc68k_destroy(r.cpu);
}

/* §4 table read miss: bus read, and no allocation. */
static void test_table_read_miss(void) {
    current_case = "table read miss";
    rig_t r;
    rig_init(&r);
    map(&r, DESC_U, DESC_U, leaf_desc(PHYS_OLD, DESC_U));
    mc68k_translate_result_t t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(t.ok && t.physical == PHYS_OLD);
    CHECK(count_log(&r, 'R', LEAF) == 1);
    CHECK(valid_dcache_lines(&r) == 0);
    CHECK(count_rmw(&r) == 0);
    check_accounting(&r);
    mc68k_destroy(r.cpu);
}

/* §4 U update RMW: dirty line is pushed and invalidated, then the bus RMW sets U. */
static void test_u_update_rmw_dirty(void) {
    current_case = "U update RMW on a dirty line";
    rig_t r;
    rig_init(&r);
    map(&r, DESC_U, DESC_U, leaf_desc(PHYS_OLD, 0));
    cache_set(&r, LEAF, leaf_desc(PHYS_NEW, 0), true);
    mc68k_translate_result_t t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(t.ok && t.physical == PHYS_NEW);
    CHECK(gl(&r.c, LEAF) == leaf_desc(PHYS_NEW, DESC_U));
    CHECK(find_line(&r, LEAF) == NULL);
    const log_t *rd = last_log(&r, 'R', LEAF), *wr = last_log(&r, 'W', LEAF);
    CHECK(rd && rd->rmw && wr && wr->rmw);
    check_accounting(&r);
    cpusha_dc(&r);
    CHECK(gl(&r.c, LEAF) == leaf_desc(PHYS_NEW, DESC_U));
    mc68k_destroy(r.cpu);
}

/* §4 clean line: RMW invalidates it without a push, so no stale copy remains. */
static void test_u_update_rmw_clean(void) {
    current_case = "U update RMW on a clean line";
    rig_t r;
    rig_init(&r);
    map(&r, DESC_U, DESC_U, leaf_desc(PHYS_OLD, 0));
    cache_set(&r, LEAF, leaf_desc(PHYS_OLD, 0), false);
    mc68k_translate_result_t t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(t.ok && t.physical == PHYS_OLD);
    CHECK(find_line(&r, LEAF) == NULL);
    CHECK(count_log(&r, 'W', LEAF) == 1);
    CHECK(gl(&r.c, LEAF) == leaf_desc(PHYS_OLD, DESC_U));
    mc68k_destroy(r.cpu);
}

/* Review C-B23: the locked RMW works on the bus value, not on a stale clean line: the result
 * and the translation come from the bus descriptor with U set. */
static void test_u_update_rmw_stale_clean(void) {
    current_case = "U update RMW on a stale clean line";
    rig_t r;
    rig_init(&r);
    map(&r, DESC_U, DESC_U, leaf_desc(PHYS_OLD, 0));
    cache_set(&r, LEAF, leaf_desc(PHYS_NEW, 0), false);
    mc68k_translate_result_t t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(t.ok &&
          t.physical == PHYS_OLD); /* the RMW result replaces the descriptor that was read */
    CHECK(gl(&r.c, LEAF) == leaf_desc(PHYS_OLD, DESC_U));
    mc68k_destroy(r.cpu);
}

/* §4 leaf write: the simple write hits a dirty line, updates it and the bus, keeps the dirty
 * mask. */
static void test_simple_write_hit(void) {
    current_case = "simple write hits a dirty line";
    rig_t r;
    rig_init(&r);
    map(&r, DESC_U, DESC_U, leaf_desc(PHYS_OLD, DESC_U));
    cache_set(&r, LEAF, leaf_desc(PHYS_NEW, DESC_U), true);
    uint8_t dirty = find_line(&r, LEAF)->dirty;
    mc68k_translate_result_t t = walk(&r, &WRITE_SUPER, MC68K_TRANSLATE_EXECUTE);
    const uint32_t updated = leaf_desc(PHYS_NEW, DESC_U | DESC_M);
    CHECK(t.ok && t.physical == PHYS_NEW);
    mc68k_040_cache_line_t *line = find_line(&r, LEAF);
    CHECK(line && line->dirty == dirty && line->data[(LEAF & 15u) >> 2] == updated);
    CHECK(gl(&r.c, LEAF) == updated);
    CHECK(count_rmw(&r) == 0);
    check_accounting(&r);
    cpusha_dc(&r);
    CHECK(gl(&r.c, LEAF) == updated);
    mc68k_destroy(r.cpu);
}

/* §4 WP leaf U update (Table 3-1, WP=1, U=0): locked RMW sets U, M is kept, and the walk
 * still completes so that a later read of the page hits the ATC. */
static void test_wp_leaf_u_update(void) {
    current_case = "WP leaf U update";
    rig_t r;
    rig_init(&r);
    map(&r, DESC_U, DESC_U, leaf_desc(PHYS_OLD, DESC_WP));
    mc68k_translate_result_t t = walk(&r, &WRITE_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(!t.ok && t.fault == MMU_FAULT_WRITE_PROTECT && t.write_protect);
    CHECK(gl(&r.c, LEAF) == leaf_desc(PHYS_OLD, DESC_WP | DESC_U));
    const log_t *wr = last_log(&r, 'W', LEAF);
    CHECK(wr && wr->rmw);
    t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(t.ok && t.physical == PHYS_OLD);
    mc68k_destroy(r.cpu);
}

/* §4 parent WP: the accumulated WP comes from the pointer descriptor; the leaf U is still set.
 */
static void test_parent_wp_leaf_u_update(void) {
    current_case = "parent WP leaf U update";
    rig_t r;
    rig_init(&r);
    map(&r, DESC_U, DESC_U | DESC_WP, leaf_desc(PHYS_OLD, 0));
    mc68k_translate_result_t t = walk(&r, &WRITE_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(t.write_protect);
    CHECK(gl(&r.c, LEAF) == leaf_desc(PHYS_OLD, DESC_U));
    mc68k_destroy(r.cpu);
}

/* §4 RMW bus fault: a failed push is reported as a table bus error at the pushed longword,
 * without the exception side effects of push_line() (PTEST must not raise an exception). */
static void test_push_failure_ptest(void) {
    current_case = "push failure under PTEST";
    rig_t r;
    rig_init(&r);
    map(&r, DESC_U, DESC_U, leaf_desc(PHYS_OLD, 0));
    cache_set(&r, LEAF, leaf_desc(PHYS_OLD, 0), false);
    cache_set(&r, LEAF + 4, 0x11111111, true);
    r.c.fail_write_at = LEAF + 4;
    mc68k_translate_result_t t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_PLOAD);
    CHECK(!t.ok && t.fault == MMU_FAULT_TABLE_BUS_ERROR);
    CHECK((t.mmusr & MMUSR_B_BIT) != 0);
    CHECK(t.table_bus_error_physical == LEAF + 4);
    CHECK(find_line(&r, LEAF) && find_line(&r, LEAF)->dirty != 0);
    CHECK(!r.cpu->fault040.valid && r.cpu->exec_state == CPU_NORMAL);
    mc68k_destroy(r.cpu);
}

static void test_rmw_write_failure(void) {
    current_case = "RMW write failure";
    rig_t r;
    rig_init(&r);
    map(&r, DESC_U, DESC_U, leaf_desc(PHYS_OLD, 0));
    r.c.fail_write_at = LEAF;
    mc68k_translate_result_t t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(!t.ok && t.fault == MMU_FAULT_TABLE_BUS_ERROR);
    CHECK((t.mmusr & MMUSR_B_BIT) != 0 && t.table_bus_error_physical == LEAF);
    mc68k_destroy(r.cpu);
}

/* §4 debug walk: sees the dirty line, changes neither U/M, the cache nor the bus. */
static void test_debug_walk(void) {
    current_case = "debug walk";
    rig_t r;
    rig_init(&r);
    map(&r, DESC_U, DESC_U, leaf_desc(PHYS_OLD, 0));
    cache_set(&r, LEAF, leaf_desc(PHYS_NEW, 0), true);
    uint8_t dirty = find_line(&r, LEAF)->dirty;
    mc68k_translate_result_t t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_DEBUG);
    CHECK(t.ok && t.physical == PHYS_NEW);
    CHECK(count_writes(&r) == 0 && r.cpu->bus_wait_accum == 0); /* no bus cycle is accounted */
    CHECK(gl(&r.c, LEAF) == leaf_desc(PHYS_OLD, 0));
    mc68k_040_cache_line_t *line = find_line(&r, LEAF);
    CHECK(line && line->dirty == dirty &&
          line->data[(LEAF & 15u) >> 2] == leaf_desc(PHYS_NEW, 0));
    mc68k_destroy(r.cpu);

    /* The result is the descriptor as the update would leave it, without writing it: a read
     * sets no M, a write sets M (design §2.2; mutants of the nd value). */
    for (unsigned write = 0; write < 2; write++) {
        rig_init(&r);
        map(&r, DESC_U, DESC_U, leaf_desc(PHYS_OLD, 0));
        t = walk(&r, write ? &WRITE_SUPER : &READ_SUPER, MC68K_TRANSLATE_DEBUG);
        CHECK(t.ok && t.physical == PHYS_OLD && t.modified == (write != 0));
        CHECK(((t.mmusr & DESC_M) != 0) == (write != 0));
        CHECK(count_writes(&r) == 0 && gl(&r.c, LEAF) == leaf_desc(PHYS_OLD, 0));
        mc68k_destroy(r.cpu);
    }
}

/* A valid line whose data was never loaded holds no descriptor: a table read goes to the bus
 * and a table write leaves the line's data alone. */
static void test_unloaded_line(void) {
    current_case = "unloaded line";
    const unsigned slot = (LEAF & 15u) >> 2;
    for (unsigned write = 0; write < 2; write++) {
        rig_t r;
        rig_init(&r);
        map(&r, DESC_U, DESC_U, leaf_desc(PHYS_OLD, DESC_U));
        uint32_t base = LEAF & ~UINT32_C(15);
        mc68k_040_cache_line_t *line =
            &r.cpu->dcache040[(base >> 4) & (MC68K_040_CACHE_SETS - 1u)][0];
        *line = (mc68k_040_cache_line_t){.physical_line = base, .valid = true, .loaded = false};
        for (unsigned i = 0; i < 4; i++)
            line->data[i] = UINT32_C(0xdeadbeef);
        mc68k_translate_result_t t =
            walk(&r, write ? &WRITE_SUPER : &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
        CHECK(t.ok && t.physical == PHYS_OLD);
        CHECK(count_log(&r, 'R', LEAF) == 1);
        CHECK(line->valid && !line->loaded && line->data[slot] == UINT32_C(0xdeadbeef));
        CHECK(gl(&r.c, LEAF) == leaf_desc(PHYS_OLD, DESC_U | (write ? DESC_M : 0)));
        mc68k_destroy(r.cpu);
    }
}

/* design §2.2: Fig 3-10 defers the U write of a pointer descriptor until the next level has
 * been fetched. */
static void test_u_write_order(void) {
    current_case = "root/pointer U write order";
    rig_t r;
    rig_init(&r);
    map(&r, 0, 0, leaf_desc(PHYS_OLD, DESC_U));
    mc68k_translate_result_t t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(t.ok);
    static const log_t expected[] = {
        {'R', ROOT_ENTRY, false}, {'R', POINTER_ENTRY, false}, {'W', ROOT_ENTRY, false},
        {'R', LEAF, false},       {'W', POINTER_ENTRY, false},
    };
    CHECK(r.c.nlog == sizeof(expected) / sizeof(expected[0]));
    for (unsigned i = 0; i < r.c.nlog && i < sizeof(expected) / sizeof(expected[0]); i++)
        CHECK(r.c.log[i].op == expected[i].op && r.c.log[i].addr == expected[i].addr &&
              r.c.log[i].rmw == expected[i].rmw);
    CHECK(gl(&r.c, ROOT_ENTRY) == (POINTER | DESC_TYPE_POINTER | DESC_U));
    CHECK(gl(&r.c, POINTER_ENTRY) == (PAGE | DESC_TYPE_POINTER | DESC_U));
    check_accounting(&r);
    mc68k_destroy(r.cpu);
}

/* #104 (040UM Fig 3-10 OTHERWISE): a table search that ends in a transfer error leaves an
 * R-clear ATC entry, and the retry aborts from it without a table search, for a read and for a
 * write. */
static void check_r_clear_entry(rig_t *r, const mc68k_access_t *access) {
    CHECK(atc_valid_entries(r) == 1);
    r->c.fail_read_at = r->c.fail_write_at = UINT32_MAX;
    for (unsigned i = 0; i < 2; i++) {
        const mc68k_access_t *retry = i ? &WRITE_SUPER : access;
        mc68k_translate_result_t t = walk(r, retry, MC68K_TRANSLATE_EXECUTE);
        CHECK(!t.ok && t.fault == MMU_FAULT_TABLE_BUS_ERROR && t.source == MC68K_TRANSLATE_ATC);
        CHECK(r->c.nlog == 0 && atc_valid_entries(r) == 1);
    }
}

/* design §2.2: a failing deferred U write reports the written descriptor; a failing read of the
 * next level still lets the scheduled U write out (Fig 3-10, Review C-B38) and is the failure
 * that is reported. */
static void test_u_write_faults(void) {
    current_case = "root/pointer U write faults";
    rig_t r;
    rig_init(&r);
    map(&r, 0, 0, leaf_desc(PHYS_OLD, DESC_U));
    r.c.fail_write_at = ROOT_ENTRY;
    mc68k_translate_result_t t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(!t.ok && t.fault == MMU_FAULT_TABLE_BUS_ERROR);
    CHECK((t.mmusr & MMUSR_B_BIT) != 0 && t.table_bus_error_physical == ROOT_ENTRY);
    CHECK(count_log(&r, 'R', LEAF) == 0);
    check_r_clear_entry(&r, &READ_SUPER); /* the U write failed */
    mc68k_destroy(r.cpu);

    rig_init(&r);
    map(&r, 0, 0, leaf_desc(PHYS_OLD, DESC_U));
    r.c.fail_read_at = POINTER_ENTRY;
    t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(!t.ok && t.fault == MMU_FAULT_TABLE_BUS_ERROR &&
          t.table_bus_error_physical == POINTER_ENTRY);
    CHECK(count_writes(&r) == 1 && count_log(&r, 'W', ROOT_ENTRY) == 1);
    CHECK(gl(&r.c, ROOT_ENTRY) == (POINTER | DESC_TYPE_POINTER | DESC_U));
    check_r_clear_entry(&r, &READ_SUPER); /* the pointer read failed */
    mc68k_destroy(r.cpu);

    rig_init(&r); /* the leaf read fails: the pointer's U is written */
    map(&r, DESC_U, 0, leaf_desc(PHYS_OLD, DESC_U));
    r.c.fail_read_at = LEAF;
    t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(!t.ok && t.fault == MMU_FAULT_TABLE_BUS_ERROR && t.table_bus_error_physical == LEAF);
    CHECK(count_writes(&r) == 1 &&
          gl(&r.c, POINTER_ENTRY) == (PAGE | DESC_TYPE_POINTER | DESC_U));
    check_r_clear_entry(&r, &READ_SUPER); /* the leaf read failed */
    mc68k_destroy(r.cpu);

    rig_init(&r); /* both the read and the U write fail: the read is reported */
    map(&r, 0, 0, leaf_desc(PHYS_OLD, DESC_U));
    r.c.fail_read_at = POINTER_ENTRY;
    r.c.fail_write_at = ROOT_ENTRY;
    t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(!t.ok && t.fault == MMU_FAULT_TABLE_BUS_ERROR &&
          t.table_bus_error_physical == POINTER_ENTRY);
    mc68k_destroy(r.cpu);

    /* Review C-B24: the pointer's U write follows the successful read of the next level even
     * when that descriptor turns out to be invalid. */
    rig_init(&r);
    map(&r, DESC_U, 0, 0);
    t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(!t.ok && t.fault == MMU_FAULT_INVALID);
    CHECK(count_log(&r, 'W', POINTER_ENTRY) == 1);
    CHECK(gl(&r.c, POINTER_ENTRY) == (PAGE | DESC_TYPE_POINTER | DESC_U));
    mc68k_destroy(r.cpu);

    /* Review C-B30: the same for the root when the pointer descriptor is invalid. */
    rig_init(&r);
    map(&r, 0, 0, leaf_desc(PHYS_OLD, DESC_U));
    pl(&r.c, POINTER_ENTRY, 0);
    t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(!t.ok && t.fault == MMU_FAULT_INVALID);
    CHECK(count_log(&r, 'W', ROOT_ENTRY) == 1 && count_writes(&r) == 1);
    CHECK(gl(&r.c, ROOT_ENTRY) == (POINTER | DESC_TYPE_POINTER | DESC_U));
    mc68k_destroy(r.cpu);

    /* Review C-B29: a user-mode walk (URP) updates root/pointer U as well. */
    rig_init(&r);
    map(&r, 0, 0, leaf_desc(PHYS_OLD, DESC_U));
    t = walk(&r, &READ_USER, MC68K_TRANSLATE_EXECUTE);
    CHECK(t.ok && count_writes(&r) == 2);
    CHECK(gl(&r.c, ROOT_ENTRY) == (POINTER | DESC_TYPE_POINTER | DESC_U));
    CHECK(gl(&r.c, POINTER_ENTRY) == (PAGE | DESC_TYPE_POINTER | DESC_U));
    mc68k_destroy(r.cpu);
}

/* Review C-B6..C-B8: the deferred U write is issued once, only for U=0, with U set, and never
 * in a debug walk. */
static void test_u_write_cases(void) {
    current_case = "root/pointer U write cases";
    rig_t r;
    rig_init(&r);
    map(&r, 0, DESC_U, leaf_desc(PHYS_OLD, DESC_U)); /* only the root needs U */
    mc68k_translate_result_t t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(t.ok);
    CHECK(count_writes(&r) == 1 && count_log(&r, 'W', ROOT_ENTRY) == 1);
    CHECK(gl(&r.c, ROOT_ENTRY) == (POINTER | DESC_TYPE_POINTER | DESC_U));
    CHECK(r.c.nlog == 4 && r.c.log[2].op == 'W' && r.c.log[3].op == 'R' &&
          r.c.log[3].addr == LEAF);
    check_accounting(&r);
    mc68k_destroy(r.cpu);

    rig_init(&r);
    map(&r, DESC_U, 0, leaf_desc(PHYS_OLD, DESC_U)); /* only the pointer needs U */
    t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(t.ok && count_writes(&r) == 1 && count_log(&r, 'W', POINTER_ENTRY) == 1);
    CHECK(gl(&r.c, POINTER_ENTRY) == (PAGE | DESC_TYPE_POINTER | DESC_U));
    check_accounting(&r);
    mc68k_destroy(r.cpu);

    rig_init(&r);
    map(&r, DESC_U, DESC_U, leaf_desc(PHYS_OLD, DESC_U));
    t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(t.ok && count_writes(&r) == 0);
    mc68k_destroy(r.cpu);

    rig_init(&r); /* PTEST (PLOAD) updates U like a normal walk */
    map(&r, 0, 0, leaf_desc(PHYS_OLD, DESC_U));
    t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_PLOAD);
    CHECK(t.ok && count_writes(&r) == 2);
    CHECK(gl(&r.c, ROOT_ENTRY) == (POINTER | DESC_TYPE_POINTER | DESC_U));
    CHECK(gl(&r.c, POINTER_ENTRY) == (PAGE | DESC_TYPE_POINTER | DESC_U));
    mc68k_destroy(r.cpu);

    rig_init(&r);
    map(&r, 0, 0, leaf_desc(PHYS_OLD, 0));
    t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_DEBUG);
    CHECK(t.ok && count_writes(&r) == 0);
    CHECK(gl(&r.c, ROOT_ENTRY) == (POINTER | DESC_TYPE_POINTER));
    CHECK(gl(&r.c, POINTER_ENTRY) == (PAGE | DESC_TYPE_POINTER));
    CHECK(gl(&r.c, LEAF) == leaf_desc(PHYS_OLD, 0));
    mc68k_destroy(r.cpu);

    /* Review C-B33: the deferred U write does not depend on the access type, WP, CACR.DE or the
     * fc. */
    current_case = "root/pointer U write, write access";
    rig_init(&r);
    map(&r, 0, 0, leaf_desc(PHYS_OLD, DESC_U | DESC_M));
    t = walk(&r, &WRITE_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(t.ok && count_writes(&r) == 2 && count_log(&r, 'W', ROOT_ENTRY) == 1 &&
          count_log(&r, 'W', POINTER_ENTRY) == 1);
    mc68k_destroy(r.cpu);

    current_case = "root/pointer U write, write below a WP pointer";
    rig_init(&r);
    map(&r, 0, DESC_WP, leaf_desc(PHYS_OLD, DESC_U | DESC_M));
    t = walk(&r, &WRITE_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(!t.ok && t.fault == MMU_FAULT_WRITE_PROTECT);
    CHECK(count_log(&r, 'W', ROOT_ENTRY) == 1 && count_log(&r, 'W', POINTER_ENTRY) == 1);
    mc68k_destroy(r.cpu);

    current_case = "root/pointer U write, CACR.DE=0";
    rig_init(&r);
    r.cpu->cacr = 0;
    map(&r, 0, 0, leaf_desc(PHYS_OLD, DESC_U));
    t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(t.ok && count_writes(&r) == 2);
    mc68k_destroy(r.cpu);

    current_case = "root/pointer U write, instruction fetch";
    rig_init(&r);
    map(&r, 0, 0, leaf_desc(PHYS_OLD, DESC_U));
    t = walk(&r, &READ_INSTR, MC68K_TRANSLATE_EXECUTE);
    CHECK(t.ok && count_writes(&r) == 2);
    mc68k_destroy(r.cpu);

    /* Review C-B36: the U write keeps the other bits of the descriptor, WP included. */
    current_case = "root/pointer U write keeps WP";
    rig_init(&r);
    map(&r, DESC_WP, DESC_WP, leaf_desc(PHYS_OLD, DESC_U));
    t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(t.ok && t.write_protect);
    CHECK(gl(&r.c, ROOT_ENTRY) == (POINTER | DESC_TYPE_POINTER | DESC_WP | DESC_U));
    CHECK(gl(&r.c, POINTER_ENTRY) == (PAGE | DESC_TYPE_POINTER | DESC_WP | DESC_U));
    mc68k_destroy(r.cpu);

    /* Review C-B32: below an indirect descriptor the pointer's U write still comes right after
     * the leaf-level read and before the read of the descriptor it points to. */
    current_case = "pointer U write before an indirect read";
    rig_init(&r);
    map(&r, DESC_U, 0, INDIRECT_DESC | DESC_TYPE_INDIRECT);
    pl(&r.c, INDIRECT_DESC, leaf_desc(PHYS_OLD, DESC_U));
    t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(t.ok && t.physical == PHYS_OLD);
    static const log_t indirect_order[] = {
        {.op = 'R', .addr = ROOT_ENTRY},    {.op = 'R', .addr = POINTER_ENTRY},
        {.op = 'R', .addr = LEAF},          {.op = 'W', .addr = POINTER_ENTRY},
        {.op = 'R', .addr = INDIRECT_DESC},
    };
    CHECK(r.c.nlog == sizeof(indirect_order) / sizeof(indirect_order[0]));
    for (unsigned i = 0; i < r.c.nlog && i < sizeof(indirect_order) / sizeof(indirect_order[0]);
         i++)
        CHECK(r.c.log[i].op == indirect_order[i].op &&
              r.c.log[i].addr == indirect_order[i].addr);
    CHECK(gl(&r.c, POINTER_ENTRY) == (PAGE | DESC_TYPE_POINTER | DESC_U));
    mc68k_destroy(r.cpu);
}

/* Review C-B39..C-B41: the U write that is let out when the next level's read fails exists only
 * if it was scheduled, does not depend on the walk mode, access, fc, CACR.DE or WP, and goes
 * through the table access (accounting, and the D-cache line when the descriptor is cached). */
static void test_u_write_on_read_failure(void) {
    current_case = "read failure with no U write scheduled";
    rig_t r;
    rig_init(&r);
    map(&r, DESC_U, DESC_U, leaf_desc(PHYS_OLD, DESC_U));
    r.c.fail_read_at = LEAF;
    mc68k_translate_result_t t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(!t.ok && t.fault == MMU_FAULT_TABLE_BUS_ERROR && t.table_bus_error_physical == LEAF);
    CHECK(count_writes(&r) == 0);
    mc68k_destroy(r.cpu);

    rig_init(&r);
    map(&r, 0, 0, leaf_desc(PHYS_OLD, DESC_U));
    r.c.fail_read_at = ROOT_ENTRY;
    t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(!t.ok && t.fault == MMU_FAULT_TABLE_BUS_ERROR &&
          t.table_bus_error_physical == ROOT_ENTRY);
    CHECK(count_writes(&r) == 0);
    mc68k_destroy(r.cpu);

    static const struct {
        const char *name;
        const mc68k_access_t *access;
        mc68k_translate_mode_t mode;
        uint32_t cacr;
        uint32_t root_flags;
    } cases[] = {
        {"read failure, PLOAD", &READ_SUPER, MC68K_TRANSLATE_PLOAD, MC68K_CACR_040_DE, DESC_U},
        {"read failure, write access", &WRITE_SUPER, MC68K_TRANSLATE_EXECUTE, MC68K_CACR_040_DE,
         DESC_U},
        {"read failure, user fc", &READ_USER, MC68K_TRANSLATE_EXECUTE, MC68K_CACR_040_DE,
         DESC_U},
        {"read failure, instruction fetch", &READ_INSTR, MC68K_TRANSLATE_EXECUTE,
         MC68K_CACR_040_DE, DESC_U},
        {"read failure, CACR.DE=0", &READ_SUPER, MC68K_TRANSLATE_EXECUTE, 0, DESC_U},
        {"read failure, WP root", &READ_SUPER, MC68K_TRANSLATE_EXECUTE, MC68K_CACR_040_DE,
         DESC_U | DESC_WP},
    };
    CHECK(sizeof(cases) / sizeof(cases[0]) == 6);
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        current_case = cases[i].name;
        rig_init(&r);
        r.cpu->cacr = cases[i].cacr;
        map(&r, cases[i].root_flags, 0,
            leaf_desc(PHYS_OLD, DESC_U)); /* the pointer's U is scheduled */
        r.c.fail_read_at = LEAF;
        t = walk(&r, cases[i].access, cases[i].mode);
        CHECK(!t.ok && t.fault == MMU_FAULT_TABLE_BUS_ERROR &&
              t.table_bus_error_physical == LEAF);
        CHECK(count_writes(&r) == 1 && count_log(&r, 'W', POINTER_ENTRY) == 1);
        CHECK(gl(&r.c, POINTER_ENTRY) == (PAGE | DESC_TYPE_POINTER | DESC_U));
        check_accounting(&r);
        mc68k_destroy(r.cpu);
    }

    current_case = "read failure, cached root";
    rig_init(&r);
    map(&r, 0, 0, leaf_desc(PHYS_OLD, DESC_U));
    cache_set(&r, ROOT_ENTRY, POINTER | DESC_TYPE_POINTER, false);
    r.c.fail_read_at = POINTER_ENTRY;
    t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(!t.ok && t.fault == MMU_FAULT_TABLE_BUS_ERROR &&
          t.table_bus_error_physical == POINTER_ENTRY);
    CHECK(count_log(&r, 'W', ROOT_ENTRY) == 1);
    const mc68k_040_cache_line_t *root = find_line(&r, ROOT_ENTRY);
    CHECK(root && root->dirty == 0 &&
          root->data[(ROOT_ENTRY & 15u) >> 2] == (POINTER | DESC_TYPE_POINTER | DESC_U));
    check_accounting(&r);
    mc68k_destroy(r.cpu);
}

/* Review C-B9/C-B11: every cell of Table 3-1 (access x WP x U x M): the update type, the final
 * descriptor, the translation result and, for a write to a protected page, the WP fault with
 * a resident walk result (MMUSR W=1, R=1, B=0). */
static void test_table_3_1_cells(void) {
    current_case = "Table 3-1 cells";
    typedef struct {
        bool write, wp;
        uint32_t u, m;
        unsigned writes;
        bool rmw;
    } cell_t;
    static const cell_t cells[] = {
        {false, false, 0, 0, 1, true},       {false, false, 0, DESC_M, 1, true},
        {false, false, DESC_U, 0, 0, false}, {false, false, DESC_U, DESC_M, 0, false},
        {false, true, 0, 0, 1, true}, /* a read ignores WP */
        {true, false, 0, 0, 1, false},       {true, false, 0, DESC_M, 1, true},
        {true, false, DESC_U, 0, 1, false},  {true, false, DESC_U, DESC_M, 0, false},
        {true, true, 0, 0, 1, true},         {true, true, 0, DESC_M, 1, true},
        {true, true, DESC_U, 0, 0, false},   {true, true, DESC_U, DESC_M, 0, false},
    };
    CHECK(sizeof(cells) / sizeof(cells[0]) ==
          13); /* 12 cells of Table 3-1 + a read with WP=1 */
    /* Table 3-1 has no fc column: the same cells hold for supervisor, user and instruction
     * reads. */
    static const mc68k_access_t *const reads[] = {&READ_SUPER, &READ_USER, &READ_INSTR};
    static const mc68k_access_t *const writes[] = {&WRITE_SUPER, &WRITE_USER};
    unsigned executed = 0;
    for (unsigned n = 0; n < 5 * sizeof(cells) / sizeof(cells[0]); n++) {
        const cell_t *c = &cells[n % (sizeof(cells) / sizeof(cells[0]))];
        const unsigned variant = n / (sizeof(cells) / sizeof(cells[0]));
        if (variant >= (c->write ? 2u : 3u))
            continue;
        const mc68k_access_t *access = c->write ? writes[variant] : reads[variant];
        executed++;
        rig_t r;
        rig_init(&r);
        map(&r, DESC_U, DESC_U, leaf_desc(PHYS_OLD, c->u | c->m | (c->wp ? DESC_WP : 0)));
        const bool protected_write = c->write && c->wp;
        const uint32_t final_u = DESC_U;
        const uint32_t final_m = c->write && !c->wp ? DESC_M : c->m;
        mc68k_translate_result_t t = walk(&r, access, MC68K_TRANSLATE_EXECUTE);
        CHECK(t.ok == !protected_write);
        CHECK(t.fault == (protected_write ? MMU_FAULT_WRITE_PROTECT : MMU_FAULT_NONE));
        CHECK(t.physical == PHYS_OLD && t.write_protect == c->wp);
        CHECK(t.modified == (final_m != 0));
        CHECK((t.mmusr & (MMUSR_B_BIT | MMUSR_R_BIT)) == MMUSR_R_BIT);
        CHECK(((t.mmusr & MMUSR_W_BIT) != 0) == c->wp);
        CHECK(gl(&r.c, LEAF) == leaf_desc(PHYS_OLD, final_u | final_m | (c->wp ? DESC_WP : 0)));
        CHECK(count_log(&r, 'W', LEAF) == c->writes && count_writes(&r) == c->writes);
        CHECK(count_log(&r, 'R', LEAF) == 1u + (c->writes && c->rmw));
        CHECK(count_rmw(&r) == (c->rmw ? 2u : 0u));
        check_accounting(&r);
        mc68k_destroy(r.cpu);
    }
    CHECK(executed == 5 * 3 + 8 * 2); /* 5 read cells x 3 fc, 8 write cells x 2 fc */
}

/* Review C-B1/C-B4: a descriptor that is not the first longword of its line. The read returns
 * the right word, the simple write updates only that word and leaves the dirty mask alone. */
static void test_cached_descriptor_offset(void) {
    current_case = "descriptor at a line offset";
    rig_t r;
    rig_init(&r);
    map(&r, DESC_U, DESC_U, leaf_desc(PHYS_OLD, DESC_U));
    pl(&r.c, LEAF2, leaf_desc(PHYS_OLD, DESC_U));
    cache_set(&r, LEAF, 0x11111111, false);
    cache_set(&r, LEAF + 4, 0x22222222, false);
    cache_set(&r, LEAF2, leaf_desc(PHYS_NEW, DESC_U), false);
    cache_set(&r, LEAF2 + 4, 0x44444444, false);
    mc68k_translate_result_t t = walk_at(&r, LOGICAL2, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(t.ok && t.physical == PHYS_NEW);
    CHECK(r.c.nlog == 2); /* only the root and pointer descriptors come from the bus */
    check_accounting(&r);
    mc68k_destroy(r.cpu);

    rig_init(&r);
    map(&r, DESC_U, DESC_U, leaf_desc(PHYS_OLD, DESC_U));
    pl(&r.c, LEAF2, leaf_desc(PHYS_OLD, DESC_U));
    cache_set(&r, LEAF, 0x11111111, false);
    cache_set(&r, LEAF + 4, 0x22222222, true); /* the only dirty longword */
    cache_set(&r, LEAF2, leaf_desc(PHYS_NEW, DESC_U), false);
    cache_set(&r, LEAF2 + 4, 0x44444444, false);
    t = walk_at(&r, LOGICAL2, &WRITE_SUPER, MC68K_TRANSLATE_EXECUTE);
    const uint32_t updated = leaf_desc(PHYS_NEW, DESC_U | DESC_M);
    CHECK(t.ok && t.physical == PHYS_NEW);
    mc68k_040_cache_line_t *line = find_line(&r, LEAF);
    CHECK(line && line->dirty == 0x2);
    CHECK(line && line->data[0] == 0x11111111 && line->data[1] == 0x22222222 &&
          line->data[2] == updated && line->data[3] == 0x44444444);
    CHECK(gl(&r.c, LEAF2) == updated && count_rmw(&r) == 0);
    check_accounting(&r);
    mc68k_destroy(r.cpu);
}

/* Review C-B5/C-B10: the locked RMW read, and the simple leaf write, can fail on their own. */
static void test_leaf_update_failures(void) {
    current_case = "leaf update bus faults";
    rig_t r;
    rig_init(&r);
    map(&r, DESC_U, DESC_U, leaf_desc(PHYS_OLD, 0));
    cache_set(&r, LEAF, leaf_desc(PHYS_OLD, 0), false); /* the first read is a cache hit */
    r.c.fail_read_at = LEAF;
    mc68k_translate_result_t t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(!t.ok && t.fault == MMU_FAULT_TABLE_BUS_ERROR);
    CHECK((t.mmusr & MMUSR_B_BIT) != 0 && t.table_bus_error_physical == LEAF);
    CHECK(count_log(&r, 'W', LEAF) == 0);
    CHECK(!t.write_protect);
    check_r_clear_entry(&r, &READ_SUPER);
    CHECK(find_line(&r, LEAF) ==
          NULL); /* the RMW invalidated the clean line before its read failed (C-B44) */
    mc68k_destroy(r.cpu);

    rig_init(&r);
    map(&r, DESC_U, DESC_U, leaf_desc(PHYS_OLD, 0));
    cache_set(&r, LEAF, leaf_desc(PHYS_OLD, 0), false);
    r.c.fail_write_at = LEAF;
    t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(!t.ok && t.fault == MMU_FAULT_TABLE_BUS_ERROR && t.table_bus_error_physical == LEAF);
    CHECK(find_line(&r, LEAF) == NULL); /* also when the RMW write fails */
    check_r_clear_entry(&r, &READ_SUPER);
    mc68k_destroy(r.cpu);

    rig_init(&r);
    map(&r, DESC_U, DESC_U, leaf_desc(PHYS_OLD, DESC_U)); /* U=1, M=0: a simple write sets M */
    r.c.fail_write_at = LEAF;
    t = walk(&r, &WRITE_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(!t.ok && t.fault == MMU_FAULT_TABLE_BUS_ERROR);
    CHECK((t.mmusr & MMUSR_B_BIT) != 0 && t.table_bus_error_physical == LEAF);
    check_r_clear_entry(&r, &READ_SUPER);
    mc68k_destroy(r.cpu);

    /* A simple write that fails on the bus leaves the line as it was (C-B46): clean or dirty,
     * it never holds a value the bus does not have. */
    for (unsigned dirty = 0; dirty < 2; dirty++) {
        rig_init(&r);
        map(&r, DESC_U, DESC_U, leaf_desc(PHYS_OLD, DESC_U));
        cache_set(&r, LEAF, leaf_desc(PHYS_OLD, DESC_U), dirty);
        r.c.fail_write_at = LEAF;
        t = walk(&r, &WRITE_SUPER, MC68K_TRANSLATE_EXECUTE);
        CHECK(!t.ok && t.fault == MMU_FAULT_TABLE_BUS_ERROR &&
              t.table_bus_error_physical == LEAF);
        mc68k_040_cache_line_t *line = find_line(&r, LEAF);
        CHECK(line && line->data[(LEAF & 15u) >> 2] == leaf_desc(PHYS_OLD, DESC_U));
        CHECK(line && (line->dirty != 0) == (dirty != 0));
        CHECK(gl(&r.c, LEAF) == leaf_desc(PHYS_OLD, DESC_U));
        mc68k_destroy(r.cpu);
    }

    /* A failing update of a write-protected page is a table bus error, not a WP fault. */
    rig_init(&r);
    map(&r, DESC_U, DESC_U, leaf_desc(PHYS_OLD, DESC_WP));
    r.c.fail_write_at = LEAF;
    t = walk(&r, &WRITE_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(!t.ok && t.fault == MMU_FAULT_TABLE_BUS_ERROR && !t.write_protect);
    CHECK((t.mmusr & MMUSR_B_BIT) != 0 && t.table_bus_error_physical == LEAF);
    check_r_clear_entry(&r, &READ_SUPER);
    mc68k_destroy(r.cpu);
}

/* Review C-B13..C-B15: the push before the locked RMW writes every dirty longword with its own
 * data, and after a partial failure only the longwords already written lose their dirty bit. */
static void test_rmw_push_longwords(void) {
    current_case = "RMW push of a fully dirty line";
    rig_t r;
    rig_init(&r);
    map(&r, DESC_U, DESC_U, leaf_desc(PHYS_OLD, 0));
    const uint32_t line_base = LEAF & ~UINT32_C(15);
    cache_set(&r, line_base, leaf_desc(PHYS_NEW, 0), true);
    cache_set(&r, line_base + 4, 0x11111111, true);
    cache_set(&r, line_base + 8, 0x22222222, true);
    cache_set(&r, line_base + 12, 0x33333333, true);
    mc68k_translate_result_t t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(t.ok && t.physical == PHYS_NEW);
    CHECK(gl(&r.c, line_base) == leaf_desc(PHYS_NEW, DESC_U));
    CHECK(gl(&r.c, line_base + 4) == 0x11111111);
    CHECK(gl(&r.c, line_base + 8) == 0x22222222);
    CHECK(gl(&r.c, line_base + 12) == 0x33333333);
    CHECK(find_line(&r, LEAF) == NULL);
    check_accounting(&r);
    CHECK(count_rmw(&r) == 2); /* only the read and the write of the leaf itself are locked */
    for (unsigned i = 0; i < r.c.nlog; i++)
        CHECK(!r.c.log[i].rmw || r.c.log[i].addr == LEAF);
    mc68k_destroy(r.cpu);

    current_case = "RMW push, partial failure";
    rig_init(&r);
    map(&r, DESC_U, DESC_U, leaf_desc(PHYS_OLD, 0));
    cache_set(&r, line_base, leaf_desc(PHYS_NEW, 0), true);
    cache_set(&r, line_base + 4, 0x11111111, true);
    cache_set(&r, line_base + 8, 0x22222222, true);
    cache_set(&r, line_base + 12, 0x33333333, true);
    r.c.fail_write_at = line_base + 8;
    t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_PLOAD);
    CHECK(!t.ok && t.fault == MMU_FAULT_TABLE_BUS_ERROR &&
          t.table_bus_error_physical == line_base + 8);
    mc68k_040_cache_line_t *line = find_line(&r, LEAF);
    CHECK(line && line->dirty == 0xC); /* longwords 0 and 1 reached the bus */
    CHECK(gl(&r.c, line_base) == leaf_desc(PHYS_NEW, 0) &&
          gl(&r.c, line_base + 4) == 0x11111111);
    CHECK(gl(&r.c, line_base + 8) != 0x22222222 && gl(&r.c, line_base + 12) != 0x33333333);
    CHECK(count_rmw(&r) == 0);
    check_accounting(&r);
    mc68k_destroy(r.cpu);
}

/* Review C-B20: a PTEST (PLOAD) table search uses a D-cache hit like any other, with no bus
 * read. */
static void test_table_read_hit_pload(void) {
    current_case = "table read hit under PLOAD";
    rig_t r;
    rig_init(&r);
    map(&r, DESC_U, DESC_U, leaf_desc(PHYS_OLD, DESC_U));
    cache_set(&r, LEAF, leaf_desc(PHYS_NEW, DESC_U), true);
    mc68k_translate_result_t t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_PLOAD);
    CHECK(t.ok && t.physical == PHYS_NEW);
    CHECK(count_log(&r, 'R', LEAF) == 0);
    check_accounting(&r);
    CHECK(valid_dcache_lines(&r) == 1);
    mc68k_destroy(r.cpu);
}

/* Review C-B19: the simple writes (leaf M, root/pointer U) also update a clean line that hits,
 * and leave it clean, so that no stale descriptor stays in the cache. */
static void test_simple_write_hit_clean(void) {
    current_case = "simple write hits a clean line";
    rig_t r;
    rig_init(&r);
    map(&r, DESC_U, DESC_U, leaf_desc(PHYS_OLD, DESC_U));
    cache_set(&r, LEAF, leaf_desc(PHYS_NEW, DESC_U), false);
    mc68k_translate_result_t t = walk(&r, &WRITE_SUPER, MC68K_TRANSLATE_EXECUTE);
    const uint32_t updated = leaf_desc(PHYS_NEW, DESC_U | DESC_M);
    CHECK(t.ok && t.physical == PHYS_NEW);
    mc68k_040_cache_line_t *line = find_line(&r, LEAF);
    CHECK(line && line->dirty == 0 && line->data[(LEAF & 15u) >> 2] == updated);
    CHECK(gl(&r.c, LEAF) == updated);
    check_accounting(&r);
    mc68k_destroy(r.cpu);

    current_case = "root/pointer U write hits clean lines";
    rig_init(&r);
    map(&r, 0, 0, leaf_desc(PHYS_OLD, DESC_U));
    cache_set(&r, ROOT_ENTRY, POINTER | DESC_TYPE_POINTER, false);
    cache_set(&r, POINTER_ENTRY, PAGE | DESC_TYPE_POINTER, false);
    t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(t.ok && t.physical == PHYS_OLD);
    CHECK(count_log(&r, 'R', ROOT_ENTRY) == 0 && count_log(&r, 'R', POINTER_ENTRY) == 0);
    const mc68k_040_cache_line_t *root = find_line(&r, ROOT_ENTRY),
                                 *pointer = find_line(&r, POINTER_ENTRY);
    CHECK(root && root->dirty == 0 &&
          root->data[(ROOT_ENTRY & 15u) >> 2] == (POINTER | DESC_TYPE_POINTER | DESC_U));
    CHECK(pointer && pointer->dirty == 0 &&
          pointer->data[(POINTER_ENTRY & 15u) >> 2] == (PAGE | DESC_TYPE_POINTER | DESC_U));
    CHECK(gl(&r.c, ROOT_ENTRY) == (POINTER | DESC_TYPE_POINTER | DESC_U));
    CHECK(gl(&r.c, POINTER_ENTRY) == (PAGE | DESC_TYPE_POINTER | DESC_U));
    check_accounting(&r);
    mc68k_destroy(r.cpu);
}

/* Review C-B21: only a WP fault leaves a resident entry.  A fault that comes with an
 * accumulated WP (invalid leaf, supervisor violation) is still a faulting entry, so a second
 * access faults again. */
static void test_fault_entry_with_wp(void) {
    current_case = "invalid leaf below a WP pointer";
    rig_t r;
    rig_init(&r);
    map(&r, DESC_U, DESC_U | DESC_WP, 0);
    mc68k_translate_result_t t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(!t.ok && t.fault == MMU_FAULT_INVALID);
    t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(!t.ok && t.fault == MMU_FAULT_INVALID && t.source == MC68K_TRANSLATE_ATC);
    mc68k_destroy(r.cpu);

    current_case = "user read of a WP supervisor page";
    rig_init(&r);
    map(&r, DESC_U, DESC_U, leaf_desc(PHYS_OLD, DESC_U | DESC_S | DESC_WP));
    t = walk(&r, &READ_USER, MC68K_TRANSLATE_EXECUTE);
    CHECK(!t.ok && t.fault == MMU_FAULT_SUPERVISOR);
    t = walk(&r, &READ_USER, MC68K_TRANSLATE_EXECUTE);
    CHECK(!t.ok && t.fault == MMU_FAULT_SUPERVISOR && t.source == MC68K_TRANSLATE_ATC);
    mc68k_destroy(r.cpu);
}

/* Review C-B22: the resident ATC entry carries the leaf's WP and M bits to later accesses. */
static void test_atc_entry_bits(void) {
    current_case = "ATC entry after a read (M=0)";
    rig_t r;
    rig_init(&r);
    map(&r, DESC_U, DESC_U, leaf_desc(PHYS_OLD, DESC_U));
    mc68k_translate_result_t t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(t.ok && !t.modified);
    t = walk(&r, &WRITE_SUPER,
             MC68K_TRANSLATE_EXECUTE); /* must revisit the descriptor to set M */
    CHECK(t.ok && t.source == MC68K_TRANSLATE_WALK && t.modified);
    CHECK(gl(&r.c, LEAF) == leaf_desc(PHYS_OLD, DESC_U | DESC_M));
    mc68k_destroy(r.cpu);

    current_case = "ATC entry after a write (M=1)";
    rig_init(&r);
    map(&r, DESC_U, DESC_U, leaf_desc(PHYS_OLD, DESC_U));
    t = walk(&r, &WRITE_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(t.ok && t.modified);
    t = walk(&r, &WRITE_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(t.ok && t.source == MC68K_TRANSLATE_ATC && r.c.nlog == 0);
    mc68k_destroy(r.cpu);

    current_case = "ATC entry after a WP fault (M=1)";
    rig_init(&r);
    map(&r, DESC_U, DESC_U, leaf_desc(PHYS_OLD, DESC_U | DESC_M | DESC_WP));
    t = walk(&r, &WRITE_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(!t.ok && t.fault == MMU_FAULT_WRITE_PROTECT);
    t = walk(&r, &WRITE_SUPER,
             MC68K_TRANSLATE_EXECUTE); /* the retry faults from the resident entry */
    CHECK(!t.ok && t.fault == MMU_FAULT_WRITE_PROTECT && t.source == MC68K_TRANSLATE_ATC &&
          r.c.nlog == 0);
    t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(t.ok && t.source == MC68K_TRANSLATE_ATC && t.write_protect && t.modified);
    mc68k_destroy(r.cpu);
}

/* Review C-B12: a CPUSH whose push fails raises the bus error at the failing longword, with the
 * line base in the access-error frame (push_line() is shared with the table walk). */
static void test_cpush_failure_address(void) {
    current_case = "CPUSH push failure";
    rig_t r;
    rig_init(&r);
    cache_set(&r, LEAF + 4, 0x11111111, true);
    r.c.fail_write_at = LEAF + 4;
    cpusha_dc(&r);
    CHECK(r.cpu->exec_state == CPU_EXCEPTION && r.cpu->fault_vector == 2);
    CHECK(r.cpu->addr_err_addr == LEAF + 4 && !r.cpu->addr_err_read);
    CHECK(r.cpu->fault040.valid && r.cpu->fault040.fault_address == (LEAF & ~UINT32_C(15)));
    CHECK(r.cpu->fault040.ssw == 0);
    /* Review C-B27: the failed line stays valid with its unwritten longword dirty. */
    mc68k_040_cache_line_t *line = find_line(&r, LEAF);
    CHECK(line && line->dirty == 0x2);
    mc68k_destroy(r.cpu);
}

/* design §2.1: with CACR.DE=0 the table access ignores the cache (approximation of the
 * lock-up described in 040UM §3.2.1). */
static void test_de_disabled(void) {
    current_case = "CACR.DE=0";
    rig_t r;
    rig_init(&r);
    r.cpu->cacr = 0;
    map(&r, DESC_U, DESC_U, leaf_desc(PHYS_OLD, DESC_U));
    cache_set(&r, LEAF, leaf_desc(PHYS_NEW, DESC_U), true);
    mc68k_translate_result_t t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(t.ok && t.physical == PHYS_OLD);
    CHECK(find_line(&r, LEAF) && find_line(&r, LEAF)->dirty != 0);
    mc68k_destroy(r.cpu);
}

/* Review C-B26: with CACR.DE=0 the updates ignore the cache as well: the locked RMW does not
 * push or invalidate a line, and the simple write does not touch it. */
static void test_de_disabled_updates(void) {
    current_case = "CACR.DE=0 locked RMW";
    rig_t r;
    rig_init(&r);
    r.cpu->cacr = 0;
    map(&r, DESC_U, DESC_U, leaf_desc(PHYS_OLD, 0));
    cache_set(&r, LEAF, leaf_desc(PHYS_NEW, 0), true);
    mc68k_translate_result_t t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(t.ok && t.physical == PHYS_OLD);
    CHECK(count_rmw(&r) == 2 && count_writes(&r) == 1);
    CHECK(gl(&r.c, LEAF) == leaf_desc(PHYS_OLD, DESC_U));
    mc68k_040_cache_line_t *line = find_line(&r, LEAF);
    CHECK(line && line->dirty != 0 && line->data[(LEAF & 15u) >> 2] == leaf_desc(PHYS_NEW, 0));
    check_accounting(&r);
    mc68k_destroy(r.cpu);

    current_case = "CACR.DE=0 simple write";
    rig_init(&r);
    r.cpu->cacr = 0;
    map(&r, DESC_U, DESC_U, leaf_desc(PHYS_OLD, DESC_U));
    cache_set(&r, LEAF, leaf_desc(PHYS_NEW, DESC_U), false);
    t = walk(&r, &WRITE_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(t.ok && t.physical == PHYS_OLD && count_rmw(&r) == 0);
    CHECK(gl(&r.c, LEAF) == leaf_desc(PHYS_OLD, DESC_U | DESC_M));
    line = find_line(&r, LEAF);
    CHECK(line && line->data[(LEAF & 15u) >> 2] == leaf_desc(PHYS_NEW, DESC_U));
    mc68k_destroy(r.cpu);
}

/* Review C-B28: a write to a write-protected page faults for every walk mode and function code,
 * and never sets M. */
static void test_wp_write_modes(void) {
    static const struct {
        const char *name;
        const mc68k_access_t *access;
        mc68k_translate_mode_t mode;
    } cases[] = {
        {"WP write, PLOAD", &WRITE_SUPER, MC68K_TRANSLATE_PLOAD},
        {"WP write, DEBUG", &WRITE_SUPER, MC68K_TRANSLATE_DEBUG},
        {"WP write, user", &WRITE_USER, MC68K_TRANSLATE_EXECUTE},
    };
    CHECK(sizeof(cases) / sizeof(cases[0]) == 3);
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        current_case = cases[i].name;
        rig_t r;
        rig_init(&r);
        map(&r, DESC_U, DESC_U, leaf_desc(PHYS_OLD, DESC_U | DESC_WP));
        mc68k_translate_result_t t = walk(&r, cases[i].access, cases[i].mode);
        CHECK(!t.ok && t.fault == MMU_FAULT_WRITE_PROTECT && t.write_protect);
        CHECK(gl(&r.c, LEAF) == leaf_desc(PHYS_OLD, DESC_U | DESC_WP) && count_writes(&r) == 0);
        mc68k_destroy(r.cpu);
    }

    /* Review C-B35: with U=0 the PTESTW still sets U with the locked RMW (Table 3-1) and then
     * faults. */
    current_case = "WP write, PLOAD, U=0";
    rig_t r;
    rig_init(&r);
    map(&r, DESC_U, DESC_U, leaf_desc(PHYS_OLD, DESC_WP));
    mc68k_translate_result_t t = walk(&r, &WRITE_SUPER, MC68K_TRANSLATE_PLOAD);
    CHECK(!t.ok && t.fault == MMU_FAULT_WRITE_PROTECT);
    CHECK(gl(&r.c, LEAF) == leaf_desc(PHYS_OLD, DESC_WP | DESC_U) && count_rmw(&r) == 2);
    mc68k_destroy(r.cpu);
}

/* design §2.2: a user access to a supervisor page never sets M. */
static void test_supervisor_violation(void) {
    current_case = "supervisor violation";
    rig_t r;
    rig_init(&r);
    map(&r, DESC_U, DESC_U, leaf_desc(PHYS_OLD, DESC_S));
    mc68k_translate_result_t t = walk(&r, &WRITE_USER, MC68K_TRANSLATE_EXECUTE);
    CHECK(!t.ok && t.fault == MMU_FAULT_SUPERVISOR);
    CHECK((gl(&r.c, LEAF) & DESC_M) == 0);
    mc68k_destroy(r.cpu);

    /* A violation returns before the U update: the descriptor is untouched and nothing is
     * written (C-B45). */
    const mc68k_access_t *accesses[] = {&WRITE_USER, &READ_USER};
    for (unsigned i = 0; i < 2; i++) {
        rig_init(&r);
        map(&r, DESC_U, DESC_U, leaf_desc(PHYS_OLD, DESC_S));
        t = walk(&r, accesses[i], MC68K_TRANSLATE_EXECUTE);
        CHECK(!t.ok && t.fault == MMU_FAULT_SUPERVISOR);
        CHECK(gl(&r.c, LEAF) == leaf_desc(PHYS_OLD, DESC_S));
        CHECK(count_writes(&r) == 0);
        mc68k_destroy(r.cpu);
    }
}

/* design §2.2: an indirect descriptor is only read; U/M, push and invalidate act on the
 * descriptor it points to. */
static void test_indirect_descriptor(void) {
    current_case = "indirect descriptor";
    rig_t r;
    rig_init(&r);
    map(&r, DESC_U, DESC_U, INDIRECT_DESC | DESC_TYPE_INDIRECT);
    pl(&r.c, INDIRECT_DESC, leaf_desc(PHYS_OLD, 0));
    cache_set(&r, INDIRECT_DESC, leaf_desc(PHYS_NEW, 0), true);
    mc68k_translate_result_t t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(t.ok && t.physical == PHYS_NEW);
    CHECK(gl(&r.c, INDIRECT_DESC) == leaf_desc(PHYS_NEW, DESC_U));
    CHECK(find_line(&r, INDIRECT_DESC) == NULL);
    CHECK(gl(&r.c, LEAF) == (INDIRECT_DESC | DESC_TYPE_INDIRECT));
    mc68k_destroy(r.cpu);
}

/* Review C-B42/C-B43: every table address and descriptor value above 64 KiB.  The tables live
 * at WIDE_HI | offset and the page frame is WIDE_PHYS; the bus aliases them onto RAM. */
#define WIDE_HI            UINT32_C(0x12340000)
#define WIDE_PHYS          UINT32_C(0x5a008000)
#define WIDE_ROOT_ENTRY    (WIDE_HI | ROOT_ENTRY)
#define WIDE_POINTER_ENTRY (WIDE_HI | POINTER_ENTRY)
#define WIDE_LEAF          (WIDE_HI | LEAF)
static void wide_init(rig_t *r, uint32_t root_flags, uint32_t pointer_flags, uint32_t leaf) {
    rig_init(r);
    r->c.mirror = true;
    r->cpu->mmu040_srp = r->cpu->mmu040_urp = WIDE_HI | ROOT;
    pl(&r->c, WIDE_ROOT_ENTRY, (WIDE_HI | POINTER) | DESC_TYPE_POINTER | root_flags);
    pl(&r->c, WIDE_POINTER_ENTRY, (WIDE_HI | PAGE) | DESC_TYPE_POINTER | pointer_flags);
    pl(&r->c, WIDE_LEAF, leaf);
}
static void check_wide_log(const rig_t *r) {
    for (unsigned i = 0; i < r->c.nlog; i++)
        CHECK((r->c.log[i].addr >> 16) == (WIDE_HI >> 16));
}

static void test_wide_addresses(void) {
    current_case = "wide addresses";
    /* Cold walk, every U clear: root/pointer U writes and the leaf RMW from the bus. */
    rig_t r;
    wide_init(&r, 0, 0, leaf_desc(WIDE_PHYS, 0));
    mc68k_translate_result_t t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(t.ok && t.physical == WIDE_PHYS);
    CHECK(gl(&r.c, WIDE_ROOT_ENTRY) == ((WIDE_HI | POINTER) | DESC_TYPE_POINTER | DESC_U));
    CHECK(gl(&r.c, WIDE_POINTER_ENTRY) == ((WIDE_HI | PAGE) | DESC_TYPE_POINTER | DESC_U));
    CHECK(gl(&r.c, WIDE_LEAF) == leaf_desc(WIDE_PHYS, DESC_U));
    CHECK(count_log(&r, 'R', WIDE_ROOT_ENTRY) == 1 && count_log(&r, 'W', WIDE_ROOT_ENTRY) == 1);
    CHECK(count_log(&r, 'R', WIDE_POINTER_ENTRY) == 1 &&
          count_log(&r, 'W', WIDE_POINTER_ENTRY) == 1);
    CHECK(count_log(&r, 'R', WIDE_LEAF) == 2 &&
          count_log(&r, 'W', WIDE_LEAF) == 1); /* fetch + RMW read */
    CHECK(r.c.nlog == 7);
    check_wide_log(&r);
    check_accounting(&r);
    mc68k_destroy(r.cpu);

    /* The same walk with every descriptor in the D-cache: no bus read, and the U/M writes
     * update the lines. */
    uint32_t root = (WIDE_HI | POINTER) | DESC_TYPE_POINTER,
             pointer = (WIDE_HI | PAGE) | DESC_TYPE_POINTER;
    wide_init(&r, 0, 0, leaf_desc(WIDE_PHYS, DESC_U));
    cache_set(&r, WIDE_ROOT_ENTRY, root, false);
    cache_set(&r, WIDE_POINTER_ENTRY, pointer, false);
    cache_set(&r, WIDE_LEAF, leaf_desc(WIDE_PHYS, DESC_U), false);
    t = walk(&r, &WRITE_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(t.ok && t.physical == WIDE_PHYS);
    CHECK(r.c.nlog == 3 && count_log(&r, 'R', WIDE_ROOT_ENTRY) == 0 &&
          count_log(&r, 'R', WIDE_POINTER_ENTRY) == 0 && count_log(&r, 'R', WIDE_LEAF) == 0);
    CHECK(last_log(&r, 'W', WIDE_ROOT_ENTRY) && last_log(&r, 'W', WIDE_POINTER_ENTRY) &&
          last_log(&r, 'W', WIDE_LEAF));
    CHECK(gl(&r.c, WIDE_ROOT_ENTRY) == (root | DESC_U) &&
          gl(&r.c, WIDE_POINTER_ENTRY) == (pointer | DESC_U));
    CHECK(gl(&r.c, WIDE_LEAF) == leaf_desc(WIDE_PHYS, DESC_U | DESC_M));
    mc68k_040_cache_line_t *line = find_line(&r, WIDE_ROOT_ENTRY);
    CHECK(line && line->data[(WIDE_ROOT_ENTRY & 15u) >> 2] == (root | DESC_U) &&
          line->dirty == 0);
    line = find_line(&r, WIDE_POINTER_ENTRY);
    CHECK(line && line->data[(WIDE_POINTER_ENTRY & 15u) >> 2] == (pointer | DESC_U) &&
          line->dirty == 0);
    line = find_line(&r, WIDE_LEAF);
    CHECK(line && line->data[(WIDE_LEAF & 15u) >> 2] == leaf_desc(WIDE_PHYS, DESC_U | DESC_M) &&
          line->dirty == 0);
    check_wide_log(&r);
    check_accounting(&r);
    mc68k_destroy(r.cpu);

    /* A cached U=0 leaf: the locked RMW writes the full descriptor back. */
    wide_init(&r, DESC_U, DESC_U, leaf_desc(WIDE_PHYS, 0));
    cache_set(&r, WIDE_LEAF, leaf_desc(WIDE_PHYS, 0), false);
    t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(t.ok && t.physical == WIDE_PHYS);
    CHECK(gl(&r.c, WIDE_LEAF) == leaf_desc(WIDE_PHYS, DESC_U) &&
          find_line(&r, WIDE_LEAF) == NULL);
    CHECK(count_rmw(&r) == 2);
    check_wide_log(&r);
    mc68k_destroy(r.cpu);

    /* An indirect descriptor above 64 KiB. */
    wide_init(&r, DESC_U, DESC_U, (WIDE_HI | INDIRECT_DESC) | DESC_TYPE_INDIRECT);
    pl(&r.c, WIDE_HI | INDIRECT_DESC, leaf_desc(WIDE_PHYS, DESC_U));
    t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(t.ok && t.physical == WIDE_PHYS && count_log(&r, 'R', WIDE_HI | INDIRECT_DESC) == 1);
    mc68k_destroy(r.cpu);

    /* The failing address is reported in full for each level. */
    uint32_t fails[] = {WIDE_ROOT_ENTRY, WIDE_POINTER_ENTRY, WIDE_LEAF};
    for (unsigned i = 0; i < 3; i++) {
        wide_init(&r, DESC_U, DESC_U, leaf_desc(WIDE_PHYS, DESC_U));
        r.c.fail_read_at = fails[i];
        t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
        CHECK(!t.ok && t.fault == MMU_FAULT_TABLE_BUS_ERROR &&
              t.table_bus_error_physical == fails[i]);
        mc68k_destroy(r.cpu);
    }
    /* ... and the root descriptor itself is read from an SRP above 64 KiB. */
    wide_init(&r, DESC_U, DESC_U, leaf_desc(WIDE_PHYS, DESC_U));
    r.c.fail_read_at = WIDE_ROOT_ENTRY;
    t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(!t.ok && last_log(&r, 'R', WIDE_ROOT_ENTRY));
    mc68k_destroy(r.cpu);
}

/* Reviews C-B47..C-B51 */
#define G_BIT    UINT32_C(0x400)
#define U1_BIT   UINT32_C(0x200)
#define U0_BIT   UINT32_C(0x100)
#define CM_SHIFT 5
static mc68k_atc_entry_t *atc_entry(rig_t *r) {
    for (unsigned i = 0; i < MC68K_ATC_ENTRIES; i++)
        if (r->cpu->atc040_d[i].valid)
            return &r->cpu->atc040_d[i];
    return NULL;
}
static void test_atc_entry_non_wp_faults(void) {
    current_case = "C-B47 a invalid leaf below WP pointer, write then read";
    rig_t r;
    rig_init(&r);
    map(&r, DESC_U, DESC_U | DESC_WP, 0);
    mc68k_translate_result_t t = walk(&r, &WRITE_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(!t.ok && t.fault == MMU_FAULT_INVALID);
    t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(!t.ok && t.fault == MMU_FAULT_INVALID);
    mc68k_destroy(r.cpu);
    current_case = "C-B47 b user write of a WP supervisor page, then read";
    rig_init(&r);
    map(&r, DESC_U, DESC_U, leaf_desc(PHYS_OLD, DESC_U | DESC_S | DESC_WP));
    t = walk(&r, &WRITE_USER, MC68K_TRANSLATE_EXECUTE);
    CHECK(!t.ok && t.fault == MMU_FAULT_SUPERVISOR);
    t = walk(&r, &READ_USER, MC68K_TRANSLATE_EXECUTE);
    CHECK(!t.ok && t.fault == MMU_FAULT_SUPERVISOR);
    mc68k_destroy(r.cpu);
}
static void test_wp_fault_mmusr(void) {
    const uint32_t f = G_BIT | U1_BIT | U0_BIT | DESC_S | (UINT32_C(1) << CM_SHIFT) | DESC_M |
                       DESC_WP | DESC_U;
    for (unsigned u = 0; u < 2; u++) {
        current_case = u ? "C-B48 PTESTW WP mmusr U=1" : "C-B48 PTESTW WP mmusr U=0";
        rig_t r;
        rig_init(&r);
        map(&r, DESC_U, DESC_U, leaf_desc(PHYS_OLD, u ? f : f & ~DESC_U));
        mc68k_translate_result_t t = walk(&r, &WRITE_SUPER, MC68K_TRANSLATE_PLOAD);
        CHECK(!t.ok && t.fault == MMU_FAULT_WRITE_PROTECT);
        CHECK(t.mmusr == (PHYS_OLD | MMUSR_R_BIT | MMUSR_W_BIT | G_BIT | U1_BIT | U0_BIT |
                          DESC_S | (UINT32_C(1) << CM_SHIFT) | DESC_M));
        mc68k_atc_entry_t *e = atc_entry(&r);
        CHECK(e && e->global && !e->bus_error && e->cache_mode == 1);
        mc68k_destroy(r.cpu);
    }
}
static void test_wp_fault_cache_mode(void) {
    for (unsigned cm = 0; cm < 4; cm++) {
        current_case = "C-B48 WP fault cache mode";
        rig_t r;
        rig_init(&r);
        map(&r, DESC_U, DESC_U, leaf_desc(PHYS_OLD, DESC_U | DESC_WP | (cm << CM_SHIFT)));
        mc68k_translate_result_t t = walk(&r, &WRITE_SUPER, MC68K_TRANSLATE_EXECUTE);
        CHECK(!t.ok && t.cache_mode == cm && t.cache_inhibit == (cm >= 2));
        t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
        CHECK(t.ok && t.source == MC68K_TRANSLATE_ATC && t.cache_mode == cm &&
              t.cache_inhibit == (cm >= 2));
        mc68k_destroy(r.cpu);
    }
}
static void test_table_line_ways(void) {
    for (unsigned way = 0; way < MC68K_040_CACHE_WAYS; way++) {
        current_case = "C-B49 descriptor line in each way";
        rig_t r;
        rig_init(&r);
        map(&r, DESC_U, DESC_U, leaf_desc(PHYS_OLD, DESC_U));
        uint32_t base = LEAF & ~UINT32_C(15);
        mc68k_040_cache_line_t *line =
            &r.cpu->dcache040[(base >> 4) & (MC68K_040_CACHE_SETS - 1u)][way];
        *line = (mc68k_040_cache_line_t){.physical_line = base, .valid = true, .loaded = true};
        for (unsigned i = 0; i < 4; i++)
            line->data[i] = gl(&r.c, base + i * 4u);
        line->data[(LEAF & 15u) >> 2] = leaf_desc(PHYS_NEW, DESC_U);
        line->dirty = 1u << ((LEAF & 15u) >> 2);
        mc68k_translate_result_t t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
        CHECK(t.ok && t.physical == PHYS_NEW && count_log(&r, 'R', LEAF) == 0);
        mc68k_destroy(r.cpu);
    }
}
static void test_rmw_result_fields(void) {
    const uint32_t f = G_BIT | U1_BIT | U0_BIT | DESC_S | (UINT32_C(3) << CM_SHIFT);
    current_case = "C-B50 RMW result mmusr fields";
    rig_t r;
    rig_init(&r);
    map(&r, DESC_U, DESC_U, leaf_desc(PHYS_OLD, f));
    mc68k_translate_result_t t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_PLOAD);
    CHECK(t.ok && t.mmusr == (PHYS_OLD | MMUSR_R_BIT | f));
    mc68k_destroy(r.cpu);
}
static void test_u_write_value_on_read_failure(void) {
    current_case = "C-B51 pending U value on a failed read with poisoned data";
    rig_t r;
    rig_init(&r);
    r.c.poison = true;
    map(&r, DESC_U, 0, leaf_desc(PHYS_OLD, DESC_U));
    r.c.fail_read_at = LEAF;
    mc68k_translate_result_t t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(!t.ok && t.fault == MMU_FAULT_TABLE_BUS_ERROR);
    CHECK(gl(&r.c, POINTER_ENTRY) == (PAGE | DESC_TYPE_POINTER | DESC_U));
    mc68k_destroy(r.cpu);
}
/* #103 A (C-N23, C-N27, C-N34): an indirect descriptor's own read, its address and the
 * descriptor address the walk reports. */
static void test_indirect_variants(void) {
    current_case = "indirect read failure";
    rig_t r;
    rig_init(&r);
    map(&r, DESC_U, DESC_U, INDIRECT_DESC | DESC_TYPE_INDIRECT);
    pl(&r.c, INDIRECT_DESC, leaf_desc(PHYS_OLD, 0));
    r.c.fail_read_at = INDIRECT_DESC;
    mc68k_translate_result_t t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(!t.ok && t.fault == MMU_FAULT_TABLE_BUS_ERROR && t.mmusr == MMUSR_B_BIT);
    CHECK(t.table_bus_error_physical ==
          INDIRECT_DESC); /* the failing read, not the leaf slot */
    CHECK(count_log(&r, 'R', INDIRECT_DESC) == 1 && count_writes(&r) == 0);
    CHECK(gl(&r.c, INDIRECT_DESC) == leaf_desc(PHYS_OLD, 0));
    check_r_clear_entry(&r, &READ_SUPER); /* the indirect read failed */
    mc68k_destroy(r.cpu);

    current_case = "indirect descriptor address in the result";
    rig_init(&r);
    map(&r, DESC_U, DESC_U, leaf_desc(PHYS_OLD, DESC_U));
    t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(t.ok && t.level == 3 && t.descriptor_physical == LEAF);
    mc68k_destroy(r.cpu);
    rig_init(&r);
    map(&r, DESC_U, DESC_U, INDIRECT_DESC | DESC_TYPE_INDIRECT);
    pl(&r.c, INDIRECT_DESC, leaf_desc(PHYS_OLD, DESC_U));
    t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(t.ok && t.level == 3 && t.descriptor_physical == INDIRECT_DESC);
    CHECK(count_log(&r, 'R', LEAF) == 1 && count_log(&r, 'R', INDIRECT_DESC) == 1 &&
          r.c.nlog == 4);
    mc68k_destroy(r.cpu);

    current_case = "indirect descriptor updates";
    rig_init(&r);
    map(&r, DESC_U, DESC_U, INDIRECT_DESC | DESC_TYPE_INDIRECT);
    pl(&r.c, INDIRECT_DESC, leaf_desc(PHYS_OLD, 0));
    t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_EXECUTE); /* U=0 read: locked RMW at the target */
    CHECK(t.ok && t.descriptor_physical == INDIRECT_DESC);
    CHECK(count_rmw(&r) == 2 && last_log(&r, 'W', INDIRECT_DESC) &&
          last_log(&r, 'W', INDIRECT_DESC)->rmw);
    CHECK(count_writes(&r) == 1 && gl(&r.c, INDIRECT_DESC) == leaf_desc(PHYS_OLD, DESC_U));
    mc68k_destroy(r.cpu);
    rig_init(&r);
    map(&r, DESC_U, DESC_U, INDIRECT_DESC | DESC_TYPE_INDIRECT);
    pl(&r.c, INDIRECT_DESC, leaf_desc(PHYS_OLD, DESC_U));
    t = walk(&r, &WRITE_SUPER,
             MC68K_TRANSLATE_EXECUTE); /* U=1, M=0 write: simple write at the target */
    CHECK(t.ok && t.modified && count_rmw(&r) == 0 && count_writes(&r) == 1);
    CHECK(last_log(&r, 'W', INDIRECT_DESC) &&
          gl(&r.c, INDIRECT_DESC) == leaf_desc(PHYS_OLD, DESC_U | DESC_M));
    CHECK(gl(&r.c, LEAF) == (INDIRECT_DESC | DESC_TYPE_INDIRECT));
    mc68k_destroy(r.cpu);

    current_case = "indirect descriptor above 64 KiB";
    wide_init(&r, DESC_U, DESC_U, (WIDE_HI | INDIRECT_DESC) | DESC_TYPE_INDIRECT);
    pl(&r.c, WIDE_HI | INDIRECT_DESC, leaf_desc(WIDE_PHYS, 0));
    t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(t.ok && t.physical == WIDE_PHYS &&
          t.descriptor_physical == (WIDE_HI | INDIRECT_DESC));
    CHECK(count_rmw(&r) == 2 && last_log(&r, 'W', WIDE_HI | INDIRECT_DESC));
    CHECK(gl(&r.c, WIDE_HI | INDIRECT_DESC) == leaf_desc(WIDE_PHYS, DESC_U));
    check_wide_log(&r);
    mc68k_destroy(r.cpu);
    wide_init(&r, DESC_U, DESC_U, (WIDE_HI | INDIRECT_DESC) | DESC_TYPE_INDIRECT);
    pl(&r.c, WIDE_HI | INDIRECT_DESC, leaf_desc(WIDE_PHYS, DESC_U));
    r.c.fail_read_at = WIDE_HI | INDIRECT_DESC;
    t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(!t.ok && t.table_bus_error_physical == (WIDE_HI | INDIRECT_DESC));
    mc68k_destroy(r.cpu);
}

/* #103 A (C-N14): the pointer descriptors' UDT, the address masks of the root and pointer
 * descriptors, and the WP accumulated over each level. */
static void test_descriptor_types_and_masks(void) {
    for (uint32_t type = 0; type < 4; type++) {
        rig_t r;
        rig_init(&r);
        current_case = "root descriptor UDT";
        map(&r, DESC_U, DESC_U, leaf_desc(PHYS_OLD, DESC_U));
        pl(&r.c, ROOT_ENTRY, POINTER | type | DESC_U);
        mc68k_translate_result_t t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
        CHECK(t.ok == (type >= 2));
        CHECK(type >= 2 || (t.fault == MMU_FAULT_INVALID && t.level == 1 && r.c.nlog == 1));
        mc68k_destroy(r.cpu);
        current_case = "pointer descriptor UDT";
        rig_init(&r);
        map(&r, DESC_U, DESC_U, leaf_desc(PHYS_OLD, DESC_U));
        pl(&r.c, POINTER_ENTRY, PAGE | type | DESC_U);
        t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
        CHECK(t.ok == (type >= 2));
        CHECK(type >= 2 || (t.fault == MMU_FAULT_INVALID && t.level == 2 && r.c.nlog == 2));
        mc68k_destroy(r.cpu);
    }
    current_case = "leaf descriptor UDT 0";
    rig_t r;
    rig_init(&r);
    map(&r, DESC_U, DESC_U, PHYS_OLD | DESC_U);
    mc68k_translate_result_t t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(!t.ok && t.fault == MMU_FAULT_INVALID && t.level == 3);
    mc68k_destroy(r.cpu);
    for (uint32_t type = 1; type < 4; type += 2) {
        current_case = "leaf descriptor UDT 1 and 3";
        rig_init(&r);
        map(&r, DESC_U, DESC_U, PHYS_OLD | DESC_U | type);
        t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
        CHECK(t.ok && t.physical == PHYS_OLD);
        mc68k_destroy(r.cpu);
    }

    /* Root table at bit 9, pointer table at bit 8, with junk below the address field of each
     * descriptor. */
    enum {
        ROOT_TBL = 0x4200,
        LEAF_TBL = 0x6100,
        LEAF_ENTRY12 = LEAF_TBL + ((LOGICAL >> 12) & 0x3f) * 4,
        PTR_ENTRY12 = ROOT_TBL + ((LOGICAL >> 18) & 0x7f) * 4
    };
    current_case = "table address masks (4 KB pages)";
    rig_init(&r);
    pl(&r.c, ROOT_ENTRY, ROOT_TBL | 0x1f0 | DESC_U | DESC_TYPE_POINTER);
    pl(&r.c, PTR_ENTRY12, LEAF_TBL | 0xf0 | DESC_U | DESC_TYPE_POINTER);
    pl(&r.c, LEAF_ENTRY12, leaf_desc(PHYS_OLD, DESC_U));
    t = walk_at(&r, LOGICAL + 0x123, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(t.ok && t.physical == PHYS_OLD + 0x123 && t.descriptor_physical == LEAF_ENTRY12);
    CHECK(r.c.nlog == 3 && count_log(&r, 'R', ROOT_ENTRY) == 1 &&
          count_log(&r, 'R', PTR_ENTRY12) == 1 && count_log(&r, 'R', LEAF_ENTRY12) == 1);
    mc68k_destroy(r.cpu);

    /* SRP/URP: bit 9 of the root table address is kept, the low 9 bits of the register are not.
     */
    enum { ROOT_ALT = 0x2200, ROOT_ALT_ENTRY = ROOT_ALT + (LOGICAL >> 25) * 4 };
    current_case = "root pointer register mask";
    rig_init(&r);
    r.cpu->mmu040_srp = r.cpu->mmu040_urp = ROOT_ALT | 0x1ff;
    map(&r, DESC_U, DESC_U, leaf_desc(PHYS_OLD, DESC_U));
    pl(&r.c, ROOT_ALT_ENTRY, POINTER | DESC_U | DESC_TYPE_POINTER);
    t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(t.ok && count_log(&r, 'R', ROOT_ALT_ENTRY) == 1);
    mc68k_destroy(r.cpu);

    /* The top bit of the pointer index (logical bit 24) and of the 6-bit leaf index (bit 17).
     */
    enum {
        FAR = LOGICAL + 0x1000000 + 0x20000,
        FAR_PTR_ENTRY = POINTER + ((FAR >> 18) & 0x7f) * 4,
        FAR_LEAF = PAGE + ((FAR >> 12) & 0x3f) * 4
    };
    current_case = "table index top bits";
    rig_init(&r);
    pl(&r.c, ROOT_ENTRY, POINTER | DESC_U | DESC_TYPE_POINTER);
    pl(&r.c, FAR_PTR_ENTRY, PAGE | DESC_U | DESC_TYPE_POINTER);
    pl(&r.c, FAR_LEAF, leaf_desc(PHYS_OLD, DESC_U));
    t = walk_at(&r, FAR, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(FAR_PTR_ENTRY == POINTER + 0x4d * 4 && FAR_LEAF == PAGE + 0x24 * 4);
    CHECK(t.ok && t.physical == PHYS_OLD && t.descriptor_physical == FAR_LEAF && r.c.nlog == 3);
    mc68k_destroy(r.cpu);

    for (unsigned level = 0; level < 3; level++) {
        current_case = "WP accumulated over one level";
        rig_init(&r);
        map(&r, DESC_U | (level == 0 ? DESC_WP : 0), DESC_U | (level == 1 ? DESC_WP : 0),
            leaf_desc(PHYS_OLD, DESC_U | (level == 2 ? DESC_WP : 0)));
        t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
        CHECK(t.ok && t.write_protect && (t.mmusr & MMUSR_W_BIT));
        CHECK(atc_entry(&r) && atc_entry(&r)->write_protect);
        mc68k_destroy(r.cpu);
        rig_init(&r);
        map(&r, DESC_U | (level == 0 ? DESC_WP : 0), DESC_U | (level == 1 ? DESC_WP : 0),
            leaf_desc(PHYS_OLD, DESC_U | (level == 2 ? DESC_WP : 0)));
        t = walk(&r, &WRITE_SUPER, MC68K_TRANSLATE_EXECUTE);
        CHECK(!t.ok && t.fault == MMU_FAULT_WRITE_PROTECT && t.write_protect);
        CHECK(gl(&r.c, LEAF) == leaf_desc(PHYS_OLD, DESC_U | (level == 2 ? DESC_WP : 0)) &&
              count_writes(&r) == 0);
        mc68k_destroy(r.cpu);
    }
    current_case = "no WP";
    rig_init(&r);
    map(&r, DESC_U, DESC_U, leaf_desc(PHYS_OLD, DESC_U));
    t = walk(&r, &WRITE_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(t.ok && !t.write_protect && !(t.mmusr & MMUSR_W_BIT));
    mc68k_destroy(r.cpu);
}

/* #103 A (C-N10): 8 KB pages (TC.P): leaf index, pointer-table mask, frame mask, ATC page size.
 */
static void test_page_8k(void) {
    enum {
        TC_P_BIT = 0x4000,
        BASE8 = 0x1234a000,
        OFFSET8 = 0x0abc,
        LEAF_TBL8 = 0x6080,
        LEAF_ENTRY8 = LEAF_TBL8 + ((BASE8 >> 13) & 0x1f) * 4
    };
    current_case = "8KB page walk";
    rig_t r;
    rig_init(&r);
    r.cpu->mmu040_tc = TC_E_BIT | TC_P_BIT;
    pl(&r.c, ROOT_ENTRY, POINTER | DESC_U | DESC_TYPE_POINTER);
    pl(&r.c, POINTER_ENTRY, LEAF_TBL8 | 0x70 | DESC_U | DESC_TYPE_POINTER);
    pl(&r.c, LEAF_ENTRY8,
       leaf_desc(PHYS_OLD | 0x1000, DESC_U)); /* bit 12 is below the 8KB frame */
    pl(&r.c, LEAF_ENTRY8 + 4, leaf_desc(PHYS_NEW, DESC_U));
    mc68k_translate_result_t t =
        walk_at(&r, BASE8 + OFFSET8, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(t.ok && t.physical == PHYS_OLD + OFFSET8 && t.descriptor_physical == LEAF_ENTRY8);
    CHECK(t.mmusr == (PHYS_OLD | MMUSR_R_BIT));
    CHECK(r.c.nlog == 3 && count_log(&r, 'R', LEAF_ENTRY8) == 1);
    mc68k_atc_entry_t *e = atc_entry(&r);
    CHECK(e && e->page_shift == 13 && e->logical_page == BASE8 && e->physical_page == PHYS_OLD);
    t = walk_at(&r, BASE8 + 0x1000 + 0x20, &READ_SUPER,
                MC68K_TRANSLATE_EXECUTE); /* same 8KB page */
    CHECK(t.ok && t.source == MC68K_TRANSLATE_ATC && t.physical == PHYS_OLD + 0x1020 &&
          r.c.nlog == 0);
    t = walk_at(&r, BASE8 + 0x2000 + 0x1abc, &READ_SUPER,
                MC68K_TRANSLATE_EXECUTE); /* the next page, offset bit 12 set */
    CHECK(t.ok && t.source == MC68K_TRANSLATE_WALK && t.physical == PHYS_NEW + 0x1abc &&
          count_log(&r, 'R', LEAF_ENTRY8 + 4) == 1);
    CHECK(atc_valid_entries(&r) == 2);
    r.cpu->mmu040_tc =
        TC_E_BIT; /* an 8KB entry does not satisfy a 4KB lookup of the same page address */
    t = walk_at(&r, BASE8 + OFFSET8, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(t.source == MC68K_TRANSLATE_WALK && r.c.nlog != 0);
    mc68k_destroy(r.cpu);

    current_case = "8KB page U/M update";
    rig_init(&r);
    r.cpu->mmu040_tc = TC_E_BIT | TC_P_BIT;
    pl(&r.c, ROOT_ENTRY, POINTER | DESC_U | DESC_TYPE_POINTER);
    pl(&r.c, POINTER_ENTRY, LEAF_TBL8 | DESC_U | DESC_TYPE_POINTER);
    pl(&r.c, LEAF_ENTRY8, leaf_desc(PHYS_OLD, 0));
    t = walk_at(&r, BASE8 + OFFSET8, &WRITE_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(t.ok && t.modified && gl(&r.c, LEAF_ENTRY8) == leaf_desc(PHYS_OLD, DESC_U | DESC_M));
    mc68k_destroy(r.cpu);
}

/* #103 A (C-N14): ATC entry registration and the hit path. */
static void test_atc_paths(void) {
    current_case = "ATC entry fields and hit";
    rig_t r;
    rig_init(&r);
    map(&r, DESC_U, DESC_U, leaf_desc(PHYS_OLD, DESC_U));
    mc68k_translate_result_t t =
        walk_at(&r, LOGICAL + 0x123, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(t.ok && t.source == MC68K_TRANSLATE_WALK && t.physical == PHYS_OLD + 0x123);
    mc68k_atc_entry_t *e = atc_entry(&r);
    CHECK(e && e->valid && e->fc == 5 && e->page_shift == 12 && e->logical_page == LOGICAL &&
          e->physical_page == PHYS_OLD && !e->bus_error && !e->global && !e->write_protect &&
          !e->modified);
    const uint32_t first = e ? e->history : 0;
    CHECK(first != 0);
    t = walk_at(&r, LOGICAL + 0x456, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(t.ok && t.source == MC68K_TRANSLATE_ATC && t.physical == PHYS_OLD + 0x456 &&
          r.c.nlog == 0);
    CHECK(!t.modified && !t.write_protect && t.mmusr == 0);
    e = atc_entry(&r);
    CHECK(e && e->history == r.cpu->atc040_clock && e->history > first);

    const uint32_t clock = r.cpu->atc040_clock;
    t = walk(&r, &WRITE_SUPER,
             MC68K_TRANSLATE_DEBUG); /* a debug hit neither refreshes nor invalidates */
    CHECK(t.ok && t.source == MC68K_TRANSLATE_ATC && r.c.nlog == 0 &&
          r.cpu->atc040_clock == clock);
    CHECK(atc_valid_entries(&r) == 1 && atc_entry(&r)->history == clock);

    t = walk(&r, &READ_USER, MC68K_TRANSLATE_EXECUTE); /* another fc has its own entry */
    CHECK(t.ok && t.source == MC68K_TRANSLATE_WALK && atc_valid_entries(&r) == 2);
    t = walk(&r, &READ_INSTR,
             MC68K_TRANSLATE_EXECUTE); /* the instruction ATC is a separate bank */
    CHECK(t.ok && t.source == MC68K_TRANSLATE_WALK && atc_valid_entries(&r) == 2);
    unsigned instruction_entries = 0;
    for (unsigned i = 0; i < MC68K_ATC_ENTRIES; i++)
        instruction_entries += r.cpu->atc040_i[i].valid;
    CHECK(instruction_entries == 1);
    t = walk(&r, &READ_INSTR, MC68K_TRANSLATE_EXECUTE);
    CHECK(t.ok && t.source == MC68K_TRANSLATE_ATC && r.c.nlog == 0);
    mc68k_destroy(r.cpu);

    /* The entry that has not been used for the longest time is replaced, and a hit counts as a
     * use. */
    current_case = "ATC replacement";
    rig_init(&r);
    map(&r, DESC_U, DESC_U, 0);
    for (unsigned i = 0; i <= MC68K_ATC_ENTRIES; i++)
        pl(&r.c, LEAF + i * 4u, leaf_desc(PHYS_OLD + i * 0x1000u, DESC_U));
    for (unsigned i = 0; i < MC68K_ATC_ENTRIES; i++) {
        t = walk_at(&r, LOGICAL + i * 0x1000u, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
        CHECK(t.ok && t.source == MC68K_TRANSLATE_WALK);
    }
    CHECK(atc_valid_entries(&r) == MC68K_ATC_ENTRIES);
    t = walk_at(&r, LOGICAL, &READ_SUPER, MC68K_TRANSLATE_EXECUTE); /* refresh page 0 */
    CHECK(t.source == MC68K_TRANSLATE_ATC);
    t = walk_at(&r, LOGICAL + MC68K_ATC_ENTRIES * 0x1000u, &READ_SUPER,
                MC68K_TRANSLATE_EXECUTE);
    CHECK(t.ok && t.source == MC68K_TRANSLATE_WALK &&
          atc_valid_entries(&r) == MC68K_ATC_ENTRIES);
    t = walk_at(&r, LOGICAL, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(t.source == MC68K_TRANSLATE_ATC);
    t = walk_at(&r, LOGICAL + MC68K_ATC_ENTRIES * 0x1000u, &READ_SUPER,
                MC68K_TRANSLATE_EXECUTE);
    CHECK(t.source == MC68K_TRANSLATE_ATC);
    t = walk_at(&r, LOGICAL + 0x1000u, &READ_SUPER,
                MC68K_TRANSLATE_EXECUTE); /* page 1 was the oldest */
    CHECK(t.ok && t.source == MC68K_TRANSLATE_WALK);
    mc68k_destroy(r.cpu);
}
/* #103 B (C-N28): the locked RMW ORs the bits into the bus word, writes and returns the whole
 * 32-bit result, and reports the failing address without touching the result. */
static void test_set_bits_direct(void) {
    current_case = "table_set_bits";
    rig_t r;
    rig_init(&r);
    uint32_t out = 0, failed = 0;
    pl(&r.c, LEAF, UINT32_C(0xa5c3e0c1));
    r.c.nlog = 0;
    CHECK(cpu_040_table_set_bits(r.cpu, LEAF, DESC_U | DESC_M, &out, &failed));
    CHECK(out == UINT32_C(0xa5c3e0d9) && gl(&r.c, LEAF) == UINT32_C(0xa5c3e0d9));
    CHECK(count_log(&r, 'R', LEAF) == 1 && count_log(&r, 'W', LEAF) == 1 && count_rmw(&r) == 2);
    out = 0;
    CHECK(cpu_040_table_set_bits(r.cpu, LEAF, 0, &out, &failed) &&
          out == UINT32_C(0xa5c3e0d9)); /* no bits: unchanged */
    pl(&r.c, LEAF, UINT32_C(0xffff0000));
    CHECK(cpu_040_table_set_bits(r.cpu, LEAF, DESC_U, &out, &failed));
    CHECK(out == UINT32_C(0xffff0008) && gl(&r.c, LEAF) == UINT32_C(0xffff0008));
    r.c.fail_read_at = LEAF;
    out = 0x1234;
    failed = 0;
    CHECK(!cpu_040_table_set_bits(r.cpu, LEAF, DESC_U, &out, &failed) && failed == LEAF &&
          out == 0x1234);
    r.c.fail_read_at = UINT32_MAX;
    r.c.fail_write_at = LEAF;
    failed = 0;
    CHECK(!cpu_040_table_set_bits(r.cpu, LEAF, DESC_U, &out, &failed) && failed == LEAF &&
          out == 0x1234);
    mc68k_destroy(r.cpu);

    current_case = "walk with a descriptor using the top bits";
    rig_init(&r);
    map(&r, DESC_U, DESC_U, leaf_desc(UINT32_C(0xa5c3e000), 0));
    mc68k_translate_result_t t = walk(&r, &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
    CHECK(t.ok && t.physical == UINT32_C(0xa5c3e000) &&
          gl(&r.c, LEAF) == leaf_desc(UINT32_C(0xa5c3e000), DESC_U));
    mc68k_destroy(r.cpu);
}

/* #103 B (C-N31): the supervisor accesses (fc 5, 6) use SRP, the user accesses (fc 1, 2) use
 * URP. */
static void test_srp_urp_selection(void) {
    static const mc68k_access_t READ_USER_INSTR = {
        .fc = 2, .read = true, .size = 4, .instruction = true};
    enum { EMPTY_ROOT = 0x2200 };
    for (unsigned swap = 0; swap < 2; swap++) {
        current_case = swap ? "URP maps, SRP empty" : "SRP maps, URP empty";
        rig_t r;
        rig_init(&r);
        map(&r, DESC_U, DESC_U, leaf_desc(PHYS_OLD, DESC_U));
        r.cpu->mmu040_srp = swap ? EMPTY_ROOT : ROOT;
        r.cpu->mmu040_urp = swap ? ROOT : EMPTY_ROOT;
        const mc68k_access_t *super[] = {&READ_SUPER, &READ_INSTR},
                             *user[] = {&READ_USER, &READ_USER_INSTR};
        for (unsigned i = 0; i < 2; i++) {
            mc68k_translate_result_t t = walk(&r, super[i], MC68K_TRANSLATE_EXECUTE);
            CHECK(swap ? (!t.ok && t.fault == MMU_FAULT_INVALID)
                       : (t.ok && t.physical == PHYS_OLD));
            t = walk(&r, user[i], MC68K_TRANSLATE_EXECUTE);
            CHECK(swap ? (t.ok && t.physical == PHYS_OLD)
                       : (!t.ok && t.fault == MMU_FAULT_INVALID));
        }
        mc68k_destroy(r.cpu);
    }
}

/* #103 B (C-N26): a table access never advances the D-cache replacement pointer (the 040 does
 * not define its replacement; the model only promises that table searches leave it alone). */
static void test_table_access_keeps_replacement(void) {
    const unsigned set = ((LEAF & ~UINT32_C(15)) >> 4) & (MC68K_040_CACHE_SETS - 1u);
    for (unsigned scenario = 0; scenario < 4; scenario++) {
        current_case = "table access and dcache040_next";
        rig_t r;
        rig_init(&r);
        map(&r, DESC_U, DESC_U, leaf_desc(PHYS_OLD, scenario == 2 ? 0 : DESC_U));
        if (scenario == 0)
            cache_set(&r, LEAF, leaf_desc(PHYS_NEW, DESC_U), true); /* read hit */
        if (scenario == 2)
            cache_set(&r, LEAF, leaf_desc(PHYS_NEW, 0),
                      true); /* locked RMW: push, invalidate */
        if (scenario == 3)
            cache_set(&r, LEAF, leaf_desc(PHYS_OLD, DESC_U), false); /* simple write hit */
        /* scenario 1 is a read miss */
        for (unsigned s = 0; s < MC68K_040_CACHE_SETS; s++)
            r.cpu->dcache040_next[s] = (uint8_t)(s % 3u + 1u);
        uint8_t before[MC68K_040_CACHE_SETS];
        memcpy(before, r.cpu->dcache040_next, sizeof(before));
        CHECK(before[set] != 0);
        mc68k_translate_result_t t =
            walk(&r, scenario == 3 ? &WRITE_SUPER : &READ_SUPER, MC68K_TRANSLATE_EXECUTE);
        CHECK(t.ok);
        CHECK(memcmp(before, r.cpu->dcache040_next, sizeof(before)) == 0);
        mc68k_destroy(r.cpu);
    }
}
int main(void) {
    test_table_read_hit();
    test_table_read_miss();
    test_u_update_rmw_dirty();
    test_u_update_rmw_clean();
    test_u_update_rmw_stale_clean();
    test_simple_write_hit();
    test_wp_leaf_u_update();
    test_parent_wp_leaf_u_update();
    test_push_failure_ptest();
    test_rmw_write_failure();
    test_debug_walk();
    test_unloaded_line();
    test_u_write_order();
    test_u_write_faults();
    test_u_write_cases();
    test_u_write_on_read_failure();
    test_table_3_1_cells();
    test_cached_descriptor_offset();
    test_leaf_update_failures();
    test_cpush_failure_address();
    test_rmw_push_longwords();
    test_table_read_hit_pload();
    test_simple_write_hit_clean();
    test_fault_entry_with_wp();
    test_atc_entry_bits();
    test_de_disabled();
    test_de_disabled_updates();
    test_wp_write_modes();
    test_supervisor_violation();
    test_indirect_descriptor();
    test_wide_addresses();
    test_atc_entry_non_wp_faults();
    test_wp_fault_mmusr();
    test_wp_fault_cache_mode();
    test_table_line_ways();
    test_rmw_result_fields();
    test_u_write_value_on_read_failure();
    test_indirect_variants();
    test_descriptor_types_and_masks();
    test_page_8k();
    test_atc_paths();
    test_set_bits_direct();
    test_srp_urp_selection();
    test_table_access_keeps_replacement();
    if (failures)
        fprintf(stderr, "%u check(s) failed\n", failures);
    return failures ? 1 : 0;
}
