/*
 * bus.h — バスフックアダプタ
 *
 * mc68k_bus_t（CPU 側が期待するコールバック群）と
 * mem_ctx_t（メモリリージョン管理）を橋渡しする。
 *
 * bus_make_hooks() で生成した mc68k_bus_t を mc68k_create() に渡すことで、
 * CPU がメモリに直接アクセスできるようになる。
 *
 * アドレスエラー検出:
 *   word / long アクセスが奇数アドレスに対して行われた場合、
 *   mc68k_bus_t コールバック内で mem_set_addr_error() を呼んでフラグを立てる。
 *   アクセス自体は行わず 0 を返す（または書き込みをスキップする）。
 *   Phase 3 で CPU がバスアクセス後に mem_has_addr_error() を確認し、
 *   アドレスエラー例外（ベクタ 3）を起こす。
 *
 * x68000 側では、メモリ以外のデバイス（CRTC・OPM 等）への
 * アクセスを横取りするために mc68k_bus_t を独自実装する。
 * そのため bus_make_hooks() はテスト・スタンドアロン用と考えてよい。
 */
#pragma once
#include "memory.h"
#include "mc68000/bus_hooks.h"

/* mem_ctx_t をバックエンドとする mc68k_bus_t を生成する（テスト・スタンドアロン用） */
mc68k_bus_t bus_make_hooks(mem_ctx_t *ctx);
