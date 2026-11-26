#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Measure the line and branch coverage of src/ against NFR-M4 (docs/REQUIREMENTS.md section 3.4).
# Run it inside the dev container (`make shell`), from the repository root: tools/coverage.sh
#
#   JOBS=2 tools/coverage.sh     # compile jobs (default 1)
#
# Why the odd choices:
# - TLS and deflate are switched ON: the preset's cache can hold OFF, and then src/tls and
#   src/extensions are not measured at all.
# - One compile job and `-k 0` with retries: with --coverage a few translation units are big, and the
#   Docker Desktop VM (about 3.6 GiB) kills the compiler when two run together.
# - gcov must be the same version as g++ (gcov-14 for GCC 14), or gcovr cannot read the data.
# - gcov counts with plain (non-atomic) counters, so a multi-threaded test can produce a negative
#   count (GCC bug 68080); gcovr reads those as zero, which can only understate the coverage.
# - Branches that exist only for exceptions or that can never be taken are left out of the figures.
set -euo pipefail
cd "$(dirname "$0")/.."

jobs="${JOBS:-1}"
cmake --preset coverage -DWSRV_ENABLE_TLS=ON -DWSRV_ENABLE_DEFLATE=ON >/dev/null
built=0
for pass in 1 2 3 4; do
    if cmake --build --preset coverage -j"${jobs}" -- -k 0; then built=1; break; fi
    echo "coverage: build pass ${pass} had failures, retrying" >&2
done
[ "${built}" = 1 ] || { echo "coverage: the build failed" >&2; exit 1; }

find build/coverage -name '*.gcda' -delete
ctest --preset coverage -j4
(cd build/coverage && gcovr --root ../.. --gcov-executable "gcov-$(g++ -dumpversion | cut -d. -f1)" \
    --gcov-ignore-parse-errors negative_hits.warn_once_per_file \
    --filter '../../src/' --exclude-throw-branches --exclude-unreachable-branches \
    --json ../coverage-src.json --json-summary ../coverage-src-summary.json >/dev/null)
python3 tools/coverage_report.py build/coverage-src-summary.json
