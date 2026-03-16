#include "swatcher.h"

#if defined(__linux__) || defined(__unix__) || defined(__unix) || defined(unix) || defined(__APPLE__)
#include <regex.h>

bool is_pattern_matched(char **patterns, const char *string)
{
    regex_t regex;
    bool matched = false;

    for (size_t i = 0; patterns[i] != NULL; i++) {
        int ret = regcomp(&regex, patterns[i], REG_EXTENDED);
        if (ret) {
            SWATCHER_LOG_DEFAULT_ERROR("Failed to compile regex: %s", patterns[i]);
            return false;
        }

        ret = regexec(&regex, string, 0, NULL, 0);
        regfree(&regex);

        if (ret == 0) {
            matched = true;
            break;
        }
    }

    return matched;
}

#else /* Windows: stub — always returns true for now */

bool is_pattern_matched(char **patterns, const char *string)
{
    (void)patterns;
    (void)string;
    return true;
}

#endif
