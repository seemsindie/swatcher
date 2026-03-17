#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__)

#include "swatcher.h"
#include "../internal/internal.h"

#include <sys/event.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>

/* Pattern matching via compiled patterns */
#include "../core/pattern.h"

/* ========== Local types ========== */

typedef struct dir_child_entry {
    char name[256];             /* hash key */
    bool is_directory;
    UT_hash_handle hh;
} dir_child_entry;

typedef struct kqueue_watch {
    int fd;                     /* hash key - opened via O_EVTONLY */
    char path[SW_PATH_MAX];
    bool is_directory;
    swatcher_target *target;
    dir_child_entry *children;  /* directory child snapshot for rescan diffing */
    UT_hash_handle hh;
} kqueue_watch;

typedef struct coalesce_entry {
    char path[SW_PATH_MAX];
    swatcher_fs_event event;
    swatcher_target *target;
    uint64_t timestamp_ms;
    UT_hash_handle hh;
} coalesce_entry;

typedef struct swatcher_kqueue {
    int kq;                     /* kqueue fd */
    kqueue_watch *watches;      /* uthash by fd */
    int watch_count;
    int max_fds;                /* from getrlimit(RLIMIT_NOFILE) */
    coalesce_entry *pending_events;
} swatcher_kqueue;

#define KQUEUE_DATA(sw) ((swatcher_kqueue *)SW_INTERNAL(sw)->backend_data)

/* ========== fd limit checking ========== */

static int kqueue_get_max_fds(void)
{
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0)
        return (int)rl.rlim_cur;
    return -1;
}

/* ========== Open path helper ========== */

static int kqueue_open_path(const char *path, bool is_directory, bool follow_symlinks)
{
    int flags = O_RDONLY;
    (void)is_directory;

#if defined(__APPLE__)
    flags = O_EVTONLY;
    if (!follow_symlinks)
        flags |= O_SYMLINK;
#else
    if (!follow_symlinks)
        flags |= O_NOFOLLOW;
#endif

    return open(path, flags);
}

/* ========== Directory snapshot helpers ========== */

static dir_child_entry *snapshot_directory(const char *path)
{
    dir_child_entry *children = NULL;
    sw_dir *d = sw_dir_open(path);
    if (!d) return NULL;

    sw_dir_entry entry;
    while (sw_dir_next(d, &entry)) {
        if (strcmp(entry.name, ".") == 0 || strcmp(entry.name, "..") == 0)
            continue;

        dir_child_entry *ce = malloc(sizeof(dir_child_entry));
        if (!ce) continue;
        strncpy(ce->name, entry.name, sizeof(ce->name) - 1);
        ce->name[sizeof(ce->name) - 1] = '\0';
        ce->is_directory = entry.is_dir;
        HASH_ADD_STR(children, name, ce);
    }
    sw_dir_close(d);
    return children;
}

static void free_children(dir_child_entry *children)
{
    dir_child_entry *current, *tmp;
    HASH_ITER(hh, children, current, tmp) {
        HASH_DEL(children, current);
        free(current);
    }
}

/* ========== Register fd with kqueue ========== */

static bool kqueue_register_fd(int kq, int fd, kqueue_watch *kw)
{
    struct kevent ev;
    unsigned int fflags = NOTE_DELETE | NOTE_WRITE | NOTE_EXTEND |
                          NOTE_ATTRIB | NOTE_RENAME | NOTE_REVOKE;

    EV_SET(&ev, fd, EVFILT_VNODE, EV_ADD | EV_CLEAR, fflags, 0, kw);

    if (kevent(kq, &ev, 1, NULL, 0, NULL) < 0) {
        SWATCHER_LOG_DEFAULT_ERROR("kevent register failed for fd %d: %s", fd, strerror(errno));
        return false;
    }
    return true;
}

/* ========== Forward declarations ========== */

static bool kqueue_add_single(swatcher *sw, swatcher_target *target);
static bool kqueue_add_recursive_locked(swatcher *sw, swatcher_target *target, bool dont_add_self);
static void kqueue_remove_watch(swatcher *sw, kqueue_watch *kw);

/* ========== Add single watch ========== */

static bool kqueue_add_single(swatcher *sw, swatcher_target *target)
{
    swatcher_kqueue *kqd = KQUEUE_DATA(sw);

    if (sw_find_target_internal(sw, target->path)) {
        SWATCHER_LOG_DEFAULT_WARNING("Path already watched: %s", target->path);
        return false;
    }

    /* Check fd limits */
    if (kqd->max_fds > 0 && kqd->watch_count >= (int)(kqd->max_fds * 0.9)) {
        SWATCHER_LOG_DEFAULT_WARNING("Approaching fd limit (%d/%d): %s",
                                      kqd->watch_count, kqd->max_fds, target->path);
    }
    if (kqd->max_fds > 0 && kqd->watch_count >= kqd->max_fds - 10) {
        SWATCHER_LOG_DEFAULT_ERROR("Too close to fd limit (%d/%d), cannot watch: %s",
                                    kqd->watch_count, kqd->max_fds, target->path);
        return false;
    }

    int fd = kqueue_open_path(target->path, target->is_directory, target->follow_symlinks);
    if (fd < 0) {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to open %s for watching: %s",
                                    target->path, strerror(errno));
        return false;
    }

    kqueue_watch *kw = malloc(sizeof(kqueue_watch));
    if (!kw) {
        close(fd);
        SWATCHER_LOG_DEFAULT_ERROR("Failed to allocate kqueue_watch");
        return false;
    }

    kw->fd = fd;
    strncpy(kw->path, target->path, SW_PATH_MAX - 1);
    kw->path[SW_PATH_MAX - 1] = '\0';
    kw->is_directory = target->is_directory;
    kw->target = target;
    kw->children = NULL;

    if (target->is_directory) {
        kw->children = snapshot_directory(target->path);
    }

    if (!kqueue_register_fd(kqd->kq, fd, kw)) {
        if (kw->children) free_children(kw->children);
        free(kw);
        close(fd);
        return false;
    }

    HASH_ADD_INT(kqd->watches, fd, kw);
    kqd->watch_count++;

    /* Create target internal and add to hash */
    swatcher_target_internal *ti = SW_TARGET_INTERNAL(target);
    if (!ti) {
        ti = sw_target_internal_create(target);
        if (!ti) {
            SWATCHER_LOG_DEFAULT_ERROR("Failed to create target internal");
            HASH_DEL(kqd->watches, kw);
            kqd->watch_count--;
            if (kw->children) free_children(kw->children);
            close(fd);
            free(kw);
            return false;
        }
    }
    ti->backend_data = kw;
    sw_add_target_internal(sw, ti);

    SWATCHER_LOG_DEFAULT_INFO("Target %s is being watched (fd=%d)", target->path, fd);
    return true;
}

/* ========== Recursive add ========== */

static bool kqueue_add_recursive_locked(swatcher *sw, swatcher_target *original_target, bool dont_add_self)
{
    if (original_target->is_file) {
        return kqueue_add_single(sw, original_target);
    }

    sw_dir *dir = sw_dir_open(original_target->path);
    if (!dir) {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to open directory: %s", original_target->path);
        return false;
    }

    sw_dir_entry entry;
    while (sw_dir_next(dir, &entry)) {
        if (SW_TARGET_INTERNAL(original_target) && SW_TARGET_INTERNAL(original_target)->compiled_ignore) {
            if (sw_pattern_matched(SW_TARGET_INTERNAL(original_target)->compiled_ignore, entry.name))
                continue;
        }

        if (!SW_TARGET_INTERNAL(original_target) ||
            !SW_TARGET_INTERNAL(original_target)->compiled_watch ||
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
                                  (SW_TARGET_INTERNAL(original_target) &&
                                   SW_TARGET_INTERNAL(original_target)->compiled_watch &&
                                   !sw_pattern_matched(SW_TARGET_INTERNAL(original_target)->compiled_watch, entry.name)));

                if (!kqueue_add_recursive_locked(sw, new_target, skip_self)) {
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

                    if (!kqueue_add_single(sw, new_target)) {
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
                            if (!kqueue_add_single(sw, new_target)) {
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
                            if (!kqueue_add_recursive_locked(sw, new_target, false)) {
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

                        if (!kqueue_add_single(sw, new_target)) {
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

    return kqueue_add_single(sw, original_target);
}

/* ========== Remove children of a directory ========== */

static void kqueue_remove_children(swatcher *sw, const char *dir_path)
{
    swatcher_kqueue *kqd = KQUEUE_DATA(sw);
    size_t dir_len = strlen(dir_path);

    kqueue_watch *current, *tmp;
    HASH_ITER(hh, kqd->watches, current, tmp) {
        const char *tpath = current->path;
        if (strncmp(tpath, dir_path, dir_len) == 0 && tpath[dir_len] == '/') {
            close(current->fd);
            HASH_DEL(kqd->watches, current);
            kqd->watch_count--;

            swatcher_target_internal *ti = SW_TARGET_INTERNAL(current->target);
            swatcher_target *t = current->target;
            if (ti) {
                sw_remove_target_internal(sw, ti);
                ti->backend_data = NULL;
                ti->target = NULL;
                sw_target_internal_destroy(ti);
            }
            if (current->children) free_children(current->children);
            free(current);

            if (t) {
                free(t->path);
                free(t);
            }
        }
    }
}

/* ========== Dynamic recursive helpers ========== */

static void kqueue_handle_dynamic_mkdir(swatcher *sw, swatcher_target *parent, const char *name)
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

    if (!kqueue_add_recursive_locked(sw, new_target, false)) {
        SWATCHER_LOG_DEFAULT_WARNING("Failed to watch new dir: %s", new_path);
        free(new_target->path);
        free(new_target);
    }
}

static void kqueue_handle_dynamic_rmdir(swatcher *sw, const char *dir_path)
{
    kqueue_remove_children(sw, dir_path);

    /* Remove the directory's own watch if it exists */
    swatcher_kqueue *kqd = KQUEUE_DATA(sw);
    swatcher_target_internal *ti = sw_find_target_internal(sw, dir_path);
    if (ti) {
        kqueue_watch *kw = (kqueue_watch *)ti->backend_data;
        if (kw) {
            close(kw->fd);
            HASH_DEL(kqd->watches, kw);
            kqd->watch_count--;
            if (kw->children) free_children(kw->children);
            free(kw);
            ti->backend_data = NULL;
        }
        sw_remove_target_internal(sw, ti);
        swatcher_target *t = ti->target;
        ti->target = NULL;
        if (t) {
            free(t->path);
            free(t);
        }
        sw_target_internal_destroy(ti);
    }
}

/* ========== Event coalescing (ported from inotify) ========== */

static void kqueue_coalesce_dispatch(swatcher *sw, coalesce_entry *ce)
{
    (void)sw;
    if (ce->target && ce->target->callback) {
        const char *name = strrchr(ce->path, '/');
        name = name ? name + 1 : ce->path;
        ce->target->callback(ce->event, ce->target, name, NULL);
        ce->target->last_event_time = time(NULL);
    }
}

static void kqueue_coalesce_flush(swatcher *sw, uint64_t now_ms, int coalesce_ms, bool flush_all)
{
    swatcher_kqueue *kqd = KQUEUE_DATA(sw);
    coalesce_entry *current, *tmp;

    HASH_ITER(hh, kqd->pending_events, current, tmp) {
        if (flush_all || (now_ms - current->timestamp_ms >= (uint64_t)coalesce_ms)) {
            kqueue_coalesce_dispatch(sw, current);
            HASH_DEL(kqd->pending_events, current);
            free(current);
        }
    }
}

static void kqueue_coalesce_add(swatcher *sw, const char *full_path,
                                 swatcher_fs_event event, swatcher_target *target)
{
    swatcher_kqueue *kqd = KQUEUE_DATA(sw);
    coalesce_entry *existing = NULL;

    HASH_FIND_STR(kqd->pending_events, full_path, existing);

    if (existing) {
        if (existing->event == SWATCHER_EVENT_CREATED && event == SWATCHER_EVENT_DELETED) {
            HASH_DEL(kqd->pending_events, existing);
            free(existing);
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
    } else {
        coalesce_entry *ce = malloc(sizeof(coalesce_entry));
        if (!ce) return;
        strncpy(ce->path, full_path, SW_PATH_MAX - 1);
        ce->path[SW_PATH_MAX - 1] = '\0';
        ce->event = event;
        ce->target = target;
        ce->timestamp_ms = sw_time_now_ms();
        HASH_ADD_STR(kqd->pending_events, path, ce);
    }
}

/* ========== Event dispatch ========== */

static void kqueue_dispatch_event(swatcher *sw, swatcher_target *target,
                                   swatcher_fs_event sw_event, const char *name)
{
    int coalesce_ms = sw->config->coalesce_ms;

    if (coalesce_ms > 0 && name) {
        char full_path[SW_PATH_MAX];
        size_t plen = strlen(target->path);
        if (plen > 0 && target->path[plen - 1] == '/')
            snprintf(full_path, SW_PATH_MAX, "%s%s", target->path, name);
        else
            snprintf(full_path, SW_PATH_MAX, "%s/%s", target->path, name);

        kqueue_coalesce_add(sw, full_path, sw_event, target);
    } else {
        if (target->callback) {
            target->callback(sw_event, target, name, NULL);
            target->last_event_time = time(NULL);
        }
    }
}

/* ========== Directory rescan ========== */

static void kqueue_rescan_directory(swatcher *sw, kqueue_watch *kw)
{
    swatcher_target *target = kw->target;
    dir_child_entry *new_children = snapshot_directory(kw->path);

    /* Find new entries (in new but not old) → CREATED */
    dir_child_entry *nc, *nc_tmp;
    HASH_ITER(hh, new_children, nc, nc_tmp) {
        dir_child_entry *old = NULL;
        HASH_FIND_STR(kw->children, nc->name, old);
        if (!old) {
            /* New child detected */
            const char *name = nc->name;

            /* Check callback pattern filter */
            swatcher_target_internal *ti = SW_TARGET_INTERNAL(target);
            if (ti && ti->compiled_callback) {
                if (!sw_pattern_matched(ti->compiled_callback, name))
                    goto skip_created;
            }

            if ((target->events & SWATCHER_EVENT_CREATED) || (target->events == SWATCHER_EVENT_ALL)) {
                kqueue_dispatch_event(sw, target, SWATCHER_EVENT_CREATED, name);
            }

skip_created:
            /* If recursive and new child is a directory, start watching it */
            if (nc->is_directory && target->is_recursive) {
                kqueue_handle_dynamic_mkdir(sw, target, name);
            }
        }
    }

    /* Find deleted entries (in old but not new) → DELETED */
    dir_child_entry *oc, *oc_tmp;
    HASH_ITER(hh, kw->children, oc, oc_tmp) {
        dir_child_entry *found = NULL;
        HASH_FIND_STR(new_children, oc->name, found);
        if (!found) {
            const char *name = oc->name;

            swatcher_target_internal *ti = SW_TARGET_INTERNAL(target);
            if (ti && ti->compiled_callback) {
                if (!sw_pattern_matched(ti->compiled_callback, name))
                    goto skip_deleted;
            }

            if ((target->events & SWATCHER_EVENT_DELETED) || (target->events == SWATCHER_EVENT_ALL)) {
                kqueue_dispatch_event(sw, target, SWATCHER_EVENT_DELETED, name);
            }

skip_deleted:
            /* If it was a directory, clean up its watches */
            if (oc->is_directory) {
                char dir_path[SW_PATH_MAX];
                size_t plen = strlen(kw->path);
                if (plen > 0 && kw->path[plen - 1] == '/')
                    snprintf(dir_path, SW_PATH_MAX, "%s%s", kw->path, name);
                else
                    snprintf(dir_path, SW_PATH_MAX, "%s/%s", kw->path, name);
                kqueue_handle_dynamic_rmdir(sw, dir_path);
            }
        }
    }

    /* Replace old snapshot with new */
    free_children(kw->children);
    kw->children = new_children;
}

/* ========== Vtable functions ========== */

static bool kqueue_init_backend(swatcher *sw)
{
    swatcher_kqueue *kqd = malloc(sizeof(swatcher_kqueue));
    if (!kqd) {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to allocate swatcher_kqueue");
        return false;
    }

    kqd->kq = kqueue();
    if (kqd->kq < 0) {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to create kqueue: %s", strerror(errno));
        free(kqd);
        return false;
    }

    kqd->watches = NULL;
    kqd->watch_count = 0;
    kqd->max_fds = kqueue_get_max_fds();
    kqd->pending_events = NULL;

    SW_INTERNAL(sw)->backend_data = kqd;
    return true;
}

static void kqueue_destroy(swatcher *sw)
{
    swatcher_kqueue *kqd = KQUEUE_DATA(sw);
    if (!kqd) return;

    /* Flush and free pending coalesce events */
    coalesce_entry *ce, *ce_tmp;
    HASH_ITER(hh, kqd->pending_events, ce, ce_tmp) {
        HASH_DEL(kqd->pending_events, ce);
        free(ce);
    }

    /* Close all watch fds and free watches */
    kqueue_watch *kw, *kw_tmp;
    HASH_ITER(hh, kqd->watches, kw, kw_tmp) {
        close(kw->fd);
        HASH_DEL(kqd->watches, kw);
        if (kw->children) free_children(kw->children);
        free(kw);
    }

    close(kqd->kq);
    free(kqd);
    SW_INTERNAL(sw)->backend_data = NULL;
}

static bool kqueue_add_target(swatcher *sw, swatcher_target *target)
{
    return kqueue_add_single(sw, target);
}

static bool kqueue_remove_target(swatcher *sw, swatcher_target *target)
{
    swatcher_kqueue *kqd = KQUEUE_DATA(sw);
    swatcher_target_internal *ti = SW_TARGET_INTERNAL(target);
    if (!ti) return false;

    kqueue_watch *kw = (kqueue_watch *)ti->backend_data;
    if (kw) {
        close(kw->fd);
        HASH_DEL(kqd->watches, kw);
        kqd->watch_count--;
        if (kw->children) free_children(kw->children);
        free(kw);
        ti->backend_data = NULL;
    }

    return true;
}

static bool kqueue_add_target_recursive(swatcher *sw, swatcher_target *target, bool dont_add_self)
{
    return kqueue_add_recursive_locked(sw, target, dont_add_self);
}

static void *kqueue_thread_func(void *arg)
{
    swatcher *sw = (swatcher *)arg;
    swatcher_kqueue *kqd = KQUEUE_DATA(sw);
    swatcher_internal *si = SW_INTERNAL(sw);
    int coalesce_ms = sw->config->coalesce_ms;

    struct kevent events[32];
    struct timespec timeout;

    /* Use poll_interval_ms as timeout; shorten for coalescing */
    int timeout_ms = sw->config->poll_interval_ms;
    if (coalesce_ms > 0 && timeout_ms > coalesce_ms)
        timeout_ms = coalesce_ms;

    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_nsec = (timeout_ms % 1000) * 1000000L;

    while (sw_atomic_load(&sw->running)) {
        int nev = kevent(kqd->kq, NULL, 0, events, 32, &timeout);
        if (nev < 0) {
            if (errno == EINTR)
                continue;
            SWATCHER_LOG_DEFAULT_ERROR("kevent failed: %s", strerror(errno));
            break;
        }

        /* Flush expired coalesce entries */
        if (coalesce_ms > 0) {
            sw_mutex_lock(si->mutex);
            kqueue_coalesce_flush(sw, sw_time_now_ms(), coalesce_ms, false);
            sw_mutex_unlock(si->mutex);
        }

        if (nev == 0)
            continue;

        sw_mutex_lock(si->mutex);

        for (int i = 0; i < nev; i++) {
            kqueue_watch *kw = (kqueue_watch *)events[i].udata;
            if (!kw) continue;

            unsigned int fflags = events[i].fflags;
            swatcher_target *target = kw->target;

            /* Directory NOTE_WRITE → rescan for child changes */
            if (kw->is_directory && (fflags & NOTE_WRITE)) {
                kqueue_rescan_directory(sw, kw);
                /* Don't also emit MODIFIED for the directory itself */
                fflags &= ~(NOTE_WRITE | NOTE_EXTEND);
            }

            /* NOTE_DELETE — the watched path was deleted */
            if (fflags & NOTE_DELETE) {
                if ((target->events & SWATCHER_EVENT_DELETED) || (target->events == SWATCHER_EVENT_ALL)) {
                    const char *name = strrchr(kw->path, '/');
                    name = name ? name + 1 : kw->path;

                    swatcher_target_internal *ti = SW_TARGET_INTERNAL(target);
                    bool pass_filter = true;
                    if (ti && ti->compiled_callback) {
                        pass_filter = sw_pattern_matched(ti->compiled_callback, name);
                    }
                    if (pass_filter) {
                        kqueue_dispatch_event(sw, target, SWATCHER_EVENT_DELETED, name);
                    }
                }

                /* Clean up: close fd, remove from hash */
                close(kw->fd);
                HASH_DEL(kqd->watches, kw);
                kqd->watch_count--;

                swatcher_target_internal *ti = SW_TARGET_INTERNAL(target);
                if (ti) {
                    sw_remove_target_internal(sw, ti);
                    ti->backend_data = NULL;
                    ti->target = NULL;
                    sw_target_internal_destroy(ti);
                }
                if (kw->children) free_children(kw->children);
                free(kw);
                free(target->path);
                free(target);
                continue;
            }

            /* NOTE_RENAME — the watched path was renamed */
            if (fflags & NOTE_RENAME) {
                if ((target->events & SWATCHER_EVENT_MOVED) || (target->events == SWATCHER_EVENT_ALL)) {
                    const char *name = strrchr(kw->path, '/');
                    name = name ? name + 1 : kw->path;

                    swatcher_target_internal *ti = SW_TARGET_INTERNAL(target);
                    bool pass_filter = true;
                    if (ti && ti->compiled_callback) {
                        pass_filter = sw_pattern_matched(ti->compiled_callback, name);
                    }
                    if (pass_filter) {
                        kqueue_dispatch_event(sw, target, SWATCHER_EVENT_MOVED, name);
                    }
                }

                close(kw->fd);
                HASH_DEL(kqd->watches, kw);
                kqd->watch_count--;

                swatcher_target_internal *ti = SW_TARGET_INTERNAL(target);
                if (ti) {
                    sw_remove_target_internal(sw, ti);
                    ti->backend_data = NULL;
                    ti->target = NULL;
                    sw_target_internal_destroy(ti);
                }
                if (kw->children) free_children(kw->children);
                free(kw);
                free(target->path);
                free(target);
                continue;
            }

            /* NOTE_WRITE/NOTE_EXTEND on file → MODIFIED */
            if (fflags & (NOTE_WRITE | NOTE_EXTEND)) {
                if ((target->events & SWATCHER_EVENT_MODIFIED) || (target->events == SWATCHER_EVENT_ALL)) {
                    const char *name = strrchr(kw->path, '/');
                    name = name ? name + 1 : kw->path;

                    swatcher_target_internal *ti = SW_TARGET_INTERNAL(target);
                    bool pass_filter = true;
                    if (ti && ti->compiled_callback) {
                        pass_filter = sw_pattern_matched(ti->compiled_callback, name);
                    }
                    if (pass_filter) {
                        kqueue_dispatch_event(sw, target, SWATCHER_EVENT_MODIFIED, name);
                    }
                }
            }

            /* NOTE_ATTRIB → ATTRIB_CHANGE */
            if (fflags & NOTE_ATTRIB) {
                if ((target->events & SWATCHER_EVENT_ATTRIB_CHANGE) || (target->events == SWATCHER_EVENT_ALL)) {
                    const char *name = strrchr(kw->path, '/');
                    name = name ? name + 1 : kw->path;

                    swatcher_target_internal *ti = SW_TARGET_INTERNAL(target);
                    bool pass_filter = true;
                    if (ti && ti->compiled_callback) {
                        pass_filter = sw_pattern_matched(ti->compiled_callback, name);
                    }
                    if (pass_filter) {
                        kqueue_dispatch_event(sw, target, SWATCHER_EVENT_ATTRIB_CHANGE, name);
                    }
                }
            }
        }

        sw_mutex_unlock(si->mutex);
    }

    /* Flush remaining coalesced events on shutdown */
    if (coalesce_ms > 0) {
        sw_mutex_lock(si->mutex);
        kqueue_coalesce_flush(sw, 0, 0, true);
        sw_mutex_unlock(si->mutex);
    }

    SWATCHER_LOG_DEFAULT_INFO("kqueue watcher thread exiting...");
    return NULL;
}

/* ========== Backend definition ========== */

static const swatcher_backend kqueue_backend = {
    .name = "kqueue",
    .init = kqueue_init_backend,
    .destroy = kqueue_destroy,
    .add_target = kqueue_add_target,
    .remove_target = kqueue_remove_target,
    .add_target_recursive = kqueue_add_target_recursive,
    .thread_func = kqueue_thread_func,
};

const swatcher_backend *swatcher_backend_kqueue(void)
{
    return &kqueue_backend;
}

#endif /* __APPLE__ || __FreeBSD__ || __OpenBSD__ || __NetBSD__ || __DragonFly__ */
