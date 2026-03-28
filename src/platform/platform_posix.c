#if defined(__linux__) || defined(__unix__) || defined(__unix) || defined(unix) || defined(__APPLE__)

#include "platform.h"
#include "../internal/alloc.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <limits.h>
#include <stdio.h>

#if defined(__linux__)
#include <sys/statfs.h>
#else
#include <sys/mount.h>
#endif

/* ========== Mutex ========== */

struct sw_mutex {
    pthread_mutex_t mtx;
};

sw_mutex *sw_mutex_create(void)
{
    sw_mutex *m = sw_malloc(sizeof(sw_mutex));
    if (!m) return NULL;
    if (pthread_mutex_init(&m->mtx, NULL) != 0) {
        sw_free(m);
        return NULL;
    }
    return m;
}

void sw_mutex_destroy(sw_mutex *m)
{
    if (!m) return;
    pthread_mutex_destroy(&m->mtx);
    sw_free(m);
}

void sw_mutex_lock(sw_mutex *m)
{
    pthread_mutex_lock(&m->mtx);
}

void sw_mutex_unlock(sw_mutex *m)
{
    pthread_mutex_unlock(&m->mtx);
}

/* ========== Thread ========== */

struct sw_thread {
    pthread_t handle;
};

sw_thread *sw_thread_create(sw_thread_func func, void *arg)
{
    sw_thread *t = sw_malloc(sizeof(sw_thread));
    if (!t) return NULL;
    if (pthread_create(&t->handle, NULL, func, arg) != 0) {
        sw_free(t);
        return NULL;
    }
    return t;
}

void sw_thread_join(sw_thread *t)
{
    if (t) pthread_join(t->handle, NULL);
}

void sw_thread_destroy(sw_thread *t)
{
    sw_free(t);
}

/* ========== Path utilities ========== */

bool sw_path_is_absolute(const char *path)
{
    return path && path[0] == '/';
}

bool sw_path_normalize(const char *input, char *output, size_t size, bool resolve_symlinks)
{
    if (!resolve_symlinks) {
        if (sw_path_is_absolute(input)) {
            strncpy(output, input, size);
            output[size - 1] = '\0';
        } else {
            char cwd[PATH_MAX];
            if (getcwd(cwd, sizeof(cwd)) == NULL)
                return false;
            snprintf(output, size, "%s/%s", cwd, input);
        }
    } else {
        if (!sw_path_is_absolute(input)) {
            char cwd[PATH_MAX];
            if (getcwd(cwd, sizeof(cwd)) == NULL)
                return false;
            char temp[PATH_MAX * 2];
            snprintf(temp, sizeof(temp), "%s/%s", cwd, input);
            if (realpath(temp, output) == NULL)
                return false;
        } else {
            if (realpath(input, output) == NULL)
                return false;
        }
    }
    return true;
}

char sw_path_separator(void)
{
    return '/';
}

/* ========== File info ========== */

bool sw_stat(const char *path, sw_file_info *info, bool follow_symlinks)
{
    struct stat st;
    int ret;
    if (follow_symlinks)
        ret = stat(path, &st);
    else
        ret = lstat(path, &st);
    if (ret != 0)
        return false;

    info->is_file = S_ISREG(st.st_mode);
    info->is_directory = S_ISDIR(st.st_mode);
    info->is_symlink = S_ISLNK(st.st_mode);
    info->size = (uint64_t)st.st_size;
    info->mtime = (uint64_t)st.st_mtime;
    return true;
}

/* ========== Directory iteration ========== */

struct sw_dir {
    DIR *handle;
};

sw_dir *sw_dir_open(const char *path)
{
    DIR *d = opendir(path);
    if (!d) return NULL;
    sw_dir *dir = sw_malloc(sizeof(sw_dir));
    if (!dir) {
        closedir(d);
        return NULL;
    }
    dir->handle = d;
    return dir;
}

bool sw_dir_next(sw_dir *d, sw_dir_entry *entry)
{
    struct dirent *de = readdir(d->handle);
    if (!de) return false;

    strncpy(entry->name, de->d_name, sizeof(entry->name) - 1);
    entry->name[sizeof(entry->name) - 1] = '\0';
    entry->is_dir = (de->d_type == DT_DIR);
    entry->is_file = (de->d_type == DT_REG);
    entry->is_symlink = (de->d_type == DT_LNK);
    return true;
}

void sw_dir_close(sw_dir *d)
{
    if (!d) return;
    closedir(d->handle);
    sw_free(d);
}

/* ========== Monotonic time ========== */

uint64_t sw_time_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* ========== Sleep ========== */

void sw_sleep_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
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

#if defined(__linux__)

bool sw_filesystem_is_remote(const char *path)
{
    struct statfs buf;
    if (statfs(path, &buf) != 0)
        return false;
    switch ((unsigned long)buf.f_type) {
    case 0x6969:       /* NFS */
    case 0xFF534D42:   /* CIFS */
    case 0xFE534D42:   /* SMB2 */
    case 0x65735546:   /* FUSE */
    case 0x5346414F:   /* AFS */
    case 0x564C:       /* NCP */
    case 0x6F636673:   /* OCFS2 */
    case 0x01021997:   /* 9P/WSL */
        return true;
    default:
        return false;
    }
}

#else /* macOS / BSD */

bool sw_filesystem_is_remote(const char *path)
{
    struct statfs buf;
    if (statfs(path, &buf) != 0)
        return false;
    const char *remote_types[] = { "nfs", "smbfs", "afpfs", "webdav", "cifs", NULL };
    for (int i = 0; remote_types[i]; i++) {
        if (strcmp(buf.f_fstypename, remote_types[i]) == 0)
            return true;
    }
    return false;
}

#endif /* linux vs macOS/BSD */

#endif /* POSIX */
