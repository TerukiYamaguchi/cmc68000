#include "vram.h"

void vram_to_argb(const vram_t *vram, uint32_t *out) {
    for (int y = 0; y < VRAM_H; y++)
        for (int x = 0; x < VRAM_W; x++)
            out[y * VRAM_W + x] = grb555_to_argb8888(vram->pixels[y][x]);
}
