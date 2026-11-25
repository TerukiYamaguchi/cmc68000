/*
 * ea_helpers.h — decoded effective-address timing descriptor (internal)
 *
 * 68020/68030 のタイミング表は、mode 6／mode 7,reg 3 を共有するフル拡張語形式を区別する。
 * mode/reg/size だけを渡すと、cycles.c が表の行を選ぶ前にその情報を失う。
 */
#pragma once

#include <stdint.h>

typedef enum {
    EA_EXT_NONE = 0,
    EA_EXT_BRIEF,
    EA_EXT_FULL,
} ea_ext_form_t;

struct ea_desc {
    uint8_t mode;             /* 0..7 */
    uint8_t reg;              /* 0..7 */
    uint8_t size;             /* operand size: 1, 2, or 4 bytes */
    uint8_t ext_form;         /* ea_ext_form_t */
    uint8_t bd_size;          /* full: 0=null, 1=word, 2=long */
    uint8_t od_size;          /* full: 0=none, 1=null, 2=word, 3=long */
    uint8_t iis;              /* full: extension-word IIS field (0..7) */
    uint8_t base_suppressed;  /* full: BS is set */
    uint8_t index_suppressed; /* full: IS is set */
};
