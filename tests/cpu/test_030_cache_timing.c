/*
 *  (現行仕様) のバスタイミングフック
 * の単体テスト。本テストが対象とするMOVE等の命令ハンドラは、
 * (現行仕様) のPhase 0時点でもモデル共通のMC68000
 * 公称サイクルのままである（Phase 0でCYC()参照に書き換わるのはDBccのみ）。
 * 命令ハンドラの公称サイクル数そのものは検証せず、かわりに「アクセス
 * タイミングフックを NULL/0 にしたときの消費サイクル（nominal）」を基準に、
 * フックが返す wait_cycles ぶんの差分だけを検証する。これによりMOVE命令の
 * 正確な公称サイクル数を本テストに埋め込む必要がなくなる。
 */
#include "mc68000/cpu.h"
#include "cpu/cpu_internal.h"
#include <assert.h>
#include <string.h>

typedef struct {
    unsigned char m[0x4000];
    uint32_t fail_addr; /* このアドレスへのアクセスはバスエラーになる。UINT32_MAX=無効 */
    uint16_t timing_wait;
    bool timing_cacheable;
    int timing_calls;
} ctx_t;

static bool rb(void *p, uint32_t a, uint8_t *v) {
    *v = ((ctx_t *)p)->m[a];
    return true;
}
static bool rw(void *p, uint32_t a, uint16_t *v) {
    ctx_t *c = p;
    if (a == c->fail_addr)
        return false;
    *v = (uint16_t)((c->m[a] << 8) | c->m[a + 1]);
    return true;
}
static bool rl(void *p, uint32_t a, uint32_t *v) {
    ctx_t *c = p;
    if (a == c->fail_addr)
        return false;
    *v = ((uint32_t)c->m[a] << 24) | ((uint32_t)c->m[a + 1] << 16) |
         ((uint32_t)c->m[a + 2] << 8) | c->m[a + 3];
    return true;
}
static bool wb(void *p, uint32_t a, uint8_t v) {
    ((ctx_t *)p)->m[a] = v;
    return true;
}
static bool ww(void *p, uint32_t a, uint16_t v) {
    ctx_t *c = p;
    if (a == c->fail_addr)
        return false;
    c->m[a] = (uint8_t)(v >> 8);
    c->m[a + 1] = (uint8_t)v;
    return true;
}
static bool wl(void *p, uint32_t a, uint32_t v) {
    ctx_t *c = p;
    if (a == c->fail_addr)
        return false;
    c->m[a] = (uint8_t)(v >> 24);
    c->m[a + 1] = (uint8_t)(v >> 16);
    c->m[a + 2] = (uint8_t)(v >> 8);
    c->m[a + 3] = (uint8_t)v;
    return true;
}
static void access_timing(void *p, const mc68k_bus_access_t *a, mc68k_bus_timing_t *out) {
    ctx_t *c = p;
    (void)a;
    c->timing_calls++;
    out->wait_cycles = c->timing_wait;
    out->cacheable = c->timing_cacheable;
}
static void pw(ctx_t *c, unsigned a, uint16_t v) {
    c->m[a] = (uint8_t)(v >> 8);
    c->m[a + 1] = (uint8_t)v;
}
static void pl(ctx_t *c, unsigned a, uint32_t v) {
    c->m[a] = (uint8_t)(v >> 24);
    c->m[a + 1] = (uint8_t)(v >> 16);
    c->m[a + 2] = (uint8_t)(v >> 8);
    c->m[a + 3] = (uint8_t)v;
}

static mc68k_bus_t make_bus(ctx_t *c) {
    return (mc68k_bus_t){.read_byte = rb,
                         .read_word = rw,
                         .read_long = rl,
                         .write_byte = wb,
                         .write_word = ww,
                         .write_long = wl,
                         .ctx = c};
}

/* MOVE.W (0100).W,D0 / MOVE.W D0,(0100).W を $300 に置いて1命令実行する。
 * オペコード fetch + 拡張ワード fetch はどちらも instruction=true、
 * オペランド自体の read/write は instruction=false になる
 * （ea_helpers.c: abs.w の拡張は cpu_fetch_word() 経由）。 */
static int consumed_with(mc68k_state_t *cpu, ctx_t *c, uint32_t cacr, uint16_t wait,
                         bool cacheable, bool store, uint16_t operand_addr) {
    cpu->cacr = cacr;
    cpu->pc = 0x300;
    cpu->exec_state = CPU_NORMAL;
    c->timing_wait = wait;
    c->timing_cacheable = cacheable;
    c->timing_calls = 0;
    pw(c, 0x300, store ? 0x31C0u : 0x3038u); /* MOVE.W D0,(xxx).W / MOVE.W (xxx).W,D0 */
    pw(c, 0x302, operand_addr);
    return mc68k_execute(cpu, 1);
}

/* EC030 の AC0/AC1 は 68030 の TT0/TT1 と同じビット配置（ac_matches()
 * 参照）。LAM=0xFF で上位バイトを無視して全アドレスにマッチさせ、
 * fcmask=0/fcbase=5 で「スーパーバイザデータ空間（fc=5）だけ」に絞る
 * ことで、命令フェッチ（fc=6）を CI の対象から外す。 */
#define AC_MATCH_SUPERVISOR_DATA_CI UINT32_C(0x00FF8550)

int main(void) {
    /* ---- 1. 後方互換: access_timing=NULL と wait=0 は同じ結果になる ---- */
    ctx_t c = {0};
    c.fail_addr = UINT32_MAX;
    pl(&c, 0, 0x3000); /* SSP */
    pl(&c, 4, 0x300);  /* PC */
    mc68k_bus_t bus = make_bus(&c);
    mc68k_state_t *cpu = mc68k_create_model(&bus, MC68K_MODEL_68EC030);
    mc68k_reset(cpu);
    cpu->cacr = 0;
    pw(&c, 0x300, 0x3038);
    pw(&c, 0x302, 0x0100);
    int consumed_null = mc68k_execute(cpu, 1);
    uint32_t d0_null = cpu->d[0];

    mc68k_reset(cpu);
    cpu->bus.access_timing = access_timing;
    int nominal = consumed_with(cpu, &c, 0, 0, true, false, 0x0100);
    assert(consumed_null == nominal);
    assert(cpu->d[0] == d0_null);
    assert(c.timing_calls == 3); /* opcode + 拡張ワード + データread */

    /* ---- 2. CACR=0（EC030）: 全アクセスに課金される ---- */
    int v = consumed_with(cpu, &c, 0, 3, true, false, 0x0100);
    assert(v - nominal == 9 && c.timing_calls == 3);

    /* ---- 3. EI のみ: 命令フェッチ2回が免除される ---- */
    v = consumed_with(cpu, &c, MC68K_CACR_EI, 3, true, false, 0x0100);
    assert(v - nominal == 3);

    /* ---- 4. ED のみ: データreadが免除される ---- */
    v = consumed_with(cpu, &c, MC68K_CACR_ED, 3, true, false, 0x0100);
    assert(v - nominal == 6);

    /* ---- 5. EI|ED: 全readが免除される ---- */
    v = consumed_with(cpu, &c, MC68K_CACR_EI | MC68K_CACR_ED, 3, true, false, 0x0100);
    assert(v - nominal == 0);

    /* ---- 6. write-through: D writeはCACRの状態に関わらず常に課金される ---- */
    int nominal_store = consumed_with(cpu, &c, 0, 0, true, true, 0x0100);
    v = consumed_with(cpu, &c, MC68K_CACR_EI | MC68K_CACR_ED, 3, true, true, 0x0100);
    assert(v - nominal_store == 3);

    /* ---- 7. cacheable=false: CACR=EI|EDでも全アクセスが課金される ---- */
    v = consumed_with(cpu, &c, MC68K_CACR_EI | MC68K_CACR_ED, 3, false, false, 0x0100);
    assert(v - nominal == 9);

    /* ---- 8. EC030 の AC0 CI: マッチすればED免除が上書きされる ---- */
    cpu->mmu_tt0 = 0;
    int no_ci = consumed_with(cpu, &c, MC68K_CACR_EI | MC68K_CACR_ED, 3, true, false, 0x0100);
    assert(no_ci - nominal == 0);
    cpu->mmu_tt0 = AC_MATCH_SUPERVISOR_DATA_CI;
    int with_ci = consumed_with(cpu, &c, MC68K_CACR_EI | MC68K_CACR_ED, 3, true, false, 0x0100);
    assert(with_ci - no_ci == 3); /* データreadだけが課金される */
    cpu->mmu_tt0 = 0;
    mc68k_destroy(cpu);

    /* ---- 9. 68030 の TT0 CI: 同じ AC 値で同じ挙動になる ---- */
    mc68k_state_t *cpu30 = mc68k_create_model(&bus, MC68K_MODEL_68030);
    mc68k_reset(cpu30);
    cpu30->bus.access_timing = access_timing;
    int nominal30 = consumed_with(cpu30, &c, 0, 0, true, false, 0x0100);
    cpu30->mmu_tt0 = 0;
    int no_ci30 =
        consumed_with(cpu30, &c, MC68K_CACR_EI | MC68K_CACR_ED, 3, true, false, 0x0100);
    assert(no_ci30 - nominal30 == 0);
    cpu30->mmu_tt0 = AC_MATCH_SUPERVISOR_DATA_CI;
    int with_ci30 =
        consumed_with(cpu30, &c, MC68K_CACR_EI | MC68K_CACR_ED, 3, true, false, 0x0100);
    assert(with_ci30 - no_ci30 == 3);
    cpu30->mmu_tt0 = 0;

    /* ---- 10. 68030 の PMMU テーブルウォーク（PTE の CI）:
     * cpu_translate_execute_access() が MC68K_TRANSLATE_WALK 経由の
     * cache_inhibit も TT と同じ経路でそのまま cpu_account_bus_cycle() へ
     * 伝える。test_68030_translate.c と同じ既知の table 構成
     * （0x00403010 -> 物理 0x0000c010、CI=1）を再利用して確認する。
     * phys_read_long()/phys_write_long() 自体は file-static のため、ここは
     * cpu_translate_execute_access() の返す cache_inhibit の伝播だけを見る
     * （テーブルウォーク自身の正しさは test_68030_translate.c が担保済み）。 */
    cpu30->mmu_tc = 0x82c0aa00u;
    cpu30->mmu_crp = cpu30->mmu_srp = UINT64_C(0x0000000200000400);
    pl(&c, 0x404, 0x00000802u);
    pl(&c, 0x80c, 0x0000c041u);
    {
        uint32_t physical = 0;
        bool cache_inhibit = false;
        const mc68k_access_t read_super = cpu_supervisor_data_access(true, 4, false);
        bool ok = cpu_translate_execute_access(cpu30, 0x00403010u, &read_super, &physical,
                                               &cache_inhibit);
        assert(ok && physical == 0x0000c010u && cache_inhibit);

        c.timing_wait = 5;
        c.timing_cacheable = true; /* ホストは cacheable=true と答えるが CI が優先される */
        int before = cpu30->bus_wait_accum;
        cpu_account_bus_cycle(cpu30, physical, &read_super, cache_inhibit);
        assert(cpu30->bus_wait_accum - before == 5); /* ED が立っていても CI で課金される */
    }
    cpu30->mmu_tc = 0;
    cpu30->mmu_crp = cpu30->mmu_srp = 0;
    mc68k_destroy(cpu30);

    /* ---- 11. バスエラー: 失敗したアクセスでもフックは呼ばれる ---- */
    mc68k_state_t *cpu_be = mc68k_create_model(&bus, MC68K_MODEL_68EC030);
    mc68k_reset(cpu_be);
    cpu_be->bus.access_timing = access_timing;
    cpu_be->cacr = 0;
    c.fail_addr = 0x0100u;
    c.timing_wait = 2;
    c.timing_cacheable = false;
    c.timing_calls = 0;
    pw(&c, 0x300, 0x3038u);
    pw(&c, 0x302, 0x0100u);
    cpu_be->pc = 0x300;
    mc68k_execute(cpu_be, 1);
    assert(c.timing_calls >= 3); /* opcode + 拡張ワード + 失敗したデータread */
    c.fail_addr = UINT32_MAX;
    mc68k_destroy(cpu_be);

    /* ---- 12. アドレスエラー: バスへ発行される前に戻るためフックは呼ばれない ---- */
    ctx_t c00 = {0};
    c00.fail_addr = UINT32_MAX;
    pl(&c00, 0, 0x3000);
    pl(&c00, 4, 0x300);
    mc68k_bus_t bus00 = make_bus(&c00);
    mc68k_state_t *cpu00 = mc68k_create_model(&bus00, MC68K_MODEL_68000);
    mc68k_reset(cpu00);
    cpu00->bus.access_timing = access_timing;
    c00.timing_wait = 9;
    c00.timing_calls = 0;
    {
        const mc68k_access_t rd = cpu_data_access(cpu00, true, 2, false);
        cpu_read_word_access(cpu00, 0x101u, &rd); /* 68000: 奇数アドレスは即アドレスエラー */
        assert(cpu00->exec_state == CPU_EXCEPTION);
        assert(c00.timing_calls == 0);
    }
    mc68k_destroy(cpu00);

    /* ---- 13. 非030モデル: CACRを立てても抑止されない ---- */
    ctx_t c20 = {0};
    c20.fail_addr = UINT32_MAX;
    pl(&c20, 0, 0x3000);
    pl(&c20, 4, 0x300);
    mc68k_bus_t bus20 = make_bus(&c20);
    mc68k_state_t *cpu20 = mc68k_create_model(&bus20, MC68K_MODEL_68020);
    mc68k_reset(cpu20);
    cpu20->bus.access_timing = access_timing;
    cpu20->cacr = 0xFFFFFFFFu; /* model_has_030_cache(68020)==false なので無関係のはず */
    c20.timing_wait = 5;
    c20.timing_cacheable = true;
    {
        const mc68k_access_t instr_acc = cpu_program_access(cpu20, 2);
        const mc68k_access_t data_acc = cpu_data_access(cpu20, true, 2, false);
        int before = cpu20->bus_wait_accum;
        cpu_account_bus_cycle(cpu20, 0x100, &instr_acc, false);
        cpu_account_bus_cycle(cpu20, 0x100, &data_acc, false);
        assert(cpu20->bus_wait_accum - before == 10);
    }

    /* ---- 14. 非整列アクセス（68020+）: byte+word+byte の3バスサイクルへ分解 ---- */
    cpu20->bus_wait_accum = 0;
    c20.timing_wait = 2;
    c20.timing_cacheable = false;
    c20.timing_calls = 0;
    {
        const mc68k_access_t rd = cpu_data_access(cpu20, true, 4, false);
        cpu_read_long_access(cpu20, 0x101u, &rd); /* addr&1 -> byte/word/byte */
        assert(cpu20->exec_state == CPU_NORMAL);
        assert(c20.timing_calls == 3);
        assert(cpu20->bus_wait_accum == 6);
    }
    mc68k_destroy(cpu20);

    /* ---- 15. アキュムレータの寿命: 単発 mc68k_execute(cpu,1) を連続で呼んでも
     * 前回ぶんが次回へ漏れたり(二重計上)、取りこぼされたりしない。
     * mc68k_execute() は「consumed < cycles の間、命令を最後まで実行する」
     * ため budget=1 でも常に1命令ぶん完走する（instruction-count accuracy、
     * オーバーシュートは元々の仕様）。したがって「2回の単発呼び出しの合計」と
     * 「1回の大きな budget 呼び出し」は一般には一致しない
     * （ちょうど2命令目の末尾で止まる保証がないため）。ここでは代わりに、
     * 単発呼び出しを連続しても毎回同じ nominal+wait×3 になる
     * （＝前回ぶんが漏れて二重計上/欠落しない）ことを直接確認する。 ---- */
    mc68k_state_t *cpu_life = mc68k_create_model(&bus, MC68K_MODEL_68EC030);
    mc68k_reset(cpu_life);
    cpu_life->bus.access_timing = access_timing;
    cpu_life->cacr = 0;
    c.fail_addr = UINT32_MAX;
    c.timing_wait = 4;
    c.timing_cacheable = false;
    pw(&c, 0x300, 0x3038u); /* MOVE.W (0100).W,D0 */
    pw(&c, 0x302, 0x0100u);
    pw(&c, 0x304, 0x3038u); /* 2つ目の同じ命令 */
    pw(&c, 0x306, 0x0100u);
    cpu_life->pc = 0x300;
    int nominal_life = consumed_with(cpu_life, &c, 0, 0, true, false, 0x0100);
    cpu_life->pc = 0x300;
    c.timing_wait = 4;
    c.timing_cacheable = false;
    int step1 = mc68k_execute(cpu_life, 1);
    int step2 = mc68k_execute(cpu_life, 1);
    assert(step1 == nominal_life + 12); /* 3アクセス×wait4 */
    assert(step2 == nominal_life + 12); /* 前回ぶんの漏れ込みがあれば+24等になる */
    assert(cpu_life->bus_wait_accum == 0);
    mc68k_destroy(cpu_life);

    /* ---- 16. MOVEC マスク回帰: CACR書込みマスクは既存値0x3313と一致する
     * （test_68ec030.c が同じ数値0x3313で既に検証済みであることの確認）。 ---- */
    assert(MC68K_CACR_030_MASK == 0x3313u);
    assert(MC68K_CACR_020_MASK == 0x03u);

    return 0;
}
