#if defined(__linux__) || defined(__unix__) || defined(__unix) || defined(unix) || defined(__APPLE__)

#include "platform.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <dirent.h>
#include <limits.h>
#include <stdio.h>

/* ========== Mutex ========== */

struct sw_mutex {
    pthread_mutex_t mtx;
};

sw_mutex *sw_mutex_create(void)
{
    sw_mutex *m = malloc(sizeof(sw_mutex));
    if (!m) return NULL;
    if (pthread_mutex_init(&m->mtx, NULL) != 0) {
        free(m);
        return NULL;
    }
    return m;
}

void sw_mutex_destroy(sw_mutex *m)
{
    if (!m) return;
    pthread_mutex_destroy(&m->mtx);
    free(m);
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
    sw_thread *t = malloc(sizeof(sw_thread));
    if (!t) return NULL;
    if (pthread_create(&t->handle, NULL, func, arg) != 0) {
        free(t);
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
    free(t);
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
    sw_dir *dir = malloc(sizeof(sw_dir));
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
    free(d);
}

/* ========== String ========== */

char *sw_strdup(const char *s)
{
    return strdup(s);
}

#endif /* POSIX */
