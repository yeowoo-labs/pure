# Authenticated document peer: direct versus Pure

This experiment uses the real `editor.pure` document app over the same TLS
1.3/TCP transport and `pure-doc/1` wire format. `direct-peer.c` is a
research-only server that parses the same authored program and calls the same
store and evaluator directly. `pure-document-server` is the selected product
server build without its native client commands. The baseline is limited to
the tested synchronous document workflow: it has no Pure work tickets,
admission/lifetime contract, cross-domain forwarding, or cancellation.

Both servers passed the existing `check_document_peer.py` suite: mutual TLS
identity, ALPN, source identity, external Python and native C clients, version
conflicts, one-byte replacement of a 1 MiB value, fragmented/invalid/truncated
frames, deadlines, and restart. The direct and Pure server builds passed the
same suite under ASan/UBSan. These checks do not establish full feature or
security equivalence under concurrent clients or failure networks.

To run the selected product checks on Linux AArch64:

```sh
make -C prototypes/connections check-document-peer
make -C prototypes/connections check-document-server
```

The first command checks the existing combined client/server executable; the
second checks the server-only code ceiling and runs the same peer scenarios
with the native C client from the combined executable. Both need permission
to bind a local TCP port. The server-only executable and budget are optional;
the default core and CLI do not link TLS.

To reproduce the read comparison:

```sh
python3 research/experiments/005-direct-document-peer/run.py \
  --body-bytes 8192 --reads 1024 --pairs 6 \
  --output research/experiments/005-direct-document-peer/results/local-8k.json
python3 research/experiments/005-direct-document-peer/run.py \
  --body-bytes 1048576 --reads 64 --pairs 4 \
  --output research/experiments/005-direct-document-peer/results/local-1m.json
```

The runner compiles both servers from source. Each trial creates and reads a
real stored document, then times repeated full reads on one authenticated,
persistent Python TLS connection. It verifies every response and the final
stored file hash. TLS handshake, certificate creation, file creation, and the
first cache-warming read are outside the timed batch. The server process RSS
is recorded before and after. Fresh stores and alternating order reduce some
order effects but do not control CPU frequency, OpenSSL or Python overhead,
network behavior beyond local loopback, or host scheduling. The JSON files
contain all individual samples, source hashes, compiler and OpenSSL versions.

The short `before-nodelay` and `after-nodelay` artifacts repeat exactly 32
authenticated 8 KiB reads through each server, differing only in the common
TLS transport's TCP_NODELAY option. This intervention supports a Nagle/delayed
ACK explanation for the prior ~44 ms per-request stall; no packet trace was
captured, so that exact kernel interaction remains an inference. The longer
post-change runs are for direct-versus-Pure cost, not a claim of Internet or
Kitex/QUIC performance.

See [the result report](../../../docs/research/055-authenticated-peer-transport-cost.md).
