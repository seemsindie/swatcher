#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/platform/platform.h"
#include "swatcher_types.h"

static int tests_run = 0;
static int tests_passed = 0;

#define RUN_TEST(fn) do { printf("  %-50s", #fn); fn(); tests_run++; tests_passed++; printf("PASS\n"); } while(0)

/* ========== Mutex ========== */

static void test_mutex_create_destroy(void)
{
    sw_mutex *m = sw_mutex_create();
    assert(m != NULL);
    sw_mutex_destroy(m);
}

static void test_mutex_lock_unlock(void)
{
    sw_mutex *m = sw_mutex_create();
    assert(m != NULL);
    sw_mutex_lock(m);
    sw_mutex_unlock(m);
    /* Lock again to verify unlock worked */
    sw_mutex_lock(m);
    sw_mutex_unlock(m);
    sw_mutex_destroy(m);
}

/* ========== Thread ========== */

static volatile int thread_flag = 0;

static void *thread_set_flag(void *arg)
{
    (void)arg;
    thread_flag = 42;
    return NULL;
}

static void test_thread_create_join(void)
{
    thread_flag = 0;
    sw_thread *t = sw_thread_create(thread_set_flag, NULL);
    assert(t != NULL);
    sw_thread_join(t);
    assert(thread_flag == 42);
    sw_thread_destroy(t);
}

/* ========== Path ========== */

static void test_path_is_absolute(void)
{
#ifdef _WIN32
    assert(sw_path_is_absolute("C:\\Users") == true);
    assert(sw_path_is_absolute("\\\\server\\share") == true);
    assert(sw_path_is_absolute("relative\\path") == false);
#else
    assert(sw_path_is_absolute("/usr/bin") == true);
    assert(sw_path_is_absolute("relative/path") == false);
#endif
    assert(sw_path_is_absolute(NULL) == false);
}

static void test_path_normalize_relative(void)
{
    char output[SW_PATH_MAX];
    bool ok = sw_path_normalize("testfile.txt", output, sizeof(output), false);
    assert(ok);
    assert(sw_path_is_absolute(output));
    /* Should contain the filename at the end */
    assert(strstr(output, "testfile.txt") != NULL);
}

static void test_path_separator(void)
{
#ifdef _WIN32
    assert(sw_path_separator() == '\\');
#else
    assert(sw_path_separator() == '/');
#endif
}

/* ========== Stat ========== */

static void test_stat_existing_file(void)
{
    /* This test file itself should exist */
    sw_file_info info;
    bool ok = sw_stat("tests/test_platform.c", &info, true);
    assert(ok);
    assert(info.is_file == true);
    assert(info.is_directory == false);
}

static void test_stat_existing_dir(void)
{
    sw_file_info info;
    bool ok = sw_stat("tests", &info, true);
    assert(ok);
    assert(info.is_directory == true);
    assert(info.is_file == false);
}

static void test_stat_nonexistent(void)
{
    sw_file_info info;
    bool ok = sw_stat("nonexistent_file_xyz", &info, true);
    assert(!ok);
}

/* ========== Directory ========== */

static void test_dir_iterate(void)
{
    sw_dir *d = sw_dir_open("tests");
    assert(d != NULL);

    sw_dir_entry entry;
    bool found_self = false;
    while (sw_dir_next(d, &entry)) {
        if (strcmp(entry.name, "test_platform.c") == 0) {
            found_self = true;
            assert(entry.is_file == true);
        }
    }
    assert(found_self);
    sw_dir_close(d);
}

/* ========== Time ========== */

static void test_time_monotonic(void)
{
    uint64_t t1 = sw_time_now_ms();
    /* Burn some cycles */
    volatile int x = 0;
    for (int i = 0; i < 1000000; i++) x += i;
    (void)x;
    uint64_t t2 = sw_time_now_ms();
    assert(t2 >= t1);
}

static void test_time_reasonable(void)
{
    uint64_t t = sw_time_now_ms();
    /* Should be nonzero (system has been up for at least 1ms) */
    assert(t > 0);
}

/* ========== Atomics ========== */

static void test_atomic_store_load(void)
{
    sw_atomic_bool flag;
    sw_atomic_store(&flag, false);
    assert(sw_atomic_load(&flag) == false);
    sw_atomic_store(&flag, true);
    assert(sw_atomic_load(&flag) == true);
    sw_atomic_store(&flag, false);
    assert(sw_atomic_load(&flag) == false);
}

static sw_atomic_bool cross_thread_flag;

static void *thread_store_true(void *arg)
{
    (void)arg;
    sw_atomic_store(&cross_thread_flag, true);
    return NULL;
}

static void test_atomic_cross_thread(void)
{
    sw_atomic_store(&cross_thread_flag, false);
    assert(sw_atomic_load(&cross_thread_flag) == false);

    sw_thread *t = sw_thread_create(thread_store_true, NULL);
    assert(t != NULL);
    sw_thread_join(t);

    assert(sw_atomic_load(&cross_thread_flag) == true);
    sw_thread_destroy(t);
}

/* ========== String ========== */

static void test_strdup(void)
{
    char *s = sw_strdup("hello");
    assert(s != NULL);
    assert(strcmp(s, "hello") == 0);
    free(s);
}

/* ========== Main ========== */

int main(void)
{
    printf("=== Platform Tests ===\n");

    RUN_TEST(test_mutex_create_destroy);
    RUN_TEST(test_mutex_lock_unlock);
    RUN_TEST(test_thread_create_join);
    RUN_TEST(test_path_is_absolute);
    RUN_TEST(test_path_normalize_relative);
    RUN_TEST(test_path_separator);
    RUN_TEST(test_stat_existing_file);
    RUN_TEST(test_stat_existing_dir);
    RUN_TEST(test_stat_nonexistent);
    RUN_TEST(test_dir_iterate);
    RUN_TEST(test_time_monotonic);
    RUN_TEST(test_time_reasonable);
    RUN_TEST(test_atomic_store_load);
    RUN_TEST(test_atomic_cross_thread);
    RUN_TEST(test_strdup);

    printf("\n%d/%d tests passed!\n", tests_passed, tests_run);
    return 0;
}
