#include <sys/inotify.h>
#include <limits.h>
#include <unistd.h>
#include <pthread.h>
#include <dirent.h>
#include <poll.h>
#include <sys/stat.h> // Include for struct stat and lstat

typedef struct swatcher_linux
{
    int inotify_fd;
    pthread_t thread;
    int thread_id;
    pthread_mutex_t mutex;
    bool running;
    struct pollfd fds;
} swatcher_linux;

typedef struct swatcher_target_linux
{
    int wd;
} swatcher_target_linux;

#define EVENT_SIZE (sizeof(struct inotify_event))
#define BUF_LEN (1024 * (EVENT_SIZE + 16))
#define DIR_BREAK '/'

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

    if ((inotify_event_mask & IN_DELETE) || (inotify_event_mask & IN_DELETE_SELF))
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

void *swatcher_watcher_thread(void *arg)
{
    swatcher *swatcher_instance = (swatcher *)arg;
    swatcher_linux *sw_linux = (swatcher_linux *)swatcher_instance->platform_data;

    char buffer[BUF_LEN];

    while (sw_linux->running)
    {
        int poll_ret = poll(&sw_linux->fds, 1, swatcher_instance->config->poll_interval_ms); // 1 second timeout

        if (poll_ret < 0)
        {
            SWATCHER_LOG_DEFAULT_ERROR("poll failed");
            break;
        }

        if (poll_ret == 0)
        {
            // Timeout
            continue;
        }

        if (poll_ret > 0)
        {
            int length = read(sw_linux->inotify_fd, buffer, BUF_LEN);

            if (length < 0)
            {
                if (errno == EINTR)
                {
                    // Interrupted by a signal, try again
                    continue;
                }

                SWATCHER_LOG_ERROR(swatcher_instance, "Error reading inotify FD");
                break;
            }

            int i = 0;
            while (i < length)
            {
                struct inotify_event *event = (struct inotify_event *)&buffer[i];

                pthread_mutex_lock(&sw_linux->mutex);
                swatcher_target_node *current = swatcher_instance->targets_head;

                time_t current_time = time(NULL);
                while (current != NULL)
                {
                    swatcher_target_linux *sw_target_linux = (swatcher_target_linux *)current->target->platform_data;
                    if (sw_target_linux->wd == event->wd)
                    {
                        swatcher_fs_event sw_event = convert_inotify_event_to_swatcher_event(event->mask);

                        if (sw_event != SWATCHER_EVENT_NONE)
                        {
                            if ((current->target->events & sw_event) || (current->target->events == SWATCHER_EVENT_ALL))
                            {
                                if (current->target->pattern != NULL && event->len > 0 && event->name != 0)
                                {
                                    bool res = is_pattern_matched(current->target->pattern, event->len > 0 ? event->name : NULL);
                                    // SWATCHER_LOG_DEBUG(swatcher_instance, "Pattern %s %s %s", current->target->pattern, res ? "matched" : "did not match", event->name);

                                    if (res)
                                    {
                                        // SWATCHER_LOG_DEBUG(swatcher_instance, "Pattern %s did match %s", current->target->pattern, event->name);

                                        current->target->callback(sw_event, current->target, event->len > 0 ? event->name : NULL);
                                        current->target->last_event_time = current_time;
                                        break;
                                    }
                                    else
                                    {
                                        // SWATCHER_LOG_DEBUG(swatcher_instance, "Pattern %s did not match %s", current->target->pattern, event->name);
                                    }
                                }
                                else
                                {
                                    if (current->target->pattern == NULL)
                                    {
                                        // SWATCHER_LOG_DEBUG(swatcher_instance, "Pattern is NULL");
                                        current->target->callback(sw_event, current->target, event->len > 0 ? event->name : NULL);
                                        current->target->last_event_time = current_time;
                                        break;
                                    }
                                }
                            }
                        }
                    }

                    current = current->next;
                }

                i += EVENT_SIZE + event->len;
                pthread_mutex_unlock(&sw_linux->mutex);
            }
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

bool swatcher_validate_and_normalize_path(const char *input_path, char *normalized_path)
{
    if (!swatcher_is_absolute_path(input_path))
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
    else
    {
        if (realpath(input_path, normalized_path) == NULL)
        {
            SWATCHER_LOG_DEFAULT_ERROR("Failed to get real path for %s", input_path);
            return false;
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
    sw_linux->running = false;
    sw_linux->thread_id = -1; // Initialize to invalid value

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

    swatcher->targets_head = NULL;
    swatcher->config = config;

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

    sw_linux->running = true;
    sw_linux->thread_id = ret;

    return true;
}

void swatcher_stop(swatcher *swatcher)
{
    swatcher_linux *sw_linux = (swatcher_linux *)swatcher->platform_data;

    sw_linux->running = false;

    // stop watcher thread
    pthread_join(sw_linux->thread, NULL);

    // close inotify
    close(sw_linux->inotify_fd);
}

void swatcher_cleanup(swatcher *swatcher)
{
    // Free the linked list of targets
    swatcher_target_node *current = swatcher->targets_head;
    while (current != NULL)
    {
        SWATCHER_LOG_DEFAULT_DEBUG("Freeing target: %s", current->target->path);
        swatcher_target_node *next = current->next;

        free(current->target->path);
        current->target->path = NULL;

        free(current->target->platform_data);
        current->target->platform_data = NULL;

        free(current->target->pattern);
        current->target->pattern = NULL;

        free(current->target);
        free(current);

        current = next;
    }

    // pthred cleanup
    swatcher_linux *sw_linux = (swatcher_linux *)swatcher->platform_data;
    pthread_mutex_destroy(&sw_linux->mutex);

    // Free platform data
    free(swatcher->platform_data);
    swatcher->platform_data = NULL;

    // Free the swatcher struct
    free(swatcher);
}

bool add_watch(swatcher *swatcher, swatcher_target *target)
{
    swatcher_linux *sw_linux = (swatcher_linux *)swatcher->platform_data;

    int wd = inotify_add_watch(sw_linux->inotify_fd, target->path, swatcher_target_events_to_inotify_mask(target));
    if (wd < 0)
    {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to add watch for %s", target->path);
        return false;
    }

    swatcher_target_node *new_node = malloc(sizeof(swatcher_target_node));
    if (new_node == NULL)
    {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to allocate swatcher_target_node");
        return false;
    }

    new_node->target = target; // Copy the target struct

    swatcher_target_linux *sw_target_linux = malloc(sizeof(swatcher_target_linux));
    if (sw_target_linux == NULL)
    {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to allocate swatcher_target_linux");
        free(new_node);
        return false;
    }

    sw_target_linux->wd = wd;

    new_node->target->platform_data = sw_target_linux;

    new_node->next = swatcher->targets_head;
    swatcher->targets_head = new_node;

    SWATCHER_LOG_DEBUG(swatcher, "Added watch for %s", target->path);

    return true;
}

bool add_watch_recursive_locked(swatcher *swatcher, swatcher_target *original_target, bool is_first_call)
{
    if (!add_watch(swatcher, original_target))
    {
        return false;
    }

    if (!original_target->is_file && !original_target->is_recursive)
    {
        return true;
    }

    DIR *dir = opendir(original_target->path);
    if (dir == NULL)
    {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to open directory: %s", original_target->path);
        return false;
    }

    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL)
    {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
        {
            // Skip . and ..
            continue;
        }

        size_t new_path_len = strlen(original_target->path) + strlen(entry->d_name) + 2; // +2 for potential slash and null terminator
        char *new_path = malloc(new_path_len);
        if (new_path == NULL)
        {
            SWATCHER_LOG_DEFAULT_ERROR("Failed to allocate new path");
            return false;
        }

        // Handle trailing slash in original path
        if (original_target->path[strlen(original_target->path) - 1] == '/')
        {
            snprintf(new_path, new_path_len, "%s%s", original_target->path, entry->d_name);
        }
        else
        {
            snprintf(new_path, new_path_len, "%s/%s", original_target->path, entry->d_name);
        }

        swatcher_target_desc new_target_desc = {
            .path = new_path,
            .is_recursive = original_target->is_recursive,
            .events = original_target->events,
            .user_data = original_target->user_data,
            .pattern = original_target->pattern,
            .callback = original_target->callback};

        swatcher_target *new_target = swatcher_target_create(&new_target_desc);
        if (entry->d_type == DT_DIR)
        {
            if (!add_watch_recursive_locked(swatcher, new_target, false))
            {
                SWATCHER_LOG_DEFAULT_ERROR("Failed to add watch for directory for %s", new_target->path);
                free(new_path);
                free(new_target->path);
                free(new_target);
                closedir(dir);
                return false;
            }

            // SWATCHER_LOG_INFO(swatcher, "Adding watch for directory for %s", new_target->path);
        }
        else
        {
            if (!add_watch(swatcher, new_target))
            {
                SWATCHER_LOG_DEFAULT_ERROR("Failed to add watch for file for %s", new_target->path);
                free(new_path);
                free(new_target->path);
                free(new_target);
                closedir(dir);
                return false;
            }

            // SWATCHER_LOG_INFO(swatcher, "Adding watch for file for %s", new_target->path);
        }

        free(new_path);
    }

    closedir(dir);
    return true;
}

bool add_watch_recursive(swatcher *swatcher, swatcher_target *target, bool is_first_call)
{
    pthread_mutex_lock(&((swatcher_linux *)swatcher->platform_data)->mutex);

    bool result = add_watch_recursive_locked(swatcher, target, is_first_call);

    pthread_mutex_unlock(&((swatcher_linux *)swatcher->platform_data)->mutex);

    return result;
}

bool swatcher_add(swatcher *swatcher, swatcher_target *target)
{

    bool result = false;

    if (!target->is_file && target->is_recursive)
    {
        result = add_watch_recursive(swatcher, target, true);
    }
    else
    {
        pthread_mutex_lock(&((swatcher_linux *)swatcher->platform_data)->mutex);
        result = add_watch(swatcher, target);
        pthread_mutex_unlock(&((swatcher_linux *)swatcher->platform_data)->mutex);
    }

    return result;
}

bool swatcher_remove(swatcher *swatcher, swatcher_target *target)
{
    swatcher_linux *sw_linux = (swatcher_linux *)swatcher->platform_data;

    pthread_mutex_lock(&sw_linux->mutex);

    swatcher_target_node *current = swatcher->targets_head;
    swatcher_target_node *prev = NULL;

    while (current != NULL)
    {
        if (current->target->path == target->path)
        {
            if (prev == NULL)
            {
                // First node
                swatcher->targets_head = current->next;
            }
            else
            {
                prev->next = current->next;
            }

            swatcher_target_linux *sw_target_linux = (swatcher_target_linux *)current->target->platform_data;
            inotify_rm_watch(sw_linux->inotify_fd, sw_target_linux->wd);

            free(current->target->path);
            free(current->target->pattern);
            free(current->target->platform_data);
            free(current->target);

            free(current);

            pthread_mutex_unlock(&sw_linux->mutex);

            return true;
        }

        prev = current;
        current = current->next;
    }

    pthread_mutex_unlock(&sw_linux->mutex);

    return false;
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
    if (!swatcher_validate_and_normalize_path(desc->path, normalized_path))
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
    }

    if (stat_result != 0)
    {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to stat path: %s", normalized_path);
        free(target);
        return NULL;
    }

    target->is_file = S_ISREG(path_stat.st_mode);
    target->is_directory = S_ISDIR(path_stat.st_mode);
    target->is_symlink = S_ISLNK(path_stat.st_mode);

    target->path = strdup(normalized_path);
    if (target->path == NULL)
    {
        SWATCHER_LOG_DEFAULT_ERROR("Failed to allocate path (string)");
        free(target);
        return NULL;
    }

    if (desc->pattern == 0)
    {
        target->pattern = NULL;
    }
    else
    {
        target->pattern = strdup(desc->pattern);
        if (target->pattern == NULL)
        {
            SWATCHER_LOG_DEFAULT_ERROR("Failed to allocate pattern (regex string)");
            free(target->path);
            free(target);
            return NULL;
        }
    }

    target->is_recursive = desc->is_recursive;
    target->events = desc->events;
    target->user_data = desc->user_data;
    target->callback = desc->callback;
    target->last_event_time = time(NULL);

    // SWATCHER_LOG_DEFAULT_DEBUG("Created target: %s", target->path);

    return target;
}
