/*
 *  (現行仕様) mc68k_executed_cycles() の単体テスト。
 *
 * 同じプログラムを「1回の大きな mc68k_execute()」と「mc68k_execute(cpu,1) の
 * 1命令ずつ」で実行し、バス書込みコールバック内で見えた値（呼出し前の累計 + 値）が
 * 一致することを確かめる。ステップ実行側は、命令の途中では必ず「その呼出しの先頭」
 * ＝ 0 から数えるため、期待値の源泉を production の値に依存させない
 * （ステップごとの mc68k_execute() 戻り値の累積）。
 */
#include "mc68000/cpu.h"
#include "mc68000/debug.h"
#include "cpu/cpu_internal.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#define RAM_SIZE             0x4000
#define LOG_MAX              64
#define STEP_MAX             256
#define RUN_CYCLES           100000
#define PROG_PC              0x300
#define ADDR_ERR_HANDLER     0x500
#define IRQ5_HANDLER         0x600
#define IRQ5_VECTOR_ADDR     0x74
#define ADDR_ERR_VECTOR_ADDR 0x0C
#define WAIT_PER_ACCESS      3
#define BUS_ERR_HANDLER      0x700
#define BUS_ERR_VECTOR_ADDR  0x08
#define SSP_INIT             0x3000
#define MSP_INIT             0x2000
#define SR_SUPERVISOR_MASTER 0x3000 /* S=1, M=1, 割り込みマスク 0 */
#define ISP_THROWAWAY_SIZE   8
#define IRQ_LEVEL_1          1
#define IRQ_LEVEL_5          5
#define IRQ5_ACK_ADDR        0x0106 /* IRQ ハンドラ内の書込み。pull 型ではこれで線を下げる */
#define TRACE_HANDLER        0x800
#define TRACE_VECTOR_ADDR    0x24
#define FAIL_STORE_ADDR      0x0110
#define G0_HANDLER_STORE     0x0114
#define TRACE_HANDLER_STORE  0x0120
#define SR_TRACE_SUPERVISOR  0xA000 /* T1=1, S=1, 割り込みマスク 0 */
#define STACK_FRAME_LOW      0x2F00 /* SSP_INIT 直下のフレーム書込みの下限 */
/* 68020 の保留グループ0例外ディスパッチ（IRQ 受付の $1 フレーム書込み失敗由来）の
 * 合計サイクル。現行実装の値の回帰固定で、一次資料との照合はしていない（推論）。 */
#define PENDING_G0_DISPATCH_CYCLES 50

typedef struct {
    unsigned char m[RAM_SIZE];
    mc68k_state_t *cpu;
    int base; /* 現在の mc68k_execute() 呼出しの直前までの累計 */
    int log[LOG_MAX];
    int raw[LOG_MAX]; /* base を足す前の mc68k_executed_cycles() の値 */
    uint32_t log_addr[LOG_MAX];
    int log_n;
    int wait;
    int irq_level; /* pull 型 irq_level_fn が返す値 */
    bool fail_en;  /* fail_addr への書込みをバスエラーにする */
    uint32_t fail_addr;
} ctx_t;

static bool rb(void *p, uint32_t a, uint8_t *v) {
    *v = ((ctx_t *)p)->m[a];
    return true;
}
static bool rw(void *p, uint32_t a, uint16_t *v) {
    ctx_t *c = p;
    *v = (uint16_t)((c->m[a] << 8) | c->m[a + 1]);
    return true;
}
static bool rl(void *p, uint32_t a, uint32_t *v) {
    ctx_t *c = p;
    *v = ((uint32_t)c->m[a] << 24) | ((uint32_t)c->m[a + 1] << 16) |
         ((uint32_t)c->m[a + 2] << 8) | c->m[a + 3];
    return true;
}
static void log_write(ctx_t *c, uint32_t a) {
    assert(c->log_n < LOG_MAX);
    if (a == IRQ5_ACK_ADDR)
        c->irq_level = 0;
    c->raw[c->log_n] = mc68k_executed_cycles(c->cpu);
    c->log[c->log_n] = c->base + c->raw[c->log_n];
    c->log_addr[c->log_n] = a;
    c->log_n++;
}
static bool wb(void *p, uint32_t a, uint8_t v) {
    ctx_t *c = p;
    if (c->fail_en && a == c->fail_addr)
        return false;
    log_write(c, a);
    c->m[a] = v;
    return true;
}
static bool ww(void *p, uint32_t a, uint16_t v) {
    ctx_t *c = p;
    if (c->fail_en && a == c->fail_addr)
        return false;
    log_write(c, a);
    c->m[a] = (uint8_t)(v >> 8);
    c->m[a + 1] = (uint8_t)v;
    return true;
}
static bool wl(void *p, uint32_t a, uint32_t v) {
    ctx_t *c = p;
    if (c->fail_en && a == c->fail_addr)
        return false;
    log_write(c, a);
    c->m[a] = (uint8_t)(v >> 24);
    c->m[a + 1] = (uint8_t)(v >> 16);
    c->m[a + 2] = (uint8_t)(v >> 8);
    c->m[a + 3] = (uint8_t)v;
    return true;
}
static int irq_level_cb(void *p) {
    return ((ctx_t *)p)->irq_level;
}
static void access_timing(void *p, const mc68k_bus_access_t *a, mc68k_bus_timing_t *out) {
    ctx_t *c = p;
    (void)a;
    out->wait_cycles = (uint16_t)c->wait;
    out->cacheable = false;
}
static void pw(ctx_t *c, unsigned a, uint16_t v) {
    c->m[a] = (uint8_t)(v >> 8);
    c->m[a + 1] = (uint8_t)v;
}
static void pl(ctx_t *c, unsigned a, uint32_t v) {
    pw(c, a, (uint16_t)(v >> 16));
    pw(c, a + 2, (uint16_t)v);
}

#define OP_MOVE_W_D0_ABSW   0x31C0
#define OP_NOP              0x4E71
#define OP_RTE              0x4E73
#define OP_MOVE_IMM_SR      0x46FC
#define OP_STOP             0x4E72
#define SR_SUPERVISOR_MASK0 0x2000

static unsigned put_store(ctx_t *c, unsigned at, uint16_t addr) {
    pw(c, at, OP_MOVE_W_D0_ABSW);
    pw(c, at + 2, addr);
    return at + 4;
}
static unsigned put_stop(ctx_t *c, unsigned at) {
    pw(c, at, OP_STOP);
    pw(c, at + 2, SR_SUPERVISOR_MASK0);
    return at + 4;
}

static void build_program(ctx_t *c) {
    memset(c->m, 0, sizeof(c->m));
    pl(c, 0, 0x3000);
    pl(c, 4, PROG_PC);
    pl(c, ADDR_ERR_VECTOR_ADDR, ADDR_ERR_HANDLER);
    pl(c, IRQ5_VECTOR_ADDR, IRQ5_HANDLER);

    unsigned at = PROG_PC;
    /* 1命令目で割り込みマスクを下げる。保留中の IRQ5 は、この呼出しの途中
     * （次の命令境界）で受け付けられる。 */
    pw(c, at, OP_MOVE_IMM_SR);
    pw(c, at + 2, SR_SUPERVISOR_MASK0);
    at += 4;
    at = put_store(c, at, 0x0100);
    pw(c, at, OP_NOP);
    at += 2;
    at = put_store(c, at, 0x0102);
    at = put_store(c, at, 0x0101); /* 68000: address error / 68030: 成功 */
    /* STOP は2つ並べる。STOP 中に IRQ で起こされてハンドラが RTE した後、
     * 2つ目の STOP で止まる。 */
    at = put_stop(c, at);
    put_stop(c, at);

    at = ADDR_ERR_HANDLER;
    pw(c, at, OP_NOP);
    at += 2;
    at = put_store(c, at, 0x0104);
    at = put_stop(c, at);
    put_stop(c, at);

    at = IRQ5_HANDLER;
    pw(c, at, OP_NOP);
    at += 2;
    at = put_store(c, at, 0x0106);
    pw(c, at, OP_RTE);
}

typedef struct {
    int log[LOG_MAX];
    int raw[LOG_MAX];
    uint32_t log_addr[LOG_MAX];
    int log_n;
    int total;
    int step_cost[STEP_MAX];
    int step_n;
} result_t;

/* STOP で止まるまで実行する。step=true なら1命令ずつ、false なら大きなサイクル数で。 */
static void run_until_stopped(ctx_t *c, bool step, result_t *r) {
    int guard = 0;
    /* do-while: STOP 中に IRQ を上げた2回目の呼出しは、STOPPED のまま入る */
    do {
        if (guard++ >= STEP_MAX)
            abort();
        c->base = r->total;
        int got = mc68k_execute(c->cpu, step ? 1 : RUN_CYCLES);
        assert(got > 0);
        /* 実行後の値は戻り値と一致する */
        assert(mc68k_executed_cycles(c->cpu) == got);
        if (step) {
            assert(r->step_n < STEP_MAX);
            r->step_cost[r->step_n++] = got;
        }
        r->total += got;
    } while (c->cpu->exec_state != CPU_STOPPED);
}

static void run_scenario(mc68k_model_t model, int wait, bool step, result_t *r) {
    static ctx_t c;
    memset(&c, 0, sizeof(c));
    memset(r, 0, sizeof(*r));
    c.wait = wait;
    build_program(&c);
    mc68k_bus_t bus = {.read_byte = rb,
                       .read_word = rw,
                       .read_long = rl,
                       .write_byte = wb,
                       .write_word = ww,
                       .write_long = wl,
                       .ctx = &c};
    if (wait)
        bus.access_timing = access_timing;
    mc68k_state_t *cpu = mc68k_create_model(&bus, model);
    c.cpu = cpu;
    mc68k_reset(cpu);
    cpu->cacr = 0; /* キャッシュ無効: 全アクセスにウェイトが課金される（#176） */

    /* 呼出しの途中で IRQ を受け付けさせる（SR マスク 7 のまま IRQ5 を保留し、
     * プログラムの1命令目がマスクを下げる） */
    mc68k_set_irq(cpu, 5);
    run_until_stopped(&c, step, r);
    assert(cpu->exec_state == CPU_STOPPED);
    /* STOP 復帰: 割り込みを受け付けてハンドラを実行し、RTE して2つ目の STOP で止まる */
    mc68k_set_irq(cpu, 5);
    run_until_stopped(&c, step, r);
    assert(cpu->exec_state == CPU_STOPPED);

    memcpy(r->log, c.log, sizeof(r->log));
    memcpy(r->raw, c.raw, sizeof(r->raw));
    memcpy(r->log_addr, c.log_addr, sizeof(r->log_addr));
    r->log_n = c.log_n;
    mc68k_destroy(cpu);
}

static int count_addr(const result_t *r, uint32_t addr) {
    int n = 0;
    for (int i = 0; i < r->log_n; i++)
        n += r->log_addr[i] == addr;
    return n;
}

static int count_addr_ctx(const ctx_t *c, uint32_t addr) {
    int n = 0;
    for (int i = 0; i < c->log_n; i++)
        n += c->log_addr[i] == addr;
    return n;
}

static void check(mc68k_model_t model, int wait) {
    result_t big, stepped;
    run_scenario(model, wait, false, &big);
    run_scenario(model, wait, true, &stepped);

    /* 積み上がりの総量が両方式で一致する */
    assert(big.total == stepped.total);
    assert(big.log_n == stepped.log_n);
    for (int i = 0; i < big.log_n; i++) {
        assert(big.log_addr[i] == stepped.log_addr[i]);
        assert(big.log[i] == stepped.log[i]);
        /* オラクルの前提: ステップ実行では、命令の途中は常に呼出しの先頭 = 0 */
        assert(stepped.raw[i] == 0);
    }

    /* 想定した経路を実際に通ったこと。0x106 は IRQ ハンドラ内の書込みで、
     * 呼出しの途中の受付と STOP 復帰の2回。0x104 は 68000 の address error
     * ハンドラ内の書込み（68030 では奇数アドレスの書込みが成功するので通らない）。 */
    assert(count_addr(&big, 0x100) == 1);
    /* 0x102: 68030 では 0x101 への奇数 word 書込みが byte 2回に分かれ、0x102 にも当たる */
    assert(count_addr(&big, 0x102) == (model == MC68K_MODEL_68000 ? 1 : 2));
    assert(count_addr(&big, 0x106) == 2);
    assert(count_addr(&big, 0x104) == (model == MC68K_MODEL_68000 ? 1 : 0));

    /* 絶対値の独立確認（ステップ実行の戻り値だけから組む）。
     * 1回の大きな実行で、1命令目（MOVE to SR）の直後の境界で IRQ を受け付ける。
     * その IRQ フレームの最初の書込みは、1命令目の分を見る。
     * ステップ実行側は同じ書込みを別の呼出しの先頭で見て 0 になる。 */
    assert(big.log_addr[0] >= 0x2F00 && big.log_addr[0] < 0x3000);
    assert(big.raw[0] == stepped.step_cost[0] && big.raw[0] > 0);
    assert(big.log[0] == stepped.step_cost[0]);
}

/* : 68020 で M=1 からの IRQ 受付が $1 フレーム（ISP 上）の書込み失敗で
 * グループ0例外を保留にすると、次の mc68k_execute() の冒頭でそれをディスパッチする。
 * そのディスパッチ分のサイクルが戻り値と mc68k_executed_cycles() に含まれること。 */
static mc68k_state_t *make_pending_group0(ctx_t *c, mc68k_bus_t *bus) {
    memset(c, 0, sizeof(*c));
    pl(c, 0, SSP_INIT);
    pl(c, 4, PROG_PC);
    pl(c, BUS_ERR_VECTOR_ADDR, BUS_ERR_HANDLER);
    pw(c, PROG_PC, OP_MOVE_IMM_SR);
    pw(c, PROG_PC + 2, SR_SUPERVISOR_MASTER);
    pw(c, PROG_PC + 4, OP_NOP);
    pw(c, BUS_ERR_HANDLER, OP_NOP);
    pw(c, BUS_ERR_HANDLER + 2, OP_NOP);
    *bus = (mc68k_bus_t){.read_byte = rb,
                         .read_word = rw,
                         .read_long = rl,
                         .write_byte = wb,
                         .write_word = ww,
                         .write_long = wl,
                         .ctx = c};
    mc68k_state_t *cpu = mc68k_create_model(bus, MC68K_MODEL_68020);
    c->cpu = cpu;
    mc68k_reset(cpu);
    cpu->msp = MSP_INIT;

    (void)mc68k_execute(cpu, 1); /* MOVE #imm,SR: M=1 */
    /* ISP 上の throwaway フレームの先頭への書込みを失敗させる */
    c->fail_en = true;
    c->fail_addr = SSP_INIT - ISP_THROWAWAY_SIZE;
    mc68k_set_irq(cpu, IRQ_LEVEL_1);
    (void)mc68k_execute(cpu, 1);
    assert(cpu->exec_state == CPU_EXCEPTION); /* 保留中（通っていない経路にしない） */
    mc68k_set_irq(cpu, 0);
    c->fail_en = false;
    c->base = 0;
    c->log_n = 0;
    return cpu;
}

static void test_pending_group0_dispatch_counted(void) {
    static ctx_t c;
    mc68k_bus_t bus;

    /* NOP 1 命令の単独コスト（ハンドラ側の NOP と同じ命令）。 */
    mc68k_state_t *ref = make_pending_group0(&c, &bus);
    mc68k_reset(ref);
    ref->pc = BUS_ERR_HANDLER;
    int nop_cost = mc68k_execute(ref, 1);
    assert(nop_cost > 0 && ref->pc == BUS_ERR_HANDLER + 2);
    mc68k_destroy(ref);

    /* 予算 1 はディスパッチだけで使い切る。ハンドラの NOP はまだ実行されない */
    mc68k_state_t *cpu = make_pending_group0(&c, &bus);
    int disp = mc68k_execute(cpu, 1);
    assert(cpu->exec_state == CPU_NORMAL && cpu->pc == BUS_ERR_HANDLER);
    assert(disp == PENDING_G0_DISPATCH_CYCLES && disp > nop_cost);
    assert(mc68k_executed_cycles(cpu) == disp);
    assert(c.log_n > 0); /* フレーム書込みは命令境界より前なので 0 のまま */
    for (int i = 0; i < c.log_n; i++)
        assert(c.raw[i] == 0);
    /* 続く呼出しは通常どおり 1 命令ぶんだけ */
    assert(mc68k_execute(cpu, 1) == nop_cost);
    mc68k_destroy(cpu);

    /* 予算が余る場合: ディスパッチ分の後にハンドラの NOP が続けて実行され、
     * 戻り値はその合計。1 命令目の開始値（命令境界）にもディスパッチ分が入る。 */
    cpu = make_pending_group0(&c, &bus);
    int total = mc68k_execute(cpu, PENDING_G0_DISPATCH_CYCLES + 1);
    assert(cpu->pc == BUS_ERR_HANDLER + 2);
    assert(total == PENDING_G0_DISPATCH_CYCLES + nop_cost);
    assert(mc68k_executed_cycles(cpu) == total);
    mc68k_destroy(cpu);
}

/* ---- : 未カバー経路 ---- */

typedef void (*build_fn_t)(ctx_t *);

static mc68k_state_t *make_cpu(ctx_t *c, mc68k_bus_t *bus, mc68k_model_t model) {
    *bus = (mc68k_bus_t){.read_byte = rb,
                         .read_word = rw,
                         .read_long = rl,
                         .write_byte = wb,
                         .write_word = ww,
                         .write_long = wl,
                         .ctx = c};
    mc68k_state_t *cpu = mc68k_create_model(bus, model);
    c->cpu = cpu;
    mc68k_reset(cpu);
    return cpu;
}

/* build で RAM を組み、STOP で止まるまで実行する。pull=true は irq_level_fn で
 * IRQ5 を渡し（build_program() の2フェーズ構成）、false は IRQ なし（1フェーズ）。 */
static void run_prog(mc68k_model_t model, build_fn_t build, bool pull, bool step, result_t *r) {
    static ctx_t c;
    static mc68k_bus_t bus;
    memset(&c, 0, sizeof(c));
    memset(r, 0, sizeof(*r));
    build(&c);
    mc68k_state_t *cpu = make_cpu(&c, &bus, model);
    if (pull) {
        mc68k_set_irq_level_hook(cpu, irq_level_cb, &c);
        c.irq_level = IRQ_LEVEL_5; /* SR マスク 7 のまま保留 → 1命令目が下げる */
    }
    run_until_stopped(&c, step, r);
    assert(cpu->exec_state == CPU_STOPPED);
    if (pull) {
        c.irq_level = IRQ_LEVEL_5; /* STOP 復帰 */
        run_until_stopped(&c, step, r);
        assert(cpu->exec_state == CPU_STOPPED);
    }
    memcpy(r->log, c.log, sizeof(r->log));
    memcpy(r->raw, c.raw, sizeof(r->raw));
    memcpy(r->log_addr, c.log_addr, sizeof(r->log_addr));
    r->log_n = c.log_n;
    mc68k_destroy(cpu);
}

/* 1回の大きな実行とステップ実行で、書込みごとの累計が一致する（check() と同じ判定）。 */
static void expect_same(const result_t *big, const result_t *stepped) {
    assert(big->total == stepped->total);
    assert(big->log_n == stepped->log_n && big->log_n > 0);
    for (int i = 0; i < big->log_n; i++) {
        assert(big->log_addr[i] == stepped->log_addr[i]);
        assert(big->log[i] == stepped->log[i]);
        assert(stepped->raw[i] == 0);
    }
}

static void put_vectors(ctx_t *c) {
    memset(c->m, 0, sizeof(c->m));
    pl(c, 0, SSP_INIT);
    pl(c, 4, PROG_PC);
}

/* 68030: 奇数 word 書込みは成功するので、書込みコールバックの false（バスエラー、
 * ベクタ2）で group0 を起こす。 */
static void build_g0_bus_error(ctx_t *c) {
    put_vectors(c);
    pl(c, BUS_ERR_VECTOR_ADDR, BUS_ERR_HANDLER);
    c->fail_en = true;
    c->fail_addr = FAIL_STORE_ADDR;
    unsigned at = PROG_PC;
    pw(c, at, OP_NOP);
    at += 2;
    at = put_store(c, at, FAIL_STORE_ADDR); /* 失敗 → ループ内で group0 ディスパッチ */
    put_stop(c, at);

    at = BUS_ERR_HANDLER;
    pw(c, at, OP_NOP);
    at += 2;
    at = put_store(c, at, G0_HANDLER_STORE);
    at = put_stop(c, at);
    put_stop(c, at);
}

static void test_group0_bus_error(mc68k_model_t model) {
    result_t big, stepped;
    run_prog(model, build_g0_bus_error, false, false, &big);
    run_prog(model, build_g0_bus_error, false, true, &stepped);
    expect_same(&big, &stepped);
    /* ハンドラの書込みが1回 = group0 ディスパッチを通った */
    assert(count_addr(&big, G0_HANDLER_STORE) == 1);
    assert(count_addr(&big, FAIL_STORE_ADDR) == 0);
    /* フレーム書込み（SSP 直下）は、ハンドラの書込みより前にある */
    assert(big.log_addr[0] >= STACK_FRAME_LOW && big.log_addr[0] < SSP_INIT);
    /* 絶対値: フレーム書込みは失敗した命令の開始値 = 1命令目（NOP）の分を見る */
    assert(stepped.step_cost[0] > 0);
    assert(big.raw[0] == stepped.step_cost[0]);
}

/* トレース例外: T=1 にした次の命令（NOP）の後でベクタ9へ入る。 */
static void build_trace(ctx_t *c) {
    put_vectors(c);
    pl(c, TRACE_VECTOR_ADDR, TRACE_HANDLER);
    unsigned at = PROG_PC;
    pw(c, at, OP_MOVE_IMM_SR);
    pw(c, at + 2, SR_TRACE_SUPERVISOR);
    at += 4;
    pw(c, at, OP_NOP); /* T=1 で実行 → 完了後にトレース例外 */
    at += 2;
    put_stop(c, at);

    at = TRACE_HANDLER;
    at = put_store(c, at, TRACE_HANDLER_STORE);
    at = put_stop(c, at);
    put_stop(c, at);
}

static void test_trace_exception(mc68k_model_t model) {
    result_t big, stepped;
    run_prog(model, build_trace, false, false, &big);
    run_prog(model, build_trace, false, true, &stepped);
    expect_same(&big, &stepped);
    assert(count_addr(&big, TRACE_HANDLER_STORE) == 1); /* ベクタ9を通った */
    assert(big.log_addr[0] >= STACK_FRAME_LOW && big.log_addr[0] < SSP_INIT);
    /* 絶対値: トレースのフレーム書込みは、トレースされた命令（NOP）の開始値
     * = 1命令目（MOVE to SR）の分を見る */
    assert(stepped.step_cost[0] > 0);
    assert(big.raw[0] == stepped.step_cost[0]);
    /* 積み上がり: ハンドラの書込みは MOVE と、NOP + トレース例外ディスパッチ
     * （ステップ実行の2呼出し目）の全部を見る */
    int last = big.log_n - 1;
    assert(big.log_addr[last] == TRACE_HANDLER_STORE);
    assert(stepped.step_cost[1] > 0);
    assert(big.raw[last] == stepped.step_cost[0] + stepped.step_cost[1]);
}

static void build_pull_irq(ctx_t *c) {
    build_program(c);
}

static void test_pull_irq(mc68k_model_t model) {
    result_t big, stepped;
    run_prog(model, build_pull_irq, true, false, &big);
    run_prog(model, build_pull_irq, true, true, &stepped);
    expect_same(&big, &stepped);
    /* IRQ ハンドラ内の書込みが、途中受付と STOP 復帰で2回 */
    assert(count_addr(&big, IRQ5_ACK_ADDR) == 2);
    /* 途中受付: IRQ フレームの最初の書込みは1命令目の分を見る */
    assert(big.log_addr[0] >= STACK_FRAME_LOW && big.log_addr[0] < SSP_INIT);
    assert(stepped.step_cost[0] > 0);
    assert(big.raw[0] == stepped.step_cost[0]);
}

/* 契約3: mc68k_debug_run() は内部で mc68k_execute(cpu,1) を繰り返し、各呼出しで
 * 0 から数え直す。累計（戻り値）ではなく、最後の呼出し分が残る。 */
static void test_debug_run_restarts_each_step(mc68k_model_t model) {
    static ctx_t c;
    static mc68k_bus_t bus;

    /* 期待値: mc68k_execute(cpu,1) を STOP まで回した合計と最後の1命令分 */
    memset(&c, 0, sizeof(c));
    build_program(&c);
    mc68k_state_t *ref = make_cpu(&c, &bus, model);
    mc68k_set_irq(ref, IRQ_LEVEL_5);
    int ref_total = 0;
    int ref_last = 0;
    int guard = 0;
    do {
        if (guard++ >= STEP_MAX)
            abort();
        ref_last = mc68k_execute(ref, 1);
        ref_total += ref_last;
    } while (ref->exec_state != CPU_STOPPED);
    mc68k_destroy(ref);
    assert(ref_last > 0 && ref_total > ref_last);

    memset(&c, 0, sizeof(c));
    build_program(&c);
    mc68k_state_t *cpu = make_cpu(&c, &bus, model);
    mc68k_set_irq(cpu, IRQ_LEVEL_5);
    int total = mc68k_debug_run(cpu, RUN_CYCLES);
    assert(cpu->exec_state == CPU_STOPPED);
    assert(total == ref_total);
    /* 書込みコールバックは全件が呼出しの先頭 = 0 を見る。IRQ の途中受付も別呼出し */
    assert(c.log_n > 0);
    for (int i = 0; i < c.log_n; i++)
        assert(c.raw[i] == 0);
    assert(count_addr_ctx(&c, IRQ5_ACK_ADDR) == 1); /* IRQ 経路を通った */
    /* 戻った後の値は、全体の累計でなく最後の mc68k_execute(cpu,1) 分 */
    assert(mc68k_executed_cycles(cpu) == ref_last);
    assert(mc68k_executed_cycles(cpu) < total);
    mc68k_destroy(cpu);
}

int main(void) {
    check(MC68K_MODEL_68000, 0);
    check(MC68K_MODEL_68030, 0);
    check(MC68K_MODEL_68030, WAIT_PER_ACCESS); /* bus_wait_accum が非0（#176） */

    /* ウェイトが加算に効いている: 同モデルでウェイトありの方が累計が大きい */
    result_t nowait, waited;
    run_scenario(MC68K_MODEL_68030, 0, false, &nowait);
    run_scenario(MC68K_MODEL_68030, WAIT_PER_ACCESS, false, &waited);
    assert(waited.total > nowait.total);
    assert(waited.log[0] > nowait.log[0]); /* 1命令目の待ちが IRQ 受付時の値に入る */

    /* 次の mc68k_execute() 呼出しで 0 から数え直す */
    {
        static ctx_t c;
        memset(&c, 0, sizeof(c));
        build_program(&c);
        mc68k_bus_t bus = {.read_byte = rb,
                           .read_word = rw,
                           .read_long = rl,
                           .write_byte = wb,
                           .write_word = ww,
                           .write_long = wl,
                           .ctx = &c};
        mc68k_state_t *cpu = mc68k_create_model(&bus, MC68K_MODEL_68000);
        c.cpu = cpu;
        mc68k_reset(cpu);
        int first = mc68k_execute(cpu, 1);
        assert(mc68k_executed_cycles(cpu) == first && first > 0);
        c.base = 0;
        c.log_n = 0;
        pw(&c, PROG_PC + 4, OP_MOVE_W_D0_ABSW); /* 次の命令も書込み */
        pw(&c, PROG_PC + 6, 0x0108);
        (void)mc68k_execute(cpu, 1);
        assert(c.log_n == 1 && c.log[0] == 0);

        /* 呼出しの先頭で保留中の例外をディスパッチするとき、その中のバス書込み
         * （フレーム構築）は命令境界より前なので、前回呼出しの値が残らず 0 を返す。
         * ea_invalid_pending 経路（cpu_dispatch_exception()）を直接立てて確かめる。 */
        int prev = mc68k_execute(cpu, 1);
        assert(prev > 0 && mc68k_executed_cycles(cpu) == prev);
        cpu->exec_state = CPU_EXCEPTION;
        cpu->ea_invalid_pending = true;
        c.base = 0;
        c.log_n = 0;
        int disp = mc68k_execute(cpu, 1);
        assert(c.log_n > 0);
        for (int i = 0; i < c.log_n; i++)
            assert(c.raw[i] == 0); /* フレーム書込みの全件 */
        assert(disp > 0 && mc68k_executed_cycles(cpu) == disp);

        /* reset 後は 0 */
        mc68k_reset(cpu);
        assert(mc68k_executed_cycles(cpu) == 0);
        mc68k_destroy(cpu);
    }
    test_pending_group0_dispatch_counted();

    /*  */
    test_group0_bus_error(MC68K_MODEL_68000);
    test_group0_bus_error(MC68K_MODEL_68030);
    test_trace_exception(MC68K_MODEL_68000);
    test_trace_exception(MC68K_MODEL_68030);
    test_pull_irq(MC68K_MODEL_68000);
    test_pull_irq(MC68K_MODEL_68030);
    test_debug_run_restarts_each_step(MC68K_MODEL_68000);
    test_debug_run_restarts_each_step(MC68K_MODEL_68030);
    return 0;
}
