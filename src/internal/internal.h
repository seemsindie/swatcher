#ifndef SWATCHER_INTERNAL_H
#define SWATCHER_INTERNAL_H

#include "swatcher_types.h"
#include "../internal/alloc.h"

/* Override uthash to use the custom allocator */
#define uthash_malloc(sz)      sw_malloc(sz)
#define uthash_free(ptr, sz)   do { (void)(sz); sw_free(ptr); } while(0)

#include "uthash.h"
#include "../platform/platform.h"
#include "../backend/backend.h"
#include "../core/pattern.h"

#include <stdlib.h>
#include <string.h>

typedef struct swatcher_target_internal {
    swatcher_target *target;
    char *path;              /* hash key */
    void *backend_data;
    UT_hash_handle hh_global;
    sw_compiled_patterns *compiled_callback;
    sw_compiled_patterns *compiled_watch;
    sw_compiled_patterns *compiled_ignore;
} swatcher_target_internal;

typedef struct swatcher_internal {
    const swatcher_backend *backend;
    void *backend_data;
    sw_mutex *mutex;
    sw_thread *thread;
    swatcher_target_internal *targets; /* uthash head */
} swatcher_internal;

#define SW_INTERNAL(sw) ((swatcher_internal *)(sw)->_internal)
#define SW_TARGET_INTERNAL(t) ((swatcher_target_internal *)(t)->_internal)

/* Hash helpers */
static inline swatcher_target_internal *sw_find_target_internal(swatcher *sw, const char *path)
{
    swatcher_target_internal *found = NULL;
    HASH_FIND(hh_global, SW_INTERNAL(sw)->targets, path, strlen(path), found);
    return found;
}

static inline void sw_add_target_internal(swatcher *sw, swatcher_target_internal *ti)
{
    HASH_ADD_KEYPTR(hh_global, SW_INTERNAL(sw)->targets, ti->path, strlen(ti->path), ti);
}

static inline void sw_remove_target_internal(swatcher *sw, swatcher_target_internal *ti)
{
    HASH_DELETE(hh_global, SW_INTERNAL(sw)->targets, ti);
}

/* Forward declarations for target internal create/destroy */
swatcher_target_internal *sw_target_internal_create(swatcher_target *target);
void sw_target_internal_destroy(swatcher_target_internal *ti);

#endif /* SWATCHER_INTERNAL_H */
