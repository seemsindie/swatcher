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

/* ========== Counting allocator ========== */

static int alloc_count = 0;
static int free_count = 0;
static size_t total_allocated = 0;

static void *counting_malloc(size_t size, void *ctx)
{
    (void)ctx;
    alloc_count++;
    total_allocated += size;
    return malloc(size);
}

static void *counting_realloc(void *ptr, size_t size, void *ctx)
{
    (void)ctx;
    return realloc(ptr, size);
}

static void counting_free(void *ptr, void *ctx)
{
    (void)ctx;
    if (ptr) free_count++;
    free(ptr);
}

static void reset_counts(void)
{
    alloc_count = 0;
    free_count = 0;
    total_allocated = 0;
}

/* ========== Test helpers ========== */

#define TEST_DIR "test_allocator_tmp"

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

static int event_count = 0;

static void test_callback(swatcher_fs_event event, swatcher_target *target,
                           const char *event_name, void *additional_data)
{
    (void)event; (void)target; (void)event_name; (void)additional_data;
    event_count++;
}

/* ========== Tests ========== */

static void test_null_allocator_uses_stdlib(void)
{
    /* Default NULL allocator should work fine (stdlib) */
    swatcher_config config = { .poll_interval_ms = 100, .allocator = NULL };
    swatcher *sw = swatcher_create(&config);
    assert(sw != NULL);
    swatcher_destroy(sw);
}

static void test_custom_allocator_called(void)
{
    reset_counts();

    swatcher_allocator alloc = {
        .malloc = counting_malloc,
        .realloc = counting_realloc,
        .free = counting_free,
        .ctx = NULL
    };

    swatcher_config config = {
        .poll_interval_ms = 100,
        .allocator = &alloc
    };

    swatcher *sw = swatcher_create(&config);
    assert(sw != NULL);
    assert(alloc_count > 0);  /* At least sw + internal + mutex + backend */

    int allocs_before_target = alloc_count;

    create_test_dir();
    char test_dir_abs[SW_PATH_MAX];
    sw_path_normalize(TEST_DIR, test_dir_abs, sizeof(test_dir_abs), false);

    swatcher_target_desc desc = {
        .path = test_dir_abs,
        .is_recursive = false,
        .events = SWATCHER_EVENT_ALL,
        .callback = test_callback,
    };

    swatcher_target *target = swatcher_target_create(&desc);
    assert(target != NULL);
    assert(alloc_count > allocs_before_target);  /* target + internal + strdup */

    assert(swatcher_add(sw, target));
    swatcher_destroy(sw);

    /* Every allocation should have a matching free */
    printf("[allocs=%d frees=%d bytes=%zu] ", alloc_count, free_count, total_allocated);
    fflush(stdout);
    assert(alloc_count == free_count);

    remove_test_dir();
}

static void test_custom_allocator_with_events(void)
{
    reset_counts();

    swatcher_allocator alloc = {
        .malloc = counting_malloc,
        .realloc = counting_realloc,
        .free = counting_free,
        .ctx = NULL
    };

    swatcher_config config = {
        .poll_interval_ms = 100,
        .allocator = &alloc
    };

    create_test_dir();
    char test_dir_abs[SW_PATH_MAX];
    sw_path_normalize(TEST_DIR, test_dir_abs, sizeof(test_dir_abs), false);

    swatcher *sw = swatcher_create_with_backend(&config, "poll");
    assert(sw != NULL);

    event_count = 0;

    swatcher_target_desc desc = {
        .path = test_dir_abs,
        .is_recursive = false,
        .events = SWATCHER_EVENT_ALL,
        .callback = test_callback,
    };

    swatcher_target *target = swatcher_target_create(&desc);
    assert(target != NULL);
    assert(swatcher_add(sw, target));
    assert(swatcher_start(sw));

    /* Create a file, wait for detection */
    char filepath[SW_PATH_MAX];
    snprintf(filepath, sizeof(filepath), "%s/alloctest.txt", test_dir_abs);
    write_file(filepath, "hello from allocator test");
    sw_sleep_ms(400);

    assert(event_count > 0);

    swatcher_destroy(sw);

    printf("[allocs=%d frees=%d events=%d] ", alloc_count, free_count, event_count);
    assert(alloc_count == free_count);

    remove(filepath);
    remove_test_dir();
}

static void test_allocator_with_context(void)
{
    reset_counts();

    /* Use context pointer to pass a tag we can verify */
    int tag = 42;

    swatcher_allocator alloc = {
        .malloc = counting_malloc,
        .realloc = counting_realloc,
        .free = counting_free,
        .ctx = &tag
    };

    swatcher_config config = {
        .poll_interval_ms = 100,
        .allocator = &alloc
    };

    swatcher *sw = swatcher_create(&config);
    assert(sw != NULL);
    assert(alloc_count > 0);

    swatcher_destroy(sw);
    assert(alloc_count == free_count);
}

/* ========== Main ========== */

int main(void)
{
    printf("=== Allocator Tests ===\n");

    RUN_TEST(test_null_allocator_uses_stdlib);
    RUN_TEST(test_custom_allocator_called);
    RUN_TEST(test_custom_allocator_with_events);
    RUN_TEST(test_allocator_with_context);

    printf("\n%d/%d tests passed!\n", tests_passed, tests_run);
    return 0;
}
