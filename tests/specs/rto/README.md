# RFC 6298's retransmission timer: two scenarios

`scenarios.json` holds one scenario (seed 2, 20 segments, five timeouts) of `research/experiments/044-specs/rto_check.py`
at tick periods of 10 and 100 ms: the events an app is given (time in ms, kind, segment) and, after each, the state the
model of the same rules (`rto.py`) has (SRTT, RTTVAR, RTO, deadline, segments outstanding, retransmissions).
`tests/check.py`'s `module_specs` gives the events to aup's `examples/rto.aup` (its tick and G made the period's) on
Pure's three paths.
