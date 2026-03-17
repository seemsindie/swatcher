/* Ensure asserts are always active, even in Release builds */
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
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

#define RUN_TEST(fn) do { printf("  %-50s", #fn); fn(); tests_run++; tests_passed++; printf("PASS\n"); } while(0)

/* ========== Test helpers ========== */

/* Temp directory for test files — created relative to CWD */
#define TEST_DIR "test_poll_tmp"

static void create_test_dir(void)
{
#ifdef _WIN32
    _mkdir(TEST_DIR);
#else
    mkdir(TEST_DIR, 0755);
#endif
}

static void remove_test_dir(void)
{
    /* Clean up test files */
    sw_dir *d = sw_dir_open(TEST_DIR);
    if (!d) return;
    sw_dir_entry entry;
    char path[SW_PATH_MAX];
    while (sw_dir_next(d, &entry)) {
        if (strcmp(entry.name, ".") == 0 || strcmp(entry.name, "..") == 0)
            continue;
        snprintf(path, sizeof(path), "%s/%s", TEST_DIR, entry.name);
        remove(path);
    }
    sw_dir_close(d);
    rmdir(TEST_DIR);
}

static void write_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "w");
    assert(f != NULL);
    fputs(content, f);
    fclose(f);
}

/* ========== Event tracking ========== */

#define MAX_EVENTS 64

typedef struct {
    swatcher_fs_event event;
    char path[SW_PATH_MAX];
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

static void test_callback(swatcher_fs_event event, swatcher_target *target,
                           const char *event_name, void *additional_data)
{
    (void)target;
    (void)additional_data;
    sw_mutex_lock(event_mutex);
    if (event_count < MAX_EVENTS) {
        events[event_count].event = event;
        if (event_name)
            strncpy(events[event_count].path, event_name, SW_PATH_MAX - 1);
        else
            events[event_count].path[0] = '\0';
        event_count++;
    }
    sw_mutex_unlock(event_mutex);
}

static bool has_event(swatcher_fs_event type, const char *path_substr)
{
    sw_mutex_lock(event_mutex);
    for (int i = 0; i < event_count; i++) {
        if (events[i].event == type) {
            if (!path_substr || strstr(events[i].path, path_substr)) {
                sw_mutex_unlock(event_mutex);
                return true;
            }
        }
    }
    sw_mutex_unlock(event_mutex);
    return false;
}

/* ========== Poll backend swap helper ========== */

/* After swatcher_init, swap from default backend to poll backend */
static void swap_to_poll_backend(swatcher *sw)
{
    swatcher_internal *si = SW_INTERNAL(sw);
    si->backend->destroy(sw);
    si->backend = swatcher_backend_poll();
    bool ok = si->backend->init(sw);
    assert(ok);
    (void)ok;
}

/* ========== Tests ========== */

static void test_poll_backend_exists(void)
{
    const swatcher_backend *b = swatcher_backend_poll();
    assert(b != NULL);
    assert(strcmp(b->name, "poll") == 0);
}

static void test_poll_detect_file_create(void)
{
    create_test_dir();

    char test_dir_abs[SW_PATH_MAX];
    sw_path_normalize(TEST_DIR, test_dir_abs, sizeof(test_dir_abs), false);

    swatcher_config config = { .poll_interval_ms = 100, .enable_logging = false };
    swatcher *sw = malloc(sizeof(swatcher));
    assert(sw != NULL);
    assert(swatcher_init(sw, &config));
    swap_to_poll_backend(sw);

    swatcher_target_desc desc = {
        .path = test_dir_abs,
        .is_recursive = false,
        .events = SWATCHER_EVENT_ALL,
        .watch_options = SWATCHER_WATCH_ALL,
        .follow_symlinks = false,
        .callback = test_callback,
    };

    swatcher_target *target = swatcher_target_create(&desc);
    assert(target != NULL);
    assert(swatcher_add(sw, target));
    assert(swatcher_start(sw));

    reset_events();

    char filepath[SW_PATH_MAX];
    snprintf(filepath, sizeof(filepath), "%s/created.txt", test_dir_abs);
    write_file(filepath, "hello");

    sw_sleep_ms(400);

    assert(get_event_count() > 0);
    assert(has_event(SWATCHER_EVENT_CREATED, "created.txt"));

    swatcher_stop(sw);
    swatcher_cleanup(sw);
    free(sw);

    remove(filepath);
    remove_test_dir();
}

static void test_poll_detect_file_modify(void)
{
    create_test_dir();

    char test_dir_abs[SW_PATH_MAX];
    sw_path_normalize(TEST_DIR, test_dir_abs, sizeof(test_dir_abs), false);

    char filepath[SW_PATH_MAX];
    snprintf(filepath, sizeof(filepath), "%s/modify.txt", test_dir_abs);
    write_file(filepath, "initial");

    swatcher_config config = { .poll_interval_ms = 100, .enable_logging = false };
    swatcher *sw = malloc(sizeof(swatcher));
    assert(sw != NULL);
    assert(swatcher_init(sw, &config));
    swap_to_poll_backend(sw);

    swatcher_target_desc desc = {
        .path = test_dir_abs,
        .is_recursive = false,
        .events = SWATCHER_EVENT_ALL,
        .watch_options = SWATCHER_WATCH_ALL,
        .follow_symlinks = false,
        .callback = test_callback,
    };

    swatcher_target *target = swatcher_target_create(&desc);
    assert(target != NULL);
    assert(swatcher_add(sw, target));
    assert(swatcher_start(sw));

    reset_events();

    /* Need mtime to actually change — sleep past 1s granularity */
    sw_sleep_ms(1100);
    write_file(filepath, "modified content that is different");
    sw_sleep_ms(400);

    assert(get_event_count() > 0);
    assert(has_event(SWATCHER_EVENT_MODIFIED, "modify.txt"));

    swatcher_stop(sw);
    swatcher_cleanup(sw);
    free(sw);

    remove(filepath);
    remove_test_dir();
}

static void test_poll_detect_file_delete(void)
{
    create_test_dir();

    char test_dir_abs[SW_PATH_MAX];
    sw_path_normalize(TEST_DIR, test_dir_abs, sizeof(test_dir_abs), false);

    char filepath[SW_PATH_MAX];
    snprintf(filepath, sizeof(filepath), "%s/delete.txt", test_dir_abs);
    write_file(filepath, "to be deleted");

    swatcher_config config = { .poll_interval_ms = 100, .enable_logging = false };
    swatcher *sw = malloc(sizeof(swatcher));
    assert(sw != NULL);
    assert(swatcher_init(sw, &config));
    swap_to_poll_backend(sw);

    swatcher_target_desc desc = {
        .path = test_dir_abs,
        .is_recursive = false,
        .events = SWATCHER_EVENT_ALL,
        .watch_options = SWATCHER_WATCH_ALL,
        .follow_symlinks = false,
        .callback = test_callback,
    };

    swatcher_target *target = swatcher_target_create(&desc);
    assert(target != NULL);
    assert(swatcher_add(sw, target));
    assert(swatcher_start(sw));

    reset_events();

    sw_sleep_ms(300);
    remove(filepath);
    sw_sleep_ms(400);

    assert(get_event_count() > 0);
    assert(has_event(SWATCHER_EVENT_DELETED, "delete.txt"));

    swatcher_stop(sw);
    swatcher_cleanup(sw);
    free(sw);
    remove_test_dir();
}

static void test_poll_detect_file_move(void)
{
    create_test_dir();

    char test_dir_abs[SW_PATH_MAX];
    sw_path_normalize(TEST_DIR, test_dir_abs, sizeof(test_dir_abs), false);

    char filepath_old[SW_PATH_MAX];
    char filepath_new[SW_PATH_MAX];
    snprintf(filepath_old, sizeof(filepath_old), "%s/before.txt", test_dir_abs);
    snprintf(filepath_new, sizeof(filepath_new), "%s/after.txt", test_dir_abs);
    write_file(filepath_old, "move me");

    swatcher_config config = { .poll_interval_ms = 100, .enable_logging = false };
    swatcher *sw = malloc(sizeof(swatcher));
    assert(sw != NULL);
    assert(swatcher_init(sw, &config));
    swap_to_poll_backend(sw);

    swatcher_target_desc desc = {
        .path = test_dir_abs,
        .is_recursive = false,
        .events = SWATCHER_EVENT_ALL,
        .watch_options = SWATCHER_WATCH_ALL,
        .follow_symlinks = false,
        .callback = test_callback,
    };

    swatcher_target *target = swatcher_target_create(&desc);
    assert(target != NULL);
    assert(swatcher_add(sw, target));
    assert(swatcher_start(sw));

    reset_events();

    sw_sleep_ms(300);
    rename(filepath_old, filepath_new);
    sw_sleep_ms(400);

    assert(get_event_count() > 0);
    assert(has_event(SWATCHER_EVENT_MOVED, "after.txt"));

    swatcher_stop(sw);
    swatcher_cleanup(sw);
    free(sw);

    remove(filepath_new);
    remove_test_dir();
}

static void test_stat_mtime_size(void)
{
    create_test_dir();
    char filepath[SW_PATH_MAX];
    snprintf(filepath, sizeof(filepath), "%s/stattest.txt", TEST_DIR);
    write_file(filepath, "some content");

    sw_file_info info;
    assert(sw_stat(filepath, &info, true));
    assert(info.is_file);
    assert(info.size > 0);
    assert(info.mtime > 0);

    remove(filepath);
    remove_test_dir();
}

static void test_sleep_ms(void)
{
    uint64_t t1 = sw_time_now_ms();
    sw_sleep_ms(50);
    uint64_t t2 = sw_time_now_ms();
    assert(t2 - t1 >= 40); /* allow some slack */
}

/* ========== Main ========== */

int main(void)
{
    event_mutex = sw_mutex_create();
    assert(event_mutex != NULL);

    printf("=== Poll Backend Tests ===\n");

    RUN_TEST(test_poll_backend_exists);
    RUN_TEST(test_stat_mtime_size);
    RUN_TEST(test_sleep_ms);
    RUN_TEST(test_poll_detect_file_create);
    RUN_TEST(test_poll_detect_file_modify);
    RUN_TEST(test_poll_detect_file_delete);
    RUN_TEST(test_poll_detect_file_move);

    printf("\n%d/%d tests passed!\n", tests_passed, tests_run);

    sw_mutex_destroy(event_mutex);
    return 0;
}
