#!/bin/sh
# Runs the aeocha unit tests for the ported Aether modules. These test
# pure-Aether leaf logic (tag math, string/UTF-8 utilities, ...) in
# isolation, complementing the end-to-end JS conformance gate in
# .tests.ae. Exit non-zero if any test fails.
set -e

AETHER=${AETHER:-/home/paul/scm/aether}
AEOCHA=${AEOCHA:-/home/paul/scm/aeocha}
ROOT=$(cd "$(dirname "$0")" && pwd)
LIB="$AEOCHA:$AETHER/std:$ROOT"

rc=0
for t in "$ROOT"/tests/ae/test_*.ae; do
    echo "=== $t ==="
    if ! AETHER_HOME="$AETHER" "$AETHER/build/ae" run --lib "$LIB" "$t"; then
        rc=1
    fi
done
exit $rc
