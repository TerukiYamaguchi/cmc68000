/*
 * fpcp_machine.h — FPU-3の試験で共有する、RAM と CPU
 * 空間フックを持つ最小のマシン。
 *
 * RAM は 64KB。CPU 空間（FC=7）のアクセスは RAM に触れず、ホストのコプロセッサ用フックへ渡す
 * （fc・addr・size・値をログに残す）。期待値は production の定数から取らず、試験側で
 * [UM20 §7]・[881UM §7] のビット配置から組み立てる。
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "fpu_model_util.h"
#include "mc68000/cpu.h"

#define MACH_RAM           0x10000u
#define MACH_START         0x1000u
#define MACH_STACK         0x8000u
#define MACH_LOG_MAX       512
#define MACH_RESPONSES_MAX 64

/* CPU 空間アドレス（[UM20 Figure 7-4、7-5]）。cpID=1 の CIR は 0x22000 から */
#define CS_BASE(cpid) (0x20000u + ((uint32_t)(cpid) << 13))
#define CIR_RESPONSE  0x00
#define CIR_CONTROL   0x02
#define CIR_SAVE      0x04
#define CIR_RESTORE   0x06
#define CIR_OPWORD    0x08
#define CIR_COMMAND   0x0A
#define CIR_CONDITION 0x0E
#define CIR_OPERAND   0x10
#define CIR_REGSEL    0x14
#define CIR_IA        0x18

typedef struct {
    bool write;
    uint8_t fc;
    uint32_t addr;
    uint8_t size;
    uint32_t val;
} cs_log_t;

typedef struct {
    uint8_t ram[MACH_RAM];
    mc68k_state_t *cpu;

    /* ホストの偽コプロセッサ */
    bool host_enabled;
    cs_log_t log[MACH_LOG_MAX];
    int nlog;                               /* 記録した件数（MACH_LOG_MAX で頭打ち） */
    int nlog_total;                         /* 実際のアクセス回数 */
    uint16_t responses[MACH_RESPONSES_MAX]; /* 応答 CIR の読出しが順に返す値 */
    int nresponses, next_response;
    uint16_t save_format;      /* セーブ CIR の値 */
    uint16_t restore_readback; /* リストア CIR の読返し値 */
    uint32_t operand_in[16];   /* オペランド CIR の読出し値 */
    int n_operand_in, next_operand_in;
    uint16_t regsel; /* レジスタセレクト CIR の値 */
    int fail_cir;    /* このオフセットへのアクセスは失敗させる（-1 = なし） */
    /* CIR の読出しで、size < 4 のとき size バイトより上位のビットに値を混ぜて返す（契約は下位
     * size バイト だけ）。全 size・全 CIR
     * が対象。実装側のマスクは呼び出し側のマスクと重複しており、これを外す変異は
     * 等価（スクリプト冒頭に記録）。契約の確認用 */
    bool dirty_upper;
    int fail_after; /* 失敗させるまでに許す、そのオフセットへの成功回数 */
    /* RAM への書込みで、先頭アドレスがこれと一致するアクセスだけを失敗させる（fail_write_en が
     * 真のとき）。RAM の外へ出る書込みと違い、続きの書込みは成功する */
    bool fail_write_en;
    uint32_t fail_write_addr;
    /* fail_write_addr の読出し版。先頭アドレスが一致する読出しだけを失敗させる */
    bool fail_read_en;
    uint32_t fail_read_addr;
} mach_t;

static inline void put16(mach_t *m, uint32_t a, uint16_t v) {
    m->ram[a] = (uint8_t)(v >> 8);
    m->ram[a + 1] = (uint8_t)v;
}

static inline void put32(mach_t *m, uint32_t a, uint32_t v) {
    put16(m, a, (uint16_t)(v >> 16));
    put16(m, a + 2, (uint16_t)v);
}

static inline uint16_t get16(const mach_t *m, uint32_t a) {
    return (uint16_t)((m->ram[a] << 8) | m->ram[a + 1]);
}

static inline uint32_t get32(const mach_t *m, uint32_t a) {
    return ((uint32_t)get16(m, a) << 16) | get16(m, a + 2);
}

static bool mach_read_fails(const mach_t *m, uint32_t a) {
    return m->fail_read_en && m->fail_read_addr == a;
}

static bool mach_rb(void *p, uint32_t a, uint8_t *v) {
    if (a >= MACH_RAM || mach_read_fails(p, a))
        return false;
    *v = ((mach_t *)p)->ram[a];
    return true;
}

static bool mach_rw(void *p, uint32_t a, uint16_t *v) {
    if (a + 1 >= MACH_RAM || mach_read_fails(p, a))
        return false;
    *v = get16(p, a);
    return true;
}

static bool mach_rl(void *p, uint32_t a, uint32_t *v) {
    if (a + 3 >= MACH_RAM || mach_read_fails(p, a))
        return false;
    *v = get32(p, a);
    return true;
}

static bool mach_write_fails(const mach_t *m, uint32_t a) {
    return m->fail_write_en && m->fail_write_addr == a;
}

static bool mach_wb(void *p, uint32_t a, uint8_t v) {
    if (a >= MACH_RAM || mach_write_fails(p, a))
        return false;
    ((mach_t *)p)->ram[a] = v;
    return true;
}

static bool mach_ww(void *p, uint32_t a, uint16_t v) {
    if (a + 1 >= MACH_RAM || mach_write_fails(p, a))
        return false;
    put16(p, a, v);
    return true;
}

static bool mach_wl(void *p, uint32_t a, uint32_t v) {
    if (a + 3 >= MACH_RAM || mach_write_fails(p, a))
        return false;
    put32(p, a, v);
    return true;
}

/* dirty_upper で上位に混ぜるビット（size バイトより上位だけが使われる） */
#define MACH_DIRTY_UPPER 0xABCDEF00u

static bool mach_cir_fails(mach_t *m, uint32_t addr) {
    if (m->fail_cir < 0 || (addr & 0x1F) != (uint32_t)m->fail_cir)
        return false;
    if (m->fail_after > 0) {
        m->fail_after--;
        return false;
    }
    return true;
}

static void mach_log(mach_t *m, bool write, uint8_t fc, uint32_t addr, uint8_t size,
                     uint32_t val) {
    m->nlog_total++;
    if (m->nlog < MACH_LOG_MAX)
        m->log[m->nlog++] = (cs_log_t){write, fc, addr, size, val};
}

static bool mach_cs_read(void *p, uint8_t fc, uint32_t addr, uint8_t size, uint32_t *out) {
    mach_t *m = p;
    uint32_t v = 0;

    if (mach_cir_fails(m, addr)) {
        mach_log(m, false, fc, addr, size, 0xDEADu);
        return false;
    }
    switch (addr & 0x1F) {
    case CIR_RESPONSE:
        CHECK(m->next_response < m->nresponses);
        v = m->responses[m->next_response];
        if (m->next_response + 1 < m->nresponses)
            m->next_response++; /* 最後の応答は読むたびに返し続ける */
        break;
    case CIR_SAVE:
        v = m->save_format;
        break;
    case CIR_RESTORE:
        v = m->restore_readback;
        break;
    case CIR_OPERAND:
        CHECK(m->next_operand_in < m->n_operand_in);
        v = m->operand_in[m->next_operand_in++];
        break;
    case CIR_REGSEL:
        v = m->regsel;
        break;
    default:
        break;
    }
    mach_log(m, false, fc, addr, size, v);
    if (m->dirty_upper && size < 4)
        v |= MACH_DIRTY_UPPER & ~((UINT32_C(1) << (8u * size)) - 1u);
    *out = v;
    return true;
}

static bool mach_cs_write(void *p, uint8_t fc, uint32_t addr, uint8_t size, uint32_t val) {
    mach_t *m = p;

    mach_log(m, true, fc, addr, size, val);
    return !mach_cir_fails(m, addr);
}

/* 例外ベクタ番号ごとのハンドラの置き場所（ハンドラは何もしない1命令 NOP の後に停止用の STOP
 * を置く）。 */
#define MACH_HANDLER(vec) (0x2000u + (uint32_t)(vec) * 0x10u)

static mach_t *mach_create(mc68k_model_t model, mc68k_fpu_config_t fpu, bool host) {
    mach_t *m = calloc(1, sizeof(*m));
    mc68k_bus_t bus;

    CHECK(m);
    memset(&bus, 0, sizeof(bus));
    bus.read_byte = mach_rb;
    bus.read_word = mach_rw;
    bus.read_long = mach_rl;
    bus.write_byte = mach_wb;
    bus.write_word = mach_ww;
    bus.write_long = mach_wl;
    bus.ctx = m;
    if (host) {
        bus.cpu_space_read = mach_cs_read;
        bus.cpu_space_write = mach_cs_write;
        m->host_enabled = true;
    }
    m->fail_cir = -1;
    put32(m, 0, MACH_STACK);
    put32(m, 4, MACH_START);
    for (int vec = 2; vec < 64; vec++) {
        put32(m, (uint32_t)vec * 4, MACH_HANDLER(vec));
        put16(m, MACH_HANDLER(vec), 0x4E71); /* NOP */
    }
    m->cpu = mc68k_create_model_with_fpu(&bus, model, fpu);
    CHECK(m->cpu);
    mc68k_reset(m->cpu);
    return m;
}

static void mach_destroy(mach_t *m) {
    mc68k_destroy(m->cpu);
    free(m);
}

/* 1命令ずつ実行する。戻り値は実行した回数 */
static int mach_run_until(mach_t *m, uint32_t pc, int max_steps) {
    int n = 0;

    while (mc68k_get_pc(m->cpu) != pc && n < max_steps) {
        mc68k_execute(m->cpu, 1);
        n++;
    }
    return n;
}

static void mach_response(mach_t *m, uint16_t r) {
    CHECK(m->nresponses < MACH_RESPONSES_MAX);
    m->responses[m->nresponses++] = r;
}
