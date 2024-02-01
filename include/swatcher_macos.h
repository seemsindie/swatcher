#include <CoreServices/CoreServices.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>

typedef struct swatcher_macos {
    FSEventStreamRef streamRef;      // Reference to the FSEvents stream
    CFRunLoopRef runLoop;            // Reference to the run loop
    pthread_t thread;                // Thread for the FSEvents processing
    pthread_mutex_t mutex;           // Mutex for thread synchronization
    bool running;                    // Flag to control the running state
} swatcher_macos;

bool swatcher_is_absolute_path(const char *path)
{
    if (path == NULL)
    {
        return false;
    }
    return path[0] == '/';
}

bool swatcher_validate_and_normalize_path(const char *input_path, char *normalized_path)
{
    if (!swatcher_is_absolute_path(input_path))
    {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd)) != NULL)
        {
            snprintf(normalized_path, PATH_MAX, "%s/%s", cwd, input_path);
        }
        else
        {
            SWATCHER_LOG_DEFAULT_ERROR("Failed to get current working directory");
            return false;
        }
    }
    else
    {
        if (realpath(input_path, normalized_path) == NULL)
        {
            SWATCHER_LOG_DEFAULT_ERROR("Failed to get real path for %s", input_path);
            return false;
        }
    }
    return true;
}

bool swatcher_init(swatcher *swatcher, swatcher_config *config)
{
    if (!swatcher)
    {
        SWATCHER_LOG_DEFAULT_ERROR("swatcher is NULL\n");
        return false;
    }

    if (!config)
    {
        SWATCHER_LOG_DEFAULT_ERROR("config is NULL\n");
        return false;
    }

    // Allocate memory for swatcher_macos structure
    swatcher->platform_data = malloc(sizeof(swatcher_macos));
    if (!swatcher->platform_data)
    {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to allocate memory for swatcher_macos\n");
        return false;
    }

    swatcher_macos *sw_mac = (swatcher_macos *)swatcher->platform_data;
    sw_mac->running = false;
    sw_mac->streamRef = NULL; // No stream yet
    sw_mac->runLoop = NULL;   // No run loop yet

    // Initialize mutex
    if (pthread_mutex_init(&sw_mac->mutex, NULL) != 0)
    {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to initialize mutex");
        free(swatcher->platform_data);
        return false;
    }

    swatcher->targets_head = NULL;
    swatcher->config = config;

    return true;
}

swatcher_target *swatcher_target_create(swatcher_target_desc *desc)
{
    if (desc == NULL)
    {
        SWATCHER_LOG_DEFAULT_ERROR("Descriptor is NULL");
        return NULL;
    }

    swatcher_target *target = malloc(sizeof(swatcher_target));
    if (target == NULL)
    {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to allocate swatcher_target");
        return NULL;
    }

    // Normalize the path
    char normalized_path[PATH_MAX];
    if (!swatcher_validate_and_normalize_path(desc->path, normalized_path))
    {
        free(target);
        return NULL;
    }

    // Check if the path is a file, directory, or symlink
    struct stat path_stat;
    if ((desc->follow_symlinks ? stat : lstat)(normalized_path, &path_stat) != 0)
    {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to stat path: %s", normalized_path);
        free(target);
        return NULL;
    }

    target->is_file = S_ISREG(path_stat.st_mode);
    target->is_directory = S_ISDIR(path_stat.st_mode);
    target->is_symlink = S_ISLNK(path_stat.st_mode);

    // Duplicate the normalized path
    target->path = strdup(normalized_path);
    if (target->path == NULL)
    {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to allocate path (string)");
        free(target);
        return NULL;
    }

    // Duplicate the pattern if it's provided
    target->pattern = desc->pattern ? strdup(desc->pattern) : NULL;
    if (desc->pattern && target->pattern == NULL)
    {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to allocate pattern (regex string)");
        free(target->path);
        free(target);
        return NULL;
    }

    // Set the other fields
    target->is_recursive = desc->is_recursive;
    target->events = desc->events;
    target->user_data = desc->user_data;
    target->callback = desc->callback;
    target->last_event_time = time(NULL);

    // SWATCHER_LOG_DEFAULT_DEBUG("Created target: %s", target->path);

    return target;
}

bool swatcher_start(swatcher *swatcher)
{
    if (!swatcher || !swatcher->platform_data) {
        fprintf(stderr, "swatcher not initialized\n");
        return false;
    }

    swatcher_macos *sw_mac = (swatcher_macos *)swatcher->platform_data;

    // Create and start the watcher thread
    int ret = pthread_create(&sw_mac->thread, NULL, swatcher_watcher_thread, swatcher);
    if (ret != 0) {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to create watcher thread");
        return false;
    }

    return true;
}

void swatcher_stop(swatcher *swatcher) {
    if (!swatcher || !swatcher->platform_data) {
        return;
    }

    swatcher_macos *sw_mac = (swatcher_macos *)swatcher->platform_data;
    sw_mac->running = false;
    CFRunLoopStop(CFRunLoopGetCurrent());
    pthread_join(sw_mac->thread, NULL);
}

void fsevents_callback(ConstFSEventStreamRef streamRef, void *userData, size_t numEvents, void *eventPaths, const FSEventStreamEventFlags eventFlags[], const FSEventStreamEventId eventIds[]) {
    swatcher *swatcher = (swatcher *)userData;
    swatcher_macos *sw_mac = (swatcher_macos *)swatcher->platform_data;

    // Lock the mutex
    pthread_mutex_lock(&sw_mac->mutex);

    // Iterate over the events
    char **paths = eventPaths;
    for (size_t i = 0; i < numEvents; i++) {
        // Get the path
        char *path = paths[i];

        // Get the target
        swatcher_target *target = swatcher->targets_head;
        while (target) {
            // Check if the path matches the target
            // if (swatcher_target_matches_path(target, path)) {
            //     // Check if the event is allowed
            //     switcher_fs_event sw_event = swatcher_target_get_event(target, eventFlags[i]);
            //     if (swatcher_target_is_event_allowed(target, eventFlags[i])) {
            //         // Check if the event is allowed by the filter
            //         if (swatcher_target_is_event_allowed_by_filter(target, path)) {
            //             // Call the callback
            //             target->callback(target, path, target->user_data);
            //         }
            //     }
            // }

            // Get the next target
            target = target->next;
        }
    }

    // Unlock the mutex
    pthread_mutex_unlock(&sw_mac->mutex);
}

void *swatcher_watcher_thread(void *arg)
{
    swatcher *swatcher = (swatcher *)arg;
    swatcher_macos *sw_mac = (swatcher_macos *)swatcher->platform_data;

    CFStringRef path = CFStringCreateWithCString(NULL, swatcher->config->path, kCFStringEncodingUTF8);
    CFArrayRef pathsToWatch = CFArrayCreate(NULL, (const void **)&path, 1, NULL);
    FSEventStreamContext context = {0, swatcher, NULL, NULL, NULL};
    CFAbsoluteTime latency = swatcher->config->latency;
    FSEventStreamRef stream = FSEventStreamCreate(NULL, fsevents_callback, &context, pathsToWatch, kFSEventStreamEventIdSinceNow, latency, kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagUseCFTypes);

    CFRunLoopRef runLoop = CFRunLoopGetCurrent();
    FSEventStreamScheduleWithRunLoop(stream, runLoop, kCFRunLoopDefaultMode);

    FSEventStreamStart(stream);

    sw_mac->running = true;

    while (sw_mac->running) {
        CFRunLoopRun();
    }

    FSEventStreamStop(stream);
    FSEventStreamInvalidate(stream);
    FSEventStreamRelease(stream);

    CFRelease(pathsToWatch);
    CFRelease(path);

    sw_mac->running = false;

    return NULL;
}

// bool swatcher_add(swatcher *swatcher, swatcher_target *target);
// linux
bool swatcher_add(swatcher *swatcher, swatcher_target *target)
{

    bool result = false;

    if (!target->is_file && target->is_recursive)
    {
        result = add_watch_recursive(swatcher, target, true);
    }
    else
    {
        pthread_mutex_lock(&((swatcher_linux *)swatcher->platform_data)->mutex);
        result = add_watch(swatcher, target);
        pthread_mutex_unlock(&((swatcher_linux *)swatcher->platform_data)->mutex);
    }

    return result;
}
