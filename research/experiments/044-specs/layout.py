# Box layout, a subset of CSS Flexbox 1, in plain Python: the reference 140 counts aup's lines against. Containers lay
# their items out in one line (no wrap) along a row or a column: padding, a gap between items, justify-content (start,
# end, center, space-between, space-around, space-evenly) and align-items (start, end, center, stretch). An item's main
# size is its flex basis grown or shrunk (9.7), its cross size its own or, auto, stretched to the line. Every size is
# known from above: nothing is sized by its content, and min sizes are 0 (an item's padding at least). Flex factors
# are whole numbers: 9.7's sum below one (4b) does not arise.
from functools import reduce
from operator import add

START, END, CENTER, BETWEEN, AROUND, EVENLY = range(6)  # justify-content; align-items: START, END, CENTER, STRETCH
STRETCH = 3


class Refused(Exception):
    pass


def total(xs):  # added from the left, as the app adds (sum compensates since Python 3.12)
    return reduce(add, xs, 0)


def layout(nodes):
    """nodes in preorder, each (parent, column, w, h, grow, shrink, basis, left, right, top, bottom, gap, justify,
    align), w and h -1 when auto; each node's box [x, y, w, h], relative to its parent's."""
    boxes = [[0.0, 0.0, 0.0, 0.0] for _ in nodes]
    boxes[0][2:] = nodes[0][2], nodes[0][3]
    for i, (_, column, _, _, _, _, _, left, right, top, bottom, gap, justify, align) in enumerate(nodes):
        kids = [j for j in range(i + 1, len(nodes)) if nodes[j][0] == i]
        if not kids:
            continue
        m, c = (3, 2) if column else (2, 3)  # the main and the cross sizes' places
        inner = boxes[i][m] - (top + bottom if column else left + right)
        line = max(boxes[i][c] - (left + right if column else top + bottom), 0)
        pads = [(nodes[k][9] + nodes[k][10], nodes[k][7] + nodes[k][8]) if column else
                (nodes[k][7] + nodes[k][8], nodes[k][9] + nodes[k][10]) for k in kids]
        bases = [max(nodes[k][6], p) for k, (p, _) in zip(kids, pads)]
        gaps = gap * (len(kids) - 1)
        free = inner - (total(bases) + gaps)
        n, growing = len(kids), free > 0
        sizes, frozen = bases[:], [free == 0 or nodes[k][4] == nodes[k][5] == 0 for k in kids]
        while not all(frozen):  # 9.7: each pass freezes one item at least
            free = inner - (total(sizes[x] if frozen[x] else bases[x] for x in range(n)) + gaps)
            loose = [x for x in range(n) if not frozen[x]]
            factors = [nodes[kids[x]][4] if growing else (bases[x] - pads[x][0]) * nodes[kids[x]][5] for x in loose]
            if free != 0 and total(factors) > 0:  # grown by the grow factors, or shrunk by the shrink factors scaled
                for x, f in zip(loose, factors):
                    sizes[x] = bases[x] + free * (f / total(factors))
            violations = [max(sizes[x], pads[x][0]) - sizes[x] for x in loose]  # min violations: below the padding
            for x, v in zip(loose, violations):
                sizes[x], frozen[x] = max(sizes[x], pads[x][0]), v > 0 or total(violations) == 0
        free = inner - (total(sizes) + gaps)  # 9.5: what is left, by justify-content
        if (len(kids) == 1 or free <= 0) and justify in (BETWEEN, AROUND, EVENLY):
            justify = START if free <= 0 or justify == BETWEEN else CENTER
        first = {START: 0, END: free, CENTER: free / 2, BETWEEN: 0, AROUND: free / n / 2, EVENLY: free / (n + 1)}[justify]
        between = {BETWEEN: free / max(n - 1, 1), AROUND: free / n, EVENLY: free / (n + 1)}.get(justify, 0) + gap
        at = top if column else left
        for x, (k, size, (_, p)) in enumerate(zip(kids, sizes, pads)):
            at = at + (first if x == 0 else between)
            own = nodes[k][c]
            if own < 0 and align != STRETCH:
                raise Refused('an auto cross size not stretched: sized by its content')
            across = max(line if own < 0 else own, p)
            offset = {START: 0, END: line - across, CENTER: (line - across) / 2}.get(align, 0)
            boxes[k][m - 2], boxes[k][c - 2] = at, (left if column else top) + offset
            boxes[k][m], boxes[k][c] = size, across
            at = at + size
    return boxes
