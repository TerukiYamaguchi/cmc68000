# 命令・機能サポート状況

この文書は `src/cpu/decode.c` の登録順と各 `register_*_ops()` の現行実装を
要約する。opcode が表に無いことだけでは未実装を意味しない。命令の可用性は CPU
モデルと EA 制約によって決まる。

## モデル

| モデル | 追加される主な機能 |
| --- | --- |
| MC68000 | 基本命令、6/14 バイト例外フレーム |
| MC68010 | VBR、MOVEC、MOVES、RTD、BKPT、format 0/8 例外フレーム |
| MC68020 | full-format EA、bit field、CAS/CAS2、CHK2/CMP2、LINK.L、TRAPcc |
| MC68EC030 / MC68030 | 68030 MMU 命令。EC030 では access-control レジスタを使う |
| MC68040 | 040 MMU、D-cache 操作、MOVE16、040 F-line/FPU 経路 |
| MC68060 | 060 MMU、未実装命令の trap、LPSTOP、PLPA、format `$4` frame、命令時間（`timing_060.c`）。MC68LC060/EC060 は未対応 |

MC68060 の詳細は `design_mc68060.md` を参照。

## 登録モジュール

`decode.c` は基本命令を登録後、モデルごとの登録で同じ opcode を上書きする。
登録順はデコード契約の一部であり変更しない。

| モジュール | 担当 |
| --- | --- |
| `ops_move.c`, `ops_transfer.c` | MOVE、MOVEM、MOVEP、LEA、PEA、EXG |
| `ops_alu.c`, `ops_arith_ext.c` | 加減算、比較、乗除算、BCD、CHK、TAS |
| `ops_logic.c`, `ops_shift.c` | 論理、ビット操作、シフト、回転 |
| `ops_branch.c`, `ops_trap.c` | 分岐、DBcc、Scc、TRAP |
| `ops_system.c` | NOP、RESET、STOP、RTE、RTR、USP 操作 |
| `ops_system_ext.c` | 68010 以降の拡張 system 命令 |
| `ops_68020_arith.c`, `ops_68020_bitfield.c`, `ops_68020_system.c` | 68020 拡張命令 |
| `ops_68030_mmu.c` | 68030/EC030 MMU と access control |
| `ops_68040_cache.c`, `ops_68040_mmu.c` | 68040 固有機能 |
| `ops_fpu.c` | 68040/68060 の内蔵 FPU |
| `ops_68060.c`, `ops_68060_mmu.c` | 68060 固有機能 |
| `coproc.c` | 68881/68882 コプロセッサ・インタフェース |

## 共通実装規約

- EA の読み書きは `ea_helpers.c` に集約する。A7 の byte pre/post increment は 2 byte。
- 奇数番地の word/long access は address error になる。バス callback の失敗は bus error。
- 特権命令は副作用の前に privilege violation を発生させる。
- IRQ は命令境界でだけ受理する。
- 命令 handler は消費サイクル数を返す。世代別の値と EA 寄与は `cycles.c` が解決する。
