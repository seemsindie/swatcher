#include "swatcher.h"
#include "../internal/internal.h"
#include "../core/pattern.h"

/* ========== Local types ========== */

typedef struct poll_file_snapshot {
    char *path;             /* full path — hash key */
    uint64_t mtime;
    uint64_t size;
    bool seen;              /* marked during current scan cycle */
    bool is_new;            /* added during current scan cycle */
    UT_hash_handle hh;
} poll_file_snapshot;

typedef struct swatcher_target_poll {
    swatcher_target *target;
    poll_file_snapshot *snapshots;   /* uthash head */
    UT_hash_handle hh;
} swatcher_target_poll;

typedef struct swatcher_poll {
    swatcher_target_poll *targets;   /* uthash head — keyed by target pointer */
} swatcher_poll;

#define POLL_DATA(sw) ((swatcher_poll *)SW_INTERNAL(sw)->backend_data)

/* ========== Snapshot helpers ========== */

static poll_file_snapshot *snapshot_find(swatcher_target_poll *tp, const char *path)
{
    poll_file_snapshot *found = NULL;
    HASH_FIND_STR(tp->snapshots, path, found);
    return found;
}

static poll_file_snapshot *snapshot_add(swatcher_target_poll *tp, const char *path,
                                        uint64_t mtime, uint64_t size)
{
    poll_file_snapshot *snap = malloc(sizeof(poll_file_snapshot));
    if (!snap) return NULL;
    snap->path = sw_strdup(path);
    if (!snap->path) {
        free(snap);
        return NULL;
    }
    snap->mtime = mtime;
    snap->size = size;
    snap->seen = true;
    HASH_ADD_STR(tp->snapshots, path, snap);
    return snap;
}

static void snapshot_free(poll_file_snapshot *snap)
{
    free(snap->path);
    free(snap);
}

static void snapshots_clear(swatcher_target_poll *tp)
{
    poll_file_snapshot *current, *tmp;
    HASH_ITER(hh, tp->snapshots, current, tmp) {
        HASH_DEL(tp->snapshots, current);
        snapshot_free(current);
    }
}

/* ========== Directory walking ========== */

static void emit_event(swatcher_target *target, swatcher_fs_event event, const char *path)
{
    if (!(target->events & event) && target->events != SWATCHER_EVENT_ALL)
        return;

    /* Extract filename from path for pattern matching */
    const char *name = strrchr(path, sw_path_separator());
    if (name) name++;
    else name = path;

    swatcher_target_internal *ti = SW_TARGET_INTERNAL(target);
    if (ti && ti->compiled_callback) {
        if (!sw_pattern_matched(ti->compiled_callback, name))
            return;
    }

    target->callback(event, target, path, NULL);
    target->last_event_time = time(NULL);
}

static void scan_path(swatcher_target_poll *tp, const char *dir_path, bool recursive)
{
    swatcher_target *target = tp->target;
    swatcher_target_internal *ti = SW_TARGET_INTERNAL(target);

    sw_dir *dir = sw_dir_open(dir_path);
    if (!dir) return;

    sw_dir_entry entry;
    char child_path[SW_PATH_MAX];
    char sep = sw_path_separator();

    while (sw_dir_next(dir, &entry)) {
        if (strcmp(entry.name, ".") == 0 || strcmp(entry.name, "..") == 0)
            continue;

        /* Check ignore patterns */
        if (ti && ti->compiled_ignore && sw_pattern_matched(ti->compiled_ignore, entry.name))
            continue;

        /* Build full path */
        size_t dlen = strlen(dir_path);
        if (dlen > 0 && dir_path[dlen - 1] == sep)
            snprintf(child_path, SW_PATH_MAX, "%s%s", dir_path, entry.name);
        else
            snprintf(child_path, SW_PATH_MAX, "%s%c%s", dir_path, sep, entry.name);

        if (entry.is_dir && recursive) {
            scan_path(tp, child_path, true);
            continue;
        }

        if (entry.is_dir)
            continue;

        /* Check watch patterns (files only) */
        if (ti && ti->compiled_watch && !sw_pattern_matched(ti->compiled_watch, entry.name))
            continue;

        /* Stat the file */
        sw_file_info info;
        if (!sw_stat(child_path, &info, target->follow_symlinks))
            continue;

        poll_file_snapshot *snap = snapshot_find(tp, child_path);
        if (!snap) {
            /* New file — mark as new, events emitted later by poll_scan_target */
            poll_file_snapshot *ns = snapshot_add(tp, child_path, info.mtime, info.size);
            if (ns) ns->is_new = true;
        } else {
            snap->seen = true;
            if (snap->mtime != info.mtime || snap->size != info.size) {
                snap->mtime = info.mtime;
                snap->size = info.size;
                emit_event(target, SWATCHER_EVENT_MODIFIED, child_path);
            }
        }
    }

    sw_dir_close(dir);
}

static void poll_scan_target(swatcher_target_poll *tp)
{
    swatcher_target *target = tp->target;

    /* Clear seen/is_new flags */
    poll_file_snapshot *snap, *tmp;
    HASH_ITER(hh, tp->snapshots, snap, tmp) {
        snap->seen = false;
        snap->is_new = false;
    }

    /* Handle single-file target (no move detection) */
    if (target->is_file) {
        sw_file_info info;
        if (sw_stat(target->path, &info, target->follow_symlinks)) {
            poll_file_snapshot *s = snapshot_find(tp, target->path);
            if (!s) {
                snapshot_add(tp, target->path, info.mtime, info.size);
                emit_event(target, SWATCHER_EVENT_CREATED, target->path);
            } else {
                s->seen = true;
                if (s->mtime != info.mtime || s->size != info.size) {
                    s->mtime = info.mtime;
                    s->size = info.size;
                    emit_event(target, SWATCHER_EVENT_MODIFIED, target->path);
                }
            }
        } else {
            poll_file_snapshot *s = snapshot_find(tp, target->path);
            if (s) {
                emit_event(target, SWATCHER_EVENT_DELETED, target->path);
                HASH_DEL(tp->snapshots, s);
                snapshot_free(s);
            }
        }
        return;
    }

    /* Directory target — scan_path marks existing as seen, adds new with is_new=true */
    scan_path(tp, target->path, target->is_recursive);

    /* Collect deleted entries (unseen) into a temporary list */
    int num_deleted = 0;
    HASH_ITER(hh, tp->snapshots, snap, tmp) {
        if (!snap->seen && !snap->is_new)
            num_deleted++;
    }

    /* Move detection: match deleted entries with new entries by size.
     * For each deleted entry, find a new entry with the same size.
     * Matched pairs → MOVED event. Unmatched deleted → DELETED.
     * Unmatched new → CREATED. */
    if (num_deleted > 0) {
        HASH_ITER(hh, tp->snapshots, snap, tmp) {
            if (!snap->seen && !snap->is_new) {
                /* This entry was deleted — look for a new entry with same size */
                bool matched = false;
                poll_file_snapshot *candidate, *ctmp;
                HASH_ITER(hh, tp->snapshots, candidate, ctmp) {
                    if (candidate->is_new && candidate->size == snap->size) {
                        /* Match found — this is a move/rename */
                        emit_event(target, SWATCHER_EVENT_MOVED, candidate->path);
                        candidate->is_new = false; /* consumed */
                        matched = true;
                        break;
                    }
                }
                if (!matched) {
                    emit_event(target, SWATCHER_EVENT_DELETED, snap->path);
                }
                HASH_DEL(tp->snapshots, snap);
                snapshot_free(snap);
            }
        }
    }

    /* Emit CREATED for remaining unmatched new entries */
    HASH_ITER(hh, tp->snapshots, snap, tmp) {
        if (snap->is_new) {
            emit_event(target, SWATCHER_EVENT_CREATED, snap->path);
            snap->is_new = false;
        }
    }
}

/* ========== Initial snapshot (no events emitted) ========== */

static void initial_scan_path(swatcher_target_poll *tp, const char *dir_path, bool recursive)
{
    swatcher_target *target = tp->target;
    swatcher_target_internal *ti = SW_TARGET_INTERNAL(target);

    sw_dir *dir = sw_dir_open(dir_path);
    if (!dir) return;

    sw_dir_entry entry;
    char child_path[SW_PATH_MAX];
    char sep = sw_path_separator();

    while (sw_dir_next(dir, &entry)) {
        if (strcmp(entry.name, ".") == 0 || strcmp(entry.name, "..") == 0)
            continue;

        if (ti && ti->compiled_ignore && sw_pattern_matched(ti->compiled_ignore, entry.name))
            continue;

        size_t dlen = strlen(dir_path);
        if (dlen > 0 && dir_path[dlen - 1] == sep)
            snprintf(child_path, SW_PATH_MAX, "%s%s", dir_path, entry.name);
        else
            snprintf(child_path, SW_PATH_MAX, "%s%c%s", dir_path, sep, entry.name);

        if (entry.is_dir && recursive) {
            initial_scan_path(tp, child_path, true);
            continue;
        }

        if (entry.is_dir)
            continue;

        if (ti && ti->compiled_watch && !sw_pattern_matched(ti->compiled_watch, entry.name))
            continue;

        sw_file_info info;
        if (!sw_stat(child_path, &info, target->follow_symlinks))
            continue;

        snapshot_add(tp, child_path, info.mtime, info.size);
    }

    sw_dir_close(dir);
}

static void poll_initial_snapshot(swatcher_target_poll *tp)
{
    swatcher_target *target = tp->target;

    if (target->is_file) {
        sw_file_info info;
        if (sw_stat(target->path, &info, target->follow_symlinks))
            snapshot_add(tp, target->path, info.mtime, info.size);
        return;
    }

    initial_scan_path(tp, target->path, target->is_recursive);
}

/* ========== Vtable functions ========== */

static bool poll_init(swatcher *sw)
{
    swatcher_poll *p = malloc(sizeof(swatcher_poll));
    if (!p) {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to allocate swatcher_poll");
        return false;
    }
    p->targets = NULL;
    SW_INTERNAL(sw)->backend_data = p;
    return true;
}

static void poll_destroy(swatcher *sw)
{
    swatcher_poll *p = POLL_DATA(sw);
    if (!p) return;

    swatcher_target_poll *current, *tmp;
    HASH_ITER(hh, p->targets, current, tmp) {
        snapshots_clear(current);
        HASH_DEL(p->targets, current);
        free(current);
    }

    free(p);
    SW_INTERNAL(sw)->backend_data = NULL;
}

static bool poll_add_target(swatcher *sw, swatcher_target *target)
{
    swatcher_poll *p = POLL_DATA(sw);

    if (sw_find_target_internal(sw, target->path)) {
        SWATCHER_LOG_DEFAULT_WARNING("Path already watched: %s", target->path);
        return false;
    }

    swatcher_target_poll *tp = malloc(sizeof(swatcher_target_poll));
    if (!tp) {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to allocate swatcher_target_poll");
        return false;
    }
    tp->target = target;
    tp->snapshots = NULL;

    /* Take initial snapshot (no events emitted) */
    poll_initial_snapshot(tp);

    HASH_ADD_PTR(p->targets, target, tp);

    /* Create target internal and add to global hash */
    swatcher_target_internal *ti = SW_TARGET_INTERNAL(target);
    if (!ti) {
        ti = sw_target_internal_create(target);
        if (!ti) {
            SWATCHER_LOG_DEFAULT_ERROR("Failed to create target internal");
            snapshots_clear(tp);
            HASH_DEL(p->targets, tp);
            free(tp);
            return false;
        }
    }
    ti->backend_data = tp;
    sw_add_target_internal(sw, ti);

    SWATCHER_LOG_DEFAULT_INFO("Poll backend watching: %s", target->path);
    return true;
}

static bool poll_remove_target(swatcher *sw, swatcher_target *target)
{
    swatcher_poll *p = POLL_DATA(sw);
    swatcher_target_internal *ti = SW_TARGET_INTERNAL(target);
    if (!ti) return false;

    swatcher_target_poll *tp = (swatcher_target_poll *)ti->backend_data;
    if (tp) {
        snapshots_clear(tp);
        HASH_DEL(p->targets, tp);
        free(tp);
        ti->backend_data = NULL;
    }

    return true;
}

static bool poll_add_target_recursive(swatcher *sw, swatcher_target *target, bool dont_add_self)
{
    (void)dont_add_self;
    /* The poll backend handles recursion in scan_path, so just add the root target */
    return poll_add_target(sw, target);
}

static void *poll_thread_func(void *arg)
{
    swatcher *sw = (swatcher *)arg;
    swatcher_poll *p = POLL_DATA(sw);
    int interval_ms = sw->config->poll_interval_ms;
    if (interval_ms <= 0) interval_ms = 500;

    while (sw_atomic_load(&sw->running)) {
        /* Sleep in small increments so we can respond to stop quickly */
        int remaining = interval_ms;
        while (remaining > 0 && sw_atomic_load(&sw->running)) {
            int chunk = remaining > 100 ? 100 : remaining;
            sw_sleep_ms(chunk);
            remaining -= chunk;
        }

        if (!sw_atomic_load(&sw->running))
            break;

        swatcher_internal *si = SW_INTERNAL(sw);
        sw_mutex_lock(si->mutex);

        swatcher_target_poll *tp, *tmp;
        HASH_ITER(hh, p->targets, tp, tmp) {
            poll_scan_target(tp);
        }

        sw_mutex_unlock(si->mutex);
    }

    SWATCHER_LOG_INFO(sw, "Poll watcher thread exiting...");
    return NULL;
}

/* ========== Backend definition ========== */

static const swatcher_backend poll_backend = {
    .name = "poll",
    .init = poll_init,
    .destroy = poll_destroy,
    .add_target = poll_add_target,
    .remove_target = poll_remove_target,
    .add_target_recursive = poll_add_target_recursive,
    .thread_func = poll_thread_func,
};

const swatcher_backend *swatcher_backend_poll(void)
{
    return &poll_backend;
}
