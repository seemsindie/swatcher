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

/* ---------- Core API ---------- */
SWATCHER_API bool swatcher_init(swatcher *sw, swatcher_config *config);
SWATCHER_API bool swatcher_start(swatcher *sw);
SWATCHER_API void swatcher_stop(swatcher *sw);
SWATCHER_API void swatcher_cleanup(swatcher *sw);

/* ---------- Target API ---------- */
SWATCHER_API swatcher_target *swatcher_target_create(swatcher_target_desc *desc);
SWATCHER_API void swatcher_target_destroy(swatcher_target *target);

/* ---------- Watch management ---------- */
SWATCHER_API bool swatcher_add(swatcher *sw, swatcher_target *target);
SWATCHER_API bool swatcher_remove(swatcher *sw, swatcher_target *target);

/* ---------- Utilities ---------- */
SWATCHER_API const char *swatcher_event_name(swatcher_fs_event event);
SWATCHER_API bool swatcher_is_watched(swatcher *sw, const char *path);

/* ---------- Error reporting ---------- */
SWATCHER_API swatcher_error  swatcher_last_error(void);
SWATCHER_API const char     *swatcher_error_string(swatcher_error err);

/* ---------- Backend selection ---------- */
SWATCHER_API bool         swatcher_init_with_backend(swatcher *sw, swatcher_config *config, const char *backend_name);
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
