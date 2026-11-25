# デバッグ API 実装仕様

公開 API は `include/mc68000/debug.h`、実装は `src/debug/` にある。CPU state を直接公開せず、
snapshot と callback で観測・制御する。

- PC/register/call/fault ring は固定長の循環バッファで、CPU 実行中に記録する。
- breakpoint、watchpoint、例外 mask、実行停止理由は `mc68k_debug_run()` が扱う。
- memory dump 用の peek hook は通常の bus callback と分離し、観測でデバイス副作用を起こさない。
- disassembler は opcode と extension word を読み、命令長と表記を返す。CPU の実行 state を変更しない。
