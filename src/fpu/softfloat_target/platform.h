/*
 * platform.h — Berkeley SoftFloat 3e 用のビルド設定（mc68000 の FPU 共通コア向け）。
 *
 * SoftFloat 付属の build/template-FAST_INT64/platform.h の必須項目（LITTLEENDIAN、
 * INLINE）を、同じ値で設定するだけのファイル（BSD-3-Clause の帰属は
 * LICENSES/SoftFloat-BSD-3-Clause.txt）。
 * SoftFloat 本体（extern/berkeley-softfloat-3）は無改変のまま使う。
 * ホストがリトルエンディアンであることを前提にする（x86-64 / arm64）。
 * インライン展開は使わず（INLINE_LEVEL 未定義）、プリミティブは各 .c から
 * 通常の関数としてリンクする。
 *
 * THREAD_LOCAL は softfloat.h が platform.h より先に取り込まれる翻訳単位が
 * あるため、ここでは定義せず CMake のコンパイル定義で全翻訳単位に渡す。
 */

#ifndef MC68K_SOFTFLOAT_PLATFORM_H
#define MC68K_SOFTFLOAT_PLATFORM_H

#define LITTLEENDIAN 1

#define INLINE inline

#endif
