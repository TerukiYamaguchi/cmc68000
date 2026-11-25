/*============================================================================

This C source file is part of the SoftFloat IEEE Floating-Point Arithmetic
Package, Release 3e, by John R. Hauser.

Copyright 2011, 2012, 2013, 2014 The Regents of the University of California.
All rights reserved.

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
 * （pin a0c6494）の source/extF80_mul.c。次の3点を修正:
 *
 *   1. `if ( ! expA ) expA = 1;` / `if ( ! expB ) expB = 1;` を削除。この補正は
 *      MotorolaのTable 3-3（e=0が正規化数[j=1]・非正規化数[j=0]の両方を表す）と
 *      食い違い、e=0の入力すべてで2倍の誤差になる（§3.2.1で確認済み）。削除して
 *      baselineをexpA/expB=0のまま再正規化に進ませる。
 *   2. `infArg:` 直前の `magBits = expB | sigB;`（Bが無限大側の分岐は`expA | sigA`）を
 *      `e!=0x7FFF かつ sig==0` によるゼロ判定に変更。元のコードは指数まで含めて
 *      ゼロを判定するため、指数が0でないゼロ（unnormalized zero）を非ゼロ扱いし、
 *      `0×∞` がNaN（invalid）ではなくInfになってしまう欠陥があった（§3.2.1第4回
 *      Opusレビュー N8）。
 *   3. `infArg:` の invalid（0×∞）分岐で使う既定NaNを、8086の`defaultNaNExtF80UI64/
 *      UI0`（x87既定NaN、符号1・仮数0xC000…）から、design §5.6・§9「生成される
 *      NaNは仮数全ビット1」（[881UM §3.2.5]）どおりの符号0・仮数全ビット1に変更
 *      （Phase B第三者レビュー B-B1。fpu_addsub.cの異符号無限大invalid分岐と揃える）。
 *
 * NaN伝播（`propagateNaN:`）は softfloat_propagateNaNExtF80UI() を呼ぶが、この
 * シンボルは softfloat_target/s_propagateNaNExtF80UI.c の68881固有dst優先規則版に
 * 差し替えられている。呼び出し側は必ず第1引数(a)にdst、第2引数(b)にsrcを渡す。
 */

#include <stdbool.h>
#include <stdint.h>
#include "platform.h"
#include "internals.h"
#include "specialize.h"
#include "softfloat.h"

extFloat80_t extF80_mul(extFloat80_t a, extFloat80_t b) {
    union {
        struct extFloat80M s;
        extFloat80_t f;
    } uA;
    uint_fast16_t uiA64;
    uint_fast64_t uiA0;
    bool signA;
    int_fast32_t expA;
    uint_fast64_t sigA;
    union {
        struct extFloat80M s;
        extFloat80_t f;
    } uB;
    uint_fast16_t uiB64;
    uint_fast64_t uiB0;
    bool signB;
    int_fast32_t expB;
    uint_fast64_t sigB;
    bool signZ;
    bool zeroArg;
    struct exp32_sig64 normExpSig;
    int_fast32_t expZ;
    struct uint128 sig128Z, uiZ;
    uint_fast16_t uiZ64;
    uint_fast64_t uiZ0;
    union {
        struct extFloat80M s;
        extFloat80_t f;
    } uZ;

    /*------------------------------------------------------------------------
     *------------------------------------------------------------------------*/
    uA.f = a;
    uiA64 = uA.s.signExp;
    uiA0 = uA.s.signif;
    signA = signExtF80UI64(uiA64);
    expA = expExtF80UI64(uiA64);
    sigA = uiA0;
    uB.f = b;
    uiB64 = uB.s.signExp;
    uiB0 = uB.s.signif;
    signB = signExtF80UI64(uiB64);
    expB = expExtF80UI64(uiB64);
    sigB = uiB0;
    signZ = signA ^ signB;
    /*------------------------------------------------------------------------
     *------------------------------------------------------------------------*/
    if (expA == 0x7FFF) {
        if ((sigA & UINT64_C(0x7FFFFFFFFFFFFFFF)) ||
            ((expB == 0x7FFF) && (sigB & UINT64_C(0x7FFFFFFFFFFFFFFF)))) {
            goto propagateNaN;
        }
        /* もう一方（B）がゼロか（指数を無視し sig==0 だけで判定。j=0の無限大を
         * 誤ってゼロ扱いしないよう expB!=0x7FFF も確認する）。 */
        zeroArg = (expB != 0x7FFF) && (sigB == 0);
        goto infArg;
    }
    if (expB == 0x7FFF) {
        if (sigB & UINT64_C(0x7FFFFFFFFFFFFFFF))
            goto propagateNaN;
        zeroArg = (expA != 0x7FFF) && (sigA == 0);
        goto infArg;
    }
    /*------------------------------------------------------------------------
     *------------------------------------------------------------------------*/
    if (!(sigA & UINT64_C(0x8000000000000000))) {
        if (!sigA)
            goto zero;
        normExpSig = softfloat_normSubnormalExtF80Sig(sigA);
        expA += normExpSig.exp;
        sigA = normExpSig.sig;
    }
    if (!(sigB & UINT64_C(0x8000000000000000))) {
        if (!sigB)
            goto zero;
        normExpSig = softfloat_normSubnormalExtF80Sig(sigB);
        expB += normExpSig.exp;
        sigB = normExpSig.sig;
    }
    /*------------------------------------------------------------------------
     *------------------------------------------------------------------------*/
    expZ = expA + expB - 0x3FFE;
    sig128Z = softfloat_mul64To128(sigA, sigB);
    if (sig128Z.v64 < UINT64_C(0x8000000000000000)) {
        --expZ;
        sig128Z = softfloat_add128(sig128Z.v64, sig128Z.v0, sig128Z.v64, sig128Z.v0);
    }
    return softfloat_roundPackToExtF80(signZ, expZ, sig128Z.v64, sig128Z.v0,
                                       extF80_roundingPrecision);
    /*------------------------------------------------------------------------
     *------------------------------------------------------------------------*/
propagateNaN:
    uiZ = softfloat_propagateNaNExtF80UI(uiA64, uiA0, uiB64, uiB0);
    uiZ64 = uiZ.v64;
    uiZ0 = uiZ.v0;
    goto uiZ;
    /*------------------------------------------------------------------------
     *------------------------------------------------------------------------*/
infArg:
    if (zeroArg) {
        softfloat_raiseFlags(softfloat_flag_invalid);
        /* 生成されるNaNは仮数全ビット1（[881UM §3.2.5]、design §5.6・§9）。
         * defaultNaNExtF80UI64/UI0（8086/specialize.hのx87既定NaN、符号1・仮数0xC000…）
         * は使わない（第三者レビュー B-B1）。 */
        uiZ64 = packToExtF80UI64(false, 0x7FFF);
        uiZ0 = UINT64_MAX;
    } else {
        uiZ64 = packToExtF80UI64(signZ, 0x7FFF);
        uiZ0 = UINT64_C(0x8000000000000000);
    }
    goto uiZ;
    /*------------------------------------------------------------------------
     *------------------------------------------------------------------------*/
zero:
    uiZ64 = packToExtF80UI64(signZ, 0);
    uiZ0 = 0;
uiZ:
    uZ.s.signExp = uiZ64;
    uZ.s.signif = uiZ0;
    return uZ.f;
}
