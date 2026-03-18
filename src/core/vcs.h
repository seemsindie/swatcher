#ifndef SWATCHER_VCS_H
#define SWATCHER_VCS_H

#include <stdbool.h>

/*
 * VCS-aware event pausing.
 *
 * Detects .git/index.lock, .hg/wlock, .svn/wc.db.lock by walking
 * up from the target path. Caches the VCS root so it's only detected once.
 */

/* Forward declaration */
typedef struct swatcher_config swatcher_config;

/* Detect and cache VCS root for a given path. Returns NULL if no VCS found. */
const char *sw_vcs_detect_root(const char *path);

/* Check if a VCS operation is in progress (lock file exists). */
bool sw_vcs_is_locked(const char *vcs_root);

/* Check if an event should be paused due to VCS lock.
 * Returns true if the event should be buffered (VCS locked). */
bool sw_vcs_should_pause(const swatcher_config *config, const char *target_path);

#endif /* SWATCHER_VCS_H */
