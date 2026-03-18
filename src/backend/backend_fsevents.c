#if defined(__APPLE__)

#include "swatcher.h"
#include "../internal/internal.h"
#include "../core/pattern.h"
#include "../core/vcs.h"

#include <CoreServices/CoreServices.h>
#include <errno.h>

/* ========== Local types ========== */

typedef struct fsevents_target {
    char path[SW_PATH_MAX];         /* hash key — original target path */
    char watch_path[SW_PATH_MAX];   /* directory path for FSEvents stream */
    size_t path_len;
    bool is_file_target;
    swatcher_target *target;
    UT_hash_handle hh;
} fsevents_target;

typedef struct coalesce_entry {
    char path[SW_PATH_MAX];
    swatcher_fs_event event;
    swatcher_target *target;
    uint64_t timestamp_ms;
    bool is_dir;
    UT_hash_handle hh;
} coalesce_entry;

typedef struct swatcher_fsevents {
    FSEventStreamRef stream;
    CFRunLoopRef run_loop;
    fsevents_target *targets;       /* uthash by path */
    int target_count;
    coalesce_entry *pending_events;
} swatcher_fsevents;

#define FSEVENTS_DATA(sw) ((swatcher_fsevents *)SW_INTERNAL(sw)->backend_data)

/* ========== Target matching ========== */

static fsevents_target *find_target_for_path(swatcher_fsevents *fse, const char *path)
{
    /* Exact match first (for file targets) */
    fsevents_target *ft = NULL;
    HASH_FIND_STR(fse->targets, path, ft);
    if (ft) return ft;

    /* Prefix match (for directory targets) — longest prefix wins */
    fsevents_target *best = NULL;
    size_t best_len = 0;
    fsevents_target *iter, *tmp;
    HASH_ITER(hh, fse->targets, iter, tmp) {
        if (strncmp(path, iter->path, iter->path_len) == 0 &&
            (path[iter->path_len] == '/' || path[iter->path_len] == '\0')) {
            if (iter->path_len > best_len) {
                best = iter;
                best_len = iter->path_len;
            }
        }
    }
    return best;
}

/* ========== Event coalescing (ported from inotify/kqueue) ========== */

static void fsevents_coalesce_dispatch(swatcher *sw, coalesce_entry *ce)
{
    if (ce->target && ce->target->callback) {
        if (sw_vcs_should_pause(sw->config, ce->target->path))
            return;
        const char *name = strrchr(ce->path, '/');
        name = name ? name + 1 : ce->path;
        swatcher_event_info info = {
            .old_path = NULL,
            .is_dir = ce->is_dir,
        };
        ce->target->callback(ce->event, ce->target, name, &info);
        ce->target->last_event_time = time(NULL);
    }
}

static void fsevents_coalesce_flush(swatcher *sw, uint64_t now_ms, int coalesce_ms, bool flush_all)
{
    swatcher_fsevents *fse = FSEVENTS_DATA(sw);
    coalesce_entry *current, *tmp;

    HASH_ITER(hh, fse->pending_events, current, tmp) {
        if (flush_all || (now_ms - current->timestamp_ms >= (uint64_t)coalesce_ms)) {
            fsevents_coalesce_dispatch(sw, current);
            HASH_DEL(fse->pending_events, current);
            sw_free(current);
        }
    }
}

static void fsevents_coalesce_add(swatcher *sw, const char *full_path,
                                   swatcher_fs_event event, swatcher_target *target,
                                   bool is_dir)
{
    swatcher_fsevents *fse = FSEVENTS_DATA(sw);
    coalesce_entry *existing = NULL;

    HASH_FIND_STR(fse->pending_events, full_path, existing);

    if (existing) {
        if (existing->event == SWATCHER_EVENT_CREATED && event == SWATCHER_EVENT_DELETED) {
            HASH_DEL(fse->pending_events, existing);
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
    } else {
        coalesce_entry *ce = sw_malloc(sizeof(coalesce_entry));
        if (!ce) return;
        strncpy(ce->path, full_path, SW_PATH_MAX - 1);
        ce->path[SW_PATH_MAX - 1] = '\0';
        ce->event = event;
        ce->target = target;
        ce->timestamp_ms = sw_time_now_ms();
        ce->is_dir = is_dir;
        HASH_ADD_STR(fse->pending_events, path, ce);
    }
}

/* ========== Event dispatch ========== */

static void fsevents_dispatch_event(swatcher *sw, swatcher_target *target,
                                     swatcher_fs_event sw_event, const char *name,
                                     const char *full_path, bool is_dir)
{
    int coalesce_ms = sw->config->coalesce_ms;

    if (coalesce_ms > 0 && full_path) {
        fsevents_coalesce_add(sw, full_path, sw_event, target, is_dir);
    } else {
        if (target->callback) {
            if (sw_vcs_should_pause(sw->config, target->path))
                return;
            swatcher_event_info info = { .old_path = NULL, .is_dir = is_dir };
            target->callback(sw_event, target, name, &info);
            target->last_event_time = time(NULL);
        }
    }
}

/* ========== FSEvents stream callback ========== */

static void fsevents_stream_callback(
    ConstFSEventStreamRef streamRef,
    void *info,
    size_t numEvents,
    void *eventPaths,
    const FSEventStreamEventFlags eventFlags[],
    const FSEventStreamEventId eventIds[])
{
    (void)streamRef;
    (void)eventIds;

    swatcher *sw = (swatcher *)info;
    swatcher_fsevents *fse = FSEVENTS_DATA(sw);
    swatcher_internal *si = SW_INTERNAL(sw);
    char **paths = (char **)eventPaths;

    sw_mutex_lock(si->mutex);

    for (size_t i = 0; i < numEvents; i++) {
        const char *path = paths[i];
        FSEventStreamEventFlags flags = eventFlags[i];

        /* Handle must-rescan — emit overflow */
        if (flags & kFSEventStreamEventFlagMustScanSubDirs) {
            fsevents_target *ft, *tmp;
            HASH_ITER(hh, fse->targets, ft, tmp) {
                if (ft->target->callback) {
                    ft->target->callback(SWATCHER_EVENT_OVERFLOW, ft->target, NULL, NULL);
                }
            }
            continue;
        }

        /* Skip root/mount/unmount/history-done events */
        if (flags & (kFSEventStreamEventFlagRootChanged |
                     kFSEventStreamEventFlagMount |
                     kFSEventStreamEventFlagUnmount |
                     kFSEventStreamEventFlagHistoryDone))
            continue;

        /* Find matching target */
        fsevents_target *ft = find_target_for_path(fse, path);
        if (!ft) continue;

        swatcher_target *target = ft->target;

        /* For file targets, only emit events for the exact file */
        if (ft->is_file_target && strcmp(path, ft->path) != 0)
            continue;

        /* Extract filename */
        const char *name = strrchr(path, '/');
        name = name ? name + 1 : path;

        /* Skip if event path is the watched directory itself (dir metadata change) */
        if (!ft->is_file_target && strcmp(path, ft->path) == 0)
            continue;

        /* Determine if item is a directory (used for filtering and event info) */
        bool is_dir_event = (flags & kFSEventStreamEventFlagItemIsDir) != 0;

        /* Watch option filtering */
        if (target->watch_options != SWATCHER_WATCH_ALL) {
            bool is_file_event = (flags & kFSEventStreamEventFlagItemIsFile) != 0;
            bool is_symlink_event = (flags & kFSEventStreamEventFlagItemIsSymlink) != 0;

            if ((target->watch_options == SWATCHER_WATCH_FILES) && !is_file_event)
                continue;
            if ((target->watch_options == SWATCHER_WATCH_DIRECTORIES) && !is_dir_event)
                continue;
            if ((target->watch_options == SWATCHER_WATCH_SYMLINKS) && !is_symlink_event)
                continue;
        }

        /* Pattern filtering (applied once for all event types from this entry) */
        swatcher_target_internal *ti = SW_TARGET_INTERNAL(target);
        if (ti && ti->compiled_callback) {
            if (!sw_pattern_matched(ti->compiled_callback, name))
                continue;
        }

        /* Map FSEvents flags to swatcher events.
         * FSEvents can set multiple flags at once (e.g., Created|Modified for
         * a file overwrite via fopen("w")), so we emit each applicable event. */
        struct { FSEventStreamEventFlags flag; swatcher_fs_event event; } map[] = {
            { kFSEventStreamEventFlagItemRemoved,  SWATCHER_EVENT_DELETED },
            { kFSEventStreamEventFlagItemCreated,  SWATCHER_EVENT_CREATED },
            { kFSEventStreamEventFlagItemRenamed,  SWATCHER_EVENT_MOVED },
            { kFSEventStreamEventFlagItemModified, SWATCHER_EVENT_MODIFIED },
            { kFSEventStreamEventFlagItemInodeMetaMod,  SWATCHER_EVENT_ATTRIB_CHANGE },
            { kFSEventStreamEventFlagItemChangeOwner,   SWATCHER_EVENT_ATTRIB_CHANGE },
            { kFSEventStreamEventFlagItemXattrMod,      SWATCHER_EVENT_ATTRIB_CHANGE },
            { kFSEventStreamEventFlagItemFinderInfoMod, SWATCHER_EVENT_ATTRIB_CHANGE },
        };

        bool any_dispatched = false;
        swatcher_fs_event already_dispatched = SWATCHER_EVENT_NONE;

        for (size_t m = 0; m < sizeof(map) / sizeof(map[0]); m++) {
            if (!(flags & map[m].flag))
                continue;

            swatcher_fs_event sw_event = map[m].event;

            /* Skip duplicate event types (e.g., multiple ATTRIB_CHANGE sources) */
            if (already_dispatched & sw_event)
                continue;

            /* Check event mask */
            if (!(target->events & sw_event) && target->events != SWATCHER_EVENT_ALL)
                continue;

            fsevents_dispatch_event(sw, target, sw_event, name, path, is_dir_event);
            already_dispatched |= sw_event;
            any_dispatched = true;
        }

        (void)any_dispatched;
    }

    sw_mutex_unlock(si->mutex);
}

/* ========== Vtable functions ========== */

static bool fsevents_init(swatcher *sw)
{
    swatcher_fsevents *fse = sw_malloc(sizeof(swatcher_fsevents));
    if (!fse) {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to allocate swatcher_fsevents");
        return false;
    }

    fse->stream = NULL;
    fse->run_loop = NULL;
    fse->targets = NULL;
    fse->target_count = 0;
    fse->pending_events = NULL;

    SW_INTERNAL(sw)->backend_data = fse;
    return true;
}

static void fsevents_destroy(swatcher *sw)
{
    swatcher_fsevents *fse = FSEVENTS_DATA(sw);
    if (!fse) return;

    /* Flush and free pending coalesce events */
    coalesce_entry *ce, *ce_tmp;
    HASH_ITER(hh, fse->pending_events, ce, ce_tmp) {
        HASH_DEL(fse->pending_events, ce);
        sw_free(ce);
    }

    /* Free target mappings */
    fsevents_target *ft, *ft_tmp;
    HASH_ITER(hh, fse->targets, ft, ft_tmp) {
        HASH_DEL(fse->targets, ft);
        sw_free(ft);
    }

    sw_free(fse);
    SW_INTERNAL(sw)->backend_data = NULL;
}

static bool fsevents_add_single(swatcher *sw, swatcher_target *target)
{
    swatcher_fsevents *fse = FSEVENTS_DATA(sw);

    if (sw_find_target_internal(sw, target->path)) {
        SWATCHER_LOG_DEFAULT_WARNING("Path already watched: %s", target->path);
        return false;
    }

    fsevents_target *ft = sw_malloc(sizeof(fsevents_target));
    if (!ft) {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to allocate fsevents_target");
        return false;
    }

    strncpy(ft->path, target->path, SW_PATH_MAX - 1);
    ft->path[SW_PATH_MAX - 1] = '\0';
    ft->path_len = strlen(ft->path);
    ft->is_file_target = target->is_file;
    ft->target = target;

    /* FSEvents watches directories — for file targets, watch parent dir */
    if (target->is_file) {
        strncpy(ft->watch_path, target->path, SW_PATH_MAX - 1);
        ft->watch_path[SW_PATH_MAX - 1] = '\0';
        char *last_slash = strrchr(ft->watch_path, '/');
        if (last_slash) *last_slash = '\0';
    } else {
        strncpy(ft->watch_path, target->path, SW_PATH_MAX - 1);
        ft->watch_path[SW_PATH_MAX - 1] = '\0';
    }

    HASH_ADD_STR(fse->targets, path, ft);
    fse->target_count++;

    /* Create target internal and add to core hash */
    swatcher_target_internal *ti = SW_TARGET_INTERNAL(target);
    if (!ti) {
        ti = sw_target_internal_create(target);
        if (!ti) {
            SWATCHER_LOG_DEFAULT_ERROR("Failed to create target internal");
            HASH_DEL(fse->targets, ft);
            fse->target_count--;
            sw_free(ft);
            return false;
        }
    }
    ti->backend_data = ft;
    sw_add_target_internal(sw, ti);

    SWATCHER_LOG_DEFAULT_INFO("FSEvents target %s registered (watch_path=%s)", target->path, ft->watch_path);
    return true;
}

static bool fsevents_add_target(swatcher *sw, swatcher_target *target)
{
    return fsevents_add_single(sw, target);
}

static bool fsevents_remove_target(swatcher *sw, swatcher_target *target)
{
    swatcher_fsevents *fse = FSEVENTS_DATA(sw);
    swatcher_target_internal *ti = SW_TARGET_INTERNAL(target);
    if (!ti) return false;

    fsevents_target *ft = (fsevents_target *)ti->backend_data;
    if (ft) {
        HASH_DEL(fse->targets, ft);
        fse->target_count--;
        sw_free(ft);
        ti->backend_data = NULL;
    }

    return true;
}

static bool fsevents_add_target_recursive(swatcher *sw, swatcher_target *target, bool dont_add_self)
{
    /* FSEvents handles recursion natively — just add the root target */
    (void)dont_add_self;
    return fsevents_add_single(sw, target);
}

static void *fsevents_thread_func(void *arg)
{
    swatcher *sw = (swatcher *)arg;
    swatcher_fsevents *fse = FSEVENTS_DATA(sw);
    swatcher_internal *si = SW_INTERNAL(sw);
    int coalesce_ms = sw->config->coalesce_ms;

    /* Build unique watch paths array */
    sw_mutex_lock(si->mutex);

    CFMutableArrayRef paths = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    if (!paths) {
        sw_mutex_unlock(si->mutex);
        SWATCHER_LOG_DEFAULT_ERROR("Failed to create CFMutableArray");
        return NULL;
    }

    /* Collect unique watch_paths */
    fsevents_target *ft, *tmp;
    HASH_ITER(hh, fse->targets, ft, tmp) {
        CFStringRef cf_path = CFStringCreateWithCString(NULL, ft->watch_path, kCFStringEncodingUTF8);
        if (cf_path) {
            /* Avoid duplicates */
            if (!CFArrayContainsValue(paths, CFRangeMake(0, CFArrayGetCount(paths)), cf_path)) {
                CFArrayAppendValue(paths, cf_path);
            }
            CFRelease(cf_path);
        }
    }

    sw_mutex_unlock(si->mutex);

    if (CFArrayGetCount(paths) == 0) {
        CFRelease(paths);
        SWATCHER_LOG_DEFAULT_WARNING("No paths to watch");
        return NULL;
    }

    /* Create FSEventStream */
    FSEventStreamContext context = {
        .version = 0,
        .info = sw,
        .retain = NULL,
        .release = NULL,
        .copyDescription = NULL
    };

    CFTimeInterval latency = sw->config->poll_interval_ms / 1000.0;
    if (latency < 0.05) latency = 0.05;

    FSEventStreamRef stream = FSEventStreamCreate(
        NULL,
        fsevents_stream_callback,
        &context,
        paths,
        kFSEventStreamEventIdSinceNow,
        latency,
        kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer
    );

    CFRelease(paths);

    if (!stream) {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to create FSEventStream");
        return NULL;
    }

    fse->stream = stream;
    fse->run_loop = CFRunLoopGetCurrent();

    /* FSEventStreamScheduleWithRunLoop is deprecated in macOS 13 in favor of
     * FSEventStreamSetDispatchQueue, but the RunLoop API is simpler for our
     * thread model and still functional. Suppress the warning. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    FSEventStreamScheduleWithRunLoop(stream, fse->run_loop, kCFRunLoopDefaultMode);
#pragma clang diagnostic pop
    FSEventStreamStart(stream);

    SWATCHER_LOG_DEFAULT_INFO("FSEvents stream started");

    /* Run loop with timeout, checking running flag */
    while (sw_atomic_load(&sw->running)) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, true);

        /* Flush expired coalesce entries */
        if (coalesce_ms > 0) {
            sw_mutex_lock(si->mutex);
            fsevents_coalesce_flush(sw, sw_time_now_ms(), coalesce_ms, false);
            sw_mutex_unlock(si->mutex);
        }
    }

    /* Cleanup stream */
    FSEventStreamStop(stream);
    FSEventStreamInvalidate(stream);
    FSEventStreamRelease(stream);
    fse->stream = NULL;
    fse->run_loop = NULL;

    /* Flush remaining coalesced events on shutdown */
    if (coalesce_ms > 0) {
        sw_mutex_lock(si->mutex);
        fsevents_coalesce_flush(sw, 0, 0, true);
        sw_mutex_unlock(si->mutex);
    }

    SWATCHER_LOG_DEFAULT_INFO("FSEvents watcher thread exiting...");
    return NULL;
}

/* ========== Backend definition ========== */

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

#endif /* __APPLE__ */
