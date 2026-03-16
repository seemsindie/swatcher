#include "pattern.h"
#include "../../include/swatcher.h"

#include <stdlib.h>
#include <string.h>

/* Heuristic: a pattern is treated as a glob if it contains '*' or '?'
 * that are NOT preceded by '\' (i.e., not regex-escaped), and does NOT
 * contain regex-specific constructs like '^', '$', '+', '[', or '\d'. */
static bool is_glob_pattern(const char *pattern)
{
    bool has_glob_meta = false;

    for (size_t i = 0; pattern[i] != '\0'; i++) {
        char c = pattern[i];

        /* If it has regex anchors or character classes, treat as regex */
        if (c == '^' || c == '$' || c == '+' || c == '[')
            return false;

        /* Backslash-escaped chars: skip next */
        if (c == '\\') {
            if (pattern[i + 1] != '\0')
                i++;
            continue;
        }

        if (c == '*' || c == '?')
            has_glob_meta = true;
    }

    return has_glob_meta;
}

bool sw_glob_to_regex(const char *glob, char *regex_buf, size_t buf_size)
{
    if (!glob || !regex_buf || buf_size < 4)
        return false;

    size_t pos = 0;
    regex_buf[pos++] = '^';

    for (size_t i = 0; glob[i] != '\0'; i++) {
        if (pos + 3 >= buf_size)
            return false;

        switch (glob[i]) {
        case '*':
            regex_buf[pos++] = '.';
            regex_buf[pos++] = '*';
            break;
        case '?':
            regex_buf[pos++] = '.';
            break;
        case '.':
            regex_buf[pos++] = '\\';
            regex_buf[pos++] = '.';
            break;
        case '\\':
            regex_buf[pos++] = '\\';
            regex_buf[pos++] = '\\';
            break;
        default:
            regex_buf[pos++] = glob[i];
            break;
        }
    }

    if (pos + 2 >= buf_size)
        return false;

    regex_buf[pos++] = '$';
    regex_buf[pos] = '\0';
    return true;
}

sw_compiled_patterns *sw_patterns_compile(char **source)
{
    if (!source || !source[0])
        return NULL;

    /* Count patterns */
    size_t count = 0;
    while (source[count] != NULL)
        count++;

    sw_compiled_patterns *cp = malloc(sizeof(sw_compiled_patterns));
    if (!cp) return NULL;

    cp->patterns = malloc(sizeof(re_t) * count);
    if (!cp->patterns) {
        free(cp);
        return NULL;
    }
    cp->count = count;

    for (size_t i = 0; i < count; i++) {
        if (is_glob_pattern(source[i])) {
            char regex_buf[512];
            if (sw_glob_to_regex(source[i], regex_buf, sizeof(regex_buf))) {
                cp->patterns[i] = re_compile(regex_buf);
            } else {
                SWATCHER_LOG_DEFAULT_WARNING("Failed to convert glob: %s", source[i]);
                cp->patterns[i] = re_compile(source[i]);
            }
        } else {
            cp->patterns[i] = re_compile(source[i]);
        }

        if (!cp->patterns[i]) {
            SWATCHER_LOG_DEFAULT_ERROR("Failed to compile pattern: %s", source[i]);
            /* Free already-compiled patterns */
            for (size_t j = 0; j < i; j++)
                re_free(cp->patterns[j]);
            free(cp->patterns);
            free(cp);
            return NULL;
        }
    }

    return cp;
}

void sw_patterns_free(sw_compiled_patterns *cp)
{
    if (!cp) return;
    for (size_t i = 0; i < cp->count; i++)
        re_free(cp->patterns[i]);
    free(cp->patterns);
    free(cp);
}

bool sw_pattern_matched(const sw_compiled_patterns *cp, const char *string)
{
    if (!cp || !string)
        return false;

    int matchlength;
    for (size_t i = 0; i < cp->count; i++) {
        if (re_matchp(cp->patterns[i], string, &matchlength) >= 0)
            return true;
    }

    return false;
}
