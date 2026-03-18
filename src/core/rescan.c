#include "rescan.h"

#include <stdio.h>
#include <string.h>

static void rescan_walk(const char *dir_path, bool recursive, sw_rescan_entry **snapshot)
{
    sw_dir *dir = sw_dir_open(dir_path);
    if (!dir) return;

    sw_dir_entry entry;
    char child_path[SW_PATH_MAX];
    char sep = sw_path_separator();

    while (sw_dir_next(dir, &entry)) {
        if (strcmp(entry.name, ".") == 0 || strcmp(entry.name, "..") == 0)
            continue;

        size_t dlen = strlen(dir_path);
        if (dlen > 0 && dir_path[dlen - 1] == sep)
            snprintf(child_path, SW_PATH_MAX, "%s%s", dir_path, entry.name);
        else
            snprintf(child_path, SW_PATH_MAX, "%s%c%s", dir_path, sep, entry.name);

        if (entry.is_dir && recursive) {
            rescan_walk(child_path, true, snapshot);
            continue;
        }

        if (entry.is_dir)
            continue;

        sw_file_info info;
        if (!sw_stat(child_path, &info, true))
            continue;

        sw_rescan_entry *re = sw_malloc(sizeof(sw_rescan_entry));
        if (!re) continue;
        re->path = sw_strdup(child_path);
        if (!re->path) { sw_free(re); continue; }
        re->mtime = info.mtime;
        re->size = info.size;
        HASH_ADD_STR(*snapshot, path, re);
    }

    sw_dir_close(dir);
}

sw_rescan_entry *sw_rescan_snapshot(const char *root_path, bool recursive)
{
    sw_rescan_entry *snapshot = NULL;
    rescan_walk(root_path, recursive, &snapshot);
    return snapshot;
}

void sw_rescan_free(sw_rescan_entry *snapshot)
{
    sw_rescan_entry *current, *tmp;
    HASH_ITER(hh, snapshot, current, tmp) {
        HASH_DEL(snapshot, current);
        sw_free(current->path);
        sw_free(current);
    }
}

void sw_rescan_diff(sw_rescan_entry *old_snap, sw_rescan_entry *new_snap,
                    swatcher_target *target)
{
    if (!target || !target->callback)
        return;

    /* Find new entries (in new but not old) -> CREATED */
    sw_rescan_entry *ne, *ne_tmp;
    HASH_ITER(hh, new_snap, ne, ne_tmp) {
        sw_rescan_entry *old = NULL;
        if (old_snap)
            HASH_FIND_STR(old_snap, ne->path, old);
        if (!old) {
            if ((target->events & SWATCHER_EVENT_CREATED) || target->events == SWATCHER_EVENT_ALL) {
                swatcher_event_info info = { .old_path = NULL, .is_dir = false };
                const char *name = strrchr(ne->path, sw_path_separator());
                name = name ? name + 1 : ne->path;
                target->callback(SWATCHER_EVENT_CREATED, target, name, &info);
            }
        } else if (old->mtime != ne->mtime || old->size != ne->size) {
            if ((target->events & SWATCHER_EVENT_MODIFIED) || target->events == SWATCHER_EVENT_ALL) {
                swatcher_event_info info = { .old_path = NULL, .is_dir = false };
                const char *name = strrchr(ne->path, sw_path_separator());
                name = name ? name + 1 : ne->path;
                target->callback(SWATCHER_EVENT_MODIFIED, target, name, &info);
            }
        }
    }

    /* Find deleted entries (in old but not new) -> DELETED */
    sw_rescan_entry *oe, *oe_tmp;
    HASH_ITER(hh, old_snap, oe, oe_tmp) {
        sw_rescan_entry *found = NULL;
        if (new_snap)
            HASH_FIND_STR(new_snap, oe->path, found);
        if (!found) {
            if ((target->events & SWATCHER_EVENT_DELETED) || target->events == SWATCHER_EVENT_ALL) {
                swatcher_event_info info = { .old_path = NULL, .is_dir = false };
                const char *name = strrchr(oe->path, sw_path_separator());
                name = name ? name + 1 : oe->path;
                target->callback(SWATCHER_EVENT_DELETED, target, name, &info);
            }
        }
    }
}
