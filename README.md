# swatcher

Cross-platform file system watching library in pure C.

[![CI](https://github.com/seemsindie/swatcher/actions/workflows/ci.yml/badge.svg)](https://github.com/seemsindie/swatcher/actions/workflows/ci.yml)

## Features

- Native backends: inotify (Linux), kqueue (macOS/BSD), FSEvents (macOS), ReadDirectoryChangesW/IOCP (Windows)
- Portable stat-based poll fallback for any platform
- Recursive directory watching with dynamic subdirectory tracking
- Glob and regex pattern filtering (bundled engine — no `regex.h` dependency)
- Event coalescing (configurable window to batch rapid edits)
- Thread-safe: add/remove targets while the watcher is running
- C++ header-only wrapper (`swatcher.hpp`) with RAII and lambdas
- Zig bindings with `build.zig` integration
- Zero external dependencies

## Quick Start

```c
#include <swatcher.h>

void on_event(swatcher_fs_event event, swatcher_target *target,
              const char *name, void *data)
{
    (void)target; (void)data;
    printf("[%s] %s\n", swatcher_event_name(event), name);
}

int main(void)
{
    swatcher_config config = { .poll_interval_ms = 100, .enable_logging = true };

    swatcher *sw = swatcher_create(&config);

    swatcher_target *t = swatcher_target_create(&(swatcher_target_desc){
        .path = "./src",
        .is_recursive = true,
        .events = SWATCHER_EVENT_ALL,
        .callback = on_event
    });

    swatcher_add(sw, t);
    swatcher_start(sw);
    getchar();               /* press Enter to stop */
    swatcher_destroy(sw);    /* stop + cleanup + free */
}
```

## Building

CMake 3.20+, C11 compiler.

```bash
cmake -B build
cmake --build build
```

With Ninja:

```bash
cmake -B build -G Ninja
cmake --build build
```

With Zig:

```bash
cd bindings/zig
zig build
```

## API Reference

### Convenience API (recommended)

| Function | Description |
|----------|-------------|
| `swatcher_create(config)` | Allocate + initialize a watcher (default backend) |
| `swatcher_create_with_backend(config, name)` | Allocate + initialize with a named backend |
| `swatcher_destroy(sw)` | Stop + cleanup + free |

### Core API (for stack allocation / custom allocators)

| Function | Description |
|----------|-------------|
| `swatcher_init(sw, config)` | Initialize a caller-allocated watcher |
| `swatcher_init_with_backend(sw, config, name)` | Initialize with a named backend |
| `swatcher_start(sw)` | Start the watcher thread |
| `swatcher_stop(sw)` | Stop the watcher thread |
| `swatcher_cleanup(sw)` | Free internal resources (not `sw` itself) |

### Targets

| Function | Description |
|----------|-------------|
| `swatcher_target_create(desc)` | Create a watch target from a descriptor |
| `swatcher_target_destroy(target)` | Destroy an un-added target |
| `swatcher_add(sw, target)` | Add a target to the watcher |
| `swatcher_remove(sw, target)` | Remove and free a target |

### Utilities

| Function | Description |
|----------|-------------|
| `swatcher_event_name(event)` | Human-readable event name |
| `swatcher_is_watched(sw, path)` | Check if a path is watched |
| `swatcher_last_error()` | Last error code (thread-local) |
| `swatcher_error_string(err)` | Human-readable error string |
| `swatcher_backends_available()` | List available backend names |

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                    Public API (swatcher.h)               │
│  swatcher_create / start / stop / destroy / add / remove│
├─────────────────────────────────────────────────────────┤
│                     Core Layer                           │
│  Event dispatch, pattern matching, target management,   │
│  coalescing, error handling                             │
├──────────────┬──────────────────────────────────────────┤
│ Pattern Engine│  Bundled regex (portable, no regex.h)   │
│ (re.h)       │  + glob convenience wrapper             │
├──────────────┴──────────────────────────────────────────┤
│                   Backend Interface                      │
│  swatcher_backend { init, add, remove, poll, destroy }  │
├────────┬─────────┬─────────┬──────────┬────────────────┤
│inotify │ kqueue  │fsevents │  Win32   │  poll (stat)   │
│(Linux) │(macOS/  │(macOS)  │  (IOCP)  │  (fallback,    │
│        │ BSD)    │         │          │   portable)    │
├────────┴─────────┴─────────┴──────────┴────────────────┤
│                  Platform Abstraction                    │
│  Threads, mutexes, paths, dir iteration, time, atomics  │
└─────────────────────────────────────────────────────────┘
```

## Backends

| Platform | Default Backend | Alternatives |
|----------|----------------|--------------|
| Linux | `inotify` | `poll` |
| macOS | `fsevents` | `kqueue`, `poll` |
| Windows | `win32` (IOCP) | `poll` |
| Other | `poll` | — |

Select a backend explicitly:

```c
swatcher *sw = swatcher_create_with_backend(&config, "kqueue");
```

List available backends at runtime:

```c
const char **backends = swatcher_backends_available();
for (const char **b = backends; *b; b++)
    printf("%s\n", *b);
```

## Language Bindings

### C++

Header-only, C++20. RAII watcher and target classes with lambda callbacks.

```cpp
#include <swatcher.hpp>

sw::Watcher w;
w.add(sw::Target("./src", SWATCHER_EVENT_ALL)
    .recursive()
    .callback_patterns({"*.cpp", "*.h"})
    .on_event([](swatcher_fs_event ev, const char *name, void *) {
        std::cout << swatcher_event_name(ev) << ": " << name << "\n";
    }));
w.start();
```

### Zig

```zig
const swatcher = @import("swatcher");
var watcher = try swatcher.Watcher.init(.{ .poll_interval_ms = 100 });
defer watcher.deinit();
```

See `bindings/zig/` for the full wrapper and build integration.

## Configuration

### `swatcher_config`

| Field | Type | Description |
|-------|------|-------------|
| `poll_interval_ms` | `int` | Poll interval in ms (poll backend). Default: 0. |
| `enable_logging` | `bool` | Log to stderr. Default: false. |
| `coalesce_ms` | `int` | Event coalescing window in ms. 0 = disabled. |

### `swatcher_target_desc`

| Field | Type | Description |
|-------|------|-------------|
| `path` | `char *` | Path to watch (file or directory). |
| `is_recursive` | `bool` | Watch subdirectories. |
| `events` | `swatcher_fs_event` | Event mask (`SWATCHER_EVENT_ALL`, or combine with `\|`). |
| `watch_options` | `swatcher_watch_option` | Filter: files, directories, symlinks, or all. |
| `callback_patterns` | `char **` | NULL-terminated glob/regex list — only matching names fire. |
| `watch_patterns` | `char **` | NULL-terminated glob/regex list — only watch matching entries. |
| `ignore_patterns` | `char **` | NULL-terminated glob/regex list — skip matching entries. |
| `user_data` | `void *` | Passed to callback. |
| `follow_symlinks` | `bool` | Follow symlinks to targets. |
| `callback` | `swatcher_callback_fn` | Event callback. |

Use the helper macros for pattern arrays:

```c
.callback_patterns = GLOB_PATTERNS("*.c", "*.h"),
.ignore_patterns   = GLOB_PATTERNS(".git", "*.tmp"),
```

## Error Handling

All functions that can fail set a thread-local error code:

```c
swatcher *sw = swatcher_create(&config);
if (!sw) {
    fprintf(stderr, "error: %s\n", swatcher_error_string(swatcher_last_error()));
    return 1;
}
```

Error codes: `SWATCHER_OK`, `SWATCHER_ERR_NULL_ARG`, `SWATCHER_ERR_ALLOC`, `SWATCHER_ERR_INVALID_PATH`, `SWATCHER_ERR_PATH_NOT_FOUND`, `SWATCHER_ERR_BACKEND_INIT`, `SWATCHER_ERR_BACKEND_NOT_FOUND`, `SWATCHER_ERR_THREAD`, `SWATCHER_ERR_MUTEX`, `SWATCHER_ERR_NOT_INITIALIZED`, `SWATCHER_ERR_TARGET_EXISTS`, `SWATCHER_ERR_TARGET_NOT_FOUND`, `SWATCHER_ERR_PATTERN_COMPILE`, `SWATCHER_ERR_WATCH_LIMIT`, `SWATCHER_ERR_UNKNOWN`.

## License

MIT
