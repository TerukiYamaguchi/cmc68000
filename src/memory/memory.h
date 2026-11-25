/*
 * memory.h — メモリリージョン管理
 *
 * MC68000 のアドレス空間（24bit = 16MB）を複数の「リージョン」に分割して管理する。
 * X68000 の場合:
 *   0x000000–0x0FFFFF : メインRAM（1MB、最大12MBまで拡張可）
 *   0xC00000–0xDFFFFF : グラフィックVRAM
 *   0xE00000–0xE7FFFF : テキストVRAM
 *   0xF00000–0xFFFFFF : IOCS ROM
 *   ...（詳細はアドレスマップ参照）
 *
 * on_write フックは将来の JIT 移行のために最初から用意しておく。
 * JIT 化した際、書き込み検出でコンパイル済みブロックを無効化できる。
 * インタプリタ段階では NULL を渡せばよい。
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

/* メモリ書き込み時に呼ばれるフック（JIT 無効化などに使う） */
typedef void (*mem_write_hook_t)(uint32_t addr, uint32_t size, void *ctx);

typedef struct {
    uint8_t *data;             /* バイト配列（呼び出し側が確保・解放する） */
    uint32_t base;             /* このリージョンが占めるアドレスの先頭 */
    uint32_t size;             /* バイト数 */
    bool read_only;            /* ROM 領域など書き込み禁止にする場合 true */
    mem_write_hook_t on_write; /* NULL = フックなし */
    void *hook_ctx;
} mem_region_t;

typedef struct mem_ctx mem_ctx_t;

mem_ctx_t *mem_create(void);
void mem_destroy(mem_ctx_t *ctx);                         /* data の解放は呼び出し側の責任 */
int mem_add_region(mem_ctx_t *ctx, mem_region_t *region); /* 0 = 成功、-1 = 失敗 */

/* ビッグエンディアン読み書き（MC68000 はビッグエンディアン） */
uint8_t mem_read_byte(mem_ctx_t *ctx, uint32_t addr);
uint16_t mem_read_word(mem_ctx_t *ctx, uint32_t addr);
uint32_t mem_read_long(mem_ctx_t *ctx, uint32_t addr);
void mem_write_byte(mem_ctx_t *ctx, uint32_t addr, uint8_t val);
void mem_write_word(mem_ctx_t *ctx, uint32_t addr, uint16_t val);
void mem_write_long(mem_ctx_t *ctx, uint32_t addr, uint32_t val);

/*
 * アドレスエラー管理
 *
 * MC68000 は word / long アクセスを奇数アドレスに行うと
 * アドレスエラー例外（ベクタ番号 3）を発生させる。
 * バス層（bus.c）がここにエラーを記録し、
 * Phase 3 の CPU 実行ループがアクセスごとにチェックして例外処理に移る。
 */
void mem_set_addr_error(mem_ctx_t *ctx, uint32_t addr); /* bus.c 内部から呼ぶ */
bool mem_has_addr_error(const mem_ctx_t *ctx);
uint32_t mem_get_addr_error_addr(const mem_ctx_t *ctx);
void mem_clear_addr_error(mem_ctx_t *ctx);
