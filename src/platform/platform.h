#ifndef SWATCHER_PLATFORM_H
#define SWATCHER_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Opaque platform types */
typedef struct sw_mutex sw_mutex;
typedef struct sw_thread sw_thread;
typedef struct sw_dir sw_dir;

/* Thread function signature (pthread-compatible) */
typedef void *(*sw_thread_func)(void *arg);

/* Mutex */
sw_mutex *sw_mutex_create(void);
void sw_mutex_destroy(sw_mutex *m);
void sw_mutex_lock(sw_mutex *m);
void sw_mutex_unlock(sw_mutex *m);

/* Thread */
sw_thread *sw_thread_create(sw_thread_func func, void *arg);
void sw_thread_join(sw_thread *t);
void sw_thread_destroy(sw_thread *t);

/* Path utilities */
bool sw_path_is_absolute(const char *path);
bool sw_path_normalize(const char *input, char *output, size_t size, bool resolve_symlinks);
char sw_path_separator(void);

/* Max path size */
#ifdef _WIN32
#define SW_PATH_MAX 260
#else
#include <limits.h>
#define SW_PATH_MAX PATH_MAX
#endif

/* File info */
typedef struct sw_file_info {
    bool is_file;
    bool is_directory;
    bool is_symlink;
    uint64_t size;       /* file size in bytes */
    uint64_t mtime;      /* modification time (seconds since epoch) */
} sw_file_info;

bool sw_stat(const char *path, sw_file_info *info, bool follow_symlinks);

/* Directory iteration */
typedef struct sw_dir_entry {
    char name[256];
    bool is_dir;
    bool is_file;
    bool is_symlink;
} sw_dir_entry;

sw_dir *sw_dir_open(const char *path);
bool sw_dir_next(sw_dir *d, sw_dir_entry *entry);
void sw_dir_close(sw_dir *d);

/* Monotonic time (milliseconds) */
uint64_t sw_time_now_ms(void);

/* Sleep (milliseconds) */
void sw_sleep_ms(int ms);

/* String */
char *sw_strdup(const char *s);

#endif /* SWATCHER_PLATFORM_H */
