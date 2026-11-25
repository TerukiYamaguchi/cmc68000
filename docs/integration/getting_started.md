# cmc68000 を始める

`cmc68000` は、ホストが提供するバスコールバックを通じて動作する C99 の
Motorola 68000 系 CPU エミュレータライブラリです。CPU はメモリや周辺機器を
所有しません。利用側がバスを実装し、CPU を生成、reset、実行します。

この文書では、MC68000 を RAM のみで起動し、`NOP` の後に `STOP` する最小例を示します。
実際のエミュレータでは、この RAM バスを ROM、RAM、I/O、割り込みコントローラなどを
扱うバス実装に置き換えます。

## 前提条件

- C99 コンパイラ
- CMake 3.16 以降
- `cmc68000` の依存サブモジュールを取得済みであること

ライブラリ本体をビルドします。

```sh
git submodule update --init extern/berkeley-softfloat-3
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

利用側の CMake プロジェクトでは、`cmc68000` をサブディレクトリとして追加し、
`mc68000` ターゲットへリンクします。

```cmake
add_subdirectory(path/to/cmc68000)

add_executable(minimal main.c)
target_link_libraries(minimal PRIVATE mc68000)
```

## 最小例

この例のバスは 64 KiB の連続した RAM だけを公開します。範囲外のアクセスは `false`
を返すため、CPU は bus error として扱います。68000 のバス上の word と long は
ビッグエンディアンです。

```c
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include <mc68000/cpu.h>

enum {
    RAM_SIZE = 64 * 1024,
    INITIAL_SSP = 0x0000ff00,
    INITIAL_PC = 0x00000100,
};

typedef struct {
    uint8_t data[RAM_SIZE];
} ram_t;

static bool ram_read_byte(void *ctx, uint32_t addr, uint8_t *out)
{
    ram_t *ram = ctx;
    if (addr >= RAM_SIZE)
        return false;
    *out = ram->data[addr];
    return true;
}

static bool ram_read_word(void *ctx, uint32_t addr, uint16_t *out)
{
    ram_t *ram = ctx;
    if (addr > RAM_SIZE - 2)
        return false;
    *out = ((uint16_t)ram->data[addr] << 8) | ram->data[addr + 1];
    return true;
}

static bool ram_read_long(void *ctx, uint32_t addr, uint32_t *out)
{
    ram_t *ram = ctx;
    if (addr > RAM_SIZE - 4)
        return false;
    *out = ((uint32_t)ram->data[addr] << 24) |
           ((uint32_t)ram->data[addr + 1] << 16) |
           ((uint32_t)ram->data[addr + 2] << 8) |
           ram->data[addr + 3];
    return true;
}

static bool ram_write_byte(void *ctx, uint32_t addr, uint8_t value)
{
    ram_t *ram = ctx;
    if (addr >= RAM_SIZE)
        return false;
    ram->data[addr] = value;
    return true;
}

static bool ram_write_word(void *ctx, uint32_t addr, uint16_t value)
{
    ram_t *ram = ctx;
    if (addr > RAM_SIZE - 2)
        return false;
    ram->data[addr] = (uint8_t)(value >> 8);
    ram->data[addr + 1] = (uint8_t)value;
    return true;
}

static bool ram_write_long(void *ctx, uint32_t addr, uint32_t value)
{
    ram_t *ram = ctx;
    if (addr > RAM_SIZE - 4)
        return false;
    ram->data[addr] = (uint8_t)(value >> 24);
    ram->data[addr + 1] = (uint8_t)(value >> 16);
    ram->data[addr + 2] = (uint8_t)(value >> 8);
    ram->data[addr + 3] = (uint8_t)value;
    return true;
}

int main(void)
{
    ram_t ram = {0};
    const mc68k_bus_t bus = {
        .read_byte = ram_read_byte,
        .read_word = ram_read_word,
        .read_long = ram_read_long,
        .write_byte = ram_write_byte,
        .write_word = ram_write_word,
        .write_long = ram_write_long,
        .ctx = &ram,
    };

    /* reset vectors: initial SSP at $000000, initial PC at $000004 */
    (void)ram_write_long(&ram, 0x000000, INITIAL_SSP);
    (void)ram_write_long(&ram, 0x000004, INITIAL_PC);

    /* $0100: NOP; STOP #$2700 */
    (void)ram_write_word(&ram, INITIAL_PC, 0x4e71);
    (void)ram_write_word(&ram, INITIAL_PC + 2, 0x4e72);
    (void)ram_write_word(&ram, INITIAL_PC + 4, 0x2700);

    mc68k_state_t *cpu = mc68k_create(&bus);
    if (cpu == NULL) {
        fputs("mc68k_create failed\n", stderr);
        return 1;
    }

    mc68k_reset(cpu);
    const int executed = mc68k_execute(cpu, 100);
    printf("executed cycles: %d\n", executed);

    mc68k_destroy(cpu);
    return 0;
}
```

`mc68k_reset()` はアドレス `$000000` の initial SSP と `$000004` の initial PC を
long word として読み込みます。`mc68k_execute()` は指定したサイクル予算まで実行しますが、
命令境界で止まるため、戻り値は予算を超えることがあります。この例では `STOP` により
CPU は停止状態になります。

## 次に読む文書

- [バス](bus.md): callback の完全な契約、IACK、タイミング、CPU 空間アクセス
- [実行](execution.md): 実行ループ、停止、TRAP hook
- [割り込みと例外](interrupts_exceptions.md): IRQ、例外、bus error
- [デバッグ](debugging.md): ブレークポイント、watchpoint、実行記録
