/*
 * coproc.c — 68020/68030 のコプロセッサインタフェース（MPU 側のプロトコル）。
 * 設計は docs/mc68000-internals/reference/fpu.md、一次資料は [UM20 §7]（MC68020 User's Manual
 * 第7章）と [881UM §7]。
 *
 * F-line 命令（cpID 1..7）の cpGEN / cpBcc / cpScc / cpDBcc / cpTRAPcc / cpSAVE / cpRESTORE
 * を、
 * CIR（コプロセッサインタフェースレジスタ）の読み書きと応答プリミティブの処理として実行する。
 * CIR の先は「ポート」が切り替える（設計書 §3.1）:
 *   1. cpID=1 かつ内蔵 FPU モデル（cpu->fpu）: モデルの型付き API。応答は [881UM Table 7-7]
 * の値へ符号化
 *   2. bus.cpu_space_read/write が非 NULL: ホストの外付けコプロセッサ（FC=7、CPU 空間アドレス）
 *   3. どちらも無い: コプロセッサ無し。F-line 例外（ベクタ 11）。ただしユーザモードの
 * cpSAVE/cpRESTORE は、 コプロセッサの有無を知る前に特権違反（ベクタ 8、[UM20 §7.5.2.3]）
 */
#include "cpu_internal.h"
#include "mc68000/fpu.h"

/* CIR のオフセット（[UM20 Figure 7-5]） */
enum {
    CIR_RESPONSE = 0x00,
    CIR_CONTROL = 0x02,
    CIR_SAVE = 0x04,
    CIR_RESTORE = 0x06,
    CIR_OPERATION_WORD = 0x08,
    CIR_COMMAND = 0x0A,
    CIR_CONDITION = 0x0E,
    CIR_OPERAND = 0x10,
    CIR_REGISTER_SELECT = 0x14,
    CIR_INSTRUCTION_ADDRESS = 0x18,
};

/* CPU 空間アドレス（[UM20 §7.1.4]）: A19-A16=0010、A15-A13=cpID、A4-A0=CIR 選択 */
#define CP_SPACE_BASE UINT32_C(0x00020000)
#define CP_ID_SHIFT   13
#define CP_ID_FPU     1u

/* コントロール CIR の値（[UM20 §7.3.2]） */
#define CP_CONTROL_ABORT         0x0001u
#define CP_CONTROL_EXCEPTION_ACK 0x0002u

/* CIR のサイズ（バイト） */
#define CIR_SIZE_WORD 2
#define CIR_SIZE_LONG 4

/* F-line オペレーションワード（[UM20 §7.2]）: 1111 cpID(3) type(3) ea(6) */
#define OP_CPID(op)    (((op) >> 9) & 7u)
#define OP_TYPE(op)    (((op) >> 6) & 7u)
#define OP_EA(op)      ((op) & 0x3Fu)
#define OP_EA_MODE(ea) (((ea) >> 3) & 7u)
#define OP_EA_REG(ea)  ((ea) & 7u)

enum {
    CP_TYPE_GEN = 0,
    CP_TYPE_COND = 1, /* cpScc / cpDBcc / cpTRAPcc。EA 欄の mode で分ける */
    CP_TYPE_BCC_WORD = 2,
    CP_TYPE_BCC_LONG = 3,
    CP_TYPE_SAVE = 4,
    CP_TYPE_RESTORE = 5,
    CP_TYPE_COUNT_VALID = 6, /* 6、7 は無効（CIR に触れず F-line） */
};

/* cpGEN 以外の種別を EA 欄から識別する */
enum { CP_EA_MODE_DBCC = 1, CP_EA_MODE_OTHER = 7 };
enum {
    CP_TRAPCC_OPMODE_ONE_WORD = 2,
    CP_TRAPCC_OPMODE_TWO_WORDS = 3,
    CP_TRAPCC_OPMODE_NONE = 4
};

/* 応答プリミティブ（[UM20 §7.4]、[UM20 §7.6]
 * の符号化）。bit15=CA、bit14=PC、bit13=DR、bit12-8=機能 */
#define RESP_CA        0x8000u
#define RESP_PC        0x4000u
#define RESP_DR        0x2000u
#define RESP_CODE(r)   (((r) >> 8) & 0x3Fu) /* bit13-8 */
#define RESP_FUNC(r)   (((r) >> 8) & 0x1Fu) /* bit12-8（DR を除く） */
#define RESP_LENGTH(r) ((r) & 0xFFu)
/* 図で 0 に固定された下位ビット（[UM20 Figure 7-23〜7-26、7-33]）。違えば未定義 */
#define RESP_FIXED_LOW_BYTE \
    0x00FFu /* Busy、supervisor check、transfer operation word: bit7-0 */
#define RESP_FIXED_NULL        0x00FCu /* Null: bit7-2（bit1=PF、bit0=TF） */
#define RESP_FIXED_MP_REGISTER 0x00F0u /* transfer single MP register: bit7-4 */

enum {
    PRIM_SUPERVISOR_CHECK = 0x04,
    PRIM_BUSY = 0x24,
    PRIM_TRANSFER_OPERATION_WORD = 0x07,
    PRIM_NULL = 0x08, /* bit8 = IA */
    PRIM_NULL_IA = 0x09,
    PRIM_TRANSFER_INSTRUCTION_STREAM = 0x0F,
    PRIM_TAKE_PRE = 0x1C,
    PRIM_TAKE_MID = 0x1D,
    PRIM_TAKE_POST = 0x1E,
    /* DR を除いた bit12-8 で識別するもの */
    FUNC_MULTIPLE_COPROCESSOR_REGISTERS = 0x01,
    FUNC_SINGLE_MP_REGISTER = 0x0C,
};
#define RESP_IS_EA_TRANSFER(r) ((RESP_CODE(r) & 0x18u) == 0x10u) /* bit12=1、bit11=0 */

#define NULL_PF                0x0002u
#define NULL_TF                0x0001u
#define MP_REGISTER_IS_ADDRESS 0x0008u

/* Evaluate EA and transfer data の valid EA（[UM20 Table 7-4]） */
enum {
    VALID_EA_CONTROL_ALTERABLE,
    VALID_EA_DATA_ALTERABLE,
    VALID_EA_MEMORY_ALTERABLE,
    VALID_EA_ALTERABLE,
    VALID_EA_CONTROL,
    VALID_EA_DATA,
    VALID_EA_MEMORY,
    VALID_EA_ANY,
};

/* コプロセッサ命令の format word（[UM20 Table 7-2]） */
enum {
    FORMAT_EMPTY = 0x00,
    FORMAT_NOT_READY = 0x01,
    FORMAT_INVALID = 0x02,
    FORMAT_RESERVED_END = 0x0F, /* 0x03-0x0F は予約（invalid として扱う） */
    FORMAT_VALID_MIN = 0x10,
};
#define FORMAT_CODE(fw)          (((fw) >> 8) & 0xFFu)
#define FORMAT_LENGTH(fw)        ((fw) & 0xFFu)
#define FRAME_HEADER_BYTES       4u /* format word + 予約 word */
#define FRAME_RESERVED_NON_EMPTY 0xFFFFu

/* 例外ベクタ */
enum {
    VEC_PRIVILEGE = 8,
    VEC_FLINE = 11,
    VEC_PROTOCOL_VIOLATION = 13,
    VEC_FORMAT_ERROR = 14,
    VEC_TRAPCC = 7,
};

/* 応答 CIR
 * の読み直しの上限。同期モデルでは応答が時間で変わらないので、ホストが同じ応答を返し続けると
 * ゲストが止まらなくなる。上限を超えたら protocol violation にする（設計書 §3.4）。 */
#define CP_REREAD_LIMIT        65536
#define CP_NOMINAL_CYCLES      24
#define CP_EXCEPTION_CYCLES    34
#define CP_FORMAT_ERROR_CYCLES 50 /* RTE の format error（ops_system.c）と同じ */
#define CP_FAULT_CYCLES \
    4 /* group0 フォルトが保留になったときのハンドラの戻り値（他のハンドラと同じ） */

/* ------------------------------------------------------------------ */
/* CIR ポート                                                          */
/* ------------------------------------------------------------------ */

static bool port_present(const mc68k_state_t *cpu, unsigned cpid) {
    if (cpid == CP_ID_FPU && cpu->fpu)
        return true;
    return cpu->bus.cpu_space_read && cpu->bus.cpu_space_write;
}

/* モデルの型付き応答を [881UM §7.4.2]／Table 7-7 の16bit値へ符号化する。 */
static uint16_t model_encode_response(const mc68k_fpu_response_t *r) {
    uint16_t v = 0;

    if (r->coprocessor_active)
        v |= RESP_CA;
    if (r->request_pc)
        v |= RESP_PC;
    switch (r->kind) {
    case MC68K_FPU_RESPONSE_NULL:
        v |= (uint16_t)(PRIM_NULL << 8);
        if (r->interruptible)
            v |= 0x0100u;
        if (!r->processing)
            v |= NULL_PF;
        if (r->condition_true)
            v |= NULL_TF;
        break;
    case MC68K_FPU_RESPONSE_EA_TRANSFER:
        v |= (uint16_t)(0x1000u | ((unsigned)(r->valid_ea & 7u) << 8) | r->length);
        if (r->transfer_to_host)
            v |= RESP_DR;
        break;
    case MC68K_FPU_RESPONSE_MP_REGISTER:
        v |= (uint16_t)((FUNC_SINGLE_MP_REGISTER << 8) | (r->data_register & 7u));
        if (r->transfer_to_host)
            v |= RESP_DR;
        break;
    case MC68K_FPU_RESPONSE_FP_REGISTERS:
        v |= (uint16_t)((FUNC_MULTIPLE_COPROCESSOR_REGISTERS << 8) | r->length);
        if (r->transfer_to_host)
            v |= RESP_DR;
        break;
    case MC68K_FPU_RESPONSE_TAKE_PRE_EXCEPTION:
        v |= (uint16_t)((PRIM_TAKE_PRE << 8) | r->vector);
        break;
    case MC68K_FPU_RESPONSE_TAKE_MID_EXCEPTION:
        v |= (uint16_t)((PRIM_TAKE_MID << 8) | r->vector);
        break;
    }
    return v;
}

static bool model_read(mc68k_state_t *cpu, unsigned cir, uint32_t *out) {
    mc68k_fpu_t *f = cpu->fpu;
    uint32_t word = 0;
    uint16_t half = 0;
    size_t body_bytes;

    switch (cir) {
    case CIR_RESPONSE: {
        mc68k_fpu_response_t r = mc68k_fpu_read_response(f);
        *out = model_encode_response(&r);
        return true;
    }
    case CIR_SAVE: {
        mc68k_fpu_begin_save(f, &half, &body_bytes);
        const size_t words = body_bytes / sizeof(uint32_t);
        /* モデルの body はメモリ順。実機はオペランド CIR へ逆順に出すので、ここで全部取り出して
         * 逆順に返す（cpu_internal.h の cp_save_body） */
        /* 上限（68882 の busy frame が 53
         * long）を超えるモデルは無い。超えたら実装の食い違いなので、 セーブ CIR
         * のアクセス失敗として F-line にする */
        if (words > CP_SAVE_BODY_MAX_WORDS)
            return false;
        for (size_t i = 0; i < words; i++)
            mc68k_fpu_read_operand(f, &cpu->cp_save_body[i]);
        cpu->cp_save_left = (unsigned)words;
        *out = half;
        return true;
    }
    case CIR_RESTORE:
        *out = cpu->cp_restore_readback;
        return true;
    case CIR_OPERAND:
        if (cpu->cp_save_left > 0) { /* FSAVE の body。常に 4 バイト */
            *out = cpu->cp_save_body[--cpu->cp_save_left];
            return true;
        }
        /* 戻り値 false はその phase で不正なアクセス。モデルが protocol violation
         * へ遷移済みで、 次の応答読出しが vector 13 を返す（設計書 §3.1）。 */
        mc68k_fpu_read_operand(f, &word);
        *out = word;
        return true;
    case CIR_REGISTER_SELECT:
        mc68k_fpu_read_register_select(f, &half);
        *out = half;
        return true;
    default:
        return false;
    }
}

static bool model_write(mc68k_state_t *cpu, unsigned cir, uint32_t val) {
    mc68k_fpu_t *f = cpu->fpu;
    uint16_t readback = 0;
    size_t body_bytes;

    /* write_* の戻り値 false は model_read() と同じく、モデルが保持する protocol violation。 */
    switch (cir) {
    case CIR_CONTROL:
        cpu->cp_save_left = 0;
        if (val & CP_CONTROL_ABORT)
            mc68k_fpu_control(f, MC68K_FPU_CONTROL_ABORT);
        else if (val & CP_CONTROL_EXCEPTION_ACK)
            mc68k_fpu_control(f, MC68K_FPU_CONTROL_EXCEPTION_ACK);
        return true;
    case CIR_RESTORE:
        cpu->cp_save_left = 0;
        mc68k_fpu_begin_restore(f, (uint16_t)val, &readback, &body_bytes);
        cpu->cp_restore_readback = readback;
        return true;
    case CIR_OPERATION_WORD: /* 68881/68882 は transfer operation word を使わない */
        return true;
    case CIR_COMMAND:
        mc68k_fpu_write_command_word(f, (uint16_t)val);
        return true;
    case CIR_CONDITION:
        mc68k_fpu_write_condition(f, (uint8_t)(val & 0x3Fu));
        return true;
    case CIR_OPERAND:
        mc68k_fpu_write_operand(f, val);
        return true;
    case CIR_INSTRUCTION_ADDRESS:
        mc68k_fpu_write_instruction_address(f, val);
        return true;
    default:
        return false;
    }
}

static uint32_t cir_address(unsigned cpid, unsigned cir) {
    return CP_SPACE_BASE | ((uint32_t)cpid << CP_ID_SHIFT) | cir;
}

static bool port_read(mc68k_state_t *cpu, unsigned cpid, unsigned cir, uint8_t size,
                      uint32_t *out) {
    const uint32_t addr = cir_address(cpid, cir);
    const mc68k_access_t access = {.fc = MC68K_FC_CPU_SPACE, .read = true, .size = size};
    bool ok;

    *out = 0;
    if (cpid == CP_ID_FPU && cpu->fpu) {
        ok = model_read(cpu, cir, out);
        /* モデルのオペランドは 32bit の MSB
         * 側に詰めた形。ホストへの契約（bus_hooks.h）は下位詰めの数値 */
        if (ok && cir == CIR_OPERAND && size < CIR_SIZE_LONG)
            *out >>= 8u * (CIR_SIZE_LONG - size);
    } else
        ok = cpu_bus_cpu_space_read(cpu, MC68K_FC_CPU_SPACE, addr, size, out);
    cpu_account_bus_cycle(cpu, addr, &access, false);
    if (ok && size < CIR_SIZE_LONG)
        *out &= (UINT32_C(1) << (8u * size)) - 1u;
    return ok;
}

static bool port_write(mc68k_state_t *cpu, unsigned cpid, unsigned cir, uint8_t size,
                       uint32_t val) {
    const uint32_t addr = cir_address(cpid, cir);
    const mc68k_access_t access = {.fc = MC68K_FC_CPU_SPACE, .read = false, .size = size};
    bool ok;

    if (size < CIR_SIZE_LONG)
        val &= (UINT32_C(1) << (8u * size)) - 1u;
    if (cpid == CP_ID_FPU && cpu->fpu) {
        if (cir == CIR_OPERAND && size < CIR_SIZE_LONG)
            val <<= 8u * (CIR_SIZE_LONG - size);
        ok = model_write(cpu, cir, val);
    } else
        ok = cpu_bus_cpu_space_write(cpu, MC68K_FC_CPU_SPACE, addr, size, val);
    cpu_account_bus_cycle(cpu, addr, &access, false);
    return ok;
}

/* ------------------------------------------------------------------ */
/* 実行コンテキストと例外                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    mc68k_state_t *cpu;
    uint16_t opword;
    unsigned cpid;
    uint32_t ea; /* 最後に評価した実効アドレス（format 9 のフレームに載せる） */
    int cycles;  /* 例外へ移ったときに返すサイクル数 */
    /* コプロセッサと対話中か。最初の CIR アクセスが成功してから対話が終わるまで
     * true。対話中の失敗は コントロール CIR の AB
     * で命令を中止する（対話の前後ではコプロセッサに触れない）。 */
    bool engaged;
} cp_run_t;

/* 命令を最初からやり直す（busy、not
 * ready）。次の命令境界で割込みが受け付けられる。命令は完了して
 * いないのでトレース例外は起こさない。 */
static void cp_restart(cp_run_t *r) {
    mc68k_state_t *cpu = r->cpu;

    cpu->pc = cpu->instr_pc;
    cpu->exc_suppress_trace = true;
    r->cycles = CP_NOMINAL_CYCLES;
}

/* コプロセッサ無し、または無効な命令。vector 11（format 0、PC = 命令先頭）。 */
static bool cp_fline(cp_run_t *r) {
    mc68k_state_t *cpu = r->cpu;

    r->cycles = cpu_dispatch_exception(cpu, cpu->instr_pc, VEC_FLINE,
                                       cpu_is_68030_family(cpu) ? CYC(cpu, CYC_EXC_ILLEGAL)
                                                                : CP_EXCEPTION_CYCLES);
    return false;
}

static void cp_abort(cp_run_t *r) {
    port_write(r->cpu, r->cpid, CIR_CONTROL, CIR_SIZE_WORD, CP_CONTROL_ABORT);
}

static bool cp_privilege_violation(cp_run_t *r) {
    mc68k_state_t *cpu = r->cpu;

    r->cycles =
        cpu_dispatch_exception(cpu, cpu->instr_pc, VEC_PRIVILEGE, CYC(cpu, CYC_EXC_PRIVILEGE));
    return false;
}

/* 命令を中止して F-line（応答プリミティブが命令の EA と合わないとき、[UM20 §7.5.2.2]）。 */
static bool cp_abort_fline(cp_run_t *r) {
    cp_abort(r);
    r->engaged = false;
    return cp_fline(r);
}

static bool cp_format_error(cp_run_t *r) {
    mc68k_state_t *cpu = r->cpu;

    cp_abort(r);
    r->engaged = false;
    r->cycles =
        cpu_dispatch_exception(cpu, cpu->instr_pc, VEC_FORMAT_ERROR, CP_FORMAT_ERROR_CYCLES);
    return false;
}

static bool cp_take_exception(cp_run_t *r, cp_exc_kind_t kind, uint16_t vector) {
    mc68k_state_t *cpu = r->cpu;

    r->cycles = cpu_dispatch_coproc_exception(cpu, kind, vector, cpu->pc, r->opword, r->ea,
                                              CP_EXCEPTION_CYCLES);
    return false;
}

/* MPU が検出する protocol violation。コントロール CIR には書かない（[UM20 §7.5.2.1]）。 */
static bool cp_violation(cp_run_t *r) {
    return cp_take_exception(r, CP_EXC_MID, VEC_PROTOCOL_VIOLATION);
}

/* CIR アクセスの失敗。initial は命令を始める最初の CIR アクセスで、失敗はコプロセッサ無しとして
 * F-line。 それ以外は命令を中止（コントロール CIR に
 * AB）して通常のバスエラー（命令は最初から再実行される。 設計書 §3.9）。 */
static bool cp_cir_fault(cp_run_t *r, unsigned cir, bool read, uint8_t size, bool initial) {
    mc68k_state_t *cpu = r->cpu;

    if (initial)
        return cp_fline(r);
    cp_abort(r);
    r->engaged = false;
    const mc68k_access_t access = {.fc = MC68K_FC_CPU_SPACE, .read = read, .size = size};
    cpu_note_access(cpu, &access);
    cpu_signal_bus_error(cpu, cir_address(r->cpid, cir), read);
    r->cycles = CP_FAULT_CYCLES;
    return false;
}

static bool cp_cir_read(cp_run_t *r, unsigned cir, uint8_t size, uint32_t *out, bool initial) {
    if (port_read(r->cpu, r->cpid, cir, size, out)) {
        r->engaged = true;
        return true;
    }
    return cp_cir_fault(r, cir, true, size, initial);
}

static bool cp_cir_write(cp_run_t *r, unsigned cir, uint8_t size, uint32_t val, bool initial) {
    if (port_write(r->cpu, r->cpid, cir, size, val)) {
        r->engaged = true;
        return true;
    }
    return cp_cir_fault(r, cir, false, size, initial);
}

/* メモリアクセスの失敗（cpu_read_* / cpu_write_* が group0 フォルトを保留した）。命令を中止して
 * 通常のバスエラー／アドレスエラーに任せる。 */
static bool cp_memory_ok(cp_run_t *r) {
    if (r->cpu->exec_state == CPU_NORMAL)
        return true;
    if (r->engaged)
        cp_abort(r);
    r->cycles = CP_FAULT_CYCLES;
    return false;
}

/* ------------------------------------------------------------------ */
/* 実効アドレスとデータ転送                                              */
/* ------------------------------------------------------------------ */

static bool ea_is_valid_encoding(unsigned ea) {
    return OP_EA_MODE(ea) != CP_EA_MODE_OTHER || OP_EA_REG(ea) <= 4u;
}

static bool ea_in_valid_class(unsigned valid, unsigned ea) {
    const int e = (int)ea;
    const unsigned mode = OP_EA_MODE(ea);

    switch (valid) {
    case VALID_EA_CONTROL_ALTERABLE:
        return ea_is_control_alterable(e);
    case VALID_EA_DATA_ALTERABLE:
        return ea_is_data_alterable(e);
    case VALID_EA_MEMORY_ALTERABLE:
        return ea_is_memory_alterable(e);
    case VALID_EA_ALTERABLE:
        return ea_is_alterable(e);
    case VALID_EA_CONTROL:
        return ea_is_control(e);
    case VALID_EA_DATA:
        return ea_is_data(e);
    case VALID_EA_MEMORY:
        return mode >= 2u && ea_is_valid_encoding(ea);
    default:
        return ea_is_valid_encoding(ea);
    }
}

/* メモリの実効アドレス。(An)+ と -(An) のレジスタ更新は commit
 * まで行わない（転送が全部成功した後に 更新するため。バスエラーで命令を再実行できる）。total
 * はこの EA への転送の総バイト数。 */
typedef struct {
    uint32_t addr;
    int reg;       /* 更新するアドレスレジスタ。無ければ -1 */
    int32_t delta; /* commit で足す値 */
} cp_mem_t;

static unsigned a7_adjusted_step(unsigned reg, unsigned total) {
    /* A7 で 1 バイトのときは語境界を保つため 2（[UM20 §7.4.9]） */
    return (reg == 7u && total == 1u) ? 2u : total;
}

static bool cp_resolve_memory(cp_run_t *r, unsigned ea, unsigned total, cp_mem_t *m) {
    mc68k_state_t *cpu = r->cpu;
    const unsigned mode = OP_EA_MODE(ea), reg = OP_EA_REG(ea);

    m->reg = -1;
    m->delta = 0;
    if (mode == 3u) {
        m->addr = cpu->a[reg];
        m->reg = (int)reg;
        m->delta = (int32_t)a7_adjusted_step(reg, total);
    } else if (mode == 4u) {
        const unsigned step = a7_adjusted_step(reg, total);
        m->addr = cpu->a[reg] - step;
        m->reg = (int)reg;
        m->delta = -(int32_t)step;
    } else {
        m->addr = ea_get_addr(cpu, (int)mode, (int)reg);
        if (!cp_memory_ok(r))
            return false;
    }
    r->ea = m->addr;
    return true;
}

static void cp_commit_memory(mc68k_state_t *cpu, const cp_mem_t *m) {
    if (m->reg >= 0)
        cpu->a[m->reg] = (uint32_t)((int64_t)cpu->a[m->reg] + m->delta);
}

/* メモリ → オペランド CIR。アドレス昇順に 4 バイトずつ。端数（1〜3
 * バイト）はその大きさのまま転送する （[UM20 §7.3.8 Figure
 * 7-21、§7.4.9]）。値は下位詰めの数値（bus_hooks.h の契約）。 */
static bool cp_memory_to_operand(cp_run_t *r, uint32_t addr, unsigned len) {
    mc68k_state_t *cpu = r->cpu;

    for (unsigned off = 0; off < len; off += 4) {
        const unsigned n = len - off < 4 ? len - off : 4;
        uint32_t v = 0;

        if (n == 4) {
            v = cpu_read_long(cpu, addr + off);
        } else if (n == 2) {
            v = cpu_read_word(cpu, addr + off);
        } else if (n == 1) {
            v = cpu_read_byte(cpu, addr + off);
        } else {
            v = (uint32_t)cpu_read_word(cpu, addr + off) << 8;
            if (cpu->exec_state == CPU_NORMAL)
                v |= cpu_read_byte(cpu, addr + off + 2);
        }
        if (!cp_memory_ok(r))
            return false;
        if (!cp_cir_write(r, CIR_OPERAND, (uint8_t)n, v, false))
            return false;
    }
    return true;
}

/* オペランド CIR → メモリ。 */
static bool cp_operand_to_memory(cp_run_t *r, uint32_t addr, unsigned len) {
    mc68k_state_t *cpu = r->cpu;

    for (unsigned off = 0; off < len; off += 4) {
        const unsigned n = len - off < 4 ? len - off : 4;
        uint32_t v;

        if (!cp_cir_read(r, CIR_OPERAND, (uint8_t)n, &v, false))
            return false;
        if (n == 4) {
            cpu_write_long(cpu, addr + off, v);
        } else if (n == 2) {
            cpu_write_word(cpu, addr + off, (uint16_t)v);
        } else if (n == 1) {
            cpu_write_byte(cpu, addr + off, (uint8_t)v);
        } else {
            cpu_write_word(cpu, addr + off, (uint16_t)(v >> 8));
            if (cpu->exec_state == CPU_NORMAL)
                cpu_write_byte(cpu, addr + off + 2, (uint8_t)v);
        }
        if (!cp_memory_ok(r))
            return false;
    }
    return true;
}

/* 命令ストリーム（scanPC）から len バイトをオペランド CIR へ。len は偶数。端数の 2 バイトは
 * word で書く （[UM20 §7.4.7]）。 */
static bool cp_stream_to_operand(cp_run_t *r, unsigned len) {
    mc68k_state_t *cpu = r->cpu;

    for (unsigned off = 0; off < len; off += 4) {
        const unsigned n = len - off < 4 ? len - off : 4;
        uint32_t v = cpu_fetch_word(cpu);

        if (n == 4 && cpu->exec_state == CPU_NORMAL)
            v = (v << 16) | cpu_fetch_word(cpu);
        if (!cp_memory_ok(r))
            return false;
        if (!cp_cir_write(r, CIR_OPERAND, (uint8_t)n, v, false))
            return false;
    }
    return true;
}

/* Dn/An との転送（length は 1/2/4）。値は下位詰めの数値。 */
static uint32_t register_to_operand(uint32_t reg, unsigned len) {
    return len == 4 ? reg : reg & ((UINT32_C(1) << (8u * len)) - 1u);
}

static uint32_t operand_to_register(uint32_t old, uint32_t operand, unsigned len,
                                    bool address) {
    if (len == 4)
        return operand;
    if (address) /* アドレスレジスタは符号拡張して long で更新（[UM20 §7.4.9]） */
        return len == 2 ? (uint32_t)(int32_t)(int16_t)operand
                        : (uint32_t)(int32_t)(int8_t)operand;
    return len == 2 ? (old & 0xFFFF0000u) | (operand & 0xFFFFu)
                    : (old & 0xFFFFFF00u) | (operand & 0xFFu);
}

/* Evaluate effective address and transfer data（[UM20 §7.4.9]）。 */
static bool cp_ea_transfer(cp_run_t *r, uint16_t resp) {
    mc68k_state_t *cpu = r->cpu;
    const unsigned ea = OP_EA(r->opword), mode = OP_EA_MODE(ea), reg = OP_EA_REG(ea);
    const unsigned valid = (resp >> 8) & 7u, len = RESP_LENGTH(resp);
    const bool to_coprocessor = !(resp & RESP_DR);
    uint32_t v;

    if (!ea_in_valid_class(valid, ea))
        return cp_abort_fline(r);
    if (!to_coprocessor && !ea_is_alterable((int)ea)) /* 変更できない EA への書込み */
        return cp_violation(r);

    if (mode <= 1u) { /* Dn / An */
        const bool address = mode == 1u;
        uint32_t *rg = address ? &cpu->a[reg] : &cpu->d[reg];

        if (len != 1u && len != 2u && len != 4u)
            return cp_violation(r);
        if (to_coprocessor)
            return cp_cir_write(r, CIR_OPERAND, (uint8_t)len, register_to_operand(*rg, len),
                                false);
        if (!cp_cir_read(r, CIR_OPERAND, (uint8_t)len, &v, false))
            return false;
        *rg = operand_to_register(*rg, v, len, address);
        return true;
    }
    if (mode == CP_EA_MODE_OTHER && reg == 4u) { /* #<data> */
        if (!to_coprocessor || (len != 1u && (len & 1u)))
            return cp_violation(r);
        if (len == 1u) {
            v = cpu_fetch_word(cpu) & 0xFFu;
            if (!cp_memory_ok(r))
                return false;
            return cp_cir_write(r, CIR_OPERAND, 1, v, false);
        }
        return cp_stream_to_operand(r, len);
    }

    cp_mem_t m;
    if (!cp_resolve_memory(r, ea, len, &m))
        return false;
    if (to_coprocessor ? !cp_memory_to_operand(r, m.addr, len)
                       : !cp_operand_to_memory(r, m.addr, len))
        return false;
    cp_commit_memory(cpu, &m);
    return true;
}

static unsigned popcount16(uint32_t v) {
    unsigned n = 0;

    for (v &= 0xFFFFu; v; v &= v - 1u)
        n++;
    return n;
}

/* Transfer multiple coprocessor registers（[UM20 §7.4.16]）。 */
static bool cp_fp_registers(cp_run_t *r, uint16_t resp) {
    mc68k_state_t *cpu = r->cpu;
    const unsigned ea = OP_EA(r->opword), mode = OP_EA_MODE(ea);
    const unsigned len = RESP_LENGTH(resp);
    const bool to_coprocessor = !(resp & RESP_DR);
    uint32_t mask;

    if (len & 1u)
        return cp_violation(r);
    /* メモリ→コプロセッサは制御と (An)+、コプロセッサ→メモリは制御可変と -(An) */
    if (to_coprocessor ? !(ea_is_control((int)ea) || mode == 3u)
                       : !(ea_is_control_alterable((int)ea) || mode == 4u))
        return cp_abort_fline(r);

    /* 実効アドレスの計算（拡張 word のフェッチ）の後にレジスタセレクト CIR を読む（[UM20
     * §7.4.16]）。 (An)+ と -(An) は転送の総バイト数が要るので、レジスタ更新量だけ後で確定する
     */
    const bool deferred = mode == 3u || mode == 4u;
    cp_mem_t m;
    if (!deferred && !cp_resolve_memory(r, ea, 0, &m))
        return false;
    if (!cp_cir_read(r, CIR_REGISTER_SELECT, CIR_SIZE_WORD, &mask, false))
        return false;
    const unsigned count = popcount16(mask), total = count * len;
    if (deferred && !cp_resolve_memory(r, ea, total, &m))
        return false;
    for (unsigned k = 0; k < count; k++) {
        /* -(An) は最初のオペランドが最上位のアドレス。ほかは昇順 */
        const uint32_t base = mode == 4u ? m.addr + (count - 1u - k) * len : m.addr + k * len;

        if (to_coprocessor ? !cp_memory_to_operand(r, base, len)
                           : !cp_operand_to_memory(r, base, len))
            return false;
    }
    cp_commit_memory(cpu, &m);
    return true;
}

static bool cp_single_mp_register(cp_run_t *r, uint16_t resp) {
    mc68k_state_t *cpu = r->cpu;
    const unsigned reg = resp & 7u;
    uint32_t *rg = (resp & MP_REGISTER_IS_ADDRESS) ? &cpu->a[reg] : &cpu->d[reg];
    uint32_t v;

    if (!(resp & RESP_DR))
        return cp_cir_write(r, CIR_OPERAND, CIR_SIZE_LONG, *rg, false);
    if (!cp_cir_read(r, CIR_OPERAND, CIR_SIZE_LONG, &v, false))
        return false;
    *rg = v;
    return true;
}

/* ------------------------------------------------------------------ */
/* 応答プリミティブの対話（cpGEN と条件系の共通部分）                      */
/* ------------------------------------------------------------------ */

typedef enum { DIALOG_CONTINUE, DIALOG_DONE, DIALOG_STOP, DIALOG_RESTART } dialog_step_t;

/* conditional: 条件系（EA を使う primitive と、CA=0 の * 付き primitive は protocol
 * violation）。 終了条件は cpGEN が「CA=0 の primitive」（トレース保留中は PF=1 の
 * NULL）、条件系が「CA=0 の NULL」。 */
static dialog_step_t cp_primitive(cp_run_t *r, uint16_t resp, bool conditional, bool *tf) {
    mc68k_state_t *cpu = r->cpu;
    const bool ca = (resp & RESP_CA) != 0;
    const unsigned code = RESP_CODE(resp), func = RESP_FUNC(resp);
    bool ok = false;

    if ((resp & RESP_PC) &&
        !cp_cir_write(r, CIR_INSTRUCTION_ADDRESS, CIR_SIZE_LONG, cpu->instr_pc, false))
        return DIALOG_STOP;

    if (code == PRIM_NULL || code == PRIM_NULL_IA) {
        if (resp & RESP_FIXED_NULL) {
            cp_violation(r);
            return DIALOG_STOP;
        }
        if (ca)
            return DIALOG_CONTINUE;
        if (conditional) {
            *tf = (resp & NULL_TF) != 0;
            return DIALOG_DONE;
        }
        /* cpGEN: トレース保留中は PF=1 の NULL（全処理の完了）まで読み続ける */
        return ((cpu->sr & SR_T1) && !(resp & NULL_PF)) ? DIALOG_CONTINUE : DIALOG_DONE;
    }
    if (code == PRIM_BUSY) {
        if (!ca || (resp & RESP_FIXED_LOW_BYTE)) { /* Busy は bit15（CA）=1、bit7-0=0
                                                      が固定（[UM20 Figure 7-23]） */
            cp_violation(r);
            return DIALOG_STOP;
        }
        cp_restart(r);
        return DIALOG_RESTART;
    }
    if (code == PRIM_TAKE_PRE || code == PRIM_TAKE_MID || code == PRIM_TAKE_POST) {
        if (ca) { /* take 系に CA は無い */
            cp_violation(r);
            return DIALOG_STOP;
        }
        if (!cp_cir_write(r, CIR_CONTROL, CIR_SIZE_WORD, CP_CONTROL_EXCEPTION_ACK, false))
            return DIALOG_STOP;
        const uint16_t vector = (uint16_t)RESP_LENGTH(resp);
        cp_exc_kind_t kind = code == PRIM_TAKE_PRE   ? CP_EXC_PRE
                             : code == PRIM_TAKE_MID ? CP_EXC_MID
                                                     : CP_EXC_POST;
        r->cycles = cpu_dispatch_coproc_exception(cpu, kind, vector, cpu->pc, r->opword, r->ea,
                                                  CP_EXCEPTION_CYCLES);
        return DIALOG_STOP;
    }

    /* 以下は CA を持つ primitive。条件系の CA=0 は protocol violation（[UM20 Table 7-6] の *
     * 付き） */
    if (code == PRIM_SUPERVISOR_CHECK) {
        if ((conditional && !ca) || (resp & RESP_FIXED_LOW_BYTE)) {
            cp_violation(r);
            return DIALOG_STOP;
        }
        if (cpu->sr &
            SR_S) /* cpGEN では bit15 によらず、応答 CIR を読み直す（[UM20 §7.4.5]） */
            return DIALOG_CONTINUE;
        cp_abort(r);
        r->engaged = false;
        cp_privilege_violation(r);
        return DIALOG_STOP;
    }
    if (conditional && !ca) {
        cp_violation(r);
        return DIALOG_STOP;
    }
    if (code == PRIM_TRANSFER_OPERATION_WORD) {
        if (resp & RESP_FIXED_LOW_BYTE) {
            cp_violation(r);
            return DIALOG_STOP;
        }
        ok = cp_cir_write(r, CIR_OPERATION_WORD, CIR_SIZE_WORD, r->opword, false);
    } else if (code == PRIM_TRANSFER_INSTRUCTION_STREAM) {
        if (RESP_LENGTH(resp) & 1u) {
            cp_violation(r);
            return DIALOG_STOP;
        }
        ok = cp_stream_to_operand(r, RESP_LENGTH(resp));
    } else if (func == FUNC_SINGLE_MP_REGISTER) {
        if (resp & RESP_FIXED_MP_REGISTER) {
            cp_violation(r);
            return DIALOG_STOP;
        }
        ok = cp_single_mp_register(r, resp);
    } else if (RESP_IS_EA_TRANSFER(resp) && !conditional) {
        ok = cp_ea_transfer(r, resp);
    } else if (func == FUNC_MULTIPLE_COPROCESSOR_REGISTERS && !conditional) {
        ok = cp_fp_registers(r, resp);
    } else {
        /* 未定義・未実装の primitive（設計書 §3.3）、条件系での EA 系 primitive */
        cp_violation(r);
        return DIALOG_STOP;
    }
    if (!ok)
        return DIALOG_STOP;
    /* cpGEN はトレース保留中、CA=0 の primitive の後も PF=1 の NULL まで読み続ける（[UM20
     * §7.4.4]） */
    return (ca || (!conditional && (cpu->sr & SR_T1))) ? DIALOG_CONTINUE : DIALOG_DONE;
}

/* 応答 CIR を読み、primitive を処理して、終了まで繰り返す。戻り値 false =
 * 例外へ移った、または命令を やり直す（r->cycles に返すサイクル数）。 */
static bool cp_dialog(cp_run_t *r, bool conditional, bool *tf) {
    for (int i = 0; i < CP_REREAD_LIMIT; i++) {
        uint32_t resp;

        if (!cp_cir_read(r, CIR_RESPONSE, CIR_SIZE_WORD, &resp, false))
            return false;
        switch (cp_primitive(r, (uint16_t)resp, conditional, tf)) {
        case DIALOG_CONTINUE:
            continue;
        case DIALOG_DONE:
            r->engaged =
                false; /* 対話は終わった。以降のメモリアクセスの失敗では AB を書かない */
            return true;
        case DIALOG_STOP:
        case DIALOG_RESTART:
            return false;
        }
    }
    cp_violation(r);
    return false;
}

/* ------------------------------------------------------------------ */
/* 命令                                                                */
/* ------------------------------------------------------------------ */

/* 変位ワード／ロングを読み、基点（変位の先頭のアドレス）に足した分岐先を返す。 */
static bool cp_fetch_branch_target(cp_run_t *r, bool long_displacement, uint32_t *target) {
    mc68k_state_t *cpu = r->cpu;
    const uint32_t base = cpu->pc;
    uint32_t disp;

    if (long_displacement) {
        disp = (uint32_t)cpu_fetch_word(cpu) << 16;
        if (cpu->exec_state == CPU_NORMAL)
            disp |= cpu_fetch_word(cpu);
    } else {
        disp = (uint32_t)(int32_t)(int16_t)cpu_fetch_word(cpu);
    }
    if (!cp_memory_ok(r))
        return false;
    *target = base + disp;
    return true;
}

static void cp_branch_to(mc68k_state_t *cpu, uint32_t target) {
    cpu->pc = target;
    cpu->flow_changed = true;
}

/* cpGEN の対話（resume のときはコマンド書込みを済ませたものとして応答の読出しから） */
static int cp_run_general(cp_run_t *r, bool resume) {
    mc68k_state_t *cpu = r->cpu;

    if (!resume) {
        const uint16_t command = cpu_fetch_word(cpu);
        if (!cp_memory_ok(r))
            return r->cycles;
        if (!cp_cir_write(r, CIR_COMMAND, CIR_SIZE_WORD, command, true))
            return r->cycles;
    }
    bool tf = false;
    return cp_dialog(r, false, &tf) ? CP_NOMINAL_CYCLES : r->cycles;
}

typedef enum { COND_SCC, COND_DBCC, COND_TRAPCC, COND_BCC_WORD, COND_BCC_LONG } cond_kind_t;

static cond_kind_t cond_kind(uint16_t opword) {
    const unsigned type = OP_TYPE(opword), ea = OP_EA(opword);

    if (type == CP_TYPE_BCC_WORD)
        return COND_BCC_WORD;
    if (type == CP_TYPE_BCC_LONG)
        return COND_BCC_LONG;
    if (OP_EA_MODE(ea) == CP_EA_MODE_DBCC)
        return COND_DBCC;
    if (OP_EA_MODE(ea) == CP_EA_MODE_OTHER && OP_EA_REG(ea) >= 2u && OP_EA_REG(ea) <= 4u)
        return COND_TRAPCC;
    return COND_SCC;
}

/* TF が決まった後の、MPU 側の完了処理 */
static int cp_complete_conditional(cp_run_t *r, cond_kind_t kind, bool tf) {
    mc68k_state_t *cpu = r->cpu;
    const unsigned ea = OP_EA(r->opword);
    uint32_t target;

    switch (kind) {
    case COND_BCC_WORD:
    case COND_BCC_LONG:
        if (!cp_fetch_branch_target(r, kind == COND_BCC_LONG, &target))
            return r->cycles;
        if (tf)
            cp_branch_to(cpu, target);
        return CP_NOMINAL_CYCLES;
    case COND_DBCC:
        if (!cp_fetch_branch_target(r, false, &target))
            return r->cycles;
        if (!tf) {
            const unsigned reg = OP_EA_REG(ea);
            const uint16_t counter = (uint16_t)((cpu->d[reg] & 0xFFFFu) - 1u);

            cpu->d[reg] = (cpu->d[reg] & 0xFFFF0000u) | counter;
            if (counter != 0xFFFFu)
                cp_branch_to(cpu, target);
        }
        return CP_NOMINAL_CYCLES;
    case COND_TRAPCC: {
        const unsigned opmode = OP_EA_REG(ea);
        const unsigned words = opmode == CP_TRAPCC_OPMODE_ONE_WORD    ? 1u
                               : opmode == CP_TRAPCC_OPMODE_TWO_WORDS ? 2u
                                                                      : 0u;

        cpu->pc += 2u * words; /* オペランド word は MPU が使わない。例外ハンドラが読む */
        if (!tf)
            return CP_NOMINAL_CYCLES;
        return cpu_dispatch_exception(cpu, cpu->pc, VEC_TRAPCC, CYC(cpu, CYC_EXC_TRAP));
    }
    case COND_SCC: {
        const unsigned mode = OP_EA_MODE(ea), reg = OP_EA_REG(ea);
        const uint8_t value = tf ? 0xFFu : 0x00u;

        if (mode == 0u) {
            cpu->d[reg] = (cpu->d[reg] & 0xFFFFFF00u) | value;
            return CP_NOMINAL_CYCLES;
        }
        cp_mem_t m;
        if (!cp_resolve_memory(r, ea, 1, &m))
            return r->cycles;
        cpu_write_byte(cpu, m.addr, value);
        if (!cp_memory_ok(r))
            return r->cycles;
        cp_commit_memory(cpu, &m);
        return CP_NOMINAL_CYCLES;
    }
    }
    return CP_NOMINAL_CYCLES;
}

static int cp_run_conditional(cp_run_t *r, bool resume) {
    mc68k_state_t *cpu = r->cpu;
    const cond_kind_t kind = cond_kind(r->opword);
    bool tf = false;

    if (!resume) {
        uint16_t condition =
            r->opword; /* cpBcc はオペレーションワード全体（[UM20 §7.2.2.1.2]） */

        if (kind != COND_BCC_WORD && kind != COND_BCC_LONG) {
            condition = cpu_fetch_word(cpu);
            if (!cp_memory_ok(r))
                return r->cycles;
        }
        if (!cp_cir_write(r, CIR_CONDITION, CIR_SIZE_WORD, condition, true))
            return r->cycles;
    }
    if (!cp_dialog(r, true, &tf))
        return r->cycles;
    return cp_complete_conditional(r, kind, tf);
}

static int cp_run_save(cp_run_t *r) {
    mc68k_state_t *cpu = r->cpu;
    const unsigned ea = OP_EA(r->opword);
    uint32_t fw;

    if (!cp_cir_read(r, CIR_SAVE, CIR_SIZE_WORD, &fw, true))
        return r->cycles;
    const unsigned code = FORMAT_CODE(fw), len = FORMAT_LENGTH(fw);
    if (code == FORMAT_NOT_READY) {
        cp_restart(r);
        return r->cycles;
    }
    const bool valid = code >= FORMAT_VALID_MIN;
    if ((code != FORMAT_EMPTY && !valid) || (valid && (len & 3u))) {
        cp_format_error(r);
        return r->cycles;
    }

    const unsigned body = valid ? len : 0u;
    cp_mem_t m;
    if (!cp_resolve_memory(r, ea, FRAME_HEADER_BYTES + body, &m))
        return r->cycles;
    cpu_write_long(cpu, m.addr, ((uint32_t)fw << 16) | (valid ? FRAME_RESERVED_NON_EMPTY : 0u));
    if (!cp_memory_ok(r))
        return r->cycles;
    /* 最初に読んだ long が最高アドレス、以後降順（[UM20 Figure 7-14] の SAVE ORDER） */
    for (unsigned i = 0; i < body / 4u; i++) {
        uint32_t v;

        if (!cp_cir_read(r, CIR_OPERAND, CIR_SIZE_LONG, &v, false))
            return r->cycles;
        cpu_write_long(cpu, m.addr + body - 4u * i, v);
        if (!cp_memory_ok(r))
            return r->cycles;
    }
    cp_commit_memory(cpu, &m);
    return CP_NOMINAL_CYCLES;
}

static int cp_run_restore(cp_run_t *r) {
    mc68k_state_t *cpu = r->cpu;
    const unsigned ea = OP_EA(r->opword);
    uint32_t rb;

    /* (An)+ で更新するバイト数は format word を読むまで分からないので、アドレスだけ先に決める
     */
    cp_mem_t m;
    if (!cp_resolve_memory(r, ea, 0, &m))
        return r->cycles;
    const uint16_t fw = cpu_read_word(cpu, m.addr);
    if (!cp_memory_ok(r))
        return r->cycles;
    if (!cp_cir_write(r, CIR_RESTORE, CIR_SIZE_WORD, fw, true))
        return r->cycles;
    unsigned code;
    int rereads = 0;
    do {
        if (!cp_cir_read(r, CIR_RESTORE, CIR_SIZE_WORD, &rb, false))
            return r->cycles;
        code = FORMAT_CODE(rb);
    } while (code == FORMAT_NOT_READY && ++rereads < CP_REREAD_LIMIT);
    if (code == FORMAT_NOT_READY) {
        cp_violation(r);
        return r->cycles;
    }
    /* 読み返し（コプロセッサの判定）が invalid なら format error。メモリから読んだ format word
     * が valid で 長さが 4 の倍数でないときも、リストア CIR を読んだ後に format error（[UM20
     * §7.5.2.7]）。転送する 長さは、MPU がメモリの format word から保持した値（[UM20 Figure
     * 7-18 Note 2]）。 */
    const unsigned len = FORMAT_LENGTH(fw);
    const bool coprocessor_valid = code >= FORMAT_VALID_MIN;
    const bool memory_valid = FORMAT_CODE(fw) >= FORMAT_VALID_MIN;
    if ((code != FORMAT_EMPTY && !coprocessor_valid) || (memory_valid && (len & 3u))) {
        cp_format_error(r);
        return r->cycles;
    }

    const unsigned body = coprocessor_valid ? len : 0u;
    for (unsigned i = 0; i < body / 4u; i++) {
        const uint32_t v = cpu_read_long(cpu, m.addr + FRAME_HEADER_BYTES + 4u * i);

        if (!cp_memory_ok(r))
            return r->cycles;
        if (!cp_cir_write(r, CIR_OPERAND, CIR_SIZE_LONG, v, false))
            return r->cycles;
    }
    if (m.reg >= 0)
        m.delta = (int32_t)(FRAME_HEADER_BYTES + 4u * (body / 4u)); /* 転送した long の分 */
    cp_commit_memory(cpu, &m);
    return CP_NOMINAL_CYCLES;
}

/* オペレーションワードの形式と EA の検査（CIR に触れる前。不正なら F-line）。 */
static bool cp_opword_is_valid(uint16_t opword) {
    const unsigned type = OP_TYPE(opword), ea = OP_EA(opword);

    switch (type) {
    case CP_TYPE_GEN:
    case CP_TYPE_BCC_WORD:
    case CP_TYPE_BCC_LONG:
        return true;
    case CP_TYPE_COND:
        switch (cond_kind(opword)) {
        case COND_DBCC:
        case COND_TRAPCC:
            return true;
        default:
            return ea_is_data_alterable((int)ea);
        }
    case CP_TYPE_SAVE:
        return ea_is_control_alterable((int)ea) || OP_EA_MODE(ea) == 4u;
    case CP_TYPE_RESTORE:
        return ea_is_control((int)ea) || OP_EA_MODE(ea) == 3u;
    default:
        return false;
    }
}

static int cp_execute(cp_run_t *r, bool resume) {
    switch (OP_TYPE(r->opword)) {
    case CP_TYPE_GEN:
        return cp_run_general(r, resume);
    case CP_TYPE_SAVE:
        return cp_run_save(r);
    case CP_TYPE_RESTORE:
        return cp_run_restore(r);
    default:
        return cp_run_conditional(r, resume);
    }
}

static int op_coproc(mc68k_state_t *cpu, uint16_t opcode) {
    cp_run_t r = {.cpu = cpu, .opword = opcode, .cpid = OP_CPID(opcode)};
    const unsigned type = OP_TYPE(opcode);

    /* CIR に触れる前に分かる無効な形式は F-line。cpSAVE/cpRESTORE
     * のユーザモードは、コプロセッサの有無を 知る前（CIR に触れる前）に特権違反になる（[UM20
     * §7.5.2.3]）。そのあと、コプロセッサが無ければ F-line（優先3、設計書 §3.1） */
    if (type >= CP_TYPE_COUNT_VALID || !cp_opword_is_valid(opcode)) {
        cp_fline(&r);
        return r.cycles;
    }
    if ((type == CP_TYPE_SAVE || type == CP_TYPE_RESTORE) && !(cpu->sr & SR_S)) {
        cp_privilege_violation(&r);
        return r.cycles;
    }
    if (!port_present(cpu, r.cpid)) {
        cp_fline(&r);
        return r.cycles;
    }
    return cp_execute(&r, false);
}

int coproc_resume(mc68k_state_t *cpu, uint16_t opword, uint32_t instr_pc, uint32_t ea) {
    cp_run_t r = {.cpu = cpu, .opword = opword, .cpid = OP_CPID(opword), .ea = ea};

    /* 再開できないフレーム（オペレーションワードが cpID 1..7 の有効な F-line
     * でない、cpSAVE/cpRESTORE、 コプロセッサが無い）は、従来どおり復帰だけをする（format 9
     * を取り出すだけだった既存の RTE の挙動を保つ）。 */
    if ((opword & 0xF000u) != 0xF000u || r.cpid == 0u || OP_TYPE(opword) >= CP_TYPE_SAVE ||
        !port_present(cpu, r.cpid) || !cp_opword_is_valid(opword))
        return 0;
    /* 再開する対話の命令アドレスはフレームのもの。再開しない場合に instr_pc を変えないのは、RTE
     * 自身の アドレスがトレースフレームに載るため（[UM20 Table 6-5]） */
    cpu->instr_pc = instr_pc;
    return cp_execute(&r, true);
}

void register_coproc_ops(op_handler_t *table) {
    for (unsigned cpid = 1; cpid <= 7; cpid++)
        for (unsigned type = 0; type < CP_TYPE_COUNT_VALID; type++)
            for (unsigned ea = 0; ea < 64; ea++)
                table[0xF000u | (cpid << 9) | (type << 6) | ea] = op_coproc;
}
