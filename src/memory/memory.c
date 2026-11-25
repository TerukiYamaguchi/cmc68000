/*
 * memory.c — メモリリージョン管理の実装
 *
 * リージョンは線形リストで管理する。X68000 のリージョン数は多くて数十程度なので
 * 線形探索で十分。アクセス頻度の高い RAM を先頭に登録すれば実用上問題ない。
 *
 * MC68000 はビッグエンディアンなので、word/long の読み書きは
 * バイト順を手動で並べ替える必要がある。
 *
 * リージョン外アクセス（マッピングされていないアドレス）:
 *   実機ではバスエラー例外が発生するが、ここでは 0xFF を返すことで
 *   「未接続バス」の動作をゆるく再現している。
 *   Phase 3 以降でバスエラー例外を実装したらここも修正する。
 */
#include "memory.h"
#include <stdlib.h>
#include <string.h>

#define MAX_REGIONS 32 /* X68000 の用途では十分な数 */

struct mem_ctx {
    mem_region_t *regions[MAX_REGIONS];
    int count;
    bool addr_error;          /* 奇数アドレスへの word/long アクセスで立つ */
    uint32_t addr_error_addr; /* エラーが発生したアドレス（例外ハンドラが参照） */
};

mem_ctx_t *mem_create(void) {
    return calloc(1, sizeof(mem_ctx_t));
}

void mem_destroy(mem_ctx_t *ctx) {
    /* data ポインタの解放は呼び出し側の責任。ここでは ctx 自体だけ解放する */
    free(ctx);
}

int mem_add_region(mem_ctx_t *ctx, mem_region_t *region) {
    if (!ctx || !region || region->size == 0)
        return -1;
    if (ctx->count >= MAX_REGIONS)
        return -1;
    ctx->regions[ctx->count++] = region;
    return 0;
}

/* アドレスが属するリージョンを線形探索する */
static mem_region_t *find_region(mem_ctx_t *ctx, uint32_t addr) {
    for (int i = 0; i < ctx->count; i++) {
        mem_region_t *r = ctx->regions[i];
        if (addr >= r->base && addr < r->base + r->size)
            return r;
    }
    return NULL; /* 未マップ領域 */
}

uint8_t mem_read_byte(mem_ctx_t *ctx, uint32_t addr) {
    mem_region_t *r = find_region(ctx, addr);
    return r ? r->data[addr - r->base] : 0xFF; /* 未マップは 0xFF（未接続バス） */
}

uint16_t mem_read_word(mem_ctx_t *ctx, uint32_t addr) {
    /* ビッグエンディアン: 上位バイトが先 */
    uint16_t hi = mem_read_byte(ctx, addr);
    uint16_t lo = mem_read_byte(ctx, addr + 1);
    return (uint16_t)((hi << 8) | lo);
}

uint32_t mem_read_long(mem_ctx_t *ctx, uint32_t addr) {
    uint32_t hi = mem_read_word(ctx, addr);
    uint32_t lo = mem_read_word(ctx, addr + 2);
    return (hi << 16) | lo;
}

void mem_write_byte(mem_ctx_t *ctx, uint32_t addr, uint8_t val) {
    mem_region_t *r = find_region(ctx, addr);
    if (!r || r->read_only)
        return; /* ROM への書き込みは黙って無視 */
    r->data[addr - r->base] = val;
    if (r->on_write)
        r->on_write(addr, 1, r->hook_ctx);
}

void mem_write_word(mem_ctx_t *ctx, uint32_t addr, uint16_t val) {
    /* ビッグエンディアン: 上位バイトを先に書く */
    mem_write_byte(ctx, addr, (uint8_t)(val >> 8));
    mem_write_byte(ctx, addr + 1, (uint8_t)(val));
}

void mem_write_long(mem_ctx_t *ctx, uint32_t addr, uint32_t val) {
    mem_write_word(ctx, addr, (uint16_t)(val >> 16));
    mem_write_word(ctx, addr + 2, (uint16_t)(val));
}

/* --- アドレスエラー管理 --- */

void mem_set_addr_error(mem_ctx_t *ctx, uint32_t addr) {
    /* 最初に発生したエラーだけを記録する（二重エラーはダブルバスフォルトになるため） */
    if (!ctx->addr_error) {
        ctx->addr_error = true;
        ctx->addr_error_addr = addr;
    }
}

bool mem_has_addr_error(const mem_ctx_t *ctx) {
    return ctx ? ctx->addr_error : false;
}

uint32_t mem_get_addr_error_addr(const mem_ctx_t *ctx) {
    return ctx ? ctx->addr_error_addr : 0;
}

void mem_clear_addr_error(mem_ctx_t *ctx) {
    if (!ctx)
        return;
    ctx->addr_error = false;
    ctx->addr_error_addr = 0;
}
