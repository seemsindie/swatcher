#include "swatcher_common.h"

#if defined(__linux__)
 #include "swatcher_linux.h"
#elif defined(__WIN32__)
    #include "swatcher_windows.h"
#elif defined(__APPLE__)
    #include "swatcher_macos.h"
#endif
