set(MUSASHI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/musashi)
set(MUSASHI_GEN ${CMAKE_CURRENT_BINARY_DIR}/musashi_gen)
file(MAKE_DIRECTORY ${MUSASHI_GEN})

# Build the opcode-table generator
add_executable(m68kmake_tool ${MUSASHI_DIR}/m68kmake.c)
set_target_properties(m68kmake_tool PROPERTIES OUTPUT_NAME "m68kmake")

# Run m68kmake to emit m68kops.c / m68kops.h (v4.60+ generates only these two)
add_custom_command(
    OUTPUT
        ${MUSASHI_GEN}/m68kops.c
        ${MUSASHI_GEN}/m68kops.h
    COMMAND m68kmake_tool ${MUSASHI_GEN} ${MUSASHI_DIR}/m68k_in.c
    DEPENDS m68kmake_tool ${MUSASHI_DIR}/m68k_in.c
    COMMENT "Generating Musashi opcode tables"
    VERBATIM
)
add_custom_target(musashi_gen_files DEPENDS
    ${MUSASHI_GEN}/m68kops.c ${MUSASHI_GEN}/m68kops.h
)

# Musashi compiled as a single translation unit (musashi_wrapper.c includes
# softfloat + m68kcpu + generated handlers in dependency order).
add_library(musashi STATIC musashi_wrapper.c)
add_dependencies(musashi musashi_gen_files)
target_include_directories(musashi PUBLIC
    ${MUSASHI_DIR}      # m68k.h, m68kcpu.h, m68kconf.h, m68kfpu.c, m68kmmu.h
    ${MUSASHI_GEN}      # m68kops.h + generated .c files
)
target_compile_definitions(musashi PRIVATE
    LSB_FIRST=1             # little-endian (macOS x86-64 / Apple Silicon)
    M68K_EMULATE_010=M68K_OPT_OFF
    M68K_EMULATE_EC020=M68K_OPT_OFF
    M68K_EMULATE_020=M68K_OPT_OFF
    M68K_EMULATE_030=M68K_OPT_OFF
    M68K_EMULATE_040=M68K_OPT_OFF
    M68K_EMULATE_PMMU=M68K_OPT_OFF
)
# -fno-common: Musashi の暫定定義（m68ki_cpu など）が __common に集まって 32KB 揃えを要求し、
# arm64 macOS の ld が "reducing alignment of section __DATA,__common" の警告を出すため
target_compile_options(musashi PRIVATE -w -fno-common)  # -w: suppress all Musashi warnings

# Musashi (68010有効版): cycle_diff.c専用。M68K_EMULATE_010=OFFの`musashi`とは
# コンパイル時定数が異なるため別ターゲットが必要。
add_library(musashi_010 STATIC musashi_wrapper.c)
add_dependencies(musashi_010 musashi_gen_files)
target_include_directories(musashi_010 PUBLIC
    ${MUSASHI_DIR}
    ${MUSASHI_GEN}
)
target_compile_definitions(musashi_010 PRIVATE
    LSB_FIRST=1
    M68K_EMULATE_010=M68K_OPT_ON
    M68K_EMULATE_EC020=M68K_OPT_OFF
    M68K_EMULATE_020=M68K_OPT_OFF
    M68K_EMULATE_030=M68K_OPT_OFF
    M68K_EMULATE_040=M68K_OPT_OFF
    M68K_EMULATE_PMMU=M68K_OPT_OFF
)
target_compile_options(musashi_010 PRIVATE -w -fno-common)

# Differential test executable
add_executable(test_diff diff_test.c)
target_link_libraries(test_diff mc68000 musashi)
target_include_directories(test_diff PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${MUSASHI_DIR}
)
add_test(NAME test_diff COMMAND test_diff)

# 逆アセンブラの命令長クロスチェック（全65536オペコード、 ステップ6）
add_executable(test_disasm_diff disasm_length_diff.c)
target_link_libraries(test_disasm_diff mc68000 musashi)
target_include_directories(test_disasm_diff PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${MUSASHI_DIR}
)
add_test(NAME test_disasm_diff COMMAND test_disasm_diff)

# サイクル数差分テスト。
# 68000モデルでの不変性回帰にのみ使う（ブロッキング）。68010の差分は記録するだけで
# 判定に使わない（Musashiの68010サイクル値は[DS10]と食い違うため、design書§9.2）。
add_executable(test_cycle_diff cycle_diff.c)
target_link_libraries(test_cycle_diff mc68000 musashi_010)
target_include_directories(test_cycle_diff PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${MUSASHI_DIR}
)
add_test(NAME test_cycle_diff COMMAND test_cycle_diff)

# Musashi (68020有効版) と自実装の68020列の差分を記録するツール（ 作業計画 5）。
# 判定には使わない（一次資料は[UM20]、Musashiは二次資料）ので add_test しない。
add_library(musashi_020 STATIC musashi_wrapper.c)
add_dependencies(musashi_020 musashi_gen_files)
target_include_directories(musashi_020 PUBLIC
    ${MUSASHI_DIR}
    ${MUSASHI_GEN}
)
target_compile_definitions(musashi_020 PRIVATE
    LSB_FIRST=1
    M68K_EMULATE_010=M68K_OPT_ON
    M68K_EMULATE_EC020=M68K_OPT_ON
    M68K_EMULATE_020=M68K_OPT_ON
    M68K_EMULATE_030=M68K_OPT_OFF
    M68K_EMULATE_040=M68K_OPT_OFF
    M68K_EMULATE_PMMU=M68K_OPT_OFF
)
target_compile_options(musashi_020 PRIVATE -w -fno-common)

add_executable(musashi_020_record musashi_020_record.c)
target_link_libraries(musashi_020_record mc68000 musashi_020)
target_include_directories(musashi_020_record PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${MUSASHI_DIR}
)
