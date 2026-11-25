/*
 * musashi_wrapper.c — Musashi を単一コンパイル単位にまとめるラッパー
 *
 * softfloat の mamesf.h は sint8/uint8 等の型を使うが自分では定義しない
 * （MAME 側が定義することを前提としている）。
 * 単一 TU にまとめることで、m68kcpu.h が型を定義してから softfloat.c が
 * コンパイルされる順序を保証する。
 *
 * 互換性のある typedef の再定義は GCC/Clang では C11 拡張として許容される
 * ため、m68kcpu.h 内の #undef → typedef と重複しても問題ない。
 */

/* --- softfloat が必要とする型（mamesf.h より先に定義する） --- */
typedef signed char sint8;
typedef signed short sint16;
typedef signed int sint32;
typedef unsigned char uint8;
typedef unsigned short uint16;
typedef unsigned int uint32;
typedef signed long long sint64;
typedef unsigned long long uint64;
typedef signed int sint;
typedef unsigned int uint;

/* --- softfloat 設定 (mamesf.h は MAME が定義することを期待) --- */
#define INLINE static
#define BITS64
#define LIT64(a) a##ULL
#ifndef FALSE
#define FALSE 0
#define TRUE  1
#endif

/* --- softfloat 実装（型が上で定義済みなので milieu.h/mamesf.h の再定義は無害） --- */
#include "musashi/softfloat/softfloat.c"

/* --- Musashi CPU コア（内部で m68kfpu.c を #include する） --- */
#include "musashi/m68kcpu.c"

/* --- 生成されたオペコードハンドラ（-I ${MUSASHI_GEN} で解決） --- */
#include "m68kops.c"

/* --- 逆アセンブラ（m68k_disassemble_raw()。 ステップ6の命令長
 * クロスチェックテスト用。通常の差分テスト（実行結果比較）では使わない） --- */
#include "musashi/m68kdasm.c"
