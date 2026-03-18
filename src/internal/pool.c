#include "pool.h"
#include "alloc.h"
#include <string.h>

typedef struct pool_node {
    struct pool_node *next;
} pool_node;

struct sw_pool {
    size_t obj_size;
    pool_node *freelist;
};

sw_pool *sw_pool_create(size_t obj_size, int initial_count)
{
    if (obj_size < sizeof(pool_node))
        obj_size = sizeof(pool_node);

    sw_pool *p = sw_malloc(sizeof(sw_pool));
    if (!p) return NULL;
    p->obj_size = obj_size;
    p->freelist = NULL;

    for (int i = 0; i < initial_count; i++) {
        void *obj = sw_malloc(obj_size);
        if (!obj) break;
        pool_node *node = (pool_node *)obj;
        node->next = p->freelist;
        p->freelist = node;
    }

    return p;
}

void *sw_pool_get(sw_pool *pool)
{
    if (!pool) return NULL;
    if (pool->freelist) {
        pool_node *node = pool->freelist;
        pool->freelist = node->next;
        memset(node, 0, pool->obj_size);
        return node;
    }
    void *obj = sw_malloc(pool->obj_size);
    if (obj) memset(obj, 0, pool->obj_size);
    return obj;
}

void sw_pool_put(sw_pool *pool, void *obj)
{
    if (!pool || !obj) return;
    pool_node *node = (pool_node *)obj;
    node->next = pool->freelist;
    pool->freelist = node;
}

void sw_pool_destroy(sw_pool *pool)
{
    if (!pool) return;
    pool_node *node = pool->freelist;
    while (node) {
        pool_node *next = node->next;
        sw_free(node);
        node = next;
    }
    sw_free(pool);
}
