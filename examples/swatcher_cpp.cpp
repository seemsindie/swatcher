#include <cstdio>
#include <cstdlib>
#include <swatcher.hpp>

int main()
{
    sw::Watcher watcher({.poll_interval_ms = 100, .enable_logging = true});
    if (!watcher) {
        fprintf(stderr, "Failed to create watcher: %s\n",
                sw::Watcher::last_error_string());
        return EXIT_FAILURE;
    }

    sw::Target target("./examples/test", SWATCHER_EVENT_ALL);
    target.recursive()
          .callback_patterns({"*.txt", "*.cpp"})
          .on_event([](swatcher_fs_event ev, const char *name, void *) {
              printf("Event: %s — %s\n", swatcher_event_name(ev), name ? name : "(null)");
          });

    if (!watcher.add(std::move(target))) {
        fprintf(stderr, "Failed to add target\n");
        return EXIT_FAILURE;
    }

    watcher.start();

    printf("Watching ./examples/test — press Enter to stop...\n");
    getchar();

    watcher.stop();
    printf("Done.\n");

    return EXIT_SUCCESS;
}
