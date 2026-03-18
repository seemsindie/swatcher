#ifndef SWATCHER_POOL_H
#define SWATCHER_POOL_H

#include <stddef.h>

typedef struct sw_pool sw_pool;

/* Create a fixed-size object pool.
 * obj_size: size of each object
 * initial_count: pre-allocate this many objects */
sw_pool *sw_pool_create(size_t obj_size, int initial_count);

/* Get an object from the pool (or malloc if pool empty) */
void *sw_pool_get(sw_pool *pool);

/* Return an object to the pool freelist */
void sw_pool_put(sw_pool *pool, void *obj);

/* Destroy pool and all allocated memory */
void sw_pool_destroy(sw_pool *pool);

#endif /* SWATCHER_POOL_H */
