#include <stdio.h>
#include <swatcher.h>
#include <signal.h>
#ifdef _WIN32
#include <conio.h>
#endif

void my_callback(swatcher_fs_event event, swatcher_target *target, const char *name, void *data)
{
    (void)data;
    SWATCHER_LOG_DEFAULT_INFO("Event: %s", swatcher_event_name(event));
    SWATCHER_LOG_DEFAULT_INFO("Target path: %s", target->path);
    SWATCHER_LOG_DEFAULT_INFO("Changed: %s", name);
}

int main(void)
{
    swatcher_config config = {
        .poll_interval_ms = 50,
        .enable_logging = true
    };

    swatcher *sw = swatcher_create(&config);
    if (!sw) {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to create swatcher");
        return EXIT_FAILURE;
    }

    swatcher_target *target = swatcher_target_create(&(swatcher_target_desc){
        .path = "./examples/test",
        .is_recursive = false,
        .events = SWATCHER_EVENT_CREATED | SWATCHER_EVENT_MODIFIED |
                  SWATCHER_EVENT_DELETED | SWATCHER_EVENT_MOVED,
        .user_data = "Hello, world (user data)!",
        .callback = my_callback
    });

    swatcher_add(sw, target);
    swatcher_start(sw);

    printf("Watching ./examples/test — press Enter to stop.\n");
#ifdef _WIN32
    _getch();
#else
    getchar();
#endif

    swatcher_destroy(sw);
    return EXIT_SUCCESS;
}
