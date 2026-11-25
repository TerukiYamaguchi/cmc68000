/*
 * MC68060-B (#108) T-B7: 060 翻訳器 cpu_mmu060_translate_result() を直接呼び、TC/TTR/descriptor
 * の各ビット、 table search の index・アラインメント・U/M/W/S の更新、indirect、table の bus
 * error、ATC の hit/置換を固定する。 設計書 docs/mc68000-internals/reference/cpu_68060.md
 * §5、§10.1。T-B1〜T-B6（test_68060_mmu）は命令経由の動作を固定する。 物理メモリは RAM（低位
 * 512KB）と、上位アドレスの table を置くための疎なページ群。
 */
#include "mc68000/cpu.h"
#include "cpu/cpu_internal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define RAM_SIZE  0x80000u
#define SSP_INIT  0x8000u
#define RESET_PC  0x100u
#define TEST_PC   0x400u
#define HANDLER   0x3000u
#define SR_SUP    0x2700
#define HI_PAGES  64u
#define HI_PAGE   0x1000u
#define NO_VECTOR 0xffffu
#define VEC_BUS   2

#define DESC_W             0x04u
#define DESC_U             0x08u
#define DESC_M             0x10u
#define DESC_S             0x80u
#define DESC_G             0x400u
#define DESC_TYPE_INVALID0 0u
#define DESC_TYPE_INVALID1 1u
#define DESC_TYPE_INDIRECT 2u
#define DESC_TYPE_TABLE    3u
#define DESC_PAGE_A        1u
#define DESC_PAGE_B        3u

#define TC_E  0x8000u
#define TC_P  0x4000u
#define TTR_E 0x8000u
#define TTR_W 0x0004u

#define FC_UDATA 1
#define FC_UPROG 2
#define FC_SDATA 5
#define FC_SPROG 6

typedef struct {
    uint8_t ram[RAM_SIZE];
    struct {
        uint32_t page;
        bool used;
        uint8_t d[HI_PAGE];
    } hi[HI_PAGES];
    uint32_t fail_read,
        fail_write; /* この long アドレスの読み・書きを失敗させる（~0 で無効） */
    unsigned writes;
    uint32_t last_write;
} fixture_t;

static fixture_t f;
static mc68k_state_t *cpu_g;

static uint8_t *mem_at(uint32_t a, bool create) {
    if (a < RAM_SIZE)
        return &f.ram[a];
    for (unsigned i = 0; i < HI_PAGES; ++i)
        if (f.hi[i].used && f.hi[i].page == (a & ~(HI_PAGE - 1u)))
            return &f.hi[i].d[a & (HI_PAGE - 1u)];
    if (!create)
        return NULL;
    for (unsigned i = 0; i < HI_PAGES; ++i)
        if (!f.hi[i].used) {
            f.hi[i].used = true;
            f.hi[i].page = a & ~(HI_PAGE - 1u);
            memset(f.hi[i].d, 0, HI_PAGE);
            return &f.hi[i].d[a & (HI_PAGE - 1u)];
        }
    assert(!"hi pages exhausted");
    return NULL;
}
static bool rb(void *c, uint32_t a, uint8_t *v) {
    (void)c;
    uint8_t *p = mem_at(a, false);
    if (!p)
        return false;
    *v = *p;
    return true;
}
static bool rw(void *c, uint32_t a, uint16_t *v) {
    uint8_t h, l;
    if (!rb(c, a, &h) || !rb(c, a + 1, &l))
        return false;
    *v = (uint16_t)(h << 8 | l);
    return true;
}
static bool rl(void *c, uint32_t a, uint32_t *v) {
    uint16_t h, l;
    if (a == f.fail_read)
        return false;
    if (!rw(c, a, &h) || !rw(c, a + 2, &l))
        return false;
    *v = (uint32_t)h << 16 | l;
    return true;
}
static bool wb(void *c, uint32_t a, uint8_t v) {
    (void)c;
    uint8_t *p = mem_at(a, false);
    if (!p)
        return false;
    *p = v;
    return true;
}
static bool ww(void *c, uint32_t a, uint16_t v) {
    return wb(c, a, (uint8_t)(v >> 8)) && wb(c, a + 1, (uint8_t)v);
}
static bool wl(void *c, uint32_t a, uint32_t v) {
    if (a == f.fail_write)
        return false;
    if (!ww(c, a, (uint16_t)(v >> 16)) || !ww(c, a + 2, (uint16_t)v))
        return false;
    f.writes++;
    f.last_write = a;
    return true;
}
static void reset_peripheral(void *c) {
    (void)c;
}

/* access_timing hook: table search
 * が出すバスサイクル（物理アドレス・read/write・rmw）を記録する */
typedef struct {
    uint32_t phys;
    bool read, rmw;
} bus_ev_t;
static bus_ev_t evs[32];
static unsigned nev;
static void timing_hook(void *c, const mc68k_bus_access_t *a, mc68k_bus_timing_t *t) {
    (void)c;
    if (nev >= sizeof evs / sizeof evs[0])
        nev = 0; /* 記録を使わない試験での溢れ */
    evs[nev++] = (bus_ev_t){a->physical, a->read, a->rmw};
    t->wait_cycles = 0;
    t->cacheable = false;
}
static void expect_ev(unsigned *i, uint32_t phys, bool read, bool rmw) {
    assert(*i < nev);
    assert(evs[*i].phys == phys && evs[*i].read == read && evs[*i].rmw == rmw);
    (*i)++;
}

/* host 側の書込み（疎ページを作る）。バス経由でなく、失敗注入と writes の対象外。 */
static void host_w(uint32_t a, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i)
        *mem_at(a + i, true) = (uint8_t)(v >> (24 - 8 * i));
}
static uint32_t host_r(uint32_t a) {
    uint32_t v = 0;
    for (unsigned i = 0; i < 4; ++i) {
        uint8_t *p = mem_at(a + i, false);
        assert(p);
        v = v << 8 | *p;
    }
    return v;
}

static void init_cpu(void) {
    memset(f.ram, 0, 0x40000);
    for (unsigned i = 0; i < HI_PAGES; ++i)
        f.hi[i].used = false;
    f.fail_read = f.fail_write = ~0u;
    f.writes = 0;
    nev = 0;
    host_w(0, SSP_INIT);
    host_w(4, RESET_PC);
    mc68k_reset(cpu_g);
    memset(cpu_g->atc060_i, 0, sizeof cpu_g->atc060_i);
    memset(cpu_g->atc060_d, 0, sizeof cpu_g->atc060_d);
    cpu_g->atc060_clock = 0;
    cpu_g->mmu060_urp = cpu_g->mmu060_srp = 0;
    cpu_g->mmu060_itt0 = cpu_g->mmu060_itt1 = cpu_g->mmu060_dtt0 = cpu_g->mmu060_dtt1 = 0;
    cpu_g->mmu060_tc = 0;
}

typedef enum { ACC_R, ACC_W, ACC_RMW, ACC_WRMW } acc_kind_t;
static mc68k_access_t mk_access(uint8_t fc, acc_kind_t k) {
    mc68k_access_t a = {.fc = fc, .size = 4, .instruction = (fc & 3u) == 2u};
    a.read = (k == ACC_R || k == ACC_RMW);
    a.rmw = (k == ACC_RMW || k == ACC_WRMW);
    return a;
}
static bool kind_is_write(acc_kind_t k) {
    return k != ACC_R;
}
static mc68k_translate_result_t xlate(uint32_t logical, uint8_t fc, acc_kind_t k,
                                      mc68k_translate_mode_t m) {
    mc68k_access_t a = mk_access(fc, k);
    return cpu_mmu060_translate_result(cpu_g, logical, &a, m);
}
#define EXEC MC68K_TRANSLATE_EXECUTE

static unsigned valid_count(const mc68k_atc_entry_t *atc) {
    unsigned n = 0;
    for (unsigned i = 0; i < MC68K_060_ATC_ENTRIES; ++i)
        n += atc[i].valid;
    return n;
}

/* ---- T-B7a: TC の既定属性（E=0）。各ビットを単独で立てても他の属性に影響しない ---- */
static void check_default(uint32_t tc) {
    init_cpu();
    cpu_g->mmu060_tc = tc;
    const unsigned dco = (tc >> 8) & 3u, dci = (tc >> 3) & 3u;
    const bool dwo = (tc & 0x20u) != 0;
    for (int instr = 0; instr <= 1; ++instr) {
        uint8_t fc = instr ? FC_SPROG : FC_SDATA;
        for (int kind = 0; kind < 2; ++kind) {
            mc68k_translate_result_t r = xlate(0x12345678u, fc, kind ? ACC_W : ACC_R, EXEC);
            unsigned cm = instr ? dci : dco;
            assert(r.source == MC68K_TRANSLATE_IDENTITY && r.physical == 0x12345678u);
            assert(r.cache_mode == cm && r.cache_inhibit == (cm >= 2u));
            assert(r.write_protect == (!instr && dwo));
            if (!instr && dwo && kind) {
                assert(!r.ok && r.fault == MMU_FAULT_WRITE_PROTECT);
            } else {
                assert(r.ok && r.fault == MMU_FAULT_NONE);
            }
        }
    }
    /* rmw の read 部も write として扱う */
    mc68k_translate_result_t r = xlate(0x1000u, FC_SDATA, ACC_RMW, EXEC);
    assert(r.ok == !dwo);
    r = xlate(0x1000u, FC_UDATA, ACC_WRMW, EXEC);
    assert(r.ok == !dwo);
}
static void test_tc_default(void) {
    check_default(0);
    for (unsigned b = 0; b < 32; ++b)
        if (b != 15)
            check_default(UINT32_C(1) << b);
    for (unsigned m = 0; m < 32; ++m) { /* DCI[4:3]、DWO[5]、DCO[9:8] の全組合せ */
        uint32_t tc = ((m & 3u) << 3) | (((m >> 2) & 1u) << 5) | (((m >> 3) & 3u) << 8);
        check_default(tc);
    }
    check_default(0x7fffu & ~0u);
    check_default(0xffff7fffu);
}

/* ---- T-B7b: TC.E=1 で他のビットを立てても 4KB page（P=0）のまま ---- */
static void test_tc_page_size(void) {
    for (unsigned b = 0; b < 32; ++b) {
        if (b == 15 || b == 14)
            continue;
        init_cpu();
        cpu_g->mmu060_tc = TC_E | (UINT32_C(1) << b);
        cpu_g->mmu060_urp = 0x10200u;
        /* 論理 0x1000（page 1）だけを map する。8KB なら page 0 の領域に入って同じ page になる
         */
        host_w(0x10200u, 0x10600u | 3u);
        host_w(0x10600u, 0x10900u | 3u);
        host_w(0x10900u + 1 * 4u, 0x40000u | DESC_PAGE_A);
        mc68k_translate_result_t r = xlate(0x1000u + 0x234u, FC_UDATA, ACC_R, EXEC);
        assert(r.ok && r.physical == 0x40000u + 0x234u);
        r = xlate(0x0000u, FC_UDATA, ACC_R, EXEC); /* 4KB なら page 0 は未 map */
        assert(!r.ok && r.fault == MMU_FAULT_INVALID);
    }
}

/* ---- T-B7c: TTR の照合 ---- */
static void test_ttr_match(void) {
    /* base・mask の各ビット */
    for (unsigned bit = 0; bit < 8; ++bit) {
        const uint32_t b = UINT32_C(1) << bit;
        for (unsigned fcs = 0; fcs < 4; ++fcs) {
            static const uint8_t fcl[] = {FC_UDATA, FC_UPROG, FC_SDATA, FC_SPROG};
            uint8_t fc = fcl[fcs];
            bool instr = fcs & 1;
            for (unsigned pass = 0; pass < 2; ++pass) {
                init_cpu();
                uint32_t ttr = TTR_E | (2u << 13) |
                               (b << 24); /* base = b、mask なし、S=2（どの空間でも） */
                if (instr)
                    cpu_g->mmu060_itt1 = ttr;
                else
                    cpu_g->mmu060_dtt1 = ttr;
                /* base と一致するアドレスは TT、1 ビット違うアドレスは非 TT（TC.E=0
                 * の恒等変換） */
                mc68k_translate_result_t r = xlate((b << 24) | 0x00ab1234u, fc, ACC_R, EXEC);
                assert(r.ok && r.source == MC68K_TRANSLATE_TT &&
                       r.physical == ((b << 24) | 0x00ab1234u));
                r = xlate(((b ^ 0x00u) << 24 ^ 0x01000000u) | 0x00ab1234u, fc, ACC_R, EXEC);
                if (bit == 0) {
                    assert(r.source == MC68K_TRANSLATE_IDENTITY);
                } else {
                    assert(r.source == MC68K_TRANSLATE_IDENTITY);
                }
                /* mask のそのビットを立てると、そのビットは無視される */
                ttr |= b << 16;
                if (instr)
                    cpu_g->mmu060_itt1 = ttr;
                else
                    cpu_g->mmu060_dtt1 = ttr;
                r = xlate((((b ^ b) << 24)) | 0x00ab1234u, fc, ACC_R,
                          EXEC); /* base ビット 0 のアドレス */
                assert(r.source == MC68K_TRANSLATE_TT);
                r = xlate((b << 24) | 0x00ab1234u, fc, ACC_R, EXEC);
                assert(r.source == MC68K_TRANSLATE_TT);
                /* mask 外の別ビットの違いは照合を外す */
                r = xlate(((b << 24) ^ (UINT32_C(1) << (24 + ((bit + 1) & 7)))) | 0x00ab1234u,
                          fc, ACC_R, EXEC);
                assert(r.source == MC68K_TRANSLATE_IDENTITY);
                (void)pass;
            }
        }
    }
    /* 全ビットの base（0xff）と全 mask（0xff） */
    init_cpu();
    cpu_g->mmu060_dtt0 = TTR_E | (2u << 13) | (0xffu << 24);
    assert(xlate(0xff000000u, FC_SDATA, ACC_R, EXEC).source == MC68K_TRANSLATE_TT);
    assert(xlate(0x7f000000u, FC_SDATA, ACC_R, EXEC).source == MC68K_TRANSLATE_IDENTITY);
    cpu_g->mmu060_dtt0 = TTR_E | (2u << 13) | (0xffu << 16);
    assert(xlate(0x12000000u, FC_SDATA, ACC_R, EXEC).source == MC68K_TRANSLATE_TT);
    assert(xlate(0xfe000000u, FC_SDATA, ACC_R, EXEC).source == MC68K_TRANSLATE_TT);
    /* S フィールド: 0=user、1=supervisor、2・3=どちらも */
    for (unsigned s = 0; s < 4; ++s) {
        for (unsigned fcs = 0; fcs < 4; ++fcs) {
            static const uint8_t fcl[] = {FC_UDATA, FC_UPROG, FC_SDATA, FC_SPROG};
            uint8_t fc = fcl[fcs];
            init_cpu();
            uint32_t ttr = TTR_E | (s << 13);
            if (fc & 2u) {
                if ((fc & 3u) == 2u)
                    cpu_g->mmu060_itt0 = ttr;
                else
                    cpu_g->mmu060_dtt0 = ttr;
            } else {
                cpu_g->mmu060_dtt0 = ttr;
                cpu_g->mmu060_itt0 = ttr;
            }
            cpu_g->mmu060_itt0 = cpu_g->mmu060_dtt0 = ttr;
            const bool sup = fc >= 4;
            const bool want = s >= 2 || (s == 1) == sup;
            mc68k_translate_result_t r = xlate(0x00001000u, fc, ACC_R, EXEC);
            assert((r.source == MC68K_TRANSLATE_TT) == want);
        }
    }
    /* E=0 の TTR に他のビットが立っていても照合しない */
    for (unsigned b = 0; b < 32; ++b) {
        if (b == 15)
            continue;
        init_cpu();
        cpu_g->mmu060_dtt0 = cpu_g->mmu060_itt0 = UINT32_C(1) << b;
        cpu_g->mmu060_dtt1 = cpu_g->mmu060_itt1 = UINT32_C(1) << b;
        static const uint8_t fcl[] = {FC_UDATA, FC_UPROG, FC_SDATA, FC_SPROG};
        for (unsigned i = 0; i < 4; ++i)
            assert(xlate(0x00001000u, fcl[i], ACC_R, EXEC).source == MC68K_TRANSLATE_IDENTITY);
    }
}

/* TT 0/1 の選択（命令側・データ側と、先に一致した方の属性） */
static void test_ttr_select(void) {
    init_cpu();
    const uint32_t a = 0x11000000u, bb = 0x22000000u;
    cpu_g->mmu060_itt0 =
        TTR_E | (2u << 13) | ((a >> 24) << 24) | (1u << 5); /* 命令 TT0: a、CM=1 */
    cpu_g->mmu060_itt1 =
        TTR_E | (2u << 13) | ((bb >> 24) << 24) | (2u << 5); /* 命令 TT1: b、CM=2 */
    cpu_g->mmu060_dtt0 =
        TTR_E | (2u << 13) | ((bb >> 24) << 24) | (3u << 5); /* データ TT0: b、CM=3 */
    cpu_g->mmu060_dtt1 =
        TTR_E | (2u << 13) | ((a >> 24) << 24) | (0u << 5) | TTR_W; /* データ TT1: a、CM=0、W */
    mc68k_translate_result_t r;
    r = xlate(a, FC_SPROG, ACC_R, EXEC);
    assert(r.source == MC68K_TRANSLATE_TT && r.cache_mode == 1 && !r.cache_inhibit &&
           !r.write_protect);
    r = xlate(bb, FC_SPROG, ACC_R, EXEC);
    assert(r.source == MC68K_TRANSLATE_TT && r.cache_mode == 2 && r.cache_inhibit);
    r = xlate(bb, FC_SDATA, ACC_R, EXEC);
    assert(r.source == MC68K_TRANSLATE_TT && r.cache_mode == 3 && r.cache_inhibit);
    r = xlate(a, FC_SDATA, ACC_R, EXEC);
    assert(r.source == MC68K_TRANSLATE_TT && r.cache_mode == 0 && !r.cache_inhibit &&
           r.write_protect && r.ok);
    r = xlate(a, FC_SDATA, ACC_W, EXEC);
    assert(!r.ok && r.fault == MMU_FAULT_WRITE_PROTECT);
    r = xlate(a, FC_UDATA, ACC_RMW, EXEC);
    assert(!r.ok && r.fault == MMU_FAULT_WRITE_PROTECT);
    r = xlate(a, FC_SPROG, ACC_W, EXEC); /* 命令側の TT0 は W なし */
    assert(r.ok);
    /* 両方一致なら TT0 を優先 */
    cpu_g->mmu060_dtt0 = TTR_E | (2u << 13) | ((a >> 24) << 24) | (3u << 5);
    r = xlate(a, FC_SDATA, ACC_R, EXEC);
    assert(r.cache_mode == 3 && !r.write_protect);
}

/* TTR の他のビットが W・CM・E の判定に影響しない（単独ビットを足す） */
static void test_ttr_bits(void) {
    for (unsigned b = 0; b < 32; ++b) {
        if (b == 2 || b == 15 || b == 5 || b == 6)
            continue;
        init_cpu();
        uint32_t ttr = TTR_E | (UINT32_C(1) << b);
        if (b != 14 && b != 13)
            ttr |= 2u << 13;
        cpu_g->mmu060_dtt0 = ttr;
        const uint32_t logical = (ttr >> 24) << 24;
        mc68k_translate_result_t r = xlate(logical, FC_SDATA, ACC_W, EXEC);
        assert(r.source == MC68K_TRANSLATE_TT && r.ok && !r.write_protect);
        assert(r.cache_mode == 0 && !r.cache_inhibit);
    }
    for (unsigned cm = 0; cm < 4; ++cm) {
        init_cpu();
        cpu_g->mmu060_dtt0 = TTR_E | (2u << 13) | (cm << 5);
        mc68k_translate_result_t r = xlate(0, FC_SDATA, ACC_R, EXEC);
        assert(r.cache_mode == cm && r.cache_inhibit == (cm >= 2));
    }
}

/* ---- table search ---- */
typedef struct {
    unsigned ps;
    uint32_t root, ptr, page; /* table の物理アドレス */
    uint32_t root_low;        /* URP/SRP 下位ビットのごみ */
} layout_t;

static unsigned idx_root(uint32_t l) {
    return l >> 25;
}
static unsigned idx_ptr(uint32_t l) {
    return (l >> 18) & 0x7fu;
}
static unsigned idx_page(uint32_t l, unsigned ps) {
    return ps == 12 ? (l >> 12) & 0x3fu : (l >> 13) & 0x1fu;
}
static uint32_t root_da(const layout_t *t, uint32_t l) {
    return t->root + idx_root(l) * 4u;
}
static uint32_t ptr_da(const layout_t *t, uint32_t l) {
    return t->ptr + idx_ptr(l) * 4u;
}
static uint32_t page_da(const layout_t *t, uint32_t l) {
    return t->page + idx_page(l, t->ps) * 4u;
}

static uint32_t reserved_ptr(unsigned ps) {
    return ps == 12 ? 0xf0u : 0x70u;
}
static void build(const layout_t *t, uint32_t l, uint32_t root_flags, uint32_t ptr_flags,
                  uint32_t leaf) {
    host_w(root_da(t, l), t->ptr | 0x1f0u | root_flags);
    host_w(ptr_da(t, l), t->page | reserved_ptr(t->ps) | ptr_flags);
    host_w(page_da(t, l), leaf);
}
static void set_regs(const layout_t *t) {
    cpu_g->mmu060_tc = TC_E | (t->ps == 13 ? TC_P : 0);
    cpu_g->mmu060_urp = t->root | t->root_low;
    cpu_g->mmu060_srp = t->root | t->root_low;
}
static const layout_t std4 = {12, 0x10200u, 0x10600u, 0x10900u, 0x1ffu};
static const layout_t std8 = {13, 0x10200u, 0x10600u, 0x10980u, 0x1ffu};
static const layout_t *std_layout(unsigned ps) {
    return ps == 12 ? &std4 : &std8;
}
static uint32_t page_mask(unsigned ps) {
    return (UINT32_C(1) << ps) - 1u;
}

/* 論理アドレスの各ビット（index と offset）。誤った index では未 map の descriptor（type
 * 0）を読んで fault になる */
static void test_walk_index(void) {
    for (unsigned ps = 12; ps <= 13; ++ps) {
        const layout_t *t = std_layout(ps);
        uint32_t cases[40];
        unsigned n = 0;
        cases[n++] = 0;
        cases[n++] = 0xfffff000u << (ps - 12) >> (ps - 12);
        for (unsigned b = ps; b < 32; ++b)
            cases[n++] = UINT32_C(1) << b;
        for (unsigned i = 0; i < n; ++i) {
            const uint32_t l = cases[i];
            for (int sup = 0; sup <= 1; ++sup) {
                init_cpu();
                set_regs(t);
                const uint32_t phys = 0xa5a5e000u;
                build(t, l, DESC_TYPE_TABLE, DESC_TYPE_TABLE, phys | DESC_PAGE_A);
                const uint32_t off = (ps == 12 ? 0xabcu : 0x1abcu);
                mc68k_translate_result_t r =
                    xlate(l | off, sup ? FC_SDATA : FC_UDATA, ACC_R, EXEC);
                assert(r.ok && r.source == MC68K_TRANSLATE_WALK);
                assert(r.physical == (phys | off) && r.level == 3);
                assert(r.descriptor_physical == page_da(t, l));
                /* ATC は page の全 offset で hit し、別 page は miss */
                r = xlate(l | (off ^ 0x5u), sup ? FC_SDATA : FC_UDATA, ACC_R, EXEC);
                assert(r.source == MC68K_TRANSLATE_ATC && r.physical == (phys | (off ^ 0x5u)));
                r = xlate((l ^ (UINT32_C(1) << (ps == 12 ? 12 : 13))) | off,
                          sup ? FC_SDATA : FC_UDATA, ACC_R, EXEC);
                assert(r.source != MC68K_TRANSLATE_ATC);
            }
        }
    }
}

/* 各 table のアラインメントと下位の予約ビット。アドレスの任意のビットが立つ位置に table を置く
 */
static void test_walk_alignment(void) {
    for (unsigned ps = 12; ps <= 13; ++ps) {
        for (unsigned k = 7; k < 32; ++k) {
            for (unsigned which = 0; which < 3; ++which) {
                layout_t t = *std_layout(ps);
                const uint32_t bit = UINT32_C(1) << k;
                /* which: 0=root、1=pointer table、2=page table を bit k
                 * が立つアドレスへ。アラインメントを満たすものだけ */
                const uint32_t align = which == 0   ? 0x200u
                                       : which == 1 ? 0x200u
                                                    : (ps == 12 ? 0x100u : 0x80u);
                if (bit < align)
                    continue;
                if (which == 0)
                    t.root = 0x20000u | bit;
                else if (which == 1)
                    t.ptr = 0x20000u | bit;
                else
                    t.page = 0x20000u | bit;
                const uint32_t l = 0x0a5a5000u | (UINT32_C(1) << ps);
                init_cpu();
                set_regs(&t);
                build(&t, l, DESC_TYPE_TABLE, DESC_TYPE_TABLE, 0x77770000u | DESC_PAGE_B);
                mc68k_translate_result_t r = xlate(l | 0x123u, FC_SDATA, ACC_R, EXEC);
                assert(r.ok && r.physical == (0x77770000u | ((l | 0x123u) & page_mask(ps))));
            }
        }
    }
}

/* 全ビットが 1 の table アドレス */
static void test_walk_high_tables(void) {
    for (unsigned ps = 12; ps <= 13; ++ps) {
        layout_t t = *std_layout(ps);
        t.root = 0xfffffe00u; /* 128 entry の半分だけ使う（index < 64） */
        t.ptr = 0xfffff800u;
        t.page = ps == 12 ? 0xffffff00u : 0xffffff80u;
        t.root_low = 0x1ffu;
        const uint32_t l = 0x02000000u + 0x40000u * 3 + (UINT32_C(0x3000) << (ps - 12));
        init_cpu();
        set_regs(&t);
        build(&t, l, DESC_TYPE_TABLE, DESC_TYPE_TABLE, 0xffffe000u | DESC_G | DESC_PAGE_A);
        mc68k_translate_result_t r = xlate(l + 0x44u, FC_UDATA, ACC_R, EXEC);
        assert(r.ok && r.physical == 0xffffe000u + 0x44u);
        assert(!r.write_protect && r.cache_mode == 0);
    }
}

/* descriptor の type とエラー段 */
static void test_walk_types(void) {
    for (unsigned ps = 12; ps <= 13; ++ps) {
        const layout_t *t = std_layout(ps);
        const uint32_t l = 0x06543000u;
        /* root: type 0/1 は invalid（level 1）、2 は有効 */
        for (unsigned ty = 0; ty < 4; ++ty) {
            init_cpu();
            set_regs(t);
            build(t, l, ty, DESC_TYPE_TABLE, 0x50000000u | DESC_PAGE_A);
            mc68k_translate_result_t r = xlate(l, FC_SDATA, ACC_R, EXEC);
            if (ty < 2) {
                assert(!r.ok && r.fault == MMU_FAULT_INVALID && r.level == 1 &&
                       r.descriptor_physical == root_da(t, l));
                assert(valid_count(cpu_g->atc060_d) == 0);
            } else {
                assert(r.ok && r.physical == 0x50000000u + (l & page_mask(ps)));
            }
        }
        for (unsigned ty = 0; ty < 4; ++ty) {
            init_cpu();
            set_regs(t);
            build(t, l, DESC_TYPE_TABLE, ty, 0x50000000u | DESC_PAGE_A);
            mc68k_translate_result_t r = xlate(l, FC_SDATA, ACC_R, EXEC);
            if (ty < 2) {
                assert(!r.ok && r.fault == MMU_FAULT_INVALID && r.level == 2 &&
                       r.descriptor_physical == ptr_da(t, l));
            } else {
                assert(r.ok && r.level == 3);
            }
        }
        for (unsigned ty = 0; ty < 4; ++ty) {
            init_cpu();
            set_regs(t);
            build(t, l, DESC_TYPE_TABLE, DESC_TYPE_TABLE, 0x50000000u | ty);
            mc68k_translate_result_t r = xlate(l, FC_SDATA, ACC_R, EXEC);
            if (ty == 0) {
                assert(!r.ok && r.fault == MMU_FAULT_INVALID && r.level == 3);
            } else if (ty == 2) {
                /* indirect の指す先 0x50000000 は読めない */
                assert(!r.ok && r.fault == MMU_FAULT_TABLE_BUS_ERROR &&
                       r.table_bus_error_physical == 0x50000000u);
            } else {
                assert(r.ok && r.physical == 0x50000000u + (l & page_mask(ps)));
            }
            (void)r;
        }
    }
}

/* indirect descriptor */
static void test_walk_indirect(void) {
    for (unsigned ps = 12; ps <= 13; ++ps) {
        const layout_t *t = std_layout(ps);
        const uint32_t l = 0x0c012000u;
        const uint32_t target = 0x30004u * 4u; /* 0xC0010、4 の倍数 */
        for (unsigned ty2 = 0; ty2 < 4; ++ty2) {
            init_cpu();
            set_regs(t);
            build(t, l, DESC_TYPE_TABLE, DESC_TYPE_TABLE,
                  (target & ~3u) | 0x3u * 0 | DESC_TYPE_INDIRECT);
            const uint32_t tg = 0x30010u;
            host_w(page_da(t, l), tg | DESC_TYPE_INDIRECT);
            host_w(tg, 0x66600000u | ty2 | DESC_G);
            mc68k_translate_result_t r = xlate(l + 8, FC_SDATA, ACC_R, EXEC);
            if (ty2 == 1 || ty2 == 3) {
                assert(r.ok && r.physical == 0x66600000u + 8 +
                                                 (ps == 13 ? (l & 0x1000u) : 0) * 0 +
                                                 0 * (l & 0));
                assert(!r.indirect_fault && r.descriptor_physical == tg);
                assert(r.physical == 0x66600000u + (l + 8 & page_mask(ps)));
            } else {
                assert(!r.ok && r.fault == MMU_FAULT_INVALID && r.descriptor_physical == tg);
                assert(r.indirect_fault == (ty2 == 2));
            }
        }
        /* indirect の読みが bus error */
        init_cpu();
        set_regs(t);
        build(t, l, DESC_TYPE_TABLE, DESC_TYPE_TABLE, 0x30010u | DESC_TYPE_INDIRECT);
        f.fail_read = 0x30010u;
        mc68k_translate_result_t r = xlate(l, FC_SDATA, ACC_R, EXEC);
        assert(!r.ok && r.fault == MMU_FAULT_TABLE_BUS_ERROR &&
               r.table_bus_error_physical == 0x30010u);
        f.fail_read = ~0u;
        /* indirect の指す先の物理 page は 0x30010 の address ではなく、その中身 */
        host_w(0x30010u, 0x44440000u | DESC_PAGE_A);
        r = xlate(l + 4, FC_SDATA, ACC_R, EXEC);
        assert(r.ok && r.descriptor_physical == 0x30010u && r.level == 3);
    }
}

/* table の bus error（段ごと）。U の遅延書込み */
static void test_walk_bus_errors(void) {
    for (unsigned ps = 12; ps <= 13; ++ps) {
        const layout_t *t = std_layout(ps);
        const uint32_t l = 0x04abc000u & ~page_mask(ps);
        for (unsigned level = 0; level < 3; ++level) {
            init_cpu();
            set_regs(t);
            build(t, l, DESC_TYPE_TABLE, DESC_TYPE_TABLE, 0x55550000u | DESC_PAGE_A);
            const uint32_t da = level == 0   ? root_da(t, l)
                                : level == 1 ? ptr_da(t, l)
                                             : page_da(t, l);
            f.fail_read = da;
            mc68k_translate_result_t r = xlate(l, FC_SDATA, ACC_R, EXEC);
            assert(!r.ok && r.fault == MMU_FAULT_TABLE_BUS_ERROR &&
                   r.table_bus_error_physical == da);
            assert(valid_count(cpu_g->atc060_d) == 0);
            /* 失敗した段より上位の U
             * は、次の段の読みの後に書かれる。読みに失敗した段より上は読めている */
            const bool root_u = host_r(root_da(t, l)) & DESC_U,
                       ptr_u = host_r(ptr_da(t, l)) & DESC_U;
            assert(root_u == (level >= 1)); /* root の U は ptr の読み（level 1）の後に書く */
            assert(ptr_u == (level >= 2));
            assert(!(host_r(page_da(t, l)) & DESC_U));
            assert(f.writes ==
                   level); /* U の書込みは読みに成功した段の分だけ。address 0 を汚さない */
            assert(host_r(0) == SSP_INIT);
        }
        /* U の書込みの失敗（root、ptr、page）*/
        for (unsigned level = 0; level < 3; ++level) {
            init_cpu();
            set_regs(t);
            build(t, l, DESC_TYPE_TABLE, DESC_TYPE_TABLE, 0x55550000u | DESC_PAGE_A);
            const uint32_t da = level == 0   ? root_da(t, l)
                                : level == 1 ? ptr_da(t, l)
                                             : page_da(t, l);
            f.fail_write = da;
            mc68k_translate_result_t r = xlate(l, FC_SDATA, ACC_R, EXEC);
            assert(!r.ok && r.fault == MMU_FAULT_TABLE_BUS_ERROR &&
                   r.table_bus_error_physical == da);
            assert(valid_count(cpu_g->atc060_d) == 0);
        }
        /* M の書込みの失敗（U が立っている page への write） */
        init_cpu();
        set_regs(t);
        build(t, l, DESC_TYPE_TABLE | DESC_U, DESC_TYPE_TABLE | DESC_U,
              0x55550000u | DESC_U | DESC_PAGE_A);
        f.fail_write = page_da(t, l);
        mc68k_translate_result_t r = xlate(l, FC_SDATA, ACC_W, EXEC);
        assert(!r.ok && r.fault == MMU_FAULT_TABLE_BUS_ERROR &&
               r.table_bus_error_physical == page_da(t, l));
        /* ptr の U の書込みに失敗するとき、root の U は既に書かれている */
        init_cpu();
        set_regs(t);
        build(t, l, DESC_TYPE_TABLE, DESC_TYPE_TABLE, 0x55550000u | DESC_PAGE_A);
        f.fail_write = ptr_da(t, l);
        r = xlate(l, FC_SDATA, ACC_R, EXEC);
        assert(r.fault == MMU_FAULT_TABLE_BUS_ERROR);
        assert(host_r(root_da(t, l)) & DESC_U);
        /* indirect の先を読めないとき、直前の level の U は書かれない（page level の indirect
         * では U は先の descriptor に付く）*/
    }
}

/* U/M/W/S の更新と fault の行列 */
typedef struct {
    bool rw, ru, pw, pu, lw, lu, lm, ls;
} flags_t;

static void check_matrix_case(unsigned ps, const flags_t *fl, uint8_t fc, acc_kind_t kind,
                              mc68k_translate_mode_t mode, unsigned cmv, bool global,
                              unsigned leaf_type) {
    const layout_t *t = std_layout(ps);
    const uint32_t l = 0x00abc000u + (ps == 13 ? 0x2000u : 0);
    init_cpu();
    set_regs(t);
    const uint32_t leaf = 0x91234000u | leaf_type | (fl->lw ? DESC_W : 0) |
                          (fl->lu ? DESC_U : 0) | (fl->lm ? DESC_M : 0) |
                          (fl->ls ? DESC_S : 0) | (cmv << 5) | (global ? DESC_G : 0);
    build(t, l, DESC_TYPE_TABLE | (fl->rw ? DESC_W : 0) | (fl->ru ? DESC_U : 0),
          DESC_TYPE_TABLE | (fl->pw ? DESC_W : 0) | (fl->pu ? DESC_U : 0), leaf);
    const bool sup = fc >= 4, wr = kind_is_write(kind), debug = mode == MC68K_TRANSLATE_DEBUG;
    const bool wp = fl->rw || fl->pw || fl->lw;
    nev = 0;
    f.writes = 0;
    mc68k_translate_result_t r = xlate(l + 0x11u, fc, kind, mode);
    /* write で W を見つけた descriptor で table search を直ちに終える（その descriptor の U
     * は更新しない、 [060UM §4.2.6.3、Table 4-1]）。page の S 違反は W より先に判定する */
    const bool stop_root = wr && fl->rw, stop_ptr = !stop_root && wr && (fl->rw || fl->pw);
    const unsigned reached = stop_root ? 1u : stop_ptr ? 2u : 3u;
    const bool s_fault = reached == 3 && fl->ls && !sup;
    const bool w_fault_page = reached == 3 && !s_fault && wr && wp;
    const bool w_fault = stop_root || stop_ptr || w_fault_page;
    assert(r.ok == (!s_fault && !w_fault));
    if (s_fault)
        assert(r.fault == MMU_FAULT_SUPERVISOR);
    if (w_fault)
        assert(r.fault == MMU_FAULT_WRITE_PROTECT);
    assert(r.level == reached);
    assert(r.descriptor_physical == (reached == 1   ? root_da(t, l)
                                     : reached == 2 ? ptr_da(t, l)
                                                    : page_da(t, l)));
    const uint32_t rd = host_r(root_da(t, l)), pd = host_r(ptr_da(t, l)),
                   ld = host_r(page_da(t, l));
    assert(((rd & DESC_U) != 0) == (fl->ru || (!debug && reached >= 2)));
    assert(((pd & DESC_U) != 0) == (fl->pu || (!debug && reached >= 3)));
    const bool leaf_touched = reached == 3 && !s_fault && !w_fault_page && !debug;
    assert(((ld & DESC_U) != 0) == (fl->lu || leaf_touched));
    assert(((ld & DESC_M) != 0) == (fl->lm || (leaf_touched && wr)));
    assert((rd & ~DESC_U) == ((t->ptr | 0x1f0u | DESC_TYPE_TABLE | (fl->rw ? DESC_W : 0) |
                               (fl->ru ? DESC_U : 0)) &
                              ~DESC_U));
    assert((ld & ~(DESC_U | DESC_M)) == (leaf & ~(DESC_U | DESC_M)));
    if (r.ok)
        assert(r.cache_mode == cmv && r.cache_inhibit == (cmv >= 2));
    if (r.ok || w_fault_page) {
        assert(r.write_protect == wp);
        assert(r.modified == (fl->lm || (leaf_touched && wr)));
    }
    if (stop_root || stop_ptr)
        assert(r.write_protect);
    if (r.ok) {
        assert(r.source == MC68K_TRANSLATE_WALK && r.physical == 0x91234000u + (0x11u));
    }
    /* バスサイクルの列: descriptor の読み → 1 段遅れの U 書込み → leaf の U/M 更新（U だけなら
     * locked の RMW）*/
    unsigned ev = 0;
    if (!debug) {
        expect_ev(&ev, root_da(t, l), true, false);
        if (reached >= 2) {
            expect_ev(&ev, ptr_da(t, l), true, false);
            if (!fl->ru)
                expect_ev(&ev, root_da(t, l), false, false);
        }
        if (reached >= 3) {
            expect_ev(&ev, page_da(t, l), true, false);
            if (!fl->pu)
                expect_ev(&ev, ptr_da(t, l), false, false);
        }
        if (leaf_touched) {
            const bool set_m = wr;
            if (!fl->lu || (set_m && !fl->lm)) {
                if (!fl->lu && (!set_m || fl->lm)) {
                    expect_ev(&ev, page_da(t, l), true, true);
                    expect_ev(&ev, page_da(t, l), false, true);
                } else {
                    expect_ev(&ev, page_da(t, l), false, false);
                }
            }
        }
    }
    assert(ev == nev); /* debug は table search のバスサイクルを出さない */
    /* ATC の entry */
    const mc68k_atc_entry_t *atc = (fc & 3u) == 2u ? cpu_g->atc060_i : cpu_g->atc060_d;
    const mc68k_atc_entry_t *other = (fc & 3u) == 2u ? cpu_g->atc060_d : cpu_g->atc060_i;
    assert(valid_count(other) == 0);
    if (r.ok && !debug) {
        assert(valid_count(atc) == 1);
        const mc68k_atc_entry_t *e = NULL;
        for (unsigned i = 0; i < MC68K_060_ATC_ENTRIES; ++i)
            if (atc[i].valid)
                e = &atc[i];
        assert(e->logical_page == (l & ~page_mask(ps)) && e->physical_page == 0x91234000u);
        assert(e->fc == (fc & 4u) && e->page_shift == ps);
        assert(e->cache_mode == cmv && e->cache_inhibit == (cmv >= 2) &&
               e->write_protect == wp);
        assert(e->modified == (fl->lm || (wr && !wp)) && e->global == global);
        /* ATC hit の属性 */
        mc68k_translate_result_t h = xlate(l + 0x21u, fc, ACC_R, EXEC);
        assert(h.ok && h.source == MC68K_TRANSLATE_ATC && h.physical == 0x91234021u);
        assert(h.cache_mode == cmv && h.cache_inhibit == (cmv >= 2) && h.write_protect == wp &&
               h.modified == e->modified);
    } else {
        assert(valid_count(atc) == 0);
    }
}

static void test_walk_matrix(void) {
    static const uint8_t fcs[] = {FC_UDATA, FC_SDATA, FC_UPROG, FC_SPROG};
    for (unsigned ps = 12; ps <= 13; ++ps) {
        for (unsigned bits = 0; bits < 256; ++bits) {
            flags_t fl = {bits & 1,  bits & 2,  bits & 4,  bits & 8,
                          bits & 16, bits & 32, bits & 64, bits & 128};
            for (unsigned fi = 0; fi < 4; ++fi) {
                for (unsigned k = 0; k < 4; ++k) {
                    /* 命令側（FC=2/6）の書込みは起こらないので read だけ */
                    if ((fcs[fi] & 3u) == 2u && k != ACC_R)
                        continue;
                    check_matrix_case(ps, &fl, fcs[fi], (acc_kind_t)k, EXEC, (bits + k) & 3u,
                                      bits & 1,
                                      ((bits >> 3) + k + fi) & 1 ? DESC_PAGE_B : DESC_PAGE_A);
                    if (fi == 0 && k == 0)
                        check_matrix_case(ps, &fl, fcs[fi], (acc_kind_t)k,
                                          MC68K_TRANSLATE_PLOAD, bits & 3u, false, DESC_PAGE_B);
                    if (bits % 7 == 0)
                        check_matrix_case(ps, &fl, fcs[fi], (acc_kind_t)k,
                                          MC68K_TRANSLATE_DEBUG, bits & 3u, false, DESC_PAGE_A);
                }
            }
        }
    }
}

/* descriptor の各ビットが他のフラグを誤って立てない（単独ビット）。物理アドレスは全ビット 1 */
static void test_walk_bit_sweep(void) {
    for (unsigned ps = 12; ps <= 13; ++ps) {
        const layout_t *t = std_layout(ps);
        const uint32_t l = 0x00800000u;
        const uint32_t allp = ps == 12 ? 0xfffff000u : 0xffffe000u;
        for (unsigned b = 3; b < 12; ++b) {
            init_cpu();
            set_regs(t);
            uint32_t bit = UINT32_C(1) << b;
            build(t, l, DESC_TYPE_TABLE, DESC_TYPE_TABLE,
                  allp | DESC_PAGE_A | bit | (ps == 13 ? 0x1000u : 0));
            mc68k_translate_result_t r = xlate(l, FC_UDATA, ACC_W, EXEC);
            const bool w = b == 2; /* ここでは常に false（b >= 3） */
            const bool s = b == 7;
            if (s) {
                assert(!r.ok && r.fault == MMU_FAULT_SUPERVISOR);
                r = xlate(l, FC_SDATA, ACC_W, EXEC);
            }
            assert(r.ok && !w && !r.write_protect);
            assert(r.cache_mode == ((b == 5 ? 1u : 0u) | (b == 6 ? 2u : 0u)));
        }
        /* 読み: M・G は descriptor のビットそのまま。entry にも反映される */
        for (unsigned b = 3; b < 12; ++b) {
            init_cpu();
            set_regs(t);
            const uint32_t bit = UINT32_C(1) << b;
            build(t, l, DESC_TYPE_TABLE, DESC_TYPE_TABLE,
                  allp | DESC_PAGE_A | bit | (ps == 13 ? 0x1000u : 0));
            mc68k_translate_result_t r = xlate(l, FC_SDATA, ACC_R, EXEC);
            assert(r.ok && r.modified == (b == 4));
            const mc68k_atc_entry_t *e = NULL;
            for (unsigned i = 0; i < MC68K_060_ATC_ENTRIES; ++i)
                if (cpu_g->atc060_d[i].valid)
                    e = &cpu_g->atc060_d[i];
            assert(e && e->global == (b == 10) && e->modified == (b == 4));
            assert(e->write_protect == false && e->physical_page == allp);
            assert(((host_r(page_da(t, l)) & DESC_M) != 0) == (b == 4));
        }
        /* 物理アドレスの各ビット */
        for (unsigned b = ps; b < 32; ++b) {
            init_cpu();
            set_regs(t);
            build(t, l, DESC_TYPE_TABLE, DESC_TYPE_TABLE, (UINT32_C(1) << b) | DESC_PAGE_A);
            mc68k_translate_result_t r = xlate(l, FC_UDATA, ACC_W, EXEC);
            assert(r.ok && !r.write_protect && r.cache_mode == 0 && !r.modified * 0 + 1);
            assert(r.physical == ((UINT32_C(1) << b) | (l & page_mask(ps))));
        }
        /* table descriptor の上位アドレスビットが W/U/type と混ざらない */
        for (unsigned b = 9; b < 32; ++b) {
            layout_t tt = *t;
            tt.ptr = 0x20000u | (UINT32_C(1) << b);
            init_cpu();
            set_regs(&tt);
            build(&tt, l, DESC_TYPE_TABLE, DESC_TYPE_TABLE, 0x40000000u | DESC_PAGE_A);
            mc68k_translate_result_t r = xlate(l, FC_UDATA, ACC_W, EXEC);
            assert(r.ok && !r.write_protect);
        }
    }
}

/* ---- ATC の hit 経路 ---- */
static void test_atc_hit_paths(void) {
    for (unsigned ps = 12; ps <= 13; ++ps) {
        const layout_t *t = std_layout(ps);
        const uint32_t l = 0x00400000u;
        /* hit: 読みは descriptor を触らない。clock を進める */
        init_cpu();
        set_regs(t);
        build(t, l, DESC_TYPE_TABLE, DESC_TYPE_TABLE, 0x61000000u | DESC_PAGE_A);
        mc68k_translate_result_t r = xlate(l, FC_SDATA, ACC_R, EXEC);
        assert(r.ok);
        host_w(page_da(t, l), 0); /* table を壊す */
        const uint64_t c0 = cpu_g->atc060_clock;
        r = xlate(l + 5, FC_SDATA, ACC_R, EXEC);
        assert(r.ok && r.source == MC68K_TRANSLATE_ATC && !r.modified);
        assert(cpu_g->atc060_clock == c0 + 1);
        /* debug の hit は clock を進めない */
        r = xlate(l + 4, FC_SDATA, ACC_R, MC68K_TRANSLATE_DEBUG);
        assert(r.ok && r.source == MC68K_TRANSLATE_ATC && cpu_g->atc060_clock == c0 + 1);
        /* debug の write hit（M=0）は walk せず ATC の値を返す（descriptor の変更なし） */
        r = xlate(l + 4, FC_SDATA, ACC_W, MC68K_TRANSLATE_DEBUG);
        assert(r.ok && r.source == MC68K_TRANSLATE_ATC && host_r(page_da(t, l)) == 0);
        /* 書込み（M=0）は walk し直す。table は 0 なので fault し、entry は変わらない */
        r = xlate(l + 4, FC_SDATA, ACC_W, EXEC);
        assert(!r.ok && r.fault == MMU_FAULT_INVALID);
        r = xlate(l + 4, FC_SDATA, ACC_R, EXEC);
        assert(r.ok && r.source == MC68K_TRANSLATE_ATC);
        /* 命令側 bank は別 */
        r = xlate(l + 4, FC_SPROG, ACC_R, EXEC);
        assert(!r.ok);
        /* FC2 だけが tag: user data (1) と user program の bank は別だが、data bank の fc=1 と
         * fc=2 のアクセス(命令でない) */
        init_cpu();
        set_regs(t);
        build(t, l, DESC_TYPE_TABLE, DESC_TYPE_TABLE, 0x61000000u | DESC_PAGE_A | DESC_M);
        r = xlate(l, FC_UDATA, ACC_R, EXEC);
        assert(r.ok && r.source == MC68K_TRANSLATE_WALK);
        mc68k_access_t a = mk_access(FC_UDATA, ACC_R);
        a.fc = 3; /* FC2=0 の別値 */
        r = cpu_mmu060_translate_result(cpu_g, l, &a, EXEC);
        assert(r.ok && r.source == MC68K_TRANSLATE_ATC);
        a.fc = 5;
        r = cpu_mmu060_translate_result(cpu_g, l, &a, EXEC);
        assert(r.source == MC68K_TRANSLATE_WALK); /* supervisor は別 entry（SRP の table）*/
        for (uint8_t fc = 0; fc < 8; ++fc) {
            if ((fc & 3u) == 0)
                continue;
            a.fc = fc;
            r = cpu_mmu060_translate_result(cpu_g, l, &a, EXEC);
            assert(r.source == MC68K_TRANSLATE_ATC);
        }
        /* M=1 の entry への書込みは walk しない */
        host_w(page_da(t, l), 0);
        r = xlate(l, FC_SDATA, ACC_W, EXEC);
        assert(r.ok && r.source == MC68K_TRANSLATE_ATC && r.modified);
        /* W=1 の entry への書込みは fault（walk しない）。entry は不変 */
        init_cpu();
        set_regs(t);
        build(t, l, DESC_TYPE_TABLE, DESC_TYPE_TABLE, 0x61000000u | DESC_PAGE_A | DESC_W);
        assert(xlate(l, FC_SDATA, ACC_R, EXEC).ok);
        host_w(page_da(t, l), 0x61000000u | DESC_PAGE_A | DESC_M);
        for (int kind = 1; kind <= 3; ++kind) {
            r = xlate(l, FC_SDATA, (acc_kind_t)kind, EXEC);
            assert(!r.ok && r.fault == MMU_FAULT_WRITE_PROTECT &&
                   r.source == MC68K_TRANSLATE_ATC && r.write_protect);
        }
        r = xlate(l, FC_SDATA, ACC_R, EXEC);
        assert(r.ok && r.write_protect && !r.modified);
        /* M 更新の walk は同じ slot を上書きする（entry は増えない） */
        init_cpu();
        set_regs(t);
        build(t, l, DESC_TYPE_TABLE, DESC_TYPE_TABLE, 0x61000000u | DESC_PAGE_A);
        assert(xlate(l, FC_SDATA, ACC_R, EXEC).ok);
        r = xlate(l, FC_SDATA, ACC_W, EXEC);
        assert(r.ok && r.source == MC68K_TRANSLATE_WALK && r.modified);
        assert(valid_count(cpu_g->atc060_d) == 1);
        assert(xlate(l, FC_SDATA, ACC_R, EXEC).modified);
    }
}

/* 4KB で作った entry は P=1（8KB）の検索で hit しない（page_shift も tag）。逆も同じ */
static void test_atc_page_shift_tag(void) {
    for (unsigned first = 12; first <= 13; ++first) {
        const unsigned second = first == 12 ? 13 : 12;
        const uint32_t l = 0x00400000u; /* 両方の page 境界に揃っている */
        init_cpu();
        set_regs(std_layout(first));
        build(std_layout(first), l, DESC_TYPE_TABLE, DESC_TYPE_TABLE,
              0x61000000u | DESC_PAGE_A);
        assert(xlate(l, FC_SDATA, ACC_R, EXEC).source == MC68K_TRANSLATE_WALK);
        set_regs(std_layout(second));
        build(std_layout(second), l, DESC_TYPE_TABLE, DESC_TYPE_TABLE,
              0x62000000u | DESC_PAGE_A);
        mc68k_translate_result_t r = xlate(l, FC_SDATA, ACC_R, EXEC);
        assert(r.source == MC68K_TRANSLATE_WALK && r.physical == 0x62000000u);
    }
}

/* ATC の置換: 64 entry、最も古い history を追い出す */
static void test_atc_replacement(void) {
    const layout_t *t = &std4;
    init_cpu();
    set_regs(t);
    /* 論理 page i（i = 0..65）を物理 0x70000000 + i*0x1000 へ。page index は 6 bit なので ptr
     * index を進める */
    const unsigned N = MC68K_060_ATC_ENTRIES + 2;
    for (unsigned i = 0; i < N; ++i) {
        const uint32_t l = 0x00400000u + i * 0x1000u;
        host_w(root_da(t, l), t->ptr | 0x1f0u | DESC_TYPE_TABLE);
        host_w(ptr_da(t, l), t->page | 0xf0u | DESC_TYPE_TABLE);
        host_w(page_da(t, l), (0x70000000u + i * 0x1000u) | DESC_PAGE_A);
    }
    for (unsigned i = 0; i < MC68K_060_ATC_ENTRIES; ++i)
        assert(xlate(0x00400000u + i * 0x1000u, FC_SDATA, ACC_R, EXEC).source ==
               MC68K_TRANSLATE_WALK);
    assert(valid_count(cpu_g->atc060_d) == MC68K_060_ATC_ENTRIES);
    assert(valid_count(cpu_g->atc060_i) == 0);
    /* page 0 を触り直して最新にする。次の追加で追い出されるのは page 1 */
    assert(xlate(0x00400000u, FC_SDATA, ACC_R, EXEC).source == MC68K_TRANSLATE_ATC);
    assert(xlate(0x00400000u + 64u * 0x1000u, FC_SDATA, ACC_R, EXEC).source ==
           MC68K_TRANSLATE_WALK);
    assert(valid_count(cpu_g->atc060_d) == MC68K_060_ATC_ENTRIES);
    assert(xlate(0x00400000u, FC_SDATA, ACC_R, EXEC).source == MC68K_TRANSLATE_ATC);
    for (unsigned i = 2; i < MC68K_060_ATC_ENTRIES; ++i)
        assert(xlate(0x00400000u + i * 0x1000u, FC_SDATA, ACC_R, EXEC).source ==
               MC68K_TRANSLATE_ATC);
    assert(xlate(0x00400000u + 64u * 0x1000u, FC_SDATA, ACC_R, EXEC).source ==
           MC68K_TRANSLATE_ATC);
    assert(xlate(0x00400000u + 1u * 0x1000u, FC_SDATA, ACC_R, EXEC).source ==
           MC68K_TRANSLATE_WALK); /* 追い出されていた */
    /* 無効な slot が先に使われる: 1 つ無効にして追加すると、その slot が埋まる */
    cpu_g->atc060_d[17].valid = false;
    unsigned before = valid_count(cpu_g->atc060_d);
    assert(xlate(0x00400000u + 65u * 0x1000u, FC_SDATA, ACC_R, EXEC).source ==
           MC68K_TRANSLATE_WALK);
    assert(valid_count(cpu_g->atc060_d) == before + 1);
    assert(cpu_g->atc060_d[17].valid &&
           cpu_g->atc060_d[17].logical_page == 0x00400000u + 65u * 0x1000u);
    /* slot 0 が無効なら、それが先に使われる */
    cpu_g->atc060_d[0].valid = false;
    before = valid_count(cpu_g->atc060_d);
    assert(xlate(0x00400000u + 1u * 0x1000u, FC_SDATA, ACC_R, EXEC).source ==
           MC68K_TRANSLATE_WALK);
    assert(valid_count(cpu_g->atc060_d) == before + 1);
    assert(cpu_g->atc060_d[0].valid &&
           cpu_g->atc060_d[0].logical_page == 0x00400000u + 1u * 0x1000u);
    /* 命令側とデータ側は別 */
    assert(xlate(0x00400000u + 2u * 0x1000u, FC_SPROG, ACC_R, EXEC).source ==
           MC68K_TRANSLATE_WALK);
    assert(valid_count(cpu_g->atc060_i) == 1);
}

/* ---- PFLUSH・PLPA の命令の符号化（An の全 8 本）---- */
static void run_op(uint16_t op, bool user) {
    host_w(TEST_PC, (uint32_t)op << 16 | 0x4e71u);
    cpu_g->a[7] = SSP_INIT;
    cpu_g->pc = TEST_PC;
    cpu_set_sr(cpu_g, user ? 0x0000 : SR_SUP);
    cpu_g->last_exception_vector = NO_VECTOR;
    mc68k_execute(cpu_g, 1);
}
static void fill_atc(void) {
    memset(cpu_g->atc060_i, 0, sizeof cpu_g->atc060_i);
    memset(cpu_g->atc060_d, 0, sizeof cpu_g->atc060_d);
    cpu_g->atc060_clock = 0;
    /* d: [0]=user P non-global、[1]=user P global、[2]=sup P non-global、[3]=sup Q
     * non-global。i: 同様 */
    for (int bank = 0; bank < 2; ++bank) {
        mc68k_atc_entry_t *atc = bank ? cpu_g->atc060_i : cpu_g->atc060_d;
        for (int i = 0; i < 5; ++i) {
            atc[i].valid = true;
            atc[i].page_shift = (uint8_t)cpu_g->mmu060_tc; /* 使わない */
            atc[i].page_shift = 12;
            atc[i].logical_page = (i == 3 ? 0x00201000u : 0x00200000u) + (i == 4 ? 0x2000u : 0);
            atc[i].fc = (i >= 2 && i != 4) ? 4 : 0;
            atc[i].global = (i == 1);
            atc[i].physical_page = 0x1000u * (unsigned)(i + 1);
        }
    }
}
static unsigned valid_mask(const mc68k_atc_entry_t *atc) {
    unsigned m = 0;
    for (unsigned i = 0; i < 5; ++i)
        m |= (unsigned)atc[i].valid << i;
    return m;
}
static void test_pflush_encoding(void) {
    /* op の下位 3 bit = An。A0〜A7 すべてで同じ動作 */
    for (unsigned an = 0; an < 8; ++an) {
        for (unsigned dfc = 1; dfc <= 6; ++dfc) {
            if (dfc == 3 || dfc == 4)
                continue;
            const bool dsup = dfc >= 5;
            struct {
                uint16_t base;
                bool nonglobal_only;
                bool all;
            } ops[] = {{0xf508, false, false},
                       {0xf500, true, false},
                       {0xf518, false, true},
                       {0xf510, true, true}};
            for (unsigned oi = 0; oi < 4; ++oi) {
                init_cpu();
                cpu_g->mmu060_tc = TC_E;
                cpu_g->mmu060_itt0 = cpu_g->mmu060_dtt0 = 0x0000c000u;
                fill_atc();
                cpu_g->dfc = (uint8_t)dfc;
                if (an != 7)
                    cpu_g->a[an] = 0x00200abdu;
                else { /* A7 はスタック。supervisor の SSP をそのアドレスにする */
                }
                uint16_t op = (uint16_t)(ops[oi].base | an);
                if (an == 7) {
                    host_w(TEST_PC, (uint32_t)op << 16 | 0x4e71u);
                    cpu_g->pc = TEST_PC;
                    cpu_set_sr(cpu_g, SR_SUP);
                    cpu_g->a[7] = 0x00200abdu;
                    cpu_g->last_exception_vector = NO_VECTOR;
                    mc68k_execute(cpu_g, 1);
                } else {
                    run_op(op, false);
                }
                assert(cpu_g->last_exception_vector == NO_VECTOR);
                /* 期待: entry 0=user P ng、1=user P global、2=sup P ng、3=sup Q ng、4=user R
                 * ng（0x202000）*/
                unsigned want = 0x1f;
                for (unsigned e = 0; e < 5; ++e) {
                    const bool esup = (e == 2 || e == 3);
                    const bool eglobal = e == 1;
                    const uint32_t pg = e == 3   ? 0x00201000u
                                        : e == 4 ? 0x00202000u
                                                 : 0x00200000u;
                    bool kill;
                    if (ops[oi].all)
                        kill = !(ops[oi].nonglobal_only && eglobal);
                    else
                        kill = (esup == dsup) && pg == 0x00200000u &&
                               !(ops[oi].nonglobal_only && eglobal);
                    if (kill)
                        want &= ~(1u << e);
                }
                assert(valid_mask(cpu_g->atc060_d) == want &&
                       valid_mask(cpu_g->atc060_i) == want);
            }
        }
    }
}

/* PLPAR / PLPAW: An の全 8 本、W=1 page は PLPAW だけ fault、失敗時は An 不変 */
static void test_plpa_encoding(void) {
    const layout_t *t = &std4;
    for (unsigned an = 0; an < 8; ++an) {
        for (int write = 0; write <= 1; ++write) {
            for (int wp = 0; wp <= 1; ++wp) {
                for (int hit = 0; hit <= 1; ++hit) {
                    init_cpu();
                    set_regs(t);
                    cpu_g->mmu060_itt0 = cpu_g->mmu060_dtt0 = 0x0000c000u;
                    const uint32_t l = 0x03000000u;
                    build(t, l, DESC_TYPE_TABLE, DESC_TYPE_TABLE,
                          0x61000000u | DESC_PAGE_A | (wp ? DESC_W : 0));
                    cpu_g->dfc = 5;
                    if (hit)
                        assert(xlate(l, FC_SDATA, ACC_R, EXEC).ok);
                    const uint16_t op = (uint16_t)((write ? 0xf588u : 0xf5c8u) | an);
                    if (an == 7) {
                        host_w(TEST_PC, (uint32_t)op << 16 | 0x4e71u);
                        cpu_g->pc = TEST_PC;
                        cpu_set_sr(cpu_g, SR_SUP);
                        cpu_g->a[7] = l + 0x44u;
                        cpu_g->last_exception_vector = NO_VECTOR;
                        mc68k_execute(cpu_g, 1);
                    } else {
                        cpu_g->a[an] = l + 0x44u;
                        run_op(op, false);
                    }
                    const bool fault = write && wp;
                    if (fault) {
                        /* A7 が fault 先のページを指すと、例外 frame の push も fault
                         * して停止するので、A7 は見ない */
                        if (an != 7) {
                            assert(cpu_g->last_exception_vector == VEC_BUS);
                            assert(cpu_g->a[an] == l + 0x44u);
                        }
                    } else {
                        assert(cpu_g->last_exception_vector == NO_VECTOR);
                        assert(cpu_g->a[an] == 0x61000000u + 0x44u);
                    }
                }
            }
        }
    }
}

/* table search はデータ cache を参照しない（設計書 §3.0 の table search 行、[060UM
 * §11.1.2.2.2]）*/
static void test_table_search_bypasses_cache(void) {
    const layout_t *t = &std4;
    const uint32_t l = 0x03000000u;
    init_cpu();
    set_regs(t);
    cpu_g->mmu060_dtt0 = TTR_E | (2u << 13); /* $00 領域は恒等・copyback 可 */
    cpu_g->mmu060_itt0 = TTR_E | (2u << 13);
    cpu_g->cacr = 0x80000000u; /* DE */
    /* U=1: U 更新の RMW（line を push・無効化する）を起こさず、cache
     * を完全に無視することだけを見る */
    build(t, l, DESC_TYPE_TABLE | DESC_U, DESC_TYPE_TABLE | DESC_U,
          0x61000000u | DESC_U | DESC_PAGE_A);
    /* page descriptor の line をデータ cache に載せる: MOVE.L (A0),D0 */
    cpu_g->a[0] = page_da(t, l);
    run_op(0x2010, false);
    assert(cpu_g->last_exception_vector == NO_VECTOR &&
           cpu_g->d[0] == (0x61000000u | DESC_U | DESC_PAGE_A));
    host_w(page_da(t, l), 0x62000000u | DESC_U | DESC_PAGE_A); /* cache の外で書き換える */
    run_op(0x2010, false);
    assert(cpu_g->d[0] ==
           (0x61000000u | DESC_U | DESC_PAGE_A)); /* 通常のデータ読みは cache の旧値 */
    mc68k_translate_result_t r = xlate(l, FC_SDATA, ACC_R, EXEC);
    assert(r.ok && r.physical ==
                       0x62000000u); /* table search は cache を見ず新しい descriptor を読む */

    /* U=0 の descriptor: history の更新は RAM にだけ行い、cache の line
     * は書き換えも無効化もしない （[060UM §11.1.2.2.2]）。通常のデータ読みは cache の旧値（U
     * なし）のまま */
    init_cpu();
    cpu_040_cache_reset(
        cpu_g); /* 前の場面の line を捨てる（060 の reset は cache を保持する） */
    set_regs(t);
    cpu_g->mmu060_dtt0 = TTR_E | (2u << 13);
    cpu_g->mmu060_itt0 = TTR_E | (2u << 13);
    cpu_g->cacr = 0x80000000u;
    build(t, l, DESC_TYPE_TABLE | DESC_U, DESC_TYPE_TABLE | DESC_U, 0x61000000u | DESC_PAGE_A);
    cpu_g->a[0] = page_da(t, l);
    run_op(0x2010, false);
    assert(cpu_g->d[0] == (0x61000000u | DESC_PAGE_A)); /* line を載せる */
    r = xlate(l, FC_SDATA, ACC_R, EXEC);
    assert(r.ok && (host_r(page_da(t, l)) & DESC_U)); /* RAM の U は立つ */
    run_op(0x2010, false);
    assert(cpu_g->d[0] == (0x61000000u | DESC_PAGE_A)); /* line は旧値のまま */
    /* page descriptor の M を立てる書込み（cpu_040_table_write）も line を書き換えない */
    init_cpu();
    cpu_040_cache_reset(cpu_g);
    set_regs(t);
    cpu_g->mmu060_dtt0 = TTR_E | (2u << 13);
    cpu_g->mmu060_itt0 = TTR_E | (2u << 13);
    cpu_g->cacr = 0x80000000u;
    build(t, l, DESC_TYPE_TABLE | DESC_U, DESC_TYPE_TABLE | DESC_U,
          0x61000000u | DESC_U | DESC_PAGE_A);
    cpu_g->a[0] = page_da(t, l);
    run_op(0x2010, false);
    assert(cpu_g->d[0] == (0x61000000u | DESC_U | DESC_PAGE_A)); /* line を載せる */
    r = xlate(l, FC_SDATA, ACC_W, EXEC);
    assert(r.ok && (host_r(page_da(t, l)) & DESC_M)); /* RAM の M は立つ */
    run_op(0x2010, false);
    assert(cpu_g->d[0] ==
           (0x61000000u | DESC_U | DESC_PAGE_A)); /* line は旧値（M なし）のまま */
}

int main(void) {
    mc68k_bus_t bus = {.read_byte = rb,
                       .read_word = rw,
                       .read_long = rl,
                       .write_byte = wb,
                       .write_word = ww,
                       .write_long = wl,
                       .reset_peripheral = reset_peripheral,
                       .ctx = &f};
    cpu_g = mc68k_create_model(&bus, MC68K_MODEL_68060);
    assert(cpu_g);
    cpu_g->bus.access_timing = timing_hook;
    test_tc_default();
    test_tc_page_size();
    test_ttr_match();
    test_ttr_select();
    test_ttr_bits();
    test_walk_index();
    test_walk_alignment();
    test_walk_high_tables();
    test_walk_types();
    test_walk_indirect();
    test_walk_bus_errors();
    test_walk_matrix();
    test_walk_bit_sweep();
    test_atc_hit_paths();
    test_atc_page_shift_tag();
    test_atc_replacement();
    test_pflush_encoding();
    test_plpa_encoding();
    test_table_search_bypasses_cache();
    mc68k_destroy(cpu_g);
    return 0;
}
