#include <stdio.h>
#include <swatcher.h>
#include <signal.h>
#ifdef _WIN32
#include <conio.h>
#endif

static swatcher *global_watcher = NULL;

void my_callback_function(swatcher_fs_event event, swatcher_target *target, const char *eventname, void *additional_data)
{
    (void)additional_data;
    SWATCHER_LOG_DEFAULT_INFO("Event: %s", get_event_name(event));
    SWATCHER_LOG_DEFAULT_INFO("Target path: %s", target->path);
    SWATCHER_LOG_DEFAULT_INFO("Event name: %s", eventname);
}

int main(void)
{
    global_watcher = malloc(sizeof(swatcher));
    char *path = "./examples/test";
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
        .user_data = "Hello, world (user data)!",
        .callback = my_callback_function});

    swatcher_add(global_watcher, target);
    swatcher_start(global_watcher);

#ifdef _WIN32
    _getch();
#else
    getchar();
#endif

    SWATCHER_LOG_DEFAULT_INFO("Removing swatcher...");
    swatcher_remove(global_watcher, target);

    SWATCHER_LOG_DEFAULT_INFO("Stopping swatcher...");
    swatcher_stop(global_watcher);

    SWATCHER_LOG_DEFAULT_INFO("Cleaning up swatcher...");
    swatcher_cleanup(global_watcher);
    free(global_watcher);

    return EXIT_SUCCESS;
}
