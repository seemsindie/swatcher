const std = @import("std");
const sw = @import("swatcher");
const posix_c = @cImport({
    @cInclude("sys/stat.h");
    @cInclude("unistd.h");
    @cInclude("stdlib.h");
    @cInclude("stdio.h");
});

test "init and deinit watcher" {
    var watcher = try sw.Watcher.init(
        std.testing.allocator,
        .{ .poll_interval_ms = 100, .enable_logging = false, .coalesce_ms = 0 },
        null,
    );
    defer watcher.deinit();
}

test "init with poll backend" {
    var watcher = try sw.Watcher.init(
        std.testing.allocator,
        .{ .poll_interval_ms = 100, .enable_logging = false, .coalesce_ms = 0 },
        "poll",
    );
    defer watcher.deinit();
}

test "init with invalid backend returns error" {
    const result = sw.Watcher.init(
        std.testing.allocator,
        .{ .poll_interval_ms = 100, .enable_logging = false, .coalesce_ms = 0 },
        "nonexistent",
    );
    try std.testing.expectError(sw.SwatcherError.BackendNotFound, result);
}

test "event name returns valid string" {
    const name = sw.eventName(sw.Event.created);
    try std.testing.expect(name[0] != 0);
}

test "backends available includes poll" {
    const backends = sw.backendsAvailable();
    var found_poll = false;
    var i: usize = 0;
    while (true) : (i += 1) {
        const ptr = backends[i];
        if (@intFromPtr(ptr) == 0) break;
        const slice = std.mem.span(ptr);
        if (std.mem.eql(u8, slice, "poll")) {
            found_poll = true;
            break;
        }
    }
    try std.testing.expect(found_poll);
}

var test_event_count: u32 = 0;

fn testCallback(_: sw.FsEvent, _: *sw.RawTarget, _: ?[*:0]const u8, _: ?*anyopaque) void {
    test_event_count += 1;
}

test "add target and start/stop" {
    const tmp_dir = "zig_test_tmp";
    _ = posix_c.mkdir(tmp_dir, 0o755);
    defer _ = posix_c.rmdir(tmp_dir);

    var watcher = try sw.Watcher.init(
        std.testing.allocator,
        .{ .poll_interval_ms = 50, .enable_logging = false, .coalesce_ms = 0 },
        "poll",
    );
    defer watcher.deinit();

    // Get absolute path via C realpath
    const abs_ptr = posix_c.realpath(tmp_dir, null) orelse return error.PathNotFound;
    defer posix_c.free(abs_ptr);
    const abs_z: [*:0]const u8 = @ptrCast(abs_ptr);

    try watcher.addTarget(.{
        .path = abs_z,
        .events = sw.Event.all,
        .callback = testCallback,
    });

    try watcher.start();

    // Create a file to trigger event
    const f = posix_c.fopen(tmp_dir ++ "/test.txt", "w");
    if (f != null) _ = posix_c.fclose(f);
    defer _ = posix_c.unlink(tmp_dir ++ "/test.txt");

    // Wait for poll cycle (200ms via usleep)
    _ = posix_c.usleep(200 * 1000);

    watcher.stop();

    try std.testing.expect(test_event_count > 0);
}
