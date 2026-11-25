/*
 * MC68060-A T-A9: 逆アセンブラ。020+ の表示が 060 で 020 と同じこと、$01000000 以上の
 * アドレスを 24bit に丸めないこと、060 の LPSTOP/PLPA/CINV/CPUSH/PFLUSH/MOVE16/FP の表示。
 */
#include "mc68000/cpu.h"
#include "mc68000/debug.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define RAM_SIZE   0x10000u
#define HIGH_BASE  0x01000000u
#define HIGH_ALIAS 0x8000u

typedef struct {
    uint8_t ram[RAM_SIZE];
} ctx_t;

static uint32_t phys(uint32_t a) {
    return a >= HIGH_BASE ? a - HIGH_BASE + HIGH_ALIAS : a;
}
static bool rb(void *c, uint32_t a, uint8_t *v) {
    *v = ((ctx_t *)c)->ram[phys(a)];
    return true;
}
static bool rw(void *c, uint32_t a, uint16_t *v) {
    uint8_t h, l;
    rb(c, a, &h);
    rb(c, a + 1, &l);
    *v = (uint16_t)(h << 8 | l);
    return true;
}
static bool rl(void *c, uint32_t a, uint32_t *v) {
    uint16_t h, l;
    rw(c, a, &h);
    rw(c, a + 2, &l);
    *v = (uint32_t)h << 16 | l;
    return true;
}
static bool wb(void *c, uint32_t a, uint8_t v) {
    ((ctx_t *)c)->ram[phys(a)] = v;
    return true;
}
static bool ww(void *c, uint32_t a, uint16_t v) {
    wb(c, a, (uint8_t)(v >> 8));
    wb(c, a + 1, (uint8_t)v);
    return true;
}
static bool wl(void *c, uint32_t a, uint32_t v) {
    ww(c, a, (uint16_t)(v >> 16));
    ww(c, a + 2, (uint16_t)v);
    return true;
}
static uint8_t pb(void *c, uint32_t a) {
    return ((ctx_t *)c)->ram[phys(a)];
}
static uint16_t pw(void *c, uint32_t a) {
    return (uint16_t)(pb(c, a) << 8 | pb(c, a + 1));
}
static uint32_t pl(void *c, uint32_t a) {
    return (uint32_t)pw(c, a) << 16 | pw(c, a + 2);
}

static mc68k_state_t *make(ctx_t *t, mc68k_model_t model) {
    mc68k_bus_t bus = {.read_byte = rb,
                       .read_word = rw,
                       .read_long = rl,
                       .write_byte = wb,
                       .write_word = ww,
                       .write_long = wl,
                       .ctx = t};
    mc68k_state_t *cpu = mc68k_create_model(&bus, model);
    assert(cpu);
    mc68k_set_peek_hooks(cpu, pb, pw, pl, t);
    return cpu;
}
static void put(ctx_t *t, uint32_t a, const uint16_t *w, int n) {
    for (int i = 0; i < n; ++i)
        ww(t, a + (uint32_t)i * 2u, w[i]);
}
static int dis(mc68k_state_t *cpu, ctx_t *t, uint32_t a, const uint16_t *w, int n, char *out) {
    memset(t->ram, 0, 64);
    put(t, a, w, n);
    return mc68k_disassemble(cpu, a, out, 128);
}
#define CHECK(cpu, t, expect, len, ...)                                       \
    do {                                                                      \
        const uint16_t w_[] = {__VA_ARGS__};                                  \
        char o_[128];                                                         \
        int n_ = dis(cpu, t, 0x100, w_, (int)(sizeof w_ / sizeof w_[0]), o_); \
        assert(!strcmp(o_, expect));                                          \
        assert(n_ == (len));                                                  \
    } while (0)

/* 060 の非 FPU F-line（$F400-$F8FF）を全 opcode で、仕様から書いた参照と照合する。
 * 拡張語は opcode ごとに「有効な値」と「1 ビットだけ崩した値」を試す。 */
typedef struct {
    char text[64];
    int len;
} ref_t;
static void ref_line_f(uint16_t op, uint16_t ext, uint32_t abs, ref_t *r) {
    static const char *const cache_names[4] = {"", "DC", "IC", "BC"};
    static const char *const scope_names[4] = {"", "L", "P", "A"};
    const int an = op & 7;
    strcpy(r->text, "LINE-F");
    r->len = 2;
    if (op == 0xf800) {
        if (ext == 0x01c0) {
            snprintf(r->text, sizeof r->text, "LPSTOP #$%04X", (unsigned)(abs >> 16));
            r->len = 6;
        }
        return;
    }
    if (op >= 0xf500 && op <= 0xf51f) {
        static const char *const names[4] = {"PFLUSHN", "PFLUSH", "PFLUSHAN", "PFLUSHA"};
        const int form = (op >> 3) & 3;
        if (form < 2)
            snprintf(r->text, sizeof r->text, "%s (A%d)", names[form], an);
        else
            strcpy(r->text, names[form]);
        return;
    }
    if ((op & 0xfff8) == 0xf588 || (op & 0xfff8) == 0xf5c8) {
        snprintf(r->text, sizeof r->text, "%s (A%d)",
                 (op & 0xfff8) == 0xf588 ? "PLPAW" : "PLPAR", an);
        return;
    }
    if ((op & 0xff00) == 0xf400) {
        const int cache = (op >> 6) & 3, scope = (op >> 3) & 3;
        if (cache == 0 || scope == 0)
            return;
        const char *m = (op & 0x20) ? "CPUSH" : "CINV";
        if (scope == 3)
            snprintf(r->text, sizeof r->text, "%s%s %s", m, scope_names[scope],
                     cache_names[cache]);
        else
            snprintf(r->text, sizeof r->text, "%s%s %s,(A%d)", m, scope_names[scope],
                     cache_names[cache], an);
        return;
    }
    if (op >= 0xf600 && op <= 0xf63f) {
        if ((op & 0xfff8) == 0xf620) {
            if ((ext & 0x8fff) != 0x8000)
                return;
            snprintf(r->text, sizeof r->text, "MOVE16 (A%d)+,(A%d)+", an, (ext >> 12) & 7);
            r->len = 4;
            return;
        }
        if (op >= 0xf620)
            return;
        static const char *const forms[4] = {"MOVE16 (A%d)+,$%08X.L", "MOVE16 $%08X.L,(A%d)+",
                                             "MOVE16 (A%d),$%08X.L", "MOVE16 $%08X.L,(A%d)"};
        const int form = (op >> 3) & 3;
        if (form & 1)
            snprintf(r->text, sizeof r->text, forms[form], abs, an);
        else
            snprintf(r->text, sizeof r->text, forms[form], an, abs);
        r->len = 6;
    }
}
static void sweep_line_f(mc68k_state_t *cpu, ctx_t *t) {
    static const uint16_t exts[] = {
        0x9000, 0x8000, 0xf000, 0x0000, 0x01c0, 0x1000, 0x2000, 0x4000, 0x0001, 0x0002, 0x0004,
        0x0008, 0x0010, 0x0020, 0x0040, 0x0080, 0x0100, 0x0200, 0x0400, 0x0800, 0x8001, 0x8002,
        0x8004, 0x8008, 0x8010, 0x8020, 0x8040, 0x8080, 0x8100, 0x8200, 0x8400, 0x8800, 0xc000,
        0xa000, 0xe000, 0x81c0, 0x00c0, 0x03c0, 0x05c0, 0x09c0, 0x11c0, 0x21c0, 0x41c0, 0xffff};
    for (uint32_t op = 0xf400; op <= 0xf8ff; ++op) {
        for (unsigned xi = 0; xi < sizeof exts / sizeof exts[0]; ++xi) {
            const uint16_t ext = exts[xi];
            const uint32_t abs = 0x89abcdefu;
            const uint16_t w[] = {(uint16_t)op, ext, (uint16_t)(abs >> 16), (uint16_t)abs};
            char out[128];
            memset(t->ram, 0, 64);
            put(t, 0x100, w, 4);
            const int n = mc68k_disassemble(cpu, 0x100, out, sizeof out);
            ref_t ref;
            /* LPSTOP の即値と MOVE16 の絶対アドレスは第 2 語以降。LPSTOP は ext の次の語が即値
             */
            if (op == 0xf800)
                ref_line_f((uint16_t)op, ext, (uint32_t)(abs & 0xffff0000u), &ref);
            else if ((op & 0xfff8) == 0xf620)
                ref_line_f((uint16_t)op, ext, 0, &ref);
            else
                ref_line_f((uint16_t)op, ext, ((uint32_t)ext << 16) | (uint16_t)(abs >> 16),
                           &ref);
            if (op == 0xf800) {
                /* w[2] = abs 上位 = 即値 */
                if (ref.len == 6)
                    snprintf(ref.text, sizeof ref.text, "LPSTOP #$%04X", (unsigned)(abs >> 16));
            }
            if (strcmp(out, ref.text) != 0 || n != ref.len) {
                fprintf(stderr, "op=%04X ext=%04X: got '%s' len=%d, want '%s' len=%d\n", op,
                        ext, out, n, ref.text, ref.len);
                assert(0);
            }
        }
    }
}

int main(void) {
    static ctx_t t;
    mc68k_state_t *c60 = make(&t, MC68K_MODEL_68060);
    mc68k_state_t *c20 = make(&t, MC68K_MODEL_68020);

    /* 060 固有 */
    CHECK(c60, &t, "LPSTOP #$2000", 6, 0xf800, 0x01c0, 0x2000);
    CHECK(c60, &t, "LINE-F", 2, 0xf800, 0x0000, 0x2000);
    CHECK(c60, &t, "PFLUSHAN", 2, 0xf510);
    CHECK(c60, &t, "PFLUSHA", 2, 0xf518);
    CHECK(c60, &t, "PFLUSHN (A1)", 2, 0xf501);
    CHECK(c60, &t, "PFLUSH (A1)", 2, 0xf509);
    CHECK(c60, &t, "PLPAW (A2)", 2, 0xf58a);
    CHECK(c60, &t, "PLPAR (A3)", 2, 0xf5cb);
    CHECK(c60, &t, "LINE-F", 2, 0xf548); /* PTESTW は 060 に無い */
    CHECK(c60, &t, "LINE-F", 2, 0xf568);
    CHECK(c60, &t, "CINVL DC,(A0)", 2, 0xf448);
    CHECK(c60, &t, "CINVP IC,(A1)", 2, 0xf491);
    CHECK(c60, &t, "CINVA BC", 2, 0xf4d8);
    CHECK(c60, &t, "CPUSHL DC,(A0)", 2, 0xf468);
    CHECK(c60, &t, "CPUSHP BC,(A7)", 2, 0xf4f7 & ~0x08u);
    CHECK(c60, &t, "CPUSHA IC", 2, 0xf498 | 0x20);
    CHECK(c60, &t, "LINE-F", 2, 0xf400); /* cache=00 */
    CHECK(c60, &t, "LINE-F", 2, 0xf4c0); /* scope=00 */
    CHECK(c60, &t, "MOVE16 (A0)+,(A1)+", 4, 0xf620, 0x9000);
    CHECK(c60, &t, "LINE-F", 2, 0xf620, 0x0000);
    CHECK(c60, &t, "MOVE16 (A2)+,$00123450.L", 6, 0xf602, 0x0012, 0x3450);
    CHECK(c60, &t, "MOVE16 $00123450.L,(A2)+", 6, 0xf60a, 0x0012, 0x3450);
    CHECK(c60, &t, "MOVE16 (A2),$00123450.L", 6, 0xf612, 0x0012, 0x3450);
    CHECK(c60, &t, "MOVE16 $00123450.L,(A2)", 6, 0xf61a, 0x0012, 0x3450);
    CHECK(c60, &t, "FNOP", 4, 0xf280, 0x0000);

    /* 020+ の表示は 060 で 020 と同じ。 */
    static const struct {
        uint16_t w[4];
        int n;
    } same[] = {
        {{0x60ff, 0x0000, 0x0100, 0}, 3},      /* BRA.L */
        {{0x61ff, 0x0000, 0x0100, 0}, 3},      /* BSR.L */
        {{0x2030, 0x1c08, 0, 0}, 2},           /* MOVE.L (8,A0,D1.L*4),D0 */
        {{0x0cd0, 0x0081, 0, 0}, 2},           /* CAS.W */
        {{0x00d0, 0x0800, 0, 0}, 2},           /* CHK2.B */
        {{0x0cfc, 0x8001, 0x8001, 0}, 3},      /* CAS2.W */
        {{0xe8d0, 0x0842, 0, 0}, 2},           /* BFEXTU */
        {{0x4eb9, 0x0123, 0x4568, 0}, 3},      /* JSR abs.L */
        {{0x4e50, 0xfff0, 0, 0}, 2},           /* LINK */
        {{0x4808, 0x0000, 0x1000, 0}, 3},      /* LINK.L */
        {{0x4c3c, 0x0400, 0x0000, 0x0004}, 4}, /* MULU.L #imm */
        {{0x49c0, 0, 0, 0}, 1},                /* EXTB.L */
        {{0x4a28, 0x0004, 0, 0}, 2},           /* TST.B (d16,A0) */
        {{0xf200, 0x0080, 0, 0}, 2},           /* FMOVE.X FP0,FP1 */
        {{0xf200, 0x0022, 0, 0}, 2},           /* FADD.X FP0,FP1 */
        {{0xf200, 0x4422, 0, 0}, 2},           /* FADD.S (A0)? 形式 */
        {{0xf281, 0x0010, 0, 0}, 2},           /* FBEQ.W */
        {{0xf2c1, 0x0000, 0x0100, 0}, 3},      /* FBEQ.L */
        {{0xf310, 0, 0, 0}, 1},                /* FSAVE (A0) */
        {{0xf350, 0, 0, 0}, 1},                /* FRESTORE (A0) */
        {{0xf240, 0x0001, 0, 0}, 2},           /* FSEQ D0 */
        {{0xf248, 0x0001, 0x0010, 0}, 3},      /* FDBEQ D0 */
        {{0xf210, 0xd0c0, 0, 0}, 2},           /* FMOVEM.X (A0),FP0-FP1 */
    };
    for (unsigned i = 0; i < sizeof same / sizeof same[0]; ++i) {
        char a[128], b[128];
        int n60 = dis(c60, &t, 0x100, same[i].w, same[i].n, a);
        int n20 = dis(c20, &t, 0x100, same[i].w, same[i].n, b);
        assert(!strcmp(a, b) && n60 == n20);
        assert(strcmp(a, "ILLEGAL") != 0);
        if (same[i].w[0] >= 0xf200 && same[i].w[0] < 0xf400)
            assert(strcmp(a, "LINE-F") != 0); /* 060 でも cpID 1 は FPU 命令として表示する */
    }
    CHECK(c60, &t, "FMOVE.X FP0,FP1", 4, 0xf200, 0x0080);
    /* $01000000 以上のアドレスを 24bit に丸めない */
    {
        const uint16_t w[] = {0x60ff, 0x0000, 0x0100};
        char o[128];
        int n = dis(c60, &t, HIGH_BASE, w, 3, o);
        assert(n == 6 && !strcmp(o, "BRA $01000102"));
        const uint16_t j[] = {0x4eb9, 0x0123, 0x4568};
        n = dis(c60, &t, HIGH_BASE, j, 3, o);
        assert(n == 6 && strstr(o, "$01234568"));
    }
    sweep_line_f(c60, &t);
    mc68k_destroy(c60);
    mc68k_destroy(c20);
    return 0;
}
