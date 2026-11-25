/* MC68060 の整数部差分。未実装命令は hardware-equivalent vector 61 に渡す。 */
#include "cpu_internal.h"

enum { MC68K_VECTOR_UNIMPLEMENTED_INTEGER = 61 };

/* LPSTOP broadcast cycle の address（[060UM §7.8.2.1、App.D D-19]。Figure 7-32 は $FFFFFFFF
 * で食い違う。設計書 §11） */
#define LPSTOP_BROADCAST_ADDRESS UINT32_C(0xFFFFFFFE)

static int op_68060_unimplemented_integer(mc68k_state_t *cpu, uint16_t opcode) {
    (void)opcode;
    return cpu_dispatch_exception(cpu, cpu->instr_pc, MC68K_VECTOR_UNIMPLEMENTED_INTEGER, 34);
}

static int op_68060_lpstop(mc68k_state_t *cpu, uint16_t opcode) {
    (void)opcode;
    /* 拡張語の判定（F-line、Table 8-3 の 2.1）は privilege violation（2.3）より先。 */
    if (cpu_fetch_word(cpu) != UINT16_C(0x01c0) || cpu->exec_state != CPU_NORMAL)
        return cpu_dispatch_exception(cpu, cpu->instr_pc, 11, 34);
    if (!(cpu->sr & SR_S)) /* stacked PC は命令先頭 */
        return cpu_dispatch_exception(cpu, cpu->instr_pc, 8, CYC(cpu, CYC_EXC_PRIVILEGE));
    uint16_t sr = cpu_fetch_word(cpu);
    if (cpu->exec_state != CPU_NORMAL)
        return 4;
    if (!(sr & SR_S))
        return cpu_privilege_violation(cpu);
    if (cpu->bus.cpu_space_write)
        (void)cpu_bus_cpu_space_write(cpu, MC68K_FC_CPU_SPACE, LPSTOP_BROADCAST_ADDRESS, 2, sr);
    bool traced = (cpu->sr & (SR_T1 | SR_T0)) != 0;
    cpu_set_sr(cpu, sr);
    cpu->flow_changed = true;
    if (!traced)
        cpu->exec_state = CPU_STOPPED;
    return 4;
}

void register_68060_ops(op_handler_t *table) {
    register_68060_fpu_ops(table); /* cpID 1。cpID 2/3 は下の MMU/cache が所有 */
    table[0xf800] = op_68060_lpstop;
    register_68060_mmu_ops(table);
    register_68060_cache_ops(table);
    for (int dn = 0; dn < 8; ++dn) {
        for (int an = 0; an < 8; ++an) {
            uint16_t base = (uint16_t)((dn << 9) | 0x08 | an);
            for (int opmode = 4; opmode <= 7; ++opmode)
                table[base | (opmode << 6)] = op_68060_unimplemented_integer;
        }
    }

    table[0x0CFC] = op_68060_unimplemented_integer;
    table[0x0EFC] = op_68060_unimplemented_integer;
    for (int ea = 0; ea < 64; ++ea) {
        if (ea_is_control(ea)) {
            table[0x00C0 | ea] = op_68060_unimplemented_integer;
            table[0x02C0 | ea] = op_68060_unimplemented_integer;
            table[0x04C0 | ea] = op_68060_unimplemented_integer;
        }
    }
}
