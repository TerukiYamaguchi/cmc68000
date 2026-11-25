#include "vram.h"
#include "pal.h"
#include <stdlib.h>
#include <stdio.h>

/*
 * テストパターン生成
 *
 * 上半分 (行 0-255): TVカラーバー（8色）
 *   GRB555 変換の正確性を目視確認するための基準色。
 *   白・黄・シアン・緑・マゼンタ・赤・青・黒の順。
 *
 * 下半分 (行 256-511): RG グラデーション（B は時間で変化）
 *   x 軸 → R 成分（0-31）、y 軸 → G 成分（0-31）。
 *   B 成分がフレームごとに 0→31→0 と変化し、GRB555 色空間の
 *   三次元的な広がりを視覚化する。
 */

/* 8色カラーバー（GRB555 値） */
static const uint16_t COLOR_BARS[8] = {
    0x7FFF, /* 白     R=31, G=31, B=31 */
    0x7FE0, /* 黄     R=31, G=31, B= 0 */
    0x7C1F, /* シアン R= 0, G=31, B=31 */
    0x7C00, /* 緑     R= 0, G=31, B= 0 */
    0x03FF, /* マゼンタ R=31, G= 0, B=31 */
    0x03E0, /* 赤     R=31, G= 0, B= 0 */
    0x001F, /* 青     R= 0, G= 0, B=31 */
    0x0000, /* 黒     R= 0, G= 0, B= 0 */
};

static void fill_color_bars(vram_t *vram) {
    for (int y = 0; y < VRAM_H / 2; y++) {
        for (int x = 0; x < VRAM_W; x++) {
            vram->pixels[y][x] = COLOR_BARS[x * 8 / VRAM_W];
        }
    }
}

static void fill_rg_gradient(vram_t *vram, int b) {
    for (int y = VRAM_H / 2; y < VRAM_H; y++) {
        int g = (y - VRAM_H / 2) * 31 / (VRAM_H / 2 - 1);
        for (int x = 0; x < VRAM_W; x++) {
            int r = x * 31 / (VRAM_W - 1);
            vram->pixels[y][x] = grb555(r, g, b);
        }
    }
}

int main(void) {
    vram_t *vram = calloc(1, sizeof(vram_t));
    if (!vram) {
        fprintf(stderr, "calloc vram\n");
        return 1;
    }

    uint32_t *argb = calloc(VRAM_W * VRAM_H, sizeof(uint32_t));
    if (!argb) {
        fprintf(stderr, "calloc argb\n");
        free(vram);
        return 1;
    }

    pal_ctx_t *pal = pal_create(VRAM_W, VRAM_H, "MC68000 VRAM Viewer — GRB555 (ESC to quit)");
    if (!pal) {
        free(argb);
        free(vram);
        return 1;
    }

    fill_color_bars(vram);

    int b = 0, db = 1;
    while (pal_poll(pal)) {
        fill_rg_gradient(vram, b);
        vram_to_argb(vram, argb);
        pal_present(pal, argb);

        /* B 成分を 0→31→0 とゆっくり変化させる */
        b += db;
        if (b >= 31) {
            b = 31;
            db = -1;
        }
        if (b <= 0) {
            b = 0;
            db = 1;
        }

        pal_delay(30); /* 約 33fps */
    }

    pal_destroy(pal);
    free(argb);
    free(vram);
    return 0;
}
