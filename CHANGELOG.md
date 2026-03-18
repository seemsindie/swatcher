# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/),
and this project adheres to [Semantic Versioning](https://semver.org/).

## [0.1.0] - 2026-03-18

### Added

- 8 native backends: inotify (Linux), fanotify (Linux), io_uring (Linux), kqueue (macOS/BSD), FSEvents (macOS), ReadDirectoryChangesW/IOCP (Windows), poll (portable fallback), backend registry with runtime selection
- Recursive directory watching with dynamic subdirectory tracking
- Glob and regex pattern filtering with bundled engine (no `regex.h` dependency)
- Event coalescing with configurable window to batch rapid edits
- Custom allocator support (`swatcher_allocator`)
- Thread-safe target add/remove while the watcher is running
- VCS-aware mode: pause events during git/hg/svn operations
- Overflow rescan: automatic directory re-scan on kernel queue overflow
- Convenience API (`swatcher_create` / `swatcher_destroy`) and core API for stack allocation
- C++ header-only wrapper (`swatcher.hpp`) with RAII and lambda callbacks (C++20)
- Zig bindings with `build.zig` integration
- 86+ tests across all backends and platforms
- CI on Linux, macOS, Windows, and FreeBSD
- CMake install targets with pkg-config and `find_package()` support
- Version macros (`SWATCHER_VERSION_MAJOR`, `SWATCHER_VERSION_MINOR`, `SWATCHER_VERSION_PATCH`, `SWATCHER_VERSION`)
