#ifndef SWATCHER_TYPES_H
#define SWATCHER_TYPES_H

#include "swatcher_version.h"
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

/** @brief File system event types (bitmask). */
typedef enum swatcher_fs_event {
    SWATCHER_EVENT_NONE          = 0,
    SWATCHER_EVENT_CREATED       = 1 << 0,  /**< File or directory created. */
    SWATCHER_EVENT_MODIFIED      = 1 << 1,  /**< File contents changed. */
    SWATCHER_EVENT_DELETED       = 1 << 2,  /**< File or directory deleted. */
    SWATCHER_EVENT_MOVED         = 1 << 3,  /**< File or directory renamed/moved. */
    SWATCHER_EVENT_OPENED        = 1 << 4,  /**< File opened (inotify only). */
    SWATCHER_EVENT_CLOSED        = 1 << 5,  /**< File closed (inotify only). */
    SWATCHER_EVENT_ACCESSED      = 1 << 6,  /**< File accessed (inotify only). */
    SWATCHER_EVENT_ATTRIB_CHANGE = 1 << 7,  /**< File attributes changed. */

    SWATCHER_EVENT_ALL           = 0xFF,     /**< All event types. */

    /** @brief Meta-event: kernel event queue overflowed. Always delivered. */
    SWATCHER_EVENT_OVERFLOW      = 1 << 8
} swatcher_fs_event;

/** @brief Filter which filesystem entry types to watch (bitmask). */
typedef enum swatcher_watch_option {
    SWATCHER_WATCH_ALL         = 0,        /**< Watch files, directories, and symlinks. */
    SWATCHER_WATCH_FILES       = 1 << 0,   /**< Watch files only. */
    SWATCHER_WATCH_DIRECTORIES = 1 << 1,   /**< Watch directories only. */
    SWATCHER_WATCH_SYMLINKS    = 1 << 2    /**< Watch symlinks only. */
} swatcher_watch_option;

/** @brief Log severity levels. */
typedef enum swatcher_log_level {
    SW_LOG_ERROR,
    SW_LOG_WARNING,
    SW_LOG_INFO,
    SW_LOG_DEBUG
} swatcher_log_level;

/** @brief Error codes returned by swatcher_last_error(). */
typedef enum swatcher_error {
    SWATCHER_OK = 0,                /**< No error. */
    SWATCHER_ERR_NULL_ARG,          /**< A required argument was NULL. */
    SWATCHER_ERR_ALLOC,             /**< Memory allocation failed. */
    SWATCHER_ERR_INVALID_PATH,      /**< Path is invalid or empty. */
    SWATCHER_ERR_PATH_NOT_FOUND,    /**< Path does not exist. */
    SWATCHER_ERR_BACKEND_INIT,      /**< Backend initialization failed. */
    SWATCHER_ERR_BACKEND_NOT_FOUND, /**< Named backend not available on this platform. */
    SWATCHER_ERR_THREAD,            /**< Thread creation failed. */
    SWATCHER_ERR_MUTEX,             /**< Mutex creation failed. */
    SWATCHER_ERR_NOT_INITIALIZED,   /**< Watcher not initialized. */
    SWATCHER_ERR_TARGET_EXISTS,     /**< Target path already watched. */
    SWATCHER_ERR_TARGET_NOT_FOUND,  /**< Target not found. */
    SWATCHER_ERR_PATTERN_COMPILE,   /**< Regex/glob pattern failed to compile. */
    SWATCHER_ERR_WATCH_LIMIT,       /**< OS watch limit reached. */
    SWATCHER_ERR_UNKNOWN            /**< Unknown error. */
} swatcher_error;

/** @brief Custom allocator. All fields may be NULL to use stdlib defaults. */
typedef struct swatcher_allocator {
    void *(*malloc)(size_t size, void *ctx);
    void *(*realloc)(void *ptr, size_t size, void *ctx);
    void  (*free)(void *ptr, void *ctx);
    void *ctx;
} swatcher_allocator;

typedef struct swatcher_target swatcher_target;

/**
 * @brief Extended event information passed via additional_data.
 *
 * For SWATCHER_EVENT_MOVED, old_path contains the previous path (if known).
 * is_dir is true when the changed entry is a directory.
 * Users who ignore additional_data are unaffected (backward compatible).
 */
typedef struct swatcher_event_info {
    const char *old_path; /**< Previous path on rename/move, or NULL if unknown. */
    bool is_dir;          /**< true if the changed entry is a directory. */
} swatcher_event_info;

/**
 * @brief Callback invoked when a file system event occurs.
 * @param event          The event type (bitmask).
 * @param target         The target that triggered the event.
 * @param event_name     The file/directory name that changed (may be NULL).
 * @param additional_data  Pointer to swatcher_event_info (may be NULL for overflow).
 */
typedef void (*swatcher_callback_fn)(swatcher_fs_event event, swatcher_target *target,
                                     const char *event_name, void *additional_data);

/** @brief A watched target (file or directory). Created via swatcher_target_create(). */
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

/**
 * @brief Descriptor for creating a watch target.
 *
 * Use with designated initializers and pass to swatcher_target_create().
 */
typedef struct swatcher_target_desc {
    char *path;                          /**< Path to watch (file or directory). */
    bool is_recursive;                   /**< Watch subdirectories recursively. */
    swatcher_fs_event events;            /**< Event types to listen for. */
    swatcher_watch_option watch_options; /**< Filter by entry type. */
    char **callback_patterns;            /**< Glob/regex patterns — only matching names trigger callback. */
    char **watch_patterns;               /**< Glob/regex patterns — only watch matching entries. */
    char **ignore_patterns;              /**< Glob/regex patterns — skip matching entries. */
    void *user_data;                     /**< Passed to callback as additional_data. */
    bool follow_symlinks;                /**< Follow symlinks to their targets. */

    swatcher_callback_fn callback;       /**< Event callback function. */
} swatcher_target_desc;

/**
 * @brief Watcher configuration.
 *
 * Zero-initialize for defaults, or set fields as needed.
 */
typedef struct swatcher_config {
    int poll_interval_ms;          /**< Poll interval in milliseconds (poll backend). */
    bool enable_logging;           /**< Enable internal logging to stderr. */
    int coalesce_ms;               /**< Event coalescing window in ms (0 = disabled). */
    swatcher_allocator *allocator; /**< Custom allocator, or NULL for stdlib. */
    bool overflow_rescan;          /**< Re-scan dirs on overflow (default: false). */
    bool vcs_aware;                /**< Pause events during VCS operations (default: false). */
} swatcher_config;

/** @brief Opaque watcher handle. */
typedef struct swatcher {
    sw_atomic_bool running;
    swatcher_config *config;
    void *_internal;
} swatcher;

#ifdef __cplusplus
}
#endif

#endif /* SWATCHER_TYPES_H */
