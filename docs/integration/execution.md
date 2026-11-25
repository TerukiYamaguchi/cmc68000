# 実行

`cmc68000` は協調的に実行します。ホストが `mc68k_execute()` を呼び、戻り値の
消費サイクル数を使って、映像、音声、DMA、タイマなどを進めます。CPU はスレッドや
フレームループを内部で作成しません。

## 初期化と reset

CPU を生成した後、実行前に必ず `mc68k_reset()` を呼びます。reset はバスから次の
ベクタを long word で読み、初期状態を設定します。

| アドレス | 内容 |
| --- | --- |
| `$000000` | initial SSP |
| `$000004` | initial PC |

reset は CPU を通常実行状態に戻します。停止または halt の後に再起動する場合も、
バスと reset vector を有効な状態にしてから `mc68k_reset()` を呼びます。

```c
mc68k_state_t *cpu = mc68k_create(&bus);
if (cpu == NULL)
    return -1;

mc68k_reset(cpu);
```

## 実行予算

`mc68k_execute(cpu, cycles)` は、最大 `cycles` サイクルを実行し、実際に消費した
サイクル数を返します。命令の途中では停止しないため、最後の命令によって戻り値が予算を
超えることがあります。通常は正の予算を渡します。

```c
enum { CPU_BUDGET_PER_FRAME = 50000 };

int used = mc68k_execute(cpu, CPU_BUDGET_PER_FRAME);
advance_devices(used);
```

バスの `access_timing` callback で設定した wait cycle は戻り値に含まれます。CPU の
停止中で、受理可能な割り込みもない場合の戻り値は 0 です。

CPU 状態は `mc68k_get_exec_state()` で取得できます。

| 状態 | 意味 | ホストの扱い |
| --- | --- | --- |
| `CPU_NORMAL` | 実行可能 | 次の予算で `mc68k_execute()` を呼ぶ |
| `CPU_EXCEPTION` | group 0 例外の処理中 | 同じ CPU を再度実行し、例外ディスパッチを継続する |
| `CPU_STOPPED` | `STOP` または LPSTOP による待機 | デバイスを進め、受理可能な IRQ を設定して再度実行する |
| `CPU_HALTED` | 二重 bus fault などによる halt | 原因を処理して `mc68k_reset()` するまで実行しない |

`mc68k_execute(cpu, 1)` はホスト側のシングルステップ用プリミティブです。ただし、保留中の
割り込みがあれば命令実行の代わりに割り込み受付を 1 ステップとして行います。また、命令内で
発生した bus error、address error、トレース例外のディスパッチも同じ呼出しに含まれます。

## 割り込みとの実行ループ

push/latch 型の IRQ では、デバイス状態の変化に合わせて `mc68k_set_irq()` を呼びます。
CPU は命令境界で IRQ を判定するため、設定直後に割り込み処理が走るわけではありません。

```c
if (mfp_irq_pending(machine))
    mc68k_set_irq(cpu, 6);
else
    mc68k_set_irq(cpu, 0);

int used = mc68k_execute(cpu, budget);
```

IRQ level 1–7 のうち、SR の割り込みマスクより高い level だけが受理されます。level 7 は
最高優先度です。実機の level-sensitive な IRQ 線をそのまま扱いたい場合は、
`mc68k_set_irq_level_hook()` に pull 型の問い合わせ callback を登録します。pull 型が有効な
間、`mc68k_set_irq()` の値は割り込み判定に使われません。IRQ と IACK の詳細は
[割り込みと例外](interrupts_exceptions.md) を参照してください。

## TRAP hook

`mc68k_set_trap_handler()` は、ゲストが `TRAP #0`〜`TRAP #15` を実行したときに呼ばれる
callback を登録します。ホストが TRAP を処理した場合は非 0 を返します。CPU は例外フレームを
積まず、TRAP の次の命令から実行を続けます。0 を返すと、通常どおりベクタ `32 + trap_num`
の例外処理を行います。

```c
static int trap_handler(mc68k_state_t *cpu, int trap_num, void *ctx)
{
    machine_t *machine = ctx;

    if (trap_num != 15)
        return 0;

    mc68k_regs_t regs;
    mc68k_get_regs(cpu, &regs);
    emulate_iocs(machine, (uint16_t)regs.d[0]);
    return 1;
}

mc68k_set_trap_handler(cpu, trap_handler, machine);
```

TRAP hook 内では `mc68k_get_regs()` で状態を読み、必要なら `mc68k_set_dreg()` で D0–D7 を
更新できます。同じ CPU に対する `mc68k_execute()` の再入呼出しはしないでください。

## 実行時間の観測

`mc68k_executed_cycles()` は、現在または直近の `mc68k_execute()` 呼出しにおける
消費サイクル数を返します。バス callback 内から呼ぶ場合は、実行中の命令を除いた命令境界の
値です。`mc68k_execute()` が戻った後は、その戻り値と一致します。

`mc68k_get_pc()` と `mc68k_get_regs()` は、ホストのログ、UI、デバッガ統合に利用できます。
ブレークポイントや制御付き実行は [デバッグ](debugging.md) を参照してください。
