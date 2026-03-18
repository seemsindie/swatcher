#ifndef SWATCHER_ALLOC_H
#define SWATCHER_ALLOC_H

#include "swatcher_types.h"
#include <stddef.h>

/*
 * Global allocator wrappers. The active allocator is set during
 * swatcher_init (via sw_alloc_set) and cleared in swatcher_cleanup.
 * When no custom allocator is configured, these fall through to stdlib.
 */
void  sw_alloc_set(const swatcher_allocator *a);
void  sw_alloc_clear(void);

void *sw_malloc(size_t size);
void *sw_realloc(void *ptr, size_t size);
void  sw_free(void *ptr);

#endif /* SWATCHER_ALLOC_H */
