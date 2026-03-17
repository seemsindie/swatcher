#include "backend.h"
#include <string.h>
#include <stddef.h>

typedef struct {
    const char *name;
    const swatcher_backend *(*ctor)(void);
} backend_entry;

static const backend_entry backends[] = {
#if defined(__linux__) || defined(__unix__)
    { "inotify",  swatcher_backend_inotify },
#endif
#if defined(__APPLE__)
    { "kqueue",   swatcher_backend_kqueue },
    { "fsevents", swatcher_backend_fsevents },
#endif
#if defined(_WIN32)
    { "win32",    swatcher_backend_win32 },
#endif
    { "poll",     swatcher_backend_poll },
    { NULL, NULL }
};

const swatcher_backend *sw_backend_find(const char *name)
{
    for (int i = 0; backends[i].name; i++) {
        if (strcmp(backends[i].name, name) == 0)
            return backends[i].ctor();
    }
    return NULL;
}

static const char *backend_names[8];

const char **sw_backend_list(void)
{
    int j = 0;
    for (int i = 0; backends[i].name; i++)
        backend_names[j++] = backends[i].name;
    backend_names[j] = NULL;
    return backend_names;
}

const swatcher_backend *swatcher_backend_default(void)
{
    return backends[0].ctor();
}
