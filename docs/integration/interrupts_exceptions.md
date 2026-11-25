# 割り込みと例外

`cmc68000` は IRQ を命令境界で受理し、例外はゲストのベクタテーブルへ配送します。
ホストは IRQ 線、IACK 応答、メモリバス、ベクタテーブルを実装します。通常の例外を
ホスト callback へ直接配送する API はありません。

## IRQ の設定

既定の push/latch 型では、デバイスの割り込み状態が変わるたびに
`mc68k_set_irq()` を呼びます。

```c
static void update_irq(mc68k_state_t *cpu, const machine_t *machine)
{
    mc68k_set_irq(cpu, machine->mfp_irq_pending ? 6 : 0);
}
```

| level | 意味 |
| --- | --- |
| `0` | IRQ 要求なし。現在の push/latch 要求を解除する |
| `1`–`6` | 通常の IRQ level |
| `7` | 最高優先度の IRQ（NMI 相当） |

CPU は SR の割り込みマスクより高い level だけを受理します。`mc68k_set_irq()` は IRQ 線の
状態を更新するだけで、例外フレーム作成やベクタ分岐は次回の `mc68k_execute()` 内の命令境界で
行われます。`STOP` 状態でも、受理可能な IRQ があれば CPU は停止を解除して割り込みを処理します。

## pull 型 IRQ

IPL 線を毎命令サンプリングしたい場合は、`mc68k_set_irq_level_hook()` を使います。

```c
static int irq_level(void *ctx)
{
    const machine_t *machine = ctx;

    if (machine->nmi_pending)
        return 7;
    if (machine->mfp_irq_pending)
        return 6;
    return 0;
}

mc68k_set_irq_level_hook(cpu, irq_level, machine);
```

登録中は push/latch 型の `mc68k_set_irq()` の値を割り込み判定に使いません。callback は
副作用なし、冪等、O(1) にします。同じ命令境界で複数回呼ばれる場合があるため、callback 内で
割り込み要求をクリアしたり、read-clear レジスタを読んだりしてはいけません。解除するには
`mc68k_set_irq_level_hook(cpu, NULL, NULL)` を呼びます。

callback の戻り値は 0–7 にします。範囲外の値は要求なしとして扱われます。callback が返した
level は、その IRQ を受理した場合に IACK callback へ渡されます。

## IACK

`mc68k_bus_t.iack` は、CPU が IRQ を受理するときに呼ばれます。未設定なら CPU は
オートベクタを使用します。

| `iack()` の結果 | 配送先 |
| --- | --- |
| `0` | オートベクタ `24 + level` |
| `1`–`255` | 戻り値をベクタ番号として使用 |
| `mc68k_signal_bus_error()` を呼んでから任意の値を返す | スプリアス割り込み、ベクタ 24 |

IACK の中でデバイスの IRQ 要求を解除して構いません。IACK がスプリアス割り込みを通知する場合に
限り、`mc68k_signal_bus_error(cpu)` を使います。通常の memory read/write callback からは
呼ばず、`false` を返して bus error を通知してください。

```c
static uint8_t iack(void *ctx, int level)
{
    machine_t *machine = ctx;

    if (level == 6 && machine->mfp_irq_pending) {
        machine->mfp_irq_pending = false;
        return 0; /* autovector */
    }

    mc68k_signal_bus_error(machine->cpu);
    return 0;
}
```

## 例外とベクタテーブル

命令実行中の例外は CPU がスタックへ例外フレームを積み、ベクタテーブルから handler の PC を
読みます。利用側は、reset vector と同じバス空間にゲスト OS または firmware の例外ベクタと
handler を配置します。

代表的なベクタは次のとおりです。

| ベクタ | 例外 |
| --- | --- |
| 2 | bus error |
| 3 | address error |
| 4 | illegal instruction |
| 8 | privilege violation |
| 9 | trace |
| 24 | spurious interrupt |
| 25–31 | autovector interrupt level 1–7 |
| 32–47 | `TRAP #0`–`TRAP #15` |

68010 以降は VBR がベクタテーブルの基底です。MC68000 は常にアドレス 0 を基底として使います。
例外フレームの形式は CPU モデルと例外種別に依存するため、ゲストの handler は対象 CPU の仕様に
従って処理する必要があります。

## bus error と address error

memory read/write callback が `false` を返すと、CPU は bus error（ベクタ 2）を配送します。
未マップ領域、応答しないデバイス、または利用側が拒否するアクセスには `false` を返します。

address error（ベクタ 3）は CPU が生成します。68000/68010 の奇数アドレスへの word/long
アクセスが代表例です。バス callback が address error を生成する必要はありません。68020 以降は
データ非整列アクセスの扱いがモデルごとに異なり、CPU が必要なバスアクセスへ分解します。

group 0 例外の処理中は `mc68k_get_exec_state()` が `CPU_EXCEPTION` になる場合があります。
この状態では `mc68k_get_addr_err_addr()` で該当アドレスを取得できます。ホストは CPU を破棄せず、
通常は `mc68k_execute()` を再度呼び出して例外配送を継続します。例外フレームやベクタ読出しが
再び失敗すると、CPU は `CPU_HALTED` になることがあります。

## 実装上の注意

- IRQ callback、IACK callback、memory callback から同じ CPU を再入実行しないでください。
- pull 型 IRQ callback から `mc68k_signal_bus_error()` を呼んではいけません。
- `CPU_HALTED` からは、障害原因を解消して `mc68k_reset()` するまで実行を再開しません。
- TRAP をホスト側で処理する場合は、例外フレームを積まない
  [TRAP hook](execution.md#trap-hook) を使います。

[バス](bus.md) は memory callback と IACK の詳細、[実行](execution.md) は実行予算と
停止状態の扱いを説明します。
