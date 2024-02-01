#include <stdio.h>
#include <swatcher.h>
#include <signal.h>

static swatcher *global_watcher = NULL;

void my_callback_function(swatcher_fs_event event, swatcher_target *target, const char *filename)
{
    SWATCHER_LOG_DEFAULT_DEBUG("Event: %s", get_event_name(event));
    SWATCHER_LOG_DEFAULT_DEBUG("Target path: %s", target->path);
    // printf("Target is_file: %s\n", target->is_file ? "true" : "false"); // might give "false" indications if file is created in a watched directory it will be false but that's becasuse that target is not but a file is created...i don't give a fuck...
    if (filename != NULL)
    {
        SWATCHER_LOG_DEFAULT_DEBUG("Event name: %s", filename);
    }
    else
    {
        if (target->is_file)
        {
            SWATCHER_LOG_DEFAULT_DEBUG("Event name: file itself");
        }
        else
        {
            SWATCHER_LOG_DEFAULT_DEBUG("Event name: Directory itself");
        }
    }
}

int main()
{
    // swatcher *watcher = malloc(sizeof(swatcher));
    global_watcher = malloc(sizeof(swatcher));
    // char *path = "/home/timelord/projects/swatcher/examples/test";
    char *path = "../examples/test/";
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
        .is_recursive = true,
        .events = SWATCHER_EVENT_DELETED | SWATCHER_EVENT_CREATED | SWATCHER_EVENT_MODIFIED | SWATCHER_EVENT_MOVED,
        // .events = SWATCHER_EVENT_ALL,
        // .pattern = ".*\\.txt$",
        .user_data = "Hello, world (user data)!",
        .callback = my_callback_function});
    swatcher_add(global_watcher, target);

    swatcher_start(global_watcher);

    getchar();

    swatcher_remove(global_watcher, target);

    swatcher_stop(global_watcher);
    swatcher_cleanup(global_watcher);

    return EXIT_SUCCESS;
}
