#include "swatcher.h"
#include "../internal/internal.h"

SWATCHER_API bool swatcher_init(swatcher *sw, swatcher_config *config)
{
    if (!sw) {
        SWATCHER_LOG_DEFAULT_ERROR("swatcher is NULL");
        return false;
    }
    if (!config) {
        SWATCHER_LOG_DEFAULT_ERROR("config is NULL");
        return false;
    }

    swatcher_internal *si = malloc(sizeof(swatcher_internal));
    if (!si) {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to allocate swatcher_internal");
        return false;
    }

    si->mutex = sw_mutex_create();
    if (!si->mutex) {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to create mutex");
        free(si);
        return false;
    }

    si->backend = swatcher_backend_default();
    si->backend_data = NULL;
    si->thread = NULL;
    si->targets = NULL;

    sw->_internal = si;
    sw->running = false;
    sw->config = config;

    if (!si->backend->init(sw)) {
        SWATCHER_LOG_DEFAULT_ERROR("Backend init failed");
        sw_mutex_destroy(si->mutex);
        free(si);
        sw->_internal = NULL;
        return false;
    }

    return true;
}

SWATCHER_API bool swatcher_start(swatcher *sw)
{
    if (!sw || !sw->_internal) {
        SWATCHER_LOG_DEFAULT_ERROR("swatcher not initialized");
        return false;
    }

    swatcher_internal *si = SW_INTERNAL(sw);

    sw->running = true;
    si->thread = sw_thread_create(si->backend->thread_func, sw);
    if (!si->thread) {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to create watcher thread");
        sw->running = false;
        return false;
    }

    return true;
}

SWATCHER_API void swatcher_stop(swatcher *sw)
{
    if (!sw || !sw->_internal) return;

    swatcher_internal *si = SW_INTERNAL(sw);
    sw->running = false;

    if (si->thread) {
        sw_thread_join(si->thread);
        sw_thread_destroy(si->thread);
        si->thread = NULL;
    }
}

SWATCHER_API bool swatcher_add(swatcher *sw, swatcher_target *target)
{
    swatcher_internal *si = SW_INTERNAL(sw);
    bool result = false;

    sw_mutex_lock(si->mutex);

    /* Internal struct (with compiled patterns) is created in swatcher_target_create() */
    swatcher_target_internal *ti = SW_TARGET_INTERNAL(target);
    if (!ti) {
        SWATCHER_LOG_DEFAULT_ERROR("Target has no internal struct");
        sw_mutex_unlock(si->mutex);
        return false;
    }

    if (!target->is_file && target->is_recursive) {
        bool dont_add_self = (target->watch_options == SWATCHER_WATCH_FILES ||
                              target->watch_options == SWATCHER_WATCH_SYMLINKS ||
                              target->watch_patterns != NULL);
        result = si->backend->add_target_recursive(sw, target, dont_add_self);
    } else {
        result = si->backend->add_target(sw, target);
    }

    sw_mutex_unlock(si->mutex);
    return result;
}

SWATCHER_API bool swatcher_remove(swatcher *sw, swatcher_target *target)
{
    swatcher_internal *si = SW_INTERNAL(sw);

    sw_mutex_lock(si->mutex);

    si->backend->remove_target(sw, target);

    swatcher_target_internal *ti = SW_TARGET_INTERNAL(target);
    if (ti) {
        sw_remove_target_internal(sw, ti);
        sw_target_internal_destroy(ti);
    }

    free(target->path);
    free(target);

    sw_mutex_unlock(si->mutex);
    return true;
}

SWATCHER_API void swatcher_cleanup(swatcher *sw)
{
    if (!sw || !sw->_internal) return;

    swatcher_internal *si = SW_INTERNAL(sw);

    /* Remove all targets */
    swatcher_target_internal *current, *tmp;
    HASH_ITER(hh_global, si->targets, current, tmp) {
        si->backend->remove_target(sw, current->target);
        HASH_DELETE(hh_global, si->targets, current);
        free(current->target->path);
        free(current->target);
        sw_target_internal_destroy(current);
    }

    si->backend->destroy(sw);

    sw_mutex_destroy(si->mutex);
    free(si);
    sw->_internal = NULL;
    free(sw);
}
