#pragma once
#include <stdint.h>

#define VRAM_W 512
#define VRAM_H 512

/*
 * X68000 グラフィック VRAM — 65536 色モード（GRB555）
 *
 * ピクセルフォーマット（16bit）:
 *   bit 15   : 未使用（0）
 *   bit 14-10: G（緑 5bit）
 *   bit  9-5 : R（赤 5bit）
 *   bit  4-0 : B（青 5bit）
 *
 * ※ RGB555 ではなく GRB555 の順序である点に注意。
 */
typedef struct {
    uint16_t pixels[VRAM_H][VRAM_W];
} vram_t;

/* GRB555 1ピクセル → ARGB8888 変換（SDL_PIXELFORMAT_ARGB8888 向け）*/
static inline uint32_t grb555_to_argb8888(uint16_t grb) {
    uint8_t g = (uint8_t)(((grb >> 10) & 0x1F) * 255 / 31);
    uint8_t r = (uint8_t)(((grb >> 5) & 0x1F) * 255 / 31);
    uint8_t b = (uint8_t)(((grb >> 0) & 0x1F) * 255 / 31);
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

/* GRB555 ピクセル値を r,g,b（各 0-31）から作成する */
static inline uint16_t grb555(int r, int g, int b) {
    return (uint16_t)(((g & 0x1F) << 10) | ((r & 0x1F) << 5) | (b & 0x1F));
}

/* VRAM 全体を ARGB8888 バッファに変換。out は VRAM_W * VRAM_H 要素 */
void vram_to_argb(const vram_t *vram, uint32_t *out);
