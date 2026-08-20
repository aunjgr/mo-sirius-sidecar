#!/usr/bin/env bash
# Build the mo-sirius image with a clean snapshot of mo-tpch source.
#
# mo-tpch is consumed via a named build context populated by `git archive`,
# so only committed files are included (no data/, no .o, no built binaries).
#
# Usage:
#   ./docker/build.sh                          # ../mo-tpch @ mo-sirius-bench
#   MO_TPCH_DIR=/path/to/mo-tpch ./docker/build.sh
#   MO_TPCH_REF=main ./docker/build.sh         # archive a different ref
#   MO_REF=<matrixone-commit-or-branch> ./docker/build.sh
#   MO_REPO=https://github.com/user/matrixone.git MO_REF=<ref> ./docker/build.sh
#   SIRIUS_REF=<sirius-commit> ./docker/build.sh
#   IMAGE_TAG=mo-sirius:dev ./docker/build.sh
#   BUILD_ENGINE=docker ./docker/build.sh
#
set -euo pipefail

REPO_DIR=$(cd "$(dirname "$0")/.." && pwd)
MO_TPCH_DIR=${MO_TPCH_DIR:-${REPO_DIR}/../mo-tpch}
MO_TPCH_REF=${MO_TPCH_REF:-mo-sirius-bench}
MO_REPO=${MO_REPO:-https://github.com/matrixorigin/matrixone.git}
MO_BRANCH=${MO_BRANCH:-main}
MO_REF=${MO_REF:-}
SIRIUS_REF=${SIRIUS_REF:-$(git -C "${REPO_DIR}/sirius" rev-parse HEAD)}
IMAGE_TAG=${IMAGE_TAG:-mo-sirius:latest}
BUILD_ENGINE=${BUILD_ENGINE:-podman}

if [[ ! -d "${MO_TPCH_DIR}/.git" ]]; then
    echo "error: ${MO_TPCH_DIR} is not a git repo" >&2
    exit 1
fi
if ! git -C "${MO_TPCH_DIR}" rev-parse --verify --quiet "${MO_TPCH_REF}" >/dev/null; then
    echo "error: ref '${MO_TPCH_REF}' not found in ${MO_TPCH_DIR}" >&2
    exit 1
fi

tmpdir=$(mktemp -d)
trap 'rm -rf "${tmpdir}"' EXIT

echo "[build] extracting mo-tpch ${MO_TPCH_REF} from ${MO_TPCH_DIR} -> ${tmpdir}"
git -C "${MO_TPCH_DIR}" archive "${MO_TPCH_REF}" | tar -C "${tmpdir}" -x

echo "[build] ${BUILD_ENGINE} build -t ${IMAGE_TAG}"
cd "${REPO_DIR}"
"${BUILD_ENGINE}" build \
    --build-context mo-tpch="${tmpdir}" \
    --build-arg MO_REPO="${MO_REPO}" \
    --build-arg MO_BRANCH="${MO_BRANCH}" \
    --build-arg MO_REF="${MO_REF}" \
    --build-arg SIRIUS_REF="${SIRIUS_REF}" \
    -t "${IMAGE_TAG}" \
    -f docker/Dockerfile \
    "$@" \
    .
