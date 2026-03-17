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

#define TEST_DIR "test_win32_tmp"

static void create_test_dir(void)
{
    mkdir(TEST_DIR, 0755);
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

static void remove_test_dir(void)
{
    remove_dir_recursive(TEST_DIR);
}

static void write_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "w");
    assert(f != NULL);
    fputs(content, f);
    fclose(f);
}

/* ========== Event tracking ========== */

#define MAX_EVENTS 256

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

/* ========== Tests ========== */

static void test_win32_backend_exists(void)
{
    const swatcher_backend *b = swatcher_backend_win32();
    assert(b != NULL);
    assert(strcmp(b->name, "win32") == 0);
}

static void test_win32_detect_create(void)
{
    create_test_dir();

    char test_dir_abs[SW_PATH_MAX];
    sw_path_normalize(TEST_DIR, test_dir_abs, sizeof(test_dir_abs), false);

    swatcher_config config = { .poll_interval_ms = 100, .enable_logging = false };
    swatcher *sw = malloc(sizeof(swatcher));
    assert(sw != NULL);
    assert(swatcher_init_with_backend(sw, &config, "win32"));

    swatcher_target_desc desc = {
        .path = test_dir_abs,
        .is_recursive = false,
        .events = SWATCHER_EVENT_ALL,
        .watch_options = SWATCHER_WATCH_ALL,
        .callback = test_callback,
    };

    swatcher_target *target = swatcher_target_create(&desc);
    assert(target != NULL);
    assert(swatcher_add(sw, target));
    assert(swatcher_start(sw));

    reset_events();

    char filepath[SW_PATH_MAX];
    snprintf(filepath, sizeof(filepath), "%s%ccreated.txt", test_dir_abs, sw_path_separator());
    write_file(filepath, "hello");

    sw_sleep_ms(800);

    assert(get_event_count() > 0);
    assert(has_event(SWATCHER_EVENT_CREATED, "created.txt"));

    swatcher_stop(sw);
    swatcher_cleanup(sw);
    free(sw);

    remove(filepath);
    remove_test_dir();
}

static void test_win32_detect_modify(void)
{
    create_test_dir();

    char test_dir_abs[SW_PATH_MAX];
    sw_path_normalize(TEST_DIR, test_dir_abs, sizeof(test_dir_abs), false);

    char filepath[SW_PATH_MAX];
    snprintf(filepath, sizeof(filepath), "%s%cmodify.txt", test_dir_abs, sw_path_separator());
    write_file(filepath, "initial");

    swatcher_config config = { .poll_interval_ms = 100, .enable_logging = false };
    swatcher *sw = malloc(sizeof(swatcher));
    assert(sw != NULL);
    assert(swatcher_init_with_backend(sw, &config, "win32"));

    swatcher_target_desc desc = {
        .path = test_dir_abs,
        .is_recursive = false,
        .events = SWATCHER_EVENT_ALL,
        .watch_options = SWATCHER_WATCH_ALL,
        .callback = test_callback,
    };

    swatcher_target *target = swatcher_target_create(&desc);
    assert(target != NULL);
    assert(swatcher_add(sw, target));
    assert(swatcher_start(sw));

    reset_events();

    write_file(filepath, "modified content");
    sw_sleep_ms(800);

    assert(get_event_count() > 0);
    assert(has_event(SWATCHER_EVENT_MODIFIED, "modify.txt"));

    swatcher_stop(sw);
    swatcher_cleanup(sw);
    free(sw);

    remove(filepath);
    remove_test_dir();
}

static void test_win32_detect_delete(void)
{
    create_test_dir();

    char test_dir_abs[SW_PATH_MAX];
    sw_path_normalize(TEST_DIR, test_dir_abs, sizeof(test_dir_abs), false);

    char filepath[SW_PATH_MAX];
    snprintf(filepath, sizeof(filepath), "%s%cdelete.txt", test_dir_abs, sw_path_separator());
    write_file(filepath, "to be deleted");

    swatcher_config config = { .poll_interval_ms = 100, .enable_logging = false };
    swatcher *sw = malloc(sizeof(swatcher));
    assert(sw != NULL);
    assert(swatcher_init_with_backend(sw, &config, "win32"));

    swatcher_target_desc desc = {
        .path = test_dir_abs,
        .is_recursive = false,
        .events = SWATCHER_EVENT_ALL,
        .watch_options = SWATCHER_WATCH_ALL,
        .callback = test_callback,
    };

    swatcher_target *target = swatcher_target_create(&desc);
    assert(target != NULL);
    assert(swatcher_add(sw, target));
    assert(swatcher_start(sw));

    reset_events();

    sw_sleep_ms(300);
    remove(filepath);
    sw_sleep_ms(800);

    assert(get_event_count() > 0);
    assert(has_event(SWATCHER_EVENT_DELETED, "delete.txt"));

    swatcher_stop(sw);
    swatcher_cleanup(sw);
    free(sw);
    remove_test_dir();
}

static void test_win32_detect_move(void)
{
    create_test_dir();

    char test_dir_abs[SW_PATH_MAX];
    sw_path_normalize(TEST_DIR, test_dir_abs, sizeof(test_dir_abs), false);

    char filepath_old[SW_PATH_MAX];
    char filepath_new[SW_PATH_MAX];
    snprintf(filepath_old, sizeof(filepath_old), "%s%cbefore.txt", test_dir_abs, sw_path_separator());
    snprintf(filepath_new, sizeof(filepath_new), "%s%cafter.txt", test_dir_abs, sw_path_separator());
    write_file(filepath_old, "move me");

    swatcher_config config = { .poll_interval_ms = 100, .enable_logging = false };
    swatcher *sw = malloc(sizeof(swatcher));
    assert(sw != NULL);
    assert(swatcher_init_with_backend(sw, &config, "win32"));

    swatcher_target_desc desc = {
        .path = test_dir_abs,
        .is_recursive = false,
        .events = SWATCHER_EVENT_ALL,
        .watch_options = SWATCHER_WATCH_ALL,
        .callback = test_callback,
    };

    swatcher_target *target = swatcher_target_create(&desc);
    assert(target != NULL);
    assert(swatcher_add(sw, target));
    assert(swatcher_start(sw));

    reset_events();

    sw_sleep_ms(300);
    rename(filepath_old, filepath_new);
    sw_sleep_ms(800);

    assert(get_event_count() > 0);
    assert(has_event(SWATCHER_EVENT_MOVED, NULL));

    swatcher_stop(sw);
    swatcher_cleanup(sw);
    free(sw);

    remove(filepath_new);
    remove_test_dir();
}

static void test_win32_recursive(void)
{
    create_test_dir();
    char subdir[SW_PATH_MAX];
    snprintf(subdir, sizeof(subdir), "%s%csub", TEST_DIR, sw_path_separator());
    mkdir(subdir, 0755);

    char test_dir_abs[SW_PATH_MAX];
    sw_path_normalize(TEST_DIR, test_dir_abs, sizeof(test_dir_abs), false);

    swatcher_config config = { .poll_interval_ms = 100, .enable_logging = false };
    swatcher *sw = malloc(sizeof(swatcher));
    assert(sw != NULL);
    assert(swatcher_init_with_backend(sw, &config, "win32"));

    swatcher_target_desc desc = {
        .path = test_dir_abs,
        .is_recursive = true,
        .events = SWATCHER_EVENT_ALL,
        .watch_options = SWATCHER_WATCH_ALL,
        .callback = test_callback,
    };

    swatcher_target *target = swatcher_target_create(&desc);
    assert(target != NULL);
    assert(swatcher_add(sw, target));
    assert(swatcher_start(sw));

    reset_events();

    /* Create a file in the subdirectory */
    char filepath[SW_PATH_MAX];
    snprintf(filepath, sizeof(filepath), "%s%csub%cnested.txt",
             test_dir_abs, sw_path_separator(), sw_path_separator());
    write_file(filepath, "nested content");

    sw_sleep_ms(800);

    assert(get_event_count() > 0);
    assert(has_event(SWATCHER_EVENT_CREATED, "nested.txt"));

    swatcher_stop(sw);
    swatcher_cleanup(sw);
    free(sw);

    remove(filepath);
    remove_test_dir();
}

static void test_win32_pattern_filtering(void)
{
    create_test_dir();

    char test_dir_abs[SW_PATH_MAX];
    sw_path_normalize(TEST_DIR, test_dir_abs, sizeof(test_dir_abs), false);

    swatcher_config config = { .poll_interval_ms = 100, .enable_logging = false };
    swatcher *sw = malloc(sizeof(swatcher));
    assert(sw != NULL);
    assert(swatcher_init_with_backend(sw, &config, "win32"));

    swatcher_target_desc desc = {
        .path = test_dir_abs,
        .is_recursive = false,
        .events = SWATCHER_EVENT_ALL,
        .watch_options = SWATCHER_WATCH_ALL,
        .callback_patterns = GLOB_PATTERNS("*.txt"),
        .callback = test_callback,
    };

    swatcher_target *target = swatcher_target_create(&desc);
    assert(target != NULL);
    assert(swatcher_add(sw, target));
    assert(swatcher_start(sw));

    reset_events();

    char txt_path[SW_PATH_MAX];
    char log_path[SW_PATH_MAX];
    snprintf(txt_path, sizeof(txt_path), "%s%cmatch.txt", test_dir_abs, sw_path_separator());
    snprintf(log_path, sizeof(log_path), "%s%cnomatch.log", test_dir_abs, sw_path_separator());

    write_file(txt_path, "should match");
    sw_sleep_ms(300);
    write_file(log_path, "should not match");
    sw_sleep_ms(700);

    assert(has_event(SWATCHER_EVENT_CREATED, "match.txt"));
    assert(!has_event(SWATCHER_EVENT_CREATED, "nomatch.log"));

    swatcher_stop(sw);
    swatcher_cleanup(sw);
    free(sw);

    remove(txt_path);
    remove(log_path);
    remove_test_dir();
}

static void test_win32_error_codes(void)
{
    swatcher_config config = { .poll_interval_ms = 100, .enable_logging = false };
    swatcher *sw = malloc(sizeof(swatcher));
    assert(sw != NULL);
    assert(swatcher_init_with_backend(sw, &config, "win32"));

    /* Try to watch a nonexistent path */
    swatcher_target_desc desc = {
        .path = "C:\\nonexistent\\path\\that\\does\\not\\exist",
        .events = SWATCHER_EVENT_ALL,
        .callback = test_callback,
    };

    swatcher_target *target = swatcher_target_create(&desc);
    /* target_create should fail since path doesn't exist */
    assert(target == NULL);
    swatcher_error err = swatcher_last_error();
    assert(err == SWATCHER_ERR_PATH_NOT_FOUND || err == SWATCHER_ERR_INVALID_PATH);

    swatcher_cleanup(sw);
    free(sw);
}

static void test_win32_overflow_event_name(void)
{
    const char *name = swatcher_event_name(SWATCHER_EVENT_OVERFLOW);
    assert(strcmp(name, "SWATCHER_EVENT_OVERFLOW") == 0);
}

/* ========== Main ========== */

int main(void)
{
    event_mutex = sw_mutex_create();
    assert(event_mutex != NULL);

    printf("=== Win32 Backend Tests ===\n");
    fflush(stdout);

    RUN_TEST(test_win32_backend_exists);
    RUN_TEST(test_win32_overflow_event_name);
    RUN_TEST(test_win32_error_codes);
    RUN_TEST(test_win32_detect_create);
    RUN_TEST(test_win32_detect_modify);
    RUN_TEST(test_win32_detect_delete);
    RUN_TEST(test_win32_detect_move);
    RUN_TEST(test_win32_recursive);
    RUN_TEST(test_win32_pattern_filtering);

    printf("\n%d/%d tests passed!\n", tests_passed, tests_run);
    fflush(stdout);

    sw_mutex_destroy(event_mutex);
    return 0;
}
