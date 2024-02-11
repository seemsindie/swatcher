#define _CRTDBG_MAP_ALLOC
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>

#include "uthash.h"
// #include "utarray.h"

#define CURRENT_LOG_LEVEL LOG_DEBUG
#define REGEX_PATTERNS(...) ((char *[]){__VA_ARGS__, NULL})

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

// ANSI color codes
#define COLOR_RED "\x1b[31m"
#define COLOR_YELLOW "\x1b[33m"
#define COLOR_BLUE "\x1b[34m"
#define COLOR_MAGENTA "\x1b[35m"
#define COLOR_RESET "\x1b[0m"

#define SWATCHER_LOG_ERROR(sw, fmt, ...) swatcher_log(sw, LOG_ERROR, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define SWATCHER_LOG_WARNING(sw, fmt, ...) swatcher_log(sw, LOG_WARNING, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define SWATCHER_LOG_INFO(sw, fmt, ...) swatcher_log(sw, LOG_INFO, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define SWATCHER_LOG_DEBUG(sw, fmt, ...) swatcher_log(sw, LOG_DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define SWATCHER_LOG_DEFAULT_ERROR(fmt, ...) swatcher_log_default(LOG_ERROR, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define SWATCHER_LOG_DEFAULT_WARNING(fmt, ...) swatcher_log_default(LOG_WARNING, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define SWATCHER_LOG_DEFAULT_INFO(fmt, ...) swatcher_log_default(LOG_INFO, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define SWATCHER_LOG_DEFAULT_DEBUG(fmt, ...) swatcher_log_default(LOG_DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#ifdef __cplusplus
extern "C"
{
#endif

    typedef enum swatcher_fs_event
    {
        SWATCHER_EVENT_NONE = 0,
        SWATCHER_EVENT_CREATED = 1 << 0,
        SWATCHER_EVENT_MODIFIED = 1 << 1,
        SWATCHER_EVENT_DELETED = 1 << 2,
        SWATCHER_EVENT_MOVED = 1 << 3,
        SWATCHER_EVENT_OPENED = 1 << 4,
        SWATCHER_EVENT_CLOSED = 1 << 5,
        SWATCHER_EVENT_ACCESSED = 1 << 6,
        SWATCHER_EVENT_ATTRIB_CHANGE = 1 << 7,

        SWATCHER_EVENT_ALL = -2,
        SWATCHER_EVENT_ALL_INOTIFY = -3
    } swatcher_fs_event;

    typedef enum swatcher_watch_option
    {
        SWATCHER_WATCH_ALL = 0,
        SWATCHER_WATCH_FILES = 1 << 0,
        SWATCHER_WATCH_DIRECTORIES = 1 << 1,
        SWATCHER_WATCH_SYMLINKS = 1 << 2

    } swatcher_watch_option;

    typedef struct swatcher_target
    {
        char *path;
        char *pattern;            // regex (depricated)
        char **callback_patterns; // regex patterns for callback triggering
        char **watch_patterns;    // regex patterns for watched items
        char **ignore_patterns;   // regex patterns for items to be ignored
        bool is_recursive;
        swatcher_fs_event events; // Bitmask of swatcher_fs_event values
        swatcher_watch_option watch_options;
        void *user_data;     // User data for callback usage
        void *platform_data; // For internal use by the library
        time_t last_event_time;

        UT_hash_handle hh_global; // internal use
        UT_hash_handle hh_inner;  // internal use
        struct swatcher_target *inner_targets;

        bool is_symlink;
        bool is_file;
        bool is_directory;
        bool follow_symlinks;

        void (*callback)(swatcher_fs_event, struct swatcher_target *, const char *event_name, void *additional_data);
    } swatcher_target;

    typedef struct swatcher_target_desc
    {
        char *path;
        bool is_recursive;
        // uint32_t events;
        swatcher_fs_event events;
        swatcher_watch_option watch_options;
        char *pattern;            // regex (depricated)
        char **callback_patterns; // regex patterns for callback triggering
        char **watch_patterns;    // regex patterns for watched items
        char **ignore_patterns;   // regex patterns for items to be ignored
        void *user_data;
        bool follow_symlinks;

        void (*callback)(swatcher_fs_event, struct swatcher_target *, const char *event_name, void *additional_data);
    } swatcher_target_desc;

    typedef struct swatcher_config
    {
        int poll_interval_ms;
        bool enable_logging;
    } swatcher_config;

    typedef struct swatcher
    {
        bool running;
        swatcher_target *targets;
        swatcher_config *config;

        void *platform_data; // For internal use by the library
    } swatcher;

    typedef enum
    {
        LOG_ERROR,
        LOG_WARNING,
        LOG_INFO,
        LOG_DEBUG
    } swatcher_log_level;

    const char *get_log_level_color(swatcher_log_level level)
    {
        switch (level)
        {
        case LOG_ERROR:
            return COLOR_RED;
        case LOG_WARNING:
            return COLOR_YELLOW;
        case LOG_INFO:
            return COLOR_BLUE;
        case LOG_DEBUG:
            return COLOR_MAGENTA;
        default:
            return COLOR_RESET;
        }
    }

    void swatcher_log(swatcher *sw, swatcher_log_level level, const char *file, int line, const char *format, ...)
    {
        if (!sw->config->enable_logging || level > CURRENT_LOG_LEVEL)
            return;

        const char *color = get_log_level_color(level);
        fprintf(stderr, "%s", color); // Set the color

        // Print timestamp, file and line
        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        // fprintf(stderr, "[%02d:%02d:%02d %s:%d] ", t->tm_hour, t->tm_min, t->tm_sec, file, line);
        // fprintf(stderr, "[%02d:%02d:%02d %s:%d] ", t->tm_hour, t->tm_min, t->tm_sec, file, line);
        fprintf(stderr, "[%02d:%02d:%02d] ", t->tm_hour, t->tm_min, t->tm_sec);

        va_list args;
        va_start(args, format);
        vfprintf(stderr, format, args);
        va_end(args);

        if (level == LOG_ERROR && errno != 0)
        {
            fprintf(stderr, ": %s", strerror(errno));
        }

        fprintf(stderr, "%s\n", COLOR_RESET); // Reset to default color and add new line
    }

    void swatcher_log_default(swatcher_log_level level, const char *file, int line, const char *format, ...)
    {
        if (level > CURRENT_LOG_LEVEL)
            return;

        const char *color = get_log_level_color(level);
        fprintf(stderr, "%s", color); // Set the color

        // Print timestamp, file, and line
        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        // fprintf(stderr, "[%02d:%02d:%02d %s:%d] ", t->tm_hour, t->tm_min, t->tm_sec, file, line);
        fprintf(stderr, "[%02d:%02d:%02d %s:%d] ", t->tm_hour, t->tm_min, t->tm_sec, file, line);
        // fprintf(stderr, "[%02d:%02d:%02d] ", t->tm_hour, t->tm_min, t->tm_sec);

        va_list args;
        va_start(args, format);
        vfprintf(stderr, format, args);
        va_end(args);

        if (level == LOG_ERROR && errno != 0)
        {
            fprintf(stderr, ": %s", strerror(errno));
        }

        fprintf(stderr, "%s\n", COLOR_RESET); // Reset to default color and add new line
    }

    SWATCHER_API const char *get_event_name(swatcher_fs_event event)
    {
        switch (event)
        {
        case SWATCHER_EVENT_CREATED:
            return "SWATCHER_EVENT_CREATED";
        case SWATCHER_EVENT_MODIFIED:
            return "SWATCHER_EVENT_MODIFIED";
        case SWATCHER_EVENT_DELETED:
            return "SWATCHER_EVENT_DELETED";
        case SWATCHER_EVENT_MOVED:
            return "SWATCHER_EVENT_MOVED";
        case SWATCHER_EVENT_OPENED:
            return "SWATCHER_EVENT_OPENED";
        case SWATCHER_EVENT_CLOSED:
            return "SWATCHER_EVENT_CLOSED";
        case SWATCHER_EVENT_ACCESSED:
            return "SWATCHER_EVENT_ACCESSED";
        case SWATCHER_EVENT_ATTRIB_CHANGE:
            return "SWATCHER_EVENT_ATTRIB_CHANGE";
        case SWATCHER_EVENT_NONE:
            return "SWATCHER_EVENT_NONE";
        case SWATCHER_EVENT_ALL:
            return "SWATCHER_EVENT_ALL";
        case SWATCHER_EVENT_ALL_INOTIFY:
            return "SWATCHER_EVENT_ALL_INOTIFY";
        default:
            return "Unknown event";
        }
    }

    SWATCHER_API bool is_already_watched(swatcher *sw, const char *path)
    {
        swatcher_target *target = NULL;
        HASH_FIND(hh_global, sw->targets, path, strlen(path), target);
        return target != NULL;
    }

    SWATCHER_API bool swatcher_start(swatcher *);
    SWATCHER_API bool swatcher_init(swatcher *, swatcher_config *);
    SWATCHER_API void swatcher_stop(swatcher *);
    SWATCHER_API void swatcher_cleanup(swatcher *);
    bool swatcher_is_absolute_path(const char *);
    bool swatcher_validate_and_normalize_path(const char *, char *, bool);

    SWATCHER_API swatcher_target *swatcher_target_create(swatcher_target_desc *);

    SWATCHER_API bool swatcher_add(swatcher *, swatcher_target *);
    SWATCHER_API bool swatcher_remove(swatcher *, swatcher_target *);

#ifdef __cplusplus
} /* extern "C" */
#endif