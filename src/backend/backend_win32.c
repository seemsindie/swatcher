#if defined(_WIN32) || defined(_WIN64)

#include "swatcher.h"
#include "../internal/internal.h"
#include "../core/pattern.h"
#include "../core/error.h"

#include <windows.h>

#define DEFAULT_BUFFER_SIZE (64 * 1024)  /* 64KB per target */
#define UNC_BUFFER_SIZE     (128 * 1024) /* 128KB for network paths */

/* ========== Local types ========== */

typedef struct swatcher_target_win32 {
    HANDLE watchHandle;
    OVERLAPPED overlapped;
    char *buffer;           /* heap-allocated notification buffer */
    DWORD buffer_size;
    swatcher_target *target;
    UT_hash_handle hh;      /* keyed by watchHandle */
} swatcher_target_win32;

typedef struct coalesce_entry {
    char path[SW_PATH_MAX];
    swatcher_fs_event event;
    swatcher_target *target;
    uint64_t timestamp_ms;
    UT_hash_handle hh;
} coalesce_entry;

typedef struct swatcher_win32 {
    HANDLE iocp;                            /* I/O completion port */
    swatcher_target_win32 *handles_to_targets; /* uthash: watchHandle -> target */
    coalesce_entry *pending_events;         /* event coalescing hash */
} swatcher_win32;

#define WIN32_DATA(sw) ((swatcher_win32 *)SW_INTERNAL(sw)->backend_data)

/* Sentinel completion key for shutdown */
#define WIN32_SHUTDOWN_KEY ((ULONG_PTR)0xDEAD)

/* ========== Helpers ========== */

static bool is_unc_path(const char *path)
{
    return path && path[0] == '\\' && path[1] == '\\';
}

static DWORD events_to_notify_filter(swatcher_target *target)
{
    DWORD filter = 0;

    if (target->events & SWATCHER_EVENT_ALL) {
        return (FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SIZE |
                FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SECURITY);
    }

    if (target->events & SWATCHER_EVENT_CREATED)       filter |= FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME;
    if (target->events & SWATCHER_EVENT_MODIFIED)       filter |= FILE_NOTIFY_CHANGE_LAST_WRITE;
    if (target->events & SWATCHER_EVENT_DELETED)        filter |= FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME;
    if (target->events & SWATCHER_EVENT_MOVED)          filter |= FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME;
    if (target->events & SWATCHER_EVENT_ATTRIB_CHANGE)  filter |= FILE_NOTIFY_CHANGE_ATTRIBUTES;

    return filter;
}

static swatcher_fs_event action_to_swatcher_event(DWORD action)
{
    switch (action) {
    case FILE_ACTION_ADDED:            return SWATCHER_EVENT_CREATED;
    case FILE_ACTION_REMOVED:          return SWATCHER_EVENT_DELETED;
    case FILE_ACTION_MODIFIED:         return SWATCHER_EVENT_MODIFIED;
    case FILE_ACTION_RENAMED_OLD_NAME:
    case FILE_ACTION_RENAMED_NEW_NAME: return SWATCHER_EVENT_MOVED;
    default:                           return SWATCHER_EVENT_NONE;
    }
}

/* ========== UTF-8 <-> UTF-16 ========== */

static WCHAR *utf8_to_wide(const char *utf8)
{
    if (!utf8) return NULL;
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (len <= 0) return NULL;
    WCHAR *wide = malloc(len * sizeof(WCHAR));
    if (!wide) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, len);
    return wide;
}

static char *wide_to_utf8(const WCHAR *wide, int wchar_count)
{
    if (!wide) return NULL;
    int len = WideCharToMultiByte(CP_UTF8, 0, wide, wchar_count, NULL, 0, NULL, NULL);
    if (len <= 0) return NULL;
    char *utf8 = malloc(len);
    if (!utf8) return NULL;
    WideCharToMultiByte(CP_UTF8, 0, wide, wchar_count, utf8, len, NULL, NULL);
    return utf8;
}

/* ========== Event coalescing ========== */

static void win32_coalesce_dispatch(swatcher *sw, coalesce_entry *ce)
{
    (void)sw;
    if (ce->target && ce->target->callback) {
        const char *name = strrchr(ce->path, '\\');
        if (!name) name = strrchr(ce->path, '/');
        name = name ? name + 1 : ce->path;
        ce->target->callback(ce->event, ce->target, name, NULL);
        ce->target->last_event_time = time(NULL);
    }
}

static void win32_coalesce_flush(swatcher *sw, uint64_t now_ms, int coalesce_ms, bool flush_all)
{
    swatcher_win32 *w = WIN32_DATA(sw);
    coalesce_entry *current, *tmp;

    HASH_ITER(hh, w->pending_events, current, tmp) {
        if (flush_all || (now_ms - current->timestamp_ms >= (uint64_t)coalesce_ms)) {
            win32_coalesce_dispatch(sw, current);
            HASH_DEL(w->pending_events, current);
            free(current);
        }
    }
}

static void win32_coalesce_add(swatcher *sw, const char *full_path,
                                swatcher_fs_event event, swatcher_target *target)
{
    swatcher_win32 *w = WIN32_DATA(sw);
    coalesce_entry *existing = NULL;

    HASH_FIND_STR(w->pending_events, full_path, existing);

    if (existing) {
        if (existing->event == SWATCHER_EVENT_CREATED && event == SWATCHER_EVENT_DELETED) {
            HASH_DEL(w->pending_events, existing);
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
        HASH_ADD_STR(w->pending_events, path, ce);
    }
}

/* ========== Event dispatch ========== */

static void win32_dispatch_event(swatcher *sw, swatcher_target *target,
                                  swatcher_fs_event sw_event, const char *name,
                                  const char *full_path)
{
    int coalesce_ms = sw->config->coalesce_ms;

    if (coalesce_ms > 0 && full_path) {
        win32_coalesce_add(sw, full_path, sw_event, target);
    } else {
        if (target->callback) {
            target->callback(sw_event, target, name, NULL);
            target->last_event_time = time(NULL);
        }
    }
}

static void process_directory_changes(swatcher *sw, const char *buffer,
                                       DWORD bytesReturned, swatcher_target_win32 *tw)
{
    (void)bytesReturned;
    const char *p = buffer;
    FILE_NOTIFY_INFORMATION *fni = NULL;
    char fullpath[SW_PATH_MAX * 2];

    do {
        fni = (FILE_NOTIFY_INFORMATION *)p;

        /* Convert filename from UTF-16 to UTF-8 */
        char *filenameUTF8 = wide_to_utf8(fni->FileName,
                                            (int)(fni->FileNameLength / sizeof(WCHAR)));
        if (!filenameUTF8) {
            p += fni->NextEntryOffset;
            if (fni->NextEntryOffset == 0) break;
            continue;
        }

        snprintf(fullpath, sizeof(fullpath), "%s\\%s", tw->target->path, filenameUTF8);

        swatcher_fs_event event = action_to_swatcher_event(fni->Action);
        if (event != SWATCHER_EVENT_NONE && tw->target->callback) {
            /* Check event mask */
            if ((tw->target->events & event) || (tw->target->events == SWATCHER_EVENT_ALL)) {
                /* Extract just the filename for pattern matching */
                const char *name = strrchr(filenameUTF8, '\\');
                name = name ? name + 1 : filenameUTF8;

                swatcher_target_internal *ti = SW_TARGET_INTERNAL(tw->target);
                if (ti && ti->compiled_callback) {
                    if (sw_pattern_matched(ti->compiled_callback, name)) {
                        win32_dispatch_event(sw, tw->target, event, fullpath, fullpath);
                    }
                } else {
                    win32_dispatch_event(sw, tw->target, event, fullpath, fullpath);
                }
            }
        }

        free(filenameUTF8);
        p += fni->NextEntryOffset;
    } while (fni->NextEntryOffset != 0);
}

/* ========== Issue ReadDirectoryChangesW ========== */

static bool win32_issue_read(swatcher_target_win32 *tw)
{
    memset(&tw->overlapped, 0, sizeof(OVERLAPPED));
    return ReadDirectoryChangesW(tw->watchHandle, tw->buffer, tw->buffer_size,
                                  tw->target->is_recursive,
                                  events_to_notify_filter(tw->target),
                                  NULL, &tw->overlapped, NULL) != 0;
}

/* ========== Vtable functions ========== */

static bool win32_init(swatcher *sw)
{
    swatcher_win32 *w = malloc(sizeof(swatcher_win32));
    if (!w) {
        sw_set_error(SWATCHER_ERR_ALLOC);
        SWATCHER_LOG_DEFAULT_ERROR("Failed to allocate swatcher_win32");
        return false;
    }

    w->handles_to_targets = NULL;
    w->pending_events = NULL;

    /* Create I/O completion port (1 concurrent thread) */
    w->iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
    if (!w->iocp) {
        sw_set_error(SWATCHER_ERR_BACKEND_INIT);
        SWATCHER_LOG_DEFAULT_ERROR("Failed to create IOCP (error %lu)", GetLastError());
        free(w);
        return false;
    }

    SW_INTERNAL(sw)->backend_data = w;
    return true;
}

static void win32_destroy(swatcher *sw)
{
    swatcher_win32 *w = WIN32_DATA(sw);
    if (!w) return;

    /* Flush and free pending coalesce events */
    coalesce_entry *ce, *ce_tmp;
    HASH_ITER(hh, w->pending_events, ce, ce_tmp) {
        HASH_DEL(w->pending_events, ce);
        free(ce);
    }

    /* Close all watch handles and free targets */
    swatcher_target_win32 *current, *tmp;
    HASH_ITER(hh, w->handles_to_targets, current, tmp) {
        CancelIo(current->watchHandle);
        CloseHandle(current->watchHandle);
        HASH_DEL(w->handles_to_targets, current);
        free(current->buffer);
        free(current);
    }

    if (w->iocp) CloseHandle(w->iocp);
    free(w);
    SW_INTERNAL(sw)->backend_data = NULL;
}

static bool win32_add_target(swatcher *sw, swatcher_target *target)
{
    swatcher_win32 *w = WIN32_DATA(sw);

    if (sw_find_target_internal(sw, target->path)) {
        sw_set_error(SWATCHER_ERR_TARGET_EXISTS);
        SWATCHER_LOG_DEFAULT_WARNING("Path already watched: %s", target->path);
        return false;
    }

    swatcher_target_win32 *tw = malloc(sizeof(swatcher_target_win32));
    if (!tw) {
        sw_set_error(SWATCHER_ERR_ALLOC);
        SWATCHER_LOG_DEFAULT_ERROR("Failed to allocate swatcher_target_win32");
        return false;
    }
    tw->target = target;

    /* Allocate notification buffer (larger for UNC paths) */
    tw->buffer_size = is_unc_path(target->path) ? UNC_BUFFER_SIZE : DEFAULT_BUFFER_SIZE;
    tw->buffer = malloc(tw->buffer_size);
    if (!tw->buffer) {
        sw_set_error(SWATCHER_ERR_ALLOC);
        SWATCHER_LOG_DEFAULT_ERROR("Failed to allocate notification buffer");
        free(tw);
        return false;
    }

    if (is_unc_path(target->path)) {
        SWATCHER_LOG_DEFAULT_WARNING("Watching UNC path — notifications may be delayed: %s", target->path);
    }

    /* Open directory with overlapped I/O using Wide API for UTF-8 support */
    WCHAR *wide_path = utf8_to_wide(target->path);
    if (!wide_path) {
        sw_set_error(SWATCHER_ERR_ALLOC);
        SWATCHER_LOG_DEFAULT_ERROR("Failed to convert path to UTF-16: %s", target->path);
        free(tw->buffer);
        free(tw);
        return false;
    }

    HANDLE dir_handle = CreateFileW(wide_path,
                                     FILE_LIST_DIRECTORY,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                     NULL, OPEN_EXISTING,
                                     FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
                                     NULL);
    free(wide_path);

    if (dir_handle == INVALID_HANDLE_VALUE) {
        sw_set_error(SWATCHER_ERR_INVALID_PATH);
        SWATCHER_LOG_DEFAULT_ERROR("Failed to open directory: %s (error %lu)",
                                    target->path, GetLastError());
        free(tw->buffer);
        free(tw);
        return false;
    }
    tw->watchHandle = dir_handle;

    /* Associate directory handle with IOCP — completion key is the tw pointer */
    if (!CreateIoCompletionPort(dir_handle, w->iocp, (ULONG_PTR)tw, 0)) {
        sw_set_error(SWATCHER_ERR_BACKEND_INIT);
        SWATCHER_LOG_DEFAULT_ERROR("Failed to associate handle with IOCP (error %lu)", GetLastError());
        CloseHandle(dir_handle);
        free(tw->buffer);
        free(tw);
        return false;
    }

    /* Issue the first ReadDirectoryChangesW */
    if (!win32_issue_read(tw)) {
        sw_set_error(SWATCHER_ERR_BACKEND_INIT);
        SWATCHER_LOG_DEFAULT_ERROR("Failed to start watching directory: %s (error %lu)",
                                    target->path, GetLastError());
        CloseHandle(dir_handle);
        free(tw->buffer);
        free(tw);
        return false;
    }

    HASH_ADD_PTR(w->handles_to_targets, watchHandle, tw);

    /* Create target internal and add to hash */
    swatcher_target_internal *ti = SW_TARGET_INTERNAL(target);
    if (!ti) {
        ti = sw_target_internal_create(target);
        if (!ti) {
            sw_set_error(SWATCHER_ERR_ALLOC);
            SWATCHER_LOG_DEFAULT_ERROR("Failed to create target internal");
            CancelIo(dir_handle);
            CloseHandle(dir_handle);
            HASH_DEL(w->handles_to_targets, tw);
            free(tw->buffer);
            free(tw);
            return false;
        }
    }
    ti->backend_data = tw;
    sw_add_target_internal(sw, ti);

    SWATCHER_LOG_DEFAULT_INFO("Win32 watching: %s", target->path);
    return true;
}

static bool win32_remove_target(swatcher *sw, swatcher_target *target)
{
    swatcher_win32 *w = WIN32_DATA(sw);
    swatcher_target_internal *ti = SW_TARGET_INTERNAL(target);
    if (!ti) return false;

    swatcher_target_win32 *tw = (swatcher_target_win32 *)ti->backend_data;
    if (tw) {
        /* Cancel pending I/O and close handle */
        CancelIo(tw->watchHandle);
        CloseHandle(tw->watchHandle);
        HASH_DEL(w->handles_to_targets, tw);
        free(tw->buffer);
        free(tw);
        ti->backend_data = NULL;
    }

    return true;
}

/* Windows ReadDirectoryChangesW handles recursion natively */
static bool win32_add_target_recursive(swatcher *sw, swatcher_target *target, bool dont_add_self)
{
    (void)dont_add_self;
    return win32_add_target(sw, target);
}

static void *win32_thread_func(void *arg)
{
    swatcher *sw = (swatcher *)arg;
    swatcher_win32 *w = WIN32_DATA(sw);
    swatcher_internal *si = SW_INTERNAL(sw);
    int coalesce_ms = sw->config->coalesce_ms;

    while (sw_atomic_load(&sw->running)) {
        DWORD bytes_transferred = 0;
        ULONG_PTR completion_key = 0;
        OVERLAPPED *overlapped = NULL;

        /* 100ms timeout so we can check sw->running and flush coalesce */
        BOOL ok = GetQueuedCompletionStatus(w->iocp, &bytes_transferred,
                                             &completion_key, &overlapped, 100);

        /* Flush expired coalesce entries */
        if (coalesce_ms > 0) {
            sw_mutex_lock(si->mutex);
            win32_coalesce_flush(sw, sw_time_now_ms(), coalesce_ms, false);
            sw_mutex_unlock(si->mutex);
        }

        if (!ok) {
            if (overlapped == NULL) {
                /* Timeout — just loop to check sw->running */
                continue;
            }
            /* I/O error on a specific handle */
            DWORD err = GetLastError();
            if (err == ERROR_OPERATION_ABORTED) {
                /* Handle was closed / CancelIo — normal during removal */
                continue;
            }
            SWATCHER_LOG_DEFAULT_ERROR("GetQueuedCompletionStatus failed (error %lu)", err);
            continue;
        }

        /* Check for shutdown sentinel */
        if (completion_key == WIN32_SHUTDOWN_KEY)
            break;

        swatcher_target_win32 *tw = (swatcher_target_win32 *)completion_key;

        sw_mutex_lock(si->mutex);

        if (bytes_transferred == 0) {
            /* Buffer overflow — ReadDirectoryChangesW had more data than fit */
            SWATCHER_LOG_DEFAULT_WARNING("ReadDirectoryChangesW buffer overflow: %s",
                                          tw->target->path);
            if (tw->target->callback) {
                tw->target->callback(SWATCHER_EVENT_OVERFLOW, tw->target, NULL, NULL);
            }
        } else {
            process_directory_changes(sw, tw->buffer, bytes_transferred, tw);
        }

        sw_mutex_unlock(si->mutex);

        /* Re-issue ReadDirectoryChangesW */
        if (!win32_issue_read(tw)) {
            SWATCHER_LOG_DEFAULT_ERROR("Failed to re-issue ReadDirectoryChangesW: %s (error %lu)",
                                        tw->target->path, GetLastError());
        }
    }

    /* Flush remaining coalesced events on shutdown */
    if (coalesce_ms > 0) {
        sw_mutex_lock(si->mutex);
        win32_coalesce_flush(sw, 0, 0, true);
        sw_mutex_unlock(si->mutex);
    }

    SWATCHER_LOG_INFO(sw, "Win32 watcher thread exiting...");
    return NULL;
}

/* ========== Backend definition ========== */

static const swatcher_backend win32_backend = {
    .name = "win32",
    .init = win32_init,
    .destroy = win32_destroy,
    .add_target = win32_add_target,
    .remove_target = win32_remove_target,
    .add_target_recursive = win32_add_target_recursive,
    .thread_func = win32_thread_func,
};

const swatcher_backend *swatcher_backend_win32(void)
{
    return &win32_backend;
}

#endif /* _WIN32 */
