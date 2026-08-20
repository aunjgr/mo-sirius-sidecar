# Combined MO + DuckDB Sidecar (GPU) + mo-tpch Docker image.
#
# All stages use Debian Trixie for consistency.
# MO is built without CUDA (standard CPU build).
# The sidecar is built with CUDA 13 via pixi
# (tae_scanner + httpserver + substrait + sirius).
# mo-tpch is shipped with a pre-built dbgen at /opt/mo-tpch.
#
# Build:
#   ./docker/build.sh                         # uses ../mo-tpch by default
#   MO_TPCH_DIR=/path/to/mo-tpch ./docker/build.sh
#   MO_REF=<matrixone-commit-or-branch> ./docker/build.sh
#
# The script runs `git archive` against MO_TPCH_DIR to feed mo-tpch source
# into the build via a named build context. Raw `podman build` works too:
#   tmpdir=$(mktemp -d) && \
#   git -C ../mo-tpch archive HEAD | tar -C $tmpdir -x && \
#   podman build --build-context mo-tpch=$tmpdir \
#     -t mo-sirius:latest -f docker/Dockerfile . && \
#   rm -rf $tmpdir
#
# Run (legacy SQL HTTP only):
#   docker run --gpus all -p 6001:6001 -p 8888:8888 -p 9999:9999 mo-sirius:latest
# Set MO_SIRIUS_FLIGHT=1 and mount the mTLS credentials to enable the local-CN
# Substrait/Flight benchmark profile. See README.md for the exact contract.
#
# Run TPC-H benchmark inside the container:
#   docker exec -it <container> tpch-bench 1            # SF=1, all phases
#   docker exec -it <container> env GEN=0 tpch-bench 10 # SF=10, skip dbgen

# ---------------------------------------------------------------------------
# Stage 1: Build MO (CPU only)
# ---------------------------------------------------------------------------
# Keep this in sync with MatrixOne's go.mod. An unversioned golang:trixie
# image can move to an incompatible experimental Go toolchain.
FROM golang:1.26.4-trixie AS mo-builder

ARG GOPROXY="https://proxy.golang.org,direct"
ENV TAR_OPTIONS=--no-same-owner
RUN go env -w GOPROXY=${GOPROXY}

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake && \
    rm -rf /var/lib/apt/lists/*

ARG MO_REPO=https://github.com/matrixorigin/matrixone.git
ARG MO_BRANCH=main
ARG MO_REF=

RUN git clone --branch "$MO_BRANCH" "$MO_REPO" /mo && \
    if [ -n "$MO_REF" ]; then git -C /mo checkout --detach "$MO_REF"; fi
WORKDIR /mo
RUN go mod download && make clean && make build && strip mo-service
RUN git rev-parse HEAD > /mo-build-ref

# ---------------------------------------------------------------------------
# Stage 2: Build sidecar (GPU via pixi)
# ---------------------------------------------------------------------------
FROM debian:trixie AS sidecar-builder

ARG SIRIUS_REF=unknown

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        git ca-certificates curl libnuma-dev binutils && \
    rm -rf /var/lib/apt/lists/*

# Install pixi (unpinned: install.sh fetches the latest release)
RUN curl -fsSL https://pixi.sh/install.sh | bash
ENV PATH="/root/.pixi/bin:${PATH}"

WORKDIR /sidecar
COPY . .

# Let pixi resolve and install all CUDA/RAPIDS/compiler dependencies,
# then build the sidecar with the GPU extension config.
RUN pixi install

RUN pixi run -- bash -c '\
    cmake -S duckdb -B build/release-gpu -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DEXTENSION_STATIC_BUILD=1 \
        -DCMAKE_CXX_COMPILER=${CONDA_PREFIX}/bin/clang++ \
        -DCMAKE_C_COMPILER=${CONDA_PREFIX}/bin/clang \
        -DCMAKE_CXX_COMPILER_LAUNCHER= \
        -DCMAKE_C_COMPILER_LAUNCHER= \
        -DCMAKE_LINKER_TYPE=MOLD \
        -DDUCKDB_EXTENSION_CONFIGS=$(pwd)/extension_config_gpu.cmake && \
    ninja -C build/release-gpu'

# Strip sidecar binary and large shared libs.
RUN strip /sidecar/build/release-gpu/duckdb && \
    find /sidecar/build/release-gpu -name '*.so' -exec strip --strip-unneeded {} + 2>/dev/null; true

# Collect only the pixi-provided runtime libraries needed by the sidecar binary.
COPY docker/collect-runtime-libs.sh /sidecar/collect-runtime-libs.sh
RUN pixi run -- bash /sidecar/collect-runtime-libs.sh \
    /sidecar/build/release-gpu/duckdb /sidecar-runtime-libs
RUN printf '%s\n' "${SIRIUS_REF}" > /sidecar/sirius-build-ref

# ---------------------------------------------------------------------------
# Stage 3: Build mo-tpch dbgen
# ---------------------------------------------------------------------------
# Source comes from a named build context "mo-tpch" populated by
# docker/build.sh via `git archive` (so only committed files, no data/).
FROM debian:trixie-slim AS mo-tpch-builder

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates build-essential && \
    rm -rf /var/lib/apt/lists/*

COPY --from=mo-tpch . /mo-tpch
WORKDIR /mo-tpch/dbgen
RUN make && rm -f *.o

# ---------------------------------------------------------------------------
# Stage 4: Runtime image
# ---------------------------------------------------------------------------
FROM debian:trixie-slim

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        libgomp1 curl tini bc python3 mariadb-client-core openssl && \
    ln -sf /usr/bin/python3 /usr/local/bin/python && \
    rm -rf /var/lib/apt/lists/*

# Ensure intra-container HTTP traffic (sidecar <-> MO debug-http) bypasses any
# proxy inherited from the host. Without this, sidecar's HTTP HEAD to
# http://localhost:8888/debug/tae/manifest gets routed through http_proxy and
# fails with "Server returned nothing".
ENV no_proxy=localhost,127.0.0.1,::1 \
    NO_PROXY=localhost,127.0.0.1,::1

# MO binary
COPY --from=mo-builder /mo/mo-service /mo-service
COPY --from=mo-builder /mo/thirdparties/install/lib/*.so /usr/local/lib/

# MO config files (maintained in this repo, includes sidecarUrl).
COPY docker/etc/launch.toml /etc/launch/launch.toml
COPY docker/etc/cn.toml     /etc/launch/cn.toml
COPY docker/etc/tn.toml     /etc/launch/tn.toml
COPY docker/etc/log.toml    /etc/launch/log.toml
COPY docker/etc/launch-flight.toml /etc/launch/launch-flight.toml
COPY docker/etc/cn-flight.toml     /etc/launch/cn-flight.toml
COPY docker/etc/tn-flight.toml     /etc/launch/tn-flight.toml

# Sidecar binary + pixi runtime libs + Sirius config
COPY --from=sidecar-builder /sidecar/build/release-gpu/duckdb /sidecar/duckdb
COPY --from=sidecar-builder /sidecar-runtime-libs/ /usr/local/lib/
COPY sirius.yaml.example /etc/sidecar/sirius.yaml
COPY sirius.dev-tls.yaml /etc/sidecar/sirius-dev-tls.yaml
COPY --from=mo-builder /mo-build-ref /etc/sidecar/matrixone-ref
COPY --from=sidecar-builder /sidecar/sirius-build-ref /etc/sidecar/sirius-ref

# mo-tpch (dbgen, queries, schema, golden answers, run.sh)
COPY --from=mo-tpch-builder /mo-tpch /opt/mo-tpch

RUN ldconfig

WORKDIR /

# /log holds:
#   - mo-<ts>.log + sidecar-<ts>.log (entrypoint.sh redirects MO and sidecar there)
#   - tpch/<ts>/  -> mo-tpch's report dir (per-query result rows + run.log)
# /opt/mo-tpch/{report,run.log} are recreated as symlinks into /log/tpch/<ts>/
# by entrypoint.sh on each container start. Container stdout (captured by the
# log driver) carries only [entrypoint] lifecycle messages. Bind-mount /log
# (-v /host/path:/log) to see everything on the host; otherwise these files
# live in the container's writable layer.
RUN mkdir -p /log/tpch && \
    rm -rf /opt/mo-tpch/report /opt/mo-tpch/run.log

# MO ports: 6001 (SQL), 8888 (debug HTTP)
# Sidecar ports: 9999 (legacy DuckDB HTTP API), 32010 (Arrow Flight)
EXPOSE 6001 8888 9999 32010

COPY docker/entrypoint.sh /entrypoint.sh
COPY docker/generate-dev-tls.sh /usr/local/bin/generate-dev-tls.sh
COPY docker/tpch-bench.sh /usr/local/bin/tpch-bench
RUN chmod +x /entrypoint.sh /usr/local/bin/generate-dev-tls.sh /usr/local/bin/tpch-bench

ENTRYPOINT ["/usr/bin/tini", "--", "/entrypoint.sh"]
