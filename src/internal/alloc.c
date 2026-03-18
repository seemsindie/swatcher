#include "alloc.h"
#include <stdlib.h>

/* File-scoped allocator — set during init, cleared during cleanup. */
static const swatcher_allocator *g_alloc = NULL;

void sw_alloc_set(const swatcher_allocator *a)
{
    g_alloc = a;
}

void sw_alloc_clear(void)
{
    g_alloc = NULL;
}

void *sw_malloc(size_t size)
{
    if (g_alloc && g_alloc->malloc)
        return g_alloc->malloc(size, g_alloc->ctx);
    return malloc(size);
}

void *sw_realloc(void *ptr, size_t size)
{
    if (g_alloc && g_alloc->realloc)
        return g_alloc->realloc(ptr, size, g_alloc->ctx);
    return realloc(ptr, size);
}

void sw_free(void *ptr)
{
    if (g_alloc && g_alloc->free) {
        g_alloc->free(ptr, g_alloc->ctx);
        return;
    }
    free(ptr);
}
