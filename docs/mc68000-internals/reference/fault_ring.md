# フォルト記録

fault ring は group 0 fault を診断用に記録する固定長リングである。各 record は fault address、
access 属性、命令開始 PC、例外 frame の生成結果を保持する。記録は `m68000.c` の group 0 dispatch
直前に行い、通常の実行結果・例外処理を変更しない。

公開取得 API と record の構造は `include/mc68000/debug.h` を正とする。
