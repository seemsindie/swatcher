/**
 * @file live_reload.c
 * @brief Watch src/ for .c/.h changes and print "rebuild needed".
 *
 * Usage: ./live_reload [directory]
 * Default directory: ./src
 */
#include <stdio.h>
#include <swatcher.h>
#ifdef _WIN32
#include <conio.h>
#endif

static void on_change(swatcher_fs_event event, swatcher_target *target, const char *name, void *data)
{
    (void)target;
    (void)data;
    printf("[%s] %s — rebuild needed\n", swatcher_event_name(event), name ? name : "(unknown)");
}

int main(int argc, char *argv[])
{
    const char *dir = (argc > 1) ? argv[1] : "./src";

    swatcher_config config = {
        .poll_interval_ms = 100,
        .enable_logging = false,
        .coalesce_ms = 200
    };

    swatcher *sw = swatcher_create(&config);
    if (!sw) {
        fprintf(stderr, "error: failed to create watcher (%s)\n",
                swatcher_error_string(swatcher_last_error()));
        return 1;
    }

    swatcher_target *target = swatcher_target_create(&(swatcher_target_desc){
        .path = (char *)dir,
        .is_recursive = true,
        .events = SWATCHER_EVENT_CREATED | SWATCHER_EVENT_MODIFIED | SWATCHER_EVENT_DELETED,
        .callback_patterns = GLOB_PATTERNS("*.c", "*.h"),
        .callback = on_change
    });

    if (!target || !swatcher_add(sw, target)) {
        fprintf(stderr, "error: failed to watch '%s'\n", dir);
        swatcher_destroy(sw);
        return 1;
    }

    swatcher_start(sw);
    printf("Watching %s for *.c / *.h changes — press Enter to stop.\n", dir);

#ifdef _WIN32
    _getch();
#else
    getchar();
#endif

    swatcher_destroy(sw);
    return 0;
}
