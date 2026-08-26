# MatrixOne Substrait/Flight control plane

The `mo_sidecar` DuckDB extension is the production MatrixOne-to-Sirius
control and result plane. It is linked only by `extension_config_gpu.cmake` and
auto-starts when `MO_SIDECAR_FLIGHT_PORT` is configured.

## Deployment scope

The supported deployment is one local sidecar owned by one MatrixOne CN. The
sidecar's Flight listener and read-resolver connection are independent mTLS
directions, and the resolver client certificate is the identity that the CN
authorizes for each opaque read lease. Do not share a sidecar or its resolver
identity across CNs.

For the bundled image, set `MO_SIRIUS_FLIGHT=1` and mount the certificate
directory at `/etc/sirius-certs:ro`. The entrypoint selects the paired
one-CN launcher, binds Flight only on the container's loopback interface, sets
the exact `MO_SIDECAR_FLIGHT_*` and `MO_SIDECAR_READ_*` values, and refuses to
start if a credential is missing. See the repository [container
instructions](../README.md#container-image-podman--docker) for certificate
roles and the TPC-H command sequence. This benchmark profile deliberately
uses non-durable local leases with TN GC disabled; it is not a multi-CN or
restart-recovery deployment.

For a same-container development benchmark only,
`MO_SIRIUS_FLIGHT_DEV_TLS=1` generates fresh, short-lived credentials before
either service starts. This avoids shipping a shared private identity while
letting a public image run without a certificate mount. It must not be used for
a separate sidecar, remote CN, production workload, or restart-recovery test.

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
- Each `StreamRead` uses a ticket-bound `DoPut` stream of framed MatrixOne
  `batch.Batch` payloads. Sirius maps `mo_stream_scan` to the GPU-native TAE
  vector decoder. Input is acknowledged only after every referenced byte has
  been copied into sidecar-owned staging; staging coalesces up to 32 MiB before
  H2D and is hard-bounded at 96 MiB. Constant vectors retain a separate 64 MiB
  per-input expansion bound.
- `DoGet(ticket)` claims the ticket once and streams the same `MOB1` framing in
  the reverse direction. Results are encoded directly from Sirius data-batch
  representations, without a DuckDB `DataChunk` or Arrow record-batch hop.
  Flight's mandatory leading message is an empty transport-only Arrow schema;
  every later `FlightData.data_header` is the native `MOB1` frame itself because
  Flight uses header presence as its end-of-stream sentinel. No result value is
  encoded in Arrow; the exact result schema remains the MO-native value in
  `FlightInfo.schema`.
  Null-free fixed-width GPU results at or above 1 MiB are packed on GPU; small,
  variable-width, null-bearing, or conversion-heavy results use the direct host
  representation. `MO_SIDECAR_GPU_RESULT_PACK_MIN_BYTES` changes the threshold
  and is clamped to 64 KiB–64 MiB.
- `DoAction(CancelExecution)` accepts a strict `CancelExecutionRequest` with
  either the 32-byte ticket or the 32-byte preparation idempotency key. The
  latter closes the lost-`FlightInfo` case where MatrixOne cannot know whether
  a ticket was created. A `quiesced` result is returned only after the worker
  has stopped and released its query-local TAE resolutions. Client stream
  destruction, explicit cancellation, deadline expiry, server shutdown, and
  execution failure converge on the same first-terminal-state cleanup.

The authoritative protobuf source is
[`proto/matrixone/sidecar/v1/sidecar.proto`](proto/matrixone/sidecar/v1/sidecar.proto).
Substrait is pinned to `0.78.0`, the sidecar protocol to v5, `TaeRead` to
v2/features=0, and `StreamRead` to v1/features=0. Older protocol versions fail
capability negotiation rather than being interpreted as native batches.

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
by one negotiated MO-native frame plus its Sirius data batch. Oversized results
are split by rows before encoding; a single row larger than `max_batch_bytes`
fails the execution rather than entering the Flight result buffer.

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
