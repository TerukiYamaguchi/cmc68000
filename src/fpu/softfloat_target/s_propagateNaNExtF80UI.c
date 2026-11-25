/*============================================================================

This C source file is part of the SoftFloat IEEE Floating-Point Arithmetic
Package, Release 3e, by John R. Hauser.

Copyright 2011, 2012, 2013, 2014, 2018 The Regents of the University of
California.  All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

 1. Redistributions of source code must retain the above copyright notice,
    this list of conditions, and the following disclaimer.

 2. Redistributions in binary form must reproduce the above copyright notice,
    this list of conditions, and the following disclaimer in the documentation
    and/or other materials provided with the distribution.

 3. Neither the name of the University nor the names of its contributors may
    be used to endorse or promote products derived from this software without
    specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS "AS IS", AND ANY
EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE, ARE
DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

=============================================================================*/

/*
 * 改変: 改変元は上記 extern/berkeley-softfloat-3
 * （pin a0c6494）の source/8086/s_propagateNaNExtF80UI.c。
 *
 * 8086版は「両方SNaNなら絶対値の大きい方」「SNaN側にquiet化で0xC0…（bit63・bit62の
 * 両方）を立てる」という x87 の規則を実装しているが、MC68881 は次の点が異なる
 * （一次資料 [881UM] §4.5.4.1/§4.5.4.2、§6.1.2 p.6-7
 * 「No bits other than the SNAN bit … are modified」）:
 *   - 両方が非シグナリングNaNなら常に destination（第1引数 uiA）側を返す。
 *   - SNaNのquiet化はフラクション最上位ビット（bit62）だけを立てる。整数部ビット
 *     （bit63、jビット）は変更しない。
 *
 * このため8086版のロジックを流用せず、dst優先規則をそのまま実装する。呼び出し側
 * （src/fpu/fpu_addsub.c、softfloat_target/extF80_mul.c・extF80_div.c）は必ず
 * 第1引数にdst（[881UM]の"Operation:"行のFPn側）を渡す。
 *
 * SNaN検出（invalidフラグ）はこの関数では行わない。EXC.SNANの検出・結線はPhase C
 * （design §6.1）で別途行う。
 */

#include <stdbool.h>
#include <stdint.h>
#include "platform.h"
#include "internals.h"
#include "specialize.h"
#include "softfloat.h"

struct uint128 softfloat_propagateNaNExtF80UI(uint_fast16_t uiA64, uint_fast64_t uiA0,
                                              uint_fast16_t uiB64, uint_fast64_t uiB0) {
    struct uint128 uiZ;
    bool isNaNA = isNaNExtF80UI(uiA64, uiA0);

    if (isNaNA) {
        uiZ.v64 = uiA64;
        uiZ.v0 = uiA0 | UINT64_C(0x4000000000000000);
    } else {
        uiZ.v64 = uiB64;
        uiZ.v0 = uiB0 | UINT64_C(0x4000000000000000);
    }
    return uiZ;
}
