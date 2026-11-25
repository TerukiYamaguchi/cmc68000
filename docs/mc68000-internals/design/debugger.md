# デバッガ

## 1. 目的と対象範囲

ホスト側デバッガの停止制御、観測、非侵襲メモリ read、逆アセンブル、実行履歴を定義する。

## 2. 実装状態

公開 API は `debug.h`、実装は `src/debug/` にある。CPU 内部状態は公開せず、snapshot と callback
を通じて観測する。

## 3. 設計の要点

| 決定 | 理由 |
| --- | --- |
| debug 実行を通常実行 API と分ける | 通常実行の hot path へ停止判定を常時入れない |
| peek hook を bus read と分離する | I/O の read 副作用を起こさない |
| breakpoint は PC 比較にする | ゲストメモリを書き換えない |

## 4. 構成とデータモデル

`debugger.c` は debug run、breakpoint、watchpoint、履歴 ring、非侵襲 read を担当し、`disasm.c` は
peek hook を使って命令を文字列化する。CPU インスタンスへの操作はホストが直列化する。

## 5. インターフェース、レジスタ、入力

`mc68k_debug_run()`、breakpoint API、watchpoint API、exception break mask、peek hook、
`mc68k_disassemble()`、PC/register/call/fault ring API が公開境界である。

## 6. 動作仕様

debug run は 1 ステップずつ実行し、halt、STOP、breakpoint、watchpoint、設定済み例外を確認する。
watchpoint はステップ境界の値比較であり、書込みアクセスそのものは検出しない。peek hook は
bus error を通知せず、デバイス状態を変更しない。

## 7. 実装上の対応

| モジュール | 責務 |
| --- | --- |
| `src/debug/debugger.c` | 実行制御、停止理由、履歴、非侵襲 read |
| `src/debug/disasm.c` | 命令逆アセンブル |

## 8. 互換性と既知の制約

デバッグ API は同一 CPU への同時実行をサポートしない。watchpoint は同値書込み、または 1 命令内で
元値へ戻る変更を検出できない。

## 9. テストと受入条件

`test_debug_bp`、`test_debug_run`、`test_debug_peek`、`test_debug_step7`、`test_disasm`、
`test_entry_probe` が停止制御、非侵襲 read、逆アセンブル、entry probe を検証する。

## 10. 根拠資料

### 一次資料

- CPU の例外・命令仕様は各 CPU User's Manual に従う。

### 参照実装・検証データ

- `tests/cpu/test_debug_*.c`、`tests/cpu/test_disasm.c`。

### ライセンスと出典

- 本機能はプロジェクト固有のホスト側デバッグ支援である。
