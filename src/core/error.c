#include "swatcher.h"
#include "error.h"

/* Thread-local error code */
#if defined(_MSC_VER)
  __declspec(thread) static swatcher_error sw_last_error = SWATCHER_OK;
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
  _Thread_local static swatcher_error sw_last_error = SWATCHER_OK;
#else
  __thread static swatcher_error sw_last_error = SWATCHER_OK;
#endif

void sw_set_error(swatcher_error err) { sw_last_error = err; }

SWATCHER_API swatcher_error swatcher_last_error(void)
{
    swatcher_error e = sw_last_error;
    sw_last_error = SWATCHER_OK;
    return e;
}

SWATCHER_API const char *swatcher_error_string(swatcher_error err)
{
    switch (err) {
    case SWATCHER_OK:                    return "No error";
    case SWATCHER_ERR_NULL_ARG:          return "NULL argument";
    case SWATCHER_ERR_ALLOC:             return "Memory allocation failed";
    case SWATCHER_ERR_INVALID_PATH:      return "Invalid path";
    case SWATCHER_ERR_PATH_NOT_FOUND:    return "Path not found";
    case SWATCHER_ERR_BACKEND_INIT:      return "Backend initialization failed";
    case SWATCHER_ERR_BACKEND_NOT_FOUND: return "Backend not found";
    case SWATCHER_ERR_THREAD:            return "Thread creation failed";
    case SWATCHER_ERR_MUTEX:             return "Mutex creation failed";
    case SWATCHER_ERR_NOT_INITIALIZED:   return "Watcher not initialized";
    case SWATCHER_ERR_TARGET_EXISTS:     return "Target already watched";
    case SWATCHER_ERR_TARGET_NOT_FOUND:  return "Target not found";
    case SWATCHER_ERR_PATTERN_COMPILE:   return "Pattern compilation failed";
    case SWATCHER_ERR_WATCH_LIMIT:       return "Watch limit exceeded";
    case SWATCHER_ERR_UNKNOWN:           return "Unknown error";
    default:                             return "Unknown error";
    }
}
