#if defined(__APPLE__)

#include "swatcher.h"
#include "../internal/internal.h"

static bool fsevents_init(swatcher *sw)
{
    (void)sw;
    SWATCHER_LOG_DEFAULT_ERROR("macOS FSEvents backend not yet implemented");
    return false;
}

static void fsevents_destroy(swatcher *sw)
{
    (void)sw;
}

static bool fsevents_add_target(swatcher *sw, swatcher_target *target)
{
    (void)sw;
    (void)target;
    SWATCHER_LOG_DEFAULT_ERROR("macOS FSEvents backend not yet implemented");
    return false;
}

static bool fsevents_remove_target(swatcher *sw, swatcher_target *target)
{
    (void)sw;
    (void)target;
    SWATCHER_LOG_DEFAULT_ERROR("macOS FSEvents backend not yet implemented");
    return false;
}

static bool fsevents_add_target_recursive(swatcher *sw, swatcher_target *target, bool dont_add_self)
{
    (void)sw;
    (void)target;
    (void)dont_add_self;
    SWATCHER_LOG_DEFAULT_ERROR("macOS FSEvents backend not yet implemented");
    return false;
}

static void *fsevents_thread_func(void *arg)
{
    (void)arg;
    SWATCHER_LOG_DEFAULT_ERROR("macOS FSEvents backend not yet implemented");
    return NULL;
}

static const swatcher_backend fsevents_backend = {
    .name = "fsevents",
    .init = fsevents_init,
    .destroy = fsevents_destroy,
    .add_target = fsevents_add_target,
    .remove_target = fsevents_remove_target,
    .add_target_recursive = fsevents_add_target_recursive,
    .thread_func = fsevents_thread_func,
};

const swatcher_backend *swatcher_backend_fsevents(void)
{
    return &fsevents_backend;
}

const swatcher_backend *swatcher_backend_default(void)
{
    return swatcher_backend_kqueue();
}

#endif /* __APPLE__ */
