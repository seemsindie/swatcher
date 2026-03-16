#define _CRTDBG_MAP_ALLOC
// #define __cplusplus
#include <stdio.h>
#include <swatcher.h>
#include <signal.h>
#ifdef _WIN32
// #include <crtdbg.h>
#include <conio.h>
#endif

static swatcher *global_watcher = NULL;

void my_callback_function(swatcher_fs_event event, swatcher_target *target, const char *eventname, void *additional_data)
{
    // SWATCHER_LOG_DEFAULT_INFO("Event: %s", get_event_name(event));
    // SWATCHER_LOG_DEFAULT_INFO("Target path: %s", target->path);
    // // printf("Target is_file: %s\n", target->is_file ? "true" : "false"); // might give "false" indications if file is created in a watched directory it will be false but that's becasuse that target is not but a file is created...i don't give a fuck...
    // if (eventname != NULL)
    // {
    //     SWATCHER_LOG_DEFAULT_INFO("Event name: %s", eventname);
    // }
    // else
    // {
    //     if (target->is_file)
    //     {
    //         SWATCHER_LOG_DEFAULT_INFO("Event name: file itself");
    //     }
    //     else
    //     {
    //         SWATCHER_LOG_DEFAULT_INFO("Event name: Directory itself");
    //     }
    // }
    // if (eventname != NULL || target->is_file) {
        SWATCHER_LOG_DEFAULT_INFO("Event: %s", get_event_name(event));
        SWATCHER_LOG_DEFAULT_INFO("Target path: %s", target->path);
        SWATCHER_LOG_DEFAULT_INFO("Event name: %s", eventname);
        // struct inotify_event *ievent = (struct inotify_event *) additional_data;

        // SWATCHER_LOG_DEFAULT_INFO("inotify event name: %s", ievent->name);
        // SWATCHER_LOG_DEFAULT_INFO("inotify event mask: %d", ievent->mask);
    // }
}

int main()
{
    // swatcher *watcher = malloc(sizeof(swatcher));
    global_watcher = malloc(sizeof(swatcher));
    // char *path = "/home/timelord/projects/swatcher/examples/test";
    // char *path = "/home/timelord/projects/swatcher/examples/editorconfig";
    // char *path = "../examples/test/";
    char *path = "C:\\Users\\seems\\swatcher\\examples\\test";
    // char *path = "/home/timelord/projects/work/host-staffing/host-staffing-ui";
    // char *path = "/home/timelord/projects/work/host-staffing/host-staffing-ui/node_modules/editorconfig/node_modules";
    swatcher_config config = {
        .poll_interval_ms = 50,
        .enable_logging = true};

    if (!global_watcher)
    {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to allocate swatcher");
        return EXIT_FAILURE;
    }

    if (!swatcher_init(global_watcher, &config))
    {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to initialize swatcher");

        free(global_watcher);

        return EXIT_FAILURE;
    }

    swatcher_target *target = swatcher_target_create(&(swatcher_target_desc){
        .path = path,
        .is_recursive = false,
        .events = SWATCHER_EVENT_DELETED | SWATCHER_EVENT_CREATED | SWATCHER_EVENT_MODIFIED | SWATCHER_EVENT_MOVED,
        // .callback_patterns = REGEX_PATTERNS(".*\\.txt$", ".*\\.c$", ".*\\.h$"),
        // .watch_patterns = REGEX_PATTERNS(".*\\.txt$"),
        // .ignore_patterns = REGEX_PATTERNS(".*\\.txt$"),
        // .watch_options = SWATCHER_WATCH_ALL,
        // .follow_symlinks = false,
        .user_data = "Hello, world (user data)!",
        .callback = my_callback_function});
    
    swatcher_add(global_watcher, target);
    swatcher_start(global_watcher);

    // getchar();
    #ifdef _WIN32
    _getch(); // Use _getch() on Windows
    #else
    getchar(); // Use getchar() on Linux
    #endif

    SWATCHER_LOG_DEFAULT_INFO("Removing swatcher...");

    swatcher_remove(global_watcher, target);
    SWATCHER_LOG_DEFAULT_INFO("Stopping swatcher...");
    swatcher_stop(global_watcher);

    SWATCHER_LOG_DEFAULT_INFO("Cleaning up swatcher...");
    swatcher_cleanup(global_watcher);

    // _CrtDumpMemoryLeaks();

    return EXIT_SUCCESS;
}
