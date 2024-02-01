# swatcher (simple watcher)

`swatcher` is a cross-platform file system watching library. It abstracts away the complexities of each platform's native file system event monitoring mechanisms, offering a unified and straightforward API for applications.

## Features

- [] Cross Platform (linux with inotify, osx with FSEvents, windows with ReadDirectoryChangesW)
- [x] Just C, mostly headers
- [x] Directory watching (with recursive)
- [x] File watching
- [x] Regex/Glob watching (.pattern)
- [x] Async
- [x] Symlink support
- [x] Absolte, relative paths
- [] Remote directories
- [] Fallback system (ex. kqueue for osx)

### Prerequisites

- Linux: Kernel 2.6.13 or newer for `inotify` support.
- macOS: OS X 10.5 or newer for `FSEvents` support.
- Windows: Windows XP or newer for `ReadDirectoryChangesW` support.
- CMake 3.10 or newer

### Building

With make:

```bash
mkdir build
cd build
cmake ..
make
```

With ninja:

```bash
mkdir build
cd build
cmake -GNinja ..
ninja
```

### Installation

_TODO: Instructions on how to install the library._

### Basic Usage

```c
#include "swatcher.h"

void my_callback_function(swatcher_fs_event event, swatcher_target *target, const char *file_name)
{
  printf("Event: %s", get_event_name(event));
  printf("Target path: %s", target->path);
  printf("File name: %s", file_name); // it can be null
}

int main()
{
  swatcher_config config = {
    .poll_interval_ms = 50,
    .enable_logging = true};

  swatcher *watcher = malloc(sizeof(swatcher));

  if (!watcher) {
    SWATCHER_LOG_DEFAULT_ERROR("Failed to allocate a watcher");
    return EXIT_FAILURE;
  }

  if (!swatcher_init(watcher, &config))
  {
      SWATCHER_LOG_DEFAULT_ERROR("Failed to initialize swatcher");
      free(watcher);
      return EXIT_FAILURE;
  }


  swatcher_target *target = swatcher_target_create(&(swatcher_target_desc){
    .path = path,
    .is_recursive = true,
    .events = SWATCHER_EVENT_DELETED | SWATCHER_EVENT_CREATED | SWATCHER_EVENT_MODIFIED | SWATCHER_EVENT_MOVED,
    // .events = SWATCHER_EVENT_ALL,
    // .pattern = ".*\\.txt$",
    .user_data = "Hello, world (user data)!",
    .callback = my_callback_function});

  swatcher_add(watcher, target);

  swatcher_start(watcher);

  getchar();

  swatcher_remove(watcher, target);

  swatcher_stop(watcher);
  swatcher_cleanup(watcher);

  return EXIT_SUCCESS;

}


```
