//! Disposable binding results and change history. No event state lives here.
const std = @import("std");

const Memo = struct {
    value: f64 = 0,
    revision: u64 = 0,
    dependencies: u32 = 0,
    start: u32 = 0,
    length: u32 = 0,
};
const Change = struct { revision: u64 = 0, mask: u32 = 0 };

pub const Evaluation = struct {
    pub const max_cache = 32;
    pub const max_history = 8;
    memo: [max_cache]Memo = @splat(.{}),
    history: [max_history]Change = @splat(.{}),
    revision: u64 = 1,
    floor: u64 = 1,
    cache_count: u32 = 0,
    history_count: u32 = 8,
    next_change: u32 = 0,
    limit: u32 = 0,
    work: u32 = 0,
    instructions: u32 = 0,
    hits: u32 = 0,
    status: u32 = 0,

    pub fn configure(self: *Evaluation, cache: u32, history: u32) bool {
        if (cache > max_cache or history > max_history) return false;
        self.cache_count = cache;
        self.history_count = history;
        self.forget(3);
        return true;
    }

    pub fn forget(self: *Evaluation, flags: u32) void {
        if (flags & 1 != 0) self.memo = @splat(.{});
        if (flags & 2 != 0) {
            self.history = @splat(.{});
            self.floor = self.revision;
            self.next_change = 0;
        }
    }

    pub fn changed(self: *Evaluation, mask: u32) void {
        if (mask == 0 or self.cache_count == 0) return;
        if (self.revision == std.math.maxInt(u64)) {
            self.revision = 1;
            self.forget(3);
        }
        self.revision += 1;
        if (self.history_count == 0) {
            self.floor = self.revision;
            return;
        }
        const change = &self.history[self.next_change];
        self.floor = @max(self.floor, change.revision);
        change.* = .{ .revision = self.revision, .mask = mask };
        self.next_change = (self.next_change + 1) % self.history_count;
    }

    pub fn begin(self: *Evaluation, limit: u32) void {
        self.limit = limit;
        self.work = 0;
        self.instructions = 0;
        self.hits = 0;
        self.status = 0;
    }

    pub fn spend(self: *Evaluation) bool {
        if (self.work == self.limit) {
            self.status = 10;
            return false;
        }
        self.work += 1;
        return true;
    }

    fn slot(self: *const Evaluation, start: u32, length: u32) u32 {
        // One candidate, including on misses; no scan proportional to cache size.
        return (start ^ (start >> 5) ^ (length *% 0x9e3779b9)) % self.cache_count;
    }

    pub fn lookup(self: *Evaluation, start: u32, length: u32) ?f64 {
        if (self.cache_count == 0) return null;
        if (!self.spend()) return null;
        const memo = &self.memo[self.slot(start, length)];
        if (memo.revision == 0 or memo.start != start or memo.length != length) return null;
        // Successful pure constants need no input-change evidence. A program
        // replacement still clears them, just like every other binding memo.
        if (memo.dependencies != 0 and memo.revision < self.floor) return null;
        if (memo.dependencies != 0 and memo.revision != self.revision) {
            for (self.history[0..self.history_count]) |change| {
                if (!self.spend()) return null;
                if (change.revision > memo.revision and change.mask & memo.dependencies != 0) return null;
            }
        }
        memo.revision = self.revision;
        self.hits += 1;
        return memo.value;
    }

    pub fn remember(self: *Evaluation, start: u32, length: u32, value: f64, dependencies: u32) void {
        if (self.cache_count == 0) return;
        self.memo[self.slot(start, length)] = .{ .start = start, .length = length,
            .value = value, .dependencies = dependencies, .revision = self.revision };
    }
};

test "record loss cannot authorize an old memo; unrelated changes can" {
    var e: Evaluation = .{};
    try std.testing.expect(e.configure(1, 1));
    e.begin(100);
    e.remember(0, 3, 12, 1);
    e.changed(2);
    try std.testing.expectEqual(@as(?f64, 12), e.lookup(0, 3));
    e.changed(2);
    e.changed(2); // The certificate now predates the surviving history.
    try std.testing.expectEqual(@as(?f64, null), e.lookup(0, 3));
    e.remember(0, 3, 12, 1);
    e.changed(1);
    try std.testing.expectEqual(@as(?f64, null), e.lookup(0, 3));
    e.remember(0, 3, 20, 1);
    e.changed(2);
    e.forget(2);
    try std.testing.expectEqual(@as(?f64, null), e.lookup(0, 3));
    e.remember(0, 3, 20, 1);
    e.revision = std.math.maxInt(u64);
    e.changed(2);
    try std.testing.expectEqual(@as(?f64, null), e.lookup(0, 3));
}

test "budget, collisions, invalid settings, and disabled history are conservative" {
    var e: Evaluation = .{};
    try std.testing.expect(e.configure(1, 0));
    e.begin(100);
    e.remember(0, 3, 12, 1);
    e.remember(1, 3, 15, 2);
    try std.testing.expectEqual(@as(?f64, null), e.lookup(0, 3));
    try std.testing.expectEqual(@as(?f64, 15), e.lookup(1, 3));
    e.changed(0x80000000);
    try std.testing.expectEqual(@as(?f64, null), e.lookup(1, 3));
    try std.testing.expect(!e.configure(33, 0));
    try std.testing.expectEqual(@as(u32, 1), e.cache_count);
    e.begin(0);
    try std.testing.expectEqual(@as(?f64, null), e.lookup(1, 3));
    try std.testing.expectEqual(@as(u32, 10), e.status);
    try std.testing.expectEqual(@as(u32, 0), e.work);
    e.begin(100);
    e.remember(2, 1, 99, 0);
    e.changed(1);
    e.forget(2);
    try std.testing.expectEqual(@as(?f64, 99), e.lookup(2, 1));
    try std.testing.expectEqual(@as(u32, 1), e.work);
}
