#include "vcs.h"
#include "../platform/platform.h"
#include "../internal/alloc.h"
#include "swatcher_types.h"
#include <string.h>
#include <stdio.h>

#define VCS_CACHE_MAX 32

typedef enum {
    VCS_NONE = 0,
    VCS_GIT,
    VCS_HG,
    VCS_SVN
} vcs_type;

typedef struct {
    char path[SW_PATH_MAX];
    char vcs_root[SW_PATH_MAX];
    vcs_type type;
} vcs_cache_entry;

static vcs_cache_entry vcs_cache[VCS_CACHE_MAX];
static int vcs_cache_count = 0;

/* Check if a directory contains a VCS dir */
static vcs_type check_vcs_dir(const char *dir)
{
    char check[SW_PATH_MAX];
    sw_file_info info;

    snprintf(check, SW_PATH_MAX, "%s%c.git", dir, sw_path_separator());
    if (sw_stat(check, &info, true) && info.is_directory) return VCS_GIT;

    snprintf(check, SW_PATH_MAX, "%s%c.hg", dir, sw_path_separator());
    if (sw_stat(check, &info, true) && info.is_directory) return VCS_HG;

    snprintf(check, SW_PATH_MAX, "%s%c.svn", dir, sw_path_separator());
    if (sw_stat(check, &info, true) && info.is_directory) return VCS_SVN;

    return VCS_NONE;
}

const char *sw_vcs_detect_root(const char *path)
{
    if (!path) return NULL;

    /* Check cache */
    for (int i = 0; i < vcs_cache_count; i++) {
        size_t plen = strlen(vcs_cache[i].path);
        if (strncmp(path, vcs_cache[i].path, plen) == 0 &&
            (path[plen] == sw_path_separator() || path[plen] == '\0')) {
            return vcs_cache[i].type != VCS_NONE ? vcs_cache[i].vcs_root : NULL;
        }
    }

    /* Walk up from path */
    char dir[SW_PATH_MAX];
    strncpy(dir, path, SW_PATH_MAX - 1);
    dir[SW_PATH_MAX - 1] = '\0';

    char sep = sw_path_separator();

    while (1) {
        vcs_type type = check_vcs_dir(dir);
        if (type != VCS_NONE) {
            /* Cache it */
            if (vcs_cache_count < VCS_CACHE_MAX) {
                strncpy(vcs_cache[vcs_cache_count].path, path, SW_PATH_MAX - 1);
                vcs_cache[vcs_cache_count].path[SW_PATH_MAX - 1] = '\0';
                strncpy(vcs_cache[vcs_cache_count].vcs_root, dir, SW_PATH_MAX - 1);
                vcs_cache[vcs_cache_count].vcs_root[SW_PATH_MAX - 1] = '\0';
                vcs_cache[vcs_cache_count].type = type;
                vcs_cache_count++;
            }
            /* Return the cached entry (stable pointer) */
            return vcs_cache[vcs_cache_count - 1].vcs_root;
        }

        /* Move up one directory */
        char *last_sep = strrchr(dir, sep);
        if (!last_sep || last_sep == dir) break;
        *last_sep = '\0';
    }

    /* Cache negative result */
    if (vcs_cache_count < VCS_CACHE_MAX) {
        strncpy(vcs_cache[vcs_cache_count].path, path, SW_PATH_MAX - 1);
        vcs_cache[vcs_cache_count].path[SW_PATH_MAX - 1] = '\0';
        vcs_cache[vcs_cache_count].vcs_root[0] = '\0';
        vcs_cache[vcs_cache_count].type = VCS_NONE;
        vcs_cache_count++;
    }
    return NULL;
}

bool sw_vcs_is_locked(const char *vcs_root)
{
    if (!vcs_root) return false;

    char lock[SW_PATH_MAX];
    sw_file_info info;
    char sep = sw_path_separator();

    snprintf(lock, SW_PATH_MAX, "%s%c.git%cindex.lock", vcs_root, sep, sep);
    if (sw_stat(lock, &info, true)) return true;

    snprintf(lock, SW_PATH_MAX, "%s%c.hg%cwlock", vcs_root, sep, sep);
    if (sw_stat(lock, &info, true)) return true;

    snprintf(lock, SW_PATH_MAX, "%s%c.svn%cwc.db.lock", vcs_root, sep, sep);
    if (sw_stat(lock, &info, true)) return true;

    return false;
}

bool sw_vcs_should_pause(const swatcher_config *config, const char *target_path)
{
    if (!config || !config->vcs_aware)
        return false;
    const char *vcs_root = sw_vcs_detect_root(target_path);
    if (!vcs_root)
        return false;
    return sw_vcs_is_locked(vcs_root);
}
