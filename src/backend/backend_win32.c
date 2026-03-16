#if defined(_WIN32) || defined(_WIN64)

#include "swatcher.h"
#include "../internal/internal.h"
#include "../core/pattern.h"

#include <windows.h>

#define BUFFER_SIZE (1024 * 1024)

/* ========== Local types ========== */

typedef struct swatcher_target_win32 {
    HANDLE watchHandle;
    OVERLAPPED overlapped;
    char buffer[BUFFER_SIZE];
    DWORD bytesReturned;
    swatcher_target *target;
    UT_hash_handle hh;
} swatcher_target_win32;

typedef struct swatcher_win32 {
    HANDLE *events;
    int num_events;
    HANDLE terminationEvent;
    swatcher_target_win32 *handles_to_targets;
} swatcher_win32;

#define WIN32_DATA(sw) ((swatcher_win32 *)SW_INTERNAL(sw)->backend_data)

/* ========== Helpers ========== */

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

static void process_directory_changes(const char *buffer, DWORD bytesReturned, swatcher_target_win32 *sw_target_win)
{
    (void)bytesReturned;
    char *p = (char *)buffer;
    FILE_NOTIFY_INFORMATION *fni = NULL;
    WCHAR filename[MAX_PATH];
    char fullpath[MAX_PATH * 2];

    do {
        fni = (FILE_NOTIFY_INFORMATION *)p;

        wcsncpy_s(filename, MAX_PATH, fni->FileName, fni->FileNameLength / sizeof(WCHAR));
        filename[fni->FileNameLength / sizeof(WCHAR)] = L'\0';

        int bufSize = WideCharToMultiByte(CP_UTF8, 0, filename, -1, NULL, 0, NULL, NULL);
        char *filenameUTF8 = (char *)malloc(bufSize);
        WideCharToMultiByte(CP_UTF8, 0, filename, -1, filenameUTF8, bufSize, NULL, NULL);
        snprintf(fullpath, sizeof(fullpath), "%s\\%s", sw_target_win->target->path, filenameUTF8);

        swatcher_fs_event event = action_to_swatcher_event(fni->Action);
        if (event != SWATCHER_EVENT_NONE && sw_target_win->target->callback) {
            swatcher_target_internal *ti = SW_TARGET_INTERNAL(sw_target_win->target);
            if (ti && ti->compiled_callback) {
                if (sw_pattern_matched(ti->compiled_callback, filenameUTF8)) {
                    sw_target_win->target->callback(event, sw_target_win->target, fullpath, fni);
                    sw_target_win->target->last_event_time = time(NULL);
                }
            } else {
                sw_target_win->target->callback(event, sw_target_win->target, fullpath, fni);
                sw_target_win->target->last_event_time = time(NULL);
            }
        }
        free(filenameUTF8);
        p += fni->NextEntryOffset;
    } while (fni->NextEntryOffset != 0);
}

/* ========== Vtable functions ========== */

static bool win32_init(swatcher *sw)
{
    swatcher_win32 *w = malloc(sizeof(swatcher_win32));
    if (!w) {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to allocate swatcher_win32");
        return false;
    }

    w->events = NULL;
    w->num_events = 0;
    w->handles_to_targets = NULL;

    w->terminationEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!w->terminationEvent) {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to create termination event");
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

    swatcher_target_win32 *current, *tmp;
    HASH_ITER(hh, w->handles_to_targets, current, tmp) {
        CloseHandle(current->watchHandle);
        CloseHandle(current->overlapped.hEvent);
        HASH_DEL(w->handles_to_targets, current);
        free(current);
    }

    if (w->terminationEvent) CloseHandle(w->terminationEvent);
    free(w->events);
    free(w);
    SW_INTERNAL(sw)->backend_data = NULL;
}

static bool win32_add_target(swatcher *sw, swatcher_target *target)
{
    swatcher_win32 *w = WIN32_DATA(sw);

    if (sw_find_target_internal(sw, target->path)) {
        SWATCHER_LOG_DEFAULT_WARNING("Path already watched: %s", target->path);
        return false;
    }

    swatcher_target_win32 *tw = malloc(sizeof(swatcher_target_win32));
    if (!tw) {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to allocate swatcher_target_win32");
        return false;
    }
    tw->target = target;

    HANDLE dir_handle = CreateFileA(target->path,
                                     FILE_LIST_DIRECTORY,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                     NULL, OPEN_EXISTING,
                                     FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
                                     NULL);
    if (dir_handle == INVALID_HANDLE_VALUE) {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to open directory: %s", target->path);
        free(tw);
        return false;
    }
    tw->watchHandle = dir_handle;

    tw->overlapped.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!tw->overlapped.hEvent) {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to create event for overlapped I/O");
        CloseHandle(dir_handle);
        free(tw);
        return false;
    }

    w->events = realloc(w->events, (w->num_events + 1) * sizeof(HANDLE));
    if (!w->events) {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to allocate memory for events");
        CloseHandle(dir_handle);
        CloseHandle(tw->overlapped.hEvent);
        free(tw);
        return false;
    }
    w->events[w->num_events] = tw->overlapped.hEvent;
    w->num_events++;

    if (!ReadDirectoryChangesW(dir_handle, tw->buffer, BUFFER_SIZE,
                                target->is_recursive,
                                events_to_notify_filter(target),
                                &tw->bytesReturned, &tw->overlapped, NULL)) {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to start watching directory: %s", target->path);
        CloseHandle(dir_handle);
        CloseHandle(tw->overlapped.hEvent);
        w->num_events--;
        free(tw);
        return false;
    }

    HASH_ADD_PTR(w->handles_to_targets, overlapped.hEvent, tw);

    /* Create target internal and add to hash */
    swatcher_target_internal *ti = SW_TARGET_INTERNAL(target);
    if (!ti) {
        ti = sw_target_internal_create(target);
        if (!ti) {
            SWATCHER_LOG_DEFAULT_ERROR("Failed to create target internal");
            /* Cleanup would be needed here but keep it simple */
            return false;
        }
    }
    ti->backend_data = tw;
    sw_add_target_internal(sw, ti);

    return true;
}

static bool win32_remove_target(swatcher *sw, swatcher_target *target)
{
    swatcher_win32 *w = WIN32_DATA(sw);
    swatcher_target_internal *ti = SW_TARGET_INTERNAL(target);
    if (!ti) return false;

    swatcher_target_win32 *tw = (swatcher_target_win32 *)ti->backend_data;
    if (tw) {
        /* Remove event from array */
        for (int i = 0; i < w->num_events; i++) {
            if (w->events[i] == tw->overlapped.hEvent) {
                for (int j = i; j < w->num_events - 1; j++)
                    w->events[j] = w->events[j + 1];
                w->num_events--;
                HANDLE *newHandles = realloc(w->events, w->num_events * sizeof(HANDLE));
                if (newHandles || w->num_events == 0)
                    w->events = newHandles;
                break;
            }
        }

        CloseHandle(tw->watchHandle);
        CloseHandle(tw->overlapped.hEvent);
        HASH_DEL(w->handles_to_targets, tw);
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

    HANDLE *tempHandles = malloc((w->num_events + 1) * sizeof(HANDLE));
    if (!tempHandles) {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to allocate memory for handles");
        return NULL;
    }

    while (sw->running) {
        memcpy(tempHandles, w->events, w->num_events * sizeof(HANDLE));
        tempHandles[w->num_events] = w->terminationEvent;

        DWORD waitStatus = WaitForMultipleObjects(w->num_events + 1, tempHandles, FALSE, INFINITE);

        if (waitStatus == WAIT_FAILED) {
            SWATCHER_LOG_DEFAULT_ERROR("WaitForMultipleObjects failed");
            break;
        }

        if (waitStatus == WAIT_OBJECT_0 + w->num_events)
            break;

        if (waitStatus >= WAIT_OBJECT_0 && waitStatus < WAIT_OBJECT_0 + (DWORD)w->num_events) {
            swatcher_target_win32 *tw = NULL;
            HANDLE signaled = w->events[waitStatus - WAIT_OBJECT_0];
            HASH_FIND_PTR(w->handles_to_targets, &signaled, tw);

            if (!tw) {
                SWATCHER_LOG_DEFAULT_ERROR("Failed to find target for event");
                continue;
            }

            BOOL result = GetOverlappedResult(tw->watchHandle, &tw->overlapped, &tw->bytesReturned, FALSE);
            if (!result) {
                SWATCHER_LOG_DEFAULT_ERROR("GetOverlappedResult failed");
                continue;
            }

            process_directory_changes(tw->buffer, tw->bytesReturned, tw);

            if (!ReadDirectoryChangesW(tw->watchHandle, tw->buffer, BUFFER_SIZE,
                                        tw->target->is_recursive,
                                        events_to_notify_filter(tw->target),
                                        &tw->bytesReturned, &tw->overlapped, NULL)) {
                SWATCHER_LOG_DEFAULT_ERROR("Failed to continue watching directory: %s", tw->target->path);
            }
        }

        Sleep(10);
    }

    free(tempHandles);
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

const swatcher_backend *swatcher_backend_default(void)
{
    return swatcher_backend_win32();
}

#endif /* _WIN32 */
