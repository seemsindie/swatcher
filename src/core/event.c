#include "swatcher.h"

SWATCHER_API const char *swatcher_event_name(swatcher_fs_event event)
{
    switch (event) {
    case SWATCHER_EVENT_CREATED:       return "SWATCHER_EVENT_CREATED";
    case SWATCHER_EVENT_MODIFIED:      return "SWATCHER_EVENT_MODIFIED";
    case SWATCHER_EVENT_DELETED:       return "SWATCHER_EVENT_DELETED";
    case SWATCHER_EVENT_MOVED:         return "SWATCHER_EVENT_MOVED";
    case SWATCHER_EVENT_OPENED:        return "SWATCHER_EVENT_OPENED";
    case SWATCHER_EVENT_CLOSED:        return "SWATCHER_EVENT_CLOSED";
    case SWATCHER_EVENT_ACCESSED:      return "SWATCHER_EVENT_ACCESSED";
    case SWATCHER_EVENT_ATTRIB_CHANGE: return "SWATCHER_EVENT_ATTRIB_CHANGE";
    case SWATCHER_EVENT_OVERFLOW:      return "SWATCHER_EVENT_OVERFLOW";
    case SWATCHER_EVENT_NONE:          return "SWATCHER_EVENT_NONE";
    case SWATCHER_EVENT_ALL:           return "SWATCHER_EVENT_ALL";
    default:                           return "Unknown event";
    }
}
