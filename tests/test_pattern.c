#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/core/pattern.h"

static int tests_run = 0;
static int tests_passed = 0;

#define RUN_TEST(fn) do { printf("  %-50s", #fn); fn(); tests_run++; tests_passed++; printf("PASS\n"); } while(0)

static void test_regex_basic(void)
{
    char *pats[] = {".*\\.txt$", NULL};
    sw_compiled_patterns *cp = sw_patterns_compile(pats);
    assert(cp != NULL);
    assert(sw_pattern_matched(cp, "hello.txt") == true);
    assert(sw_pattern_matched(cp, "hello.csv") == false);
    assert(sw_pattern_matched(cp, ".txt") == true);
    assert(sw_pattern_matched(cp, "path/to/file.txt") == true);
    sw_patterns_free(cp);
}

static void test_regex_multiple(void)
{
    char *pats[] = {".*\\.txt$", ".*\\.md$", NULL};
    sw_compiled_patterns *cp = sw_patterns_compile(pats);
    assert(cp != NULL);
    assert(sw_pattern_matched(cp, "readme.txt") == true);
    assert(sw_pattern_matched(cp, "readme.md") == true);
    assert(sw_pattern_matched(cp, "readme.csv") == false);
    sw_patterns_free(cp);
}

static void test_null_empty(void)
{
    assert(sw_patterns_compile(NULL) == NULL);
    char *empty[] = {NULL};
    assert(sw_patterns_compile(empty) == NULL);
    assert(sw_pattern_matched(NULL, "test") == false);
}

static void test_glob_to_regex_star(void)
{
    char buf[256];
    assert(sw_glob_to_regex("*.txt", buf, sizeof(buf)) == true);
    assert(strcmp(buf, "^.*\\.txt$") == 0);
}

static void test_glob_to_regex_question(void)
{
    char buf[256];
    assert(sw_glob_to_regex("file?.log", buf, sizeof(buf)) == true);
    assert(strcmp(buf, "^file.\\.log$") == 0);
}

static void test_glob_to_regex_exact(void)
{
    char buf[256];
    assert(sw_glob_to_regex("exact.name", buf, sizeof(buf)) == true);
    assert(strcmp(buf, "^exact\\.name$") == 0);
}

static void test_glob_to_regex_small_buf(void)
{
    char buf[4];
    assert(sw_glob_to_regex("*.txt", buf, sizeof(buf)) == false);
}

static void test_glob_compile_txt(void)
{
    char *pats[] = {"*.txt", NULL};
    sw_compiled_patterns *cp = sw_patterns_compile(pats);
    assert(cp != NULL);
    assert(sw_pattern_matched(cp, "hello.txt") == true);
    assert(sw_pattern_matched(cp, "hello.csv") == false);
    assert(sw_pattern_matched(cp, ".txt") == true);
    sw_patterns_free(cp);
}

static void test_glob_compile_multi(void)
{
    char *pats[] = {"*.c", "*.h", NULL};
    sw_compiled_patterns *cp = sw_patterns_compile(pats);
    assert(cp != NULL);
    assert(sw_pattern_matched(cp, "main.c") == true);
    assert(sw_pattern_matched(cp, "header.h") == true);
    assert(sw_pattern_matched(cp, "main.o") == false);
    sw_patterns_free(cp);
}

static void test_hidden_files(void)
{
    char *pats[] = {".*", NULL};
    sw_compiled_patterns *cp = sw_patterns_compile(pats);
    assert(cp != NULL);
    assert(sw_pattern_matched(cp, ".gitignore") == true);
    assert(sw_pattern_matched(cp, ".hidden") == true);
    assert(sw_pattern_matched(cp, "visible") == false);
    sw_patterns_free(cp);
}

static void test_no_extension(void)
{
    char *pats[] = {"^[^.]+$", NULL};
    sw_compiled_patterns *cp = sw_patterns_compile(pats);
    assert(cp != NULL);
    assert(sw_pattern_matched(cp, "Makefile") == true);
    assert(sw_pattern_matched(cp, "file.txt") == false);
    sw_patterns_free(cp);
}

static void test_nested_dots(void)
{
    char *pats[] = {"*.tar.gz", NULL};
    sw_compiled_patterns *cp = sw_patterns_compile(pats);
    assert(cp != NULL);
    assert(sw_pattern_matched(cp, "archive.tar.gz") == true);
    assert(sw_pattern_matched(cp, "archive.tar.bz2") == false);
    assert(sw_pattern_matched(cp, "file.gz") == false);
    sw_patterns_free(cp);
}

static void test_question_mark_glob(void)
{
    char *pats[] = {"file?.log", NULL};
    sw_compiled_patterns *cp = sw_patterns_compile(pats);
    assert(cp != NULL);
    assert(sw_pattern_matched(cp, "file1.log") == true);
    assert(sw_pattern_matched(cp, "fileA.log") == true);
    assert(sw_pattern_matched(cp, "file12.log") == false);
    assert(sw_pattern_matched(cp, "file.log") == false);
    sw_patterns_free(cp);
}

static void test_regex_passthrough(void)
{
    char *pats[] = {"^.*\\.txt$", NULL};
    sw_compiled_patterns *cp = sw_patterns_compile(pats);
    assert(cp != NULL);
    assert(sw_pattern_matched(cp, "hello.txt") == true);
    assert(sw_pattern_matched(cp, "hello.csv") == false);
    sw_patterns_free(cp);
}

static void test_charclass_passthrough(void)
{
    char *pats[] = {"[abc]\\.txt", NULL};
    sw_compiled_patterns *cp = sw_patterns_compile(pats);
    assert(cp != NULL);
    assert(sw_pattern_matched(cp, "a.txt") == true);
    assert(sw_pattern_matched(cp, "d.txt") == false);
    sw_patterns_free(cp);
}

int main(void)
{
    printf("=== Pattern Tests ===\n");

    RUN_TEST(test_regex_basic);
    RUN_TEST(test_regex_multiple);
    RUN_TEST(test_null_empty);
    RUN_TEST(test_glob_to_regex_star);
    RUN_TEST(test_glob_to_regex_question);
    RUN_TEST(test_glob_to_regex_exact);
    RUN_TEST(test_glob_to_regex_small_buf);
    RUN_TEST(test_glob_compile_txt);
    RUN_TEST(test_glob_compile_multi);
    RUN_TEST(test_hidden_files);
    RUN_TEST(test_no_extension);
    RUN_TEST(test_nested_dots);
    RUN_TEST(test_question_mark_glob);
    RUN_TEST(test_regex_passthrough);
    RUN_TEST(test_charclass_passthrough);

    printf("\n%d/%d tests passed!\n", tests_passed, tests_run);
    return 0;
}
