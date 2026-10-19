# Box layout (a subset of CSS Flexbox 1): 100 trees

`trees.bin` holds the first 100 trees `research/experiments/044-specs/layout_check.py` draws, each a u32 count of its
nodes, their rows in preorder (14 binary64s: parent, column, w, h, grow, shrink, basis, left, right, top, bottom, gap,
justify, align), then their boxes (x, y, w, h, each relative to its parent's), as `layout.py` works them out; Taffy
0.15's are the same within 0.01. `tests/check.py`'s `module_specs` gives each tree to aup's `examples/layout.aup` as a
message, on Pure's three paths: its boxes must be these, bit for bit.
