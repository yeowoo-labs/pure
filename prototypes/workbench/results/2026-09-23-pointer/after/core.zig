//! Bounded scalar interaction VM experiment. No application behavior is built in.
//! Same source for native tests and freestanding Wasm; no allocation or imports.
//! Error ABI: 0 ok, 1 capacity, 2 invalid, 3 stack, 4 division by zero,
//! 5 non-finite, 6 bad branch, 7 bad state, 8 bad frame, 9 bad event.
//! Counters wrap modulo 2^32. Failed loads preserve the active program, authored
//! state and event diagnostics; beginning a load discards optional reuse records.
//! Failed events preserve values and counters but
//! report their error and attempted instruction count. Binding evaluation is pure.

const std = @import("std");
const Evaluation = @import("evaluation.zig").Evaluation;
const Tables = @import("evaluation.zig").Tables;

const max_states = 32;
const max_inputs = 8;
const max_instructions = 512;
const max_handlers = 64;
const max_stack = 64;
const no_pc = std.math.maxInt(u32);
const frame_dt: f64 = 1.0 / 60.0;

const Code = enum(u32) {
    ok = 0,
    capacity = 1,
    invalid = 2,
    stack = 3,
    division_by_zero = 4,
    non_finite = 5,
    bad_branch = 6,
    bad_state = 7,
    bad_frame = 8,
    bad_event = 9,
    work_limit = 10,
};

const Instruction = struct { value: f64 = 0, op: u32 = 0, arg: u32 = 0 };
const Handler = struct { event: u32 = 0, start: u32 = 0, length: u32 = 0 };
const Program = struct {
    instructions: [max_instructions]Instruction = @splat(.{}),
    handlers: [max_handlers]Handler = @splat(.{}),
    initials: [max_states]f64 = @splat(0),
    old_ids: [max_states]i32 = @splat(-1),
    declared: [max_states]bool = @splat(false),
    state_count: u32 = 0,
    input_count: u32 = 0,
    instruction_count: u32 = 0,
    handler_count: u32 = 0,
};
// Loading and result reuse are mutually exclusive phases. Current execution
// always reads active, and can recompute bindings while this workspace loads.
const Workspace = union { staging: Program, reuse: Tables };
const Engine = struct {
    active: Program = .{},
    workspace: Workspace = .{ .reuse = .{} },
    values: [max_states]f64 = @splat(0),
    staging_open: bool = false,
    staging_error: Code = .ok,
    tick: u32 = 0,
    last_error: Code = .ok,
    error_pc: u32 = no_pc,
    last_ops: u32 = 0,
    revision: u32 = 0,
    successful_events: u32 = 0,
    evaluation: Evaluation = .{},
};
var engine: Engine = .{};

fn reuseTables() ?*Tables {
    return if (engine.staging_open) null else &engine.workspace.reuse;
}

fn finishStaging() void {
    engine.workspace = .{ .reuse = .{} };
    engine.staging_open = false;
    engine.evaluation.forget(null, 3);
}

fn public(code: Code) u32 {
    return @intFromEnum(code);
}

fn stagingFail(code: Code) u32 {
    if (engine.staging_error == .ok) engine.staging_error = code;
    return public(code);
}

export fn reset_all() void {
    engine = .{};
}

export fn begin_program(state_count: u32, input_count: u32) u32 {
    engine.staging_error = .ok;
    if (state_count > max_states or input_count > max_inputs) {
        if (engine.staging_open) finishStaging();
        return stagingFail(.capacity);
    }
    engine.workspace = .{ .staging = .{ .state_count = state_count, .input_count = input_count } };
    engine.staging_open = true;
    engine.evaluation.forget(null, 3);
    return 0;
}

export fn abort_program() void {
    if (engine.staging_open) finishStaging();
}

export fn put_state(id: u32, initial: f64, old_id: i32) u32 {
    if (!engine.staging_open) return stagingFail(.invalid);
    if (id >= engine.workspace.staging.state_count or old_id < -1) return stagingFail(.bad_state);
    if (old_id >= 0 and @as(u32, @intCast(old_id)) >= engine.active.state_count) return stagingFail(.bad_state);
    if (engine.workspace.staging.declared[id]) return stagingFail(.bad_state);
    if (!std.math.isFinite(initial)) return stagingFail(.non_finite);
    engine.workspace.staging.initials[id] = initial;
    engine.workspace.staging.old_ids[id] = old_id;
    engine.workspace.staging.declared[id] = true;
    return 0;
}

export fn put_instruction(op: u32, arg: u32, value: f64) u32 {
    if (!engine.staging_open) return stagingFail(.invalid);
    const p = &engine.workspace.staging;
    if (p.instruction_count == max_instructions) return stagingFail(.capacity);
    if (op > 23) return stagingFail(.invalid);
    if (!std.math.isFinite(value)) return stagingFail(.non_finite);
    p.instructions[p.instruction_count] = .{ .op = op, .arg = arg, .value = value };
    p.instruction_count += 1;
    return 0;
}

fn validEvent(event: u32, inputs: u32) bool {
    return event <= inputs * 2 or
        (event >= 17 and event - 17 < inputs) or
        (event >= 25 and event - 25 < inputs);
}

export fn put_handler(event: u32, start: u32, length: u32) u32 {
    if (!engine.staging_open) return stagingFail(.invalid);
    const p = &engine.workspace.staging;
    if (p.handler_count == max_handlers) return stagingFail(.capacity);
    if (!validEvent(event, p.input_count)) return stagingFail(.bad_event);
    // Handlers may be appended before their instructions, so range validation
    // is deferred until commit. Arithmetic remains guarded there.
    p.handlers[p.handler_count] = .{ .event = event, .start = start, .length = length };
    p.handler_count += 1;
    return 0;
}

fn validate(p: *const Program) Code {
    for (0..p.state_count) |id| {
        if (!p.declared[id]) return .bad_state;
        if (!std.math.isFinite(p.initials[id])) return .non_finite;
        const old_id = p.old_ids[id];
        if (old_id < -1 or (old_id >= 0 and @as(u32, @intCast(old_id)) >= engine.active.state_count)) return .bad_state;
    }
    for (p.handlers[0..p.handler_count], 0..) |h, i| {
        if (!validEvent(h.event, p.input_count)) return .bad_event;
        if (h.start > p.instruction_count or h.length > p.instruction_count - h.start) return .invalid;
        if (h.length == 0) continue;
        for (p.handlers[0..i]) |other| {
            if (other.length != 0 and h.start < other.start + other.length and other.start < h.start + h.length) return .invalid;
        }
    }
    for (p.instructions[0..p.instruction_count], 0..) |inst, pc| {
        if (inst.op > 23) return .invalid;
        if (!std.math.isFinite(inst.value)) return .non_finite;
        if ((inst.op == 1 or inst.op == 20) and inst.arg >= p.state_count) return .bad_state;
        if (inst.op >= 20) {
            var owner: ?Handler = null;
            for (p.handlers[0..p.handler_count]) |h| {
                if (pc >= h.start and pc < h.start + h.length) {
                    owner = h;
                    break;
                }
            }
            const h = owner orelse return .invalid;
            if (inst.op == 21 and (inst.arg <= pc or inst.arg > h.start + h.length)) return .bad_branch;
        }
    }
    return .ok;
}

export fn commit_program() u32 {
    if (!engine.staging_open) return public(.invalid);
    if (engine.staging_error != .ok) return public(engine.staging_error);
    const code = validate(&engine.workspace.staging);
    if (code != .ok) return stagingFail(code);
    var next: [max_states]f64 = @splat(0);
    for (0..engine.workspace.staging.state_count) |i| {
        const old_id = engine.workspace.staging.old_ids[i];
        next[i] = if (old_id < 0) engine.workspace.staging.initials[i] else engine.values[@intCast(old_id)];
    }
    engine.active = engine.workspace.staging;
    engine.values = next;
    finishStaging();
    engine.revision +%= 1;
    engine.last_error = .ok;
    engine.error_pc = no_pc;
    engine.last_ops = 0;
    return 0;
}

export fn reset_state() void {
    engine.values = engine.active.initials;
    engine.evaluation.forget(reuseTables(), 3);
    engine.tick = 0;
    engine.successful_events = 0;
    engine.last_error = .ok;
    engine.error_pc = no_pc;
    engine.last_ops = 0;
}

const Execution = struct {
    code: Code = .ok,
    pc: u32 = no_pc,
    ops: u32 = 0,
    value: f64 = 0,
    dependencies: u32 = 0,
};

fn executionFail(result: Execution, code: Code, pc: u32) Execution {
    var failed = result;
    failed.code = code;
    failed.pc = pc;
    return failed;
}

const EventContext = struct { dt: f64 = 0, x: f64 = 0, y: f64 = 0, point: bool = false };

fn execute(values: *[max_states]f64, start: u32, length: u32, context: EventContext, pure: bool, budget: ?*Evaluation) Execution {
    const p = &engine.active;
    var result: Execution = .{};
    if (start > p.instruction_count or length > p.instruction_count - start) return executionFail(result, .invalid, no_pc);
    const end = start + length;
    var stack: [max_stack]f64 = undefined;
    var sp: usize = 0;
    var pc = start;
    while (pc < end) {
        if (budget) |b| {
            if (!b.spend()) return executionFail(result, .work_limit, pc);
            b.instructions += 1;
        }
        const inst = p.instructions[pc];
        result.ops += 1;
        if (pure and (inst.op == 2 or inst.op >= 20)) return executionFail(result, .invalid, pc);
        const need: usize = switch (inst.op) {
            0...2, 22, 23 => 0,
            7, 16, 20, 21 => 1,
            3...6, 8...15, 17, 18 => 2,
            19 => 3,
            else => return executionFail(result, .invalid, pc),
        };
        if (sp < need) return executionFail(result, .stack, pc);
        if (inst.op <= 2 or inst.op == 22 or inst.op == 23) {
            if (sp == max_stack) return executionFail(result, .stack, pc);
            const value = switch (inst.op) {
                0 => inst.value,
                1 => blk: {
                    if (inst.arg >= p.state_count) return executionFail(result, .bad_state, pc);
                    if (budget != null and budget.?.cache_count != 0)
                        result.dependencies |= @as(u32, 1) << @as(u5, @intCast(inst.arg));
                    break :blk values[inst.arg];
                },
                2 => context.dt,
                22, 23 => if (!context.point) return executionFail(result, .bad_event, pc) else if (inst.op == 22) context.x else context.y,
                else => unreachable,
            };
            if (!std.math.isFinite(value)) return executionFail(result, .non_finite, pc);
            stack[sp] = value;
            sp += 1;
        } else if (inst.op == 20) {
            if (inst.arg >= p.state_count) return executionFail(result, .bad_state, pc);
            sp -= 1;
            values[inst.arg] = stack[sp];
        } else if (inst.op == 21) {
            if (inst.arg <= pc or inst.arg > end) return executionFail(result, .bad_branch, pc);
            sp -= 1;
            if (stack[sp] == 0) {
                pc = inst.arg;
                continue;
            }
        } else {
            const a = stack[sp - need];
            const b = if (need >= 2) stack[sp - need + 1] else 0;
            const value: f64 = switch (inst.op) {
                3 => a + b,
                4 => a - b,
                5 => a * b,
                6 => blk: {
                    if (b == 0) return executionFail(result, .division_by_zero, pc);
                    break :blk a / b;
                },
                7 => -a,
                8 => @floatFromInt(@intFromBool(a < b)),
                9 => @floatFromInt(@intFromBool(a <= b)),
                10 => @floatFromInt(@intFromBool(a > b)),
                11 => @floatFromInt(@intFromBool(a >= b)),
                12 => @floatFromInt(@intFromBool(a == b)),
                13 => @floatFromInt(@intFromBool(a != b)),
                14 => @floatFromInt(@intFromBool(a != 0 and b != 0)),
                15 => @floatFromInt(@intFromBool(a != 0 or b != 0)),
                16 => @floatFromInt(@intFromBool(a == 0)),
                17 => @min(a, b),
                18 => @max(a, b),
                19 => blk: {
                    const hi = stack[sp - 1];
                    if (b > hi) return executionFail(result, .invalid, pc);
                    break :blk @max(b, @min(a, hi));
                },
                else => unreachable,
            };
            if (!std.math.isFinite(value)) return executionFail(result, .non_finite, pc);
            sp = sp - need + 1;
            stack[sp - 1] = value;
        }
        pc += 1;
    }
    if (sp != @as(usize, if (pure) 1 else 0)) return executionFail(result, .stack, end);
    if (pure) result.value = stack[0];
    return result;
}

export fn dispatch(event: u32, dt: f64) u32 {
    return dispatchContext(event, .{ .dt = dt });
}

export fn dispatch_point(event: u32, x: f64, y: f64) u32 {
    return dispatchContext(event, .{ .x = x, .y = y, .point = true });
}

fn dispatchContext(event: u32, context: EventContext) u32 {
    const dt = context.dt;
    engine.last_error = .ok;
    engine.error_pc = no_pc;
    engine.last_ops = 0;
    if (!validEvent(event, engine.active.input_count) or
        (context.point and event == 0) or (!context.point and event > engine.active.input_count * 2)) {
        engine.last_error = .bad_event;
        return public(.bad_event);
    }
    if (!std.math.isFinite(dt) or !std.math.isFinite(context.x) or !std.math.isFinite(context.y)) {
        engine.last_error = .non_finite;
        return public(.non_finite);
    }
    if ((event == 0 and dt != frame_dt) or (event != 0 and dt != 0)) {
        engine.last_error = .bad_frame;
        return public(.bad_frame);
    }
    var pending = engine.values;
    for (engine.active.handlers[0..engine.active.handler_count]) |h| {
        if (h.event != event) continue;
        const result = execute(&pending, h.start, h.length, context, false, null);
        engine.last_ops += result.ops;
        if (result.code != .ok) {
            engine.last_error = result.code;
            engine.error_pc = result.pc;
            return public(result.code);
        }
    }
    if (engine.evaluation.cache_count != 0 and !engine.staging_open) {
        var changed: u32 = 0;
        for (0..engine.active.state_count) |i| {
            // Signed zero is observable, so compare representation, not ==.
            if (@as(u64, @bitCast(pending[i])) != @as(u64, @bitCast(engine.values[i])))
                changed |= @as(u32, 1) << @as(u5, @intCast(i));
        }
        engine.evaluation.changed(reuseTables(), changed);
    }
    engine.values = pending;
    if (event == 0) engine.tick +%= 1;
    engine.successful_events +%= 1;
    return 0;
}

export fn eval_at(start: u32, length: u32) f64 {
    // A local copy makes the no-side-effects boundary structural as well as
    // checked by the pure instruction filter. Diagnostics are never touched.
    var values = engine.values;
    const result = execute(&values, start, length, .{}, true, null);
    return if (result.code == .ok) result.value else std.math.nan(f64);
}

export fn configure_evaluation(cache: u32, history: u32) u32 {
    return if (engine.evaluation.configure(reuseTables(), cache, history)) 0 else public(.capacity);
}

export fn forget_evaluation(flags: u32) void {
    engine.evaluation.forget(reuseTables(), flags);
}

export fn begin_evaluation(limit: u32) void {
    engine.evaluation.begin(limit);
}

export fn eval_bounded(start: u32, length: u32) f64 {
    const e = &engine.evaluation;
    e.status = 0;
    // Charge per request as well as per instruction and validation probe.
    if (!e.spend()) return std.math.nan(f64);
    if (e.lookup(reuseTables(), start, length)) |value| return value;
    if (e.status != 0) return std.math.nan(f64);
    var values = engine.values;
    const result = execute(&values, start, length, .{}, true, e);
    e.status = public(result.code);
    if (result.code != .ok) return std.math.nan(f64);
    e.remember(reuseTables(), start, length, result.value, result.dependencies);
    return result.value;
}

export fn evaluation_info(id: u32) u32 {
    const e = &engine.evaluation;
    return switch (id) {
        0 => e.status,
        1 => e.work,
        2 => e.instructions,
        3 => e.hits,
        4 => e.cache_count,
        5 => e.history_count,
        6 => @sizeOf(Evaluation) + @sizeOf(Tables),
        7 => @sizeOf(Evaluation),
        8 => @sizeOf(Tables),
        9 => @intFromBool(!engine.staging_open),
        10 => @sizeOf(Workspace) - @sizeOf(Program),
        else => 0,
    };
}

export fn get_state(id: u32) f64 {
    return if (id < engine.active.state_count) engine.values[id] else std.math.nan(f64);
}

export fn info(id: u32) u32 {
    return switch (id) {
        0 => engine.tick,
        1 => public(engine.last_error),
        2 => engine.error_pc,
        3 => engine.last_ops,
        4 => engine.revision,
        5 => engine.active.state_count,
        6 => engine.active.instruction_count,
        7 => engine.active.handler_count,
        8 => engine.active.input_count,
        9 => engine.successful_events,
        else => 0,
    };
}

export fn layout(id: u32) u32 {
    return switch (id) {
        0 => @sizeOf(Engine),
        1 => @sizeOf(Program),
        2 => @sizeOf(Instruction),
        3 => @sizeOf(Handler),
        4 => engine.active.instruction_count * @sizeOf(Instruction) + engine.active.handler_count * @sizeOf(Handler),
        5 => engine.active.state_count * @sizeOf(f64),
        6 => @sizeOf(Program),
        7 => max_stack * @sizeOf(f64),
        else => 0,
    };
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
        try ok(put_handler(0, 0, engine.workspace.staging.instruction_count));
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
    try std.testing.expectEqual(@as(u32, 12), layout(3));
    try std.testing.expectEqual(@as(u32, 76), layout(4));
    try std.testing.expectEqual(@as(u32, 8), layout(5));
    try std.testing.expectEqual(layout(1), layout(6));
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
    try std.testing.expect(@sizeOf(Workspace) >= @sizeOf(Program));
    try std.testing.expect(@sizeOf(Workspace) <= @sizeOf(Program) + @alignOf(Program));
    try std.testing.expectEqual(@as(u32, @sizeOf(Tables)), evaluation_info(8));
}
