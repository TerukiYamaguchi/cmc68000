/* fpu_snapshot.c — 外付け 68881/68882 のメモリ内 snapshot。 */
#include <stdlib.h>
#include <string.h>
#include "fpu_model.h"

struct mc68k_fpu_snapshot {
    struct mc68k_fpu image;
};

mc68k_fpu_snapshot_t *mc68k_fpu_snapshot_create(const mc68k_fpu_t *fpu) {
    mc68k_fpu_snapshot_t *snap;

    if (!fpu)
        return NULL;
    snap = malloc(sizeof(*snap));
    if (!snap)
        return NULL;
    memcpy(&snap->image, fpu, sizeof(snap->image));
    return snap;
}

bool mc68k_fpu_snapshot_check(const mc68k_fpu_t *fpu, const mc68k_fpu_snapshot_t *snap) {
    return fpu && snap && fpu->config == snap->image.config;
}

void mc68k_fpu_snapshot_restore(mc68k_fpu_t *fpu, const mc68k_fpu_snapshot_t *snap) {
    if (!mc68k_fpu_snapshot_check(fpu, snap))
        return;
    memcpy(fpu, &snap->image, sizeof(*fpu));
}

void mc68k_fpu_snapshot_destroy(mc68k_fpu_snapshot_t *snap) {
    free(snap);
}
