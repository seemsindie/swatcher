/* Ensure asserts are always active, even in Release builds */
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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

/* ========== Tests ========== */

static void test_error_string(void)
{
    /* Every enum value should return a non-NULL string */
    assert(swatcher_error_string(SWATCHER_OK) != NULL);
    assert(swatcher_error_string(SWATCHER_ERR_NULL_ARG) != NULL);
    assert(swatcher_error_string(SWATCHER_ERR_ALLOC) != NULL);
    assert(swatcher_error_string(SWATCHER_ERR_INVALID_PATH) != NULL);
    assert(swatcher_error_string(SWATCHER_ERR_PATH_NOT_FOUND) != NULL);
    assert(swatcher_error_string(SWATCHER_ERR_BACKEND_INIT) != NULL);
    assert(swatcher_error_string(SWATCHER_ERR_BACKEND_NOT_FOUND) != NULL);
    assert(swatcher_error_string(SWATCHER_ERR_THREAD) != NULL);
    assert(swatcher_error_string(SWATCHER_ERR_MUTEX) != NULL);
    assert(swatcher_error_string(SWATCHER_ERR_NOT_INITIALIZED) != NULL);
    assert(swatcher_error_string(SWATCHER_ERR_TARGET_EXISTS) != NULL);
    assert(swatcher_error_string(SWATCHER_ERR_TARGET_NOT_FOUND) != NULL);
    assert(swatcher_error_string(SWATCHER_ERR_PATTERN_COMPILE) != NULL);
    assert(swatcher_error_string(SWATCHER_ERR_WATCH_LIMIT) != NULL);
    assert(swatcher_error_string(SWATCHER_ERR_UNKNOWN) != NULL);

    /* Verify SWATCHER_OK says "No error" */
    assert(strcmp(swatcher_error_string(SWATCHER_OK), "No error") == 0);
}

static void test_error_null_init(void)
{
    /* Passing NULL sw should fail and set error */
    swatcher_config config = { .poll_interval_ms = 100, .enable_logging = false };
    bool ok = swatcher_init(NULL, &config);
    assert(!ok);
    assert(swatcher_last_error() == SWATCHER_ERR_NULL_ARG);
}

static void test_error_null_config(void)
{
    swatcher sw;
    bool ok = swatcher_init(&sw, NULL);
    assert(!ok);
    assert(swatcher_last_error() == SWATCHER_ERR_NULL_ARG);
}

static void test_error_bad_path(void)
{
    /* swatcher_target_create with nonexistent path should fail */
    swatcher_target_desc desc = {
        .path = "/nonexistent/path/that/does/not/exist/anywhere",
        .events = SWATCHER_EVENT_ALL,
        .callback = NULL,
    };

    swatcher_target *target = swatcher_target_create(&desc);
    assert(target == NULL);
    swatcher_error err = swatcher_last_error();
    assert(err == SWATCHER_ERR_PATH_NOT_FOUND || err == SWATCHER_ERR_INVALID_PATH);
}

static void test_error_clears(void)
{
    /* After reading error, next call returns SWATCHER_OK */
    swatcher_init(NULL, NULL);
    swatcher_error first = swatcher_last_error();
    assert(first != SWATCHER_OK);

    swatcher_error second = swatcher_last_error();
    assert(second == SWATCHER_OK);
}

static void test_error_not_initialized(void)
{
    swatcher sw;
    memset(&sw, 0, sizeof(sw));
    sw._internal = NULL;
    bool ok = swatcher_start(&sw);
    assert(!ok);
    assert(swatcher_last_error() == SWATCHER_ERR_NOT_INITIALIZED);
}

/* ========== Main ========== */

int main(void)
{
    printf("=== Error Reporting Tests ===\n");

    RUN_TEST(test_error_string);
    RUN_TEST(test_error_null_init);
    RUN_TEST(test_error_null_config);
    RUN_TEST(test_error_bad_path);
    RUN_TEST(test_error_clears);
    RUN_TEST(test_error_not_initialized);

    printf("\n%d/%d tests passed!\n", tests_passed, tests_run);
    return 0;
}
