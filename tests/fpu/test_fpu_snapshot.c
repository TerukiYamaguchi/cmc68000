/* test_fpu_snapshot.c — 外付け FPU snapshot の公開 API。 */
#include "mc68000/fpu.h"
#include <assert.h>

#define FMOVE_LONG_TO_FP0 0x4000u
#define SAVE_WORD_MAX 64u

int main(void) {
    mc68k_fpu_t *f81 = mc68k_fpu_create(MC68K_FPU_68881);
    mc68k_fpu_t *f82 = mc68k_fpu_create(MC68K_FPU_68882);
    assert(f81 && f82);
    mc68k_fpu_snapshot_t *snap = mc68k_fpu_snapshot_create(f81);
    assert(snap);
    assert(mc68k_fpu_snapshot_check(f81, snap));
    assert(!mc68k_fpu_snapshot_check(f82, snap));
    mc68k_fpu_snapshot_restore(f81, snap);
    mc68k_fpu_snapshot_destroy(snap);

    /* <ea>,FP0 の入力を受け付けた後、operand 待ちの状態を復元する。 */
    assert(mc68k_fpu_write_command_word(f81, FMOVE_LONG_TO_FP0));
    assert(mc68k_fpu_read_response(f81).kind == MC68K_FPU_RESPONSE_EA_TRANSFER);
    snap = mc68k_fpu_snapshot_create(f81);
    assert(snap);
    assert(mc68k_fpu_write_operand(f81, 42));
    mc68k_fpu_snapshot_restore(f81, snap);
    assert(mc68k_fpu_write_operand(f81, 7));
    mc68k_fpu_regs_t regs;
    mc68k_get_fpu_regs(f81, &regs);
    assert(regs.fp[0].sign_exp == 0x4001);
    assert(regs.fp[0].signif == UINT64_C(0xE000000000000000));
    mc68k_fpu_snapshot_destroy(snap);

    /* FSAVE 本文の読出し途中を復元し、同じ最初の語を再度返す。 */
    uint16_t format;
    size_t body_bytes;
    uint32_t body[SAVE_WORD_MAX];
    mc68k_fpu_begin_save(f81, &format, &body_bytes);
    assert(body_bytes > 0 && body_bytes / sizeof(body[0]) <= SAVE_WORD_MAX);
    snap = mc68k_fpu_snapshot_create(f81);
    assert(snap);
    assert(mc68k_fpu_read_operand(f81, &body[0]));
    mc68k_fpu_snapshot_restore(f81, snap);
    uint32_t replay;
    assert(mc68k_fpu_read_operand(f81, &replay));
    assert(replay == body[0]);
    mc68k_fpu_snapshot_destroy(snap);

    /* FRESTORE 本文の入力途中も restore 後に残りを受理する。 */
    mc68k_fpu_t *donor = mc68k_fpu_create(MC68K_FPU_68881);
    assert(donor);
    assert(mc68k_fpu_write_command_word(donor, FMOVE_LONG_TO_FP0));
    (void)mc68k_fpu_read_response(donor);
    assert(mc68k_fpu_write_operand(donor, 3));
    mc68k_fpu_begin_save(donor, &format, &body_bytes);
    size_t words = body_bytes / sizeof(body[0]);
    assert(words > 1 && words <= SAVE_WORD_MAX);
    for (size_t i = 0; i < words; ++i)
        assert(mc68k_fpu_read_operand(donor, &body[i]));
    uint16_t readback;
    mc68k_fpu_begin_restore(f81, format, &readback, &body_bytes);
    assert(readback == format);
    assert(mc68k_fpu_write_operand(f81, body[0]));
    snap = mc68k_fpu_snapshot_create(f81);
    assert(snap);
    assert(mc68k_fpu_write_operand(f81, body[1]));
    mc68k_fpu_snapshot_restore(f81, snap);
    for (size_t i = 1; i < words; ++i)
        assert(mc68k_fpu_write_operand(f81, body[i]));
    mc68k_fpu_snapshot_destroy(snap);
    mc68k_fpu_destroy(donor);
    mc68k_fpu_destroy(f81);
    mc68k_fpu_destroy(f82);
    return 0;
}
