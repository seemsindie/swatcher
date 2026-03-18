#ifndef SWATCHER_RESCAN_H
#define SWATCHER_RESCAN_H

#include "../internal/internal.h"
#include <stdbool.h>

/*
 * Shared rescan logic: walk a directory tree, compare against previous
 * snapshot, and emit synthetic CREATE/DELETE/MODIFY events.
 *
 * The snapshot is a simple array of {path, mtime, size} entries stored
 * in a uthash keyed by path.
 */

typedef struct sw_rescan_entry {
    char *path;             /* hash key */
    uint64_t mtime;
    uint64_t size;
    UT_hash_handle hh;
} sw_rescan_entry;

/* Take a snapshot of a directory tree. Caller must free with sw_rescan_free(). */
sw_rescan_entry *sw_rescan_snapshot(const char *root_path, bool recursive);

/* Free a snapshot */
void sw_rescan_free(sw_rescan_entry *snapshot);

/* Diff old vs new snapshot, calling target->callback for each difference.
 * Events: CREATED (new only), DELETED (old only), MODIFIED (both, mtime/size changed) */
void sw_rescan_diff(sw_rescan_entry *old_snap, sw_rescan_entry *new_snap,
                    swatcher_target *target);

#endif /* SWATCHER_RESCAN_H */
