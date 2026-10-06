#include "capsule.h"

/* The fixed callback table is owned by the sole parked-worker controller. */
__attribute__((visibility("hidden"))) void ds_profile_enter(uint32_t id) {
    ds_imports *table=*(ds_imports * volatile *)DS_IMPORT_SLOT;
    table->profile_enter(id);
}
__attribute__((visibility("hidden"))) void ds_profile_exit(uint32_t id) {
    ds_imports *table=*(ds_imports * volatile *)DS_IMPORT_SLOT;
    table->profile_exit(id);
}
