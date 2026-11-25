/*
 * bus.c — バスフックアダプタの実装
 *
 * mc68k_bus_t の各コールバックを mem_ctx_t のラッパーとして定義する。
 *
 * アドレスエラー検出の仕組み:
 *   MC68000 は word（2 バイト）と long（4 バイト）のアクセスを
 *   必ず偶数アドレスに対して行う制約がある。
 *   奇数アドレスへの word/long アクセスはアドレスエラー例外を引き起こす。
 *
 *   実機では CPU がアドレスバスを駆動した直後に外部回路がエラーを検出するが、
 *   エミュレータではバスコールバック内で addr & 1 をチェックして再現する。
 *   エラー検出時はアクセスを行わず mem_set_addr_error() でフラグを立てる。
 *   CPU（Phase 3）はバスアクセス後にフラグを確認して例外処理に進む。
 */
#include "bus.h"
#include <stdbool.h>
#include <stddef.h>

/* --- バイト: アドレスエラーなし（バイトはどのアドレスでも OK） --- */
/* mem_ctx_t は 24bit 空間全域を mem_region_t でカバーするため未マップは
 * 発生せず、バスエラー（戻り値 false）は起こり得ない。常に true を返す。 */

static bool bus_rb(void *ctx, uint32_t addr, uint8_t *out) {
    *out = mem_read_byte(ctx, addr);
    return true;
}

static bool bus_wb(void *ctx, uint32_t addr, uint8_t v) {
    mem_write_byte(ctx, addr, v);
    return true;
}

/* --- ワード: 奇数アドレスでアドレスエラー --- */

static bool bus_rw(void *ctx, uint32_t addr, uint16_t *out) {
    if (addr & 1) {
        /* 奇数アドレスへの word 読み出し → アドレスエラー */
        mem_set_addr_error(ctx, addr);
        *out = 0;
        return true;
    }
    *out = mem_read_word(ctx, addr);
    return true;
}

static bool bus_ww(void *ctx, uint32_t addr, uint16_t v) {
    if (addr & 1) {
        /* 奇数アドレスへの word 書き込み → アドレスエラー */
        mem_set_addr_error(ctx, addr);
        return true;
    }
    mem_write_word(ctx, addr, v);
    return true;
}

/* --- ロング: 奇数アドレスでアドレスエラー --- */

static bool bus_rl(void *ctx, uint32_t addr, uint32_t *out) {
    if (addr & 1) {
        /* 奇数アドレスへの long 読み出し → アドレスエラー */
        mem_set_addr_error(ctx, addr);
        *out = 0;
        return true;
    }
    *out = mem_read_long(ctx, addr);
    return true;
}

static bool bus_wl(void *ctx, uint32_t addr, uint32_t v) {
    if (addr & 1) {
        /* 奇数アドレスへの long 書き込み → アドレスエラー */
        mem_set_addr_error(ctx, addr);
        return true;
    }
    mem_write_long(ctx, addr, v);
    return true;
}

/* --- 補助コールバック --- */

/* DMA 同期。テスト用スタンドアロン環境では何もしなくてよい */
static void bus_sync(void *ctx, int cycles) {
    (void)ctx;
    (void)cycles;
}

/* JIT 無効化フック。インタプリタ段階では空実装 */
static void bus_wh(void *ctx, uint32_t addr, uint32_t size) {
    (void)ctx;
    (void)addr;
    (void)size;
}

mc68k_bus_t bus_make_hooks(mem_ctx_t *ctx) {
    /* C99 の指示付き初期化子でフィールドを名前指定して設定する。
     * フィールドの並び順を覚えなくてよいので可読性が高い。 */
    mc68k_bus_t b = {
        .read_byte = bus_rb,
        .read_word = bus_rw,
        .read_long = bus_rl,
        .write_byte = bus_wb,
        .write_word = bus_ww,
        .write_long = bus_wl,
        .sync = bus_sync,
        .write_hook = bus_wh,
        .iack = NULL,
        .reset_peripheral = NULL,
        .ctx = ctx,
    };
    return b;
}
