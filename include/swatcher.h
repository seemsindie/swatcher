#include "swatcher_common.h"

#if defined(__linux__) || defined(__unix__) || defined(__unix) || defined(unix)
 #include "swatcher_linux.h"
#elif defined(_WIN32) || defined(_WIN64)
    #include "swatcher_windows.h"
#elif defined(__APPLE__)
    #include "swatcher_macos.h"
#endif
