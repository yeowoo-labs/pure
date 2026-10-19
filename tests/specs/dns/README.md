# DNS messages dnspython made

`messages.bin` is the first 200 of the 400 messages `research/experiments/044-specs/dns_check.py` makes with dnspython
2.7.0 (its seed 140, records unshuffled): queries and responses with A, AAAA, NS, CNAME, PTR, MX, SOA, TXT and SRV records, EDNS, names
sharing suffixes (so compression points back), labels of 63 bytes and of bytes above 0x7f, and names of 245 bytes. Each
is a u16 big-endian length, then its bytes, as over TCP. `tests/check.py`'s `module_specs` reads them with aup's
`examples/dns.aup` on Pure's three paths, then reads what its view `wire` writes back; the research check compares the
records with dnspython's reading.
