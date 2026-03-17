/* Ensure asserts are always active, even in Release builds */
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define mkdir(d, m) _mkdir(d)
#define rmdir _rmdir
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "swatcher.h"
#include "../src/internal/internal.h"

static int tests_run = 0;
static int tests_passed = 0;

#define RUN_TEST(fn) do { \
    printf("  %-55s", #fn); \
    fflush(stdout); \
    fn(); \
    tests_run++; \
    tests_passed++; \
    printf("PASS\n"); \
    fflush(stdout); \
} while(0)

/* ========== Test helpers ========== */

#define STRESS_DIR "stress_win32_tmp"

static void create_stress_dir(void)
{
    mkdir(STRESS_DIR, 0755);
}

static void remove_dir_recursive(const char *path)
{
    sw_dir *d = sw_dir_open(path);
    if (!d) return;
    sw_dir_entry entry;
    char child[SW_PATH_MAX];
    while (sw_dir_next(d, &entry)) {
        if (strcmp(entry.name, ".") == 0 || strcmp(entry.name, "..") == 0)
            continue;
        snprintf(child, sizeof(child), "%s%c%s", path, sw_path_separator(), entry.name);
        if (entry.is_dir) {
            remove_dir_recursive(child);
        } else {
            remove(child);
        }
    }
    sw_dir_close(d);
    rmdir(path);
}

static void remove_stress_dir(void)
{
    remove_dir_recursive(STRESS_DIR);
}

static void write_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "w");
    assert(f != NULL);
    fputs(content, f);
    fclose(f);
}

/* ========== Event tracking ========== */

#define MAX_EVENTS 8192

typedef struct {
    swatcher_fs_event event;
    char path[256];
} recorded_event;

static recorded_event events[MAX_EVENTS];
static int event_count = 0;
static sw_mutex *event_mutex = NULL;

static void reset_events(void)
{
    sw_mutex_lock(event_mutex);
    event_count = 0;
    sw_mutex_unlock(event_mutex);
}

static int get_event_count(void)
{
    sw_mutex_lock(event_mutex);
    int c = event_count;
    sw_mutex_unlock(event_mutex);
    return c;
}

static void stress_callback(swatcher_fs_event event, swatcher_target *target,
                              const char *event_name, void *additional_data)
{
    (void)target;
    (void)additional_data;
    sw_mutex_lock(event_mutex);
    if (event_count < MAX_EVENTS) {
        events[event_count].event = event;
        if (event_name)
            strncpy(events[event_count].path, event_name, 255);
        else
            events[event_count].path[0] = '\0';
        event_count++;
    }
    sw_mutex_unlock(event_mutex);
}

static int count_events_of_type(swatcher_fs_event type)
{
    sw_mutex_lock(event_mutex);
    int c = 0;
    for (int i = 0; i < event_count; i++) {
        if (events[i].event == type)
            c++;
    }
    sw_mutex_unlock(event_mutex);
    return c;
}

/* ========== Stress Tests ========== */

static void stress_rapid_create(void)
{
    create_stress_dir();

    char stress_dir_abs[SW_PATH_MAX];
    sw_path_normalize(STRESS_DIR, stress_dir_abs, sizeof(stress_dir_abs), false);

    swatcher_config config = { .poll_interval_ms = 50, .enable_logging = false };
    swatcher *sw = malloc(sizeof(swatcher));
    assert(sw != NULL);
    assert(swatcher_init_with_backend(sw, &config, "win32"));

    swatcher_target_desc desc = {
        .path = stress_dir_abs,
        .is_recursive = false,
        .events = SWATCHER_EVENT_CREATED,
        .watch_options = SWATCHER_WATCH_ALL,
        .callback = stress_callback,
    };

    swatcher_target *target = swatcher_target_create(&desc);
    assert(target != NULL);
    assert(swatcher_add(sw, target));
    assert(swatcher_start(sw));

    reset_events();

    /* Create 500 files rapidly */
    for (int i = 0; i < 500; i++) {
        char filepath[SW_PATH_MAX];
        snprintf(filepath, sizeof(filepath), "%s%cfile_%04d.txt",
                 stress_dir_abs, sw_path_separator(), i);
        write_file(filepath, "x");
    }

    sw_sleep_ms(3000);

    int created = count_events_of_type(SWATCHER_EVENT_CREATED);
    printf("[%d/500 CREATED] ", created);
    fflush(stdout);
    assert(created >= 400); /* Allow some missed under extreme load */

    swatcher_stop(sw);
    swatcher_cleanup(sw);
    free(sw);
    remove_stress_dir();
}

static void stress_rapid_modify(void)
{
    create_stress_dir();

    char stress_dir_abs[SW_PATH_MAX];
    sw_path_normalize(STRESS_DIR, stress_dir_abs, sizeof(stress_dir_abs), false);

    char filepath[SW_PATH_MAX];
    snprintf(filepath, sizeof(filepath), "%s%crapid_modify.txt",
             stress_dir_abs, sw_path_separator());
    write_file(filepath, "initial");

    swatcher_config config = { .poll_interval_ms = 50, .enable_logging = false };
    swatcher *sw = malloc(sizeof(swatcher));
    assert(sw != NULL);
    assert(swatcher_init_with_backend(sw, &config, "win32"));

    swatcher_target_desc desc = {
        .path = stress_dir_abs,
        .is_recursive = false,
        .events = SWATCHER_EVENT_MODIFIED,
        .watch_options = SWATCHER_WATCH_ALL,
        .callback = stress_callback,
    };

    swatcher_target *target = swatcher_target_create(&desc);
    assert(target != NULL);
    assert(swatcher_add(sw, target));
    assert(swatcher_start(sw));

    reset_events();

    for (int i = 0; i < 500; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "write %d", i);
        write_file(filepath, buf);
    }

    sw_sleep_ms(3000);

    int modified = count_events_of_type(SWATCHER_EVENT_MODIFIED);
    printf("[%d MODIFIED] ", modified);
    fflush(stdout);
    assert(modified > 0);

    swatcher_stop(sw);
    swatcher_cleanup(sw);
    free(sw);
    remove_stress_dir();
}

static void stress_many_directories(void)
{
    /* Proves IOCP works beyond the old 64-handle limit */
    create_stress_dir();

    char stress_dir_abs[SW_PATH_MAX];
    sw_path_normalize(STRESS_DIR, stress_dir_abs, sizeof(stress_dir_abs), false);

    /* Create 100 subdirectories */
    for (int i = 0; i < 100; i++) {
        char subdir[SW_PATH_MAX];
        snprintf(subdir, sizeof(subdir), "%s%cdir_%03d",
                 stress_dir_abs, sw_path_separator(), i);
        mkdir(subdir, 0755);
    }

    swatcher_config config = { .poll_interval_ms = 50, .enable_logging = false };
    swatcher *sw = malloc(sizeof(swatcher));
    assert(sw != NULL);
    assert(swatcher_init_with_backend(sw, &config, "win32"));

    /* Add each subdir as a separate watch target (100 targets = exceeds old 64 limit) */
    int added = 0;
    for (int i = 0; i < 100; i++) {
        char subdir[SW_PATH_MAX];
        snprintf(subdir, sizeof(subdir), "%s%cdir_%03d",
                 stress_dir_abs, sw_path_separator(), i);

        swatcher_target_desc desc = {
            .path = subdir,
            .is_recursive = false,
            .events = SWATCHER_EVENT_CREATED,
            .watch_options = SWATCHER_WATCH_ALL,
            .callback = stress_callback,
        };

        swatcher_target *target = swatcher_target_create(&desc);
        if (target && swatcher_add(sw, target))
            added++;
    }

    printf("[%d/100 dirs added] ", added);
    fflush(stdout);
    assert(added >= 95); /* Most should succeed */

    assert(swatcher_start(sw));

    reset_events();

    /* Create a file in dir_050 */
    char filepath[SW_PATH_MAX];
    snprintf(filepath, sizeof(filepath), "%s%cdir_050%ctest.txt",
             stress_dir_abs, sw_path_separator(), sw_path_separator());
    write_file(filepath, "test");

    sw_sleep_ms(1000);

    assert(get_event_count() > 0);

    swatcher_stop(sw);
    swatcher_cleanup(sw);
    free(sw);
    remove_stress_dir();
}

static void stress_churn(void)
{
    create_stress_dir();

    char stress_dir_abs[SW_PATH_MAX];
    sw_path_normalize(STRESS_DIR, stress_dir_abs, sizeof(stress_dir_abs), false);

    swatcher_config config = { .poll_interval_ms = 50, .enable_logging = false };
    swatcher *sw = malloc(sizeof(swatcher));
    assert(sw != NULL);
    assert(swatcher_init_with_backend(sw, &config, "win32"));

    swatcher_target_desc desc = {
        .path = stress_dir_abs,
        .is_recursive = false,
        .events = SWATCHER_EVENT_ALL,
        .watch_options = SWATCHER_WATCH_ALL,
        .callback = stress_callback,
    };

    swatcher_target *target = swatcher_target_create(&desc);
    assert(target != NULL);
    assert(swatcher_add(sw, target));
    assert(swatcher_start(sw));

    reset_events();

    char filepath[SW_PATH_MAX];
    snprintf(filepath, sizeof(filepath), "%s%cchurn.txt",
             stress_dir_abs, sw_path_separator());

    for (int i = 0; i < 100; i++) {
        write_file(filepath, "churn");
        remove(filepath);
    }

    sw_sleep_ms(2000);

    int total = get_event_count();
    printf("[%d events from 100 create+delete cycles] ", total);
    fflush(stdout);
    assert(total > 0);

    swatcher_stop(sw);
    swatcher_cleanup(sw);
    free(sw);
    remove_stress_dir();
}

/* ========== Main ========== */

int main(void)
{
    event_mutex = sw_mutex_create();
    assert(event_mutex != NULL);

    printf("=== Win32 Stress Tests ===\n");
    fflush(stdout);

    RUN_TEST(stress_rapid_create);
    RUN_TEST(stress_rapid_modify);
    RUN_TEST(stress_many_directories);
    RUN_TEST(stress_churn);

    printf("\n%d/%d stress tests passed!\n", tests_passed, tests_run);
    fflush(stdout);

    sw_mutex_destroy(event_mutex);
    return 0;
}
