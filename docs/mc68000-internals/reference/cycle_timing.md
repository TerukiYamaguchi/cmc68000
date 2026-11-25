# MC680x0 命令サイクル timing

## 構造

`cycles.h` は model profile、instruction class、EA cost kind を定義する。CPU 作成時に
`cyc_resolve()` が profile 継承を解決して state に固定する。handler は `CYC(cpu, class)` で
基本値を取得し、必要な場合だけ `ea_cycles()` を加算する。

## profile

68000、68010、68020、68030/68EC030、68040 に profile がある。`CYC_INHERIT` は親 profile の値を、
`CYC_ABSENT` はその model で利用できない class を表す。68040 固有 handler は、命令表の計算値を
直接返す箇所もある。

## EA cost

EA cost kind は fetch、calculate、その immediate variant、jump、none である。68020/68030 の
full extension は EA descriptor の base displacement、outer displacement、memory indirect、
suppression 情報で表す。`EA_COST_NONE` の class へ EA cost を重ねてはならない。

## bus wait

bus callback の timing hook が返す wait は、実際に callback を呼んだ access にだけ蓄積する。
`mc68k_execute()` の戻り値は handler cycle とその呼出し中に蓄積した wait の合計である。
