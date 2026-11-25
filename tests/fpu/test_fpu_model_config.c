/*
 * test_fpu_model_config.c — FPU 構成 API（ / #62 Phase A、
 * 現行仕様、現行仕様）。
 *
 * 全 CPU model × FPU config の受理表（6 × 5 の30セルと未定義 enum 値）を、表の各セルを手で
 * 書いて検査する。受理表は production の関数から取らず、設計書 §3.1 の表を写した。
 *
 * 落とす変異: 受理表の1セルの反転、未定義 enum 値の受理、getter が別の構成を返す、reset が
 * FPU を残す、互換ラッパ（2引数版）が NONE 以外を渡す、2引数版のシグネチャ変更。
 */
#include <string.h>
#include "cpu/cpu_internal.h"
#include "fpu_model_util.h"
#include "mc68000/cpu.h"

typedef struct {
    unsigned char m[0x4000];
} ram_t;

static bool rb(void *p, uint32_t a, uint8_t *v) {
    *v = ((ram_t *)p)->m[a];
    return true;
}

static bool rw(void *p, uint32_t a, uint16_t *v) {
    ram_t *c = p;

    *v = (uint16_t)((c->m[a] << 8) | c->m[a + 1]);
    return true;
}

static bool rl(void *p, uint32_t a, uint32_t *v) {
    ram_t *c = p;

    *v = ((uint32_t)c->m[a] << 24) | ((uint32_t)c->m[a + 1] << 16) |
         ((uint32_t)c->m[a + 2] << 8) | c->m[a + 3];
    return true;
}

static bool wb(void *p, uint32_t a, uint8_t v) {
    ((ram_t *)p)->m[a] = v;
    return true;
}

static bool ww(void *p, uint32_t a, uint16_t v) {
    ram_t *c = p;

    c->m[a] = (uint8_t)(v >> 8);
    c->m[a + 1] = (uint8_t)v;
    return true;
}

static bool wl(void *p, uint32_t a, uint32_t v) {
    ram_t *c = p;

    c->m[a] = (uint8_t)(v >> 24);
    c->m[a + 1] = (uint8_t)(v >> 16);
    c->m[a + 2] = (uint8_t)(v >> 8);
    c->m[a + 3] = (uint8_t)v;
    return true;
}

static mc68k_bus_t make_bus(ram_t *ram) {
    mc68k_bus_t b;

    memset(&b, 0, sizeof(b));
    b.read_byte = rb;
    b.read_word = rw;
    b.read_long = rl;
    b.write_byte = wb;
    b.write_word = ww;
    b.write_long = wl;
    b.ctx = ram;
    return b;
}

/* 現行仕様 と 現行仕様 の受理表。
 * 行: CPU model、列: NONE / 68881 / 68882 / 68040 / NONE_040 / 68060 / NONE_060。1=受理。
 * 060-E（#111）の T-E1: 設計書 §8.2 の 7 機種 × 7 構成値の全 49 セル。 */
enum { FPU_CONFIG_COLUMNS = 7 };
static const struct {
    mc68k_model_t model;
    int accept[FPU_CONFIG_COLUMNS];
} accept_table[] = {
    {MC68K_MODEL_68000, {1, 0, 0, 0, 0, 0, 0}}, {MC68K_MODEL_68010, {1, 0, 0, 0, 0, 0, 0}},
    {MC68K_MODEL_68020, {1, 1, 1, 0, 0, 0, 0}}, {MC68K_MODEL_68EC030, {1, 1, 1, 0, 0, 0, 0}},
    {MC68K_MODEL_68030, {1, 1, 1, 0, 0, 0, 0}}, {MC68K_MODEL_68040, {1, 0, 0, 1, 1, 0, 0}},
    {MC68K_MODEL_68060, {1, 0, 0, 0, 0, 1, 0}},
};

static void test_accept_table(void) {
    ram_t ram;
    mc68k_bus_t bus;
    unsigned cells = 0, accepted = 0, rejected = 0;
    size_t row;
    unsigned col;

    memset(&ram, 0, sizeof(ram));
    bus = make_bus(&ram);
    for (row = 0; row < sizeof(accept_table) / sizeof(accept_table[0]); row++) {
        for (col = 0; col < FPU_CONFIG_COLUMNS; col++) {
            mc68k_state_t *cpu = mc68k_create_model_with_fpu(&bus, accept_table[row].model,
                                                             (mc68k_fpu_config_t)col);

            cells++;
            if (accept_table[row].accept[col]) {
                CHECK(cpu != NULL);
                CHECK_EQ(mc68k_get_model(cpu), accept_table[row].model);
                CHECK_EQ(cpu->fpu_config, col);
                if (col == MC68K_FPU_68881 || col == MC68K_FPU_68882) {
                    mc68k_fpu_t *f = mc68k_get_fpu(cpu);

                    CHECK(f != NULL);
                    CHECK(mc68k_get_fpu_const(cpu) == f);
                    CHECK_EQ(mc68k_fpu_get_config(f), col);
                    CHECK(cpu->fpu040 == NULL);
                } else if (col == MC68K_FPU_68040 || col == MC68K_FPU_68060) {
                    CHECK(mc68k_get_fpu(cpu) == NULL); /* 内蔵FPUはCIRモデルではない */
                    CHECK(cpu->fpu040 != NULL);
                    CHECK_EQ(cpu->fpu040->state.fpcr_enable, 0);
                    CHECK_EQ(cpu->fpu040->state.fpsr_exc, 0);
                } else {
                    CHECK(mc68k_get_fpu(cpu) == NULL);
                    CHECK(mc68k_get_fpu_const(cpu) == NULL);
                    CHECK(cpu->fpu040 == NULL);
                }
                accepted++;
                mc68k_destroy(cpu);
            } else {
                CHECK(cpu == NULL);
                rejected++;
            }
        }
        CHECK_EQ(col, FPU_CONFIG_COLUMNS); /* ループが縮まない */
    }
    CHECK_EQ(cells, 49u);
    CHECK_EQ(accepted, 16u);
    CHECK_EQ(rejected, 33u);
}

static void test_undefined_values(void) {
    ram_t ram;
    mc68k_bus_t bus;
    int bad[] = {-1, 7, 8, 99};
    size_t i;
    unsigned checked = 0;

    memset(&ram, 0, sizeof(ram));
    bus = make_bus(&ram);
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        CHECK(mc68k_create_model_with_fpu(&bus, MC68K_MODEL_68030,
                                          (mc68k_fpu_config_t)bad[i]) == NULL);
        CHECK(mc68k_create_model_with_fpu(&bus, MC68K_MODEL_68000,
                                          (mc68k_fpu_config_t)bad[i]) == NULL);
        CHECK(mc68k_fpu_create((mc68k_fpu_config_t)bad[i]) == NULL);
        checked++;
    }
    CHECK_EQ(checked, 4u); /* bad[] の要素数。ループの縮小を見逃さない */
    CHECK(mc68k_fpu_create(MC68K_FPU_NONE) == NULL);
    CHECK(mc68k_get_fpu(NULL) == NULL);
    CHECK(mc68k_get_fpu_const(NULL) == NULL);
    CHECK_EQ(mc68k_fpu_get_config(NULL), MC68K_FPU_NONE);
    mc68k_fpu_destroy(NULL); /* 何もしない */
}

/* 旧2引数 mc68k_create_model()/mc68k_create() を x68000 の既存呼出しと同じ形で呼ぶ */
static void test_compat_wrappers(void) {
    ram_t ram;
    mc68k_bus_t bus;
    mc68k_state_t *cpu;
    mc68k_model_t models[] = {MC68K_MODEL_68000,   MC68K_MODEL_68010, MC68K_MODEL_68020,
                              MC68K_MODEL_68EC030, MC68K_MODEL_68030, MC68K_MODEL_68040};
    size_t i;

    memset(&ram, 0, sizeof(ram));
    bus = make_bus(&ram);
    CHECK_EQ(sizeof(models) / sizeof(models[0]),
             6u); /* models の要素数。表の行の削除・ループの縮小を見逃さない */
    for (i = 0; i < sizeof(models) / sizeof(models[0]); i++) {
        cpu = mc68k_create_model(&bus, models[i]);
        CHECK(cpu != NULL);
        CHECK(mc68k_get_fpu(cpu) == NULL); /* 互換ラッパは NONE を渡す */
        mc68k_destroy(cpu);
    }
    CHECK_EQ(i, 6u); /* ループが縮まない */
    cpu = mc68k_create(&bus);
    CHECK(cpu != NULL);
    CHECK_EQ(mc68k_get_model(cpu), MC68K_MODEL_68000);
    CHECK(mc68k_get_fpu(cpu) == NULL);
    mc68k_destroy(cpu);
}

static void test_040_reset(void) {
    ram_t ram;
    mc68k_bus_t bus;
    mc68k_state_t *cpu;
    unsigned n;

    memset(&ram, 0, sizeof(ram));
    ram.m[2] = 0x30; /* SSP=0x3000 */
    ram.m[7] = 0x40; /* PC=0x40 */
    bus = make_bus(&ram);
    cpu = mc68k_create_model_with_fpu(&bus, MC68K_MODEL_68040, MC68K_FPU_68040);
    CHECK(cpu != NULL);
    CHECK(cpu->fpu040 != NULL);
    cpu->fpu040->state.fp[3].sign_exp = 0;
    cpu->fpu040->state.fp[3].signif = 0;
    cpu->fpu040->state.fpcr_enable = 0xFF;
    cpu->fpu040->state.fpsr_exc = 0xFF;
    mc68k_reset(cpu);
    for (n = 0; n < 8; n++) {
        CHECK_EQ(cpu->fpu040->state.fp[n].sign_exp, 0x7FFF);
        CHECK_EQ(cpu->fpu040->state.fp[n].signif, UINT64_MAX);
    }
    CHECK_EQ(n, 8u); /* ループが縮まない */
    CHECK_EQ(cpu->fpu040->state.fpcr_enable, 0);
    CHECK_EQ(cpu->fpu040->state.fpsr_exc, 0);
    mc68k_destroy(cpu);
}

/* mc68k_reset() は FPU を reset する（FPn=NaN、FPCR/FPSR/FPIAR=0、context_valid
 * 偽、phase=idle） */
static void test_reset(void) {
    ram_t ram;
    mc68k_bus_t bus;
    mc68k_state_t *cpu;
    mc68k_fpu_t *f;
    uint16_t fmt;
    size_t bytes;
    unsigned i;

    memset(&ram, 0, sizeof(ram));
    ram.m[2] = 0x30; /* SSP=0x3000 */
    ram.m[7] = 0x40; /* PC=0x40 */
    bus = make_bus(&ram);
    for (i = 1; i <= 2; i++) {
        cpu = mc68k_create_model_with_fpu(&bus, MC68K_MODEL_68030, (mc68k_fpu_config_t)i);
        CHECK(cpu != NULL);
        f = mc68k_get_fpu(cpu);
        CHECK(f != NULL);
        load_long(f, 0, 5);
        set_fpcr(f, 0x00001090u);
        CHECK(mc68k_fpu_write_command_word(f,
                                           CMD(2, FMT_L, 1, OPM_FMOVE))); /* ダイアログの途中 */
        rd_kind(f, MC68K_FPU_RESPONSE_EA_TRANSFER); /* FMOVE.L <ea>,FPn の EA 転送 */
        mc68k_reset(cpu);
        {
            mc68k_fpu_regs_t r = regs(f);
            unsigned n;

            for (n = 0; n < 8; n++) {
                CHECK_EQ(r.fp[n].sign_exp, 0x7FFF);
                CHECK_EQ(r.fp[n].signif, UINT64_MAX); /* 非シグナリング NaN（[881UM §2.1]） */
            }
            CHECK_EQ(n, 8); /* ループが縮まない */
            CHECK_EQ(r.fpcr, 0);
            CHECK_EQ(r.fpsr, 0);
            CHECK_EQ(r.fpiar, 0);
        }
        expect_idle(f); /* phase=idle: 進行中のダイアログは残らない */
        mc68k_fpu_begin_save(f, &fmt, &bytes);
        CHECK_EQ(fmt, 0); /* context_valid 偽: null frame */
        CHECK_EQ(bytes, 0);
        mc68k_destroy(cpu);
    }
    CHECK_EQ(i, 3u); /* 68881・68882 の2機種 */
}

int main(void) {
    test_accept_table();
    test_undefined_values();
    test_compat_wrappers();
    test_reset();
    test_040_reset();
    printf("test_fpu_model_config: OK\n");
    return 0;
}
