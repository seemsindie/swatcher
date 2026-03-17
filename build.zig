const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    // ── C library module ───────────────────────────────────────────

    const c_mod = b.addModule("swatcher_c", .{
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });

    c_mod.addCSourceFiles(.{
        .files = &.{
            "src/core/swatcher.c",
            "src/core/log.c",
            "src/core/event.c",
            "src/core/target.c",
            "src/core/pattern.c",
            "src/core/error.c",
            "src/regex/re.c",
            "src/backend/backend_poll.c",
            "src/backend/backend_registry.c",
            "src/platform/platform_posix.c",
            "src/backend/backend_inotify.c",
        },
        .flags = &.{
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-D_DEFAULT_SOURCE",
            "-D_POSIX_C_SOURCE=200809L",
        },
    });

    c_mod.addIncludePath(b.path("include"));
    c_mod.addIncludePath(b.path("src"));
    c_mod.linkSystemLibrary("pthread", .{});

    const lib = b.addLibrary(.{
        .name = "swatcher",
        .root_module = c_mod,
    });

    b.installArtifact(lib);

    // ── Zig wrapper module ─────────────────────────────────────────

    const swatcher_mod = b.addModule("swatcher", .{
        .root_source_file = b.path("bindings/zig/swatcher.zig"),
        .target = target,
        .optimize = optimize,
    });
    swatcher_mod.addIncludePath(b.path("include"));
    swatcher_mod.addIncludePath(b.path("src"));
    swatcher_mod.linkLibrary(lib);

    // ── Zig integration test ───────────────────────────────────────

    const zig_test = b.addTest(.{
        .root_module = b.createModule(.{
            .root_source_file = b.path("bindings/zig/test_swatcher.zig"),
            .target = target,
            .optimize = optimize,
            .imports = &.{
                .{ .name = "swatcher", .module = swatcher_mod },
            },
        }),
    });
    zig_test.root_module.linkLibrary(lib);

    const run_zig_test = b.addRunArtifact(zig_test);
    const test_step = b.step("test-zig", "Run Zig wrapper integration tests");
    test_step.dependOn(&run_zig_test.step);
}
