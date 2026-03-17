#ifndef SWATCHER_TYPES_H
#define SWATCHER_TYPES_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/* Atomic bool — used for cross-thread flags like sw->running */
#if defined(SWATCHER_ZIG_COMPAT)
  /* Zig's translate-c cannot handle _Atomic — use plain volatile */
  typedef volatile int sw_atomic_bool;
  #define sw_atomic_load(p)       (*(p) != 0)
  #define sw_atomic_store(p, v)   (*(p) = (int)(v))
#elif defined(_MSC_VER)
  #include <intrin.h>
  typedef volatile long sw_atomic_bool;
  #define sw_atomic_load(p)       (_InterlockedOr((p), 0) != 0)
  #define sw_atomic_store(p, v)   _InterlockedExchange((p), (long)(v))
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
  #include <stdatomic.h>
  typedef _Atomic bool sw_atomic_bool;
  #define sw_atomic_load(p)       atomic_load(p)
  #define sw_atomic_store(p, v)   atomic_store((p), (v))
#else
  /* GCC/Clang intrinsics fallback */
  typedef volatile int sw_atomic_bool;
  #define sw_atomic_load(p)       (__sync_add_and_fetch((p), 0) != 0)
  #define sw_atomic_store(p, v)   __sync_lock_test_and_set((p), (int)(v))
#endif

#ifdef _WIN32
#ifdef SWATCHER_BUILD_DLL
#define SWATCHER_API __declspec(dllexport)
#elif defined(SWATCHER_USE_DLL)
#define SWATCHER_API __declspec(dllimport)
#else
#define SWATCHER_API
#endif
#else
#define SWATCHER_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum swatcher_fs_event {
    SWATCHER_EVENT_NONE          = 0,
    SWATCHER_EVENT_CREATED       = 1 << 0,
    SWATCHER_EVENT_MODIFIED      = 1 << 1,
    SWATCHER_EVENT_DELETED       = 1 << 2,
    SWATCHER_EVENT_MOVED         = 1 << 3,
    SWATCHER_EVENT_OPENED        = 1 << 4,
    SWATCHER_EVENT_CLOSED        = 1 << 5,
    SWATCHER_EVENT_ACCESSED      = 1 << 6,
    SWATCHER_EVENT_ATTRIB_CHANGE = 1 << 7,

    SWATCHER_EVENT_ALL           = 0xFF,

    /* Meta-event: always delivered regardless of event mask */
    SWATCHER_EVENT_OVERFLOW      = 1 << 8
} swatcher_fs_event;

typedef enum swatcher_watch_option {
    SWATCHER_WATCH_ALL         = 0,
    SWATCHER_WATCH_FILES       = 1 << 0,
    SWATCHER_WATCH_DIRECTORIES = 1 << 1,
    SWATCHER_WATCH_SYMLINKS    = 1 << 2
} swatcher_watch_option;

typedef enum swatcher_log_level {
    SW_LOG_ERROR,
    SW_LOG_WARNING,
    SW_LOG_INFO,
    SW_LOG_DEBUG
} swatcher_log_level;

typedef enum swatcher_error {
    SWATCHER_OK = 0,
    SWATCHER_ERR_NULL_ARG,
    SWATCHER_ERR_ALLOC,
    SWATCHER_ERR_INVALID_PATH,
    SWATCHER_ERR_PATH_NOT_FOUND,
    SWATCHER_ERR_BACKEND_INIT,
    SWATCHER_ERR_BACKEND_NOT_FOUND,
    SWATCHER_ERR_THREAD,
    SWATCHER_ERR_MUTEX,
    SWATCHER_ERR_NOT_INITIALIZED,
    SWATCHER_ERR_TARGET_EXISTS,
    SWATCHER_ERR_TARGET_NOT_FOUND,
    SWATCHER_ERR_PATTERN_COMPILE,
    SWATCHER_ERR_WATCH_LIMIT,
    SWATCHER_ERR_UNKNOWN
} swatcher_error;

typedef struct swatcher_target swatcher_target;

typedef void (*swatcher_callback_fn)(swatcher_fs_event event, swatcher_target *target,
                                     const char *event_name, void *additional_data);

struct swatcher_target {
    char *path;
    char **callback_patterns;
    char **watch_patterns;
    char **ignore_patterns;
    bool is_recursive;
    swatcher_fs_event events;
    swatcher_watch_option watch_options;
    void *user_data;
    time_t last_event_time;

    bool is_symlink;
    bool is_file;
    bool is_directory;
    bool follow_symlinks;

    swatcher_callback_fn callback;

    void *_internal;
};

typedef struct swatcher_target_desc {
    char *path;
    bool is_recursive;
    swatcher_fs_event events;
    swatcher_watch_option watch_options;
    char **callback_patterns;
    char **watch_patterns;
    char **ignore_patterns;
    void *user_data;
    bool follow_symlinks;

    swatcher_callback_fn callback;
} swatcher_target_desc;

typedef struct swatcher_config {
    int poll_interval_ms;
    bool enable_logging;
    int coalesce_ms;      /* event coalescing window (0 = disabled) */
} swatcher_config;

typedef struct swatcher {
    sw_atomic_bool running;
    swatcher_config *config;
    void *_internal;
} swatcher;

#ifdef __cplusplus
}
#endif

#endif /* SWATCHER_TYPES_H */
