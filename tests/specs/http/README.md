# HTTP/1.1 request streams and their reading

`streams.bin` is the first 80 of the 200 streams `research/experiments/044-specs/http_check.py` makes (its seed 140):
pipelined requests with methods, targets, HTTP/1.1 and a last HTTP/1.0, fields with mixed-case names and spaces around
their values, bodies by Content-Length or chunked with extensions and trailers, some ending in part of a request. Each
is a u32 big-endian length, then its bytes. `read.json` is what `http_requests.py` reads from each (its requests, its
bytes left, in hex), which the research check compares with h11 on every cut. `tests/check.py`'s `module_specs` feeds
each stream to aup's `examples/http.aup` whole, at random boundaries and (if it is short) a byte at a time, each packet
an event, and on Pure's three paths whole.
