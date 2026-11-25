add_subdirectory(diff)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_executable(test_bus   memory/test_bus.c)
add_executable(test_move  cpu/test_move.c)
add_executable(test_alu    cpu/test_alu.c)
add_executable(test_branch cpu/test_branch.c)
add_executable(test_trap   cpu/test_trap.c)
add_executable(test_misc   cpu/test_misc.c)
add_executable(test_irq    cpu/test_irq.c)
add_executable(test_extra  cpu/test_extra.c)
add_executable(test_extra2  cpu/test_extra2.c)
add_executable(test_missing cpu/test_missing.c)
add_executable(test_exg     cpu/test_exg.c)
add_executable(test_lea_full cpu/test_lea_full.c)
add_executable(test_bus_error cpu/test_bus_error.c)
add_executable(test_privilege cpu/test_privilege.c)
add_executable(test_trace     cpu/test_trace.c)
add_executable(test_spurious_irq cpu/test_spurious_irq.c)
add_executable(test_irq_pull cpu/test_irq_pull.c)
add_executable(test_debug_peek cpu/test_debug_peek.c)
add_executable(test_debug_bp cpu/test_debug_bp.c)
add_executable(test_debug_run cpu/test_debug_run.c)
add_executable(test_entry_probe cpu/test_entry_probe.c)
add_executable(test_disasm cpu/test_disasm.c)
add_executable(test_debug_step7 cpu/test_debug_step7.c)
add_executable(test_pc_ring cpu/test_pc_ring.c)
add_executable(test_ring_read cpu/test_ring_read.c)
add_executable(test_fault_ring cpu/test_fault_ring.c)
add_executable(test_badaddr_log cpu/test_badaddr_log.c)
add_executable(test_invalid_ea cpu/test_invalid_ea.c)
add_executable(test_line_a cpu/test_line_a.c)
add_executable(test_op_table cpu/test_op_table.c)
add_executable(test_68010 cpu/test_68010.c)
add_executable(test_68020_ea cpu/test_68020_ea.c)
add_executable(test_68020_misalign cpu/test_68020_misalign.c)
add_executable(test_68020_misc cpu/test_68020_misc.c)
add_executable(test_68020_arith cpu/test_68020_arith.c)
add_executable(test_68020_bitfield cpu/test_68020_bitfield.c)
add_executable(test_68020_decode_matrix cpu/test_68020_decode_matrix.c)
add_executable(test_68060 cpu/test_68060.c)
add_executable(test_68060_a cpu/test_68060_a.c)
add_executable(test_68060_mmu cpu/test_68060_mmu.c)
add_executable(test_68060_mmu_walk cpu/test_68060_mmu_walk.c)
add_executable(test_68060_ifetch_fast cpu/test_68060_ifetch_fast.c)
add_executable(test_68060_ifetch_line cpu/test_68060_ifetch_line.c)
add_executable(test_68060_data_fast cpu/test_68060_data_fast.c)
add_executable(test_68060_dcache_hit cpu/test_68060_dcache_hit.c)
add_executable(test_page_map cpu/test_page_map.c)
add_executable(test_68060_c cpu/test_68060_c.c)
add_executable(test_68060_d cpu/test_68060_d.c)
add_executable(test_68060_e cpu/test_68060_e.c)
add_executable(test_68060_timing cpu/test_68060_timing.c)
add_executable(test_68060_timing_expect cpu/test_68060_timing_expect.c)
add_executable(test_disasm_68060 cpu/test_disasm_68060.c)
add_executable(test_68ec030 cpu/test_68ec030.c)
add_executable(test_68030_mmu_regs cpu/test_68030_mmu_regs.c)
add_executable(test_68030_translate cpu/test_68030_translate.c)
add_executable(test_68030_mmu_boundaries cpu/test_68030_mmu_boundaries.c)
add_executable(test_68030_atc_control cpu/test_68030_atc_control.c)
add_executable(test_68030_fline cpu/test_68030_fline.c)
add_executable(test_68030_fault cpu/test_68030_fault.c)
add_executable(test_68040_exception cpu/test_68040_exception.c)
add_executable(test_68020_fetch_fault_usp cpu/test_68020_fetch_fault_usp.c)
add_executable(test_exception_t0 cpu/test_exception_t0.c)
add_executable(test_68020_restart_faults cpu/test_68020_restart_faults.c)
add_executable(test_fetch_word_fault_stop cpu/test_fetch_word_fault_stop.c)
add_executable(test_68030_debug cpu/test_68030_debug.c)
add_executable(test_68040_mmu cpu/test_68040_mmu.c)
add_executable(test_68040_cache cpu/test_68040_cache.c)
add_executable(test_68040_table_walk_cache cpu/test_68040_table_walk_cache.c)
add_executable(test_phase2b_contract cpu/test_phase2b_contract.c)
add_executable(test_phase2c_contract cpu/test_phase2c_contract.c)
add_executable(test_phase2d_contract cpu/test_phase2d_contract.c)
add_executable(test_phase2e_contract cpu/test_phase2e_contract.c)
add_executable(test_phase3_contract cpu/test_phase3_contract.c)
add_executable(test_conformance_basic cpu/test_conformance_basic.c)
add_executable(test_conformance_timing cpu/test_conformance_timing.c)
add_executable(test_030_cache_timing cpu/test_030_cache_timing.c)
add_executable(test_executed_cycles cpu/test_executed_cycles.c)
add_executable(test_bus_access_cycles cpu/test_bus_access_cycles.c)
add_executable(test_snapshot cpu/test_snapshot.c)
add_executable(test_cycle_table cpu/test_cycle_table.c)
add_executable(test_68040_timing_coverage cpu/test_68040_timing_coverage.c)
add_executable(test_68040_timing_expect cpu/test_68040_timing_expect.c)
add_executable(test_cycle_dbcc cpu/test_cycle_dbcc.c)
add_executable(test_cycle_golden cpu/test_cycle_golden.c)
add_executable(test_cycle_phase1a_real cpu/test_cycle_phase1a_real.c)
add_executable(test_cycle_ea cpu/test_cycle_ea.c)
add_executable(test_cycle_phase2b cpu/test_cycle_phase2b.c)
add_executable(test_cycle_phase2c cpu/test_cycle_phase2c.c)
add_executable(test_cycle_phase2c_models cpu/test_cycle_phase2c_models.c)
add_executable(test_cycle_phase3 cpu/test_cycle_phase3.c)

foreach(t test_bus test_move test_alu test_branch test_trap test_misc test_irq test_extra test_extra2 test_missing test_exg test_lea_full test_bus_error test_privilege test_trace test_spurious_irq test_irq_pull test_debug_peek test_debug_bp test_debug_run test_entry_probe test_disasm test_debug_step7 test_pc_ring test_ring_read test_fault_ring test_badaddr_log test_invalid_ea test_line_a test_op_table test_68010 test_68020_ea test_68020_misalign test_68020_misc test_68020_arith test_68020_bitfield test_68020_decode_matrix test_68060 test_68060_a test_68060_mmu test_68060_mmu_walk test_68060_ifetch_fast test_68060_ifetch_line test_68060_data_fast test_68060_dcache_hit test_page_map test_68060_c test_68060_d test_68060_e test_68060_timing test_disasm_68060 test_68ec030 test_68030_mmu_regs test_68030_translate test_68030_mmu_boundaries test_68030_atc_control test_68030_fline test_68030_fault test_68040_exception test_68020_fetch_fault_usp test_exception_t0 test_68020_restart_faults test_fetch_word_fault_stop test_68030_debug test_68040_mmu test_68040_cache test_phase2b_contract test_phase2c_contract test_phase2d_contract test_phase2e_contract test_phase3_contract test_conformance_basic test_conformance_timing test_030_cache_timing test_executed_cycles test_bus_access_cycles test_snapshot test_cycle_table test_cycle_dbcc test_cycle_golden test_cycle_phase1a_real test_cycle_ea test_cycle_phase2c test_cycle_phase2c_models)
    target_link_libraries(${t} mc68000)
    target_include_directories(${t} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME ${t} COMMAND ${t})
endforeach()

target_link_libraries(test_68040_timing_coverage mc68000)
target_include_directories(test_68040_timing_coverage PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(test_68040_timing_expect mc68000)
target_include_directories(test_68040_timing_expect PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(test_68060_timing_expect mc68000)
target_include_directories(test_68060_timing_expect PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(test_cycle_phase3 mc68000)
target_include_directories(test_cycle_phase3 PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME test_cycle_phase3 COMMAND test_cycle_phase3)

target_link_libraries(test_68040_table_walk_cache mc68000)
target_include_directories(test_68040_table_walk_cache PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME test_68040_table_walk_cache COMMAND test_68040_table_walk_cache)

target_link_libraries(test_cycle_phase2b mc68000)
target_include_directories(test_cycle_phase2b PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME test_cycle_phase2b COMMAND test_cycle_phase2b)

add_test(NAME test_opcode_closure
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check_opcode_rules.py
            --check-classification ${CMAKE_SOURCE_DIR}/tests/data/cpu/opcode_classification.csv)
set_tests_properties(test_opcode_closure PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_test(NAME test_ea_categories
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check_ea_categories.py)
set_tests_properties(test_ea_categories PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_test(NAME test_cycle_inherit
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check_cycle_inherit.py
            $<TARGET_FILE:test_cycle_table>)

add_test(NAME test_68040_timing_coverage
    COMMAND test_68040_timing_coverage ${CMAKE_SOURCE_DIR}/tests/data/cpu/mc68040_cycle_classification.csv)

add_test(NAME test_68040_timing_expect
    COMMAND test_68040_timing_expect ${CMAKE_SOURCE_DIR}/tests/data/cpu/mc68040_cycle_expectations.csv
            ${CMAKE_SOURCE_DIR}/tests/data/cpu/mc68040_cycle_raw.csv)

add_test(NAME test_68060_timing_expect
    COMMAND test_68060_timing_expect ${CMAKE_SOURCE_DIR}/tests/data/cpu/mc68060_cycle_expectations.csv)

add_test(NAME test_68040_cycle_data
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check_68040_cycle_data.py)

add_test(NAME test_68060_cycle_data
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check_68060_cycle_data.py)

add_test(NAME test_design_060_traceability
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/check_design_060_traceability.py)
set_tests_properties(test_design_060_traceability PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# FPU共通コア。SoftFloat の内部関数・グローバルを直接検査するため、
# SoftFloat のインクルードパスとコンパイル定義を mc68000 と揃える。
add_executable(test_fpu_core fpu/test_fpu_core.c)
target_link_libraries(test_fpu_core mc68000)
target_include_directories(test_fpu_core PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_include_directories(test_fpu_core SYSTEM PRIVATE ${MC68K_SOFTFLOAT_INCLUDE_DIRS})
target_compile_definitions(test_fpu_core PRIVATE ${MC68K_SOFTFLOAT_DEFINITIONS}
    MC68K_FPU_VECTOR_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fpu/vectors")
add_test(NAME test_fpu_core COMMAND test_fpu_core)
add_test(NAME test_fpu_vectors_fresh
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/fpu/gen_fpu_core_vectors.py --check)

# コプロセッサモデル。公開 API だけを使う。
add_executable(test_fpu_disasm fpu/test_fpu_disasm.c)
target_link_libraries(test_fpu_disasm mc68000)
add_test(NAME test_fpu_disasm COMMAND test_fpu_disasm)
add_executable(test_fpu_model_dialog fpu/test_fpu_model_dialog.c)
target_link_libraries(test_fpu_model_dialog mc68000)
add_test(NAME test_fpu_model_dialog COMMAND test_fpu_model_dialog)
add_executable(test_fpu_model_transition fpu/test_fpu_model_transition.c)
target_link_libraries(test_fpu_model_transition mc68000)
target_include_directories(test_fpu_model_transition PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(test_fpu_model_transition PRIVATE
    MC68K_TRANSITION_CSV="${CMAKE_SOURCE_DIR}/tests/data/fpu/fpu2_take_transition.csv")
add_test(NAME test_fpu_model_transition COMMAND test_fpu_model_transition)
add_executable(test_fpu_model_restore fpu/test_fpu_model_restore.c)
target_link_libraries(test_fpu_model_restore mc68000)
target_include_directories(test_fpu_model_restore PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(test_fpu_model_restore PRIVATE
    MC68K_RESTORE_CSV="${CMAKE_SOURCE_DIR}/tests/data/fpu/fpu2_restore_transition.csv")
add_test(NAME test_fpu_model_restore COMMAND test_fpu_model_restore)
add_executable(test_fpu_model_protocol fpu/test_fpu_model_protocol.c)
target_link_libraries(test_fpu_model_protocol mc68000)
target_include_directories(test_fpu_model_protocol PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(test_fpu_model_protocol PRIVATE
    MC68K_MATRIX_CSV="${CMAKE_SOURCE_DIR}/tests/data/fpu/fpu2_protocol_matrix.csv")
add_test(NAME test_fpu_model_protocol COMMAND test_fpu_model_protocol)
add_executable(test_fpu_model_frame fpu/test_fpu_model_frame.c)
target_link_libraries(test_fpu_model_frame mc68000)
target_include_directories(test_fpu_model_frame PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME test_fpu_model_frame COMMAND test_fpu_model_frame)
add_executable(test_fpu_model_pc fpu/test_fpu_model_pc.c)
target_link_libraries(test_fpu_model_pc mc68000)
target_include_directories(test_fpu_model_pc PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME test_fpu_model_pc COMMAND test_fpu_model_pc)
add_executable(test_fpu_model_exceptions fpu/test_fpu_model_exceptions.c)
target_link_libraries(test_fpu_model_exceptions mc68000)
target_include_directories(test_fpu_model_exceptions PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME test_fpu_model_exceptions COMMAND test_fpu_model_exceptions)
add_test(NAME test_fpu2_tables_check
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/check_fpu2_tables.py)
add_test(NAME test_fpu040_opmodes_check
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/check_fpu040_opmodes.py)
add_test(NAME test_fpu_model_transition_fresh
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/gen_fpu2_take_transition.py --check)
add_executable(test_fpu_model_decode fpu/test_fpu_model_decode.c)
target_link_libraries(test_fpu_model_decode mc68000)
add_test(NAME test_fpu_model_decode COMMAND test_fpu_model_decode)
add_executable(test_fpu_model_abort_window fpu/test_fpu_model_abort_window.c)
target_link_libraries(test_fpu_model_abort_window mc68000)
target_include_directories(test_fpu_model_abort_window PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(test_fpu_model_abort_window PRIVATE
    MC68K_BIU_CSV="${CMAKE_SOURCE_DIR}/tests/data/fpu/fpu2_phase_biu_assignment.csv")
add_test(NAME test_fpu_model_abort_window COMMAND test_fpu_model_abort_window)
add_executable(test_fpu_model_unstarted fpu/test_fpu_model_unstarted.c)
target_link_libraries(test_fpu_model_unstarted mc68000)
target_include_directories(test_fpu_model_unstarted PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME test_fpu_model_unstarted COMMAND test_fpu_model_unstarted)
add_executable(test_fpu040_fline fpu/test_fpu040_fline.c)
target_link_libraries(test_fpu040_fline mc68000)
target_include_directories(test_fpu040_fline PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME test_fpu040_fline COMMAND test_fpu040_fline)
add_executable(test_fpu_model_040only fpu/test_fpu_model_040only.c)
target_link_libraries(test_fpu_model_040only mc68000)
target_include_directories(test_fpu_model_040only PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME test_fpu_model_040only COMMAND test_fpu_model_040only)
add_executable(test_fpu_model_config fpu/test_fpu_model_config.c)
target_link_libraries(test_fpu_model_config mc68000)
target_include_directories(test_fpu_model_config PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME test_fpu_model_config COMMAND test_fpu_model_config)
add_executable(test_fpu_snapshot fpu/test_fpu_snapshot.c)
target_link_libraries(test_fpu_snapshot mc68000)
add_test(NAME test_fpu_snapshot COMMAND test_fpu_snapshot)

# FPU共通コア Phase B。
add_executable(test_fpu_unrounded fpu/test_fpu_unrounded.c)
target_link_libraries(test_fpu_unrounded mc68000)
add_test(NAME test_fpu_unrounded COMMAND test_fpu_unrounded)

add_executable(test_fpu_arith fpu/test_fpu_arith.c)
target_link_libraries(test_fpu_arith mc68000)
add_test(NAME test_fpu_arith COMMAND test_fpu_arith)

# FPU共通コア Phase C。
add_executable(test_fpu_exceptions fpu/test_fpu_exceptions.c)
target_link_libraries(test_fpu_exceptions mc68000)
add_test(NAME test_fpu_exceptions COMMAND test_fpu_exceptions)

# FMOVECR 定数ROM（ Phase A、設計書 1b §4.4）。
add_executable(test_fpu_movecr fpu/test_fpu_movecr.c)
target_link_libraries(test_fpu_movecr mc68000)
target_include_directories(test_fpu_movecr PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_include_directories(test_fpu_movecr SYSTEM PRIVATE ${MC68K_SOFTFLOAT_INCLUDE_DIRS})
target_compile_definitions(test_fpu_movecr PRIVATE ${MC68K_SOFTFLOAT_DEFINITIONS}
    MC68K_FPU_ROM_CSV="${CMAKE_SOURCE_DIR}/tests/data/fpu/fpu_1b_rom_constants.csv")
add_test(NAME test_fpu_movecr COMMAND test_fpu_movecr)
# 超越関数の期待値は mpmath が要る。uv があれば PEP 723 の依存宣言で解決し、無ければ mpmath の要らない
# 部分（ROM 定数・IEEE 規定演算）だけを検査する（gen_fpu_1b_vectors.py --check が自動で切り替える）。
find_program(UV_EXECUTABLE uv)
if(UV_EXECUTABLE)
    add_test(NAME test_fpu_1b_fresh
        COMMAND ${UV_EXECUTABLE} run --script ${CMAKE_CURRENT_SOURCE_DIR}/fpu/gen_fpu_1b_vectors.py --check)
else()
    add_test(NAME test_fpu_1b_fresh
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/fpu/gen_fpu_1b_vectors.py --check)
endif()

# 1b Phase B1: FSQRT・FINT・FINTRZ・FMOD と超越関数。
add_executable(test_fpu_b1 fpu/test_fpu_b1.c)
target_link_libraries(test_fpu_b1 mc68000)
target_include_directories(test_fpu_b1 PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_include_directories(test_fpu_b1 SYSTEM PRIVATE ${MC68K_SOFTFLOAT_INCLUDE_DIRS})
target_compile_definitions(test_fpu_b1 PRIVATE ${MC68K_SOFTFLOAT_DEFINITIONS}
    MC68K_FPU_VECTOR_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fpu/vectors")
add_test(NAME test_fpu_b1 COMMAND test_fpu_b1)

# 1b Phase B2: FREM・FSGLMUL・FSGLDIV・FGETEXP・FGETMAN・FSCALE・FSINCOS・FETOXM1・FLOGNP1・FTWOTOX・FTENTOX。
add_executable(test_fpu_b2 fpu/test_fpu_b2.c)
target_link_libraries(test_fpu_b2 mc68000)
target_include_directories(test_fpu_b2 PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_include_directories(test_fpu_b2 SYSTEM PRIVATE ${MC68K_SOFTFLOAT_INCLUDE_DIRS})
target_compile_definitions(test_fpu_b2 PRIVATE ${MC68K_SOFTFLOAT_DEFINITIONS}
    MC68K_FPU_VECTOR_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fpu/vectors")
add_test(NAME test_fpu_b2 COMMAND test_fpu_b2)

# 1b Phase C: packed decimal の入出力。
add_executable(test_fpu_c fpu/test_fpu_c.c)
target_link_libraries(test_fpu_c mc68000)
target_compile_definitions(test_fpu_c PRIVATE
    MC68K_FPU_VECTOR_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fpu/vectors")
add_test(NAME test_fpu_c COMMAND test_fpu_c)

# FPU-3: 68020/68030 のコプロセッサインタフェース。
add_executable(test_fpcp_model fpu/test_fpcp_model.c)
target_link_libraries(test_fpcp_model mc68000)
target_include_directories(test_fpcp_model PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME test_fpcp_model COMMAND test_fpcp_model)
add_executable(test_fpcp_host fpu/test_fpcp_host.c)
target_link_libraries(test_fpcp_host mc68000)
target_include_directories(test_fpcp_host PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME test_fpcp_host COMMAND test_fpcp_host)

add_executable(test_fpu040_exec fpu/test_fpu040_exec.c)
target_link_libraries(test_fpu040_exec mc68000)
target_include_directories(test_fpu040_exec PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME test_fpu040_exec COMMAND test_fpu040_exec)

add_executable(test_fpu040_frame fpu/test_fpu040_frame.c)
target_link_libraries(test_fpu040_frame mc68000)
target_include_directories(test_fpu040_frame PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME test_fpu040_frame COMMAND test_fpu040_frame)

# 全数・全ビットの試験。
add_executable(test_fpu040_gaps fpu/test_fpu040_gaps.c)
target_link_libraries(test_fpu040_gaps mc68000)
target_include_directories(test_fpu040_gaps PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME test_fpu040_gaps COMMAND test_fpu040_gaps)
