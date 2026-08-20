#!/usr/bin/env bash
set -euo pipefail

MO_DEBUG_HTTP="${MO_DEBUG_HTTP:-:8888}"
MO_SIRIUS_FLIGHT="${MO_SIRIUS_FLIGHT:-0}"
if [ "${MO_SIRIUS_FLIGHT}" = "1" ]; then
    MO_LAUNCH_CONF="${MO_LAUNCH_CONF:-/etc/launch/launch-flight.toml}"
    cert_dir=/etc/sirius-certs
    for cert_file in \
        sidecar-flight-ca.crt mo-flight-client-ca.crt mo-flight-client.crt mo-flight-client.key \
        mo-resolver-server-ca.crt sidecar-read-client-ca.crt mo-resolver-server.crt mo-resolver-server.key \
        sidecar-flight-server.crt sidecar-flight-server.key \
        sidecar-read-client.crt sidecar-read-client.key; do
        if [ ! -r "${cert_dir}/${cert_file}" ]; then
            echo "[entrypoint] ERROR: Flight profile requires ${cert_dir}/${cert_file}." >&2
            exit 1
        fi
    done
    export MO_SIDECAR_FLIGHT_HOST=127.0.0.1
    export MO_SIDECAR_FLIGHT_PORT=32010
    export MO_SIDECAR_FLIGHT_CERT="${cert_dir}/sidecar-flight-server.crt"
    export MO_SIDECAR_FLIGHT_KEY="${cert_dir}/sidecar-flight-server.key"
    export MO_SIDECAR_FLIGHT_CLIENT_CA="${cert_dir}/mo-flight-client-ca.crt"
    export MO_SIDECAR_READ_URL=https://localhost:32011/internal/v1/sidecar/read/resolve
    export MO_SIDECAR_READ_CA="${cert_dir}/mo-resolver-server-ca.crt"
    export MO_SIDECAR_READ_CLIENT_CERT="${cert_dir}/sidecar-read-client.crt"
    export MO_SIDECAR_READ_CLIENT_KEY="${cert_dir}/sidecar-read-client.key"
else
    MO_LAUNCH_CONF="${MO_LAUNCH_CONF:-/etc/launch/launch.toml}"
fi

export SIRIUS_CONFIG_FILE="${SIRIUS_CONFIG_FILE:-/etc/sidecar/sirius.yaml}"
export DUCKDB_HTTPSERVER_HOST="${DUCKDB_HTTPSERVER_HOST:-0.0.0.0}"
export DUCKDB_HTTPSERVER_PORT="${DUCKDB_HTTPSERVER_PORT:-9999}"
export DUCKDB_HTTPSERVER_AUTH="${DUCKDB_HTTPSERVER_AUTH:-}"
export DUCKDB_HTTPSERVER_FOREGROUND=1

# MO and the sidecar are both very chatty (debug-level by default). When the
# container's log driver is journald (podman/docker default) every line ends
# up in the host's syslog, which is unpleasant for benchmarking sessions.
# Redirect their stdout/stderr to files under /log so the container's own
# stdout (captured by the log driver) only carries [entrypoint] lifecycle
# messages. Bind-mount /log if you want the files on the host.
LOG_DIR="${LOG_DIR:-/log}"
LOG_TS="$(date +%Y%m%d-%H%M%S)"
mkdir -p "${LOG_DIR}/tpch/${LOG_TS}"
SIDECAR_LOG="${LOG_DIR}/sidecar-${LOG_TS}.log"
MO_LOG="${LOG_DIR}/mo-${LOG_TS}.log"

# Point mo-tpch's report dir + run.log at this run's timestamped subdir so
# repeated container starts don't clobber each other's TPC-H output. mo-tpch's
# run.sh writes to ${WORKSPACE}/run.log and ${WORKSPACE}/report/<CASE>/ via
# two independent paths, hence two symlinks landing in the same dir; the
# touch makes the run.log symlink live from the start (tee -a / >> would
# create on first write anyway, but a dead symlink confuses ls/tail).
touch "${LOG_DIR}/tpch/${LOG_TS}/run.log"
ln -sfn "${LOG_DIR}/tpch/${LOG_TS}"          /opt/mo-tpch/report
ln -sfn "${LOG_DIR}/tpch/${LOG_TS}/run.log"  /opt/mo-tpch/run.log

MO_PID=""
SIDECAR_PID=""
SIDECAR_STDIN_DIR=""

cleanup() {
    echo "[entrypoint] Shutting down ..."
    # Closing the parent's write end lets the DuckDB shell observe EOF. Its
    # foreground atexit blocker then waits only until SIGINT stops HTTP/Flight.
    exec 9>&- 2>/dev/null || true
    if [ -n "${SIDECAR_STDIN_DIR}" ]; then
        rm -f "${SIDECAR_STDIN_DIR}/pipe"
        rmdir "${SIDECAR_STDIN_DIR}" 2>/dev/null || true
    fi
    # Both MO and the sidecar httpserver use SIGINT for graceful shutdown.
    [ -n "$MO_PID" ]      && kill -INT "$MO_PID"      2>/dev/null || true
    [ -n "$SIDECAR_PID" ] && kill -INT "$SIDECAR_PID"  2>/dev/null || true
    wait 2>/dev/null || true
}
trap cleanup EXIT

# --- Start sidecar first and wait for it to be ready ---
echo "[entrypoint] Starting DuckDB sidecar on ${DUCKDB_HTTPSERVER_HOST}:${DUCKDB_HTTPSERVER_PORT} (logs: ${SIDECAR_LOG}) ..."
# Keep stdin open while the service is live. Redirecting /dev/null lets the
# DuckDB shell enter process teardown before the HTTP atexit blocker runs;
# CUDA then unloads underneath still-serving Flight workers.
SIDECAR_STDIN_DIR="$(mktemp -d "${LOG_DIR}/.sidecar-stdin.XXXXXX")"
SIDECAR_STDIN_FIFO="${SIDECAR_STDIN_DIR}/pipe"
mkfifo "${SIDECAR_STDIN_FIFO}"
exec 9<>"${SIDECAR_STDIN_FIFO}"
rm -f "${SIDECAR_STDIN_FIFO}"
rmdir "${SIDECAR_STDIN_DIR}"
SIDECAR_STDIN_DIR=""
/sidecar/duckdb <&9 9>&- >>"${SIDECAR_LOG}" 2>&1 &
SIDECAR_PID=$!

SIDECAR_URL="http://127.0.0.1:${DUCKDB_HTTPSERVER_PORT}"
echo "[entrypoint] Waiting for sidecar to be ready ..."
for i in $(seq 1 30); do
    if curl -sf --noproxy '*' "${SIDECAR_URL}/ping" >/dev/null 2>&1; then
        echo "[entrypoint] Sidecar ready."
        break
    fi
    if ! kill -0 "$SIDECAR_PID" 2>/dev/null; then
        echo "[entrypoint] ERROR: Sidecar process exited before becoming ready."
        exit 1
    fi
    sleep 1
done

if ! curl -sf --noproxy '*' "${SIDECAR_URL}/ping" >/dev/null 2>&1; then
    echo "[entrypoint] ERROR: Sidecar did not become ready within 30 seconds."
    exit 1
fi

# --- Start MO ---
echo "[entrypoint] Starting mo-service (logs: ${MO_LOG}) ..."
/mo-service -debug-http="${MO_DEBUG_HTTP}" -launch "${MO_LAUNCH_CONF}" >>"${MO_LOG}" 2>&1 &
MO_PID=$!

# Wait for either process to exit; if one dies, the EXIT trap tears down the other.
set +e
wait -n "$MO_PID" "$SIDECAR_PID"
EXIT_CODE=$?
set -e

echo "[entrypoint] A process exited (code=$EXIT_CODE), shutting down."
exit $EXIT_CODE
