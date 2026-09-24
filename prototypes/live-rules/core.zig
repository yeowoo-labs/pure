//! A bounded, deterministic practice-game experiment, not a general runtime.
//! The same source is tested natively and compiled to freestanding WebAssembly.
//! All persistent storage is static; functions perform no allocation.

const std = @import("std");

const Rules = struct {
    mode: u32 = 0,
    impulse: u32 = 10,
    enabled: u32 = 1,
    revision: u32 = 0,
};

const State = struct {
    tick: u32 = 0,
    y: i32 = 0,
    vy: i32 = 0,
    distance: u32 = 0,
    obstacle_x: i32 = 800,
    passed: u32 = 0,
    hits: u32 = 0,
    jumps_used: u32 = 0,
    running: u32 = 0,
    last_reason: u32 = 0,
    last_source: u32 = std.math.maxInt(u32),
    total_inputs: u32 = 0,
    accepted_jumps: u32 = 0,
    obstacle_height: u32 = 48,
    rng: u32 = 0x50555245,
    obstacle_touched: u32 = 0,
    obstacle_passed: u32 = 0,
};

const Decision = struct {
    tick: u32 = 0,
    revision: u32 = 0,
    y: i32 = 0,
    jumps: u32 = 0,
    values: [6]u8 = @splat(0),
    valid: bool = false,
};

const Event = struct {
    tick: u32 = 0,
    kind: u32 = 0,
    detail: u32 = 0,
    revision: u32 = 0,
};

const history_capacity: u32 = 16;

const History = struct {
    entries: [history_capacity]Event = @splat(.{}),
    next: u32 = 0,
    count: u32 = 0,
};

const Engine = struct {
    rules: Rules = .{},
    state: State = .{},
    decision: Decision = .{},
    history: History = .{},
};

const Op = enum(u8) { request, enabled, grounded, extra, select, all };
const Node = struct { op: Op, a: u8 = 0, b: u8 = 0, c: u8 = 0 };

// Node indices are topologically ordered. These descriptors drive evaluation;
// the display is observing this actual graph, not a separate diagram model.
const graph = [_]Node{
    .{ .op = .request },
    .{ .op = .enabled },
    .{ .op = .grounded },
    .{ .op = .extra },
    .{ .op = .select, .a = 2, .b = 3 },
    .{ .op = .all, .a = 0, .b = 1, .c = 4 },
};

var engine: Engine = .{};

fn asPublic(value: u32) i32 {
    return @bitCast(value);
}

fn grounded() bool {
    // A jump accepted while paused already consumes the ground jump even though
    // position will not be integrated until the next tick.
    return engine.state.y == 0 and engine.state.vy == 0;
}

fn record(kind: u32, detail: u32) void {
    const h = &engine.history;
    h.entries[h.next] = .{
        .tick = engine.state.tick,
        .kind = kind,
        .detail = detail,
        .revision = engine.rules.revision,
    };
    h.next = (h.next + 1) % history_capacity;
    h.count = @min(h.count + 1, history_capacity);
}

fn evaluate(source: u32) void {
    var decision: Decision = .{
        .tick = engine.state.tick,
        .revision = engine.rules.revision,
        .y = engine.state.y,
        .jumps = engine.state.jumps_used,
        .valid = true,
    };
    for (graph, 0..) |node, i| {
        decision.values[i] = switch (node.op) {
            .request => @intFromBool(source <= 2),
            .enabled => @intFromBool(engine.rules.enabled == 1),
            .grounded => @intFromBool(grounded()),
            .extra => @intFromBool(engine.state.jumps_used < 2),
            .select => if (engine.rules.mode == 0) decision.values[node.a] else decision.values[node.b],
            .all => decision.values[node.a] & decision.values[node.b] & decision.values[node.c],
        };
    }
    engine.decision = decision;
}

export fn reset() void {
    engine = .{};
}

export fn set_running(flag: u32) void {
    engine.state.running = @intFromBool(flag != 0);
}

export fn apply_rules(mode: u32, impulse: u32, enabled: u32) u32 {
    // Validate the complete candidate before mutating any part of the engine.
    if (mode > 1 or impulse < 6 or impulse > 16 or enabled > 1) return 1;
    const revision = engine.rules.revision +% 1;
    engine.rules = .{ .mode = mode, .impulse = impulse, .enabled = enabled, .revision = revision };
    record(3, mode);
    return 0;
}

export fn input(source: u32) u32 {
    evaluate(source);
    const state = &engine.state;
    state.total_inputs +%= 1;
    state.last_source = source;
    if (engine.decision.values[5] != 0) {
        state.last_reason = 1;
        state.vy = @as(i32, @intCast(engine.rules.impulse)) * 100;
        state.jumps_used +%= 1;
        state.accepted_jumps +%= 1;
        record(1, source);
        return 1;
    }
    state.last_reason = if (source > 2) 5 else if (engine.rules.enabled == 0) 2 else if (engine.rules.mode == 0) 3 else 4;
    record(2, state.last_reason);
    return 0;
}

fn nextRandom() u32 {
    engine.state.rng = engine.state.rng *% 1664525 +% 1013904223;
    return engine.state.rng;
}

fn tickOnce() void {
    const state = &engine.state;
    state.tick +%= 1;
    state.distance +%= 4;

    if (!grounded()) {
        state.vy -= 55;
        state.y += state.vy;
        if (state.y <= 0) {
            state.y = 0;
            state.vy = 0;
            state.jumps_used = 0;
            record(4, 0);
        }
    }

    state.obstacle_x -= 4;
    const horizontal_overlap = state.obstacle_x < 156 and state.obstacle_x + 32 > 120;
    if (horizontal_overlap and state.y < @as(i32, @intCast(state.obstacle_height)) * 100 and state.obstacle_touched == 0) {
        state.hits +%= 1;
        state.obstacle_touched = 1;
        record(5, state.hits);
    }
    if (state.obstacle_x + 32 <= 120 and state.obstacle_passed == 0) {
        state.passed +%= 1;
        state.obstacle_passed = 1;
        record(6, state.passed);
    }
    if (state.obstacle_x + 32 < 0) {
        state.obstacle_x = 500 + @as(i32, @intCast(nextRandom() % 301));
        state.obstacle_height = 32 + (nextRandom() % 4) * 8;
        state.obstacle_touched = 0;
        state.obstacle_passed = 0;
    }
}

export fn advance(ticks: u32) u32 {
    if (ticks > 600 or engine.state.running == 0) return 0;
    for (0..ticks) |_| tickOnce();
    return ticks;
}

export fn step() void {
    tickOnce();
}

export fn get(field: u32) i32 {
    const s = &engine.state;
    return switch (field) {
        0 => asPublic(s.tick),
        1 => s.y,
        2 => s.vy,
        3 => asPublic(s.distance),
        4 => s.obstacle_x,
        5 => asPublic(s.passed),
        6 => asPublic(s.hits),
        7 => asPublic(s.jumps_used),
        8 => asPublic(s.running),
        9 => asPublic(engine.rules.revision),
        10 => asPublic(s.last_reason),
        11 => asPublic(s.last_source),
        12 => asPublic(s.total_inputs),
        13 => asPublic(s.accepted_jumps),
        14 => asPublic(engine.rules.mode),
        15 => asPublic(engine.rules.impulse),
        16 => asPublic(engine.rules.enabled),
        17 => @intFromBool(grounded()),
        18 => asPublic(s.obstacle_height),
        else => -1,
    };
}

export fn node_value(id: u32) i32 {
    if (!engine.decision.valid or id >= engine.decision.values.len) return -1;
    return engine.decision.values[id];
}

export fn decision_meta(field: u32) i32 {
    if (!engine.decision.valid) return -1;
    return switch (field) {
        0 => asPublic(engine.decision.tick),
        1 => asPublic(engine.decision.revision),
        2 => engine.decision.y,
        3 => asPublic(engine.decision.jumps),
        else => -1,
    };
}

export fn history_count() u32 {
    return engine.history.count;
}

export fn history_get(newest_index: u32, field: u32) i32 {
    const h = &engine.history;
    if (newest_index >= h.count) return -1;
    const index = (h.next + history_capacity - 1 - newest_index) % history_capacity;
    const event = &h.entries[index];
    return switch (field) {
        0 => asPublic(event.tick),
        1 => asPublic(event.kind),
        2 => asPublic(event.detail),
        3 => asPublic(event.revision),
        else => -1,
    };
}

export fn bytes(kind: u32) u32 {
    return switch (kind) {
        0 => @sizeOf(Rules),
        1 => @sizeOf(State),
        2 => @sizeOf(Decision),
        3 => @sizeOf(History),
        4 => @sizeOf(Engine),
        5 => @sizeOf(@TypeOf(graph)),
        6 => @sizeOf(Engine) + @sizeOf(@TypeOf(graph)),
        else => 0,
    };
}

test "edits preserve complete simulation state and latest observation" {
    reset();
    set_running(1);
    try std.testing.expectEqual(@as(u32, 1), input(0));
    try std.testing.expectEqual(@as(u32, 8), advance(8));
    const before_state = engine.state;
    const before_decision = engine.decision;
    try std.testing.expectEqual(@as(u32, 0), apply_rules(1, 16, 0));
    try std.testing.expectEqualDeep(before_state, engine.state);
    try std.testing.expectEqualDeep(before_decision, engine.decision);
    try std.testing.expectEqual(@as(i32, 1), get(9));
    try std.testing.expectEqual(@as(i32, 3), history_get(0, 1));
    try std.testing.expectEqual(@as(i32, 1), history_get(0, 3));
}

test "invalid edits and oversized advance are atomic" {
    reset();
    set_running(1);
    _ = input(2);
    step();
    const before = engine;
    const invalid = [_][3]u32{
        .{ 2, 10, 1 }, .{ 0, 5, 1 },                     .{ 0, 17, 1 },
        .{ 0, 10, 2 }, .{ std.math.maxInt(u32), 10, 1 },
    };
    for (invalid) |candidate| {
        try std.testing.expectEqual(@as(u32, 1), apply_rules(candidate[0], candidate[1], candidate[2]));
        try std.testing.expectEqualDeep(before, engine);
    }
    try std.testing.expectEqual(@as(u32, 0), advance(601));
    try std.testing.expectEqual(@as(u32, 0), advance(std.math.maxInt(u32)));
    try std.testing.expectEqualDeep(before, engine);
}

test "pause, step, and ground-only jump handle input before integration" {
    reset();
    try std.testing.expectEqual(@as(i32, -1), node_value(0));
    try std.testing.expectEqual(@as(i32, -1), decision_meta(0));
    try std.testing.expectEqual(@as(u32, 0), advance(600));
    try std.testing.expectEqual(@as(u32, 1), input(0));
    try std.testing.expectEqual(@as(i32, 0), get(1));
    try std.testing.expectEqual(@as(i32, 1000), get(2));
    try std.testing.expectEqual(@as(i32, 0), get(17));
    try std.testing.expectEqual(@as(u32, 0), input(0));
    try std.testing.expectEqual(@as(i32, 3), get(10));
    step();
    try std.testing.expectEqual(@as(i32, 1), get(0));
    try std.testing.expectEqual(@as(i32, 945), get(1));
    try std.testing.expectEqual(@as(i32, 0), get(8));
    for (0..100) |_| step();
    try std.testing.expectEqual(@as(i32, 1), get(17));
    try std.testing.expectEqual(@as(i32, 0), get(7));
    try std.testing.expectEqual(@as(u32, 1), input(0));
}

test "double jump, disable, mode changes, and impulse apply at input boundaries" {
    reset();
    _ = input(0);
    for (0..5) |_| step();
    const old_velocity = get(2);
    try std.testing.expectEqual(@as(u32, 0), apply_rules(1, 16, 1));
    try std.testing.expectEqual(old_velocity, get(2));
    try std.testing.expectEqual(@as(u32, 1), input(1));
    try std.testing.expectEqual(@as(i32, 1600), get(2));
    try std.testing.expectEqual(@as(i32, 2), get(7));
    try std.testing.expectEqual(@as(u32, 0), input(2));
    try std.testing.expectEqual(@as(i32, 4), get(10));
    _ = apply_rules(0, 6, 1);
    try std.testing.expectEqual(@as(u32, 0), input(0));
    try std.testing.expectEqual(@as(i32, 3), get(10));
    for (0..150) |_| step();
    _ = apply_rules(1, 6, 0);
    try std.testing.expectEqual(@as(u32, 0), input(0));
    try std.testing.expectEqual(@as(i32, 2), get(10));
    _ = apply_rules(1, 6, 1);
    try std.testing.expectEqual(@as(u32, 1), input(0));
    try std.testing.expectEqual(@as(i32, 600), get(2));
    try std.testing.expectEqual(@as(u32, 1), input(0));
    try std.testing.expectEqual(@as(u32, 0), input(0));
}

test "all input sources share transition behavior and retain provenance" {
    var reference: State = undefined;
    for (0..3) |source| {
        reset();
        try std.testing.expectEqual(@as(u32, 1), input(@intCast(source)));
        for (0..7) |_| step();
        try std.testing.expectEqual(@as(i32, @intCast(source)), get(11));
        try std.testing.expectEqual(@as(i32, @intCast(source)), history_get(0, 2));
        var normalized = engine.state;
        normalized.last_source = 0;
        if (source == 0) reference = normalized else try std.testing.expectEqualDeep(reference, normalized);
    }
    reset();
    try std.testing.expectEqual(@as(u32, 0), input(std.math.maxInt(u32)));
    try std.testing.expectEqual(@as(i32, 5), get(10));
    try std.testing.expectEqual(@as(i32, 0), get(2));
    try std.testing.expectEqual(@as(i32, 0), node_value(0));
    try std.testing.expectEqual(@as(i32, 5), history_get(0, 2));
}

test "fixed history overwrites oldest entry and handles invalid access" {
    reset();
    for (0..40) |i| {
        _ = apply_rules(@intCast(i % 2), 10, 1);
        step();
    }
    try std.testing.expectEqual(@as(u32, 16), history_count());
    for (0..16) |i| {
        try std.testing.expectEqual(@as(i32, @intCast(40 - i)), history_get(@intCast(i), 3));
        try std.testing.expectEqual(@as(i32, @intCast(39 - i)), history_get(@intCast(i), 0));
    }
    try std.testing.expectEqual(@as(i32, -1), history_get(16, 0));
    try std.testing.expectEqual(@as(i32, -1), history_get(0, 4));
    try std.testing.expectEqual(@as(i32, -1), history_get(std.math.maxInt(u32), 0));
}

test "decision is pre-input state and remains at its original revision" {
    reset();
    _ = apply_rules(1, 10, 1);
    step();
    _ = input(2);
    try std.testing.expectEqual(@as(i32, 1), decision_meta(0));
    try std.testing.expectEqual(@as(i32, 1), decision_meta(1));
    try std.testing.expectEqual(@as(i32, 0), decision_meta(2));
    try std.testing.expectEqual(@as(i32, 0), decision_meta(3));
    try std.testing.expectEqual(@as(i32, 1), get(7));
    try std.testing.expectEqual(@as(i32, 1), node_value(2));
    try std.testing.expectEqual(@as(i32, 1), node_value(5));
    const snapshot = engine.decision;
    _ = apply_rules(0, 16, 0);
    step();
    try std.testing.expectEqualDeep(snapshot, engine.decision);
    try std.testing.expectEqual(@as(i32, -1), node_value(6));
    try std.testing.expectEqual(@as(i32, -1), decision_meta(4));
}

test "graph truth table agrees with a direct permission predicate" {
    for (0..2) |mode| {
        for (0..2) |enabled| {
            for (0..2) |airborne| {
                for (0..2) |pending_jump| {
                    for (0..3) |jumps| {
                        for (0..4) |source| {
                            reset();
                            engine.rules.mode = @intCast(mode);
                            engine.rules.enabled = @intCast(enabled);
                            engine.state.y = if (airborne == 1) 500 else 0;
                            engine.state.vy = if (pending_jump == 1) 1000 else 0;
                            engine.state.jumps_used = @intCast(jumps);
                            const on_ground = airborne == 0 and pending_jump == 0;
                            const permission = if (mode == 0) on_ground else jumps < 2;
                            const expected = source < 3 and enabled == 1 and permission;
                            const accepted = input(@intCast(source));
                            try std.testing.expectEqual(@as(u32, @intFromBool(expected)), accepted);
                            try std.testing.expectEqual(@as(i32, @intFromBool(on_ground)), node_value(2));
                            try std.testing.expectEqual(@as(i32, @intFromBool(jumps < 2)), node_value(3));
                            try std.testing.expectEqual(@as(i32, @intFromBool(permission)), node_value(4));
                            try std.testing.expectEqual(@as(i32, @intFromBool(expected)), node_value(5));
                        }
                    }
                }
            }
        }
    }
}

fn replay() Engine {
    reset();
    set_running(1);
    for (0..3000) |i| {
        if (i % 127 == 0) _ = apply_rules(@intCast((i / 127) % 2), @intCast(6 + i % 11), @intFromBool(i % 254 != 0));
        if (i % 29 == 0) _ = input(@intCast((i / 29) % 3));
        _ = advance(1);
    }
    return engine;
}

test "replay includes respawns and produces identical complete engine state" {
    const first = replay();
    const second = replay();
    try std.testing.expectEqualDeep(first, second);
    try std.testing.expect(first.state.passed > 5);
    try std.testing.expect(first.state.hits > 0);
    try std.testing.expectEqual(@as(u32, 3000), first.state.tick);
    try std.testing.expectEqual(@as(u32, 12000), first.state.distance);
}

test "collisions do not reset progress and each obstacle is counted once" {
    reset();
    set_running(1);
    _ = advance(180);
    try std.testing.expectEqual(@as(i32, 1), get(6));
    try std.testing.expectEqual(@as(i32, 1), get(5));
    try std.testing.expectEqual(@as(i32, 180), get(0));
    try std.testing.expectEqual(@as(i32, 720), get(3));
    _ = advance(10);
    try std.testing.expectEqual(@as(i32, 1), get(6));
    try std.testing.expectEqual(@as(i32, 1), get(5));
    try std.testing.expectEqual(@as(i32, 1), get(8));
}

test "counters wrap explicitly and memory accounting has no overlapping total" {
    reset();
    engine.state.tick = std.math.maxInt(u32);
    engine.state.distance = std.math.maxInt(u32) - 1;
    engine.state.total_inputs = std.math.maxInt(u32);
    engine.state.accepted_jumps = std.math.maxInt(u32);
    engine.rules.revision = std.math.maxInt(u32);
    _ = input(0);
    _ = apply_rules(1, 10, 1);
    step();
    try std.testing.expectEqual(@as(i32, 0), get(0));
    try std.testing.expectEqual(@as(i32, 2), get(3));
    try std.testing.expectEqual(@as(i32, 0), get(9));
    try std.testing.expectEqual(@as(i32, 0), get(12));
    try std.testing.expectEqual(@as(i32, 0), get(13));
    try std.testing.expectEqual(bytes(4) + bytes(5), bytes(6));
    try std.testing.expect(bytes(4) >= bytes(0) + bytes(1) + bytes(2) + bytes(3));
    try std.testing.expectEqual(@as(u32, 0), bytes(7));
    try std.testing.expectEqual(@as(i32, -1), get(19));
}
