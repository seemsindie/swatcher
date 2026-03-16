#if defined(__linux__) || defined(__unix__) || defined(__unix) || defined(unix)

#include "swatcher.h"
#include "../internal/internal.h"

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

typedef struct swatcher_inotify {
    int inotify_fd;
    struct pollfd fds;
    swatcher_target_inotify *wd_to_target;
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

/* ========== add single watch (called with mutex held) ========== */

static bool inotify_add_single(swatcher *sw, swatcher_target *target)
{
    swatcher_inotify *ino = INOTIFY_DATA(sw);

    if (sw_find_target_internal(sw, target->path)) {
        SWATCHER_LOG_DEFAULT_WARNING("Path already watched: %s", target->path);
        return false;
    }

    SWATCHER_LOG_DEFAULT_INFO("Target %s is being watched", target->path);

    swatcher_target_inotify *ino_target = malloc(sizeof(swatcher_target_inotify));
    if (!ino_target) {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to allocate swatcher_target_inotify");
        return false;
    }

    ino_target->target = target;
    ino_target->wd = inotify_add_watch(ino->inotify_fd, target->path, events_to_inotify_mask(target));
    if (ino_target->wd < 0) {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to add watch for %s", target->path);
        free(ino_target);
        return false;
    }

    HASH_ADD_INT(ino->wd_to_target, wd, ino_target);

    /* Create target internal and add to hash */
    swatcher_target_internal *ti = SW_TARGET_INTERNAL(target);
    if (!ti) {
        ti = sw_target_internal_create(target);
        if (!ti) {
            SWATCHER_LOG_DEFAULT_ERROR("Failed to create target internal");
            inotify_rm_watch(ino->inotify_fd, ino_target->wd);
            HASH_DEL(ino->wd_to_target, ino_target);
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

    SW_INTERNAL(sw)->backend_data = ino;
    return true;
}

static void inotify_destroy(swatcher *sw)
{
    swatcher_inotify *ino = INOTIFY_DATA(sw);
    if (!ino) return;

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
        free(ino_target);
        ti->backend_data = NULL;
    }

    return true;
}

static bool inotify_add_target_recursive(swatcher *sw, swatcher_target *target, bool dont_add_self)
{
    return inotify_add_recursive_locked(sw, target, dont_add_self);
}

static void *inotify_thread_func(void *arg)
{
    swatcher *sw = (swatcher *)arg;
    swatcher_inotify *ino = INOTIFY_DATA(sw);
    char buffer[BUF_LEN];

    while (sw->running) {
        int poll_ret = poll(&ino->fds, 1, sw->config->poll_interval_ms);
        if (poll_ret < 0) {
            SWATCHER_LOG_DEFAULT_ERROR("poll failed");
            break;
        }
        if (poll_ret == 0)
            continue;

        int length = read(ino->inotify_fd, buffer, BUF_LEN);
        if (length < 0) {
            SWATCHER_LOG_ERROR(sw, "Error reading inotify FD");
            break;
        }

        int i = 0;
        while (i < length) {
            struct inotify_event *event = (struct inotify_event *)&buffer[i];
            swatcher_target_inotify *target_data = NULL;

            HASH_FIND_INT(ino->wd_to_target, &event->wd, target_data);
            if (!target_data) {
                i += sizeof(struct inotify_event) + event->len;
                continue;
            }

            swatcher_target *target = target_data->target;
            swatcher_fs_event sw_event = inotify_to_swatcher_event(event->mask);

            if (sw_event != SWATCHER_EVENT_NONE) {
                if ((target->events & sw_event) || (target->events == SWATCHER_EVENT_ALL)) {
                    const char *name = event->len > 0 ? event->name : NULL;

                    if (SW_TARGET_INTERNAL(target)->compiled_callback && name) {
                        if (sw_pattern_matched(SW_TARGET_INTERNAL(target)->compiled_callback, name)) {
                            target->callback(sw_event, target, name, event);
                            target->last_event_time = time(NULL);
                        }
                    } else if (!SW_TARGET_INTERNAL(target)->compiled_callback) {
                        target->callback(sw_event, target, name, event);
                        target->last_event_time = time(NULL);
                    }
                }
            }

            i += sizeof(struct inotify_event) + event->len;
        }
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

const swatcher_backend *swatcher_backend_default(void)
{
    return swatcher_backend_inotify();
}

#endif /* __linux__ */
