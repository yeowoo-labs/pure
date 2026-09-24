# Workbench experiment contract

An author starts with empty source. There is no built-in player, physics, jump,
score, or timer behavior. All behavior is declared by the author. This is a
bounded scalar interaction language experiment, not a general language claim.

## Source language

```text
state count = 0
input tap = Space
box tile(80, 150, 60, 60, "#356548")
circle dot(200, 180, 20, "#c78638")
text label(80, 100, count, "Count: ")
on tap.down {
  count = count + 1
}
on frame {
  if count > 20 {
    count = 0
  }
}
```

Declarations and statements end at a newline or semicolon; `#` comments outside
strings. ASCII identifiers, finite numeric literals, state initializers must be
numeric constants (optional unary sign). Input key codes accepted:
Space, Enter, ArrowLeft/Right/Up/Down, KeyA..KeyZ, Digit0..Digit9. Input names
must be unique, key codes unique, names cannot shadow `frame`, `dt`, `min`, `max`,
`clamp`, `event_x`, `event_y`. State and input names use separate namespaces, entity IDs unique.

Expressions: parentheses, state references, dt (seconds on frame, zero on input),
unary - and !, + - * /, < <= > >= == !=, && ||, min(a,b), max(a,b), clamp(x,lo,hi).
Logical operators evaluate both sides. Division by zero/non-finite results fail
the whole event without committing state changes. No loops, recursion or eval.
An if without else executes its condition once; assignment statements are ordered.
Multiple handlers for the same event execute in source order within one atomic event.
`dt` is allowed in handlers, not view bindings. A frame always uses 1/60 seconds.

box: x,y,width,height expressions + literal hex color; circle: x,y,radius + color;
text: x,y,value expression + optional literal prefix. Values are world units in a
640x360 view. No implicit clipping/collision/physics. Render expressions are pure.
Unknown identifiers and invalid syntax include 1-based line+column diagnostics.

Initial scene is empty. Optional examples are explicit insertions, never auto-run.
Successful apply preserves scalar states by exact name; new names use initializers,
removed names disappear. Apply/reset are separate. Failed parse/load changes nothing.
Apply is refused while any input is held; release first. This avoids inventing
release events across changed mappings. Focus loss sends .up to ordinary buttons
and .cancel to pointer inputs.

## Compiler interface (web/compiler.mjs, browser and Node)

`compile(source)` returns:
```
{
 states:[{name,initial,line}], inputs:[{name,key,line} | {name,target,line} | {name,type,routes,line}],
 instructions:[{op,arg,value,line}], handlers:[{event,start,length,line}],
 views:[{id,type,line,color?,prefix?,bindings:{x:{start,length},...}}],
 pointerTargets:[{input,base,count}], metrics:{...}
}
```
Bindings: box x/y/width/height; circle x/y/radius; text x/y/value.
Compiler throws an Error with .line and .column. Empty source is a valid empty program.
Handlers and binding expressions all occupy instructions (maximum 512 total), but
only handlers execute on events; bindings are evaluated on demand by VM eval_at.
Limits: 32 states, 8 inputs, 24 views, 64 handlers, 512 instructions, stack 64,
source 32768 UTF-8 bytes. State IDs follow declaration order. Input event IDs:
frame=0; input i down=1+2*i, up=2+2*i, move=17+i, cancel=25+i.
Move/cancel handlers require a pointer input in source. States may be referenced before declaration;
symbols must be resolved after gathering top-level declarations. `metrics` can
record source bytes/lines and instruction/handler/view/state counts.

## VM opcodes

| op | operation |
|---:|---|
|0| push constant `value` |
|1| push state `arg` |
|2| push dt |
|3..6| add, subtract, multiply, divide |
|7| negate |
|8..13| less, less-equal, greater, greater-equal, equal, not-equal |
|14..16| AND, OR, NOT (nonzero true) |
|17..19| min, max, clamp (push x,lo,hi; invalid lo>hi fails) |
|20| store state `arg` (pop) |
|21| jump-if-zero to absolute instruction index `arg` (pop), forward only |
|22,23| push current pointer event x,y; fail without point context |
|24,25| load/store field at handler parameter_base + arg; handler-bound only |

Rules start with an empty stack and must end with empty stack. Bindings must finish
with exactly one value and cannot STORE, branch, or read dt/event coordinates. Branch targets may be handler end.

## Zig exported ABI

`reset_all()` clears programs and state. `begin_program(state_count,input_count)`
starts staging, returns0 success; `put_state(id,initial,old_id)` old_id=-1 means init;
`put_instruction(op,arg,value)` append; `put_handler(event,start,length)` append;
`commit_program()` all0 success otherwise error code, atomic; candidate is separate
from the active program, but shares its workspace with optional evaluation records.
Preserved IDs read active values only at successful commit. Validate limits,
indices, finite values and forward branch ranges; runtime stack errors are checked.
`reset_state()` restores active initializers, resets tick/event diagnostics.
`dispatch(event,dt)` accepts0..16 and finite dt; event0 dt must equal1/60, input dt0;
returns0 success, other code failure with no state mutation. Tick increments for
successful frame; one event runs all matching handlers in source order.
`dispatch_point(event,x,y)` accepts existing input down/up IDs plus move/cancel IDs
within input_count, never frame0. Coordinates must be finite; dt is implicitly0.
The core only knows input count, not host device kinds. The compiler and host enforce
pointer ownership and device mapping. Calling legacy dispatch on a coordinate-reading
handler fails with bad_event9. Point operands are ephemeral execution context, not
extra persistent state slots. Both dispatch paths use the same atomic commit/history.
The host requires this new export; older Wasm must be rebuilt.
`eval_at(start,length)` returns f64 or NaN on invalid/pure-expression failure;
must not mutate state or event diagnostics. `get_state(id)` returns f64 orNaN.
`info(id)` u32: 0 tick,1 last_error,2 error_pc (0xffffffff none),3 last_executed_ops,
4 revision,5 state_count,6 instruction_count,7 handler_count,8 input_count,
9 successful_event_count. `layout(id)` u32: 0 Engine reserved @sizeOf,
1 Program reserved @sizeOf,2 Instruction @sizeOf,3 Handler @sizeOf,
4 active used IR bytes (instructions+handlers+pointer targets),5 active state-value bytes,
6 staging reserved bytes (subset of Engine),7 evaluation stack capacity bytes,8 pointer target entry bytes,9 active pointer target count.
`error_message` belongs to host map; core error codes documented in core.zig.
No allocation or imported runtime in Wasm. Fixed64KiB linear memory target with
16KiB stack; increase only if actual linker evidence requires and document it.

## Host interface (web/runtime.mjs)

`await createRuntime(wasmBytes?, evaluationOptions?)` fetches core.wasm in browser if omitted.
Returns object with apply(source), reset(), frame(count=1), down(name,source),
up(name,source), point(name,phase,x,y,source,target?), releaseAll(), snapshot(), exportProject(), currentProgram.
Multiple sources per input share one logical held state; down emits only first
source, up only last. Source strings identify keyboard vs onscreen pointer.
`apply` returns compiled program or throws with diagnostic. It must fully validate
in host and stage+commit in core. It preserves by name. `reset` keeps active program.
`snapshot` returns {states:{name:value}, views:[{id,type,x,y,width?,height?,radius?,
value?,prefix?,color?}], tick,revision,lastError,errorLine,lastOps,memory:{...},held:[]}
`currentProgram` is the latest successfully applied compiled program or null.
The program and all nested metadata are frozen so readers cannot corrupt state
name mappings or binding ranges. `lastFailure` records the last failed event;
its `lastError`/line remain visible through successful release or focus cleanup
until successful apply or explicit reset. `lastEvent` still describes the latest
event. A view evaluation failure uses host error100 and does not roll back events.
UI draft and applied code are distinct. Browser/CLI share compiler/runtime/wasm;
instances are separate, not a live collaborative session. No arbitrary JS evaluation.

## Bounded display evaluation (2026-09-22)

The existing event and float semantics above are unchanged. The forgetful C
prototype's contract is adapted into `evaluation.zig`; its uint64 DAG is not
used as a substitute for this VM's float instructions.

Each snapshot starts one display budget, shared by all bindings. Default options
are `{cacheEntries:0, historyEntries:8, workLimit:2048}`. A unit is a binding
request, a candidate-cache probe, a change-record probe, or an executed VM
instruction. State extraction, host object creation, event dispatch and DOM work
are outside this budget. It is not a time limit or a bound on whole-app work.
For the current compiler limits, 96 bindings * 10 units plus 512 instructions
is at most 1,472 units; the default admits even an uncached valid scene with
cache lookup/validation enabled. Malformed bindings may fail sooner.

`configureEvaluation({cacheEntries?,historyEntries?,workLimit?})` accepts integers
0..32, 0..8, and 0..4294967295 respectively. Missing fields retain prior settings.
Valid configuration clears reuse records; invalid configuration changes nothing.
`forgetEvaluation('values'|'history'|'all')` drops optional records without
changing authored state, event diagnostics or program identity.
`evaluationStats()` returns the options plus work, instructions, cacheHits for
the last snapshot, and reservedBytes for the complete fixed evaluation structure.
Statistics are separate from snapshots so repeated observations remain equivalent.

Memo entries contain the value, instruction range, input dependency bit mask
and validated revision. Direct mapping uses one candidate probe, with collisions
causing recomputation. Only successful computations are cached. Constant results
need no input-change history. All other reuse requires a complete retained
history since validation and no intersection with changed states. Dependencies
are collected from actual VM LOADs, not trusted compiler-supplied annotations.
History loss causes recomputation. All history and memo storage is fixed size;
setting capacities to zero does not return reserved linear memory.

Only bitwise-different committed states advance change history (including signed
zero). Failed events and net-zero updates do not. Successful apply and reset
discard all reuse records. Parse/host validation failure before staging preserves
records. Beginning core staging discards them, including if that load is later
rejected or aborted. Epoch wrap also clears records. No inputs or effectful event
handlers are replayed to rebuild a binding.

Budget exhaustion returns an empty views array for the entire snapshot, with
displayError `{code:101,line,message}`. Successfully committed event state remains.
There is no automatic retry or silent return of an old scene. Raising the budget
allows a fresh attempt. Arithmetic/size errors retain the earlier per-view error100
behavior. `displayError` is null on display success; a latched execution error
still takes precedence in lastError/errorMessage, while displayError independently
reports display failure. Partial successful binding memos may survive a failed
snapshot but no partial scene is published.

New core exports: `configure_evaluation(cache,history)` returns 0 or capacity1;
`forget_evaluation(flags)` bits 1=values, 2=history; `begin_evaluation(limit)`;
`eval_bounded(start,length)` returns f64 or NaN and consumes that shared budget.
`evaluation_info(id)` returns 0 status (10=work limit), 1 work, 2 instructions,
3 cache hits, 4 cache capacity, 5 history capacity, 6 reserved bytes. These
diagnostics do not replace event info. The unbounded pure `eval_at` remains for
reference checks and is not the host snapshot path.

## Shared load/reuse workspace (2026-09-23)

The pending definition/restore values and evaluation Tables occupy one untagged union. The active
Program, current state and evaluation control fields remain separate. All core
calls are single-threaded and synchronous; no callback can reenter a phase change.
No pointer to the reused tables is retained between calls.

`begin_program` switches the workspace to staging and drops optional records.
Until commit/abort, old events and bindings still run against active. Binding
evaluation bypasses reuse and recomputes within its budget; cache configuration,
forget and reset must not write the staging payload. Old-id state preservation
reads the current values at successful commit, including events/reset during load.

Successful commit publishes the new Program and then initializes empty Tables in
the shared workspace. `abort_program()` discards a pending definition and restores
empty Tables without changing active program/state/event diagnostics. Inactive
abort is a no-op. A failed commit leaves staging open for explicit abort or the
next begin. An invalid begin closes any previous pending load. Host apply aborts
after a core loading failure. Lost reuse may make a previously warm tiny budget
insufficient; neither a preserved cache nor completion at that budget is promised.

`evaluation_info` additionally reports 7 dedicated control bytes, 8 shared Tables
bytes, 9 reuse available (false during load), and 10 union safety-tag overhead.
The host exposes these as dedicatedBytes, sharedBytes, reuseAvailable and
workspaceTagBytes. reservedBytes is control + Tables capacity, not incremental
Engine storage. The Tables capacity overlaps stagingReservedBytes and must not
be added to it. Before document restoration was added, the Wasm control was 48 B, shared Tables
1,152 B, union overhead 8 B. That Engine was 19,128 B, only 56 B larger than the
19,072 B pre-evaluation core; linear memory remains 65,536 B. No OS pages are
returned and this is not a measurement of browser/process memory.

## Direct output publication (2026-09-23)

`present(target)` evaluates the same bindings, in the same order and shared
budget as `snapshot()`, then calls a synchronous trusted output adapter:

- `begin(program)`: the frozen current definition, or null at startup.
- `state(index, value)`: each current authored state, without a host state map.
- `view(index, valid)`: every declared view's publication status.
- `value(viewIndex, bindingName, value)`: each binding of a valid view only.
- `end(visibleCount)`: number of valid views.

It returns the snapshot's diagnostics, held inputs, event information and memory
report, without `states` or `views`. Evaluation finishes before the first adapter
callback. On budget error101 every view is invalid and no binding values are
published; committed state values are still published. Error100 invalidates only
the affected views, as before. No stale value is silently substituted.

Runtime mutations, `snapshot()` and nested `present()` are rejected during
callbacks. Read-only program/export/statistics access remains available. Adapters
must be synchronous and must not throw. If one does throw, the exception
propagates and the runtime unlocks in a finally block; arbitrary external adapter
effects cannot be rolled back. A later call republishes every value. This is not
a transaction across arbitrary output devices or an untrusted plugin boundary.

Evaluation uses a private reusable Float64Array(96) and Uint8Array(24), allocated
on first snapshot/presentation. Their payload totals 792 host bytes, outside the
64 KiB Wasm memory. Typed-array object overhead, binding-entry arrays built at
apply, and DOM destination references are additional host memory. The numeric
buffer is a publication barrier, not another authoritative state or public
mutable scene. `snapshot()` remains an independent object representation for
CLI/export/debugging, built only when explicitly requested.

The browser calls `present()` for normal refreshes. Its output adapter binds DOM
destinations once per successful definition change and compares values with the
existing DOM before writing. Input refreshes reuse scene, state and cost elements.
Invalid views remain allocated but are hidden using SVG `display="none"`; recovery
reveals them. A definition change can replace all scene/state destinations. This
does not skip unchanged binding evaluation, eliminate all host allocations, or
provide measured process-memory/paint-latency savings.

## Authored direct manipulation (2026-09-23)

`input drag = pointer(tile)` binds a logical input to a declared view, including a
forward declaration. One input per target; it shares the existing 8-input limit
with keyboard inputs. `on drag.down/move/up/cancel` handlers may read `event_x`
and `event_y` in the same 640x360 world coordinates as output. They are unavailable
in keyboard/frame handlers and display bindings; copy to authored state to retain
them. There is no built-in drag constraint, snap, grab offset, or gesture rollback.
The direct.pure example authors these using ordinary assignments and expressions.

Host `point` requires a pointer input, valid phase, finite coordinates and nonempty
source string. Each logical input has one owning source; different targets may
have independent active contacts. Same-owner repeated down is a no-op; another
owner's down throws. Move/up/cancel without that owner are no-ops. Down establishes
ownership before dispatch; up/cancel release it before dispatch, even if authored
arithmetic fails. An event failure rolls back that event's state writes, not the
whole gesture. `releaseAll` cancels owned pointers at their last delivered position,
then releases ordinary buttons. `reset` directly clears ownership and resets state.
Last point events include x,y. A failed cancel can leave authored state unchanged,
but cannot strand host ownership; the failure remains visible until reset/apply.

Browser hit testing selects the declared target on primary-button pointerdown.
The stable SVG root captures that pointer, including movement/release outside the
scene. Client coordinates are transformed through the inverse SVG screen matrix.
Pointerup exposes its own final coordinates to authored handlers. The direct.pure
example updates position only on move and ends dragging on up, preserving the
last displayed position even if a host repeats stale coordinates on release. Termination is observed in the window capture phase, including releases outside
the SVG. A primary mouseup can finish an owned mouse gesture if pointerup was
not delivered; it cannot finish a touch gesture. Duplicate termination is ignored.
Only capture loss of the SVG root ends its owner, not a bubbled descendant event.
Pointercancel, lostpointercapture and
focus loss use the last delivered position and dispatch cancel, never fake up.
Missing/non-invertible coordinate transforms cancel an active gesture. A second
contact on the same target is ignored; contacts on different targets are independent.
Touch scenes disable native panning on the SVG. Mouse/touch share this adapter;
pen-specific pressure, tilt and button meanings are not implemented or validated.

Every delivered pointermove executes synchronously; this layer adds no event queue,
coalescing, frame gating or newest-only drop policy. This is not a guarantee that
browsers expose every hardware sample. Keyboard commands and pointer coordinates
enter the same authored state/commit/output path. CLI `point NAME PHASE X Y` uses
that host API and core with source `terminal`; it does not replay browser hit testing.


## Placement documents (2026-09-23)

Code-only version1 JSON and .pure files keep their earlier behavior. Version2
stores a restorable authored document, not a suspended execution image:

```json
{"format":"pure-workbench","version":2,"source":"state x=80","draft":"state x=80",
 "states":[{"name":"x","value":350}]}
```

`source` is the applied definition; optional `draft` is separate unapplied editor
text (default source). Each is at most 32768 UTF-8 bytes. Draft syntax may be
unfinished. State records must contain exactly one finite value for every declared
state name, with no extra, missing or duplicate names. Record order is irrelevant.
JSON numbers preserve finite f64 values, except negative zero which is encoded as
the exact string `"-0"`. No other numeric string is accepted. Input latches, running
clock, diagnostics, caches, DOM, compiled IR and external resources are not saved.
The serialized input file limit is 1449984 bytes, including escaped source/draft
and up to 32 generated item.field names (which can repeat a long schema name).

`exportDocument(draft?)` returns an independent version2 object. It refuses an
unapplied runtime, held input or latched execution failure. It reads core state,
without constructing an output snapshot. `restoreDocument(document)` validates
and compiles before staging, refuses held input, then commits the definition and
all saved values together. It returns `{source,draft}`. Failed validation leaves
active program/values/diagnostics intact. A failed core load is aborted; optional
reuse records may be lost as with ordinary apply. Successful restore discards old
reuse, clears diagnostics, resets tick/event count and starts with no held inputs.
The original source initializers remain intact: explicit reset restores those,
not saved values. Existing `apply` continues to preserve current values by name.
Both new document APIs reject reentry during output publication.

Core exports `put_restored_value(id,value)` and `commit_restored_program()` extend
the existing begin/put/commit loader. Every declared state needs exactly one finite
restored value; duplicate/out-of-range/nonfinite input poisons staging. Restored
commit checks completeness and program validity before touching active state.
Ordinary commit rejects staged restored values. Pending values live alongside the
pending Program in the same union as optional evaluation Tables; active Program
layout does not grow. `layout(6)` now includes Pending, rather than Program only.
Before shared rule bindings, the Wasm Pending was 9656 B (264 B added for 32 values, mask and alignment),
Engine was 19392 B; linear memory remains fixed at 65536 B. These are build-layout
facts, not a browser RAM or performance improvement claim. `evaluation_info(10)`
continues to report only union tag overhead, excluding restoration payload.

Browser **배치 저장** stores applied source, all current values and editor draft.
It requires a stopped clock. **파일 열기** restores version2 after replacement
confirmation when existing work is present; it starts stopped. Version1/plain
source still only replaces the editor draft. Async reading checks draft generation,
program identity and successful event count so intervening edits/inputs cannot be
overwritten by a stale read. Invalid files do not alter the working document.
**.pure 저장 / 코드 JSON 저장** remain draft-only exports.

CLI `save-project PATH` writes version2 with exclusive creation; `restore PATH`
restores it. Loading a .json path (including at startup) distinguishes versions.
A browser draft is retained through CLI save-project, but never applied implicitly.
Existing `save PATH` remains applied-source-only. No filesystem overwrite is added.


## Shared rules and named records (2026-09-23)

```text
record Position { x=0; y=0 }
item green = Position(x=80,y=120)
item blue = Position(x=280,y=160)
rule move(t: Position) { t.x=event_x; t.y=event_y }
input drag_green = pointer(green_box)
input drag_blue = pointer(blue_box)
box green_box(green.x,green.y,56,44,"#c9f08b")
box blue_box(blue.x,blue.y,56,44,"#74c4d4")
on drag_green.move use move(green)
on drag_blue.move use move(blue)
```

A record defines 1..8 named finite numeric initializers. An item has its own values;
optional literal field overrides replace defaults. Its fields occupy contiguous
slots in the existing scalar state array, with public names `item.field`. They
count toward the same 32-state limit. Schemas and items may be forward-referenced.
Unknown/duplicate fields, types or items fail compilation. Scalar and item names
cannot collide; type and rule names have separate namespaces. There are no runtime
record objects, nested records, record copying, dynamic item creation or deletion.

A rule takes exactly one named record parameter with an exact schema name match.
Its body uses the existing expressions, ordered assignments and conditional blocks.
Parameter fields read/write the connected item's current slots; globals and other
qualified state names remain accessible. The parameter shadows the same qualified
prefix only inside its body. No rule calls from expressions or rule bodies, recursion,
return values, stack frames, or implicit event loop are introduced. Frame handlers
may use rules; coordinate-reading rules may only bind to pointer events.
Unused rules are name/type checked without emitting a body into the VM program.

`on input.edge use rule(item)` is one event handler. Each used rule's instructions
are emitted once at its first use. Every use adds handler metadata referencing that
same exact instruction range, with parameterBase/parameterCount. Bindings run in
source order, including repeated uses of one item. All matching bindings and
ordinary handlers participate in the same atomic event: a failure in any rolls
back every state write in that event. Error PCs refer to the shared source body.

Core `bind_handler(handler_index,base,count)` sets a previously unbound staged
handler's contiguous state range. Counts are 1..8; the entire range must fit current
staged state_count. A duplicate binding poisons staging. All owners of shared code
must have the same nonzero field count and exactly the same range. Partial overlap
remains invalid. Parameter opcodes must belong to a bound handler and use an index
below every owner's field count. Pure display evaluation rejects parameter ops;
view bindings use ordinary resolved state loads and their normal cache dependencies.

Handler metadata grows from 12 to 16 B (base/count plus alignment). There is no
per-field indirection list or per-event parameter-value copy. At the shared-rules step, Wasm Program
was 9648 B, Pending 9912 B, Engine 19904 B: 512 B more fixed reservation across active
and pending handler arrays. The linear-memory limit stays 64 KiB. The required
bind_handler export makes the host reject older incompatible Wasm.

Apply preserves each qualified field by name. Reordering schema fields or item
instances rebuilds physical bindings without changing their named values. Removing
an item removes its fields; adding it again initializes new fields. An active
input still blocks definition changes. Version2 documents save qualified field
names, not physical offsets; restoration recompiles all bindings and validates all
values before commit. This is static named-field identity, not a general lifetime
handle for dynamically deleted/reused objects. Example `id` and `selected` are
ordinary author-defined numbers; uniqueness/selection policy is not enforced by
an implicit object system.


## Target-routed pointer groups (2026-09-23)

```text
input drag = pointer { green_box: green; blue_box: blue }
on drag.move use move(drag.target)
```

A group resolves named views to named items of exactly one record type. Forward
references work. Every view may be connected only once across single-target inputs
and groups. Every item may occur in only one group route, even across groups;
this prevents two group captures from writing aliased field ranges. Explicit
ordinary handlers and statically bound rules may still write other states as authored.
Groups must be nonempty. There are at most 24 routes in the program, independently
of the existing eight logical inputs. State/view/instruction limits are unchanged.
`input.target` is a contextual rule argument for the current group's event only,
not a scalar expression or persistent object handle. Wrong groups/types are rejected.

A group handler is emitted once per authored `on`, independent of target count.
It can share its exact body with statically bound handlers. One route consists of
three bytes: logical input index, field base, field count. The compiler's routes
also retain source view/item names for the host. The host builds one bounded view
lookup Map per successful load; target-name lookup does not scan all group targets
on each event. JS Map/metadata allocations are outside the core memory accounting.

`put_pointer_target(input,base,count)` appends a staged route, with count 1..8 and
an in-bounds, nonoverlapping state range. Same-input routes must have equal counts.
`bind_target_handler(handler,count)` binds a previously unbound staged handler to
the event target, encoded as parameter_base=255. Commit requires a non-frame event
with matching routes/counts. Existing `bind_handler` accepts only concrete bases.
`dispatch_target(event,route,x,y)` validates the active route's membership in the
event's logical input, then supplies its base to the matching handlers. Missing,
foreign, and out-of-range targets fail before any authored writes. Ordinary dispatch
and dispatch_point reject inputs that require a route. All matching static, routed,
and inline handlers still run in source order within one atomic event.
The VM indexes the route directly; it still scans the bounded handler list to match
event IDs. No claim of constant-time complete dispatch is made.

Host `point(name,phase,x,y,source,target)` requires a group view name in the final
argument. Legacy single-target calls keep the old form and reject an extra target.
Capture/held ownership is `(input,view)`, so two views in a group can be manipulated
concurrently; a second source cannot steal the same view. Continuations from a
non-owner are ignored. Browser capture records retain the initial view across
movement, release outside the view, and cancellation. `releaseAll` cancels every
owned view once. `held` uses `input:view` for groups and the old input names for
legacy inputs; lastEvent/lastFailure add `target` for group events.

Route indices live only in the active definition. Apply/restore remain blocked
while inputs are held. Reset clears owners; load rebuilds routes by source names.
Documents still store source and named scalar fields, never physical route indices.
Deletion/recreation at runtime and generation-checked dynamic identity are not added.
CLI uses `point-target NAME VIEW PHASE X Y`; existing `point` is unchanged.

Each Program adds 80 B (24 three-byte routes, mask/count and alignment). Current
Program=9728 B, Pending=9992 B, Engine=20064 B, an additional 160 B of fixed engine
reservation over the shared-rules step. Handler remains 16 B, Instruction 16 B.
The active IR byte count now includes used routes. Linear memory remains 65536 B.
The host requires put_pointer_target, bind_target_handler, and dispatch_target.
