#if defined(__linux__)

#include "swatcher.h"
#include "../internal/internal.h"
#include "../internal/uring_syscall.h"
#include "../core/error.h"
#include "../core/pattern.h"
#include "../core/rescan.h"
#include "../core/vcs.h"

#include <sys/inotify.h>
#include <sys/mman.h>
#include <poll.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>

/* ========== Constants ========== */

#define URING_ENTRIES     32
#define URING_READ_BUFSZ  8192
#define EVENT_SIZE        (sizeof(struct inotify_event))

/* ========== Local types (mirrors inotify backend) ========== */

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
    bool is_dir;
    char old_path[SW_PATH_MAX];
    UT_hash_handle hh;
} coalesce_entry;

typedef struct move_pending {
    uint32_t cookie;
    char path[SW_PATH_MAX];
    swatcher_target *target;
    bool is_dir;
    uint64_t timestamp_ms;
    UT_hash_handle hh;
} move_pending;

typedef struct swatcher_uring {
    int inotify_fd;
    int ring_fd;
    void *sq_ring;
    void *cq_ring;
    void *sqes;
    struct io_sqring_offsets sq_off;
    struct io_cqring_offsets cq_off;
    unsigned sq_entries;
    unsigned cq_entries;
    size_t sq_ring_sz;
    size_t cq_ring_sz;
    size_t sqes_sz;
    /* inotify target tracking */
    swatcher_target_inotify *wd_to_target;
    int watch_count;
    int max_watches;
    coalesce_entry *pending_events;
    move_pending *pending_moves;
    char read_buf[URING_READ_BUFSZ] __attribute__((aligned(8)));
} swatcher_uring;

#define URING_DATA(sw) ((swatcher_uring *)SW_INTERNAL(sw)->backend_data)

/* ========== io_uring ring helpers ========== */

static inline uint32_t *uring_sq_head(swatcher_uring *u)
{
    return (uint32_t *)((char *)u->sq_ring + u->sq_off.head);
}

static inline uint32_t *uring_sq_tail(swatcher_uring *u)
{
    return (uint32_t *)((char *)u->sq_ring + u->sq_off.tail);
}

static inline uint32_t *uring_sq_ring_mask(swatcher_uring *u)
{
    return (uint32_t *)((char *)u->sq_ring + u->sq_off.ring_mask);
}

static inline uint32_t *uring_sq_array(swatcher_uring *u)
{
    return (uint32_t *)((char *)u->sq_ring + u->sq_off.array);
}

static inline uint32_t *uring_cq_head(swatcher_uring *u)
{
    return (uint32_t *)((char *)u->cq_ring + u->cq_off.head);
}

static inline uint32_t *uring_cq_tail(swatcher_uring *u)
{
    return (uint32_t *)((char *)u->cq_ring + u->cq_off.tail);
}

static inline uint32_t *uring_cq_ring_mask(swatcher_uring *u)
{
    return (uint32_t *)((char *)u->cq_ring + u->cq_off.ring_mask);
}

static inline struct io_uring_cqe *uring_cqe_at(swatcher_uring *u, uint32_t idx)
{
    struct io_uring_cqe *cqes = (struct io_uring_cqe *)((char *)u->cq_ring + u->cq_off.cqes);
    return &cqes[idx & *uring_cq_ring_mask(u)];
}

/* Memory barriers for shared ring access */
#define io_uring_smp_store_release(p, v)  __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#define io_uring_smp_load_acquire(p)      __atomic_load_n((p), __ATOMIC_ACQUIRE)

/* Submit an IORING_OP_READ for the inotify fd into read_buf */
static bool uring_submit_read(swatcher_uring *u)
{
    uint32_t tail = io_uring_smp_load_acquire(uring_sq_tail(u));
    uint32_t head = io_uring_smp_load_acquire(uring_sq_head(u));
    uint32_t mask = *uring_sq_ring_mask(u);

    /* Check if the SQ is full */
    if ((tail - head) >= u->sq_entries)
        return false;

    uint32_t idx = tail & mask;
    struct io_uring_sqe *sqe = &((struct io_uring_sqe *)u->sqes)[idx];

    memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = IORING_OP_READ;
    sqe->fd = u->inotify_fd;
    sqe->addr = (uint64_t)(uintptr_t)u->read_buf;
    sqe->len = URING_READ_BUFSZ;
    sqe->off = (uint64_t)-1; /* read at current position */
    sqe->user_data = 1;      /* tag: inotify read */

    /* Update the SQ array and tail */
    uring_sq_array(u)[idx] = idx;
    io_uring_smp_store_release(uring_sq_tail(u), tail + 1);

    return true;
}

/* ========== Helpers (same as inotify backend) ========== */

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

static int uring_get_max_watches(void)
{
    FILE *f = fopen("/proc/sys/fs/inotify/max_user_watches", "r");
    if (!f) return -1;

    int val = -1;
    if (fscanf(f, "%d", &val) != 1)
        val = -1;
    fclose(f);
    return val;
}

/* ========== add single watch ========== */

static bool uring_add_single(swatcher *sw, swatcher_target *target)
{
    swatcher_uring *u = URING_DATA(sw);

    if (sw_find_target_internal(sw, target->path)) {
        sw_set_error(SWATCHER_ERR_TARGET_EXISTS);
        SWATCHER_LOG_DEFAULT_WARNING("Path already watched: %s", target->path);
        return false;
    }

    if (u->max_watches > 0 && u->watch_count >= (int)(u->max_watches * 0.9)) {
        SWATCHER_LOG_DEFAULT_WARNING("Approaching inotify watch limit (%d/%d): %s",
                                      u->watch_count, u->max_watches, target->path);
    }

    SWATCHER_LOG_DEFAULT_INFO("Target %s is being watched (io_uring)", target->path);

    swatcher_target_inotify *ino_target = sw_malloc(sizeof(swatcher_target_inotify));
    if (!ino_target) {
        sw_set_error(SWATCHER_ERR_ALLOC);
        SWATCHER_LOG_DEFAULT_ERROR("Failed to allocate swatcher_target_inotify");
        return false;
    }

    ino_target->target = target;
    ino_target->wd = inotify_add_watch(u->inotify_fd, target->path, events_to_inotify_mask(target));
    if (ino_target->wd < 0) {
        if (errno == ENOSPC) {
            sw_set_error(SWATCHER_ERR_WATCH_LIMIT);
            SWATCHER_LOG_DEFAULT_ERROR(
                "inotify watch limit reached (%d/%d). "
                "Increase via: sysctl fs.inotify.max_user_watches=524288",
                u->watch_count, u->max_watches);
        } else {
            sw_set_error(SWATCHER_ERR_BACKEND_INIT);
            SWATCHER_LOG_DEFAULT_ERROR("Failed to add watch for %s: %s",
                                        target->path, strerror(errno));
        }
        sw_free(ino_target);
        return false;
    }

    HASH_ADD_INT(u->wd_to_target, wd, ino_target);
    u->watch_count++;

    swatcher_target_internal *ti = SW_TARGET_INTERNAL(target);
    if (!ti) {
        ti = sw_target_internal_create(target);
        if (!ti) {
            SWATCHER_LOG_DEFAULT_ERROR("Failed to create target internal");
            inotify_rm_watch(u->inotify_fd, ino_target->wd);
            HASH_DEL(u->wd_to_target, ino_target);
            u->watch_count--;
            sw_free(ino_target);
            return false;
        }
    }
    ti->backend_data = ino_target;
    sw_add_target_internal(sw, ti);

    return true;
}

/* ========== Recursive add ========== */

static bool uring_add_recursive_locked(swatcher *sw, swatcher_target *original_target, bool dont_add_self)
{
    if (original_target->is_file) {
        return uring_add_single(sw, original_target);
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

                if (!uring_add_recursive_locked(sw, new_target, skip_self)) {
                    SWATCHER_LOG_DEFAULT_WARNING("Failed to add watch for %s", new_target->path);
                    sw_free(new_target->path);
                    sw_free(new_target);
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

                    if (!uring_add_single(sw, new_target)) {
                        SWATCHER_LOG_DEFAULT_WARNING("Failed to add watch (file) for %s", new_target->path);
                        sw_free(new_target->path);
                        sw_free(new_target);
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
                            if (!uring_add_single(sw, new_target)) {
                                sw_free(new_target->path);
                                sw_free(new_target);
                                continue;
                            }
                        } else {
                            sw_free(new_target->path);
                            sw_free(new_target);
                            continue;
                        }
                    } else {
                        if (original_target->watch_options & SWATCHER_WATCH_DIRECTORIES ||
                            original_target->watch_options == SWATCHER_WATCH_ALL) {
                            if (!uring_add_recursive_locked(sw, new_target, false)) {
                                sw_free(new_target->path);
                                sw_free(new_target);
                                continue;
                            }
                        } else {
                            sw_free(new_target->path);
                            sw_free(new_target);
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

                        if (!uring_add_single(sw, new_target)) {
                            sw_free(new_target->path);
                            sw_free(new_target);
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

    return uring_add_single(sw, original_target);
}

/* ========== Dynamic recursive helpers ========== */

static void uring_remove_children(swatcher *sw, const char *dir_path)
{
    swatcher_uring *u = URING_DATA(sw);
    size_t dir_len = strlen(dir_path);

    swatcher_target_inotify *current, *tmp;
    HASH_ITER(hh, u->wd_to_target, current, tmp) {
        const char *tpath = current->target->path;
        if (strncmp(tpath, dir_path, dir_len) == 0 && tpath[dir_len] == '/') {
            inotify_rm_watch(u->inotify_fd, current->wd);
            HASH_DEL(u->wd_to_target, current);
            u->watch_count--;

            swatcher_target_internal *ti = SW_TARGET_INTERNAL(current->target);
            swatcher_target *t = current->target;
            if (ti) {
                sw_remove_target_internal(sw, ti);
                ti->backend_data = NULL;
                ti->target = NULL;
                sw_target_internal_destroy(ti);
            }
            sw_free(t->path);
            sw_free(t);
            sw_free(current);
        }
    }
}

static void uring_handle_dynamic_mkdir(swatcher *sw, swatcher_target *parent, const char *name)
{
    if (!parent->is_recursive) return;

    char new_path[SW_PATH_MAX];
    size_t plen = strlen(parent->path);
    if (plen > 0 && parent->path[plen - 1] == '/')
        snprintf(new_path, SW_PATH_MAX, "%s%s", parent->path, name);
    else
        snprintf(new_path, SW_PATH_MAX, "%s/%s", parent->path, name);

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

    if (!uring_add_recursive_locked(sw, new_target, false)) {
        SWATCHER_LOG_DEFAULT_WARNING("Failed to watch new dir: %s", new_path);
        sw_free(new_target->path);
        sw_free(new_target);
    }
}

static void uring_handle_dynamic_rmdir(swatcher *sw, swatcher_target *parent, const char *name)
{
    char dir_path[SW_PATH_MAX];
    size_t plen = strlen(parent->path);
    if (plen > 0 && parent->path[plen - 1] == '/')
        snprintf(dir_path, SW_PATH_MAX, "%s%s", parent->path, name);
    else
        snprintf(dir_path, SW_PATH_MAX, "%s/%s", parent->path, name);

    uring_remove_children(sw, dir_path);

    swatcher_uring *u = URING_DATA(sw);
    swatcher_target_internal *ti = sw_find_target_internal(sw, dir_path);
    if (ti) {
        swatcher_target_inotify *ino_target = (swatcher_target_inotify *)ti->backend_data;
        if (ino_target) {
            inotify_rm_watch(u->inotify_fd, ino_target->wd);
            HASH_DEL(u->wd_to_target, ino_target);
            u->watch_count--;
            sw_free(ino_target);
            ti->backend_data = NULL;
        }
        sw_remove_target_internal(sw, ti);
        swatcher_target *t = ti->target;
        ti->target = NULL;
        sw_free(t->path);
        sw_free(t);
        sw_target_internal_destroy(ti);
    }
}

/* ========== Rescan on overflow ========== */

static void uring_rescan_all(swatcher *sw)
{
    swatcher_uring *u = URING_DATA(sw);

    swatcher_target_inotify *current, *tmp;
    HASH_ITER(hh, u->wd_to_target, current, tmp) {
        if (current->target->is_recursive && current->target->is_directory) {
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
                    uring_handle_dynamic_mkdir(sw, current->target, entry.name);
                }
            }
            sw_dir_close(dir);
        }
    }
}

/* ========== Event coalescing ========== */

static void uring_coalesce_dispatch(swatcher *sw, coalesce_entry *ce)
{
    if (ce->target && ce->target->callback) {
        if (sw_vcs_should_pause(sw->config, ce->target->path))
            return;
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

static void uring_coalesce_flush(swatcher *sw, uint64_t now_ms, int coalesce_ms, bool flush_all)
{
    swatcher_uring *u = URING_DATA(sw);
    coalesce_entry *current, *tmp;

    HASH_ITER(hh, u->pending_events, current, tmp) {
        if (flush_all || (now_ms - current->timestamp_ms >= (uint64_t)coalesce_ms)) {
            uring_coalesce_dispatch(sw, current);
            HASH_DEL(u->pending_events, current);
            sw_free(current);
        }
    }
}

static void uring_coalesce_add(swatcher *sw, const char *full_path,
                                swatcher_fs_event event, swatcher_target *target,
                                bool is_dir, const char *old_path)
{
    swatcher_uring *u = URING_DATA(sw);
    coalesce_entry *existing = NULL;

    HASH_FIND_STR(u->pending_events, full_path, existing);

    if (existing) {
        if (existing->event == SWATCHER_EVENT_CREATED && event == SWATCHER_EVENT_DELETED) {
            HASH_DEL(u->pending_events, existing);
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
        HASH_ADD_STR(u->pending_events, path, ce);
    }
}

static void uring_dispatch_event(swatcher *sw, swatcher_target *target,
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

        uring_coalesce_add(sw, full_path, sw_event, target, is_dir, old_path);
    } else {
        if (sw_vcs_should_pause(sw->config, target->path))
            return;
        swatcher_event_info info = { .old_path = old_path, .is_dir = is_dir };
        target->callback(sw_event, target, name, &info);
        target->last_event_time = time(NULL);
    }
}

/* ========== Process inotify events from read buffer ========== */

static void uring_process_inotify_events(swatcher *sw, int length)
{
    swatcher_uring *u = URING_DATA(sw);
    swatcher_internal *si = SW_INTERNAL(sw);
    int coalesce_ms = sw->config->coalesce_ms;
    int i = 0;

    sw_mutex_lock(si->mutex);

    while (i < length) {
        struct inotify_event *event = (struct inotify_event *)&u->read_buf[i];

        /* Handle IN_Q_OVERFLOW first (wd == -1) */
        if (event->mask & IN_Q_OVERFLOW) {
            SWATCHER_LOG_DEFAULT_WARNING("inotify queue overflow -- rescanning all watches (io_uring)");
            uring_rescan_all(sw);

            swatcher_target_inotify *ot, *ot_tmp;
            HASH_ITER(hh, u->wd_to_target, ot, ot_tmp) {
                if (ot->target->callback) {
                    ot->target->callback(SWATCHER_EVENT_OVERFLOW, ot->target, NULL, NULL);
                }
            }

            if (sw->config->overflow_rescan) {
                swatcher_target_inotify *rt, *rt_tmp;
                HASH_ITER(hh, u->wd_to_target, rt, rt_tmp) {
                    if (rt->target->is_directory) {
                        sw_rescan_entry *snap = sw_rescan_snapshot(rt->target->path, rt->target->is_recursive);
                        sw_rescan_diff(NULL, snap, rt->target);
                        sw_rescan_free(snap);
                    }
                }
            }

            i += sizeof(struct inotify_event) + event->len;
            continue;
        }

        /* Handle IN_IGNORED */
        if (event->mask & IN_IGNORED) {
            i += sizeof(struct inotify_event) + event->len;
            continue;
        }

        swatcher_target_inotify *target_data = NULL;
        HASH_FIND_INT(u->wd_to_target, &event->wd, target_data);
        if (!target_data) {
            i += sizeof(struct inotify_event) + event->len;
            continue;
        }

        swatcher_target *target = target_data->target;

        /* Dynamic recursive watching: new directory appeared */
        if ((event->mask & (IN_CREATE | IN_ISDIR)) == (IN_CREATE | IN_ISDIR) ||
            (event->mask & (IN_MOVED_TO | IN_ISDIR)) == (IN_MOVED_TO | IN_ISDIR)) {
            if (event->len > 0) {
                uring_handle_dynamic_mkdir(sw, target, event->name);
            }
        }

        /* Dynamic recursive watching: directory removed */
        if ((event->mask & (IN_DELETE | IN_ISDIR)) == (IN_DELETE | IN_ISDIR) ||
            (event->mask & (IN_MOVED_FROM | IN_ISDIR)) == (IN_MOVED_FROM | IN_ISDIR)) {
            if (event->len > 0) {
                uring_handle_dynamic_rmdir(sw, target, event->name);
            }
        }

        bool ev_is_dir = (event->mask & IN_ISDIR) != 0;

        /* Move pairing */
        if (event->mask & IN_MOVED_FROM) {
            if (event->len > 0 && event->cookie != 0) {
                move_pending *mp = sw_malloc(sizeof(move_pending));
                if (mp) {
                    mp->cookie = event->cookie;
                    size_t plen = strlen(target->path);
                    if (plen > 0 && target->path[plen - 1] == '/')
                        snprintf(mp->path, SW_PATH_MAX, "%s%s", target->path, event->name);
                    else
                        snprintf(mp->path, SW_PATH_MAX, "%s/%s", target->path, event->name);
                    mp->target = target;
                    mp->is_dir = ev_is_dir;
                    mp->timestamp_ms = sw_time_now_ms();
                    HASH_ADD(hh, u->pending_moves, cookie, sizeof(uint32_t), mp);
                }
            }
            i += sizeof(struct inotify_event) + event->len;
            continue;
        }

        if (event->mask & IN_MOVED_TO) {
            const char *name = event->len > 0 ? event->name : NULL;
            const char *old_path = NULL;
            move_pending *mp = NULL;

            if (event->cookie != 0) {
                HASH_FIND(hh, u->pending_moves, &event->cookie, sizeof(uint32_t), mp);
            }
            if (mp) {
                old_path = mp->path;
                HASH_DEL(u->pending_moves, mp);
            }

            if (name && ((target->events & SWATCHER_EVENT_MOVED) || (target->events == SWATCHER_EVENT_ALL))) {
                swatcher_target_internal *ti = SW_TARGET_INTERNAL(target);
                bool pass = !ti->compiled_callback || sw_pattern_matched(ti->compiled_callback, name);
                if (pass)
                    uring_dispatch_event(sw, target, SWATCHER_EVENT_MOVED, name, ev_is_dir, old_path);
            }

            if (mp) sw_free(mp);
            i += sizeof(struct inotify_event) + event->len;
            continue;
        }

        swatcher_fs_event sw_event = inotify_to_swatcher_event(event->mask);

        if (sw_event != SWATCHER_EVENT_NONE) {
            if ((target->events & sw_event) || (target->events == SWATCHER_EVENT_ALL)) {
                const char *name = event->len > 0 ? event->name : NULL;

                if (SW_TARGET_INTERNAL(target)->compiled_callback && name) {
                    if (sw_pattern_matched(SW_TARGET_INTERNAL(target)->compiled_callback, name)) {
                        uring_dispatch_event(sw, target, sw_event, name, ev_is_dir, NULL);
                    }
                } else if (!SW_TARGET_INTERNAL(target)->compiled_callback) {
                    uring_dispatch_event(sw, target, sw_event, name, ev_is_dir, NULL);
                }
            }
        }

        i += sizeof(struct inotify_event) + event->len;
    }

    /* Flush expired coalesce entries */
    if (coalesce_ms > 0) {
        uring_coalesce_flush(sw, sw_time_now_ms(), coalesce_ms, false);
    }

    sw_mutex_unlock(si->mutex);
}

/* ========== Vtable functions ========== */

static bool uring_init_backend(swatcher *sw)
{
    swatcher_uring *u = sw_malloc(sizeof(swatcher_uring));
    if (!u) {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to allocate swatcher_uring");
        return false;
    }
    memset(u, 0, sizeof(*u));
    u->ring_fd = -1;
    u->inotify_fd = -1;

    /* Try to set up io_uring -- if this fails, the caller can fall back */
    struct io_uring_params params;
    memset(&params, 0, sizeof(params));

    u->ring_fd = io_uring_setup(URING_ENTRIES, &params);
    if (u->ring_fd < 0) {
        SWATCHER_LOG_DEFAULT_INFO("io_uring_setup failed (errno %d: %s) -- kernel may be too old",
                                   errno, strerror(errno));
        sw_free(u);
        return false;
    }

    /* Save ring offsets */
    u->sq_off = params.sq_off;
    u->cq_off = params.cq_off;
    u->sq_entries = params.sq_entries;
    u->cq_entries = params.cq_entries;

    /* mmap the submission ring */
    u->sq_ring_sz = params.sq_off.array + params.sq_entries * sizeof(uint32_t);
    u->sq_ring = mmap(0, u->sq_ring_sz, PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_POPULATE, u->ring_fd, IORING_OFF_SQ_RING);
    if (u->sq_ring == MAP_FAILED) {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to mmap SQ ring: %s", strerror(errno));
        close(u->ring_fd);
        sw_free(u);
        return false;
    }

    /* mmap the completion ring */
    u->cq_ring_sz = params.cq_off.cqes + params.cq_entries * sizeof(struct io_uring_cqe);
    u->cq_ring = mmap(0, u->cq_ring_sz, PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_POPULATE, u->ring_fd, IORING_OFF_CQ_RING);
    if (u->cq_ring == MAP_FAILED) {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to mmap CQ ring: %s", strerror(errno));
        munmap(u->sq_ring, u->sq_ring_sz);
        close(u->ring_fd);
        sw_free(u);
        return false;
    }

    /* mmap the SQE array */
    u->sqes_sz = params.sq_entries * sizeof(struct io_uring_sqe);
    u->sqes = mmap(0, u->sqes_sz, PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_POPULATE, u->ring_fd, IORING_OFF_SQES);
    if (u->sqes == MAP_FAILED) {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to mmap SQEs: %s", strerror(errno));
        munmap(u->cq_ring, u->cq_ring_sz);
        munmap(u->sq_ring, u->sq_ring_sz);
        close(u->ring_fd);
        sw_free(u);
        return false;
    }

    /* Create inotify fd */
    u->inotify_fd = inotify_init();
    if (u->inotify_fd < 0) {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to initialize inotify for io_uring backend");
        munmap(u->sqes, u->sqes_sz);
        munmap(u->cq_ring, u->cq_ring_sz);
        munmap(u->sq_ring, u->sq_ring_sz);
        close(u->ring_fd);
        sw_free(u);
        return false;
    }

    u->wd_to_target = NULL;
    u->watch_count = 0;
    u->max_watches = uring_get_max_watches();
    u->pending_events = NULL;
    u->pending_moves = NULL;

    SW_INTERNAL(sw)->backend_data = u;
    SWATCHER_LOG_DEFAULT_INFO("io_uring backend initialized (sq=%u, cq=%u)",
                               u->sq_entries, u->cq_entries);
    return true;
}

static void uring_destroy(swatcher *sw)
{
    swatcher_uring *u = URING_DATA(sw);
    if (!u) return;

    /* Flush and free pending coalesce events */
    coalesce_entry *ce, *ce_tmp;
    HASH_ITER(hh, u->pending_events, ce, ce_tmp) {
        HASH_DEL(u->pending_events, ce);
        sw_free(ce);
    }

    /* Free pending move entries */
    move_pending *mp, *mp_tmp;
    HASH_ITER(hh, u->pending_moves, mp, mp_tmp) {
        HASH_DEL(u->pending_moves, mp);
        sw_free(mp);
    }

    /* Remove all inotify watches */
    swatcher_target_inotify *current, *tmp;
    HASH_ITER(hh, u->wd_to_target, current, tmp) {
        inotify_rm_watch(u->inotify_fd, current->wd);
        HASH_DEL(u->wd_to_target, current);
        sw_free(current);
    }

    if (u->inotify_fd >= 0)
        close(u->inotify_fd);

    /* Unmap rings */
    if (u->sqes && u->sqes != MAP_FAILED)
        munmap(u->sqes, u->sqes_sz);
    if (u->cq_ring && u->cq_ring != MAP_FAILED)
        munmap(u->cq_ring, u->cq_ring_sz);
    if (u->sq_ring && u->sq_ring != MAP_FAILED)
        munmap(u->sq_ring, u->sq_ring_sz);

    if (u->ring_fd >= 0)
        close(u->ring_fd);

    sw_free(u);
    SW_INTERNAL(sw)->backend_data = NULL;
}

static bool uring_add_target(swatcher *sw, swatcher_target *target)
{
    return uring_add_single(sw, target);
}

static bool uring_remove_target(swatcher *sw, swatcher_target *target)
{
    swatcher_uring *u = URING_DATA(sw);
    swatcher_target_internal *ti = SW_TARGET_INTERNAL(target);
    if (!ti) return false;

    swatcher_target_inotify *ino_target = (swatcher_target_inotify *)ti->backend_data;
    if (ino_target) {
        inotify_rm_watch(u->inotify_fd, ino_target->wd);
        HASH_DEL(u->wd_to_target, ino_target);
        u->watch_count--;
        sw_free(ino_target);
        ti->backend_data = NULL;
    }

    return true;
}

static bool uring_add_target_recursive(swatcher *sw, swatcher_target *target, bool dont_add_self)
{
    return uring_add_recursive_locked(sw, target, dont_add_self);
}

static void *uring_thread_func(void *arg)
{
    swatcher *sw = (swatcher *)arg;
    swatcher_uring *u = URING_DATA(sw);
    swatcher_internal *si = SW_INTERNAL(sw);
    int coalesce_ms = sw->config->coalesce_ms;

    bool read_pending = false;

    while (sw_atomic_load(&sw->running)) {
        /* Submit a read on the inotify fd if none pending */
        if (!read_pending) {
            if (!uring_submit_read(u)) {
                SWATCHER_LOG_DEFAULT_ERROR("io_uring: failed to prepare SQE for read");
                sw_sleep_ms(10);
                continue;
            }
            /* Submit the SQE (no wait) */
            int ret = io_uring_enter(u->ring_fd, 1, 0, 0, NULL);
            if (ret < 0 && errno != EINTR) {
                SWATCHER_LOG_DEFAULT_ERROR("io_uring_enter submit failed: %s", strerror(errno));
                break;
            }
            read_pending = true;
        }

        /* Poll the ring fd with a short timeout so we can check sw->running */
        struct pollfd pfd = { .fd = u->ring_fd, .events = POLLIN };
        int poll_ret = poll(&pfd, 1, 100);
        if (poll_ret < 0) {
            if (errno == EINTR) continue;
            SWATCHER_LOG_DEFAULT_ERROR("poll on ring_fd failed: %s", strerror(errno));
            break;
        }
        if (poll_ret == 0)
            goto flush_coalesce; /* timeout — check running flag and flush */

        /* Process completions */
        uint32_t cq_head = io_uring_smp_load_acquire(uring_cq_head(u));
        uint32_t cq_tail = io_uring_smp_load_acquire(uring_cq_tail(u));

        while (cq_head != cq_tail) {
            struct io_uring_cqe *cqe = uring_cqe_at(u, cq_head);

            if (cqe->user_data == 1) {
                /* inotify read completion */
                read_pending = false;
                if (cqe->res > 0) {
                    uring_process_inotify_events(sw, cqe->res);
                } else if (cqe->res < 0 && cqe->res != -EINTR) {
                    SWATCHER_LOG_DEFAULT_ERROR("io_uring read completion error: %s",
                                                strerror(-cqe->res));
                }
            }

            cq_head++;
        }

        /* Advance the CQ head */
        io_uring_smp_store_release(uring_cq_head(u), cq_head);

flush_coalesce:
        /* Expire unmatched MOVED_FROM entries and flush coalesce */
        sw_mutex_lock(si->mutex);
        {
            uint64_t now = sw_time_now_ms();
            int expire_ms = coalesce_ms > 0 ? coalesce_ms : 100;
            move_pending *mp, *mp_tmp;
            HASH_ITER(hh, u->pending_moves, mp, mp_tmp) {
                if (now - mp->timestamp_ms >= (uint64_t)expire_ms) {
                    const char *name = strrchr(mp->path, '/');
                    name = name ? name + 1 : mp->path;
                    if (mp->target && mp->target->callback &&
                        ((mp->target->events & SWATCHER_EVENT_DELETED) || (mp->target->events == SWATCHER_EVENT_ALL))) {
                        uring_dispatch_event(sw, mp->target, SWATCHER_EVENT_DELETED, name, mp->is_dir, NULL);
                    }
                    HASH_DEL(u->pending_moves, mp);
                    sw_free(mp);
                }
            }

            if (coalesce_ms > 0) {
                uring_coalesce_flush(sw, sw_time_now_ms(), coalesce_ms, false);
            }
        }
        sw_mutex_unlock(si->mutex);
    }

    /* Flush remaining coalesced events on shutdown */
    if (coalesce_ms > 0) {
        sw_mutex_lock(si->mutex);
        uring_coalesce_flush(sw, 0, 0, true);
        sw_mutex_unlock(si->mutex);
    }

    SWATCHER_LOG_INFO(sw, "io_uring watcher thread exiting...");
    return NULL;
}

/* ========== Backend definition ========== */

static const swatcher_backend uring_backend = {
    .name = "io_uring",
    .init = uring_init_backend,
    .destroy = uring_destroy,
    .add_target = uring_add_target,
    .remove_target = uring_remove_target,
    .add_target_recursive = uring_add_target_recursive,
    .thread_func = uring_thread_func,
};

const swatcher_backend *swatcher_backend_uring(void)
{
    return &uring_backend;
}

#endif /* __linux__ */
