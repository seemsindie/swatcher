const std = @import("std");
pub const c = @cImport({
    @cDefine("SWATCHER_ZIG_COMPAT", "1");
    @cInclude("swatcher.h");
});

// Re-export C types for convenience
pub const FsEvent = c.swatcher_fs_event;
pub const WatchOption = c.swatcher_watch_option;
pub const Config = c.swatcher_config;
pub const RawTarget = c.swatcher_target;
pub const RawWatcher = c.swatcher;

pub const Event = struct {
    pub const created: FsEvent = c.SWATCHER_EVENT_CREATED;
    pub const modified: FsEvent = c.SWATCHER_EVENT_MODIFIED;
    pub const deleted: FsEvent = c.SWATCHER_EVENT_DELETED;
    pub const moved: FsEvent = c.SWATCHER_EVENT_MOVED;
    pub const opened: FsEvent = c.SWATCHER_EVENT_OPENED;
    pub const closed: FsEvent = c.SWATCHER_EVENT_CLOSED;
    pub const accessed: FsEvent = c.SWATCHER_EVENT_ACCESSED;
    pub const attrib_change: FsEvent = c.SWATCHER_EVENT_ATTRIB_CHANGE;
    pub const all: FsEvent = c.SWATCHER_EVENT_ALL;
    pub const overflow: FsEvent = c.SWATCHER_EVENT_OVERFLOW;
};

pub const SwatcherError = error{
    NullArg,
    Alloc,
    InvalidPath,
    PathNotFound,
    BackendInit,
    BackendNotFound,
    Thread,
    Mutex,
    NotInitialized,
    TargetExists,
    TargetNotFound,
    PatternCompile,
    WatchLimit,
    Unknown,
};

fn mapError() SwatcherError {
    const err = c.swatcher_last_error();
    return switch (err) {
        c.SWATCHER_ERR_NULL_ARG => SwatcherError.NullArg,
        c.SWATCHER_ERR_ALLOC => SwatcherError.Alloc,
        c.SWATCHER_ERR_INVALID_PATH => SwatcherError.InvalidPath,
        c.SWATCHER_ERR_PATH_NOT_FOUND => SwatcherError.PathNotFound,
        c.SWATCHER_ERR_BACKEND_INIT => SwatcherError.BackendInit,
        c.SWATCHER_ERR_BACKEND_NOT_FOUND => SwatcherError.BackendNotFound,
        c.SWATCHER_ERR_THREAD => SwatcherError.Thread,
        c.SWATCHER_ERR_MUTEX => SwatcherError.Mutex,
        c.SWATCHER_ERR_NOT_INITIALIZED => SwatcherError.NotInitialized,
        c.SWATCHER_ERR_TARGET_EXISTS => SwatcherError.TargetExists,
        c.SWATCHER_ERR_TARGET_NOT_FOUND => SwatcherError.TargetNotFound,
        c.SWATCHER_ERR_PATTERN_COMPILE => SwatcherError.PatternCompile,
        c.SWATCHER_ERR_WATCH_LIMIT => SwatcherError.WatchLimit,
        else => SwatcherError.Unknown,
    };
}

pub fn errorString(err: c.swatcher_error) [*:0]const u8 {
    return c.swatcher_error_string(err);
}

pub fn eventName(event: FsEvent) [*:0]const u8 {
    return c.swatcher_event_name(event);
}

pub fn backendsAvailable() [*]const [*:0]const u8 {
    const raw = c.swatcher_backends_available();
    return @ptrCast(raw);
}

/// Callback function type for Zig.
pub const CallbackFn = *const fn (event: FsEvent, target: *RawTarget, name: ?[*:0]const u8, data: ?*anyopaque) callconv(.c) void;

pub const TargetOpts = struct {
    path: [*:0]const u8,
    events: FsEvent = Event.all,
    recursive: bool = false,
    watch_options: WatchOption = c.SWATCHER_WATCH_ALL,
    follow_symlinks: bool = false,
    callback: CallbackFn,
    user_data: ?*anyopaque = null,
    callback_patterns: ?[*:null]?[*:0]u8 = null,
    watch_patterns: ?[*:null]?[*:0]u8 = null,
    ignore_patterns: ?[*:null]?[*:0]u8 = null,
};

pub const Watcher = struct {
    handle: *RawWatcher,
    config: *Config,
    allocator: std.mem.Allocator,

    pub fn init(allocator: std.mem.Allocator, config: Config, backend: ?[*:0]const u8) !Watcher {
        const sw = allocator.create(RawWatcher) catch return SwatcherError.Alloc;

        const cfg = allocator.create(Config) catch {
            allocator.destroy(sw);
            return SwatcherError.Alloc;
        };
        cfg.* = config;

        const ok = if (backend) |b|
            c.swatcher_init_with_backend(sw, cfg, b)
        else
            c.swatcher_init(sw, cfg);

        if (!ok) {
            const err = mapError();
            allocator.destroy(cfg);
            allocator.destroy(sw);
            return err;
        }

        return .{
            .handle = sw,
            .config = cfg,
            .allocator = allocator,
        };
    }

    pub fn deinit(self: *Watcher) void {
        c.swatcher_stop(self.handle);
        c.swatcher_cleanup(self.handle);
        self.allocator.destroy(self.config);
        self.allocator.destroy(self.handle);
    }

    pub fn addTarget(self: *Watcher, opts: TargetOpts) !void {
        var desc: c.swatcher_target_desc = .{
            .path = @constCast(@ptrCast(opts.path)),
            .is_recursive = opts.recursive,
            .events = opts.events,
            .watch_options = opts.watch_options,
            .follow_symlinks = opts.follow_symlinks,
            .user_data = opts.user_data,
            .callback = @ptrCast(opts.callback),
            .callback_patterns = @ptrCast(opts.callback_patterns),
            .watch_patterns = @ptrCast(opts.watch_patterns),
            .ignore_patterns = @ptrCast(opts.ignore_patterns),
        };

        const target = c.swatcher_target_create(&desc);
        if (target == null) return mapError();

        if (!c.swatcher_add(self.handle, target)) return mapError();
    }

    pub fn start(self: *Watcher) !void {
        if (!c.swatcher_start(self.handle)) return mapError();
    }

    pub fn stop(self: *Watcher) void {
        c.swatcher_stop(self.handle);
    }

    pub fn isWatched(self: *const Watcher, path: [*:0]const u8) bool {
        return c.swatcher_is_watched(self.handle, path);
    }
};
