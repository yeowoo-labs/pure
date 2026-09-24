//! Native C ABI. The caller supplies aligned instance storage; no global VM,
//! allocator, OS call or browser dependency is part of this selected module.
const Engine = @import("engine.zig").Engine;

// Keep ReleaseSafe bounds/overflow checks. Unexpected internal invariant
// failures trap without importing a process-wide stack unwinder or I/O layer.
// Expected authored errors still return the ordinary scalar status codes.
pub const panic = @import("std").debug.no_panic;

export fn pure_scalar_bytes() usize {
    return @sizeOf(Engine);
}
export fn pure_scalar_alignment() usize {
    return @alignOf(Engine);
}

export fn pure_scalar_initialize(memory: ?*anyopaque, size: usize) ?*Engine {
    const pointer = memory orelse return null;
    if (size < @sizeOf(Engine) or @intFromPtr(pointer) % @alignOf(Engine) != 0) return null;
    const instance: *Engine = @ptrCast(@alignCast(pointer));
    instance.* = .{};
    return instance;
}

export fn pure_scalar_reset_all(instance: *Engine) void {
    instance.reset_all();
}

export fn pure_scalar_begin_program(instance: *Engine, state_count: u32, input_count: u32) u32 {
    return instance.begin_program(state_count, input_count);
}

export fn pure_scalar_abort_program(instance: *Engine) void {
    instance.abort_program();
}

export fn pure_scalar_put_state(instance: *Engine, id: u32, initial: f64, old_id: i32) u32 {
    return instance.put_state(id, initial, old_id);
}

export fn pure_scalar_put_instruction(instance: *Engine, op: u32, arg: u32, value: f64) u32 {
    return instance.put_instruction(op, arg, value);
}

export fn pure_scalar_put_handler(instance: *Engine, event: u32, start: u32, length: u32) u32 {
    return instance.put_handler(event, start, length);
}

export fn pure_scalar_bind_handler(instance: *Engine, handler: u32, base: u32, count: u32) u32 {
    return instance.bind_handler(handler, base, count);
}

export fn pure_scalar_put_pointer_target(instance: *Engine, input: u32, base: u32, count: u32) u32 {
    return instance.put_pointer_target(input, base, count);
}

export fn pure_scalar_bind_target_handler(instance: *Engine, handler: u32, count: u32) u32 {
    return instance.bind_target_handler(handler, count);
}

export fn pure_scalar_put_restored_value(instance: *Engine, id: u32, value: f64) u32 {
    return instance.put_restored_value(id, value);
}

export fn pure_scalar_commit_program(instance: *Engine) u32 {
    return instance.commit_program();
}

export fn pure_scalar_commit_restored_program(instance: *Engine) u32 {
    return instance.commit_restored_program();
}

export fn pure_scalar_reset_state(instance: *Engine) void {
    instance.reset_state();
}

export fn pure_scalar_dispatch(instance: *Engine, event: u32, dt: f64) u32 {
    return instance.dispatch(event, dt);
}

export fn pure_scalar_dispatch_point(instance: *Engine, event: u32, x: f64, y: f64) u32 {
    return instance.dispatch_point(event, x, y);
}

export fn pure_scalar_dispatch_target(instance: *Engine, event: u32, target: u32, x: f64, y: f64) u32 {
    return instance.dispatch_target(event, target, x, y);
}

export fn pure_scalar_dispatch_value(instance: *Engine, event: u32, value: f64) u32 {
    return instance.dispatch_value(event, value);
}

export fn pure_scalar_eval_at(instance: *Engine, start: u32, length: u32) f64 {
    return instance.eval_at(start, length);
}

export fn pure_scalar_configure_evaluation(instance: *Engine, cache: u32, history: u32) u32 {
    return instance.configure_evaluation(cache, history);
}

export fn pure_scalar_forget_evaluation(instance: *Engine, flags: u32) void {
    instance.forget_evaluation(flags);
}

export fn pure_scalar_begin_evaluation(instance: *Engine, limit: u32) void {
    instance.begin_evaluation(limit);
}

export fn pure_scalar_eval_bounded(instance: *Engine, start: u32, length: u32) f64 {
    return instance.eval_bounded(start, length);
}

export fn pure_scalar_evaluation_info(instance: *Engine, id: u32) u32 {
    return instance.evaluation_info(id);
}

export fn pure_scalar_get_state(instance: *Engine, id: u32) f64 {
    return instance.get_state(id);
}

export fn pure_scalar_info(instance: *Engine, id: u32) u32 {
    return instance.info(id);
}

export fn pure_scalar_layout(instance: *Engine, id: u32) u32 {
    return instance.layout(id);
}
