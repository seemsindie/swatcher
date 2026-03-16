#if defined(_WIN32) || defined(_WIN64)

#include "platform.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <windows.h>

/* ========== Mutex ========== */

struct sw_mutex {
    CRITICAL_SECTION cs;
};

sw_mutex *sw_mutex_create(void)
{
    sw_mutex *m = malloc(sizeof(sw_mutex));
    if (!m) return NULL;
    InitializeCriticalSection(&m->cs);
    return m;
}

void sw_mutex_destroy(sw_mutex *m)
{
    if (!m) return;
    DeleteCriticalSection(&m->cs);
    free(m);
}

void sw_mutex_lock(sw_mutex *m)
{
    EnterCriticalSection(&m->cs);
}

void sw_mutex_unlock(sw_mutex *m)
{
    LeaveCriticalSection(&m->cs);
}

/* ========== Thread ========== */

typedef struct sw_thread_trampoline_data {
    sw_thread_func func;
    void *arg;
} sw_thread_trampoline_data;

static DWORD WINAPI sw_thread_trampoline(LPVOID param)
{
    sw_thread_trampoline_data *data = (sw_thread_trampoline_data *)param;
    sw_thread_func func = data->func;
    void *arg = data->arg;
    free(data);
    func(arg);
    return 0;
}

struct sw_thread {
    HANDLE handle;
};

sw_thread *sw_thread_create(sw_thread_func func, void *arg)
{
    sw_thread *t = malloc(sizeof(sw_thread));
    if (!t) return NULL;

    sw_thread_trampoline_data *data = malloc(sizeof(sw_thread_trampoline_data));
    if (!data) {
        free(t);
        return NULL;
    }
    data->func = func;
    data->arg = arg;

    DWORD tid;
    t->handle = CreateThread(NULL, 0, sw_thread_trampoline, data, 0, &tid);
    if (t->handle == NULL) {
        free(data);
        free(t);
        return NULL;
    }
    return t;
}

void sw_thread_join(sw_thread *t)
{
    if (t && t->handle) WaitForSingleObject(t->handle, INFINITE);
}

void sw_thread_destroy(sw_thread *t)
{
    if (!t) return;
    if (t->handle) CloseHandle(t->handle);
    free(t);
}

/* ========== Path utilities ========== */

bool sw_path_is_absolute(const char *path)
{
    if (!path) return false;
    return (isalpha(path[0]) && path[1] == ':' && (path[2] == '\\' || path[2] == '/')) ||
           (path[0] == '\\' && path[1] == '\\');
}

bool sw_path_normalize(const char *input, char *output, size_t size, bool resolve_symlinks)
{
    (void)resolve_symlinks; /* Windows: _fullpath resolves what it can */

    if (!sw_path_is_absolute(input)) {
        char cwd[MAX_PATH];
        if (GetCurrentDirectoryA(MAX_PATH, cwd) != 0) {
            snprintf(output, size, "%s\\%s", cwd, input);
            for (size_t i = 0; output[i] != '\0'; i++) {
                if (output[i] == '/') output[i] = '\\';
            }
        } else {
            return false;
        }
    } else {
        if (_fullpath(output, input, size) == NULL)
            return false;
    }
    return true;
}

char sw_path_separator(void)
{
    return '\\';
}

/* ========== File info ========== */

bool sw_stat(const char *path, sw_file_info *info, bool follow_symlinks)
{
    (void)follow_symlinks;
    DWORD attr = GetFileAttributesA(path);
    if (attr == INVALID_FILE_ATTRIBUTES)
        return false;

    info->is_directory = (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    info->is_file = !info->is_directory;
    info->is_symlink = (attr & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    return true;
}

/* ========== Directory iteration ========== */

struct sw_dir {
    HANDLE handle;
    WIN32_FIND_DATAA ffd;
    bool first;
};

sw_dir *sw_dir_open(const char *path)
{
    char pattern[MAX_PATH];
    snprintf(pattern, MAX_PATH, "%s\\*", path);

    sw_dir *d = malloc(sizeof(sw_dir));
    if (!d) return NULL;

    d->handle = FindFirstFileA(pattern, &d->ffd);
    if (d->handle == INVALID_HANDLE_VALUE) {
        free(d);
        return NULL;
    }
    d->first = true;
    return d;
}

bool sw_dir_next(sw_dir *d, sw_dir_entry *entry)
{
    if (d->first) {
        d->first = false;
    } else {
        if (!FindNextFileA(d->handle, &d->ffd))
            return false;
    }

    strncpy(entry->name, d->ffd.cFileName, sizeof(entry->name) - 1);
    entry->name[sizeof(entry->name) - 1] = '\0';
    entry->is_dir = (d->ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    entry->is_file = !entry->is_dir;
    entry->is_symlink = (d->ffd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    return true;
}

void sw_dir_close(sw_dir *d)
{
    if (!d) return;
    FindClose(d->handle);
    free(d);
}

/* ========== String ========== */

char *sw_strdup(const char *s)
{
    return _strdup(s);
}

#endif /* _WIN32 */
