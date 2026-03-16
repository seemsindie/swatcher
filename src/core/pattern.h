#ifndef SWATCHER_PATTERN_H
#define SWATCHER_PATTERN_H

#include <stdbool.h>
#include <stddef.h>
#include "../regex/re.h"

typedef struct sw_compiled_patterns {
    re_t   *patterns;  /* malloc'd array of compiled re_t */
    size_t  count;
} sw_compiled_patterns;

/* Compile a NULL-terminated char** array into pre-compiled patterns.
 * Auto-detects glob patterns and converts them to regex.
 * Returns NULL if source is NULL or empty. */
sw_compiled_patterns *sw_patterns_compile(char **source);

/* Free compiled patterns (safe to call with NULL). */
void sw_patterns_free(sw_compiled_patterns *cp);

/* Returns true if any compiled pattern matches the string.
 * Returns false if cp is NULL. */
bool sw_pattern_matched(const sw_compiled_patterns *cp, const char *string);

/* Convert a glob pattern to regex. Returns true on success.
 * Example: *.txt -> ^.*\.txt$ */
bool sw_glob_to_regex(const char *glob, char *regex_buf, size_t buf_size);

#endif /* SWATCHER_PATTERN_H */
