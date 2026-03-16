#include "swatcher.h"

#include <errno.h>

static const char *get_log_level_color(swatcher_log_level level)
{
    switch (level) {
    case SW_LOG_ERROR:   return COLOR_RED;
    case SW_LOG_WARNING: return COLOR_YELLOW;
    case SW_LOG_INFO:    return COLOR_BLUE;
    case SW_LOG_DEBUG:   return COLOR_MAGENTA;
    default:             return COLOR_RESET;
    }
}

void swatcher_log(swatcher *sw, swatcher_log_level level, const char *file, int line, const char *format, ...)
{
    (void)file;
    (void)line;

    if (!sw->config->enable_logging || level > CURRENT_LOG_LEVEL)
        return;

    const char *color = get_log_level_color(level);
    fprintf(stderr, "%s", color);

    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    fprintf(stderr, "[%02d:%02d:%02d] ", t->tm_hour, t->tm_min, t->tm_sec);

    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);

    if (level == SW_LOG_ERROR && errno != 0) {
        fprintf(stderr, ": %s", strerror(errno));
    }

    fprintf(stderr, "%s\n", COLOR_RESET);
}

void swatcher_log_default(swatcher_log_level level, const char *file, int line, const char *format, ...)
{
    if (level > CURRENT_LOG_LEVEL)
        return;

    const char *color = get_log_level_color(level);
    fprintf(stderr, "%s", color);

    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    fprintf(stderr, "[%02d:%02d:%02d %s:%d] ", t->tm_hour, t->tm_min, t->tm_sec, file, line);

    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);

    if (level == SW_LOG_ERROR && errno != 0) {
        fprintf(stderr, ": %s", strerror(errno));
    }

    fprintf(stderr, "%s\n", COLOR_RESET);
}
