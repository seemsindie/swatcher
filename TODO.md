# swatcher - Project Plan

> Pure C, minimal-dependency, cross-platform file system watching library.
> Goal: feature-complete, fast, stable, zero leaks.

---

## Architecture Overview

```
┌─────────────────────────────────────────────────────────┐
│                    Public API (swatcher.h)               │
│  swatcher_init / start / stop / cleanup / add / remove  │
├─────────────────────────────────────────────────────────┤
│                     Core Layer                           │
│  Event dispatch, pattern matching, target management,   │
│  deduplication, error handling                          │
├──────────────┬──────────────────────────────────────────┤
│ Pattern Engine│  Bundled regex (portable, no regex.h)   │
│ (sregex.h)   │  + glob convenience wrapper             │
├──────────────┴──────────────────────────────────────────┤
│                   Backend Interface                      │
│  swatcher_backend { init, add, remove, poll, destroy }  │
├────────┬─────────┬─────────┬──────────┬────────────────┤
│inotify │ kqueue  │fsevents │ReadDir-  │  poll (stat)   │
│(Linux) │(macOS/  │(macOS)  │ChangesW  │  (fallback,    │
│        │ BSD)    │         │(Windows) │   portable)    │
├────────┴─────────┴─────────┴──────────┴────────────────┤
│                  Platform Abstraction                    │
│  Threads, mutexes, paths, dir iteration, time, atomics  │
│  (Linux / macOS / Windows / future: Android, iOS)       │
└─────────────────────────────────────────────────────────┘
```

### Key Design Decisions

- **No `regex.h` dependency** — bundle a small portable regex engine (SLRE, tiny-regex-c, or custom). Works everywhere including MSVC.
- **Backend interface** — vtable of function pointers. Backends are selected at compile time (or runtime with fallback).
- **Platform abstraction layer (PAL)** — thin wrappers for OS primitives (threads, mutexes, paths, directory iteration). Every platform-specific `#ifdef` lives here, nowhere else.
- **Proper .c/.h split** — move out of header-only. Headers declare, .c files implement. Build as static/shared lib.
- **No uthash dependency** — replace with a simple internal hash table (or keep uthash but vendor it properly).

---

## Phase 0: Foundation & Cleanup
> Restructure the project so everything else builds on solid ground.

- [x] **0.1** Define directory structure:
  ```
  include/
    swatcher.h          (public API - declarations only)
    swatcher_types.h    (public types, enums, structs)
  src/
    core/
      swatcher.c        (init, start, stop, cleanup, add, remove)
      event.c           (event dispatch, deduplication)
      target.c          (target create, destroy, management)
      pattern.c         (regex/glob matching - portable)
    platform/
      platform.h        (PAL interface: threads, mutex, paths, dir, time)
      platform_linux.c
      platform_windows.c
      platform_macos.c
    backend/
      backend.h         (backend vtable interface)
      backend_inotify.c
      backend_kqueue.c
      backend_fsevents.c
      backend_win32.c
      backend_poll.c    (stat-based fallback)
    regex/
      sregex.h          (bundled portable regex - header-only or .c)
      sregex.c
  tests/
    test_main.c
    test_pattern.c
    test_target.c
    test_watcher.c
    test_platform.c
  examples/
    basic.c
    recursive.c
    patterns.c
    multi_target.c
  ```
- [x] **0.2** Define the backend vtable interface (`backend.h`):
  ```c
  typedef struct swatcher_backend {
      const char *name;
      bool (*init)(swatcher *sw);
      void (*destroy)(swatcher *sw);
      bool (*add_target)(swatcher *sw, swatcher_target *target);
      bool (*remove_target)(swatcher *sw, swatcher_target *target);
      int  (*poll_events)(swatcher *sw, int timeout_ms);
      // Returns number of events dispatched, -1 on error
  } swatcher_backend;
  ```
- [x] **0.3** Define the platform abstraction interface (`platform.h`):
  - `sw_thread_create / join / detach`
  - `sw_mutex_init / lock / unlock / destroy`
  - `sw_dir_open / read / close` (directory iteration)
  - `sw_path_normalize / is_absolute / join / separator`
  - `sw_stat` (file info: type, size, mtime)
  - `sw_time_now_ms` (monotonic clock)
  - `sw_atomic_load / store / cas` (atomics for lock-free flags)
- [x] **0.4** Clean up public types (`swatcher_types.h`):
  - Audit `swatcher_fs_event` — remove `ALL_INOTIFY`, make events platform-neutral
  - Clean up `swatcher_target` — remove platform_data from public struct, use opaque pointer
  - Remove uthash handles from public struct
  - Add `swatcher_error` enum for error reporting
- [x] **0.5** Port existing Linux inotify code into `backend_inotify.c` using new interfaces
- [x] **0.6** Port existing Windows code into `backend_win32.c` using new interfaces
- [x] **0.7** Update CMakeLists.txt:
  - Proper source file lists per platform
  - `target_compile_definitions` for platform detection
  - Install targets (headers + lib)
  - C11 standard (for atomics, anonymous structs)
  - Compiler warnings: `-Wall -Wextra -Wpedantic -Werror` / `/W4 /WX`
- [x] **0.8** Verify the restructured code compiles and runs on Linux + Windows

---

## Phase 1: Portable Regex / Pattern Engine
> Solve the `regex.h` problem once and for all.

- [x] **1.1** Evaluate and select a bundled regex engine: **tiny-regex-c** (public domain, ~500 LOC, modified for malloc-based compilation)
- [x] **1.2** Integrate chosen engine into `src/regex/` (re.h, re.c — vendored with heap-allocated compile)
- [x] **1.3** Build `pattern.c` — unified pattern matching API:
  ```c
  typedef struct sw_pattern {
      // opaque compiled pattern
  } sw_pattern;

  bool sw_pattern_compile(sw_pattern *p, const char *regex);
  bool sw_pattern_match(const sw_pattern *p, const char *str);
  void sw_pattern_free(sw_pattern *p);

  // Convenience: match against NULL-terminated array of patterns
  bool sw_patterns_any_match(sw_pattern *patterns[], const char *str);
  ```
- [x] **1.4** Pre-compile patterns at target creation time (not on every event)
- [x] **1.5** Add glob-to-regex convenience wrapper (auto-detected in sw_patterns_compile):
  ```c
  // Converts "*.txt" → "^.*\\.txt$"
  bool sw_glob_to_regex(const char *glob, char *regex_buf, size_t buf_size);
  ```
- [x] **1.6** Write pattern tests (test_pattern.c) — 15 tests, valgrind clean
- [ ] **1.7** Verify patterns work on all three platforms (Linux done, Windows/macOS pending)

---

## Phase 2: Platform Abstraction Layer (PAL)
> Every `#ifdef _WIN32` outside of `src/platform/` is a bug.

- [ ] **2.1** Implement `platform_linux.c`:
  - pthreads wrappers
  - `opendir/readdir/closedir` wrappers
  - `realpath`, POSIX path handling
  - `clock_gettime(CLOCK_MONOTONIC)` for time
  - `stat()` wrappers
- [ ] **2.2** Implement `platform_windows.c`:
  - `CreateThread / WaitForSingleObject` wrappers
  - `CRITICAL_SECTION` wrappers
  - `FindFirstFile/FindNextFile` wrappers
  - Windows path normalization (backslash → forward slash, UNC paths)
  - `QueryPerformanceCounter` for time
  - `GetFileAttributesEx` wrappers
- [ ] **2.3** Implement `platform_macos.c`:
  - Same as Linux (POSIX) with macOS-specific path quirks
  - Case-insensitive filesystem awareness
- [ ] **2.4** Write platform tests (test_platform.c)
- [ ] **2.5** Ensure all platform code compiles cleanly on each target OS

---

## Phase 3: Backend — stat-based Polling (Fallback)
> Universal fallback that works everywhere, even without OS-specific APIs.

- [ ] **3.1** Implement `backend_poll.c`:
  - Walk directory tree, `stat()` each file, store mtime/size snapshot
  - On each poll cycle, re-walk and compare snapshots
  - Detect: created (new entry), deleted (missing entry), modified (mtime/size changed)
  - Detect: moved (deleted + created with same inode/size in same cycle)
- [ ] **3.2** Configurable poll interval (default 500ms for poll backend)
- [ ] **3.3** Efficient snapshot storage (hash table by path)
- [ ] **3.4** Handle large directories without excessive memory (streaming walk)
- [ ] **3.5** Write poll backend tests
- [ ] **3.6** Benchmark: measure overhead for 1K, 10K, 100K files

---

## Phase 4: Backend — inotify (Linux)
> Port and improve existing Linux implementation.

- [ ] **4.1** Port existing `swatcher_linux.h` into `backend_inotify.c` using backend vtable
- [ ] **4.2** Fix recursive watching:
  - Dynamically add watches on `IN_CREATE | IN_ISDIR`
  - Remove watches on `IN_DELETE | IN_ISDIR`
  - Handle race conditions (dir created between scan and watch add)
- [ ] **4.3** Handle inotify limits gracefully:
  - Check `/proc/sys/fs/inotify/max_user_watches`
  - Fall back to poll backend if limit exceeded
  - Log warning when approaching limit
- [ ] **4.4** Event coalescing:
  - Batch rapid events on same path (e.g., editor save = truncate + write)
  - Configurable coalesce window (default 50ms)
- [ ] **4.5** Handle `IN_Q_OVERFLOW`:
  - Re-scan watched directories
  - Re-establish watches
  - Emit overflow event to user
- [ ] **4.6** Use `epoll` instead of `poll` for multiplexing (scales better with many watches)
- [ ] **4.7** Write inotify backend tests
- [ ] **4.8** Stress test with rapid file operations

---

## Phase 5: Backend — kqueue (macOS / BSD)
> Native backend for macOS and FreeBSD/OpenBSD/NetBSD.

- [ ] **5.1** Implement `backend_kqueue.c`:
  - `kqueue()` + `kevent()` based file monitoring
  - `EVFILT_VNODE` with `NOTE_WRITE | NOTE_DELETE | NOTE_RENAME | NOTE_ATTRIB | NOTE_EXTEND`
  - Open file descriptors for each watched path
- [ ] **5.2** Recursive watching:
  - Monitor directories for `NOTE_WRITE` (indicates child added/removed)
  - Re-scan on directory change, add/remove watches for new/deleted entries
- [ ] **5.3** Handle fd limits:
  - Check `getrlimit(RLIMIT_NOFILE)`
  - Fall back to poll for directories exceeding limit
  - Log warnings
- [ ] **5.4** Symlink handling:
  - `O_SYMLINK` flag for watching symlinks themselves
  - `O_NOFOLLOW` for not following
- [ ] **5.5** Write kqueue backend tests
- [ ] **5.6** Test on macOS and at least one BSD (FreeBSD)

---

## Phase 6: Backend — FSEvents (macOS)
> High-level macOS backend, better for large directory trees.

- [ ] **6.1** Rewrite `swatcher_macos.h` into `backend_fsevents.c`:
  - `FSEventStreamCreate` with proper flags
  - `kFSEventStreamCreateFlagFileEvents` for file-level events
  - `kFSEventStreamCreateFlagNoDefer` for immediate delivery
- [ ] **6.2** Run loop management:
  - Dedicated thread with `CFRunLoop`
  - Proper start/stop/invalidate lifecycle
- [ ] **6.3** Event translation:
  - Map `kFSEventStreamEventFlagItem*` to `swatcher_fs_event`
  - Handle coalesced events (FSEvents batches by design)
- [ ] **6.4** Historical events support (optional):
  - `kFSEventStreamEventIdSinceNow` vs specific event ID
  - Could be useful for "catch up" after reconnect
- [ ] **6.5** Write FSEvents backend tests
- [ ] **6.6** Benchmark vs kqueue for large trees (>10K files)

---

## Phase 7: Backend — ReadDirectoryChangesW (Windows)
> Port and improve existing Windows implementation.

- [ ] **7.1** Port existing `swatcher_windows.h` into `backend_win32.c` using backend vtable
- [ ] **7.2** Fix the 64-handle limit (MAXIMUM_WAIT_OBJECTS):
  - Use I/O Completion Ports (IOCP) instead of `WaitForMultipleObjects`
  - Or use thread pool with smaller handle groups
- [ ] **7.3** Add pattern matching support (now possible with portable regex from Phase 1)
- [ ] **7.4** Handle network paths (UNC `\\server\share`):
  - Detect and warn about higher latency
  - Larger buffers for remote notifications
- [ ] **7.5** Buffer overflow handling:
  - Detect when `ReadDirectoryChangesW` buffer overflows
  - Re-scan directory on overflow
  - Emit overflow event
- [ ] **7.6** Proper UTF-8 handling throughout (convert UTF-16 ↔ UTF-8)
- [ ] **7.7** Write Windows backend tests
- [ ] **7.8** Stress test with rapid file operations

---

## Phase 8: Core — Event System & Lifecycle
> Robust event dispatch, error handling, and resource management.

- [ ] **8.1** Event dispatch pipeline:
  ```
  Backend raw event
    → Event translation (platform → swatcher_fs_event)
    → Pattern filtering (ignore_patterns → watch_patterns → callback_patterns)
    → Event coalescing (optional, configurable)
    → User callback invocation
  ```
- [ ] **8.2** Error reporting:
  ```c
  typedef enum {
      SWATCHER_OK = 0,
      SWATCHER_ERR_NOMEM,
      SWATCHER_ERR_INVALID_PATH,
      SWATCHER_ERR_PERMISSION,
      SWATCHER_ERR_LIMIT_EXCEEDED,
      SWATCHER_ERR_BACKEND_INIT,
      SWATCHER_ERR_OVERFLOW,
      // ...
  } swatcher_error;

  swatcher_error swatcher_last_error(swatcher *sw);
  const char *swatcher_error_string(swatcher_error err);
  ```
- [ ] **8.3** Backend selection & fallback:
  ```c
  // Auto-select best backend for platform
  swatcher_init(sw, config);

  // Force specific backend
  swatcher_init_with_backend(sw, config, SWATCHER_BACKEND_KQUEUE);

  // Auto with fallback chain: inotify → poll (Linux)
  //                           fsevents → kqueue → poll (macOS)
  //                           win32 → poll (Windows)
  ```
- [ ] **8.4** Thread-safe add/remove while running
- [ ] **8.5** Graceful shutdown: drain pending events, join threads, free all resources
- [ ] **8.6** Zero-leak guarantee: run all tests under Valgrind / AddressSanitizer
- [ ] **8.7** Event deduplication: configurable window to merge rapid events on same path

---

## Phase 9: Testing
> Comprehensive test suite, run on all platforms.

- [ ] **9.1** Select test framework:
  - Option A: **greatest** (single header, ~1000 lines, C89)
  - Option B: **µnit** (single header, MIT)
  - Option C: **Custom minimal** (assert + counting macros, <200 lines)
- [ ] **9.2** Unit tests:
  - Pattern matching (regex compile, match, edge cases)
  - Path normalization (absolute, relative, Unicode, trailing slashes)
  - Event mask conversion (each platform)
  - Target creation / destruction
  - Hash table operations
- [ ] **9.3** Integration tests:
  - Create file → expect CREATED event
  - Modify file → expect MODIFIED event
  - Delete file → expect DELETED event
  - Rename file → expect MOVED event
  - Create directory → expect event + recursive watch added
  - Rapid operations → expect coalesced events
  - Pattern filtering → expect only matching events
- [ ] **9.4** Stress tests:
  - 10K files created rapidly
  - Deep nested directories (100+ levels)
  - Large number of concurrent watches
  - Long-running stability (1+ hour)
- [ ] **9.5** Memory tests:
  - Valgrind (Linux) / LeakSanitizer for every test
  - AddressSanitizer for buffer overflows
  - ThreadSanitizer for race conditions
- [ ] **9.6** CI setup:
  - GitHub Actions: Linux (GCC + Clang), macOS (Clang), Windows (MSVC + MinGW)
  - Run tests + sanitizers on every PR

---

## Phase 10: API Polish & Documentation
> Make it a joy to use.

- [ ] **10.1** Simplify the public API:
  ```c
  // Simple one-shot setup
  swatcher *sw = swatcher_create(&(swatcher_config){ .poll_interval_ms = 100 });
  swatcher_watch(sw, "/path", SWATCHER_EVENT_ALL, my_callback, NULL);
  swatcher_start(sw);
  // ...
  swatcher_destroy(sw); // stops + cleans up everything
  ```
- [ ] **10.2** Builder-pattern for targets:
  ```c
  swatcher_target *t = swatcher_target_builder()
      .path("/some/path")
      .recursive(true)
      .events(SWATCHER_EVENT_CREATED | SWATCHER_EVENT_MODIFIED)
      .ignore("*.tmp", "*.swp", ".git")  // glob patterns
      .callback(my_cb)
      .build();
  ```
  (Or just keep the designated initializer approach — it's already clean in C99+)
- [ ] **10.3** Header documentation (Doxygen-style comments on all public functions)
- [ ] **10.4** Usage examples:
  - `basic.c` — minimal watch + callback
  - `recursive.c` — recursive directory watching with patterns
  - `multi_backend.c` — explicit backend selection
  - `live_reload.c` — practical example: rebuild on source change
- [ ] **10.5** README rewrite with architecture diagram, quick start, API reference
- [ ] **10.6** Man page or single-page HTML docs

---

## Phase 11: Performance & Hardening
> Make it production-grade.

- [ ] **11.1** Benchmarks:
  - Event latency: time from file op to callback invocation
  - Throughput: events per second under load
  - Memory: RSS per watched directory / file
  - Compare backends (inotify vs poll, fsevents vs kqueue)
- [ ] **11.2** Memory pool for frequent small allocations (events, paths)
- [ ] **11.3** Lock-free event queue (SPSC ring buffer: backend thread → dispatch thread)
- [ ] **11.4** Batch event delivery option (callback receives array of events)
- [ ] **11.5** Fuzz testing (AFL/libFuzzer on pattern matching, path parsing)
- [ ] **11.6** Static analysis (clang-tidy, cppcheck, PVS-Studio)

---

## Phase 12: Future Platforms
> Expand beyond desktop.

- [ ] **12.1** Android:
  - `inotify` works on Android (Linux kernel)
  - NDK build support (CMake toolchain file)
  - JNI wrapper for Java/Kotlin interop
- [ ] **12.2** iOS:
  - `dispatch_source` (GCD) for file monitoring
  - Or kqueue (available on iOS)
  - Framework build target
- [ ] **12.3** FreeBSD / OpenBSD / NetBSD:
  - kqueue backend already covers this
  - Test and verify
- [ ] **12.4** WebAssembly / Emscripten:
  - Poll backend only (no native FS events in WASM)
  - Virtual filesystem support
- [ ] **12.5** Package management:
  - vcpkg port
  - Conan recipe
  - pkg-config `.pc` file
  - CMake `find_package` support

---

## Priority & Ordering

```
Phase 0 (Foundation)     ██████████  ← START HERE, do this first
Phase 1 (Regex)          ████████    ← unblocks pattern matching everywhere
Phase 2 (PAL)            ████████    ← unblocks all backends
Phase 3 (Poll backend)   ██████      ← universal fallback
Phase 4 (inotify)        ████████    ← Linux primary
Phase 5 (kqueue)         ██████      ← macOS/BSD
Phase 6 (FSEvents)       ██████      ← macOS alternative
Phase 7 (Win32)          ████████    ← Windows primary
Phase 8 (Core)           ████████    ← ties everything together
Phase 9 (Testing)        ██████████  ← ongoing, starts at Phase 1
Phase 10 (Docs)          ████        ← after API stabilizes
Phase 11 (Performance)   ██████      ← after correctness
Phase 12 (Platforms)     ████        ← future expansion
```

**Estimated LOC:** ~5,000-8,000 (excluding tests and vendored code)

---

## Current State (as of 2026-03-17)

| Component | Status | Notes |
|-----------|--------|-------|
| **Phase 0: Restructuring** | **DONE** | .c/.h split, backend vtable, PAL interface, CMake |
| **Phase 1: Portable Regex** | **DONE** | tiny-regex-c vendored, pre-compiled patterns, glob support, 15 tests, valgrind clean |
| Phase 2: PAL | ~70% | Core PAL works (threads, mutex, paths, dirs, stat). Missing: monotonic time, atomics. #ifdef leaks in swatcher_types.h, backend.h |
| Linux inotify | ~90% | Restructured, uses compiled patterns, recursive watching works |
| Windows ReadDirChanges | ~80% | Restructured, callback pattern filtering added, 64-handle limit remains |
| macOS FSEvents | ~10% | Skeleton only |
| macOS kqueue | 0% | Not started |
| Poll fallback | 0% | Not started |
| Tests | ~15% | test_pattern.c done (15 tests), need platform + integration tests |
| Build system | ~70% | CMake works, per-platform source selection, test targets |
| Documentation | ~20% | README exists, no API docs |
