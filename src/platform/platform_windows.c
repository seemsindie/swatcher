#if defined(_WIN32) || defined(_WIN64)

#include "platform.h"
#include "../internal/alloc.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <windows.h>

/* ========== UTF-8 <-> UTF-16 helpers ========== */

static WCHAR *utf8_to_wide(const char *utf8)
{
    if (!utf8) return NULL;
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (len <= 0) return NULL;
    WCHAR *wide = sw_malloc(len * sizeof(WCHAR));
    if (!wide) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, len);
    return wide;
}

static char *wide_to_utf8_alloc(const WCHAR *wide)
{
    if (!wide) return NULL;
    int len = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    if (len <= 0) return NULL;
    char *utf8 = sw_malloc(len);
    if (!utf8) return NULL;
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, len, NULL, NULL);
    return utf8;
}

/* ========== Mutex ========== */

struct sw_mutex {
    CRITICAL_SECTION cs;
};

sw_mutex *sw_mutex_create(void)
{
    sw_mutex *m = sw_malloc(sizeof(sw_mutex));
    if (!m) return NULL;
    InitializeCriticalSection(&m->cs);
    return m;
}

void sw_mutex_destroy(sw_mutex *m)
{
    if (!m) return;
    DeleteCriticalSection(&m->cs);
    sw_free(m);
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
    sw_free(data);
    func(arg);
    return 0;
}

struct sw_thread {
    HANDLE handle;
};

sw_thread *sw_thread_create(sw_thread_func func, void *arg)
{
    sw_thread *t = sw_malloc(sizeof(sw_thread));
    if (!t) return NULL;

    sw_thread_trampoline_data *data = sw_malloc(sizeof(sw_thread_trampoline_data));
    if (!data) {
        sw_free(t);
        return NULL;
    }
    data->func = func;
    data->arg = arg;

    DWORD tid;
    t->handle = CreateThread(NULL, 0, sw_thread_trampoline, data, 0, &tid);
    if (t->handle == NULL) {
        sw_free(data);
        sw_free(t);
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
    sw_free(t);
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
        WCHAR cwd_wide[MAX_PATH];
        if (GetCurrentDirectoryW(MAX_PATH, cwd_wide) != 0) {
            char *cwd = wide_to_utf8_alloc(cwd_wide);
            if (!cwd) return false;
            snprintf(output, size, "%s\\%s", cwd, input);
            sw_free(cwd);
            for (size_t i = 0; output[i] != '\0'; i++) {
                if (output[i] == '/') output[i] = '\\';
            }
        } else {
            return false;
        }
    } else {
        if (_fullpath(output, input, (int)size) == NULL)
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

    WCHAR *wide_path = utf8_to_wide(path);
    if (!wide_path) return false;

    DWORD attr = GetFileAttributesW(wide_path);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        sw_free(wide_path);
        return false;
    }

    info->is_directory = (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    info->is_file = !info->is_directory;
    info->is_symlink = (attr & FILE_ATTRIBUTE_REPARSE_POINT) != 0;

    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExW(wide_path, GetFileExInfoStandard, &fad)) {
        ULARGE_INTEGER li;
        li.LowPart = fad.nFileSizeLow;
        li.HighPart = fad.nFileSizeHigh;
        info->size = (uint64_t)li.QuadPart;
        /* Convert FILETIME to seconds since Unix epoch */
        ULARGE_INTEGER ft;
        ft.LowPart = fad.ftLastWriteTime.dwLowDateTime;
        ft.HighPart = fad.ftLastWriteTime.dwHighDateTime;
        info->mtime = (uint64_t)((ft.QuadPart - 116444736000000000ULL) / 10000000ULL);
    } else {
        info->size = 0;
        info->mtime = 0;
    }

    sw_free(wide_path);
    return true;
}

/* ========== Directory iteration ========== */

struct sw_dir {
    HANDLE handle;
    WIN32_FIND_DATAW ffd;
    bool first;
};

sw_dir *sw_dir_open(const char *path)
{
    char pattern[SW_PATH_MAX];
    snprintf(pattern, SW_PATH_MAX, "%s\\*", path);

    WCHAR *wide_pattern = utf8_to_wide(pattern);
    if (!wide_pattern) return NULL;

    sw_dir *d = sw_malloc(sizeof(sw_dir));
    if (!d) {
        sw_free(wide_pattern);
        return NULL;
    }

    d->handle = FindFirstFileW(wide_pattern, &d->ffd);
    sw_free(wide_pattern);

    if (d->handle == INVALID_HANDLE_VALUE) {
        sw_free(d);
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
        if (!FindNextFileW(d->handle, &d->ffd))
            return false;
    }

    /* Convert filename from UTF-16 to UTF-8 */
    char *utf8_name = wide_to_utf8_alloc(d->ffd.cFileName);
    if (!utf8_name) return false;
    strncpy(entry->name, utf8_name, sizeof(entry->name) - 1);
    entry->name[sizeof(entry->name) - 1] = '\0';
    sw_free(utf8_name);

    entry->is_dir = (d->ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    entry->is_file = !entry->is_dir;
    entry->is_symlink = (d->ffd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    return true;
}

void sw_dir_close(sw_dir *d)
{
    if (!d) return;
    FindClose(d->handle);
    sw_free(d);
}

/* ========== Monotonic time ========== */

uint64_t sw_time_now_ms(void)
{
    return (uint64_t)GetTickCount64();
}

/* ========== Sleep ========== */

void sw_sleep_ms(int ms)
{
    Sleep((DWORD)ms);
}

/* ========== String ========== */

char *sw_strdup(const char *s)
{
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char *d = sw_malloc(len);
    if (d) memcpy(d, s, len);
    return d;
}

/* ========== Remote filesystem detection ========== */

bool sw_filesystem_is_remote(const char *path)
{
    if (!path) return false;
    /* Extract drive root (e.g., "C:\") or use UNC path */
    char root[4] = {0};
    if (path[0] == '\\' && path[1] == '\\')
        return true; /* UNC paths are always remote */
    if (isalpha(path[0]) && path[1] == ':') {
        root[0] = path[0];
        root[1] = ':';
        root[2] = '\\';
        root[3] = '\0';
    } else {
        return false;
    }
    WCHAR *wide_root = utf8_to_wide(root);
    if (!wide_root) return false;
    UINT type = GetDriveTypeW(wide_root);
    sw_free(wide_root);
    return type == DRIVE_REMOTE;
}

#endif /* _WIN32 */
