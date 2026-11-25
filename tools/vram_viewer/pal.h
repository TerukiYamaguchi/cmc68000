#pragma once
#include <stdint.h>

/*
 * PAL — Platform Abstraction Layer（描画バックエンド抽象化）
 *
 * SDL2 に直接依存させず、このインターフェース越しに描画する。
 * 将来的に上位プロジェクト側で別バックエンド（Metal / Vulkan 等）に
 * 差し替えられるよう、SDL2 の型はこのヘッダに一切露出しない。
 */

typedef struct pal_ctx pal_ctx_t;

/* ウィンドウを生成して返す。失敗時は NULL */
pal_ctx_t *pal_create(int width, int height, const char *title);

/* ウィンドウを破棄する */
void pal_destroy(pal_ctx_t *ctx);

/*
 * ARGB8888 ピクセル配列を画面に転送して表示する。
 * pixels は width * height 個の uint32_t（0xAARRGGBB）。
 */
void pal_present(pal_ctx_t *ctx, const uint32_t *pixels);

/*
 * イベントをポーリングする。
 * 戻り値: 1 = 継続, 0 = 終了要求（ウィンドウ閉じる / ESC）
 */
int pal_poll(pal_ctx_t *ctx);

/* ms ミリ秒スリープする */
void pal_delay(unsigned int ms);
