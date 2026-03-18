#if defined(__linux__)

#define _GNU_SOURCE

#include "swatcher.h"
#include "../internal/internal.h"
#include "../core/error.h"
#include "../core/pattern.h"
#include "../core/rescan.h"

#include <sys/fanotify.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>

/* ========== Local types ========== */

typedef struct swatcher_fanotify {
    int fan_fd;
    struct pollfd fds;
    swatcher_target **targets;
    int target_count;
    int target_capacity;
} swatcher_fanotify;

typedef struct coalesce_entry {
    char path[SW_PATH_MAX];
    swatcher_fs_event event;
    swatcher_target *target;
    uint64_t timestamp_ms;
    bool is_dir;
    char old_path[SW_PATH_MAX];
    UT_hash_handle hh;
} coalesce_entry;

#define FANOTIFY_DATA(sw) ((swatcher_fanotify *)SW_INTERNAL(sw)->backend_data)

/* We store pending coalesce events in the swatcher_internal->backend_data area,
 * but since fanotify doesn't need a hash for watches (it uses filesystem marks),
 * we keep a separate coalesce hash rooted in a file-scope pointer per watcher.
 * To keep things simple and thread-safe (all access under mutex), we embed
 * the hash head in a small struct alongside the main fanotify data. */

typedef struct fanotify_extra {
    swatcher_fanotify fan;
    coalesce_entry *pending_events;
} fanotify_extra;

#define FANOTIFY_EXTRA(sw) ((fanotify_extra *)SW_INTERNAL(sw)->backend_data)

/* Event buffer: fanotify events can be large with FID info */
#define FAN_BUF_LEN (8192)

/* ========== Helpers ========== */

static uint64_t events_to_fanotify_mask(swatcher_target *target)
{
    uint64_t mask = 0;

    if (target->events & SWATCHER_EVENT_ALL) {
        return (FAN_CREATE | FAN_MODIFY | FAN_DELETE | FAN_MOVE |
                FAN_ATTRIB | FAN_ONDIR);
    }

    if (target->events & SWATCHER_EVENT_CREATED)       mask |= FAN_CREATE;
    if (target->events & SWATCHER_EVENT_MODIFIED)       mask |= FAN_MODIFY;
    if (target->events & SWATCHER_EVENT_DELETED)        mask |= FAN_DELETE;
    if (target->events & SWATCHER_EVENT_MOVED)          mask |= FAN_MOVE;
    if (target->events & SWATCHER_EVENT_ATTRIB_CHANGE)  mask |= FAN_ATTRIB;

    /* Always include FAN_ONDIR so we get directory events */
    mask |= FAN_ONDIR;

    return mask;
}

static swatcher_fs_event fanotify_to_swatcher_event(uint64_t mask)
{
    if (mask & FAN_CREATE)  return SWATCHER_EVENT_CREATED;
    if (mask & FAN_MODIFY)  return SWATCHER_EVENT_MODIFIED;
    if (mask & FAN_DELETE)  return SWATCHER_EVENT_DELETED;
    if (mask & (FAN_MOVED_FROM | FAN_MOVED_TO)) return SWATCHER_EVENT_MOVED;
    if (mask & FAN_ATTRIB)  return SWATCHER_EVENT_ATTRIB_CHANGE;
    return SWATCHER_EVENT_NONE;
}

/**
 * Resolve an fanotify FID event to a full path.
 * Returns true on success with the path written to out_path.
 */
static bool resolve_fid_path(int fan_fd, struct fanotify_event_info_fid *fid,
                              char *out_path, size_t out_size)
{
    struct file_handle *fh = (struct file_handle *)fid->handle;

    /* Use AT_FDCWD with the fsid to open the handle */
    int mount_fd = open_by_handle_at(AT_FDCWD, fh, O_RDONLY | O_PATH);
    if (mount_fd < 0) {
        /* Try with the directory mount fd from /proc */
        return false;
    }

    char proc_path[64];
    snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", mount_fd);

    ssize_t len = readlink(proc_path, out_path, out_size - 1);
    close(mount_fd);

    if (len < 0)
        return false;

    out_path[len] = '\0';
    return true;
}

/**
 * Find the best matching target for a given path.
 * For non-recursive targets, the event path must be a direct child.
 * For recursive targets, the event path must be under the target path.
 */
static swatcher_target *find_matching_target(swatcher_fanotify *fan,
                                              const char *event_path)
{
    swatcher_target *best = NULL;
    size_t best_len = 0;

    for (int i = 0; i < fan->target_count; i++) {
        swatcher_target *t = fan->targets[i];
        size_t tlen = strlen(t->path);

        /* Event path must start with target path */
        if (strncmp(event_path, t->path, tlen) != 0)
            continue;

        /* Must be exact match or followed by '/' */
        if (event_path[tlen] != '/' && event_path[tlen] != '\0')
            continue;

        if (t->is_recursive) {
            /* Recursive: any depth under target is fine */
            if (tlen > best_len) {
                best = t;
                best_len = tlen;
            }
        } else {
            /* Non-recursive: only direct children */
            if (event_path[tlen] == '\0') {
                /* Event is the directory itself */
                if (tlen > best_len) {
                    best = t;
                    best_len = tlen;
                }
            } else {
                /* event_path[tlen] == '/' — check for single level */
                const char *rest = event_path + tlen + 1;
                if (strchr(rest, '/') == NULL) {
                    /* Direct child */
                    if (tlen > best_len) {
                        best = t;
                        best_len = tlen;
                    }
                }
            }
        }
    }

    return best;
}

/* ========== Event coalescing (same pattern as inotify) ========== */

static void fanotify_coalesce_dispatch(swatcher *sw, coalesce_entry *ce)
{
    (void)sw;
    if (ce->target && ce->target->callback) {
        const char *name = strrchr(ce->path, '/');
        name = name ? name + 1 : ce->path;
        swatcher_event_info info = {
            .old_path = ce->old_path[0] ? ce->old_path : NULL,
            .is_dir = ce->is_dir,
        };
        ce->target->callback(ce->event, ce->target, name, &info);
        ce->target->last_event_time = time(NULL);
    }
}

static void fanotify_coalesce_flush(swatcher *sw, uint64_t now_ms, int coalesce_ms, bool flush_all)
{
    fanotify_extra *ext = FANOTIFY_EXTRA(sw);
    coalesce_entry *current, *tmp;

    HASH_ITER(hh, ext->pending_events, current, tmp) {
        if (flush_all || (now_ms - current->timestamp_ms >= (uint64_t)coalesce_ms)) {
            fanotify_coalesce_dispatch(sw, current);
            HASH_DEL(ext->pending_events, current);
            sw_free(current);
        }
    }
}

static void fanotify_coalesce_add(swatcher *sw, const char *full_path,
                                   swatcher_fs_event event, swatcher_target *target,
                                   bool is_dir, const char *old_path)
{
    fanotify_extra *ext = FANOTIFY_EXTRA(sw);
    coalesce_entry *existing = NULL;

    HASH_FIND_STR(ext->pending_events, full_path, existing);

    if (existing) {
        /* Merge rules (same as inotify backend) */
        if (existing->event == SWATCHER_EVENT_CREATED && event == SWATCHER_EVENT_DELETED) {
            HASH_DEL(ext->pending_events, existing);
            sw_free(existing);
            return;
        } else if (existing->event == SWATCHER_EVENT_CREATED && event == SWATCHER_EVENT_MODIFIED) {
            existing->timestamp_ms = sw_time_now_ms();
            return;
        } else if (existing->event == SWATCHER_EVENT_MODIFIED && event == SWATCHER_EVENT_MODIFIED) {
            existing->timestamp_ms = sw_time_now_ms();
            return;
        } else if (existing->event == SWATCHER_EVENT_MODIFIED && event == SWATCHER_EVENT_DELETED) {
            existing->event = SWATCHER_EVENT_DELETED;
            existing->timestamp_ms = sw_time_now_ms();
            return;
        }
        existing->event = event;
        existing->timestamp_ms = sw_time_now_ms();
        existing->is_dir = is_dir;
        if (old_path) {
            strncpy(existing->old_path, old_path, SW_PATH_MAX - 1);
            existing->old_path[SW_PATH_MAX - 1] = '\0';
        } else {
            existing->old_path[0] = '\0';
        }
    } else {
        coalesce_entry *ce = sw_malloc(sizeof(coalesce_entry));
        if (!ce) return;
        strncpy(ce->path, full_path, SW_PATH_MAX - 1);
        ce->path[SW_PATH_MAX - 1] = '\0';
        ce->event = event;
        ce->target = target;
        ce->timestamp_ms = sw_time_now_ms();
        ce->is_dir = is_dir;
        if (old_path) {
            strncpy(ce->old_path, old_path, SW_PATH_MAX - 1);
            ce->old_path[SW_PATH_MAX - 1] = '\0';
        } else {
            ce->old_path[0] = '\0';
        }
        HASH_ADD_STR(ext->pending_events, path, ce);
    }
}

/* ========== Dispatch helper ========== */

static void fanotify_dispatch_event(swatcher *sw, swatcher_target *target,
                                     swatcher_fs_event sw_event, const char *name,
                                     bool is_dir, const char *old_path)
{
    int coalesce_ms = sw->config->coalesce_ms;

    if (coalesce_ms > 0 && name) {
        char full_path[SW_PATH_MAX];
        size_t plen = strlen(target->path);
        if (plen > 0 && target->path[plen - 1] == '/')
            snprintf(full_path, SW_PATH_MAX, "%s%s", target->path, name);
        else
            snprintf(full_path, SW_PATH_MAX, "%s/%s", target->path, name);

        fanotify_coalesce_add(sw, full_path, sw_event, target, is_dir, old_path);
    } else {
        swatcher_event_info info = { .old_path = old_path, .is_dir = is_dir };
        target->callback(sw_event, target, name, &info);
        target->last_event_time = time(NULL);
    }
}

/* ========== Vtable functions ========== */

static bool fanotify_init_backend(swatcher *sw)
{
    fanotify_extra *ext = sw_malloc(sizeof(fanotify_extra));
    if (!ext) {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to allocate fanotify data");
        return false;
    }
    memset(ext, 0, sizeof(*ext));

    /* Try to initialize fanotify with FID reporting (Linux 5.1+) */
    ext->fan.fan_fd = fanotify_init(FAN_CLASS_NOTIF | FAN_REPORT_FID | FAN_REPORT_DIR_FID,
                                     O_RDONLY | O_CLOEXEC);
    if (ext->fan.fan_fd < 0) {
        /* Fall back: try without FAN_REPORT_DIR_FID (Linux 5.1 w/o 5.9 features) */
        ext->fan.fan_fd = fanotify_init(FAN_CLASS_NOTIF | FAN_REPORT_FID,
                                         O_RDONLY | O_CLOEXEC);
    }

    if (ext->fan.fan_fd < 0) {
        SWATCHER_LOG_DEFAULT_WARNING(
            "fanotify_init failed (errno %d: %s). "
            "Requires CAP_SYS_ADMIN or Linux 5.1+. Falling back to another backend.",
            errno, strerror(errno));
        sw_free(ext);
        return false;
    }

    ext->fan.fds.fd = ext->fan.fan_fd;
    ext->fan.fds.events = POLLIN;
    ext->fan.targets = NULL;
    ext->fan.target_count = 0;
    ext->fan.target_capacity = 0;
    ext->pending_events = NULL;

    SW_INTERNAL(sw)->backend_data = ext;
    SWATCHER_LOG_DEFAULT_INFO("fanotify backend initialized (fd=%d)", ext->fan.fan_fd);
    return true;
}

static void fanotify_destroy(swatcher *sw)
{
    fanotify_extra *ext = FANOTIFY_EXTRA(sw);
    if (!ext) return;

    /* Flush and free pending coalesce events */
    coalesce_entry *ce, *ce_tmp;
    HASH_ITER(hh, ext->pending_events, ce, ce_tmp) {
        HASH_DEL(ext->pending_events, ce);
        sw_free(ce);
    }

    /* Free target array (targets themselves are freed by the core) */
    sw_free(ext->fan.targets);

    close(ext->fan.fan_fd);
    sw_free(ext);
    SW_INTERNAL(sw)->backend_data = NULL;
}

static bool fanotify_add_single(swatcher *sw, swatcher_target *target)
{
    fanotify_extra *ext = FANOTIFY_EXTRA(sw);
    swatcher_fanotify *fan = &ext->fan;

    if (sw_find_target_internal(sw, target->path)) {
        sw_set_error(SWATCHER_ERR_TARGET_EXISTS);
        SWATCHER_LOG_DEFAULT_WARNING("Path already watched: %s", target->path);
        return false;
    }

    SWATCHER_LOG_DEFAULT_INFO("fanotify: watching %s", target->path);

    uint64_t mask = events_to_fanotify_mask(target);

    /* Mark the filesystem at this path.
     * FAN_MARK_FILESYSTEM watches the entire filesystem — we filter by path
     * in the event loop instead. For per-directory granularity, use FAN_MARK_ADD
     * without FAN_MARK_FILESYSTEM. */
    int ret = fanotify_mark(fan->fan_fd,
                             FAN_MARK_ADD,
                             mask,
                             AT_FDCWD, target->path);
    if (ret < 0) {
        sw_set_error(SWATCHER_ERR_BACKEND_INIT);
        SWATCHER_LOG_DEFAULT_ERROR("fanotify_mark failed for %s: %s",
                                    target->path, strerror(errno));
        return false;
    }

    /* Track the target in our array */
    if (fan->target_count >= fan->target_capacity) {
        int new_cap = fan->target_capacity ? fan->target_capacity * 2 : 16;
        swatcher_target **new_arr = sw_realloc(fan->targets,
                                                (size_t)new_cap * sizeof(swatcher_target *));
        if (!new_arr) {
            sw_set_error(SWATCHER_ERR_ALLOC);
            return false;
        }
        fan->targets = new_arr;
        fan->target_capacity = new_cap;
    }
    fan->targets[fan->target_count++] = target;

    /* Create target internal and add to hash */
    swatcher_target_internal *ti = SW_TARGET_INTERNAL(target);
    if (!ti) {
        ti = sw_target_internal_create(target);
        if (!ti) {
            SWATCHER_LOG_DEFAULT_ERROR("Failed to create target internal");
            fan->target_count--;
            return false;
        }
    }
    sw_add_target_internal(sw, ti);

    return true;
}

static bool fanotify_add_target(swatcher *sw, swatcher_target *target)
{
    return fanotify_add_single(sw, target);
}

static bool fanotify_remove_target(swatcher *sw, swatcher_target *target)
{
    fanotify_extra *ext = FANOTIFY_EXTRA(sw);
    swatcher_fanotify *fan = &ext->fan;

    /* Remove the fanotify mark */
    uint64_t mask = events_to_fanotify_mask(target);
    fanotify_mark(fan->fan_fd, FAN_MARK_REMOVE, mask, AT_FDCWD, target->path);

    /* Remove from target array */
    for (int i = 0; i < fan->target_count; i++) {
        if (fan->targets[i] == target) {
            fan->targets[i] = fan->targets[--fan->target_count];
            break;
        }
    }

    return true;
}

static bool fanotify_add_recursive(swatcher *sw, swatcher_target *original_target, bool dont_add_self)
{
    /* For fanotify, recursive watching is simpler than inotify because
     * a single mark can cover a directory and its subtree. We still need to
     * add sub-directory targets for proper pattern matching and filtering. */

    if (original_target->is_file) {
        return fanotify_add_single(sw, original_target);
    }

    sw_dir *dir = sw_dir_open(original_target->path);
    if (!dir) {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to open directory: %s", original_target->path);
        return false;
    }

    sw_dir_entry entry;
    while (sw_dir_next(dir, &entry)) {
        if (SW_TARGET_INTERNAL(original_target) &&
            SW_TARGET_INTERNAL(original_target)->compiled_ignore) {
            if (sw_pattern_matched(SW_TARGET_INTERNAL(original_target)->compiled_ignore, entry.name))
                continue;
        }

        bool watch_match = !SW_TARGET_INTERNAL(original_target) ||
                           !SW_TARGET_INTERNAL(original_target)->compiled_watch ||
                           sw_pattern_matched(SW_TARGET_INTERNAL(original_target)->compiled_watch, entry.name);

        if (watch_match || entry.is_dir) {
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
                    SWATCHER_LOG_DEFAULT_WARNING("Failed to create target for %s", new_path);
                    continue;
                }

                bool skip_self = (original_target->watch_options == SWATCHER_WATCH_FILES ||
                                  original_target->watch_options == SWATCHER_WATCH_SYMLINKS ||
                                  (SW_TARGET_INTERNAL(original_target) &&
                                   SW_TARGET_INTERNAL(original_target)->compiled_watch &&
                                   !sw_pattern_matched(SW_TARGET_INTERNAL(original_target)->compiled_watch, entry.name)));

                if (!fanotify_add_recursive(sw, new_target, skip_self)) {
                    SWATCHER_LOG_DEFAULT_WARNING("Failed to watch dir: %s", new_target->path);
                    sw_free(new_target->path);
                    sw_free(new_target);
                    continue;
                }
            }
        }
    }

    sw_dir_close(dir);

    if (dont_add_self)
        return true;

    return fanotify_add_single(sw, original_target);
}

static bool fanotify_add_target_recursive(swatcher *sw, swatcher_target *target, bool dont_add_self)
{
    return fanotify_add_recursive(sw, target, dont_add_self);
}

/* ========== Thread function ========== */

static void *fanotify_thread_func(void *arg)
{
    swatcher *sw = (swatcher *)arg;
    fanotify_extra *ext = FANOTIFY_EXTRA(sw);
    swatcher_fanotify *fan = &ext->fan;
    swatcher_internal *si = SW_INTERNAL(sw);
    char buffer[FAN_BUF_LEN] __attribute__((aligned(__alignof__(struct fanotify_event_metadata))));
    int coalesce_ms = sw->config->coalesce_ms;
    int poll_timeout = sw->config->poll_interval_ms;

    if (coalesce_ms > 0 && poll_timeout > coalesce_ms)
        poll_timeout = coalesce_ms;

    while (sw_atomic_load(&sw->running)) {
        int poll_ret = poll(&fan->fds, 1, poll_timeout);
        if (poll_ret < 0) {
            if (errno == EINTR)
                continue;
            SWATCHER_LOG_DEFAULT_ERROR("fanotify poll failed: %s", strerror(errno));
            break;
        }

        sw_mutex_lock(si->mutex);

        /* Flush expired coalesce entries */
        if (coalesce_ms > 0) {
            fanotify_coalesce_flush(sw, sw_time_now_ms(), coalesce_ms, false);
        }

        sw_mutex_unlock(si->mutex);

        if (poll_ret == 0)
            continue;

        ssize_t len = read(fan->fan_fd, buffer, sizeof(buffer));
        if (len < 0) {
            if (errno == EINTR)
                continue;
            SWATCHER_LOG_DEFAULT_ERROR("Error reading fanotify fd: %s", strerror(errno));
            break;
        }

        sw_mutex_lock(si->mutex);

        struct fanotify_event_metadata *meta = (struct fanotify_event_metadata *)buffer;
        while (FAN_EVENT_OK(meta, len)) {
            /* Skip permission events (we don't use them) */
            if (meta->mask & (FAN_ALL_PERM_EVENTS)) {
                /* Shouldn't happen with FAN_CLASS_NOTIF, but be safe */
                meta = FAN_EVENT_NEXT(meta, len);
                continue;
            }

            /* Handle overflow */
            if (meta->mask & FAN_Q_OVERFLOW) {
                SWATCHER_LOG_DEFAULT_WARNING("fanotify queue overflow");
                for (int i = 0; i < fan->target_count; i++) {
                    if (fan->targets[i]->callback) {
                        fan->targets[i]->callback(SWATCHER_EVENT_OVERFLOW,
                                                   fan->targets[i], NULL, NULL);
                    }
                }
                meta = FAN_EVENT_NEXT(meta, len);
                continue;
            }

            /* With FAN_REPORT_FID, meta->fd is FAN_NOFD and we get FID info */
            char resolved_path[SW_PATH_MAX];
            bool path_resolved = false;

            if (meta->fd == FAN_NOFD) {
                /* Walk the event info headers to find FID */
                struct fanotify_event_info_header *info =
                    (struct fanotify_event_info_header *)(meta + 1);
                size_t remaining = meta->event_len - sizeof(*meta);

                while (remaining >= sizeof(*info)) {
                    if (info->info_type == FAN_EVENT_INFO_TYPE_DFID_NAME) {
                        /* Directory FID + filename */
                        struct fanotify_event_info_fid *fid =
                            (struct fanotify_event_info_fid *)info;

                        /* The filename follows the file_handle */
                        struct file_handle *fh = (struct file_handle *)fid->handle;
                        const char *filename = (const char *)(fh->f_handle + fh->handle_bytes);

                        /* Resolve the directory handle */
                        char dir_path[SW_PATH_MAX];
                        int mount_fd = open_by_handle_at(AT_FDCWD, fh, O_RDONLY | O_PATH);
                        if (mount_fd >= 0) {
                            char proc_path[64];
                            snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", mount_fd);
                            ssize_t rlen = readlink(proc_path, dir_path, sizeof(dir_path) - 1);
                            close(mount_fd);

                            if (rlen > 0) {
                                dir_path[rlen] = '\0';
                                if (filename[0] != '\0') {
                                    snprintf(resolved_path, SW_PATH_MAX, "%s/%s", dir_path, filename);
                                } else {
                                    strncpy(resolved_path, dir_path, SW_PATH_MAX - 1);
                                    resolved_path[SW_PATH_MAX - 1] = '\0';
                                }
                                path_resolved = true;
                            }
                        }
                        break;
                    } else if (info->info_type == FAN_EVENT_INFO_TYPE_FID) {
                        /* Plain FID without name — resolve directly */
                        struct fanotify_event_info_fid *fid =
                            (struct fanotify_event_info_fid *)info;
                        if (resolve_fid_path(fan->fan_fd, fid, resolved_path, SW_PATH_MAX))
                            path_resolved = true;
                        break;
                    }

                    /* Advance to next info header */
                    if (info->len == 0) break;
                    remaining -= info->len;
                    info = (struct fanotify_event_info_header *)((char *)info + info->len);
                }
            } else {
                /* Older-style event with fd — read via /proc/self/fd */
                char proc_path[64];
                snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", meta->fd);
                ssize_t rlen = readlink(proc_path, resolved_path, SW_PATH_MAX - 1);
                if (rlen > 0) {
                    resolved_path[rlen] = '\0';
                    path_resolved = true;
                }
                close(meta->fd);
            }

            if (!path_resolved) {
                meta = FAN_EVENT_NEXT(meta, len);
                continue;
            }

            /* Determine if the event is for a directory */
            bool is_dir = (meta->mask & FAN_ONDIR) != 0;

            /* Find the matching target for this path */
            swatcher_target *target = find_matching_target(fan, resolved_path);
            if (!target) {
                meta = FAN_EVENT_NEXT(meta, len);
                continue;
            }

            /* Translate the event */
            swatcher_fs_event sw_event = fanotify_to_swatcher_event(meta->mask);
            if (sw_event == SWATCHER_EVENT_NONE) {
                meta = FAN_EVENT_NEXT(meta, len);
                continue;
            }

            /* Check if the target subscribes to this event type */
            if (!(target->events & sw_event) && target->events != SWATCHER_EVENT_ALL) {
                meta = FAN_EVENT_NEXT(meta, len);
                continue;
            }

            /* Extract the filename from the resolved path */
            const char *name = strrchr(resolved_path, '/');
            name = name ? name + 1 : resolved_path;

            /* Apply callback pattern matching */
            swatcher_target_internal *ti = SW_TARGET_INTERNAL(target);
            if (ti && ti->compiled_callback) {
                if (!sw_pattern_matched(ti->compiled_callback, name)) {
                    meta = FAN_EVENT_NEXT(meta, len);
                    continue;
                }
            }

            /* Dynamic recursive watching: new directory appeared */
            if (is_dir && (sw_event == SWATCHER_EVENT_CREATED || sw_event == SWATCHER_EVENT_MOVED) &&
                target->is_recursive) {
                /* Check if this new directory needs a watch */
                if (!sw_find_target_internal(sw, resolved_path)) {
                    swatcher_target_desc desc = {
                        .path = resolved_path,
                        .is_recursive = target->is_recursive,
                        .events = target->events,
                        .watch_options = target->watch_options,
                        .follow_symlinks = target->follow_symlinks,
                        .user_data = target->user_data,
                        .callback_patterns = target->callback_patterns,
                        .watch_patterns = target->watch_patterns,
                        .ignore_patterns = target->ignore_patterns,
                        .callback = target->callback
                    };

                    swatcher_target *new_target = swatcher_target_create(&desc);
                    if (new_target) {
                        if (!fanotify_add_recursive(sw, new_target, false)) {
                            sw_free(new_target->path);
                            sw_free(new_target);
                        }
                    }
                }
            }

            /* Dispatch the event */
            fanotify_dispatch_event(sw, target, sw_event, name, is_dir, NULL);

            meta = FAN_EVENT_NEXT(meta, len);
        }

        sw_mutex_unlock(si->mutex);
    }

    /* Flush remaining coalesced events on shutdown */
    if (coalesce_ms > 0) {
        sw_mutex_lock(si->mutex);
        fanotify_coalesce_flush(sw, 0, 0, true);
        sw_mutex_unlock(si->mutex);
    }

    SWATCHER_LOG_DEFAULT_INFO("fanotify watcher thread exiting...");
    return NULL;
}

/* ========== Backend definition ========== */

static const swatcher_backend fanotify_backend = {
    .name = "fanotify",
    .init = fanotify_init_backend,
    .destroy = fanotify_destroy,
    .add_target = fanotify_add_target,
    .remove_target = fanotify_remove_target,
    .add_target_recursive = fanotify_add_target_recursive,
    .thread_func = fanotify_thread_func,
};

const swatcher_backend *swatcher_backend_fanotify(void)
{
    return &fanotify_backend;
}

#endif /* __linux__ */
