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

/* Backend constructors */
#if defined(__linux__) || defined(__unix__) || defined(__unix) || defined(unix)
const swatcher_backend *swatcher_backend_inotify(void);
#endif

#if defined(__APPLE__)
const swatcher_backend *swatcher_backend_fsevents(void);
#endif

#if defined(_WIN32) || defined(_WIN64)
const swatcher_backend *swatcher_backend_win32(void);
#endif

/* Returns the default backend for the current platform */
const swatcher_backend *swatcher_backend_default(void);

#endif /* SWATCHER_BACKEND_H */
