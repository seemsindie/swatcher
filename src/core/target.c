#include "swatcher.h"
#include "../internal/internal.h"
#include "error.h"

SWATCHER_API swatcher_target *swatcher_target_create(swatcher_target_desc *desc)
{
    if (!desc) {
        sw_set_error(SWATCHER_ERR_NULL_ARG);
        SWATCHER_LOG_DEFAULT_ERROR("desc is NULL");
        return NULL;
    }

    swatcher_target *target = malloc(sizeof(swatcher_target));
    if (!target) {
        sw_set_error(SWATCHER_ERR_ALLOC);
        SWATCHER_LOG_DEFAULT_ERROR("Failed to allocate swatcher_target");
        return NULL;
    }

    char normalized_path[SW_PATH_MAX];
    if (!sw_path_normalize(desc->path, normalized_path, SW_PATH_MAX, desc->follow_symlinks)) {
        sw_set_error(SWATCHER_ERR_INVALID_PATH);
        SWATCHER_LOG_DEFAULT_ERROR("Failed to normalize path: %s", desc->path);
        free(target);
        return NULL;
    }

    sw_file_info info;
    if (!sw_stat(normalized_path, &info, desc->follow_symlinks)) {
        sw_set_error(SWATCHER_ERR_PATH_NOT_FOUND);
        SWATCHER_LOG_DEFAULT_ERROR("Failed to stat path: %s", normalized_path);
        free(target);
        return NULL;
    }

    target->is_file = info.is_file;
    target->is_directory = info.is_directory;
    target->is_symlink = info.is_symlink;

    target->path = sw_strdup(normalized_path);
    if (!target->path) {
        sw_set_error(SWATCHER_ERR_ALLOC);
        SWATCHER_LOG_DEFAULT_ERROR("Failed to allocate path (string)");
        free(target);
        return NULL;
    }

    target->callback_patterns = desc->callback_patterns ? desc->callback_patterns : NULL;
    target->watch_patterns = desc->watch_patterns ? desc->watch_patterns : NULL;
    target->ignore_patterns = desc->ignore_patterns ? desc->ignore_patterns : NULL;

    if (desc->watch_options == 0)
        target->watch_options = SWATCHER_WATCH_ALL;
    else
        target->watch_options = desc->watch_options;

    target->is_recursive = desc->is_recursive;
    target->events = desc->events;
    target->watch_options = desc->watch_options;
    target->user_data = desc->user_data;
    target->callback = desc->callback;
    target->last_event_time = time(NULL);
    target->follow_symlinks = desc->follow_symlinks;
    target->_internal = NULL;

    /* Create internal struct early so compiled patterns are available
     * before backends start recursive directory traversal. */
    if (!sw_target_internal_create(target)) {
        sw_set_error(SWATCHER_ERR_ALLOC);
        SWATCHER_LOG_DEFAULT_ERROR("Failed to create target internal for %s", target->path);
        free(target->path);
        free(target);
        return NULL;
    }

    return target;
}

SWATCHER_API void swatcher_target_destroy(swatcher_target *target)
{
    if (!target) return;
    if (target->_internal)
        sw_target_internal_destroy(SW_TARGET_INTERNAL(target));
    free(target->path);
    free(target);
}

SWATCHER_API bool swatcher_is_watched(swatcher *sw, const char *path)
{
    return sw_find_target_internal(sw, path) != NULL;
}

swatcher_target_internal *sw_target_internal_create(swatcher_target *target)
{
    swatcher_target_internal *ti = malloc(sizeof(swatcher_target_internal));
    if (!ti) return NULL;
    ti->target = target;
    ti->path = target->path;
    ti->backend_data = NULL;
    memset(&ti->hh_global, 0, sizeof(ti->hh_global));
    ti->compiled_callback = sw_patterns_compile(target->callback_patterns);
    ti->compiled_watch    = sw_patterns_compile(target->watch_patterns);
    ti->compiled_ignore   = sw_patterns_compile(target->ignore_patterns);
    target->_internal = ti;
    return ti;
}

void sw_target_internal_destroy(swatcher_target_internal *ti)
{
    if (!ti) return;
    sw_patterns_free(ti->compiled_callback);
    sw_patterns_free(ti->compiled_watch);
    sw_patterns_free(ti->compiled_ignore);
    if (ti->target)
        ti->target->_internal = NULL;
    free(ti);
}
