# JSONTestSuite's parsing cases

From [nst/JSONTestSuite](https://github.com/nst/JSONTestSuite) at 1ef36fa, `test_parsing/`: `y_` files are JSON texts,
`n_` files are not, and on `i_` files a parser may go either way (MIT license, `LICENSE`). `tests/check.py`'s
`module_specs` runs aup's `examples/json.aup` on each of them, on Pure's three paths.

Two `n_` files are left out for their size and made by the check: `n_structure_100000_opening_arrays.json` is `[`
100,000 times (SHA-256 13f86ea1e7edd116...), and `n_structure_open_array_object.json` is `[{"":` 50,000 times and a
newline (48b232fcd18ce2f7...).
