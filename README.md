# mo-sirius-sidecar

DuckDB-based query sidecar for MatrixOne, powered by the
[Sirius](https://github.com/matrixorigin/sirius) GPU execution engine.
[MatrixOne](https://github.com/matrixorigin/matrixone) rewrites and forwards
queries annotated with `/*+ SIDECAR */` (DuckDB on CPU) or
`/*+ SIDECAR GPU */` (SiriusDB on GPU) to this sidecar to take advantage of GPU
for analytic query processing. The legacy HTTP/SQL path remains available for
compatibility. The Sirius path is an explicit Substrait/Arrow Flight contract;
it preserves the MatrixOne logical plan and does not send SQL, manifests, or
storage credentials to the sidecar.

The production offload path accepts a strict binary Substrait plan over
mutually-authenticated Arrow Flight. It resolves opaque, expiring `TaeRead`
references against MatrixOne over a separate mTLS connection, executes the
validated logical plan directly through Sirius, and streams Arrow batches with
one-batch backpressure. The SQL/HTTP path remains available for benchmarks and
compatibility, but it is not the credential or result-streaming contract.

MatrixOne distinguishes itself from other SiriusDB integrations through its
fundamental architecture as a Hybrid Transactional/Analytical Processing (HTAP)
system. This dual-capability framework allows MatrixOne to consistently maintain
high-throughput transactional performance, executing tens of thousands of
transactions per second on modest CPU configurations. By integrating with
SiriusDB, the system facilitates the offloading of complex analytical workloads
to high-performance GPUs. This synergy enables the processing of
near-instantaneous, transaction-consistent data, effectively bridging the gap
between real-time operational state and deep computational analysis without the
latency typically associated with traditional data movement.

## Execution Paths

| Path | Hint | Engine | Scan Pipeline |
|------|------|--------|---------------|
| **CPU** | `/*+ SIDECAR */` | DuckDB vectorized | `tae_scan()` → pread → LZ4 (CPU) → DuckDB vectors |
| **GPU** | `/*+ SIDECAR GPU */` | Sirius + cuDF | `tae_scan_task` → coalesced pread → pinned host → cudaMemcpy → nvCOMP LZ4 (GPU) → CUDA decode → cudf tables |

Both paths apply object-level and block-level zone-map pruning to skip data that
cannot match filter predicates. The GPU path uses coalesced I/O to merge adjacent
reads into single `pread()` calls (e.g., 360 reads → 12 I/O calls for a 5-column
scan), and CRC stripping is performed in memory when reading local MO files.

The GPU path bypasses the DuckDB execution engine entirely — compressed TAE data
goes directly from disk to GPU memory, with decompression and column decoding
performed by CUDA kernels. Filter predicates are pushed down and evaluated on GPU
via `cudf::compute_column()`.
See [DESIGN.md §13](DESIGN.md#13-gpu-native-tae-scan-sirius) for full architecture.

## Extensions

| Extension | Source | Description |
|-----------|--------|-------------|
| **tae-scanner** | [duckdb-tae-scanner](https://github.com/matrixorigin/duckdb-tae-scanner) | Reads MatrixOne TAE storage objects as DuckDB table functions |
| **httpserver** | [duckdb-httpserver](https://github.com/matrixorigin/duckdb-httpserver) | DuckDB HTTP server for accepting SQL queries |
| **substrait** | [duckdb-substrait](https://github.com/matrixorigin/duckdb-substrait) | Imports and exports Substrait query plans (GPU build only) |
| **sirius** | [sirius](https://github.com/matrixorigin/sirius) | GPU-accelerated SQL execution via cuCascade/cuDF |
| **mo-sidecar** | this repository | mTLS Flight server, strict execution envelope, authenticated `TaeRead` resolution, bounded Arrow streaming |

Extensions are statically linked into the DuckDB binary — no manual `LOAD` needed.
The GPU build adds Substrait and Sirius on top of the base extensions.

## Prerequisites

CMake ≥ 3.15, Ninja, Clang (recommended) or GCC ≥ 11, plus lz4 and OpenSSL dev
libraries.

**Debian / Ubuntu:**
```bash
sudo apt install clang cmake ninja-build liblz4-dev libssl-dev git libcurl4-openssl-dev
```

**Fedora / RHEL / Rocky:**
```bash
sudo dnf install clang cmake ninja-build lz4-devel openssl-devel git
```

**Arch Linux:**
```bash
sudo pacman -S clang cmake ninja lz4 openssl git
```

## Build

```bash
git clone --recurse-submodules https://github.com/matrixorigin/mo-sirius-sidecar.git
cd mo-sirius-sidecar

# Configure (first time only)
cmake -S duckdb -B build/release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DDUCKDB_EXTENSION_CONFIGS="$(pwd)/extension_config.cmake"

# Build
ninja -C build/release
```

Artifacts:
- `build/release/duckdb` — DuckDB shell with all extensions linked
- `build/release/extension/tae_scanner/tae_scanner.duckdb_extension` — loadable
- `build/release/extension/httpserver/httpserver.duckdb_extension` — loadable
- `build/release-gpu/extension/substrait/substrait.duckdb_extension` — loadable (GPU build only)
- `build/release-gpu/extension/sirius/sirius.duckdb_extension` — loadable (GPU build only)

### GPU build (requires CUDA)

The composite sidecar uses [pixi](https://pixi.sh) to pin CUDA, RAPIDS, Arrow
Flight, gRPC, and the compiler toolchain. Install pixi first, then initialize
the repository environment:

```bash
# Install pixi (one-time)
curl -fsSL https://pixi.sh/install.sh | bash

# Initialize Sirius submodule deps (cucascade is required at build time)
git -C sirius submodule update --init cucascade

# Install the locked composite toolchain into .pixi/
pixi install
```

Build from within the pixi environment so the compiler can find CUDA, cuDF,
lz4, and OpenSSL:

```bash
# Configure (first time only)
pixi run -- bash -c '
  cmake -S duckdb -B build/release-gpu -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DEXTENSION_STATIC_BUILD=1 \
    -DCMAKE_CXX_COMPILER=${CONDA_PREFIX}/bin/clang++ \
    -DCMAKE_C_COMPILER=${CONDA_PREFIX}/bin/clang \
    -DCMAKE_CXX_COMPILER_LAUNCHER= \
    -DCMAKE_C_COMPILER_LAUNCHER= \
    -DDUCKDB_EXTENSION_CONFIGS=$(pwd)/extension_config_gpu.cmake'

# Build
pixi run -- ninja -C build/release-gpu
```

> **Note:** The pixi compiler wrapper (conda-forge GCC) does not see system
> `/usr/include`, so native libraries are declared in the root `pixi.toml` and
> pinned by the root `pixi.lock`.

> **Note:** On machines without an NVIDIA GPU the build succeeds but the
> binary will print "NVML not available" and refuse GPU queries at runtime.

This adds the Substrait plan extension and Sirius GPU execution engine on top
of tae_scanner + httpserver.

## Deploy

Run the sidecar binary directly. The HTTP server auto-starts on the
specified port. Keep the DuckDB shell alive while serving; the packaged Docker
entrypoint holds stdin open for this purpose. Send `SIGINT` to shut down
gracefully.

```bash
# CPU sidecar
DUCKDB_HTTPSERVER_FOREGROUND=1 DUCKDB_HTTPSERVER_PORT=9876 \
  ./build/release/duckdb

# GPU sidecar (RPATH already points at sirius/.pixi/envs/default/lib —
# no `pixi run` wrap needed on the host where it was built)
DUCKDB_HTTPSERVER_FOREGROUND=1 DUCKDB_HTTPSERVER_PORT=9876 \
SIRIUS_LOG_LEVEL=info \
  ./build/release-gpu/duckdb
```

When backgrounding without a TTY, keep a writer open on a private FIFO. Do not
redirect stdin from `/dev/null`: that starts process teardown and unloads CUDA
while Flight workers are still serving.

```bash
fifo_dir=$(mktemp -d /tmp/mo-sidecar-stdin.XXXXXX)
fifo="$fifo_dir/pipe"
mkfifo "$fifo"
exec 9<>"$fifo"
rm -f "$fifo"
rmdir "$fifo_dir"
DUCKDB_HTTPSERVER_FOREGROUND=1 DUCKDB_HTTPSERVER_PORT=9876 \
  ./build/release-gpu/duckdb <&9 9>&- > sidecar.log 2>&1 &
sidecar_pid=$!
# Shutdown: close stdin, then stop HTTP/Flight.
exec 9>&-
kill -INT "$sidecar_pid"
```

Set `SIRIUS_LOG_LEVEL=debug` for verbose GPU execution logs (very noisy).

### Standalone Flight endpoint

The Flight endpoint is disabled unless `MO_SIDECAR_FLIGHT_PORT` is set. Once
enabled, every TLS/read-service setting is required and checked before the port
is bound:

| Variable | Purpose |
|---|---|
| `MO_SIDECAR_FLIGHT_HOST` | Bind host (default `0.0.0.0`) |
| `MO_SIDECAR_FLIGHT_PORT` | TLS Flight port; setting it enables the endpoint |
| `MO_SIDECAR_FLIGHT_CERT`, `MO_SIDECAR_FLIGHT_KEY` | Server certificate and private key PEM paths |
| `MO_SIDECAR_FLIGHT_CLIENT_CA` | CA used to require and verify MatrixOne client certificates |
| `MO_SIDECAR_READ_URL` | MatrixOne `https://.../internal/v1/sidecar/read/resolve` endpoint |
| `MO_SIDECAR_READ_CA` | CA used to verify the MatrixOne read service |
| `MO_SIDECAR_READ_CLIENT_CERT`, `MO_SIDECAR_READ_CLIENT_KEY` | Sidecar workload identity PEM paths |
| `MO_SIDECAR_MAX_ACTIVE_TICKETS` | Pending + running execution bound (default 128) |
| `MO_SIDECAR_MAX_BATCH_BYTES` | Per-batch hard bound (default 64 MiB) |
| `MO_SIDECAR_TICKET_TTL_MS` | Maximum unclaimed/running ticket lifetime (default 15 min; maximum 20 min) |

The wire schema and server behavior are documented in
[`mo-sidecar/README.md`](mo-sidecar/README.md). `GetCapabilities` returns the
canonical capability document; clients hash those exact bytes with SHA-256 and
include the digest in both the execution envelope and every `TaeRead`.

The supported bundled deployment is the local-CN profile below. Use these raw
environment variables only when starting a sidecar separately from MatrixOne;
they deliberately expose all transport choices and are not a multi-CN recipe.

### Container image (podman / docker)

A combined MO + GPU sidecar image is defined in `docker/Dockerfile`. The
canonical build entrypoint is `docker/build.sh`, which defaults to
`podman` (override with `BUILD_ENGINE=docker`):

```bash
./docker/build.sh                          # uses ../mo-tpch by default
MO_TPCH_DIR=/path/to/mo-tpch ./docker/build.sh
IMAGE_TAG=mo-sirius:dev ./docker/build.sh
BUILD_ENGINE=docker ./docker/build.sh

# Build MatrixOne from a specific repository and revision.
MO_REPO=https://github.com/aunjgr/matrixone.git MO_REF=<revision> \
  ./docker/build.sh
```

The build records the resolved MatrixOne and Sirius revisions at
`/etc/sidecar/matrixone-ref` and `/etc/sidecar/sirius-ref`. The MatrixOne
builder is pinned to the Go version declared in that source tree, and the
Dockerfile tolerates archive ownership metadata so the same command works with
rootless Podman as well as Docker.

#### Pulling the published image

The validated Flight image is published as the immutable tag
`ghcr.io/aunjgr/mo-sirius:flight-dd712e795f-db05a6c`. The GHCR package is
public, so anyone can pull it without a registry login:

```bash
podman pull ghcr.io/aunjgr/mo-sirius:flight-dd712e795f-db05a6c
```

Docker users use `docker pull` with the same tag. The tag records MatrixOne
`dd712e795f7734b90400941a8be396525ab32274` and Sirius
`fc5e6765db019f72ba2228276ebb9503fd4f061e`; verify the files in
`/etc/sidecar/` after starting a container if you need to audit provenance.

Choose the image profile before starting it:

| Profile | Select it with | MatrixOne transport | Host ports | Intended use |
|---|---|---|---|---|
| Legacy HTTP | default | rewritten SQL over `http://127.0.0.1:9999` | `6001`, `8888`, optionally `9999` | compatibility and side-by-side benchmarks |
| Flight/Substrait | `MO_SIRIUS_FLIGHT=1` | logical Substrait plan and Arrow batches over local mTLS Flight | `6001`, `8888`; never publish Flight | one local CN and its paired GPU sidecar |

The two profiles are mutually exclusive. The Flight profile does not use
`cn.frontend.sidecarUrl`, does not accept a remote sidecar, and must not be
combined with individually overridden `MO_SIDECAR_FLIGHT_*` or
`MO_SIDECAR_READ_*` values.

The legacy profile can start without an mTLS bundle. The Flight profile cannot
and should not: the image deliberately contains no CA, certificate, or private
key. Its entrypoint exits before starting either service unless the twelve
directional mTLS files listed below are mounted. A user therefore needs three
things before the regular Flight profile is runnable: an NVIDIA GPU exposed to
the container runtime, a certificate bundle issued for that local CN/sidecar
pair, and the local MatrixOne data volume.

#### Legacy HTTP profile

A typical run with all bind-mounts (data, TPC-H scratch, logs, sirius
config) — daemonized so we can drive it later via `podman exec`:

```bash
mkdir -p $(pwd)/{mo-data,tpch-data,log}
podman run -d --name mo-sirius --device nvidia.com/gpu=all \
  -p 6001:6001 -p 8888:8888 -p 9999:9999 \
  -v $(pwd)/mo-data:/mo-data \
  -v $(pwd)/tpch-data:/opt/mo-tpch/data \
  -v $(pwd)/log:/log \
  -v $(pwd)/sirius.yaml:/etc/sidecar/sirius.yaml:ro \
  mo-sirius:latest
```

> **GPU access:** podman uses CDI (`--device nvidia.com/gpu=all` or
> `=<index>` / `=<UUID>` to pin one GPU). Docker users substitute
> `--gpus all`.

What each mount is for:

- **`/mo-data`** — MO catalog, logs, and TAE objects. The bundled MO
  configs use `data-dir = "./mo-data"` and the entrypoint runs from
  `/`. Without this mount the data lives in the container's writable
  layer and is **lost when the container is removed**. Same convention
  as upstream's `etc/docker-multi-cn-local-disk/docker-compose.yml`.
- **`/opt/mo-tpch/data`** — TPC-H `dbgen` output. Required for SF ≥ 10
  to keep multi-GB `.tbl` files out of the writable layer. Set
  `DATA_DIR` env to override.
- **`/log`** — see "Container logs" below.

#### Flight/Substrait local-CN benchmark profile

The image defaults to the legacy HTTP profile above. To start one local CN and
its paired GPU sidecar with the authenticated Flight path, opt in explicitly:

```bash
mkdir -p $(pwd)/{mo-data,tpch-data,log,certs}
podman run -d --name mo-sirius-flight --device nvidia.com/gpu=all \
  -p 6001:6001 -p 8888:8888 \
  -e MO_SIRIUS_FLIGHT=1 \
  -v $(pwd)/mo-data:/mo-data \
  -v $(pwd)/tpch-data:/opt/mo-tpch/data \
  -v $(pwd)/log:/log \
  -v $(pwd)/certs:/etc/sirius-certs:ro \
  ghcr.io/aunjgr/mo-sirius:latest
```

`MO_SIRIUS_FLIGHT=1` selects `launch-flight.toml`, starts the sidecar's TLS
Flight service on the container-only loopback address, and starts the MatrixOne
read resolver with mTLS. No Flight port is published to the host. The entrypoint
sets the endpoint and credential environment itself; do not override individual
`MO_SIDECAR_FLIGHT_*` or `MO_SIDECAR_READ_*` variables for this profile.

The read-only `certs` mount must contain these PEM files:

| Direction | Server CA | Client CA | Server identity | Client identity |
|---|---|---|---|---|
| MatrixOne CN → sidecar Flight | `sidecar-flight-ca.crt` | `mo-flight-client-ca.crt` | `sidecar-flight-server.crt`, `sidecar-flight-server.key` | `mo-flight-client.crt`, `mo-flight-client.key` |
| Sidecar → MatrixOne resolver | `mo-resolver-server-ca.crt` | `sidecar-read-client-ca.crt` | `mo-resolver-server.crt`, `mo-resolver-server.key` | `sidecar-read-client.crt`, `sidecar-read-client.key` |

The Flight server certificate must identify `sidecar`, and the resolver server
certificate must identify `localhost`. The profile intentionally enables the
non-durable, local lease adapter and disables GC in its paired TN. It is for a
one-CN benchmark only: do not use it for production traffic, restart recovery,
or multiple CNs.

#### Zero-config development TLS

For a fresh, same-container benchmark, the `flight-dev-tls-v1` image release
adds `MO_SIRIUS_FLIGHT_DEV_TLS=1`. It creates fresh, one-day, directional mTLS
roots and leaf certificates inside a new container before it starts either the
sidecar or MatrixOne. Nothing is baked into the image and nothing needs to be
mounted at `/etc/sirius-certs`:

```bash
podman pull ghcr.io/aunjgr/mo-sirius:flight-dev-tls-v1
mkdir -p $(pwd)/{mo-data,tpch-data,log}
podman run -d --name mo-sirius-flight-dev --device nvidia.com/gpu=all \
  -p 6001:6001 -p 8888:8888 \
  -e MO_SIRIUS_FLIGHT=1 \
  -e MO_SIRIUS_FLIGHT_DEV_TLS=1 \
  -v $(pwd)/mo-data:/mo-data \
  -v $(pwd)/tpch-data:/opt/mo-tpch/data \
  -v $(pwd)/log:/log \
  ghcr.io/aunjgr/mo-sirius:flight-dev-tls-v1
```

The generated identities are deliberately ephemeral and local to this one
container. A container restart reuses its still-valid credentials and rotates
them when they are close to expiry. Do not use this mode with a certificate
mount, a separately started sidecar, a remote CN, or any production or
restart-recovery workflow. Use the mounted certificate profile above whenever
the mTLS identity must outlive the container.

Confirm the running image was built from the MatrixOne and Sirius revisions you
expect, then run the GPU TPC-H path through Flight:

```bash
podman exec mo-sirius-flight sh -c \
  'printf "MatrixOne: "; cat /etc/sidecar/matrixone-ref; \
   printf "Sirius: "; cat /etc/sidecar/sirius-ref'

# Complete SF1 run. The Flight profile turns ENGINE=gpu into explicit
# MatrixOne Substrait/Flight offload rather than legacy HTTP forwarding.
podman exec mo-sirius-flight bash -lc 'ENGINE=gpu tpch-bench 1'

# Reuse an already loaded SF10 data set and run queries only.
podman exec mo-sirius-flight bash -lc \
  'ENGINE=gpu GEN=0 CTAB=0 LOAD=0 tpch-bench 10'
```

**Running TPC-H benchmarks.** The image bundles
[`mo-tpch`](https://github.com/matrixorigin/mo-tpch) at `/opt/mo-tpch`
with a pre-built `dbgen`, the schema (`mo.ddl`), all 22 queries, and
golden answers. A convenience wrapper `tpch-bench` runs the full
generate → create-tables → load → query pipeline, with an `ENGINE`
switch to route queries through MO native, the CPU sidecar, or the
GPU sidecar:

```bash
# inside the running container (or via podman exec):
tpch-bench 1                          # SF=1, all phases, ENGINE=native (default)
SF=10 tpch-bench                      # SF=10
GEN=0 LOAD=0 tpch-bench 10            # SF=10, queries only
ENGINE=cpu  GEN=0 LOAD=0 tpch-bench 10   # route via CPU sidecar (/*+ SIDECAR */)
ENGINE=gpu  GEN=0 LOAD=0 tpch-bench 10   # route via GPU sidecar (/*+ SIDECAR GPU */)

# override MO connection or data location:
MO_HOST=mo MO_PORT=6001 tpch-bench 1
DATA_DIR=/data/sf10 tpch-bench 10     # bind-mount /data for large SFs
```

`ENGINE=cpu|gpu` injects the corresponding sidecar hint as the first line of
every query before piping to `mariadb --comments`. In the default image profile
MO forwards rewritten SQL to `http://127.0.0.1:9999`. With
`MO_SIRIUS_FLIGHT=1`, use `ENGINE=gpu`: MO exports its logical plan as
Substrait and streams results through the local mTLS Flight connection instead.
The Flight schema represents MatrixOne `CHAR(n)` as `VARCHAR(n)` without
trimming or padding; MatrixOne converts the result back to the original output
type.

To drive a legacy HTTP benchmark from the host against the daemonized legacy
container above, use `podman exec`:

```bash
podman exec mo-sirius bash -lc 'ENGINE=gpu tpch-bench 10'

# Reuse already-loaded data — queries only:
podman exec mo-sirius bash -lc 'ENGINE=gpu GEN=0 CTAB=0 LOAD=0 tpch-bench 10'

# Run in the background (detached); follow output via the /log bind-mount:
podman exec -d mo-sirius bash -lc 'ENGINE=gpu tpch-bench 10'
tail -f log/tpch/*/run.log
```

Use the `mo-sirius-flight` commands above for Flight; the two container names
are deliberately different so a legacy HTTP benchmark cannot be confused with
an authenticated Substrait run.

**Container logs.** MO and the sidecar run at debug level by default
and would otherwise flood the host's syslog through the journald log
driver. The entrypoint redirects them — and mo-tpch's
`/opt/mo-tpch/report` and `run.log` — into files under `/log` inside
the container:

```text
/log/
├── mo-YYYYMMDD-HHMMSS.log       # mo-service stdout/stderr (per container start)
├── sidecar-YYYYMMDD-HHMMSS.log  # DuckDB sidecar stdout/stderr (per container start)
└── tpch/
    └── YYYYMMDD-HHMMSS/         # one subdir per container start (symlinked
        ├── run.log              #   from /opt/mo-tpch/{report,run.log})
        └── TPCH_<SF>/q*.txt
```

Container stdout only carries `[entrypoint]` lifecycle messages.
Bind-mount `/log` (see the run example above) to harvest everything
on the host, or set `LOG_DIR` to a different in-container path.

**Runtime configuration overrides.** The image ships a default
`sirius.yaml` at `/etc/sidecar/sirius.yaml` and MO configs at
`/etc/launch/*.toml`. The typical run above already shows the
sirius.yaml bind-mount; you can do the same for the MO configs, or
bypass them entirely:

- **Point `SIRIUS_CONFIG_FILE` at a custom path:**

  ```bash
  podman run --device nvidia.com/gpu=all ... \
    -v /host/configs:/custom:ro \
    -e SIRIUS_CONFIG_FILE=/custom/my-sirius.yaml \
    mo-sirius:latest
  ```

- **Tune knobs via environment variables** (see table below) — all
  `SIRIUS_*`, `DUCKDB_HTTPSERVER_*`, `MO_DEBUG_HTTP`, and
  `MO_LAUNCH_CONF` are passed through. `MO_SIRIUS_FLIGHT=1` owns the
  Flight endpoint variables and requires its bundled launch profile. Do not
  override `MO_LAUNCH_CONF` in that mode unless the replacement preserves the
  same one-CN, loopback-only, mTLS configuration:

  ```bash
  podman run --device nvidia.com/gpu=all ... \
    -e SIRIUS_TAE_BASELINE_COLS=6 \
    -e SIRIUS_LOG_LEVEL=info \
    -e DUCKDB_HTTPSERVER_AUTH=my-secret-token \
    mo-sirius:latest
  ```

### Environment variables

| Variable | Default | Description |
|----------|---------|-------------|
| `DUCKDB_HTTPSERVER_PORT` | *(none)* | Set to auto-start HTTP server on this port |
| `DUCKDB_HTTPSERVER_HOST` | `0.0.0.0` | Listen address |
| `DUCKDB_HTTPSERVER_AUTH` | *(empty)* | Auth token (X-API-Key or Basic auth) |
| `DUCKDB_HTTPSERVER_FOREGROUND` | `0` | Set to `1` to block after startup (daemon mode) |
| `SIRIUS_LOG_LEVEL` | `warn` | Sirius GPU engine log level (`info`, `debug`, `trace`) |
| `SIRIUS_TAE_BASELINE_COLS` | `4` | GPU TAE scan: projected-col count at which `scan_task_batch_size` is used as-is. Effective cap scales as `scan_task_batch_size × baseline / proj_cols` (floored at 32MB); wider projections get smaller per-task batches to reduce GPU tail latency. `0` disables scaling. |
| `MO_SIRIUS_FLIGHT` | `0` | Set to `1` to select the bundled local-CN Substrait/Flight benchmark profile; requires `/etc/sirius-certs` unless development TLS is enabled |
| `MO_SIRIUS_FLIGHT_DEV_TLS` | `0` | With `MO_SIRIUS_FLIGHT=1`, generate fresh in-container development mTLS credentials; never use with a cert mount or outside the one-container benchmark |
| `MO_LAUNCH_CONF` | `/etc/launch/launch.toml` | MO launch manifest; defaults to `launch-flight.toml` when `MO_SIRIUS_FLIGHT=1` |

### Manual start (interactive)

```bash
./build/release/duckdb \
  -cmd "SELECT httpserve_start('0.0.0.0', 9876, '')"
```

### Verify

```bash
curl 'http://localhost:9876/?default_format=JSONCompact&query=SELECT+42'
# Expected: {"meta":[{"name":"42","type":"Int32"}],"data":[[42]],"rows":1}
```

## MatrixOne integration


### 1. Start the sidecar

Start the CPU or GPU sidecar on port 9876 (see [Deploy](#deploy) above).

### 2. Start MatrixOne

MatrixOne supports SiriusDB sidecar offloading.  Currently MO must be started with
the `-debug-http` flag — this enables the internal
`/debug/tae/manifest` endpoint that the sidecar uses to discover TAE objects:

```bash
cd /path/to/matrixone
./mo-service -debug-http :8888 -launch etc/launch/launch.toml
```

### 3. Configure the sidecar URL

Add to `cn.toml`:

```toml
[cn.frontend]
sidecarUrl = "http://localhost:9876"
```

Or set it per-session (useful for testing):

```sql
SET sidecar_url = 'http://localhost:9876';
```

### 4. Run queries

TPC-H dataset can be loaded from [mo-tpch](https://github.com/matrixorigin/mo-tpch).

```sql
-- CPU sidecar (DuckDB vectorized engine):
/*+ SIDECAR */ SELECT count(*) FROM tpch.lineitem WHERE l_shipdate < '1998-09-01';

-- GPU sidecar (Sirius + cuDF, wraps query in gpu_execution()):
/*+ SIDECAR GPU */ SELECT count(*) FROM tpch.lineitem WHERE l_shipdate < '1998-09-01';
```

If the sidecar is not configured or not reachable, MO silently falls back to
native execution (the hint is stripped).

NOTE: MatrixOne uses a MySQL-compatible client protocol.  Any MySQL client can connect
to MatrixOne and run queries.  If using the `mariadb` client, add `--comments` so that
SQL hints are preserved:

```bash
mariadb --skip-ssl -h 127.0.0.1 -P 6001 -u dump -p111 --comments
```

### Known issues

- **MO HTTP timeout:** MO's `fileservice` package overrides `http.DefaultTransport`
  with a 20-second `ResponseHeaderTimeout`. The sidecar HTTP client in MO must use
  a dedicated `http.Transport` to avoid this — see `pkg/frontend/sidecar_offload.go`.
- **GPU VRAM limits:** Multi-table joins at SF100+ may hang if the GPU has
  insufficient VRAM (tested: RTX 3070 8GB handles SF10 fully, SF100 Q1-Q2 only).

## How it works

```
Client                  MatrixOne                Sidecar (DuckDB + Sirius)
  │                         │                         │
  │ /*+ SIDECAR [GPU] */    │                         │
  │────────────────────────>│                         │
  │                         │  GET /debug/tae/manifest│
  │                         │  (internal, for schema) │
  │                         │                         │
  │                         │  Rewrite: table refs →  │
  │                         │  tae_scan(manifest_url) │
  │                         │  GPU: wrap in           │
  │                         │  gpu_execution()        │
  │                         │                         │
  │                         │  POST rewritten SQL     │
  │                         │────────────────────────>│
  │                         │                         │
  │                         │                         │ CPU path:
  │                         │                         │  tae_scan → pread →
  │                         │                         │  LZ4 decompress (CPU) →
  │                         │                         │  DuckDB vectors →
  │                         │                         │  DuckDB engine
  │                         │                         │
  │                         │                         │ GPU path:
  │                         │                         │  tae_scan_task → pread →
  │                         │                         │  pinned host memory →
  │                         │                         │  cudaMemcpy to GPU →
  │                         │                         │  nvCOMP LZ4 decompress →
  │                         │                         │  CUDA decode kernels →
  │                         │                         │  cudf filter pushdown →
  │                         │                         │  Sirius GPU engine
  │                         │                         │
  │                         │  JSONCompact response   │
  │                         │<────────────────────────│
  │  MySQL result set       │                         │
  │<────────────────────────│                         │
```

## Architecture

```
mo-sirius-sidecar/
├── duckdb/                    ← DuckDB v1.5.2 (submodule)
├── extension-ci-tools/        ← DuckDB build helpers (submodule)
├── tae-scanner/               ← TAE storage reader (submodule)
│   ├── src/                   ← Scanner, filter, object reader
│   └── include/               ← Headers
├── httpserver/                ← HTTP query server (submodule)
│   └── src/                   ← Server, serializers
├── sirius/                    ← GPU SQL engine (submodule)
│   ├── substrait/             ← Substrait plan extension (nested submodule)
│   └── src/
│       ├── op/scan/           ← tae_scan_task (GPU native TAE reader)
│       ├── data/              ← host_tae→gpu_table converter (nvCOMP + CUDA)
│       ├── cuda/tae/          ← CUDA kernels (fixed decode, varchar, null mask)
│       ├── tae/               ← TAE metadata parser
│       └── ...                ← GPU operators, cuCascade, planner
├── extension_config.cmake     ← CPU extensions config
├── extension_config_gpu.cmake ← GPU extensions config
├── Makefile                   ← Build wrapper
├── DESIGN.md                  ← Architecture document
└── README.md
```
