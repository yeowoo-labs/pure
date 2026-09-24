//! Fixed-definition native build root. The profile is generated from a
//! validated .pure definition; native.zig supplies the unchanged C ABI.
const profile = @import("profile");
pub const pure_definition = profile;
pub const panic = @import("std").debug.no_panic;
pub const pure_max_states = profile.states;
pub const pure_max_inputs = profile.inputs;
pub const pure_max_instructions = profile.instructions;
pub const pure_max_handlers = profile.handlers;
pub const pure_max_pointer_targets = profile.pointer_targets;
comptime { _ = @import("native.zig"); }
