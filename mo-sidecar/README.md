# MatrixOne Substrait/Flight control plane

The `mo_sidecar` DuckDB extension is the production MatrixOne-to-Sirius
control and result plane. It is linked only by `extension_config_gpu.cmake` and
auto-starts when `MO_SIDECAR_FLIGHT_PORT` is configured.

## RPC contract

- `DoAction(GetCapabilities)` returns a canonical JSON document. Its SHA-256
  digest is the capability hash used by all later requests.
- `GetFlightInfo(FlightDescriptor::CMD)` is `BeginExecution`. The command is a
  strict `matrixone.sidecar.v1.ExecuteSubstraitRequest`; unknown, duplicate,
  missing, oversized, or wrongly-typed protobuf fields are rejected. Its
  account, 16-byte query ID, plan digest, and 32-byte idempotency key are
  cryptographically cross-checked. An identical in-flight retry waits for and
  reuses the original unclaimed ticket; key reuse with different command bytes
  is rejected.
- A successful `GetFlightInfo` returns the result schema and one opaque,
  random, single-use 32-byte ticket. Preparation has already completed, so an
  `UNSUPPORTED_PLAN` response is safe for MatrixOne to classify for native
  fallback before any offloaded result is exposed.
- `DoGet(ticket)` claims the ticket once and streams Arrow record batches.
  Sirius's synchronous chunk callback and the Flight reader share a one-batch
  queue; the producer cannot advance until the consumer releases that batch.
- `DoAction(CancelExecution)` accepts a strict `CancelExecutionRequest` with
  either the 32-byte ticket or the 32-byte preparation idempotency key. The
  latter closes the lost-`FlightInfo` case where MatrixOne cannot know whether
  a ticket was created. A `quiesced` result is returned only after the worker
  has stopped and released its query-local TAE resolutions. Client stream
  destruction, explicit cancellation, deadline expiry, server shutdown, and
  execution failure converge on the same first-terminal-state cleanup.

The authoritative protobuf source is
[`proto/matrixone/sidecar/v1/sidecar.proto`](proto/matrixone/sidecar/v1/sidecar.proto).
Substrait is pinned to `0.78.0`, the sidecar protocol to v2, and `TaeRead` to
v1/features=0.

## Authenticated TAE resolution

The retained Substrait plan contains only `matrixone.sirius.v1.TaeRead`; it
never contains a manifest URL, filesystem path, user token, or object-store
credential. For each read the resolver:

1. verifies that every signed read has the Flight execution's account and
   query identity, then verifies the local capability and requested-schema hashes;
2. sends the canonical `TaeRead` plus requested schema to MatrixOne over mTLS;
3. requires an exact `TaeRead` echo and verifies the returned manifest and
   canonical-schema SHA-256 digests;
4. writes the authenticated manifest to a private `0600` temporary file and
   binds a generated query-local temporary view over `tae_scan`; and
5. drops the view and unlinks the manifest when Sirius releases the move-only
   resolution token.

MatrixOne's read service remains authoritative for query/account/database/table,
snapshot, lease expiry, schema, and manifest/object-set binding. TLS failures,
authentication mismatches, malformed responses, and scanner bind failures are
fail-closed and are never classified as unsupported-plan fallback.

## Bounded lifecycle

Pending and running executions share the configured active-ticket bound.
Deadlines are capped by `MO_SIDECAR_TICKET_TTL_MS`; a reaper interrupts expired
DuckDB contexts even when a client never calls `DoGet`. Result memory is bounded
by one negotiated Arrow batch plus the Sirius/DuckDB producer chunk. A batch
larger than `max_batch_bytes` fails the execution rather than entering the
Flight result buffer.

State transitions are monotonic:

```text
prepared -> claimed/running -> succeeded | failed | cancelled | expired
    |                              ^
    +---------- cancel/expiry -----+
```

The first terminal transition wins. Registry removal does not destroy a
running stream: the Flight reader and worker retain the execution until the
worker has stopped and the resolution tokens have been released.

## Validation

```bash
pixi run -- bash -c '
  cmake -S duckdb -B build/debug-sidecar -G Ninja \
    -DBUILD_UNITTESTS=ON \
    -DCMAKE_CUDA_ARCHITECTURES=75 \
    -DEXTENSION_STATIC_BUILD=1 \
    -DCMAKE_CXX_COMPILER=${CONDA_PREFIX}/bin/clang++ \
    -DCMAKE_C_COMPILER=${CONDA_PREFIX}/bin/clang \
    -DCMAKE_CXX_COMPILER_LAUNCHER= \
    -DCMAKE_C_COMPILER_LAUNCHER= \
    -DDUCKDB_EXTENSION_CONFIGS=$(pwd)/extension_config_gpu.cmake'

pixi run -- cmake --build build/debug-sidecar --target mo_sidecar_contract_unittest
pixi run -- build/debug-sidecar/extension/mo_sidecar/test/mo_sidecar_contract_unittest
pixi run -- cmake --build build/debug-sidecar --target duckdb
```
