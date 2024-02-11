#define _CRTDBG_MAP_ALLOC

#include <windows.h>
#include <ctype.h>
#include <stdio.h>
#include <crtdbg.h>
// #define BUF_LEN 4096
#define BUFFER_SIZE (1024 * 1024)

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct swatcher_target_windows
    {
        HANDLE watchHandle;      // Handle for the specific watch target
        OVERLAPPED overlapped;   // Overlapped I/O structure for asynchronous operations
        char buffer[BUFFER_SIZE]; // Buffer for receiving change notifications
        DWORD bytesReturned;      // Number of bytes returned by ReadDirectoryChangesW
        swatcher_target *target; // Pointer to the target
        UT_hash_handle hh;       // internal use
    } swatcher_target_windows;

    typedef struct swatcher_windows
    {
        HANDLE threadHandle;                         // Handle for the watcher thread
        DWORD threadId;                              // Thread identifier
        CRITICAL_SECTION mutex;                      // Mutex for thread synchronization
        swatcher_target_windows *handles_to_targets; // internal use
        HANDLE *events;
        int num_events;
        HANDLE terminationEvent;
    } swatcher_windows;

    DWORD WINAPI swatcher_watcher_thread(LPVOID arg);

#ifdef __cplusplus
}
#endif

bool swatcher_is_absolute_path(const char *path)
{
    if (path == NULL)
    {
        return false;
    }
    return (isalpha(path[0]) && path[1] == ':' && path[2] == '\\') ||
           (path[0] == '\\' && path[1] == '\\'); // UNC path, e.g., \\server\share
}

bool swatcher_validate_and_normalize_path(const char *input_path, char *normalized_path, bool resolve_symlinks)
{
    if (!swatcher_is_absolute_path(input_path))
    {
        char cwd[MAX_PATH];
        if (GetCurrentDirectory(MAX_PATH, cwd) != 0)
        {
            snprintf(normalized_path, MAX_PATH, "%s\\%s", cwd, input_path);
            for (int i = 0; normalized_path[i] != '\0'; i++)
            {
                if (normalized_path[i] == '/')
                {
                    normalized_path[i] = '\\';
                }
            }
        }
        else
        {
            perror("GetCurrentDirectory error");
            return false;
        }
    }
    else
    {
        if (_fullpath(normalized_path, input_path, MAX_PATH) == NULL)
        {
            perror("Error resolving full path");
            return false;
        }
    }
    return true;
}

const char *get_event_name_from_action(DWORD action)
{
    switch (action)
    {
    case FILE_ACTION_ADDED:
        return "FILE_ACTION_ADDED";
    case FILE_ACTION_REMOVED:
        return "FILE_ACTION_REMOVED";
    case FILE_ACTION_MODIFIED:
        return "FILE_ACTION_MODIFIED";
    case FILE_ACTION_RENAMED_OLD_NAME:
        return "FILE_ACTION_RENAMED_OLD_NAME";
    case FILE_ACTION_RENAMED_NEW_NAME:
        return "FILE_ACTION_RENAMED_NEW_NAME";
    default:
        return "Unknown action";
    }
}

swatcher_fs_event convert_notify_action_to_swatcher_event(DWORD action)
{
    switch (action)
    {
    case FILE_ACTION_ADDED:
        return SWATCHER_EVENT_CREATED;
    case FILE_ACTION_REMOVED:
        return SWATCHER_EVENT_DELETED;
    case FILE_ACTION_MODIFIED:
        return SWATCHER_EVENT_MODIFIED;
    case FILE_ACTION_RENAMED_OLD_NAME:
    case FILE_ACTION_RENAMED_NEW_NAME:
        return SWATCHER_EVENT_MOVED;
    // There are no direct mappings for FILE_ACTION_* to SWATCHER_EVENT_OPENED,
    // SWATCHER_EVENT_CLOSED, or SWATCHER_EVENT_ACCESSED as those events don't exist in the same way on Windows.
    default:
        return SWATCHER_EVENT_NONE;
    }
}

DWORD swatcher_target_events_to_notify_filter(swatcher_target *target)
{
    DWORD notifyFilter = 0;

    // SWATCHER_EVENT_ALL is an example flag for all events.
    if (target->events & SWATCHER_EVENT_ALL)
    {
        return (FILE_NOTIFY_CHANGE_FILE_NAME |
                FILE_NOTIFY_CHANGE_DIR_NAME |
                FILE_NOTIFY_CHANGE_ATTRIBUTES |
                FILE_NOTIFY_CHANGE_SIZE |
                FILE_NOTIFY_CHANGE_LAST_WRITE |
                FILE_NOTIFY_CHANGE_SECURITY);
    }

    if (target->events & SWATCHER_EVENT_CREATED)
    {
        notifyFilter |= FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME;
    }

    if (target->events & SWATCHER_EVENT_MODIFIED)
    {
        notifyFilter |= FILE_NOTIFY_CHANGE_LAST_WRITE;
    }

    if (target->events & SWATCHER_EVENT_DELETED)
    {
        notifyFilter |= FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME;
    }

    if (target->events & SWATCHER_EVENT_MOVED)
    {
        // Windows does not distinguish moves from renames
        notifyFilter |= FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME;
    }

    // Windows does not directly support opened, closed, or accessed events
    // in ReadDirectoryChangesW notifications.
    // You might need to use other mechanisms or omit these events.

    if (target->events & SWATCHER_EVENT_ATTRIB_CHANGE)
    {
        notifyFilter |= FILE_NOTIFY_CHANGE_ATTRIBUTES;
    }

    return notifyFilter;
}

// bool swatcher_validate_and_normalize_path(const char *input_path, char *normalized_path, bool resolve_symlinks)
// {
//     DWORD dwAttr;
//     HANDLE hFile;
//     TCHAR targetPath[MAX_PATH] = {0};

//     if (!swatcher_is_absolute_path(input_path))
//     {
//         char cwd[MAX_PATH];
//         if (GetCurrentDirectory(MAX_PATH, cwd) != 0)
//         {
//             snprintf(normalized_path, MAX_PATH, "%s\\%s", cwd, input_path);
//             for (int i = 0; normalized_path[i] != '\0'; i++)
//             {
//                 if (normalized_path[i] == '/')
//                 {
//                     normalized_path[i] = '\\';
//                 }
//             }
//         }
//         else
//         {
//             perror("GetCurrentDirectory error");
//             return false;
//         }
//     }
//     else
//     {
//         strncpy(normalized_path, input_path, MAX_PATH);
//         normalized_path[MAX_PATH - 1] = '\0'; // Ensure null-termination
//     }

//     // Additional step to resolve symlinks, if necessary
//     if (resolve_symlinks)
//     {
//         dwAttr = GetFileAttributesA(normalized_path);
//         if (dwAttr != INVALID_FILE_ATTRIBUTES && (dwAttr & FILE_ATTRIBUTE_REPARSE_POINT))
//         {
//             hFile = CreateFile(normalized_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
//             if (hFile == INVALID_HANDLE_VALUE)
//             {
//                 perror("CreateFile error");
//                 return false;
//             }

//             // Get the final path name (resolving any symlinks)
//             if (GetFinalPathNameByHandle(hFile, targetPath, MAX_PATH, FILE_NAME_NORMALIZED) == 0)
//             {
//                 perror("GetFinalPathNameByHandle error");
//                 CloseHandle(hFile);
//                 return false;
//             }

//             // Convert to a standard path (strip possible "\\?\" prefix)
//             if (wcsncmp((const wchar_t *)targetPath, L"\\\\?\\", 4) == 0)
//             {
//                 wcscpy((wchar_t *)normalized_path, (const wchar_t *)targetPath + 4);
//             }
//             else
//             {
//                 wcscpy((wchar_t *)normalized_path, (const wchar_t *)targetPath);
//             }

//             CloseHandle(hFile);
//         }
//         else
//         {
//             // If not a symlink or failed to get attributes, just copy the input path
//             strncpy(normalized_path, input_path, MAX_PATH);
//             normalized_path[MAX_PATH - 1] = '\0'; // Ensure null-termination
//         }
//     }

//     // Replace backslashes with slashes if needed, or adjust normalization as necessary

//     return true;
// }

SWATCHER_API bool swatcher_init(swatcher *sw, swatcher_config *config)
{
    if (!sw)
    {
        SWATCHER_LOG_DEFAULT_ERROR("swatcher is NULL\n");
        return false;
    }

    if (!config)
    {
        SWATCHER_LOG_DEFAULT_ERROR("config is NULL\n");
        return false;
    }

    sw->platform_data = malloc(sizeof(swatcher_windows));
    if (sw->platform_data == NULL)
    {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to allocate swatcher_windows");
        return false;
    }

    swatcher_windows *sw_win = (swatcher_windows *)sw->platform_data;
    sw->running = false;
    sw_win->threadId = -1; // Initialize to zero
    sw_win->threadHandle = NULL;
    sw_win->events = NULL;
    sw_win->num_events = 0;

    sw_win->terminationEvent = CreateEvent(NULL, TRUE, FALSE, NULL); // Manual-reset event, initially non-signaled
    if (sw_win->terminationEvent == NULL)
    {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to create termination event");
        free(sw_win);
        return false;
    }

    // Initialize the critical section for synchronization
    InitializeCriticalSection(&sw_win->mutex);

    // Directory handle and other file watching setup will be done
    // when adding targets to the watcher

    sw->targets = NULL;
    sw_win->handles_to_targets = NULL;
    sw->config = config;

    return true;
}

SWATCHER_API bool swatcher_start(swatcher *sw)
{
    if (!sw)
    {
        SWATCHER_LOG_DEFAULT_ERROR("swatcher is NULL\n");
        return false;
    }

    swatcher_windows *sw_win = (swatcher_windows *)sw->platform_data;

    // Check if the watcher is already running
    if (sw->running)
    {
        SWATCHER_LOG_DEFAULT_ERROR("Watcher is already running\n");
        return false;
    }

    // Start the watcher thread
    sw->running = true;
    sw_win->threadHandle = CreateThread(NULL, 0, swatcher_watcher_thread, sw, 0, &sw_win->threadId);
    if (sw_win->threadHandle == NULL)
    {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to create watcher thread\n");
        sw->running = false;
        return false;
    }

    return true;
}

SWATCHER_API void swatcher_stop(swatcher *sw)
{
    if (!sw || !sw->platform_data)
    {
        SWATCHER_LOG_DEFAULT_ERROR("swatcher or swatcher->platform_data is NULL\n");
        return;
    }

    swatcher_windows *sw_win = (swatcher_windows *)sw->platform_data;

    // Check if the watcher is already stopped
    if (!sw->running)
    {
        SWATCHER_LOG_DEFAULT_ERROR("Watcher is already stopped\n");
        return;
    }
    sw->running = false;

    // Stop the watcher thread
    if (sw_win->threadHandle != NULL)
    {
        SetEvent(sw_win->terminationEvent); // Signal the event to terminate the thread
        WaitForSingleObject(sw_win->threadHandle, INFINITE);
        // WaitForMultipleObjects(1, &sw_win->threadHandle, TRUE, INFINITE);
        CloseHandle(sw_win->threadHandle);
        sw_win->threadHandle = NULL;
        sw_win->threadId = 0;
        CloseHandle(sw_win->terminationEvent);
    }

    // if (sw_win->overlapped.hEvent != NULL)
    // {
    //     CloseHandle(sw_win->overlapped.hEvent);
    //     sw_win->overlapped.hEvent = NULL;
    // }
    // sw_win->overlapped.hEvent = NULL;

    // Destroy the critical section for synchronization
    DeleteCriticalSection(&sw_win->mutex);
}

SWATCHER_API swatcher_target *swatcher_target_create(swatcher_target_desc *desc)
{
    swatcher_target *target = malloc(sizeof(swatcher_target));
    if (target == NULL)
    {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to allocate swatcher_target");
        return NULL;
    }

    char normalized_path[MAX_PATH];
    if (!swatcher_validate_and_normalize_path(desc->path, normalized_path, desc->follow_symlinks))
    {
        free(target);
        return NULL;
    }

    DWORD attr = GetFileAttributesA(normalized_path);
    if (attr == INVALID_FILE_ATTRIBUTES)
    {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to get file attributes: %s", normalized_path);
        free(target);
        return NULL;
    }

    target->is_file = (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
    target->is_directory = (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    target->is_symlink = (attr & FILE_ATTRIBUTE_REPARSE_POINT) != 0;

    target->path = _strdup(normalized_path);
    if (target->path == NULL)
    {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to allocate path (string)");
        free(target);
        return NULL;
    }

    // if (desc->pattern == 0)
    // {
    //     target->pattern = NULL;
    // }
    // else
    // {
    //     target->pattern = _strdup(desc->pattern);
    //     if (target->pattern == NULL)
    //     {
    //         SWATCHER_LOG_DEFAULT_ERROR("Failed to allocate pattern (regex string)");
    //         free(target->path);
    //         free(target);
    //         return NULL;
    //     }
    // }

    target->is_recursive = desc->is_recursive;
    target->events = desc->events;
    target->user_data = desc->user_data;
    target->callback = desc->callback;
    target->last_event_time = time(NULL);

    // SWATCHER_LOG_DEFAULT_DEBUG("Created target: %s", target->path);

    return target;
}

bool add_watch(swatcher *sw, swatcher_target *target)
{
    // First, check if the path is already being watched.
    if (is_already_watched(sw, target->path))
    {
        SWATCHER_LOG_DEFAULT_WARNING("Path already watched: %s", target->path);
        return false;
    }

    swatcher_target_windows *sw_target_win = malloc(sizeof(swatcher_target_windows));
    if (sw_target_win == NULL)
    {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to allocate swatcher_target_windows");
        // CloseHandle(dir_handle);
        return false;
    }

    sw_target_win->target = target;

    HANDLE dir_handle = CreateFileA(target->path,
                                    FILE_LIST_DIRECTORY,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    NULL,
                                    OPEN_EXISTING,
                                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
                                    NULL);
    if (dir_handle == INVALID_HANDLE_VALUE)
    {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to open directory: %s", target->path);
        free(sw_target_win);
        return false;
    }

    sw_target_win->watchHandle = dir_handle;

    // Create an event for overlapped I/O
    sw_target_win->overlapped.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (sw_target_win->overlapped.hEvent == NULL)
    {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to create event for overlapped I/O");
        CloseHandle(dir_handle);
        free(sw_target_win);
        return false;
    }

    swatcher_windows *sw_win = (swatcher_windows *)sw->platform_data;
    sw_win->events = realloc(sw_win->events, (sw_win->num_events + 1) * sizeof(HANDLE));
    if (sw_win->events == NULL)
    {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to allocate memory for events");
        CloseHandle(dir_handle);
        CloseHandle(sw_target_win->overlapped.hEvent);
        free(sw_target_win);
        return false;
    }

    sw_win->events[sw_win->num_events] = sw_target_win->overlapped.hEvent;
    sw_win->num_events++;

    // Call ReadDirectoryChangesW to start watching the directory
    if (!ReadDirectoryChangesW(dir_handle,
                               sw_target_win->buffer,
                               BUFFER_SIZE,
                               target->is_recursive,
                               swatcher_target_events_to_notify_filter(target),
                               &sw_target_win->bytesReturned,
                               &sw_target_win->overlapped,
                               NULL))
    {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to start watching directory: %s", target->path);
        CloseHandle(dir_handle);
        CloseHandle(sw_target_win->overlapped.hEvent);
        free(sw_target_win);
        return false;
    }

    // add windows target to handles_to_targets
    // HASH_ADD_PTR(((swatcher_windows *)sw->platform_data)->handles_to_targets, watchHandle, sw_target_win);
    HASH_ADD_PTR(((swatcher_windows *)sw->platform_data)->handles_to_targets, overlapped.hEvent, sw_target_win);

    target->platform_data = sw_target_win;

    swatcher_target *found_target = NULL;
    HASH_FIND(hh_global, sw->targets, target->path, strlen(target->path), found_target);
    if (found_target == NULL)
    {
        HASH_ADD_KEYPTR(hh_global, sw->targets, target->path, strlen(target->path), target);
    }

    return true;
}

SWATCHER_API bool swatcher_add(swatcher *sw, swatcher_target *target)
{
    bool result = false;
    // if (!target->is_file && target->is_recursive)
    // {
    //     result = add_watch_recursive(sw, target, true);
    // }
    // else
    // {
    EnterCriticalSection(&((swatcher_windows *)sw->platform_data)->mutex);
    result = add_watch(sw, target);
    LeaveCriticalSection(&((swatcher_windows *)sw->platform_data)->mutex);
    // }
    return result;
}

SWATCHER_API bool swatcher_remove(swatcher *sw, swatcher_target *target)
{
    if (!sw || !sw->platform_data)
    {
        SWATCHER_LOG_DEFAULT_ERROR("swatcher or swatcher->platform_data is NULL\n");
        return false;
    }

    swatcher_windows *sw_win = (swatcher_windows *)sw->platform_data;

    EnterCriticalSection(&sw_win->mutex);

    swatcher_target_windows *sw_target_win = (swatcher_target_windows *)target->platform_data;
    if (sw_target_win != NULL)
    {
         // Remove the event handle from the events array
        for (int i = 0; i < sw_win->num_events; i++) {
            if (sw_win->events[i] == sw_target_win->overlapped.hEvent) {
                // Shift the remaining handles down
                for (int j = i; j < sw_win->num_events - 1; j++) {
                    sw_win->events[j] = sw_win->events[j + 1];
                }
                sw_win->num_events--;
                // Reallocate the eventHandles array to its new size
                HANDLE* newEventHandles = realloc(sw_win->events, sw_win->num_events * sizeof(HANDLE));
                if (newEventHandles != NULL || sw_win->num_events == 0) {
                    sw_win->events = newEventHandles;
                } // If realloc fails but num_events is > 0, it's a rare case of realloc failure during shrinkage, which we might choose to ignore since the memory is still valid
                break;
            }
        }

        CloseHandle(sw_target_win->watchHandle);
        CloseHandle(sw_target_win->overlapped.hEvent);

        HASH_DEL(sw_win->handles_to_targets, sw_target_win);
        free(sw_target_win);
    }

    swatcher_target *found_target = NULL;
    HASH_FIND(hh_global, sw->targets, target->path, strlen(target->path), found_target);
    if (found_target != NULL)
    {
        HASH_DELETE(hh_global, sw->targets, found_target);
        free(found_target->path);
        // free(found_target->pattern);
        free(found_target);
    }

    LeaveCriticalSection(&sw_win->mutex);
    return true;
}

SWATCHER_API void swatcher_cleanup(swatcher *sw)
{
    swatcher_target *current, *tmp;
    swatcher_windows *sw_win = (swatcher_windows *)sw->platform_data;

    HASH_ITER(hh_global, sw->targets, current, tmp)
    {
        swatcher_target_windows *sw_target_win = (swatcher_target_windows *)current->platform_data;
        if (sw_target_win != NULL)
        {
            HASH_DEL(sw_win->handles_to_targets, sw_target_win);

            CloseHandle(sw_target_win->watchHandle);
            CloseHandle(sw_target_win->overlapped.hEvent);
            free(sw_target_win);
        }

        HASH_DELETE(hh_global, sw->targets, current);
        free(current->path);
        free(current);
    }

    sw_win->num_events = 0;
    free(sw_win->events);
    sw_win->events = NULL;

    DeleteCriticalSection(&sw_win->mutex);

    free(sw_win);
    sw->platform_data = NULL;

    free(sw);

    _CrtDumpMemoryLeaks();

    return;
}

void process_directory_changes(const char *buffer, DWORD bytesReturned, swatcher_target_windows *sw_target_win)
{
    char *p = (char *)buffer;
    FILE_NOTIFY_INFORMATION *fni = NULL;
    WCHAR filename[MAX_PATH]; // Use wide char for filename
    char fullpath[MAX_PATH * 2];
    swatcher_fs_event event = SWATCHER_EVENT_NONE;

    do
    {
        fni = (FILE_NOTIFY_INFORMATION *)p;

        // Copy the filename from the notification information
        wcsncpy_s(filename, MAX_PATH, fni->FileName, fni->FileNameLength / sizeof(WCHAR));
        filename[fni->FileNameLength / sizeof(WCHAR)] = L'\0'; // Ensure null-termination

        // Convert filename from wide char to multibyte UTF-8
        int bufferSize = WideCharToMultiByte(CP_UTF8, 0, filename, -1, NULL, 0, NULL, NULL);
        char *filenameUTF8 = (char *)malloc(bufferSize);

        WideCharToMultiByte(CP_UTF8, 0, filename, -1, filenameUTF8, bufferSize, NULL, NULL);
        snprintf(fullpath, sizeof(fullpath), "%s\\%s", sw_target_win->target->path, filenameUTF8);

        switch (fni->Action)
        {
        case FILE_ACTION_ADDED:
            event = SWATCHER_EVENT_CREATED;
            break;
        case FILE_ACTION_REMOVED:
            event = SWATCHER_EVENT_DELETED;
            break;
        case FILE_ACTION_MODIFIED:
            event = SWATCHER_EVENT_MODIFIED;
            break;
        case FILE_ACTION_RENAMED_OLD_NAME:
            event = SWATCHER_EVENT_MOVED;
            break;
        case FILE_ACTION_RENAMED_NEW_NAME:
            event = SWATCHER_EVENT_MOVED;
            break;
        default:
            SWATCHER_LOG_DEFAULT_ERROR("Unknown file action: %d", fni->Action);
            continue;
        }

        if (sw_target_win->target->callback != NULL)
        {
            sw_target_win->target->callback(event, sw_target_win->target, fullpath, fni);
            free(filenameUTF8);
        }

        sw_target_win->target->last_event_time = time(NULL);

        p += fni->NextEntryOffset;
    } while (fni->NextEntryOffset != 0);
}

DWORD WINAPI swatcher_watcher_thread(LPVOID arg)
{
    swatcher *sw = (swatcher *)arg;
    swatcher_windows *sw_win = (swatcher_windows *)sw->platform_data;

    DWORD waitStatus;
    BOOL result = FALSE;

    // Temporary array for handles, assuming sw_win->num_events does not include the termination event
    HANDLE* tempHandles = malloc((sw_win->num_events + 1) * sizeof(HANDLE));
    if (!tempHandles) {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to allocate memory for handles");
        return 1; // Use an appropriate error code
    }

    while (sw->running)
    {
        // Copy existing event handles
        memcpy(tempHandles, sw_win->events, sw_win->num_events * sizeof(HANDLE));
        // Add termination event as the last handle in the array
        tempHandles[sw_win->num_events] = sw_win->terminationEvent;

        waitStatus = WaitForMultipleObjects(sw_win->num_events + 1, tempHandles, FALSE, INFINITE);

        if (waitStatus == WAIT_FAILED)
        {
            SWATCHER_LOG_DEFAULT_ERROR("WaitForMultipleObjects failed");
            break;
        }

        if (waitStatus == WAIT_OBJECT_0 + sw_win->num_events) {
            break; // Exit loop if termination event was signaled
        }

        if (waitStatus >= WAIT_OBJECT_0 && waitStatus < WAIT_OBJECT_0 + sw_win->num_events)
        {
            swatcher_target_windows *sw_target_win = NULL;
            for (int i = 0; i < sw_win->num_events; i++)
            {
                if (sw_win->events[i] == sw_win->events[waitStatus - WAIT_OBJECT_0])
                {
                    HASH_FIND_PTR(sw_win->handles_to_targets, &sw_win->events[i], sw_target_win);
                    break;
                }
            }

            if (sw_target_win == NULL)
            {
                SWATCHER_LOG_DEFAULT_ERROR("Failed to find target for event");
                continue;
            }

            result = GetOverlappedResult(sw_target_win->watchHandle, &sw_target_win->overlapped, &sw_target_win->bytesReturned, FALSE);
            if (!result)
            {
                SWATCHER_LOG_DEFAULT_ERROR("GetOverlappedResult failed");
                continue;
            }

            process_directory_changes(sw_target_win->buffer, sw_target_win->bytesReturned, sw_target_win);

            // Call ReadDirectoryChangesW to continue watching the directory
            if (!ReadDirectoryChangesW(sw_target_win->watchHandle,
                                       sw_target_win->buffer,
                                       BUFFER_SIZE,
                                       sw_target_win->target->is_recursive,
                                       swatcher_target_events_to_notify_filter(sw_target_win->target),
                                       &sw_target_win->bytesReturned,
                                       &sw_target_win->overlapped,
                                       NULL))
            {
                SWATCHER_LOG_DEFAULT_ERROR("Failed to continue watching directory: %s", sw_target_win->target->path);
            }
        }
    
        Sleep(10);
    }

    free(tempHandles);
    return 0;
}
