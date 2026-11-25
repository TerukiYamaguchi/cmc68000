/*
 * fpu_decode.c — command word の共有デコーダ。
 *
 * CIR と FPCP の両入口が同じ判定を使うため、デコードと F-line（不正 command）判定は
 * ここだけで行う。opmode→共通コア関数の対応は fpu_exec.c が持つ。
 *
 * command word の形式（[881UM] Table 4-11〜4-18）:
 *   bit15-13 オペレーションクラス
 *     000 FPm,FPn の演算       bit12-10=FPm、bit9-7=FPn、bit6-0=opmode
 *     010 <ea>,FPn の演算      bit12-10=ソース形式（7 は
 * FMOVECR）、bit9-7=FPn、bit6-0=opmode/ROM offset 011 FMOVE FPm,<ea>
 * bit12-10=宛先形式、bit9-7=FPm、bit6-0=k-factor 100 control 入力、101 control 出力
 * bit12-10=FPCR/FPSR/FPIAR、bit9-0=0 110 FMOVEM 入力、111 FMOVEM 出力
 * bit12-11=mode、bit10-8=0、bit7-0=list（動的は bit6-4=Dn）
 *
 * 一次資料の規則（[881UM] Table 4-16 の Note 2・3、p.4-130〜133、Table 4-17、Table 4-18 の Note
 * 1）:
 *  - 予約ビット（control の bit9-0、k-factor 欄の bit3-0、出力 FMOVE の P 以外の k-factor
 * 欄）は0で あるべきだが、0でなくても F-line
 * は発生しない。実装は無視する（一次資料の明記あり）。FMOVEM の bit10-8 と動的 list
 * の未使用ビットを無視するのは、4.7.1.7 の他の予約欄の扱いからの推論。 operation word の EA
 * 欄（FPm,FPn で未使用）と条件の bit15-6 は command word の外／条件命令の
 *    規則で、このデコーダの対象ではない。
 *  - control の select が000は「未定義・予約」だが、現行実装では001（FPIAR だけ）と同じ（Table
 * 4-17 Note 2）。
 *  - FMOVEM 入力（dr=0）の mode 00/01（predecrement）は F-line ではなく、FP7→FP0 の順で転送する
 *    （Table 4-18 Note 1。EA との不整合は MPU 側の問題）。
 *  - k-factor は7bit の2の補数（-64〜+63）で、descriptor には符号拡張した値を置く。
 * このデコーダが F-line（valid=false）にするのは、クラス001と未定義の opmode だけである。
 */
#include <string.h>
#include "fpu_model.h"

#define CMD_CLASS_SHIFT        13
#define CMD_SRC_SHIFT          10
#define CMD_DST_SHIFT          7
#define CMD_FIELD3             7u
#define CMD_OPMODE_MASK        0x7Fu
#define CMD_CLASS_REG_REG      0u
#define CMD_CLASS_EA_REG       2u
#define CMD_CLASS_REG_EA       3u
#define CMD_CLASS_CTRL_IN      4u
#define CMD_CLASS_CTRL_OUT     5u
#define CMD_CLASS_MOVEM_IN     6u
#define CMD_CLASS_MOVEM_OUT    7u
#define CMD_SRC_ROM            7u /* クラス010 のソース形式 7 = FMOVECR */
#define CMD_FMT_DYNAMIC_P      7u /* クラス011 の形式 7 = 動的 k-factor の packed */
#define CMD_KFACTOR_REG_SHIFT  4
#define CMD_MOVEM_MODE_SHIFT   11
#define CMD_MOVEM_MODE_MASK    3u
#define CMD_MOVEM_MODE_DYNAMIC 1u /* mode bit0 = 動的 list */
#define CMD_MOVEM_MODE_POSTINC 2u /* mode bit1 = 1 は postincrement/control */
#define CMD_LIST_MASK          0xFFu
#define CMD_K_SIGN             0x40u /* k-factor の符号 bit（7bit の2の補数） */
#define CMD_K_SIGN_EXT         0x80u
#define OPMODE_FMOVE           0x00u
#define OPMODE_FSINCOS_BASE    0x30u
#define OPMODE_FSINCOS_MASK    0x78u

/* クラス000/010 で有効な opmode。
 * Note 3 の冗長 opmode は F-line にしない規則だが、対応先が不明なため F-line
 * のまま */
static bool valid_opmode(uint8_t op) {
    static const uint8_t valid[] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x06, 0x08, 0x09, 0x0A, 0x0C, 0x0D, 0x0E, 0x0F,
        0x10, 0x11, 0x12, 0x14, 0x15, 0x16, 0x18, 0x19, 0x1A, 0x1C, 0x1D, 0x1E, 0x1F,
        0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x38, 0x3A,
    };
    size_t i;

    if ((op & OPMODE_FSINCOS_MASK) == OPMODE_FSINCOS_BASE)
        return true; /* FSINCOS: 0x30〜0x37 */
    for (i = 0; i < sizeof(valid); i++)
        if (valid[i] == op)
            return true;
    return false;
}

/* クラス010/011 の形式コード → 形式。7 は呼出し側が処理済み */
static mc68k_fpu_format_t format_from_code(unsigned code) {
    static const mc68k_fpu_format_t map[] = {
        MC68K_FPU_FORMAT_L, MC68K_FPU_FORMAT_S, MC68K_FPU_FORMAT_X, MC68K_FPU_FORMAT_P,
        MC68K_FPU_FORMAT_W, MC68K_FPU_FORMAT_D, MC68K_FPU_FORMAT_B,
    };
    return map[code];
}

/* 7bit の2の補数 */
static int8_t sign_extend_k(uint8_t low) {
    return (int8_t)((low & CMD_K_SIGN) ? (low | CMD_K_SIGN_EXT) : low);
}

static bool decode_register_list(uint16_t word, mc68k_fpu_command_t *out, bool to_external) {
    unsigned mode = (word >> CMD_MOVEM_MODE_SHIFT) & CMD_MOVEM_MODE_MASK;
    uint8_t list = (uint8_t)(word & CMD_LIST_MASK);

    out->command_class = MC68K_FPU_COMMAND_MOVEM;
    out->format = MC68K_FPU_FORMAT_X;
    out->transfer_to_external = to_external;
    out->predecrement = !(mode & CMD_MOVEM_MODE_POSTINC);
    out->source_kind = to_external ? MC68K_FPU_SOURCE_FP_REGISTER : MC68K_FPU_SOURCE_EXTERNAL;
    if (mode & CMD_MOVEM_MODE_DYNAMIC) {
        out->dynamic_register_mask = true;
        out->register_mask_data_register =
            (uint8_t)((list >> CMD_KFACTOR_REG_SHIFT) & CMD_FIELD3);
    } else {
        out->register_mask = list;
    }
    return true;
}

bool mc68k_fpu_decode_command_word(mc68k_fpu_config_t config, uint16_t word,
                                   mc68k_fpu_command_t *out) {
    unsigned cls = (unsigned)(word >> CMD_CLASS_SHIFT);
    unsigned field = (word >> CMD_SRC_SHIFT) & CMD_FIELD3;
    unsigned dst = (word >> CMD_DST_SHIFT) & CMD_FIELD3;
    uint8_t low = (uint8_t)(word & CMD_OPMODE_MASK);
    bool ok = false;

    if (!out)
        return false;
    memset(out, 0, sizeof(*out));
    out->command_word = word;
    if (config != MC68K_FPU_68881 && config != MC68K_FPU_68882)
        return false;

    switch (cls) {
    case CMD_CLASS_REG_REG:
        if (!valid_opmode(low))
            break;
        out->command_class =
            (low == OPMODE_FMOVE) ? MC68K_FPU_COMMAND_MOVE : MC68K_FPU_COMMAND_ARITHMETIC;
        out->opmode = low;
        out->dst_fp = (uint8_t)dst;
        out->source_kind = MC68K_FPU_SOURCE_FP_REGISTER;
        out->src_fp = (uint8_t)field;
        out->format = MC68K_FPU_FORMAT_X;
        if ((low & OPMODE_FSINCOS_MASK) == OPMODE_FSINCOS_BASE)
            out->secondary_dst_fp = (uint8_t)(low & CMD_FIELD3);
        ok = true;
        break;
    case CMD_CLASS_EA_REG:
        if (field == CMD_SRC_ROM) {
            out->command_class = MC68K_FPU_COMMAND_MOVE;
            out->dst_fp = (uint8_t)dst;
            out->source_kind = MC68K_FPU_SOURCE_IMMEDIATE;
            out->format = MC68K_FPU_FORMAT_X;
            out->movecr_offset = low;
            ok = true;
            break;
        }
        if (!valid_opmode(low))
            break;
        out->command_class =
            (low == OPMODE_FMOVE) ? MC68K_FPU_COMMAND_MOVE : MC68K_FPU_COMMAND_ARITHMETIC;
        out->opmode = low;
        out->dst_fp = (uint8_t)dst;
        out->source_kind = MC68K_FPU_SOURCE_EXTERNAL;
        out->format = format_from_code(field);
        if ((low & OPMODE_FSINCOS_MASK) == OPMODE_FSINCOS_BASE)
            out->secondary_dst_fp = (uint8_t)(low & CMD_FIELD3);
        ok = true;
        break;
    case CMD_CLASS_REG_EA:
        out->command_class = MC68K_FPU_COMMAND_MOVE;
        out->source_kind = MC68K_FPU_SOURCE_FP_REGISTER;
        out->src_fp = (uint8_t)dst;
        out->transfer_to_external = true;
        if (field == CMD_FMT_DYNAMIC_P) {
            out->format = MC68K_FPU_FORMAT_P;
            out->dynamic_packed_k_factor = true;
            out->packed_k_factor_data_register =
                (uint8_t)((low >> CMD_KFACTOR_REG_SHIFT) & CMD_FIELD3);
        } else {
            out->format = format_from_code(field);
            if (out->format == MC68K_FPU_FORMAT_P)
                out->packed_k_factor = sign_extend_k(low);
        }
        ok = true;
        break;
    case CMD_CLASS_CTRL_IN:
    case CMD_CLASS_CTRL_OUT:
        out->command_class = MC68K_FPU_COMMAND_MOVE_CONTROL;
        /* select 000 は FPIAR だけと同じ（Table 4-17 Note 2） */
        out->control_mask = field ? (uint8_t)field : (uint8_t)MC68K_FPU_CONTROL_FPIAR;
        out->format = MC68K_FPU_FORMAT_L;
        out->transfer_to_external = (cls == CMD_CLASS_CTRL_OUT);
        out->source_kind = (cls == CMD_CLASS_CTRL_OUT) ? MC68K_FPU_SOURCE_FP_REGISTER
                                                       : MC68K_FPU_SOURCE_EXTERNAL;
        ok = true;
        break;
    case CMD_CLASS_MOVEM_IN:
        ok = decode_register_list(word, out, false);
        break;
    case CMD_CLASS_MOVEM_OUT:
        ok = decode_register_list(word, out, true);
        break;
    default:
        break;
    }
    if (!ok) {
        uint16_t keep = word;
        memset(out, 0, sizeof(*out));
        out->command_word = keep;
        return false;
    }
    out->valid = true;
    return true;
}
