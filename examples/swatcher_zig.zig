const std = @import("std");
const sw = @import("swatcher");
const c = @cImport({
    @cInclude("stdio.h");
});

fn onEvent(event: sw.FsEvent, target: *sw.RawTarget, name: ?[*:0]const u8, data: ?*anyopaque) callconv(.c) void {
    _ = target;
    _ = data;
    _ = c.printf("[%s] %s\n", sw.eventName(event), name orelse "(unknown)");
}

pub fn main() !void {
    const allocator = std.heap.smp_allocator;

    var watcher = try sw.Watcher.init(allocator, .{
        .poll_interval_ms = 100,
        .enable_logging = true,
        .coalesce_ms = 200,
    }, null);
    defer watcher.deinit();

    try watcher.addTarget(.{
        .path = "./examples/test",
        .events = sw.Event.created | sw.Event.modified | sw.Event.deleted | sw.Event.moved,
        .recursive = true,
        .callback = onEvent,
    });

    try watcher.start();
    std.debug.print("Watching ./examples/test — press Enter to stop.\n", .{});

    _ = c.getchar();

    watcher.stop();
}
