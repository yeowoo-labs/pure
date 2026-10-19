#!/usr/bin/env python3
"""Research 047: "the leaf is the commit" (feedback 009) before any store code: what a store whose commit writes the
list leaves it changed, in the bytes memory keeps them in, would write for a workload, against what today's store
writes (baseline.py: the log and the images). A byte-accounting model, not a store.

The layout (the memory's, core/module-list.c, as the disk's: one byte shape):
- a leaf: a 40-byte header (Node) and, per field, 32 cells (8 bytes; a text a 16-byte span, its bytes elsewhere):
  296 B for one number, 1,320 B for the bank's account (three numbers and a text), 808 a teller, 552 a branch,
  1,320 a transfer;
- an inner node: 680 B (the header, 32 children, the guide: 32 counts before and 32 keys);
- a commit: a 128-byte record (as the tree's today) and 16 bytes a list it changed (its root and count).
Two ways to write a commit:
- cow: every leaf the commit changed is written anew, and every inner node above them (the root too); the old copies
  are garbage until a compaction.
- inplace: tail, and a change elsewhere written in place too (its cell alone): not a way the store may take (a kept
  snapshot reads the cell, and a crash between the write and the commit loses the old value): counted to show it.
- tail: as cow, but an append at a list's end writes only its cells, in place, into its last leaf's free slots (and a
  new leaf its header, and its parent one child entry, 20 B, in place): what a reader of an older commit cannot see,
  since it reads no cell past its commit's count. Changes elsewhere are cow.
Compaction by the tree's own rule: when the file's end past its 8 KiB of slots passes 4 x live + 1 MiB, the live nodes
are written again (counted). Snapshots: one kept every 64 commits for 16 x 64 commits; every in-place write is checked
against the counts the kept ones read (a write a kept snapshot could see stops the model).

Workloads (each as baseline.py drives the store):
- entries: one number appended a message (046's entries.aup);
- bank: 10 branches, 100 tellers, 10,000 accounts loaded first (leaves full), then a transfer a message: its account's,
  teller's and branch's balance set (uniform aid, tid, bid; the same generator as baseline.py), a transfer appended.

The bars, set before running (today's list bytes, the log and images, by grouping 1 / 16 / 256 messages a commit):
- appends: the leaf commit's list bytes a message at most today's (entries: 65.5 / 15.7 / 12.4);
- sparse updates: at most twice today's (bank: 2 x 122 / 35.6 / 30.1 = 244 / 71.2 / 60.2);
- snapshot: no in-place write a kept snapshot can see (cow and tail; inplace is there to break it).
After the run (not in the bars above, so not held to them): delta, the leaf with room for 8 changes after its cells
(16 B each: its place, field and cell), appended in place, a full room writing the leaf anew with them folded in. One
object still: the log's records live in the leaf they change. Its readers' price (a read looks at the room) is not
counted here.
What it means, said before: if appends pass and updates do not, "one unit" fails as stated: the leaf is the unit for
appends and bulk, and a sparse change needs a delta form, which is what the log is; the honest design is then two
units (or the log kept for updates alone), not one.

  model.py WORKLOAD MESSAGES     one JSON line a way and grouping"""
import json
import os
import random
import sys

HEADER, INNER, FANOUT, RECORD, PER_LIST, SLOTS, SLACK = 40, 680, 32, 128, 16, 8192, 1 << 20
ENTRY = 20  # a child entry written into a parent in place: its pointer (8), count before (4) and key (8)
KEEP_EVERY, KEPT = 64, 16
DELTA = 16  # delta: a change in a leaf's room after its cells (u32 place, u8 field, pad, the 8-byte cell)
DELTAS = int(os.environ.get('MODEL_DELTAS', 8))  # the room's changes (MODEL_DELTAS: 8 unless set)


class List:
    """A list's tree, its leaves full as loaded: leaf i holds items 32 i .. 32 i + 31."""

    def __init__(self, name, cell, items):
        self.name, self.cell, self.items = name, cell, items
        self.leaf = HEADER + FANOUT * cell
        self.version = {}  # node (level, index) -> the commit that wrote it
        self.deltas = {}   # delta: a leaf's changes in its room since it was last written whole
        self.written = {}  # delta: how often a leaf was written whole (its version: a new one is another object)

    def leaves(self):
        return max(1, -(-self.items // FANOUT))

    def levels(self):
        """Inner levels above the leaves (0 when one leaf is the root)."""
        n, levels = self.leaves(), 0
        while n > 1:
            n, levels = -(-n // FANOUT), levels + 1
        return levels

    def path(self, leaf):
        """The inner nodes above a leaf: (level, index) from the leaves' parents up."""
        out, index = [], leaf
        for level in range(1, self.levels() + 1):
            index //= FANOUT
            out.append((level, index))
        return out

    def live(self):
        n, total, level = self.leaves(), self.leaves() * (self.leaf + DELTAS * DELTA * (self.way == 'delta')), 0
        while n > 1:
            n = -(-n // FANOUT)
            total += n * INNER
        return total


def workload(name, messages, seed=47):
    """Each message's changes: (list, 'set', position) or (list, 'append', None)."""
    if name == 'entries':
        lists = {'entries': List('entries', 8, 0)}
        return lists, [[('entries', 'append', None)] for _ in range(messages)]
    rng = random.Random(seed)
    accounts = 10000
    lists = {'accounts': List('accounts', 40, accounts), 'tellers': List('tellers', 24, 100),
             'branches': List('branches', 16, 10), 'history': List('history', 40, 0)}
    out = []
    for _ in range(messages):
        aid, tid, bid = rng.randrange(accounts), rng.randrange(100), rng.randrange(10)
        rng.randint(-5000, 5000)  # the delta, drawn as baseline.py draws it (the same stream)
        out.append([('accounts', 'set', aid), ('tellers', 'set', tid), ('branches', 'set', bid), ('history', 'append', None)])
    return lists, out


def run(name, messages, batch, way):
    lists, changes = workload(name, messages)
    for l in lists.values():
        l.way = way
        if way == 'delta':
            l.leaf += DELTAS * DELTA  # the room is part of the leaf
    written = {'leaves': 0, 'cells': 0, 'inner': 0, 'entries': 0, 'deltas': 0, 'records': 0, 'compaction': 0}
    end, live0 = SLOTS, sum(l.live() for l in lists.values())
    end += live0  # what the setup loaded (not counted: baseline.py takes it away too)
    kept = []  # snapshots kept: {list name: count it reads}
    violations, commits = 0, 0
    for start in range(0, messages, batch):
        group = changes[start:start + batch]
        touched = {n: set() for n in lists}  # leaves to write anew
        tail_cells = {n: 0 for n in lists}
        delta_writes = {n: 0 for n in lists}
        delta_leaves = {n: set() for n in lists}
        for message in group:
            for list_name, op, position in message:
                l = lists[list_name]
                if op == 'append':
                    at = l.items
                    l.items += 1
                    leaf = at // FANOUT
                    if way in ('tail', 'inplace', 'delta'):
                        for snapshot in kept:  # a kept snapshot never reads at or past its count
                            if at < snapshot.get(list_name, 0):
                                violations += 1
                        tail_cells[list_name] += 1
                        if at % FANOUT == 0:  # a new leaf: its header, and an entry in its parent (in place); a
                            written['leaves'] += HEADER  # parent full: a new one (its header) and an entry above it
                            index = leaf
                            while index:
                                written['entries'] += ENTRY
                                if index % FANOUT:
                                    break
                                index //= FANOUT
                                written['inner'] += HEADER
                    else:
                        touched[list_name].add(leaf)
                elif way == 'delta':  # the change appended into its leaf's room, in place; a full room: the leaf anew
                    leaf = position // FANOUT
                    n, version = l.deltas.get(leaf, 0), l.written.get(leaf, 0)
                    for snapshot in kept:  # a kept snapshot reads no more of its leaf's room than its commit said
                        seen = snapshot.get((list_name, leaf))
                        if seen and seen[0] == version and n < seen[1]:
                            violations += 1
                    if n < DELTAS:
                        l.deltas[leaf] = n + 1
                        delta_writes[list_name] += 1
                        delta_leaves[list_name].add(leaf)
                    else:
                        l.deltas[leaf] = 0
                        l.written[leaf] = version + 1
                        touched[list_name].add(leaf)
                elif way == 'inplace':  # the cell written where it is: what a kept snapshot (and a crash) would see
                    for snapshot in kept:
                        if position < snapshot.get(list_name, 0):
                            violations += 1
                    tail_cells[list_name] += 1
                else:
                    touched[list_name].add(position // FANOUT)
        size = 0
        for n, leaves in touched.items():
            l = lists[n]
            inner = set()
            for leaf in leaves:
                inner.update(l.path(leaf))
            size += len(leaves) * l.leaf + len(inner) * INNER
            written['leaves'] += len(leaves) * l.leaf
            written['inner'] += len(inner) * INNER
        for n, cells in tail_cells.items():
            written['cells'] += cells * lists[n].cell
            size += cells * lists[n].cell
        for n in lists:  # delta: each change's 16 bytes, and each leaf with new ones its 16 in the commit record
            written['deltas'] += delta_writes[n] * DELTA + PER_LIST * len(delta_leaves[n])
            size += delta_writes[n] * DELTA + PER_LIST * len(delta_leaves[n])
        changed = sum(1 for n in lists if touched[n] or tail_cells[n] or delta_writes[n])
        written['records'] += RECORD + PER_LIST * changed
        end += size + RECORD + PER_LIST * changed
        commits += 1
        if commits % KEEP_EVERY == 0:
            snapshot = {n: l.items for n, l in lists.items()}
            snapshot.update({(n, leaf): (l.written.get(leaf, 0), c) for n, l in lists.items() for leaf, c in l.deltas.items()})
            kept.append(snapshot)
            kept = kept[-KEPT:]
        live = sum(l.live() for l in lists.values())
        if end - SLOTS > 4 * live + SLACK:
            written['compaction'] += live
            end = SLOTS + live
    total = sum(written.values())
    lists_bytes = written['leaves'] + written['cells'] + written['inner'] + written['entries'] + written['deltas']
    return {'workload': name, 'messages': messages, 'batch': batch, 'way': way, 'commits': commits,
            'list_bytes_per_message': round((lists_bytes + written['records'] - RECORD * commits) / messages, 1),
            'by_part_per_message': {k: round(v / messages, 1) for k, v in written.items()},
            'all_bytes_per_message': round(total / messages, 1), 'snapshot_violations': violations}


def main():
    name, messages = sys.argv[1], int(sys.argv[2])
    for batch in (1, 16, 256):
        for way in ('cow', 'tail', 'inplace', 'delta'):
            print(json.dumps(run(name, messages, batch, way)), flush=True)


if __name__ == '__main__':
    main()
