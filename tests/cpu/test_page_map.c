/*
 * 物理ページ→配列ポインタ表（mc68k_page_map_t、bus_hooks.h）の差分試験（#133）。
 * 設計書: x68000 リポジトリ local の mc68000_133_page_map_design（段階 A:
 * 読み出しとウェイトの課金）と mc68000_133_stage_b_design（段階 B: 書き込み）。
 *
 * 同じプログラムを「表あり」と「表なし」で実行し、各命令後のレジスタ・消費サイクル・メモリが
 * 一致することを確認する。あわせて、表ありで残ったコールバック列が、表なしの列から
 * 「表が肩代わりしたもの」を除いた列と一致することを確認する。
 *   - 直接読みできるページ（READ_DIRECT）への read_* は呼ばれない。
 *   - 直接書けるページ（WRITE_DIRECT）への write_* は呼ばれない（段階 B）。
 *   - TIMING のあるページへの access_timing は呼ばれない。
 *   -
 * スーパーバイザ専用ページ、表に載らないページ、ページをまたぐアクセスは従来どおり呼ばれる。
 * 表の値を 1 つずらすとサイクルが変わり、試験が落ちること（変異試験）も確かめる。
 */
#include "mc68000/cpu.h"
#include "cpu/cpu_internal.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    RAM_SIZE = 0x10000,
    PAGE_COUNT = RAM_SIZE >> MC68K_PAGE_SHIFT,
    SSP_INIT = 0x7f00,
    USP_INIT = 0x7e00,
    PROG_PC = 0x400,
    HANDLER_PC = 0x600,
    VEC_BUS_ERROR = 2,
    VEC_ADDR_ERROR = 3,
    VEC_TRAP0 = 32,
    MAX_STEPS = 400,
    MAX_LOG = 4096,
    SR_SUPER_MASK = 0x2700,
    SR_USER_VALUE = 0x0000,
    PAGE_DIRECT_FIRST = 0,
    PAGE_DIRECT_LAST = 9,
    PAGE_TIMING_ONLY = 10,
    PAGE_SUPER_ONLY = 11,
    PAGE_NO_ENTRY = 12,
    PAGE_GAP = 15,
};

/* ページ種別ごとのウェイト（access_timing と表の両方がこの関数を使う）。 */
typedef struct {
    uint16_t read, write, read_long, write_long;
    bool cacheable;
} wait_def_t;

static wait_def_t wait_of_page(unsigned page) {
    if (page == 8 || page == 9)
        return (wait_def_t){4, 0, 5, 0, true};
    if (page >= PAGE_NO_ENTRY && page < PAGE_GAP)
        return (wait_def_t){7, 6, 9, 8, false};
    if (page == PAGE_GAP)
        return (wait_def_t){0, 0, 0, 0, false};
    return (wait_def_t){2, 1, 3, 2, false};
}

enum { KIND_RB, KIND_RW, KIND_RL, KIND_WB, KIND_WW, KIND_WL, KIND_TIMING };
typedef struct {
    uint8_t kind;
    uint32_t addr;
    uint32_t value;
} log_t;

typedef struct {
    uint8_t ram[RAM_SIZE];
    log_t log[MAX_LOG];
    unsigned log_count;
} ctx_t;

static void note(ctx_t *c, uint8_t kind, uint32_t addr, uint32_t value) {
    assert(c->log_count < MAX_LOG);
    c->log[c->log_count++] = (log_t){kind, addr, value};
}
static bool in_ram(uint32_t addr, unsigned size) {
    return addr + size <= RAM_SIZE && (addr >> MC68K_PAGE_SHIFT) != PAGE_GAP &&
           ((addr + size - 1u) >> MC68K_PAGE_SHIFT) != PAGE_GAP;
}
static bool rb(void *p, uint32_t a, uint8_t *v) {
    ctx_t *c = p;
    note(c, KIND_RB, a, 0);
    if (!in_ram(a, 1))
        return false;
    *v = c->ram[a];
    return true;
}
static bool rw(void *p, uint32_t a, uint16_t *v) {
    ctx_t *c = p;
    note(c, KIND_RW, a, 0);
    if (!in_ram(a, 2))
        return false;
    *v = (uint16_t)((c->ram[a] << 8) | c->ram[a + 1]);
    return true;
}
static bool rl(void *p, uint32_t a, uint32_t *v) {
    ctx_t *c = p;
    note(c, KIND_RL, a, 0);
    if (!in_ram(a, 4))
        return false;
    *v = ((uint32_t)c->ram[a] << 24) | ((uint32_t)c->ram[a + 1] << 16) |
         ((uint32_t)c->ram[a + 2] << 8) | c->ram[a + 3];
    return true;
}
static bool wb(void *p, uint32_t a, uint8_t v) {
    ctx_t *c = p;
    note(c, KIND_WB, a, v);
    if (!in_ram(a, 1))
        return false;
    c->ram[a] = v;
    return true;
}
static bool ww(void *p, uint32_t a, uint16_t v) {
    ctx_t *c = p;
    note(c, KIND_WW, a, v);
    if (!in_ram(a, 2))
        return false;
    c->ram[a] = (uint8_t)(v >> 8);
    c->ram[a + 1] = (uint8_t)v;
    return true;
}
static bool wl(void *p, uint32_t a, uint32_t v) {
    ctx_t *c = p;
    note(c, KIND_WL, a, v);
    if (!in_ram(a, 4))
        return false;
    for (unsigned i = 0; i < 4; i++)
        c->ram[a + i] = (uint8_t)(v >> (24u - 8u * i));
    return true;
}
static void timing(void *p, const mc68k_bus_access_t *access, mc68k_bus_timing_t *out) {
    ctx_t *c = p;
    note(c, KIND_TIMING, access->physical,
         (uint32_t)access->size | (access->read ? 0x100u : 0u));
    out->wait_cycles = 0;
    out->cacheable = false;
    if (access->fc == MC68K_FC_CPU_SPACE)
        return;
    const wait_def_t w = wait_of_page((access->physical >> MC68K_PAGE_SHIFT) % PAGE_COUNT);
    if (access->size == 4)
        out->wait_cycles = access->read ? w.read_long : w.write_long;
    else
        out->wait_cycles = access->read ? w.read : w.write;
    out->cacheable = w.cacheable;
}

/* ---- 表 ---- */

typedef struct {
    mc68k_page_map_t map;
    mc68k_page_entry_t l2[MC68K_PAGE_L2_COUNT];
    ctx_t *ctx;
} table_t;

/* 変異の種類。 */
enum { MUT_NONE = 0, MUT_WAIT = 1, MUT_HOST = 2, MUT_NO_SUPER_ONLY = 3 };

static void build_table(table_t *t, ctx_t *c, int mutation) {
    memset(t, 0, sizeof *t);
    t->ctx = c;
    /* 同じウェイトの組を 1 つの class に集める。 */
    unsigned classes = 0;
    uint8_t class_of_page[PAGE_COUNT];
    for (unsigned page = 0; page < PAGE_COUNT; page++) {
        const wait_def_t w = wait_of_page(page);
        unsigned k;
        for (k = 0; k < classes; k++) {
            const mc68k_page_wait_t *e = &t->map.wait_table[k];
            if (e->read == w.read && e->write == w.write && e->read_long == w.read_long &&
                e->write_long == w.write_long)
                break;
        }
        if (k == classes) {
            assert(classes < MC68K_PAGE_WAIT_CLASSES);
            t->map.wait_table[classes++] =
                (mc68k_page_wait_t){w.read, w.write, w.read_long, w.write_long};
        }
        class_of_page[page] = (uint8_t)k;
    }
    for (unsigned page = 0; page < PAGE_COUNT; page++) {
        mc68k_page_entry_t *e = &t->l2[page];
        if (page >= PAGE_NO_ENTRY)
            continue; /* 表に載らない（ページ 15 は隙間でもある） */
        const wait_def_t w = wait_of_page(page);
        e->wait_class = class_of_page[page];
        e->flags = MC68K_PAGE_TIMING | (w.cacheable ? MC68K_PAGE_CACHEABLE : 0u);
        if (page <= PAGE_DIRECT_LAST || page == PAGE_SUPER_ONLY) {
            e->flags |= MC68K_PAGE_READ_DIRECT | MC68K_PAGE_WRITE_DIRECT;
            e->host = c->ram + (page << MC68K_PAGE_SHIFT);
        }
        if (page == PAGE_SUPER_ONLY && mutation != MUT_NO_SUPER_ONLY)
            e->flags |= MC68K_PAGE_SUPERVISOR_ONLY;
    }
    t->map.l1[0] = t->l2;
    if (mutation == MUT_HOST) {
        /* 変異: ページ 2 の host を隣のページへずらす（書き込みが別の場所へ入る） */
        t->l2[2].host = c->ram + (3u << MC68K_PAGE_SHIFT);
    }
    if (mutation == MUT_WAIT) {
        /* 変異: ページ 1 と同じ class の読み出しウェイト（キャッシュ有効時は充填の long
         * を通る） */
        t->map.wait_table[class_of_page[1]].read += 1;
        t->map.wait_table[class_of_page[1]].read_long += 1;
    }
}

/* 実行中のプログラムが、スーパーバイザでページ 11（SUPERVISOR_ONLY）を触るか。
 * 真のとき、ページ 11 の直接読み書きも表が肩代わりする。 */
static bool g_page11_direct;

/* 表が肩代わりするコールバックか。 */
static bool covered_by_table(const log_t *e, bool supervisor_unknown) {
    (void)supervisor_unknown;
    const unsigned page = (e->addr >> MC68K_PAGE_SHIFT) % PAGE_COUNT;
    if (e->kind == KIND_TIMING) {
        const unsigned size = e->value & 0xffu;
        if ((e->addr & MC68K_PAGE_MASK) + size > MC68K_PAGE_SIZE)
            return false;
        return page < PAGE_NO_ENTRY;
    }
    unsigned size = 0;
    switch (e->kind) {
    case KIND_RB:
    case KIND_WB:
        size = 1u;
        break;
    case KIND_RW:
    case KIND_WW:
        size = 2u;
        break;
    case KIND_RL:
    case KIND_WL:
        size = 4u;
        break;
    default:
        return false;
    }
    if ((e->addr & MC68K_PAGE_MASK) + size > MC68K_PAGE_SIZE)
        return false;
    return page <= PAGE_DIRECT_LAST || (page == PAGE_SUPER_ONLY && g_page11_direct);
}

/* ---- プログラム ---- */

typedef struct {
    uint16_t w[128];
    unsigned n;
} prog_t;
static void emit(prog_t *p, uint16_t v) {
    assert(p->n < sizeof p->w / sizeof p->w[0]);
    p->w[p->n++] = v;
}
static void emit_long(prog_t *p, uint32_t v) {
    emit(p, (uint16_t)(v >> 16));
    emit(p, (uint16_t)v);
}
static void move_w_abs(prog_t *p, unsigned dn, uint32_t addr) {
    emit(p, (uint16_t)(0x3039u | (dn << 9)));
    emit_long(p, addr);
}
static void move_l_abs(prog_t *p, unsigned dn, uint32_t addr) {
    emit(p, (uint16_t)(0x2039u | (dn << 9)));
    emit_long(p, addr);
}
static void move_b_abs(prog_t *p, unsigned dn, uint32_t addr) {
    emit(p, (uint16_t)(0x1039u | (dn << 9)));
    emit_long(p, addr);
}
static void add_l_abs(prog_t *p, unsigned dn, uint32_t addr) {
    emit(p, (uint16_t)(0xd0b9u | (dn << 9)));
    emit_long(p, addr);
}
static void move_w_to_abs(prog_t *p, unsigned dn, uint32_t addr) {
    emit(p, (uint16_t)(0x33c0u | dn));
    emit_long(p, addr);
}
static void move_l_to_abs(prog_t *p, unsigned dn, uint32_t addr) {
    emit(p, (uint16_t)(0x23c0u | dn));
    emit_long(p, addr);
}
static void move_b_to_abs(prog_t *p, unsigned dn, uint32_t addr) {
    emit(p, (uint16_t)(0x13c0u | dn));
    emit_long(p, addr);
}
static void tas_abs(prog_t *p, uint32_t addr) {
    emit(p, 0x4af9);
    emit_long(p, addr);
}
static void movem_l_abs(prog_t *p, uint16_t mask, uint32_t addr) {
    emit(p, 0x4cf9);
    emit(p, mask);
    emit_long(p, addr);
}
static void stop(prog_t *p) {
    emit(p, 0x4e72);
    emit(p, SR_SUPER_MASK);
}

enum {
    DATA_P1 = 0x1000,
    DATA_STRADDLE = 0x1ffe,
    DATA_P9 = 0x9000,
    DATA_P10 = 0xa000,
    DATA_P11 = 0xb000,
    DATA_P11_STRADDLE = 0xbffe,
    DATA_P12 = 0xc000,
    DATA_GAP = 0xf000,
    DATA_WRITE = 0x2000,
    DATA_W1 = 0x1100,
    DATA_W9 = 0x9010,
    DATA_W10 = 0xa010,
    DATA_W12 = 0xc010,
    DATA_TAS = 0x2100,
    DATA_ODD = 0x1001
};

/* スーパーバイザで、直接読み・ページまたぎ・キャッシュ可ページ・表なしページ・書き込みを巡る。
 */
static void prog_super(prog_t *p) {
    for (unsigned pass = 0; pass < 2; pass++) { /* 2 周目はキャッシュにヒットしうる */
        move_w_abs(p, 0, DATA_P1);
        move_l_abs(p, 1, DATA_P1 + 4);
        move_b_abs(p, 2, DATA_P1 + 9);
        move_l_abs(p, 3, DATA_STRADDLE);
        move_w_abs(p, 4, DATA_P9);
        move_l_abs(p, 5, DATA_P9 + 8);
        move_w_abs(p, 6, DATA_P10);
        move_l_abs(p, 7, DATA_P12);
        add_l_abs(p, 0, DATA_P1 + 12);
        movem_l_abs(p, 0x000f, DATA_P1 + 0x100);
        move_w_to_abs(p, 0, DATA_WRITE);
        move_l_to_abs(p, 1, DATA_WRITE + 4);
        move_w_abs(p, 2, DATA_WRITE);
    }
    stop(p);
}

/* ユーザモードへ切り替え、スーパーバイザ専用ページと表なしページを読み、隙間でバスエラー。 */
static void prog_user(prog_t *p) {
    emit(p, 0x46fc); /* MOVE #$0000,SR（ユーザモードへ） */
    emit(p, SR_USER_VALUE);
    move_w_abs(p, 0, DATA_P11);
    move_l_abs(p, 1, DATA_P11 + 4);
    move_l_abs(p, 2, DATA_P11_STRADDLE);
    move_w_abs(p, 3, DATA_P1); /* ユーザでも直接読めるページ */
    move_w_abs(p, 4, DATA_P12);
    move_w_abs(p, 5, DATA_GAP); /* バスエラー → ハンドラの STOP */
    stop(p);
}

/* 68020 以降: 奇数アドレスの word/long（byte 片に分かれる）。 */
static void prog_odd(prog_t *p) {
    move_w_abs(p, 0, DATA_ODD);
    move_l_abs(p, 1, DATA_ODD + 8);
    move_b_abs(p, 2, DATA_ODD);
    stop(p);
}

/* スーパーバイザで、直接書き（byte/word/long、キャッシュ可ページ）・ページまたぎ・
 * 表なしページ・TAS・書いた直後の読み戻しを巡る。最後に隙間への書き込みでバスエラー。 */
static void prog_write(prog_t *p) {
    for (unsigned pass = 0; pass < 2; pass++) {
        move_w_abs(p, 0, DATA_P1);
        move_l_abs(p, 1, DATA_P1 + 4);
        move_b_to_abs(p, 0, DATA_W1 + 1);
        move_w_to_abs(p, 0, DATA_W1 + 2);
        move_l_to_abs(p, 1, DATA_W1 + 4);
        move_w_abs(p, 2, DATA_W1 + 2); /* 書いた直後の読み戻し */
        move_l_abs(p, 3, DATA_W1 + 4);
        move_l_to_abs(p, 1, DATA_STRADDLE); /* ページ 1 と 2 をまたぐ */
        move_l_abs(p, 4, DATA_STRADDLE);
        move_w_to_abs(p, 1, DATA_W9);
        move_l_to_abs(p, 0, DATA_W9 + 4);
        move_l_abs(p, 5, DATA_W9 + 4);
        move_w_to_abs(p, 2, DATA_W10); /* TIMING だけのページ（コールバック書き） */
        move_l_to_abs(p, 3, DATA_W10 + 4);
        move_b_to_abs(p, 4, DATA_W12); /* 表なしページ */
        move_l_to_abs(p, 5, DATA_W12 + 4);
        tas_abs(p, DATA_TAS);
        move_b_abs(p, 6, DATA_TAS);
        move_l_to_abs(p, 2, DATA_WRITE);
        move_l_abs(p, 7, DATA_WRITE);
    }
    move_w_to_abs(p, 1, DATA_GAP); /* バスエラー → ハンドラの STOP */
    stop(p);
}

/* スーパーバイザで SUPERVISOR_ONLY ページ（11）へ読み書きする。ページ 11→12 をまたぐ long は
 * コールバックに落ちる。 */
static void prog_write_p11(prog_t *p) {
    move_w_abs(p, 0, DATA_P1);
    move_l_abs(p, 1, DATA_P1 + 4);
    move_w_to_abs(p, 0, DATA_P11);
    move_l_to_abs(p, 1, DATA_P11 + 4);
    move_b_to_abs(p, 0, DATA_P11 + 9);
    move_l_abs(p, 2, DATA_P11 + 4);
    move_l_to_abs(p, 1, DATA_P11_STRADDLE);
    move_l_abs(p, 3, DATA_P11_STRADDLE);
    stop(p);
}

/* ユーザモードで書く。直接書けるページは直接、SUPERVISOR_ONLY（ページ 11）と表なしは
 * コールバックに落ちる（保護しないホストでは書かれる）。最後は隙間でバスエラー。 */
static void prog_user_write(prog_t *p) {
    emit(p, 0x46fc); /* MOVE #$0000,SR（ユーザモードへ） */
    emit(p, SR_USER_VALUE);
    move_w_abs(p, 0, DATA_P1);
    move_l_abs(p, 1, DATA_P1 + 4);
    move_w_to_abs(p, 0, DATA_W1);
    move_l_to_abs(p, 1, DATA_W1 + 4);
    move_w_to_abs(p, 0, DATA_P11);
    move_l_to_abs(p, 1, DATA_P11 + 4);
    move_b_to_abs(p, 0, DATA_W12);
    move_w_abs(p, 2, DATA_P11);
    move_w_to_abs(p, 1, DATA_GAP); /* バスエラー → ハンドラの STOP */
    stop(p);
}

/* 68020 以降: 奇数アドレスの word/long 書き込み（byte 片に分かれる）。 */
static void prog_odd_write(prog_t *p) {
    move_w_abs(p, 0, DATA_ODD);
    move_l_abs(p, 1, DATA_ODD + 8);
    move_w_to_abs(p, 0, DATA_ODD + 0x100);
    move_l_to_abs(p, 1, DATA_ODD + 0x108);
    move_b_to_abs(p, 1, DATA_ODD + 0x110);
    stop(p);
}

typedef struct {
    mc68k_regs_t regs[MAX_STEPS];
    int cycles[MAX_STEPS];
    unsigned steps;
    uint8_t ram[RAM_SIZE];
    log_t log[MAX_LOG];
    unsigned log_count;
} result_t;

static void put_word(ctx_t *c, uint32_t a, uint16_t v) {
    c->ram[a] = (uint8_t)(v >> 8);
    c->ram[a + 1] = (uint8_t)v;
}
static void put_long(ctx_t *c, uint32_t a, uint32_t v) {
    put_word(c, a, (uint16_t)(v >> 16));
    put_word(c, a + 2, (uint16_t)v);
}

static void run(mc68k_model_t model, bool caches, void (*build)(prog_t *), bool use_table,
                int mutation, result_t *out) {
    static ctx_t ctx;
    static table_t table;
    memset(&ctx, 0, sizeof ctx);
    for (uint32_t a = 0; a < RAM_SIZE; a++)
        ctx.ram[a] = (uint8_t)(a * 7u + (a >> 8) * 13u + 1u);
    put_long(&ctx, 0, SSP_INIT);
    put_long(&ctx, 4, PROG_PC);
    put_long(&ctx, VEC_BUS_ERROR * 4, HANDLER_PC);
    put_long(&ctx, VEC_ADDR_ERROR * 4, HANDLER_PC);
    put_long(&ctx, VEC_TRAP0 * 4, HANDLER_PC);
    put_word(&ctx, HANDLER_PC, 0x4e72); /* STOP #$2700 */
    put_word(&ctx, HANDLER_PC + 2, SR_SUPER_MASK);
    prog_t prog;
    memset(&prog, 0, sizeof prog);
    build(&prog);
    for (unsigned i = 0; i < prog.n; i++)
        put_word(&ctx, PROG_PC + 2u * i, prog.w[i]);

    mc68k_bus_t bus = {.read_byte = rb,
                       .read_word = rw,
                       .read_long = rl,
                       .write_byte = wb,
                       .write_word = ww,
                       .write_long = wl,
                       .ctx = &ctx,
                       .access_timing = timing};
    if (use_table) {
        build_table(&table, &ctx, mutation);
        bus.page_map = &table.map;
    }
    mc68k_state_t *cpu = mc68k_create_model(&bus, model);
    assert(cpu);
    mc68k_reset(cpu);
    if (caches && (model == MC68K_MODEL_68040 || model == MC68K_MODEL_68060))
        cpu->cacr = MC68K_CACR_040_IE | MC68K_CACR_040_DE;
    ctx.log_count = 0; /* リセット時のベクタ読みは比べない */
    memset(&out->regs, 0, sizeof out->regs);
    out->steps = 0;
    while (out->steps < MAX_STEPS) {
        out->cycles[out->steps] = mc68k_execute(cpu, 1);
        mc68k_get_regs(cpu, &out->regs[out->steps]);
        out->steps++;
        if (mc68k_get_exec_state(cpu) != CPU_NORMAL)
            break;
    }
    assert(out->steps < MAX_STEPS);
    memcpy(out->ram, ctx.ram, sizeof out->ram);
    out->log_count = ctx.log_count;
    memcpy(out->log, ctx.log, sizeof(log_t) * ctx.log_count);
    mc68k_destroy(cpu);
}

static bool same_log(const log_t *a, const log_t *b) {
    return a->kind == b->kind && a->addr == b->addr && a->value == b->value;
}

/* 表あり／なしの結果が一致することを確かめる。一致すれば true。 */
static bool compare(const result_t *base, const result_t *tab, unsigned *covered) {
    if (base->steps != tab->steps)
        return false;
    for (unsigned i = 0; i < base->steps; i++) {
        if (base->cycles[i] != tab->cycles[i])
            return false;
        if (memcmp(&base->regs[i], &tab->regs[i], sizeof base->regs[i]) != 0)
            return false;
    }
    if (memcmp(base->ram, tab->ram, sizeof base->ram) != 0)
        return false;
    /* 表なしの列から肩代わり分を除くと、表ありの列に一致する。
     * スーパーバイザ専用ページ（11）と表なしページ（12 以降）の読み出しは除かれない。 */
    unsigned j = 0;
    *covered = 0;
    for (unsigned i = 0; i < base->log_count; i++) {
        if (covered_by_table(&base->log[i], false)) {
            (*covered)++;
            continue;
        }
        if (j >= tab->log_count || !same_log(&base->log[i], &tab->log[j]))
            return false;
        j++;
    }
    return j == tab->log_count;
}

static const mc68k_model_t models[] = {MC68K_MODEL_68000, MC68K_MODEL_68010, MC68K_MODEL_68020,
                                       MC68K_MODEL_68030, MC68K_MODEL_68040, MC68K_MODEL_68060};
static const char *const model_names[] = {"68000", "68010", "68020", "68030", "68040", "68060"};

static result_t base_r, tab_r;

typedef struct {
    const char *name;
    void (*build)(prog_t *);
    bool needs_020;
    bool page11_direct; /* スーパーバイザでページ 11 を触る（表が肩代わりする） */
    bool reads_page1;   /* MUT_WAIT が観測できる（ページ 1 を読む） */
    bool writes_page2;  /* MUT_HOST が観測できる（ページ 2 を読み書きする） */
    bool user_mode;     /* ユーザモードで走る（MUT_NO_SUPER_ONLY が観測できる） */
} program_t;

static void expect_mutation_detected(const program_t *pr, mc68k_model_t model,
                                     unsigned model_idx, unsigned caches, int mutation) {
    run(model, caches != 0, pr->build, true, mutation, &tab_r);
    unsigned c2 = 0;
    const bool still_equal = compare(&base_r, &tab_r, &c2);
    if (still_equal)
        fprintf(stderr, "MUTATION SURVIVED: %s model=%s caches=%u mutation=%d\n", pr->name,
                model_names[model_idx], caches, mutation);
    assert(!still_equal);
}

static void check_program(const program_t *pr) {
    g_page11_direct = pr->page11_direct;
    for (unsigned m = 0; m < sizeof models / sizeof models[0]; m++) {
        if (pr->needs_020 && (models[m] == MC68K_MODEL_68000 || models[m] == MC68K_MODEL_68010))
            continue;
        for (unsigned caches = 0; caches < 2; caches++) {
            run(models[m], caches != 0, pr->build, false, MUT_NONE, &base_r);
            run(models[m], caches != 0, pr->build, true, MUT_NONE, &tab_r);
            unsigned covered = 0;
            const bool ok = compare(&base_r, &tab_r, &covered);
            if (!ok)
                fprintf(stderr, "MISMATCH: %s model=%s caches=%u\n", pr->name, model_names[m],
                        caches);
            assert(ok);
            assert(covered > 0); /* 表が実際に何かを肩代わりしている */
            /* 変異: 表の値をずらすと、サイクルや RAM が変わって差分が出る。 */
            if (pr->reads_page1)
                expect_mutation_detected(pr, models[m], m, caches, MUT_WAIT);
            if (pr->writes_page2)
                expect_mutation_detected(pr, models[m], m, caches, MUT_HOST);
            /* ユーザモードで SUPERVISOR_ONLY
             * ページを触るプログラムは、フラグを外す変異が見える。 */
            if (pr->user_mode)
                expect_mutation_detected(pr, models[m], m, caches, MUT_NO_SUPER_ONLY);
        }
    }
}

int main(void) {
    static const program_t programs[] = {
        {"super", prog_super, false, false, true, true, false},
        {"user", prog_user, false, false, true, false, true},
        {"odd", prog_odd, true, false, false, false, false},
        {"write", prog_write, false, false, true, true, false},
        {"write_p11", prog_write_p11, false, true, true, false, false},
        {"user_write", prog_user_write, false, false, true, false, true},
        {"odd_write", prog_odd_write, true, false, false, false, false},
    };
    for (unsigned i = 0; i < sizeof programs / sizeof programs[0]; i++)
        check_program(&programs[i]);
    printf("test_page_map: OK\n");
    return 0;
}
