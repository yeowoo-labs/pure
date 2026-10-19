# Line breaking's properties and lines (UAX #14, Unicode 16.0)

`ranges.bin` and `lines.bin` are what `research/experiments/044-specs/linebreak_check.py` writes (its first argument)
from the Unicode Character Database 16.0.0 that Vega's text is frozen at (`vega/build/upstream/ucd-16.0.0`, read by
Vega's `text/unicode.py`): Copyright © 1991-2024 Unicode, Inc., under the Unicode License v3
(https://www.unicode.org/license.txt), which lets data derived from it be copied with this notice.

- `ranges.bin`: the properties' ranges, 2,787 rows of six little-endian binary64s (start, class after LB1 in
  `linebreak.aup`'s numbering, initial quotation, final quotation, East Asian, pictographic and unassigned), as the app's
  load message takes them.
- `lines.bin`: every line of `auxiliary/LineBreakTest.txt` (16,672), then 2,000 random lines (code points drawn from each
  class, their boundaries as `linebreak.py` works them out). Each is a u32 count, its u32 code points and count + 1
  bytes, 1 where a line may break.

`tests/check.py`'s `module_specs` loads the ranges into aup's `examples/linebreak.aup` and gives it every line, on Pure's
three paths.
