#ifndef SWATCHER_H
#define SWATCHER_H

#include "swatcher_types.h"

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <string.h>

/* ---------- ANSI color codes ---------- */
#define COLOR_RED     "\x1b[31m"
#define COLOR_YELLOW  "\x1b[33m"
#define COLOR_BLUE    "\x1b[34m"
#define COLOR_MAGENTA "\x1b[35m"
#define COLOR_RESET   "\x1b[0m"

/* ---------- Current log level ---------- */
#define CURRENT_LOG_LEVEL SW_LOG_DEBUG

/* ---------- Logging macros ---------- */
#define SWATCHER_LOG_ERROR(sw, fmt, ...)   swatcher_log(sw, SW_LOG_ERROR, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define SWATCHER_LOG_WARNING(sw, fmt, ...) swatcher_log(sw, SW_LOG_WARNING, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define SWATCHER_LOG_INFO(sw, fmt, ...)    swatcher_log(sw, SW_LOG_INFO, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define SWATCHER_LOG_DEBUG(sw, fmt, ...)   swatcher_log(sw, SW_LOG_DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#define SWATCHER_LOG_DEFAULT_ERROR(fmt, ...)   swatcher_log_default(SW_LOG_ERROR, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define SWATCHER_LOG_DEFAULT_WARNING(fmt, ...) swatcher_log_default(SW_LOG_WARNING, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define SWATCHER_LOG_DEFAULT_INFO(fmt, ...)    swatcher_log_default(SW_LOG_INFO, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define SWATCHER_LOG_DEFAULT_DEBUG(fmt, ...)   swatcher_log_default(SW_LOG_DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

/* ---------- Pattern helpers ---------- */
#define REGEX_PATTERNS(...) ((char *[]){__VA_ARGS__, NULL})
#define GLOB_PATTERNS(...)  ((char *[]){__VA_ARGS__, NULL})

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- Convenience API ---------- */

/**
 * @brief Create a file system watcher with the default backend.
 * @param config  Watcher configuration (poll interval, logging, coalescing).
 * @return Initialized watcher, or NULL on failure (check swatcher_last_error()).
 */
SWATCHER_API swatcher *swatcher_create(swatcher_config *config);

/**
 * @brief Create a file system watcher with a specific backend.
 * @param config        Watcher configuration.
 * @param backend_name  Backend name (e.g. "inotify", "kqueue", "poll"), or NULL for default.
 * @return Initialized watcher, or NULL on failure (check swatcher_last_error()).
 */
SWATCHER_API swatcher *swatcher_create_with_backend(swatcher_config *config, const char *backend_name);

/**
 * @brief Stop, clean up, and free a watcher created with swatcher_create().
 * @param sw  Watcher to destroy (NULL is safe).
 */
SWATCHER_API void swatcher_destroy(swatcher *sw);

/* ---------- Core API ---------- */

/**
 * @brief Initialize a caller-allocated watcher with the default backend.
 * @param sw      Pre-allocated swatcher struct.
 * @param config  Watcher configuration.
 * @return true on success, false on failure.
 */
SWATCHER_API bool swatcher_init(swatcher *sw, swatcher_config *config);

/**
 * @brief Start the watcher thread (begins monitoring).
 * @param sw  Initialized watcher.
 * @return true on success, false on failure.
 */
SWATCHER_API bool swatcher_start(swatcher *sw);

/**
 * @brief Stop the watcher thread and join it.
 * @param sw  Running watcher.
 */
SWATCHER_API void swatcher_stop(swatcher *sw);

/**
 * @brief Free all internal resources. Does not free sw itself.
 * @param sw  Stopped watcher.
 */
SWATCHER_API void swatcher_cleanup(swatcher *sw);

/* ---------- Target API ---------- */

/**
 * @brief Create a watch target from a descriptor.
 * @param desc  Target descriptor (path, events, callback, etc.).
 * @return Allocated target, or NULL on failure.
 */
SWATCHER_API swatcher_target *swatcher_target_create(swatcher_target_desc *desc);

/**
 * @brief Destroy a target that has not been added to a watcher.
 * @param target  Target to free.
 */
SWATCHER_API void swatcher_target_destroy(swatcher_target *target);

/* ---------- Watch management ---------- */

/**
 * @brief Add a target to the watcher.
 * @param sw      Initialized watcher.
 * @param target  Target to watch.
 * @return true on success, false on failure.
 */
SWATCHER_API bool swatcher_add(swatcher *sw, swatcher_target *target);

/**
 * @brief Remove and free a target from the watcher.
 * @param sw      Initialized watcher.
 * @param target  Target to remove (freed after this call).
 * @return true on success.
 */
SWATCHER_API bool swatcher_remove(swatcher *sw, swatcher_target *target);

/* ---------- Utilities ---------- */

/**
 * @brief Get a human-readable name for an event type.
 * @param event  Event type.
 * @return Static string (e.g. "CREATED", "MODIFIED").
 */
SWATCHER_API const char *swatcher_event_name(swatcher_fs_event event);

/**
 * @brief Check if a path is currently being watched.
 * @param sw    Initialized watcher.
 * @param path  Path to check.
 * @return true if the path is watched.
 */
SWATCHER_API bool swatcher_is_watched(swatcher *sw, const char *path);

/* ---------- Error reporting ---------- */

/**
 * @brief Get the last error code (thread-local).
 * @return Most recent error, or SWATCHER_OK.
 */
SWATCHER_API swatcher_error  swatcher_last_error(void);

/**
 * @brief Get a human-readable string for an error code.
 * @param err  Error code.
 * @return Static string describing the error.
 */
SWATCHER_API const char     *swatcher_error_string(swatcher_error err);

/* ---------- Backend selection ---------- */

/**
 * @brief Initialize a caller-allocated watcher with a named backend.
 * @param sw            Pre-allocated swatcher struct.
 * @param config        Watcher configuration.
 * @param backend_name  Backend name, or NULL for platform default.
 * @return true on success, false on failure.
 */
SWATCHER_API bool         swatcher_init_with_backend(swatcher *sw, swatcher_config *config, const char *backend_name);

/**
 * @brief List available backend names on this platform.
 * @return NULL-terminated array of static strings.
 */
SWATCHER_API const char **swatcher_backends_available(void);

/* ---------- Logging ---------- */
void swatcher_log(swatcher *sw, swatcher_log_level level, const char *file, int line, const char *format, ...);
void swatcher_log_default(swatcher_log_level level, const char *file, int line, const char *format, ...);

/* ---------- Compat macros ---------- */
#define get_event_name    swatcher_event_name
#define is_already_watched swatcher_is_watched

/* Compat: old log level names */
#define LOG_ERROR   SW_LOG_ERROR
#define LOG_WARNING SW_LOG_WARNING
#define LOG_INFO    SW_LOG_INFO
#define LOG_DEBUG   SW_LOG_DEBUG

#ifdef __cplusplus
}
#endif

#endif /* SWATCHER_H */
