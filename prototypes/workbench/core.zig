//! Browser ABI and regression tests for the explicit-instance scalar engine.
const std = @import("std");
const impl = @import("engine.zig");
const Engine = impl.Engine;
const Code = impl.Code;
const max_states = impl.max_states;
const max_instructions = impl.max_instructions;
const max_stack = impl.max_stack;
const no_pc = impl.no_pc;
const frame_dt = impl.frame_dt;
const Pending = impl.Pending;
const Workspace = impl.Workspace;
const Tables = impl.Tables;

var engine: Engine = .{};

fn public(code: Code) u32 {
    return @intFromEnum(code);
}

export fn reset_all() void {
    engine.reset_all();
}

export fn begin_program(state_count: u32, input_count: u32) u32 {
    return engine.begin_program(state_count, input_count);
}

export fn abort_program() void {
    engine.abort_program();
}

export fn put_state(id: u32, initial: f64, old_id: i32) u32 {
    return engine.put_state(id, initial, old_id);
}

export fn put_instruction(op: u32, arg: u32, value: f64) u32 {
    return engine.put_instruction(op, arg, value);
}

export fn put_handler(event: u32, start: u32, length: u32) u32 {
    return engine.put_handler(event, start, length);
}

export fn bind_handler(handler: u32, base: u32, count: u32) u32 {
    return engine.bind_handler(handler, base, count);
}

export fn put_pointer_target(input: u32, base: u32, count: u32) u32 {
    return engine.put_pointer_target(input, base, count);
}

export fn bind_target_handler(handler: u32, count: u32) u32 {
    return engine.bind_target_handler(handler, count);
}

export fn put_restored_value(id: u32, value: f64) u32 {
    return engine.put_restored_value(id, value);
}

export fn commit_program() u32 {
    return engine.commit_program();
}

export fn commit_restored_program() u32 {
    return engine.commit_restored_program();
}

export fn reset_state() void {
    engine.reset_state();
}

export fn dispatch(event: u32, dt: f64) u32 {
    return engine.dispatch(event, dt);
}

export fn dispatch_point(event: u32, x: f64, y: f64) u32 {
    return engine.dispatch_point(event, x, y);
}

export fn dispatch_target(event: u32, target: u32, x: f64, y: f64) u32 {
    return engine.dispatch_target(event, target, x, y);
}

export fn dispatch_value(event: u32, value: f64) u32 {
    return engine.dispatch_value(event, value);
}

export fn eval_at(start: u32, length: u32) f64 {
    return engine.eval_at(start, length);
}

export fn configure_evaluation(cache: u32, history: u32) u32 {
    return engine.configure_evaluation(cache, history);
}

export fn forget_evaluation(flags: u32) void {
    engine.forget_evaluation(flags);
}

export fn begin_evaluation(limit: u32) void {
    engine.begin_evaluation(limit);
}

export fn eval_bounded(start: u32, length: u32) f64 {
    return engine.eval_bounded(start, length);
}

export fn evaluation_info(id: u32) u32 {
    return engine.evaluation_info(id);
}

export fn get_state(id: u32) f64 {
    return engine.get_state(id);
}

export fn info(id: u32) u32 {
    return engine.info(id);
}

export fn layout(id: u32) u32 {
    return engine.layout(id);
}

// Test helpers describe programs directly; the product compiler is separately
// tested against this ABI. These tests exercise semantics and failure boundaries.
fn ok(code: u32) !void {
    try std.testing.expectEqual(@as(u32, 0), code);
}

fn emit(op: u32, arg: u32, value: f64) !void {
    try ok(put_instruction(op, arg, value));
}

fn counterProgram() !void {
    try ok(begin_program(1, 1));
    try ok(put_state(0, 10, -1));
    try emit(1, 0, 0);
    try emit(0, 0, 1);
    try emit(3, 0, 0);
    try emit(20, 0, 0);
    try ok(put_handler(1, 0, 4));
    try ok(commit_program());
}

test "blank program, no hidden application behavior, and reset" {
    reset_all();
    try ok(begin_program(0, 0));
    try ok(commit_program());
    try ok(dispatch(0, frame_dt));
    try std.testing.expectEqual(@as(u32, 1), info(0));
    try std.testing.expectEqual(@as(u32, 0), info(3));
    try std.testing.expectEqual(@as(u32, 0), info(5));
    try std.testing.expect(std.math.isNan(get_state(0)));
    try std.testing.expect(std.math.isNan(eval_at(0, 0)));
    reset_state();
    try std.testing.expectEqual(@as(u32, 0), info(0));
    try std.testing.expectEqual(@as(u32, 1), info(4));
}

test "arbitrary input counter and unused events" {
    reset_all();
    try counterProgram();
    try ok(dispatch(1, 0));
    try ok(dispatch(2, 0));
    try ok(dispatch(0, frame_dt));
    try ok(dispatch(1, 0));
    try std.testing.expectEqual(@as(f64, 12), get_state(0));
    try std.testing.expectEqual(@as(u32, 4), info(9));
    try std.testing.expectEqual(@as(u32, 4), info(3));
    reset_state();
    try std.testing.expectEqual(@as(f64, 10), get_state(0));
    try std.testing.expectEqual(@as(u32, 4), info(6));
}

test "charge and release behavior exists only in the authored instruction program" {
    reset_all();
    try ok(begin_program(3, 1)); // held, charge, released_value
    for (0..3) |i| try ok(put_state(@intCast(i), 0, -1));
    try emit(0, 0, 1);
    try emit(20, 0, 0);
    try ok(put_handler(1, 0, 2));
    try emit(1, 1, 0);
    try emit(1, 0, 0);
    try emit(2, 0, 0);
    try emit(5, 0, 0);
    try emit(3, 0, 0);
    try emit(20, 1, 0);
    try ok(put_handler(0, 2, 6));
    try emit(1, 1, 0);
    try emit(20, 2, 0);
    try emit(0, 0, 0);
    try emit(20, 0, 0);
    try emit(0, 0, 0);
    try emit(20, 1, 0);
    try ok(put_handler(2, 8, 6));
    try ok(commit_program());
    try ok(dispatch(0, frame_dt));
    try std.testing.expectEqual(@as(f64, 0), get_state(1));
    try ok(dispatch(1, 0));
    for (0..30) |_| try ok(dispatch(0, frame_dt));
    try ok(dispatch(2, 0));
    try std.testing.expectApproxEqAbs(@as(f64, 0.5), get_state(2), 1e-12);
    try std.testing.expectEqual(@as(f64, 0), get_state(1));
    for (0..30) |_| try ok(dispatch(0, frame_dt));
    try std.testing.expectEqual(@as(f64, 0), get_state(1));
}

test "forward condition executes once and assignments and handlers are ordered" {
    reset_all();
    try ok(begin_program(2, 1));
    try ok(put_state(0, 1, -1));
    try ok(put_state(1, 0, -1));
    try emit(1, 0, 0);
    try emit(21, 6, 0);
    try emit(0, 0, 0);
    try emit(20, 0, 0);
    try emit(0, 0, 7);
    try emit(20, 1, 0);
    try ok(put_handler(1, 0, 6));
    try emit(1, 1, 0);
    try emit(0, 0, 2);
    try emit(5, 0, 0);
    try emit(20, 1, 0);
    try ok(put_handler(1, 6, 4));
    try ok(commit_program());
    try ok(dispatch(1, 0));
    try std.testing.expectEqual(@as(f64, 14), get_state(1));
    try ok(dispatch(1, 0));
    try std.testing.expectEqual(@as(f64, 28), get_state(1));
    try std.testing.expectEqual(@as(u32, 6), info(3));
}

test "branch to program end is valid" {
    reset_all();
    try ok(begin_program(0, 0));
    try emit(0, 0, 0);
    try emit(21, 2, 0);
    try ok(put_handler(0, 0, 2));
    try ok(commit_program());
    try ok(dispatch(0, frame_dt));
}

test "one event rolls back writes from all handlers on arithmetic failure" {
    reset_all();
    try ok(begin_program(1, 0));
    try ok(put_state(0, 8, -1));
    try emit(0, 0, 99);
    try emit(20, 0, 0);
    try ok(put_handler(0, 0, 2));
    try emit(0, 0, 1);
    try emit(0, 0, 0);
    try emit(6, 0, 0);
    try emit(20, 0, 0);
    try ok(put_handler(0, 2, 4));
    try ok(commit_program());
    try std.testing.expectEqual(public(.division_by_zero), dispatch(0, frame_dt));
    try std.testing.expectEqual(@as(f64, 8), get_state(0));
    try std.testing.expectEqual(@as(u32, 0), info(0));
    try std.testing.expectEqual(@as(u32, 0), info(9));
    try std.testing.expectEqual(@as(u32, 4), info(2));
    try std.testing.expectEqual(@as(u32, 5), info(3));
}

test "nonfinite arithmetic and invalid clamp fail atomically" {
    reset_all();
    try ok(begin_program(1, 0));
    try ok(put_state(0, 5, -1));
    try emit(0, 0, 1e308);
    try emit(0, 0, 1e308);
    try emit(5, 0, 0);
    try emit(20, 0, 0);
    try ok(put_handler(0, 0, 4));
    try ok(commit_program());
    try std.testing.expectEqual(public(.non_finite), dispatch(0, frame_dt));
    try std.testing.expectEqual(@as(f64, 5), get_state(0));
    try ok(begin_program(1, 0));
    try ok(put_state(0, 5, 0));
    try emit(0, 0, 1);
    try emit(0, 0, 10);
    try emit(0, 0, 2);
    try emit(19, 0, 0);
    try emit(20, 0, 0);
    try ok(put_handler(0, 0, 5));
    try ok(commit_program());
    try std.testing.expectEqual(public(.invalid), dispatch(0, frame_dt));
    try std.testing.expectEqual(@as(f64, 5), get_state(0));
}

test "stack underflow overflow and leftover values reject the event" {
    for (0..3) |mode| {
        reset_all();
        try ok(begin_program(0, 0));
        if (mode == 0) {
            try emit(3, 0, 0);
        } else {
            for (0..@as(usize, if (mode == 1) 65 else 1)) |_| try emit(0, 0, 7);
        }
        try ok(put_handler(0, 0, engine.workspace.staging.program.instruction_count));
        try ok(commit_program());
        try std.testing.expectEqual(public(.stack), dispatch(0, frame_dt));
        try std.testing.expectEqual(@as(u32, 0), info(0));
    }
}

test "failed candidates leave active values program revision and diagnostics unchanged" {
    reset_all();
    try counterProgram();
    try ok(dispatch(1, 0));
    const old_revision = info(4);
    const old_ops = info(3);
    try ok(begin_program(1, 1));
    try ok(put_state(0, 900, 0));
    try emit(1, 5, 0);
    try std.testing.expectEqual(public(.bad_state), commit_program());
    try std.testing.expectEqual(old_revision, info(4));
    try std.testing.expectEqual(old_ops, info(3));
    try std.testing.expectEqual(@as(f64, 11), get_state(0));
    try ok(dispatch(1, 0));
    try std.testing.expectEqual(@as(f64, 12), get_state(0));
    try ok(begin_program(0, 0));
    try std.testing.expectEqual(public(.invalid), put_instruction(100, 0, 0));
    try std.testing.expectEqual(public(.invalid), commit_program());
    try std.testing.expectEqual(@as(f64, 12), get_state(0));
}

test "state preservation mapping reads active values at commit and reset uses new initials" {
    reset_all();
    try counterProgram();
    try ok(dispatch(1, 0));
    try ok(begin_program(2, 0));
    try ok(put_state(0, 42, -1)); // new symbol first
    try ok(put_state(1, 100, 0)); // old symbol moves from id 0 to 1
    try ok(dispatch(1, 0)); // changed after staging began
    try ok(commit_program());
    try std.testing.expectEqual(@as(f64, 42), get_state(0));
    try std.testing.expectEqual(@as(f64, 12), get_state(1));
    try std.testing.expectEqual(@as(u32, 2), info(9));
    reset_state();
    try std.testing.expectEqual(@as(f64, 100), get_state(1));
    try std.testing.expectEqual(@as(u32, 2), info(4));
    try ok(begin_program(0, 0));
    try ok(commit_program());
    try std.testing.expect(std.math.isNan(get_state(0)));
}

test "capacity boundaries and sticky load errors" {
    reset_all();
    try std.testing.expectEqual(public(.capacity), begin_program(33, 0));
    try std.testing.expectEqual(public(.capacity), begin_program(0, 9));
    try ok(begin_program(32, 8));
    for (0..32) |i| try ok(put_state(@intCast(i), @floatFromInt(i), -1));
    for (0..512) |_| try emit(0, 0, 0);
    for (0..64) |_| try ok(put_handler(16, 0, 0));
    try ok(commit_program());
    try std.testing.expectEqual(@as(u32, 512), info(6));
    try std.testing.expectEqual(@as(u32, 64), info(7));
    try std.testing.expectEqual(@as(f64, 31), get_state(31));
    try ok(begin_program(0, 0));
    for (0..512) |_| try emit(0, 0, 0);
    try std.testing.expectEqual(public(.capacity), put_instruction(0, 0, 0));
    try std.testing.expectEqual(public(.capacity), commit_program());
    try ok(begin_program(0, 0));
    for (0..64) |_| try ok(put_handler(0, 0, 0));
    try std.testing.expectEqual(public(.capacity), put_handler(0, 0, 0));
    try std.testing.expectEqual(public(.capacity), commit_program());
}

test "invalid declaration values state mappings event ids and handler ranges" {
    reset_all();
    try ok(begin_program(1, 0));
    try std.testing.expectEqual(public(.bad_state), commit_program());
    try ok(begin_program(1, 0));
    try std.testing.expectEqual(public(.bad_state), put_state(0, 0, 0));
    try ok(begin_program(1, 0));
    try std.testing.expectEqual(public(.non_finite), put_state(0, std.math.inf(f64), -1));
    try ok(begin_program(1, 0));
    try ok(put_state(0, 0, -1));
    try std.testing.expectEqual(public(.bad_state), put_state(0, 1, -1));
    try ok(begin_program(0, 0));
    try std.testing.expectEqual(public(.non_finite), put_instruction(0, 0, std.math.nan(f64)));
    try ok(begin_program(0, 0));
    try std.testing.expectEqual(public(.bad_event), put_handler(1, 0, 0));
    try ok(begin_program(0, 0));
    try ok(put_handler(0, no_pc, 5));
    try std.testing.expectEqual(public(.invalid), commit_program());
    try ok(begin_program(0, 0));
    try emit(0, 0, 1);
    try ok(put_handler(0, 0, 1));
    try ok(put_handler(0, 0, 1));
    try std.testing.expectEqual(public(.invalid), commit_program());
}

test "backward self out-of-handler branches and stores without handlers are rejected" {
    for ([_]u32{ 0, 1, 3 }) |target| {
        reset_all();
        try ok(begin_program(0, 0));
        try emit(0, 0, 0);
        try emit(21, target, 0);
        try emit(0, 0, 7);
        try ok(put_handler(0, 0, 2));
        try std.testing.expectEqual(public(.bad_branch), commit_program());
    }
    reset_all();
    try ok(begin_program(1, 0));
    try ok(put_state(0, 0, -1));
    try emit(0, 0, 5);
    try emit(20, 0, 0);
    try std.testing.expectEqual(public(.invalid), commit_program());
}

test "event inputs and dt enforce deterministic frame contract" {
    reset_all();
    try counterProgram();
    try std.testing.expectEqual(public(.bad_event), dispatch(3, 0));
    try std.testing.expectEqual(public(.bad_event), dispatch(no_pc, 0));
    try std.testing.expectEqual(public(.bad_frame), dispatch(0, 0));
    try std.testing.expectEqual(public(.bad_frame), dispatch(1, frame_dt));
    try std.testing.expectEqual(public(.non_finite), dispatch(0, std.math.nan(f64)));
    try std.testing.expectEqual(@as(u32, 0), info(9));
    try std.testing.expectEqual(@as(f64, 10), get_state(0));
    try ok(dispatch(1, 0));
    try std.testing.expectEqual(@as(u32, 0), info(1));
    try std.testing.expectEqual(no_pc, info(2));
}

test "binding evaluation is pure on both success and failure" {
    reset_all();
    try ok(begin_program(1, 0));
    try ok(put_state(0, 4, -1));
    try emit(1, 0, 0);
    try emit(0, 0, 3);
    try emit(5, 0, 0);
    try emit(20, 0, 0);
    try emit(2, 0, 0);
    try emit(0, 0, 0);
    try emit(21, 7, 0);
    try ok(put_handler(0, 3, 4));
    try ok(commit_program());
    try std.testing.expectEqual(public(.bad_frame), dispatch(0, 0));
    const before = engine;
    try std.testing.expectEqual(@as(f64, 12), eval_at(0, 3));
    try std.testing.expect(std.math.isNan(eval_at(0, 4))); // attempted store
    try std.testing.expect(std.math.isNan(eval_at(4, 1))); // dt forbidden in views
    try std.testing.expect(std.math.isNan(eval_at(5, 2))); // branch forbidden
    try std.testing.expect(std.math.isNan(eval_at(no_pc, 7)));
    try std.testing.expect(std.math.isNan(eval_at(0, no_pc)));
    try std.testing.expect(std.math.isNan(eval_at(0, 2))); // two results
    // Compare only the live member of the untagged workspace, never padding
    // or the inactive member's indeterminate storage.
    try std.testing.expectEqual(before.staging_open, engine.staging_open);
    inline for (std.meta.fields(Engine)) |field| {
        if (comptime std.mem.eql(u8, field.name, "workspace")) {
            if (before.staging_open) {
                try std.testing.expectEqualDeep(before.workspace.staging, engine.workspace.staging);
            } else {
                try std.testing.expectEqualDeep(before.workspace.reuse, engine.workspace.reuse);
            }
        } else try std.testing.expectEqualDeep(@field(before, field.name), @field(engine, field.name));
    }
}

test "arithmetic comparison logical min max and clamp expression semantics" {
    const cases = [_]struct { op: u32, a: f64, b: f64, want: f64 }{
        .{ .op = 3, .a = 7, .b = 2, .want = 9 },
        .{ .op = 4, .a = 7, .b = 2, .want = 5 },
        .{ .op = 5, .a = 7, .b = 2, .want = 14 },
        .{ .op = 6, .a = 7, .b = 2, .want = 3.5 },
        .{ .op = 8, .a = 2, .b = 7, .want = 1 },
        .{ .op = 9, .a = 7, .b = 7, .want = 1 },
        .{ .op = 10, .a = 2, .b = 7, .want = 0 },
        .{ .op = 11, .a = 7, .b = 7, .want = 1 },
        .{ .op = 12, .a = 7, .b = 2, .want = 0 },
        .{ .op = 13, .a = 7, .b = 2, .want = 1 },
        .{ .op = 14, .a = -7, .b = 2, .want = 1 },
        .{ .op = 15, .a = 0, .b = 2, .want = 1 },
        .{ .op = 17, .a = 7, .b = 2, .want = 2 },
        .{ .op = 18, .a = 7, .b = 2, .want = 7 },
    };
    for (cases) |case| {
        reset_all();
        try ok(begin_program(0, 0));
        try emit(0, 0, case.a);
        try emit(0, 0, case.b);
        try emit(case.op, 0, 0);
        try ok(commit_program());
        try std.testing.expectEqual(case.want, eval_at(0, 3));
    }
    reset_all();
    try ok(begin_program(0, 0));
    try emit(0, 0, -8);
    try emit(7, 0, 0);
    try emit(0, 0, 0);
    try emit(0, 0, 5);
    try emit(19, 0, 0);
    try emit(16, 0, 0);
    try ok(commit_program());
    try std.testing.expectEqual(@as(f64, 5), eval_at(0, 5));
    try std.testing.expectEqual(@as(f64, 0), eval_at(0, 6));
}

test "diagnostic counters wrap without affecting scalar values" {
    reset_all();
    try counterProgram();
    engine.tick = no_pc;
    engine.successful_events = no_pc;
    engine.revision = no_pc;
    try ok(dispatch(0, frame_dt));
    try std.testing.expectEqual(@as(u32, 0), info(0));
    try std.testing.expectEqual(@as(u32, 0), info(9));
    try ok(begin_program(1, 0));
    try ok(put_state(0, 99, 0));
    try ok(commit_program());
    try std.testing.expectEqual(@as(u32, 0), info(4));
    try std.testing.expectEqual(@as(f64, 10), get_state(0));
}

test "memory accounting separates reserved storage used IR and stack capacity" {
    reset_all();
    try std.testing.expectEqual(@as(u32, 0), layout(4));
    try std.testing.expectEqual(@as(u32, 0), layout(5));
    try counterProgram();
    try std.testing.expectEqual(@as(u32, @sizeOf(Engine)), layout(0));
    try std.testing.expectEqual(@as(u32, 16), layout(2));
    try std.testing.expectEqual(@as(u32, 16), layout(3));
    try std.testing.expectEqual(@as(u32, 80), layout(4));
    try std.testing.expectEqual(@as(u32, 8), layout(5));
    try std.testing.expectEqual(@as(u32, @sizeOf(Pending)), layout(6));
    try std.testing.expect(layout(6) >= layout(1) + max_states * @sizeOf(f64));
    try std.testing.expect(layout(0) >= layout(1) * 2);
    try std.testing.expectEqual(@as(u32, 512), layout(7));
    try std.testing.expect(layout(0) + 16 * 1024 < 64 * 1024);
}

fn displayCounterProgram() !void {
    try ok(begin_program(1, 1));
    try ok(put_state(0, 10, -1));
    try emit(1, 0, 0);
    try emit(0, 0, 1);
    try emit(3, 0, 0);
    try emit(20, 0, 0);
    try ok(put_handler(1, 0, 4));
    try emit(1, 0, 0);
    try emit(0, 0, 2);
    try emit(5, 0, 0);
    try ok(commit_program());
    try ok(configure_evaluation(32, 8));
}

fn boundedEquals(want: f64, start: u32, length: u32) !void {
    begin_evaluation(100);
    try std.testing.expectEqual(want, eval_bounded(start, length));
    try ok(evaluation_info(0));
}

test "point coordinates are ephemeral event operands and unavailable to pure evaluation" {
    reset_all();
    try ok(begin_program(2, 1));
    try ok(put_state(0, 0, -1));
    try ok(put_state(1, 0, -1));
    const events = [_]u32{ 1, 17, 2, 25 };
    for (events, 0..) |event, i| {
        try emit(22, 0, 0);
        try emit(20, 0, 0);
        try emit(23, 0, 0);
        try emit(20, 1, 0);
        try ok(put_handler(event, @intCast(i * 4), 4));
    }
    try ok(commit_program());
    for (events, 0..) |event, i| {
        const x: f64 = @floatFromInt(i + 10);
        try ok(dispatch_point(event, x, -x));
        try std.testing.expectEqual(x, get_state(0));
        try std.testing.expectEqual(-x, get_state(1));
    }
    try std.testing.expectEqual(public(.bad_event), dispatch(1, 0));
    try std.testing.expectEqual(@as(f64, 13), get_state(0));
    const diagnostics = [_]u32{ info(1), info(2), info(3), info(9) };
    try std.testing.expect(std.math.isNan(eval_at(0, 1)));
    try std.testing.expectEqualSlices(u32, &diagnostics, &.{ info(1), info(2), info(3), info(9) });
    try std.testing.expectEqual(@as(u32, 4), info(9));
}

test "point event ranges reject holes and invalid payloads without committing" {
    reset_all();
    try counterProgram();
    for ([_]u32{ 0, 3, 16, 18, 24, 26, 32, 33, no_pc }) |event|
        try std.testing.expectEqual(public(.bad_event), dispatch_point(event, 1, 2));
    try std.testing.expectEqual(public(.non_finite), dispatch_point(1, std.math.nan(f64), 0));
    try std.testing.expectEqual(public(.non_finite), dispatch_point(1, 0, std.math.inf(f64)));
    try std.testing.expectEqual(@as(f64, 10), get_state(0));
    try std.testing.expectEqual(@as(u32, 0), info(9));
    try ok(begin_program(0, 8));
    try ok(put_handler(24, 0, 0));
    try ok(put_handler(32, 0, 0));
    try ok(commit_program());
    try ok(dispatch_point(24, 1, 2));
    try ok(dispatch_point(32, 1, 2));
    try std.testing.expectEqual(public(.bad_event), dispatch(24, 0));
    try std.testing.expectEqual(@as(u32, 2), info(9));
}

test "point event failure rolls back writes from earlier matching handlers" {
    reset_all();
    try ok(begin_program(1, 1));
    try ok(put_state(0, 7, -1));
    try emit(22, 0, 0);
    try emit(20, 0, 0);
    try ok(put_handler(17, 0, 2));
    try emit(23, 0, 0);
    try emit(0, 0, 0);
    try emit(6, 0, 0);
    try emit(20, 0, 0);
    try ok(put_handler(17, 2, 4));
    try ok(commit_program());
    try std.testing.expectEqual(public(.division_by_zero), dispatch_point(17, 400, 200));
    try std.testing.expectEqual(@as(f64, 7), get_state(0));
    try std.testing.expectEqual(@as(u32, 0), info(9));
    try std.testing.expectEqual(@as(u32, 4), info(2));
}

test "loading shares storage with records while old events and bindings remain executable" {
    reset_all();
    try displayCounterProgram();
    try boundedEquals(20, 4, 3);
    try boundedEquals(20, 4, 3);
    try std.testing.expectEqual(@as(u32, 1), evaluation_info(3));
    try ok(begin_program(1, 1));
    try ok(put_state(0, 900, 0));
    try std.testing.expectEqual(@as(u32, 0), evaluation_info(9));
    try emit(1, 0, 0);
    try ok(dispatch(1, 0)); // Active state advances after staging begins.
    try boundedEquals(22, 4, 3);
    try std.testing.expectEqual(@as(u32, 0), evaluation_info(3));
    try ok(configure_evaluation(32, 1));
    forget_evaluation(3); // Must not overwrite the pending program.
    try emit(0, 0, 3);
    try emit(5, 0, 0);
    try ok(commit_program());
    try std.testing.expectEqual(@as(f64, 11), get_state(0));
    try std.testing.expectEqual(@as(u32, 1), evaluation_info(9));
    try boundedEquals(33, 0, 3);
    try boundedEquals(33, 0, 3);
    try std.testing.expectEqual(@as(u32, 1), evaluation_info(3));
}

test "failed and abandoned candidates restore empty reuse storage without changing active work" {
    reset_all();
    try displayCounterProgram();
    try ok(begin_program(1, 1));
    try ok(put_state(0, 900, 0));
    try emit(20, 0, 0); // Store without a handler fails validation.
    try std.testing.expectEqual(public(.invalid), commit_program());
    try boundedEquals(20, 4, 3);
    try ok(dispatch(1, 0));
    abort_program();
    try boundedEquals(22, 4, 3);
    try boundedEquals(22, 4, 3);
    try std.testing.expectEqual(@as(u32, 1), evaluation_info(3));
    abort_program(); // Inactive abort is a no-op, including for warmed records.
    try boundedEquals(22, 4, 3);
    try std.testing.expectEqual(@as(u32, 1), evaluation_info(3));
    try ok(begin_program(1, 1));
    try std.testing.expectEqual(public(.capacity), begin_program(max_states + 1, 0));
    try std.testing.expectEqual(@as(u32, 1), evaluation_info(9));
    try boundedEquals(22, 4, 3);
    try std.testing.expectEqual(@as(f64, 11), get_state(0));
}

test "full pending program survives repeated evaluation configuration record loss and reset" {
    reset_all();
    try displayCounterProgram();
    try ok(dispatch(1, 0));
    try ok(begin_program(1, 1));
    try ok(put_state(0, 900, 0));
    for (0..max_instructions) |i| {
        try emit(0, 0, @floatFromInt(i));
        try ok(configure_evaluation(32, @intCast(i % 9)));
        forget_evaluation(3);
        try boundedEquals(22, 4, 3);
    }
    reset_state(); // The pending definition and old-id mapping survive reset.
    try boundedEquals(20, 4, 3);
    try ok(commit_program());
    try std.testing.expectEqual(@as(u32, max_instructions), info(6));
    try std.testing.expectEqual(@as(f64, 10), get_state(0));
    try boundedEquals(511, 511, 1);
    // Safe Zig builds may reserve a hidden union tag in addition to the payload.
    try std.testing.expect(@sizeOf(Workspace) >= @sizeOf(Pending));
    try std.testing.expect(@sizeOf(Workspace) <= @sizeOf(Pending) + @alignOf(Pending));
    try std.testing.expectEqual(@as(u32, @sizeOf(Tables)), evaluation_info(8));
}

test "restored definition and values commit together without changing reset initializers" {
    reset_all();
    try counterProgram();
    try ok(dispatch(1, 0));
    try ok(dispatch(0, frame_dt));
    const revision = info(4);
    try ok(begin_program(2, 0));
    try ok(put_state(0, 100, -1));
    try ok(put_state(1, 200, -1));
    try ok(put_restored_value(1, -0.0));
    try ok(put_restored_value(0, 37));
    try std.testing.expectEqual(@as(f64, 11), get_state(0));
    try ok(dispatch(1, 0)); // Old execution remains independent of pending values.
    try ok(commit_restored_program());
    try std.testing.expectEqual(@as(f64, 37), get_state(0));
    try std.testing.expectEqual(@as(u64, @bitCast(@as(f64, -0.0))), @as(u64, @bitCast(get_state(1))));
    try std.testing.expectEqual(revision + 1, info(4));
    try std.testing.expectEqual(@as(u32, 0), info(0));
    try std.testing.expectEqual(@as(u32, 0), info(9));
    reset_state();
    try std.testing.expectEqual(@as(f64, 100), get_state(0));
    try std.testing.expectEqual(@as(f64, 200), get_state(1));
}

test "missing duplicate nonfinite or invalid restored candidates leave active work untouched" {
    reset_all();
    try counterProgram();
    try ok(dispatch(1, 0));
    const revision = info(4);
    for (0..4) |mode| {
        try ok(begin_program(1, 0));
        try ok(put_state(0, 999, -1));
        if (mode == 1) {
            try ok(put_restored_value(0, 30));
            try std.testing.expectEqual(public(.bad_state), put_restored_value(0, 40));
        } else if (mode == 2) {
            try std.testing.expectEqual(public(.non_finite), put_restored_value(0, std.math.inf(f64)));
        } else if (mode == 3) {
            try ok(put_restored_value(0, 30));
            try emit(20, 0, 0); // Invalid store outside a handler.
        }
        try std.testing.expect(commit_restored_program() != 0);
        try std.testing.expectEqual(@as(f64, 11), get_state(0));
        try std.testing.expectEqual(revision, info(4));
        try std.testing.expectEqual(@as(u32, 1), info(9));
        abort_program();
    }
    try ok(begin_program(1, 0));
    try ok(put_state(0, 100, -1));
    try ok(put_restored_value(0, 42));
    try std.testing.expectEqual(public(.bad_state), commit_program());
    abort_program();
    try ok(dispatch(1, 0));
    try std.testing.expectEqual(@as(f64, 12), get_state(0));
}

test "empty and full restored programs validate all values and discard old display records" {
    reset_all();
    try displayCounterProgram();
    try ok(configure_evaluation(32, 8));
    try boundedEquals(20, 4, 3);
    try ok(begin_program(32, 0));
    for (0..32) |i| {
        try ok(put_state(@intCast(i), 0, -1));
        try ok(put_restored_value(@intCast(i), @floatFromInt(i + 1)));
    }
    try emit(1, 31, 0);
    try ok(commit_restored_program());
    try boundedEquals(32, 0, 1);
    try ok(begin_program(0, 0));
    try ok(commit_restored_program());
    try std.testing.expectEqual(@as(u32, 0), info(5));
    try std.testing.expect(std.math.isNan(get_state(0)));
}

test "shared handler bodies access their own bound state slots in source order" {
    reset_all();
    try ok(begin_program(2, 1));
    try ok(put_state(0, 10, -1));
    try ok(put_state(1, 20, -1));
    try emit(24, 0, 0);
    try emit(0, 0, 1);
    try emit(3, 0, 0);
    try emit(25, 0, 0);
    try ok(put_handler(1, 0, 4));
    try ok(bind_handler(0, 0, 1));
    try ok(put_handler(1, 0, 4));
    try ok(bind_handler(1, 1, 1));
    try ok(put_handler(1, 0, 4));
    try ok(bind_handler(2, 0, 1));
    try ok(commit_program());
    try ok(dispatch(1, 0));
    try std.testing.expectEqual(@as(f64, 12), get_state(0));
    try std.testing.expectEqual(@as(f64, 21), get_state(1));
    try std.testing.expectEqual(@as(u32, 4), info(6)); // Three bindings, one body.
    try std.testing.expectEqual(@as(u32, 12), info(3));
    const diagnostics = [_]u32{ info(1), info(2), info(3), info(9) };
    try std.testing.expect(std.math.isNan(eval_at(0, 1)));
    try std.testing.expectEqualSlices(u32, &diagnostics, &.{ info(1), info(2), info(3), info(9) });
}

test "failure in a later shared binding rolls back all earlier bindings" {
    reset_all();
    try ok(begin_program(4, 1));
    for ([_]f64{ 8, 2, 12, 0 }, 0..) |value, i| try ok(put_state(@intCast(i), value, -1));
    try emit(24, 0, 0);
    try emit(24, 1, 0);
    try emit(6, 0, 0);
    try emit(25, 0, 0);
    for (0..2) |i| {
        try ok(put_handler(1, 0, 4));
        try ok(bind_handler(@intCast(i), @intCast(i * 2), 2));
    }
    try ok(commit_program());
    try std.testing.expectEqual(public(.division_by_zero), dispatch(1, 0));
    try std.testing.expectEqual(@as(f64, 8), get_state(0));
    try std.testing.expectEqual(@as(f64, 12), get_state(2));
    try std.testing.expectEqual(@as(u32, 0), info(9));
}

test "shared bindings reject missing arguments partial overlaps and invalid slot mappings" {
    reset_all();
    try counterProgram();
    const revision = info(4);
    for (0..5) |mode| {
        try ok(begin_program(1, 1));
        try ok(put_state(0, 9, -1));
        try emit(24, 0, 0);
        try emit(25, 0, 0);
        try ok(put_handler(1, 0, 2));
        switch (mode) {
            0 => {}, // Parameter op without a binding.
            1 => {
                try ok(bind_handler(0, 0, 1));
                try std.testing.expectEqual(public(.invalid), bind_handler(0, 0, 1));
            },
            2 => {
                try std.testing.expectEqual(public(.bad_state), bind_handler(0, 0, 2));
            },
            3 => {
                try ok(bind_handler(0, 0, 1));
                try ok(put_handler(1, 1, 1));
                try ok(bind_handler(1, 0, 1));
            },
            else => {
                try ok(bind_handler(0, 0, 1));
                try ok(put_handler(1, 0, 2)); // Another owner missing its mapping.
            },
        }
        try std.testing.expect(commit_program() != 0);
        abort_program();
        try std.testing.expectEqual(@as(f64, 10), get_state(0));
        try std.testing.expectEqual(revision, info(4));
    }
}

test "one routed handler changes only its event target and rejects missing or foreign routes" {
    reset_all();
    try ok(begin_program(2, 2));
    try ok(put_state(0, 10, -1));
    try ok(put_state(1, 20, -1));
    try ok(put_pointer_target(0, 0, 1));
    try ok(put_pointer_target(0, 1, 1));
    try emit(22, 0, 0);
    try emit(25, 0, 0);
    try ok(put_handler(17, 0, 2));
    try ok(bind_target_handler(0, 1));
    try ok(commit_program());
    try ok(dispatch_target(17, 1, 77, 0));
    try std.testing.expectEqual(@as(f64, 10), get_state(0));
    try std.testing.expectEqual(@as(f64, 77), get_state(1));
    try std.testing.expectEqual(@as(u32, 2), info(3));
    try std.testing.expectEqual(public(.bad_event), dispatch_point(17, 99, 0));
    try std.testing.expectEqual(public(.bad_event), dispatch(1, 0));
    try std.testing.expectEqual(public(.bad_event), dispatch_target(17, 2, 99, 0));
    try std.testing.expectEqual(public(.bad_event), dispatch_target(17, no_pc, 99, 0));
    try std.testing.expectEqual(public(.bad_event), dispatch_target(18, 0, 99, 0));
    try std.testing.expectEqual(public(.bad_event), dispatch_target(0, 0, 99, 0));
    try std.testing.expectEqual(public(.non_finite), dispatch_target(17, 0, std.math.inf(f64), 0));
    try std.testing.expectEqual(@as(u32, 1), info(9));
    try std.testing.expectEqual(@as(f64, 10), get_state(0));
    try std.testing.expectEqual(@as(f64, 77), get_state(1));
    try std.testing.expectEqual(@as(u32, 3), layout(8));
    try std.testing.expectEqual(@as(u32, 2), layout(9));
    try std.testing.expectEqual(@as(u32, 54), layout(4));
}

test "routes reject invalid mappings and bindings without replacing active work" {
    for (0..7) |mode| {
        reset_all();
        try counterProgram();
        try ok(begin_program(2, 2));
        try ok(put_state(0, 0, -1));
        try ok(put_state(1, 0, -1));
        switch (mode) {
            0 => try std.testing.expectEqual(public(.bad_state), put_pointer_target(0, 1, 2)),
            1 => try std.testing.expectEqual(public(.bad_event), put_pointer_target(2, 0, 1)),
            2 => {
                try ok(put_pointer_target(0, 0, 2));
                try std.testing.expectEqual(public(.invalid), put_pointer_target(1, 1, 1));
            },
            else => {
                if (mode != 3) try ok(put_pointer_target(0, 0, 1));
                try emit(0, 0, 2);
                try emit(25, 0, 0);
                try ok(put_handler(if (mode == 4) 0 else 1, 0, 2));
                try ok(bind_target_handler(0, if (mode == 5) 2 else 1));
                if (mode == 6) try std.testing.expectEqual(public(.invalid), bind_target_handler(0, 1));
            },
        }
        try std.testing.expect(commit_program() != 0);
        // Active program still executes while the failed candidate occupies staging.
        try ok(dispatch(1, 0));
        try std.testing.expectEqual(@as(f64, 11), get_state(0));
        abort_program();
        try std.testing.expectEqual(@as(u32, 0), layout(9));
    }
    reset_all();
    try ok(begin_program(32, 1));
    for (0..32) |i| try ok(put_state(@intCast(i), 0, -1));
    for (0..24) |i| try ok(put_pointer_target(0, @intCast(i), 1));
    try std.testing.expectEqual(public(.capacity), put_pointer_target(0, 24, 1));
    try std.testing.expectEqual(public(.capacity), commit_program());
}

test "routed event rollback includes globals and earlier handlers and survives staged route replacement" {
    reset_all();
    try ok(begin_program(3, 1));
    for (0..3) |i| try ok(put_state(@intCast(i), @floatFromInt(i), -1));
    try ok(put_pointer_target(0, 1, 1));
    try ok(put_pointer_target(0, 2, 1));
    try emit(22, 0, 0);
    try emit(25, 0, 0);
    try ok(put_handler(17, 0, 2));
    try ok(bind_target_handler(0, 1));
    try emit(0, 0, 99);
    try emit(20, 0, 0);
    try emit(0, 0, 1);
    try emit(23, 0, 0);
    try emit(6, 0, 0);
    try emit(25, 0, 0);
    try ok(put_handler(17, 2, 6));
    try ok(bind_target_handler(1, 1));
    try ok(commit_program());
    try ok(begin_program(1, 1));
    try ok(put_state(0, 0, -1));
    try ok(put_pointer_target(0, 0, 1));
    try std.testing.expectEqual(public(.division_by_zero), dispatch_target(17, 1, 88, 0));
    try std.testing.expectEqual(@as(f64, 0), get_state(0));
    try std.testing.expectEqual(@as(f64, 1), get_state(1));
    try std.testing.expectEqual(@as(f64, 2), get_state(2));
    abort_program();
    try ok(dispatch_target(17, 1, 88, 2));
    try std.testing.expectEqual(@as(f64, 99), get_state(0));
    try std.testing.expectEqual(@as(f64, 1), get_state(1));
    try std.testing.expectEqual(@as(f64, 0.5), get_state(2));
}

test "scalar inputs carry a finite ephemeral value without adding state or pointer ownership" {
    reset_all();
    try ok(begin_program(1, 1));
    try ok(put_state(0, 50, -1));
    try emit(26, 0, 0);
    try emit(0, 0, 20);
    try emit(0, 0, 80);
    try emit(19, 0, 0);
    try emit(20, 0, 0);
    try ok(put_handler(33, 0, 5));
    try ok(commit_program());
    try ok(dispatch_value(33, 200));
    try std.testing.expectEqual(@as(f64, 80), get_state(0));
    try ok(dispatch_value(33, -200));
    try std.testing.expectEqual(@as(f64, 20), get_state(0));
    try std.testing.expectEqual(@as(u32, 1), info(5));
    try std.testing.expect(std.math.isNan(eval_at(0, 1)));
    try std.testing.expectEqual(@as(u32, 2), info(9));
    for ([_]u32{ 0, 1, 17, 25, 34, no_pc }) |event|
        try std.testing.expectEqual(public(.bad_event), dispatch_value(event, 60));
    try std.testing.expectEqual(public(.bad_event), dispatch(33, 0));
    try std.testing.expectEqual(public(.bad_event), dispatch_point(33, 60, 0));
    try std.testing.expectEqual(public(.non_finite), dispatch_value(33, std.math.nan(f64)));
    try std.testing.expectEqual(@as(f64, 20), get_state(0));
    try std.testing.expectEqual(@as(u32, 2), info(9));
}

test "scalar input failure rolls back earlier writes and old scalar events survive staging" {
    reset_all();
    try ok(begin_program(1, 1));
    try ok(put_state(0, 10, -1));
    try emit(26, 0, 0);
    try emit(20, 0, 0);
    try ok(put_handler(33, 0, 2));
    try emit(0, 0, 1);
    try emit(26, 0, 0);
    try emit(6, 0, 0);
    try emit(20, 0, 0);
    try ok(put_handler(33, 2, 4));
    try ok(commit_program());
    try ok(begin_program(0, 0));
    try std.testing.expectEqual(public(.division_by_zero), dispatch_value(33, 0));
    try std.testing.expectEqual(@as(f64, 10), get_state(0));
    try ok(dispatch_value(33, 2));
    try std.testing.expectEqual(@as(f64, 0.5), get_state(0));
    abort_program();
    try std.testing.expectEqual(@as(u32, 1), info(9));
}
