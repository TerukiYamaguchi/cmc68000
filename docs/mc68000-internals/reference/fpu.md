# FPU 実装仕様

## 構成

公開 API は `include/mc68000/fpu.h`、演算実装は `src/fpu/` にある。CPU 作成時の FPU
構成は固定で、68020/68030 では `NONE`、`68881`、`68882`、68040 では `NONE`、`68040`、
`NONE_040`、68060 では `NONE`、`68060` を選べる。`NONE_060`（LC/EC060）は予約で全機種が
拒否する。

68881/68882 は CPU 空間の CIR を介して接続し、CIR protocol は `src/cpu/coproc.c`、FPU
state machine は `src/fpu/fpu_model.c` と `fpu_proto.c` が担当する。68040 内蔵 FPU の
F-line entry と state frame は `src/cpu/ops_fpu.c` が担当する。68060 の F-line
（`op_fpu060`）も同じ実行部と FPU 状態（`cpu->fpu040`）を共有し、060 が実装しない命令
（Table 6-11 の超越関数・FMOVECR・FMOD/FREM/FSCALE・FDBcc/FScc/FTRAPcc 等）は vector 11 の
format `$2`、FPU 無効（PCR.DFP）は `$4`、未実装 EA（動的 FMOVEM.X、2〜3 個の制御レジスタへの
FMOVEM.L #imm、X/P の即値）は vector 60 の format `$0` にする。FINT/FINTRZ は 060 だけが
直接実行する。060 の FSAVE/FRESTORE は NULL を含め 3 long word（format は long 0 の bits 15-8）。

## 数値演算

FP80 の変換、丸め、基本算術、比較、packed decimal、transcendental operation は `src/fpu/`
に分割されている。各 operation は FPU state の rounding mode、precision、exception flag を
更新する。SoftFloat target 層は extended precision の内部演算を補助する。

## 例外と frame

FPU の exception は FPSR と enable mask により報告される。68881/68882 は protocol state を
含む FSAVE/FRESTORE frame を、68040 は null、idle、unimplemented、busy frame を扱う。
frame の読み書き中の bus error は CPU の group 0 exception として処理する。
