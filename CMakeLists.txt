cmake_minimum_required(VERSION 3.16)
project(mc68000 C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Berkeley SoftFloat 3e（extern/berkeley-softfloat-3）。FPU共通コア（src/fpu/）が使う。
# submodule 側の s_roundPackToExtF80.c は使わず、src/fpu/softfloat_target/ の自前実装を
# 同名関数としてリンクする（docs/mc68000-internals/design/fpu.md §3.3, §3.5）。
set(SOFTFLOAT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/extern/berkeley-softfloat-3)
if(NOT EXISTS ${SOFTFLOAT_DIR}/source/include/softfloat.h)
    message(FATAL_ERROR "extern/berkeley-softfloat-3 が未取得です。"
                        "git submodule update --init extern/berkeley-softfloat-3 を実行してください")
endif()
set(MC68K_SOFTFLOAT_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/fpu/softfloat_target
    ${SOFTFLOAT_DIR}/source/8086
    ${SOFTFLOAT_DIR}/source/include
)
set(MC68K_SOFTFLOAT_DEFINITIONS SOFTFLOAT_FAST_INT64)
if(NOT MSVC)
    list(APPEND MC68K_SOFTFLOAT_DEFINITIONS THREAD_LOCAL=__thread)
endif()

# Phase B（Issue #58）: extF80_mul/extF80_div・NaN伝播はsubmodule側の同名ファイルの
# 代わりにsrc/fpu/softfloat_target/の改変コピーをリンクする（docs/mc68000-internals/design/fpu.md
# §3.2.1, §3.5）。加減算（fpu_addsub.c）は自前実装だが、128bitビット演算プリミティブ
# （add128/sub128/shiftRightJam128/shortShiftLeft128/shortShiftRightJam128）はISA非依存の
# 汎用ユーティリティなのでsubmodule側を無改変でリンクする（§3.2.2）。他はextF80_mul/div・
# roundPackが使うヘルパー（正規化・128bit乗算・逆数近似）で、いずれも無改変。
add_library(softfloat STATIC
    ${SOFTFLOAT_DIR}/source/softfloat_state.c
    ${SOFTFLOAT_DIR}/source/8086/softfloat_raiseFlags.c
    ${SOFTFLOAT_DIR}/source/s_shiftRightJam64Extra.c
    ${SOFTFLOAT_DIR}/source/s_normSubnormalExtF80Sig.c
    ${SOFTFLOAT_DIR}/source/s_countLeadingZeros64.c
    ${SOFTFLOAT_DIR}/source/s_countLeadingZeros8.c
    ${SOFTFLOAT_DIR}/source/s_add128.c
    ${SOFTFLOAT_DIR}/source/s_sub128.c
    ${SOFTFLOAT_DIR}/source/s_shiftRightJam128.c
    ${SOFTFLOAT_DIR}/source/s_shortShiftLeft128.c
    ${SOFTFLOAT_DIR}/source/s_shortShiftRightJam128.c
    ${SOFTFLOAT_DIR}/source/s_mul64To128.c
    ${SOFTFLOAT_DIR}/source/s_mul64ByShifted32To128.c
    ${SOFTFLOAT_DIR}/source/s_le128.c
    ${SOFTFLOAT_DIR}/source/s_approxRecip32_1.c
    ${SOFTFLOAT_DIR}/source/s_approxRecip_1Ks.c
    # 1b（Issue #59）の超越関数が中間精度に使う float128。extF80_to_f128/f128_to_extF80 は
    # 指数フィールド0の欠陥があるため使わない（docs/mc68000-internals/design/fpu.md §5.1-1）。
    ${SOFTFLOAT_DIR}/source/f128_add.c
    ${SOFTFLOAT_DIR}/source/f128_sub.c
    ${SOFTFLOAT_DIR}/source/f128_mul.c
    ${SOFTFLOAT_DIR}/source/f128_div.c
    ${SOFTFLOAT_DIR}/source/f128_sqrt.c
    ${SOFTFLOAT_DIR}/source/f128_lt.c
    ${SOFTFLOAT_DIR}/source/f128_to_i32.c
    ${SOFTFLOAT_DIR}/source/s_addMagsF128.c
    ${SOFTFLOAT_DIR}/source/s_subMagsF128.c
    ${SOFTFLOAT_DIR}/source/s_normRoundPackToF128.c
    ${SOFTFLOAT_DIR}/source/s_roundPackToF128.c
    ${SOFTFLOAT_DIR}/source/s_normSubnormalF128Sig.c
    ${SOFTFLOAT_DIR}/source/s_shiftRightJam128Extra.c
    ${SOFTFLOAT_DIR}/source/s_shortShiftRightJam128Extra.c
    ${SOFTFLOAT_DIR}/source/s_shortShiftRight128.c
    ${SOFTFLOAT_DIR}/source/s_mul128By32.c
    ${SOFTFLOAT_DIR}/source/s_lt128.c
    ${SOFTFLOAT_DIR}/source/s_eq128.c
    ${SOFTFLOAT_DIR}/source/s_mul128To256M.c
    ${SOFTFLOAT_DIR}/source/s_shiftRightJam64.c
    ${SOFTFLOAT_DIR}/source/s_roundToI32.c
    ${SOFTFLOAT_DIR}/source/s_approxRecipSqrt32_1.c
    ${SOFTFLOAT_DIR}/source/s_approxRecipSqrt_1Ks.c
    ${SOFTFLOAT_DIR}/source/8086/s_propagateNaNF128UI.c
    ${SOFTFLOAT_DIR}/source/8086/s_f128UIToCommonNaN.c
    ${SOFTFLOAT_DIR}/source/8086/s_commonNaNToF128UI.c
    src/fpu/softfloat_target/s_roundPackToExtF80.c
    src/fpu/softfloat_target/s_propagateNaNExtF80UI.c
    src/fpu/softfloat_target/extF80_mul.c
    src/fpu/softfloat_target/extF80_div.c
)
target_include_directories(softfloat SYSTEM PRIVATE ${MC68K_SOFTFLOAT_INCLUDE_DIRS})
target_compile_definitions(softfloat PRIVATE ${MC68K_SOFTFLOAT_DEFINITIONS})
# SoftFloat 本体は警告を付けずにビルドし、自前の改変コピーだけ警告を有効にする
if(NOT MSVC)
    set_source_files_properties(
        src/fpu/softfloat_target/s_roundPackToExtF80.c
        src/fpu/softfloat_target/s_propagateNaNExtF80UI.c
        src/fpu/softfloat_target/extF80_mul.c
        src/fpu/softfloat_target/extF80_div.c
        PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Wpedantic")
endif()

add_library(mc68000 STATIC
    src/cpu/m68000.c
    src/cpu/decode.c
    src/cpu/cycles.c
    src/cpu/timing_060.c
    src/cpu/ea_helpers.c
    src/cpu/ops_move.c
    src/cpu/ops_alu.c
    src/cpu/ops_branch.c
    src/cpu/ops_trap.c
    src/cpu/ops_logic.c
    src/cpu/ops_transfer.c
    src/cpu/ops_arith_ext.c
    src/cpu/ops_shift.c
    src/cpu/ops_system.c
    src/cpu/ops_system_ext.c
    src/cpu/ops_68020_arith.c
    src/cpu/ops_68060.c
    src/cpu/ops_68060_mmu.c
    src/cpu/ops_68020_bitfield.c
    src/cpu/ops_68020_system.c
    src/cpu/ops_68030_mmu.c
    src/cpu/ops_68040_mmu.c
    src/cpu/ops_68040_cache.c
    src/cpu/ops_fpu.c
    src/cpu/coproc.c
    src/cpu/snapshot.c
    src/memory/memory.c
    src/memory/bus.c
    src/debug/debugger.c
    src/debug/disasm.c
    src/fpu/fpu_core.c
    src/fpu/fpu_internal.c
    src/fpu/fpu_addsub.c
    src/fpu/fpu_muldiv.c
    src/fpu/fpu_absneg.c
    src/fpu/fpu_cmp.c
    src/fpu/fpu_movecr.c
    src/fpu/fpu_sqrt.c
    src/fpu/fpu_int.c
    src/fpu/fpu_mod.c
    src/fpu/fpu_sgl.c
    src/fpu/fpu_exponent.c
    src/fpu/fpu_wide.c
    src/fpu/fpu_transcend_tables.c
    src/fpu/fpu_transcend_trig.c
    src/fpu/fpu_transcend_atan.c
    src/fpu/fpu_transcend_explog.c
    src/fpu/fpu_transcend_hyp.c
    src/fpu/fpu_transcend_b2.c
    src/fpu/fpu_rom_table.c
    src/fpu/fpu_bcd.c
    src/fpu/fpu_model.c
    src/fpu/fpu_decode.c
    src/fpu/fpu_exec.c
    src/fpu/fpu_proto.c
    src/fpu/fpu_frame.c
    src/fpu/fpu_snapshot.c
)

target_include_directories(mc68000
    PUBLIC  include
    PRIVATE src
)
target_include_directories(mc68000 SYSTEM PRIVATE ${MC68K_SOFTFLOAT_INCLUDE_DIRS})
target_compile_definitions(mc68000 PRIVATE ${MC68K_SOFTFLOAT_DEFINITIONS})
target_link_libraries(mc68000 PRIVATE softfloat)

if(MSVC)
    target_compile_options(mc68000 PRIVATE /W4)
else()
    target_compile_options(mc68000 PRIVATE -Wall -Wextra -Wpedantic)
endif()

# サブモジュールとして使われる場合はテストをビルドしない
if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    enable_testing()
    add_subdirectory(tests)
endif()

if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    find_package(SDL2 QUIET)
    if(SDL2_FOUND)
        add_subdirectory(tools/vram_viewer)
        message(STATUS "SDL2 found — vram_viewer will be built")
    else()
        message(STATUS "SDL2 not found — skipping tools/vram_viewer")
    endif()
endif()
