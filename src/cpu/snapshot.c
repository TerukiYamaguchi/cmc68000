/* snapshot.c — CPU の停止中デバッグ用メモリ内 snapshot。 */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "cpu_internal.h"
#include "mc68000/cpu.h"

typedef struct {
    size_t offset;
    size_t size;
} snapshot_exclude_range_t;

struct mc68k_snapshot {
    mc68k_state_t image;
    mc68k_fpu040_t *fpu040_image;
    mc68k_fpu_snapshot_t *fpu_image;
    mc68k_exception_frame_origin_t *origins;
    size_t origin_count;
};

struct mc68k_restore_plan {
    mc68k_exception_frame_origin_t *origins;
};

static int snapshot_alloc_fail_after = -1;

static void *snapshot_calloc(size_t count, size_t size) {
    if (snapshot_alloc_fail_after == 0)
        return NULL;
    if (snapshot_alloc_fail_after > 0)
        --snapshot_alloc_fail_after;
    return calloc(count, size);
}

static void *snapshot_malloc(size_t size) {
    return snapshot_calloc(1, size);
}

void cpu_snapshot_test_fail_alloc_after(int count) {
    snapshot_alloc_fail_after = count;
}

#define EXCLUDE(member) {offsetof(mc68k_state_t, member), sizeof(((mc68k_state_t *)0)->member)}

/* 配線と観測者設定は復元しない。offset 順を保つ。 */
static const snapshot_exclude_range_t snapshot_excludes[] = {
    EXCLUDE(model),
    EXCLUDE(fpu_config),
    EXCLUDE(fpu),
    EXCLUDE(fpu040),
    EXCLUDE(cyc),
    EXCLUDE(ifast_gen),
    EXCLUDE(irq_level_fn),
    EXCLUDE(irq_level_ctx),
    EXCLUDE(bus),
    EXCLUDE(op_table),
    EXCLUDE(entry_probe_saved_table),
    EXCLUDE(entry_probe_pcs),
    EXCLUDE(entry_probe_count),
    EXCLUDE(entry_probe_fn),
    EXCLUDE(entry_probe_ctx),
    EXCLUDE(entry_probe_active),
    EXCLUDE(trap_fn),
    EXCLUDE(trap_ctx),
    EXCLUDE(pc_ring),
    EXCLUDE(pc_ring_pos),
    EXCLUDE(pc_ring_count),
    EXCLUDE(pc_ring_on),
    EXCLUDE(insn_seq),
    EXCLUDE(reg_ring),
    EXCLUDE(reg_ring_pos),
    EXCLUDE(reg_ring_count),
    EXCLUDE(reg_ring_on),
    EXCLUDE(badmovea_log_on),
    EXCLUDE(badaddr_log_on),
    EXCLUDE(pctrace1024_on),
    EXCLUDE(pctrace1024_count),
    EXCLUDE(badjump_log_on),
    EXCLUDE(badjump_dumped),
    EXCLUDE(badori_log_on),
    EXCLUDE(badori_dumped),
    EXCLUDE(log_fn),
    EXCLUDE(log_ctx),
    EXCLUDE(last_exception_vector),
    EXCLUDE(stop_reason),
    EXCLUDE(peek_byte),
    EXCLUDE(peek_word),
    EXCLUDE(peek_long),
    EXCLUDE(peek_ctx),
    EXCLUDE(breakpoints),
    EXCLUDE(exc_break_mask),
    EXCLUDE(watchpoints),
    EXCLUDE(wp_hit_id),
    EXCLUDE(wp_hit_old_value),
    EXCLUDE(wp_hit_new_value),
    EXCLUDE(call_ring),
    EXCLUDE(call_ring_pos),
    EXCLUDE(call_ring_count),
    EXCLUDE(fault_ring),
    EXCLUDE(fault_ring_pos),
    EXCLUDE(fault_ring_count),
    EXCLUDE(fault_ring_on),
    EXCLUDE(exception_frame_origins),
};

static void status_set(mc68k_snapshot_status_t *status, mc68k_snapshot_status_t value) {
    if (status)
        *status = value;
}

static void copy_except(void *dst, const void *src) {
    size_t pos = 0;
    for (size_t i = 0; i < sizeof(snapshot_excludes) / sizeof(snapshot_excludes[0]); ++i) {
        const snapshot_exclude_range_t *range = &snapshot_excludes[i];
        memcpy((char *)dst + pos, (const char *)src + pos, range->offset - pos);
        pos = range->offset + range->size;
    }
    memcpy((char *)dst + pos, (const char *)src + pos, sizeof(mc68k_state_t) - pos);
}

bool cpu_snapshot_ranges_valid(void) {
    size_t end = 0;
    for (size_t i = 0; i < sizeof(snapshot_excludes) / sizeof(snapshot_excludes[0]); ++i) {
        const snapshot_exclude_range_t *range = &snapshot_excludes[i];
        if (range->offset < end || range->offset > sizeof(mc68k_state_t) ||
            range->size > sizeof(mc68k_state_t) - range->offset)
            return false;
        end = range->offset + range->size;
    }
    return true;
}

static void origins_destroy(mc68k_exception_frame_origin_t *origins) {
    while (origins) {
        mc68k_exception_frame_origin_t *next = origins->next;
        free(origins);
        origins = next;
    }
}

static mc68k_exception_frame_origin_t *origins_copy(const mc68k_exception_frame_origin_t *src,
                                                    size_t *count) {
    mc68k_exception_frame_origin_t *head = NULL;
    mc68k_exception_frame_origin_t **next = &head;
    *count = 0;
    while (src) {
        mc68k_exception_frame_origin_t *node = snapshot_malloc(sizeof(*node));
        if (!node) {
            origins_destroy(head);
            return NULL;
        }
        *node = *src;
        node->next = NULL;
        *next = node;
        next = &node->next;
        src = src->next;
        ++*count;
    }
    return head;
}

mc68k_snapshot_t *mc68k_snapshot_create(const mc68k_state_t *cpu,
                                        mc68k_snapshot_status_t *status) {
    mc68k_snapshot_t *snap;

    if (!cpu) {
        status_set(status, MC68K_SNAPSHOT_BAD_ARG);
        return NULL;
    }
    snap = snapshot_calloc(1, sizeof(*snap));
    if (!snap) {
        status_set(status, MC68K_SNAPSHOT_NOMEM);
        return NULL;
    }
    memcpy(&snap->image, cpu, sizeof(snap->image));
    if (cpu->fpu040) {
        snap->fpu040_image = snapshot_malloc(sizeof(*snap->fpu040_image));
        if (!snap->fpu040_image)
            goto nomem;
        memcpy(snap->fpu040_image, cpu->fpu040, sizeof(*snap->fpu040_image));
    }
    if (cpu->fpu) {
        snap->fpu_image = mc68k_fpu_snapshot_create(cpu->fpu);
        if (!snap->fpu_image)
            goto nomem;
    }
    snap->origins = origins_copy(cpu->exception_frame_origins, &snap->origin_count);
    if (cpu->exception_frame_origins && !snap->origins)
        goto nomem;
    status_set(status, MC68K_SNAPSHOT_OK);
    return snap;
nomem:
    mc68k_snapshot_destroy(snap);
    status_set(status, MC68K_SNAPSHOT_NOMEM);
    return NULL;
}

mc68k_restore_plan_t *mc68k_snapshot_prepare(const mc68k_state_t *cpu,
                                             const mc68k_snapshot_t *snap,
                                             mc68k_snapshot_status_t *status) {
    mc68k_restore_plan_t *plan;
    size_t count;

    if (!cpu || !snap) {
        status_set(status, MC68K_SNAPSHOT_BAD_ARG);
        return NULL;
    }
    if (cpu->model != snap->image.model || cpu->fpu_config != snap->image.fpu_config ||
        (cpu->fpu == NULL) != (snap->fpu_image == NULL) ||
        (cpu->fpu040 == NULL) != (snap->fpu040_image == NULL) ||
        (cpu->fpu && !mc68k_fpu_snapshot_check(cpu->fpu, snap->fpu_image))) {
        status_set(status, MC68K_SNAPSHOT_MISMATCH);
        return NULL;
    }
    plan = snapshot_calloc(1, sizeof(*plan));
    if (!plan) {
        status_set(status, MC68K_SNAPSHOT_NOMEM);
        return NULL;
    }
    plan->origins = origins_copy(snap->origins, &count);
    if (snap->origin_count && !plan->origins) {
        free(plan);
        status_set(status, MC68K_SNAPSHOT_NOMEM);
        return NULL;
    }
    status_set(status, MC68K_SNAPSHOT_OK);
    return plan;
}

void mc68k_snapshot_commit(mc68k_state_t *cpu, const mc68k_snapshot_t *snap,
                           mc68k_restore_plan_t *plan) {
    if (!cpu || !snap || !plan)
        return;
    cpu_exception_frame_origins_clear(cpu);
    copy_except(cpu, &snap->image);
    /* atc060_ifast と ATC はスナップショットの内容に戻る。最初のフェッチで再検証させる。 */
    cpu_ifetch_fast_invalidate(cpu);
    if (cpu->fpu040)
        memcpy(cpu->fpu040, snap->fpu040_image, sizeof(*cpu->fpu040));
    if (cpu->fpu)
        mc68k_fpu_snapshot_restore(cpu->fpu, snap->fpu_image);
    cpu->exception_frame_origins = plan->origins;
    plan->origins = NULL;
    cpu->wp_hit_id = -1;
    cpu->wp_hit_old_value = 0;
    cpu->wp_hit_new_value = 0;
    cpu->stop_reason = MC68K_STOP_NONE;
    cpu->last_exception_vector = 0;
    free(plan);
}

void mc68k_snapshot_plan_destroy(mc68k_restore_plan_t *plan) {
    if (!plan)
        return;
    origins_destroy(plan->origins);
    free(plan);
}

void mc68k_snapshot_destroy(mc68k_snapshot_t *snap) {
    if (!snap)
        return;
    free(snap->fpu040_image);
    mc68k_fpu_snapshot_destroy(snap->fpu_image);
    origins_destroy(snap->origins);
    free(snap);
}
