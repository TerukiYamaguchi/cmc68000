/*
 * 040/060 の D-cache 読出しヒットの inline 経路（cpu_dcache_read_hit, #134）の試験。
 *
 * 乱数で選んだ読出し（1/2/4 バイト、奇数アドレスを含む）・書込み・RMW・キャッシュの攪乱
 * （ラインの無効化・未ロード化・同じ物理ラインの重複）を、「inline 経路あり」と「なし
 * （dcache_hit_fast_disable）」で同じ順に実行し、各操作の後の返値・待ち時間の積算・バス読出しの回数・
 * D-cache
 * の全ライン（valid・loaded・dirty・物理ライン・データ）と置換位置・メモリの内容が一致する
 * ことを確認する。cache mode は TTR で
 * 0（write-through）、1（copyback）、2・3（noncachable）の順に 切り替え、DE
 * の切替、無効化を挟む。
 */
#include "mc68000/cpu.h"
#include "cpu/cpu_internal.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RAM_SIZE     0x10000u
#define SSP_INIT     0x8000u
#define RESET_PC     0x100u
#define TTR_ALL      0x0000c000u /* base $00・mask $00・E=1 */
#define TTR_CM_SHIFT 5u
#define WAIT_CYCLES  2u
#define OPS_PER_MODE 3000
#define RANGE 0x4000u /* 触るアドレスの範囲（128 セットの一部。同じセットの衝突を起こす） */
#define RANGE_BASE 0x1000u
#define HOT_RANGE \
    0x200u /* 攪乱したラインがすぐ読まれるよう、半分のアクセスをここへ集中させる */
#define NUM_MODES 4u

typedef struct {
    uint8_t ram[RAM_SIZE];
    uint64_t reads;
} fixture_t;

static fixture_t *f_g;
static mc68k_state_t *cpu_g;

static bool rb(void *c, uint32_t a, uint8_t *v) {
    if (a >= RAM_SIZE)
        return false;
    ((fixture_t *)c)->reads++;
    *v = ((fixture_t *)c)->ram[a];
    return true;
}
static bool rw(void *c, uint32_t a, uint16_t *v) {
    uint8_t hi, lo;
    if (!rb(c, a, &hi) || !rb(c, a + 1, &lo))
        return false;
    *v = (uint16_t)(hi << 8 | lo);
    return true;
}
static bool rl(void *c, uint32_t a, uint32_t *v) {
    uint16_t hi, lo;
    if (!rw(c, a, &hi) || !rw(c, a + 2, &lo))
        return false;
    *v = (uint32_t)hi << 16 | lo;
    return true;
}
static bool wb(void *c, uint32_t a, uint8_t v) {
    if (a >= RAM_SIZE)
        return false;
    ((fixture_t *)c)->ram[a] = v;
    return true;
}
static bool ww(void *c, uint32_t a, uint16_t v) {
    return wb(c, a, (uint8_t)(v >> 8)) && wb(c, a + 1, (uint8_t)v);
}
static bool wl(void *c, uint32_t a, uint32_t v) {
    return ww(c, a, (uint16_t)(v >> 16)) && ww(c, a + 2, (uint16_t)v);
}
static void reset_peripheral(void *c) {
    (void)c;
}
/* 全アクセスに待ちを課す。一部のアドレスはホストから見て cacheable
 * にして、待ちの計上の分岐を通す。 */
static void timing_cb(void *c, const mc68k_bus_access_t *a, mc68k_bus_timing_t *t) {
    (void)c;
    t->wait_cycles = WAIT_CYCLES;
    t->cacheable = (a->physical & 0x1000u) != 0;
}

static uint32_t rng;
static uint32_t next_rand(void) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static uint64_t mix(uint64_t h, uint64_t v) {
    return (h ^ v) * 1099511628211ull;
}
static uint64_t hash_dcache(const mc68k_state_t *cpu) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned set = 0; set < MC68K_CACHE_SETS_MAX; set++) {
        h = mix(h, cpu->dcache040_next[set]);
        for (unsigned way = 0; way < MC68K_040_CACHE_WAYS; way++) {
            const mc68k_040_cache_line_t *l = &cpu->dcache040[set][way];
            h = mix(h, (uint64_t)l->valid | (uint64_t)l->loaded << 1 | (uint64_t)l->dirty << 2);
            h = mix(h, l->physical_line);
            for (unsigned i = 0; i < 4; i++)
                h = mix(h, l->data[i]);
        }
    }
    return h;
}
static uint64_t hash_ram(void) {
    uint64_t h = 1469598103934665603ull;
    for (uint32_t i = 0; i < RAM_SIZE; i++)
        h = mix(h, f_g->ram[i]);
    return h;
}

typedef struct {
    uint32_t value;
    int wait;
    uint64_t reads, dcache, ram;
    uint32_t state;
} rec_t;

static void init_cpu(bool disable_fast, bool is060) {
    memset(f_g, 0, sizeof *f_g);
    uint32_t seed = 12345u;
    for (uint32_t i = 0; i < RAM_SIZE; i++) {
        seed = seed * 1664525u + 1013904223u;
        f_g->ram[i] = (uint8_t)(seed >> 24);
    }
    f_g->ram[0] = 0;
    f_g->ram[1] = 0;
    f_g->ram[2] = (uint8_t)(SSP_INIT >> 8);
    f_g->ram[3] = (uint8_t)SSP_INIT;
    f_g->ram[6] = (uint8_t)(RESET_PC >> 8);
    f_g->ram[7] = (uint8_t)RESET_PC;
    mc68k_reset(cpu_g);
    cpu_040_cache_reset(cpu_g);
    cpu_g->bus_wait_accum = 0;
    cpu_g->dcache_hit_fast_disable = disable_fast;
    cpu_g->cacr = MC68K_CACR_040_DE;
    cpu_g->sr = SR_S;
    if (is060) {
        cpu_g->mmu060_tc = 0;
        cpu_g->mmu060_itt0 = cpu_g->mmu060_itt1 = cpu_g->mmu060_dtt1 = 0;
        cpu_g->mmu060_dtt0 = TTR_ALL;
    }
    rng = 0x2545f491u;
}

static void set_mode(unsigned mode, bool is060) {
    if (is060)
        cpu_g->mmu060_dtt0 = TTR_ALL | (mode << TTR_CM_SHIFT);
}

/* 攪乱したラインの物理ラインの先頭を返す（直後にそのラインを読み、攪乱が効く状態を通常経路と比べる）。
 */
static uint32_t perturb(void) {
    const unsigned set =
        ((next_rand() % 10u) < 7u ? (RANGE_BASE >> 4) + next_rand() % (HOT_RANGE >> 4)
                                  : next_rand() >> 4) &
        (MC68K_060_CACHE_SETS - 1u);
    const unsigned way = next_rand() % MC68K_040_CACHE_WAYS;
    mc68k_040_cache_line_t *l = &cpu_g->dcache040[set][way];
    uint32_t target = l->physical_line;
    switch (next_rand() % 4u) {
    case 0:
        l->valid = false;
        break;
    case 1:
        l->loaded = false;
        break;
    case 2: { /* 有効なラインがあれば、同じセットの別ウェイに、同じ物理ラインで内容の違う重複を作る
               */
        unsigned src = MC68K_040_CACHE_WAYS;
        for (unsigned k = 0; k < MC68K_040_CACHE_WAYS; k++) {
            const unsigned w = (way + k) % MC68K_040_CACHE_WAYS;
            if (cpu_g->dcache040[set][w].valid) {
                src = w;
                break;
            }
        }
        if (src == MC68K_040_CACHE_WAYS)
            return 0;
        const unsigned other = (src + 1u + next_rand() % 3u) % MC68K_040_CACHE_WAYS;
        mc68k_040_cache_line_t copy = cpu_g->dcache040[set][src];
        for (unsigned i = 0; i < 4; i++)
            copy.data[i] ^= next_rand();
        if (next_rand() & 1u)
            copy.loaded = false; /* 重複の一方だけ未ロードにして、先に見つかる方の違いを出す */
        cpu_g->dcache040[set][other] = copy;
        target = copy.physical_line;
        break;
    }
    default:
        l->data[next_rand() & 3u] ^=
            next_rand(); /* ホストの書込みでメモリと食い違った古い内容 */
        break;
    }
    return target;
}

static void run_ops(rec_t *rec, unsigned *n, unsigned count, bool is060) {
    for (unsigned i = 0; i < count; i++) {
        const uint32_t addr =
            RANGE_BASE + next_rand() % ((next_rand() & 1u) ? RANGE : HOT_RANGE);
        const unsigned size_pick = next_rand() % 3u;
        const unsigned size = size_pick == 0 ? 1u : size_pick == 1 ? 2u : 4u;
        const unsigned kind = next_rand() % 100u;
        uint32_t value = 0;
        if (kind < 70) {
            const mc68k_access_t a = cpu_data_access(cpu_g, true, (uint8_t)size, false);
            value = size == 1   ? cpu_read_byte_access(cpu_g, addr, &a)
                    : size == 2 ? cpu_read_word_access(cpu_g, addr, &a)
                                : cpu_read_long_access(cpu_g, addr, &a);
        } else if (kind < 90) {
            const uint32_t v = next_rand();
            const mc68k_access_t a = cpu_data_access(cpu_g, false, (uint8_t)size, false);
            if (size == 1)
                cpu_write_byte_access(cpu_g, addr, (uint8_t)v, &a);
            else if (size == 2)
                cpu_write_word_access(cpu_g, addr, (uint16_t)v, &a);
            else
                cpu_write_long_access(cpu_g, addr, v, &a);
        } else if (kind < 94) {
            const mc68k_access_t a = cpu_data_access(cpu_g, true, (uint8_t)size, true);
            value = size == 1   ? cpu_read_byte_access(cpu_g, addr, &a)
                    : size == 2 ? cpu_read_word_access(cpu_g, addr, &a)
                                : cpu_read_long_access(cpu_g, addr, &a);
        } else if (kind < 98) {
            const uint32_t target = perturb();
            if (target != 0) { /* 攪乱したラインを、整列したアドレスで直ちに読む */
                const mc68k_access_t a = cpu_data_access(cpu_g, true, 2, false);
                value = cpu_read_word_access(cpu_g, target + 2u * (next_rand() & 7u), &a);
            }
        } else if (kind < 99) {
            cpu_g->cacr ^= MC68K_CACR_040_DE; /* DE の切替 */
        } else {
            cpu_040_cache_reset(cpu_g); /* CINVA 相当 */
        }
        (void)is060;
        assert(cpu_g->exec_state == CPU_NORMAL);
        rec[*n] = (rec_t){.value = value,
                          .wait = cpu_g->bus_wait_accum,
                          .reads = f_g->reads,
                          .dcache = hash_dcache(cpu_g),
                          .ram = (i % 64u == 0) ? hash_ram() : 0,
                          .state = cpu_g->cacr};
        (*n)++;
    }
}

#define MAX_OPS (OPS_PER_MODE * NUM_MODES)

static void run_all(bool disable_fast, bool is060, rec_t *rec, unsigned *n) {
    *n = 0;
    init_cpu(disable_fast, is060);
    for (unsigned mode = 0; mode < (is060 ? NUM_MODES : 1u); mode++) {
        set_mode(mode, is060);
        if (!(cpu_g->cacr & MC68K_CACR_040_DE))
            cpu_g->cacr |= MC68K_CACR_040_DE;
        run_ops(rec, n, OPS_PER_MODE, is060);
    }
    rec[*n] = (rec_t){.ram = hash_ram()};
    (*n)++;
}

static void compare(const char *name, bool is060) {
    static rec_t fast[MAX_OPS + 1], slow[MAX_OPS + 1];
    unsigned nf, ns;
    run_all(false, is060, fast, &nf);
    run_all(true, is060, slow, &ns);
    assert(nf == ns);
    for (unsigned i = 0; i < nf; i++) {
        if (fast[i].value != slow[i].value || fast[i].wait != slow[i].wait ||
            fast[i].reads != slow[i].reads || fast[i].dcache != slow[i].dcache ||
            fast[i].ram != slow[i].ram || fast[i].state != slow[i].state) {
            fprintf(stderr, "mismatch: %s op %u (value %08x/%08x wait %d/%d reads %llu/%llu)\n",
                    name, i, fast[i].value, slow[i].value, fast[i].wait, slow[i].wait,
                    (unsigned long long)fast[i].reads, (unsigned long long)slow[i].reads);
            abort();
        }
    }
}

int main(void) {
    static fixture_t f;
    f_g = &f;
    mc68k_bus_t bus = {.read_byte = rb,
                       .read_word = rw,
                       .read_long = rl,
                       .write_byte = wb,
                       .write_word = ww,
                       .write_long = wl,
                       .reset_peripheral = reset_peripheral,
                       .access_timing = timing_cb,
                       .ctx = &f};
    cpu_g = mc68k_create_model(&bus, MC68K_MODEL_68060);
    if (!cpu_g)
        abort();
    compare("68060", true);
    mc68k_destroy(cpu_g);
    cpu_g = mc68k_create_model(&bus, MC68K_MODEL_68040);
    if (!cpu_g)
        abort();
    compare("68040", false);
    mc68k_destroy(cpu_g);
    return 0;
}
