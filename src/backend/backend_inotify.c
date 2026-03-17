#if defined(__linux__) || defined(__unix__) || defined(__unix) || defined(unix)

#include "swatcher.h"
#include "../internal/internal.h"
#include "../core/error.h"

#include <sys/inotify.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>

/* Pattern matching via compiled patterns */
#include "../core/pattern.h"

#define EVENT_SIZE (sizeof(struct inotify_event))
#define BUF_LEN    (1024 * (EVENT_SIZE + 16))

/* ========== Local types ========== */

typedef struct swatcher_target_inotify {
    int wd;
    swatcher_target *target;
    UT_hash_handle hh;
} swatcher_target_inotify;

typedef struct coalesce_entry {
    char path[SW_PATH_MAX];
    swatcher_fs_event event;
    swatcher_target *target;
    uint64_t timestamp_ms;
    UT_hash_handle hh;
} coalesce_entry;

typedef struct swatcher_inotify {
    int inotify_fd;
    struct pollfd fds;
    swatcher_target_inotify *wd_to_target;
    int watch_count;
    int max_watches;
    coalesce_entry *pending_events;
} swatcher_inotify;

#define INOTIFY_DATA(sw) ((swatcher_inotify *)SW_INTERNAL(sw)->backend_data)

/* ========== Helpers ========== */

static uint32_t events_to_inotify_mask(swatcher_target *target)
{
    uint32_t mask = 0;

    if (target->events & SWATCHER_EVENT_ALL) {
        return (IN_CREATE | IN_MODIFY | IN_DELETE | IN_DELETE_SELF |
                IN_MOVE | IN_OPEN | IN_CLOSE | IN_ACCESS | IN_ATTRIB);
    }

    if (target->events & SWATCHER_EVENT_CREATED)       mask |= IN_CREATE;
    if (target->events & SWATCHER_EVENT_MODIFIED)       mask |= IN_MODIFY;
    if (target->events & SWATCHER_EVENT_DELETED)        mask |= IN_DELETE | IN_DELETE_SELF;
    if (target->events & SWATCHER_EVENT_MOVED)          mask |= IN_MOVE;
    if (target->events & SWATCHER_EVENT_OPENED)         mask |= IN_OPEN;
    if (target->events & SWATCHER_EVENT_CLOSED)         mask |= IN_CLOSE;
    if (target->events & SWATCHER_EVENT_ACCESSED)       mask |= IN_ACCESS;
    if (target->events & SWATCHER_EVENT_ATTRIB_CHANGE)  mask |= IN_ATTRIB;

    return mask;
}

static swatcher_fs_event inotify_to_swatcher_event(uint32_t mask)
{
    if (mask & IN_CREATE)  return SWATCHER_EVENT_CREATED;
    if (mask & IN_MODIFY)  return SWATCHER_EVENT_MODIFIED;
    if (mask & IN_DELETE)  return SWATCHER_EVENT_DELETED;
    if (mask & IN_MOVE)    return SWATCHER_EVENT_MOVED;
    if (mask & IN_OPEN)    return SWATCHER_EVENT_OPENED;
    if (mask & IN_CLOSE)   return SWATCHER_EVENT_CLOSED;
    if (mask & IN_ACCESS)  return SWATCHER_EVENT_ACCESSED;
    if (mask & IN_ATTRIB)  return SWATCHER_EVENT_ATTRIB_CHANGE;
    return SWATCHER_EVENT_NONE;
}

/* ========== inotify limit checking ========== */

static int inotify_get_max_watches(void)
{
    FILE *f = fopen("/proc/sys/fs/inotify/max_user_watches", "r");
    if (!f) return -1;

    int val = -1;
    if (fscanf(f, "%d", &val) != 1)
        val = -1;
    fclose(f);
    return val;
}

/* ========== add single watch (called with mutex held) ========== */

static bool inotify_add_single(swatcher *sw, swatcher_target *target)
{
    swatcher_inotify *ino = INOTIFY_DATA(sw);

    if (sw_find_target_internal(sw, target->path)) {
        sw_set_error(SWATCHER_ERR_TARGET_EXISTS);
        SWATCHER_LOG_DEFAULT_WARNING("Path already watched: %s", target->path);
        return false;
    }

    /* Check inotify limits */
    if (ino->max_watches > 0 && ino->watch_count >= (int)(ino->max_watches * 0.9)) {
        SWATCHER_LOG_DEFAULT_WARNING("Approaching inotify watch limit (%d/%d): %s",
                                      ino->watch_count, ino->max_watches, target->path);
    }

    SWATCHER_LOG_DEFAULT_INFO("Target %s is being watched", target->path);

    swatcher_target_inotify *ino_target = malloc(sizeof(swatcher_target_inotify));
    if (!ino_target) {
        sw_set_error(SWATCHER_ERR_ALLOC);
        SWATCHER_LOG_DEFAULT_ERROR("Failed to allocate swatcher_target_inotify");
        return false;
    }

    ino_target->target = target;
    ino_target->wd = inotify_add_watch(ino->inotify_fd, target->path, events_to_inotify_mask(target));
    if (ino_target->wd < 0) {
        if (errno == ENOSPC) {
            sw_set_error(SWATCHER_ERR_WATCH_LIMIT);
            SWATCHER_LOG_DEFAULT_ERROR(
                "inotify watch limit reached (%d/%d). "
                "Increase via: sysctl fs.inotify.max_user_watches=524288",
                ino->watch_count, ino->max_watches);
        } else {
            sw_set_error(SWATCHER_ERR_BACKEND_INIT);
            SWATCHER_LOG_DEFAULT_ERROR("Failed to add watch for %s: %s",
                                        target->path, strerror(errno));
        }
        free(ino_target);
        return false;
    }

    HASH_ADD_INT(ino->wd_to_target, wd, ino_target);
    ino->watch_count++;

    /* Create target internal and add to hash */
    swatcher_target_internal *ti = SW_TARGET_INTERNAL(target);
    if (!ti) {
        ti = sw_target_internal_create(target);
        if (!ti) {
            SWATCHER_LOG_DEFAULT_ERROR("Failed to create target internal");
            inotify_rm_watch(ino->inotify_fd, ino_target->wd);
            HASH_DEL(ino->wd_to_target, ino_target);
            ino->watch_count--;
            free(ino_target);
            return false;
        }
    }
    ti->backend_data = ino_target;
    sw_add_target_internal(sw, ti);

    return true;
}

/* ========== Recursive add (called with mutex held) ========== */

static bool inotify_add_recursive_locked(swatcher *sw, swatcher_target *original_target, bool dont_add_self)
{
    if (original_target->is_file) {
        return inotify_add_single(sw, original_target);
    }

    sw_dir *dir = sw_dir_open(original_target->path);
    if (!dir) {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to open directory: %s", original_target->path);
        return false;
    }

    sw_dir_entry entry;
    while (sw_dir_next(dir, &entry)) {
        if (SW_TARGET_INTERNAL(original_target)->compiled_ignore) {
            if (sw_pattern_matched(SW_TARGET_INTERNAL(original_target)->compiled_ignore, entry.name))
                continue;
        }

        if (!SW_TARGET_INTERNAL(original_target)->compiled_watch ||
            sw_pattern_matched(SW_TARGET_INTERNAL(original_target)->compiled_watch, entry.name) ||
            entry.is_dir) {

            if (entry.is_dir) {
                if (strcmp(entry.name, ".") == 0 || strcmp(entry.name, "..") == 0)
                    continue;

                char new_path[SW_PATH_MAX];
                size_t plen = strlen(original_target->path);
                if (plen > 0 && original_target->path[plen - 1] == '/')
                    snprintf(new_path, SW_PATH_MAX, "%s%s", original_target->path, entry.name);
                else
                    snprintf(new_path, SW_PATH_MAX, "%s/%s", original_target->path, entry.name);

                swatcher_target_desc desc = {
                    .path = new_path,
                    .is_recursive = original_target->is_recursive,
                    .events = original_target->events,
                    .watch_options = original_target->watch_options,
                    .follow_symlinks = original_target->follow_symlinks,
                    .user_data = original_target->user_data,
                    .callback_patterns = original_target->callback_patterns,
                    .watch_patterns = original_target->watch_patterns,
                    .ignore_patterns = original_target->ignore_patterns,
                    .callback = original_target->callback
                };

                swatcher_target *new_target = swatcher_target_create(&desc);
                if (!new_target) {
                    SWATCHER_LOG_DEFAULT_WARNING("Failed to create new target for %s", new_path);
                    continue;
                }

                bool skip_self = (original_target->watch_options == SWATCHER_WATCH_FILES ||
                                  original_target->watch_options == SWATCHER_WATCH_SYMLINKS ||
                                  (SW_TARGET_INTERNAL(original_target)->compiled_watch &&
                                   !sw_pattern_matched(SW_TARGET_INTERNAL(original_target)->compiled_watch, entry.name)));

                if (!inotify_add_recursive_locked(sw, new_target, skip_self)) {
                    SWATCHER_LOG_DEFAULT_WARNING("Failed to add watch for %s", new_target->path);
                    free(new_target->path);
                    free(new_target);
                    continue;
                }
            } else if (entry.is_file) {
                if (original_target->watch_options & SWATCHER_WATCH_FILES ||
                    original_target->watch_options == SWATCHER_WATCH_ALL) {

                    char new_path[SW_PATH_MAX];
                    size_t plen = strlen(original_target->path);
                    if (plen > 0 && original_target->path[plen - 1] == '/')
                        snprintf(new_path, SW_PATH_MAX, "%s%s", original_target->path, entry.name);
                    else
                        snprintf(new_path, SW_PATH_MAX, "%s/%s", original_target->path, entry.name);

                    swatcher_target_desc desc = {
                        .path = new_path,
                        .is_recursive = original_target->is_recursive,
                        .events = original_target->events,
                        .watch_options = original_target->watch_options,
                        .follow_symlinks = original_target->follow_symlinks,
                        .user_data = original_target->user_data,
                        .callback_patterns = original_target->callback_patterns,
                        .watch_patterns = original_target->watch_patterns,
                        .ignore_patterns = original_target->ignore_patterns,
                        .callback = original_target->callback
                    };

                    swatcher_target *new_target = swatcher_target_create(&desc);
                    if (!new_target) {
                        SWATCHER_LOG_DEFAULT_WARNING("Failed to create new target for %s", new_path);
                        continue;
                    }

                    if (!inotify_add_single(sw, new_target)) {
                        SWATCHER_LOG_DEFAULT_WARNING("Failed to add watch (file) for %s", new_target->path);
                        free(new_target->path);
                        free(new_target);
                        continue;
                    }
                }
            } else if (entry.is_symlink) {
                if (original_target->follow_symlinks) {
                    char new_path[SW_PATH_MAX];
                    size_t plen = strlen(original_target->path);
                    if (plen > 0 && original_target->path[plen - 1] == '/')
                        snprintf(new_path, SW_PATH_MAX, "%s%s", original_target->path, entry.name);
                    else
                        snprintf(new_path, SW_PATH_MAX, "%s/%s", original_target->path, entry.name);

                    swatcher_target_desc desc = {
                        .path = new_path,
                        .is_recursive = original_target->is_recursive,
                        .events = original_target->events,
                        .watch_options = original_target->watch_options,
                        .follow_symlinks = original_target->follow_symlinks,
                        .user_data = original_target->user_data,
                        .callback_patterns = original_target->callback_patterns,
                        .watch_patterns = original_target->watch_patterns,
                        .ignore_patterns = original_target->ignore_patterns,
                        .callback = original_target->callback
                    };

                    swatcher_target *new_target = swatcher_target_create(&desc);
                    if (!new_target) {
                        SWATCHER_LOG_DEFAULT_ERROR("Failed to create new target for %s", new_path);
                        continue;
                    }

                    if (new_target->is_file) {
                        if (original_target->watch_options & SWATCHER_WATCH_FILES ||
                            original_target->watch_options == SWATCHER_WATCH_ALL) {
                            if (!inotify_add_single(sw, new_target)) {
                                free(new_target->path);
                                free(new_target);
                                continue;
                            }
                        } else {
                            free(new_target->path);
                            free(new_target);
                            continue;
                        }
                    } else {
                        if (original_target->watch_options & SWATCHER_WATCH_DIRECTORIES ||
                            original_target->watch_options == SWATCHER_WATCH_ALL) {
                            if (!inotify_add_recursive_locked(sw, new_target, false)) {
                                free(new_target->path);
                                free(new_target);
                                continue;
                            }
                        } else {
                            free(new_target->path);
                            free(new_target);
                            continue;
                        }
                    }
                } else {
                    if (original_target->watch_options & SWATCHER_WATCH_SYMLINKS ||
                        original_target->watch_options == SWATCHER_WATCH_ALL) {

                        char new_path[SW_PATH_MAX];
                        size_t plen = strlen(original_target->path);
                        if (plen > 0 && original_target->path[plen - 1] == '/')
                            snprintf(new_path, SW_PATH_MAX, "%s%s", original_target->path, entry.name);
                        else
                            snprintf(new_path, SW_PATH_MAX, "%s/%s", original_target->path, entry.name);

                        swatcher_target_desc desc = {
                            .path = new_path,
                            .is_recursive = original_target->is_recursive,
                            .events = original_target->events,
                            .watch_options = original_target->watch_options,
                            .follow_symlinks = original_target->follow_symlinks,
                            .user_data = original_target->user_data,
                            .callback_patterns = original_target->callback_patterns,
                            .watch_patterns = original_target->watch_patterns,
                            .ignore_patterns = original_target->ignore_patterns,
                            .callback = original_target->callback
                        };

                        swatcher_target *new_target = swatcher_target_create(&desc);
                        if (!new_target) {
                            SWATCHER_LOG_DEFAULT_ERROR("Failed to create new target for %s", original_target->path);
                            continue;
                        }

                        if (!inotify_add_single(sw, new_target)) {
                            free(new_target->path);
                            free(new_target);
                            continue;
                        }
                    }
                }
            }
        }
    }

    sw_dir_close(dir);

    if (dont_add_self)
        return true;

    return inotify_add_single(sw, original_target);
}

/* ========== Dynamic recursive helpers (called with mutex held) ========== */

static void inotify_remove_children(swatcher *sw, const char *dir_path)
{
    swatcher_inotify *ino = INOTIFY_DATA(sw);
    size_t dir_len = strlen(dir_path);

    swatcher_target_inotify *current, *tmp;
    HASH_ITER(hh, ino->wd_to_target, current, tmp) {
        const char *tpath = current->target->path;
        /* Check if target path starts with dir_path/ */
        if (strncmp(tpath, dir_path, dir_len) == 0 && tpath[dir_len] == '/') {
            inotify_rm_watch(ino->inotify_fd, current->wd);
            HASH_DEL(ino->wd_to_target, current);
            ino->watch_count--;

            swatcher_target_internal *ti = SW_TARGET_INTERNAL(current->target);
            swatcher_target *t = current->target;
            if (ti) {
                sw_remove_target_internal(sw, ti);
                ti->backend_data = NULL;
                ti->target = NULL; /* prevent use-after-free in destroy */
                sw_target_internal_destroy(ti);
            }
            free(t->path);
            free(t);
            free(current);
        }
    }
}

static void inotify_handle_dynamic_mkdir(swatcher *sw, swatcher_target *parent, const char *name)
{
    if (!parent->is_recursive) return;

    char new_path[SW_PATH_MAX];
    size_t plen = strlen(parent->path);
    if (plen > 0 && parent->path[plen - 1] == '/')
        snprintf(new_path, SW_PATH_MAX, "%s%s", parent->path, name);
    else
        snprintf(new_path, SW_PATH_MAX, "%s/%s", parent->path, name);

    /* Already watched? */
    if (sw_find_target_internal(sw, new_path))
        return;

    swatcher_target_desc desc = {
        .path = new_path,
        .is_recursive = parent->is_recursive,
        .events = parent->events,
        .watch_options = parent->watch_options,
        .follow_symlinks = parent->follow_symlinks,
        .user_data = parent->user_data,
        .callback_patterns = parent->callback_patterns,
        .watch_patterns = parent->watch_patterns,
        .ignore_patterns = parent->ignore_patterns,
        .callback = parent->callback
    };

    swatcher_target *new_target = swatcher_target_create(&desc);
    if (!new_target) {
        SWATCHER_LOG_DEFAULT_WARNING("Failed to create target for new dir: %s", new_path);
        return;
    }

    /* Add the new dir and scan its contents (handles race where files appeared before watch) */
    if (!inotify_add_recursive_locked(sw, new_target, false)) {
        SWATCHER_LOG_DEFAULT_WARNING("Failed to watch new dir: %s", new_path);
        free(new_target->path);
        free(new_target);
    }
}

static void inotify_handle_dynamic_rmdir(swatcher *sw, swatcher_target *parent, const char *name)
{
    char dir_path[SW_PATH_MAX];
    size_t plen = strlen(parent->path);
    if (plen > 0 && parent->path[plen - 1] == '/')
        snprintf(dir_path, SW_PATH_MAX, "%s%s", parent->path, name);
    else
        snprintf(dir_path, SW_PATH_MAX, "%s/%s", parent->path, name);

    inotify_remove_children(sw, dir_path);

    /* Remove the directory's own watch if it exists */
    swatcher_inotify *ino = INOTIFY_DATA(sw);
    swatcher_target_internal *ti = sw_find_target_internal(sw, dir_path);
    if (ti) {
        swatcher_target_inotify *ino_target = (swatcher_target_inotify *)ti->backend_data;
        if (ino_target) {
            inotify_rm_watch(ino->inotify_fd, ino_target->wd);
            HASH_DEL(ino->wd_to_target, ino_target);
            ino->watch_count--;
            free(ino_target);
            ti->backend_data = NULL;
        }
        sw_remove_target_internal(sw, ti);
        swatcher_target *t = ti->target;
        ti->target = NULL; /* prevent use-after-free in destroy */
        free(t->path);
        free(t);
        sw_target_internal_destroy(ti);
    }
}

/* ========== IN_Q_OVERFLOW rescan (called with mutex held) ========== */

static void inotify_rescan_all(swatcher *sw)
{
    swatcher_inotify *ino = INOTIFY_DATA(sw);

    /* Collect all current recursive targets for rescan */
    swatcher_target_inotify *current, *tmp;
    HASH_ITER(hh, ino->wd_to_target, current, tmp) {
        if (current->target->is_recursive && current->target->is_directory) {
            /* Re-scan this directory for new subdirs */
            sw_dir *dir = sw_dir_open(current->target->path);
            if (!dir) continue;

            sw_dir_entry entry;
            while (sw_dir_next(dir, &entry)) {
                if (!entry.is_dir) continue;
                if (strcmp(entry.name, ".") == 0 || strcmp(entry.name, "..") == 0)
                    continue;

                char child_path[SW_PATH_MAX];
                size_t plen = strlen(current->target->path);
                if (plen > 0 && current->target->path[plen - 1] == '/')
                    snprintf(child_path, SW_PATH_MAX, "%s%s", current->target->path, entry.name);
                else
                    snprintf(child_path, SW_PATH_MAX, "%s/%s", current->target->path, entry.name);

                if (!sw_find_target_internal(sw, child_path)) {
                    inotify_handle_dynamic_mkdir(sw, current->target, entry.name);
                }
            }
            sw_dir_close(dir);
        }
    }
}

/* ========== Event coalescing ========== */

static void inotify_coalesce_dispatch(swatcher *sw, coalesce_entry *ce)
{
    (void)sw;
    if (ce->target && ce->target->callback) {
        /* Extract just the filename from the path for the callback */
        const char *name = strrchr(ce->path, '/');
        name = name ? name + 1 : ce->path;
        ce->target->callback(ce->event, ce->target, name, NULL);
        ce->target->last_event_time = time(NULL);
    }
}

static void inotify_coalesce_flush(swatcher *sw, uint64_t now_ms, int coalesce_ms, bool flush_all)
{
    swatcher_inotify *ino = INOTIFY_DATA(sw);
    coalesce_entry *current, *tmp;

    HASH_ITER(hh, ino->pending_events, current, tmp) {
        if (flush_all || (now_ms - current->timestamp_ms >= (uint64_t)coalesce_ms)) {
            inotify_coalesce_dispatch(sw, current);
            HASH_DEL(ino->pending_events, current);
            free(current);
        }
    }
}

static void inotify_coalesce_add(swatcher *sw, const char *full_path,
                                  swatcher_fs_event event, swatcher_target *target)
{
    swatcher_inotify *ino = INOTIFY_DATA(sw);
    coalesce_entry *existing = NULL;

    HASH_FIND_STR(ino->pending_events, full_path, existing);

    if (existing) {
        /* Merge rules */
        if (existing->event == SWATCHER_EVENT_CREATED && event == SWATCHER_EVENT_DELETED) {
            /* CREATED + DELETED = drop both */
            HASH_DEL(ino->pending_events, existing);
            free(existing);
            return;
        } else if (existing->event == SWATCHER_EVENT_CREATED && event == SWATCHER_EVENT_MODIFIED) {
            /* CREATED + MODIFIED = CREATED (keep timestamp) */
            existing->timestamp_ms = sw_time_now_ms();
            return;
        } else if (existing->event == SWATCHER_EVENT_MODIFIED && event == SWATCHER_EVENT_MODIFIED) {
            /* MODIFIED + MODIFIED = MODIFIED (update timestamp) */
            existing->timestamp_ms = sw_time_now_ms();
            return;
        } else if (existing->event == SWATCHER_EVENT_MODIFIED && event == SWATCHER_EVENT_DELETED) {
            /* MODIFIED + DELETED = DELETED */
            existing->event = SWATCHER_EVENT_DELETED;
            existing->timestamp_ms = sw_time_now_ms();
            return;
        }
        /* For other combinations, just replace */
        existing->event = event;
        existing->timestamp_ms = sw_time_now_ms();
    } else {
        coalesce_entry *ce = malloc(sizeof(coalesce_entry));
        if (!ce) return;
        strncpy(ce->path, full_path, SW_PATH_MAX - 1);
        ce->path[SW_PATH_MAX - 1] = '\0';
        ce->event = event;
        ce->target = target;
        ce->timestamp_ms = sw_time_now_ms();
        HASH_ADD_STR(ino->pending_events, path, ce);
    }
}

/* ========== Vtable functions ========== */

static bool inotify_init_backend(swatcher *sw)
{
    swatcher_inotify *ino = malloc(sizeof(swatcher_inotify));
    if (!ino) {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to allocate swatcher_inotify");
        return false;
    }

    ino->inotify_fd = inotify_init();
    if (ino->inotify_fd < 0) {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to initialize inotify");
        free(ino);
        return false;
    }

    ino->fds.fd = ino->inotify_fd;
    ino->fds.events = POLLIN;
    ino->wd_to_target = NULL;
    ino->watch_count = 0;
    ino->max_watches = inotify_get_max_watches();
    ino->pending_events = NULL;

    SW_INTERNAL(sw)->backend_data = ino;
    return true;
}

static void inotify_destroy(swatcher *sw)
{
    swatcher_inotify *ino = INOTIFY_DATA(sw);
    if (!ino) return;

    /* Flush and free pending coalesce events */
    coalesce_entry *ce, *ce_tmp;
    HASH_ITER(hh, ino->pending_events, ce, ce_tmp) {
        HASH_DEL(ino->pending_events, ce);
        free(ce);
    }

    /* Remove all inotify watches */
    swatcher_target_inotify *current, *tmp;
    HASH_ITER(hh, ino->wd_to_target, current, tmp) {
        inotify_rm_watch(ino->inotify_fd, current->wd);
        HASH_DEL(ino->wd_to_target, current);
        free(current);
    }

    close(ino->inotify_fd);
    free(ino);
    SW_INTERNAL(sw)->backend_data = NULL;
}

static bool inotify_add_target(swatcher *sw, swatcher_target *target)
{
    return inotify_add_single(sw, target);
}

static bool inotify_remove_target(swatcher *sw, swatcher_target *target)
{
    swatcher_inotify *ino = INOTIFY_DATA(sw);
    swatcher_target_internal *ti = SW_TARGET_INTERNAL(target);
    if (!ti) return false;

    swatcher_target_inotify *ino_target = (swatcher_target_inotify *)ti->backend_data;
    if (ino_target) {
        inotify_rm_watch(ino->inotify_fd, ino_target->wd);
        HASH_DEL(ino->wd_to_target, ino_target);
        ino->watch_count--;
        free(ino_target);
        ti->backend_data = NULL;
    }

    return true;
}

static bool inotify_add_target_recursive(swatcher *sw, swatcher_target *target, bool dont_add_self)
{
    return inotify_add_recursive_locked(sw, target, dont_add_self);
}

static void inotify_dispatch_event(swatcher *sw, swatcher_target *target,
                                    swatcher_fs_event sw_event, const char *name,
                                    struct inotify_event *event)
{
    int coalesce_ms = sw->config->coalesce_ms;

    if (coalesce_ms > 0 && name) {
        /* Build full path for coalescing key */
        char full_path[SW_PATH_MAX];
        size_t plen = strlen(target->path);
        if (plen > 0 && target->path[plen - 1] == '/')
            snprintf(full_path, SW_PATH_MAX, "%s%s", target->path, name);
        else
            snprintf(full_path, SW_PATH_MAX, "%s/%s", target->path, name);

        inotify_coalesce_add(sw, full_path, sw_event, target);
    } else {
        target->callback(sw_event, target, name, event);
        target->last_event_time = time(NULL);
    }
}

static void *inotify_thread_func(void *arg)
{
    swatcher *sw = (swatcher *)arg;
    swatcher_inotify *ino = INOTIFY_DATA(sw);
    swatcher_internal *si = SW_INTERNAL(sw);
    char buffer[BUF_LEN];
    int coalesce_ms = sw->config->coalesce_ms;
    int poll_timeout = sw->config->poll_interval_ms;

    /* If coalescing, use shorter poll timeout for timely flushes */
    if (coalesce_ms > 0 && poll_timeout > coalesce_ms)
        poll_timeout = coalesce_ms;

    while (sw_atomic_load(&sw->running)) {
        int poll_ret = poll(&ino->fds, 1, poll_timeout);
        if (poll_ret < 0) {
            if (errno == EINTR)
                continue;
            SWATCHER_LOG_DEFAULT_ERROR("poll failed: %s", strerror(errno));
            break;
        }

        /* Flush expired coalesce entries on timeout or after processing events */
        if (coalesce_ms > 0) {
            sw_mutex_lock(si->mutex);
            inotify_coalesce_flush(sw, sw_time_now_ms(), coalesce_ms, false);
            sw_mutex_unlock(si->mutex);
        }

        if (poll_ret == 0)
            continue;

        int length = read(ino->inotify_fd, buffer, BUF_LEN);
        if (length < 0) {
            if (errno == EINTR)
                continue;
            SWATCHER_LOG_ERROR(sw, "Error reading inotify FD: %s", strerror(errno));
            break;
        }

        /* Lock mutex for the entire event parsing+dispatch loop */
        sw_mutex_lock(si->mutex);

        int i = 0;
        while (i < length) {
            struct inotify_event *event = (struct inotify_event *)&buffer[i];

            /* Handle IN_Q_OVERFLOW first (wd == -1) */
            if (event->mask & IN_Q_OVERFLOW) {
                SWATCHER_LOG_DEFAULT_WARNING("inotify queue overflow — rescanning all watches");
                inotify_rescan_all(sw);

                /* Emit overflow to every active target's callback */
                swatcher_target_inotify *ot, *ot_tmp;
                HASH_ITER(hh, ino->wd_to_target, ot, ot_tmp) {
                    if (ot->target->callback) {
                        ot->target->callback(SWATCHER_EVENT_OVERFLOW, ot->target, NULL, NULL);
                    }
                }

                i += sizeof(struct inotify_event) + event->len;
                continue;
            }

            /* Handle IN_IGNORED (watch removed by kernel) — skip silently */
            if (event->mask & IN_IGNORED) {
                i += sizeof(struct inotify_event) + event->len;
                continue;
            }

            swatcher_target_inotify *target_data = NULL;
            HASH_FIND_INT(ino->wd_to_target, &event->wd, target_data);
            if (!target_data) {
                i += sizeof(struct inotify_event) + event->len;
                continue;
            }

            swatcher_target *target = target_data->target;

            /* Dynamic recursive watching: new directory appeared */
            if ((event->mask & (IN_CREATE | IN_ISDIR)) == (IN_CREATE | IN_ISDIR) ||
                (event->mask & (IN_MOVED_TO | IN_ISDIR)) == (IN_MOVED_TO | IN_ISDIR)) {
                if (event->len > 0) {
                    inotify_handle_dynamic_mkdir(sw, target, event->name);
                }
            }

            /* Dynamic recursive watching: directory removed */
            if ((event->mask & (IN_DELETE | IN_ISDIR)) == (IN_DELETE | IN_ISDIR) ||
                (event->mask & (IN_MOVED_FROM | IN_ISDIR)) == (IN_MOVED_FROM | IN_ISDIR)) {
                if (event->len > 0) {
                    inotify_handle_dynamic_rmdir(sw, target, event->name);
                }
            }

            swatcher_fs_event sw_event = inotify_to_swatcher_event(event->mask);

            if (sw_event != SWATCHER_EVENT_NONE) {
                if ((target->events & sw_event) || (target->events == SWATCHER_EVENT_ALL)) {
                    const char *name = event->len > 0 ? event->name : NULL;

                    if (SW_TARGET_INTERNAL(target)->compiled_callback && name) {
                        if (sw_pattern_matched(SW_TARGET_INTERNAL(target)->compiled_callback, name)) {
                            inotify_dispatch_event(sw, target, sw_event, name, event);
                        }
                    } else if (!SW_TARGET_INTERNAL(target)->compiled_callback) {
                        inotify_dispatch_event(sw, target, sw_event, name, event);
                    }
                }
            }

            i += sizeof(struct inotify_event) + event->len;
        }

        sw_mutex_unlock(si->mutex);
    }

    /* Flush remaining coalesced events on shutdown */
    if (coalesce_ms > 0) {
        sw_mutex_lock(si->mutex);
        inotify_coalesce_flush(sw, 0, 0, true);
        sw_mutex_unlock(si->mutex);
    }

    SWATCHER_LOG_INFO(sw, "Watcher thread exiting...");
    return NULL;
}

/* ========== Backend definition ========== */

static const swatcher_backend inotify_backend = {
    .name = "inotify",
    .init = inotify_init_backend,
    .destroy = inotify_destroy,
    .add_target = inotify_add_target,
    .remove_target = inotify_remove_target,
    .add_target_recursive = inotify_add_target_recursive,
    .thread_func = inotify_thread_func,
};

const swatcher_backend *swatcher_backend_inotify(void)
{
    return &inotify_backend;
}

#endif /* __linux__ */
