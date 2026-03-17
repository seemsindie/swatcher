#ifndef SWATCHER_BACKEND_H
#define SWATCHER_BACKEND_H

#include "swatcher_types.h"
#include <stdbool.h>

typedef struct swatcher_backend {
    const char *name;
    bool (*init)(swatcher *sw);
    void (*destroy)(swatcher *sw);
    bool (*add_target)(swatcher *sw, swatcher_target *target);
    bool (*remove_target)(swatcher *sw, swatcher_target *target);
    bool (*add_target_recursive)(swatcher *sw, swatcher_target *target, bool dont_add_self);
    void *(*thread_func)(void *arg);
} swatcher_backend;

/* Backend constructors — only the platform-relevant one is defined */
const swatcher_backend *swatcher_backend_inotify(void);
const swatcher_backend *swatcher_backend_fsevents(void);
const swatcher_backend *swatcher_backend_win32(void);
const swatcher_backend *swatcher_backend_kqueue(void);
const swatcher_backend *swatcher_backend_poll(void);
const swatcher_backend *swatcher_backend_default(void);

/* Backend registry */
const swatcher_backend *sw_backend_find(const char *name);
const char **sw_backend_list(void);

#endif /* SWATCHER_BACKEND_H */
