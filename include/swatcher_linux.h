#include <sys/inotify.h>
#include <sys/stat.h>
#include <limits.h>
#include <unistd.h>
#include <pthread.h>
#include <dirent.h>
#include <poll.h>
typedef struct swatcher_target_linux
{
    int wd;
    swatcher_target *target;
    UT_hash_handle hh;
} swatcher_target_linux;
typedef struct swatcher_linux
{
    int inotify_fd;
    pthread_t thread;
    int thread_id;
    pthread_mutex_t mutex;
    struct pollfd fds;
    swatcher_target_linux *wd_to_target;
} swatcher_linux;

#define EVENT_SIZE (sizeof(struct inotify_event))
#define BUF_LEN (1024 * (EVENT_SIZE + 16))
#define DIR_BREAK '/'

bool is_already_watched(swatcher *swatcher, const char *path)
{
    swatcher_target *target = NULL;
    HASH_FIND(hh_global, swatcher->targets, path, strlen(path), target);
    return target != NULL;
}

uint32_t swatcher_target_events_to_inotify_mask(swatcher_target *target)
{

    uint32_t inotify_mask = 0;

    if (target->events & SWATCHER_EVENT_ALL_INOTIFY)
    {
        return IN_ALL_EVENTS;
    }

    if (target->events & SWATCHER_EVENT_ALL)
    {
        return (IN_CREATE | IN_MODIFY | IN_DELETE | IN_DELETE_SELF | IN_MOVE | IN_OPEN | IN_CLOSE | IN_ACCESS | IN_ATTRIB);
    }

    if (target->events & SWATCHER_EVENT_CREATED)
    {
        inotify_mask |= IN_CREATE;
    }

    if (target->events & SWATCHER_EVENT_MODIFIED)
    {
        inotify_mask |= IN_MODIFY;
    }

    if (target->events & SWATCHER_EVENT_DELETED)
    {
        inotify_mask |= IN_DELETE | IN_DELETE_SELF;
    }

    if (target->events & SWATCHER_EVENT_MOVED)
    {
        inotify_mask |= IN_MOVE;
    }

    if (target->events & SWATCHER_EVENT_OPENED)
    {
        inotify_mask |= IN_OPEN;
    }

    if (target->events & SWATCHER_EVENT_CLOSED)
    {
        inotify_mask |= IN_CLOSE;
    }

    if (target->events & SWATCHER_EVENT_ACCESSED)
    {
        inotify_mask |= IN_ACCESS;
    }

    if (target->events & SWATCHER_EVENT_ATTRIB_CHANGE)
    {
        inotify_mask |= IN_ATTRIB;
    }

    return inotify_mask;
}

swatcher_fs_event convert_inotify_event_to_swatcher_event(uint32_t inotify_event_mask)
{
    if (inotify_event_mask & IN_CREATE)
    {
        return SWATCHER_EVENT_CREATED;
    }

    if (inotify_event_mask & IN_MODIFY)
    {
        return SWATCHER_EVENT_MODIFIED;
    }

    if ((inotify_event_mask & IN_DELETE))
    {
        return SWATCHER_EVENT_DELETED;
    }

    if (inotify_event_mask & IN_MOVE)
    {
        return SWATCHER_EVENT_MOVED;
    }

    if (inotify_event_mask & IN_OPEN)
    {
        return SWATCHER_EVENT_OPENED;
    }

    if (inotify_event_mask & IN_CLOSE)
    {
        return SWATCHER_EVENT_CLOSED;
    }

    if (inotify_event_mask & IN_ACCESS)
    {
        return SWATCHER_EVENT_ACCESSED;
    }

    if (inotify_event_mask & IN_ATTRIB)
    {
        return SWATCHER_EVENT_ATTRIB_CHANGE;
    }

    return SWATCHER_EVENT_NONE;
}

const char *get_event_name_from_inotify_mask(uint32_t inotify_event_mask)
{
    switch (inotify_event_mask)
    {
    case IN_CREATE:
        return "IN_CREATE";
    case IN_MODIFY:
        return "IN_MODIFY";
    case IN_DELETE:
        return "IN_DELETE";
    case IN_DELETE_SELF:
        return "IN_DELETE_SELF";
    case IN_MOVE_SELF:
        return "IN_MOVE_SELF";
    case IN_MOVED_FROM:
        return "IN_MOVED_FROM";
    case IN_MOVED_TO:
        return "IN_MOVED_TO";
    case IN_CLOSE_WRITE:
        return "IN_CLOSE_WRITE";
    case IN_CLOSE_NOWRITE:
        return "IN_CLOSE_NOWRITE";
    case IN_OPEN:
        return "IN_OPEN";
    case IN_ACCESS:
        return "IN_ACCESS";
    case IN_ATTRIB:
        return "IN_ATTRIB";
    case IN_IGNORED:
        return "IN_IGNORED";
    case IN_ISDIR:
        return "IN_ISDIR";
    case IN_Q_OVERFLOW:
        return "IN_Q_OVERFLOW";
    case IN_UNMOUNT:
        return "IN_UNMOUNT";
    default:
        return "Unknown event";
    }
}

bool add_watch(swatcher *swatcher, swatcher_target *target)
{
    if (is_already_watched(swatcher, target->path))
    {
        SWATCHER_LOG_DEFAULT_WARNING("Path already watched: %s", target->path);
        return false;
    }
    else
    {
        SWATCHER_LOG_DEFAULT_INFO("Target %s is being watched", target->path);
    }
    swatcher_linux *sw_linux = (swatcher_linux *)swatcher->platform_data;
    swatcher_target_linux *sw_target_linux = malloc(sizeof(swatcher_target_linux));
    if (sw_target_linux == NULL)
    {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to allocate swatcher_target_linux");
        return false;
    }

    sw_target_linux->target = target;
    sw_target_linux->wd = inotify_add_watch(sw_linux->inotify_fd, target->path, swatcher_target_events_to_inotify_mask(target));
    if (sw_target_linux->wd < 0)
    {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to add watch for %s", target->path);
        free(sw_target_linux);
        return false;
    }

    HASH_ADD_INT(sw_linux->wd_to_target, wd, sw_target_linux);
    target->platform_data = sw_target_linux;

    swatcher_target *found_target = NULL;
    HASH_FIND(hh_global, swatcher->targets, target->path, strlen(target->path), found_target);
    if (found_target == NULL)
    {
        HASH_ADD_KEYPTR(hh_global, swatcher->targets, target->path, strlen(target->path), target);
    }

    return true;
}

bool add_watch_recursive_locked(swatcher *swatcher, swatcher_target *original_target, bool dont_add_watch)
{
    swatcher_linux *sw_linux = (swatcher_linux *)swatcher->platform_data;
    swatcher_target *target = original_target;

    if (target->is_file)
    {
        return add_watch(swatcher, target);
    }

    DIR *dir = opendir(target->path);
    if (dir == NULL)
    {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to open directory: %s", target->path);
        return false;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL)
    {

        if (original_target->ignore_patterns != NULL)
        {
            if (is_pattern_matched(original_target->ignore_patterns, entry->d_name))
            {
                continue;
            }
        }

        if (!original_target->watch_patterns || is_pattern_matched(original_target->watch_patterns, entry->d_name) || entry->d_type == DT_DIR)
        {

            if (entry->d_type == DT_DIR)
            {
                // if (target->watch_options & SWATCHER_WATCH_DIRECTORIES || target->watch_options == SWATCHER_WATCH_ALL)
                // {
                if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                {
                    continue;
                }

                char new_path[PATH_MAX];
                // snprintf(new_path, PATH_MAX, "%s/%s", target->path, entry->d_name);

                // trailing slash check
                if (original_target->path[strlen(original_target->path) - 1] == '/')
                {
                    snprintf(new_path, PATH_MAX, "%s%s", original_target->path, entry->d_name);
                }
                else
                {
                    snprintf(new_path, PATH_MAX, "%s/%s", original_target->path, entry->d_name);
                }

                swatcher_target_desc new_target_desc = {
                    .path = new_path,
                    .is_recursive = target->is_recursive,
                    .events = target->events,
                    .watch_options = target->watch_options,
                    .follow_symlinks = target->follow_symlinks,
                    .user_data = target->user_data,
                    .callback_patterns = target->callback_patterns,
                    .watch_patterns = target->watch_patterns,
                    .ignore_patterns = target->ignore_patterns,
                    .callback = target->callback};

                swatcher_target *new_target = swatcher_target_create(&new_target_desc);
                if (new_target == NULL)
                {
                    SWATCHER_LOG_DEFAULT_WARNING("Failed to create new target for %s", new_path);
                    continue;
                }

                if (!add_watch_recursive_locked(swatcher, new_target, target->watch_options == SWATCHER_WATCH_FILES || target->watch_options == SWATCHER_WATCH_SYMLINKS || (original_target->watch_patterns && !is_pattern_matched(original_target->watch_patterns, entry->d_name))))
                {
                    SWATCHER_LOG_DEFAULT_WARNING("Failed to add watch for %s", new_target->path);
                    free(new_target->path);
                    free(new_target);
                    continue;
                }

                // HASH_ADD_KEYPTR(hh_inner, target->inner_targets, new_target->path, strlen(new_target->path), new_target);
                // }
            }
            else if (entry->d_type == DT_REG) // just a file
            {
                if (target->watch_options & SWATCHER_WATCH_FILES || target->watch_options == SWATCHER_WATCH_ALL)
                {
                    char new_path[PATH_MAX];
                    // snprintf(new_path, PATH_MAX, "%s/%s", target->path, entry->d_name);

                    // trailing slash check
                    if (original_target->path[strlen(original_target->path) - 1] == '/')
                    {
                        snprintf(new_path, PATH_MAX, "%s%s", original_target->path, entry->d_name);
                    }
                    else
                    {
                        snprintf(new_path, PATH_MAX, "%s/%s", original_target->path, entry->d_name);
                    }

                    swatcher_target_desc new_target_desc = {
                        .path = new_path,
                        .is_recursive = target->is_recursive,
                        .events = target->events,
                        .watch_options = target->watch_options,
                        .follow_symlinks = target->follow_symlinks,
                        .user_data = target->user_data,
                        .callback_patterns = target->callback_patterns,
                        .watch_patterns = target->watch_patterns,
                        .ignore_patterns = target->ignore_patterns,
                        .callback = target->callback};

                    swatcher_target *new_target = swatcher_target_create(&new_target_desc);
                    if (new_target == NULL)
                    {
                        SWATCHER_LOG_DEFAULT_WARNING("Failed to create new target for %s", new_path);
                        continue;
                    }

                    if (!add_watch(swatcher, new_target))
                    {
                        SWATCHER_LOG_DEFAULT_WARNING("Failed to add watch (DT_REG) for %s", new_target->path);
                        free(new_target->path);
                        free(new_target);
                        continue;
                    }

                    // HASH_ADD_KEYPTR(hh_inner, target->inner_targets, new_target->path, strlen(new_target->path), new_target);
                }
            }
            else if (entry->d_type == DT_LNK)
            {
                if (target->follow_symlinks)
                {
                    char new_path[PATH_MAX];
                    // snprintf(new_path, PATH_MAX, "%s/%s", target->path, entry->d_name);

                    // trailing slash check
                    if (original_target->path[strlen(original_target->path) - 1] == '/')
                    {
                        snprintf(new_path, PATH_MAX, "%s%s", original_target->path, entry->d_name);
                    }
                    else
                    {
                        snprintf(new_path, PATH_MAX, "%s/%s", original_target->path, entry->d_name);
                    }

                    swatcher_target_desc new_target_desc = {
                        .path = new_path,
                        .is_recursive = target->is_recursive,
                        .events = target->events,
                        .watch_options = target->watch_options,
                        .follow_symlinks = target->follow_symlinks,
                        .user_data = target->user_data,
                        .callback_patterns = target->callback_patterns,
                        .watch_patterns = target->watch_patterns,
                        .ignore_patterns = target->ignore_patterns,
                        .callback = target->callback};

                    swatcher_target *new_target = swatcher_target_create(&new_target_desc);
                    if (new_target == NULL)
                    {
                        SWATCHER_LOG_DEFAULT_ERROR("Failed to create new target for %s", new_path);
                        continue;
                    }

                    if (new_target->is_file)
                    {
                        if (original_target->watch_options & SWATCHER_WATCH_FILES || original_target->watch_options == SWATCHER_WATCH_ALL)
                        {
                            if (!add_watch(swatcher, new_target))
                            {
                                SWATCHER_LOG_DEFAULT_WARNING("Failed to add watch (DT_LNK) for %s", new_target->path);
                                free(new_target->path);
                                free(new_target);
                                continue;
                            }
                        }
                        else
                        {
                            // SWATCHER_LOG_DEFAULT_WARNING("Not adding watch (DT_LNK, file) for %s", new_target->path);
                            free(new_target->path);
                            free(new_target);
                            continue;
                        }
                    }
                    else
                    {
                        if (original_target->watch_options & SWATCHER_WATCH_DIRECTORIES || original_target->watch_options == SWATCHER_WATCH_ALL)
                        {
                            if (!add_watch_recursive_locked(swatcher, new_target, false))
                            {
                                SWATCHER_LOG_DEFAULT_WARNING("Failed to add watch (DT_LNK, dir) for %s", new_target->path);
                                free(new_target->path);
                                free(new_target);
                                continue;
                            }
                        }
                        else
                        {
                            // SWATCHER_LOG_DEFAULT_WARNING("Not adding watch (DT_LNK, dir) for %s", new_target->path);
                            free(new_target->path);
                            free(new_target);
                            continue;
                        }
                    }

                    // HASH_ADD_KEYPTR(hh_inner, target->inner_targets, new_target->path, strlen(new_target->path), new_target);
                }
                else
                {
                    if (original_target->watch_options & SWATCHER_WATCH_SYMLINKS || original_target->watch_options == SWATCHER_WATCH_ALL)
                    {
                        char new_path[PATH_MAX];
                        // snprintf(new_path, PATH_MAX, "%s/%s", target->path, entry->d_name);

                        // trailing slash check
                        if (original_target->path[strlen(original_target->path) - 1] == '/')
                        {
                            snprintf(new_path, PATH_MAX, "%s%s", original_target->path, entry->d_name);
                        }
                        else
                        {
                            snprintf(new_path, PATH_MAX, "%s/%s", original_target->path, entry->d_name);
                        }

                        // just add a watch for the symlink itself
                        swatcher_target_desc new_target_desc = {
                            .path = new_path,
                            .is_recursive = original_target->is_recursive,
                            .events = original_target->events,
                            .watch_options = original_target->watch_options,
                            .follow_symlinks = original_target->follow_symlinks,
                            .user_data = original_target->user_data,
                            .callback_patterns = original_target->callback_patterns,
                            .watch_patterns = original_target->watch_patterns,
                            .ignore_patterns = original_target->ignore_patterns,
                            .callback = original_target->callback};

                        swatcher_target *new_target = swatcher_target_create(&new_target_desc);
                        if (new_target == NULL)
                        {
                            SWATCHER_LOG_DEFAULT_ERROR("Failed to create new target for %s", original_target->path);
                            continue;
                        }

                        if (!add_watch(swatcher, new_target))
                        {
                            SWATCHER_LOG_DEFAULT_WARNING("Failed to add watch (DT_LNK) for %s", new_target->path);
                            free(new_target->path);
                            free(new_target);
                            continue;
                        }
                    }
                }
            }
        }
    }

    closedir(dir);

    if (dont_add_watch)
    {
        return true;
    }

    return add_watch(swatcher, target);
}

bool add_watch_recursive(swatcher *swatcher, swatcher_target *target, bool dont_add_watch)
{
    pthread_mutex_lock(&((swatcher_linux *)swatcher->platform_data)->mutex);

    bool result = add_watch_recursive_locked(swatcher, target, dont_add_watch);

    pthread_mutex_unlock(&((swatcher_linux *)swatcher->platform_data)->mutex);

    return result;
}

bool swatcher_add(swatcher *swatcher, swatcher_target *target)
{

    bool result = false;

    if (!target->is_file && target->is_recursive)
    {
        // don't add dirs to watch recursively if watch option is just files or links
        result = add_watch_recursive(swatcher, target, target->watch_options == SWATCHER_WATCH_FILES || target->watch_options == SWATCHER_WATCH_SYMLINKS || target->watch_patterns != NULL);
    }
    else
    {
        pthread_mutex_lock(&((swatcher_linux *)swatcher->platform_data)->mutex);
        result = add_watch(swatcher, target);
        pthread_mutex_unlock(&((swatcher_linux *)swatcher->platform_data)->mutex);
    }

    return result;
}

bool handle_create_event(swatcher *swatcher, swatcher_target *original_target, struct inotify_event *event)
{
    char new_path[PATH_MAX];
    snprintf(new_path, PATH_MAX, "%s/%s", original_target->path, event->name);

    bool is_dir = event->mask & IN_ISDIR;
    bool should_watch = false;

    if (is_dir && (original_target->watch_options & SWATCHER_WATCH_DIRECTORIES || original_target->watch_options == SWATCHER_WATCH_ALL))
    {
        should_watch = true;
    }
    else if (!is_dir && (original_target->watch_options & SWATCHER_WATCH_FILES || original_target->watch_options == SWATCHER_WATCH_ALL))
    {
        should_watch = true;
    }

    bool is_watching_already = is_already_watched(swatcher, new_path); // || is_already_watched(swatcher, new_path);

    // SWATCHER_LOG_DEBUG(swatcher, "handle_create_event: %s, is_dir: %d, should_watch: %d, is_watching_already: %d", new_path, is_dir, should_watch, is_watching_already);

    if (should_watch && !is_watching_already)
    {
        swatcher_target_desc new_target_desc = {
            .path = new_path,
            .is_recursive = original_target->is_recursive,
            .events = original_target->events,
            .watch_options = original_target->watch_options,
            .user_data = original_target->user_data,
            .pattern = original_target->pattern,
            .callback = original_target->callback};

        swatcher_target *new_target = swatcher_target_create(&new_target_desc);
        if (new_target == NULL)
        {
            SWATCHER_LOG_DEFAULT_ERROR("Failed to create new target for %s", new_path);

            free(new_target->path);
            free(new_target);

            return false;
        }

        if (!add_watch_recursive_locked(swatcher, new_target, false))
        {
            SWATCHER_LOG_DEFAULT_ERROR("Failed to add watch for %s", new_target->path);
            free(new_target->path);
            free(new_target);
            return false;
        }

        // SWATCHER_LOG_DEBUG(swatcher, "Added watch for %s", new_target->path);

        return true;
    }
}

void *swatcher_watcher_thread(void *arg)
{
    swatcher *swatcher_instance = (swatcher *)arg;
    swatcher_linux *sw_linux = (swatcher_linux *)swatcher_instance->platform_data;
    char buffer[BUF_LEN];
    char full_path[PATH_MAX];

    while (swatcher_instance->running)
    {
        int poll_ret = poll(&sw_linux->fds, 1, swatcher_instance->config->poll_interval_ms);
        if (poll_ret < 0)
        {
            SWATCHER_LOG_DEFAULT_ERROR("poll failed");
            break;
        }

        if (poll_ret == 0)
        {
            // Timeout, continue to next iteration
            continue;
        }

        int length = read(sw_linux->inotify_fd, buffer, BUF_LEN);
        if (length < 0)
        {
            SWATCHER_LOG_ERROR(swatcher_instance, "Error reading inotify FD");
            break;
        }

        int i = 0;
        while (i < length)
        {
            struct inotify_event *event = (struct inotify_event *)&buffer[i];
            swatcher_target_linux *target_data = NULL;

            HASH_FIND_INT(sw_linux->wd_to_target, &event->wd, target_data);
            if (target_data == NULL)
            {
                // SWATCHER_LOG_WARNING(swatcher_instance, "Failed to find target for wd: %d", event->wd);
                i += sizeof(struct inotify_event) + event->len;
                continue;
            }

            swatcher_target *target = target_data->target;

            // TODO: move this into a separate function
            // if (event->mask & IN_ISDIR) {
            //     if (event->mask & IN_CREATE) {
            //         snprintf(full_path, PATH_MAX, "%s/%s", target->path, event->name);
            //         swatcher_target_desc new_target_desc = {
            //             .path = full_path,
            //             .is_recursive = target->is_recursive,
            //             .events = target->events,
            //             .watch_options = target->watch_options,
            //             .user_data = target->user_data,
            //             .pattern = target->pattern,
            //             .callback = target->callback
            //         };
            //         swatcher_target *new_target = swatcher_target_create(&new_target_desc);
            //         if (new_target == NULL) {
            //             SWATCHER_LOG_ERROR(swatcher_instance, "Failed to create new target for %s", full_path);
            //             i += sizeof(struct inotify_event) + event->len;
            //             continue;
            //         }

            //         if (!add_watch_recursive(swatcher_instance, new_target, false)) {
            //             SWATCHER_LOG_ERROR(swatcher_instance, "Failed to add watch for %s", new_target->path);
            //             free(new_target->path);
            //             free(new_target);
            //             i += sizeof(struct inotify_event) + event->len;
            //             continue;
            //         }
            //     }
            // }

            swatcher_fs_event sw_event = convert_inotify_event_to_swatcher_event(event->mask);
            // SWATCHER_LOG_DEBUG(swatcher_instance, "Event: %s", get_event_name_from_inotify_mask(event->mask));
            // SWATCHER_LOG_DEBUG(swatcher_instance, "Event len: %d", event->len);
            // SWATCHER_LOG_DEBUG(swatcher_instance, "Event wd: %d", event->wd);
            // SWATCHER_LOG_DEBUG(swatcher_instance, "Event mask: %d", event->mask);
            // SWATCHER_LOG_DEBUG(swatcher_instance, "Event cookie: %d", event->cookie);
            // SWATCHER_LOG_DEBUG(swatcher_instance, "Event name: %s", event->len > 0 ? event->name : "NULL");
            // SWATCHER_LOG_DEBUG(swatcher_instance, "Target path: %s", target->path);
            if (sw_event != SWATCHER_EVENT_NONE)
            {
                if ((target->events & sw_event) || (target->events == SWATCHER_EVENT_ALL))
                {
                    if (target->callback_patterns != NULL && event->len > 0 && event->name != 0)
                    {
                        bool res = is_pattern_matched(target->callback_patterns, event->len > 0 ? event->name : NULL);
                        if (res)
                        {
                            target->callback(sw_event, target, event->len > 0 ? event->name : NULL, event);
                            target->last_event_time = time(NULL);
                        }
                    }
                    else
                    {
                        if (target->callback_patterns == NULL)
                        {
                            target->callback(sw_event, target, event->len > 0 ? event->name : NULL, event);
                            target->last_event_time = time(NULL);
                        }
                    }
                }
            }

            i += sizeof(struct inotify_event) + event->len;
        }
    }

    SWATCHER_LOG_INFO(swatcher_instance, "Watcher thread exiting...");
    return NULL;
}

bool swatcher_is_absolute_path(const char *path)
{
    if (path == NULL)
    {
        return false;
    }
    return path[0] == '/';
}

bool swatcher_validate_and_normalize_path(const char *input_path, char *normalized_path, bool resolve_symlinks)
{
    if (!resolve_symlinks)
    {
        // When not resolving symlinks, just check if the path is absolute
        // and copy it directly or prepend the current working directory.
        if (swatcher_is_absolute_path(input_path))
        {
            strncpy(normalized_path, input_path, PATH_MAX);
            normalized_path[PATH_MAX - 1] = '\0'; // Ensure null-termination
        }
        else
        {
            char cwd[PATH_MAX];
            if (getcwd(cwd, sizeof(cwd)) != NULL)
            {
                snprintf(normalized_path, PATH_MAX, "%s/%s", cwd, input_path);
            }
            else
            {
                SWATCHER_LOG_DEFAULT_ERROR("Failed to get current working directory");
                return false;
            }
        }
    }
    else
    {
        // Resolve the path fully, including symlinks.
        if (!swatcher_is_absolute_path(input_path))
        {
            char cwd[PATH_MAX];
            if (getcwd(cwd, sizeof(cwd)) != NULL)
            {
                char temp_path[PATH_MAX];
                snprintf(temp_path, PATH_MAX, "%s/%s", cwd, input_path);
                if (realpath(temp_path, normalized_path) == NULL)
                {
                    SWATCHER_LOG_DEFAULT_ERROR("Failed to get real path for %s", temp_path);
                    return false;
                }
            }
            else
            {
                SWATCHER_LOG_DEFAULT_ERROR("Failed to get current working directory");
                return false;
            }
        }
        else
        {
            if (realpath(input_path, normalized_path) == NULL)
            {
                SWATCHER_LOG_DEFAULT_ERROR("Failed to get real path for %s", input_path);
                return false;
            }
        }
    }
    return true;
}

bool swatcher_init(swatcher *swatcher, swatcher_config *config)
{
    if (!swatcher)
    {
        SWATCHER_LOG_DEFAULT_ERROR("swatcher is NULL\n");
        return false;
    }

    if (!config)
    {
        SWATCHER_LOG_DEFAULT_ERROR("config is NULL\n");
        return false;
    }

    swatcher->platform_data = malloc(sizeof(swatcher_linux));
    if (swatcher->platform_data == NULL)
    {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to allocate swatcher_linux");
        return false;
    }

    swatcher_linux *sw_linux = (swatcher_linux *)swatcher->platform_data;
    swatcher->running = false;
    sw_linux->thread_id = -1;

    if (pthread_mutex_init(&sw_linux->mutex, NULL) != 0)
    {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to initialize mutex");
        free(swatcher->platform_data);
        return false;
    }

    sw_linux->inotify_fd = inotify_init();
    if (sw_linux->inotify_fd < 0)
    {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to initialize inotify");
        free(swatcher->platform_data);
        return false;
    }

    sw_linux->fds.fd = sw_linux->inotify_fd;
    sw_linux->fds.events = POLLIN;
    sw_linux->wd_to_target = NULL;

    swatcher->targets = NULL;
    swatcher->config = config;
    swatcher->targets = NULL;

    return true;
}

bool swatcher_start(swatcher *swatcher)
{
    if (!swatcher || !swatcher->platform_data)
    {
        fprintf(stderr, "swatcher not initialized\n");
        return false;
    }

    swatcher_linux *sw_linux = (swatcher_linux *)swatcher->platform_data;

    // initialize watcher thread
    int ret = pthread_create(&sw_linux->thread, NULL, swatcher_watcher_thread, swatcher);
    if (ret != 0)
    {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to create watcher thread");
        close(sw_linux->inotify_fd);
        free(swatcher->platform_data);
        return false;
    }

    swatcher->running = true;
    sw_linux->thread_id = ret;

    return true;
}

void swatcher_stop(swatcher *swatcher)
{
    swatcher_linux *sw_linux = (swatcher_linux *)swatcher->platform_data;
    swatcher->running = false;

    // stop watcher thread
    pthread_join(sw_linux->thread, NULL);

    // close inotify
    close(sw_linux->inotify_fd);
}

// TODO: check if null before freeing
void swatcher_cleanup(swatcher *swatcher)
{
    swatcher_target *current, *tmp;
    swatcher_linux *sw_linux = (swatcher_linux *)swatcher->platform_data;

    HASH_ITER(hh_global, swatcher->targets, current, tmp)
    {
        swatcher_target_linux *sw_target_linux = (swatcher_target_linux *)current->platform_data;
        if (sw_target_linux)
        {
            inotify_rm_watch(sw_linux->inotify_fd, sw_target_linux->wd);
            HASH_DEL(sw_linux->wd_to_target, sw_target_linux);
            free(sw_target_linux);
        }

        // delete inner targets
        // swatcher_target *inner_current, *inner_tmp;
        // HASH_ITER(hh_inner, current->inner_targets, inner_current, inner_tmp)
        // {
        //     // todo
        // }

        HASH_DELETE(hh_global, swatcher->targets, current);
        free(current->path);
        // free(current->callback_patterns);
        // free(current->watch_patterns);
        // free(current->ignore_patterns);
        free(current);
    }

    pthread_mutex_destroy(&sw_linux->mutex);
    free(swatcher->platform_data);
    free(swatcher);
}

bool swatcher_remove(swatcher *swatcher, swatcher_target *target)
{
    swatcher_linux *sw_linux = (swatcher_linux *)swatcher->platform_data;
    pthread_mutex_lock(&sw_linux->mutex);

    swatcher_target_linux *sw_target_linux = (swatcher_target_linux *)target->platform_data;
    if (sw_target_linux)
    {
        inotify_rm_watch(sw_linux->inotify_fd, sw_target_linux->wd);

        HASH_DEL(sw_linux->wd_to_target, sw_target_linux);

        free(sw_target_linux);
        target->platform_data = NULL;
    }

    swatcher_target *found_target = NULL;
    HASH_FIND(hh_global, swatcher->targets, target->path, strlen(target->path), found_target);
    if (found_target)
    {
        // Find and delete inner targets
        // swatcher_target *inner_current, *inner_tmp;
        // HASH_ITER(hh_inner, found_target->inner_targets, inner_current, inner_tmp)
        // {
        //     todo
        // }

        HASH_DELETE(hh_global, swatcher->targets, found_target);
        free(found_target->path);
        // free(found_target->callback_patterns);
        // free(found_target->watch_patterns);
        // free(found_target->ignore_patterns);
        free(found_target);
    }

    pthread_mutex_unlock(&sw_linux->mutex);
    return true;
}

swatcher_target *swatcher_target_create(swatcher_target_desc *desc)
{
    swatcher_target *target = malloc(sizeof(swatcher_target));
    if (target == NULL)
    {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to allocate swatcher_target");
        return NULL;
    }

    char normalized_path[PATH_MAX];
    if (!swatcher_validate_and_normalize_path(desc->path, normalized_path, desc->follow_symlinks))
    {
        free(target);
        return NULL;
    }

    struct stat path_stat;
    int stat_result;

    if (desc->follow_symlinks)
    {
        stat_result = stat(normalized_path, &path_stat);
    }
    else
    {
        stat_result = lstat(normalized_path, &path_stat);
        target->is_symlink = S_ISLNK(path_stat.st_mode);
    }

    if (stat_result != 0)
    {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to stat path: %s", normalized_path);
        free(target);
        return NULL;
    }

    target->is_file = S_ISREG(path_stat.st_mode);
    target->is_directory = S_ISDIR(path_stat.st_mode);

    // SWATCHER_LOG_DEFAULT_DEBUG("Path: %s, is_file: %d, is_directory: %d, is_symlink: %d", normalized_path, target->is_file, target->is_directory, target->is_symlink);

    target->path = strdup(normalized_path);
    if (target->path == NULL)
    {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to allocate path (string)");
        free(target);
        return NULL;
    }

    // if (desc->pattern == 0)
    // {
    //     target->pattern = NULL;
    // }
    // else
    // {
    //     target->pattern = strdup(desc->pattern);
    //     if (target->pattern == NULL)
    //     {
    //         SWATCHER_LOG_DEFAULT_ERROR("Failed to allocate pattern (regex string)");
    //         free(target->path);
    //         free(target);
    //         return NULL;
    //     }
    // }
    // char **callback_patterns; // regex patterns for callback triggering

    if (desc->callback_patterns == 0)
    {
        target->callback_patterns = NULL;
    }
    else
    {
        target->callback_patterns = desc->callback_patterns;
    }

    if (desc->watch_patterns == 0)
    {
        target->watch_patterns = NULL;
    }
    else
    {
        target->watch_patterns = desc->watch_patterns;
    }

    if (desc->ignore_patterns == 0)
    {
        target->ignore_patterns = NULL;
    }
    else
    {
        target->ignore_patterns = desc->ignore_patterns;
    }

    // watch options if NULL then default to SWATCHER_WATCH_ALL
    if (desc->watch_options == 0)
    {
        target->watch_options = SWATCHER_WATCH_ALL;
    }
    else
    {
        target->watch_options = desc->watch_options;
    }

    target->is_recursive = desc->is_recursive;
    target->events = desc->events;
    target->watch_options = desc->watch_options;
    target->user_data = desc->user_data;
    target->callback = desc->callback;
    target->last_event_time = time(NULL);
    target->inner_targets = NULL;
    target->follow_symlinks = desc->follow_symlinks;

    // SWATCHER_LOG_DEFAULT_DEBUG("Created target: %s", target->path);

    return target;
}
