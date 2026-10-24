#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Run a command inside the wsrv-dev image against this checkout, exactly as CI does.
# Usage: tools/ci-run.sh 'cmake --preset dev && cmake --build --preset dev && ctest --preset dev'
# Build the image first with `make image` (local) or the dev-env composite action (CI).
set -euo pipefail
[ "$#" -ge 1 ] || { echo "usage: $0 '<command>'" >&2; exit 2; }
root=$(cd "$(dirname "$0")/.." && pwd)
image=${WSRV_DEV_IMAGE:-wsrv-dev:latest}
exec docker run --rm \
    -v "$root:$root" -w "$root" \
    --cap-add SYS_PTRACE --security-opt seccomp=unconfined \
    "$image" bash -c "$*"
