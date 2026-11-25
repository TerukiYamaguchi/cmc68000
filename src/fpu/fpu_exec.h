/*
 * fpu_exec.h — command の実行部（fpu_exec.c）の内部インタフェース。。公開しない。
 */
#pragma once
#include "fpu_model.h"

/* §8.1: EXC&ENABLE（OVFL 起因の INEX2 候補を含む）から選んだ例外のベクタ。無ければ0。
 * *exc_bit に選んだ EXC ビットを返す */
uint8_t fpu_select_exception(uint8_t exc, uint8_t enable, uint8_t *exc_bit);

void fpu_eval_predicate(mc68k_fpu_state_t *st, uint8_t predicate, bool *tf, bool *bsun);
/* 条件命令の開始。戻り値 true は BSUN trap（TAKE_PRE を返す） */
bool fpu_evaluate_condition(struct mc68k_fpu *f, uint8_t predicate, bool *tf);

unsigned fpu_format_words(mc68k_fpu_format_t fmt);
unsigned fpu_format_bytes(mc68k_fpu_format_t fmt);

/* dlg.buf の入力 word 列で演算・FMOVE・FMOVECR を実行する。保留算術例外は arith に積む */
void fpu_exec_operation(struct mc68k_fpu *f);
/* 外部宛て FMOVE の変換。出力 word 列を dlg.buf に置く */
void fpu_prepare_ext_output(struct mc68k_fpu *f);
void fpu_exec_control_in(struct mc68k_fpu *f);
void fpu_prepare_control_out(struct mc68k_fpu *f);
unsigned fpu_movem_registers(const struct fpu_dialog *d, uint8_t regs[8]);
void fpu_exec_movem_in(struct mc68k_fpu *f);
void fpu_prepare_movem_out(struct mc68k_fpu *f);
