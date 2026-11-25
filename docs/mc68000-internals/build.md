# ビルド

## 必要環境

- C99 コンパイラ
- CMake 3.16 以降
- Git submodule として取得する Berkeley SoftFloat 3e

```sh
git submodule update --init extern/berkeley-softfloat-3
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

`extern/berkeley-softfloat-3` が未取得の場合、CMake configure は失敗する。ネットワーク取得は
configure 中に行わないため、明示的に submodule を初期化する。

## 成果物

ビルドは静的ライブラリ `mc68000` を生成する。公開ヘッダは `include/mc68000/` にあり、
ライブラリ利用側にはこのディレクトリだけを公開する。`src/` は内部実装である。

```cmake
add_subdirectory(path/to/cmc68000)
target_link_libraries(your_target PRIVATE mc68000)
```

別プロジェクトから `add_subdirectory()` で組み込んだ場合、テストは自動では有効にならない。
リポジトリ自身を CMake の最上位プロジェクトとして構成した場合だけ、CTest と `tests/` を有効にする。

## コンパイラ警告

MSVC では `/W4`、その他のコンパイラでは `-Wall -Wextra -Wpedantic` をライブラリへ設定する。
警告は修正対象として扱い、警告抑制で通過させない。

## テスト

`ctest --test-dir build --output-on-failure` は memory、CPU、FPU、差分テストを実行する。
特定のテストだけを実行する場合は CTest の正規表現を使う。

```sh
ctest --test-dir build --output-on-failure -R '^test_68010$'
```

公開文書中の C コード例は、対象ヘッダとリンク設定を使って警告なしでコンパイルできる状態を維持する。

## PGO（プロファイル最適化）

PGO（Profile-Guided Optimization）は、実際に実行したときの分岐の偏りや関数の呼び出し頻度をコンパイラに
教えて、インライン化・分岐配置・コード配置を最適化させる手法である。エミュレータの実行ループのように、
分岐が多く実行の偏りが大きいコードで効きやすい。ゲストから見える挙動は変わらない（最適化だけが変わる）。

**プロファイルの作成が必要**: 通常のビルドだけでは効果がない。計測用のビルドを実行してプロファイルを採り、
そのプロファイルを使って再ビルドする。計測ビルドは遅い（実行時に計数するため）ので、日常の開発には向かない。

**運用方針: リリースビルドのときだけ作る**

- 毎ビルドではプロファイルを作らない。日常のビルド・試験は PGO なしの通常ビルドで行う。
- リリース用ビルドの直前に、そのときの `src/` で計測、merge、使用ビルドの順に行う。
- プロファイルはコミットしない（`.gitignore` 済み）。リリース手順の中で毎回作る。

**プロファイルが古い場合**

古いプロファイル（`src/` を変更する前に採ったもの）でも、ビルドは通り、動作も正しい。コンパイラは、
関数の形がプロファイル採取時と食い違う関数のデータを無視する（`-Wprofile-instr-out-of-date` が出る）。
ただし次の弊害がある。

- 食い違った関数と新しく追加した関数は、プロファイルの恩恵を受けない。最高速にならない。
- 食い違っていない関数でも、呼び出し頻度や分岐の偏りが現状と違えば、最適化が外れて通常ビルドより
  遅くなることがありうる。
- 食い違いは警告で気づけるが、形が同じまま挙動だけ変わった関数は検出できない。

したがって、速度が必要なリリースの前には必ず採り直す。

以下は Apple clang 21（Xcode 同梱）で、計測ビルド、`llvm-profdata merge`、使用ビルドの流れが
通ることを確認した手順である。CMake に PGO 専用の設定はなく、`CMAKE_C_FLAGS` でフラグを渡す。
GCC（`-fprofile-generate` / `-fprofile-use`）は未検証である。

1. 計測ビルドを作り、代表的なワークロードを実行する。`%p` はプロセスごとに別ファイルにする。

```sh
cmake -S . -B build-pgo-gen -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_FLAGS="-fprofile-instr-generate=$PWD/pgo-raw/%p.profraw"
cmake --build build-pgo-gen
# ワークロードを実行する（下記）
```

2. 生データを 1 つにまとめる。

```sh
xcrun llvm-profdata merge -output=mc68000.profdata pgo-raw/*.profraw
```

3. プロファイルを使ってライブラリをビルドする。

```sh
cmake -S . -B build-pgo-use -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_FLAGS="-fprofile-instr-use=$PWD/mc68000.profdata"
cmake --build build-pgo-use --target mc68000
```

注意:

- **ワークロード**: `ctest` は機能試験で、実利用の命令分布とは異なる。最適化の効果を見るには、上位プロジェクトを
  計測ビルドのライブラリにリンクし、実際の負荷（OS 起動など）を走らせて採取する。上位プロジェクトにも同じ
  `-fprofile-instr-generate` を渡す。
- **警告**: プロファイルに現れないファイルは `-Wprofile-instr-unprofiled` を出し、ソースを変えた後の古いプロファイルは
  `-Wprofile-instr-out-of-date` を出す。使用ビルドは `--target mc68000` に限ると、テストコード由来の警告は出ない。
  ワークロードが通らないライブラリのファイル（試験 1 本だけで採取した場合の `memory.c`・`bus.c` など）は警告になるので、
  警告を消すためのフラグ追加ではなく、ワークロードを広げて解消する。
- **鮮度**: `src/` を変えたらプロファイルを採り直す。`.profdata` と `pgo-raw/` はコミットしない。
- **試験**: PGO ビルドでも `ctest` の通過を確認する。
- **コンパイラの版**: `.profdata` の形式は `llvm-profdata` の版に依存する。計測ビルド、merge、使用ビルドは
  同じツールチェーンで行う。コンパイラを更新したら採り直す。
- **同じ構成で採る**: 最適化レベル・`CMAKE_BUILD_TYPE`・マクロ定義が計測ビルドと使用ビルドで違うと、
  関数の形が食い違って無視される。両方 `Release` にそろえる。
- **偏ったワークロード**: 採取した負荷に偏った最適化になる。たとえば 060 だけの負荷で採ると、
  他モデルの経路は最適化されない（遅くはなりにくいが速くもならない）。使う可能性が高い負荷を複数混ぜて採る。
- **計測ビルドは遅い**: 実行時間がかかる。ワークロードの長さは、計測ビルドで現実的に回せる範囲にする。
- **効果は測る**: PGO の有無で同じ負荷の実行時間を比べて、効果を確認してから採用する。
