#!/usr/bin/env python3
"""FPU-3（Issue #61）の変異確認。src/cpu/coproc.c などへ 1 箇所ずつ欠陥を入れ、test_fpcp_model と
test_fpcp_host のどちらかが落ちることを確かめる（落ちなければ SURVIVED）。

使い方: scripts/fpcp_mutation_check.py [build_dir]
各変異は (ファイル, 置換前, 置換後)。置換前は 1 箇所だけに当たること（--check-patterns は当たる数を表示する）。
macOS の make はタイムスタンプが1秒粒度なので、変異のたびにオブジェクトと実行ファイルを消してから再ビルドする。
"""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = Path(sys.argv[1]) if len(sys.argv) > 1 and not sys.argv[1].startswith("--") else ROOT / "build"
CHECK_ONLY = "--check-patterns" in sys.argv

COPROC = "src/cpu/coproc.c"
M68000 = "src/cpu/m68000.c"
SYSTEM = "src/cpu/ops_system.c"
DECODE = "src/cpu/decode.c"

# 等価と判定した変異（登録しない）:
#  - register_to_operand() の下位 size バイトへのマスク除去: port_write() が size で再度マスクする
#  - model_write() の `& 0x3F`（条件の下位 6 bit）の除去: fpu_proto.c が再度マスクする
#  - port_read() の size マスク除去: 呼び出し側（レジスタセレクトの popcount、format word、条件など）がすべて自前でマスクする
#  - model_read() の `words > CP_SAVE_BODY_MAX_WORDS` の分岐: 現行のモデルでは到達しない
#  - cp_fp_registers() の非 deferred 時の total 0→4: A7 の補正は mode 3/4 でしか使わない
#  - op_rte() の format 9 のフォルト時に `a7 += 20` する: group0 の処理が A7 を命令開始時へ戻すので、観測できる違いが無い
#    （A7 は同じ。A-N52）
#  - cp_opword_is_valid() の条件系 default の `ea_is_data_alterable` → `ea_is_alterable`: mode 1 は先に DBcc に分類される（A-N52、A-N66）
#  - coproc_resume() の cp_run_t を `.engaged = true` で始める: 再開は応答 CIR の読出しから始まり、成功すれば engaged=true に
#    なり、失敗すれば cp_cir_fault() が engaged に関係なく AB を書くので、どの経路でも結果が同じ（A-N45、A-N66）
#  - coproc_resume() の `OP_TYPE(opword) >= CP_TYPE_SAVE` を `== SAVE || == RESTORE` にする: type 6/7 は
#    cp_opword_is_valid() の default で false になる（A-N45、A-N66）
#  - 以下は Opus 10 回目レビューが 470 件の変異を当てて、実測で等価と判定した項目（A-N59。項目ごとの理由は Issue #61 のレビュー報告）:
#    CP_TYPE_COUNT_VALID を 7 にする、op_coproc() の `type >=` の項を外す、cp_opword_is_valid() の default を true にする、
#    cpScc の ea_is_data_alterable → ea_is_data、即値の `!to_coprocessor ||`（直前の `!to_coprocessor && !ea_is_alterable`
#    が即値を先に弾く死んだ項）、cp_abort_fline()・cp_format_error() の `engaged=false`、cp_cir_write() の `engaged=true`、
#    port_write() のマスク、即値 1 バイトの `& 0xFF`、popcount16() のマスク、model_read()・model_write() の default、
#    opword CIR の書込みを false にする、restore 時の `cp_save_left=0`、エンコーダの IA・MP の DR・FP レジスタ群の length、
#    cp_run_restore() の `cp_resolve_memory(…, 0)` の total、supervisor check の code を func にする、終了判定の
#    `!conditional &&`、転送前の commit、68020 で CYC_EXC_ILLEGAL と CP_EXCEPTION_CYCLES が同じ値
#  - RESP_FIXED_* のマスクの bit11・12（LOW_BYTE）、bit9・10・12・13（NULL）、bit8・9・12（MP レジスタ）を反転する変異（A-N75。
#    Opus 13 回目が 3 機種で実測）: 符号の判定（code 0x04/0x24/0x07/0x08/0x09、func 0x0C）でその位置が常に 0 に決まるため結果が変わらない
# 注記（A-N71・A-N74）: RESP_FIXED_NULL を 0x00FEu・0x00FDu にする変異（PF・TF を固定ビットに入れる方向）は test_primitive_fixed_bits 単独では生き残り、
#   0x00FEu（PF を含む）は PF=1 の正規の Null `$0802`、0x00FDu（TF を含む）は TF=1 の Null `$0801` を出す他の試験が検出する
#   （スクリプトは全試験を流すので検出になる）。
# 観測できるが、現行の挙動を意図して固定しない変異（等価ではない。登録しない）:
#  - 公称サイクル（CP_NOMINAL_CYCLES、CP_FORMAT_ERROR_CYCLES、CP_FAULT_CYCLES など）の ±1、cpu_dispatch_coproc_exception() の
#    HALT 時の戻り値（`return 8`・`return 12`）、take・violation 経路の `return nominal_cycles` の ±1、op_rte() の format 9 の
#    フォルト時の `return 8` の値（A-N58）: mc68k_execute() の戻り値として観測できる。一次資料に値が無い近似（設計書 §3.10、§5）で、
#    試験に書くと production の値の写しになるため固定しない。CP_EXCEPTION_CYCLES だけは、コプロセッサ無しの F-line のサイクル数が
#    従来の F-line と等しいことを 68020 で試験する（test_absent_fline_cycles_match_legacy）
#  - CP_REREAD_LIMIT の値と `++rereads` の位置、分岐先・cpScc の書込みフォルト後の cp_memory_ok の除去、命令アドレス CIR・ACK の
#    失敗後に DONE を返す変異、cp_run_general() の失敗時の戻り値（A-N59。観測できるのはサイクル数だけ）
#  - EA を評価していないときの mid フレームの EA 欄（[UM20 p.7-49] で undefined）、RTE の +8・+14・+16 の読出し順（A-N59）
#  - RTE の format 9 のサイクル 32/26: 既存コア（Phase 2c）の値で FPU-3 の対象外（A-N59）
#  - RTE の format 9 の追加欄の読出し失敗を halt にする: UM20 §6.1.2・§6.1.12 に近いが、現行（vector 2）を近似として固定する（A-B46、Issue #67）
#  - cpu_dispatch_coproc_exception() の MID 欄の書込みをフォルト時に飛ばす: 失敗した段で止める方が
#    design_mc68020_bus_fault_frame_fix §3.3 に近く、現行（書き続けて RAM を上書きしうる）を試験で固定しない。Issue #67 で扱う（A-N28）
#  - op_rte() の `cycles += coproc_resume(...)` の戻り値を捨てる: RTE の戻りサイクルから、再開した対話の公称値と、対話が
#    例外で終わるときの例外処理のサイクル（特権違反 CYC_EXC_PRIVILEGE、cpTRAPcc、take・violation、バスエラーなど）が
#    落ちる（例: ユーザモードの mid → RTE → SUPERVISOR_CHECK で 66→32）。サイクルは設計書 §3.10 の近似で、値を試験に
#    書くと production の写しになるため固定しない（A-N45、A-N51）
# 未検査（一次資料で順序が確定できない。backlog）:
#  - op_rte() の format 9 で flow_changed を立てない: T0 のハンドラからの RTE の変更フロートレースと再開した対話の順序（A-N41）
MUTATIONS = [
    (COPROC, "CIR_COMMAND = 0x0A,", "CIR_COMMAND = 0x0B,"),
    (COPROC, "CIR_CONDITION = 0x0E,", "CIR_CONDITION = 0x0C,"),
    (COPROC, "CIR_OPERAND = 0x10,", "CIR_OPERAND = 0x12,"),
    (COPROC, "#define CP_ID_SHIFT 13", "#define CP_ID_SHIFT 12"),
    (COPROC, "UINT32_C(0x00020000)", "UINT32_C(0x00010000)"),
    (COPROC, "cpu->bus.cpu_space_read(cpu->bus.ctx, MC68K_FC_CPU_SPACE,", "cpu->bus.cpu_space_read(cpu->bus.ctx, 5,"),
    (COPROC, "cpu->bus.cpu_space_write(cpu->bus.ctx, MC68K_FC_CPU_SPACE,", "cpu->bus.cpu_space_write(cpu->bus.ctx, 6,"),
    (COPROC, "m.addr + body - 4u * i", "m.addr + 4u + 4u * i"),
    (COPROC, "m.addr + FRAME_HEADER_BYTES + 4u * i", "m.addr + body - 4u * i"),
    (COPROC, "m.addr + (count - 1u - k) * len : m.addr + k * len", "m.addr + k * len : m.addr + k * len"),
    (COPROC, "*target = base + disp;", "*target = base + 2u + disp;"),
    (COPROC, "if (counter != 0xFFFFu)", "if (counter != 0u)"),
    (COPROC, "opmode == CP_TRAPCC_OPMODE_ONE_WORD   ? 1u", "opmode == CP_TRAPCC_OPMODE_ONE_WORD   ? 2u"),
    (COPROC, "        return cp_fline(r);\n    cp_abort(r);\n    r->engaged = false;\n    const mc68k_access_t access",
             "        return cp_fline(r);\n    r->engaged = false;\n    const mc68k_access_t access"),
    (COPROC, "return ((cpu->sr & SR_T1) && !(resp & NULL_PF)) ? DIALOG_CONTINUE : DIALOG_DONE;", "return DIALOG_DONE;"),
    (COPROC, "CP_CONTROL_EXCEPTION_ACK 0x0002u", "CP_CONTROL_EXCEPTION_ACK 0x0001u"),
    (COPROC, "(valid ? FRAME_RESERVED_NON_EMPTY : 0u)", "(valid ? 0u : FRAME_RESERVED_NON_EMPTY)"),
    (COPROC, "((unsigned)(r->valid_ea & 7u) << 8)", "((unsigned)(r->valid_ea & 7u) << 9)"),
    (COPROC, "return (reg == 7u && total == 1u) ? 2u : total;", "return total;"),
    (COPROC, "condition, true", "condition & 0x3Fu, true"),
    (COPROC, "if (cpu->sr & SR_S) /* cpGEN では bit15 によらず、応答 CIR を読み直す（[UM20 §7.4.5]） */\n            return DIALOG_CONTINUE;", "if (cpu->sr & SR_S)\n            return ca ? DIALOG_CONTINUE : DIALOG_DONE;"),
    (COPROC, "(ca || (!conditional && (cpu->sr & SR_T1))) ? DIALOG_CONTINUE", "ca ? DIALOG_CONTINUE"),
    (COPROC, "    if (!port_present(cpu, r.cpid)) {\n        cp_fline(&r);\n        return r.cycles;\n    }\n", ""),
    (COPROC, "    if ((type == CP_TYPE_SAVE || type == CP_TYPE_RESTORE) && !(cpu->sr & SR_S)) {\n        cp_privilege_violation(&r);\n        return r.cycles;\n    }\n    if (!port_present(cpu, r.cpid)) {", "    if (!port_present(cpu, r.cpid)) {"),
    (COPROC, "    cpu->instr_pc = instr_pc;\n    return cp_execute(&r, true);", "    return cp_execute(&r, true);"),
    (COPROC, "    if ((opword & 0xF000u) != 0xF000u || r.cpid == 0u", "    cpu->instr_pc = instr_pc;\n    if ((opword & 0xF000u) != 0xF000u || r.cpid == 0u"),
    (COPROC, "if (n == 4) {\n            v = cpu_read_long(cpu, addr + off);\n        } else if (n == 2) {\n            v = cpu_read_word(cpu, addr + off);", "if (n == 4) {\n            v = cpu_read_long(cpu, addr + off);\n        } else if (n == 2) {\n            v = (uint32_t)cpu_read_word(cpu, addr + off) << 16;"),
    (COPROC, "v |= cpu_read_byte(cpu, addr + off + 2);\n        }\n        if (!cp_memory_ok(r))\n            return false;\n        if (!cp_cir_write(r, CIR_OPERAND, (uint8_t)n, v, false))", "v |= cpu_read_byte(cpu, addr + off + 2);\n        }\n        if (!cp_memory_ok(r))\n            return false;\n        if (!cp_cir_write(r, CIR_OPERAND, CIR_SIZE_LONG, v, false))"),
    (COPROC, "v = (v << 16) | cpu_fetch_word(cpu);\n        if (!cp_memory_ok(r))\n            return false;\n        if (!cp_cir_write(r, CIR_OPERAND, (uint8_t)n, v, false))", "v = (v << 16) | cpu_fetch_word(cpu);\n        if (!cp_memory_ok(r))\n            return false;\n        if (!cp_cir_write(r, CIR_OPERAND, CIR_SIZE_LONG, v, false))"),
    (COPROC, "(uint32_t)(int32_t)(int16_t)operand : (uint32_t)(int32_t)(int8_t)operand;", "(uint32_t)(uint16_t)operand : (uint32_t)(int32_t)(int8_t)operand;"),
    (COPROC, "if (!cp_cir_read(r, CIR_OPERAND, (uint8_t)n, &v, false))", "if (!cp_cir_read(r, CIR_OPERAND, CIR_SIZE_LONG, &v, false))"),
    (COPROC, "    if (!deferred && !cp_resolve_memory(r, ea, 0, &m))\n        return false;\n", ""),
    (COPROC, "for (v &= 0xFFFFu; v; v &= v - 1u)", "for (v = (v >> 8) & 0xFFu; v; v &= v - 1u)"),
    (COPROC, "return (reg == 7u && total == 1u) ? 2u : total;", "return (reg == 7u && total == 1u && false) ? 2u : total;"),
    (COPROC, "            disp |= cpu_fetch_word(cpu);", "            disp = (uint32_t)(int32_t)(int16_t)cpu_fetch_word(cpu);"),
    (COPROC, "        if (!r->processing)\n            v |= NULL_PF;", "        if (r->processing)\n            v |= NULL_PF;"),
    (COPROC, "(r->data_register & 7u));", "0u);"),
    (COPROC, "mc68k_fpu_control(f, MC68K_FPU_CONTROL_EXCEPTION_ACK);", "mc68k_fpu_control(f, MC68K_FPU_CONTROL_ABORT);"),
    (COPROC, "cpu->cp_restore_readback = readback;", "cpu->cp_restore_readback = (uint16_t)val;"),
    (SYSTEM, "cp_instr_pc = cpu_read_long(cpu, cpu->a[7] + 8);", "cp_instr_pc = cpu_read_long(cpu, cpu->a[7] + 10);"),
    (COPROC, "    if (!resume) {\n        uint16_t condition = r->opword;", "    if (true) {\n        uint16_t condition = r->opword;"),
    (M68000, "cpu->exc_suppress_trace = kind != CP_EXC_POST;", "cpu->exc_suppress_trace = true;"),
    (COPROC, "val <<= 8u * (CIR_SIZE_LONG - size);", "val <<= 0u;"),
    (COPROC, "val <<= 8u * (CIR_SIZE_LONG - size);", "val <<= (size == 1u) ? 0u : 8u * (CIR_SIZE_LONG - size);"),
    (COPROC, "val <<= 8u * (CIR_SIZE_LONG - size);", "val <<= (size == 2u) ? 0u : 8u * (CIR_SIZE_LONG - size);"),
    (COPROC, "(ca || (!conditional && (cpu->sr & SR_T1))) ? DIALOG_CONTINUE", "(ca || !conditional) ? DIALOG_CONTINUE"),
    (COPROC, "        !port_present(cpu, r.cpid) || !cp_opword_is_valid(opword))\n        return 0;", "        !cp_opword_is_valid(opword))\n        return 0;"),
    (COPROC, "OP_TYPE(opword) >= CP_TYPE_SAVE ||", "OP_TYPE(opword) > CP_TYPE_RESTORE ||"),
    (COPROC, "if (type >= CP_TYPE_COUNT_VALID || !cp_opword_is_valid(opcode)) {", "if (type >= CP_TYPE_COUNT_VALID || (!cp_opword_is_valid(opcode) && (cpu->sr & SR_S))) {"),
    (COPROC, "const unsigned len = FORMAT_LENGTH(fw);\n    const bool coprocessor_valid", "const unsigned len = FORMAT_LENGTH(rb);\n    const bool coprocessor_valid"),
    (COPROC, "|| (memory_valid && (len & 3u))) {", "|| false) {"),
    (COPROC, "const unsigned body = coprocessor_valid ? len : 0u;", "const unsigned body = memory_valid ? len : 0u;"),
    (COPROC, "        default:\n            return ea_is_data_alterable((int)ea);", "        default:\n            return true;"),
    (COPROC, "    cpu->pc = target;\n    cpu->flow_changed = true;", "    cpu->pc = target;"),
    (COPROC, "            if (counter != 0xFFFFu)\n                cp_branch_to(cpu, target);\n        }", "            if (counter != 0xFFFFu)\n                cp_branch_to(cpu, target);\n            else\n                cpu->flow_changed = true;\n        }"),
    (COPROC, "        if (!tf)\n            return CP_NOMINAL_CYCLES;\n        return cpu_dispatch_exception(cpu, cpu->pc, VEC_TRAPCC", "        cpu->flow_changed = true;\n        if (!tf)\n            return CP_NOMINAL_CYCLES;\n        return cpu_dispatch_exception(cpu, cpu->pc, VEC_TRAPCC"),
    (COPROC, "        if (ca)\n            return DIALOG_CONTINUE;\n        if (conditional) {", "        if (ca && !conditional)\n            return DIALOG_CONTINUE;\n        if (conditional) {"),
    (COPROC, "        if (conditional) {\n            *tf = (resp & NULL_TF) != 0;", "        if (conditional && !(cpu->sr & SR_T1)) {\n            *tf = (resp & NULL_TF) != 0;"),
    (COPROC, "return mode >= 2u && ea_is_valid_encoding(ea);", "return mode >= 1u && ea_is_valid_encoding(ea);"),
    (COPROC, "        return ea_is_control(e);", "        return ea_is_control_alterable(e);"),
    (COPROC, "r.cpid == 0u || OP_TYPE(opword) >= CP_TYPE_SAVE", "OP_TYPE(opword) >= CP_TYPE_SAVE"),
    (COPROC, "if ((opword & 0xF000u) != 0xF000u ||", "if ("),
    (COPROC, "!port_present(cpu, r.cpid) || !cp_opword_is_valid(opword))", "!port_present(cpu, r.cpid))"),
    (COPROC, "if (ca) { /* take 系に CA は無い */", "if (false) { /* take 系に CA は無い */"),
    (COPROC, "return (reg == 7u && total == 1u) ? 2u : total;", "return reg == 7u ? ((total + 1u) & ~1u) : total;"),
    (COPROC, "if (len & 1u)\n        return cp_violation(r);\n    /* メモリ→コプロセッサは", "if (len & 3u)\n        return cp_violation(r);\n    /* メモリ→コプロセッサは"),
    (COPROC, "    case CIR_CONTROL:\n        cpu->cp_save_left = 0;\n", "    case CIR_CONTROL:\n"),
    (COPROC, "{.fc = MC68K_FC_CPU_SPACE, .read = true, .size = size};\n    bool ok;\n\n    *out = 0;", "{.fc = MC68K_FC_CPU_SPACE, .read = true, .size = 4};\n    bool ok;\n\n    *out = 0;"),
    (COPROC, "{.fc = MC68K_FC_CPU_SPACE, .read = false, .size = size};\n    bool ok;\n\n    if (size", "{.fc = MC68K_FC_CPU_SPACE, .read = true, .size = size};\n    bool ok;\n\n    if (size"),
    (COPROC, "    if (!cp_resolve_memory(r, ea, len, &m))\n        return false;\n    if (to_coprocessor ?", "    if (!cp_resolve_memory(r, ea, len, &m))\n        return false;\n    cp_commit_memory(cpu, &m);\n    if (to_coprocessor ?"),
    (M68000, "    cpu->a[7] -= frame_bytes;\n    uint32_t base = cpu->a[7];\n    cpu_write_word(cpu, base, saved_sr);", "    cpu->a[7] -= frame_bytes;\n    uint32_t base = cpu->a[7];\n    cpu_write_word(cpu, base, cpu->sr);"),
    (COPROC, "cpu_signal_bus_error(cpu, cir_address(r->cpid, cir), read);", "cpu_signal_bus_error(cpu, cir_address(r->cpid, cir), !read);"),
    (M68000, "    cpu->exc_suppress_trace = kind != CP_EXC_POST;\n    cpu->last_exception_vector = vector;", "    cpu->exc_suppress_trace = kind != CP_EXC_POST;"),
    (DECODE, "    if (model >= MC68K_MODEL_68020)\n        register_coproc_ops(table);", "    if (model >= MC68K_MODEL_68010)\n        register_coproc_ops(table);"),
    (COPROC, "m.delta = (int32_t)(FRAME_HEADER_BYTES + 4u * (body / 4u));", "m.delta = (int32_t)(FRAME_HEADER_BYTES + body);"),
    (COPROC, "return mode >= 2u && ea_is_valid_encoding(ea);", "return mode >= 2u;"),
    (COPROC, "    default:\n        return ea_is_valid_encoding(ea);", "    default:\n        return true;"),
    (COPROC, "    if (cpid == CP_ID_FPU && cpu->fpu)\n        return true;", "    if (cpu->fpu)\n        return true;"),
    (COPROC, "    if (cpid == CP_ID_FPU && cpu->fpu) {\n        ok = model_read", "    if (cpu->fpu) {\n        ok = model_read"),
    (M68000, "& ~(SR_T1 | SR_T0)));\n    cpu->a[7] -= frame_bytes;", "& ~SR_T1));\n    cpu->a[7] -= frame_bytes;"),
    (M68000, "& ~(SR_T1 | SR_T0)));\n    cpu->a[7] -= frame_bytes;", "& ~SR_T0));\n    cpu->a[7] -= frame_bytes;"),
    (M68000, "    uint32_t new_pc = cpu_read_long(cpu, cpu_vector_addr(cpu, vector));\n    if (cpu->exec_state != CPU_NORMAL) {\n        cpu->exec_state = CPU_HALTED;\n        return 12;\n    }\n    cpu->pc = new_pc;\n    /* PRE と MID", "    uint32_t new_pc = cpu_read_long(cpu, (uint32_t)vector * 4u);\n    if (cpu->exec_state != CPU_NORMAL) {\n        cpu->exec_state = CPU_HALTED;\n        return 12;\n    }\n    cpu->pc = new_pc;\n    /* PRE と MID"),
    (COPROC, "cpu_is_68030_family(cpu) ? CYC(cpu, CYC_EXC_ILLEGAL) : CP_EXCEPTION_CYCLES", "cpu_is_68030_family(cpu) ? CYC(cpu, CYC_EXC_ILLEGAL) + 1 : CP_EXCEPTION_CYCLES"),
    (COPROC, "const uint8_t value = tf ? 0xFFu : 0x00u;", "const uint8_t value = tf ? 0x00u : 0xFFu;"),
    (COPROC, "*out = cpu->cp_save_body[--cpu->cp_save_left];", "*out = cpu->cp_save_body[(cpu->cp_save_left--, 0u)];"),
    (COPROC, "(resp & RESP_PC) &&", "(resp & RESP_PC) && false &&"),
    (COPROC, "if (!ea_in_valid_class(valid, ea))\n        return cp_abort_fline(r);", "if (false)\n        return cp_abort_fline(r);"),
    (COPROC, "if (!to_coprocessor && !ea_is_alterable((int)ea))", "if (false)"),
    (COPROC, "if (len & 1u)\n        return cp_violation(r);\n    /* メモリ→", "if (false)\n        return cp_violation(r);\n    /* メモリ→"),
    (COPROC, "(valid && (len & 3u))) {\n        cp_format_error(r);\n        return r->cycles;\n    }\n\n    const unsigned body = valid ? len : 0u;\n    cp_mem_t m;", "false) {\n        cp_format_error(r);\n        return r->cycles;\n    }\n\n    const unsigned body = valid ? len : 0u;\n    cp_mem_t m;"),
    (COPROC, "    if (code == FORMAT_NOT_READY) {\n        cp_restart(r);\n        return r->cycles;\n    }", "    if (code == FORMAT_NOT_READY) {\n        return CP_NOMINAL_CYCLES;\n    }"),
    (COPROC, "    cpu->pc = cpu->instr_pc;\n    cpu->exc_suppress_trace = true;", "    cpu->exc_suppress_trace = true;"),
    (COPROC, "if ((type == CP_TYPE_SAVE || type == CP_TYPE_RESTORE) && !(cpu->sr & SR_S))", "if (false)"),
    (COPROC, "cpu->a[reg] - step;", "cpu->a[reg];"),
    (COPROC, "    if (r->engaged)\n        cp_abort(r);\n    r->cycles = CP_FAULT_CYCLES;", "    cp_abort(r);\n    r->cycles = CP_FAULT_CYCLES;"),
    (COPROC, "    if (r->engaged)\n        cp_abort(r);\n    r->cycles = CP_FAULT_CYCLES;", "    r->cycles = CP_FAULT_CYCLES;"),
    (COPROC, "const bool valid = code >= FORMAT_VALID_MIN;\n    if ((code != FORMAT_EMPTY && !valid) || (valid && (len & 3u))) {\n        cp_format_error(r);\n        return r->cycles;\n    }\n\n    const unsigned body = valid ? len : 0u;\n    cp_mem_t m;", "const bool valid = code > FORMAT_VALID_MIN;\n    if ((code != FORMAT_EMPTY && !valid) || (valid && (len & 3u))) {\n        cp_format_error(r);\n        return r->cycles;\n    }\n\n    const unsigned body = valid ? len : 0u;\n    cp_mem_t m;"),
    (COPROC, "cpu_note_access(cpu, &access);\n    cpu_signal_bus_error(cpu, cir_address(r->cpid, cir), read);", "cpu_signal_bus_error(cpu, cir_address(r->cpid, cir), read);"),
    (COPROC, "cpu->exc_suppress_trace = true;\n    r->cycles = CP_NOMINAL_CYCLES;", "r->cycles = CP_NOMINAL_CYCLES;"),
    (M68000, "cpu_write_word(cpu, base + 14, opword);", "cpu_write_word(cpu, base + 12, opword);"),
    (M68000, "const uint16_t format = kind == CP_EXC_PRE ? 0x0000u : kind == CP_EXC_POST ? 0x2000u : 0x9000u;", "const uint16_t format = kind == CP_EXC_PRE ? 0x0000u : kind == CP_EXC_POST ? 0x2000u : 0xA000u;"),
    (M68000, "kind == CP_EXC_PRE ? cpu->instr_pc : scan_pc", "kind == CP_EXC_PRE ? scan_pc : scan_pc"),
    (M68000, "cpu_write_long(cpu, base + 16, ea);", "cpu_write_long(cpu, base + 16, 0);"),
    (SYSTEM, "cp_opword = cpu_read_word(cpu, cpu->a[7] + 14);", "cp_opword = cpu_read_word(cpu, cpu->a[7] + 12);"),
    (SYSTEM, "cycles += coproc_resume(cpu, cp_opword, cp_instr_pc, cp_ea);", "cycles += 0;"),
    # A-B37: +16（EA）読出しの失敗後の検査（+8 の読出しの後ではなく、3 つの読出しの後にある）
    (SYSTEM, "cp_ea = cpu_read_long(cpu, cpu->a[7] + 16);\n                if (cpu->exec_state != CPU_NORMAL)\n                    return 8;",
             "cp_ea = cpu_read_long(cpu, cpu->a[7] + 16);"),
    # A-B38: mid の例外もトレースを抑止する（post だけが追随する）
    (M68000, "cpu->exc_suppress_trace = kind != CP_EXC_POST;\n    cpu->last_exception_vector = vector;", "cpu->exc_suppress_trace = kind == CP_EXC_PRE;\n    cpu->last_exception_vector = vector;"),
    # A-B39: 読み返しの予約コード（$03..$0F）も format error
    (COPROC, "if ((code != FORMAT_EMPTY && !coprocessor_valid) || (memory_valid && (len & 3u))) {", "if ((code == FORMAT_INVALID) || (memory_valid && (len & 3u))) {"),
    # A-B40: リストア CIR の読み返しは最初のアクセスではない（失敗は F-line でなくバスエラー）
    (COPROC, "if (!cp_cir_read(r, CIR_RESTORE, CIR_SIZE_WORD, &rb, false))", "if (!cp_cir_read(r, CIR_RESTORE, CIR_SIZE_WORD, &rb, true))"),
    # A-B41: フレーム書込み失敗後の HALTED 化（除去すると、停止後にベクタ読出しがホストに出る）
    (M68000, "    if (cpu->exec_state != CPU_NORMAL) {\n        cpu->exec_state = CPU_HALTED;\n        return 8;\n    }\n    uint32_t new_pc = cpu_read_long(cpu, cpu_vector_addr(cpu, vector));\n    if (cpu->exec_state != CPU_NORMAL) {\n        cpu->exec_state = CPU_HALTED;\n        return 12;\n    }\n    cpu->pc = new_pc;\n    /* PRE と MID",
             "    uint32_t new_pc = cpu_read_long(cpu, cpu_vector_addr(cpu, vector));\n    if (cpu->exec_state != CPU_NORMAL) {\n        cpu->exec_state = CPU_HALTED;\n        return 12;\n    }\n    cpu->pc = new_pc;\n    /* PRE と MID"),
    # A-N25: ベクタ読出し失敗後の HALTED 化
    (M68000, "    if (cpu->exec_state != CPU_NORMAL) {\n        cpu->exec_state = CPU_HALTED;\n        return 12;\n    }\n    cpu->pc = new_pc;\n    /* PRE と MID",
             "    cpu->pc = new_pc;\n    /* PRE と MID"),
    # A-B42: ユーザモードで取る例外は、S をセットしてからスーパーバイザスタックにフレームを積む
    (M68000, "    cpu_set_sr(cpu, (uint16_t)((saved_sr | SR_S) & ~(SR_T1 | SR_T0)));\n    cpu->a[7] -= frame_bytes;",
             "    cpu->a[7] -= frame_bytes;\n    cpu_set_sr(cpu, (uint16_t)((saved_sr | SR_S) & ~(SR_T1 | SR_T0)));"),
    # A-B45: 再開するのは上位 4 ビットがすべて 1（F-line）の opword だけ。1 ビットずつ判定から外す
    (COPROC, "    if ((opword & 0xF000u) != 0xF000u || r.cpid == 0u", "    if ((opword & 0xE000u) != 0xE000u || r.cpid == 0u"),
    (COPROC, "    if ((opword & 0xF000u) != 0xF000u || r.cpid == 0u", "    if ((opword & 0xD000u) != 0xD000u || r.cpid == 0u"),
    (COPROC, "    if ((opword & 0xF000u) != 0xF000u || r.cpid == 0u", "    if ((opword & 0xB000u) != 0xB000u || r.cpid == 0u"),
    (COPROC, "    if ((opword & 0xF000u) != 0xF000u || r.cpid == 0u", "    if ((opword & 0x7000u) != 0x7000u || r.cpid == 0u"),
    # A-B44: RTE の再開は復元した SR（特権状態）で行う。resume を cpu_set_sr の前に移す
    (SYSTEM, '            cpu_set_sr(cpu, new_sr);\n            cpu->flow_changed = true;\n            int cycles = 20;\n            if (cpu_is_68020(cpu))\n                cycles = format == 9 ? 32 : format == 10 ? 43 : format == 11 ? 92 : 21;\n            else if (cpu_is_68030_family(cpu))\n                cycles = format == 0 || format == 2 ? 18 : format == 1 ? 12\n                         : format == 9 ? 26 : format == 10 ? 36 : 76;\n            if (format == 9)\n                cycles += coproc_resume(cpu, cp_opword, cp_instr_pc, cp_ea);',
             '            int resumed = format == 9 ? coproc_resume(cpu, cp_opword, cp_instr_pc, cp_ea) : 0;\n            cpu_set_sr(cpu, new_sr);\n            cpu->flow_changed = true;\n            int cycles = 20 + resumed;\n            if (cpu_is_68020(cpu))\n                cycles = format == 9 ? 32 : format == 10 ? 43 : format == 11 ? 92 : 21;\n            else if (cpu_is_68030_family(cpu))\n                cycles = format == 0 || format == 2 ? 18 : format == 1 ? 12\n                         : format == 9 ? 26 : format == 10 ? 36 : 76;\n            if (format == 9)\n                cycles += 0;'),
    # A-B47: 再開する側の cpID の条件（F-line 判定の上位集合、port_present の cpID）
    (COPROC, "    if ((opword & 0xF000u) != 0xF000u || r.cpid == 0u", "    if ((opword & 0xF200u) != 0xF200u || r.cpid == 0u"),
    (COPROC, "        !port_present(cpu, r.cpid) || !cp_opword_is_valid(opword))\n        return 0;\n    /* 再開する対話", "        !port_present(cpu, 1u) || !cp_opword_is_valid(opword))\n        return 0;\n    /* 再開する対話"),
    # A-B48: 再開する側の type の上限（cpBcc.L）
    (COPROC, "r.cpid == 0u || OP_TYPE(opword) >= CP_TYPE_SAVE ||\n        !port_present(cpu, r.cpid) || !cp_opword_is_valid(opword))\n        return 0;\n    /* 再開する対話", "r.cpid == 0u || OP_TYPE(opword) >= CP_TYPE_BCC_LONG ||\n        !port_present(cpu, r.cpid) || !cp_opword_is_valid(opword))\n        return 0;\n    /* 再開する対話"),
    # A-B49: フレームの long 欄（PC、instruction address、EA）の上位ワード
    (SYSTEM, "cp_instr_pc = cpu_read_long(cpu, cpu->a[7] + 8);", "cp_instr_pc = cpu_read_word(cpu, cpu->a[7] + 10);"),
    (SYSTEM, "cp_ea = cpu_read_long(cpu, cpu->a[7] + 16);\n                if (cpu->exec_state != CPU_NORMAL)\n                    return 8;\n            }\n            int bytes", "cp_ea = cpu_read_long(cpu, cpu->a[7] + 16) & 0xFFFFu;\n                if (cpu->exec_state != CPU_NORMAL)\n                    return 8;\n            }\n            int bytes"),
    (M68000, "cpu_write_long(cpu, base + 16, ea);", "cpu_write_word(cpu, base + 18, (uint16_t)ea);"),
    (M68000, "        cpu_write_long(cpu, base + 8, cpu->instr_pc);\n        if (kind == CP_EXC_MID) {", "        cpu_write_word(cpu, base + 10, (uint16_t)cpu->instr_pc);\n        if (kind == CP_EXC_MID) {"),
    (M68000, "cpu_write_long(cpu, base + 2, kind == CP_EXC_PRE ? cpu->instr_pc : scan_pc);", "cpu_write_word(cpu, base + 4, (uint16_t)(kind == CP_EXC_PRE ? cpu->instr_pc : scan_pc));"),
    # A-B50: フレームの外（上）を書かない
    (M68000, "    if (kind != CP_EXC_PRE) {\n        cpu_write_long(cpu, base + 8,", "    if (true) {\n        cpu_write_long(cpu, base + 8,"),
    (M68000, "        if (kind == CP_EXC_MID) {\n            cpu_write_word(cpu, base + 12, 0);", "        if (kind != CP_EXC_PRE) {\n            cpu_write_word(cpu, base + 12, 0);"),
    # A-B51: take のベクタ番号は 8 bit
    (COPROC, "const uint16_t vector = (uint16_t)RESP_LENGTH(resp);\n        cp_exc_kind_t kind", "const uint16_t vector = (uint16_t)(resp & 0x7Fu);\n        cp_exc_kind_t kind"),
    (M68000, "(uint16_t)(format | (vector * 4u))", "(uint16_t)(format | ((vector * 4u) & 0xFFu))"),
    (M68000, "(uint16_t)(format | (vector * 4u))", "(uint16_t)(format | ((vector & 0x3Fu) * 4u))"),
    (M68000, "    uint32_t new_pc = cpu_read_long(cpu, cpu_vector_addr(cpu, vector));\n    if (cpu->exec_state != CPU_NORMAL) {\n        cpu->exec_state = CPU_HALTED;\n        return 12;\n    }\n    cpu->pc = new_pc;\n    /* PRE と MID", "    uint32_t new_pc = cpu_read_long(cpu, cpu_vector_addr(cpu, vector & 0x3Fu));\n    if (cpu->exec_state != CPU_NORMAL) {\n        cpu->exec_state = CPU_HALTED;\n        return 12;\n    }\n    cpu->pc = new_pc;\n    /* PRE と MID"),

    # A-B52: コプロセッサの存在は read と write の両方
    (COPROC, "return cpu->bus.cpu_space_read && cpu->bus.cpu_space_write;", "return cpu->bus.cpu_space_write != NULL;"),
    # A-B53: 最初の CIR アクセスより後の CIR フォルトは vector 2（initial を true にしない）
    (COPROC, "            return false;\n        if (!cp_cir_write(r, CIR_OPERAND, (uint8_t)n, v, false))\n            return false;\n    }\n    return true;\n}\n\n/* オペランド CIR → メモリ。",
             "            return false;\n        if (!cp_cir_write(r, CIR_OPERAND, (uint8_t)n, v, true))\n            return false;\n    }\n    return true;\n}\n\n/* オペランド CIR → メモリ。"),
    (COPROC, "if (!cp_cir_read(r, CIR_OPERAND, (uint8_t)n, &v, false))", "if (!cp_cir_read(r, CIR_OPERAND, (uint8_t)n, &v, true))"),
    (COPROC, "            return false;\n        if (!cp_cir_write(r, CIR_OPERAND, (uint8_t)n, v, false))\n            return false;\n    }\n    return true;\n}\n\n/* Dn/An との転送",
             "            return false;\n        if (!cp_cir_write(r, CIR_OPERAND, (uint8_t)n, v, true))\n            return false;\n    }\n    return true;\n}\n\n/* Dn/An との転送"),
    (COPROC, "register_to_operand(*rg, len),\n                                false);", "register_to_operand(*rg, len),\n                                true);"),
    (COPROC, "if (!cp_cir_read(r, CIR_OPERAND, (uint8_t)len, &v, false))", "if (!cp_cir_read(r, CIR_OPERAND, (uint8_t)len, &v, true))"),
    (COPROC, "return cp_cir_write(r, CIR_OPERAND, 1, v, false);", "return cp_cir_write(r, CIR_OPERAND, 1, v, true);"),
    (COPROC, "CIR_REGISTER_SELECT, CIR_SIZE_WORD, &mask, false)", "CIR_REGISTER_SELECT, CIR_SIZE_WORD, &mask, true)"),
    (COPROC, "return cp_cir_write(r, CIR_OPERAND, CIR_SIZE_LONG, *rg, false);", "return cp_cir_write(r, CIR_OPERAND, CIR_SIZE_LONG, *rg, true);"),
    (COPROC, "*rg, false);\n    if (!cp_cir_read(r, CIR_OPERAND, CIR_SIZE_LONG, &v, false))", "*rg, false);\n    if (!cp_cir_read(r, CIR_OPERAND, CIR_SIZE_LONG, &v, true))"),
    (COPROC, "CIR_INSTRUCTION_ADDRESS, CIR_SIZE_LONG, cpu->instr_pc, false))", "CIR_INSTRUCTION_ADDRESS, CIR_SIZE_LONG, cpu->instr_pc, true))"),
    (COPROC, "CP_CONTROL_EXCEPTION_ACK, false))", "CP_CONTROL_EXCEPTION_ACK, true))"),
    (COPROC, "CP_CONTROL_EXCEPTION_ACK, false))\n            return DIALOG_STOP;", "CP_CONTROL_EXCEPTION_ACK, false)) {\n        }"),
    (COPROC, "CIR_OPERATION_WORD, CIR_SIZE_WORD, r->opword, false)", "CIR_OPERATION_WORD, CIR_SIZE_WORD, r->opword, true)"),
    (COPROC, "        uint32_t v;\n\n        if (!cp_cir_read(r, CIR_OPERAND, CIR_SIZE_LONG, &v, false))", "        uint32_t v;\n\n        if (!cp_cir_read(r, CIR_OPERAND, CIR_SIZE_LONG, &v, true))"),
    (COPROC, "if (!cp_cir_write(r, CIR_OPERAND, CIR_SIZE_LONG, v, false))", "if (!cp_cir_write(r, CIR_OPERAND, CIR_SIZE_LONG, v, true))"),
    # A-B54: CIR バスエラーのフレームの SSW と fault address
    (COPROC, "const mc68k_access_t access = {.fc = MC68K_FC_CPU_SPACE, .read = read, .size = size};", "const mc68k_access_t access = {.fc = MC68K_FC_CPU_SPACE, .read = !read, .size = size};"),
    (COPROC, "const mc68k_access_t access = {.fc = MC68K_FC_CPU_SPACE, .read = read, .size = size};", "const mc68k_access_t access = {.fc = MC68K_FC_CPU_SPACE, .read = read, .size = 2};"),
    (COPROC, "cpu_signal_bus_error(cpu, cir_address(r->cpid, cir), read);", "cpu_signal_bus_error(cpu, cir_address(1u, cir), read);"),
    # A-B55: メモリのフォルト後の検査
    (COPROC, "const uint16_t command = cpu_fetch_word(cpu);\n        if (!cp_memory_ok(r))\n            return r->cycles;\n", "const uint16_t command = cpu_fetch_word(cpu);\n"),
    (COPROC, "condition = cpu_fetch_word(cpu);\n            if (!cp_memory_ok(r))\n                return r->cycles;\n", "condition = cpu_fetch_word(cpu);\n"),
    (COPROC, "v = (v << 16) | cpu_fetch_word(cpu);\n        if (!cp_memory_ok(r))\n            return false;\n", "v = (v << 16) | cpu_fetch_word(cpu);\n"),
    (COPROC, "v = cpu_fetch_word(cpu) & 0xFFu;\n            if (!cp_memory_ok(r))\n                return false;\n", "v = cpu_fetch_word(cpu) & 0xFFu;\n"),
    (COPROC, "(valid ? FRAME_RESERVED_NON_EMPTY : 0u));\n    if (!cp_memory_ok(r))\n        return r->cycles;\n", "(valid ? FRAME_RESERVED_NON_EMPTY : 0u));\n"),
    (COPROC, "m.addr + FRAME_HEADER_BYTES + 4u * i);\n\n        if (!cp_memory_ok(r))\n            return r->cycles;\n", "m.addr + FRAME_HEADER_BYTES + 4u * i);\n\n"),
    (COPROC, "if (cpu->exec_state == CPU_NORMAL)\n                v |= cpu_read_byte(cpu, addr + off + 2);", "if (true)\n                v |= cpu_read_byte(cpu, addr + off + 2);"),
    (COPROC, "if (cpu->exec_state == CPU_NORMAL)\n                cpu_write_byte(cpu, addr + off + 2, (uint8_t)v);", "if (true)\n                cpu_write_byte(cpu, addr + off + 2, (uint8_t)v);"),
    (COPROC, "if (n == 4 && cpu->exec_state == CPU_NORMAL)", "if (n == 4)"),
    (COPROC, "if (cpu->exec_state == CPU_NORMAL)\n            disp |= cpu_fetch_word(cpu);", "if (true)\n            disp |= cpu_fetch_word(cpu);"),
    (COPROC, "v = cpu_read_byte(cpu, addr + off);\n        } else {", "v = (uint32_t)(cpu_read_word(cpu, addr + off) >> 8);\n        } else {"),
    (COPROC, "cpu_write_byte(cpu, addr + off, (uint8_t)v);\n        } else {", "cpu_write_word(cpu, addr + off, (uint16_t)(v << 8));\n        } else {"),
    # A-B56: protocol violation のフレームの EA 欄
    (COPROC, "cpu->pc, r->opword, r->ea,\n                                              CP_EXCEPTION_CYCLES);\n    return false;", "cpu->pc, r->opword, 0,\n                                              CP_EXCEPTION_CYCLES);\n    return false;"),
    # A-B57: コプロセッサ例外後の SR は M・IPL・CCR を保つ
    (M68000, "cpu_set_sr(cpu, (uint16_t)((saved_sr | SR_S) & ~(SR_T1 | SR_T0)));\n    cpu->a[7] -= frame_bytes;", "cpu_set_sr(cpu, (uint16_t)((saved_sr | SR_S) & ~(SR_T1 | SR_T0 | 0x1000u)));\n    cpu->a[7] -= frame_bytes;"),
    (M68000, "cpu_set_sr(cpu, (uint16_t)((saved_sr | SR_S) & ~(SR_T1 | SR_T0)));\n    cpu->a[7] -= frame_bytes;", "cpu_set_sr(cpu, (uint16_t)((saved_sr | SR_S | 0x0700u) & ~(SR_T1 | SR_T0)));\n    cpu->a[7] -= frame_bytes;"),
    (M68000, "cpu_set_sr(cpu, (uint16_t)((saved_sr | SR_S) & ~(SR_T1 | SR_T0)));\n    cpu->a[7] -= frame_bytes;", "cpu_set_sr(cpu, (uint16_t)((saved_sr | SR_S) & ~(SR_T1 | SR_T0 | 0x001Fu)));\n    cpu->a[7] -= frame_bytes;"),
    # A-B58: トレース保留は T1 だけ
    (COPROC, "return ((cpu->sr & SR_T1) && !(resp & NULL_PF)) ? DIALOG_CONTINUE : DIALOG_DONE;", "return ((cpu->sr & (SR_T1 | SR_T0)) && !(resp & NULL_PF)) ? DIALOG_CONTINUE : DIALOG_DONE;"),
    (COPROC, "(ca || (!conditional && (cpu->sr & SR_T1))) ? DIALOG_CONTINUE", "(ca || (!conditional && (cpu->sr & (SR_T1 | SR_T0)))) ? DIALOG_CONTINUE"),
    # A-B59: 図で固定されたビットが違う primitive は未定義
    (COPROC, "    if (code == PRIM_BUSY) {\n        if (!ca || (resp & RESP_FIXED_LOW_BYTE)) {", "    if (code == PRIM_BUSY) {\n        if ((resp & RESP_FIXED_LOW_BYTE)) {"),
    (COPROC, "code == PRIM_TAKE_PRE || code == PRIM_TAKE_MID || code == PRIM_TAKE_POST", "func == PRIM_TAKE_PRE || func == PRIM_TAKE_MID || func == PRIM_TAKE_POST"),
    (COPROC, "if (code == PRIM_TRANSFER_OPERATION_WORD) {", "if (func == PRIM_TRANSFER_OPERATION_WORD) {"),
    (COPROC, "} else if (code == PRIM_TRANSFER_INSTRUCTION_STREAM) {", "} else if (func == PRIM_TRANSFER_INSTRUCTION_STREAM) {"),
    (COPROC, "if (code == PRIM_NULL || code == PRIM_NULL_IA) {", "if (func == PRIM_NULL || func == PRIM_NULL_IA) {"),
    (COPROC, "#define RESP_FUNC(r) (((r) >> 8) & 0x1Fu)", "#define RESP_FUNC(r) (((r) >> 8) & 0x0Fu)"),
    # A-B64: 下位の固定ビット
    (COPROC, "#define RESP_FIXED_LOW_BYTE 0x00FFu", "#define RESP_FIXED_LOW_BYTE 0x00FEu"),
    (COPROC, "#define RESP_FIXED_LOW_BYTE 0x00FFu", "#define RESP_FIXED_LOW_BYTE 0x007Fu"),
    (COPROC, "#define RESP_FIXED_NULL 0x00FCu", "#define RESP_FIXED_NULL 0x00F8u"),
    (COPROC, "#define RESP_FIXED_NULL 0x00FCu", "#define RESP_FIXED_NULL 0x007Cu"),
    (COPROC, "#define RESP_FIXED_NULL 0x00FCu", "#define RESP_FIXED_NULL 0x00FEu"),
    (COPROC, "#define RESP_FIXED_MP_REGISTER 0x00F0u", "#define RESP_FIXED_MP_REGISTER 0x00E0u"),
    (COPROC, "#define RESP_FIXED_MP_REGISTER 0x00F0u", "#define RESP_FIXED_MP_REGISTER 0x0070u"),
    (COPROC, "if (!ca || (resp & RESP_FIXED_LOW_BYTE)) {", "if (!ca) {"),
    # A-B65: マスクの中間のビットを落とす
    (COPROC, "#define RESP_FIXED_LOW_BYTE 0x00FFu", "#define RESP_FIXED_LOW_BYTE 0x00FDu"),
    (COPROC, "#define RESP_FIXED_LOW_BYTE 0x00FFu", "#define RESP_FIXED_LOW_BYTE 0x00FBu"),
    (COPROC, "#define RESP_FIXED_LOW_BYTE 0x00FFu", "#define RESP_FIXED_LOW_BYTE 0x00F7u"),
    (COPROC, "#define RESP_FIXED_LOW_BYTE 0x00FFu", "#define RESP_FIXED_LOW_BYTE 0x00EFu"),
    (COPROC, "#define RESP_FIXED_LOW_BYTE 0x00FFu", "#define RESP_FIXED_LOW_BYTE 0x00DFu"),
    (COPROC, "#define RESP_FIXED_LOW_BYTE 0x00FFu", "#define RESP_FIXED_LOW_BYTE 0x00BFu"),
    (COPROC, "#define RESP_FIXED_NULL 0x00FCu", "#define RESP_FIXED_NULL 0x00F4u"),
    (COPROC, "#define RESP_FIXED_NULL 0x00FCu", "#define RESP_FIXED_NULL 0x00ECu"),
    (COPROC, "#define RESP_FIXED_NULL 0x00FCu", "#define RESP_FIXED_NULL 0x00DCu"),
    (COPROC, "#define RESP_FIXED_NULL 0x00FCu", "#define RESP_FIXED_NULL 0x00BCu"),
    (COPROC, "#define RESP_FIXED_MP_REGISTER 0x00F0u", "#define RESP_FIXED_MP_REGISTER 0x00D0u"),
    (COPROC, "#define RESP_FIXED_MP_REGISTER 0x00F0u", "#define RESP_FIXED_MP_REGISTER 0x00B0u"),
    # A-B67: PC ビット（bit14）は可変。マスクに入れない
    (COPROC, "#define RESP_FIXED_LOW_BYTE 0x00FFu", "#define RESP_FIXED_LOW_BYTE 0x40FFu"),
    (COPROC, "#define RESP_FIXED_MP_REGISTER 0x00F0u", "#define RESP_FIXED_MP_REGISTER 0x40F0u"),
    (COPROC, "#define RESP_FIXED_NULL 0x00FCu", "#define RESP_FIXED_NULL 0x00FDu"),
    # A-B66: 条件系でも下位の固定ビットを検査する
    (COPROC, "if (!ca || (resp & RESP_FIXED_LOW_BYTE)) {", "if (!ca || (!conditional && (resp & RESP_FIXED_LOW_BYTE))) {"),
    (COPROC, "if ((conditional && !ca) || (resp & RESP_FIXED_LOW_BYTE)) {", "if ((conditional && !ca) || (!conditional && (resp & RESP_FIXED_LOW_BYTE))) {"),
    (COPROC, "        if (resp & RESP_FIXED_LOW_BYTE) {\n            cp_violation(r);\n            return DIALOG_STOP;\n        }\n        ok =", "        if (!conditional && (resp & RESP_FIXED_LOW_BYTE)) {\n            cp_violation(r);\n            return DIALOG_STOP;\n        }\n        ok ="),
    (COPROC, "if (resp & RESP_FIXED_MP_REGISTER) {", "if (!conditional && (resp & RESP_FIXED_MP_REGISTER)) {"),

    (COPROC, "if ((conditional && !ca) || (resp & RESP_FIXED_LOW_BYTE)) {", "if (conditional && !ca) {"),
    (COPROC, "        if (resp & RESP_FIXED_LOW_BYTE) {\n            cp_violation(r);\n            return DIALOG_STOP;\n        }\n        ok =", "        ok ="),
    (COPROC, "        if (resp & RESP_FIXED_NULL) {\n            cp_violation(r);\n            return DIALOG_STOP;\n        }\n", ""),
    (COPROC, "        if (resp & RESP_FIXED_MP_REGISTER) {\n            cp_violation(r);\n            return DIALOG_STOP;\n        }\n", ""),
    # A-B60: 条件系でも CA=1 の opword・stream は有効
    (COPROC, "if (code == PRIM_TRANSFER_OPERATION_WORD) {", "if (code == PRIM_TRANSFER_OPERATION_WORD && !conditional) {"),
    (COPROC, "} else if (code == PRIM_TRANSFER_INSTRUCTION_STREAM) {", "} else if (code == PRIM_TRANSFER_INSTRUCTION_STREAM && !conditional) {"),
    # A-B61: EA・符号化の境界
    (COPROC, "OP_EA_REG(ea) >= 2u && OP_EA_REG(ea) <= 4u", "OP_EA_REG(ea) >= 1u && OP_EA_REG(ea) <= 4u"),
    (COPROC, "to_coprocessor ? !(ea_is_control((int)ea) || mode == 3u)", "to_coprocessor ? !(ea_is_control_alterable((int)ea) || mode == 3u)"),
    (COPROC, "for (unsigned ea = 0; ea < 64; ea++)", "for (unsigned ea = 0; ea < 63; ea++)"),
    # A-B62: format word の値の幅と境界
    (COPROC, "#define FORMAT_CODE(fw) (((fw) >> 8) & 0xFFu)", "#define FORMAT_CODE(fw) (((fw) >> 8) & 0x7Fu)"),
    (COPROC, "#define FORMAT_LENGTH(fw) ((fw) & 0xFFu)", "#define FORMAT_LENGTH(fw) ((fw) & 0x7Fu)"),
    (COPROC, "FORMAT_CODE(fw) >= FORMAT_VALID_MIN;", "FORMAT_CODE(fw) > FORMAT_VALID_MIN;"),
    (COPROC, "(memory_valid && (len & 3u))", "(memory_valid && (len & 2u))"),
    (COPROC, "(valid && (len & 3u))", "(valid && (len & 2u))"),
    (COPROC, "(valid && (len & 3u))", "(len & 3u)"),
    (COPROC, "const unsigned body = valid ? len : 0u;", "const unsigned body = len;"),
    # A-B63: レジスタ番号と type の値域
    (COPROC, "if (m.reg >= 0)\n        m.delta", "if (m.reg > 0)\n        m.delta"),
    (COPROC, "if ((type == CP_TYPE_SAVE || type == CP_TYPE_RESTORE) && !(cpu->sr & SR_S)) {", "if (type >= CP_TYPE_BCC_LONG && !(cpu->sr & SR_S)) {"),
    (COPROC, "(FUNC_SINGLE_MP_REGISTER << 8) | (r->data_register & 7u)", "(FUNC_SINGLE_MP_REGISTER << 8) | (r->data_register & 3u)"),
]


def run(cmd, **kw):
    return subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, **kw)


def build_and_test():
    for name in ("test_fpcp_model", "test_fpcp_host"):
        for p in list(BUILD.glob(f"tests/CMakeFiles/{name}.dir/**/*.o")) + [BUILD / "tests" / name]:
            p.unlink(missing_ok=True)
    for p in BUILD.glob("CMakeFiles/mc68000.dir/**/*.o"):
        if p.name in ("coproc.c.o", "m68000.c.o", "ops_system.c.o", "decode.c.o"):
            p.unlink()
    (BUILD / "libmc68000.a").unlink(missing_ok=True)
    r = run(["cmake", "--build", str(BUILD), "--target", "test_fpcp_model", "test_fpcp_host", "-j8"])
    if r.returncode != 0:
        return "BUILD-ERROR", r.stdout[-400:] + r.stderr[-400:]
    for name in ("test_fpcp_model", "test_fpcp_host"):
        t = run([str(BUILD / "tests" / name)], timeout=300)
        if t.returncode != 0:
            return "KILLED", f"{name}: {t.stderr.strip().splitlines()[-1] if t.stderr.strip() else t.returncode}"
    return "SURVIVED", ""


def main():
    if not CHECK_ONLY:
        status, detail = build_and_test()
        if status != "SURVIVED":  # 変異なしで落ちるなら、全変異が「検出」に見えてしまう
            sys.exit(f"baseline failed: {status} {detail}")
    results = []
    for i, (path, old, new) in enumerate(MUTATIONS):
        f = ROOT / path
        src = f.read_text()
        n = src.count(old)
        if CHECK_ONLY:
            print(f"{i:2d} hits={n} {path}: {old[:60]!r}")
            continue
        if n != 1:
            print(f"{i:2d} PATTERN-HITS={n} {path}: {old[:60]!r}")
            results.append((i, "PATTERN", ""))
            continue
        f.write_text(src.replace(old, new))
        try:
            status, detail = build_and_test()
        finally:
            f.write_text(src)
        print(f"{i:2d} {status:10s} {path}: {old[:50]!r} {detail}")
        results.append((i, status, detail))
    if not CHECK_ONLY:
        bad = [r for r in results if r[1] not in ("KILLED",)]
        print(f"\n{len(MUTATIONS) - len(bad)} / {len(MUTATIONS)} killed")
        for r in bad:
            print("NOT KILLED:", r)
        # 最後の変異のオブジェクトが（1 秒粒度のタイムスタンプで）残らないよう消してから、元のソースで再ビルドする
        for p in BUILD.glob("CMakeFiles/mc68000.dir/**/*.o"):
            if p.name in ("coproc.c.o", "m68000.c.o", "ops_system.c.o", "decode.c.o"):
                p.unlink()
        (BUILD / "libmc68000.a").unlink(missing_ok=True)
        run(["cmake", "--build", str(BUILD), "-j8"])


main()
