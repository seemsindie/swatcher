#include <windows.h>
#include <ctype.h>

typedef struct swatcher_windows
{
    HANDLE dirHandle;     // Handle for directory changes
    HANDLE threadHandle;    // Handle for the watcher thread
    DWORD threadId;         // Thread identifier
    CRITICAL_SECTION mutex; // Mutex for thread synchronization
    bool running;           // Flag to control the running state
    OVERLAPPED overlapped;  // Overlapped I/O structure for asynchronous operations
} swatcher_windows;

typedef struct swatcher_target_windows
{
    HANDLE watchHandle; // Handle for the specific watch target
} swatcher_target_windows;

#define BUF_LEN 4096

bool swatcher_is_absolute_path(const char *path)
{
    if (path == NULL)
    {
        return false;
    }
    return (isalpha(path[0]) && path[1] == ':' && path[2] == '\\') || // Drive letter, e.g., C:\
           (path[0] == '\\' && path[1] == '\\'); // UNC path, e.g., \\server\share
}

bool swatcher_validate_and_normalize_path(const char *input_path, char *normalized_path)
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

bool swatcher_init(swatcher *swatcher, swatcher_config *config)
{
    if (!swatcher)
    {
        SWATCHER_LOG_DEFAULT_ERROR("swatcher is NULL\n");
        return false;
    }

    if (!config)
    {
        SWATCHER_LOG_DEFAULT_ERROR("config is NULL\n");
        return false;
    }

    swatcher->platform_data = malloc(sizeof(swatcher_windows));
    if (swatcher->platform_data == NULL)
    {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to allocate swatcher_windows");
        return false;
    }

    swatcher_windows *sw_win = (swatcher_windows *)swatcher->platform_data;
    sw_win->running = false;
    sw_win->threadId = 0; // Initialize to zero

    // Initialize the critical section for synchronization
    InitializeCriticalSection(&sw_win->mutex);

    // Create an event for overlapped I/O
    sw_win->overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (sw_win->overlapped.hEvent == NULL)
    {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to create event for overlapped I/O");
        DeleteCriticalSection(&sw_win->mutex);
        free(swatcher->platform_data);
        return false;
    }

    // Directory handle and other file watching setup will be done
    // when adding targets to the watcher

    swatcher->targets_head = NULL;
    swatcher->config = config;

    return true;
}

bool swatcher_start(swatcher *swatcher)
{
    if (!swatcher)
    {
        SWATCHER_LOG_DEFAULT_ERROR("swatcher is NULL\n");
        return false;
    }

    swatcher_windows *sw_win = (swatcher_windows *)swatcher->platform_data;

    // Check if the watcher is already running
    if (sw_win->running)
    {
        SWATCHER_LOG_DEFAULT_ERROR("Watcher is already running\n");
        return false;
    }

    // Start the watcher thread
    sw_win->running = true;
    sw_win->threadHandle = CreateThread(NULL, 0, watcher_thread, swatcher, 0, &sw_win->threadId);
    if (sw_win->threadHandle == NULL)
    {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to create watcher thread\n");
        sw_win->running = false;
        return false;
    }

    return true;
}

void swatcher_stop(swatcher *swatcher)
{
    if (!swatcher || !swatcher->platform_data)
    {
        SWATCHER_LOG_DEFAULT_ERROR("swatcher or swatcher->platform_data is NULL\n");
        return;
    }

    swatcher_windows *sw_win = (swatcher_windows *)swatcher->platform_data;

    // Check if the watcher is already stopped
    if (!sw_win->running)
    {
        SWATCHER_LOG_DEFAULT_ERROR("Watcher is already stopped\n");
        return;
    }

    // Stop the watcher thread
    if (sw_win->threadHandle != NULL) {
        sw_win->running = false;
        WaitForSingleObject(sw_win->threadHandle, INFINITE);
        CloseHandle(sw_win->threadHandle);
        sw_win->threadHandle = NULL;
        sw_win->threadId = 0;
    }

    if (sw_win->dirHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(sw_win->dirHandle);
        sw_win->dirHandle = INVALID_HANDLE_VALUE;
    }
    sw_win->dirHandle = INVALID_HANDLE_VALUE;

    if (sw_win->overlapped.hEvent != NULL) {
        CloseHandle(sw_win->overlapped.hEvent);
        sw_win->overlapped.hEvent = NULL;
    }
    sw_win->overlapped.hEvent = NULL;

    // Destroy the critical section for synchronization
    DeleteCriticalSection(&sw_win->mutex);
}

swatcher_target *swatcher_target_create(swatcher_target_desc *desc)
{
    swatcher_target *target = malloc(sizeof(swatcher_target));
    if (target == NULL)
    {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to allocate swatcher_target");
        return NULL;
    }

    char normalized_path[MAX_PATH];
    if (!swatcher_validate_and_normalize_path(desc->path, normalized_path))
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

    if (desc->pattern == 0)
    {
        target->pattern = NULL;
    }
    else
    {
        target->pattern = _strdup(desc->pattern);
        if (target->pattern == NULL)
        {
            SWATCHER_LOG_DEFAULT_ERROR("Failed to allocate pattern (regex string)");
            free(target->path);
            free(target);
            return NULL;
        }
    }

    target->is_recursive = desc->is_recursive;
    target->events = desc->events;
    target->user_data = desc->user_data;
    target->callback = desc->callback;
    target->last_event_time = time(NULL);

    // SWATCHER_LOG_DEFAULT_DEBUG("Created target: %s", target->path);

    return target;
}

bool swatcher_add(swatcher *swatcher, swatcher_target *target)
{
    bool result = false;
    if (!target->is_file && target->is_recursive)
    {
        result = add_watch_recursive(swatcher, target, true);
    }
    else
    {
        EnterCriticalSection(&((swatcher_windows *)swatcher->platform_data)->mutex);
        result = add_watch(swatcher, target);
        LeaveCriticalSection(&((swatcher_windows *)swatcher->platform_data)->mutex);
    }
    return result;
}

bool swatcher_remove(swatcher *swatcher, swatcher_target *target)
{
    if (!swatcher || !swatcher->platform_data)
    {
        SWATCHER_LOG_DEFAULT_ERROR("swatcher or swatcher->platform_data is NULL\n");
        return false;
    }

    swatcher_windows *sw_win = (swatcher_windows *)swatcher->platform_data;

    EnterCriticalSection(&sw_win->mutex);

    swatcher_target_node *prev = NULL;
    swatcher_target_node *curr = swatcher->targets_head;
    while (curr != NULL)
    {
        if (curr->target == target)
        {
            if (prev == NULL)
            {
                swatcher->targets_head = curr->next;
            }
            else
            {
                prev->next = curr->next;
            }

            swatcher_target_windows *sw_target_win = (swatcher_target_windows *)curr->platform_data;
            CloseHandle(sw_target_win->watchHandle);
            free(sw_target_win);
            free(curr->target->path);
            free(curr->target->pattern);
            free(curr->target);
            free(curr);
            LeaveCriticalSection(&sw_win->mutex);
            return true;
        }
        prev = curr;
        curr = curr->next;
    }

    LeaveCriticalSection(&sw_win->mutex);
    return false;
}

void swatcher_cleanup(swatcher *swatcher)
{
    if (!swatcher || !swatcher->platform_data)
    {
        SWATCHER_LOG_DEFAULT_ERROR("swatcher or swatcher->platform_data is NULL\n");
        return;
    }

    swatcher_windows *sw_win = (swatcher_windows *)swatcher->platform_data;

    // Stop the watcher thread if it is running
    if (sw_win->running)
    {
        swatcher_stop(swatcher);
    }

    // Free the targets
    swatcher_target_node *curr = swatcher->targets_head;
    while (curr != NULL)
    {
        swatcher_target_node *next = curr->next;
        swatcher_target_windows *sw_target_win = (swatcher_target_windows *)curr->platform_data;

        if (sw_target_win->watchHandle != INVALID_HANDLE_VALUE) {
            CloseHandle(sw_target_win->watchHandle);
        }
        
        free(sw_target_win);
        free(curr->target->path);
        free(curr->target->pattern);
        free(curr->target);
        free(curr);
        curr = next;
    }
    swatcher->targets_head = NULL;

    // Free the platform data
    free(swatcher->platform_data);
    swatcher->platform_data = NULL;

    // Free the swatcher
    free(swatcher);
}

bool add_watch(swatcher *swatcher, swatcher_target *target)
{
    swatcher_windows *sw_win = (swatcher_windows *)swatcher->platform_data;

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
        return false;
    }

    swatcher_target_node *new_node = malloc(sizeof(swatcher_target_node));
    if (new_node == NULL)
    {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to allocate swatcher_target_node");
        CloseHandle(dir_handle);
        return false;
    }

    new_node->target = target;

    swatcher_target_windows *sw_target_win = malloc(sizeof(swatcher_target_windows));
    if (sw_target_win == NULL) {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to allocate swatcher_target_windows");
        CloseHandle(dir_handle);
        free(new_node);
        return false;
    }

    sw_target_win->watchHandle = dir_handle;
    new_node->platform_data = sw_target_win;

    // Add the new node to the list
    new_node->next = swatcher->targets_head;
    swatcher->targets_head = new_node;

    // SWATCHER_LOG_DEFAULT_DEBUG("Added target: %s", target->path);

    return true;
}

bool add_watch_recursive(swatcher *swatcher, swatcher_target *target, bool is_first_call)
{
    EnterCriticalSection(&((swatcher_windows *)swatcher->platform_data)->mutex);
    bool result = add_watch_recursive_locked(swatcher, target, is_first_call);
    LeaveCriticalSection(&((swatcher_windows *)swatcher->platform_data)->mutex);
    return result;
}

bool add_watch_recursive_locked(swatcher *swatcher, swatcher_target *target, bool is_first_call)
{

    if (!add_watch(swatcher, target))
    {
        return false;
    }

    // if (target->is_symlink) {
    //     SWATCHER_LOG_DEFAULT_ERROR("Target is a symlink: %s", target->path);
    //     return false;
    // }

    if (!target->is_directory)
    {
        return false;
    }

    char search_path[MAX_PATH];
    snprintf(search_path, MAX_PATH, "%s\\*", target->path);

    WIN32_FIND_DATAA find_data;
    HANDLE find_handle = FindFirstFileA(search_path, &find_data);
    if (find_handle == INVALID_HANDLE_VALUE)
    {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to find first file: %s", search_path);
        return false;
    }

    do
    {
        if (strcmp(find_data.cFileName, ".") == 0 || strcmp(find_data.cFileName, "..") == 0)
        {
            continue;
        }

        char child_path[MAX_PATH];
        snprintf(child_path, MAX_PATH, "%s\\%s", target->path, find_data.cFileName);

        DWORD attr = GetFileAttributesA(child_path);
        if (attr == INVALID_FILE_ATTRIBUTES)
        {
            SWATCHER_LOG_DEFAULT_ERROR("Failed to get file attributes: %s", child_path);
            continue;
        }

        if ((attr & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            swatcher_target_desc desc = {
                .path = child_path,
                .pattern = target->pattern,
                .is_recursive = target->is_recursive,
                .events = target->events,
                .user_data = target->user_data,
                .callback = target->callback};
            swatcher_target *child_target = swatcher_target_create(&desc);
            if (child_target == NULL)
            {
                SWATCHER_LOG_DEFAULT_ERROR("Failed to create child target: %s", child_path);
                continue;
            }

            if (!add_watch_recursive_locked(swatcher, child_target, false))
            {
                SWATCHER_LOG_DEFAULT_ERROR("Failed to add child target: %s", child_path);
                swatcher_target_destroy(child_target);
                continue;
            }
        }
    } while (FindNextFileA(find_handle, &find_data) != 0);

    FindClose(find_handle);

    return true;
}

void process_directory_changes(const char *buffer, DWORD bytesReturned, swatcher *swatcher_instance)
{
    char *p = buffer;
    FILE_NOTIFY_INFORMATION *fni = NULL;
    char filename[MAX_PATH];
    char fullpath[MAX_PATH];
    swatcher_target *target = NULL;
    swatcher_event event = SWATCHER_EVENT_NONE;

    do
    {
        fni = (FILE_NOTIFY_INFORMATION *)p;

        // Convert the filename to a null-terminated string
        memcpy(filename, fni->FileName, fni->FileNameLength);
        filename[fni->FileNameLength / sizeof(WCHAR)] = '\0';

        // Get the full path of the file
        swatcher_target_node *curr = swatcher_instance->targets_head;
        while (curr != NULL)
        {
            target = curr->target;
            if (strcmp(target->path, filename) == 0)
            {
                break;
            }
            curr = curr->next;
        }

        if (curr == NULL)
        {
            SWATCHER_LOG_DEFAULT_ERROR("Failed to find target for file: %s", filename);
            continue;
        }

        snprintf(fullpath, MAX_PATH, "%s\\%s", target->path, filename);

        // SWATCHER_LOG_DEFAULT_DEBUG("File change: %s", fullpath);

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
            event = SWATCHER_EVENT_RENAMED_OLD;
            break;
        case FILE_ACTION_RENAMED_NEW_NAME:
            event = SWATCHER_EVENT_RENAMED_NEW;
            break;
        default:
            SWATCHER_LOG_DEFAULT_ERROR("Unknown file action: %d", fni->Action);
            continue;
        }

        if (target->callback != NULL)
        {
            target->callback(target, event, fullpath, target->user_data);
        }

        target->last_event_time = time(NULL);

        p += fni->NextEntryOffset;
    } while (fni->NextEntryOffset != 0);
}

DWORD WINAPI swatcher_watcher_thread(LPVOID arg)
{
    swatcher *swatcher_instance = (swatcher *)arg;
    swatcher_windows *sw_win = (swatcher_windows *)swatcher_instance->platform_data;

    // SWATCHER_LOG_DEFAULT_DEBUG("Watcher thread started");

    while (sw_win->running)
    {
        // Wait for the directory to change
        DWORD bytesReturned = 0;
        if (!ReadDirectoryChangesW(sw_win->dirHandle,
                                   sw_win->overlapped.hEvent,
                                   BUF_LEN,
                                   TRUE,
                                   FILE_NOTIFY_CHANGE_FILE_NAME |
                                       FILE_NOTIFY_CHANGE_DIR_NAME |
                                       FILE_NOTIFY_CHANGE_ATTRIBUTES |
                                       FILE_NOTIFY_CHANGE_SIZE |
                                       FILE_NOTIFY_CHANGE_LAST_WRITE |
                                       FILE_NOTIFY_CHANGE_LAST_ACCESS |
                                       FILE_NOTIFY_CHANGE_CREATION |
                                       FILE_NOTIFY_CHANGE_SECURITY,
                                   &bytesReturned,
                                   &sw_win->overlapped,
                                   NULL))
        {
            SWATCHER_LOG_DEFAULT_ERROR("ReadDirectoryChangesW failed");
            break;
        }

        // Wait for the overlapped I/O to complete
        DWORD waitResult = WaitForSingleObject(sw_win->overlapped.hEvent, INFINITE);
        if (waitResult != WAIT_OBJECT_0)
        {
            SWATCHER_LOG_DEFAULT_ERROR("WaitForSingleObject failed");
            break;
        }

        // Get the results of the overlapped I/O
        if (!GetOverlappedResult(sw_win->dirHandle, &sw_win->overlapped, &bytesReturned, FALSE))
        {
            SWATCHER_LOG_DEFAULT_ERROR("GetOverlappedResult failed");
            break;
        }

        // Process the directory changes
        process_directory_changes(sw_win->overlapped.Pointer, bytesReturned, swatcher_instance);
    }

    // SWATCHER_LOG_DEFAULT_DEBUG("Watcher thread stopped");

    return 0;
}
