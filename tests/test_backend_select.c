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

static int tests_run = 0;
static int tests_passed = 0;

#define RUN_TEST(fn) do { \
    printf("  %-55s", #fn); \
    fn(); \
    tests_run++; \
    tests_passed++; \
    printf("PASS\n"); \
} while(0)

#define TEST_DIR "test_backend_select_tmp"

static void create_test_dir(void)
{
    mkdir(TEST_DIR, 0755);
}

static void remove_test_dir(void)
{
    rmdir(TEST_DIR);
}

/* ========== Tests ========== */

static void test_default_backend(void)
{
    create_test_dir();

    swatcher_config config = { .poll_interval_ms = 100, .enable_logging = false };
    swatcher *sw = malloc(sizeof(swatcher));
    assert(sw != NULL);
    assert(swatcher_init(sw, &config));

    swatcher_cleanup(sw);
    free(sw);
    remove_test_dir();
}

static void test_poll_backend(void)
{
    create_test_dir();

    swatcher_config config = { .poll_interval_ms = 100, .enable_logging = false };
    swatcher *sw = malloc(sizeof(swatcher));
    assert(sw != NULL);
    assert(swatcher_init_with_backend(sw, &config, "poll"));

    swatcher_cleanup(sw);
    free(sw);
    remove_test_dir();
}

static void test_invalid_backend(void)
{
    swatcher_config config = { .poll_interval_ms = 100, .enable_logging = false };
    swatcher *sw = malloc(sizeof(swatcher));
    assert(sw != NULL);

    bool ok = swatcher_init_with_backend(sw, &config, "nonexistent");
    assert(!ok);
    assert(swatcher_last_error() == SWATCHER_ERR_BACKEND_NOT_FOUND);

    free(sw);
}

static void test_backends_available(void)
{
    const char **backends = swatcher_backends_available();
    assert(backends != NULL);

    /* "poll" should always be available */
    bool found_poll = false;
    for (int i = 0; backends[i] != NULL; i++) {
        if (strcmp(backends[i], "poll") == 0)
            found_poll = true;
    }
    assert(found_poll);
}

static void test_inotify_backend_by_name(void)
{
    create_test_dir();

    swatcher_config config = { .poll_interval_ms = 100, .enable_logging = false };
    swatcher *sw = malloc(sizeof(swatcher));
    assert(sw != NULL);

#if defined(__linux__)
    assert(swatcher_init_with_backend(sw, &config, "inotify"));
    swatcher_cleanup(sw);
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
    assert(swatcher_init_with_backend(sw, &config, "kqueue"));
    swatcher_cleanup(sw);
#endif

    free(sw);
    remove_test_dir();
}

/* ========== Main ========== */

int main(void)
{
    printf("=== Backend Selection Tests ===\n");

    RUN_TEST(test_default_backend);
    RUN_TEST(test_poll_backend);
    RUN_TEST(test_invalid_backend);
    RUN_TEST(test_backends_available);
    RUN_TEST(test_inotify_backend_by_name);

    printf("\n%d/%d tests passed!\n", tests_passed, tests_run);
    return 0;
}
