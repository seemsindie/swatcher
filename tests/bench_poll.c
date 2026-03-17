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

#define BENCH_DIR "bench_poll_tmp"

static void swap_to_poll_backend(swatcher *sw)
{
    swatcher_internal *si = SW_INTERNAL(sw);
    si->backend->destroy(sw);
    si->backend = swatcher_backend_poll();
    si->backend->init(sw);
}

static void create_dir(const char *path)
{
#ifdef _WIN32
    _mkdir(path);
#else
    mkdir(path, 0755);
#endif
}

static void create_files(const char *dir, int count)
{
    char path[SW_PATH_MAX];
    for (int i = 0; i < count; i++) {
        snprintf(path, sizeof(path), "%s/file_%06d.txt", dir, i);
        FILE *f = fopen(path, "w");
        if (f) {
            fprintf(f, "content %d", i);
            fclose(f);
        }
    }
}

static void remove_files(const char *dir, int count)
{
    char path[SW_PATH_MAX];
    for (int i = 0; i < count; i++) {
        snprintf(path, sizeof(path), "%s/file_%06d.txt", dir, i);
        remove(path);
    }
}

static volatile int event_count = 0;

static void bench_callback(swatcher_fs_event event, swatcher_target *target,
                             const char *event_name, void *additional_data)
{
    (void)event; (void)target; (void)event_name; (void)additional_data;
    event_count++;
}

static void bench_scan(int file_count)
{
    char dir_abs[SW_PATH_MAX];
    sw_path_normalize(BENCH_DIR, dir_abs, sizeof(dir_abs), false);

    create_dir(BENCH_DIR);

    printf("  Creating %d files...", file_count);
    fflush(stdout);
    uint64_t tc1 = sw_time_now_ms();
    create_files(BENCH_DIR, file_count);
    uint64_t tc2 = sw_time_now_ms();
    printf(" done (%lu ms)\n", (unsigned long)(tc2 - tc1));

    swatcher_config config = { .poll_interval_ms = 10, .enable_logging = false };
    swatcher *sw = malloc(sizeof(swatcher));
    swatcher_init(sw, &config);
    swap_to_poll_backend(sw);

    swatcher_target_desc desc = {
        .path = dir_abs,
        .is_recursive = false,
        .events = SWATCHER_EVENT_ALL,
        .watch_options = SWATCHER_WATCH_ALL,
        .follow_symlinks = false,
        .callback = bench_callback,
    };

    swatcher_target *target = swatcher_target_create(&desc);

    /* Benchmark: initial snapshot (happens during add) */
    uint64_t t1 = sw_time_now_ms();
    swatcher_add(sw, target);
    uint64_t t2 = sw_time_now_ms();
    printf("  %6d files: initial snapshot %lu ms\n", file_count, (unsigned long)(t2 - t1));

    /* Benchmark: poll scan cycle (no changes → pure scan overhead) */
    event_count = 0;
    uint64_t t3 = sw_time_now_ms();
    swatcher_start(sw);
    /* Wait for a few poll cycles */
    sw_sleep_ms(500);
    swatcher_stop(sw);
    uint64_t t4 = sw_time_now_ms();
    uint64_t elapsed = t4 - t3;
    /* Estimate cycles: each cycle sleeps ~10ms + scan time */
    int approx_cycles = event_count > 0 ? 1 : (int)(elapsed / 15);
    if (approx_cycles < 1) approx_cycles = 1;
    /* With no changes and short interval, many cycles ran. Overhead ≈ elapsed - (cycles * sleep) */
    /* Better: just report total time and events */
    printf("  %6d files: %lu ms for ~500ms window, %d events (expect 0)\n",
           file_count, (unsigned long)elapsed, event_count);

    swatcher_cleanup(sw);

    printf("  Cleaning up...");
    fflush(stdout);
    uint64_t tr1 = sw_time_now_ms();
    remove_files(BENCH_DIR, file_count);
    rmdir(BENCH_DIR);
    uint64_t tr2 = sw_time_now_ms();
    printf(" done (%lu ms)\n\n", (unsigned long)(tr2 - tr1));
}

int main(void)
{
    printf("=== Poll Backend Benchmark ===\n");
    printf("Measures initial snapshot and steady-state scan overhead.\n\n");

    bench_scan(1000);
    bench_scan(10000);
    bench_scan(100000);

    return 0;
}
