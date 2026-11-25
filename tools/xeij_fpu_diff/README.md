# XEiJ との FPU 差分照合（任意のツール。設計書 `現行仕様` §9.2）

XEiJ の `EFPBox`（ローカルにクラスファイルがあるときだけ使える第二のオラクル）と本ライブラリの FPU 関数を同じ入力で走らせ、
値と EXC を比べる。ctest には入れない（XEiJ が必要なため）。結果は設計書 §9.2 に記録してある。

```sh
export XEIJ_CLASS_DIR=~/Programs/XEiJ_trace/class      # xeij/EFPBox.class があるディレクトリ
cmake --build build                                    # build/libmc68000.a・build/libsoftfloat.a が必要
W=/tmp/xeij_fpu_diff; mkdir -p $W
cp tools/xeij_fpu_diff/*.java $W/ && javac -cp $XEIJ_CLASS_DIR -d $W $W/Harness.java $W/H2.java
cc -I include tools/xeij_fpu_diff/drv.c  build/libmc68000.a build/libsoftfloat.a -o $W/drv
cc -I include tools/xeij_fpu_diff/drv2.c build/libmc68000.a build/libsoftfloat.a -o $W/drv2
python3 tools/xeij_fpu_diff/diff_random.py $W    # 有限の乱数（丸めモード 4 通り）
python3 tools/xeij_fpu_diff/diff_edge.py   $W    # 特殊値・境界（既知の相違が出る）
python3 tools/xeij_fpu_diff/diff_packed.py $W    # packed 入出力
python3 tools/xeij_fpu_diff/diff_prec.py   $W    # PREC=S/D・FSGLMUL/FSGLDIV・FMOVECR
```

注意: `H2.java` は `XEiJ.fmtInit()` を先に呼ぶ必要がある（BCD 表が未初期化だと `getp012()` が止まらない）。
`diff_edge.py` の特殊値（Inf・NaN・ゼロ）は XEiJ の `EFP` 層と命令ディスパッチ層の違いでほとんど比較できない
（Inf の仮数表現が 0 になる等）。有限値の境界だけを見る。
