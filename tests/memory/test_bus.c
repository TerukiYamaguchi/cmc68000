/*
 * test_bus.c — バス層のユニットテスト
 *
 * MC68000 のメモリアクセス特性を一通り検証する。
 * assert() で失敗したらプロセスが終了するシンプルな形式。
 * テストフレームワークは使わず、純粋な C99 で書く。
 *
 * テスト一覧:
 *   test_read_write_byte  : バイト読み書き
 *   test_read_write_word  : ワード読み書き（ビッグエンディアン確認）
 *   test_read_write_long  : ロング読み書き（ビッグエンディアン確認）
 *   test_readonly_region  : ROM 領域への書き込みが無視されること
 *   test_unmapped_access  : マップ外アドレスが 0xFF を返すこと
 *   test_addr_error_word  : 奇数アドレスへの word アクセスでエラーフラグが立つこと
 *   test_addr_error_long  : 奇数アドレスへの long アクセスでエラーフラグが立つこと
 *   test_addr_error_clear : エラーフラグのクリアが機能すること
 */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "memory/memory.h"
#include "memory/bus.h"

/* テスト用に 256 バイトの RAM リージョンを base アドレスに作る */
static mem_ctx_t *make_mem(uint8_t *data, uint32_t base, uint32_t size, bool read_only) {
    mem_ctx_t *mem = mem_create();
    mem_region_t *r = calloc(1, sizeof(mem_region_t));
    r->data = data;
    r->base = base;
    r->size = size;
    r->read_only = read_only;
    mem_add_region(mem, r);
    return mem;
}

static void test_read_write_byte(void) {
    uint8_t data[256] = {0};
    mem_ctx_t *mem = make_mem(data, 0x1000, 256, false);
    mc68k_bus_t bus = bus_make_hooks(mem);

    uint8_t v8;
    bus.write_byte(bus.ctx, 0x1000, 0xAB);
    bus.read_byte(bus.ctx, 0x1000, &v8);
    assert(v8 == 0xAB);

    /* バイトアクセスはアドレスエラーを起こさない（奇数アドレスも OK） */
    bus.write_byte(bus.ctx, 0x1001, 0xCD);
    bus.read_byte(bus.ctx, 0x1001, &v8);
    assert(v8 == 0xCD);
    assert(!mem_has_addr_error(mem));

    mem_destroy(mem);
}

static void test_read_write_word(void) {
    uint8_t data[256] = {0};
    mem_ctx_t *mem = make_mem(data, 0x2000, 256, false);
    mc68k_bus_t bus = bus_make_hooks(mem);

    /*
     * MC68000 はビッグエンディアン。
     * 0x1234 を 0x2000 に書くと、メモリは [0x12][0x34] の順になる。
     * x86（リトルエンディアン）と逆なので注意。
     */
    uint16_t v16;
    bus.write_word(bus.ctx, 0x2000, 0x1234);
    bus.read_word(bus.ctx, 0x2000, &v16);
    assert(v16 == 0x1234);
    assert(data[0] == 0x12); /* 上位バイトが先（ビッグエンディアン） */
    assert(data[1] == 0x34);
    assert(!mem_has_addr_error(mem));

    mem_destroy(mem);
}

static void test_read_write_long(void) {
    uint8_t data[256] = {0};
    mem_ctx_t *mem = make_mem(data, 0x3000, 256, false);
    mc68k_bus_t bus = bus_make_hooks(mem);

    /*
     * 0xDEADBEEF を 0x3000 に書くと
     * [0xDE][0xAD][0xBE][0xEF] の順にメモリに格納される。
     */
    uint32_t v32;
    bus.write_long(bus.ctx, 0x3000, 0xDEADBEEF);
    bus.read_long(bus.ctx, 0x3000, &v32);
    assert(v32 == 0xDEADBEEF);
    assert(data[0] == 0xDE);
    assert(data[1] == 0xAD);
    assert(data[2] == 0xBE);
    assert(data[3] == 0xEF);
    assert(!mem_has_addr_error(mem));

    mem_destroy(mem);
}

static void test_readonly_region(void) {
    uint8_t data[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    mem_ctx_t *mem = make_mem(data, 0x4000, 4, true); /* read_only = true */
    mc68k_bus_t bus = bus_make_hooks(mem);

    /* ROM への書き込みは黙って無視される（実機の動作と同じ） */
    uint8_t v8;
    bus.write_byte(bus.ctx, 0x4000, 0x00);
    bus.read_byte(bus.ctx, 0x4000, &v8);
    assert(v8 == 0xDE); /* 変わっていないこと */

    mem_destroy(mem);
}

static void test_unmapped_access(void) {
    uint8_t data[16] = {0};
    mem_ctx_t *mem = make_mem(data, 0x5000, 16, false);
    mc68k_bus_t bus = bus_make_hooks(mem);

    /* マップ外アドレスへの読み出しは 0xFF（未接続バスの動作） */
    uint8_t v8;
    bus.read_byte(bus.ctx, 0x9999, &v8);
    assert(v8 == 0xFF);

    /* マップ外への書き込みは何も起きない（クラッシュしないこと） */
    bus.write_byte(bus.ctx, 0x9999, 0x42);

    mem_destroy(mem);
}

static void test_addr_error_word(void) {
    uint8_t data[256] = {0};
    mem_ctx_t *mem = make_mem(data, 0x6000, 256, false);
    mc68k_bus_t bus = bus_make_hooks(mem);

    /* 偶数アドレスへの word アクセスはエラーなし */
    bus.write_word(bus.ctx, 0x6000, 0x1234);
    assert(!mem_has_addr_error(mem));

    /* 奇数アドレスへの word 書き込み → アドレスエラーフラグが立つ */
    bus.write_word(bus.ctx, 0x6001, 0xABCD);
    assert(mem_has_addr_error(mem));
    assert(mem_get_addr_error_addr(mem) == 0x6001);

    mem_clear_addr_error(mem);
    assert(!mem_has_addr_error(mem));

    /* 奇数アドレスへの word 読み出しもエラー */
    uint16_t v16;
    bus.read_word(bus.ctx, 0x6003, &v16);
    assert(mem_has_addr_error(mem));
    assert(mem_get_addr_error_addr(mem) == 0x6003);

    mem_destroy(mem);
}

static void test_addr_error_long(void) {
    uint8_t data[256] = {0};
    mem_ctx_t *mem = make_mem(data, 0x7000, 256, false);
    mc68k_bus_t bus = bus_make_hooks(mem);

    /* 偶数アドレスへの long アクセスはエラーなし */
    bus.write_long(bus.ctx, 0x7000, 0x12345678);
    assert(!mem_has_addr_error(mem));

    /* 奇数アドレスへの long 書き込み → アドレスエラーフラグが立つ */
    bus.write_long(bus.ctx, 0x7001, 0xDEADBEEF);
    assert(mem_has_addr_error(mem));
    assert(mem_get_addr_error_addr(mem) == 0x7001);

    mem_clear_addr_error(mem);

    /* 奇数アドレスへの long 読み出しもエラー */
    uint32_t v32;
    bus.read_long(bus.ctx, 0x7003, &v32);
    assert(mem_has_addr_error(mem));
    assert(mem_get_addr_error_addr(mem) == 0x7003);

    mem_destroy(mem);
}

static void test_addr_error_clear(void) {
    uint8_t data[256] = {0};
    mem_ctx_t *mem = make_mem(data, 0x8000, 256, false);
    mc68k_bus_t bus = bus_make_hooks(mem);

    bus.write_word(bus.ctx, 0x8001, 0x1234); /* エラー発生 */
    assert(mem_has_addr_error(mem));

    mem_clear_addr_error(mem);
    assert(!mem_has_addr_error(mem));

    /* クリア後は再度正常アクセスできる */
    uint16_t v16;
    bus.write_word(bus.ctx, 0x8000, 0x5678);
    bus.read_word(bus.ctx, 0x8000, &v16);
    assert(v16 == 0x5678);
    assert(!mem_has_addr_error(mem));

    mem_destroy(mem);
}

int main(void) {
    test_read_write_byte();
    test_read_write_word();
    test_read_write_long();
    test_readonly_region();
    test_unmapped_access();
    test_addr_error_word();
    test_addr_error_long();
    test_addr_error_clear();
    return 0;
}
