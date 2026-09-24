//! Bounded scalar interaction VM experiment. No application behavior is built in.
//! Same source for native tests and freestanding Wasm; no allocation or imports.
//! Error ABI: 0 ok, 1 capacity, 2 invalid, 3 stack, 4 division by zero,
//! 5 non-finite, 6 bad branch, 7 bad state, 8 bad frame, 9 bad event.
//! Counters wrap modulo 2^32. Failed loads preserve the active program, authored
//! state and event diagnostics; beginning a load discards optional reuse records.
//! Failed events preserve values and counters but
//! report their error and attempted instruction count. Binding evaluation is pure.

pub const std = @import("std");
const root = @import("root");
const fixed_definition = @hasDecl(root, "pure_definition");
pub const Evaluation = @import("evaluation.zig").Evaluation;
pub const Tables = @import("evaluation.zig").Tables;

// A fixed native definition may request exact capacities from its generated
// build root. Browser/authoring builds retain the full language limits.
pub const max_states = if (@hasDecl(root, "pure_max_states")) root.pure_max_states else 32;
pub const max_inputs = if (@hasDecl(root, "pure_max_inputs")) root.pure_max_inputs else 8;
pub const max_instructions = if (@hasDecl(root, "pure_max_instructions")) root.pure_max_instructions else 512;
pub const max_handlers = if (@hasDecl(root, "pure_max_handlers")) root.pure_max_handlers else 64;
pub const max_stack = 64;
pub const max_pointer_targets = if (@hasDecl(root, "pure_max_pointer_targets")) root.pure_max_pointer_targets else 24;
comptime {
    if (max_states > 32 or max_inputs > 8 or max_instructions > 512 or
        max_handlers > 64 or max_pointer_targets > 24)
        @compileError("native profile exceeds the language limits");
}
pub const event_target = 255;
pub const no_pc = std.math.maxInt(u32);
pub const frame_dt: f64 = 1.0 / 60.0;

pub const Code = enum(u32) {
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

pub const Instruction = struct { value: f64 = 0, op: u32 = 0, arg: u32 = 0 };
pub const Handler = struct {
    event: u32 = 0,
    start: u32 = 0,
    length: u32 = 0,
    parameter_base: u8 = 0,
    parameter_count: u8 = 0,
};
pub const PointerTarget = struct { input: u8, base: u8, count: u8 };
pub const Program = struct {
    pointer_targets: [max_pointer_targets]PointerTarget = undefined,
    pointer_target_count: u8 = 0,
    routed_inputs: u8 = 0,
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
const shared_program: Program = if (fixed_definition) blk: {
    const definition = root.pure_definition;
    var program: Program = .{};
    program.state_count = definition.states_count;
    program.input_count = definition.inputs_count;
    program.instruction_count = definition.instructions_count;
    program.handler_count = definition.handlers_count;
    program.pointer_target_count = definition.pointer_targets_count;
    program.routed_inputs = definition.routed_inputs;
    for (definition.initials, 0..) |value, i| program.initials[i] = value;
    for (definition.code, 0..) |inst, i|
        program.instructions[i] = .{ .value = inst.value, .op = inst.op, .arg = inst.arg };
    for (definition.handlers_data, 0..) |handler, i|
        program.handlers[i] = .{ .event = handler.event, .start = handler.start,
                                 .length = handler.length, .parameter_base = handler.base,
                                 .parameter_count = handler.count };
    for (definition.targets_data, 0..) |target, i|
        program.pointer_targets[i] = .{ .input = target.input, .base = target.base, .count = target.count };
    break :blk program;
} else .{};
// Loading a definition/optional restored values and result reuse are mutually exclusive phases. Current execution
// always reads active, and can recompute bindings while this workspace loads.
pub const Pending = struct {
    program: Program = .{},
    values: [max_states]f64 = @splat(0),
    value_mask: u32 = 0,
};
pub const Workspace = union { staging: Pending, reuse: Tables };
pub const Engine = struct {
    active: if (fixed_definition) bool else Program = if (fixed_definition) false else .{},
    workspace: if (fixed_definition) void else Workspace = if (fixed_definition) {} else .{ .reuse = .{} },
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

    fn program(self: *Engine) *const Program {
        return if (comptime fixed_definition) &shared_program else &self.active;
    }

    pub fn reuseTables(self: *Engine) ?*Tables {
        if (comptime fixed_definition) return null;
        return if (self.staging_open) null else &self.workspace.reuse;
    }

    pub fn finishStaging(self: *Engine) void {
        if (comptime fixed_definition) return;
        self.workspace = .{ .reuse = .{} };
        self.staging_open = false;
        self.evaluation.forget(null, 3);
    }

    pub fn public(_: *Engine, code: Code) u32 {
        return @intFromEnum(code);
    }

    pub fn stagingFail(self: *Engine, code: Code) u32 {
        if (self.staging_error == .ok) self.staging_error = code;
        return self.public(code);
    }

    pub fn reset_all(self: *Engine) void {
        self.* = .{};
    }

    pub fn installFixed(self: *Engine, identity: *const [32]u8,
                        state_count: u32, input_count: u32, instruction_count: u32,
                        handler_count: u32, target_count: u32, restored: ?[*]const f64) u32 {
        if (comptime !fixed_definition) return self.public(.invalid);
        const definition = root.pure_definition;
        if (!std.mem.eql(u8, identity, &definition.identity) or
            state_count != definition.states_count or input_count != definition.inputs_count or
            instruction_count != definition.instructions_count or
            handler_count != definition.handlers_count or target_count != definition.pointer_targets_count)
            return self.public(.invalid);
        var next: [max_states]f64 = @splat(0);
        for (0..state_count) |i| {
            const value = if (restored) |values| values[i] else shared_program.initials[i];
            if (!std.math.isFinite(value)) return self.public(.non_finite);
            next[i] = value;
        }
        self.active = true;
        self.values = next;
        self.staging_open = false;
        self.staging_error = .ok;
        self.evaluation.forget(null, 3);
        if (restored != null) {
            self.tick = 0;
            self.successful_events = 0;
        }
        self.revision +%= 1;
        self.last_error = .ok;
        self.error_pc = no_pc;
        self.last_ops = 0;
        return 0;
    }

    pub fn begin_program(self: *Engine, state_count: u32, input_count: u32) u32 {
        self.staging_error = .ok;
        if (state_count > max_states or input_count > max_inputs) {
            if (self.staging_open) self.finishStaging();
            return self.stagingFail(.capacity);
        }
        self.workspace = .{ .staging = .{ .program = .{ .state_count = state_count, .input_count = input_count } } };
        self.staging_open = true;
        self.evaluation.forget(null, 3);
        return 0;
    }

    pub fn abort_program(self: *Engine) void {
        if (self.staging_open) self.finishStaging();
    }

    pub fn put_state(self: *Engine, id: u32, initial: f64, old_id: i32) u32 {
        if (!self.staging_open) return self.stagingFail(.invalid);
        if (id >= self.workspace.staging.program.state_count or old_id < -1) return self.stagingFail(.bad_state);
        if (old_id >= 0 and @as(u32, @intCast(old_id)) >= self.program().state_count) return self.stagingFail(.bad_state);
        if (self.workspace.staging.program.declared[id]) return self.stagingFail(.bad_state);
        if (!std.math.isFinite(initial)) return self.stagingFail(.non_finite);
        self.workspace.staging.program.initials[id] = initial;
        self.workspace.staging.program.old_ids[id] = old_id;
        self.workspace.staging.program.declared[id] = true;
        return 0;
    }

    pub fn put_instruction(self: *Engine, op: u32, arg: u32, value: f64) u32 {
        if (!self.staging_open) return self.stagingFail(.invalid);
        const p = &self.workspace.staging.program;
        if (p.instruction_count == max_instructions) return self.stagingFail(.capacity);
        if (op > 26) return self.stagingFail(.invalid);
        if (!std.math.isFinite(value)) return self.stagingFail(.non_finite);
        p.instructions[p.instruction_count] = .{ .op = op, .arg = arg, .value = value };
        p.instruction_count += 1;
        return 0;
    }

    pub fn validEvent(_: *Engine, event: u32, inputs: u32) bool {
        return event <= inputs * 2 or
            (event >= 17 and event - 17 < inputs) or
            (event >= 25 and event - 25 < inputs) or
            (event >= 33 and event - 33 < inputs);
    }

    pub fn put_handler(self: *Engine, event: u32, start: u32, length: u32) u32 {
        if (!self.staging_open) return self.stagingFail(.invalid);
        const p = &self.workspace.staging.program;
        if (p.handler_count == max_handlers) return self.stagingFail(.capacity);
        if (!self.validEvent(event, p.input_count)) return self.stagingFail(.bad_event);
        // Handlers may be appended before their instructions, so range validation
        // is deferred until commit. Arithmetic remains guarded there.
        p.handlers[p.handler_count] = .{ .event = event, .start = start, .length = length };
        p.handler_count += 1;
        return 0;
    }

    pub fn bind_handler(self: *Engine, handler: u32, base: u32, count: u32) u32 {
        if (!self.staging_open) return self.stagingFail(.invalid);
        const p = &self.workspace.staging.program;
        if (handler >= p.handler_count or base >= p.state_count) return self.stagingFail(.bad_state);
        if (count == 0 or count > 8 or count > p.state_count - base) return self.stagingFail(.bad_state);
        const h = &p.handlers[handler];
        if (h.parameter_count != 0) return self.stagingFail(.invalid);
        h.parameter_base = @intCast(base);
        h.parameter_count = @intCast(count);
        return 0;
    }

    // A route is a static program-local index, never a persistent object identity.
    pub fn put_pointer_target(self: *Engine, input: u32, base: u32, count: u32) u32 {
        if (!self.staging_open) return self.stagingFail(.invalid);
        const p = &self.workspace.staging.program;
        if (p.pointer_target_count == max_pointer_targets) return self.stagingFail(.capacity);
        if (input >= p.input_count) return self.stagingFail(.bad_event);
        if (base >= p.state_count or count == 0 or count > 8 or count > p.state_count - base) return self.stagingFail(.bad_state);
        for (p.pointer_targets[0..p.pointer_target_count]) |t| {
            if (base < @as(u32, t.base) + t.count and t.base < base + count) return self.stagingFail(.invalid);
            if (t.input == input and t.count != count) return self.stagingFail(.bad_state);
        }
        p.pointer_targets[p.pointer_target_count] = .{ .input = @intCast(input), .base = @intCast(base), .count = @intCast(count) };
        p.pointer_target_count += 1;
        p.routed_inputs |= @as(u8, 1) << @as(u3, @intCast(input));
        return 0;
    }

    pub fn bind_target_handler(self: *Engine, handler: u32, count: u32) u32 {
        if (!self.staging_open) return self.stagingFail(.invalid);
        const p = &self.workspace.staging.program;
        if (handler >= p.handler_count or count == 0 or count > 8) return self.stagingFail(.bad_state);
        const h = &p.handlers[handler];
        if (h.parameter_count != 0) return self.stagingFail(.invalid);
        h.parameter_base = event_target;
        h.parameter_count = @intCast(count);
        return 0;
    }

    pub fn eventInput(_: *Engine, event: u32) u32 {
        return if (event >= 33) event - 33 else if (event >= 25) event - 25 else if (event >= 17) event - 17 else (event - 1) / 2;
    }

    pub fn validate(self: *Engine, p: *const Program) Code {
        for (0..p.state_count) |id| {
            if (!p.declared[id]) return .bad_state;
            if (!std.math.isFinite(p.initials[id])) return .non_finite;
            const old_id = p.old_ids[id];
            if (old_id < -1 or (old_id >= 0 and @as(u32, @intCast(old_id)) >= self.program().state_count)) return .bad_state;
        }
        for (p.handlers[0..p.handler_count], 0..) |h, i| {
            if (!self.validEvent(h.event, p.input_count)) return .bad_event;
            if (h.start > p.instruction_count or h.length > p.instruction_count - h.start) return .invalid;
            if (h.parameter_base == event_target) {
                if (h.event == 0 or h.event >= 33 or h.parameter_count == 0 or h.parameter_count > 8) return .bad_event;
                var found = false;
                for (p.pointer_targets[0..p.pointer_target_count]) |t| {
                    if (t.input != self.eventInput(h.event)) continue;
                    if (t.count != h.parameter_count) return .bad_state;
                    found = true;
                }
                if (!found) return .bad_event;
            } else if (h.parameter_count > 8 or @as(u32, h.parameter_base) + h.parameter_count > p.state_count) return .bad_state;
            if (h.length == 0) continue;
            for (p.handlers[0..i]) |other| {
                if (other.length != 0 and h.start < other.start + other.length and other.start < h.start + h.length) {
                    // Bound rules may share exactly one body, never a partial range.
                    if (h.parameter_count == 0 or h.parameter_count != other.parameter_count or
                        h.start != other.start or h.length != other.length) return .invalid;
                }
            }
        }
        for (p.instructions[0..p.instruction_count], 0..) |inst, pc| {
            if (inst.op > 26) return .invalid;
            if (!std.math.isFinite(inst.value)) return .non_finite;
            if ((inst.op == 1 or inst.op == 20) and inst.arg >= p.state_count) return .bad_state;
            if (inst.op >= 20) {
                var owner: ?Handler = null;
                for (p.handlers[0..p.handler_count]) |h| {
                    if (pc >= h.start and pc < h.start + h.length) {
                        owner = h;
                        if (inst.op == 21 and (inst.arg <= pc or inst.arg > h.start + h.length)) return .bad_branch;
                        if ((inst.op == 24 or inst.op == 25) and inst.arg >= h.parameter_count) return .bad_state;
                    }
                }
                if (owner == null) return .invalid;
            }
        }
        return .ok;
    }

    pub fn put_restored_value(self: *Engine, id: u32, value: f64) u32 {
        if (!self.staging_open) return self.stagingFail(.invalid);
        const pending = &self.workspace.staging;
        if (id >= pending.program.state_count) return self.stagingFail(.bad_state);
        const bit = @as(u32, 1) << @as(u5, @intCast(id));
        if (pending.value_mask & bit != 0) return self.stagingFail(.bad_state);
        if (!std.math.isFinite(value)) return self.stagingFail(.non_finite);
        pending.values[id] = value;
        pending.value_mask |= bit;
        return 0;
    }

    pub fn commit_program(self: *Engine) u32 {
        return self.commit(false);
    }

    pub fn commit_restored_program(self: *Engine) u32 {
        return self.commit(true);
    }

    pub fn commit(self: *Engine, restoring: bool) u32 {
        if (!self.staging_open) return self.public(.invalid);
        if (self.staging_error != .ok) return self.public(self.staging_error);
        const count = self.workspace.staging.program.state_count;
        const expected: u32 = if (count == 32) 0xffffffff else (@as(u32, 1) << @as(u5, @intCast(count))) - 1;
        if (self.workspace.staging.value_mask != (if (restoring) expected else @as(u32, 0)))
            return self.stagingFail(.bad_state);
        const code = self.validate(&self.workspace.staging.program);
        if (code != .ok) return self.stagingFail(code);
        var next: [max_states]f64 = @splat(0);
        for (0..self.workspace.staging.program.state_count) |i| {
            const old_id = self.workspace.staging.program.old_ids[i];
            next[i] = if (restoring) self.workspace.staging.values[i] else if (old_id < 0) self.workspace.staging.program.initials[i] else self.values[@intCast(old_id)];
        }
        self.active = self.workspace.staging.program;
        self.values = next;
        self.finishStaging();
        if (restoring) {
            self.tick = 0;
            self.successful_events = 0;
        }
        self.revision +%= 1;
        self.last_error = .ok;
        self.error_pc = no_pc;
        self.last_ops = 0;
        return 0;
    }

    pub fn reset_state(self: *Engine) void {
        self.values = if (fixed_definition and !self.active) @splat(0) else self.program().initials;
        self.evaluation.forget(self.reuseTables(), 3);
        self.tick = 0;
        self.successful_events = 0;
        self.last_error = .ok;
        self.error_pc = no_pc;
        self.last_ops = 0;
    }

    const Execution = struct {
        code: Code = .ok,
        pc: u32 = no_pc,
        ops: u32 = 0,
        value: f64 = 0,
        dependencies: u32 = 0,
    };

    pub fn executionFail(_: *Engine, result: Execution, code: Code, pc: u32) Execution {
        var failed = result;
        failed.code = code;
        failed.pc = pc;
        return failed;
    }

    const EventContext = struct { dt: f64 = 0, x: f64 = 0, y: f64 = 0, parameter_base: u32 = 0, parameter_count: u32 = 0, point: bool = false, scalar: bool = false, target: ?u32 = null };

    pub fn execute(self: *Engine, values: *[max_states]f64, start: u32, length: u32, context: EventContext, pure: bool, budget: ?*Evaluation) Execution {
        var result: Execution = .{};
        if (comptime fixed_definition) {
            if (!self.active) {
                if (start != 0 or length != 0) return self.executionFail(result, .invalid, no_pc);
                if (pure) return self.executionFail(result, .stack, 0);
                return result;
            }
        }
        const p = self.program();
        if (start > p.instruction_count or length > p.instruction_count - start) return self.executionFail(result, .invalid, no_pc);
        const end = start + length;
        var stack: [max_stack]f64 = undefined;
        var sp: usize = 0;
        var pc = start;
        while (pc < end) {
            if (budget) |b| {
                if (!b.spend()) return self.executionFail(result, .work_limit, pc);
                b.instructions += 1;
            }
            const inst = p.instructions[pc];
            result.ops += 1;
            if (pure and (inst.op == 2 or inst.op >= 20)) return self.executionFail(result, .invalid, pc);
            const need: usize = switch (inst.op) {
                0...2, 22, 23, 24, 26 => 0,
                7, 16, 20, 21, 25 => 1,
                3...6, 8...15, 17, 18 => 2,
                19 => 3,
                else => return self.executionFail(result, .invalid, pc),
            };
            if (sp < need) return self.executionFail(result, .stack, pc);
            if (inst.op <= 2 or inst.op == 22 or inst.op == 23 or inst.op == 24 or inst.op == 26) {
                if (sp == max_stack) return self.executionFail(result, .stack, pc);
                const value = switch (inst.op) {
                    0 => inst.value,
                    1 => blk: {
                        if (inst.arg >= p.state_count) return self.executionFail(result, .bad_state, pc);
                        if (budget != null and budget.?.cache_count != 0)
                            result.dependencies |= @as(u32, 1) << @as(u5, @intCast(inst.arg));
                        break :blk values[inst.arg];
                    },
                    24 => blk: {
                        if (inst.arg >= context.parameter_count) return self.executionFail(result, .bad_state, pc);
                        const state = context.parameter_base + inst.arg;
                        if (state >= p.state_count) return self.executionFail(result, .bad_state, pc);
                        break :blk values[state];
                    },
                    26 => if (context.scalar) context.x else return self.executionFail(result, .bad_event, pc),
                    2 => context.dt,
                    22, 23 => if (!context.point) return self.executionFail(result, .bad_event, pc) else if (inst.op == 22) context.x else context.y,
                    else => unreachable,
                };
                if (!std.math.isFinite(value)) return self.executionFail(result, .non_finite, pc);
                stack[sp] = value;
                sp += 1;
            } else if (inst.op == 20 or inst.op == 25) {
                if (inst.op == 25 and inst.arg >= context.parameter_count) return self.executionFail(result, .bad_state, pc);
                const state = if (inst.op == 25) context.parameter_base + inst.arg else inst.arg;
                if (state >= p.state_count) return self.executionFail(result, .bad_state, pc);
                sp -= 1;
                values[state] = stack[sp];
            } else if (inst.op == 21) {
                if (inst.arg <= pc or inst.arg > end) return self.executionFail(result, .bad_branch, pc);
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
                        if (b == 0) return self.executionFail(result, .division_by_zero, pc);
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
                        if (b > hi) return self.executionFail(result, .invalid, pc);
                        break :blk @max(b, @min(a, hi));
                    },
                    else => unreachable,
                };
                if (!std.math.isFinite(value)) return self.executionFail(result, .non_finite, pc);
                sp = sp - need + 1;
                stack[sp - 1] = value;
            }
            pc += 1;
        }
        if (sp != @as(usize, if (pure) 1 else 0)) return self.executionFail(result, .stack, end);
        if (pure) result.value = stack[0];
        return result;
    }

    pub fn dispatch(self: *Engine, event: u32, dt: f64) u32 {
        return self.dispatchContext(event, .{ .dt = dt });
    }

    pub fn dispatch_point(self: *Engine, event: u32, x: f64, y: f64) u32 {
        return self.dispatchContext(event, .{ .x = x, .y = y, .point = true });
    }

    pub fn dispatch_target(self: *Engine, event: u32, target: u32, x: f64, y: f64) u32 {
        return self.dispatchContext(event, .{ .x = x, .y = y, .point = true, .target = target });
    }

    pub fn dispatch_value(self: *Engine, event: u32, value: f64) u32 {
        return self.dispatchContext(event, .{ .x = value, .scalar = true });
    }

    pub fn dispatchContext(self: *Engine, event: u32, context: EventContext) u32 {
        const dt = context.dt;
        self.last_error = .ok;
        self.error_pc = no_pc;
        self.last_ops = 0;
        if (comptime fixed_definition) {
            if (!self.active) {
                if (event != 0 or context.point or context.scalar) {
                    self.last_error = .bad_event;
                    return self.public(.bad_event);
                }
                if (!std.math.isFinite(dt) or !std.math.isFinite(context.x) or !std.math.isFinite(context.y)) {
                    self.last_error = .non_finite;
                    return self.public(.non_finite);
                }
                if (dt != frame_dt) {
                    self.last_error = .bad_frame;
                    return self.public(.bad_frame);
                }
                self.tick +%= 1;
                self.successful_events +%= 1;
                return 0;
            }
        }
        if (!self.validEvent(event, self.program().input_count) or
            ((context.point or context.scalar) and event == 0) or
            (context.scalar != (event >= 33)) or
            (!context.point and !context.scalar and event > self.program().input_count * 2))
        {
            self.last_error = .bad_event;
            return self.public(.bad_event);
        }
        if (!std.math.isFinite(dt) or !std.math.isFinite(context.x) or !std.math.isFinite(context.y)) {
            self.last_error = .non_finite;
            return self.public(.non_finite);
        }
        if ((event == 0 and dt != frame_dt) or (event != 0 and dt != 0)) {
            self.last_error = .bad_frame;
            return self.public(.bad_frame);
        }
        var route: ?PointerTarget = null;
        const routed = event != 0 and self.program().routed_inputs & (@as(u8, 1) << @as(u3, @intCast(self.eventInput(event)))) != 0;
        if (context.target) |target| {
            if (!routed or target >= self.program().pointer_target_count or
                self.program().pointer_targets[target].input != self.eventInput(event))
            {
                self.last_error = .bad_event;
                return self.public(.bad_event);
            }
            route = self.program().pointer_targets[target];
        } else if (routed) {
            self.last_error = .bad_event;
            return self.public(.bad_event);
        }
        var pending = self.values;
        for (self.program().handlers[0..self.program().handler_count]) |h| {
            if (h.event != event) continue;
            var bound = context;
            bound.parameter_base = if (h.parameter_base == event_target) route.?.base else h.parameter_base;
            bound.parameter_count = h.parameter_count;
            const result = self.execute(&pending, h.start, h.length, bound, false, null);
            self.last_ops += result.ops;
            if (result.code != .ok) {
                self.last_error = result.code;
                self.error_pc = result.pc;
                return self.public(result.code);
            }
        }
        if (self.evaluation.cache_count != 0 and !self.staging_open) {
            var changed: u32 = 0;
            for (0..self.program().state_count) |i| {
                // Signed zero is observable, so compare representation, not ==.
                if (@as(u64, @bitCast(pending[i])) != @as(u64, @bitCast(self.values[i])))
                    changed |= @as(u32, 1) << @as(u5, @intCast(i));
            }
            self.evaluation.changed(self.reuseTables(), changed);
        }
        self.values = pending;
        if (event == 0) self.tick +%= 1;
        self.successful_events +%= 1;
        return 0;
    }

    pub fn eval_at(self: *Engine, start: u32, length: u32) f64 {
        // A local copy makes the no-side-effects boundary structural as well as
        // checked by the pure instruction filter. Diagnostics are never touched.
        var values = self.values;
        const result = self.execute(&values, start, length, .{}, true, null);
        return if (result.code == .ok) result.value else std.math.nan(f64);
    }

    pub fn configure_evaluation(self: *Engine, cache: u32, history: u32) u32 {
        if (comptime fixed_definition) if (cache != 0) return self.public(.capacity);
        return if (self.evaluation.configure(self.reuseTables(), cache, history)) 0 else self.public(.capacity);
    }

    pub fn forget_evaluation(self: *Engine, flags: u32) void {
        self.evaluation.forget(self.reuseTables(), flags);
    }

    pub fn begin_evaluation(self: *Engine, limit: u32) void {
        self.evaluation.begin(limit);
    }

    pub fn eval_bounded(self: *Engine, start: u32, length: u32) f64 {
        const e = &self.evaluation;
        e.status = 0;
        // Charge per request as well as per instruction and validation probe.
        if (!e.spend()) return std.math.nan(f64);
        if (e.lookup(self.reuseTables(), start, length)) |value| return value;
        if (e.status != 0) return std.math.nan(f64);
        var values = self.values;
        const result = self.execute(&values, start, length, .{}, true, e);
        e.status = self.public(result.code);
        if (result.code != .ok) return std.math.nan(f64);
        e.remember(self.reuseTables(), start, length, result.value, result.dependencies);
        return result.value;
    }

    pub fn evaluation_info(self: *Engine, id: u32) u32 {
        const e = &self.evaluation;
        return switch (id) {
            0 => e.status,
            1 => e.work,
            2 => e.instructions,
            3 => e.hits,
            4 => e.cache_count,
            5 => e.history_count,
            6 => @sizeOf(Evaluation) + if (fixed_definition) @as(u32, 0) else @as(u32, @sizeOf(Tables)),
            7 => @sizeOf(Evaluation),
            8 => if (fixed_definition) 0 else @sizeOf(Tables),
            9 => @intFromBool(!self.staging_open),
            10 => if (fixed_definition) 0 else @sizeOf(Workspace) - @sizeOf(Pending),
            else => 0,
        };
    }

    pub fn get_state(self: *Engine, id: u32) f64 {
        if (comptime fixed_definition) if (!self.active) return std.math.nan(f64);
        return if (id < self.program().state_count) self.values[id] else std.math.nan(f64);
    }

    pub fn info(self: *Engine, id: u32) u32 {
        const loaded = if (fixed_definition) self.active else true;
        return switch (id) {
            0 => self.tick,
            1 => self.public(self.last_error),
            2 => self.error_pc,
            3 => self.last_ops,
            4 => self.revision,
            5 => if (loaded) self.program().state_count else 0,
            6 => if (loaded) self.program().instruction_count else 0,
            7 => if (loaded) self.program().handler_count else 0,
            8 => if (loaded) self.program().input_count else 0,
            9 => self.successful_events,
            else => 0,
        };
    }

    pub fn layout(self: *Engine, id: u32) u32 {
        if (comptime fixed_definition) {
            if (!self.active and (id == 4 or id == 5 or id == 9)) return 0;
        }
        return switch (id) {
            0 => @sizeOf(Engine),
            1 => @sizeOf(Program),
            2 => @sizeOf(Instruction),
            3 => @sizeOf(Handler),
            4 => self.program().instruction_count * @sizeOf(Instruction) + self.program().handler_count * @sizeOf(Handler) + @as(u32, self.program().pointer_target_count) * @sizeOf(PointerTarget),
            5 => self.program().state_count * @sizeOf(f64),
            6 => if (fixed_definition) 0 else @sizeOf(Pending),
            7 => max_stack * @sizeOf(f64),
            8 => @sizeOf(PointerTarget),
            9 => self.program().pointer_target_count,
            else => 0,
        };
    }
};
